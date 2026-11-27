#include "orbit.h"
#include "packages.h"
#include "library_verify.h"
#include "kraken.h"
#include <ctype.h>
#include <curl/curl.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ShadowMount's documented loopback API only. Mutations are explicit, capability
 * checked and revalidated in the worker. Browser paths/URLs are never forwarded. */
#define LIBRARY_LIMIT (8U * 1024U * 1024U)
#define LIBRARY_GAMES 4096
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool started, stopping, pending, busy, completion_scan_busy;
static cJSON *games;
static time_t attempted, checked, updated;
static time_t refreshed;
static char status[24] = "idle", message[256], version[48], provider[24] = "ShadowMount";
static cJSON *capabilities, *storage, *storage_job, *command, *last_request, *action_result;
static bool storage_pending, storage_busy, storage_stale = true, monitor_job;
static time_t storage_attempted, storage_updated, job_poll_at;
static char storage_error[256];
/* Automatic work, so nobody has to press Refresh, Scan or Measure: a scan when
 * Orbit adds a game outside a download (Files) or a drive is connected, at most
 * once a minute, with re-reads while ShadowMount registers what it found; and
 * game sizes measured once, then again when games without a size appear, at
 * most once a minute. Times are monotonic seconds. */
#define AUTO_SCAN_GAP 60
#define AUTO_MEASURE_GAP 60
#define DRIVE_CHECK_GAP 10
static bool auto_scan_wanted, auto_scanning, auto_measuring;
static time_t auto_scan_after, auto_measure_after, follow_until, follow_next, drives_checked;
static cJSON *measured; /* sourceKeys the last automatic measure saw */
static time_t measure_seen; /* the inventory read last checked for games without a size */
static time_t auto_gap(time_t normal) {
#ifdef ORBIT_TEST
    const char *test = getenv("ORBIT_TEST_LIBRARY_AUTO_GAP");
    if (test) return atoi(test);
#endif
    return normal;
}
static char drives_seen[ORBIT_MAX_STORAGE][24];
static size_t drives_seen_count;
static bool drives_known;

static bool success(const cJSON *o);
static void read_storage(int port);
static void read_job(int port);
static void execute_command(cJSON *input);

static time_t monotonic_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

typedef struct { char *data; size_t length; } Buffer;
static bool is_stopping(void) {
    pthread_mutex_lock(&lock);
    bool stop = stopping;
    pthread_mutex_unlock(&lock);
    return stop;
}
static int progress(void *context, curl_off_t total, curl_off_t received,
                    curl_off_t upload_total, curl_off_t uploaded) {
    (void)context; (void)total; (void)received;
    (void)upload_total; (void)uploaded;
    return is_stopping() || orbit_is_stopping();
}
static size_t receive(void *data, size_t size, size_t count, void *context) {
    Buffer *b = context;
    if (size && count > LIBRARY_LIMIT / size) return 0;
    size_t n = size * count;
    if (n > LIBRARY_LIMIT - b->length) return 0;
    char *next = realloc(b->data, b->length + n + 1);
    if (!next) return 0;
    b->data = next;
    memcpy(b->data + b->length, data, n);
    b->length += n;
    b->data[b->length] = 0;
    return n;
}
static cJSON *read_api(int port, const char *route, const char *body,
                       char *error, size_t cap) {
    if (is_stopping()) return NULL;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/api/v1/%s", port, route);
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    Buffer buffer = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_PROXY, "");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 750L);
    /* app.db and drive availability checks can take several seconds on a console.
     * This is a cancellable background read, not an HTTP request-thread wait. */
    bool mutation = !strcmp(route, "scan") || !strcmp(route, "manual/add") || !strcmp(route, "manual/remove") || !strcmp(route, "sources/delete") || !strcmp(route, "games/delete") || !strcmp(route, "games/uninstall") || !strcmp(route, "games/mount") ||
        !strcmp(route, "games/unmount") || !strcmp(route, "games/move") || !strcmp(route, "games/copy");
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, mutation || !strcmp(route, "games/info") ||
        (!strcmp(route, "games") && strstr(body, "true")) ? 60000L :
        !strcmp(route, "games") ? 10000L : 3000L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buffer);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    cJSON *result = rc == CURLE_OK && (code == 200 || code == 202) && buffer.data
        ? cJSON_ParseWithLengthOpts(buffer.data, buffer.length + 1, NULL, true) : NULL;
    if (!result && error) {
        if (rc == CURLE_OPERATION_TIMEDOUT)
            copy_text(error, cap, "ShadowMount took too long to return its inventory. Try again in a moment.");
        else if (rc != CURLE_OK)
            snprintf(error, cap, "Could not read ShadowMount inventory: %s.", curl_easy_strerror(rc));
        else if (code != 200 && code != 202) {
            cJSON *detail = buffer.data ? cJSON_Parse(buffer.data) : NULL;
            snprintf(error, cap, "ShadowMount: %.180s (HTTP %ld).",
                *json_text(detail, "error") ? json_text(detail, "error") : "request failed", code);
            cJSON_Delete(detail);
        }
        else copy_text(error, cap, "ShadowMount returned invalid inventory JSON.");
    }
    free(buffer.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return result;
}
static bool has(const cJSON *array, const char *value) {
    const cJSON *v;
    cJSON_ArrayForEach(v, array)
        if (cJSON_IsString(v) && !strcmp(v->valuestring, value)) return true;
    return false;
}
static bool number(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 &&
        v->valuedouble <= 9007199254740991.0 && floor(v->valuedouble) == v->valuedouble;
}
static void identity(const char *a, const char *b, const char *c, char out[65]) {
    char data[12300];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    snprintf(data, sizeof data, "%s\n%s\n%s", a, b, c);
    SHA256((const unsigned char *)data, strlen(data), digest);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}
static void set_gate(bool value) {
    pthread_mutex_lock(&orbit.mutex);
    orbit.library_storage_busy = value;
    if (!value) memset(&orbit.holds[HOLD_LIBRARY], 0, sizeof orbit.holds[HOLD_LIBRARY]);
    pthread_cond_signal(&orbit.changed);
    pthread_mutex_unlock(&orbit.mutex);
}
/* What the provider's job still writes, for downloads starting meanwhile. */
static void hold_left(const cJSON *job) {
    const int64_t total = json_int(job, "totalBytes"), done = json_int(job, "processedBytes");
    storage_hold_left(HOLD_LIBRARY, total > done ? (uint64_t)(total - done) : 0);
}
static bool success(const cJSON *o) {
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(o, "status");
    return cJSON_IsObject(o) && cJSON_IsNumber(s) && s->valuedouble == 0;
}
static int api_port(void) {
#ifdef ORBIT_TEST
    const char *test = getenv("ORBIT_TEST_LIBRARY_PORT");
    if (test) {
        char *end;
        long n = strtol(test, &end, 10);
        return !*end && n > 0 && n <= 65535 && n != orbit.port ? (int)n : 0;
    }
#endif
    /* Desktop previews never discover or connect to a real console. */
    if (orbit.desktop) return 0;
    int port = 10101;
    unsigned char *data = NULL;
    size_t length = 0;
    if (!read_regular_file("/data/shadowmount/config.ini", 16384, &data, &length)) {
        char *save = NULL;
        for (char *line = strtok_r((char *)data, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            while (isspace((unsigned char)*line)) line++;
            char key[64], value[64];
            if (sscanf(line, "%63[^=]=%63s", key, value) != 2) continue;
            size_t k = strlen(key);
            while (k && isspace((unsigned char)key[k - 1])) key[--k] = 0;
            if (!strcmp(key, "api_enabled") && !strcmp(value, "0")) { port = 0; break; }
            if (!strcmp(key, "api_port")) {
                char *end;
                long n = strtol(value, &end, 10);
                if (*end || n < 1 || n > 65535) { port = 0; break; }
                port = (int)n;
            }
        }
        free(data);
    }
    return port == orbit.port ? 0 : port;
}
static bool text_field(const cJSON *g, const char *key, size_t max) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max) return false;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}
static bool title_id(const char *id) {
    if (strlen(id) != 9) return false;
    for (int i = 0; i < 9; i++)
        if (i < 4 ? !(id[i] >= 'A' && id[i] <= 'Z') : !isdigit((unsigned char)id[i]))
            return false;
    return true;
}
static const char *format(const cJSON *g) {
    const char *source = json_text(g, "source_type"), *image = json_text(g, "image_type");
    if (!strcmp(source, "pkg")) return "PKG";
    if (!strcmp(source, "folder")) return "Folder";
    if (!strcmp(image, "exfatfs")) return "exFAT";
    if (!strcmp(image, "pfsc")) return "FFPFSC";
    if (!strcmp(image, "pfs")) return "FFPFS";
    if (!strcmp(image, "ufs")) return "FFPKG";
    return "Image";
}
static void location(const char *path, char *out, size_t cap) {
    const char *p = !strncmp(path, "/mnt/usb", 8) ? path + 8 :
                    !strncmp(path, "/mnt/ext", 8) ? path + 8 : NULL;
    if (!strncmp(path, "/mnt/ext1/", 10))
        copy_text(out, cap, "M.2 SSD");
    else if (p && isdigit((unsigned char)p[0]) && p[1] == '/')
        snprintf(out, cap, "%s %c", path[5] == 'u' ? "USB" : "Extended storage", *p);
    else if (!strncmp(path, "/data/", 6) || !strncmp(path, "/user/", 6))
        copy_text(out, cap, "Console storage");
    else if (!strncmp(path, "/mnt/shadowmnt/", 15))
        copy_text(out, cap, "ShadowMount");
    else copy_text(out, cap, "Other location");
}
/* Catalogue artwork is optional enrichment, independent of source selection.
 * Never trust/forward ShadowMount's supplied icon URL to a browser. An empty
 * title, when given, takes the catalogue's name. */
