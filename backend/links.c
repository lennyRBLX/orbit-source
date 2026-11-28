/* Download links the user pastes (POST /api/v1/links), for files outside the
 * catalogue: a direct link from a file host, TorBox or another debrid service.
 *
 * Each link first gets one small request on this file's own thread (the API
 * answers every request on one thread, so it must never wait for a server). The
 * answer gives the file's name, size and identity, and must be a file, not a web
 * page. Only https links, and the provider guard refuses this console's own
 * network on every connection and redirect. Then the file joins Downloads like a
 * catalogue game: the same queue, connections, pause and resume, and unpacking for
 * one-part ZIP, 7z and RAR archives. Parts of a split archive are kept as they are.
 *
 * Links stay private: status replies name only the host, and the saved queue keeps
 * the link so a download can resume. One batch of up to 20 links at a time. */
#include "links.h"
#include "image_format.h"
#include <ctype.h>
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool started, stopping, pending;
static atomic_bool cancel;
typedef struct {
    char url[sizeof ((Release *)0)->url];
    char host[128], filename[sizeof ((Release *)0)->filename], state[12], message[256];
    char job_id[sizeof ((Job *)0)->id];
} Item;
static struct {
    char id[33], state[12];
    char replace[sizeof ((Job *)0)->id]; /* Set: items[0] is a fresh link for this download. */
    Storage drive;
    Item items[ORBIT_MAX_LINKS];
    unsigned count;
} batch = {.state = "idle"};

bool link_url_allowed(const char *url) {
    if (!url || !*url || strlen(url) >= sizeof ((Release *)0)->url) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++)
        if (*p <= 32 || *p >= 127) return false;
    CURLU *parsed = curl_url();
    if (!parsed) return false;
    char *scheme = NULL, *host = NULL, *user = NULL, *password = NULL, *fragment = NULL;
    bool ok = !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
              !curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) &&
              !curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    curl_url_get(parsed, CURLUPART_USER, &user, 0);
    curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0);
    curl_url_get(parsed, CURLUPART_FRAGMENT, &fragment, 0);
    ok = ok && host && *host && !user && !password && !fragment;
#ifdef ORBIT_TEST
    ok = ok && !strcmp(scheme, "http") && !strcmp(host, "127.0.0.1");
#else
    ok = ok && !strcmp(scheme, "https");
#endif
    curl_free(scheme); curl_free(host); curl_free(user); curl_free(password); curl_free(fragment);
    curl_url_cleanup(parsed);
    return ok;
}
void link_host(const char *url, char *out, size_t cap) {
    out[0] = 0;
    CURLU *parsed = curl_url();
    char *host = NULL;
    if (parsed && !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
        !curl_url_get(parsed, CURLUPART_HOST, &host, 0) && host && strlen(host) < cap)
        copy_text(out, cap, host);
    curl_free(host);
    curl_url_cleanup(parsed);
}
/* The last part of a link's path, decoded. */
static bool path_name(const char *url, char *out, size_t cap) {
    if (!url || !*url) return false;
    CURLU *parsed = curl_url();
    char *path = NULL;
    bool ok = parsed && !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
              !curl_url_get(parsed, CURLUPART_PATH, &path, 0) && path;
    if (ok) {
        const char *last = strrchr(path, '/');
        last = last ? last + 1 : path;
        ok = *last && provider_percent_decode(last, strlen(last), out, cap) && *out;
    }
    curl_free(path);
    curl_url_cleanup(parsed);
    return ok;
}
static bool ends_with(const char *name, const char *suffix) {
    size_t a = strlen(name), b = strlen(suffix);
    return a > b && !strcasecmp(name + a - b, suffix);
}
/* A name that is safe on every PS5 drive: no folders, control characters or
 * characters exFAT refuses, no leading dots and never "..", at most 150 bytes
 * of whole UTF-8 characters, keeping a short extension when it must shorten. */
