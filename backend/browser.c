#include "orbit.h"
#include "archive_account.h"
#include "remote_sources.h"
#include "debrid.h"
#include "browser_platform.h"
#include "browser_match.h"
#include "provider_network.h"
#include "pspkg.h"
#include "viking_session.h"
#include "viking_pass.h"
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    size_t index;
    char url[sizeof ((PspkgLink *)0)->provider];
} PspkgPage;

/* One user-requested session, held only in memory. The queue receives a validated
 * selected file route, never arbitrary browser bytes or account credentials. */
static struct {
    pthread_t thread;
    pthread_cond_t changed;
    bool ready, stop, pending, busy, thread_started, return_to_browser, native_dialog, dialog_opened, account;
    bool check_account, resolving, pspkg_fetch, pspkg_download;
    atomic_bool cancel;
    char id[33], state[16], message[256], job_id[33], title[128], client_id[65];
    char queued_job_id[sizeof ((Job *)0)->id];
    char mirror_page[sizeof ((Release *)0)->browser_url];
    char native_failure[256];
    char pspkg_release_id[sizeof ((Release *)0)->id];
    char pspkg_game[64], pspkg_status[32];
    bool pspkg_torbox;
    PspkgLink pspkg_links[PSPKG_LINK_LIMIT], pspkg_selected;
    PspkgPage pspkg_pages[PSPKG_LINK_LIMIT];
    size_t pspkg_selected_index;
    size_t pspkg_count, pspkg_page_count;
    time_t pspkg_expires;
    Release release;
    Storage storage;
    Job refresh_job;
    int64_t started_ms, capture_ms, link_ms, validation_ms, queued_ms, finished_ms;
    uint64_t scanned_bytes;
    unsigned candidates, browser_processes;
    unsigned validations, validation_failures;
    bool last_validation_failed;
    const char *native_failure_kind;
    const char *diagnostic_provider, *diagnostic_operation;
    int32_t native_error;
} session = {.changed = PTHREAD_COND_INITIALIZER};

