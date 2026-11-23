#include "remote_sources.h"
#include "catalog_view.h"
#include "ca.h"
#include "provider_network.h"
#include "state_file.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SOURCE_FILE "remote-sources-v1.json"
#define CACHE_DIRECTORY "remote-cache"
#define STATE_LIMIT (256 * 1024)
#define BODY_LIMIT (8 * 1024 * 1024)
#define SOURCE_LIMIT 32
#define CUSTOM_LIMIT 24
#define REFRESH_INTERVAL (6 * 60 * 60)
#define REFRESH_COOLDOWN_MS 60000
#define URL_LIMIT 2048
#define REMOTE_RELEASE_LIMIT 12288
#define METADATA_LIMIT 128
#define METADATA_REQUEST_LIMIT 32

/* Mirrors the Pegasus default-sources manifest, with a revision stored alongside
 * the user's choices. Future defaults are merged by id, never by enabled state. */
static const struct {
    unsigned schema_version, revision, onboarding_revision;
    struct { const char *id, *name, *description, *url; bool default_enabled; } sources[5];
} manifest = {1, 2, 1, {
    {"dlps", "DLPS Catalog", "Games and DLCs from DLPSGAME in various formats",
     "https://pegasus-dl.dev/catalogs/dlps.json", true},
    {"pippo-exfat", "Pippo .exFAT", "Games and DLCs in .exfat and .pkg format",
     "https://pegasus-dl.dev/catalogs/pippo.json", true},
    {"pfs", "PFS Catalog", "Games and DLCs in .ffpfsc and .pkg format",
     "https://pegasus-dl.dev/catalogs/pfs.json", true},
    {"evox-coreos", "evoX-CoreOS", "Homebrew apps and community utilities",
     "https://raw.githubusercontent.com/nexgen999/evoX-CoreOS/refs/heads/main/json/pegasus-dl/catalog.json", true},
    {"zer0", "ZER0GAME Catalog", "Games and DLCs in .pkg format",
     "https://pegasus-dl.dev/catalogs/zer0.json", true},
}};

typedef struct {
    char id[25], name[128], url[URL_LIMIT + 1], etag[256], cache_hash[65];
    const char *error;
    bool enabled, built_in;
    time_t fetched;
    size_t entries, skipped;
    unsigned long epoch;
} RemoteSource;

static struct {
    RemoteSource sources[SOURCE_LIMIT];
    size_t count;
    unsigned revision, onboarding_revision;
    unsigned long epoch, generation;
    int cache_fd;
    pthread_t thread;
    pthread_cond_t changed;
    bool pending, empty_pending, busy, started, unavailable;
    time_t next;
    int64_t empty_after, manual_after;
} remote = {.cache_fd = -1, .changed = PTHREAD_COND_INITIALIZER};

enum MetadataState { META_EMPTY, META_QUEUED, META_LOADING, META_DONE };
typedef struct {
    char id[24];
    enum MetadataState state;
    time_t expires;
    RemoteFileMetadata file;
} MetadataRequest;

static struct {
    pthread_cond_t changed;
    pthread_t threads[2];
    size_t count;
    MetadataRequest requests[METADATA_LIMIT];
} metadata = {.changed = PTHREAD_COND_INITIALIZER};

static const char *const errors[] = {
    "", "network", "http", "too-large", "invalid-catalogue", "invalid-entries", "storage", "limit",
};

static RemoteSource *find(const char *id) {
    for (size_t i = 0; i < remote.count; i++)
        if (!strcmp(remote.sources[i].id, id)) return &remote.sources[i];
    return NULL;
}

static bool notice_acknowledged_locked(void) {
    return orbit.sources.notice_version == ORBIT_SOURCE_NOTICE_VERSION &&
           orbit.sources.acknowledged_at > 0;
}

static bool sources_available_locked(bool empty_only) {
    for (size_t i = 0; i < remote.count; i++)
        if (remote.sources[i].enabled && (!empty_only || !remote.sources[i].entries)) return true;
    return false;
}

static int64_t refresh_clock(void) {
#ifdef ORBIT_REMOTE_TEST_CLOCK
    extern int64_t orbit_remote_test_clock(void);
    return orbit_remote_test_clock();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
#endif
}

unsigned remote_sources_retry_after_locked(void) {
    int64_t remaining = remote.manual_after - refresh_clock();
    return remaining > 0 ? (unsigned)((remaining + 999) / 1000) : 0;
}

bool remote_source_enabled_locked(const Release *r) {
    const RemoteSource *source = find(r->remote_catalog_id);
    return notice_acknowledged_locked() && (orbit.sources.enabled & ORBIT_SOURCE_REMOTE) &&
           source && source->enabled;
}

