#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include "art_disk.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#ifndef ART_DISK_BUDGET
#define ART_DISK_BUDGET (256u * 1024u * 1024u)
#endif
#ifndef ART_DISK_ENTRIES
#define ART_DISK_ENTRIES 2048
#endif
#define IMAGE_LIMIT (12u * 1024u * 1024u)
#define HEADER_SIZE 40
static int directory = -1;
static unsigned serial;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool digest(const void *bytes, size_t size, unsigned char out[32]) {
    unsigned n = 0;
    return EVP_Digest(bytes, size, out, &n, EVP_sha256(), NULL) == 1 && n == 32;
}
void art_disk_key(const char *text, char out[65]) {
    unsigned char hash[32];
    out[0] = 0;
    if (!digest(text, strlen(text), hash)) return;
    for (size_t i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", hash[i]);
}
static bool filename(const char *name) {
    return strlen(name) == 68 && strspn(name, "0123456789abcdef") == 64 && !strcmp(name + 64, ".img");
}
typedef struct { char name[69]; off_t size; time_t used; } Saved;
static int oldest(const void *a, const void *b) {
    const Saved *x = a, *y = b;
    return (x->used > y->used) - (x->used < y->used);
}
/* Only our hashed image files are removed, never another state file. */
static void trim(void) {
    int fd = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    DIR *dir = fd < 0 ? NULL : fdopendir(fd);
    if (!dir) { if (fd >= 0) close(fd); return; }
    Saved *rows = NULL; size_t count = 0, capacity = 0; unsigned long long bytes = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        struct stat st;
        size_t length = strlen(entry->d_name);
        if (length > 70 && entry->d_name[0] == '.' && strspn(entry->d_name + 1, "0123456789abcdef") == 64 &&
            entry->d_name[65] == '.' && !strcmp(entry->d_name + length - 4, ".tmp") &&
            !fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && time(NULL) - st.st_mtime > 86400) {
            unlinkat(directory, entry->d_name, 0); continue;
        }
        if (!filename(entry->d_name) || fstatat(directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
        if (st.st_size < HEADER_SIZE || st.st_size > IMAGE_LIMIT + HEADER_SIZE) { unlinkat(directory, entry->d_name, 0); continue; }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            Saved *grown = realloc(rows, next * sizeof *rows);
            if (!grown) break;
            rows = grown; capacity = next;
        }
        Saved *row = &rows[count++];
        memcpy(row->name, entry->d_name, 69); row->size = st.st_size; row->used = st.st_mtime;
        bytes += (unsigned long long)st.st_size;
    }
    closedir(dir);
    if (count > 1) qsort(rows, count, sizeof *rows, oldest);
    for (size_t i = 0; i < count && (bytes > ART_DISK_BUDGET || count - i > ART_DISK_ENTRIES); i++)
        if (!unlinkat(directory, rows[i].name, 0)) bytes -= (unsigned long long)rows[i].size;
    free(rows);
}
void art_disk_open(const char *state_dir) {
    pthread_mutex_lock(&mutex);
    if (directory >= 0) close(directory);
    directory = -1;
    int root = state_dir && *state_dir ? open(state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW) : -1;
    if (root >= 0) {
        if (!mkdirat(root, "art", 0700) || errno == EEXIST)
            directory = openat(root, "art", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        close(root);
    }
    if (directory >= 0) trim();
    pthread_mutex_unlock(&mutex);
}
void art_disk_close(void) {
    pthread_mutex_lock(&mutex);
    if (directory >= 0) close(directory);
    directory = -1;
    pthread_mutex_unlock(&mutex);
}
static bool io_all(int fd, void *bytes, size_t size, bool writing) {
    unsigned char *p = bytes;
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= (size_t)n;
    }
    return true;
}
bool art_disk_load(const char *url, unsigned char **data, size_t *size) {
    char key[65], name[69]; art_disk_key(url, key);
    *data = NULL; *size = 0;
    if (!*key) return false;
    snprintf(name, sizeof name, "%s.img", key);
    pthread_mutex_lock(&mutex);
    int fd = directory < 0 ? -1 : openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st; unsigned char header[HEADER_SIZE], sum[32], *bytes = NULL; bool ok = false;
    if (fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > HEADER_SIZE && st.st_size <= IMAGE_LIMIT + HEADER_SIZE) {
        size_t n = (size_t)st.st_size - HEADER_SIZE;
        bytes = malloc(n);
        ok = bytes && io_all(fd, header, sizeof header, false) && !memcmp(header, "ORBITART", 8) &&
             io_all(fd, bytes, n, false) && digest(bytes, n, sum) && !memcmp(header + 8, sum, 32);
        if (ok) { *data = bytes; *size = n; futimens(fd, NULL); }
    }
    if (fd >= 0) {
        close(fd);
        if (!ok) unlinkat(directory, name, 0);
    }
    if (!ok) free(bytes);
    pthread_mutex_unlock(&mutex);
    return ok;
}
void art_disk_save(const char *url, const unsigned char *data, size_t size) {
    if (!size || size > IMAGE_LIMIT || size + HEADER_SIZE > ART_DISK_BUDGET) return;
    unsigned char header[HEADER_SIZE]; memcpy(header, "ORBITART", 8);
    if (!digest(data, size, header + 8)) return;
    char key[65], name[69], temp[100]; art_disk_key(url, key);
    if (!*key) return;
    snprintf(name, sizeof name, "%s.img", key);
    pthread_mutex_lock(&mutex);
    snprintf(temp, sizeof temp, ".%s.%ld.%u.tmp", key, (long)getpid(), ++serial);
    int fd = directory < 0 ? -1 : openat(directory, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        bool ok = io_all(fd, header, sizeof header, true) && io_all(fd, (void *)data, size, true) && !fsync(fd);
        if (close(fd)) ok = false;
        if (ok) ok = !renameat(directory, temp, directory, name);
        if (!ok) unlinkat(directory, temp, 0);
        else trim();
    }
    pthread_mutex_unlock(&mutex);
}
