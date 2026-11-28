#include "orbit.h"
#include "links.h"
#include "debrid.h"
#include "extract.h"
#include "remote_sources.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
/* Case-insensitive comparisons also protect exFAT destinations. */
bool job_paths_overlap(const Job *a, const Job *b) {
    if (!a->filename[0] || !b->filename[0] || a->device != b->device ||
        a->inode != b->inode || strcmp(a->root, b->root)) return false;
    char ap[300], bp[300];
    snprintf(ap, sizeof ap, "%s.part", a->filename);
    snprintf(bp, sizeof bp, "%s.part", b->filename);
    return !strcasecmp(a->filename, b->filename) || !strcasecmp(ap, b->filename) ||
           !strcasecmp(a->filename, bp);
}
bool job_waiting(const Job *j) {
    return !j->retry_worker && (!strcmp(j->status, "queued") || !strcmp(j->status, "paused") ||
           !strcmp(j->status, "retrying"));
}
bool job_active(const Job *j) {
    return j->retry_worker || !strcmp(j->status, "downloading") || !strcmp(j->status, "verifying") ||
           !strcmp(j->status, "extracting") || !strcmp(j->status, "inspecting") || !strcmp(j->status, "waiting-extraction");
}
/* A new download needs room for its file only. An archive's files are checked just before
 * it is unpacked (unpack_room() in transfer.c), so games can be queued before that room exists. */
uint64_t job_space_needed(const Release *r) {
    return r->size > 0 ? (uint64_t)r->size : 0;
}
/* What unpacking a downloaded archive writes, once its size is known. */
static uint64_t unpack_size(const Job *j) {
    int64_t expected = j->inspected_size > 0 ? j->inspected_size : j->release.extracted_size;
    return expected > 0 ? (uint64_t)expected : 0;
}
static uint64_t download_remaining(const Job *j) {
    if (j->total <= 0 || j->received >= j->total) return 0;
    uint64_t present = j->received > 0 ? (uint64_t)j->received : 0;
    if (j->segmented) {
        /* exFAT may allocate the gap before a tail write. Those blocks already
         * reduce free space, but they must NEVER count as downloaded progress. */
        int dir = storage_open(j);
        if (dir >= 0) {
            char partial[192];
            snprintf(partial, sizeof partial, "%s.part", j->filename);
            struct stat st;
            if (!fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW) &&
                S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_blocks > 0) {
                uint64_t allocated = (uint64_t)st.st_blocks * 512;
                if (allocated > present) present = allocated;
            }
            close(dir);
        }
    }
    return present >= (uint64_t)j->total ? 0 : (uint64_t)j->total - present;
}
/* An archive's files count only while it is being unpacked: before that, unpacking checks
 * for its own room. */
static uint64_t remaining(const Job *j) {
    uint64_t left = download_remaining(j);
    if (!j->release.compression[0] || j->keep_packed ||
        (strcmp(j->status, "extracting") && strcmp(j->status, "inspecting"))) return left;
    uint64_t total = unpack_size(j);
    uint64_t staged = j->extracted > 0 ? (uint64_t)j->extracted : 0;
    uint64_t files = staged >= total ? 0 : total - staged;
    return UINT64_MAX - left < files ? UINT64_MAX : left + files;
}
uint64_t storage_pending_locked(const Storage *s, const Job *exclude) {
    uint64_t pending = 0;
    for (size_t i = 0; i < orbit.job_count; i++) {
        const Job *j = &orbit.jobs[i];
        if (j == exclude || !j->id[0] || !strcmp(j->status, "complete") ||
            !strcmp(j->status, "cancelled") || strcmp(j->storage_id, s->id) ||
            strcmp(j->root, s->root) || j->device != s->device || j->inode != s->inode)
            continue;
        uint64_t left = remaining(j);
        if (UINT64_MAX - pending < left)
            return UINT64_MAX;
        pending += left;
    }
    return pending;
}
/* ---- a storage operation running alongside downloads ---- */
/* Names first, before the operation checks what downloads use: a download
 * created after this sees them, and one created before is in that check. */