static bool mirror_enabled(const Release *original, const Release *other) {
    return original->mirror_key[0] && !strcmp(original->mirror_key, other->mirror_key) &&
           strcmp(original->id, other->id) && remote_source_enabled_locked(other);
}

bool remote_next_mirror_locked(const Release *release, const char *after, Release *mirror) {
    const Release *best = NULL;
    for (size_t i = 0; i < orbit.remote_release_count && release->mirror_key[0]; i++) {
        const Release *next = &orbit.remote_releases[i];
        if (strcmp(next->id, after) > 0 && (!best || strcmp(next->id, best->id) < 0) &&
            mirror_enabled(release, next))
            best = next;
    }
    if (best) *mirror = *best;
    return best != NULL;
}

bool remote_mirror_locked(const Release *release, const char *id, Release *mirror) {
    const Release *candidate = release_find(id);
    if (!candidate || !mirror_enabled(release, candidate)) return false;
    *mirror = *candidate;
    return true;
}

bool remote_mirror_file(const Release *selected, const Release *file) {
    return (!selected->size || selected->size == file->size) &&
           (!selected->compression[0] || !strcmp(selected->compression, file->compression)) &&
           (!selected->format[0] || !strcmp(selected->format, "Unknown") ||
            !strcmp(selected->format, file->format));
}

unsigned long remote_sources_generation_locked(void) { return remote.generation; }

size_t remote_sources_entry_count_locked(void) {
    size_t count = 0;
    for (size_t i = 0; i < remote.count; i++)
        if (remote.sources[i].enabled) count += remote.sources[i].entries;
    return count;
}

static void invalidate_locked(void) {
    remote.generation++;
    catalog_view_invalidate_locked();
}

void remote_sources_changed_locked(void) {
    remote.pending = true;
    pthread_cond_signal(&remote.changed);
}

/* The existing service tick queues retries for empty catalogues. The worker
 * keeps its normal refresh interval for sources that already have options. */
void remote_sources_tick(void) {
    pthread_mutex_lock(&orbit.mutex);
    if (remote.started && notice_acknowledged_locked() &&
        !remote.pending && !remote.empty_pending && !remote.busy &&
        refresh_clock() >= remote.empty_after && sources_available_locked(true)) {
        remote.empty_pending = true;
        pthread_cond_signal(&remote.changed);
    }
    pthread_mutex_unlock(&orbit.mutex);
}

static void hash_text(const char *data, size_t length, char out[65]) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)data, length, digest);
    for (size_t i = 0; i < sizeof digest; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

/* Host and normalized path define a custom catalogue. Queries may carry
 * private parameters, so neither the URL nor its query becomes part of the id. */
static bool custom_id(const char *url, char id[25]) {
    if (strlen(url) > URL_LIMIT || !remote_https_url(url)) return false;
    CURLU *parsed = curl_url();
    char *host = NULL, *path = NULL;
    bool valid = parsed && !curl_url_set(parsed, CURLUPART_URL, url, CURLU_DISALLOW_USER) &&
                 !curl_url_get(parsed, CURLUPART_HOST, &host, 0) &&
                 !curl_url_get(parsed, CURLUPART_PATH, &path, 0);
    if (valid) {
        for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
        size_t n = strlen(host);
        if (n && host[n - 1] == '.') host[n - 1] = 0;
        char key[URL_LIMIT + 2], hash[65];
        int length = snprintf(key, sizeof key, "%s%s", host, path);
        valid = length > 0 && (size_t)length < sizeof key;
        if (valid) {
            hash_text(key, (size_t)length, hash);
            snprintf(id, 25, "custom-%.16s", hash);
        }
    }
    curl_free(host);
    curl_free(path);
    curl_url_cleanup(parsed);
    return valid;
}

static bool valid_etag(const char *etag) {
    size_t n = strlen(etag);
    size_t start = !strncmp(etag, "W/", 2) ? 2 : 0;
    if (n < start + 2 || n >= sizeof remote.sources[0].etag || etag[start] != '"' || etag[n - 1] != '"') return false;
    for (size_t i = start + 1; i < n - 1; i++)
        if ((unsigned char)etag[i] <= 32 || (unsigned char)etag[i] >= 127 || etag[i] == '"') return false;
    return true;
}

static cJSON *sources_json(bool disk) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *rows = cJSON_AddArrayToObject(root, "sources");
    bool ok = rows && cJSON_AddNumberToObject(root, "schemaVersion", manifest.schema_version) &&
        cJSON_AddNumberToObject(root, "revision", remote.revision) &&
        cJSON_AddNumberToObject(root, "onboardingRevision", remote.onboarding_revision);
    if (!disk) {
        bool queued = remote.started && notice_acknowledged_locked() &&
                      (remote.empty_pending || (remote.pending && sources_available_locked(false)));
        ok = ok && cJSON_AddBoolToObject(root, "refreshing", remote.busy || queued) &&
                  cJSON_AddNumberToObject(root, "retryAfter", remote_sources_retry_after_locked());
    }
    for (size_t i = 0; ok && i < remote.count; i++) {
        const RemoteSource *s = &remote.sources[i];
        cJSON *r = cJSON_CreateObject();
        ok = r && cJSON_AddItemToArray(rows, r);
        if (!ok) { cJSON_Delete(r); break; }
        ok = cJSON_AddStringToObject(r, "id", s->id) &&
            cJSON_AddStringToObject(r, "name", s->name) &&
            cJSON_AddStringToObject(r, "url", s->url) &&
            cJSON_AddBoolToObject(r, "enabled", s->enabled) &&
            cJSON_AddBoolToObject(r, "builtIn", s->built_in) &&
            cJSON_AddNumberToObject(r, "lastFetchedAt", (double)s->fetched) &&
            cJSON_AddStringToObject(r, "etag", s->etag) &&
            cJSON_AddStringToObject(r, "lastError", s->error ? s->error : "") &&
            cJSON_AddNumberToObject(r, "entryCount", s->entries) &&
            cJSON_AddNumberToObject(r, "skippedEntries", s->skipped);
        if (disk) ok = ok && cJSON_AddStringToObject(r, "cacheHash", s->cache_hash);
        else if (s->built_in) {
            for (size_t j = 0; j < sizeof manifest.sources / sizeof *manifest.sources; j++) {
                if (strcmp(s->id, manifest.sources[j].id)) continue;
                ok = ok && cJSON_AddStringToObject(r, "description", manifest.sources[j].description) &&
                     cJSON_AddBoolToObject(r, "defaultEnabled", manifest.sources[j].default_enabled);
                break;
            }
        }
    }
    if (ok) return root;
    cJSON_Delete(root);
    return NULL;
}