static void clean_name(const char *raw, char *out, size_t cap) {
    const char *base = raw;
    for (const char *p = raw; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char name[2048];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)base; *p && n + 1 < sizeof name; p++) {
        if (*p < 32 || *p == 127) continue;
        char c = strchr("\"*:<>?|", *p) ? '_' : (char)*p;
        if (c == '.' && n && name[n - 1] == '.') continue;
        name[n++] = c;
    }
    name[n] = 0;
    if (!utf8_text(name))
        for (char *p = name; *p; p++)
            if ((unsigned char)*p >= 0x80) *p = '_';
    char *start = name;
    while (*start == ' ' || *start == '.') start++;
    n = strlen(start);
    while (n && (start[n - 1] == ' ' || start[n - 1] == '.')) start[--n] = 0;
    if (!n) { copy_text(out, cap, "download"); return; }
    size_t limit = cap - 1 < 150 ? cap - 1 : 150;
    if (n <= limit) { copy_text(out, cap, start); return; }
    const char *dot = strrchr(start, '.');
    size_t extension = dot && strlen(dot) <= 16 && dot != start ? strlen(dot) : 0;
    size_t keep = limit - extension;
    while (keep && ((unsigned char)start[keep] & 0xc0) == 0x80) keep--;
    memcpy(out, start, keep);
    memcpy(out + keep, start + n - extension, extension);
    out[keep + extension] = 0;
}
void link_filename(const char *disposition, const char *final_url, const char *url, char *out, size_t cap) {
    char name[2048] = "";
    if (!(disposition && *disposition && provider_content_filename(disposition, name, sizeof name)) &&
        !path_name(final_url, name, sizeof name) && !path_name(url, name, sizeof name))
        name[0] = 0;
    clean_name(name, out, cap);
}
/* game.part2.rar, game.part02.rar: one part of a split RAR archive. */
static bool rar_part(const char *name) {
    size_t n = strlen(name);
    if (n < 10 || strcasecmp(name + n - 4, ".rar")) return false;
    size_t digits = n - 4;
    while (digits && isdigit((unsigned char)name[digits - 1])) digits--;
    return digits < n - 4 && digits >= 5 && !strncasecmp(name + digits - 5, ".part", 5);
}
void link_kind(Release *r) {
    const char *name = r->filename;
    r->compression[0] = 0;
    r->archive_unverified = false;
    if (ends_with(name, ".rar") && !rar_part(name)) copy_text(r->compression, sizeof r->compression, "rar");
    else if (ends_with(name, ".zip")) copy_text(r->compression, sizeof r->compression, "zip");
    else if (ends_with(name, ".7z")) copy_text(r->compression, sizeof r->compression, "7z");
    if (r->compression[0]) {
        /* Like catalogue archives whose contents aren't listed: Orbit looks inside once
         * it has the file, then checks the room to unpack it. */
        r->archive_unverified = true;
        copy_text(r->format, sizeof r->format, "Unknown");
    } else {
        const char *image = image_format_for_suffix(strrchr(name, '.'));
        copy_text(r->format, sizeof r->format, image ? image :
                  ends_with(name, ".pkg") ? "PKG" : ends_with(name, ".fpkg") ? "FPKG" : "File");
    }
}
bool link_release_valid(const Release *r) {
    if (strcmp(r->source, "link") || strncmp(r->id, "link-", 5) || strlen(r->id) != 21 ||
        strspn(r->id + 5, "0123456789abcdef") != 16 || !link_url_allowed(r->url) ||
        !r->title[0] || !utf8_text(r->title) || !r->filename[0] || !utf8_text(r->filename) ||
        r->filename[0] == '.' || strlen(r->filename) > 150 || r->size < 1 ||
        strpbrk(r->filename, "/\\\"*:<>?|") || strstr(r->filename, "..") ||
        r->title_id[0] || r->game_id[0] || r->platform[0] || r->sha256[0] || r->browser_url[0] ||
        r->addon_id[0] || r->base_game_id[0] || r->base_title[0] || r->addon_type[0] ||
        r->content_id[0] || r->requirements[0] || r->content_format[0] || r->asset_compression[0] ||
        r->archive_password[0] || r->extracted_size)
        return false;
    char clean[sizeof r->filename];
    clean_name(r->filename, clean, sizeof clean);
    Release kind = *r;
    link_kind(&kind);
    /* Older queues saved these three extensions with the generic File label. */
    bool legacy = !strcmp(r->format, "File") &&
        (!strcmp(kind.format, "FFPKG") || !strcmp(kind.format, "FFPFS") || !strcmp(kind.format, "FPKG"));
    return !strcmp(clean, r->filename) && !strcmp(kind.compression, r->compression) &&
           (legacy || !strcmp(kind.format, r->format)) && kind.archive_unverified == r->archive_unverified;
}
/* The same link always gets the same id, so adding it twice to a drive is caught. */
static void link_id(const char *url, char *out, size_t cap) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    char hex[17] = "0000000000000000";
    if (EVP_Digest(url, strlen(url), digest, &length, EVP_sha256(), NULL) == 1 && length >= 8)
        for (unsigned i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    snprintf(out, cap, "link-%s", hex);
}
static bool drive_locked(const char *id, const Storage *pin, Storage *out) {
    Storage drives[ORBIT_MAX_STORAGE];
    size_t count = storage_list(drives);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(drives[i].id, id) && (!pin || (drives[i].device == pin->device && drives[i].inode == pin->inode))) {
            *out = drives[i];
            return true;
        }
    return false;
}
static void check(unsigned index) {
    Release r = {0};
    Storage pin;
    char replace[sizeof batch.replace];
    pthread_mutex_lock(&lock);
    copy_text(r.url, sizeof r.url, batch.items[index].url);
    copy_text(replace, sizeof replace, batch.replace);
    pin = batch.drive;
    copy_text(batch.items[index].state, sizeof batch.items[index].state, "checking");
    copy_text(batch.items[index].message, sizeof batch.items[index].message, "Checking the file…");
    pthread_mutex_unlock(&lock);
    copy_text(r.source, sizeof r.source, "link");
    copy_text(r.id, sizeof r.id, "link-check");
    LinkProbe probe;
    char error[256] = {0}, job_id[sizeof ((Job *)0)->id] = {0};
    int invalid = transfer_probe_link(&r, &cancel, &probe, error, sizeof error);
    if (!invalid) {
        link_filename(probe.disposition, probe.final_url, r.url, r.filename, sizeof r.filename);
        copy_text(r.title, sizeof r.title, r.filename);
        r.size = probe.size;
        link_kind(&r);
        link_id(r.url, r.id, sizeof r.id);
    }
    OPENSSL_cleanse(probe.final_url, sizeof probe.final_url);
    bool queued = false;
    pthread_mutex_lock(&orbit.mutex);
    Storage drive;
    if (orbit.stop || atomic_load(&cancel)) {
        copy_text(error, sizeof error, "Orbit is stopping. Nothing was added to Downloads.");
    } else if (invalid) {
        if (!error[0]) copy_text(error, sizeof error, "This link didn't give Orbit a file.");
    } else if (replace[0]) {
        Job *job = job_find(replace);
        if (!job) copy_text(error, sizeof error, "This download is no longer in Downloads.");
        else if (job_replace_link_locked(job, &r, probe.etag, probe.modified, error, sizeof error) == 200) {
            copy_text(job_id, sizeof job_id, job->id);
            queued = true;
        }
    } else if (!drive_locked(pin.id, &pin, &drive)) {
        copy_text(error, sizeof error, "Reconnect the selected drive first.");
    } else {
        Job *job = NULL;
        int code = job_create_link_locked(&r, &drive, probe.etag, probe.modified, error, sizeof error, &job);
        if ((code == 200 || code == 201) && job) {
            copy_text(job_id, sizeof job_id, job->id);
            queued = true;
        }
    }
    pthread_mutex_unlock(&orbit.mutex);
    OPENSSL_cleanse(r.url, sizeof r.url);
    pthread_mutex_lock(&lock);
    Item *item = &batch.items[index];
    OPENSSL_cleanse(item->url, sizeof item->url);
    if (!invalid) copy_text(item->filename, sizeof item->filename, r.filename);
    copy_text(item->state, sizeof item->state, queued ? "queued" : "failed");
    copy_text(item->message, sizeof item->message,
              queued ? (replace[0] ? "Link replaced. The download continues." : "Added to Downloads.") : error);
    copy_text(item->job_id, sizeof item->job_id, job_id);
    pthread_mutex_unlock(&lock);
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        while (!pending && !stopping)
            pthread_cond_wait(&wake, &lock);
        if (stopping) break;
        pending = false;
        for (unsigned i = 0; i < batch.count && !stopping; i++) {
            pthread_mutex_unlock(&lock);
            check(i);
            pthread_mutex_lock(&lock);
        }
        for (unsigned i = 0; i < batch.count; i++) {
            OPENSSL_cleanse(batch.items[i].url, sizeof batch.items[i].url);
            if (!strcmp(batch.items[i].state, "waiting")) {
                copy_text(batch.items[i].state, sizeof batch.items[i].state, "failed");
                copy_text(batch.items[i].message, sizeof batch.items[i].message,
                          "Orbit is stopping. Nothing was added to Downloads.");
            }
        }
        copy_text(batch.state, sizeof batch.state, "done");
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
int links_start(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc) rc = pthread_create(&thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    started = !rc;
    return rc;
}
void links_stop(void) {
    if (!started) return;
    atomic_store(&cancel, true);
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    started = false;
}
/* Starts a batch: the caller filled batch.items' urls and count. */
static int begin_locked(char *error, size_t cap) {
    if (!started || stopping) {
        copy_text(error, cap, "Orbit can't check links right now. Restart Orbit, then try again.");
        return 503;
    }
    if (pending || !strcmp(batch.state, "checking")) {
        copy_text(error, cap, "Orbit is still checking your last links. Try again when it finishes.");
        return 409;
    }
    return 202;
}
int links_request(const cJSON *input, char *error, size_t cap) {
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(input, "urls");
    int count = cJSON_IsArray(urls) ? cJSON_GetArraySize(urls) : 0;
    if (count < 1) { copy_text(error, cap, "Paste at least one link."); return 400; }
    if (count > ORBIT_MAX_LINKS) {
        _Static_assert(ORBIT_MAX_LINKS == 20, "Update the message below");
        copy_text(error, cap, "Add up to 20 links at a time.");
        return 400;
    }
    for (int i = 0; i < count; i++) {
        const cJSON *url = cJSON_GetArrayItem(urls, i);
        if (!cJSON_IsString(url) || !link_url_allowed(url->valuestring)) {
            snprintf(error, cap, "Link %d isn't a download link that starts with https://.", i + 1);
            return 400;
        }
        for (int k = 0; k < i; k++)
            if (!strcmp(cJSON_GetArrayItem(urls, k)->valuestring, url->valuestring)) {
                snprintf(error, cap, "Link %d is the same as link %d.", i + 1, k + 1);
                return 400;
            }
    }
    Storage drive;
    pthread_mutex_lock(&orbit.mutex);
    bool have_drive = drive_locked(json_text(input, "storageId"), NULL, &drive);
    pthread_mutex_unlock(&orbit.mutex);
    if (!have_drive) { copy_text(error, cap, "Reconnect the selected drive first."); return 409; }
    pthread_mutex_lock(&lock);
    int code = begin_locked(error, cap);
    if (code == 202) {
        memset(batch.items, 0, sizeof batch.items);
        batch.count = (unsigned)count;
        batch.drive = drive;
        batch.replace[0] = 0;
        for (int i = 0; i < count; i++) {
            Item *item = &batch.items[i];
            copy_text(item->url, sizeof item->url, cJSON_GetArrayItem(urls, i)->valuestring);
            link_host(item->url, item->host, sizeof item->host);
            copy_text(item->state, sizeof item->state, "waiting");
        }
        random_hex(batch.id, 16);
        copy_text(batch.state, sizeof batch.state, "checking");
        pending = true;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return code;
}
int links_replace_request(const char *job_id, const cJSON *input, char *error, size_t cap) {
    const char *url = json_text(input, "url");
    if (!link_url_allowed(url)) {
        copy_text(error, cap, "Paste a download link that starts with https://.");
        return 400;
    }
    pthread_mutex_lock(&orbit.mutex);
    Job *job = job_find(job_id);
    int code = !job ? 404 : !strcmp(job->release.source, "link") ? 200 : 400;
    if (code == 404) copy_text(error, cap, "This download is no longer in Downloads.");
    else if (code == 400) copy_text(error, cap, "Only downloads you added from a link can take a new link.");
    else if (!job_can_replace_link(job)) {
        copy_text(error, cap, "Pause the download first, or wait until it stops.");
        code = 409;
    }
    pthread_mutex_unlock(&orbit.mutex);
    if (code != 200) return code;
    pthread_mutex_lock(&lock);
    code = begin_locked(error, cap);
    if (code == 202) {
        memset(batch.items, 0, sizeof batch.items);
        batch.count = 1;
        memset(&batch.drive, 0, sizeof batch.drive);
        copy_text(batch.replace, sizeof batch.replace, job_id);
        copy_text(batch.items[0].url, sizeof batch.items[0].url, url);
        link_host(url, batch.items[0].host, sizeof batch.items[0].host);
        copy_text(batch.items[0].state, sizeof batch.items[0].state, "waiting");
        random_hex(batch.id, 16);
        copy_text(batch.state, sizeof batch.state, "checking");
        pending = true;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return code;
}
cJSON *links_status(void) {
    pthread_mutex_lock(&lock);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", batch.id);
    cJSON_AddStringToObject(o, "state", batch.state);
    cJSON_AddStringToObject(o, "replaceJobId", batch.replace);
    cJSON *items = cJSON_AddArrayToObject(o, "items");
    for (unsigned i = 0; i < batch.count; i++) {
        const Item *item = &batch.items[i];
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "host", item->host);
        cJSON_AddStringToObject(x, "filename", item->filename);
        cJSON_AddStringToObject(x, "state", item->state);
        cJSON_AddStringToObject(x, "message", item->message);
        cJSON_AddStringToObject(x, "jobId", item->job_id);
        cJSON_AddItemToArray(items, x);
    }
    pthread_mutex_unlock(&lock);
    return o;
}
bool links_busy(void) {
    pthread_mutex_lock(&lock);
    bool busy = pending || !strcmp(batch.state, "checking");
    pthread_mutex_unlock(&lock);
    return busy;
}