static void add_artwork(cJSON *item, const char *id, char *title, size_t cap) {
    pthread_mutex_lock(&orbit.mutex);
    cJSON *release;
    cJSON_ArrayForEach(release, orbit.catalog)
        if (!strcmp(json_text(release, "titleId"), id)) {
            cJSON_AddStringToObject(item, "cover", json_text(release, "cover"));
            const char *fallback = json_text(release, "coverFallback");
            if (*fallback) cJSON_AddStringToObject(item, "coverFallback", fallback);
            if (title && !*title) copy_text(title, cap, json_text(release, "title"));
            break;
        }
    pthread_mutex_unlock(&orbit.mutex);
}
static cJSON *normalize(const cJSON *response, char *error, size_t cap) {
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(response, "games");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(response, "count");
    int n = cJSON_GetArraySize(list);
    if (!success(response) || !cJSON_IsArray(list) || n > LIBRARY_GAMES ||
        !cJSON_IsNumber(count) || count->valuedouble != n) {
        copy_text(error, cap, "ShadowMount returned an incomplete inventory list or count.");
        return NULL;
    }
    cJSON *out = cJSON_CreateArray(), *g;
    const char *field = "title ID";
    cJSON_ArrayForEach(g, list) {
        const char *id = json_text(g, "title_id"), *platform = json_text(g, "platform");
        const char *source = json_text(g, "source_type"), *path = json_text(g, "path");
        field = "title ID";
        if (!cJSON_IsObject(g) || !title_id(id)) goto invalid;
        const char *texts[] = {"title_name", "path", "runtime_path", "image_type"};
        const size_t limits[] = {512, 4096, 4096, 32};
        for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) {
            field = texts[i];
            if (!text_field(g, field, limits[i])) goto invalid;
        }
        field = "source path";
        if (*path && path[0] != '/') goto invalid;
        field = "runtime path";
        if (*json_text(g, "runtime_path") && *json_text(g, "runtime_path") != '/') goto invalid;
        field = "platform";
        if (strcmp(platform, "ps5") && strcmp(platform, "ps4") && strcmp(platform, "unknown")) goto invalid;
        field = "source type";
        if (strcmp(source, "folder") && strcmp(source, "image") && strcmp(source, "pkg")) goto invalid;
        const char *flags[] = {"installed", "mounted", "source_available"};
        for (size_t i = 0; i < sizeof flags / sizeof *flags; i++) {
            field = flags[i];
            if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(g, flags[i]))) goto invalid;
        }
        field = "duplicate title ID";
        cJSON *prior;
        cJSON_ArrayForEach(prior, out)
            if (!strcmp(json_text(prior, "titleId"), id)) goto invalid;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "titleId", id);
        cJSON_AddStringToObject(item, "title", *json_text(g, "title_name") ? json_text(g, "title_name") : id);
        cJSON_AddStringToObject(item, "platform", platform);
        cJSON_AddStringToObject(item, "format", format(g));
        cJSON_AddStringToObject(item, "path", path);
        cJSON_AddStringToObject(item, "runtimePath", json_text(g, "runtime_path"));
        char key[65];
        identity(id, path, format(g), key);
        cJSON_AddStringToObject(item, "sourceKey", key);
        cJSON_AddBoolToObject(item, "managed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "managed")) && strcmp(source, "pkg"));
        cJSON_AddBoolToObject(item, "canManageSource", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "can_manage_source")) && strcmp(source, "pkg"));
        bool measured = number(g, "size_status") && json_int(g, "size_status") == 0 && number(g, "size_bytes");
        if (measured) cJSON_AddNumberToObject(item, "sizeBytes", (double)json_int(g, "size_bytes"));
        else cJSON_AddNullToObject(item, "sizeBytes");
        cJSON_AddStringToObject(item, "sizeStatus", measured ? "ready" : cJSON_HasObjectItem(g, "size_status") ? "unavailable" : "unknown");
        char where[64];
        location(path, where, sizeof where);
        cJSON_AddStringToObject(item, "location", where);
        cJSON_AddBoolToObject(item, "installed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "installed")));
        cJSON_AddBoolToObject(item, "mounted", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "mounted")));
        cJSON_AddBoolToObject(item, "onDrive", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "source_available")));
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(g, "app_db_size_bytes");
        if (cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble <= 9007199254740991.0)
            cJSON_AddNumberToObject(item, "installedSizeBytes", size->valuedouble);
        else cJSON_AddNullToObject(item, "installedSizeBytes");
        add_artwork(item, id, NULL, 0);
        cJSON_AddItemToArray(out, item);
    }
    return out;
invalid:
    snprintf(error, cap, "ShadowMount inventory entry %d has an unsupported %s.",
             cJSON_GetArraySize(out) + 1, field);
    cJSON_Delete(out);
    return NULL;
}
/* Kraken Mount's games, as Library lists ShadowMount's. Kraken Mount registers games
 * without installing them and offers no move, copy or delete through Orbit. */
static cJSON *normalize_kraken(const cJSON *list) {
    cJSON *out = cJSON_CreateArray(), *g;
    cJSON_ArrayForEach(g, list) {
        const char *id = json_text(g, "titleId"), *path = json_text(g, "path");
        const char *kind = !strcmp(json_text(g, "kind"), "pkg") ? "PKG" : "Folder";
        cJSON *item = cJSON_CreateObject();
        char title[513];
        copy_text(title, sizeof title, json_text(g, "title"));
        cJSON_AddStringToObject(item, "titleId", id);
        cJSON_AddStringToObject(item, "platform", "ps5");
        cJSON_AddStringToObject(item, "format", kind);
        cJSON_AddStringToObject(item, "path", path);
        cJSON_AddStringToObject(item, "runtimePath", "");
        char key[65];
        identity(id, path, kind, key);
        cJSON_AddStringToObject(item, "sourceKey", key);
        cJSON_AddBoolToObject(item, "managed", false);
        cJSON_AddBoolToObject(item, "canManageSource", false);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(g, "sizeBytes");
        if (cJSON_IsNumber(size)) cJSON_AddNumberToObject(item, "sizeBytes", size->valuedouble);
        else cJSON_AddNullToObject(item, "sizeBytes");
        cJSON_AddStringToObject(item, "sizeStatus", cJSON_IsNumber(size) ? "ready" : "unknown");
        char where[64];
        location(path, where, sizeof where);
        cJSON_AddStringToObject(item, "location", where);
        cJSON_AddBoolToObject(item, "installed", false);
        cJSON_AddBoolToObject(item, "mounted", !strcmp(json_text(g, "status"), "ready"));
        cJSON_AddBoolToObject(item, "onDrive", true);
        cJSON_AddNullToObject(item, "installedSizeBytes");
        add_artwork(item, id, title, sizeof title);
        cJSON_AddStringToObject(item, "title", *title ? title : id);
        cJSON_AddItemToArray(out, item);
    }
    return out;
}
/* ShadowMount lists games installed from PKG files only from 1.7beta4 on. With an
 * older build, Orbit finds them where ShadowMount would: an app.pkg in an app folder. */
static const char *const app_roots[] = {"/user/app", "/mnt/ext0/user/app", "/mnt/ext0/ps5/user/app",
                                        "/mnt/ext1/user/app", "/mnt/ext1/ps5/user/app"};