void storage_hold_paths(int which, const char *path, const char *source) {
    pthread_mutex_lock(&orbit.mutex);
    StorageHold *h = &orbit.holds[which];
    copy_text(h->path, sizeof h->path, path ? path : "");
    copy_text(h->source, sizeof h->source, source ? source : "");
    pthread_mutex_unlock(&orbit.mutex);
}
/* Space once its size is known and checked against unfinished downloads. */
void storage_hold_space(int which, dev_t device, uint64_t bytes) {
    pthread_mutex_lock(&orbit.mutex);
    orbit.holds[which].device = device;
    orbit.holds[which].bytes = bytes;
    pthread_mutex_unlock(&orbit.mutex);
}
void storage_hold_left(int which, uint64_t bytes) {
    pthread_mutex_lock(&orbit.mutex);
    orbit.holds[which].bytes = bytes;
    pthread_mutex_unlock(&orbit.mutex);
}
void storage_hold_clear(int which) {
    pthread_mutex_lock(&orbit.mutex);
    memset(&orbit.holds[which], 0, sizeof orbit.holds[which]);
    pthread_mutex_unlock(&orbit.mutex);
}
uint64_t storage_held_locked(const Storage *s) {
    uint64_t held = 0;
    for (int i = 0; i < HOLD_COUNT; i++) {
        const StorageHold *h = &orbit.holds[i];
        if (h->bytes && h->device == s->device)
            held = UINT64_MAX - held < h->bytes ? UINT64_MAX : held + h->bytes;
    }
    return held;
}
/* One path inside the other, or the same; exFAT drives ignore case. */
static bool related(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (!na || !nb) return false;
    if (na == nb) return !strcasecmp(a, b);
    return na < nb ? !strncasecmp(a, b, na) && b[na] == '/' : !strncasecmp(a, b, nb) && a[nb] == '/';
}
/* The names a download writes: its file, its partial and what it unpacks to. */
static size_t job_names(const Job *j, char names[3][1100]) {
    if (!j->filename[0]) return 0;
    snprintf(names[0], 1100, "%s/homebrew/%s", j->root, j->filename);
    snprintf(names[1], 1100, "%s/homebrew/%s.part", j->root, j->filename);
    if (!j->output[0]) return 2;
    snprintf(names[2], 1100, "%s/homebrew/%s", j->root, j->output);
    return 3;
}
bool storage_hold_conflict_locked(const Job *j) {
    char names[3][1100];
    size_t n = job_names(j, names);
    for (int i = 0; i < HOLD_COUNT; i++)
        for (size_t k = 0; k < n; k++)
            if (related(names[k], orbit.holds[i].path) || related(names[k], orbit.holds[i].source))
                return true;
    return false;
}
/* An unfinished download writes this path or something inside it. */
bool storage_path_downloading_locked(const char *path) {
    for (size_t i = 0; i < orbit.job_count; i++) {
        const Job *j = &orbit.jobs[i];
        if (!j->id[0] || !strcmp(j->status, "complete") || !strcmp(j->status, "cancelled")) continue;
        char names[3][1100];
        size_t n = job_names(j, names);
        for (size_t k = 0; k < n; k++)
            if (related(names[k], path)) return true;
    }
    return false;
}
static bool fits(const Storage *s, uint64_t size, const Job *exclude) {
    uint64_t pending = storage_pending_locked(s, exclude), held = storage_held_locked(s), margin = 16 * 1024 * 1024;
    pending = UINT64_MAX - pending < held ? UINT64_MAX : pending + held;
    return pending <= s->free_bytes && size <= s->free_bytes - pending &&
           margin <= s->free_bytes - pending - size;
}

/* A queued community page only claims a path once the selected file is known. */
bool job_assign_remote_file_locked(Job *j, const Release *file, char *err, size_t cap) {
    if (j->cancel || j->pause || orbit.stop || !source_enabled_locked(&j->release)) return false;
    Storage drives[ORBIT_MAX_STORAGE], *drive = NULL;
    size_t count = storage_list(drives);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(drives[i].id, j->storage_id) && !strcmp(drives[i].root, j->root) &&
            drives[i].device == j->device && drives[i].inode == j->inode) { drive = &drives[i]; break; }
    if (!drive || !fits(drive, (uint64_t)file->size, j)) {
        copy_text(err, cap, drive ? "Not enough free space to finish this download."
                                  : "Destination is missing or not writable.");
        return false;
    }
    Job destination = {.device = j->device, .inode = j->inode};
    copy_text(destination.root, sizeof destination.root, j->root);
    copy_text(destination.filename, sizeof destination.filename, file->filename);
    bool conflict = storage_hold_conflict_locked(&destination);
    for (size_t i = 0; i < orbit.job_count && !conflict; i++) {
        const Job *other = &orbit.jobs[i];
        conflict = other != j && other->id[0] && strcmp(other->status, "complete") &&
            strcmp(other->status, "cancelled") && job_paths_overlap(other, &destination);
    }
    if (conflict) {
        copy_text(err, cap, "Another operation uses this filename on that drive.");
        return false;
    }
    int dir = storage_open(j);
    if (dir < 0) {
        copy_text(err, cap, "Destination is missing or not writable.");
        return false;
    }
    struct stat st;
    char partial[sizeof file->filename + 5];
    snprintf(partial, sizeof partial, "%s.part", file->filename);
    bool absent = fstatat(dir, file->filename, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT &&
                  fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT;
    close(dir);
    if (!absent) {
        copy_text(err, cap, "A destination or partial file already exists. Orbit will not overwrite it.");
        return false;
    }
    Release previous = j->release;
    bool packed = j->keep_packed;
    j->release = *file;
    j->total = file->size;
    j->keep_packed = packed && file->compression[0];
    copy_text(j->filename, sizeof j->filename, file->filename);
    if (state_save_locked() == 0) return true;
    j->release = previous;
    j->total = previous.size;
    j->keep_packed = packed;
    copy_text(j->filename, sizeof j->filename, previous.filename);
    copy_text(err, cap, "Could not persist source identity.");
    return false;
}
/* Never orphan a kept partial file. Removing a history record deletes no disk files. */
static bool removable(const Job *j) {
    if (j->history_removing) return false;
    if (!strcmp(j->status, "complete"))
        return true;
    if (strcmp(j->status, "cancelled"))
        return false;
    if (!j->filename[0]) return true;
    int dir = storage_open(j);
    if (dir < 0)
        return false;
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    struct stat st;
    int rc = fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW), saved = errno;
    close(dir);
    return rc && saved == ENOENT;
}
/* Get everything: whether bytes more fit on this drive after unfinished downloads. */
bool job_space_fits_locked(const Storage *s, uint64_t bytes) { return fits(s, bytes, NULL); }
/* Unpacking: room for the archive's files beside what other downloads still need. free_bytes
 * is measured on the drive itself. */