cJSON *remote_sources_json_locked(void) { return sources_json(false); }

cJSON *remote_sources_diagnostics_locked(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", remote.count);
    cJSON_AddNumberToObject(root, "entryCount", orbit.remote_release_count);
    cJSON *codes = cJSON_AddArrayToObject(root, "lastErrorCodes");
    for (size_t i = 0; i < remote.count; i++)
        if (remote.sources[i].error && *remote.sources[i].error)
            cJSON_AddItemToArray(codes, cJSON_CreateString(remote.sources[i].error));
    if (remote.unavailable) cJSON_AddItemToArray(codes, cJSON_CreateString("storage"));
    return root;
}

static int save_locked(void) {
    cJSON *root = sources_json(true);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;
    int result = state_file_replace(orbit.state_fd, SOURCE_FILE, "remote-sources-v1.tmp", body, strlen(body));
    free(body);
    return result;
}

static char *read_file(int dir, const char *name, size_t limit, size_t *length) {
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct stat st;
    char *body = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && (uint64_t)st.st_size <= limit) {
        size_t size = (size_t)st.st_size;
        body = malloc(size + 1);
        if (body) {
            size_t pos = 0;
            while (pos < size) {
                ssize_t n = read(fd, body + pos, size - pos);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                pos += (size_t)n;
            }
            if (pos == size) { body[size] = 0; *length = size; }
            else { free(body); body = NULL; }
        }
    }
    close(fd);
    if (!body) errno = EIO;
    return body;
}

static bool read_row(const cJSON *row, RemoteSource *s) {
    const char *id = json_text(row, "id"), *url = json_text(row, "url"), *name = json_text(row, "name");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(row, "enabled"),
                *built_in = cJSON_GetObjectItemCaseSensitive(row, "builtIn");
    char canonical[25];
    if (!cJSON_IsBool(enabled) || !cJSON_IsBool(built_in) || !*id || strlen(id) >= sizeof s->id ||
        strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(id) ||
        !*name || strlen(name) >= sizeof s->name || !utf8_text(name) ||
        !custom_id(url, canonical) || (!cJSON_IsTrue(built_in) && strcmp(id, canonical))) return false;
    copy_text(s->id, sizeof s->id, id);
    copy_text(s->name, sizeof s->name, name);
    copy_text(s->url, sizeof s->url, url);
    s->enabled = cJSON_IsTrue(enabled);
    s->built_in = cJSON_IsTrue(built_in);
    const char *etag = json_text(row, "etag"), *hash = json_text(row, "cacheHash");
    if (valid_etag(etag)) copy_text(s->etag, sizeof s->etag, etag);
    if (strlen(hash) == 64 && strspn(hash, "0123456789abcdef") == 64)
        copy_text(s->cache_hash, sizeof s->cache_hash, hash);
    int64_t fetched = json_int(row, "lastFetchedAt");
    if (fetched > 0 && fetched <= INT32_MAX * 4LL) s->fetched = (time_t)fetched;
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++)
        if (!strcmp(json_text(row, "lastError"), errors[i])) s->error = errors[i];
    return true;
}