/* Prepended to every path Orbit reads here. Tests point it at a fixture folder. */
static const char *app_prefix(void) {
#ifdef ORBIT_TEST
    return getenv("ORBIT_TEST_APP_ROOT");
#else
    return orbit.desktop ? NULL : "";
#endif
}
static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t le32(const unsigned char *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* TITLE from a PS4 param.sfo. */
static bool sfo_title(const unsigned char *d, size_t n, char *out, size_t cap) {
    if (n < 20 || memcmp(d, "\0PSF", 4)) return false;
    size_t keys = le32(d + 8), data = le32(d + 12), count = le32(d + 16);
    if (keys > n || data > n || count > (n - 20) / 16) return false;
    for (size_t i = 0; i < count; i++) {
        const unsigned char *e = d + 20 + i * 16;
        size_t key = keys + le16(e), length = le32(e + 4), at = data + (size_t)le32(e + 12);
        if (le16(e + 2) != 0x0204 || key >= n || !memchr(d + key, 0, n - key) ||
            strcmp((const char *)d + key, "TITLE") || at > n || length > n - at) continue;
        size_t used = strnlen((const char *)d + at, length);
        if (used >= cap) used = cap - 1;
        memcpy(out, d + at, used);
        out[used] = 0;
        return true;
    }
    return false;
}
/* The default language's titleName from a PS5 param.json. */
static bool json_title(const unsigned char *d, size_t n, char *out, size_t cap) {
    cJSON *param = cJSON_ParseWithLength((const char *)d, n);
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(param, "localizedParameters");
    const cJSON *names_for = cJSON_GetObjectItemCaseSensitive(names, json_text(names, "defaultLanguage"));
    if (!cJSON_IsObject(names_for)) names_for = cJSON_GetObjectItemCaseSensitive(names, "en-US");
    copy_text(out, cap, json_text(names_for, "titleName"));
    cJSON_Delete(param);
    return *out;
}
/* Keeps a console-supplied name printable and within the Library's 512-byte limit. */
static void clean_title(char *s) {
    for (char *p = s; *p; p++)
        if ((unsigned char)*p < 32 || *p == 127) *p = ' ';
    size_t n = strlen(s);
    if (n > 512) {
        n = 512;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    while (n && s[n - 1] == ' ') n--;
    s[n] = 0;
    char *start = s;
    while (*start == ' ') start++;
    memmove(s, start, strlen(start) + 1);
}
/* The game's name and platform from its param file, wherever the console keeps one. */
static void read_param(const char *prefix, const char *root, const char *id,
                       char *title, size_t cap, const char **platform) {
    char drive[64], folders[4][160];
    size_t drive_length = strlen(root) - strlen("/user/app");
    snprintf(drive, sizeof drive, "%.*s", (int)drive_length, root);
    snprintf(folders[0], sizeof folders[0], "%s/user/appmeta/%s", drive, id);
    snprintf(folders[1], sizeof folders[1], "/user/appmeta/%s", id);
    snprintf(folders[2], sizeof folders[2], "/user/appmeta/external/%s", id);
    snprintf(folders[3], sizeof folders[3], "%s/%s/sce_sys", root, id);
    for (size_t i = 0; i < 4; i++)
        for (int ps5 = 1; ps5 >= 0; ps5--) {
            char path[4096];
            unsigned char *data = NULL;
            size_t length = 0;
            snprintf(path, sizeof path, "%s%s/%s", prefix, folders[i], ps5 ? "param.json" : "param.sfo");
            if (read_regular_file(path, 1024 * 1024, &data, &length)) continue;
            *title = 0;
            bool found = ps5 ? json_title(data, length, title, cap) : sfo_title(data, length, title, cap);
            free(data);
            clean_title(title);
            if (found && *title) {
                if (!strcmp(*platform, "unknown")) *platform = ps5 ? "ps5" : "ps4";
                return;
            }
        }
    *title = 0;
}
static void add_installed_packages(cJSON *rows, const cJSON *caps) {
    const char *prefix = app_prefix();
    if (!prefix || has(caps, "installed_pkg_games")) return;
    for (size_t r = 0; r < sizeof app_roots / sizeof *app_roots; r++) {
        char folder[4096];
        snprintf(folder, sizeof folder, "%s%s", prefix, app_roots[r]);
        DIR *dir = opendir(folder);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) && cJSON_GetArraySize(rows) < LIBRARY_GAMES) {
            const char *id = entry->d_name;
            if (!title_id(id)) continue;
            bool listed = false;
            const cJSON *row;
            cJSON_ArrayForEach(row, rows)
                if (!strcmp(json_text(row, "titleId"), id)) { listed = true; break; }
            char path[4096], file[4096];
            struct stat st;
            snprintf(path, sizeof path, "%s/%s/app.pkg", app_roots[r], id);
            snprintf(file, sizeof file, "%s%s", prefix, path);
            if (listed || stat(file, &st) || !S_ISREG(st.st_mode)) continue;
            const char *platform = !strncmp(id, "CUSA", 4) ? "ps4" : !strncmp(id, "PPSA", 4) ? "ps5" : "unknown";
            char title[1024], key[65], where[64];
            read_param(prefix, app_roots[r], id, title, sizeof title, &platform);
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "titleId", id);
            cJSON_AddStringToObject(item, "title", title);
            cJSON_AddStringToObject(item, "platform", platform);
            cJSON_AddStringToObject(item, "format", "PKG");
            cJSON_AddStringToObject(item, "path", path);
            cJSON_AddStringToObject(item, "runtimePath", "");
            identity(id, path, "PKG", key);
            cJSON_AddStringToObject(item, "sourceKey", key);
            cJSON_AddBoolToObject(item, "managed", false);
            cJSON_AddBoolToObject(item, "canManageSource", false);
            /* As ShadowMount measures an installed PKG without app.db: the package's size. */
            cJSON_AddNumberToObject(item, "sizeBytes", (double)st.st_size);
            cJSON_AddStringToObject(item, "sizeStatus", "ready");
            location(path, where, sizeof where);
            cJSON_AddStringToObject(item, "location", where);
            cJSON_AddBoolToObject(item, "installed", true);
            cJSON_AddBoolToObject(item, "mounted", false);
            cJSON_AddBoolToObject(item, "onDrive", true);
            cJSON_AddNullToObject(item, "installedSizeBytes");
            cJSON_AddBoolToObject(item, "foundByOrbit", true);
            add_artwork(item, id, title, sizeof title);
            /* No param file: the catalogue's name, else the title ID, as for ShadowMount's entries. */
            cJSON_ReplaceItemInObjectCaseSensitive(item, "title", cJSON_CreateString(*title ? title : id));
            cJSON_AddItemToArray(rows, item);
        }
        closedir(dir);
    }
}
/* Only normalized, provider-reported locations can become copy/move targets. */
static bool safe_path(const char *p) {
    return p[0] == '/' && strlen(p) < 4096 && !strstr(p, "/../") &&
        !strstr(p, "/./") && !strstr(p, "//") && strcmp(p, "/") &&
        strcmp(p + (strlen(p) > 3 ? strlen(p) - 3 : 0), "/..") &&
        strcmp(p + (strlen(p) > 2 ? strlen(p) - 2 : 0), "/.");
}
static bool visible_drive(const char *p) {
    return !strcmp(p, "/user") ||
        (!strncmp(p, "/mnt/usb", 8) && isdigit((unsigned char)p[8]) && !p[9]) ||
        (!strncmp(p, "/mnt/ext", 8) && isdigit((unsigned char)p[8]) && !p[9]);
}
static cJSON *normalize_storage(const cJSON *response) {
    const cJSON *mounts = cJSON_GetObjectItemCaseSensitive(response, "mounts");
    const cJSON *destinations = cJSON_GetObjectItemCaseSensitive(response, "destinations");
    if (!success(response) || !cJSON_IsArray(mounts) || !cJSON_IsArray(destinations) ||
        !number(response, "count") || json_int(response, "count") != cJSON_GetArraySize(mounts) ||
        !number(response, "destination_count") || json_int(response, "destination_count") != cJSON_GetArraySize(destinations) ||
        cJSON_GetArraySize(mounts) > LIBRARY_GAMES || cJSON_GetArraySize(destinations) > 128) return NULL;
    cJSON *out = cJSON_CreateObject();
    for (int k = 0; k < 2; k++) {
        cJSON *list = cJSON_AddArrayToObject(out, k ? "destinations" : "drives");
        const cJSON *d;
        cJSON_ArrayForEach(d, (k ? destinations : mounts)) {
            /* ShadowMount includes the root/system and mounted-game filesystems.
             * They are not selectable drives. In particular, '/' is a valid
             * mount point but is deliberately forbidden as a destination. */
            if (!text_field(d, "mount_point", 4096)) goto invalid;
            if (!k && !visible_drive(json_text(d, "mount_point"))) continue;
            const char *path = json_text(d, k ? "path" : "mount_point");
            if (!text_field(d, "source", 4096) || !text_field(d, "mount_point", 4096) ||
                !text_field(d, "filesystem", 64) || (k && !text_field(d, "path", 4096)) ||
                !safe_path(path) || (strcmp(json_text(d, "mount_point"), "/") &&
                                    !safe_path(json_text(d, "mount_point"))) ||
                !number(d, "total_bytes") || !number(d, "available_bytes") || !number(d, "used_bytes") ||
                json_int(d, "available_bytes") > json_int(d, "total_bytes") ||
                json_int(d, "used_bytes") > json_int(d, "total_bytes") ||
                !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(d, "read_only"))) goto invalid;
            cJSON *item = cJSON_CreateObject();
            char id[65], where[64], label_path[4100];
            identity(path, json_text(d, "source"), json_text(d, "mount_point"), id);
            snprintf(label_path, sizeof label_path, "%s/", json_text(d, "mount_point"));
            location(label_path, where, sizeof where);
            cJSON_AddStringToObject(item, "id", id);
            cJSON_AddStringToObject(item, "path", path);
            cJSON_AddStringToObject(item, "mountPoint", json_text(d, "mount_point"));
            cJSON_AddStringToObject(item, "label", where);
            cJSON_AddStringToObject(item, "filesystem", json_text(d, "filesystem"));
            cJSON_AddBoolToObject(item, "readOnly", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "read_only")));
            cJSON_AddNumberToObject(item, "totalBytes", (double)json_int(d, "total_bytes"));
            cJSON_AddNumberToObject(item, "freeBytes", (double)json_int(d, "available_bytes"));
            cJSON_AddNumberToObject(item, "usedBytes", (double)json_int(d, "used_bytes"));
            cJSON_AddItemToArray(list, item);
        }
    }
    return out;
invalid:
    cJSON_Delete(out);
    return NULL;
}
static void read_storage(int port) {
    char error[256] = {0};
    cJSON *response = read_api(port, "storage", "{}", error, sizeof error);
    cJSON *next = response ? normalize_storage(response) : NULL;
    cJSON_Delete(response);
    pthread_mutex_lock(&lock);
    if (next) {
        cJSON_Delete(storage);
        storage = next;
        storage_updated = time(NULL);
    }
    storage_stale = !next;
    copy_text(storage_error, sizeof storage_error, next ? "" : *error ? error : "Could not read drive capacity from ShadowMount.");
    pthread_mutex_unlock(&lock);
}
static cJSON *normalize_job(const cJSON *r) {
    if (!success(r) || !number(r, "job_id") || !number(r, "total_bytes") ||
        !number(r, "processed_bytes") || !number(r, "result_status") ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(r, "active")) ||
        !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(r, "cancellable"))) return NULL;
    const char *texts[] = {"operation", "state", "title_id", "source", "destination", "result_error"};
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++)
        if (!text_field(r, texts[i], 4096)) return NULL;
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "id", (double)json_int(r, "job_id"));
    cJSON_AddStringToObject(out, "operation", json_text(r, "operation"));
    cJSON_AddStringToObject(out, "state", json_text(r, "state"));
    cJSON_AddStringToObject(out, "titleId", json_text(r, "title_id"));
    cJSON_AddStringToObject(out, "source", json_text(r, "source"));
    cJSON_AddStringToObject(out, "destination", json_text(r, "destination"));
    cJSON_AddStringToObject(out, "error", json_text(r, "result_error"));
    cJSON_AddNumberToObject(out, "result", (double)json_int(r, "result_status"));
    cJSON_AddNumberToObject(out, "totalBytes", (double)json_int(r, "total_bytes"));
    cJSON_AddNumberToObject(out, "processedBytes", (double)json_int(r, "processed_bytes"));
    cJSON_AddNumberToObject(out, "speed", number(r, "speed_bytes_per_second") ? (double)json_int(r, "speed_bytes_per_second") : 0);
    cJSON_AddBoolToObject(out, "active", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "active")));
    cJSON_AddBoolToObject(out, "cancellable", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "cancellable")));
    cJSON_AddBoolToObject(out, "cancelRequested", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "cancel_requested")));
    return out;
}
static void accept_job(cJSON *next) {
    bool active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(next, "active"));
    pthread_mutex_lock(&lock);
    bool ended = monitor_job && !active;
    cJSON_Delete(storage_job);
    storage_job = next;
    monitor_job = active;
    job_poll_at = monotonic_seconds() + 2;
    if (ended) { pending = true; storage_pending = true; }
    pthread_mutex_unlock(&lock);
    if (active) hold_left(next);
    set_gate(active);
}
static void read_job(int port) {
    char error[256] = {0};
    cJSON *r = read_api(port, "games/storage/status", "{}", error, sizeof error);
    cJSON *next = r ? normalize_job(r) : NULL;
    cJSON_Delete(r);
    if (next) accept_job(next);
    else {
        /* An unknown result is not permission to start another operation. */
        pthread_mutex_lock(&lock);
        if (monitor_job) {
            copy_text(storage_error, sizeof storage_error,
                "Cannot confirm storage job status. Reconnect ShadowMount; the job may still be running.");
            storage_stale = true;
        }
        job_poll_at = monotonic_seconds() + 5;
        pthread_mutex_unlock(&lock);
    }
}
static const char *capability(const char *action) {
    return !strcmp(action, "scan") ? "rescan" : !strcmp(action, "mount") ? "mount_game" :
        !strcmp(action, "unmount") ? "unmount_game" : !strcmp(action, "copy") ? "copy_game_source" :
        !strcmp(action, "move") ? "move_game_source" : !strcmp(action, "cancel") ? "storage_job_cancel" :
        !strcmp(action, "measure") ? "list_games" : !strcmp(action, "delete") ? "delete_game_source" : "";
}
/* Copies and moves take turns with Files actions and Zero transfers. Downloads
 * keep running: they leave room for the copy and keep away from its names. */
