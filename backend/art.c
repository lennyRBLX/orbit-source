/* Catalogue artwork shared by the browser and native TV app. Authenticated
 * routes accept only catalogue artwork. Workers fetch asynchronously, retaining
 * bounded compressed images on disk so a restart does not refetch every cover. */
#include "ca.h"
#include "art_disk.h"
#include "orbit.h"
#include "catalog_view.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define ART_ENTRIES 256
#define ART_WORKERS 2
#define ART_QUEUE 64
#define ART_MAX_BYTES (12 * 1024 * 1024)
#define ART_BUDGET (32 * 1024 * 1024)
#define ART_RETRY_FAILED 120 /* Seconds before a failed image is fetched again. */
#define ART_KEY 80

typedef enum { ART_EMPTY, ART_PENDING, ART_READY, ART_FAILED } ArtState;
typedef struct {
    char key[ART_KEY];
    char urls[2][2300];
    size_t count;
    bool browser;
    ArtState state;
    unsigned char *data;
    size_t size;
    char mime[16];
    time_t used, failed_at;
} ArtEntry;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t threads[ART_WORKERS];
    size_t started;
    bool stop;
    ArtEntry entries[ART_ENTRIES];
    char queue[ART_QUEUE][ART_KEY];
    size_t queued;
    size_t bytes;
} art = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

typedef struct {
    unsigned char *data;
    size_t size;
} Buffer;

/* Every artwork URL in the catalogue and add-ons, sorted, so the browser
 * proxy's check is a binary search instead of a scan of every entry for each
 * cover (issue 185). The strings belong to orbit.catalog and orbit.addons:
 * read only with orbit.mutex held, rebuilt after the catalogue is replaced. */
static struct {
    const char **urls;
    size_t count;
    unsigned long generation;
    bool built;
} listed;

static int compare_url(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool listed_ready_locked(void) {
    const unsigned long generation = catalog_generation_locked();
    if (listed.built && listed.generation == generation) return true;
    static const char *fields[] = {"cover", "coverFallback", "hero", "heroFallback"};
    const size_t field_count = sizeof fields / sizeof fields[0];
    size_t capacity = field_count * ((size_t)cJSON_GetArraySize(orbit.catalog) +
                                     (size_t)cJSON_GetArraySize(orbit.addons));
    const char **urls = malloc((capacity ? capacity : 1) * sizeof *urls);
    if (!urls) return false;
    size_t count = 0;
    for (unsigned group = 0; group < 2; group++) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, (group ? orbit.addons : orbit.catalog))
            for (size_t i = 0; i < field_count && count < capacity; i++) {
                const char *url = json_text(entry, fields[i]);
                if (*url) urls[count++] = url;
            }
    }
    qsort(urls, count, sizeof *urls, compare_url);
    free(listed.urls);
    listed.urls = urls;
    listed.count = count;
    listed.generation = generation;
    listed.built = true;
    return true;
}

static bool art_identifier(const char *id) {
    size_t n = strlen(id);
    if (!n || n > 63)
        return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_' || *p == '.'))
            return false;
    return strcmp(id, ".") && strcmp(id, "..");
}

