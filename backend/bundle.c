/* Get everything: a game and its add-ons, one ordered list added to Downloads one item at a time.
 * An item the console can fetch itself (Archive.org, TorBox, a direct link) is added at
 * once. One that needs its provider page opens it, as a normal Download does, and the
 * next item waits until that page adds its file. Each added download installs once it is
 * checked; with installInOrder it installs after the one added before it. The list lives
 * in memory only; the downloads it adds are saved like any other. */
#include "bundle.h"
#include "debrid.h"
#include "packages.h"
#include "remote_sources.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BUNDLE_ITEMS 64
typedef struct {
    char release_id[24], delivery[8], state[12], kind[8], job_id[24];
    char title[512], message[256];
} Item;
/* Protected by orbit.mutex. */
static struct {
    pthread_t thread;
    pthread_cond_t changed;
    bool started, stop, active, cancel, in_order, return_to_orbit, page_cancel_sent;
    char id[33], state[12], message[256], storage_id[24], session[33], last_job[24];
    /* The TV app that started it: it opens each provider page in its own window, which
     * closes once the file is added (the PS5 browser's own window can't be closed). */
    char client_id[65];
    Item items[BUNDLE_ITEMS];
    size_t count, next;
} b = {.changed = PTHREAD_COND_INITIALIZER};

static const char *kind_of(const Release *r) {
    return r->addon_id[0] ? "dlc" : "game";
}
/* Already in Downloads on this drive and not cancelled: Get everything reuses that download
 * (next_item_locked), and the room it needs is counted with the unfinished downloads. */