static bool storage_taken(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool taken = orbit.receiver_active || orbit.files_storage_busy;
    if (!taken) orbit.library_storage_busy = true;
    pthread_mutex_unlock(&orbit.mutex);
    return taken;
}
static void result_message(const char *state, const char *text) {
    pthread_mutex_lock(&lock);
    cJSON_ReplaceItemInObjectCaseSensitive(action_result, "state", cJSON_CreateString(state));
    cJSON_ReplaceItemInObjectCaseSensitive(action_result, "message", cJSON_CreateString(text));
    pthread_mutex_unlock(&lock);
}
static void preserve_sizes(cJSON *next) {
    cJSON *g, *old;
    cJSON_ArrayForEach(g, next) {
        if (strcmp(json_text(g, "sizeStatus"), "unknown")) continue;
        cJSON_ArrayForEach(old, games) {
            if (!strcmp(json_text(old, "sourceKey"), json_text(g, "sourceKey")) &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "onDrive"))) {
                const cJSON *value = cJSON_GetObjectItemCaseSensitive(old, "sizeBytes");
                if (cJSON_IsNumber(value)) {
                    cJSON_ReplaceItemInObjectCaseSensitive(g, "sizeBytes", cJSON_Duplicate(value, true));
                    cJSON_ReplaceItemInObjectCaseSensitive(g, "sizeStatus", cJSON_CreateString("ready"));
                }
                break;
            }
        }
    }
}
/* ShadowMount registers what a scan finds over the next minute or so: read the
 * inventory again every ten seconds meanwhile. */
static void follow_scan(void) {
    pthread_mutex_lock(&lock);
    time_t now = monotonic_seconds();
    follow_until = now + 90;
    follow_next = now + 5;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
}
/* A move is deliberately copy -> read back -> register -> remove original.
 * ShadowMount owns physical mutations and its runtime/scanner locks. If Orbit
 * stops between stages, the original survives unless verification completed. */
static atomic_bool verified_move_active, verified_move_cancel;
static bool verified_move_finalizing;
static void show_move_job(cJSON *next, bool deleting) {
    pthread_mutex_lock(&lock);
    if (!deleting && !strcmp(json_text(next, "operation"), "delete") && storage_job &&
        !strcmp(json_text(storage_job, "operation"), "move")) {
        const char *fields[] = {"titleId", "destination", "totalBytes", "processedBytes"};
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
            cJSON_ReplaceItemInObjectCaseSensitive(next, fields[i],
                cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(storage_job, fields[i]), true));
        cJSON_ReplaceItemInObjectCaseSensitive(next, "state", cJSON_CreateString("finalizing"));
        cJSON_ReplaceItemInObjectCaseSensitive(next, "cancellable", cJSON_CreateBool(false));
    }
    cJSON_ReplaceItemInObjectCaseSensitive(next, "operation", cJSON_CreateString(deleting ? "delete" : "move"));
    cJSON_ReplaceItemInObjectCaseSensitive(next, "active", cJSON_CreateBool(true));
    cJSON_Delete(storage_job); storage_job = next;
    pthread_mutex_unlock(&lock);
    hold_left(next);
}
/* deleting: the job deletes a game's source on its own (Free up space), not a move's. */
static int wait_move_job(int port, cJSON *response, const char *source, const char *destination,
                         bool deleting, char *error, size_t cap) {
    cJSON *job = response ? normalize_job(response) : NULL;
    int64_t expected = json_int(job, "id");
    bool sent_cancel = false;
    for (;;) {
        if (!job || !expected || json_int(job, "id") != expected || strcmp(json_text(job, "source"), source) ||
            (destination && strcmp(json_text(job, "destination"), destination))) {
            cJSON_Delete(job);
            copy_text(error, cap, deleting ? "Cannot confirm the storage operation. Check Library before trying again."
                : "Cannot confirm the storage operation. Check Library before trying again. The verified destination, if complete, is kept.");
            pthread_mutex_lock(&lock); monitor_job = true; job_poll_at = 0; pthread_mutex_unlock(&lock);
            return -2;
        }
        bool active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "active"));
        int64_t result = json_int(job, "result");
        bool complete = !strcmp(json_text(job, "state"), "complete") || !strcmp(json_text(job, "state"), "completed");
        if (!active && (!complete || result)) {
            copy_text(error, cap, *json_text(job, "error") ? json_text(job, "error") : deleting
                ? "ShadowMount stopped deleting the game. Check Library before trying again."
                : "Move stopped. Check both drives; no further files were removed.");
            show_move_job(job, deleting); return -1;
        }
        show_move_job(job, deleting);
        if (!active) return 0;
        if (atomic_load(&verified_move_cancel) && !sent_cancel) {
            char body[80]; snprintf(body, sizeof body, "{\"job_id\":%lld}", (long long)expected);
            cJSON_Delete(read_api(port, "games/storage/cancel", body, NULL, 0)); sent_cancel = true;
        }
        if (orbit_is_stopping()) return -2;
        struct timespec delay = {.tv_nsec = 200000000L}; nanosleep(&delay, NULL);
        cJSON *r = read_api(port, "games/storage/status", "{}", error, cap);
        job = r ? normalize_job(r) : NULL; cJSON_Delete(r);
    }
}
static void verify_move_progress(int64_t bytes, void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "state", cJSON_CreateString("verifying"));
    cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "cancellable", cJSON_CreateBool(!verified_move_finalizing));
    cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "processedBytes", cJSON_CreateNumber((double)bytes));
    cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "speed", cJSON_CreateNumber(0));
    pthread_mutex_unlock(&lock);
}
static bool manual_path(int port, const char *route, const char *path, char *error, size_t cap) {
    cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "path", path);
    char *encoded = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    cJSON *r = encoded ? read_api(port, route, encoded, error, cap) : NULL;
    free(encoded); bool ok = success(r); cJSON_Delete(r); return ok;
}
static int verified_move(int port, const cJSON *body, const char *source, const char *target, char *error, size_t cap) {
    struct stat original;
    if (library_source_identity(source, &original)) {
        copy_text(error, cap, "This game's source is not on an available Orbit drive. Reconnect the drive and try again."); return -1;
    }
    const char *leaf = strrchr(source, '/');
    char destination[1024];
    if (!leaf || snprintf(destination, sizeof destination, "%s%s", target, leaf) >= (int)sizeof destination) return -1;
    pthread_mutex_lock(&lock);
    atomic_store(&verified_move_active, true); atomic_store(&verified_move_cancel, false); verified_move_finalizing = false;
    pthread_mutex_unlock(&lock);
    char *encoded = cJSON_PrintUnformatted(body);
    cJSON *response = encoded ? read_api(port, "games/copy", encoded, error, cap) : NULL;
    free(encoded);
    int rc = wait_move_job(port, response, source, destination, false, error, cap); cJSON_Delete(response);
    if (rc) goto done;
    verify_move_progress(0, NULL);
    rc = library_verify_copy(source, destination, original.st_dev, original.st_ino,
                             &verified_move_cancel, verify_move_progress, NULL, error, cap);
    if (rc) goto done;
    struct stat verified_destination;
    if (!library_source_unchanged(source, &original) || library_source_identity(destination, &verified_destination)) {
        copy_text(error, cap, "The game location changed during the move. Both copies have been kept."); rc = -1; goto done;
    }
    if (!manual_path(port, "manual/add", destination, error, cap)) {
        copy_text(error, cap, "The copy is verified, but ShadowMount could not register its new location. The original has been kept."); rc = -1; goto done;
    }
    if (!library_source_unchanged(source, &original) || !library_source_unchanged(destination, &verified_destination)) {
        copy_text(error, cap, "The game location changed during the move. Both copies have been kept."); rc = -1; goto done;
    }
    pthread_mutex_lock(&lock);
    bool cancel = atomic_load(&verified_move_cancel) || orbit_is_stopping();
    if (!cancel) {
        verified_move_finalizing = true;
        cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "state", cJSON_CreateString("finalizing"));
        cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "cancellable", cJSON_CreateBool(false));
    }
    pthread_mutex_unlock(&lock);
    if (cancel) { copy_text(error, cap, "Move cancelled. Both copies have been kept."); rc = -1; goto done; }
    /* Delete by the exact physical source, never by title ID: the scan after
     * copying may have made the new destination the title's selected source. */
    cJSON *remove = cJSON_CreateObject();
    cJSON_AddStringToObject(remove, "path", source); cJSON_AddBoolToObject(remove, "confirm", true);
    encoded = cJSON_PrintUnformatted(remove); cJSON_Delete(remove);
    response = encoded ? read_api(port, "sources/delete", encoded, error, cap) : NULL; free(encoded);
    if (response && !success(response)) {
        copy_text(error, cap, "The copy is verified, but ShadowMount could not remove the original. Both locations are kept; update ShadowMount or check its storage status."); rc = -1;
    } else rc = wait_move_job(port, response, source, NULL, false, error, cap);
    cJSON_Delete(response);
    if (!rc && !manual_path(port, "manual/remove", source, error, cap)) {
        copy_text(error, cap, "The verified game has moved, but ShadowMount's old source entry could not be removed. The new copy is kept."); rc = -1;
    }
    if (!rc) cJSON_Delete(read_api(port, "scan", "{\"reset_attempts\":false}", NULL, 0));
