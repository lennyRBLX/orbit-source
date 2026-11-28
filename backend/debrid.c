/* TorBox web downloads. Credentials and signed CDN URLs stay in the backend. */
#include "debrid.h"
#include "ca.h"
#include "provider_network.h"
#include "remote_sources.h"
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define KEY_CAP 513
#define RESPONSE_MAX (1024 * 1024)
#define HOST_TTL 600
#define HOST_COUNT 5
#define CACHE_PATH "/webdl/checkcached?"
/* How long TorBox gets to list a download whose create answer was lost before Orbit
 * decides the request never arrived and sends it again. */
#define SUBMIT_GRACE 120
static const char *const host_sources[HOST_COUNT] = {"archive", "vikingfile", "datanodes", "1fichier", "akirabox"};
static int domain_host(const char *domain) {
    if (!strcmp(domain, "archive.org")) return 0;
    if (!strcmp(domain, "vikingfile.com") || !strcmp(domain, "vik1ngfile.site")) return 1;
    if (!strcmp(domain, "datanodes.to")) return 2;
    if (!strcmp(domain, "1fichier.com")) return 3;
    if (!strcmp(domain, "akirabox.com") || !strcmp(domain, "akirabox.to")) return 4;
    return -1;
}
typedef struct {
    char name[254];
    bool available;
    int64_t limit;
} DebridHost;

static const char *bare_domain(const char *domain) {
    return !strncasecmp(domain, "www.", 4) ? domain + 4 : domain;
}

static struct {
    pthread_t thread;
    bool started, stop, busy, connected, check;
    char key[KEY_CAP], pending_key[KEY_CAP], account[65], error[256];
    time_t checked, retry_at, next_check, cache_retry_at;
    unsigned revision;
    struct { bool available; int64_t limit; } hosts[HOST_COUNT];
    DebridHost *community_hosts;
    size_t community_host_count;
} debrid;
typedef struct {
    char *body;
    size_t length;
    long status;
    time_t retry_at;
    Job *job;
} Reply;
static const char *api_base(void) {
#ifdef ORBIT_TEST
    const char *base = getenv("ORBIT_TEST_TORBOX_URL");
    return base && !strncmp(base, "http://127.0.0.1:", 17) ? base : NULL;
#else
    return "https://api.torbox.app/v1/api";
#endif
}
static bool halted(Job *job) {
    pthread_mutex_lock(&orbit.mutex);
    const Release *mirror = job && job->mirror_id[0] ? release_find(job->mirror_id) : NULL;
    bool stop = orbit.stop || debrid.stop || (job && (job->pause || job->cancel ||
                !debrid_job_account_locked(job) || !source_enabled_locked(&job->release) ||
                (job->mirror_id[0] && (!mirror || !source_enabled_locked(mirror)))));
    pthread_mutex_unlock(&orbit.mutex);
    return stop;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return halted(((Reply *)ctx)->job);
}
static size_t collect(char *data, size_t a, size_t b, void *ctx) {
    Reply *r = ctx;
    if (a && b > SIZE_MAX / a) return 0;
    size_t n = a * b;
    if (n > RESPONSE_MAX - r->length) return 0;
    char *next = realloc(r->body, r->length + n + 1);
    if (!next) return 0;
    r->body = next;
    memcpy(next + r->length, data, n); r->length += n; next[r->length] = 0;
    return n;
}
static size_t headers(char *data, size_t a, size_t b, void *ctx) {
    Reply *r = ctx;
    if (a && b > SIZE_MAX / a) return 0;
    size_t n = a * b;
    if (n > 12 && !strncasecmp(data, "Retry-After:", 12)) {
        char value[100]; size_t len = n - 12;
        if (len < sizeof value) {
            memcpy(value, data + 12, len); value[len] = 0;
            char *end = NULL; long wait = strtol(value, &end, 10);
            while (end && isspace((unsigned char)*end)) end++;
            time_t at = end && end != value && !*end && wait >= 0 && wait <= 86400
                      ? time(NULL) + wait : curl_getdate(value, NULL);
            if (at > r->retry_at) r->retry_at = at;
        }
    }
    return n;
}
#define REMOTE_FILE_REMOVED "The source no longer has this file. Choose another download option if there is one."
#define TORBOX_STALLED "TorBox could not download this file from its source. The source may be offline or the file removed. Choose another download option."
/* TorBox says that a link's source has taken the file down only in prose.
 * Classify that text without relaying it. */
static bool source_removed(const char *text) {
    static const char *const signs[] = {"not found", "404", "410", "no longer exists", "no longer available",
                                        "does not exist", "doesn't exist", "dead link", "taken down",
                                        "deleted", "file removed", "source unavailable"};
    char lower[512];
    size_t n = 0;
    for (; text && text[n] && n < sizeof lower - 1; n++) lower[n] = (char)tolower((unsigned char)text[n]);
    lower[n] = 0;
    for (size_t i = 0; i < sizeof signs / sizeof *signs; i++)
        if (strstr(lower, signs[i])) return true;
    return false;
}
static bool state_has(const char *state, const char *word) {
    char lower[128];
    size_t n = 0;
    for (; state && state[n] && n < sizeof lower - 1; n++) lower[n] = (char)tolower((unsigned char)state[n]);
    lower[n] = 0;
    return strstr(lower, word) != NULL;
}
/* TorBox reports a failed web download as "error", "failed" or a variant
 * such as "failed (...)". */