static bool merge_manifest(void) {
    bool changed = remote.revision < manifest.revision ||
                   remote.onboarding_revision < manifest.onboarding_revision;
    for (size_t i = 0; i < sizeof manifest.sources / sizeof *manifest.sources; i++) {
        RemoteSource *s = find(manifest.sources[i].id);
        if (!s) {
            if (remote.count == SOURCE_LIMIT) continue;
            s = &remote.sources[remote.count++];
            *s = (RemoteSource){.enabled = manifest.sources[i].default_enabled, .built_in = true};
            copy_text(s->id, sizeof s->id, manifest.sources[i].id);
            changed = true;
        }
        if (strcmp(s->url, manifest.sources[i].url)) {
            if (*s->url && remote.cache_fd >= 0) {
                char filename[32];
                snprintf(filename, sizeof filename, "%s.json", s->id);
                unlinkat(remote.cache_fd, filename, 0);
            }
            s->etag[0] = s->cache_hash[0] = 0;
            s->fetched = 0;
            s->error = NULL;
            copy_text(s->url, sizeof s->url, manifest.sources[i].url);
            changed = true;
        }
        if (strcmp(s->name, manifest.sources[i].name)) {
            copy_text(s->name, sizeof s->name, manifest.sources[i].name);
            changed = true;
        }
        s->built_in = true;
    }
    if (remote.revision < manifest.revision) remote.revision = manifest.revision;
    if (remote.onboarding_revision < manifest.onboarding_revision)
        remote.onboarding_revision = manifest.onboarding_revision;
    return changed;
}

/* Caller commits the cache only after the replacement backing array is
 * available. A failed allocation or fetch leaves the old options visible. */
static int reserve_locked(const RemoteSource *source, size_t count) {
    size_t total = orbit.remote_release_count - source->entries + count;
    if (total > REMOTE_RELEASE_LIMIT) return -1;
    if (total <= orbit.remote_release_count) return 0;
    Release *next = realloc(orbit.remote_releases, total * sizeof *next);
    if (!next) return -1;
    orbit.remote_releases = next;
    return 0;
}

static void install_locked(RemoteSource *source, const Release *releases, size_t count) {
    size_t write = 0;
    for (size_t i = 0; i < orbit.remote_release_count; i++) {
        if (!strcmp(orbit.remote_releases[i].remote_catalog_id, source->id)) continue;
        orbit.remote_releases[write++] = orbit.remote_releases[i];
    }
    if (count) memcpy(orbit.remote_releases + write, releases, count * sizeof *releases);
    orbit.remote_release_count = write + count;
    source->entries = count;
    if (!orbit.remote_release_count) {
        free(orbit.remote_releases);
        orbit.remote_releases = NULL;
    }
    invalidate_locked();
}

static void cache_load(RemoteSource *s) {
    char file[32], hash[65];
    snprintf(file, sizeof file, "%s.json", s->id);
    size_t length = 0, count, skipped;
    char *body = read_file(remote.cache_fd, file, BODY_LIMIT, &length);
    if (!body) {
        s->etag[0] = s->cache_hash[0] = 0;
        s->fetched = 0;
        if (errno != ENOENT) s->error = "storage";
        return;
    }
    Release *releases = NULL;
    if (remote_catalog_parse(body, length, s->id, &releases, &count, &skipped) ||
        (!count && skipped) || reserve_locked(s, count)) {
        s->error = "invalid-catalogue";
        s->etag[0] = s->cache_hash[0] = 0;
        s->fetched = 0;
    } else {
        hash_text(body, length, hash);
        if (strcmp(hash, s->cache_hash)) {
            s->etag[0] = 0;
            s->fetched = 0;
        }
        copy_text(s->cache_hash, sizeof s->cache_hash, hash);
        install_locked(s, releases, count);
        s->skipped = skipped;
        if (skipped && (!s->error || !*s->error)) s->error = "invalid-entries";
    }
    free(releases);
    free(body);
}