done:
    pthread_mutex_lock(&lock);
    atomic_store(&verified_move_active, false);
    if (rc != -2 && storage_job) {
        cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "active", cJSON_CreateBool(false));
        cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "cancellable", cJSON_CreateBool(false));
        cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "state", cJSON_CreateString(rc ? "failed" : "completed"));
        cJSON_ReplaceItemInObjectCaseSensitive(storage_job, "error", cJSON_CreateString(rc ? error : ""));
    }
    pthread_mutex_unlock(&lock);
    return rc;
}

/* Free up space: delete one game's source. Orbit unmounts it first when needed, ShadowMount
 * deletes the title's source (its job must name this exact path), then Orbit forgets it as a
 * manual source, uninstalls it so its icon goes too, and scans again. 0 deleted,
 * -1 kept, -2 unconfirmed (the storage job is still being followed). *icon_left: deleted, but
 * ShadowMount didn't take the icon off the home screen. */
static int delete_game(int port, const cJSON *caps, const cJSON *g, bool *icon_left, char *error, size_t cap) {
    const char *path = json_text(g, "path"), *id = json_text(g, "titleId");
    struct stat original;
    if (library_source_identity(path, &original)) {
        copy_text(error, cap, "This game's source is not on an available Orbit drive. Reconnect the drive and try again."); return -1;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "mounted"))) {
        if (!has(caps, "unmount_game")) { copy_text(error, cap, "Update ShadowMount to delete a mounted game."); return -1; }
        cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "title_id", id);
        char *encoded = cJSON_PrintUnformatted(body); cJSON_Delete(body);
        cJSON *r = encoded ? read_api(port, "games/unmount", encoded, error, cap) : NULL; free(encoded);
        bool unmounted = success(r) && !strcmp(json_text(r, "title_id"), id) &&
                         cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(r, "mounted"));
        cJSON_Delete(r);
        if (!unmounted) { copy_text(error, cap, "ShadowMount couldn't unmount this game, so it was kept. Close the game and try again."); return -1; }
    }
    if (!library_source_unchanged(path, &original)) { copy_text(error, cap, "This game's source changed. Select it again."); return -1; }
    /* games/delete, as ShadowMount's own Delete source: its sources/delete is for the file
     * browser and refuses an image inside a scan folder, where most games are. */
    cJSON *remove = cJSON_CreateObject();
    cJSON_AddStringToObject(remove, "title_id", id); cJSON_AddBoolToObject(remove, "confirm", true);
    char *encoded = cJSON_PrintUnformatted(remove); cJSON_Delete(remove);
    if (cap) *error = 0;
    cJSON *response = encoded ? read_api(port, "games/delete", encoded, error, cap) : NULL; free(encoded);
    int rc;
    /* ShadowMount answers a refusal with an HTTP error and its reason: nothing started. */
    if ((response && !success(response)) || (!response && !strncmp(error, "ShadowMount: ", 13))) {
        copy_text(error, cap, "ShadowMount could not delete this game. It was kept."); rc = -1;
    }
    else rc = wait_move_job(port, response, path, NULL, true, error, cap);
    cJSON_Delete(response);
    if (rc) return rc;
    /* Only a source added by hand has an entry to forget; a scanned one has none. */
    if (has(caps, "remove_manual_source")) (void)manual_path(port, "manual/remove", path, NULL, 0);
    /* Deleting a game takes its icon off the home screen too: ShadowMount's Uninstall, the PS5's
     * own, which leaves saved data in the console's Saved Data. The files are already gone, so a
     * refusal only leaves the icon. */
    *icon_left = false;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "installed"))) {
        cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "title_id", id);
        char *text = has(caps, "uninstall_game") ? cJSON_PrintUnformatted(body) : NULL; cJSON_Delete(body);
        cJSON *r = text ? read_api(port, "games/uninstall", text, NULL, 0) : NULL; free(text);
        *icon_left = !success(r);
        cJSON_Delete(r);
    }
    cJSON_Delete(read_api(port, "scan", "{\"reset_attempts\":false}", NULL, 0));
    follow_scan();
    return 0;
}
static void execute_command(cJSON *input) {
    const char *action = json_text(input, "action"), *id = json_text(input, "titleId");
    bool transfer = !strcmp(action, "copy") || !strcmp(action, "move"), gate = false;
    bool deleting = !strcmp(action, "delete");
    char error[256] = {0}, ok[256] = "Library updated.";
    cJSON *v = NULL, *response = NULL, *rows = NULL, *body = NULL, *drives = NULL;
    int port = api_port();
    result_message("running", "Checking current game and drive status…");
    v = port ? read_api(port, "version", "{}", error, sizeof error) : NULL;
    if (!success(v) || !number(v, "api_version") || json_int(v, "api_version") != 1 ||
        !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(v, "capabilities")) ||
        !has(cJSON_GetObjectItemCaseSensitive(v, "capabilities"), capability(action))) {
        copy_text(error, sizeof error, "This action is unavailable in the running ShadowMount version."); goto done;
    }
    if (!strcmp(action, "measure")) {
        response = read_api(port, "games", "{\"include_size\":true}", error, sizeof error);
        rows = response ? normalize(response, error, sizeof error) : NULL;
        if (!rows) goto done;
        add_installed_packages(rows, cJSON_GetObjectItemCaseSensitive(v, "capabilities"));
        pthread_mutex_lock(&lock);
        cJSON_Delete(games); games = rows; rows = NULL;
        updated = checked = time(NULL); refreshed = monotonic_seconds();
        copy_text(status, sizeof status, "ready"); message[0] = 0;
        pthread_mutex_unlock(&lock);
        copy_text(ok, sizeof ok, "Game sizes updated. Unavailable sizes are shown separately.");
        goto succeeded;
    }
    if (transfer || deleting || !strcmp(action, "cancel")) {
        if (!has(cJSON_GetObjectItemCaseSensitive(v, "capabilities"), "storage_job_status")) {
            copy_text(error, sizeof error, "ShadowMount cannot report storage progress. Update it before managing files."); goto done;
        }
        response = read_api(port, "games/storage/status", "{}", error, sizeof error);
        cJSON *job = response ? normalize_job(response) : NULL;
        cJSON_Delete(response); response = NULL;
        if (!job) { copy_text(error, sizeof error, "Cannot confirm the current storage job. Try again in a moment."); goto done; }
        bool active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "active"));
        bool can_cancel = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "cancellable"));
        double job_id = (double)json_int(job, "id");
        accept_job(job);
        if (!strcmp(action, "cancel")) {
            if (!active || !can_cancel || job_id != (double)json_int(input, "jobId")) {
                copy_text(error, sizeof error, "This job has finished or can no longer be cancelled."); goto done;
            }
            body = cJSON_CreateObject();
            cJSON_AddNumberToObject(body, "job_id", job_id);
        } else if (active) {
            copy_text(error, sizeof error, "A storage job is already running. Wait for it to finish."); goto done;
        }
    }
    if (strcmp(action, "scan") && strcmp(action, "cancel")) {
        response = read_api(port, "games", "{\"include_size\":false}", error, sizeof error);
        rows = response ? normalize(response, error, sizeof error) : NULL;
        cJSON_Delete(response); response = NULL;
        if (!rows) goto done;
        cJSON *g = NULL, *candidate;
        cJSON_ArrayForEach(candidate, rows)
            if (!strcmp(json_text(candidate, "titleId"), id)) { g = candidate; break; }
        if (!g || strcmp(json_text(g, "sourceKey"), json_text(input, "sourceKey"))) {
            copy_text(error, sizeof error, "This game's source changed. Select it again."); goto done;
        }
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "managed")) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "onDrive")) ||
            ((transfer || deleting) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "canManageSource")))) {
            copy_text(error, sizeof error, "This source is missing or does not support that action."); goto done;
        }
        if (transfer && packages_path_busy(json_text(g, "path"))) {
            copy_text(error, sizeof error, "Wait for package installation to finish before moving or copying its source."); goto done;
        }
        if (deleting) {
            const char *path = json_text(g, "path");
            if (packages_path_busy(path)) { copy_text(error, sizeof error, "Wait for package installation to finish before deleting this game."); goto done; }
            if (storage_taken()) { copy_text(error, sizeof error, "Another file operation is running. Wait for it to finish, or cancel it."); goto done; }
            gate = true;
            storage_hold_paths(HOLD_LIBRARY, "", path);
            pthread_mutex_lock(&orbit.mutex);
            bool downloading = storage_path_downloading_locked(path);
            pthread_mutex_unlock(&orbit.mutex);
            if (downloading) { copy_text(error, sizeof error, "A download or transfer is using this."); goto done; }
            bool icon_left = false;
            int deleted = delete_game(port, cJSON_GetObjectItemCaseSensitive(v, "capabilities"), g, &icon_left, error, sizeof error);
            if (deleted == -2) gate = false;
            if (deleted) goto done;
            copy_text(ok, sizeof ok, icon_left ? "Game deleted. Delete its icon from the PS5 home screen." : "Game deleted.");
            goto succeeded;
        }
        bool mounted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "mounted"));
        if ((!strcmp(action, "mount") && mounted) || (!strcmp(action, "unmount") && !mounted)) {
            copy_text(ok, sizeof ok, mounted ? "Already mounted." : "Already unmounted."); goto succeeded;
        }
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "title_id", id);
        if (transfer) {
            if (mounted) { copy_text(error, sizeof error, "Unmount this game before copying or moving its source."); goto done; }
            if (storage_taken()) { copy_text(error, sizeof error, "Another file operation is running. Wait for it to finish, or cancel it."); goto done; }
            gate = true;
            response = read_api(port, "storage", "{}", error, sizeof error);
            drives = response ? normalize_storage(response) : NULL;
            cJSON_Delete(response); response = NULL;
            cJSON *destination = NULL;
            cJSON_ArrayForEach(candidate, cJSON_GetObjectItemCaseSensitive(drives, "destinations"))
                if (!strcmp(json_text(candidate, "id"), json_text(input, "destinationId"))) { destination = candidate; break; }
            if (!destination || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(destination, "readOnly"))) {
                copy_text(error, sizeof error, "The selected destination changed or is no longer writable. Choose it again."); goto done;
            }
            const char *target = json_text(destination, "path"), *path = json_text(g, "path");
            const char *base = strrchr(path, '/');
            if (!base || !strcmp(path, target) ||
                (strlen(target) > strlen(path) && !strncmp(target, path, strlen(path)) && target[strlen(path)] == '/') ||
                ((size_t)(base - path) == strlen(target) && !strncmp(target, path, strlen(target)))) {
                copy_text(error, sizeof error, "Choose a different destination outside the source folder."); goto done;
            }
            char copied[1100];
            snprintf(copied, sizeof copied, "%s%s", target, base);
            storage_hold_paths(HOLD_LIBRARY, copied, !strcmp(action, "move") ? path : "");
            pthread_mutex_lock(&orbit.mutex);
            bool downloading = storage_path_downloading_locked(copied);
            pthread_mutex_unlock(&orbit.mutex);
            if (downloading) { copy_text(error, sizeof error, "A download or transfer is using this."); goto done; }
            char *encoded = cJSON_PrintUnformatted(body);
            response = read_api(port, "games/info", encoded, error, sizeof error);
            free(encoded);
            if (!success(response) || strcmp(json_text(response, "path"), path) ||
                !number(response, "size_status") || json_int(response, "size_status") || !number(response, "size_bytes")) {
                copy_text(error, sizeof error, "Could not verify this game's source size. Try again in a moment."); goto done;
            }
            int64_t size = json_int(response, "size_bytes"), free_bytes = json_int(destination, "freeBytes");
            /* Reserve space for unfinished Orbit downloads on the destination filesystem. */
            Storage local[ORBIT_MAX_STORAGE]; size_t count = storage_list(local);
            uint64_t reserved = 0;
            const char *mount = json_text(destination, "mountPoint");
            struct stat destination_stat;
            bool local_identity = !stat(target, &destination_stat);
            dev_t written = local_identity ? destination_stat.st_dev : 0;
            pthread_mutex_lock(&orbit.mutex);
            for (size_t i = 0; i < count; i++)
                if (local_identity ? local[i].device == destination_stat.st_dev :
                    (!strncmp(local[i].root, mount, strlen(mount)) && local[i].root[strlen(mount)] == '/')) {
                    uint64_t n = storage_pending_locked(&local[i], NULL);
                    reserved = n > UINT64_MAX - reserved ? UINT64_MAX : reserved + n;
                    if (!written) written = local[i].device;
                }
            bool fits = size >= 0 && free_bytes >= 0 && reserved <= (uint64_t)free_bytes &&
                        size <= free_bytes - (int64_t)reserved &&
                        free_bytes - (int64_t)reserved - size >= 16 * 1024 * 1024;
            if (fits && written) {
                orbit.holds[HOLD_LIBRARY].device = written;
                orbit.holds[HOLD_LIBRARY].bytes = (uint64_t)size;
            }
            pthread_mutex_unlock(&orbit.mutex);
            if (!fits) {
                copy_text(error, sizeof error, "Not enough free space after unfinished downloads on the destination drive."); goto done;
            }
            cJSON_Delete(response); response = NULL;
            cJSON_AddStringToObject(body, "destination_dir", target);
            if (!strcmp(action, "move")) {
                const cJSON *caps = cJSON_GetObjectItemCaseSensitive(v, "capabilities");
                if (!has(caps, "copy_game_source") || !has(caps, "delete_game_source") ||
                    !has(caps, "add_manual_source") || !has(caps, "remove_manual_source")) {
                    copy_text(error, sizeof error, "Update ShadowMount to move games with copy verification and source registration."); goto done;
                }
                int moved = verified_move(port, body, path, target, error, sizeof error);
                if (moved == -2) gate = false;
                if (moved) goto done;
                copy_text(ok, sizeof ok, "Game moved. The copy was verified before the original was removed.");
                goto succeeded;
            }
        }
    }
    if (!strcmp(action, "scan")) {
        body = cJSON_CreateObject(); cJSON_AddBoolToObject(body, "reset_attempts", false);
    }
    char route[64];
    snprintf(route, sizeof route, "%s%s", !strcmp(action, "scan") ? "" : !strcmp(action, "cancel") ? "games/storage/" : "games/", action);
    char *encoded = cJSON_PrintUnformatted(body);
    response = read_api(port, route, encoded, error, sizeof error);
    free(encoded);
    if (!success(response)) {
        if (!*error) copy_text(error, sizeof error, *json_text(response, "error") ? json_text(response, "error") : "The action could not be confirmed. Try again in a moment.");
        if (transfer) {
            /* A timed-out POST may have started a job. Hold the gate until status is known. */
            pthread_mutex_lock(&lock); monitor_job = true; job_poll_at = 0; pthread_mutex_unlock(&lock);
            gate = false;
        }
        goto done;
    }
    if (transfer || !strcmp(action, "cancel")) {
        cJSON *job = normalize_job(response);
        if (!job) {
            copy_text(error, sizeof error, "Storage request sent, but its result is unconfirmed. Check storage progress.");
            pthread_mutex_lock(&lock); monitor_job = true; job_poll_at = 0; pthread_mutex_unlock(&lock);
            gate = false; goto done;
        }
        accept_job(job); gate = false;
        copy_text(ok, sizeof ok, !strcmp(action, "cancel") ? "Cancellation requested. Waiting for the storage job to stop." : "Storage operation started. Progress is available in Library and Storage.");
    } else if (!strcmp(action, "scan")) {
        follow_scan();
        copy_text(ok, sizeof ok, "Scan requested. Games ShadowMount finds appear here in a minute or so.");
    } else {
        if (strcmp(json_text(response, "title_id"), id) ||
            !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(response, "mounted")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "mounted")) != !strcmp(action, "mount")) {
            copy_text(error, sizeof error, "The mount result could not be confirmed. Try again in a moment."); goto done;
        }
        copy_text(ok, sizeof ok, !strcmp(action, "mount") ? "Game mounted." : "Game unmounted.");
    }
