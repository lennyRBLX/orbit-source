/* Kraken Mount's loopback web API (port 8097): GET /api/state lists its games and says
 * whether it is waiting before it starts adding them; POST /api/rescan wakes its scan.
 * Read every 15 s, and every 3 s for a while after Orbit asks for a scan. All network
 * work happens on this worker, never on an HTTP request thread. */
#include "kraken.h"
#include "orbit.h"
#include "catalog_view.h"
#include <ctype.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define KRAKEN_PORT 8097
#define KRAKEN_LIMIT (4U * 1024U * 1024U)
#define KRAKEN_GAMES 4096
#define KRAKEN_CHECK_GAP 15
#define KRAKEN_FOLLOW_GAP 3
#define KRAKEN_FOLLOW_TIME 600
/* Unanswered or unreadable reads in a row before Kraken Mount counts as stopped: one slow
 * answer while it builds a game must not end what Orbit is following. */
#define KRAKEN_FAILURES 3

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool started, stopping, scan_wanted;
static atomic_bool active, checked, waiting_state;
static int failures;
static cJSON *games;
static char version[32];
static time_t follow_until;

static time_t now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}
static int api_port(void) {
#ifdef ORBIT_TEST
    const char *test = getenv("ORBIT_TEST_KRAKEN_PORT");
    if (test) {
        char *end;
        long n = strtol(test, &end, 10);
        return !*end && n > 0 && n <= 65535 && n != orbit.port ? (int)n : 0;
    }
#endif
    /* Desktop previews never connect to a real console's payloads. */
    if (orbit.desktop) return 0;
    return KRAKEN_PORT == orbit.port ? 0 : KRAKEN_PORT;
}
typedef struct { char *data; size_t length; } Buffer;
static size_t receive(void *data, size_t size, size_t count, void *context) {
    Buffer *b = context;
    if (size && count > KRAKEN_LIMIT / size) return 0;
    size_t n = size * count;
    if (n > KRAKEN_LIMIT - b->length) return 0;
    char *next = realloc(b->data, b->length + n + 1);
    if (!next) return 0;
    b->data = next;
    memcpy(b->data + b->length, data, n);
    b->length += n;
    b->data[b->length] = 0;
    return n;
}
static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)context; (void)a; (void)b; (void)c; (void)d;
    pthread_mutex_lock(&lock);
    bool stop = stopping;
    pthread_mutex_unlock(&lock);
    return stop || orbit_is_stopping();
}
/* A GET returns the parsed JSON body; a POST returns NULL and only reports success. */
static cJSON *request(int port, const char *route, bool post, bool *ok) {
    CURL *c = curl_easy_init();
    if (ok) *ok = false;
    if (!c) return NULL;
    char url[96];
    snprintf(url, sizeof url, "http://127.0.0.1:%d%s", port, route);
    Buffer buffer = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 750L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 5000L);
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    cJSON *result = NULL;
    if (rc == CURLE_OK && code >= 200 && code < 300) {
        if (ok) *ok = true;
        if (!post && buffer.data) result = cJSON_ParseWithLengthOpts(buffer.data, buffer.length + 1, NULL, true);
    }
    free(buffer.data);
    curl_easy_cleanup(c);
    return result;
}
static bool plain_text(const cJSON *v, size_t max) {
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max) return false;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}
static bool title_id(const char *id) {
    if (strlen(id) != 9) return false;
    for (int i = 0; i < 9; i++)
        if (i < 4 ? !(id[i] >= 'A' && id[i] <= 'Z') : !isdigit((unsigned char)id[i])) return false;
    return true;
}
/* Kraken Mount's game list in Orbit's names. Entries Orbit can't trust are left out. A
 * duplicate (a second copy of a title it already runs) stays, with that status, so a
 * download that is one can say so; each path is listed once. */