static int64_t milliseconds(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? 0 :
        (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static bool available(void) {
#ifdef ORBIT_TEST
    return true;
#else
    return !orbit.desktop;
#endif
}
#ifndef ORBIT_TEST
static time_t monotonic(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? 0 : now.tv_sec;
}
#endif
static void state_locked(const char *state, const char *message) {
    copy_text(session.state, sizeof session.state, state);
    copy_text(session.message, sizeof session.message, message);
}
static void reset_diagnostics_locked(const char *provider, const char *operation) {
    session.validations = session.validation_failures = 0;
    session.last_validation_failed = false;
    session.native_failure_kind = NULL;
    session.native_error = 0;
    /* Retain fixed labels after the selected release and its URLs are wiped. */
    session.diagnostic_operation = operation;
    session.diagnostic_provider = "none";
    const char *providers[] = {"vikingfile", "akirabox", "datanodes", "1fichier"};
    for (size_t i = 0; i < sizeof providers / sizeof *providers; i++)
        if (!strcmp(provider, providers[i])) session.diagnostic_provider = providers[i];
}
/* Only a small set of native-owned failures can be reported. Never accept
 * page content, signed URLs or arbitrary error text from a browser client. */
static const char *native_failure_message(const char *failure) {
    if (!strcmp(failure, "open"))
        return "The PS5 browser could not open. Close any other browser page, then try again.";
    if (!strcmp(failure, "browser"))
        return "The PS5 browser could not load this page. Check the PS5's date, time and network settings, then open the download page again.";
    if (!strcmp(failure, "timeout"))
        return "Verification timed out. Open the download page again and complete verification if shown.";
    if (!strcmp(failure, "connection"))
        return "Connection to Orbit was lost. Reconnect to Orbit, then open the download page again.";
    return NULL;
}
static bool selected_locked(void) {
    if (session.account) return true;
    if (session.queued_job_id[0]) {
        const Job *j = job_find(session.queued_job_id);
        return j && j->browser_pending && !j->cancel && !j->pause &&
            !strcmp(j->status, "verifying") && source_enabled_locked(&j->release);
    }
    if (session.refresh_job.id[0]) {
        const Job *j = job_find(session.refresh_job.id), *p = &session.refresh_job;
        return job_can_refresh_link(j) && source_enabled_locked(&j->release) &&
            !strcmp(j->status, p->status) && j->received == p->received && j->attempts == p->attempts &&
            !strcmp(j->release.url, p->release.url) && !strcmp(j->etag, p->etag) &&
            !strcmp(j->modified, p->modified) && j->device == p->device && j->inode == p->inode;
    }
    const Release *r = release_find(session.release.id), *p = &session.release;
    return r && source_enabled_locked(r) && r->size == p->size &&
        !strcmp(r->source, p->source) && !strcmp(r->game_id, p->game_id) &&
        !strcmp(r->title_id, p->title_id) && !strcmp(r->filename, p->filename) &&
        !strcmp(r->format, p->format) && !strcmp(r->sha256, p->sha256) &&
        !strcmp(r->url, p->url) && !strcmp(r->browser_url, p->browser_url);
}
static bool active(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool ok = !session.stop && !orbit.stop && !atomic_load(&session.cancel) && selected_locked();
    pthread_mutex_unlock(&orbit.mutex);
    return ok;
}
static void delay(void) {
    struct timespec t = {.tv_nsec = 100000000};
    nanosleep(&t, NULL);
}
static int validate_capture(Release *r, char *etag, char *modified, char *error, size_t cap) {
    const int64_t start = milliseconds();
    int result = transfer_validate_capture(r, &session.cancel, etag, modified, error, cap);
    pthread_mutex_lock(&orbit.mutex);
    session.validation_ms += milliseconds() - start;
    session.validations++;
    session.validation_failures += result != 0;
    session.last_validation_failed = result != 0;
    pthread_mutex_unlock(&orbit.mutex);
    return result;
}
#ifndef ORBIT_TEST
static bool native_active(void *context) {
    return active() && probe_target_active(context);
}
#endif
static int verify_community_capture(Release *r, const Release *locator, const char *url,
                                    char *error, size_t cap) {
    Release file = *r;
    copy_text(file.filename, sizeof file.filename, locator->filename);
    copy_text(file.format, sizeof file.format, locator->format);
    int result = remote_download_verify(&file, url, &session.cancel, error, cap);
    if (!result) *r = file;
    return result;
}

static bool capture(Release *r, Release *community, char *etag, char *modified, bool *validated, char *error, size_t cap) {
    *validated = false;
    /* Native owns the dialog. Do not scan an old browser before it confirms
     * opening this exact session. An abandoned prepare expires in 30 seconds. */
    if (session.native_dialog) {
        bool opened = false;
        for (unsigned i = 0; i < 300 && active(); ++i) {
            pthread_mutex_lock(&orbit.mutex);
            opened = session.dialog_opened;
            pthread_mutex_unlock(&orbit.mutex);
            if (opened) break;
            delay();
        }
        if (!opened || !active()) {
            copy_text(error, cap, "The native download page did not open. Return to Orbit and try again.");
            return false;
        }
    }
    pthread_mutex_lock(&orbit.mutex);
    session.capture_ms = milliseconds() - session.started_ms;
    pthread_mutex_unlock(&orbit.mutex);
#ifdef ORBIT_TEST
    (void)etag; (void)modified;
    /* Compile-time-only simulator: not exposed through the API or in ELF builds. */
    const char *candidate = getenv("ORBIT_TEST_CAPTURE_URL");
    const char *wait = getenv("ORBIT_TEST_CAPTURE_DELAY_MS");
    unsigned steps = wait ? (unsigned)strtoul(wait, NULL, 10) / 100 : 1;
    if (steps > 300) steps = 300;
    for (unsigned i = 0; i < steps && active(); i++) delay();
    if (!active()) return false;
    if (!candidate || !provider_capture_url(r, candidate)) {
        copy_text(error, cap, "No matching download route was found."); return false;
    }
    copy_text(r->url, sizeof r->url, candidate);
    pthread_mutex_lock(&orbit.mutex);
    session.link_ms = milliseconds() - session.started_ms;
    session.candidates++;
    pthread_mutex_unlock(&orbit.mutex);
    return !strcmp(r->source, "pspkg") || !community ||
        !verify_community_capture(community, r, candidate, error, cap);
#else
    if (!active()) return false;
    if (!session.native_dialog && probe_browser_open(r->browser_url)) {
        copy_text(error, cap, "The PS5 browser could not open. Return to Orbit and try again.");
        return false;
    }
    const time_t deadline = monotonic() + PROBE_SESSION_SECONDS;
    ProbeTarget targets[PROBE_TARGET_LIMIT] = {{0}};
    int count = 0;
    time_t missing_since = monotonic();
    char rejected[6][2048] = {{0}};
    int64_t rejected_until[6] = {0};
    unsigned failures = 0;
    BrowserMatch match = {.release = r, .rejected = rejected, .rejected_until = rejected_until};
    ProbeReader reader = {.read = probe_target_read,
        .active = native_active, .match = browser_match, .match_context = &match,
        .remaining = PROBE_SESSION_BUDGET};
    while (active() && monotonic() < deadline && reader.remaining) {
        ProbeTarget found[PROBE_TARGET_LIMIT] = {{0}};
        int n = probe_targets_find(found, PROBE_TARGET_LIMIT);
        if (n < 0) {
            copy_text(error, cap, n == -2
                ? "Close other PS5 browser pages, then try opening this download again."
                : "Orbit could not read the PS5 browser status. Return to Orbit and try again.");
            return false;
        }
        if (!n) {
            if (monotonic() - missing_since >= 10) {
                copy_text(error, cap, "The PS5 browser did not become ready. Return to Orbit and try again.");
                return false;
            }
            delay();
            continue;
        }
        missing_since = monotonic();
        for (int i = 0; i < n; ++i) {
            found[i].cancel = &session.cancel;
            found[i].deadline = deadline;
            for (int j = 0; j < count; ++j)
                if (found[i].pid == targets[j].pid &&
                    found[i].started_seconds == targets[j].started_seconds &&
                    found[i].started_microseconds == targets[j].started_microseconds &&
                    !strcmp(found[i].name, targets[j].name))
                    found[i].cursor = targets[j].cursor;
        }
        memcpy(targets, found, sizeof targets);
        count = n;
        const uint64_t before = reader.attempted;
        for (int i = 0; i < count && active() && reader.remaining; ++i) {
            reader.context = &targets[i];
            match.now_ms = milliseconds();
            /* Share one read/time budget fairly. An older ShellUI browser must
             * not hide or delay the current native dialog's network process. */
            ProbeResult result = probe_target_scan_slice(&targets[i], &reader,
                !strcmp(r->source, "vikingfile") ? "https://vikingfile.com/d/" : "https://",
                PROBE_SCAN_SLICE);
            pthread_mutex_lock(&orbit.mutex);
            session.scanned_bytes = reader.attempted;
            session.browser_processes = count;
            if (result == PROBE_FOUND) {
                session.link_ms = milliseconds() - session.started_ms;
                session.candidates++;
            }
            pthread_mutex_unlock(&orbit.mutex);
            if (result == PROBE_FOUND && active()) {
                /* All candidates still require the selected filename/host and
                 * actual response identity. A stale link in a previous page
                 * cannot stop us looking for the newly generated one. */
                Release candidate = *r;
                copy_text(candidate.url, sizeof candidate.url, match.url);
                pthread_mutex_lock(&orbit.mutex);
                if (!session.stop && !atomic_load(&session.cancel))
                    state_locked("validating", "Checking the selected file before adding it to Downloads…");
                pthread_mutex_unlock(&orbit.mutex);
                int invalid = !strcmp(r->source, "pspkg") ? 0 : community
                    ? verify_community_capture(community, &candidate, candidate.url, error, cap)
                    : validate_capture(&candidate, etag, modified, error, cap);
                memset(&candidate, 0, sizeof candidate);
                if (invalid) {
                    pthread_mutex_lock(&orbit.mutex);
                    if (!session.stop && !atomic_load(&session.cancel))
                        state_locked("waiting-link", "Waiting for a fresh download link from the provider…");
                    pthread_mutex_unlock(&orbit.mutex);
                    /* A generated route may briefly fail before the provider
                     * makes it usable. Recheck after a cooldown, always with
                     * full identity validation and the existing session limit. */
                    size_t slot = failures % 6;
                    copy_text(rejected[slot], sizeof rejected[0], match.url);
                    rejected_until[slot] = milliseconds() + 8000;
                    if (match.rejected_count < 6) match.rejected_count++;
                    match.url[0] = 0;
                    if (++failures == 18) return false;
                    continue;
                }
                *validated = strcmp(r->source, "pspkg") != 0;
                copy_text(r->url, sizeof r->url, match.url);
                memset(&match, 0, sizeof match);
                memset(rejected, 0, sizeof rejected);
                return true;
            }
            /* A process can exit/restart while the new page opens. Discovery
             * refreshes its identity next round; never keep reading a reused PID. */
        }
        if (reader.attempted == before) delay();
    }
    if (!error[0]) copy_text(error, cap,
        "No matching download was found within the verification window. Return to Orbit to try again.");
    return false;
#endif
}

/* A mirror supplies the bytes; the queue still uses the selected option's
 * identity and stable provider page. */
static void community_file(Release *selected, const Release *file) {
    copy_text(selected->url, sizeof selected->url, file->url);
    copy_text(selected->filename, sizeof selected->filename, file->filename);
    selected->remote_image_suffix_added = file->remote_image_suffix_added;
    copy_text(selected->format, sizeof selected->format, file->format);
    copy_text(selected->compression, sizeof selected->compression, file->compression);
    copy_text(selected->archive_password, sizeof selected->archive_password, file->archive_password);
    selected->size = file->size;
}

static bool recover_community_locked(const Release *selected, const Release *route) {
    if (orbit.stop || session.stop || atomic_load(&session.cancel) || !selected_locked() ||
        !source_enabled_locked(route) || !debrid_link_covered_locked(route)) return false;
    Job *j = session.queued_job_id[0] ? job_find(session.queued_job_id) : NULL;
    if (j) {
        if (!debrid_resume_after_missing_locked(j, route)) return false;
        if (state_save_locked()) {
            copy_text(j->status, sizeof j->status, "error");
            state_locked("failed", "Could not persist the queue.");
            return true;
        }
    } else {
        char error[256] = "";
        if (job_create_torbox_recovery_locked(selected, route, &session.storage, error, sizeof error, &j) != 201)
            return false;
    }
    copy_text(session.job_id, sizeof session.job_id, j->id);
    session.return_to_browser = false;
    state_locked("queued", "Your verified file has been added to Downloads.");
    session.queued_ms = milliseconds() - session.started_ms;
    pthread_cond_broadcast(&orbit.changed);
    return true;
}

typedef enum { COMMUNITY_FAILED, COMMUNITY_FOUND, COMMUNITY_ROUTED } CommunityResult;

static CommunityResult resolve_community(Release *selected, char *etag, char *modified,
                                         char *error, size_t cap) {
    Release candidate = *selected, interactive = {0}, interactive_locator = {0};
    char after[sizeof candidate.id] = "", preferred[sizeof candidate.id] = "";
    pthread_mutex_lock(&orbit.mutex);
    const Job *j = session.queued_job_id[0] ? job_find(session.queued_job_id) : NULL;
    bool queued = j != NULL;
    bool found = !j || !j->mirror_id[0] || remote_mirror_locked(selected, j->mirror_id, &candidate, false);
    if (j && !j->mirror_id[0]) {
        const Release *original = release_find(selected->id);
        if (original) candidate = *original;
        else if (candidate.remote_page) candidate.url[0] = 0;
    } else if (!j && remote_direct_route_locked(selected, &candidate) && strcmp(candidate.id, selected->id)) {
        copy_text(preferred, sizeof preferred, candidate.id);
    }
    pthread_mutex_unlock(&orbit.mutex);
    if (!found) {
        copy_text(error, cap, "This mirror is no longer available.");
        return COMMUNITY_FAILED;
    }
    bool all_missing = true;
    while (active()) {
        Release locator;
        char candidate_error[256] = "";
        bool primary = !strcmp(candidate.id, selected->id);
        pthread_mutex_lock(&orbit.mutex);
        bool known_missing = remote_link_missing_locked(&candidate);
        pthread_mutex_unlock(&orbit.mutex);
        int result = known_missing ? REMOTE_DOWNLOAD_MISSING : remote_page_pending(&candidate)
            ? remote_download_resolve(&candidate, &locator, &session.cancel, candidate_error, sizeof candidate_error)
            : remote_download_verify(&candidate, candidate.url, &session.cancel, candidate_error, sizeof candidate_error);
        if (result == 1 && !strcmp(locator.source, "vikingfile") && active()) {
            bool viking_missing = false;
            if (viking_resolve_download(&locator, session.native_dialog, &session.cancel, &viking_missing) &&
                !verify_community_capture(&candidate, &locator, locator.url, candidate_error, sizeof candidate_error))
                result = 0;
            else if (viking_missing) result = REMOTE_DOWNLOAD_MISSING;
        }
        if (result == REMOTE_DOWNLOAD_MISSING) {
            pthread_mutex_lock(&orbit.mutex);
            remote_mark_missing_locked(&candidate);
            bool routed = recover_community_locked(selected, &candidate);
            pthread_mutex_unlock(&orbit.mutex);
            if (routed) return COMMUNITY_ROUTED;
        } else {
            all_missing = false;
            if (candidate_error[0]) copy_text(error, cap, candidate_error);
        }
        if (!result && remote_mirror_file(selected, &candidate) &&
            (!primary || !selected->filename[0] || !strcmp(selected->filename, candidate.filename))) {
            community_file(selected, &candidate);
            pthread_mutex_lock(&orbit.mutex);
            session.return_to_browser = false;
            pthread_mutex_unlock(&orbit.mutex);
            return COMMUNITY_FOUND;
        }
        if (result == 1 && !interactive.id[0]) {
            interactive = candidate;
            interactive_locator = locator;
        }
        if (queued) break;
        if (preferred[0] && !after[0] && !strcmp(candidate.id, preferred)) {
            candidate = *selected;
            continue;
        }
        pthread_mutex_lock(&orbit.mutex);
        bool next;
        do {
            next = remote_next_mirror_locked(selected, after, &candidate);
            if (next) copy_text(after, sizeof after, candidate.id);
        } while (next && !strcmp(candidate.id, preferred));
        pthread_mutex_unlock(&orbit.mutex);
        if (!next) break;
    }
    if (all_missing) copy_text(error, cap, REMOTE_DOWNLOAD_REMOVED);
    if (!interactive.id[0] || !active()) return COMMUNITY_FAILED;

    pthread_mutex_lock(&orbit.mutex);
    session.resolving = false;
    if (!atomic_load(&session.cancel)) {
        copy_text(session.mirror_page, sizeof session.mirror_page, interactive_locator.browser_url);
        state_locked("opening", "Opening the download page on your PS5…");
    }
    pthread_mutex_unlock(&orbit.mutex);
    bool validated = false;
    error[0] = 0;
    if (!capture(&interactive_locator, &interactive, etag, modified, &validated, error, cap) ||
        (strcmp(interactive.id, selected->id) && !remote_mirror_file(selected, &interactive))) return COMMUNITY_FAILED;
    community_file(selected, &interactive);
    return COMMUNITY_FOUND;
}

static void finish_queued_locked(Job *j, const Release *resolved, bool ok,
                                 const char *etag, const char *modified, char *error, size_t cap) {
    if (!j || !j->browser_pending || strcmp(j->status, "verifying")) return;
    j->delivery_phase[0] = 0;
    const char *status = "error";
    if (orbit.stop || session.stop || j->pause || !source_enabled_locked(&j->release)) {
        status = "paused";
    } else if (j->cancel || atomic_load(&session.cancel)) {
        status = "cancelled";
    } else if (ok) {
        j->mirror_verified = j->mirror_id[0];
        j->browser_pending = false;
        copy_text(j->status, sizeof j->status, "queued");
        copy_text(j->etag, sizeof j->etag, etag);
        copy_text(j->modified, sizeof j->modified, modified);
        if (job_assign_remote_file_locked(j, resolved, error, cap)) {
            copy_text(session.job_id, sizeof session.job_id, j->id);
            state_locked("queued", "Your verified file has been added to Downloads.");
            session.queued_ms = milliseconds() - session.started_ms;
            pthread_cond_broadcast(&orbit.changed);
            return;
        }
        j->browser_pending = true;
        j->mirror_verified = false;
        j->etag[0] = j->modified[0] = 0;
    } else if (job_advance_mirror_locked(j)) {
        copy_text(session.job_id, sizeof session.job_id, j->id);
        state_locked("queued", "Trying another download option.");
        (void)state_save_locked();
        pthread_cond_broadcast(&orbit.changed);
        return;
    }
    copy_text(j->status, sizeof j->status, status);
    if (!strcmp(status, "error")) {
        copy_text(j->error, sizeof j->error, error[0] ? error : "Verification stopped. Return to Orbit to try again.");
        state_locked("failed", j->error);
    } else {
        state_locked("cancelled", "Verification cancelled");
    }
    (void)state_save_locked();
    pthread_cond_broadcast(&orbit.changed);
}

typedef struct {
    char platform[4], game_id[13];
    const char *page;
    cJSON *fields;
    bool verified;
    PspkgLink links[PSPKG_LINK_LIMIT];
    size_t count;
} PspkgCollection;

static void collect_pspkg_html(PspkgCollection *found, const char *html, const char *edition,
                               const char *field_id) {
    cJSON *rows = remote_pspkg_session_links(html, found->page, edition);
    const cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        const char *url = json_text(row, "url");
        if (!*url || strlen(url) >= sizeof found->links[0].url) continue;
        if (field_id && !pspkg_link_matches(url, found->page, found->platform, found->game_id, field_id))
            continue;
        if (field_id) found->verified = true;
        size_t i = 0;
        for (; i < found->count; i++)
            if (!strcmp(found->links[i].url, url) &&
                (!found->links[i].edition[0] ||
                 !strcmp(found->links[i].edition, json_text(row, "edition")))) break;
        if (i == found->count && found->count == PSPKG_LINK_LIMIT) break;
        PspkgLink *link = &found->links[i];
        if (i == found->count) {
            copy_text(link->url, sizeof link->url, url);
            found->count++;
        }
        copy_text(link->label, sizeof link->label, json_text(row, "label"));
        copy_text(link->edition, sizeof link->edition, json_text(row, "edition"));
        copy_text(link->group, sizeof link->group, json_text(row, "group"));
        copy_text(link->filename, sizeof link->filename, json_text(row, "filename"));
        copy_text(link->archive_password, sizeof link->archive_password, json_text(row, "archivePassword"));
        link->size = json_int(row, "sizeEstimateBytes");
        copy_text(link->title_id, sizeof link->title_id, json_text(row, "titleId"));
        copy_text(link->version, sizeof link->version, json_text(row, "version"));
        copy_text(link->region, sizeof link->region, json_text(row, "region"));
        copy_text(link->format, sizeof link->format, json_text(row, "format"));
        copy_text(link->compression, sizeof link->compression, json_text(row, "compression"));
        copy_text(link->reason, sizeof link->reason, json_text(row, "filterReason"));
        link->intermediate = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "intermediate"));
        const char *id = link->intermediate ? strstr(url, "&link_id=") : NULL;
        if (id) {
            id += sizeof "&link_id=" - 1;
            size_t length = strspn(id, "0123456789");
            if (length && length < sizeof link->link_id && (!id[length] || id[length] == '&'))
                snprintf(link->link_id, sizeof link->link_id, "%.*s", (int)length, id);
        }
    }
    cJSON_Delete(rows);
}