bool job_unpack_fits_locked(const Job *j, uint64_t free_bytes, uint64_t bytes) {
    Storage s = {.device = j->device, .inode = j->inode, .free_bytes = free_bytes};
    copy_text(s.id, sizeof s.id, j->storage_id);
    copy_text(s.root, sizeof s.root, j->root);
    return fits(&s, bytes, j);
}
/* Cancelled with its partial file kept on the drive it was cancelled on: Downloads offers
 * Resume, and a new download of the same file there is refused. One on a drive that has
 * since changed identity makes way for a new download (as job_create does). */
bool job_resumable_cancel_locked(const Job *j) {
    if (strcmp(j->status, "cancelled") || removable(j)) return false;
    Storage drives[ORBIT_MAX_STORAGE];
    size_t count = storage_list(drives);
    for (size_t i = 0; i < count; i++)
        if (!strcmp(drives[i].id, j->storage_id))
            return !strcmp(j->root, drives[i].root) && j->device == drives[i].device && j->inode == drives[i].inode;
    return false;
}
/* Call only after the writer stops. An absent file is already cleaned up;
 * other errors must not discard the saved resume map, even at zero progress. */
int job_delete_partial_locked(Job *j, char *err, size_t cap) {
    if (!j->filename[0]) return 0;
    (void)storage_rebind_locked(j);
    int dir = storage_open(j);
    if (dir < 0) {
        copy_text(err, cap, "Reconnect the original drive before deleting the partial file.");
        return -1;
    }
    char partial[192], path[540];
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    snprintf(path, sizeof path, "%s/homebrew", j->root);
    int rc = unlinkat(dir, partial, 0), saved = errno;
    /* An interrupted extraction leaves only Orbit's own staging folder. */
    int staged = j->release.compression[0] ? extract_discard(dir, path, j->id) : 0;
    close(dir);
    if ((rc && saved != ENOENT) || staged) {
        copy_text(err, cap, "Could not delete the partial file.");
        return -1;
    }
    j->archive_ready = false;
    j->archive_locked = false;
    j->inspected_size = 0;
    j->inspected_format[0] = 0;
    j->extracted = 0;
    j->received = 0;
    j->segmented = false;
    j->range_count = 0;
    memset(j->range_received, 0, sizeof j->range_received);
    j->etag[0] = 0;
    j->modified[0] = 0;
    j->error[0] = 0;
    return 0;
}
int history_clear_locked(const cJSON *input, char *err, size_t cap) {
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(input, "status");
    const char *status = json_text(input, "status");
    if (filter && (!cJSON_IsString(filter) ||
                   (strcmp(status, "complete") && strcmp(status, "cancelled")))) {
        copy_text(err, cap, "History status must be complete or cancelled.");
        return 400;
    }
    if (orbit.stop) {
        copy_text(err, cap, "Orbit is stopping.");
        return 503;
    }
    Job *backup = malloc(sizeof orbit.jobs);
    if (!backup) {
        copy_text(err, cap, "Could not clear history.");
        return 503;
    }
    memcpy(backup, orbit.jobs, sizeof orbit.jobs);
    for (size_t i = 0; i < orbit.job_count; i++)
        if (orbit.jobs[i].id[0] && (!filter || !strcmp(orbit.jobs[i].status, status)) &&
            removable(&orbit.jobs[i]))
            memset(&orbit.jobs[i], 0, sizeof(Job));
    int rc = state_save_locked();
    if (rc) {
        /* Only cleared, inactive slots changed. Never overwrite an active job's
         * atomics while its network callbacks are reading them. */
        for (size_t i = 0; i < orbit.job_count; i++)
            if (!orbit.jobs[i].id[0] && backup[i].id[0]) orbit.jobs[i] = backup[i];
        copy_text(err, cap, "Could not save history changes.");
    }
    free(backup);
    return rc ? 503 : 200;
}
static int create_locked(const Release *r, const char *sid, const Storage *pin,
                         const char *etag, const char *modified, bool dry_run, const char *delivery,
                         const Release *recovery, char *err, size_t cap, Job **result) {
    const char *rid = r ? r->id : "";
    if (orbit.stop) {
        copy_text(err, cap, "Orbit is stopping. Reconnect after restarting it.");
        return 503;
    }
    if (!r) {
        copy_text(err, cap, "Unknown curated release.");
        return 404;
    }
    bool torbox = delivery && !strcmp(delivery, "torbox");
    bool pending_file = remote_page_pending(r);
    if (!source_enabled_locked(r)) {
        copy_text(err, cap,
                  "This download option is no longer available.");
        return 409;
    }
    Job delivery_job = {0};
    if (delivery && *delivery && strcmp(delivery, "direct") && !torbox) {
        copy_text(err, cap, "Choose direct or torbox delivery."); return 400;
    }
    if (torbox) {
        const Release *route = recovery ? recovery : r;
        Release mirror;
        if (!recovery && (remote_link_missing_locked(r) || !debrid_link_covered_locked(r))) {
            char after[sizeof mirror.id] = "";
            while (remote_next_mirror_locked(r, after, &mirror)) {
                if (!remote_link_missing_locked(&mirror) && debrid_link_covered_locked(&mirror)) {
                    route = &mirror;
                    copy_text(delivery_job.mirror_id, sizeof delivery_job.mirror_id, mirror.id);
                    copy_text(delivery_job.mirror_first, sizeof delivery_job.mirror_first, mirror.id);
                    delivery_job.mirror_torbox = true;
                    break;
                }
                copy_text(after, sizeof after, mirror.id);
            }
        }
        if ((!recovery && remote_link_missing_locked(route)) ||
            debrid_prepare_locked(route, &delivery_job, err, cap) != 200) {
            if (recovery) return 409;
            torbox = false;
            delivery_job.mirror_id[0] = delivery_job.mirror_first[0] = 0;
            delivery_job.mirror_torbox = false;
            err[0] = 0;
        }
    }
    if (recovery) {
        delivery_job.direct_missing = true;
        if (strcmp(r->id, recovery->id)) {
            copy_text(delivery_job.mirror_id, sizeof delivery_job.mirror_id, recovery->id);
            delivery_job.mirror_torbox = true;
        }
    }
    delivery_job.browser_pending = !torbox && !pin && job_needs_browser(r);
    if (!torbox && !delivery_job.browser_pending && !provider_supported(r) &&
        !(dry_run && provider_browser_supported(r))) {
        copy_text(err, cap, provider_browser_supported(r)
            ? (!strcmp(r->source, "akirabox") || !strcmp(r->source, "vikingfile")
                ? "Complete verification if shown, then wait. Orbit will check the generated download link automatically."
                : "Open this option's download page on PS5 first, select Download, then return to Orbit.")
            : "This provider does not support direct console downloads yet.");
        return 409;
    }
#ifndef ORBIT_TEST
    if (orbit.desktop) {
        copy_text(err, cap, "Desktop preview cannot download games. Connect to Orbit on your PS5.");
        return 409;
    }
#endif
    if (orbit.state_failed) {
        copy_text(err, cap, "Queue state cannot be saved. Check console storage.");
        return 503;
    }
    Storage a[ORBIT_MAX_STORAGE], *s = NULL;
    size_t n = storage_list(a);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(a[i].id, sid)) {
            s = &a[i];
            break;
        }
    if (!s) {
        copy_text(err, cap, "Selected storage is not connected or writable.");
        return 409;
    }
    if (pin && (strcmp(s->root, pin->root) || s->device != pin->device || s->inode != pin->inode)) {
        copy_text(err, cap, "The selected drive changed. Start verification again.");
        return 409;
    }
    Job destination = {.device = s->device, .inode = s->inode};
    copy_text(destination.root, sizeof destination.root, s->root);
    copy_text(destination.filename, sizeof destination.filename, r->filename);
    if (storage_hold_conflict_locked(&destination)) {
        copy_text(err, cap, "Another file operation is using this.");
        return 409;
    }
    long finished = -1, unused = -1;
    unsigned last_order = 0;
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (!j->id[0]) {
            if (unused < 0)
                unused = (long)i;
            continue;
        }
        if (j->order > last_order)
            last_order = j->order;
        if (strcmp(j->release_id, rid) && strcmp(j->status, "complete") &&
            strcmp(j->status, "cancelled") && job_paths_overlap(j, &destination)) {
            copy_text(err, cap, "Another download uses this filename on that drive. Open Downloads to manage it.");
            return 409;
        }
        if (!strcmp(j->release_id, rid) && !strcmp(j->storage_id, sid)) {
            /* Reconnecting a drive can change its identity. A cancelled job on
             * an unverified old drive must not block a NEW download to the
             * selected drive. Retain that record (and any original partial),
             * never rebind its resume map or delete files on the new drive.
             * The destination checks below still require both names absent. */
            if (!strcmp(j->status, "cancelled") &&
                (strcmp(j->root, s->root) || j->device != s->device || j->inode != s->inode))
                continue;
            /* Completed jobs and cancellations without a kept partial are history.
               Verify the original drive before reusing a cancelled slot, even when
               received is zero. The file check below still refuses to overwrite. */
            if (removable(j)) {
                finished = (long)i;
                continue;
            }
            copy_text(err, cap,
                      !strcmp(j->status, "cancelled")
                          ? "Open Downloads to resume this cancelled download, or reconnect its original drive and delete the partial file before downloading again."
                          : !strcmp(r->source, "link") ? "This link is already in Downloads for that drive."
                          : "This release already has a job on that drive. Open Downloads to resume or retry it.");
            return 409;
        }
    }
    if (finished < 0)
        finished = unused;
    if (finished < 0 && orbit.job_count >= ORBIT_MAX_JOBS) {
        copy_text(err, cap, "Queue is full. Clear finished or cancelled history to make room.");
        return 409;
    }
    const bool packed = (pending_file || r->compression[0]) && !orbit.extract_archives;
    if (!fits(s, job_space_needed(r), NULL)) {
        copy_text(
            err, cap,
            "Not enough space after unfinished downloads. Free space or cancel a queued download.");
        return 409;
    }
    Job j = delivery_job;
    j.release = *r;
    j.keep_packed = packed;
    copy_text(j.etag, sizeof j.etag, etag ? etag : "");
    copy_text(j.modified, sizeof j.modified, modified ? modified : "");
    j.order = last_order + 1;
    random_hex(j.id, 8);
    copy_text(j.release_id, sizeof j.release_id, rid);
    copy_text(j.storage_id, sizeof j.storage_id, sid);
    copy_text(j.root, sizeof j.root, s->root);
    copy_text(j.filename, sizeof j.filename, r->filename);
    copy_text(j.status, sizeof j.status, "queued");
    j.device = s->device;
    j.inode = s->inode;
    j.total = r->size;
    int dir = storage_open(&j);
    if (dir < 0) {
        copy_text(err, cap, "Cannot open the selected homebrew folder.");
        return 409;
    }
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j.filename);
    struct stat st;
    bool absent = !j.filename[0] || (fstatat(dir, j.filename, &st, AT_SYMLINK_NOFOLLOW) < 0 && errno == ENOENT);
    bool leftover = false;
    if (j.filename[0] && absent && fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        /* A download that failed before its first byte (TorBox couldn't prepare the
         * file, say) leaves an empty .part, and removing that download from the list
         * keeps it. A new download may take over an empty one that no download in the
         * list still has; one with bytes in it, or one a download still has, stays. */
        leftover = S_ISREG(st.st_mode) && st.st_size == 0;
        for (size_t i = 0; leftover && i < orbit.job_count; i++)
            if (orbit.jobs[i].id[0] && strcmp(orbit.jobs[i].status, "complete") &&
                job_paths_overlap(&orbit.jobs[i], &destination))
                leftover = false;
        absent = leftover;
    } else if (j.filename[0] && absent) {
        absent = errno == ENOENT;
    }
    if (absent && leftover && !dry_run && unlinkat(dir, partial, 0) && errno != ENOENT)
        absent = false;
    if (!absent) {
        close(dir);
        copy_text(err, cap,
                  "A destination or partial file already exists. Orbit will not overwrite it.");
        return 409;
    }
    if (!dry_run && storage_check_writable(dir)) {
        close(dir);
        copy_text(err, cap, "Destination is missing or not writable.");
        return 409;
    }
    close(dir);
    if (dry_run) return 200;
    (void)storage_mark_locked(&j);
    size_t slot = finished >= 0 ? (size_t)finished : orbit.job_count;
    Job previous = orbit.jobs[slot];
    orbit.jobs[slot] = j;
    if (finished < 0)
        orbit.job_count++;
    *result = &orbit.jobs[slot];
    copy_text(orbit.preferred_storage, sizeof orbit.preferred_storage, sid);
    char previous_delivery[sizeof orbit.last_delivery];
    copy_text(previous_delivery, sizeof previous_delivery, orbit.last_delivery);
    if (strcmp(r->source, "link"))
        copy_text(orbit.last_delivery, sizeof orbit.last_delivery, torbox ? "torbox" : "direct");
    if (state_save_locked()) {
        copy_text(orbit.last_delivery, sizeof orbit.last_delivery, previous_delivery);
        if (finished >= 0)
            orbit.jobs[slot] = previous;
        else
            orbit.job_count--;
        copy_text(err, cap, "Could not persist the queue.");
        return 503;
    }
    pthread_cond_broadcast(&orbit.changed);
    diagnostics_event_locked("queued", *result, 0, 0);
    return 201;
}
int job_create_locked(const char *rid, const char *sid, char *err, size_t cap, Job **result) {
    return job_create_delivery_locked(rid, sid, NULL, err, cap, result);
}
int job_create_delivery_locked(const char *rid, const char *sid, const char *delivery,
                                char *err, size_t cap, Job **result) {
    if (delivery && *delivery && strcmp(delivery, "direct") && strcmp(delivery, "torbox")) {
        copy_text(err, cap, "Choose direct or torbox delivery."); return 400;
    }
    const Release *r = release_find(rid);
    const char *method = delivery && *delivery ? delivery :
                         debrid_default_locked(r) ? "torbox" : "direct";
    return create_locked(r, sid, NULL, NULL, NULL, false, method, NULL, err, cap, result);
}
int job_create_torbox_recovery_locked(const Release *r, const Release *route, const Storage *storage,
                                      char *err, size_t cap, Job **result) {
    if (!source_enabled_locked(route) || !debrid_link_covered_locked(route)) return 409;
    return create_locked(r, storage->id, storage, NULL, NULL, false, "torbox", route, err, cap, result);
}
int job_check_capture_locked(const Release *r, const Storage *s, char *err, size_t cap) {
    return create_locked(r, s->id, s, NULL, NULL, true, "direct", NULL, err, cap, NULL);
}
int job_create_captured_locked(const Release *r, const Storage *s, const char *etag,
                               const char *modified, char *err, size_t cap, Job **result) {
    bool file_verified = r->remote_page && !strcmp(r->source, "remote");
    if (file_verified ? (!provider_supported(r) || !r->filename[0] || r->size <= 0)
                  : (!provider_capture_url(r, r->url) || (!etag[0] && !modified[0]))) {
        copy_text(err, cap, "The provider file has not been validated.");
        return 409;
    }
    return create_locked(r, s->id, s, etag, modified, false, "direct", NULL, err, cap, result);
}
int job_create_pspkg_torbox_locked(const Release *r, const Storage *s,
                                   char *err, size_t cap, Job **result) {
    if (!r || !s || strcmp(r->source, "remote") || !remote_page_pending(r) ||
        (strcmp(r->remote_catalog_id, PSPKG_PS5_SOURCE) &&
         strcmp(r->remote_catalog_id, PSPKG_PS4_SOURCE)) ||
        strcmp(remote_browser_provider(r->browser_url), "pspkg") ||
        !r->remote_provider_page[0] || !*remote_browser_provider(r->remote_provider_page) ||
        !strcmp(remote_browser_provider(r->remote_provider_page), "pspkg") || !debrid_link_covered_locked(r)) {
        copy_text(err, cap, "TorBox does not support this PSPKG provider.");
        return 409;
    }
    /* create_locked can fall back to direct when preparation fails. A PSPKG
     * provider page has no direct file yet, so reject that case up front. */
    Job prepared = {0};
    if (debrid_prepare_locked(r, &prepared, err, cap) != 200) return 409;
    return create_locked(r, s->id, s, NULL, NULL, false, "torbox", NULL, err, cap, result);
}
int job_create_link_locked(const Release *r, const Storage *s, const char *etag,
                           const char *modified, char *err, size_t cap, Job **result) {
    if (!link_release_valid(r)) {
        copy_text(err, cap, "This link didn't give Orbit a file it can save."); return 400;
    }
    return create_locked(r, s->id, s, etag, modified, false, "direct", NULL, err, cap, result);
}
bool job_can_replace_link(const Job *j) {
    return j && !strcmp(j->release.source, "link") && !job_active(j) &&
        (!strcmp(j->status, "paused") || !strcmp(j->status, "error")) &&
        !j->archive_ready && !(j->release.compression[0] && j->received >= j->total);
}
/* The fresh link must be the same file: the same size, and the same identity when both
 * links give one and part of the file is already here. The download keeps its name. */
