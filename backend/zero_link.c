/* A download link Orbit Zero caught in its own provider window, for this console to
 * download itself (POST /api/v1/zero/link).
 *
 * Zero opens Vikingfile, AkiraBox, DataNodes or 1fichier on the computer, where
 * Cloudflare's check passes quickly, and catches the file link the page produces.
 * The link then gets exactly the checks of a link caught in the console's own
 * browser (browser.c): the option's address rules, one bounded request for the
 * file's identity, and a job pinned to the chosen drive. It never carries the
 * console's browser cookies; Zero sends signed links, which need none.
 *
 * The check runs on its own thread: the API answers every request on one thread,
 * so it must never wait for a provider. One check at a time; Zero reads
 * GET /api/v1/zero/link until the state is "queued" or "failed". */
#include "zero_link.h"
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool started, stopping, pending;
static atomic_bool cancel;
static struct {
    char id[33], state[12], message[256], job_id[sizeof ((Job *)0)->id];
    Release option; /* The catalogue option as it was when the check began. */
    Storage drive;
    char url[sizeof ((Release *)0)->url];
} check = {.state = "idle"};

/* Whether the catalogue still offers the same file from the same place. */
static bool same_option(const Release *a, const Release *b) {
    return a->size == b->size && !strcmp(a->source, b->source) && !strcmp(a->game_id, b->game_id) &&
           !strcmp(a->title_id, b->title_id) && !strcmp(a->filename, b->filename) &&
           !strcmp(a->format, b->format) && !strcmp(a->sha256, b->sha256) &&
           !strcmp(a->url, b->url) && !strcmp(a->browser_url, b->browser_url);
}
/* The drive with this id, if it is still the same drive. */
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
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    while (!stopping) {
        while (!pending && !stopping)
            pthread_cond_wait(&wake, &lock);
        if (stopping)
            break;
        pending = false;
        Release option = check.option, candidate = check.option;
        Storage pin = check.drive;
        copy_text(candidate.url, sizeof candidate.url, check.url);
        pthread_mutex_unlock(&lock);
        /* A signed link: never the console browser's cookies or sign-in. */
        candidate.viking_context = 0;
        candidate.viking_user = 0;
        candidate.viking_authenticated = false;
        char error[256] = {0}, etag[256] = {0}, modified[128] = {0}, job_id[sizeof check.job_id] = {0};
        int invalid = transfer_validate_capture(&candidate, &cancel, etag, modified, error, sizeof error);
        bool queued = false;
        pthread_mutex_lock(&orbit.mutex);
        const Release *now = release_find(option.id);
        Storage drive;
        if (orbit.stop || atomic_load(&cancel)) {
            copy_text(error, sizeof error, "Orbit is stopping. Nothing was added to Downloads.");
        } else if (!now || !same_option(now, &option) || !source_enabled_locked(now)) {
            /* Checked before the result: the check itself stops when the source is turned off. */
            copy_text(error, sizeof error, "The source or catalogue option changed. Open the download page again.");
        } else if (invalid) {
            if (!error[0]) copy_text(error, sizeof error, "The provider did not return the expected file. Nothing was added to Downloads.");
        } else if (!drive_locked(pin.id, &pin, &drive)) {
            copy_text(error, sizeof error, "Reconnect the selected drive first.");
        } else {
            Job *job = NULL;
            int code = job_create_captured_locked(&candidate, &drive, etag, modified, error, sizeof error, &job);
            if ((code == 200 || code == 201) && job) {
                copy_text(job_id, sizeof job_id, job->id);
                queued = true;
            }
        }
        pthread_mutex_unlock(&orbit.mutex);
        OPENSSL_cleanse(candidate.url, sizeof candidate.url);
        pthread_mutex_lock(&lock);
        OPENSSL_cleanse(check.url, sizeof check.url);
        copy_text(check.state, sizeof check.state, queued ? "queued" : "failed");
        copy_text(check.message, sizeof check.message, queued ? "Added to Downloads." : error);
        copy_text(check.job_id, sizeof check.job_id, job_id);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
int zero_link_start(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc)
        rc = pthread_create(&thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    started = !rc;
    return rc;
}
void zero_link_stop(void) {
    if (!started)
        return;
    atomic_store(&cancel, true);
    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(thread, NULL);
    started = false;
}
int zero_link_request(const cJSON *input, char *error, size_t cap) {
    const char *url = json_text(input, "url");
    Release option;
    Storage drive;
    if (!*url || strlen(url) >= sizeof option.url) {
        copy_text(error, cap, "This download link doesn't match the selected option.");
        return 400;
    }
    pthread_mutex_lock(&orbit.mutex);
    const Release *found = release_find(json_text(input, "releaseId"));
    bool have_drive = drive_locked(json_text(input, "storageId"), NULL, &drive);
    int code = 200;
    if (!found || !provider_browser_supported(found)) {
        copy_text(error, cap, "This option has no supported provider browser page.");
        code = 400;
    } else if (!have_drive) {
        copy_text(error, cap, "Reconnect the selected drive first.");
        code = 409;
    } else {
        code = job_check_capture_locked(found, &drive, error, cap);
    }
    if (code == 200)
        option = *found;
    pthread_mutex_unlock(&orbit.mutex);
    if (code != 200)
        return code;
    if (!provider_capture_url(&option, url)) {
        copy_text(error, cap, "This download link doesn't match the selected option.");
        return 400;
    }
    pthread_mutex_lock(&lock);
    code = 202;
    if (!started || stopping) {
        copy_text(error, cap, "Orbit can't check download links right now. Restart Orbit, then try again.");
        code = 503;
    } else if (pending || !strcmp(check.state, "checking")) {
        copy_text(error, cap, "Orbit is already checking a download link. Try again shortly.");
        code = 409;
    } else {
        check.option = option;
        check.drive = drive;
        copy_text(check.url, sizeof check.url, url);
        random_hex(check.id, 16);
        copy_text(check.state, sizeof check.state, "checking");
        copy_text(check.message, sizeof check.message, "Checking the file before adding it to Downloads…");
        check.job_id[0] = 0;
        pending = true;
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return code;
}
cJSON *zero_link_status(void) {
    pthread_mutex_lock(&lock);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", check.id);
    cJSON_AddStringToObject(o, "releaseId", check.option.id);
    cJSON_AddStringToObject(o, "state", check.state);
    cJSON_AddStringToObject(o, "message", check.message);
    cJSON_AddStringToObject(o, "jobId", check.job_id);
    pthread_mutex_unlock(&lock);
    return o;
}
bool zero_link_busy(void) {
    pthread_mutex_lock(&lock);
    bool busy = pending || !strcmp(check.state, "checking");
    pthread_mutex_unlock(&lock);
    return busy;
}