/* The landing page publishes field IDs. After the user's verification, its
 * script replaces those elements with HTML from the browser's JSON response.
 * If Cloudflare blocks the public landing request, bind revealed rows using
 * their same-origin game and edition IDs instead. */
static void collect_pspkg_reveal(PspkgCollection *found, const char *json, size_t length) {
    cJSON *reply = cJSON_ParseWithLength(json, length);
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(reply, "fields");
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "success")) && cJSON_IsObject(fields)) {
        bool has_labels = cJSON_GetArraySize(found->fields) > 0;
        const cJSON *field;
        cJSON_ArrayForEach(field, fields) {
            const char *edition = json_text(found->fields, field->string);
            if (!cJSON_IsString(field) || (has_labels && !*edition)) continue;
            if (has_labels) found->verified = true;
            collect_pspkg_html(found, field->valuestring, edition, has_labels ? NULL : field->string);
        }
    }
    cJSON_Delete(reply);
}

#ifndef ORBIT_TEST
#define PSPKG_REVEAL_SPAN (32U * 1024U)
static bool pspkg_match(void *context, const unsigned char *bytes, size_t length) {
    PspkgCollection *found = context;
    /* Search only for the shape of a successful reveal response. The parser
     * checks its field IDs against the chosen game before admitting anchors. */
    static const char success[] = "{\"success\":true";
    for (size_t i = 0; i + sizeof success <= length; i++) {
        if (bytes[i] != '{') continue;
        size_t span = length - i < PSPKG_REVEAL_SPAN ? length - i : PSPKG_REVEAL_SPAN;
        if (!memcmp(bytes + i, success, sizeof success - 1))
            collect_pspkg_reveal(found, (const char *)bytes + i, span);
        if (found->verified) return true;
        if (i + 2 * sizeof success > length || bytes[i + 1]) continue;
        size_t j = 0;
        while (j < sizeof success - 1 && bytes[i + j * 2] == (unsigned char)success[j] &&
               !bytes[i + j * 2 + 1]) j++;
        if (j != sizeof success - 1) continue;
        size_t units = span / 2;
        char *json = malloc(units * 6 + 1);
        if (!json) continue;
        size_t used = 0;
        for (j = 0; j < units; j++) {
            unsigned code = bytes[i + j * 2] | ((unsigned)bytes[i + j * 2 + 1] << 8);
            if (!code) break;
            if (code < 0x80) json[used++] = (char)code;
            /* JSON decodes these UTF-16 code units, including surrogate pairs. */
            else used += (size_t)snprintf(json + used, 7, "\\u%04x", code);
        }
        json[used] = 0;
        collect_pspkg_reveal(found, json, used);
        free(json);
        if (found->verified) return true;
    }
    if (!cJSON_GetArraySize(found->fields))
        pspkg_scan_links(bytes, length, found->platform, found->game_id,
                         found->links, &found->count, PSPKG_LINK_LIMIT);
    return false;
}
#endif

static bool collect_pspkg(const Release *release, PspkgCollection *found,
                          char *error, size_t cap) {
    if (!pspkg_page_identity(release->browser_url, found->platform,
                             sizeof found->platform, found->game_id,
                             sizeof found->game_id)) {
        copy_text(error, cap, "This PSPKG game has no valid download page.");
        return false;
    }
    found->page = release->browser_url;
    RemoteMetadataStatus public_status;
    found->fields = remote_pspkg_public_fields(release->browser_url, &public_status, &session.cancel);
    if (public_status == REMOTE_METADATA_MISSING) {
        copy_text(error, cap, "missing-page");
        return false;
    }
#ifdef ORBIT_TEST
    const char *html = getenv("ORBIT_TEST_PSPKG_HTML");
    const char *wait = getenv("ORBIT_TEST_PSPKG_DELAY_MS");
    unsigned steps = wait ? (unsigned)strtoul(wait, NULL, 10) / 100 : 1;
    for (unsigned i = 0; i < steps && i < 300 && active(); i++) delay();
    if (!active()) return false;
    if (html) {
        if (*html == '{') collect_pspkg_reveal(found, html, strlen(html));
        else collect_pspkg_html(found, html, NULL, NULL);
        if (!found->count && !strstr(html, "Awaiting verification")) copy_text(error, cap, "no-anchors");
    }
#else
    if (session.native_dialog) {
        bool opened = false;
        for (unsigned i = 0; i < 300 && active(); i++) {
            pthread_mutex_lock(&orbit.mutex);
            opened = session.dialog_opened;
            pthread_mutex_unlock(&orbit.mutex);
            if (opened) break;
            delay();
        }
        if (!opened || !active()) {
            copy_text(error, cap, "The PSPKG page did not open. Return to Orbit and try again.");
            return false;
        }
    } else if (probe_browser_open(release->browser_url)) {
        copy_text(error, cap, "The PS5 browser could not open the PSPKG page.");
        return false;
    }
    pthread_mutex_lock(&orbit.mutex);
    if (!atomic_load(&session.cancel))
        state_locked("waiting-link", "Complete PSPKG verification if shown, then wait for its download choices.");
    pthread_mutex_unlock(&orbit.mutex);
    const time_t deadline = monotonic() + PROBE_SESSION_SECONDS;
    ProbeTarget targets[PROBE_TARGET_LIMIT] = {{0}};
    int count = 0;
    int64_t last_new = 0;
    PspkgCollection *collection = found;
    ProbeReader reader = {.read = probe_target_read, .active = native_active,
        .match = pspkg_match, .match_context = collection, .match_span = PSPKG_REVEAL_SPAN,
        .remaining = PROBE_SESSION_BUDGET};
    while (active() && monotonic() < deadline && reader.remaining) {
        ProbeTarget current[PROBE_TARGET_LIMIT] = {{0}};
        int n = probe_targets_find(current, PROBE_TARGET_LIMIT);
        if (n < 0) {
            copy_text(error, cap, "Orbit could not read the PS5 browser status.");
            return false;
        }
        for (int i = 0; i < n; i++) {
            current[i].cancel = &session.cancel;
            current[i].deadline = deadline;
            for (int j = 0; j < count; j++)
                if (current[i].pid == targets[j].pid &&
                    current[i].started_seconds == targets[j].started_seconds &&
                    current[i].started_microseconds == targets[j].started_microseconds &&
                    !strcmp(current[i].name, targets[j].name))
                    current[i].cursor = targets[j].cursor;
        }
        memcpy(targets, current, sizeof targets);
        count = n;
        for (int i = 0; i < count && active() && reader.remaining && !found->verified; i++) {
            reader.context = &targets[i];
            size_t before = found->count;
            (void)probe_target_scan_slice(&targets[i], &reader,
                                          "download.php?game_id=", PROBE_SCAN_SLICE);
            if (found->count > before) last_new = milliseconds();
        }
        pthread_mutex_lock(&orbit.mutex);
        session.scanned_bytes = reader.attempted;
        session.browser_processes = (unsigned)count;
        session.candidates = (unsigned)found->count;
        pthread_mutex_unlock(&orbit.mutex);
        if (found->verified || (found->count && milliseconds() - last_new >= 1500)) break;
        delay();
    }
#endif
    if (found->count) return true;
    if (found->verified && !error[0]) copy_text(error, cap, "no-anchors");
    if (!error[0]) copy_text(error, cap,
        "PSPKG did not reveal download links. Complete the check on that game's page and try again.");
    return false;
}