int remote_sources_load(void) {
    if (mkdirat(orbit.state_fd, CACHE_DIRECTORY, 0700) == 0 || errno == EEXIST)
        remote.cache_fd = openat(orbit.state_fd, CACHE_DIRECTORY, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    size_t size = 0;
    char *body = read_file(orbit.state_fd, SOURCE_FILE, STATE_LIMIT, &size);
    bool missing = !body && errno == ENOENT;
    cJSON *root = body && !memchr(body, 0, size) ? cJSON_ParseWithLengthOpts(body, size + 1, NULL, true) : NULL;
    free(body);
    const cJSON *rows = cJSON_GetObjectItemCaseSensitive(root, "sources"), *row;
    int64_t revision = json_int(root, "revision"), onboarding = json_int(root, "onboardingRevision");
    bool valid = json_int(root, "schemaVersion") == manifest.schema_version && cJSON_IsArray(rows) &&
        revision > 0 && revision <= UINT32_MAX && onboarding > 0 && onboarding <= UINT32_MAX;
    if (valid) {
        remote.revision = (unsigned)revision;
        remote.onboarding_revision = (unsigned)onboarding;
        cJSON_ArrayForEach(row, rows) {
            RemoteSource s = {0};
            if (remote.count == SOURCE_LIMIT || !read_row(row, &s) || find(s.id)) { valid = false; break; }
            remote.sources[remote.count++] = s;
        }
    }
    cJSON_Delete(root);
    remote.unavailable = remote.cache_fd < 0 || (!valid && !missing);
    if (!valid) remote.count = 0;
    bool changed = merge_manifest();
    if (!remote.unavailable) {
        for (size_t i = 0; i < remote.count; i++) cache_load(&remote.sources[i]);
        if (changed && save_locked()) remote.unavailable = true;
    }
    return remote.unavailable ? -1 : 0;
}

typedef struct {
    char *data;
    size_t size, capacity;
    char etag[256];
    bool oversized;
} Response;

static bool stopping(void) { return orbit_is_stopping() || orbit.stop; }

static size_t collect(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    if (a && b > SIZE_MAX / a) return 0;
    size_t count = a * b;
    if (count > BODY_LIMIT - r->size) { r->oversized = true; return 0; }
    if (stopping()) return 0;
    size_t needed = r->size + count + 1;
    if (needed > r->capacity) {
        size_t capacity = r->capacity ? r->capacity * 2 : 16384;
        if (capacity < needed) capacity = needed;
        if (capacity > BODY_LIMIT + 1) capacity = BODY_LIMIT + 1;
        char *next = realloc(r->data, capacity);
        if (!next) return 0;
        r->data = next;
        r->capacity = capacity;
    }
    memcpy(r->data + r->size, data, count);
    r->size += count;
    r->data[r->size] = 0;
    return count;
}

static size_t headers(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    size_t n = a * b;
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) r->etag[0] = 0;
    if (n > 5 && !strncasecmp(data, "ETag:", 5)) {
        size_t start = 5;
        while (start < n && (data[start] == ' ' || data[start] == '\t')) start++;
        while (n > start && strchr(" \t\r\n", data[n - 1])) n--;
        if (n - start < sizeof r->etag) {
            memcpy(r->etag, data + start, n - start);
            r->etag[n - start] = 0;
            if (!valid_etag(r->etag)) r->etag[0] = 0;
        }
    }
    return a * b;
}

static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    const RemoteSource *snapshot = context;
    if (stopping()) return 1;
    pthread_mutex_lock(&orbit.mutex);
    const RemoteSource *live = find(snapshot->id);
    bool cancel = !notice_acknowledged_locked() || !live || !live->enabled || live->epoch != snapshot->epoch;
    pthread_mutex_unlock(&orbit.mutex);
    return cancel;
}

static int before_request(void *context, char *remote_ip, char *local_ip, int remote_port, int local_port) {
    (void)remote_ip; (void)local_ip; (void)remote_port; (void)local_port;
    char *url = NULL;
    curl_easy_getinfo(context, CURLINFO_EFFECTIVE_URL, &url);
    return remote_https_url(url) ? CURL_PREREQFUNC_OK : CURL_PREREQFUNC_ABORT;
}

static const char *fetch(const RemoteSource *s, Response *response, long *status) {
    CURL *curl = curl_easy_init();
    if (!curl) return "network";
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    char condition[sizeof s->etag + 20];
    struct curl_slist *request_headers = NULL;
    if (*s->etag) {
        snprintf(condition, sizeof condition, "If-None-Match: %s", s->etag);
        request_headers = curl_slist_append(NULL, condition);
    }
    curl_easy_setopt(curl, CURLOPT_URL, s->url);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ORBIT_TEST
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, provider_open_socket);
    curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, before_request);
    curl_easy_setopt(curl, CURLOPT_PREREQDATA, curl);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)BODY_LIMIT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(request_headers);
    if (response->oversized || result == CURLE_FILESIZE_EXCEEDED) return "too-large";
    if (result != CURLE_OK) return "network";
    if (*status != 200 && (*status != 304 || !*s->etag || !*s->cache_hash)) return "http";
    return "";
}

