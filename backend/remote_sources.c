#include "remote_sources.h"
#include "pspkg.h"
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
#define PSPKG_REFRESH_INTERVAL (24 * 60 * 60)
#define REFRESH_COOLDOWN_MS 60000
#define URL_LIMIT 2048
#define REMOTE_RELEASE_LIMIT 12288
#define METADATA_LIMIT 512


/* Mirrors the Pegasus default-sources manifest, with a revision stored alongside
 * the user's choices. Future defaults are merged by id, never by enabled state. */
static const struct {
    unsigned schema_version, revision, onboarding_revision;
    struct { const char *id, *name, *description, *url; bool default_enabled; } sources[8];
} manifest = {1, 4, 1, {
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
    {PADRINO_SOURCE, "Padrino Store", "Bundled PS4 and PS5 catalog snapshot, without ZSTD",
     "https://github.com/B4ckd0orl/Padrino-Store", true},
    {PSPKG_PS5_SOURCE, "PSPKG PS5", "Public links from the general PSPKG PS5 games catalogue",
     "https://pspkg.com/ps5/", true},
    {PSPKG_PS4_SOURCE, "PSPKG PS4", "Public links from the general PSPKG PS4 games catalogue",
     "https://pspkg.com/ps4/", true},
}};

typedef struct {
    char id[25], name[128], url[URL_LIMIT + 1], etag[256], cache_hash[65];
    const char *error;
    bool enabled, built_in;
    time_t fetched;
    size_t entries, skipped, discovered;
    bool truncated;
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
    cJSON *public_links;
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
        if (remote.sources[i].enabled && strcmp(remote.sources[i].id, PADRINO_SOURCE) &&
            (!empty_only || (strcmp(remote.sources[i].id, PSPKG_PS5_SOURCE) &&
                             strcmp(remote.sources[i].id, PSPKG_PS4_SOURCE))) &&
            (!empty_only || !remote.sources[i].entries)) return true;
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
        if (ok && (!strcmp(s->id, PSPKG_PS4_SOURCE) || !strcmp(s->id, PSPKG_PS5_SOURCE)))
            ok = cJSON_AddNumberToObject(r, "discovered", s->discovered) &&
                 cJSON_AddBoolToObject(r, "truncated", s->truncated);
        if (disk) ok = ok && cJSON_AddStringToObject(r, "cacheHash", s->cache_hash);
        else if (s->built_in) {
            if (!strcmp(s->id, PADRINO_SOURCE)) ok = ok && cJSON_AddTrueToObject(r, "bundled");
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

cJSON *remote_source_package_diagnostics_locked(const char *source, const char *package) {
    const RemoteSource *s = find(source ? source : "");
    if (!s || !package || !*package || strlen(package) >= 128) return NULL;
    cJSON *feed = NULL;
    const cJSON *entry = NULL;
    if (!strcmp(s->id, PADRINO_SOURCE)) entry = padrino_game(package);
    else {
        char filename[32];
        snprintf(filename, sizeof filename, "%s.json", s->id);
        size_t length = 0;
        char *body = read_file(remote.cache_fd, filename, BODY_LIMIT, &length);
        if (body) feed = cJSON_ParseWithLength(body, length);
        free(body);
        const cJSON *candidate;
        cJSON_ArrayForEach(candidate, cJSON_GetObjectItemCaseSensitive(feed, "packages")) {
            if (strcmp(json_text(candidate, "id"), package) && strcmp(json_text(candidate, "titleId"), package) &&
                strcasecmp(json_text(candidate, "title"), package)) continue;
            entry = candidate;
            break;
        }
    }
    cJSON *result = remote_catalog_diagnostics_locked(entry, s->id);
    cJSON_AddBoolToObject(result, "indexTruncated", s->truncated);
    cJSON_AddStringToObject(result, "sourceError", s->error ? s->error : "");
    cJSON_Delete(feed);
    return result;
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
    int64_t discovered = json_int(row, "discovered");
    if (discovered > 0 && discovered <= PSPKG_INDEX_LIMIT) s->discovered = (size_t)discovered;
    s->truncated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "truncated"));
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
    const RemoteSource *padrino = find(PADRINO_SOURCE);
    size_t index_entries = 0;
    for (size_t i = 0; i < remote.count; i++) {
        const RemoteSource *candidate = &remote.sources[i];
        if (!strcmp(candidate->id, PSPKG_PS4_SOURCE) || !strcmp(candidate->id, PSPKG_PS5_SOURCE))
            index_entries += candidate == source ? count : candidate->entries;
    }
    if (total < index_entries || total - index_entries > REMOTE_RELEASE_LIMIT + (padrino ? padrino->entries : 0)) return -1;
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
    RemoteSource *padrino = find(PADRINO_SOURCE);
    if (padrino) {
        if (padrino_catalog_load(&orbit.remote_releases, &orbit.remote_release_count, &padrino->fetched))
            padrino->error = "invalid-catalogue";
        else {
            padrino->entries = orbit.remote_release_count;
            padrino->error = NULL;
            invalidate_locked();
        }
    }
    if (!remote.unavailable) {
        for (size_t i = 0; i < remote.count; i++)
            if (&remote.sources[i] != padrino) cache_load(&remote.sources[i]);
        if (changed && save_locked()) remote.unavailable = true;
    }
    return remote.unavailable ? -1 : 0;
}

