#ifdef __linux__
#define _POSIX_C_SOURCE 200809L
#endif
#include "install.h"
#include "launcher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char owner[] = "Orbit Store ORBT00001 launcher v1\n";
static const char *title_id = "ORBT00001";
static char last_error[192];

const char *orbit_launcher_last_error(void) {
    return last_error;
}

void orbit_launcher_record_error(const char *step, int system_error, int platform_error) {
    snprintf(last_error, sizeof last_error, "%s: errno=%d, platform=0x%08x", step, system_error,
             (unsigned)platform_error);
}

/* 1 matches, 0 missing, -1 unsafe/different/unreadable. Never follow symlinks. */
static int matches(int parent, const char *name, const unsigned char *data, size_t length) {
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat st;
    int result = -1;
    if (fstat(fd, &st))
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_size != (off_t)length) {
        errno = EINVAL;
        goto done;
    }
    unsigned char buffer[512];
    size_t offset = 0;
    while (offset < length) {
        size_t wanted = length - offset;
        if (wanted > sizeof buffer)
            wanted = sizeof buffer;
        ssize_t n = read(fd, buffer, wanted);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto done;
        if (!n || memcmp(buffer, data + offset, (size_t)n)) {
            errno = EINVAL;
            goto done;
        }
        offset += (size_t)n;
    }
    result = 1;
done: {
    int saved_error = errno;
    close(fd);
    errno = saved_error;
}
    return result;
}

static int ensure_file(int parent, const char *name, const unsigned char *data, size_t length) {
    int existing = matches(parent, name, data, length);
    if (existing)
        return existing == 1 ? 0 : -1;
    int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    size_t offset = 0;
    int result = -1;
    while (offset < length) {
        ssize_t n = write(fd, data + offset, length - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            goto done;
        }
        offset += (size_t)n;
    }
    /* World-readable whatever the payload's umask: system services read the app files. */
    result = fchmod(fd, 0644) || fsync(fd) ? -1 : 0;
done: {
    int saved_error = errno;
    if (close(fd) && result == 0)
        result = -1;
    else
        errno = saved_error;
}
    /* A partial write is preserved and rejected on retry, never overwritten. */
    return result;
}

/* Replaces an owned regular file (or creates it) with a complete copy, never through a symlink. */
static int replace_file(int parent, const char *name, const unsigned char *data, size_t length) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    char temp[64];
    snprintf(temp, sizeof temp, "%s.orbit-new", name);
    unlinkat(parent, temp, 0);
    if (ensure_file(parent, temp, data, length))
        return -1;
    if (renameat(parent, temp, parent, name)) {
        unlinkat(parent, temp, 0);
        return -1;
    }
    return 0;
}

/* Writes a small state record only when its content changes. */
static int record(int parent, const char *name, const char *text) {
    int current = matches(parent, name, (const unsigned char *)text, strlen(text));
    return current == 1 ? 0 : replace_file(parent, name, (const unsigned char *)text, strlen(text));
}

/* Identifies this build's manifest and icon, so a newer build can update an installed icon. */
static void fingerprint(char out[18]) {
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *parts[] = {launcher_manifest, launcher_icon};
    size_t lengths[] = {sizeof launcher_manifest, sizeof launcher_icon};
    for (size_t p = 0; p < 2; p++)
        for (size_t i = 0; i < lengths[p]; i++)
            hash = (hash ^ parts[p][i]) * 1099511628211ULL;
    snprintf(out, 18, "%016llx\n", (unsigned long long)hash);
}

static int open_dir(int parent, const char *name, bool create) {
    if (create && mkdirat(parent, name, 0755) && errno != EEXIST)
        return -1;
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    /* The installer already verified ownership. Let the system app service
     * traverse these directories even if the loader started with a private umask. */
    struct stat info;
    if (fstat(fd, &info) || ((info.st_mode & 055) != 055 && fchmod(fd, (info.st_mode & 0777) | 055))) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

static int absent(int parent, const char *name) {
    struct stat st;
    if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0)
        return 0;
    return errno == ENOENT ? 1 : -1;
}

/* True for a non-empty regular file, never through a symlink. */
static bool complete(int parent, const char *name) {
    struct stat st;
    return fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
           st.st_size > 0;
}