static void refresh_one(const RemoteSource *snapshot) {
    Response response = {0};
    long status = 0;
    const char *error = fetch(snapshot, &response, &status);
    Release *releases = NULL;
    size_t count = 0, skipped = 0;
    if (!*error && status == 200 &&
        (!response.data || remote_catalog_parse(response.data, response.size, snapshot->id, &releases, &count, &skipped)))
        error = "invalid-catalogue";

    pthread_mutex_lock(&orbit.mutex);
    RemoteSource *live = find(snapshot->id);
    if (live && live->enabled && notice_acknowledged_locked() && live->epoch == snapshot->epoch && !stopping()) {
        if (!*error && status == 200) {
            char filename[32];
            snprintf(filename, sizeof filename, "%s.json", live->id);
            if (!count && skipped) error = "invalid-entries";
            else if (reserve_locked(live, count)) error = "limit";
            else if (state_file_replace(remote.cache_fd, filename, "catalogue.tmp", response.data, response.size))
                error = "storage";
            else {
                install_locked(live, releases, count);
                hash_text(response.data, response.size, live->cache_hash);
                copy_text(live->etag, sizeof live->etag, response.etag);
                live->skipped = skipped;
                live->fetched = time(NULL);
            }
            if (!count && skipped) live->skipped = skipped;
        } else if (!*error && status == 304) {
            if (*response.etag) copy_text(live->etag, sizeof live->etag, response.etag);
            live->fetched = time(NULL);
        }
        live->error = *error ? error : live->skipped ? "invalid-entries" : "";
        if (save_locked()) live->error = "storage";
    }
    pthread_mutex_unlock(&orbit.mutex);
    free(releases);
    free(response.data);
}

static MetadataRequest *metadata_request_locked(const char *id, time_t now) {
    MetadataRequest *entry = NULL;
    for (size_t i = 0; i < METADATA_LIMIT; i++) {
        MetadataRequest *request = &metadata.requests[i];
        if (!strcmp(id, request->id)) {
            if (request->state != META_DONE || request->expires > now) return request;
            entry = request;
            break;
        }
        if (request->state == META_EMPTY ||
            (request->state == META_DONE && (!entry || request->expires < entry->expires)))
            entry = request;
    }
    if (!entry) return NULL;
    *entry = (MetadataRequest){.state = META_QUEUED};
    copy_text(entry->id, sizeof entry->id, id);
    pthread_cond_signal(&metadata.changed);
    return entry;
}

static bool metadata_provider(const Release *r) {
    return remote_page_pending(r) && remote_metadata_url(r->browser_url);
}

static bool next_metadata_mirror_locked(const Release *r, const char *after, Release *mirror) {
    char cursor[sizeof mirror->id];
    copy_text(cursor, sizeof cursor, after);
    while (remote_next_mirror_locked(r, cursor, mirror)) {
        if (metadata_provider(mirror)) return true;
        copy_text(cursor, sizeof cursor, mirror->id);
    }
    return false;
}

cJSON *remote_metadata_json_locked(const char *ids) {
    cJSON *root = cJSON_CreateObject();
    cJSON *rows = cJSON_AddArrayToObject(root, "files");
    if (!rows) { cJSON_Delete(root); return NULL; }
    bool pending = false;
    time_t now = time(NULL);
    for (unsigned i = 0; metadata.count && ids && *ids && i < METADATA_REQUEST_LIMIT; i++) {
        char id[24];
        size_t length = strcspn(ids, ",");
        if (!length || length >= sizeof id) break;
        memcpy(id, ids, length);
        id[length] = 0;
        ids += length + (ids[length] == ',');
        const Release *release = release_find(id);
        Release mirror;
        if (!release || strcmp(release->source, "remote") || !remote_source_enabled_locked(release) ||
            (!metadata_provider(release) && !next_metadata_mirror_locked(release, "", &mirror))) continue;
        const MetadataRequest *request = metadata_request_locked(id, now);
        if (!request || request->state != META_DONE) {
            pending = true;
            continue;
        }
        if (!request->file.size && !request->file.filename[0]) continue;
        cJSON *row = cJSON_CreateObject();
        if (!cJSON_AddStringToObject(row, "id", id) ||
            !cJSON_AddNumberToObject(row, "sizeBytes", (double)request->file.size) ||
            !cJSON_AddBoolToObject(row, "exact", request->file.exact) ||
            (request->file.filename[0] && !cJSON_AddStringToObject(row, "filename", request->file.filename)) ||
            (request->file.format[0] && !cJSON_AddStringToObject(row, "format", request->file.format)) ||
            (request->file.compression[0] && !cJSON_AddStringToObject(row, "compression", request->file.compression)) ||
            !cJSON_AddItemToArray(rows, row)) cJSON_Delete(row);
    }
    cJSON_AddBoolToObject(root, "pending", pending);
    return root;
}