/* The image format from its first bytes; the server's Content-Type is not trusted. */
static const char *sniff(const unsigned char *d, size_t n, bool browser) {
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return "image/webp";
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return "image/jpeg";
    if (browser && n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return "image/gif";
    if (browser && n >= 16 && !memcmp(d + 4, "ftyp", 4)) {
        size_t box = ((size_t)d[0] << 24) | ((size_t)d[1] << 16) | ((size_t)d[2] << 8) | d[3];
        if (box >= 16 && box <= n) {
            if (!memcmp(d + 8, "avif", 4) || !memcmp(d + 8, "avis", 4)) return "image/avif";
            for (size_t i = 16; i + 4 <= box && i < 128; i += 4)
                if (!memcmp(d + i, "avif", 4) || !memcmp(d + i, "avis", 4)) return "image/avif";
        }
    }
    return NULL;
}

/* Up to two catalogue URLs for a visible game, in preference order. Returns
 * the count; 0 when the game is unknown, hidden or has no such artwork. */
static size_t urls_for(const char *game_id, bool hero, char out[2][2048]) {
    size_t count = 0;
    pthread_mutex_lock(&orbit.mutex);
    const cJSON *entry;
    for (unsigned group = 0; group < 2 && !count; group++)
    cJSON_ArrayForEach(entry, (group ? orbit.addons : orbit.catalog)) {
        if (strcmp(json_text(entry, "gameId"), game_id) ||
            !source_enabled_locked(release_find(json_text(entry, "id"))))
            continue;
        /* Wide layouts have a publisher banner as hero; ambient ones may have a
         * banner only as heroFallback (docs/api.md). */
        const char *keys[2] = {"cover", "coverFallback"};
        if (hero) {
            bool wide = !strcmp(json_text(entry, "artworkLayout"), "wide");
            keys[0] = wide ? "hero" : "heroFallback";
            keys[1] = wide ? "heroFallback" : NULL;
        }
        for (size_t k = 0; k < 2; k++) {
            const char *url = keys[k] ? json_text(entry, keys[k]) : "";
            if (!strncmp(url, "https://", 8) && strlen(url) < 2048)
                copy_text(out[count++], 2048, url);
        }
        break;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return count;
}

static size_t collect(char *data, size_t a, size_t b, void *context) {
    Buffer *buf = context;
    if (a && b > SIZE_MAX / a)
        return 0;
    size_t n = a * b;
    if (n > ART_MAX_BYTES - buf->size)
        return 0;
    unsigned char *next = realloc(buf->data, buf->size + n);
    if (!next)
        return 0;
    buf->data = next;
    memcpy(next + buf->size, data, n);
    buf->size += n;
    return n;
}

static int stopping(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ctx;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    pthread_mutex_lock(&art.mutex);
    bool stop = art.stop;
    pthread_mutex_unlock(&art.mutex);
    return stop;
}

/* This CDN supports width variants. Other hosts (including PlayStation's
 * preset-based gmedia service) keep their original URLs. Ask for fewer source
 * pixels before the native PNG/JPEG decoder allocates its full-size buffer. */
static void sized_url(const char *source, const char *kind, char *out, size_t cap) {
    static const char prefix[] = "https://image.api.playstation.com/";
    copy_text(out, cap, source);
    if (strncasecmp(source, prefix, sizeof prefix - 1)) return;
    const char *fragment = strchr(source, '#');
    const char *end = fragment ? fragment : source + strlen(source);
    const char *query = strchr(source, '?');
    if (query && query >= end) query = NULL;
    unsigned width = !strcmp(kind, "hero") ? 1920 : !strcmp(kind, "thumbnail") ? 640 : 600;
    char resized[2300];
    size_t used = (size_t)((query ? query : end) - source);
    if (used >= sizeof resized) return;
    memcpy(resized, source, used);
    bool has_query = false;
    for (const char *part = query ? query + 1 : end; part < end;) {
        const char *next = memchr(part, '&', (size_t)(end - part));
        if (!next) next = end;
        const char *equal = memchr(part, '=', (size_t)(next - part));
        int key_length = 0;
        char *key = curl_easy_unescape(NULL, part, (int)((equal ? equal : next) - part), &key_length);
        bool is_width = key && key_length == 1 && key[0] == 'w';
        curl_free(key);
        if (is_width && equal) {
            int length = 0;
            char *value = curl_easy_unescape(NULL, equal + 1, (int)(next - equal - 1), &length);
            unsigned candidate = 0;
            bool valid = value && length > 0;
            for (int i = 0; valid && i < length; i++) {
                if (value[i] < '0' || value[i] > '9' || candidate > 1000000u) valid = false;
                else candidate = candidate * 10u + (unsigned)(value[i] - '0');
            }
            if (valid && candidate && candidate < width) width = candidate;
            curl_free(value);
        } else if (!is_width && next > part) {
            size_t length = (size_t)(next - part);
            if (used + length + 1 >= sizeof resized) return;
            resized[used++] = has_query ? '&' : '?';
            memcpy(resized + used, part, length); used += length;
            has_query = true;
        }
        part = next < end ? next + 1 : end;
    }
    int added = snprintf(resized + used, sizeof resized - used, "%cw=%u%s",
                         has_query ? '&' : '?', width, fragment ? fragment : "");
    if (added < 0 || (size_t)added >= sizeof resized - used || used + (size_t)added >= cap) return;
    copy_text(out, cap, resized);
}

/* Fetches one URL. In test builds ORBIT_TEST_ART_ORIGIN replaces the scheme
 * and host with a loopback fixture, keeping the path. */
static bool fetch(const char *source, Buffer *buf, const char **mime, bool browser) {
    char url[2300];
    const char *protocols = "https";
    copy_text(url, sizeof url, source);
#ifdef ORBIT_TEST
    const char *origin = getenv("ORBIT_TEST_ART_ORIGIN");
    if (origin && *origin) {
        char sized[2300];
        copy_text(sized, sizeof sized, url);
        const char *path = strchr(sized + 8, '/');
        snprintf(url, sizeof url, "%s%s", origin, path ? path : "/");
        protocols = "http";
    }
#endif
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)ART_MAX_BYTES);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopping);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    *mime = code == CURLE_OK && status == 200 ? sniff(buf->data, buf->size, browser) : NULL;
    return *mime != NULL;
}