typedef struct {
    char *data;
    size_t size, capacity;
    char etag[256];
    char effective_url[URL_LIMIT + 1];
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
    char *effective = NULL;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective) copy_text(response->effective_url, sizeof response->effective_url, effective);
    curl_easy_cleanup(curl);
    curl_slist_free_all(request_headers);
    if (response->oversized || result == CURLE_FILESIZE_EXCEEDED) return "too-large";
    if (result != CURLE_OK) return "network";
    if (*status != 200 && (*status != 304 || !*s->etag || !*s->cache_hash)) return "http";
    return "";
}

static bool pspkg_source(const char *id) {
    return !strcmp(id, PSPKG_PS5_SOURCE) || !strcmp(id, PSPKG_PS4_SOURCE);
}

static bool pspkg_text(const char *start, size_t length, char *out, size_t cap) {
    size_t used = 0;
    bool tag = false;
    for (size_t i = 0; i < length; i++) {
        if (start[i] == '<') { tag = true; continue; }
        if (tag) { if (start[i] == '>') tag = false; continue; }
        const char *value = NULL;
        char decoded = 0;
        if (start[i] == '&') {
            const struct { const char *name; char value; } entities[] = {
                {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''},
                {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", ' '}, {"&#160;", ' '}
            };
            for (size_t j = 0; j < sizeof entities / sizeof *entities; j++)
                if (strlen(entities[j].name) <= length - i &&
                    !memcmp(start + i, entities[j].name, strlen(entities[j].name))) {
                    value = entities[j].name;
                    decoded = entities[j].value;
                    break;
                }
        }
        char c = value ? decoded : start[i];
        if (value) i += strlen(value) - 1;
        if (isspace((unsigned char)c)) {
            if (!used || out[used - 1] == ' ') continue;
            c = ' ';
        }
        if (used + 1 >= cap) return false;
        out[used++] = c;
    }
    while (used && out[used - 1] == ' ') used--;
    out[used] = 0;
    return used && utf8_text(out);
}

static bool pspkg_attribute(const char *tag, const char *end, const char *key, char *out, size_t cap) {
    size_t n = strlen(key);
    for (const char *p = tag; p + n < end; p++) {
        if (p != tag && !isspace((unsigned char)p[-1])) continue;
        if (strncasecmp(p, key, n)) continue;
        const char *q = p + n;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q == end || *q++ != '=') continue;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q == end || (*q != '"' && *q != '\'')) continue;
        char quote = *q++;
        const char *finish = memchr(q, quote, (size_t)(end - q));
        return finish && pspkg_text(q, (size_t)(finish - q), out, cap);
    }
    return false;
}

static bool pspkg_game_url(const char *href, const char *platform, const char *base, char *out, size_t cap) {
    const char *path = href;
    if (!strncmp(path, "https://pspkg.com", 17)) path += 17;
    else if (!strncmp(path, "https://www.pspkg.com", 21)) path += 21;
    else if (*path != '/') return false;
    char prefix[8];
    snprintf(prefix, sizeof prefix, "/%s/", platform);
    if (strncmp(path, prefix, strlen(prefix))) return false;
    path += strlen(prefix);
    if (!strncmp(path, "download-", 9) || strchr(path, '/') || strchr(path, '?') || strchr(path, '#')) return false;
    const char *suffix = strstr(path, ".html");
    if (!suffix || suffix[5] || suffix == path) return false;
    const char *dash = suffix;
    while (dash > path && isdigit((unsigned char)dash[-1])) dash--;
    if (dash == suffix || dash == path || dash[-1] != '-') return false;
    const char *origin_end = strstr(base, "://");
    origin_end = origin_end ? strchr(origin_end + 3, '/') : NULL;
    if (!origin_end) return false;
    int size = snprintf(out, cap, "%.*s/%s/%s", (int)(origin_end - base), base, platform, path);
    return size > 0 && (size_t)size < cap;
}