/* A PSPKG choice is a browser-only handoff. Resolve its actual provider page
 * first, then use TorBox for a covered host or verify the provider's file. */
static void download_pspkg(void) {
    Release selected = session.release;
    PspkgLink choice = session.pspkg_selected;
    size_t selected_index = session.pspkg_selected_index;
    Storage storage = session.storage;
    pthread_mutex_unlock(&orbit.mutex);
    char error[256] = {0}, etag[256] = {0}, modified[128] = {0};
    bool validated = false, ok = active();
    const char *route_status = "";
    Release locator = selected;
    copy_text(locator.source, sizeof locator.source, "pspkg");
    locator.url[0] = 0;
    if (ok && choice.provider[0]) copy_text(locator.url, sizeof locator.url, choice.provider);
    else if (ok) {
        route_status = remote_pspkg_route(choice.url, choice.intermediate, &session.cancel,
                                          locator.url, sizeof locator.url, NULL);
        if (*route_status) {
            ok = false;
            if (!strcmp(route_status, "verify-required")) {
                copy_text(error, sizeof error, "Open this PSPKG link and complete verification again.");
                bool bound = choice.intermediate && strlen(choice.title_id) == 9 &&
                    strlen(choice.url) < sizeof locator.browser_url;
                if (bound && active()) {
                    copy_text(locator.title_id, sizeof locator.title_id, choice.title_id);
                    copy_text(locator.browser_url, sizeof locator.browser_url, choice.url);
                    remote_pspkg_identify_choice(&locator, choice.url, selected_index);
                    pthread_mutex_lock(&orbit.mutex);
                    state_locked("waiting-link", "Complete PSPKG verification for this link in the browser.");
                    pthread_mutex_unlock(&orbit.mutex);
                    ok = capture(&locator, NULL, etag, modified, &validated, error, sizeof error);
                    if (ok) route_status = "";
                }
#ifndef ORBIT_DESKTOP
                else if (active() && choice.intermediate) probe_browser_open(choice.url);
#endif
            } else copy_text(error, sizeof error, "The PSPKG provider link could not be resolved.");
        }
    }
    Release route = selected;
    if (choice.archive_password[0])
        copy_text(route.archive_password, sizeof route.archive_password, choice.archive_password);
    if (choice.format[0]) copy_text(route.format, sizeof route.format, choice.format);
    if (choice.edition[0]) copy_text(route.remote_edition, sizeof route.remote_edition, choice.edition);
    if (choice.version[0]) copy_text(route.remote_version, sizeof route.remote_version, choice.version);
    if (choice.region[0]) copy_text(route.remote_region, sizeof route.remote_region, choice.region);
    if (choice.compression[0]) {
        copy_text(route.compression, sizeof route.compression, choice.compression);
        for (char *p = route.compression; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    }
    if (choice.size && !choice.size_exact) route.display_size = choice.size;
    remote_pspkg_identify_choice(&route, choice.url, selected_index);
    const char *provider = ok ? remote_browser_provider(locator.url) : "";
    bool prefer_torbox = false;
    if (*provider) {
        copy_text(route.remote_provider_page, sizeof route.remote_provider_page, locator.url);
        route.url[0] = 0;
        pthread_mutex_lock(&orbit.mutex);
        prefer_torbox = session.pspkg_torbox && debrid_link_covered_locked(&route) &&
            (strcmp(choice.provider, locator.url) || !choice.torbox_cached_revision ||
             choice.torbox_cached_revision != debrid_cache_revision_locked(&route) ||
             debrid_size_allowed(choice.torbox_size));
        pthread_mutex_unlock(&orbit.mutex);
    }
    bool missing = *provider && !prefer_torbox && remote_download_confirm_missing(locator.url, &session.cancel);
    if (missing) {
        copy_text(error, sizeof error, REMOTE_DOWNLOAD_REMOVED);
        ok = false;
    }
    if (ok && !*provider) {
        pthread_mutex_lock(&orbit.mutex);
        state_locked("validating", "Checking the PSPKG file before adding it to Downloads…");
        pthread_mutex_unlock(&orbit.mutex);
        int result = remote_download_verify(&route, locator.url, &session.cancel, error, sizeof error);
        missing = result == REMOTE_DOWNLOAD_MISSING;
        ok = result == 0;
        validated = ok;
    }
    pthread_mutex_lock(&orbit.mutex);
    if (ok && *provider && !session.stop && !atomic_load(&session.cancel) && selected_locked() &&
        prefer_torbox && debrid_link_covered_locked(&route)) {
        Job *job = NULL;
        int code = job_create_pspkg_torbox_locked(&route, &storage, error, sizeof error, &job);
        if (code == 201 && job && !strcmp(job->delivery, "torbox")) {
            copy_text(session.job_id, sizeof session.job_id, job->id);
            state_locked("queued", "The PSPKG provider has been added to Downloads through TorBox.");
            session.queued_ms = milliseconds() - session.started_ms;
            ok = false; /* Completed; do not also start a direct download. */
        } else error[0] = 0; /* Provider may still offer a direct file. */
    }
    bool queued = !strcmp(session.state, "queued");
    if (ok && *provider && !queued && !session.stop && !atomic_load(&session.cancel) && selected_locked())
        state_locked("waiting-link", "Open Download on the provider page. Orbit will check the file before adding it to Downloads.");
    pthread_mutex_unlock(&orbit.mutex);
    if (ok && *provider && !queued && active()) {
        Release provider_page = route;
        int result = remote_download_resolve(&route, &provider_page, &session.cancel, error, sizeof error);
        missing = result == REMOTE_DOWNLOAD_MISSING;
        validated = ok = result == 0;
        if (result == 1 && active())
            ok = capture(&provider_page, &route, etag, modified, &validated, error, sizeof error);
        if (ok && !validated) {
            copy_text(error, sizeof error, "The provider file could not be verified.");
            ok = false;
        }
        memset(&provider_page, 0, sizeof provider_page);
    }
    pthread_mutex_lock(&orbit.mutex);
    if (!queued) {
        if (missing) route_status = "missing-page";
        if (*route_status && selected_index < session.pspkg_count)
            copy_text(session.pspkg_links[selected_index].reason,
                      sizeof session.pspkg_links[selected_index].reason, route_status);
        if (session.stop || atomic_load(&session.cancel))
            state_locked("cancelled", "PSPKG verification cancelled. No download was added.");
        else if (!selected_locked())
            state_locked("failed", "The source or catalogue option changed. Fetch PSPKG links again.");
        else if (ok) {
            Job *job = NULL;
            int code = job_create_captured_locked(&route, &storage, etag, modified,
                                                  error, sizeof error, &job);
            if (code == 201 && job) {
                copy_text(session.job_id, sizeof session.job_id, job->id);
                state_locked("queued", "The verified PSPKG file has been added to Downloads.");
                session.queued_ms = milliseconds() - session.started_ms;
            } else state_locked("failed", error);
        } else state_locked("failed", error[0] ? error : "PSPKG verification stopped. Try this link again.");
    }
    memset(&locator, 0, sizeof locator);
    memset(&route, 0, sizeof route);
    memset(&session.release, 0, sizeof session.release);
    memset(&session.pspkg_selected, 0, sizeof session.pspkg_selected);
    session.pspkg_download = session.busy = false;
    session.finished_ms = milliseconds() - session.started_ms;
}

static void update_pspkg_metadata(PspkgLink *link, const RemoteFileMetadata *metadata) {
    link->archive_requires_login = metadata->archive_requires_login;
    if (metadata->filename[0]) {
        copy_text(link->filename, sizeof link->filename, metadata->filename);
        pspkg_version(link->edition, link->filename, link->version, sizeof link->version);
    }
    if (metadata->size) {
        link->size = metadata->size;
        link->size_exact = metadata->exact;
    }
}

static void update_pspkg_provider(PspkgLink *link, const char *url) {
    copy_text(link->provider, sizeof link->provider, url);
    Release route = {.remote_page = true};
    copy_text(route.remote_provider_page, sizeof route.remote_provider_page, url);
    pthread_mutex_lock(&orbit.mutex);
    unsigned revision = link->archive_requires_login ? 0 : debrid_cache_revision_locked(&route);
    pthread_mutex_unlock(&orbit.mutex);
    link->torbox_size = 0;
    link->torbox_cached_revision = revision &&
        debrid_web_metadata(&route, revision, !link->size_exact, &link->torbox_size) ? revision : 0;
}

static void pspkg_choices_ready_locked(void) {
    size_t usable = 0, waiting = 0;
    for (size_t i = 0; i < session.pspkg_count; i++) {
        usable += !session.pspkg_links[i].reason[0];
        waiting += !strcmp(session.pspkg_links[i].reason, "verify-required");
    }
    copy_text(session.pspkg_status, sizeof session.pspkg_status, usable
        ? usable < session.pspkg_count ? "partial" : "ready" : waiting ? "verify-required" : "rejected");
    state_locked("links-ready", usable ? "PSPKG download choices are ready. Select one to continue."
        : "PSPKG links need verification or were rejected by download policy.");
}

/* A paired client can report public provider pages seen in its own browser.
 * This worker reads HTML and updates link choices without creating jobs. */
static void read_pspkg_pages_locked(void) {
    for (size_t i = 0; i < session.pspkg_page_count; i++) {
        if (session.stop || atomic_load(&session.cancel) || !selected_locked() ||
            time(NULL) >= session.pspkg_expires) break;
        PspkgPage page = session.pspkg_pages[i];
        PspkgLink link = session.pspkg_links[page.index];
        pthread_mutex_unlock(&orbit.mutex);
        RemoteFileMetadata metadata = {0};
        bool read = remote_pspkg_provider_metadata(page.url, &session.cancel, &metadata);
        bool missing = metadata.status == REMOTE_METADATA_MISSING;
        bool requires_login = metadata.status == REMOTE_METADATA_ARCHIVE_LOGIN;
        if (missing) copy_text(link.reason, sizeof link.reason, "missing-page");
        else if (requires_login) copy_text(link.reason, sizeof link.reason, "archive-login-required");
        else if (read && active()) {
            update_pspkg_metadata(&link, &metadata);
            update_pspkg_provider(&link, page.url);
            link.reason[0] = 0;
        }
        pthread_mutex_lock(&orbit.mutex);
        if ((read || missing || requires_login) && page.index < session.pspkg_count && !session.stop &&
            !atomic_load(&session.cancel) && selected_locked() && time(NULL) < session.pspkg_expires)
            session.pspkg_links[page.index] = link;
    }
    if (atomic_load(&session.cancel)) {
        copy_text(session.pspkg_status, sizeof session.pspkg_status, "cancelled");
    } else if (selected_locked()) {
        pspkg_choices_ready_locked();
    } else {
        copy_text(session.pspkg_status, sizeof session.pspkg_status, "expired");
        state_locked("failed", "The source or catalogue option changed. Fetch PSPKG links again.");
    }
    memset(session.pspkg_pages, 0, sizeof session.pspkg_pages);
    memset(&session.release, 0, sizeof session.release);
    session.pspkg_page_count = 0;
    session.finished_ms = milliseconds() - session.started_ms;
    session.resolving = session.busy = false;
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!session.stop) {
        while (!session.pending && !session.stop)
            pthread_cond_wait(&session.changed, &orbit.mutex);
        if (session.stop) break;
        session.pending = false;
        if (session.pspkg_page_count) {
            read_pspkg_pages_locked();
            continue;
        }
        /* Account pages use the provider's own cookie jar. Do not scan their
         * memory, read credentials, validate a file, or create a queue entry. */
        if (session.account) {
            if (session.check_account) {
                pthread_mutex_unlock(&orbit.mutex);
                (void)viking_check_session(session.native_dialog, &session.cancel);
                pthread_mutex_lock(&orbit.mutex);
                state_locked("idle", "Vikingfile");
                session.busy = false;
                continue;
            }
            if (session.native_dialog) {
                /* The native app can acknowledge the open before this worker
                 * is scheduled. Preserve that acknowledgement (or failure). */
                if (!session.dialog_opened && !atomic_load(&session.cancel))
                    state_locked("opening", "Opening Vikingfile sign-in…");
                const int64_t deadline = milliseconds() + 240000;
                while (!session.stop && !orbit.stop && !atomic_load(&session.cancel) && milliseconds() < deadline) {
                    pthread_mutex_unlock(&orbit.mutex);
                    delay();
                    pthread_mutex_lock(&orbit.mutex);
                }
                if (!session.stop && !orbit.stop && !session.native_failure[0]) {
                    pthread_mutex_unlock(&orbit.mutex);
                    (void)viking_check_session(true, NULL);
                    pthread_mutex_lock(&orbit.mutex);
                }
            } else if (!atomic_load(&session.cancel)) {
                pthread_mutex_unlock(&orbit.mutex);
#ifdef ORBIT_TEST
                const int opened = 0;
#else
                const int opened = probe_browser_open("https://vikingfile.com/login");
#endif
                pthread_mutex_lock(&orbit.mutex);
                if (opened) copy_text(session.native_failure, sizeof session.native_failure,
                    "The PS5 browser could not open. Close any other browser page, then try again.");
            }
            state_locked(session.native_failure[0] ? "failed" : "idle", session.native_failure[0]
                ? session.native_failure : "Sign in on Vikingfile, then return to Orbit.");
            session.finished_ms = milliseconds() - session.started_ms;
            session.busy = false;
            continue;
        }
        if (session.pspkg_fetch) {
            Release selected = session.release;
            pthread_mutex_unlock(&orbit.mutex);
            PspkgCollection found = {0};
            char error[256] = {0};
            bool ok = active() && collect_pspkg(&selected, &found, error, sizeof error);
            if (ok) {
                pthread_mutex_lock(&orbit.mutex);
                ok = selected_locked() && !atomic_load(&session.cancel);
                if (ok) {
                    memcpy(session.pspkg_links, found.links, found.count * sizeof found.links[0]);
                    session.pspkg_count = found.count;
                    session.pspkg_expires = time(NULL) + 300;
                    for (size_t i = 0; i < found.count; i++)
                        if (!session.pspkg_links[i].reason[0])
                            copy_text(session.pspkg_links[i].reason, sizeof session.pspkg_links[i].reason, "resolving");
                    copy_text(session.pspkg_status, sizeof session.pspkg_status, "resolving");
                    state_locked("validating", "Checking the PSPKG provider links.");
#ifndef ORBIT_DESKTOP
                    if (session.return_to_browser && !session.stop) {
                        char home[144];
                        snprintf(home, sizeof home, "http://127.0.0.1:%d/?game=%s", ORBIT_HTTP_PORT, session.pspkg_game);
                        pthread_mutex_unlock(&orbit.mutex);
                        probe_browser_open(home);
                        pthread_mutex_lock(&orbit.mutex);
                    }
#endif
                }
                pthread_mutex_unlock(&orbit.mutex);
            }
            for (size_t i = 0; ok && active() && i < found.count; i++) {
                PspkgLink *link = &found.links[i];
                if (link->reason[0]) continue;
                char resolved[2048] = "";
                RemoteFileMetadata metadata = {0};
                const char *reason = remote_pspkg_route(link->url, link->intermediate, &session.cancel,
                                                       resolved, sizeof resolved, &metadata);
                copy_text(link->reason, sizeof link->reason, reason);
                if (resolved[0] && (!*reason || !strcmp(reason, "verify-required"))) {
                    update_pspkg_metadata(link, &metadata);
                    const char *provider = remote_browser_provider(resolved);
                    if (*provider && strlen(resolved) < sizeof link->provider)
                        update_pspkg_provider(link, resolved);
                    else copy_text(link->url, sizeof link->url, resolved);
                }
                pthread_mutex_lock(&orbit.mutex);
                if (!atomic_load(&session.cancel)) session.pspkg_links[i] = *link;
                pthread_mutex_unlock(&orbit.mutex);
            }
            pthread_mutex_lock(&orbit.mutex);
            if (ok && selected_locked() && !atomic_load(&session.cancel)) {
                memcpy(session.pspkg_links, found.links, found.count * sizeof found.links[0]);
                session.pspkg_count = found.count;
                session.pspkg_expires = time(NULL) + 300;
#ifdef ORBIT_TEST
                const char *ttl = getenv("ORBIT_TEST_PSPKG_TTL");
                if (ttl) session.pspkg_expires = time(NULL) + atoi(ttl);
#endif
                copy_text(session.pspkg_release_id, sizeof session.pspkg_release_id, selected.id);
                pspkg_choices_ready_locked();
            } else {
                const char *status = atomic_load(&session.cancel) ? "cancelled" : !selected_locked() ? "scan-miss" :
                    error[0] && !strcmp(error, "missing-page") ? "missing-page" :
                    error[0] && !strcmp(error, "no-anchors") ? "no-anchors" : "verify-required";
                copy_text(session.pspkg_status, sizeof session.pspkg_status, status);
                state_locked(session.native_failure[0] ? "failed" :
                             atomic_load(&session.cancel) ? "cancelled" : "failed",
                             session.native_failure[0] ? session.native_failure :
                             !strcmp(status, "missing-page") ? "The PSPKG download page is missing." :
                             !strcmp(status, "no-anchors") ? "PSPKG verification completed without usable download links." :
                             error[0] ? error : "PSPKG verification stopped.");
            }
            if (atomic_load(&session.cancel))
                for (size_t i = 0; i < session.pspkg_count; i++)
                    if (!strcmp(session.pspkg_links[i].reason, "resolving"))
                        copy_text(session.pspkg_links[i].reason, sizeof session.pspkg_links[i].reason, "provider-unavailable");
            cJSON_Delete(found.fields);
            memset(&found, 0, sizeof found);
            memset(&session.release, 0, sizeof session.release);
            session.finished_ms = milliseconds() - session.started_ms;
            session.pspkg_fetch = session.busy = false;
            continue;
        }
        if (session.pspkg_download) {
            download_pspkg();
            continue;
        }
        Release resolved = session.release;
        if (atomic_load(&session.cancel)) {
            /* The owner may report failure before this worker is scheduled. */
        } else if (session.resolving)
            state_locked("validating", remote_page_pending(&resolved)
                ? "Reading the community catalogue download option…" : "Checking sign-in…");
        else if (session.native_dialog && !session.dialog_opened)
            state_locked("opening", "Opening the download page in Orbit…");
        else if (!strcmp(resolved.source, "akirabox") || !strcmp(resolved.source, "vikingfile"))
            state_locked("waiting-link", "Complete verification if shown, then wait. Orbit will check the generated download link automatically.");
        else
            state_locked("waiting", "Complete the provider’s steps, select Download, then return to Orbit.");
        pthread_mutex_unlock(&orbit.mutex);
        char error[256] = {0}, etag[256] = {0}, modified[128] = {0};
        bool validated = false;
        bool ok = false, routed = false;
        if (remote_page_pending(&resolved) || (!strcmp(resolved.source, "remote") && session.queued_job_id[0])) {
            CommunityResult result = resolve_community(&resolved, etag, modified, error, sizeof error);
            validated = ok = result == COMMUNITY_FOUND;
            routed = result == COMMUNITY_ROUTED;
        } else {
            bool missing = false;
            ok = active() && viking_resolve_download(&resolved, session.native_dialog, &session.cancel, &missing);
            if (ok) {
                ok = !validate_capture(&resolved, etag, modified, error, sizeof error);
                validated = ok;
            }
            pthread_mutex_lock(&orbit.mutex);
            session.resolving = false;
            if (missing) {
                remote_mark_missing_locked(&resolved);
                copy_text(error, sizeof error, REMOTE_DOWNLOAD_REMOVED);
            } else if (!ok) {
                resolved = session.release;
                error[0] = etag[0] = modified[0] = 0;
                if (!atomic_load(&session.cancel)) state_locked("opening", "Opening the download page on your PS5…");
            }
            if (ok) session.return_to_browser = false;
            pthread_mutex_unlock(&orbit.mutex);
            viking_download_context(&resolved, session.native_dialog);
            if (!ok && !missing && active())
                ok = capture(&resolved, NULL, etag, modified, &validated, error, sizeof error);
        }
        pthread_mutex_lock(&orbit.mutex);
        ok = ok && !session.stop && !atomic_load(&session.cancel) && selected_locked();
        if (ok) state_locked("validating", "Checking the selected file before adding it to Downloads…");
        pthread_mutex_unlock(&orbit.mutex);
        if (ok && !validated) ok = !validate_capture(&resolved, etag, modified, error, sizeof error);
        pthread_mutex_lock(&orbit.mutex);
        if (!routed && session.queued_job_id[0]) {
            finish_queued_locked(job_find(session.queued_job_id), &resolved, ok,
                                 etag, modified, error, sizeof error);
        } else if (routed) {
            /* The existing or newly queued job now owns the download. */
        } else if (session.stop || atomic_load(&session.cancel)) {
            if (session.native_failure[0]) state_locked("failed", session.native_failure);
            else state_locked("cancelled", "Verification cancelled. No download was added.");
        } else if (!selected_locked()) {
            state_locked("failed", "The source or catalogue option changed. Start verification again.");
        } else if (ok) {
            Job *job = session.refresh_job.id[0] ? job_find(session.refresh_job.id) : NULL;
            int code = session.refresh_job.id[0]
                ? job_refresh_link_locked(job, &resolved, &session.storage, etag, modified, error, sizeof error)
                : job_create_captured_locked(&resolved, &session.storage, etag, modified,
                                             error, sizeof error, &job);
            if (code == 200 || code == 201) {
                copy_text(session.job_id, sizeof session.job_id, job->id);
                state_locked("queued", "Your verified file has been added to Downloads.");
                session.queued_ms = milliseconds() - session.started_ms;
            } else state_locked("failed", error);
        } else state_locked("failed", error[0] ? error : "Verification stopped. Return to Orbit to try again.");
#ifndef ORBIT_DESKTOP
        /* Native closes only its own dialog after observing the committed job.
         * Relaunching an already-running native title does not close ShellUI's
         * browser overlay. Older native clients return manually. */
        if (session.return_to_browser && !strcmp(session.state, "queued") &&
            !session.stop && !orbit.stop && !atomic_load(&session.cancel)) {
            char home[64];
            snprintf(home, sizeof home, "http://127.0.0.1:%d/", ORBIT_HTTP_PORT);
            pthread_mutex_unlock(&orbit.mutex);
            int returned = probe_browser_open(home);
            pthread_mutex_lock(&orbit.mutex);
            if (returned)
                state_locked("queued", "Your verified file has been added to Downloads. Return to Orbit to view it.");
        }
#endif
        /* No pending verification survives a restart. No browser bytes are saved. */
        memset(&resolved, 0, sizeof resolved);
        memset(&session.release, 0, sizeof session.release);
        session.mirror_page[0] = 0;
        session.queued_job_id[0] = 0;
        memset(&session.refresh_job, 0, sizeof session.refresh_job);
        session.finished_ms = milliseconds() - session.started_ms;
        session.resolving = session.busy = false;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return NULL;
}
int browser_start(void) {
    pthread_mutex_lock(&orbit.mutex);
    /* Readiness only. No thread or native service until an explicit request. */
    session.ready = true;
    pthread_mutex_unlock(&orbit.mutex);
    return 0;
}
static int start_worker_locked(void) {
    if (session.thread_started) return 0;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc) rc = pthread_create(&session.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    session.thread_started = !rc;
    return rc;
}
static void begin_download_locked(const Release *r, const Storage *storage, const Job *refresh,
                                  bool native, const char *client, bool return_to_browser) {
    session.account = session.check_account = false;
    session.pspkg_fetch = session.pspkg_download = false;
    session.resolving = remote_page_pending(r) || !strcmp(r->source, "vikingfile");
    session.release = *r;
    if (storage) session.storage = *storage;
    session.native_dialog = native;
    session.dialog_opened = false;
    session.native_failure[0] = 0;
    copy_text(session.client_id, sizeof session.client_id, native ? client : "");
    session.return_to_browser = return_to_browser;
    memset(&session.refresh_job, 0, sizeof session.refresh_job);
    if (refresh) session.refresh_job = *refresh;
    copy_text(session.title, sizeof session.title, r->title);
    random_hex(session.id, 16);
    session.job_id[0] = 0;
    session.started_ms = milliseconds();
    session.capture_ms = session.link_ms = session.validation_ms = 0;
    session.queued_ms = session.finished_ms = 0;
    session.scanned_bytes = session.candidates = session.browser_processes = 0;
    reset_diagnostics_locked(r->source, refresh ? "link-refresh" : "download");
    session.busy = session.pending = true;
    atomic_store(&session.cancel, false);
    state_locked(session.resolving ? "validating" : "opening", remote_page_pending(r)
        ? "Reading the community catalogue download option…"
        : session.resolving ? "Checking sign-in…" : "Opening the download page on your PS5…");
    pthread_cond_signal(&session.changed);
}

int browser_pspkg_pages(const cJSON *input, char *error, size_t cap) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(input, "pages");
    int length = cJSON_GetArraySize(items);
    if (!cJSON_IsArray(items) || length < 1 || length > PSPKG_LINK_LIMIT) {
        copy_text(error, cap, "Select valid PSPKG links and provider pages.");
        return 400;
    }
    pthread_mutex_lock(&orbit.mutex);
    int code = 409;
    const Release *r = release_find(session.pspkg_release_id);
    if (!session.ready || session.stop || session.busy || !available() || !r ||
        strcmp(session.pspkg_release_id, json_text(input, "releaseId")) ||
        strcmp(session.id, json_text(input, "id")) ||
        time(NULL) >= session.pspkg_expires || !source_enabled_locked(r)) {
        copy_text(error, cap, "Fetch this game's PSPKG links again.");
        goto done;
    }
    PspkgPage pages[PSPKG_LINK_LIMIT];
    bool included[PSPKG_LINK_LIMIT] = {0};
    size_t count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        const char *id = json_text(item, "handoffId");
        const char *url = json_text(item, "providerUrl");
        const char *provider = remote_browser_provider(url);
        size_t index = session.pspkg_count;
        for (size_t i = 0; *id && i < session.pspkg_count; i++) {
            const PspkgLink *link = &session.pspkg_links[i];
            if (link->intermediate && !strcmp(link->link_id, id) && !strcmp(link->reason, "verify-required")) {
                if (index != session.pspkg_count) { index = session.pspkg_count; break; }
                index = i;
            }
        }
        if (!*provider || !strcmp(provider, "pspkg") || index == session.pspkg_count ||
            included[index] || strlen(url) >= sizeof pages[0].url || !remote_https_url(url)) {
            code = 400;
            copy_text(error, cap, "Select valid PSPKG links and provider pages.");
            goto done;
        }
        included[index] = true;
        pages[count].index = index;
        copy_text(pages[count].url, sizeof pages[count].url, url);
        count++;
    }
    if (start_worker_locked()) {
        code = 503;
        copy_text(error, cap, "PSPKG verification could not start. Try again.");
        goto done;
    }
    begin_download_locked(r, NULL, NULL, false, "", false);
    memcpy(session.pspkg_pages, pages, count * sizeof pages[0]);
    session.pspkg_page_count = count;
    session.pspkg_expires = time(NULL) + 300;
    copy_text(session.pspkg_status, sizeof session.pspkg_status, "resolving");
    state_locked("validating", "Checking the PSPKG provider links.");
    code = 202;
