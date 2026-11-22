#include "library_verify.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERIFY_CHUNK (1024U * 1024U)
typedef struct {
    atomic_bool *cancel;
    LibraryVerifyProgress progress;
    void *context;
    unsigned entries;
    int64_t bytes;
    char *buffers;
} Verify;
static int open_parent(const char *path, char name[256]) {
    Storage drives[ORBIT_MAX_STORAGE];
    size_t count = storage_list(drives);
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(drives[i].root);
        if (strncmp(path, drives[i].root, n) || path[n] != '/') continue;
        const char *relative = path + n + 1, *slash = strrchr(relative, '/');
        char parent[768] = "";
        const char *leaf = slash ? slash + 1 : relative;
        if (!*leaf || strlen(leaf) >= 256 || !strcmp(leaf, ".") || !strcmp(leaf, "..")) return -1;
        if (slash) {
            if ((size_t)(slash - relative) >= sizeof parent) return -1;
            memcpy(parent, relative, (size_t)(slash - relative));
        }
        Job pin = {0};
        copy_text(pin.storage_id, sizeof pin.storage_id, drives[i].id);
        copy_text(pin.root, sizeof pin.root, drives[i].root);
        pin.device = drives[i].device; pin.inode = drives[i].inode;
        copy_text(name, 256, leaf);
        return storage_open_path(&pin, parent);
    }
    return -1;
}
int library_source_identity(const char *path, struct stat *identity) {
    char name[256]; int parent = open_parent(path, name);
    if (parent < 0) return -1;
    int rc = fstatat(parent, name, identity, AT_SYMLINK_NOFOLLOW);
    close(parent);
    return rc || (!S_ISREG(identity->st_mode) && !S_ISDIR(identity->st_mode)) ? -1 : 0;
}
static bool unchanged(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size && a->st_mode == b->st_mode &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
bool library_source_unchanged(const char *path, const struct stat *expected) {
    struct stat current;
    return !library_source_identity(path, &current) && unchanged(expected, &current);
}
static bool stopped(Verify *v) { return atomic_load(v->cancel) || orbit_is_stopping(); }
static int read_exact(int fd, char *buffer, size_t length) {
    size_t used = 0;
    while (used < length) {
        ssize_t n = read(fd, buffer + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    return 0;
}
static int compare_tree(int a, const char *an, int b, const char *bn, unsigned depth, dev_t ad, dev_t bd, Verify *v) {
    if (stopped(v)) return 1;
    if (depth > 64 || ++v->entries > 200000) return -1;
    struct stat x, y, after;
    if (fstatat(a, an, &x, AT_SYMLINK_NOFOLLOW) || fstatat(b, bn, &y, AT_SYMLINK_NOFOLLOW) ||
        x.st_dev != ad || y.st_dev != bd) return -1;
    bool folder = S_ISDIR(x.st_mode);
    if (folder ? !S_ISDIR(y.st_mode) : !S_ISREG(x.st_mode) || !S_ISREG(y.st_mode) || x.st_size != y.st_size) return -1;
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | (folder ? O_DIRECTORY : 0);
    int from = openat(a, an, flags), to = openat(b, bn, flags), rc = -1;
    if (from < 0 || to < 0) goto done;
    if (fstat(from, &after) || !unchanged(&x, &after) || fstat(to, &after) || !unchanged(&y, &after)) goto done;
    if (folder) {
        int scan = dup(from); DIR *dir = scan < 0 ? NULL : fdopendir(scan);
        if (!dir) { if (scan >= 0) close(scan); goto done; }
        unsigned children = 0, others = 0;
        rc = 0;
        for (;;) {
            errno = 0; struct dirent *entry = readdir(dir);
            if (!entry) { if (errno) rc = -1; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            children++;
            rc = compare_tree(from, entry->d_name, to, entry->d_name, depth + 1, ad, bd, v);
            if (rc) break;
        }
        closedir(dir);
        if (rc) goto done;
        scan = dup(to); dir = scan < 0 ? NULL : fdopendir(scan);
        if (!dir) { if (scan >= 0) close(scan); rc = -1; goto done; }
        for (;;) {
            errno = 0; struct dirent *entry = readdir(dir);
            if (!entry) { if (errno) rc = -1; break; }
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) others++;
            if (others > children || stopped(v)) { rc = stopped(v) ? 1 : -1; break; }
        }
        closedir(dir);
        if (children != others) rc = -1;
    } else {
        rc = 0;
        for (off_t at = 0; at < x.st_size;) {
            if (stopped(v)) { rc = 1; break; }
            size_t n = (uint64_t)(x.st_size - at) > VERIFY_CHUNK ? VERIFY_CHUNK : (size_t)(x.st_size - at);
            if (read_exact(from, v->buffers, n) || read_exact(to, v->buffers + VERIFY_CHUNK, n) ||
                memcmp(v->buffers, v->buffers + VERIFY_CHUNK, n)) { rc = -1; break; }
            at += (off_t)n; v->bytes += (int64_t)n;
            if (v->progress) v->progress(v->bytes, v->context);
        }
    }
    if (!rc && (fstat(from, &after) || !unchanged(&x, &after) || fstat(to, &after) || !unchanged(&y, &after))) rc = -1;
done:
    if (from >= 0) close(from);
    if (to >= 0) close(to);
    return rc;
}
int library_verify_copy(const char *source, const char *destination, dev_t source_device,
                        ino_t source_inode, atomic_bool *cancel, LibraryVerifyProgress progress,
                        void *context, char *error, size_t cap) {
    char an[256], bn[256]; struct stat x, y;
    int a = open_parent(source, an), b = open_parent(destination, bn), rc = -1;
    Verify v = {.cancel = cancel, .progress = progress, .context = context, .buffers = malloc(2 * VERIFY_CHUNK)};
    if (a >= 0 && b >= 0 && v.buffers && !fstatat(a, an, &x, AT_SYMLINK_NOFOLLOW) &&
        !fstatat(b, bn, &y, AT_SYMLINK_NOFOLLOW) && x.st_dev == source_device && x.st_ino == source_inode)
        rc = compare_tree(a, an, b, bn, 0, x.st_dev, y.st_dev, &v);
    if (rc) copy_text(error, cap, rc == 1 ? "Move cancelled. Both copies have been kept."
        : "The copied files could not be verified. The original has been kept; check both drives before trying again.");
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    free(v.buffers);
    return rc;
}