/* PSPKG's games-list pages contain the complete platform index in one
 * response. Save translated rows through the existing durable source cache.
 * The stable download page is inspected only when the title is selected. */
static const char *pspkg_scan(const RemoteSource *snapshot, Response *response, long *status,
                              size_t limit, size_t *discovered, bool *truncated) {
    const char *platform = !strcmp(snapshot->id, PSPKG_PS5_SOURCE) ? "ps5" : "ps4";
    RemoteSource request = *snapshot;
    snprintf(request.url, sizeof request.url, "%.*s%s-games-list",
             (int)strnlen(snapshot->url, sizeof request.url - 20), snapshot->url, platform);
    const char *error = fetch(&request, response, status);
    if (*error || *status == 304) return error;
    if (!response->data || strcmp(response->effective_url, request.url)) return "invalid-catalogue";
    cJSON *root = cJSON_CreateObject();
    cJSON *packages = cJSON_AddArrayToObject(root, "packages");
    if (!packages) { cJSON_Delete(root); return "limit"; }
    for (const char *tag = response->data; (tag = strstr(tag, "<a")); tag += 2) {
        if (!isspace((unsigned char)tag[2])) continue;
        const char *end = strchr(tag, '>');
        const char *close = end ? strstr(end, "</a>") : NULL;
        if (!end) break;
        if (!close || close - end > 4096) continue;
        char href[256], game[256], download[256], title[256];
        if (!pspkg_attribute(tag, end, "href", href, sizeof href) ||
            !pspkg_game_url(href, platform, snapshot->url, game, sizeof game) ||
            !pspkg_download_page(game, download, sizeof download) ||
            !pspkg_text(end + 1, (size_t)(close - end - 1), title, sizeof title)) continue;
        char *name = title;
        size_t n = strspn(name, "0123456789");
        size_t len = strlen(name);
        /* Rows include a sequence number and publication date outside the title. */
        if (n && name[n] == ' ' && len > n + 12 && name[len - 11] == ' ' &&
            name[len - 6] == '/' && name[len - 3] == '/') {
            name[len - 11] = 0;
            name += n + 1;
        }
        len = strlen(name);
        if (len >= sizeof ((Release *)0)->title) {
            len = sizeof ((Release *)0)->title - 1;
            while (len && ((unsigned char)name[len] & 0xc0) == 0x80) len--;
            name[len] = 0;
        }
        if (!*name || !utf8_text(name)) continue;
        if (*discovered >= limit) { *truncated = true; break; }
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char *)game, strlen(game), digest);
        char id[32], description[350];
        snprintf(id, sizeof id, "pspkg-%02x%02x%02x%02x%02x%02x",
                 digest[0], digest[1], digest[2], digest[3], digest[4], digest[5]);
        /* Keep the canonical PSPKG origin in the source metadata in host fixtures too. */
        const char *path = strchr(strstr(game, "://") + 3, '/');
        snprintf(description, sizeof description, "Public PSPKG index.\nSource: https://pspkg.com%s\n", path);
#ifdef ORBIT_TEST
        if (!strncmp(game, "http://127.0.0.1:", 17))
            snprintf(description, sizeof description, "Public PSPKG index.\nSource: %s\n", game);
#endif
        cJSON *entry = cJSON_CreateObject();
        cJSON *links = cJSON_AddArrayToObject(entry, "downloadLinks");
        cJSON *choice = cJSON_CreateObject();
        bool ok = links && choice && cJSON_AddStringToObject(entry, "id", id) &&
            cJSON_AddStringToObject(entry, "title", name) &&
            cJSON_AddStringToObject(entry, "category", !strcmp(platform, "ps5") ? "PS5" : "PS4") &&
            cJSON_AddStringToObject(entry, "description", description) &&
            cJSON_AddStringToObject(choice, "name", "PSPKG") &&
            cJSON_AddStringToObject(choice, "url", download) &&
            cJSON_AddTrueToObject(choice, "page");
        if (ok && cJSON_AddItemToArray(links, choice)) choice = NULL;
        else ok = false;
        if (!ok || !cJSON_AddItemToArray(packages, entry)) {
            cJSON_Delete(choice);
            cJSON_Delete(entry);
            cJSON_Delete(root);
            return "limit";
        }
        (*discovered)++;
    }
    if (!*discovered) { cJSON_Delete(root); return "invalid-catalogue"; }
    cJSON_AddBoolToObject(root, "truncated", *truncated);
    cJSON_AddNumberToObject(root, "discovered", *discovered);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return "limit";
    free(response->data);
    response->data = body;
    response->size = strlen(body);
    return "";
}