int job_replace_link_locked(Job *j, const Release *r, const char *etag, const char *modified,
                            char *err, size_t cap) {
    if (orbit.stop || orbit.state_failed) {
        copy_text(err, cap, "Queue state cannot be saved. Try again after restarting Orbit."); return 503;
    }
    if (!job_can_replace_link(j)) {
        copy_text(err, cap, "Pause the download first, or wait until it stops."); return 409;
    }
    if (!link_url_allowed(r->url) || r->size != j->total) {
        copy_text(err, cap, "This link is for a different file (its size differs). Your partial file was kept."); return 409;
    }
    if (j->received > 0 && ((j->etag[0] && etag[0] && strcmp(j->etag, etag)) ||
                            (!j->etag[0] && !etag[0] && j->modified[0] && modified[0] && strcmp(j->modified, modified)))) {
        copy_text(err, cap, "This link is for a different copy of the file. Your partial file was kept."); return 409;
    }
    Job previous = *j;
    copy_text(j->release.url, sizeof j->release.url, r->url);
    copy_text(j->etag, sizeof j->etag, etag);
    copy_text(j->modified, sizeof j->modified, modified);
    copy_text(j->status, sizeof j->status, "queued");
    j->error[0] = 0; j->retry_at = 0; j->attempts = 0;
    atomic_store(&j->pause, false); atomic_store(&j->cancel, false);
    if (state_save_locked()) { *j = previous; copy_text(err, cap, "Could not persist the queue."); return 503; }
    pthread_cond_broadcast(&orbit.changed);
    return 200;
}
bool job_can_refresh_link(const Job *j) {
    return j && (!strcmp(j->status, "paused") || !strcmp(j->status, "error")) &&
        !j->browser_pending &&
        !j->archive_ready && !(j->release.compression[0] && j->received >= j->total) &&
        strcmp(j->delivery, "torbox") &&
        (!strcmp(j->release.source, "vikingfile") || !strcmp(j->release.source, "akirabox") ||
         !strcmp(j->release.source, "datanodes") || !strcmp(j->release.source, "1fichier")) &&
        provider_browser_supported(&j->release);
}
int job_refresh_link_locked(Job *j, const Release *r, const Storage *pin,
                            const char *etag, const char *modified, char *err, size_t cap) {
    if (orbit.stop || orbit.state_failed) {
        copy_text(err, cap, "Queue state cannot be saved. Try again after restarting Orbit."); return 503;
    }
    if (!job_can_refresh_link(j) || !source_enabled_locked(&j->release) ||
        strcmp(j->release_id, r->id) || strcmp(j->release.source, r->source) ||
        strcmp(j->filename, r->filename) || strcmp(j->release.browser_url, r->browser_url) ||
        j->total != r->size || strcmp(j->release.compression, r->compression) ||
        j->release.extracted_size != r->extracted_size || j->release.archive_unverified != r->archive_unverified || !provider_capture_url(r, r->url)) {
        copy_text(err, cap, "This download changed. Open its download page again."); return 409;
    }
    if ((!etag[0] && !modified[0]) || (j->etag[0] ? strcmp(j->etag, etag) :
        j->modified[0] ? strcmp(j->modified, modified) : j->received > 0)) {
        copy_text(err, cap, "The provider file changed. Your partial file was kept."); return 409;
    }
    (void)storage_rebind_locked(j);
    Storage drives[ORBIT_MAX_STORAGE]; size_t count = storage_list(drives);
    bool same = false;
    for (size_t i = 0; i < count; i++) {
        Storage *s = &drives[i];
        if (!strcmp(s->id, j->storage_id) && !strcmp(s->root, j->root) &&
            s->device == j->device && s->inode == j->inode &&
            !strcmp(s->root, pin->root) && s->device == pin->device && s->inode == pin->inode)
            same = fits(s, remaining(j), j);
    }
    if (!same) { copy_text(err, cap, "Reconnect the original drive and check its free space."); return 409; }
    int dir = storage_open(j);
    if (dir < 0) { copy_text(err, cap, "Cannot open the original homebrew folder."); return 409; }
    close(dir);
    Job previous = *j;
    copy_text(j->release.url, sizeof j->release.url, r->url);
    copy_text(j->etag, sizeof j->etag, etag);
    copy_text(j->modified, sizeof j->modified, modified);
    copy_text(j->status, sizeof j->status, "queued");
    j->error[0] = 0; j->retry_at = 0; j->attempts = 0;
    atomic_store(&j->pause, false); atomic_store(&j->cancel, false);
    if (state_save_locked()) { *j = previous; copy_text(err, cap, "Could not persist the queue."); return 503; }
    pthread_cond_broadcast(&orbit.changed);
    return 200;
}
int job_password_locked(Job *j, const char *password, char *err, size_t cap) {
    if (!j->release.compression[0] || !j->archive_ready ||
        (strcmp(j->status, "error") && strcmp(j->status, "paused"))) {
        copy_text(err, cap, "Pause extraction or wait for it to finish before entering a password."); return 409;
    }
    if (!password || !*password || strlen(password) >= sizeof j->archive_password) {
        copy_text(err, cap, "Enter an archive password of up to 128 bytes."); return 400;
    }
    for (const unsigned char *p = (const void *)password; *p; p++)
        if (*p < 32 || *p == 127) {
            copy_text(err, cap, "The archive password cannot contain control characters."); return 400;
        }
    Job previous = *j;
    copy_text(j->archive_password, sizeof j->archive_password, password);
    int code = job_action_locked(j, "retry", false, err, cap);
    if (code != 200) *j = previous;
    return code;
}
int job_action_locked(Job *j, const char *action, bool remove, char *err, size_t cap) {
    if (j->history_removing) { copy_text(err, cap, "Wait for file removal to finish."); return 409; }
    browser_job_changed_locked(j->id);
    if (orbit.stop) {
        copy_text(err, cap, "Orbit is stopping. Reconnect after restarting it.");
        return 503;
    }
    bool active = job_active(j);
    if (!strcmp(action, "delete") || !strcmp(action, "forget")) {
        if (strcmp(j->status, "cancelled")) {
            copy_text(err, cap, "Only cancelled downloads can be deleted.");
            return 409;
        }
        /* "forget" is an explicit history-only confirmation. It must never
         * open a drive or delete a file, even when the original is unavailable. */
        if (!strcmp(action, "delete") && job_delete_partial_locked(j, err, cap))
            return 409;
        Job previous = *j;
        memset(j, 0, sizeof *j);
        if (state_save_locked()) {
            *j = previous;
            copy_text(err, cap, "Could not save history changes.");
            return 503;
        }
        return 200;
    }
    if (!strcmp(action, "remove")) {
        (void)storage_rebind_locked(j);
        if (!removable(j)) {
            copy_text(err, cap,
                      "Only completed or cancelled history can be removed. Reconnect the original "
                      "drive and delete any kept partial file first.");
            return 409;
        }
        Job previous = *j;
        memset(j, 0, sizeof *j);
        if (state_save_locked()) {
            *j = previous;
            copy_text(err, cap, "Could not save history changes.");
            return 503;
        }
        return 200;
    }
    if (!strcmp(action, "move-up") || !strcmp(action, "move-down")) {
        if (!job_waiting(j)) {
            copy_text(err, cap, "Only waiting downloads can be reordered.");
            return 409;
        }
        bool up = !strcmp(action, "move-up");
        Job *neighbor = NULL;
        for (size_t i = 0; i < orbit.job_count; i++) {
            Job *other = &orbit.jobs[i];
            if (other == j || !job_waiting(other))
                continue;
            if ((up ? other->order < j->order : other->order > j->order) &&
                (!neighbor ||
                 (up ? other->order > neighbor->order : other->order < neighbor->order)))
                neighbor = other;
        }
        if (!neighbor)
            return 200;
        unsigned previous = j->order;
        j->order = neighbor->order;
        neighbor->order = previous;
        if (state_save_locked()) {
            neighbor->order = j->order;
            j->order = previous;
            copy_text(err, cap, "Could not save queue order.");
            return 503;
        }
        return 200;
    }
    if (!strcmp(action, "pause")) {
        if (active)
            j->pause = true;
        else if (!strcmp(j->status, "queued") || !strcmp(j->status, "retrying"))
            copy_text(j->status, sizeof j->status, "paused");
        else {
            copy_text(err, cap, "This job is not running.");
            return 409;
        }
    } else if (!strcmp(action, "resume") || !strcmp(action, "retry")) {
        if (storage_hold_conflict_locked(j)) {
            copy_text(err, cap, "Another file operation is using this.");
            return 409;
        }
        Release *r = &j->release;
        bool torbox = !strcmp(j->delivery, "torbox");
        if (!source_enabled_locked(r) || (!torbox && !j->browser_pending && !provider_supported(r))) {
            copy_text(err, cap, "This download option is no longer available.");
            return 409;
        }
        if (torbox) {
            if (!debrid_job_account_locked(j)) {
                copy_text(err, cap, "Reconnect the original TorBox account before resuming.");
                return 409;
            }
        }
        if (active || !strcmp(j->status, "complete") || !strcmp(j->status, "queued")) {
            copy_text(err, cap, "This job cannot be resumed in its current state.");
            return 409;
        }
        if (!storage_rebind_locked(j)) {
            copy_text(err, cap, "Reconnect the original destination drive.");
            return 409;
        }
        Storage drives[ORBIT_MAX_STORAGE];
        size_t count = storage_list(drives);
        bool enough = false;
        for (size_t i = 0; i < count; i++)
            if (!strcmp(drives[i].id, j->storage_id))
                enough = fits(&drives[i], remaining(j), j);
        if (!enough) {
            copy_text(err, cap,
                      "Not enough space after unfinished downloads. Free space or cancel a queued "
                      "download.");
            return 409;
        }
        /* A downloaded archive waiting for room to unpack: say so at once, not after it starts. */
        bool unpack = j->archive_ready && j->release.compression[0] && !j->keep_packed && unpack_size(j) > 0;
        for (size_t i = 0; unpack && i < count; i++)
            if (!strcmp(drives[i].id, j->storage_id) && !fits(&drives[i], unpack_size(j), j)) {
                copy_text(err, cap, "Not enough free space to unpack this download. Free up space on its drive, then select Resume. The downloaded archive was kept.");
                return 409;
            }
        j->pause = false;
        j->cancel = false;
        j->remove = false;
        j->attempts = 0;
        j->retry_at = 0;
        j->debrid_watch = 0;
        j->debrid_retries = 0;
        j->error[0] = 0;
        copy_text(j->status, sizeof j->status, "queued");
    } else if (!strcmp(action, "cancel")) {
        if (!strcmp(j->status, "complete")) {
            copy_text(err, cap, "Completed files cannot be deleted through cancellation.");
            return 409;
        }
        if (!active) {
            if (remove && job_delete_partial_locked(j, err, cap))
                return 409;
            copy_text(j->status, sizeof j->status, "cancelled");
        }
        j->cancel = true;
        j->remove = remove;
    } else {
        copy_text(err, cap, "Unknown queue action.");
        return 404;
    }
    if (state_save_locked()) {
        copy_text(err, cap, "Could not persist the queue change.");
        return 503;
    }
    browser_job_changed_locked(j->id);
    pthread_cond_broadcast(&orbit.changed);
    diagnostics_event_locked(action, j, 0, 0);
    return 200;
}
