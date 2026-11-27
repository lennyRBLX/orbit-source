/* Free up space: the list behind "Not enough space" and Library's Free up space.
 *
 * Orbit's own leftovers on a drive: packages the PS5 has already installed, archives
 * kept beside the game they unpacked, and the partial files of cancelled downloads.
 * Then the games ShadowMount can delete for Orbit. Nothing here deletes saves, and a
 * game installed from a package isn't offered: only the PS5 can remove that.
 *
 * Every item is checked again just before it is deleted, by the same code that
 * deletes it elsewhere (installed packages, history, Library), so a stale list can
 * never delete something else. */
#include "space.h"
#include "files.h"
#include "packages.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char id[112], title[256];
    int64_t size;
} Choice;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool running, joinable;
static atomic_bool stopping;
static struct {
    char state[12], storage[24], message[256], current[256];
    unsigned count, done, failed;
    int64_t freed;
    Choice items[SPACE_MAX_ITEMS];
} run = {.state = "idle"};

static bool drive(const char *id, Storage *out) {
    Storage drives[ORBIT_MAX_STORAGE];
    size_t count = storage_list(drives);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(drives[i].id, id)) { *out = drives[i]; return true; }
    return false;
}
static int bigger_first(const void *a, const void *b) {
    double x = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(*(cJSON *const *)a, "sizeBytes"));
    double y = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(*(cJSON *const *)b, "sizeBytes"));
    return x < y ? 1 : x > y ? -1 : 0;
}
static void sort(cJSON *array) {
    int n = cJSON_GetArraySize(array);
    if (n < 2) return;
    cJSON **items = calloc((size_t)n, sizeof *items);
    if (!items) return;
    for (int i = n - 1; i >= 0; i--) items[i] = cJSON_DetachItemFromArray(array, i);
    qsort(items, (size_t)n, sizeof *items, bigger_first);
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(array, items[i]);
    free(items);
}
static void leftover(cJSON *list, const char *id, const char *kind, const char *title, const char *name, int64_t size) {
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "id", id);
    cJSON_AddStringToObject(item, "kind", kind);
    cJSON_AddStringToObject(item, "title", title);
    cJSON_AddStringToObject(item, "name", name);
    cJSON_AddNumberToObject(item, "sizeBytes", (double)size);
    cJSON_AddItemToArray(list, item);
}
/* What a cancelled download's partial file takes on the drive; 0 when it is gone. */
static int64_t partial_size(const Job *job) {
    int dir = storage_open(job);
    if (dir < 0) return 0;
    char name[300];
    snprintf(name, sizeof name, "%s.part", job->filename);
    struct stat st;
    int64_t size = 0;
    if (!fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && st.st_dev == job->device) {
        /* A segmented partial is sparse: only its written blocks come back. */
        int64_t blocks = (int64_t)st.st_blocks * 512;
        size = blocks > 0 && blocks < (int64_t)st.st_size ? blocks : (int64_t)st.st_size;
    }
    close(dir);
    return size;
}
cJSON *space_list(const char *storage_id, int *code, char *error, size_t cap) {
    Storage d;
    if (!storage_id || !*storage_id || !drive(storage_id, &d)) {
        *code = 404; copy_text(error, cap, "Reconnect the selected drive first."); return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "storageId", d.id);
    cJSON_AddStringToObject(o, "label", d.label);
    cJSON_AddNumberToObject(o, "freeBytes", (double)d.free_bytes);
    cJSON_AddNumberToObject(o, "totalBytes", (double)d.total_bytes);
    /* Downloads keep the room they still need, as Download counts it. */
    static Job pins[ORBIT_MAX_JOBS];
    static pthread_mutex_t pins_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&pins_lock);
    size_t count = 0;
    pthread_mutex_lock(&orbit.mutex);
    uint64_t pending = storage_pending_locked(&d, NULL);
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (!j->id[0] || strcmp(j->storage_id, d.id) || j->history_removing) continue;
        if ((!strcmp(j->status, "complete") && j->archive_kept) || !strcmp(j->status, "cancelled")) {
            (void)storage_rebind_locked(j);
            pins[count++] = *j;
        }
    }
    pthread_mutex_unlock(&orbit.mutex);
    cJSON_AddNumberToObject(o, "projectedFreeBytes", (double)d.free_bytes - (double)pending);
    cJSON *leftovers = cJSON_AddArrayToObject(o, "leftovers"), *item;
    cJSON *packages = packages_space_leftovers(d.id);
    cJSON_ArrayForEach(item, packages) {
        char id[112];
        snprintf(id, sizeof id, "package:%s:%s", json_text(item, "receipt"), json_text(item, "package"));
        leftover(leftovers, id, "package", json_text(item, "title"), json_text(item, "name"),
                 (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "sizeBytes")));
    }
    cJSON_Delete(packages);
    for (size_t i = 0; i < count; i++) {
        const Job *j = &pins[i];
        char id[112];
        bool cancelled = !strcmp(j->status, "cancelled");
        int64_t size = cancelled ? partial_size(j) : files_kept_archive_size(j);
        if (size <= 0) continue;
        snprintf(id, sizeof id, "%s:%s", cancelled ? "partial" : "archive", j->id);
        leftover(leftovers, id, cancelled ? "partial" : "archive", j->release.title, j->filename, size);
    }
    pthread_mutex_unlock(&pins_lock);
    sort(leftovers);
    cJSON *games = library_space_games(d.root), *game;
    cJSON_ArrayForEach(game, games) {
        char id[112];
        snprintf(id, sizeof id, "game:%s:%s", json_text(game, "titleId"), json_text(game, "sourceKey"));
        cJSON_AddStringToObject(game, "id", id);
        cJSON_AddStringToObject(game, "kind", "game");
    }
    sort(games);
    cJSON_AddItemToObject(o, "games", games);
    cJSON_AddItemToObject(o, "deletion", space_status());
    *code = 200;
    return o;
}