#ifdef ORBIT_TEST
cJSON *remote_pspkg_preview(const char *source_id, const char *url, size_t game_limit) {
    cJSON *result = cJSON_CreateObject();
    const RemoteSource *source = find(source_id);
    RemoteSource snapshot;
    if (source) snapshot = *source;
    if (source && url && *url) copy_text(snapshot.url, sizeof snapshot.url, url);
    Response response = {0};
    size_t discovered = 0;
    bool truncated = false;
    long status = 0;
    const char *error = !source || !pspkg_source(source_id) || !game_limit || game_limit > PSPKG_INDEX_LIMIT
        ? "invalid-catalogue" : pspkg_scan(&snapshot, &response, &status, game_limit, &discovered, &truncated);
    cJSON_AddStringToObject(result, "error", error);
    cJSON_AddNumberToObject(result, "discovered", discovered);
    cJSON_AddBoolToObject(result, "truncated", truncated);
    cJSON *feed = !*error && response.data ? cJSON_ParseWithLength(response.data, response.size) : NULL;
    if (feed) cJSON_AddItemToObject(result, "feed", feed);
    free(response.data);
    return result;
}
#endif

static void refresh_one(const RemoteSource *snapshot) {
    Response response = {0};
    long status = 200;
    size_t discovered = 0;
    bool truncated = false;
    const char *error = pspkg_source(snapshot->id)
        ? pspkg_scan(snapshot, &response, &status, PSPKG_INDEX_LIMIT, &discovered, &truncated)
        : fetch(snapshot, &response, &status);
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
            if ((!count && skipped) || (pspkg_source(live->id) && !count && live->entries))
                error = "invalid-entries";
            else if (reserve_locked(live, count)) error = "limit";
            else if (state_file_replace(remote.cache_fd, filename, "catalogue.tmp", response.data, response.size))
                error = "storage";
            else {
                install_locked(live, releases, count);
                hash_text(response.data, response.size, live->cache_hash);
                copy_text(live->etag, sizeof live->etag, response.etag);
                live->skipped = skipped;
                live->discovered = discovered;
                live->truncated = truncated;
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
        if (request->state == META_EMPTY || (request->state == META_DONE &&
            (!entry || (entry->state != META_EMPTY && request->expires < entry->expires)))) entry = request;
    }
    if (!entry) return NULL;
    cJSON_Delete(entry->public_links);
    *entry = (MetadataRequest){.state = META_QUEUED};
    copy_text(entry->id, sizeof entry->id, id);
    pthread_cond_signal(&metadata.changed);
    return entry;
}

static int file_rank(const RemoteFileMetadata *file) {
    if (file->size) return 2 + file->confidence;
    return file->filename[0] ? 1 : 0;
}

static void metadata_snapshot(cJSON *views, cJSON *record, const Release *r, const MetadataRequest *request,
                              const char *group, bool visible) {
    cJSON *row = cJSON_CreateObject();
    if (!row) return;
    const RemoteFileMetadata *file = &request->file;
    bool done = request->state == META_DONE;
    const char *status = done ? remote_metadata_status(file->status) :
        request->state == META_LOADING ? "fetching" : "queued";
    cJSON_AddStringToObject(row, "id", r->id);
    cJSON_AddStringToObject(row, "source", r->remote_catalog_id);
    cJSON_AddStringToObject(row, "titleId", r->title_id);
    cJSON_AddStringToObject(row, "label", r->remote_label);
    cJSON_AddStringToObject(row, "mirrorGroup", group);
    cJSON_AddBoolToObject(row, "visible", visible);
    cJSON_AddBoolToObject(row, "metadataEligible", true);
    cJSON_AddStringToObject(row, "status", status);
    cJSON_AddBoolToObject(row, "getAttempted", file->attempted || request->state == META_LOADING);
    if (done) {
        cJSON_AddNumberToObject(row, "httpStatus", file->http_status);
        cJSON_AddStringToObject(row, "effectiveHost", file->effective_host);
        cJSON_AddNumberToObject(row, "sizeBytes", (double)file->size);
        cJSON_AddBoolToObject(row, "exact", file->exact);
        cJSON_AddNumberToObject(row, "sizeConfidence", file->confidence);
        cJSON *sizes = cJSON_AddArrayToObject(row, "sizeCandidates");
        for (unsigned i = 0; sizes && i < file->candidate_count; i++)
            cJSON_AddItemToArray(sizes, cJSON_CreateNumber((double)file->candidates[i]));
        if (request->public_links)
            cJSON_AddItemToObject(row, "links", cJSON_Duplicate(request->public_links, true));
    }
    cJSON_AddItemToArray(views, row);
    if (visible) cJSON_AddStringToObject(record, "status", status);
}