/* True when the directory holds nothing but the allowed names. */
static bool only_entries(int dir, const char *const *allowed, size_t count) {
    int listing = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *d = listing < 0 ? NULL : fdopendir(listing);
    if (!d) {
        if (listing >= 0)
            close(listing);
        return false;
    }
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while (ok && (entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        ok = false;
        for (size_t i = 0; i < count && !ok; i++)
            ok = !strcmp(entry->d_name, allowed[i]);
    }
    if (errno)
        ok = false;
    closedir(d);
    return ok;
}

/* An Orbit launcher whose state records were lost (for example after /data/orbit-store was
 * reset): ORBT00001 holds only Orbit's sce_sys files and an Orbit local-deeplink manifest.
 * Anything else is an unknown title and is never adopted or changed. */
static bool orbit_layout(int up) {
    static const char *const top[] = {"sce_sys"};
    static const char *const sys[] = {"param.json", "icon0.png", "param.json.orbit-new",
                                      "icon0.png.orbit-new"};
    char text[4096];
    size_t length = 0;
    struct stat st;
    bool result = false;
    int us = -1, fd = -1;
    int user = openat(up, title_id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (user < 0 || !only_entries(user, top, sizeof top / sizeof top[0]))
        goto done;
    us = openat(user, "sce_sys", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (us < 0 || !only_entries(us, sys, sizeof sys / sizeof sys[0]))
        goto done;
    fd = openat(us, "param.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size >= (off_t)sizeof text)
        goto done;
    while (length < (size_t)st.st_size) {
        ssize_t n = read(fd, text + length, (size_t)st.st_size - length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto done;
        length += (size_t)n;
    }
    text[length] = '\0';
    result = strlen(text) == length && strstr(text, "\"titleId\": \"ORBT00001\"") &&
             strstr(text, "\"titleName\": \"Orbit Store\"") &&
             strstr(text, "\"deeplinkUri\": \"http://127.0.0.1:");
done:
    if (fd >= 0)
        close(fd);
    if (us >= 0)
        close(us);
    if (user >= 0)
        close(user);
    return result;
}

int orbit_launcher_ensure_at(int state_fd, const char *user_parent, bool (*registration_missing)(void),
                             int (*prepare)(void), int (*register_title)(void)) {
    int lock = -1, up = -1, user = -1, us = -1, result = -1;
    const char *step = "lock-launcher";
    last_error[0] = '\0';
    lock = openat(state_fd, "launcher.lock", O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lock < 0)
        goto done;
    struct stat lock_info;
    if (fstat(lock, &lock_info))
        goto done;
    if (!S_ISREG(lock_info.st_mode)) {
        errno = EINVAL;
        goto done;
    }
    /* A new payload can repair the shortcut while the service already runs.
     * Serialize claims, file replacement and the native registration call. */
    while (flock(lock, LOCK_EX))
        if (errno != EINTR)
            goto done;
    step = "open-app-parent";
    up = open(user_parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (up < 0)
        goto done;
    step = "verify-ownership-record";
    int owned = matches(state_fd, "launcher-owned-v1", owner, sizeof owner - 1);
    if (owned < 0)
        goto done;
    step = "check-existing-title";
    int missing = absent(up, title_id);
    if (missing < 0)
        goto done;
    bool adopted = false;
    if (!owned && !missing) {
        adopted = orbit_layout(up);
        if (!adopted) {
            /* Never change a title Orbit did not create. */
            errno = EEXIST;
            goto done;
        }
        puts("Orbit launcher: adopting the ORBT00001 icon left by an earlier Orbit.");
    }
    step = "verify-ready-record";
    int ready = matches(state_fd, "launcher-ready-v1", owner, sizeof owner - 1);
    if (ready < 0)
        goto done;
    if (ready && !owned && !adopted) {
        errno = EINVAL;
        goto done;
    }
    if (adopted)
        ready = 0; /* Claimed and registered again below, like a first installation. */
    if (ready && missing) {
        /* Removing the home-screen app leaves Orbit's state behind. Invalidate
         * its old registration before rebuilding, so a failed repair retries. */
        step = "clear-deleted-launcher-record";
        if (unlinkat(state_fd, "launcher-ready-v1", 0))
            goto done;
        step = "sync-deleted-launcher-record";
        if (fsync(state_fd))
            goto done;
        ready = 0;
    }
    if (!ready) {
        step = "prepare-app-registration";
        errno = 0;
        if (prepare())
            goto done;
        step = "write-ownership-record";
        if (ensure_file(state_fd, "launcher-owned-v1", owner, sizeof owner - 1))
            goto done;
        step = "sync-ownership-directory";
        if (fsync(state_fd))
            goto done;
    }
    step = "open-title-directory";
    user = open_dir(up, title_id, !ready);
    if (user < 0)
        goto done;
    step = "open-sce-sys-directory";
    /* The title is Orbit's here: restore a missing sce_sys instead of leaving a registered
     * icon with nothing to start. */
    us = open_dir(user, "sce_sys", true);
    if (us < 0)
        goto done;
    struct {
        int fd;
        const char *name;
        const unsigned char *data;
        size_t length;
    } files[] = {{us, "param.json", launcher_manifest, sizeof launcher_manifest},
                 {us, "icon0.png", launcher_icon, sizeof launcher_icon}};
    char build[18];
    fingerprint(build);
    step = "verify-files-record";
    int current =
        ready ? matches(state_fd, "launcher-files-v1", (const unsigned char *)build, strlen(build))
              : 1;
    bool intact = true, usable = true;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        intact = intact && matches(files[i].fd, files[i].name, files[i].data, files[i].length) == 1;
        usable = usable && complete(files[i].fd, files[i].name);
    }
    /* The saved record certifies the previous registration. The PS5 can
     * remove its Media entry while leaving Orbit's files and records behind. */
    bool entry_missing = ready && registration_missing();
    if (ready && current == 1 && intact && !entry_missing) {
        result = 0;
        goto done;
    }
    if (ready) {
        /* Registered by an older build, or its files were removed or damaged since. The title
         * is Orbit's: restore its files and registration as needed. The files record is
         * written last, so a failed update is retried on the next start. */
        step = "prepare-app-update";
        errno = 0;
        bool registrable = !prepare();
        if (!registrable && usable) {
            /* The existing registration and its complete files remain usable; keep them. */
            if (entry_missing)
                goto done;
            puts("Orbit launcher: registration unavailable; keeping the installed icon.");
            result = 0;
            goto done;
        }
        for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
            step = i ? "update-icon" : "update-manifest";
            if (matches(files[i].fd, files[i].name, files[i].data, files[i].length) != 1 &&
                replace_file(files[i].fd, files[i].name, files[i].data, files[i].length))
                goto done;
        }
        if (!registrable) {
            /* The files are repaired. Recheck registration on the next start. */
            result = entry_missing ? -1 : 0;
            goto done;
        }
        step = "register-updated-title";
        errno = 0;
        if (register_title())
            goto done;
        step = "write-files-record";
        if (record(state_fd, "launcher-files-v1", build) || fsync(state_fd))
            goto done;
        result = 1;
        goto done;
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        step = i ? "verify-or-write-icon" : "verify-or-write-manifest";
        /* Owned (or just created) here: files left by an older build or a failed attempt are
         * replaced with complete copies, never through a symlink or over a directory. */
        if (matches(files[i].fd, files[i].name, files[i].data, files[i].length) != 1 &&
            replace_file(files[i].fd, files[i].name, files[i].data, files[i].length)) {
            puts("Orbit launcher: app files are unsafe or cannot be written; preserved.");
            goto done;
        }
    }
    step = "register-title";
    errno = 0;
    if (register_title())
        goto done;
    step = "write-ready-record";
    if (ensure_file(state_fd, "launcher-ready-v1", owner, sizeof owner - 1))
        goto done;
    step = "write-files-record";
    if (record(state_fd, "launcher-files-v1", build))
        goto done;
    step = "sync-ready-directory";
    if (fsync(state_fd))
        goto done;
    result = 1;
done:
    if (result < 0 && !last_error[0])
        orbit_launcher_record_error(step, errno, 0);
    if (us >= 0)
        close(us);
    if (user >= 0)
        close(user);
    if (up >= 0)
        close(up);
    if (lock >= 0)
        close(lock);
    if (result < 0)
        printf("Orbit launcher is not ready: %s. Server remains available.\n", last_error);
    return result;
}