cJSON *space_status(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&lock);
    cJSON_AddStringToObject(o, "state", run.state);
    cJSON_AddStringToObject(o, "storageId", run.storage);
    cJSON_AddStringToObject(o, "message", run.message);
    cJSON_AddStringToObject(o, "current", run.current);
    cJSON_AddNumberToObject(o, "total", run.count);
    cJSON_AddNumberToObject(o, "done", run.done);
    cJSON_AddNumberToObject(o, "failed", run.failed);
    cJSON_AddNumberToObject(o, "freedBytes", (double)run.freed);
    pthread_mutex_unlock(&lock);
    return o;
}
bool space_busy(void) {
    pthread_mutex_lock(&lock);
    bool busy = running;
    pthread_mutex_unlock(&lock);
    return busy;
}

/* A game goes through Library's own delete action, as if chosen there. */
static int delete_game(const char *id, char *error, size_t cap) {
    char title[16], key[80];
    const char *colon = strchr(id + 5, ':');
    if (!colon || (size_t)(colon - (id + 5)) >= sizeof title || strlen(colon + 1) >= sizeof key) {
        copy_text(error, cap, "This item changed. Check the list again."); return -1;
    }
    memcpy(title, id + 5, (size_t)(colon - (id + 5))); title[colon - (id + 5)] = 0;
    copy_text(key, sizeof key, colon + 1);
    char request[40] = "space-";
    random_hex(request + 6, 12);
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "action", "delete");
    cJSON_AddStringToObject(input, "requestId", request);
    cJSON_AddStringToObject(input, "titleId", title);
    cJSON_AddStringToObject(input, "sourceKey", key);
    cJSON_AddBoolToObject(input, "confirmed", true);
    int code = 409;
    /* Library does one thing at a time: wait a little for an automatic scan to end. */
    for (int tries = 0; tries < 60 && code == 409 && !atomic_load(&stopping); tries++) {
        code = library_action(input, error, cap);
        if (code == 409) { struct timespec delay = {.tv_nsec = 500000000L}; nanosleep(&delay, NULL); }
    }
    cJSON_Delete(input);
    if (code != 202) return -1;
    for (;;) {
        if (atomic_load(&stopping)) { copy_text(error, cap, "Orbit stopped before the game was deleted. Check Library."); return -1; }
        cJSON *result = library_action_result(request);
        const char *state = json_text(result, "state");
        int rc = !result ? -1 : !strcmp(state, "complete") ? 0 : !strcmp(state, "error") ? -1 : 1;
        if (rc < 0) copy_text(error, cap, result ? json_text(result, "message") : "Library started something else. Check Library.");
        cJSON_Delete(result);
        if (rc <= 0) return rc;
        struct timespec delay = {.tv_nsec = 300000000L};
        nanosleep(&delay, NULL);
    }
}
static int delete_one(const Choice *c, int64_t *freed, char *error, size_t cap) {
    if (!strncmp(c->id, "package:", 8)) {
        char receipt[17] = {0};
        const char *package = c->id + 8 + 16;
        if (strlen(c->id) != 8 + 16 + 1 + 12 || *package != ':') { copy_text(error, cap, "This item changed. Check the list again."); return -1; }
        memcpy(receipt, c->id + 8, 16);
        return packages_space_delete(receipt, package + 1, freed, error, cap);
    }
    if (!strncmp(c->id, "archive:", 8)) {
        pthread_mutex_lock(&orbit.mutex);
        Job *j = job_find(c->id + 8), pin;
        bool ok = j && !j->history_removing && !strcmp(j->status, "complete") && j->archive_kept;
        if (ok) { (void)storage_rebind_locked(j); pin = *j; }
        pthread_mutex_unlock(&orbit.mutex);
        if (!ok) { copy_text(error, cap, "This item changed. Check the list again."); return -1; }
        if (files_delete_kept_archive(&pin, freed, error, cap)) return -1;
        pthread_mutex_lock(&orbit.mutex);
        if ((j = job_find(pin.id))) { j->archive_kept = false; (void)state_save_locked(); }
        pthread_mutex_unlock(&orbit.mutex);
        return 0;
    }
    if (!strncmp(c->id, "partial:", 8)) {
        pthread_mutex_lock(&orbit.mutex);
        Job *j = job_find(c->id + 8);
        int code = !j || strcmp(j->status, "cancelled") ? 409 : job_action_locked(j, "delete", false, error, cap);
        pthread_mutex_unlock(&orbit.mutex);
        if (code >= 400) { if (!*error) copy_text(error, cap, "This item changed. Check the list again."); return -1; }
        *freed += c->size;
        return 0;
    }
    if (!strncmp(c->id, "game:", 5)) {
        if (delete_game(c->id, error, cap)) return -1;
        *freed += c->size;
        return 0;
    }
    copy_text(error, cap, "This item changed. Check the list again.");
    return -1;
}
static void *worker(void *unused) {
    (void)unused;
    for (unsigned i = 0;; i++) {
        pthread_mutex_lock(&lock);
        bool more = i < run.count && !atomic_load(&stopping);
        Choice c = more ? run.items[i] : (Choice){0};
        if (more) copy_text(run.current, sizeof run.current, c.title);
        pthread_mutex_unlock(&lock);
        if (!more) break;
        char error[256] = {0};
        int64_t freed = 0;
        int rc = delete_one(&c, &freed, error, sizeof error);
        pthread_mutex_lock(&lock);
        run.done++;
        run.freed += freed;
        if (rc) {
            run.failed++;
            if (!run.message[0]) copy_text(run.message, sizeof run.message, *error ? error : "An item couldn't be deleted. It was kept.");
        }
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    if (run.done < run.count && !run.message[0])
        copy_text(run.message, sizeof run.message, "Orbit stopped before deleting everything. Check the list again.");
    copy_text(run.state, sizeof run.state, run.failed || run.done < run.count ? "failed" : "done");
    run.current[0] = 0;
    running = false;
    pthread_mutex_unlock(&lock);
    return NULL;
}
static bool listed(const cJSON *list, const char *id, Choice *out) {
    const char *groups[] = {"leftovers", "games"};
    const cJSON *item;
    for (size_t g = 0; g < 2; g++)
        cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(list, groups[g]))
            if (!strcmp(json_text(item, "id"), id)) {
                copy_text(out->id, sizeof out->id, id);
                copy_text(out->title, sizeof out->title, json_text(item, "title"));
                out->size = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(item, "sizeBytes"));
                return true;
            }
    return false;
}
int space_delete(const cJSON *input, char *error, size_t cap) {
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(input, "ids"), *id;
    int n = cJSON_IsArray(ids) ? cJSON_GetArraySize(ids) : 0;
    if (n < 1 || n > SPACE_MAX_ITEMS) { copy_text(error, cap, "Choose what to delete."); return 400; }
    cJSON_ArrayForEach(id, ids)
        if (!cJSON_IsString(id) || !*id->valuestring || strlen(id->valuestring) >= sizeof run.items[0].id) {
            copy_text(error, cap, "Choose what to delete."); return 400;
        }
    int code;
    cJSON *list = space_list(json_text(input, "storageId"), &code, error, cap);
    if (!list) return code;
    Choice *chosen = calloc((size_t)n, sizeof *chosen);
    if (!chosen) { cJSON_Delete(list); copy_text(error, cap, "Orbit couldn't start deleting. Try again."); return 503; }
    int k = 0;
    cJSON_ArrayForEach(id, ids) {
        for (int i = 0; i < k; i++)
            if (!strcmp(chosen[i].id, id->valuestring)) { copy_text(error, cap, "Choose what to delete."); code = 400; goto out; }
        if (!listed(list, id->valuestring, &chosen[k])) {
            copy_text(error, cap, "Something on this drive changed. Check the list again."); code = 409; goto out;
        }
        k++;
    }
    pthread_mutex_lock(&lock);
    if (joinable && !running) { pthread_join(thread, NULL); joinable = false; }
    if (running || atomic_load(&stopping)) {
        pthread_mutex_unlock(&lock);
        copy_text(error, cap, "Orbit is still deleting. Wait for it to finish."); code = 409; goto out;
    }
    memset(&run, 0, sizeof run);
    copy_text(run.state, sizeof run.state, "deleting");
    copy_text(run.storage, sizeof run.storage, json_text(list, "storageId"));
    /* Leftovers first: they're quick, and may be all that's needed. */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < k; i++)
            if ((strncmp(chosen[i].id, "game:", 5) != 0) == !pass) run.items[run.count++] = chosen[i];
    running = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    int rc = pthread_create(&thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc) {
        running = false;
        copy_text(run.state, sizeof run.state, "idle");
        pthread_mutex_unlock(&lock);
        copy_text(error, cap, "Orbit couldn't start deleting. Try again."); code = 503; goto out;
    }
    joinable = true;
    pthread_mutex_unlock(&lock);
    code = 202;
out:
    free(chosen);
    cJSON_Delete(list);
    return code;
}
void space_stop(void) {
    atomic_store(&stopping, true);
    pthread_mutex_lock(&lock);
    bool wait = joinable;
    joinable = false;
    pthread_mutex_unlock(&lock);
    if (wait) pthread_join(thread, NULL);
}