succeeded:
    result_message("complete", ok);
    pthread_mutex_lock(&lock); pending = true; storage_pending = true; pthread_mutex_unlock(&lock);
    goto cleanup;
done:
    result_message("error", *error ? error : "Could not complete the Library action. Try again in a moment.");
    pthread_mutex_lock(&lock); pending = true; pthread_mutex_unlock(&lock);
cleanup:
    if (gate) set_gate(false);
    cJSON_Delete(v); cJSON_Delete(response); cJSON_Delete(rows); cJSON_Delete(body); cJSON_Delete(drives);
}

/* Nothing else in hand: no command, no storage job, no completion scan. Locked. */
static bool idle_locked(void) {
    return !command && !monitor_job && !completion_scan_busy && !busy && !storage_busy &&
        strcmp(json_text(action_result, "state"), "queued") && strcmp(json_text(action_result, "state"), "running");
}
/* A game on a drive without a size that no automatic measure has seen yet. */
static bool unmeasured_locked(void) {
    cJSON *g;
    cJSON_ArrayForEach(g, games)
        if (!strcmp(json_text(g, "sizeStatus"), "unknown") && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "onDrive")) &&
            !has(measured, json_text(g, "sourceKey")))
            return true;
    return false;
}
/* Which automatic task is due now, if any: 's' scan, 'm' measure, 'd' a look at
 * the drives. Each inventory read is checked for games without a size once.
 * Locked; the worker asks once per wake. */
static char auto_due_locked(time_t now) {
    if (!started || stopping) return 0;
    bool ready = !strcmp(status, "ready");
    if (auto_scan_wanted && now >= auto_scan_after && ready && has(capabilities, "rescan") && idle_locked()) return 's';
    if (ready && now >= auto_measure_after && refreshed != measure_seen && has(capabilities, "list_games") && idle_locked()) {
        measure_seen = refreshed;
        if (unmeasured_locked()) return 'm';
    }
    if (now - drives_checked >= DRIVE_CHECK_GAP) return 'd';
    return 0;
}
/* A drive that wasn't connected before asks for a scan; any change re-reads games
 * and drives. The first look only learns them. */
