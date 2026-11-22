#include "orbit.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
/* Use the same directory operations as downloads. access() can disagree with
 * actual I/O under console credentials; it must not hide an otherwise usable
 * internal or M.2 drive. Creation/writes are checked only on download actions. */
static const char *inspect_destination(int root, dev_t device) {
    struct stat expected, actual;
    if (fstatat(root, "homebrew", &expected, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? NULL : "destination-stat-failed";
    if (!S_ISDIR(expected.st_mode)) return "destination-not-directory";
    if (expected.st_dev != device) return "destination-on-other-device";
    int fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return "destination-not-accessible";
    const char *reason = NULL;
    if (fstat(fd, &actual) || !S_ISDIR(actual.st_mode) ||
        actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino)
        reason = "destination-changed";
    close(fd);
    return reason;
}
/* Return a fixed, path-free reason for diagnostics. Listing never creates files. */
static const char *inspect(Storage *s, const char *id, const char *label, const char *root,
                           bool external) {
    struct stat st, parent, opened;
    struct statvfs fs;
    if (lstat(root, &st)) return errno == ENOENT ? "not-present" : "root-stat-failed";
    if (!S_ISDIR(st.st_mode)) return "root-not-directory";
    if (statvfs(root, &fs)) return "capacity-unavailable";
    if (fs.f_flag & ST_RDONLY) return "read-only-filesystem";
    if (external && !orbit.desktop) {
        if (stat("/mnt", &parent)) return "mount-check-failed";
        if (st.st_dev == parent.st_dev) return "not-mounted";
    }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return "root-not-accessible";
    const char *reason;
    if (fstat(fd, &opened) || !S_ISDIR(opened.st_mode) ||
        opened.st_dev != st.st_dev || opened.st_ino != st.st_ino)
        reason = "root-changed";
    else
        reason = inspect_destination(fd, st.st_dev);
    close(fd);
    if (reason) return reason;
    memset(s, 0, sizeof *s);
    copy_text(s->id, sizeof s->id, id);
    copy_text(s->label, sizeof s->label, label);
    copy_text(s->root, sizeof s->root, root);
    s->device = st.st_dev;
    s->inode = st.st_ino;
    s->external = external;
    uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    s->free_bytes = (uint64_t)fs.f_bavail * unit;
    s->total_bytes = (uint64_t)fs.f_blocks * unit;
#ifdef ORBIT_TEST
    /* The free space downloads see; Files checks ORBIT_TEST_FILES_FREE_BYTES. */
    const char *limited = getenv("ORBIT_TEST_STORAGE_FREE_BYTES");
    if (limited) s->free_bytes = strtoull(limited, NULL, 10);
#endif
    return NULL;
}
static bool inspect_report(Storage *s, const char *id, const char *label, const char *root,
                           bool external, cJSON *report) {
    const char *reason = inspect(s, id, label, root, external);
    if (report) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", id);
        cJSON_AddBoolToObject(item, "available", !reason);
        cJSON_AddStringToObject(item, "reason", reason ? reason : "ready");
        cJSON_AddItemToArray(report, item);
    }
    return !reason;
}
static size_t enumerate(Storage *out, cJSON *report) {
    size_t n = 0;
    if (orbit.desktop) {
        if (orbit.desktop_storage[0] &&
            inspect_report(out, "desktop", "Desktop test storage", orbit.desktop_storage, false, report))
            n++;
        return n;
    }
    for (int i = 0; i < 8; i++) {
        char id[24], label[64], root[64];
        snprintf(id, sizeof id, "usb%d", i);
        snprintf(label, sizeof label, "USB storage %d", i + 1);
        snprintf(root, sizeof root, "/mnt/usb%d", i);
        if (inspect_report(&out[n], id, label, root, true, report))
            n++;
    }
    for (int i = 0; i < 8; i++) {
        char id[24], label[64], root[64];
        snprintf(id, sizeof id, "ext%d", i);
        snprintf(label, sizeof label, "External storage %d", i + 1);
        if (i == 1) copy_text(label, sizeof label, "M.2 SSD");
        snprintf(root, sizeof root, "/mnt/ext%d", i);
        if (inspect_report(&out[n], id, label, root, true, report))
            n++;
    }
    if (inspect_report(&out[n], "internal", "Internal storage", "/data", false, report))
        n++;
    return n;
}
size_t storage_list(Storage *out) {
    return enumerate(out, NULL);
}
cJSON *storage_diagnostics(void) {
    Storage out[ORBIT_MAX_STORAGE];
    cJSON *report = cJSON_CreateArray();
    enumerate(out, report);
    return report;
}
/* The drive new downloads start with ("" lets clients choose: the first external drive). Creating
 * a download also sets it, so the last drive used stays the default. */
int storage_prefer(const char *id, char *error, size_t cap) {
    if (*id) {
        Storage s[ORBIT_MAX_STORAGE];
        size_t n = storage_list(s), i = 0;
        while (i < n && strcmp(s[i].id, id))
            i++;
        if (i == n) {
            copy_text(error, cap, "Selected storage is not connected or writable.");
            return 409;
        }
    }
    pthread_mutex_lock(&orbit.mutex);
    char previous[sizeof orbit.preferred_storage];
    copy_text(previous, sizeof previous, orbit.preferred_storage);
    copy_text(orbit.preferred_storage, sizeof orbit.preferred_storage, id);
    int code = 200;
    if (state_save_locked()) {
        copy_text(orbit.preferred_storage, sizeof orbit.preferred_storage, previous);
        copy_text(error, cap, "Could not save the default drive.");
        code = 503;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}
/* The only folder a job's drive can be at: its saved root for its drive ID. */
static bool known_root(const Job *j) {
    bool known = orbit.desktop ?
        !strcmp(j->storage_id, "desktop") && !strcmp(j->root, orbit.desktop_storage) :
        !strcmp(j->storage_id, "internal") && !strcmp(j->root, "/data");
    if (!orbit.desktop && !known && strlen(j->storage_id) == 4 &&
        j->storage_id[3] >= '0' && j->storage_id[3] <= '7' &&
        (!strncmp(j->storage_id, "usb", 3) || !strncmp(j->storage_id, "ext", 3))) {
        char root[32];
        snprintf(root, sizeof root, "/mnt/%s", j->storage_id);
        known = !strcmp(j->root, root);
    }
    return known;
}
bool storage_matches(const Job *j) {
    /* Identity checks run on every writer, and during queue actions. Never scan
     * unrelated mounts or ask statvfs for free space here: an allocating USB
     * volume must not block access to a different drive. Capacity is checked
     * separately by discovery/preflight and the transfer's own fstatvfs. */
    if (!known_root(j)) return false;
    struct stat expected, actual;
    if (lstat(j->root, &expected) || !S_ISDIR(expected.st_mode) ||
        expected.st_dev != j->device || expected.st_ino != j->inode) return false;
    int fd = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return false;
    bool same = !fstat(fd, &actual) && S_ISDIR(actual.st_mode) &&
        actual.st_dev == j->device && actual.st_ino == j->inode &&
        !inspect_destination(fd, j->device);
    close(fd);
    return same;
}

/* Orbit's mark at the top of a drive it downloads to: 32 random hex digits.
 * A job keeps its drive's mark, so it recognises that drive when the console
 * remounts it with new device numbers, as it does after repairing a drive. */
#define DRIVE_MARK ".orbit-drive"
static bool read_mark(int root, char mark[33]) {
    char line[40];
    int fd = openat(root, DRIVE_MARK, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    ssize_t n;
    do { n = read(fd, line, sizeof line); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n < 32) return false;
    for (int i = 0; i < 32; i++)
        if (!isxdigit((unsigned char)line[i])) return false;
    memcpy(mark, line, 32);
    mark[32] = 0;
    return true;
}
static bool write_mark(int root, char mark[33]) {
    char nonce[17], temp[64], line[33];
    random_hex(nonce, 8);
    snprintf(temp, sizeof temp, DRIVE_MARK "-%s.tmp", nonce);
    random_hex(mark, 16);
    memcpy(line, mark, 32);
    line[32] = '\n';
    int fd = openat(root, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, line, sizeof line) == (ssize_t)sizeof line && !fsync(fd);
    if (close(fd)) ok = false;
    /* Replaces only an unreadable mark: a valid one was read first. */
    if (ok) ok = !renameat(root, temp, root, DRIVE_MARK);
    if (!ok) unlinkat(root, temp, 0);
    else (void)fsync(root);
    return ok;
}
/* With the queue locked: give a job on its matching drive the drive's mark,
 * leaving one there if it has none. True when the job's mark was set. A drive
 * Orbit cannot mark keeps working, only without remount recognition. */
bool storage_mark_locked(Job *j) {
    if (j->drive_mark[0] || !storage_matches(j)) return false;
    int root = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0) return false;
    struct stat st;
    char mark[33];
    bool marked = !fstat(root, &st) && st.st_dev == j->device && st.st_ino == j->inode &&
                  (read_mark(root, mark) || write_mark(root, mark));
    close(root);
    if (marked) copy_text(j->drive_mark, sizeof j->drive_mark, mark);
    return marked;
}
/* Unfinished downloads saved before drives were marked get their mark now. */
void storage_mark_jobs_locked(void) {
    bool changed = false;
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (j->id[0] && strcmp(j->status, "complete") && strcmp(j->status, "cancelled") && storage_mark_locked(j))
            changed = true;
    }
    if (changed) (void)state_save_locked();
}
/* The job's own partial file on the drive at root, with at least its saved
 * progress. Without saved progress there is nothing to protect, as long as no
 * partial file of that name is in the way. */
static bool partial_matches(int root, const Job *j, dev_t device) {
    bool unstarted = !j->received && !j->segmented;
    int dir = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return unstarted && errno == ENOENT;
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    struct stat st;
    int rc = fstatat(dir, partial, &st, AT_SYMLINK_NOFOLLOW), error = errno;
    close(dir);
    if (rc) return unstarted && error == ENOENT;
    return S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_dev == device &&
           st.st_size > 0 && st.st_size >= j->received;
}
/* With the queue locked, for a job whose drive does not match: is the drive at
 * its path the same one, remounted with new device numbers? Its mark must match,
 * if the job has one. File integrity is checked separately by the transfer:
 * a filesystem repair can shorten/remove a partial without changing the drive.
 * Legacy unmarked jobs still require their partial as evidence. A different
 * marked drive at the same path stays refused. */
bool storage_rebind_locked(Job *j) {
    if (storage_matches(j)) return true;
    if (!known_root(j)) return false;
    struct stat st, opened, parent;
    if (lstat(j->root, &st) || !S_ISDIR(st.st_mode)) return false;
    if (!orbit.desktop && strcmp(j->storage_id, "internal") &&
        (stat("/mnt", &parent) || st.st_dev == parent.st_dev))
        return false;
    int root = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0) return false;
    char mark[33];
    bool same = !fstat(root, &opened) && S_ISDIR(opened.st_mode) && opened.st_dev == st.st_dev &&
                opened.st_ino == st.st_ino && !inspect_destination(root, st.st_dev) &&
                (j->drive_mark[0] ? (read_mark(root, mark) && !strcmp(mark, j->drive_mark))
                                  : partial_matches(root, j, st.st_dev));
    close(root);
    if (!same) return false;
    dev_t device = j->device;
    ino_t inode = j->inode;
    j->device = st.st_dev;
    j->inode = st.st_ino;
    if (!storage_matches(j) || state_save_locked()) {
        j->device = device;
        j->inode = inode;
        return false;
    }
    diagnostics_event_locked("drive-recognised", j, 0, 0);
    if (storage_mark_locked(j)) (void)state_save_locked();
    return true;
}
int storage_open(const Job *j) {
    if (!storage_matches(j)) {
        errno = ENODEV;
        return -1;
    }
    int root = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0)
        return -1;
    struct stat st;
    if (fstat(root, &st) || st.st_dev != j->device || st.st_ino != j->inode) {
        close(root);
        errno = ENODEV;
        return -1;
    }
    int fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        /* Opening an existing destination must not require permission to
         * create entries in its parent. Create only when it is absent. */
        if (!mkdirat(root, "homebrew", 0755) || errno == EEXIST)
            fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    }
    int error = errno;
    close(root);
    if (fd < 0) {
        errno = error;
        return -1;
    }
    if (fstat(fd, &st) || st.st_dev != j->device) {
        close(fd);
        errno = ENODEV;
        return -1;
    }
    return fd;
}
/* A folder of a drive from its top ("" is the top), walked one name at a time
 * without following links or leaving the drive's device. "homebrew" is the
 * download destination, created if missing, as storage_open() does. */