cJSON *remote_metadata_json_locked(const char *ids) {
    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_AddArrayToObject(root, "files");
    cJSON *views = cJSON_AddArrayToObject(root, "options");
    if (!files || !views) { cJSON_Delete(root); return NULL; }
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
        if (!release || strcmp(release->source, "remote") || !remote_source_enabled_locked(release)) continue;
        const Release *candidate = release;
        Release alternate;
        char after[sizeof alternate.id] = "";
        RemoteFileMetadata best = {0};
        cJSON *row = cJSON_CreateObject();
        if (!row) continue;
        cJSON_AddStringToObject(row, "id", id);
        unsigned mirrors = 0;
        bool conflicting_size = false, group_pending = false;
        do {
            mirrors++;
            if (!remote_metadata_option(candidate)) continue;
            const MetadataRequest *request = metadata_request_locked(candidate->id, now);
            if (!request || request->state != META_DONE) group_pending = true;
            if (!request) continue;
            metadata_snapshot(views, row, candidate, request, release->mirror_key, candidate == release);
            if (request->state == META_DONE) {
                int rank = file_rank(&request->file);
                if (rank > file_rank(&best)) {
                    best = request->file;
                    conflicting_size = false;
                } else if (rank == file_rank(&best) && request->file.size != best.size) {
                    conflicting_size = true;
                }
            }
        } while (remote_next_mirror_locked(release, after, &alternate) &&
                 (copy_text(after, sizeof after, alternate.id), candidate = &alternate, true));
        pending |= group_pending;
        if (mirrors > 1) cJSON_AddNumberToObject(row, "mirrorCount", mirrors);
        if (conflicting_size) {
            best.size = 0;
            best.exact = false;
            cJSON_AddBoolToObject(row, "mirrorSizeConflict", true);
        }
        if (!group_pending && file_rank(&best)) {
            cJSON_AddNumberToObject(row, "sizeBytes", (double)best.size);
            cJSON_AddBoolToObject(row, "exact", best.exact);
            if (best.filename[0]) cJSON_AddStringToObject(row, "filename", best.filename);
            if (best.format[0]) cJSON_AddStringToObject(row, "format", best.format);
            if (best.compression[0]) cJSON_AddStringToObject(row, "compression", best.compression);
            cJSON_AddItemToArray(files, row);
        } else cJSON_Delete(row);
    }
    cJSON_AddBoolToObject(root, "pending", pending);
    cJSON_AddBoolToObject(root, "truncated", ids && *ids);
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
        cJSON *links = NULL;
        const Release *release = release_find(request->id);
        if (release && remote_source_enabled_locked(release)) {
            Release candidate = *release;
            pthread_mutex_unlock(&orbit.mutex);
            remote_download_metadata(&candidate, &file, &links);
            pthread_mutex_lock(&orbit.mutex);
        }
        request->file = file;
        request->public_links = links;
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
        bool forced = remote.pending;
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
            if (snapshots[i].enabled && strcmp(snapshots[i].id, PADRINO_SOURCE) && (full || !snapshots[i].entries) &&
                (!pspkg_source(snapshots[i].id) || forced || !snapshots[i].fetched ||
                 now - snapshots[i].fetched >= PSPKG_REFRESH_INTERVAL) &&
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
    for (size_t i = 0; i < METADATA_LIMIT; i++) cJSON_Delete(metadata.requests[i].public_links);
    memset(metadata.requests, 0, sizeof metadata.requests);
    if (remote.cache_fd >= 0) close(remote.cache_fd);
    pthread_mutex_lock(&orbit.mutex);
    free(orbit.remote_releases);
    padrino_catalog_clear();
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