static void check_drives(void) {
    Storage now[ORBIT_MAX_STORAGE];
    size_t n = storage_list(now);
    pthread_mutex_lock(&lock);
    bool appeared = false;
    for (size_t i = 0; i < n; i++) {
        bool known = false;
        for (size_t k = 0; k < drives_seen_count; k++)
            if (!strcmp(drives_seen[k], now[i].id)) known = true;
        if (!known && drives_known) appeared = true;
    }
    bool changed_set = drives_known && (appeared || n != drives_seen_count);
    drives_seen_count = n;
    for (size_t i = 0; i < n; i++) copy_text(drives_seen[i], sizeof drives_seen[i], now[i].id);
    drives_known = true;
    if (appeared) auto_scan_wanted = true;
    if (changed_set && !strcmp(status, "ready")) pending = storage_pending = true;
    pthread_mutex_unlock(&lock);
}
static void auto_scan(void) {
    /* Kraken Mount scans by itself every few seconds; asking just makes it sooner. */
    if (kraken_active()) kraken_scan_soon();
    int port = api_port();
    cJSON *response = port ? read_api(port, "scan", "{\"reset_attempts\":false}", NULL, 0) : NULL;
    bool ok = success(response);
    cJSON_Delete(response);
    if (ok) follow_scan();
    pthread_mutex_lock(&lock);
    auto_scanning = false;
    pthread_mutex_unlock(&lock);
}
static void auto_measure(void) {
    int port = api_port();
    char error[256] = {0};
    cJSON *v = port ? read_api(port, "version", "{}", NULL, 0) : NULL;
    cJSON *response = success(v) ? read_api(port, "games", "{\"include_size\":true}", error, sizeof error) : NULL;
    cJSON *rows = response ? normalize(response, error, sizeof error) : NULL;
    if (rows) add_installed_packages(rows, cJSON_GetObjectItemCaseSensitive(v, "capabilities"));
    cJSON_Delete(response);
    cJSON_Delete(v);
    pthread_mutex_lock(&lock);
    if (rows) {
        cJSON_Delete(games);
        games = rows;
        updated = checked = time(NULL);
        refreshed = monotonic_seconds();
        /* Games ShadowMount couldn't measure wait for the next new game, not a loop. */
        cJSON *keys = cJSON_CreateArray(), *g;
        cJSON_ArrayForEach(g, games) cJSON_AddItemToArray(keys, cJSON_CreateString(json_text(g, "sourceKey")));
        cJSON_Delete(measured);
        measured = keys;
    } else {
        auto_measure_after = monotonic_seconds() + 300;
    }
    auto_measuring = false;
    pthread_mutex_unlock(&lock);
}
void library_auto_scan(void) {
    pthread_mutex_lock(&lock);
    auto_scan_wanted = true;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
}
static void *run(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        char task = 0;
        while (!pending && !storage_pending && !command && !stopping &&
               !(monitor_job && monotonic_seconds() >= job_poll_at) &&
               !(follow_until && monotonic_seconds() >= follow_next) && !(task = auto_due_locked(monotonic_seconds()))) {
            struct timespec deadline = {.tv_sec = time(NULL) + 1, .tv_nsec = 0};
            pthread_cond_timedwait(&changed, &lock, &deadline);
        }
        if (stopping) break;
        time_t now = monotonic_seconds();
        if (follow_until && now >= follow_next && !pending) {
            /* While ShadowMount registers a scan's games: read the inventory again. */
            pending = true;
            follow_next = now + 10;
            if (now >= follow_until) follow_until = 0;
        }
        /* Asked-for work first; a skipped measure is checked again after the next read. */
        if (command || pending || storage_pending || (monitor_job && now >= job_poll_at)) task = 0;
        if (task == 's') {
            auto_scan_wanted = false;
            auto_scanning = true;
            auto_scan_after = now + auto_gap(AUTO_SCAN_GAP);
            pthread_mutex_unlock(&lock);
            auto_scan();
            pthread_mutex_lock(&lock);
            continue;
        }
        if (task == 'm') {
            auto_measuring = true;
            auto_measure_after = now + auto_gap(AUTO_MEASURE_GAP);
            pthread_mutex_unlock(&lock);
            auto_measure();
            pthread_mutex_lock(&lock);
            continue;
        }
        if (task == 'd') {
            drives_checked = now;
            pthread_mutex_unlock(&lock);
            check_drives();
            pthread_mutex_lock(&lock);
            continue;
        }
        if (command) {
            cJSON *input = command; command = NULL;
            pthread_mutex_unlock(&lock);
            execute_command(input);
            cJSON_Delete(input);
            pthread_mutex_lock(&lock);
            continue;
        }
        if (monitor_job && monotonic_seconds() >= job_poll_at) {
            pthread_mutex_unlock(&lock);
            read_job(api_port());
            pthread_mutex_lock(&lock);
            continue;
        }
        if (!pending && storage_pending) {
            storage_pending = false; storage_busy = true;
            bool can_read = has(capabilities, "storage_space");
            bool can_track = has(capabilities, "storage_job_status");
            pthread_mutex_unlock(&lock);
            if (can_read) read_storage(api_port());
            if (can_track) read_job(api_port());
            pthread_mutex_lock(&lock);
            if (!can_read) {
                storage_stale = true;
                copy_text(storage_error, sizeof storage_error, "Storage management needs a compatible ShadowMount version.");
            }
            storage_busy = false;
            continue;
        }
        pending = false;
        busy = true;
        pthread_mutex_unlock(&lock);
        const char *next_status = "unavailable";
        const char *error = "Run ShadowMount with its local HTTP API enabled to see installed games and games on your drives.";
        char next_version[48] = "";
        cJSON *next = NULL;
        cJSON *next_capabilities = NULL;
        int port = api_port();
        cJSON *v = port ? read_api(port, "version", "{}", NULL, 0) : NULL;
        char detail[256] = {0};
        if (success(v)) {
            const cJSON *api = cJSON_GetObjectItemCaseSensitive(v, "api_version");
            if (cJSON_IsNumber(api) && api->valuedouble == 1) {
                const cJSON *caps = cJSON_GetObjectItemCaseSensitive(v, "capabilities");
                next_capabilities = cJSON_IsArray(caps) && cJSON_GetArraySize(caps) <= 64 ? cJSON_Duplicate(caps, true) : cJSON_CreateArray();
                copy_text(next_version, sizeof next_version, json_text(v, "shadowmount_version"));
                cJSON *response = read_api(port, "games", "{\"include_size\":false}", detail, sizeof detail);
                next = response ? normalize(response, detail, sizeof detail) : NULL;
                if (next) add_installed_packages(next, next_capabilities);
                cJSON_Delete(response);
                next_status = next ? "ready" : "error";
                error = next ? "" : *detail ? detail : "ShadowMount did not return a complete, supported inventory. Try again in a moment.";
            } else {
                next_status = "unsupported";
                error = "This ShadowMount API version is not supported. Library needs the v1 games API with installed-game status.";
            }
        }
        cJSON_Delete(v);
        /* No ShadowMount: Kraken Mount, when it runs, is what adds games instead. */
        const char *next_provider = "ShadowMount";
        if (!next && kraken_active()) {
            cJSON *list = kraken_games();
            if (list) {
                next = normalize_kraken(list);
                add_installed_packages(next, NULL);
                next_capabilities = next_capabilities ? next_capabilities : cJSON_CreateArray();
                kraken_version(next_version, sizeof next_version);
                next_provider = "Kraken Mount";
                next_status = "ready";
                error = "";
            }
            cJSON_Delete(list);
        }
        pthread_mutex_lock(&lock);
        checked = time(NULL);
        copy_text(provider, sizeof provider, next_provider);
        if (next) {
            preserve_sizes(next);
            cJSON_Delete(games);
            games = next;
            updated = checked;
            refreshed = monotonic_seconds();
            copy_text(version, sizeof version, next_version);
            /* Discover an existing provider job after Orbit restarts, even if
             * the user stays in Discover instead of opening Storage. */
            if (!storage_attempted && has(next_capabilities, "storage_job_status")) {
                storage_pending = true;
                storage_attempted = monotonic_seconds();
            }
        }
        cJSON_Delete(capabilities);
        capabilities = next_capabilities;
        copy_text(status, sizeof status, next_status);
        copy_text(message, sizeof message, error);
        busy = false;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
static void load_dismissed(void);
int library_start(void) {
    load_dismissed();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    pthread_mutex_lock(&lock);
    int rc = pthread_create(&worker, &attr, run, NULL);
    started = !rc;
    pthread_mutex_unlock(&lock);
    pthread_attr_destroy(&attr);
    return rc;
}
void library_stop(void) {
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
    if (started) pthread_join(worker, NULL);
    /* A status request admitted just before shutdown can still be finishing. */
    pthread_mutex_lock(&lock);
    started = false;
    cJSON_Delete(games);
    games = NULL;
    cJSON_Delete(capabilities); cJSON_Delete(storage); cJSON_Delete(storage_job);
    cJSON_Delete(command); cJSON_Delete(last_request); cJSON_Delete(action_result); cJSON_Delete(measured);
    capabilities = storage = storage_job = command = last_request = action_result = measured = NULL;
    pthread_mutex_unlock(&lock);
}
/* A finished copy or move someone dismissed. ShadowMount keeps reporting its most recent job;
 * this hides that one result in every interface, across restarts, until another job comes. */
#define DISMISSED "library-dismissed.txt"
static char dismissed[1200];
static void job_key(const cJSON *job, char *out, size_t cap) {
    snprintf(out, cap, "%lld|%s|%s|%s", (long long)json_int(job, "id"), json_text(job, "operation"),
             json_text(job, "source"), json_text(job, "destination"));
}
static bool dismissed_job(const cJSON *job) {
    char key[sizeof dismissed];
    if (!job || !*dismissed || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "active"))) return false;
    job_key(job, key, sizeof key);
    return !strcmp(key, dismissed);
}
static void load_dismissed(void) {
    int fd = openat(orbit.state_fd, DISMISSED, O_RDONLY | O_NOFOLLOW);
    ssize_t n = fd < 0 ? -1 : read(fd, dismissed, sizeof dismissed - 1);
    dismissed[n > 0 ? n : 0] = 0;
    if (fd >= 0) close(fd);
}
static int save_dismissed(void) {
    int fd = openat(orbit.state_fd, DISMISSED ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    size_t n = strlen(dismissed);
    int rc = fd < 0 || write(fd, dismissed, n) != (ssize_t)n || fsync(fd) ? -1 : 0;
    if (fd >= 0) close(fd);
    return rc || renameat(orbit.state_fd, DISMISSED ".tmp", orbit.state_fd, DISMISSED) ? -1 : 0;
}
static int dismiss(const cJSON *input, char *error, size_t cap) {
    if (!number(input, "jobId") || json_int(input, "jobId") <= 0) {
        copy_text(error, cap, "Expected a supported action and its current Library identifiers."); return 400;
    }
    pthread_mutex_lock(&lock);
    int code = 202;
    /* A result already gone, or replaced by a newer job, needs nothing more. */
    if (storage_job && json_int(storage_job, "id") == json_int(input, "jobId")) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(storage_job, "active"))) {
            code = 409; copy_text(error, cap, "Another Library operation is in progress. Wait for it to finish.");
        } else {
            char previous[sizeof dismissed];
            copy_text(previous, sizeof previous, dismissed);
            job_key(storage_job, dismissed, sizeof dismissed);
            if (save_dismissed()) {
                copy_text(dismissed, sizeof dismissed, previous);
                code = 503; copy_text(error, cap, "Could not save history changes.");
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return code;
}

cJSON *library_snapshot(bool refresh) {
    pthread_mutex_lock(&lock);
    time_t now = monotonic_seconds();
    if (started && !stopping && !busy && !pending &&
        (!attempted || now - attempted >= (refresh ? 5 : 30))) {
        pending = true;
        attempted = now;
        pthread_cond_signal(&changed);
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "status", started ? status : "unavailable");
    cJSON_AddStringToObject(out, "message", started ? message : "Library is unavailable. Restart Orbit to try again.");
    cJSON_AddStringToObject(out, "provider", provider);
    cJSON_AddStringToObject(out, "providerVersion", version);
    cJSON_AddBoolToObject(out, "busy", busy || pending);
    cJSON_AddBoolToObject(out, "stale", updated && (strcmp(status, "ready") || now - refreshed >= 30));
    cJSON_AddNumberToObject(out, "checkedAt", (double)checked);
    cJSON_AddNumberToObject(out, "updatedAt", (double)updated);
    cJSON_AddNumberToObject(out, "refreshAfter", attempted && now - attempted < 5 ? 5 - (now - attempted) : 0);
    cJSON_AddItemToObject(out, "games", games ? cJSON_Duplicate(games, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(out, "capabilities", capabilities ? cJSON_Duplicate(capabilities, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(out, "action", action_result ? cJSON_Duplicate(action_result, true) : cJSON_CreateNull());
    cJSON_AddItemToObject(out, "storageJob", storage_job && !dismissed_job(storage_job)
                                                 ? cJSON_Duplicate(storage_job, true) : cJSON_CreateNull());
    cJSON_AddStringToObject(out, "storageError", storage_error);
    cJSON_AddBoolToObject(out, "storageBusy", monitor_job);
    /* Automatic work under way: the interfaces show it instead of buttons. */
    cJSON_AddBoolToObject(out, "scanning", auto_scanning || (follow_until && now < follow_until));
    cJSON_AddBoolToObject(out, "measuring", auto_measuring);
    pthread_mutex_unlock(&lock);
    return out;
}

/* Free up space: games whose source is on the drive at root and that ShadowMount can
 * delete for Orbit (a game installed from a package isn't one). */
cJSON *library_space_games(const char *root) {
    cJSON *out = cJSON_CreateArray(), *g;
    size_t n = strlen(root);
    pthread_mutex_lock(&lock);
    bool able = started && !stopping && !strcmp(status, "ready") && has(capabilities, "delete_game_source") &&
                has(capabilities, "storage_job_status");
    cJSON *list = able ? games : NULL;
    cJSON_ArrayForEach(g, list) {
        const char *path = json_text(g, "path");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(g, "sizeBytes");
        bool mounted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "mounted"));
        /* Orbit's own TV app is never offered: deleting it would remove the app in use. */
        if (!strcmp(json_text(g, "titleId"), "PPSA99177") ||
            strncmp(path, root, n) || path[n] != '/' || !cJSON_IsNumber(size) || size->valuedouble <= 0 ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "onDrive")) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "managed")) ||
            !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "canManageSource")) ||
            (mounted && !has(capabilities, "unmount_game"))) continue;
        cJSON *item = cJSON_CreateObject();
        const char *keys[] = {"titleId", "title", "sourceKey", "format", "cover"};
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            cJSON_AddStringToObject(item, keys[i], json_text(g, keys[i]));
        cJSON_AddNumberToObject(item, "sizeBytes", size->valuedouble);
        cJSON_AddBoolToObject(item, "installed", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "installed")));
        cJSON_AddBoolToObject(item, "mounted", mounted);
        cJSON_AddItemToArray(out, item);
    }
    pthread_mutex_unlock(&lock);
    return out;
}
/* The Library action with this request ID: {state, message}, or NULL when another
 * action has replaced it. */