int storage_open_path(const Job *j, const char *path) {
    if (!strcmp(path, "homebrew")) return storage_open(j);
    if (!storage_matches(j)) {
        errno = ENODEV;
        return -1;
    }
    char copy[1024];
    if (strlen(path) >= sizeof copy) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) || st.st_dev != j->device || st.st_ino != j->inode)) {
        close(fd);
        errno = ENODEV;
        return -1;
    }
    copy_text(copy, sizeof copy, path);
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); fd >= 0 && part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".") || !strcmp(part, "..")) {
            close(fd);
            errno = EINVAL;
            return -1;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(fd);
        if (next >= 0 && (fstat(next, &st) || !S_ISDIR(st.st_mode) || st.st_dev != j->device)) {
            close(next);
            errno = EXDEV;
            return -1;
        }
        fd = next;
    }
    return fd;
}
/* Explicit download preflight, never discovery: exclusively create a private
 * file relative to the verified destination and unlink it before writing so
 * failed writes leave no probe behind. Never truncate or remove a collision. */
int storage_check_writable(int directory) {
    for (int attempt = 0; attempt < 4; attempt++) {
        char nonce[33], name[64];
        random_hex(nonce, 16);
        snprintf(name, sizeof name, ".orbit-write-%s.tmp", nonce);
        int fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return -1;
        }
        int error = 0;
        if (unlinkat(directory, name, 0)) {
            error = errno;
        } else {
            ssize_t n;
            do { n = write(fd, "", 1); } while (n < 0 && errno == EINTR);
            if (n != 1) error = n < 0 ? errno : EIO;
        }
        if (close(fd) && !error) error = errno;
        if (error) { errno = error; return -1; }
        return 0;
    }
    errno = EEXIST;
    return -1;
}