static bool queued_on_locked(const Release *r, const char *storage_id) {
    for (size_t i = 0; i < orbit.job_count; i++) {
        const Job *j = &orbit.jobs[i];
        if (j->id[0] && !j->history_removing && !strcmp(j->release_id, r->id) &&
            !strcmp(j->storage_id, storage_id) && strcmp(j->status, "cancelled")) return true;
    }
    return false;
}
static void item_done(Item *it, const char *state, const char *message) {
    copy_text(it->state, sizeof it->state, state);
    copy_text(it->message, sizeof it->message, message ? message : "");
}
/* An item that could not be added: in order, nothing after it is added either. */
static void item_failed(Item *it, const char *message) {
    item_done(it, "failed", message);
    b.next++;
    if (b.in_order) {
        for (size_t i = b.next; i < b.count; i++)
            item_done(&b.items[i], "skipped", "");
        b.next = b.count;
        copy_text(b.state, sizeof b.state, "stopped");
        copy_text(b.message, sizeof b.message, message ? message : "");
    }
}
static void item_added(Item *it, Job *j) {
    j->auto_install = true;
    copy_text(j->install_after, sizeof j->install_after, b.in_order ? b.last_job : "");
    (void)state_save_locked(); /* A failed save shows in /system stateHealthy. */
    packages_auto_install(j->id, j->install_after);
    pthread_cond_broadcast(&orbit.changed);
    copy_text(b.last_job, sizeof b.last_job, j->id);
    copy_text(it->job_id, sizeof it->job_id, j->id);
    item_done(it, "added", "");
    b.next++;
}
static void finish_locked(void) {
    for (size_t i = b.next; i < b.count; i++) item_done(&b.items[i], "skipped", "");
    b.next = b.count;
    if (b.cancel) copy_text(b.state, sizeof b.state, "stopped");
    else if (strcmp(b.state, "stopped")) copy_text(b.state, sizeof b.state, "done");
    b.active = false;
    b.session[0] = 0;
}
static void wait_locked(long ms) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += ms * 1000000L;
    deadline.tv_sec += deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(&b.changed, &orbit.mutex, &deadline);
}
/* The provider page this list opened: added, still open, or closed without a file. */
static void follow_page_locked(void) {
    Item *it = &b.items[b.next];
    pthread_mutex_unlock(&orbit.mutex);
    cJSON *status = browser_status();
    pthread_mutex_lock(&orbit.mutex);
    if (!b.active || !b.session[0]) { cJSON_Delete(status); return; }
    const char *state = json_text(status, "state"), *job_id = json_text(status, "jobId");
    bool same = !strcmp(json_text(status, "id"), b.session);
    bool busy = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "active"));
    Job *job = same && !strcmp(state, "queued") && *job_id ? job_find(job_id) : NULL;
    if (job) {
        b.session[0] = 0;
        copy_text(b.state, sizeof b.state, "running");
        item_added(it, job);
    } else if (!same || (!busy && strcmp(state, "queued"))) {
        b.session[0] = 0;
        copy_text(b.state, sizeof b.state, "running");
        const char *why = same ? json_text(status, "message") : "";
        item_failed(it, b.in_order ? "The page was closed, so the rest wasn't added." : why);
        if (b.in_order && *why) copy_text(it->message, sizeof it->message, why);
    } else if (b.cancel && !b.page_cancel_sent) {
        cJSON *input = cJSON_CreateObject();
        cJSON_AddStringToObject(input, "action", "cancel");
        cJSON_AddStringToObject(input, "id", b.session);
        b.page_cancel_sent = true;
        char error[256];
        pthread_mutex_unlock(&orbit.mutex);
        (void)browser_action(input, error, sizeof error);
        pthread_mutex_lock(&orbit.mutex);
        cJSON_Delete(input);
    }
    cJSON_Delete(status);
}
static void next_item_locked(void) {
    Item *it = &b.items[b.next];
    char error[256] = "";
    Release *r = release_find(it->release_id);
    if (!r) {
        item_failed(it, "This item isn't in the PS5's catalogue yet. Refresh the catalogue on the PS5, then try again.");
        return;
    }
    copy_text(it->state, sizeof it->state, "adding");
    /* Already in Downloads on this drive (Get everything again after a failure): one
     * that failed or was paused is retried, and a cancelled one that kept its partial
     * file resumed, as Downloads would; one downloading or downloaded counts as added.
     * Any other cancelled one makes way for a new download. */
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *old = &orbit.jobs[i];
        bool cancelled = !strcmp(old->status, "cancelled");
        if (!old->id[0] || strcmp(old->release_id, r->id) || strcmp(old->storage_id, b.storage_id) ||
            old->history_removing || (cancelled && !job_resumable_cancel_locked(old))) continue;
        int code = 200;
        if (cancelled) code = job_action_locked(old, "resume", false, error, sizeof error);
        else if (!strcmp(old->status, "error") || !strcmp(old->status, "paused"))
            code = job_action_locked(old, "retry", false, error, sizeof error);
        if (code == 200) item_added(it, old);
        else item_failed(it, error);
        return;
    }
    bool torbox = (orbit.always_use_torbox && debrid_covers_locked(r)) || !strcmp(it->delivery, "torbox") ||
                  (!it->delivery[0] && debrid_default_locked(r));
    if (torbox || provider_supported(r) || (!remote_page_pending(r) && !provider_browser_supported(r))) {
        Job *job = NULL;
        int code = job_create_delivery_locked(r->id, b.storage_id, it->delivery[0] ? it->delivery : NULL,
                                              error, sizeof error, &job);
        if (code == 201 && job) item_added(it, job);
        else item_failed(it, error);
        return;
    }
    /* Its provider page, opened as a normal Download opens it. */
    cJSON *input = cJSON_CreateObject();
    cJSON_AddStringToObject(input, "action", "start");
    cJSON_AddStringToObject(input, "releaseId", r->id);
    cJSON_AddStringToObject(input, "storageId", b.storage_id);
    if (b.client_id[0]) {
        cJSON_AddStringToObject(input, "returnTarget", "native");
        cJSON_AddTrueToObject(input, "nativeDialog");
        cJSON_AddStringToObject(input, "clientId", b.client_id);
    } else if (b.return_to_orbit) cJSON_AddTrueToObject(input, "returnToOrbit");
    copy_text(it->state, sizeof it->state, "page");
    copy_text(b.state, sizeof b.state, "page");
    pthread_mutex_unlock(&orbit.mutex);
    int code = browser_action(input, error, sizeof error);
    cJSON *status = code == 202 ? browser_status() : NULL;
    pthread_mutex_lock(&orbit.mutex);
    cJSON_Delete(input);
    if (code == 202 && b.active) {
        copy_text(b.session, sizeof b.session, json_text(status, "id"));
        b.page_cancel_sent = false;
    } else {
        copy_text(b.state, sizeof b.state, "running");
        item_failed(it, error);
    }
    cJSON_Delete(status);
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!b.stop && !orbit.stop) {
        if (!b.active) { pthread_cond_wait(&b.changed, &orbit.mutex); continue; }
        if (b.session[0]) { follow_page_locked(); if (b.session[0]) wait_locked(500); continue; }
        if (b.cancel || b.next >= b.count) { finish_locked(); continue; }
        next_item_locked();
    }
    pthread_mutex_unlock(&orbit.mutex);
    return NULL;
}
int bundle_start(void) {
    pthread_mutex_lock(&orbit.mutex);
    if (b.started) { pthread_mutex_unlock(&orbit.mutex); return 0; }
    b.stop = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    int rc = pthread_create(&b.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    b.started = !rc;
    pthread_mutex_unlock(&orbit.mutex);
    return rc ? -1 : 0;
}
void bundle_stop(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool started = b.started;
    b.stop = true;
    pthread_cond_broadcast(&b.changed);
    pthread_mutex_unlock(&orbit.mutex);
    if (started) pthread_join(b.thread, NULL);
    pthread_mutex_lock(&orbit.mutex);
    b.started = false;
    pthread_mutex_unlock(&orbit.mutex);
}
static int start_locked(const cJSON *input, char *error, size_t cap) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(input, "items"), *x;
    const cJSON *order = cJSON_GetObjectItemCaseSensitive(input, "installInOrder");
    const cJSON *back = cJSON_GetObjectItemCaseSensitive(input, "returnToOrbit");
    const cJSON *client = cJSON_GetObjectItemCaseSensitive(input, "clientId");
    if (b.stop || orbit.stop) { copy_text(error, cap, "Orbit is stopping. Reconnect after restarting it."); return 503; }
    if (!b.started) { copy_text(error, cap, "Download all isn't available right now. Restart Orbit, then try again."); return 503; }
    if (b.active) { copy_text(error, cap, "Wait for the current Download all to finish."); return 409; }
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) < 1 || cJSON_GetArraySize(items) > BUNDLE_ITEMS ||
        (order && !cJSON_IsBool(order)) || (back && !cJSON_IsBool(back))) {
        copy_text(error, cap, "Choose between 1 and 64 items."); return 400;
    }
    if (client && (!cJSON_IsString(client) || cJSON_IsTrue(back) || !*client->valuestring ||
                   strlen(client->valuestring) > 64 ||
                   strspn(client->valuestring, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                       strlen(client->valuestring))) {
        copy_text(error, cap, "Choose a supported native browser session."); return 400;
    }
    Storage drives[ORBIT_MAX_STORAGE], *drive = NULL;
    size_t count = storage_list(drives);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(drives[i].id, json_text(input, "storageId"))) drive = &drives[i];
    Item list[BUNDLE_ITEMS];
    size_t n = 0;
    uint64_t total = 0;
    memset(list, 0, sizeof list);
    cJSON_ArrayForEach(x, items) {
        const char *id = json_text(x, "releaseId"), *delivery = json_text(x, "delivery");
        const cJSON *field = cJSON_GetObjectItemCaseSensitive(x, "delivery");
        if (!cJSON_IsObject(x) || (field && !cJSON_IsString(field)) ||
            (*delivery && strcmp(delivery, "direct") && strcmp(delivery, "torbox"))) {
            copy_text(error, cap, "Choose direct or torbox delivery."); return 400;
        }
        for (size_t i = 0; i < n; i++)
            if (!strcmp(list[i].release_id, id)) { copy_text(error, cap, "Choose each item once."); return 400; }
        const Release *r = release_find(id);
        if (!r || !strcmp(r->id, "orbit-network-check")) {
            copy_text(error, cap, "This item isn't in the PS5's catalogue yet. Refresh the catalogue on the PS5, then try again.");
            return 404;
        }
        if (!source_enabled_locked(r)) {
            copy_text(error, cap, "Enable this source and acknowledge the download notice in Sources first.");
            return 409;
        }
        Item *it = &list[n++];
        copy_text(it->release_id, sizeof it->release_id, r->id);
        copy_text(it->delivery, sizeof it->delivery, delivery);
        copy_text(it->kind, sizeof it->kind, kind_of(r));
        copy_text(it->state, sizeof it->state, "pending");
        release_download_title(r, it->title, sizeof it->title);
        /* Only the download counts: unpacking checks its own room later. */
        uint64_t need = drive && queued_on_locked(r, drive->id) ? 0 : job_space_needed(r);
        total = UINT64_MAX - total < need ? UINT64_MAX : total + need;
    }
    if (!drive) { copy_text(error, cap, "Selected storage is not connected or writable."); return 409; }
    if (!job_space_fits_locked(drive, total)) {
        copy_text(error, cap, "Not enough space for everything on this drive."); return 409;
    }
    memcpy(b.items, list, sizeof list);
    b.count = n;
    b.next = 0;
    b.in_order = cJSON_IsTrue(order);
    b.return_to_orbit = cJSON_IsTrue(back);
    copy_text(b.client_id, sizeof b.client_id, client ? client->valuestring : "");
    b.cancel = b.page_cancel_sent = false;
    b.session[0] = b.last_job[0] = b.message[0] = 0;
    copy_text(b.storage_id, sizeof b.storage_id, drive->id);
    copy_text(b.state, sizeof b.state, "running");
    random_hex(b.id, 16);
    b.active = true;
    pthread_cond_broadcast(&b.changed);
    return 202;
}
int bundle_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action");
    pthread_mutex_lock(&orbit.mutex);
    int code;
    if (!strcmp(action, "start")) code = start_locked(input, error, cap);
    else if (!strcmp(action, "cancel")) {
        if (!b.active || strcmp(json_text(input, "id"), b.id)) {
            copy_text(error, cap, "This Download all is no longer running."); code = 409;
        } else {
            b.cancel = true;
            pthread_cond_broadcast(&b.changed);
            code = 202;
        }
    } else { copy_text(error, cap, "Choose start or cancel."); code = 400; }
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}
cJSON *bundle_status(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&orbit.mutex);
    cJSON_AddStringToObject(o, "id", b.id);
    cJSON_AddStringToObject(o, "state", b.state[0] ? b.state : "idle");
    cJSON_AddStringToObject(o, "message", b.message);
    cJSON_AddBoolToObject(o, "installInOrder", b.in_order);
    cJSON *list = cJSON_AddArrayToObject(o, "items");
    for (size_t i = 0; i < b.count; i++) {
        const Item *it = &b.items[i];
        cJSON *x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "releaseId", it->release_id);
        cJSON_AddStringToObject(x, "title", it->title);
        cJSON_AddStringToObject(x, "kind", it->kind);
        cJSON_AddStringToObject(x, "state", it->state);
        cJSON_AddStringToObject(x, "jobId", it->job_id);
        cJSON_AddStringToObject(x, "message", it->message);
        cJSON_AddItemToArray(list, x);
    }
    pthread_mutex_unlock(&orbit.mutex);
    return o;
}