static bool failed_state(const char *state) {
    return state_has(state, "fail") || state_has(state, "error") || state_has(state, "cancel") ||
           state_has(state, "unavailable") || state_has(state, "unsupported");
}
static time_t stall_limit(void) {
#ifdef ORBIT_TEST
    const char *limit = getenv("ORBIT_TEST_TORBOX_STALL");
    if (limit && *limit) return (time_t)atoi(limit);
#endif
    return 300;
}
/* A source TorBox cannot reach can leave its web download waiting with no
 * error at all. Give up after five minutes without any progress, unless TorBox
 * says it is queued or paused, which ends only when the account frees up. */
static bool stalled(Job *job, const cJSON *item, const char *state) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(item, "progress");
    const cJSON *down = cJSON_GetObjectItemCaseSensitive(item, "download_speed");
    const cJSON *up = cJSON_GetObjectItemCaseSensitive(item, "upload_speed");
    double progress = cJSON_IsNumber(p) && isfinite(p->valuedouble) ? p->valuedouble : 0;
    bool moving = (cJSON_IsNumber(down) && isfinite(down->valuedouble) && down->valuedouble > 0) ||
                  (cJSON_IsNumber(up) && isfinite(up->valuedouble) && up->valuedouble > 0);
    bool waiting = state_has(state, "queue") || state_has(state, "pause");
    time_t now = time(NULL);
    pthread_mutex_lock(&orbit.mutex);
    bool fresh = !job->debrid_watch || progress > job->debrid_progress || moving || waiting || progress >= 1;
    if (fresh) { job->debrid_progress = progress; job->debrid_watch = now; }
    bool expired = !fresh && now - job->debrid_watch >= stall_limit();
    pthread_mutex_unlock(&orbit.mutex);
    return expired;
}
static void message(const cJSON *json, long status, char *err, size_t cap) {
    const char *code = json_text(json, "error");
    const char *text = !strcmp(code, "BAD_TOKEN") || !strcmp(code, "NO_AUTH")
        ? "TorBox rejected the API key. Reconnect your account."
        : status == 429 ? "TorBox rate limit reached. Wait before retrying."
        : !strcmp(code, "PLAN_RESTRICTED_FEATURE") ? "Your TorBox plan does not include this download feature."
        : !strcmp(code, "DOWNLOAD_TOO_LARGE") ? "This file exceeds your TorBox download limit."
        : status == 401 || status == 403 ? "TorBox rejected the API key. Reconnect your account."
        : !strcmp(code, "ITEM_NOT_FOUND") ? "This download is no longer in your TorBox account."
        : !strcmp(code, "DUPLICATE_ITEM") ? "This link is already in your TorBox account. Retry to check it."
        : "TorBox could not prepare this file. Check your account and try again.";
    /* Never relay upstream text: it may contain credentials, a URL or account data. */
    copy_text(err, cap, text);
}
static cJSON *call(const char *key, const char *path, const char *link, bool bearer,
                   Job *job, time_t *retry, char *err, size_t cap, DebridResult *outcome) {
    if (outcome) *outcome = DEBRID_TRANSIENT;
    const char *base = api_base();
    if (!base || halted(job)) { if (outcome) *outcome = DEBRID_FAILED; copy_text(err, cap, "TorBox request stopped."); return NULL; }
    char url[4096], authorization[KEY_CAP + 32];
    if (snprintf(url, sizeof url, "%s%s", base, path) >= (int)sizeof url) return NULL;
    Reply reply = {.job = job};
    CURL *curl = curl_easy_init(); struct curl_slist *h = NULL;
    if (!curl) { copy_text(err, cap, "Could not initialize TorBox HTTPS."); return NULL; }
    curl_mime *form = NULL;
    if (link) {
        form = curl_mime_init(curl);
        curl_mimepart *url_part = form ? curl_mime_addpart(form) : NULL;
        curl_mimepart *queued_part = form ? curl_mime_addpart(form) : NULL;
        if (!url_part || !queued_part || curl_mime_name(url_part, "link") ||
            curl_mime_data(url_part, link, CURL_ZERO_TERMINATED) || curl_mime_name(queued_part, "as_queued") ||
            curl_mime_data(queued_part, "false", CURL_ZERO_TERMINATED) ||
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, form)) {
            curl_easy_cleanup(curl);
            curl_mime_free(form);
            if (outcome) *outcome = DEBRID_FAILED;
            copy_text(err, cap, "Could not prepare the TorBox request.");
            return NULL;
        }
    }
    if (bearer) {
        snprintf(authorization, sizeof authorization, "Authorization: Bearer %s", key);
        h = curl_slist_append(h, authorization);
        OPENSSL_cleanse(authorization, sizeof authorization);
    }
    h = curl_slist_append(h, "Accept: application/json");
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ORBIT_TEST
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#endif
    /* Authentication is sent to the fixed API only; never follow its redirects. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, provider_open_socket);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OrbitStore/" ORBIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    bool cache_lookup = !strncmp(path, CACHE_PATH, sizeof CACHE_PATH - 1);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cache_lookup ? 4L : 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cache_lookup ? 8L : 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &reply);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &reply);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
    curl_easy_cleanup(curl);
    curl_mime_free(form);
    for (struct curl_slist *p = h; p; p = p->next) OPENSSL_cleanse(p->data, strlen(p->data));
    curl_slist_free_all(h); OPENSSL_cleanse(url, sizeof url);
    cJSON *json = rc == CURLE_OK && reply.body ? cJSON_Parse(reply.body) : NULL;
    if (reply.body) { OPENSSL_cleanse(reply.body, reply.length); free(reply.body); }
    if (reply.status == 429 && reply.retry_at < time(NULL) + 60) reply.retry_at = time(NULL) + 60;
    if (reply.retry_at > *retry) *retry = reply.retry_at;
    bool success_status = reply.status >= 200 && reply.status < 300;
    if (rc != CURLE_OK || !success_status || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "success"))) {
        /* A complete rejection can be retried. A lost response or server failure
           may have created a remote item, so the queue must reconcile first. */
        const char *code = json_text(json, "error");
        bool refused = rc == CURLE_OK && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "success"));
        bool removed = link && refused && strcmp(code, "ENDPOINT_NOT_FOUND") &&
                       (source_removed(json_text(json, "detail")) || !strcmp(code, "SOURCE_NOT_FOUND") ||
                        !strcmp(code, "FILE_NOT_FOUND"));
        bool invalid_key = rc == CURLE_OK &&
            (reply.status == 401 || !strcmp(code, "BAD_TOKEN") || !strcmp(code, "NO_AUTH"));
        if (invalid_key && job) {
            pthread_mutex_lock(&orbit.mutex);
            if (!strcmp(key, debrid.key)) debrid.checked = 0;
            pthread_mutex_unlock(&orbit.mutex);
        }
        if (outcome) {
            if (removed) *outcome = DEBRID_MISSING;
            else if (reply.status == 429 || (*retry > time(NULL) && !invalid_key))
                *outcome = DEBRID_PROCESSING;
            else if (rc == CURLE_OK && (invalid_key || reply.status == 400 || reply.status == 403 ||
                     reply.status == 404 || reply.status == 410 || reply.status == 422 ||
                     (success_status && refused && strcmp(code, "DUPLICATE_ITEM"))))
                *outcome = DEBRID_FAILED;
        }
        if (rc != CURLE_OK) copy_text(err, cap, halted(job) ? "TorBox request stopped." : "Could not reach TorBox. Check your connection and retry.");
        else if (removed) copy_text(err, cap, REMOTE_FILE_REMOVED);
        else message(json, reply.status, err, cap);
        cJSON_Delete(json); return NULL;
    }
    return json;
}
static bool identifier(const cJSON *o, const char *key, int64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > 9007199254740991.0 || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (int64_t)v->valuedouble; return true;
}
static bool account_hash(const cJSON *data, char out[65]) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "id");
    if ((!cJSON_IsString(id) || !*id->valuestring || strlen(id->valuestring) > 128) && !cJSON_IsNumber(id)) return false;
    char *identity = cJSON_PrintUnformatted(id); unsigned char hash[32]; unsigned n = 0;
    bool ok = identity && EVP_Digest(identity, strlen(identity), hash, &n, EVP_sha256(), NULL) && n == 32;
    free(identity);
    if (ok) for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
    return ok;
}
static int persist(const char *key, const char *account) {
    if (!key[0]) {
        if (unlinkat(orbit.state_fd, "debrid.json", 0) && errno != ENOENT) return -1;
        return fsync(orbit.state_fd);
    }
    cJSON *o = cJSON_CreateObject(); cJSON_AddNumberToObject(o, "schema", 1);
    cJSON_AddStringToObject(o, "key", key); cJSON_AddStringToObject(o, "account", account);
    char *json = cJSON_PrintUnformatted(o);
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(o, "key");
    if (secret && secret->valuestring) OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(o); if (!json) return -1;
    char name[64], random[17]; random_hex(random, 8); snprintf(name, sizeof name, "debrid-%s.tmp", random);
    int fd = openat(orbit.state_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600), rc = -1;
    if (fd >= 0) {
        size_t size = strlen(json), done = 0;
        while (done < size) { ssize_t n = write(fd, json + done, size - done); if (n <= 0) break; done += (size_t)n; }
        if (done == size && !fsync(fd)) rc = 0;
        if (close(fd)) rc = -1;
        if (!rc) rc = renameat(orbit.state_fd, name, orbit.state_fd, "debrid.json");
        if (!rc) rc = fsync(orbit.state_fd);
        unlinkat(orbit.state_fd, name, 0);
    }
    OPENSSL_cleanse(json, strlen(json)); free(json); return rc;
}
static bool valid_key(const char *key) {
    size_t n = strlen(key); if (n < 16 || n >= KEY_CAP) return false;
    for (const unsigned char *p = (const void *)key; *p; p++)
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '=')) return false;
    return true;
}
static void pause_jobs_locked(void) {
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (strcmp(j->delivery, "torbox") || !j->id[0] || !strcmp(j->status, "complete") || !strcmp(j->status, "cancelled")) continue;
        j->pause = true;
        if (!job_active(j)) copy_text(j->status, sizeof j->status, "paused");
    }
}
static void check_account(const char *key) {
    char err[256] = {0}, account[65] = {0}; time_t retry = 0;
    bool available[HOST_COUNT] = {0}; int64_t limits[HOST_COUNT] = {0};
    DebridHost *community_hosts = NULL;
    size_t community_count = 0;
    cJSON *user = call(key, "/user/me?settings=false", NULL, true, NULL, &retry, err, sizeof err, NULL);
    bool ok = user && account_hash(cJSON_GetObjectItemCaseSensitive(user, "data"), account);
    cJSON_Delete(user);
    if (!ok && !err[0]) copy_text(err, sizeof err, "TorBox returned an invalid account response.");
    cJSON *hosters = ok ? call(key, "/webdl/hosters", NULL, true, NULL, &retry, err, sizeof err, NULL) : NULL;
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(hosters, "data"), *host;
    ok = ok && cJSON_IsArray(hosts);
    if (ok) {
        /* The same hoster can advertise several domains. Size the lookup from
         * the response so later aliases remain available to community links. */
        size_t domain_count = 0;
        cJSON_ArrayForEach(host, hosts) {
            const cJSON *domains = cJSON_GetObjectItemCaseSensitive(host, "domains");
            if (cJSON_IsArray(domains)) domain_count += (size_t)cJSON_GetArraySize(domains);
        }
        if (domain_count) {
            community_hosts = calloc(domain_count, sizeof *community_hosts);
            if (!community_hosts) {
                ok = false;
                copy_text(err, sizeof err, "Could not store TorBox host availability.");
            }
        }
    }
    cJSON_ArrayForEach(host, (ok ? hosts : NULL)) {
        int64_t limit = 0, daily = 0, used = 0, links = 0, link_used = 0;
        /* Host size metadata is advisory. Let createwebdownload decide for this account. */
        identifier(host, "per_link_size_limit", &limit);
        bool bandwidth = !identifier(host, "daily_bandwidth_limit", &daily) || !daily ||
                         (identifier(host, "daily_bandwidth_used", &used) && used < daily);
        bool count = !identifier(host, "daily_link_limit", &links) || !links ||
                     (identifier(host, "daily_link_used", &link_used) && link_used < links);
        bool up = bandwidth && count && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(host, "status"));
        int64_t ceiling = daily > used && (!limit || daily - used < limit) ? daily - used : limit;
        const cJSON *domains = cJSON_GetObjectItemCaseSensitive(host, "domains"), *domain;
        cJSON_ArrayForEach(domain, (cJSON_IsArray(domains) ? domains : NULL)) {
            if (!cJSON_IsString(domain)) continue;
            const char *name = bare_domain(domain->valuestring);
            int index = domain_host(name);
            if (index >= 0 && up) { available[index] = true; limits[index] = ceiling; }
            if (!*name || strlen(name) >= sizeof community_hosts[0].name ||
                strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-") != strlen(name))
                continue;
            size_t i = 0;
            while (i < community_count && strcasecmp(name, community_hosts[i].name)) i++;
            if (i == community_count) community_count++;
            DebridHost *entry = &community_hosts[i];
            copy_text(entry->name, sizeof entry->name, name);
            for (char *c = entry->name; *c; c++) *c = (char)tolower((unsigned char)*c);
            entry->available = up;
            entry->limit = ceiling;
        }
    }
    cJSON_Delete(hosters);
    if (!ok && !err[0]) copy_text(err, sizeof err, "TorBox returned invalid host availability.");
    pthread_mutex_lock(&orbit.mutex);
    if (ok && !debrid.stop) {
        if (*debrid.account && strcmp(debrid.account, account)) {
            pause_jobs_locked();
            if (state_save_locked()) { ok = false; copy_text(err, sizeof err, "Could not pause downloads before changing TorBox accounts."); }
        }
    }
    if (ok && !debrid.stop) {
        if (persist(key, account)) { ok = false; copy_text(err, sizeof err, "Could not save the TorBox connection on this console."); }
        else {
            if (strcmp(debrid.key, key)) debrid.cache_retry_at = 0;
            OPENSSL_cleanse(debrid.key, sizeof debrid.key);
            copy_text(debrid.key, sizeof debrid.key, key); copy_text(debrid.account, sizeof debrid.account, account);
            debrid.connected = true; debrid.checked = time(NULL);
            for (int i = 0; i < HOST_COUNT; i++) { debrid.hosts[i].available = available[i]; debrid.hosts[i].limit = limits[i]; }
            free(debrid.community_hosts);
            debrid.community_hosts = community_hosts;
            debrid.community_host_count = community_count;
            community_hosts = NULL;
            debrid.revision++;
        }
    }
    if (!ok) {
        /* A failed refresh must not leave the last successful host response
           looking usable. Keep the saved connection so it can be retried. */
        debrid.checked = 0;
        for (int i = 0; i < HOST_COUNT; i++) debrid.hosts[i].available = false;
        debrid.revision++;
    }
    copy_text(debrid.error, sizeof debrid.error, err);
    if (retry > debrid.retry_at) debrid.retry_at = retry;
    debrid.next_check = time(NULL) + (ok ? HOST_TTL - 60 : 60);
    if (debrid.next_check < debrid.retry_at) debrid.next_check = debrid.retry_at;
    debrid.busy = false;
    remote_torbox_changed_locked();
    pthread_mutex_unlock(&orbit.mutex);
    free(community_hosts);
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!debrid.stop && !orbit.stop) {
        if (!debrid.busy && debrid.connected && debrid.key[0] && time(NULL) >= debrid.next_check) {
            copy_text(debrid.pending_key, sizeof debrid.pending_key, debrid.key);
            debrid.busy = debrid.check = true;
        }
        if (debrid.check) {
            char key[KEY_CAP]; copy_text(key, sizeof key, debrid.pending_key);
            OPENSSL_cleanse(debrid.pending_key, sizeof debrid.pending_key); debrid.check = false;
            pthread_mutex_unlock(&orbit.mutex); check_account(key); OPENSSL_cleanse(key, sizeof key);
            pthread_mutex_lock(&orbit.mutex);
        } else {
            struct timespec until = {.tv_sec = time(NULL) + 1};
            pthread_cond_timedwait(&orbit.changed, &orbit.mutex, &until);
        }
    }
    pthread_mutex_unlock(&orbit.mutex); return NULL;
}
int debrid_start(void) {
    int fd = openat(orbit.state_fd, "debrid.json", O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat st; char buf[2048] = {0};
        if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size > 0 && st.st_size < (off_t)sizeof buf &&
            read(fd, buf, (size_t)st.st_size) == st.st_size) {
            cJSON *o = cJSON_Parse(buf); const char *key = json_text(o, "key"), *account = json_text(o, "account");
            if (json_int(o, "schema") == 1 && valid_key(key) && strlen(account) == 64) {
                copy_text(debrid.key, sizeof debrid.key, key); copy_text(debrid.account, sizeof debrid.account, account);
                copy_text(debrid.pending_key, sizeof debrid.pending_key, key); debrid.busy = debrid.check = true;
            }
            cJSON *secret = cJSON_GetObjectItemCaseSensitive(o, "key");
            if (secret && secret->valuestring) OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
            cJSON_Delete(o);
        }
        OPENSSL_cleanse(buf, sizeof buf); close(fd);
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc) rc = pthread_create(&debrid.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr); debrid.started = !rc;
    if (rc) debrid.busy = debrid.check = false;
    return rc ? -1 : 0;
}
void debrid_stop(void) {
    pthread_mutex_lock(&orbit.mutex); debrid.stop = true; pthread_cond_broadcast(&orbit.changed); pthread_mutex_unlock(&orbit.mutex);
    if (debrid.started) pthread_join(debrid.thread, NULL);
    pthread_mutex_lock(&orbit.mutex);
    debrid.connected = false;
    free(debrid.community_hosts);
    debrid.community_hosts = NULL;
    debrid.community_host_count = 0;
    OPENSSL_cleanse(debrid.key, sizeof debrid.key); OPENSSL_cleanse(debrid.pending_key, sizeof debrid.pending_key);
    pthread_mutex_unlock(&orbit.mutex);
}
cJSON *debrid_status(void) {
    pthread_mutex_lock(&orbit.mutex);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "provider", "torbox");
    cJSON_AddBoolToObject(o, "connected", debrid.connected);
    cJSON_AddBoolToObject(o, "busy", debrid.busy);
    cJSON_AddBoolToObject(o, "available", debrid.started);
    cJSON_AddStringToObject(o, "lastDelivery", !strcmp(orbit.last_delivery, "torbox") ? "torbox" : "direct");
    cJSON_AddStringToObject(o, "error", debrid.error);
    cJSON_AddNumberToObject(o, "checkedAt", (double)debrid.checked);
    cJSON_AddNumberToObject(o, "retryAt", (double)debrid.retry_at);
    cJSON_AddNumberToObject(o, "revision", debrid.revision);
    cJSON *hosts = cJSON_AddArrayToObject(o, "hosts");
    for (int i = 0; i < HOST_COUNT; i++) {
        cJSON *h = cJSON_CreateObject(); cJSON_AddStringToObject(h, "sourceId", host_sources[i]);
        cJSON_AddBoolToObject(h, "available", debrid.connected && debrid.hosts[i].available && time(NULL) < debrid.checked + HOST_TTL);
        cJSON_AddNumberToObject(h, "maxFileSize", (double)debrid.hosts[i].limit); cJSON_AddItemToArray(hosts, h);
    }
    cJSON *domains = cJSON_AddArrayToObject(o, "domains");
    if (debrid.connected && time(NULL) < debrid.checked + HOST_TTL)
        for (size_t i = 0; i < debrid.community_host_count; i++) {
            const DebridHost *host = &debrid.community_hosts[i];
            if (!host->available) continue;
            cJSON *item = cJSON_CreateObject();
            if (!cJSON_AddStringToObject(item, "host", host->name) ||
                !cJSON_AddNumberToObject(item, "maxFileSize", (double)host->limit) ||
                !cJSON_AddItemToArray(domains, item)) cJSON_Delete(item);
        }
    pthread_mutex_unlock(&orbit.mutex); return o;
}
int debrid_action(const cJSON *input, char *err, size_t cap) {
    const char *action = json_text(input, "action"), *key = json_text(input, "apiKey");
    if (strcmp(action, "connect") && strcmp(action, "refresh") && strcmp(action, "disconnect")) { copy_text(err, cap, "Choose connect, refresh or disconnect."); return 400; }
    if (!strcmp(action, "connect") && !valid_key(key)) { copy_text(err, cap, "Enter a valid TorBox API key."); return 400; }
    pthread_mutex_lock(&orbit.mutex); int code = 202;
    if (!debrid.started || debrid.stop || orbit.stop) { code = 503; copy_text(err, cap, "TorBox integration is unavailable."); }
    else if (debrid.busy) { code = 409; copy_text(err, cap, "A TorBox account check is already running."); }
    else if (!strcmp(action, "disconnect")) {
        if (persist("", "")) { code = 503; copy_text(err, cap, "Could not remove the saved TorBox connection."); }
        else {
            pause_jobs_locked();
            OPENSSL_cleanse(debrid.key, sizeof debrid.key); debrid.account[0] = debrid.error[0] = 0;
            debrid.connected = false; debrid.checked = 0; debrid.revision++;
            remote_torbox_changed_locked();
            if (state_save_locked()) { code = 503; copy_text(err, cap, "TorBox was disconnected, but download state could not be saved."); }
        }
    } else if (debrid.retry_at > time(NULL)) { code = 429; copy_text(err, cap, "Wait before checking TorBox again."); }
    else if (!strcmp(action, "refresh") && !debrid.key[0]) { code = 409; copy_text(err, cap, "Connect TorBox first."); }
    else {
        copy_text(debrid.pending_key, sizeof debrid.pending_key, !strcmp(action, "connect") ? key : debrid.key);
        debrid.busy = debrid.check = true; debrid.error[0] = 0;
        debrid.retry_at = time(NULL) + 10;
        pthread_cond_broadcast(&orbit.changed);
    }
    pthread_mutex_unlock(&orbit.mutex); return code;
}
/* -1 means no currently usable host. Zero is an unreported size limit. */
static int64_t covered_limit(const Release *r) {
    if (!r || !strcmp(r->id, "orbit-network-check")) return -1;
    if (!remote_page_pending(r) && !provider_supported(r) && !provider_browser_supported(r)) return -1;
    if (!strcmp(r->source, ORBIT_DIRECT_SOURCE)) return -1;
    char host[sizeof debrid.community_hosts[0].name];
    if (!remote_link_host(r, host, sizeof host)) return -1;
    int64_t limit = -1;
    size_t length = strlen(host), matched = 0;
    for (size_t i = 0; i < debrid.community_host_count; i++) {
        const DebridHost *entry = &debrid.community_hosts[i];
        size_t n = strlen(entry->name);
        if (n <= matched || n > length || (n < length && host[length - n - 1] != '.') ||
            strcmp(host + length - n, entry->name)) continue;
        matched = n;
        limit = entry->available ? entry->limit : -1;
    }
    return limit;
}
static bool current_account(void) {
    return debrid.connected && debrid.key[0] && time(NULL) < debrid.checked + HOST_TTL;
}
bool debrid_link_covered_locked(const Release *r) {
    return current_account() && covered_limit(r) >= 0;
}
unsigned debrid_cache_revision_locked(const Release *r) {
    return !debrid.busy && debrid_link_covered_locked(r) ? debrid.revision : 0;
}

