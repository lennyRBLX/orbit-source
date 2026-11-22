/* Announcements the owner posts in the public repo's announcements.json: a title and a message
 * the apps show as a notice until it is closed (docs/announcements.md). Read at most hourly, the
 * last valid copy kept in Orbit's state folder. The apps get the ones that show now and remember
 * which they have shown. Never fetch a user-supplied destination here. */
#define _POSIX_C_SOURCE 200809L
#include "orbit.h"
#include "announcements.h"
#include "ca.h"
#include <curl/curl.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANNOUNCEMENTS_URL "https://raw.githubusercontent.com/saawant12/orbit-store-ps5/main/announcements.json"
#define MAX_FEED 65536
#define MAX_ANNOUNCEMENTS 10
#define MAX_LANGUAGES 32
#define TITLE_LIMIT 120  /* bytes */
#define MESSAGE_LIMIT 600
#define REFRESH (60 * 60)
static pthread_mutex_t announcements_lock = PTHREAD_MUTEX_INITIALIZER;
static char saved[MAX_FEED + 1];
static time_t checked;
static bool loaded;
static bool refreshing;
static bool worker_started;
static pthread_t worker;

static bool text_ok(const cJSON *v, size_t limit, bool required) {
    if (!v) return !required;
    if (!cJSON_IsString(v)) return false;
    size_t n = strlen(v->valuestring);
    return (n || !required) && n <= limit && utf8_text(v->valuestring);
}
static bool id_ok(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 64 || *s == '-') return false;
    for (; *s; s++)
        if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '-') return false;
    return true;
}
/* "pt-BR", "zh-Hans", "de": the apps' language codes. */
static bool language_ok(const char *s) {
    size_t n = strlen(s), i = 0;
    while (i < n && islower((unsigned char)s[i])) i++;
    if (i < 2 || i > 3) return false;
    if (i == n) return true;
    if (s[i] != '-' || n - i - 1 < 2 || n - i - 1 > 4) return false;
    for (i++; i < n; i++)
        if (!isalpha((unsigned char)s[i])) return false;
    return true;
}
/* 2026-11-03 or 2026-11-03T18:00:00Z, in UTC. */
static bool when(const cJSON *v, time_t *out) {
    if (!cJSON_IsString(v)) return false;
    int y, mo, d, h = 0, mi = 0, s = 0, used = 0;
    const char *text = v->valuestring;
    size_t length = strlen(text);
    if (length != 10 && length != 20) return false;
    for (size_t i = 0; i < length; i++) {
        char expected = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : i == 19 ? 'Z' : 0;
        if (expected ? text[i] != expected : text[i] < '0' || text[i] > '9') return false;
    }
    if (sscanf(text, "%4d-%2d-%2d%n", &y, &mo, &d, &used) != 3 || used != 10) return false;
    if (text[10]) {
        int more = 0;
        if (sscanf(text + 10, "T%2d:%2d:%2dZ%n", &h, &mi, &s, &more) != 3 || more != 10 || text[20]) return false;
    }
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (y < 2020 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > month_days[mo - 1] + (mo == 2 && leap) ||
        h > 23 || mi > 59 || s > 59)
        return false;
    /* Days since 1970-01-01, from the civil date. */
    int yy = y - (mo <= 2), era = yy / 400;
    unsigned yoe = (unsigned)(yy - era * 400), doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    long days = (long)era * 146097 + (long)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    *out = (time_t)(days * 86400L + h * 3600L + mi * 60L + s);
    return true;
}
static bool version_ok(const cJSON *v) {
    return !v || (cJSON_IsString(v) && strlen(v->valuestring) < 32 &&
                  orbit_version_compare(v->valuestring, v->valuestring) == 0);
}
static cJSON *texts(const cJSON *from) {
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(from, "title"),
                *message = cJSON_GetObjectItemCaseSensitive(from, "message");
    if (!text_ok(title, TITLE_LIMIT, true) || !text_ok(message, MESSAGE_LIMIT, false)) return NULL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", title->valuestring);
    if (message && *message->valuestring) cJSON_AddStringToObject(o, "message", message->valuestring);
    return o;
}
static cJSON *entry(const cJSON *in, time_t now, const char *version) {
    if (!cJSON_IsObject(in)) return NULL;
    const char *id = json_text(in, "id");
    const cJSON *starts = cJSON_GetObjectItemCaseSensitive(in, "startsAt"),
                *ends = cJSON_GetObjectItemCaseSensitive(in, "endsAt"),
                *min = cJSON_GetObjectItemCaseSensitive(in, "minVersion"),
                *max = cJSON_GetObjectItemCaseSensitive(in, "maxVersion"),
                *translations = cJSON_GetObjectItemCaseSensitive(in, "translations"), *t;
    time_t from = 0, until = 0;
    if (!id_ok(id) || (starts && !when(starts, &from)) || (ends && !when(ends, &until)) ||
        !version_ok(min) || !version_ok(max) || (translations && !cJSON_IsObject(translations)) ||
        cJSON_GetArraySize(translations) > MAX_LANGUAGES)
        return NULL;
    /* Not yet, no longer, or not for this version. */
    if ((starts && now < from) || (ends && now >= until) ||
        (min && orbit_version_compare(version, min->valuestring) < 0) ||
        (max && orbit_version_compare(version, max->valuestring) > 0))
        return NULL;
    cJSON *out = texts(in);
    if (!out) return NULL;
    cJSON_AddStringToObject(out, "id", id);
    cJSON *languages = NULL;
    cJSON_ArrayForEach(t, translations) {
        cJSON *text = language_ok(t->string) ? texts(t) : NULL;
        if (!text) continue; /* A bad translation falls back to the English text. */
        if (!languages) languages = cJSON_AddObjectToObject(out, "translations");
        cJSON_AddItemToObject(languages, t->string, text);
    }
    return out;
}
cJSON *announcements_parse(const char *json, time_t now, const char *version) {
    cJSON *list = cJSON_CreateArray();
    cJSON *feed = json && strlen(json) <= MAX_FEED ? cJSON_Parse(json) : NULL;
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(feed, "announcements"), *item;
    if (!cJSON_IsArray(items)) items = NULL;
    cJSON_ArrayForEach(item, items) {
        if (cJSON_GetArraySize(list) >= MAX_ANNOUNCEMENTS) break;
        cJSON *one = entry(item, now, version), *seen;
        bool twice = false;
        cJSON_ArrayForEach(seen, list) twice |= one && !strcmp(json_text(seen, "id"), json_text(one, "id"));
        if (one && !twice) cJSON_AddItemToArray(list, one);
        else cJSON_Delete(one);
    }
    cJSON_Delete(feed);
    return list;
}
static bool valid(const char *json) {
    cJSON *feed = json ? cJSON_Parse(json) : NULL;
    bool ok = cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(feed, "announcements"));
    cJSON_Delete(feed);
    return ok;
}
static const char *feed_url(void) {
#ifdef ORBIT_TEST
    const char *url = getenv("ORBIT_TEST_ANNOUNCEMENTS_URL");
    if (url && !strncmp(url, "http://127.0.0.1:", 17)) return url;
#endif
    return ANNOUNCEMENTS_URL;
}
typedef struct { char data[MAX_FEED + 1]; size_t size; } Buffer;
static size_t collect(void *data, size_t a, size_t b, void *ctx) {
    Buffer *buf = ctx; if (a && b > MAX_FEED / a) return 0;
    size_t n = a*b; if (n > MAX_FEED - buf->size) return 0;
    memcpy(buf->data + buf->size, data, n); buf->size += n; buf->data[buf->size] = 0; return n;
}
/* The API daemon has one polling thread. Network I/O must never hold that
 * thread or the cache lock: it would also stop receiver uploads and controls. */