done:
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}
int browser_start_queued_locked(Job *j, char *error, size_t cap) {
    if (session.busy) return 0;
    if (!session.ready || session.stop || !available() || start_worker_locked()) {
        copy_text(error, cap, "Provider verification could not start. Try again.");
        return -1;
    }
    copy_text(session.queued_job_id, sizeof session.queued_job_id, j->id);
    begin_download_locked(&j->release, NULL, NULL, false, "", true);
    return 1;
}
void browser_stop(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool join = session.thread_started;
    session.stop = true;
    atomic_store(&session.cancel, true);
    pthread_cond_signal(&session.changed);
    pthread_mutex_unlock(&orbit.mutex);
    if (join) pthread_join(session.thread, NULL);
}
/* Caller holds orbit.mutex. Export only fixed categories and counters, never
 * the public page, captured route, file identity, user ID or raw error text. */
cJSON *browser_diagnostics_locked(void) {
    cJSON *o = cJSON_CreateObject();
    const char *state = "idle";
    const char *states[] = {"opening", "waiting", "waiting-link", "validating", "links-ready", "queued", "cancelled", "failed"};
    for (size_t i = 0; i < sizeof states / sizeof *states; i++)
        if (!strcmp(session.state, states[i])) state = states[i];
    cJSON_AddStringToObject(o, "state", state);
    cJSON_AddStringToObject(o, "provider", session.diagnostic_provider ? session.diagnostic_provider : "none");
    cJSON_AddStringToObject(o, "operation", session.diagnostic_operation ? session.diagnostic_operation : "none");
    cJSON_AddBoolToObject(o, "active", session.busy);
    cJSON_AddBoolToObject(o, "nativeDialog", session.native_dialog);
    cJSON_AddBoolToObject(o, "dialogOpened", session.dialog_opened);
    cJSON_AddStringToObject(o, "nativeFailure", session.native_failure_kind ? session.native_failure_kind : "none");
    cJSON_AddNumberToObject(o, "nativeError", session.native_error);
    cJSON_AddNumberToObject(o, "scannedBytes", (double)session.scanned_bytes);
    cJSON_AddNumberToObject(o, "browserProcesses", session.browser_processes);
    cJSON_AddNumberToObject(o, "candidates", session.candidates);
    cJSON_AddNumberToObject(o, "validations", session.validations);
    cJSON_AddNumberToObject(o, "validationFailures", session.validation_failures);
    cJSON_AddStringToObject(o, "lastValidation", !session.validations ? "none" :
        session.last_validation_failed ? "failed" : "passed");
    cJSON_AddNumberToObject(o, "validationMs", (double)session.validation_ms);
    return o;
}
cJSON *browser_status(void) {
    cJSON *o = cJSON_CreateObject();
    /* Foreground-user lookup shares the system-service lock with browser
     * launching, whose diagnostics acquire orbit.mutex. Preserve lock order. */
    const char *browser_account = viking_cached_state(false);
    const char *native_account = viking_cached_state(true);
    const char *pass_account = viking_pass_state();
    pthread_mutex_lock(&orbit.mutex);
    cJSON_AddBoolToObject(o, "available", session.ready && available() && !session.stop);
    cJSON_AddBoolToObject(o, "active", session.busy);
    cJSON_AddBoolToObject(o, "vikingAccountAvailable", session.ready && available() && !session.stop);
    cJSON_AddStringToObject(o, "vikingBrowserState", browser_account);
    cJSON_AddStringToObject(o, "vikingNativeState", native_account);
    cJSON_AddStringToObject(o, "vikingPass", pass_account);
    cJSON_AddBoolToObject(o, "accountPage", session.account);
    cJSON_AddBoolToObject(o, "nativeDialogAvailable", session.ready && available() && !session.stop);
    if (session.native_dialog) {
        cJSON_AddStringToObject(o, "clientId", session.client_id);
        cJSON_AddBoolToObject(o, "dialogOpened", session.dialog_opened);
        /* Only the catalogue's public page, never a captured/signed file URL. */
        if (session.busy && !session.resolving && !session.check_account && !session.dialog_opened && !atomic_load(&session.cancel))
            cJSON_AddStringToObject(o, "pageUrl", session.mirror_page[0] ? session.mirror_page : session.release.browser_url);
    }
    cJSON_AddStringToObject(o, "id", session.id);
    cJSON_AddStringToObject(o, "state", session.state[0] ? session.state : "idle");
    cJSON_AddStringToObject(o, "message", session.message);
    cJSON_AddStringToObject(o, "jobId", session.job_id);
    cJSON_AddStringToObject(o, "title", session.title);
    if (session.pspkg_release_id[0]) {
        cJSON_AddStringToObject(o, "pspkgReleaseId", session.pspkg_release_id);
        cJSON_AddStringToObject(o, "pspkgGameId", session.pspkg_game);
        bool expired = session.pspkg_expires && time(NULL) >= session.pspkg_expires;
        cJSON_AddStringToObject(o, "pspkgStatus", expired ? "expired" : session.pspkg_status);
        if (expired && session.pspkg_count) {
            memset(session.pspkg_links, 0, sizeof session.pspkg_links);
            session.pspkg_count = 0;
        }
        cJSON *choices = cJSON_AddArrayToObject(o, "pspkgLinks");
        for (size_t i = 0; !expired && choices && i < session.pspkg_count; i++) {
            const PspkgLink *link = &session.pspkg_links[i];
            if (!strcmp(link->reason, "missing-page") || !strcmp(link->reason, "provider-unavailable") ||
                !strcmp(link->reason, "archive-login-required") ||
                (link->archive_requires_login && !archive_account_signed_in())) continue;
            cJSON *choice = cJSON_CreateObject();
            if (!choice) break;
            char id[24];
            snprintf(id, sizeof id, "%zu", i + 1);
            cJSON_AddStringToObject(choice, "id", id);
            cJSON_AddNumberToObject(choice, "number", (double)i + 1);
            if (link->link_id[0]) cJSON_AddStringToObject(choice, "handoffId", link->link_id);
            cJSON_AddStringToObject(choice, "label", link->label[0] ? link->label : "PSPKG");
            cJSON_AddStringToObject(choice, "edition", link->edition);
            if (link->group[0]) cJSON_AddStringToObject(choice, "group", link->group);
            if (link->filename[0]) cJSON_AddStringToObject(choice, "filename", link->filename);
            if (link->size > 0) {
                cJSON_AddNumberToObject(choice, "sizeBytes", (double)link->size);
                cJSON_AddBoolToObject(choice, "sizeExact", link->size_exact);
            }
            if (link->archive_password[0]) cJSON_AddTrueToObject(choice, "hasArchivePassword");
            cJSON_AddStringToObject(choice, "titleId", link->title_id);
            char version[sizeof link->version];
            pspkg_display_version(link->edition, link->filename, version, sizeof version);
            cJSON_AddStringToObject(choice, "version", version[0] ? version : link->version);
            cJSON_AddStringToObject(choice, "region", link->region);
            cJSON_AddStringToObject(choice, "format", link->format);
            cJSON_AddStringToObject(choice, "compression", link->compression);
            cJSON_AddStringToObject(choice, "reason", link->reason);
            if (link->provider[0]) {
                Release route = {.remote_page = true};
                copy_text(route.remote_provider_page, sizeof route.remote_provider_page, link->provider);
                char host[254];
                if (remote_link_host(&route, host, sizeof host)) cJSON_AddStringToObject(choice, "providerHost", host);
                bool cached = link->torbox_cached_revision &&
                    link->torbox_cached_revision == debrid_cache_revision_locked(&route);
                bool allowed = !cached || debrid_size_allowed(link->torbox_size);
                cJSON_AddBoolToObject(choice, "torboxAvailable",
                                      allowed && !link->archive_requires_login && debrid_link_covered_locked(&route));
                if (cached && allowed) {
                    cJSON_AddTrueToObject(choice, "torboxCached");
                    if (link->torbox_size) cJSON_AddNumberToObject(choice, "torboxSizeBytes", (double)link->torbox_size);
                }
            }
            cJSON_AddItemToArray(choices, choice);
        }
    }
    /* Durations/counters only. Never disclose browser bytes, file URLs, process
     * addresses, cookies or verification data through diagnostics. */
    if (session.started_ms) {
        cJSON *timing = cJSON_AddObjectToObject(o, "timing");
        cJSON_AddNumberToObject(timing, "elapsedMs", session.busy ? milliseconds() - session.started_ms : session.finished_ms);
        cJSON_AddNumberToObject(timing, "captureStartedMs", session.capture_ms);
        cJSON_AddNumberToObject(timing, "linkFoundMs", session.link_ms);
        cJSON_AddNumberToObject(timing, "validationMs", session.validation_ms);
        cJSON_AddNumberToObject(timing, "queuedMs", session.queued_ms);
        cJSON_AddNumberToObject(timing, "scannedBytes", (double)session.scanned_bytes);
        cJSON_AddNumberToObject(timing, "candidates", session.candidates);
        cJSON_AddNumberToObject(timing, "browserProcesses", session.browser_processes);
    }
    pthread_mutex_unlock(&orbit.mutex);
    return o;
}
static int perform_action(const cJSON *input, const char *action, char *error, size_t cap) {
    int code = 202;
    pthread_mutex_lock(&orbit.mutex);
    if (!strcmp(action, "opened")) {
        if (!session.busy || !session.native_dialog || atomic_load(&session.cancel) ||
            strcmp(json_text(input, "id"), session.id) ||
            strcmp(json_text(input, "clientId"), session.client_id)) {
            copy_text(error, cap, "This verification session is no longer active."); code = 409;
        } else {
            session.dialog_opened = true; /* idempotent acknowledgement */
            if (!strcmp(session.state, "opening")) {
                if (session.account) state_locked("waiting", "Sign in on Vikingfile, then return to Orbit.");
                else state_locked("waiting-link", "Complete verification if shown, then wait for Orbit to check the download link.");
            }
        }
    } else if (!strcmp(action, "cancel")) {
        const cJSON *failure = cJSON_GetObjectItemCaseSensitive(input, "nativeFailure");
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(input, "errorCode");
        const char *message = failure && cJSON_IsString(failure)
            ? native_failure_message(failure->valuestring) : NULL;
        if ((failure && !message) || (number && (!failure || !cJSON_IsNumber(number) ||
            !(number->valuedouble >= INT32_MIN && number->valuedouble <= 0) ||
            number->valuedouble != (double)(int32_t)number->valuedouble))) {
            copy_text(error, cap, "Choose a supported browser failure."); code = 400;
            goto done;
        }
        if (!strcmp(session.state, "queued")) {
            copy_text(error, cap, "The file is already in Downloads. Manage it there."); code = 409;
        } else if (!session.busy || strcmp(json_text(input, "id"), session.id)) {
            copy_text(error, cap, "This verification session is no longer active."); code = 409;
        } else if (failure && (!session.native_dialog ||
                   strcmp(json_text(input, "clientId"), session.client_id))) {
            copy_text(error, cap, "This verification session belongs to another browser."); code = 409;
        } else {
            if (failure && !atomic_load(&session.cancel)) {
                copy_text(session.native_failure, sizeof session.native_failure, message);
                session.native_failure_kind = !strcmp(failure->valuestring, "open") ? "open" :
                    !strcmp(failure->valuestring, "browser") ? "browser" :
                    !strcmp(failure->valuestring, "timeout") ? "timeout" : "connection";
                session.native_error = number ? (int32_t)number->valuedouble : 0;
                if (number && number->valuedouble < 0) {
                    size_t used = strlen(session.native_failure);
                    snprintf(session.native_failure + used, sizeof session.native_failure - used,
                             " Browser error: 0x%08X.", (unsigned)(int32_t)number->valuedouble);
                }
            }
            atomic_store(&session.cancel, true);
            if (session.native_failure[0]) state_locked("failed", session.native_failure);
            else state_locked("cancelled", "Verification cancelled. No download was added.");
        }
    } else if (strcmp(action, "start") && strcmp(action, "refresh") &&
               strcmp(action, "pspkg-links") && strcmp(action, "pspkg-download") &&
               strcmp(action, "viking-account") && strcmp(action, "viking-check")) {
        copy_text(error, cap, "Unknown browser action."); code = 400;
    } else if (!session.ready || session.stop || !available()) {
        copy_text(error, cap, "Provider verification is available on the PS5 only."); code = 409;
    } else if (session.busy) {
        copy_text(error, cap, "Finish or cancel the current verification first."); code = 409;
    } else {
        const cJSON *target = cJSON_GetObjectItemCaseSensitive(input, "returnTarget");
        const cJSON *legacy = cJSON_GetObjectItemCaseSensitive(input, "returnToOrbit");
        if ((target && (!cJSON_IsString(target) ||
                       (strcmp(target->valuestring, "native") && strcmp(target->valuestring, "browser")))) ||
            (legacy && !cJSON_IsBool(legacy)) || (target && legacy)) {
            copy_text(error, cap, "Choose a supported Orbit return target."); code = 400;
            goto done;
        }
        const cJSON *dialog = cJSON_GetObjectItemCaseSensitive(input, "nativeDialog");
        const char *client = json_text(input, "clientId");
        if ((dialog && !cJSON_IsBool(dialog)) || (cJSON_IsTrue(dialog) &&
            (!target || strcmp(target->valuestring, "native") || !*client || strlen(client) > 64 ||
             strspn(client, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != strlen(client)))) {
            copy_text(error, cap, "Choose a supported native browser session."); code = 400;
            goto done;
        }
        if (!strcmp(action, "viking-account") || !strcmp(action, "viking-check")) {
            /* A fixed official URL, never a caller-supplied URL or account hash. */
            if (start_worker_locked()) {
                copy_text(error, cap, "Provider verification could not start. Try again."); code = 503;
                goto done;
            }
            memset(&session.release, 0, sizeof session.release);
            memset(&session.refresh_job, 0, sizeof session.refresh_job);
            copy_text(session.release.browser_url, sizeof session.release.browser_url, "https://vikingfile.com/login");
            session.account = true;
            session.check_account = !strcmp(action, "viking-check");
            session.resolving = false;
            session.native_dialog = cJSON_IsTrue(dialog);
            session.dialog_opened = false;
            session.return_to_browser = false;
            session.native_failure[0] = session.job_id[0] = 0;
            copy_text(session.client_id, sizeof session.client_id, session.native_dialog ? client : "");
            copy_text(session.title, sizeof session.title, "Vikingfile");
            random_hex(session.id, 16);
            session.started_ms = milliseconds();
            session.capture_ms = session.link_ms = session.validation_ms = session.queued_ms = session.finished_ms = 0;
            session.scanned_bytes = session.candidates = session.browser_processes = 0;
            reset_diagnostics_locked("vikingfile", session.check_account ? "account-check" : "sign-in");
            session.busy = session.pending = true;
            atomic_store(&session.cancel, false);
            state_locked(session.check_account ? "validating" : "opening", session.check_account ? "Checking sign-in…" : "Opening Vikingfile sign-in…");
            pthread_cond_signal(&session.changed);
            code = 202;
            goto done;
        }
        if (!strcmp(action, "pspkg-links") || !strcmp(action, "pspkg-download")) {
            bool download = !strcmp(action, "pspkg-download");
            Release *r = release_find(json_text(input, "releaseId"));
            char platform[4], game_id[13];
            if (!r || strcmp(r->source, "remote") ||
                (strcmp(r->remote_catalog_id, PSPKG_PS5_SOURCE) &&
                 strcmp(r->remote_catalog_id, PSPKG_PS4_SOURCE)) ||
                !pspkg_page_identity(r->browser_url, platform, sizeof platform,
                                     game_id, sizeof game_id)) {
                copy_text(error, cap, "Select a general PSPKG PS4 or PS5 game."); code = 400;
                goto done;
            }
            if (!source_enabled_locked(r)) {
                copy_text(error, cap, "This download option is no longer available."); code = 409;
                goto done;
            }
            Storage drives[ORBIT_MAX_STORAGE], *storage = NULL;
            PspkgLink *choice = NULL;
            if (download) {
                size_t n = storage_list(drives);
                for (size_t i = 0; i < n; i++)
                    if (!strcmp(drives[i].id, json_text(input, "storageId"))) storage = &drives[i];
                if (strcmp(session.pspkg_release_id, r->id) ||
                    time(NULL) >= session.pspkg_expires) {
                    copy_text(error, cap, "Fetch this game's PSPKG links again."); code = 409;
                    goto done;
                }
                for (size_t i = 0; i < session.pspkg_count; i++) {
                    char id[24];
                    snprintf(id, sizeof id, "%zu", i + 1);
                    if (!strcmp(id, json_text(input, "linkId")))
                        choice = &session.pspkg_links[i];
                }
                if (!choice || !storage || (choice->archive_requires_login && !archive_account_signed_in()) ||
                    (choice->reason[0] && strcmp(choice->reason, "verify-required"))) {
                    copy_text(error, cap, "Select a PSPKG link and connected drive."); code = 400;
                    goto done;
                }
            }
            if (start_worker_locked()) {
                copy_text(error, cap, "PSPKG verification could not start. Try again."); code = 503;
                goto done;
            }
            PspkgLink selected = choice ? *choice : (PspkgLink){0};
            begin_download_locked(r, storage, NULL, false, "", true);
            session.pspkg_fetch = !download;
            session.pspkg_download = download;
            session.pspkg_selected = selected;
            session.pspkg_selected_index = choice ? (size_t)(choice - session.pspkg_links) : 0;
            session.pspkg_torbox = !selected.archive_requires_login && !strcmp(json_text(input, "delivery"), "torbox");
            if (!download) {
                const char *game = json_text(input, "gameId");
                if (!*game || strlen(game) >= sizeof session.pspkg_game ||
                    strspn(game, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(game))
                    game = r->game_id;
                if (strcmp(session.pspkg_release_id, r->id)) {
                    memset(session.pspkg_links, 0, sizeof session.pspkg_links);
                    session.pspkg_count = 0;
                    session.pspkg_expires = 0;
                }
                copy_text(session.pspkg_status, sizeof session.pspkg_status, "verify-required");
                copy_text(session.pspkg_release_id, sizeof session.pspkg_release_id, r->id);
                copy_text(session.pspkg_game, sizeof session.pspkg_game, game);
                state_locked("opening", "Opening this game's PSPKG download page…");
            } else state_locked("opening", "Opening the selected PSPKG link…");
            code = 202;
            goto done;
        }
        bool refreshing = !strcmp(action, "refresh");
        Job *job = refreshing ? job_find(json_text(input, "jobId")) : NULL;
        Release *r = refreshing ? (job ? &job->release : NULL) : release_find(json_text(input, "releaseId"));
        Storage drives[ORBIT_MAX_STORAGE], *s = NULL;
        if (refreshing && job) (void)storage_rebind_locked(job);
        size_t count = storage_list(drives);
        for (size_t i = 0; i < count; i++)
            if (!strcmp(drives[i].id, refreshing && job ? job->storage_id : json_text(input, "storageId"))) s = &drives[i];
        if (refreshing && (!job_can_refresh_link(job) || !source_enabled_locked(&job->release))) {
            copy_text(error, cap, "Pause the download and enable its source before refreshing its link."); code = 409;
        } else if (!r || (!remote_page_pending(r) && !provider_browser_supported(r))) {
            copy_text(error, cap, "This option has no supported provider browser page."); code = 400;
        } else if (!source_enabled_locked(r)) {
            copy_text(error, cap, "This download option is no longer available."); code = 409;
        } else if (!s) {
            copy_text(error, cap, "Reconnect the selected drive first."); code = 409;
        } else if (refreshing && (strcmp(s->root, job->root) || s->device != job->device || s->inode != job->inode)) {
            copy_text(error, cap, "Reconnect the original drive first."); code = 409;
        } else if ((code = refreshing || remote_page_pending(r) ? 200 : job_check_capture_locked(r, s, error, cap)) == 200) {
            if (start_worker_locked()) {
                copy_text(error, cap, "Provider verification could not start. Try again.");
                state_locked("failed", error);
                code = 503;
                goto done;
            }
            begin_download_locked(r, s, job, cJSON_IsTrue(dialog), client,
                (target && !strcmp(target->valuestring, "browser")) || cJSON_IsTrue(legacy));
            code = 202;
        }
    }
done:
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}

int browser_action(const cJSON *input, char *error, size_t cap) {
    return perform_action(input, json_text(input, "action"), error, cap);
}

void browser_job_changed_locked(const char *id) {
    if (session.busy && session.refresh_job.id[0] && !strcmp(session.refresh_job.id, id))
        atomic_store(&session.cancel, true);
    if (session.busy && session.queued_job_id[0] && !strcmp(session.queued_job_id, id)) {
        const Job *j = job_find(id);
        if (j && (j->pause || j->cancel)) atomic_store(&session.cancel, true);
    }
}