static cJSON *normalize(const cJSON *state) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(state, "games"), *g;
    if (!cJSON_IsArray(list)) return NULL;
    cJSON *out = cJSON_CreateArray();
    cJSON_ArrayForEach(g, list) {
        if (cJSON_GetArraySize(out) >= KRAKEN_GAMES) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(g, "title_id"), *path = cJSON_GetObjectItemCaseSensitive(g, "path");
        const char *kind = json_text(g, "kind"), *status = json_text(g, "status");
        if (!plain_text(id, 9) || !title_id(id->valuestring) || !plain_text(path, 4095) || path->valuestring[0] != '/' ||
            (strcmp(kind, "pkg") && strcmp(kind, "folder")) ||
            (strcmp(status, "pending") && strcmp(status, "checking") && strcmp(status, "building") &&
             strcmp(status, "ready") && strcmp(status, "error") && strcmp(status, "duplicate")))
            continue;
        bool listed = false;
        const cJSON *prior;
        cJSON_ArrayForEach(prior, out) if (!strcmp(json_text(prior, "path"), path->valuestring)) listed = true;
        if (listed) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "titleId", id->valuestring);
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(g, "title_name");
        cJSON_AddStringToObject(item, "title", plain_text(name, 512) ? name->valuestring : "");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(g, "version");
        cJSON_AddStringToObject(item, "version", plain_text(v, 32) ? v->valuestring : "");
        cJSON_AddStringToObject(item, "path", path->valuestring);
        cJSON_AddStringToObject(item, "kind", kind);
        cJSON_AddStringToObject(item, "status", status);
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(g, "error");
        cJSON_AddStringToObject(item, "error", plain_text(error, 400) ? error->valuestring : "");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(g, "size");
        if (cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble <= 9007199254740991.0)
            cJSON_AddNumberToObject(item, "sizeBytes", size->valuedouble);
        else cJSON_AddNullToObject(item, "sizeBytes");
        cJSON_AddItemToArray(out, item);
    }
    return out;
}
static void *run(void *unused) {
    (void)unused;
    time_t next = 0;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        time_t now = now_seconds();
        if (!scan_wanted && now < next) {
            struct timespec deadline = {.tv_sec = time(NULL) + 1, .tv_nsec = 0};
            pthread_cond_timedwait(&changed, &lock, &deadline);
            continue;
        }
        bool scan = scan_wanted;
        scan_wanted = false;
        pthread_mutex_unlock(&lock);
        int port = api_port();
        if (scan && port && atomic_load(&active)) request(port, "/api/rescan", true, NULL);
        cJSON *state = port ? request(port, "/api/state", false, NULL) : NULL;
        cJSON *list = state ? normalize(state) : NULL;
        const cJSON *waiting = cJSON_GetObjectItemCaseSensitive(state, "waiting");
        bool answered = list != NULL, held = answered && waiting && !cJSON_IsNull(waiting);
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(state, "version");
        pthread_mutex_lock(&lock);
        failures = answered ? 0 : failures + 1;
        /* No answer: keep the last good list and state until KRAKEN_FAILURES in a row. */
        bool next_active = answered ? !held : atomic_load(&active) && failures < KRAKEN_FAILURES;
        if (answered || !next_active) {
            cJSON_Delete(games);
            games = answered && next_active ? list : NULL;
            if (games != list) cJSON_Delete(list);
            copy_text(version, sizeof version, next_active && plain_text(v, 31) ? v->valuestring : "");
            atomic_store(&waiting_state, held);
        }
        bool was = atomic_exchange(&active, next_active);
        atomic_store(&checked, true);
        next = now_seconds() + (follow_until > now_seconds() ? KRAKEN_FOLLOW_GAP : KRAKEN_CHECK_GAP);
        cJSON_Delete(state);
        if (was != next_active) {
            /* Download options for PS5 packages are labelled by whether Kraken Mount runs them. */
            pthread_mutex_unlock(&lock);
            pthread_mutex_lock(&orbit.mutex);
            catalog_view_invalidate_locked();
            pthread_mutex_unlock(&orbit.mutex);
            /* "ok" while Kraken Mount adds games, "skipped" once it stops. */
            diagnostics_stage("kraken", next_active ? "ok" : "skipped", 0);
            pthread_mutex_lock(&lock);
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
int kraken_start(void) {
    pthread_mutex_lock(&lock);
    if (started) { pthread_mutex_unlock(&lock); return 0; }
    stopping = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    int rc = pthread_create(&worker, &attr, run, NULL);
    pthread_attr_destroy(&attr);
    started = !rc;
    /* Nothing will ever ask: say so, so nothing waits for Kraken Mount's first answer. */
    if (rc) atomic_store(&checked, true);
    pthread_mutex_unlock(&lock);
    return rc;
}
void kraken_stop(void) {
    pthread_mutex_lock(&lock);
    bool join = started;
    stopping = true;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
    if (join) pthread_join(worker, NULL);
    pthread_mutex_lock(&lock);
    started = false;
    cJSON_Delete(games);
    games = NULL;
    atomic_store(&active, false);
    atomic_store(&checked, false);
    atomic_store(&waiting_state, false);
    failures = 0;
    pthread_mutex_unlock(&lock);
}
bool kraken_active(void) { return atomic_load(&active); }
bool kraken_checked(void) { return atomic_load(&checked); }
bool kraken_waiting(void) { return atomic_load(&waiting_state); }
void kraken_version(char *out, size_t cap) {
    pthread_mutex_lock(&lock);
    copy_text(out, cap, version);
    pthread_mutex_unlock(&lock);
}
cJSON *kraken_games(void) {
    pthread_mutex_lock(&lock);
    cJSON *out = games && atomic_load(&active) ? cJSON_Duplicate(games, true) : NULL;
    pthread_mutex_unlock(&lock);
    return out;
}
bool kraken_path_in_use(const char *abs_path, char *status, size_t cap) {
    if (cap) *status = 0;
    bool found = false;
    pthread_mutex_lock(&lock);
    const cJSON *g;
    cJSON_ArrayForEach(g, games) {
        const char *path = json_text(g, "path");
        size_t n = strlen(path);
        if (n && !strncmp(abs_path, path, n) && (!abs_path[n] || abs_path[n] == '/')) {
            copy_text(status, cap, json_text(g, "status"));
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&lock);
    return found;
}
void kraken_scan_soon(void) {
    pthread_mutex_lock(&lock);
    scan_wanted = true;
    follow_until = now_seconds() + KRAKEN_FOLLOW_TIME;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
}
bool kraken_scan_folder(const char *root, const char *folder) {
    if (!strcmp(folder, "homebrew") || !strcmp(folder, "etaHEN/games")) return true;
    /* The top of an M.2/extended or USB drive, never the console's own storage. */
    return !*folder && (!strncmp(root, "/mnt/ext", 8) || !strncmp(root, "/mnt/usb", 8));
}
bool kraken_scan_place(const char *root, const char *folder) {
    if (kraken_scan_folder(root, folder)) return true;
    const char *leaf = strrchr(folder, '/');
    char parent[1024];
    if (!leaf || (size_t)(leaf - folder) >= sizeof parent) return false;
    memcpy(parent, folder, (size_t)(leaf - folder));
    parent[leaf - folder] = 0;
    leaf++;
    /* A drive's top is never descended; dot folders and backports (any case) are skipped. */
    return *parent && kraken_scan_folder(root, parent) && *leaf && *leaf != '.' && strcasecmp(leaf, "backports");
}
