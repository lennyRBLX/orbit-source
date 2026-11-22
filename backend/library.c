#include "orbit.h"
#include "kraken.h"
#include <ctype.h>
#include <dirent.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Library: the games Kraken Mount runs (kraken.c reads its loopback API) and the
 * games installed on the PS5 from PKG files, which Orbit finds on disk itself.
 * Orbit never changes Kraken Mount's games; its one action is asking it to scan.
 * Browser paths/URLs are never forwarded. */
#define LIBRARY_GAMES 4096
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool started, stopping, pending, busy;
static cJSON *games;
static time_t attempted, checked, updated;
static time_t refreshed;
static char status[24] = "idle", message[256], version[48];
static cJSON *capabilities, *last_request, *action_result;
/* A scan Orbit asked Kraken Mount for: the list is read again more often until
 * then, and the interfaces say it is looking. A drive connected since the last
 * look asks for one too. Times are monotonic seconds. */
#define SCAN_FOLLOW 30
#define DRIVE_CHECK_GAP 10
static time_t scan_until, drives_checked;
static char drives_seen[ORBIT_MAX_STORAGE][24];
static size_t drives_seen_count;
static bool drives_known;

static time_t monotonic_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}
static void identity(const char *a, const char *b, const char *c, char out[65]) {
    char data[12300];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    snprintf(data, sizeof data, "%s\n%s\n%s", a, b, c);
    SHA256((const unsigned char *)data, strlen(data), digest);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
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
static void location(const char *path, char *out, size_t cap) {
    const char *p = !strncmp(path, "/mnt/usb", 8) ? path + 8 :
                    !strncmp(path, "/mnt/ext", 8) ? path + 8 : NULL;
    if (!strncmp(path, "/mnt/ext1/", 10))
        copy_text(out, cap, "M.2 SSD");
    else if (p && isdigit((unsigned char)p[0]) && p[1] == '/')
        snprintf(out, cap, "%s %c", path[5] == 'u' ? "USB" : "Extended storage", *p);
    else if (!strncmp(path, "/data/", 6) || !strncmp(path, "/user/", 6))
        copy_text(out, cap, "Console storage");
    else copy_text(out, cap, "Other location");
}
/* Catalogue artwork is optional enrichment, independent of source selection.
 * A mounter's own icon URL is never forwarded to a browser. An empty title,
 * when given, takes the catalogue's name. */
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
/* Kraken Mount's games as Library rows. Kraken Mount registers games without
 * installing them and offers no move, copy or delete through Orbit. A duplicate (a
 * second copy of a title it already runs) isn't a game of its own: one row per title. */
static cJSON *normalize_kraken(const cJSON *list) {
    cJSON *out = cJSON_CreateArray(), *g;
    cJSON_ArrayForEach(g, list) {
        const char *id = json_text(g, "titleId"), *path = json_text(g, "path");
        if (!strcmp(json_text(g, "status"), "duplicate")) continue;
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
/* Games installed on the PS5 from PKG files, which Kraken Mount doesn't list: Orbit
 * finds each by its app.pkg in an app folder. */
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
static void add_installed_packages(cJSON *rows) {
    const char *prefix = app_prefix();
    if (!prefix) return;
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
            /* An installed PKG measured without app.db: the package's size. */
            cJSON_AddNumberToObject(item, "sizeBytes", (double)st.st_size);
            cJSON_AddStringToObject(item, "sizeStatus", "ready");
            location(path, where, sizeof where);
            cJSON_AddStringToObject(item, "location", where);
            cJSON_AddBoolToObject(item, "installed", true);
            cJSON_AddBoolToObject(item, "mounted", false);
            cJSON_AddBoolToObject(item, "onDrive", true);
            cJSON_AddNullToObject(item, "installedSizeBytes");
            add_artwork(item, id, title, sizeof title);
            /* No param file: the catalogue's name, else the title ID. */
            cJSON_ReplaceItemInObjectCaseSensitive(item, "title", cJSON_CreateString(*title ? title : id));
            cJSON_AddItemToArray(rows, item);
        }
        closedir(dir);
    }
}
/* A drive that wasn't connected before asks Kraken Mount to scan; any change re-reads
 * the games. The first look only learns the drives. */
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
    if (appeared) scan_until = monotonic_seconds() + SCAN_FOLLOW;
    if (changed_set) pending = true;
    pthread_mutex_unlock(&lock);
    if (appeared && kraken_active()) kraken_scan_soon();
}
/* Files put a game folder where Kraken Mount looks for games: ask it to scan now. */
void library_auto_scan(void) {
    if (kraken_active()) kraken_scan_soon();
    pthread_mutex_lock(&lock);
    scan_until = monotonic_seconds() + SCAN_FOLLOW;
    pending = true;
    pthread_cond_signal(&changed);
    pthread_mutex_unlock(&lock);
}
static void *run(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        while (!pending && !stopping && monotonic_seconds() - drives_checked < DRIVE_CHECK_GAP) {
            struct timespec deadline = {.tv_sec = time(NULL) + 1, .tv_nsec = 0};
            pthread_cond_timedwait(&changed, &lock, &deadline);
        }
        if (stopping) break;
        if (!pending) {
            drives_checked = monotonic_seconds();
            pthread_mutex_unlock(&lock);
            check_drives();
            pthread_mutex_lock(&lock);
            continue;
        }
        pending = false;
        busy = true;
        pthread_mutex_unlock(&lock);
        /* Kraken Mount's list as kraken.c last read it, then what is installed. */
        bool active = kraken_active();
        cJSON *list = active ? kraken_games() : NULL;
        cJSON *next = list ? normalize_kraken(list) : cJSON_CreateArray();
        cJSON_Delete(list);
        add_installed_packages(next);
        char next_version[48] = "";
        kraken_version(next_version, sizeof next_version);
        cJSON *next_capabilities = cJSON_CreateArray();
        if (active) cJSON_AddItemToArray(next_capabilities, cJSON_CreateString("rescan"));
        pthread_mutex_lock(&lock);
        checked = updated = time(NULL);
        refreshed = monotonic_seconds();
        cJSON_Delete(games);
        games = next;
        copy_text(version, sizeof version, next_version);
        cJSON_Delete(capabilities);
        capabilities = next_capabilities;
        copy_text(status, sizeof status, "ready");
        copy_text(message, sizeof message, active || !kraken_checked() ? ""
                  : kraken_waiting() ? "Kraken Mount is waiting for kstuff before it adds games. Games installed from PKG files are listed either way."
                  : "Kraken Mount isn't running. Start it to see the games it runs. Games installed from PKG files are listed either way.");
        busy = false;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
int library_start(void) {
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
    cJSON_Delete(capabilities); cJSON_Delete(last_request); cJSON_Delete(action_result);
    capabilities = last_request = action_result = NULL;
    pthread_mutex_unlock(&lock);
}

cJSON *library_snapshot(bool refresh) {
    pthread_mutex_lock(&lock);
    time_t now = monotonic_seconds();
    bool scanning = scan_until && now < scan_until;
    if (started && !stopping && !busy && !pending &&
        (!attempted || now - attempted >= (refresh || scanning ? 5 : 30))) {
        pending = true;
        attempted = now;
        pthread_cond_signal(&changed);
    }
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "status", started ? status : "unavailable");
    cJSON_AddStringToObject(out, "message", started ? message : "Library is unavailable. Restart Orbit to try again.");
    cJSON_AddStringToObject(out, "provider", "Kraken Mount");
    cJSON_AddStringToObject(out, "providerVersion", version);
    cJSON_AddBoolToObject(out, "busy", busy || pending);
    cJSON_AddBoolToObject(out, "stale", updated && (strcmp(status, "ready") || now - refreshed >= 30));
    cJSON_AddNumberToObject(out, "checkedAt", (double)checked);
    cJSON_AddNumberToObject(out, "updatedAt", (double)updated);
    cJSON_AddNumberToObject(out, "refreshAfter", attempted && now - attempted < 5 ? 5 - (now - attempted) : 0);
    cJSON_AddItemToObject(out, "games", games ? cJSON_Duplicate(games, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(out, "capabilities", capabilities ? cJSON_Duplicate(capabilities, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(out, "action", action_result ? cJSON_Duplicate(action_result, true) : cJSON_CreateNull());
    /* A scan Orbit asked for: the interfaces show it instead of the button. */
    cJSON_AddBoolToObject(out, "scanning", scanning);
    pthread_mutex_unlock(&lock);
    return out;
}

/* Orbit's drives and their free space, from Orbit's own storage list. */
cJSON *library_storage_snapshot(bool refresh) {
    (void)refresh;
    Storage drives[ORBIT_MAX_STORAGE];
    size_t n = storage_list(drives);
    cJSON *out = cJSON_CreateObject(), *list = cJSON_AddArrayToObject(out, "drives");
    for (size_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", drives[i].id);
        cJSON_AddStringToObject(item, "path", drives[i].root);
        cJSON_AddStringToObject(item, "label", drives[i].label);
        cJSON_AddNumberToObject(item, "totalBytes", (double)drives[i].total_bytes);
        cJSON_AddNumberToObject(item, "freeBytes", (double)drives[i].free_bytes);
        cJSON_AddNumberToObject(item, "usedBytes", drives[i].total_bytes > drives[i].free_bytes
                                ? (double)(drives[i].total_bytes - drives[i].free_bytes) : 0);
        cJSON_AddItemToArray(list, item);
    }
    cJSON_AddNumberToObject(out, "updatedAt", (double)time(NULL));
    return out;
}
/* Library's one action: ask Kraken Mount to scan its folders now. */
int library_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action"), *request = json_text(input, "requestId");
    if (strcmp(action, "scan") || !text_field(input, "requestId", 64) || strlen(request) < 8) {
        copy_text(error, cap, "Expected a supported action and its current Library identifiers."); return 400;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "confirmed"))) {
        copy_text(error, cap, "Confirm this action first."); return 400;
    }
    pthread_mutex_lock(&lock);
    int code = 202;
    if (!started || stopping) { code = 503; copy_text(error, cap, "Library is unavailable."); }
    else if (!strcmp(request, json_text(last_request, "requestId"))) {
        if (!cJSON_Compare(input, last_request, true)) { code = 409; copy_text(error, cap, "This request ID was already used for another action."); }
    } else if (!kraken_active()) {
        code = 409; copy_text(error, cap, "Kraken Mount isn't running. Start it, then scan again.");
    } else {
        kraken_scan_soon();
        scan_until = monotonic_seconds() + SCAN_FOLLOW;
        pending = true;
        cJSON_Delete(last_request); last_request = cJSON_Duplicate(input, true);
        cJSON_Delete(action_result); action_result = cJSON_CreateObject();
        cJSON_AddStringToObject(action_result, "id", request);
        cJSON_AddStringToObject(action_result, "action", action);
        cJSON_AddStringToObject(action_result, "titleId", "");
        cJSON_AddStringToObject(action_result, "state", "complete");
        cJSON_AddStringToObject(action_result, "message", "Scan requested. Games Kraken Mount finds appear here shortly.");
        pthread_cond_signal(&changed);
    }
    pthread_mutex_unlock(&lock);
    return code;
}