bool debrid_check_cached(const Release *r, unsigned revision) {
    const char *url = provider_torbox_url(r);
    unsigned char digest[16];
    unsigned length = 0;
    if (!EVP_Digest(url, strlen(url), digest, &length, EVP_md5(), NULL) || length != sizeof digest)
        return false;
    char link_hash[33];
    for (size_t i = 0; i < sizeof digest; i++) snprintf(link_hash + 2 * i, 3, "%02x", digest[i]);
    char key[KEY_CAP];
    pthread_mutex_lock(&orbit.mutex);
    bool current = revision && revision == debrid_cache_revision_locked(r) &&
                   time(NULL) >= debrid.cache_retry_at;
    if (current) copy_text(key, sizeof key, debrid.key);
    pthread_mutex_unlock(&orbit.mutex);
    if (!current) return false;

    char err[256] = "";
    char path[96];
    snprintf(path, sizeof path, CACHE_PATH "format=object&list_files=false&hash=%s", link_hash);
    time_t retry = 0;
    cJSON *reply = call(key, path, NULL, true, NULL, &retry, err, sizeof err, NULL);
    OPENSSL_cleanse(key, sizeof key);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(reply, "data");
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(data, link_hash);
    bool cached = cJSON_IsObject(entry) && !strcasecmp(json_text(entry, "hash"), link_hash) &&
                  !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(entry, "cached"));
    cJSON_Delete(reply);
    pthread_mutex_lock(&orbit.mutex);
    current = revision == debrid_cache_revision_locked(r);
    if (current && retry > debrid.cache_retry_at) debrid.cache_retry_at = retry;
    pthread_mutex_unlock(&orbit.mutex);
    return current && cached;
}
bool debrid_job_account_locked(const Job *job) {
    return debrid.connected && debrid.key[0] && !strcmp(job->debrid_account, debrid.account);
}
bool debrid_default_locked(const Release *r) {
    Release route;
    return r && !debrid.busy && current_account() && remote_torbox_route_locked(r, &route);
}
int debrid_prepare_locked(const Release *r, Job *job, char *err, size_t cap) {
    if (!debrid.connected || !debrid.key[0]) { copy_text(err, cap, "Connect TorBox in App settings first."); return 409; }
    if (debrid.busy || time(NULL) >= debrid.checked + HOST_TTL) { copy_text(err, cap, "Refresh TorBox in App settings before starting this download."); return 409; }
    if (covered_limit(r) < 0) {
        copy_text(err, cap, "TorBox is unavailable for this option. Choose the original source or check App settings."); return 409;
    }
    copy_text(job->delivery, sizeof job->delivery, "torbox");
    copy_text(job->debrid_account, sizeof job->debrid_account, debrid.account);
    job->debrid_id = job->debrid_file = -1;
    job->debrid_submitted = false;
    job->debrid_submitted_at = job->debrid_watch = 0;
    job->debrid_progress = 0;
    job->debrid_retries = 0;
    return 200;
}
bool debrid_resume_after_missing_locked(Job *job, const Release *route) {
    if (job->direct_missing || job->debrid_fallback == DEBRID_MISSING ||
        job->debrid_fallback == DEBRID_FAILED || orbit.state_failed ||
        !source_enabled_locked(route) || debrid.busy ||
        !debrid_link_covered_locked(route) ||
        job_delete_partial_locked(job, job->error, sizeof job->error) ||
        debrid_prepare_locked(route, job, job->error, sizeof job->error) != 200)
        return false;
    job->direct_missing = true;
    job->browser_pending = false;
    job->attempts = 0;
    job->retry_at = 0;
    job->error[0] = job->delivery_phase[0] = 0;
    copy_text(job->status, sizeof job->status, "queued");
    return true;
}
static bool item_ready(const cJSON *item) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "download_present")) &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "download_finished"));
}
static bool item_failed(const cJSON *item) {
    const char *state = json_text(item, "download_state");
    return failed_state(state) || source_removed(state) || *json_text(item, "error");
}
static cJSON *find_item(cJSON *reply, Job *job, const char *original_url) {
    cJSON *data = cJSON_GetObjectItemCaseSensitive(reply, "data");
    if (!cJSON_IsArray(data)) return cJSON_IsObject(data) ? data : NULL;
    cJSON *item, *match = NULL;
    cJSON_ArrayForEach(item, data) {
        int64_t id;
        if (!identifier(item, "id", &id) || strcmp(json_text(item, "original_url"), original_url)) continue;
        if (job->debrid_id >= 0) {
            if (id == job->debrid_id) return item;
        } else {
            if (item_ready(item)) return item;
            if (!match || (item_failed(match) && !item_failed(item))) match = item;
        }
    }
    return match;
}
DebridResult debrid_resolve(Job *job, const Release *route, Release *resolved, time_t *retry_at, char *err, size_t cap) {
    const char *original_url = provider_torbox_url(route);
    const bool using_mirror = strcmp(route->id, job->release.id) != 0;
    char key[KEY_CAP], path[2048];
    DebridResult result = DEBRID_FAILED;
    pthread_mutex_lock(&orbit.mutex);
    bool account = debrid_job_account_locked(job);
    bool checking = debrid.busy && debrid.key[0] && !strcmp(job->debrid_account, debrid.account);
    bool covered = source_enabled_locked(route) && debrid_link_covered_locked(route);
    copy_text(key, sizeof key, account ? debrid.key : "");
    pthread_mutex_unlock(&orbit.mutex);
    if (!account && checking) { *retry_at = time(NULL) + 5; result = DEBRID_PROCESSING; goto end; }
    if (!account) { copy_text(err, cap, "Reconnect the original TorBox account before resuming this download."); goto end; }
    if (!covered) {
        if (checking) { *retry_at = time(NULL) + 5; result = DEBRID_PROCESSING; goto end; }
        copy_text(err, cap, "TorBox is unavailable for this option. Choose the original source or check App settings.");
        goto end;
    }
    if (halted(job)) goto end;
    snprintf(path, sizeof path, job->debrid_id >= 0 ? "/webdl/mylist?id=%lld&bypass_cache=true" : "/webdl/mylist?limit=1000&bypass_cache=true", (long long)job->debrid_id);
    DebridResult outcome;
    cJSON *list = call(key, path, NULL, true, job, retry_at, err, cap, &outcome);
    if (!list) { result = outcome; goto end; }
    cJSON *item = find_item(list, job, original_url);
    if (job->direct_missing && job->debrid_fallback == DEBRID_TRANSIENT && !item) {
        cJSON_Delete(list);
        copy_text(err, cap, "TorBox does not have a download for this source.");
        result = DEBRID_TRANSIENT;
        goto end;
    }
    if (job->debrid_id < 0 && item) {
        int64_t id;
        if (identifier(item, "id", &id)) {
            pthread_mutex_lock(&orbit.mutex); job->debrid_id = id; job->debrid_submitted = true;
            int saved = state_save_locked(); pthread_mutex_unlock(&orbit.mutex);
            if (saved) { cJSON_Delete(list); copy_text(err, cap, "Could not save the TorBox download identity."); goto end; }
        }
    }
    if (job->debrid_id < 0) {
        cJSON_Delete(list);
        pthread_mutex_lock(&orbit.mutex);
        /* An earlier request whose answer was lost: the list above, read past TorBox's
         * cache, would show it if it arrived. TorBox gets SUBMIT_GRACE seconds to list
         * it; still missing after that, it never arrived and is sent again, rather than
         * failing until the download is deleted. */
        time_t now = time(NULL), listed_by = job->debrid_submitted_at + SUBMIT_GRACE;
        bool listing = job->debrid_submitted && now < listed_by;
        time_t next = orbit.debrid_next_create;
        bool saved = true;
        if (!listing && now >= next) {
            bool was = job->debrid_submitted;
            time_t was_at = job->debrid_submitted_at;
            job->debrid_submitted = true;
            job->debrid_submitted_at = now;
            orbit.debrid_next_create = now + 61;
            saved = !state_save_locked();
            if (!saved) {
                job->debrid_submitted = was;
                job->debrid_submitted_at = was_at;
            }
        }
        pthread_mutex_unlock(&orbit.mutex);
        if (listing) { *retry_at = listed_by; result = DEBRID_PROCESSING; goto end; }
        if (!saved) { copy_text(err, cap, "Could not save the TorBox submission state."); goto end; }
        if (time(NULL) < next) { *retry_at = next; result = DEBRID_PROCESSING; goto end; }
        cJSON *created = call(key, "/webdl/createwebdownload", original_url, true, job, retry_at, err, cap, &outcome);
        int64_t id = -1;
        bool valid = created && identifier(cJSON_GetObjectItemCaseSensitive(created, "data"), "webdownload_id", &id);
        cJSON_Delete(created);
        if (!valid) {
            if (outcome != DEBRID_TRANSIENT) {
                pthread_mutex_lock(&orbit.mutex);
                job->debrid_submitted = false;
                if (*retry_at > orbit.debrid_next_create) orbit.debrid_next_create = *retry_at;
                int saved_rejection = state_save_locked();
                pthread_mutex_unlock(&orbit.mutex);
                if (saved_rejection) { copy_text(err, cap, "Could not save the TorBox retry state."); goto end; }
            }
            result = outcome;
            if (!err[0]) copy_text(err, cap, "TorBox did not return a download identity. Retry to check the account.");
            goto end;
        }
        pthread_mutex_lock(&orbit.mutex); job->debrid_id = id; int rc = state_save_locked(); pthread_mutex_unlock(&orbit.mutex);
        if (rc) { copy_text(err, cap, "Could not save the TorBox download identity."); goto end; }
        *retry_at = time(NULL) + 5; result = DEBRID_PROCESSING; goto end;
    }
    if (!item && job->debrid_submitted_at && time(NULL) < job->debrid_submitted_at + SUBMIT_GRACE) {
        cJSON_Delete(list);
        result = DEBRID_PROCESSING;
        goto end;
    }
    int64_t item_id = -1;
    if (!item || !identifier(item, "id", &item_id) || item_id != job->debrid_id ||
        strcmp(json_text(item, "original_url"), original_url)) {
        cJSON_Delete(list); copy_text(err, cap, "TorBox returned a different download identity."); goto end;
    }
    const char *state = json_text(item, "download_state");
    if (!item_ready(item) && item_failed(item)) {
        bool removed = source_removed(json_text(item, "error")) || source_removed(state);
        cJSON_Delete(list);
        copy_text(err, cap, removed ? REMOTE_FILE_REMOVED : "TorBox could not prepare this file. Check its status in your TorBox account.");
        result = removed ? DEBRID_MISSING : DEBRID_FAILED;
        goto end;
    }
    if (!item_ready(item)) {
        bool stuck = stalled(job, item, state);
        cJSON_Delete(list);
        if (stuck) { copy_text(err, cap, TORBOX_STALLED); goto end; }
        *retry_at = time(NULL) + 5; result = DEBRID_PROCESSING; goto end;
    }
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(item, "files");
    const cJSON *file = cJSON_GetArrayItem(files, 0); int64_t file_id = -1, size = 0;
    const char *name = json_text(file, "short_name");
    if (!*name) name = json_text(file, "name");
    bool needs_metadata = !strcmp(resolved->source, "remote") && (!resolved->filename[0] || !resolved->size);
    Release selected = *resolved;
    bool match = cJSON_IsArray(files) && cJSON_GetArraySize(files) == 1 &&
        identifier(file, "id", &file_id) && identifier(file, "size", &size) && size > 0 &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file, "infected")) &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file, "zipped")) &&
        (job->debrid_file < 0 || job->debrid_file == file_id);
    if (match && using_mirror) {
        Release file_from_mirror = *route;
        file_from_mirror.size = size;
        match = !remote_file_format(&file_from_mirror, name) && remote_mirror_file(resolved, &file_from_mirror) &&
            (!needs_metadata || !remote_file_format(&selected, resolved->filename[0] ? resolved->filename : name));
    } else if (match) {
        match = (!resolved->size || resolved->size == size) &&
            (!resolved->filename[0] || !strcmp(name, resolved->filename)) &&
            (!needs_metadata || !remote_file_format(&selected, name));
    }
    cJSON_Delete(list);
    if (!match) { copy_text(err, cap, "TorBox's file does not match the selected single-file download. Nothing was transferred."); goto end; }
    pthread_mutex_lock(&orbit.mutex);
    int64_t previous_file = job->debrid_file;
    job->debrid_file = file_id;
    selected.size = size;
    bool saved = needs_metadata ? job_assign_remote_file_locked(job, &selected, err, cap) :
                                 state_save_locked() == 0;
    if (!saved) job->debrid_file = previous_file;
    pthread_mutex_unlock(&orbit.mutex);
    if (!saved) {
        if (!err[0]) copy_text(err, cap, "Could not save the TorBox file identity.");
        goto end;
    }
    *resolved = selected;
    char *escaped = curl_easy_escape(NULL, key, 0);
    if (!escaped) goto end;
    /* Appending the filename can produce literal spaces in TorBox's URL.
     * File identity and the local filename were already checked above; request
     * the canonical address without this optional query value. Keep URL and
     * redirect validation strict, including rejecting unescaped whitespace. */
    snprintf(path, sizeof path, "/webdl/requestdl?token=%s&web_id=%lld&file_id=%lld&redirect=false&zip_link=false&append_name=false", escaped, (long long)job->debrid_id, (long long)file_id); curl_free(escaped);
    cJSON *download = call(key, path, NULL, false, job, retry_at, err, cap, &outcome);
    OPENSSL_cleanse(path, sizeof path);
    const char *address = json_text(download, "data");
    if (download && strlen(address) < sizeof resolved->url && debrid_transfer_url_allowed(address)) {
        copy_text(resolved->url, sizeof resolved->url, address); result = DEBRID_READY;
    } else if (download) copy_text(err, cap, "TorBox returned an unsupported download address.");
    else result = outcome;
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(download, "data");
    if (cJSON_IsString(secret)) OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(download);
end:
    OPENSSL_cleanse(key, sizeof key); OPENSSL_cleanse(path, sizeof path);
    if (result == DEBRID_PROCESSING && *retry_at < time(NULL) + 5) *retry_at = time(NULL) + 5;
    return result;
}