static void *refresh(void *unused) {
    (void)unused;
    static Buffer b; b.size = 0; b.data[0] = 0; CURL *c = curl_easy_init(); long status = 0;
        if (c) {
            const char *url = feed_url();
            struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
            curl_easy_setopt(c, CURLOPT_URL, url); curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
            curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, strncmp(url, "https://", 8) ? "http" : "https");
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L); curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &b); curl_easy_setopt(c, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
            CURLcode rc = curl_easy_perform(c); curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status); curl_easy_cleanup(c);
            pthread_mutex_lock(&announcements_lock);
            /* 404 until the owner first posts one: nothing to show, nothing kept. */
            if (rc == CURLE_OK && status == 404) { saved[0] = 0; unlinkat(orbit.state_fd, "announcements.json", 0); }
            if (rc == CURLE_OK && status == 200 && !memchr(b.data, 0, b.size) && valid(b.data)) {
                snprintf(saved, sizeof saved, "%s", b.data);
                int fd = openat(orbit.state_fd, "announcements.json.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
                if (fd >= 0) { bool ok = write(fd, saved, strlen(saved)) == (ssize_t)strlen(saved); close(fd);
                    if (ok) renameat(orbit.state_fd, "announcements.json.tmp", orbit.state_fd, "announcements.json"); }
            }
            pthread_mutex_unlock(&announcements_lock);
        }
    pthread_mutex_lock(&announcements_lock);
    refreshing = false;
    pthread_mutex_unlock(&announcements_lock);
    return NULL;
}
void announcements_stop(void) {
    /* Called after the API thread stops, before state/curl teardown. */
    if (worker_started) pthread_join(worker, NULL);
    worker_started = false;
}
cJSON *announcements_status(void) {
    pthread_mutex_lock(&announcements_lock);
    if (!loaded) {
        loaded = true; int fd = openat(orbit.state_fd, "announcements.json", O_RDONLY | O_NOFOLLOW);
        if (fd >= 0) { static char b[MAX_FEED + 2]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
            if (n > 0 && n <= MAX_FEED) { b[n] = 0; if (valid(b)) snprintf(saved, sizeof saved, "%s", b); }
        }
    }
    time_t now = time(NULL);
    if (!refreshing && (!checked || now < checked || now - checked >= REFRESH)) {
        /* A completed worker no longer takes the lock. Reap it before reuse. */
        if (worker_started) pthread_join(worker, NULL);
        worker_started = false;
        checked = now;
        pthread_attr_t attr;
        if (!pthread_attr_init(&attr)) {
            refreshing = true;
            int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
            if (!rc) rc = pthread_create(&worker, &attr, refresh, NULL);
            pthread_attr_destroy(&attr);
            worker_started = !rc;
            if (rc) refreshing = false;
        }
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "announcements", announcements_parse(saved, now, ORBIT_VERSION));
    cJSON_AddBoolToObject(o, "refreshing", refreshing);
    pthread_mutex_unlock(&announcements_lock); return o;
}