static void *metadata_worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!orbit.stop) {
        MetadataRequest *request = NULL;
        for (size_t i = 0; i < METADATA_LIMIT; i++)
            if (metadata.requests[i].state == META_QUEUED) {
                request = &metadata.requests[i];
                break;
            }
        if (!request) {
            pthread_cond_wait(&metadata.changed, &orbit.mutex);
            continue;
        }
        request->state = META_LOADING;
        RemoteFileMetadata file = {0};
        const Release *release = release_find(request->id);
        if (release && remote_source_enabled_locked(release)) {
            Release selected = *release, candidate = selected;
            bool found = metadata_provider(&candidate) || next_metadata_mirror_locked(&selected, "", &candidate);
            while (found && !orbit.stop && remote_source_enabled_locked(&selected)) {
                pthread_mutex_unlock(&orbit.mutex);
                RemoteFileMetadata current;
                remote_download_metadata(&candidate, &current, NULL);
                pthread_mutex_lock(&orbit.mutex);
                if (!strcmp(candidate.id, selected.id)) file = current;
                if (current.size) {
                    file.size = current.size;
                    file.exact = current.exact;
                    break;
                }
                found = next_metadata_mirror_locked(&selected, strcmp(candidate.id, selected.id) ? candidate.id : "",
                                                    &candidate);
            }
        }
        request->file = file;
        request->expires = time(NULL) + (file.size || file.filename[0] ? 1800 : 300);
        request->state = META_DONE;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return NULL;
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!orbit.stop) {
        if (!notice_acknowledged_locked()) {
            pthread_cond_wait(&remote.changed, &orbit.mutex);
            continue;
        }
        time_t now = time(NULL);
        bool full = remote.pending || now >= remote.next;
        if (!full && !remote.empty_pending) {
            struct timespec deadline = {.tv_sec = remote.next};
            pthread_cond_timedwait(&remote.changed, &orbit.mutex, &deadline);
            continue;
        }
        remote.pending = false;
        remote.empty_pending = false;
        remote.busy = true;
        /* Membership changes during a fetch trigger another pass. Take a
         * snapshot so an add or remove cannot invalidate an in-flight pointer. */
        RemoteSource snapshots[SOURCE_LIMIT];
        size_t count = remote.count;
        memcpy(snapshots, remote.sources, count * sizeof *snapshots);
        pthread_mutex_unlock(&orbit.mutex);
        for (size_t i = 0; i < count && !stopping(); i++)
            if (snapshots[i].enabled && (full || !snapshots[i].entries) &&
                !progress(&snapshots[i], 0, 0, 0, 0)) refresh_one(&snapshots[i]);
        pthread_mutex_lock(&orbit.mutex);
        if (full) remote.next = time(NULL) + REFRESH_INTERVAL;
        remote.empty_after = refresh_clock() + REFRESH_COOLDOWN_MS;
        remote.busy = false;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return NULL;
}

int remote_sources_start(void) {
    if (remote.unavailable) return -1;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    pthread_mutex_lock(&orbit.mutex);
    if (!rc) rc = pthread_create(&remote.thread, &attr, worker, NULL);
    remote.started = rc == 0;
    if (remote.started)
        for (size_t i = 0; i < sizeof metadata.threads / sizeof *metadata.threads; i++)
            if (!pthread_create(&metadata.threads[metadata.count], &attr, metadata_worker, NULL))
                metadata.count++;
    pthread_mutex_unlock(&orbit.mutex);
    pthread_attr_destroy(&attr);
    return rc ? -1 : 0;
}

void remote_sources_stop(void) {
    pthread_mutex_lock(&orbit.mutex);
    pthread_cond_signal(&remote.changed);
    pthread_cond_broadcast(&metadata.changed);
    pthread_mutex_unlock(&orbit.mutex);
    if (remote.started) pthread_join(remote.thread, NULL);
    for (size_t i = 0; i < metadata.count; i++) pthread_join(metadata.threads[i], NULL);
    if (remote.cache_fd >= 0) close(remote.cache_fd);
    pthread_mutex_lock(&orbit.mutex);
    free(orbit.remote_releases);
    orbit.remote_releases = NULL;
    orbit.remote_release_count = 0;
    pthread_mutex_unlock(&orbit.mutex);
}

static void pause_disabled_locked(void) {
    bool changed = false;
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (strcmp(j->release.source, "remote") || remote_source_enabled_locked(&j->release)) continue;
        if (job_active(j) || !strcmp(j->status, "queued") || !strcmp(j->status, "retrying")) {
            if (!job_active(j)) copy_text(j->status, sizeof j->status, "paused");
            j->pause = true;
            copy_text(j->error, sizeof j->error, "Source disabled. Enable it in Sources before resuming.");
            changed = true;
        }
    }
    if (changed) (void)state_save_locked();
    pthread_cond_broadcast(&orbit.changed);
}