static ArtEntry *find(const char *key) {
    for (size_t i = 0; i < ART_ENTRIES; i++)
        if (art.entries[i].state != ART_EMPTY && !strcmp(art.entries[i].key, key))
            return &art.entries[i];
    return NULL;
}

/* Drops the least recently served images until the cache fits its budget. */
static void trim_locked(const ArtEntry *keep) {
    while (art.bytes > ART_BUDGET) {
        ArtEntry *oldest = NULL;
        for (size_t i = 0; i < ART_ENTRIES; i++) {
            ArtEntry *e = &art.entries[i];
            if (e->state == ART_READY && e != keep && (!oldest || e->used < oldest->used))
                oldest = e;
        }
        if (!oldest)
            return;
        art.bytes -= oldest->size;
        free(oldest->data);
        memset(oldest, 0, sizeof *oldest);
    }
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&art.mutex);
    while (!art.stop) {
        if (!art.queued) {
            pthread_cond_wait(&art.changed, &art.mutex);
            continue;
        }
        char key[ART_KEY];
        copy_text(key, sizeof key, art.queue[0]);
        memmove(art.queue[0], art.queue[1], (art.queued - 1) * ART_KEY);
        art.queued--;
        ArtEntry *pending = find(key);
        char urls[2][2300];
        size_t count = pending ? pending->count : 0;
        bool browser = pending && pending->browser;
        if (pending) memcpy(urls, pending->urls, sizeof urls);
        pthread_mutex_unlock(&art.mutex);

        Buffer buf = {0};
        const char *mime = NULL;
        /* Any already-cached candidate beats waiting for a broken primary.
         * The URL/size variant is the disk identity, including after refresh. */
        for (size_t i = 0; i < count && !mime; i++) {
            if (art_disk_load(urls[i], &buf.data, &buf.size)) {
                mime = sniff(buf.data, buf.size, browser);
                if (!mime) { free(buf.data); buf = (Buffer){0}; }
            }
        }
        for (size_t i = 0; i < count && !mime; i++) {
            free(buf.data);
            buf = (Buffer){0};
            if (fetch(urls[i], &buf, &mime, browser)) art_disk_save(urls[i], buf.data, buf.size);
        }

        pthread_mutex_lock(&art.mutex);
        ArtEntry *e = find(key);
        if (!e || e->state != ART_PENDING) {
            free(buf.data);
            continue;
        }
        e->used = time(NULL);
        if (mime) {
            e->state = ART_READY;
            e->data = buf.data;
            e->size = buf.size;
            copy_text(e->mime, sizeof e->mime, mime);
            art.bytes += buf.size;
            trim_locked(e);
        } else {
            free(buf.data);
            e->state = ART_FAILED;
            e->failed_at = time(NULL);
        }
    }
    pthread_mutex_unlock(&art.mutex);
    return NULL;
}

