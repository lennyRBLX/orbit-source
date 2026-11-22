#include "history.h"
#include "files.h"
#include <stdlib.h>
#include <string.h>
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t thread;
static bool running, joinable;
static atomic_bool stopping;
typedef struct { size_t count; char ids[ORBIT_MAX_JOBS][24]; } Removal;
bool history_stopping(void) { return atomic_load(&stopping); }
static bool choice(const char *s) { return !strcmp(s, "ask") || !strcmp(s, "keep") || !strcmp(s, "delete"); }
cJSON *history_status(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&orbit.mutex);
    cJSON_AddStringToObject(o, "preference", *orbit.history_files ? orbit.history_files : "ask");
    pthread_mutex_unlock(&orbit.mutex); return o;
}
static int ids(const cJSON *input, Removal *out, char *error, size_t cap) {
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(input, "ids"), *id;
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 1 || cJSON_GetArraySize(a) > ORBIT_MAX_JOBS) goto invalid;
    cJSON_ArrayForEach(id, a) {
        if (!cJSON_IsString(id) || !*id->valuestring || strlen(id->valuestring) >= 24) goto invalid;
        for (size_t i = 0; i < out->count; i++) if (!strcmp(out->ids[i], id->valuestring)) goto invalid;
        copy_text(out->ids[out->count++], 24, id->valuestring);
    }
    return 0;
invalid: copy_text(error, cap, "Choose downloads to remove from history."); return 400;
}
static bool snapshot(const char *id, Job *pin) {
    pthread_mutex_lock(&orbit.mutex);
    Job *j = job_find(id);
    bool ok = j && !j->history_removing && (!strcmp(j->status, "complete") || !strcmp(j->status, "cancelled"));
    if (ok) { (void)storage_rebind_locked(j); *pin = *j; }
    pthread_mutex_unlock(&orbit.mutex); return ok;
}
cJSON *history_preview(const cJSON *input, int *code, char *error, size_t cap) {
    Removal plan = {0}; *code = ids(input, &plan, error, cap);
    if (*code) return NULL;
    cJSON *o = history_status(), *list = cJSON_AddArrayToObject(o, "downloads");
    for (size_t i = 0; i < plan.count; ++i) {
        Job pin;
        if (!snapshot(plan.ids[i], &pin)) { cJSON_Delete(o); *code = 409; copy_text(error, cap, "Wait for the download to finish or cancel it before removing its files."); return NULL; }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", pin.id);
        cJSON_AddStringToObject(item, "title", pin.release.title);
        char e[256] = {0}; int status = 200;
        cJSON *files = files_download_plan(&pin, &status, e, sizeof e);
        cJSON_AddBoolToObject(item, "deleteAllowed", files != NULL);
        cJSON_AddStringToObject(item, "error", e);
        cJSON_AddItemToObject(item, "files", files ? files : cJSON_CreateArray());
        cJSON_AddItemToArray(list, item);
    }
    *code = 200; return o;
}
static void *remove_files(void *argument) {
    Removal *plan = argument;
    for (size_t i = 0; i < plan->count; i++) {
        Job pin; bool found;
        pthread_mutex_lock(&orbit.mutex);
        Job *j = job_find(plan->ids[i]); found = j != NULL;
        if (found) pin = *j;
        pthread_mutex_unlock(&orbit.mutex);
        if (!found) continue;
        char error[256] = {0}; int rc = 0;
        if (history_stopping()) { rc = -1; copy_text(error, sizeof error, "File removal was interrupted. Check the files and try again."); }
        else if (!strcmp(pin.status, "complete")) rc = files_delete_download(&pin, error, sizeof error);
        pthread_mutex_lock(&orbit.mutex);
        j = job_find(pin.id);
        if (j) {
            j->history_removing = false;
            if (!rc) rc = job_action_locked(j, !strcmp(j->status, "cancelled") ? "delete" : "remove", false, error, sizeof error) >= 400;
            if (rc && (j = job_find(pin.id))) {
                copy_text(j->removal_error, sizeof j->removal_error, *error ? error : "Downloaded files could not be removed. The history entry was kept.");
                (void)state_save_locked();
            }
        }
        pthread_mutex_unlock(&orbit.mutex);
    }
    free(plan);
    pthread_mutex_lock(&lock); running = false; pthread_mutex_unlock(&lock);
    return NULL;
}
int history_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action"), *selected = json_text(input, "choice");
    bool settings = !strcmp(action, "settings");
    if ((!settings && strcmp(action, "remove")) || !choice(selected) || (!settings && !strcmp(selected, "ask"))) {
        copy_text(error, cap, "Choose whether to keep or delete downloaded files."); return 400;
    }
    Removal *plan = settings ? NULL : calloc(1, sizeof *plan);
    if (!settings && (!plan || ids(input, plan, error, cap))) { free(plan); return 400; }
    pthread_mutex_lock(&lock);
    if (joinable && !running) { pthread_join(thread, NULL); joinable = false; }
    if (running || history_stopping()) { pthread_mutex_unlock(&lock); free(plan); copy_text(error, cap, "Wait for file removal to finish."); return 409; }
    pthread_mutex_lock(&orbit.mutex);
    for (size_t i = 0; plan && i < plan->count; i++) {
        Job *j = job_find(plan->ids[i]);
        if (!j || j->history_removing || (strcmp(j->status, "complete") && strcmp(j->status, "cancelled"))) {
            pthread_mutex_unlock(&orbit.mutex); pthread_mutex_unlock(&lock); free(plan);
            copy_text(error, cap, "Wait for the download to finish or cancel it before removing its files."); return 409;
        }
    }
    char previous[8]; copy_text(previous, sizeof previous, orbit.history_files);
    if (settings || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "remember"))) copy_text(orbit.history_files, sizeof orbit.history_files, selected);
    if (state_save_locked()) {
        copy_text(orbit.history_files, sizeof orbit.history_files, previous);
        pthread_mutex_unlock(&orbit.mutex); pthread_mutex_unlock(&lock); free(plan);
        copy_text(error, cap, "Could not save the setting."); return 503;
    }
    int code = 200;
    if (plan && !strcmp(selected, "keep")) {
        for (size_t i = 0; i < plan->count && code < 400; i++) {
            Job *j = job_find(plan->ids[i]);
            code = job_action_locked(j, !strcmp(j->status, "cancelled") ? "forget" : "remove", false, error, cap);
        }
        free(plan);
    } else if (plan) {
        for (size_t i = 0; i < plan->count; i++) { Job *j = job_find(plan->ids[i]); j->history_removing = true; j->removal_error[0] = 0; }
        running = true;
        pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
        int rc = pthread_create(&thread, &attr, remove_files, plan); pthread_attr_destroy(&attr);
        if (rc) {
            running = false;
            for (size_t i = 0; i < plan->count; i++) job_find(plan->ids[i])->history_removing = false;
            free(plan); copy_text(error, cap, "Orbit couldn't start file removal. Try again."); code = 503;
        } else { joinable = true; code = 202; }
    }
    pthread_mutex_unlock(&orbit.mutex); pthread_mutex_unlock(&lock); return code;
}
void history_stop(void) {
    atomic_store(&stopping, true);
    pthread_mutex_lock(&lock); bool wait = joinable; pthread_t previous = thread; joinable = false; pthread_mutex_unlock(&lock);
    if (wait) pthread_join(previous, NULL);
}
