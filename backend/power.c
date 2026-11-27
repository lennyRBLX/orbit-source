/* One completion schedule for the console, shared by all paired interfaces.
 * Merely enabling the preference on an idle console never shuts it down. */
#include "power.h"
#include "packages.h"
#include "files.h"
#include "zero_link.h"
#include "links.h"
#include "space.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef ORBIT_DESKTOP
#include <dlfcn.h>
#endif
static atomic_uint_fast64_t completed;
static atomic_uint_fast64_t install_failures;
static uint64_t baseline;
static uint64_t failure_baseline;
static bool armed, requested;
static time_t deadline;
static unsigned requests;
static uint64_t request_epoch;
static char failure[192];
static struct { char owner[65]; bool busy; unsigned completed; } clients[16];
static time_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }
static unsigned grace(void) {
#ifdef ORBIT_TEST
    const char *s = getenv("ORBIT_TEST_POWER_GRACE");
    if (s && !strcmp(s, "1")) return 1;
#endif
    return 60;
}
#ifndef ORBIT_DESKTOP
static pthread_once_t platform_once = PTHREAD_ONCE_INIT;
static int (*request_shutdown)(int);
static void load_platform(void) {
    void *module = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_LOCAL);
    if (module) request_shutdown = (int (*)(int))dlsym(module, "sceShellCoreUtilRequestShutdown");
}
#endif
bool power_available(void) {
#ifdef ORBIT_TEST
    return true;
#elif defined(ORBIT_DESKTOP)
    return false;
#else
    pthread_once(&platform_once, load_platform);
    return request_shutdown != NULL;
#endif
}
void power_download_completed(void) { atomic_fetch_add(&completed, 1); }
void power_installation_failed(void) { atomic_fetch_add(&install_failures, 1); }
bool power_request_begin(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool ok = !orbit.stop && !requested;
    if (ok) { requests++; request_epoch++; }
    pthread_mutex_unlock(&orbit.mutex);
    return ok;
}
void power_request_end(void) {
    pthread_mutex_lock(&orbit.mutex);
    if (requests) requests--;
    pthread_mutex_unlock(&orbit.mutex);
}
static bool unfinished(void) {
    for (size_t i = 0; i < orbit.job_count; i++) {
        const Job *j = &orbit.jobs[i];
        if (j->history_removing) return true;
        if (j->id[0] && strcmp(j->status, "complete") && strcmp(j->status, "cancelled")) return true;
    }
    for (size_t i = 0; i < 16; i++) if (clients[i].busy || orbit.shutdown_holds[i][0]) return true;
    return orbit.receiver_active || orbit.files_storage_busy || orbit.library_storage_busy;
}
cJSON *power_status(void) {
    bool available = power_available();
    pthread_mutex_lock(&orbit.mutex);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", available);
    cJSON_AddBoolToObject(o, "enabled", orbit.shutdown_when_done);
    cJSON_AddBoolToObject(o, "armed", armed);
    cJSON_AddStringToObject(o, "jobId", orbit.shutdown_job);
    cJSON_AddStringToObject(o, "error", failure);
    time_t left = deadline > now() ? deadline - now() : 0;
    cJSON_AddNumberToObject(o, "seconds", (double)left);
    cJSON_AddStringToObject(o, "state", requested ? "stopping" : !orbit.shutdown_when_done ? "off" : deadline ? "countdown" : "waiting");
    pthread_mutex_unlock(&orbit.mutex);
    return o;
}
int power_action(const cJSON *input, char *error, size_t cap) {
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled");
    const char *id = json_text(input, "jobId");
    if (!cJSON_IsBool(enabled) || strlen(id) >= sizeof orbit.shutdown_job) {
        copy_text(error, cap, "Choose when to shut down the PS5."); return 400;
    }
    if (cJSON_IsTrue(enabled) && !power_available()) {
        copy_text(error, cap, "Automatic shutdown is unavailable on this PS5."); return 409;
    }
    pthread_mutex_lock(&orbit.mutex);
    if (orbit.stop || requested) { pthread_mutex_unlock(&orbit.mutex); copy_text(error, cap, "Orbit is stopping."); return 409; }
    Job *job = *id ? job_find(id) : NULL;
    if (*id && (!job || !strcmp(job->status, "cancelled"))) {
        pthread_mutex_unlock(&orbit.mutex); copy_text(error, cap, "Choose a download that is still in Downloads."); return 409;
    }
    bool previous = orbit.shutdown_when_done;
    char previous_job[24]; copy_text(previous_job, sizeof previous_job, orbit.shutdown_job);
    orbit.shutdown_when_done = cJSON_IsTrue(enabled);
    copy_text(orbit.shutdown_job, sizeof orbit.shutdown_job, id);
    if (state_save_locked()) {
        orbit.shutdown_when_done = previous;
        copy_text(orbit.shutdown_job, sizeof orbit.shutdown_job, previous_job);
        pthread_mutex_unlock(&orbit.mutex); copy_text(error, cap, "Could not save the setting."); return 503;
    }
    baseline = atomic_load(&completed);
    failure_baseline = atomic_load(&install_failures);
    armed = job && !strcmp(job->status, "complete");
    deadline = 0; failure[0] = 0;
    pthread_mutex_unlock(&orbit.mutex);
    return 200;
}
int power_client_work(const cJSON *input, const char *owner, char *error, size_t cap) {
    const cJSON *busy = cJSON_GetObjectItemCaseSensitive(input, "busy"), *count = cJSON_GetObjectItemCaseSensitive(input, "completed");
    if (!owner || !*owner || strlen(owner) > 64 || !cJSON_IsBool(busy) || !cJSON_IsNumber(count) ||
        count->valuedouble < 0 || count->valuedouble > 1000000 || floor(count->valuedouble) != count->valuedouble) {
        copy_text(error, cap, "Invalid computer download status."); return 400;
    }
    pthread_mutex_lock(&orbit.mutex);
    size_t i = 0;
    for (; i < 16 && strcmp(clients[i].owner, owner); i++);
    bool existing = i < 16;
    if (!existing) for (i = 0; i < 16 && clients[i].owner[0] && clients[i].busy; i++);
    if (i == 16) { pthread_mutex_unlock(&orbit.mutex); copy_text(error, cap, "Too many computers are reporting active downloads."); return 409; }
    size_t hold = 0;
    for (; hold < 16 && strcmp(orbit.shutdown_holds[hold], owner); ++hold);
    const bool held = hold < 16;
    if (cJSON_IsTrue(busy) != held) {
        if (!held) for (hold = 0; hold < 16 && orbit.shutdown_holds[hold][0]; ++hold);
        if (hold == 16) { pthread_mutex_unlock(&orbit.mutex); copy_text(error, cap, "Too many computers are reporting active downloads."); return 409; }
        copy_text(orbit.shutdown_holds[hold], 65, cJSON_IsTrue(busy) ? owner : "");
        if (state_save_locked()) {
            copy_text(orbit.shutdown_holds[hold], 65, held ? owner : "");
            pthread_mutex_unlock(&orbit.mutex); copy_text(error, cap, "Could not save the setting."); return 503;
        }
    }
    if (existing && (unsigned)count->valueint > clients[i].completed) power_download_completed();
    copy_text(clients[i].owner, sizeof clients[i].owner, owner);
    clients[i].completed = (unsigned)count->valueint;
    clients[i].busy = cJSON_IsTrue(busy);
    if (clients[i].busy) deadline = 0;
    /* Losing a busy computer must block shutdown, never silently expire its
     * lease. It reports completion on reconnect; disabling the setting cancels. */
    pthread_mutex_unlock(&orbit.mutex);
    return 200;
}
static bool background_busy(void) {
    bool busy = zero_link_busy() || links_busy() || space_busy();
    cJSON *status = packages_status(), *r, *p;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(status, "downloads")) {
        const char *s = json_text(r, "state");
        const char *mount = json_text(r, "shadowMount");
        if (!strcmp(s, "checking") || !strcmp(mount, "pending") || !strcmp(mount, "requested")) busy = true;
        cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(r, "packages")) {
            s = json_text(p, "state");
            if (!strcmp(s, "queued") || !strcmp(s, "installing") || !strcmp(s, "unconfirmed")) busy = true;
        }
    }
    cJSON_Delete(status);
    status = files_status();
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(status, "operation");
    const char *s = json_text(op, "state");
    if (*s && strcmp(s, "complete") && strcmp(s, "cancelled")) busy = true;
    cJSON_Delete(status);
    status = updates_status();
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "busy"))) busy = true;
    cJSON_Delete(status);
    status = browser_status();
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "active"))) busy = true;
    cJSON_Delete(status);
    return busy;
}
void power_tick(void) {
    if (!power_available() || orbit_is_stopping()) return;
    pthread_mutex_lock(&orbit.mutex);
    uint64_t before = request_epoch;
    pthread_mutex_unlock(&orbit.mutex);
    bool busy = background_busy(), announce = false;
    pthread_mutex_lock(&orbit.mutex);
    if (!orbit.shutdown_when_done || orbit.stop || requested) { pthread_mutex_unlock(&orbit.mutex); return; }
    if (requests || before != request_epoch) { pthread_mutex_unlock(&orbit.mutex); return; }
    if (atomic_load(&install_failures) != failure_baseline) {
        orbit.shutdown_when_done = false; deadline = 0; armed = false;
        copy_text(failure, sizeof failure, "Automatic shutdown was cancelled because installation failed.");
        (void)state_save_locked();
        pthread_mutex_unlock(&orbit.mutex); return;
    }
    if (atomic_load(&completed) != baseline) armed = true;
    if (*orbit.shutdown_job) {
        const Job *job = job_find(orbit.shutdown_job);
        if (!job || !strcmp(job->status, "cancelled")) {
            copy_text(failure, sizeof failure, "The selected download was removed or cancelled. Choose it again to schedule shutdown.");
            armed = false; deadline = 0; pthread_mutex_unlock(&orbit.mutex); return;
        }
        if (strcmp(job->status, "complete")) busy = true;
    }
    if (!armed || unfinished() || busy) deadline = 0;
    else if (!deadline) { deadline = now() + grace(); announce = true; }
    else if (now() >= deadline) {
        requested = true;
        orbit.stop = true; /* Refuse new writes while the normal stop path drains. */
        orbit_request_stop();
        pthread_cond_broadcast(&orbit.changed);
    }
    pthread_mutex_unlock(&orbit.mutex);
    if (announce) notify_shutdown(false);
}
void power_finish(void) {
    if (!requested) return;
    /* Workers and the receiver have already stopped and fsynced. */
#ifdef ORBIT_TEST
    const char *path = getenv("ORBIT_TEST_POWER_LOG");
    FILE *f = path ? fopen(path, "a") : NULL;
    if (f) { fputs("shutdown\n", f); fclose(f); }
#elif !defined(ORBIT_DESKTOP)
    if (!request_shutdown || request_shutdown(2) < 0)
        notify_shutdown(true);
#endif
}