static int refuse(char *error, size_t cap, unsigned status, const char *message) {
    copy_text(error, cap, message);
    return (int)status;
}

int remote_sources_change_locked(const char *method, const char *id, const cJSON *input,
                                 char *error, size_t cap) {
    if (stopping()) return refuse(error, cap, 503, "Orbit is stopping.");
    if (remote.unavailable) return refuse(error, cap, 503, "Could not save community catalogues.");
    if (!strcmp(method, "POST") && !strcmp(id, "refresh")) {
        if (!notice_acknowledged_locked())
            return refuse(error, cap, 409, "Enable this source and acknowledge the download notice in Sources first.");
        if (!remote.started) return refuse(error, cap, 503, "Could not start catalogue refresh.");
        if (!sources_available_locked(false))
            return refuse(error, cap, 409, "No sources enabled");
        if (remote_sources_retry_after_locked())
            return refuse(error, cap, 429, "Please wait before refreshing the catalogue again.");
        remote.manual_after = refresh_clock() + REFRESH_COOLDOWN_MS;
        remote_sources_changed_locked();
        return 202;
    }
    size_t old_count = remote.count;
    bool add = !*id && !strcmp(method, "POST"), remove = !strcmp(method, "DELETE");
    RemoteSource old = {0}, *s = NULL;
    size_t index;
    if (add) {
        const char *url = json_text(input, "url"), *name = json_text(input, "name");
        char canonical[25];
        if (!custom_id(url, canonical))
            return refuse(error, cap, 400, "Enter a valid HTTPS catalogue URL.");
        const cJSON *name_field = cJSON_GetObjectItemCaseSensitive(input, "name");
        if ((name_field && !cJSON_IsString(name_field)) || strlen(name) >= sizeof old.name ||
            (*name && !utf8_text(name)))
            return refuse(error, cap, 400, "Enter a shorter catalogue name.");
        size_t custom_count = 0;
        for (size_t i = 0; i < remote.count; i++) {
            if (!remote.sources[i].built_in) custom_count++;
            char existing[25];
            if (custom_id(remote.sources[i].url, existing) && !strcmp(existing, canonical))
                return refuse(error, cap, 409, "This catalogue is already listed.");
        }
        if (remote.count >= SOURCE_LIMIT || custom_count >= CUSTOM_LIMIT)
            return refuse(error, cap, 409, "Community catalogues are full.");
        index = remote.count;
        s = &remote.sources[remote.count++];
        *s = (RemoteSource){.enabled = true, .epoch = ++remote.epoch};
        copy_text(s->id, sizeof s->id, canonical);
        copy_text(s->name, sizeof s->name, *name ? name : "Custom catalogue");
        copy_text(s->url, sizeof s->url, url);
        /* A previously removed source must not inherit an orphaned cache. */
        char filename[32];
        snprintf(filename, sizeof filename, "%s.json", s->id);
        if (unlinkat(remote.cache_fd, filename, 0) && errno != ENOENT) {
            remote.count = old_count;
            return refuse(error, cap, 503, "Could not save community catalogues.");
        }
    } else {
        s = find(id);
        if (!s) return refuse(error, cap, 404, "Community catalogue was not found.");
        index = (size_t)(s - remote.sources);
        old = *s;
        if (remove) {
            if (s->built_in) return refuse(error, cap, 409, "Built-in catalogues can be disabled.");
            remote.count--;
            memmove(s, s + 1, (remote.count - index) * sizeof *s);
        } else {
            const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled");
            if (!cJSON_IsBool(enabled)) return refuse(error, cap, 400, "Choose whether to enable this catalogue.");
            if (s->enabled == cJSON_IsTrue(enabled)) return 200;
            s->enabled = cJSON_IsTrue(enabled);
            s->epoch = ++remote.epoch;
        }
    }
    if (save_locked()) {
        if (remove) memmove(s + 1, s, (remote.count - index) * sizeof *s);
        if (!add) *s = old;
        remote.count = old_count;
        return refuse(error, cap, 503, "Could not save community catalogues.");
    }
    if (remove) {
        char filename[32];
        snprintf(filename, sizeof filename, "%s.json", old.id);
        unlinkat(remote.cache_fd, filename, 0);
        install_locked(&old, NULL, 0);
    } else if (!add) invalidate_locked();
    if (remove || !s->enabled) pause_disabled_locked();
    if (add || (!remove && s->enabled)) remote_sources_changed_locked();
    return add ? 201 : 200;
}