static int request_art(char urls[2][2300], size_t count, bool browser, unsigned char **data,
                       size_t *size, char *mime, size_t mime_cap) {
    *data = NULL;
    *size = 0;
    char identity[4610], key[ART_KEY];
    snprintf(identity, sizeof identity, "%s\n%s\n%s", browser ? "browser" : "native", urls[0], count > 1 ? urls[1] : "");
    art_disk_key(identity, key);
    if (!key[0]) return 503;
    time_t now = time(NULL);
    int code = 202;
    pthread_mutex_lock(&art.mutex);
    ArtEntry *e = find(key);
    if (e && e->state == ART_READY) {
        *data = malloc(e->size);
        if (*data) {
            memcpy(*data, e->data, e->size);
            *size = e->size;
            copy_text(mime, mime_cap, e->mime);
            e->used = now;
            code = 200;
        } else {
            code = 503;
        }
    } else if (e && e->state == ART_PENDING) {
        code = 202;
    } else if (e && e->state == ART_FAILED && now - e->failed_at < ART_RETRY_FAILED) {
        code = 502;
    } else if (!art.started || art.queued == ART_QUEUE) {
        code = 503;
    } else {
        /* A free slot, else the least recently used finished one. */
        for (size_t i = 0; i < ART_ENTRIES && !e; i++)
            if (art.entries[i].state == ART_EMPTY)
                e = &art.entries[i];
        for (size_t i = 0; i < ART_ENTRIES && (!e || e->state != ART_EMPTY); i++) {
            ArtEntry *c = &art.entries[i];
            if (c->state != ART_PENDING && (!e || c->used < e->used))
                e = c;
        }
        if (!e) {
            code = 503;
        } else {
            if (e->state == ART_READY)
                art.bytes -= e->size;
            free(e->data);
            memset(e, 0, sizeof *e);
            copy_text(e->key, sizeof e->key, key);
            memcpy(e->urls, urls, sizeof e->urls);
            e->count = count;
            e->browser = browser;
            e->state = ART_PENDING;
            e->used = now;
            copy_text(art.queue[art.queued++], ART_KEY, key);
            pthread_cond_signal(&art.changed);
        }
    }
    pthread_mutex_unlock(&art.mutex);
    return code;
}

static bool valid_kind(const char *kind) {
    return kind && (!strcmp(kind, "cover") || !strcmp(kind, "hero") || !strcmp(kind, "thumbnail"));
}
int art_get(const char *game_id, const char *kind, unsigned char **data, size_t *size, char *mime,
            size_t mime_cap) {
    *data = NULL; *size = 0;
    if (!art_identifier(game_id) || !valid_kind(kind)) return 404;
    char source[2][2048], urls[2][2300] = {{0}};
    size_t count = urls_for(game_id, strcmp(kind, "cover") != 0, source);
    if (!count) return 404;
    for (size_t i = 0; i < count; i++) sized_url(source[i], kind, urls[i], sizeof urls[i]);
    return request_art(urls, count, false, data, size, mime, mime_cap);
}
/* Browser variants may prefer the publisher banner. Accept only exact artwork
 * URLs already supplied by our catalogue; this is never an arbitrary proxy. */
int art_get_urls(const char *source, const char *fallback, const char *kind, unsigned char **data,
                 size_t *size, char *mime, size_t mime_cap) {
    *data = NULL; *size = 0;
    if (!source || !valid_kind(kind)) return 404;
    const char *input[2] = {source, fallback};
    size_t count = fallback && *fallback && strcmp(source, fallback) ? 2 : 1;
    bool known[2] = {false, false};
    pthread_mutex_lock(&orbit.mutex);
    const bool ready = listed_ready_locked();
    for (size_t i = 0; ready && i < count; i++)
        known[i] = bsearch(&input[i], listed.urls, listed.count, sizeof *listed.urls, compare_url) != NULL;
    pthread_mutex_unlock(&orbit.mutex);
    if (!ready) return 503;
    char urls[2][2300] = {{0}};
    for (size_t i = 0; i < count; i++) {
        if (!known[i] || strncmp(input[i], "https://", 8) || strlen(input[i]) >= 2048) return 404;
        sized_url(input[i], kind, urls[i], sizeof urls[i]);
    }
    return request_art(urls, count, true, data, size, mime, mime_cap);
}

int art_start(void) {
    art_disk_open(orbit.state_dir);
    art.stop = false;
    art.started = 0;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    pthread_mutex_lock(&art.mutex);
    for (size_t i = 0; !rc && i < ART_WORKERS; i++) {
        rc = pthread_create(&art.threads[i], &attr, worker, NULL);
        if (!rc)
            art.started++;
    }
    pthread_mutex_unlock(&art.mutex);
    pthread_attr_destroy(&attr);
    return rc ? -1 : 0;
}

void art_stop(void) {
    pthread_mutex_lock(&art.mutex);
    size_t started = art.started;
    art.stop = true;
    pthread_cond_broadcast(&art.changed);
    pthread_mutex_unlock(&art.mutex);
    for (size_t i = 0; i < started; i++)
        pthread_join(art.threads[i], NULL);
    for (size_t i = 0; i < ART_ENTRIES; i++)
        free(art.entries[i].data);
    memset(art.entries, 0, sizeof art.entries);
    art.bytes = 0;
    art.queued = art.started = 0;
    art_disk_close();
    pthread_mutex_lock(&orbit.mutex);
    free(listed.urls);
    listed.urls = NULL;
    listed.count = 0;
    listed.built = false;
    pthread_mutex_unlock(&orbit.mutex);
}