cJSON *library_action_result(const char *request) {
    pthread_mutex_lock(&lock);
    cJSON *out = action_result && !strcmp(json_text(action_result, "id"), request) ? cJSON_Duplicate(action_result, true) : NULL;
    pthread_mutex_unlock(&lock);
    return out;
}
cJSON *library_storage_snapshot(bool refresh) {
    cJSON_Delete(library_snapshot(false));
    pthread_mutex_lock(&lock);
    time_t now = monotonic_seconds();
    if (started && !stopping && !storage_busy && !storage_pending &&
        (!storage_attempted || now - storage_attempted >= (refresh ? 5 : 30))) {
        storage_pending = true; storage_attempted = now;
        pthread_cond_signal(&changed);
    }
    cJSON *out = storage ? cJSON_Duplicate(storage, true) : cJSON_CreateObject();
    if (!storage) { cJSON_AddArrayToObject(out, "drives"); cJSON_AddArrayToObject(out, "destinations"); }
    cJSON_AddBoolToObject(out, "busy", storage_busy || storage_pending);
    cJSON_AddBoolToObject(out, "stale", storage_stale || (storage_attempted && now - storage_attempted >= 30));
    cJSON_AddNumberToObject(out, "updatedAt", (double)storage_updated);
    cJSON_AddStringToObject(out, "error", storage_error);
    pthread_mutex_unlock(&lock);
    return out;
}
int library_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action"), *id = json_text(input, "titleId");
    if (!strcmp(action, "dismiss"))
        return dismiss(input, error, cap);
    const char *request = json_text(input, "requestId");
    if (!*capability(action) || !text_field(input, "requestId", 64) || strlen(request) < 8 ||
        (strcmp(action, "scan") && strcmp(action, "measure") && strcmp(action, "cancel") &&
         (!title_id(id) || !text_field(input, "sourceKey", 64) || strlen(json_text(input, "sourceKey")) != 64)) ||
        ((!strcmp(action, "copy") || !strcmp(action, "move")) &&
         (!text_field(input, "destinationId", 64) || strlen(json_text(input, "destinationId")) != 64)) ||
        (!strcmp(action, "cancel") && (!number(input, "jobId") || json_int(input, "jobId") <= 0))) {
        copy_text(error, cap, "Expected a supported action and its current Library identifiers."); return 400;
    }
    if ((!strcmp(action, "scan") || !strcmp(action, "move") || !strcmp(action, "unmount") || !strcmp(action, "delete")) &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "confirmed"))) {
        copy_text(error, cap, "Confirm this action first."); return 400;
    }
    pthread_mutex_lock(&lock);
    int code = 202;
    if (!started || stopping) { code = 503; copy_text(error, cap, "Library is unavailable."); }
    else if (!strcmp(action, "cancel") && atomic_load(&verified_move_active) &&
             json_int(input, "jobId") == json_int(storage_job, "id")) {
        if (verified_move_finalizing) { code = 409; copy_text(error, cap, "The verified move is finishing and can no longer be cancelled."); }
        else atomic_store(&verified_move_cancel, true);
    }
    else if (!strcmp(request, json_text(last_request, "requestId"))) {
        if (!cJSON_Compare(input, last_request, true)) { code = 409; copy_text(error, cap, "This request ID was already used for another action."); }
    } else if (completion_scan_busy || command || !strcmp(json_text(action_result, "state"), "running") ||
               !strcmp(json_text(action_result, "state"), "queued") || (monitor_job && strcmp(action, "cancel"))) {
        code = 409; copy_text(error, cap, "Another Library operation is in progress. Wait for it to finish.");
    } else if (strcmp(status, "ready") || !has(capabilities, capability(action))) {
        code = 409; copy_text(error, cap, "This action needs a compatible ShadowMount version.");
    } else {
        command = cJSON_Duplicate(input, true);
        cJSON_Delete(last_request); last_request = cJSON_Duplicate(input, true);
        cJSON_Delete(action_result); action_result = cJSON_CreateObject();
        cJSON_AddStringToObject(action_result, "id", request);
        cJSON_AddStringToObject(action_result, "action", action);
        cJSON_AddStringToObject(action_result, "titleId", id);
        cJSON_AddStringToObject(action_result, "state", "queued");
        cJSON_AddStringToObject(action_result, "message", "Waiting to start…");
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&lock);
    return code;
}

/* Completion scans are requested by Orbit's download/receiver worker. A scan
 * can register/mount games, so wait while Library is moving files. The response
 * acknowledges a request; only a matching installed inventory row confirms it. */
bool library_package_waiting(void) {
    pthread_mutex_lock(&lock);
    bool wait = command || monitor_job || completion_scan_busy ||
        !strcmp(json_text(action_result, "state"), "queued") || !strcmp(json_text(action_result, "state"), "running");
    pthread_mutex_unlock(&lock); return wait;
}
static int scan_finished(int result) {
    pthread_mutex_lock(&lock); completion_scan_busy = false; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
    return result;
}
/* manual: the game is nested or outside homebrew. ShadowMount 1.7beta5+ adds it
 * by path (add_manual_source); otherwise discovery depends on its scan depth. */
int library_scan_completed(const char *path, bool request, bool manual, char *message, size_t cap) {
    pthread_mutex_lock(&lock);
    bool unavailable = !started || stopping;
    bool occupied = completion_scan_busy || command || monitor_job || storage_busy ||
        !strcmp(json_text(action_result, "state"), "queued") || !strcmp(json_text(action_result, "state"), "running");
    if (!unavailable && !occupied) completion_scan_busy = true;
    pthread_mutex_unlock(&lock);
    if (unavailable) { copy_text(message, cap, "Start ShadowMount with its local API enabled, then select Scan with ShadowMount."); return -1; }
    if (occupied) { copy_text(message, cap, "Waiting for the current Library action before scanning with ShadowMount."); return 2; }
    int port = api_port();
    cJSON *v = port ? read_api(port, "version", "{}", NULL, 0) : NULL;
    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(v, "capabilities");
    bool by_path = manual && has(caps, "add_manual_source");
    bool compatible = success(v) && json_int(v, "api_version") == 1 && has(caps, "list_games") &&
        (by_path || has(caps, "rescan"));
    bool reachable = !manual || by_path || strstr(path, "/homebrew/");
    cJSON_Delete(v);
    if (!compatible) { copy_text(message, cap, "ShadowMount is unavailable. Start it with its local API enabled, then select Scan with ShadowMount."); return scan_finished(-1); }
    if (!reachable) { copy_text(message, cap, "This ShadowMount version adds games only from a homebrew folder. Update ShadowMount, or move the game into homebrew."); return scan_finished(-1); }
    if (request) {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "path", path);
        char *add = by_path ? cJSON_PrintUnformatted(body) : NULL;
        cJSON_Delete(body);
        cJSON *response = by_path ? (add ? read_api(port, "manual/add", add, message, cap) : NULL)
                                  : read_api(port, "scan", "{\"reset_attempts\":false}", message, cap);
        free(add);
        bool ok = success(response); cJSON_Delete(response);
        if (!ok) { copy_text(message, cap, by_path ? "ShadowMount could not add this game. Check Library before trying again."
                                                   : "Could not confirm the ShadowMount scan. Check Library before trying again."); return scan_finished(-1); }
        follow_scan();
    }
    cJSON *response = read_api(port, "games", "{\"include_size\":false}", NULL, 0);
    bool registered = false;
    if (success(response)) {
        cJSON *g;
        cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(response, "games")) {
            const char *found = json_text(g, "path");
            if (!strcmp(found, path) &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(g, "installed"))) registered = true;
        }
    }
    cJSON_Delete(response);
    pthread_mutex_lock(&lock); pending = true; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
    copy_text(message, cap, registered ? "Registered by ShadowMount. Check your PS5 home screen." : "Scan requested. Waiting for ShadowMount to register the game…");
    return scan_finished(registered ? 0 : 1);
}
