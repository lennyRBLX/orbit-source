#include "game_verify.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_MAXIMUM 768
#define ISSUE_LIMIT 256
#define ENTRY_LIMIT 100000
#define SCRIPT_LIMIT (1024U * 1024U)
#define TEXT_LIMIT (16U * SCRIPT_LIMIT)
typedef struct { size_t from, length; char *replacement; } Edit;
typedef struct {
    char path[PATH_MAXIMUM];
    struct stat stamp;
    mode_t mode;
    char *text;
    size_t length;
    Edit *edits;
    unsigned count;
} Change;
typedef struct {
    int root;
    dev_t device;
    atomic_bool *cancel;
    EVP_MD_CTX *hash;
    cJSON *report, *issues;
    Change *changes;
    unsigned count, entries, fixes, warnings, lookups, names;
    size_t bytes;
    bool incomplete;
} Check;
static bool cancelled(Check *c) { return c->cancel && atomic_load(c->cancel); }
static bool suffix(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext); return a >= b && !strcasecmp(name + a - b, ext);
}
static bool same(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mode == b->st_mode && a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}
static bool safe_path(const char *path) {
    if (!*path || *path == '/' || strlen(path) >= PATH_MAXIMUM || strchr(path, '\\')) return false;
    const char *p = path;
    while (*p) {
        const char *end = strchr(p, '/'); size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n || (n == 1 && *p == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        for (size_t i = 0; i < n; ++i) if ((unsigned char)p[i] < 32 || (unsigned char)p[i] == 127) return false;
        p = end ? end + 1 : p + n;
        if (end && !*p) return false;
    }
    return true;
}
/* Open components separately; a replacement symlink never escapes the root. */
static int parent(Check *c, const char *path, char *leaf) {
    if (!safe_path(path)) return -1;
    char copy[PATH_MAXIMUM]; copy_text(copy, sizeof copy, path);
    char *last = strrchr(copy, '/'); copy_text(leaf, 256, last ? last + 1 : copy);
    if (last) *last = 0;
    int fd = openat(c->root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (last) for (char *save = NULL, *part = strtok_r(copy, "/", &save); fd >= 0 && part; part = strtok_r(NULL, "/", &save)) {
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); close(fd); fd = next;
        struct stat st; if (fd >= 0 && (fstat(fd, &st) || st.st_dev != c->device)) { close(fd); fd = -1; }
    }
    return fd;
}
static cJSON *issue(Check *c, const char *kind, const char *path, bool fix) {
    if (cJSON_GetArraySize(c->issues) >= ISSUE_LIMIT) { c->incomplete = true; return NULL; }
    cJSON *o = cJSON_CreateObject(); cJSON_AddItemToArray(c->issues, o);
    cJSON_AddStringToObject(o, "kind", kind); cJSON_AddStringToObject(o, "path", path);
    cJSON_AddBoolToObject(o, "repairable", fix);
    if (fix) c->fixes++; else c->warnings++;
    return o;
}
static Change *change(Check *c, const char *path, const struct stat *st) {
    for (unsigned i = 0; i < c->count; ++i) if (!strcmp(c->changes[i].path, path)) return &c->changes[i];
    if (c->count == ISSUE_LIMIT) { c->incomplete = true; return NULL; }
    Change *n = &c->changes[c->count++]; copy_text(n->path, sizeof n->path, path); n->stamp = *st; n->mode = st->st_mode & 0777; return n;
}
/* Resolve each component by exact spelling first. Two folded matches are
 * ambiguous, even if the filesystem happens to return one first. */
static int resolve(Check *c, const char *path, char *actual) {
    if (!safe_path(path)) return 0;
    if (++c->lookups > 10000) { c->incomplete = true; return 0; }
    int dir = openat(c->root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    char copy[PATH_MAXIMUM]; copy_text(copy, sizeof copy, path); actual[0] = 0;
    int result = 0;
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); dir >= 0 && part; part = strtok_r(NULL, "/", &save)) {
        char name[256] = ""; struct stat st;
        int listing = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        DIR *d = listing < 0 ? NULL : fdopendir(listing);
        if (!d) { if (listing >= 0) close(listing); break; }
        unsigned matches = 0; bool exact = false; struct dirent *e;
        while ((e = readdir(d))) {
            if (++c->names > 1000000) { c->incomplete = true; break; }
            if (!strcmp(e->d_name, part)) { copy_text(name, sizeof name, e->d_name); exact = true; }
            else if (!strcasecmp(e->d_name, part)) { if (!exact) copy_text(name, sizeof name, e->d_name); matches++; }
        }
        closedir(d);
        if (!exact && matches > 1) { result = -1; break; }
        if ((!exact && matches != 1) || fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) break;
        if (st.st_dev != c->device || S_ISLNK(st.st_mode)) break;
        size_t used = strlen(actual), size = strlen(name);
        if (used + size + 2 >= PATH_MAXIMUM) break;
        if (used) actual[used++] = '/'; memcpy(actual + used, name, size + 1);
        if (!save || !*save) { result = S_ISREG(st.st_mode) ? 1 : 0; break; }
        int next = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); close(dir); dir = next;
    }
    if (dir >= 0) close(dir); return result;
}
static bool reference(const char *s) {
    return suffix(s, ".bin") || suffix(s, ".prx") || suffix(s, ".sprx") || suffix(s, ".elf") ||
           suffix(s, ".self") || suffix(s, ".lua") || suffix(s, ".iso") || suffix(s, ".cue");
}
static bool long_string(const char *text, size_t size, size_t start, size_t *last) {
    if(start>=size || text[start]!='[')return false;
    size_t marker=start+1;while(marker<size && text[marker]=='=')marker++;
    if(marker>=size || text[marker]!='[')return false;
    const size_t equals=marker-start-1;
    for(size_t i=marker+1;i<size;++i)if(text[i]==']') {
        size_t n=0;while(n<equals && i+1+n<size && text[i+1+n]=='=')n++;
        if(n==equals && i+1+n<size && text[i+1+n]==']'){*last=i+1+n;return true;}
    }
    *last=size;return true;
}
static void lua(Check *c, int dir, const char *name, const char *path, const struct stat *stamp) {
    if (stamp->st_size <= 0) return;
    if (stamp->st_size > SCRIPT_LIMIT || c->bytes + (size_t)stamp->st_size > TEXT_LIMIT) { c->incomplete = true; return; }
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK); struct stat st;
    if (fd < 0 || fstat(fd, &st) || !same(&st, stamp) || st.st_nlink != 1) { if (fd >= 0) close(fd); c->incomplete = true; return; }
    char *text = calloc(1, (size_t)st.st_size + 1);
    if (!text) { close(fd); c->incomplete = true; return; }
    size_t used = 0;
    while (used < (size_t)st.st_size) { ssize_t n = read(fd, text + used, (size_t)st.st_size - used); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; used += (size_t)n; }
    struct stat after; bool valid = used == (size_t)st.st_size && !fstat(fd, &after) && same(&st, &after) && !memchr(text, 0, used);
    close(fd); c->bytes += used;
    if (!valid) { free(text); c->incomplete = true; return; }
    EVP_DigestUpdate(c->hash, text, used);
    char containing[PATH_MAXIMUM]; copy_text(containing, sizeof containing, path); char *slash = strrchr(containing, '/'); if (slash) *slash = 0; else containing[0] = 0;
    Change *target = NULL;
    for (size_t i = 0; i < used && !cancelled(c); ++i) {
        if (text[i] == '-' && i + 1 < used && text[i + 1] == '-') {
            size_t last;
            if(long_string(text,used,i+2,&last)){i=last;continue;}
            while (i < used && text[i] != '\n') i++; continue;
        }
        size_t last;
        if(long_string(text,used,i,&last)){i=last;continue;}
        if (text[i] != '\'' && text[i] != '"') continue;
        char quote = text[i]; size_t first = i + 1, end = first; bool escaped = false;
        for (; end < used && text[end] != quote && text[end] != '\n'; ++end) if (text[end] == '\\') { escaped = true; if (end + 1 < used) end++; }
        i = end;
        if (end >= used || text[end] != quote || escaped || end - first >= PATH_MAXIMUM) continue;
        char old[PATH_MAXIMUM]; memcpy(old, text + first, end - first); old[end - first] = 0;
        if (!reference(old)) continue;
        const char *plain = !strncmp(old, "./", 2) ? old + 2 : old;
        if (!safe_path(plain)) { issue(c, "unsupported-reference", path, false); continue; }
        char root_match[PATH_MAXIMUM], local_match[PATH_MAXIMUM], local[PATH_MAXIMUM];
        int a = resolve(c, plain, root_match), b = 0;
        if (*containing && snprintf(local, sizeof local, "%s/%s", containing, plain) < (int)sizeof local) b = resolve(c, local, local_match);
        if ((a == 1 && !strcmp(root_match, plain)) || (b == 1 && !strcmp(local_match, local))) continue;
        if (a < 0 || b < 0 || (a == 1 && b == 1 && strcmp(root_match, local_match))) { issue(c, "ambiguous-reference", path, false); continue; }
        const char *match = a == 1 ? root_match : b == 1 && !strncmp(local_match, containing, strlen(containing)) ? local_match + strlen(containing) + 1 : NULL;
        if (!match) { cJSON *o = issue(c, "missing-reference", path, false); if (o) cJSON_AddStringToObject(o, "reference", old); continue; }
        char replacement[PATH_MAXIMUM]; snprintf(replacement, sizeof replacement, "%s%s", plain != old ? "./" : "", match);
        if (strcmp(replacement, old)) {
            if (!target) target = change(c, path, &st);
            if (!target || target->count >= ISSUE_LIMIT) { c->incomplete = true; break; }
            cJSON *o = issue(c, "filename-case", path, true); if (!o) break;
            cJSON_AddStringToObject(o, "reference", old); cJSON_AddStringToObject(o, "replacement", replacement);
            Edit *edits = realloc(target->edits, (target->count + 1) * sizeof *edits);
            if (!edits) { c->incomplete = true; break; }
            target->edits = edits; target->edits[target->count++] = (Edit){first, end - first, strdup(replacement)};
            if (!target->edits[target->count - 1].replacement) { c->incomplete = true; break; }
        }
    }
    if (target && target->count) { target->text = text; target->length = used; } else free(text);
}
static int compare_names(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static int walk(Check *c, int dir, const char *prefix, unsigned depth) {
    if (depth > 48 || cancelled(c)) return -1;
    int listing = openat(dir, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW); DIR *d = listing < 0 ? NULL : fdopendir(listing);
    if (!d) { if (listing >= 0) close(listing); return -1; }
    char **entries = NULL; size_t count = 0; struct dirent *e;
    /* Stable ordering makes the preview token independent of readdir order. */
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strncmp(e->d_name, ".orbit-repair-", 14)) continue;
        if (++c->entries > ENTRY_LIMIT) { c->incomplete = true; break; }
        char **next = realloc(entries, (count + 1) * sizeof *next);
        if (!next) { c->incomplete = true; break; } entries = next;
        entries[count] = strdup(e->d_name); if (!entries[count]) { c->incomplete = true; break; } count++;
    }
    closedir(d);
    qsort(entries, count, sizeof *entries, compare_names);
    int result = 0;
    for (size_t i = 0; i < count; ++i) {
        const char *name = entries[i]; char path[PATH_MAXIMUM]; struct stat st;
        if (cancelled(c)) { result = -1; break; }
        if (snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", name) >= (int)sizeof path ||
            fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW)) { c->incomplete = true; continue; }
        if (st.st_dev != c->device || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) || (st.st_nlink > 1 && S_ISREG(st.st_mode))) { issue(c, "unsupported-entry", path, false); continue; }
        char stamp[160]; snprintf(stamp, sizeof stamp, "%ju:%ju:%ju:%ju:%jd:%ld", (uintmax_t)st.st_dev, (uintmax_t)st.st_ino,
            (uintmax_t)st.st_mode, (uintmax_t)st.st_size, (intmax_t)st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
        EVP_DigestUpdate(c->hash, path, strlen(path) + 1); EVP_DigestUpdate(c->hash, stamp, strlen(stamp) + 1);
        mode_t need = S_ISDIR(st.st_mode) || !strcasecmp(name, "eboot.bin") || suffix(name, ".prx") || suffix(name, ".sprx") || suffix(name, ".elf") || suffix(name, ".self") ? 0755 : 0644;
        if ((st.st_mode & need) != need) { Change *v = change(c, path, &st); if (v) { v->mode |= need; cJSON *o = issue(c, "permissions", path, true); if (o) { char mode[8]; snprintf(mode, sizeof mode, "%04o", v->mode); cJSON_AddStringToObject(o, "mode", mode); } } }
        if (S_ISDIR(st.st_mode)) { int child = openat(dir, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); if (child < 0 || walk(c, child, path, depth + 1)) c->incomplete = true; if (child >= 0) close(child); }
        else if (suffix(name, ".lua")) lua(c, dir, name, path, &st);
    }
    for (size_t i = 0; i < count; ++i) free(entries[i]); free(entries); return result;
}
static void destroy(Check *c, bool report) {
    for (unsigned i = 0; i < c->count; ++i) { Change *v = &c->changes[i]; for (unsigned j = 0; j < v->count; ++j) free(v->edits[j].replacement); free(v->edits); free(v->text); }
    free(c->changes); EVP_MD_CTX_free(c->hash); if (report) cJSON_Delete(c->report);
}
static int inspect(Check *c, int root, atomic_bool *cancel, char *error, size_t cap) {
    *c = (Check){.root = root, .cancel = cancel}; struct stat st;
    c->hash = EVP_MD_CTX_new(); c->changes = calloc(ISSUE_LIMIT, sizeof *c->changes); c->report = cJSON_CreateObject();
    c->issues = cJSON_AddArrayToObject(c->report, "issues");
    if (!c->hash || !c->changes || !c->report || fstat(root, &st) || !S_ISDIR(st.st_mode) || EVP_DigestInit_ex(c->hash, EVP_sha256(), NULL) != 1) {
        copy_text(error, cap, "The game folder could not be checked."); return -1;
    }
    c->device = st.st_dev;
    EVP_DigestUpdate(c->hash, &st.st_mode, sizeof st.st_mode);
    if ((st.st_mode & 0755) != 0755) {
        Change *v = change(c, ".", &st);
        if (v) { v->mode |= 0755; cJSON *o = issue(c, "permissions", ".", true); if (o) cJSON_AddStringToObject(o, "mode", "0755"); }
    }
    if (walk(c, root, "", 0)) c->incomplete = true;
    unsigned char digest[32]; unsigned length = 0;
    EVP_DigestFinal_ex(c->hash, digest, &length); char token[65] = "";
    if (length == 32) for (unsigned i = 0; i < 32; ++i) snprintf(token + i * 2, 3, "%02x", digest[i]);
    cJSON_AddStringToObject(c->report, "token", token); cJSON_AddNumberToObject(c->report, "checkedFiles", c->entries);
    cJSON_AddNumberToObject(c->report, "fixCount", c->fixes); cJSON_AddNumberToObject(c->report, "warningCount", c->warnings);
    cJSON_AddBoolToObject(c->report, "complete", !c->incomplete && !cancelled(c));
    cJSON_AddBoolToObject(c->report, "canRepair", c->fixes && !c->incomplete && !cancelled(c));
    return 0;
}
cJSON *game_verify(int root, atomic_bool *cancel, char *error, size_t cap) {
    Check c; if (inspect(&c, root, cancel, error, cap)) { destroy(&c, true); return NULL; }
    cJSON *report = c.report; destroy(&c, false); return report;
}
static int write_all(int fd, const char *text, size_t size) {
    size_t written = 0;
    while (written < size) { ssize_t n = write(fd, text + written, size - written); if (n < 0 && errno == EINTR) continue; if (n <= 0) return -1; written += (size_t)n; } return 0;
}
int game_repair(int root, const char *preview, atomic_bool *cancel, cJSON **report, char *error, size_t cap) {
    *report = NULL; Check c;
    if (inspect(&c, root, cancel, error, cap)) { destroy(&c, true); return -1; }
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c.report, "canRepair")) || !preview || strcmp(preview, json_text(c.report, "token"))) {
        copy_text(error, cap, "The game folder changed or its check is incomplete. Verify it again before repairing."); destroy(&c, true); return -1;
    }
    int result = 0;
    for (unsigned i = 0; i < c.count; ++i) {
        Change *v = &c.changes[i]; char leaf[256]; int dir;
        if (!strcmp(v->path, ".")) { dir = dup(root); copy_text(leaf, sizeof leaf, "."); }
        else dir = parent(&c, v->path, leaf);
        struct stat st;
        int fd = dir < 0 ? -1 : openat(dir, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (cancelled(&c) || fd < 0 || fstat(fd, &st) || !same(&st, &v->stamp)) result = -1;
        if (!result && v->text) {
            char buffer[4096]; size_t offset = 0;
            while (offset < v->length) {
                size_t wanted = v->length - offset; if (wanted > sizeof buffer) wanted = sizeof buffer;
                ssize_t got = pread(fd, buffer, wanted, (off_t)offset);
                if (got < 0 && errno == EINTR) continue;
                if (got <= 0 || memcmp(buffer, v->text + offset, (size_t)got)) { result = -1; break; }
                offset += (size_t)got;
            }
        }
        if (!result && v->text) {
            char backup[128], temp[128]; snprintf(backup, sizeof backup, ".orbit-repair-%.20s-%u.bak", preview, i);
            snprintf(temp, sizeof temp, ".orbit-repair-%.20s-%u.tmp", preview, i);
            int saved = openat(dir, backup, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (saved < 0) result = -1;
            else {
                const bool backup_failed = write_all(saved, v->text, v->length) || fsync(saved);
                close(saved);
                if (backup_failed) { unlinkat(dir, backup, 0); result = -1; }
            }
            int out = result ? -1 : openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            if (out < 0) result = -1;
            size_t at = 0;
            for (unsigned j = 0; !result && j < v->count; ++j) {
                const Edit *edit = &v->edits[j];
                if (write_all(out, v->text + at, edit->from - at) || write_all(out, edit->replacement, strlen(edit->replacement))) result = -1;
                at = edit->from + edit->length;
            }
            struct stat current;
            if (!result && (write_all(out, v->text + at, v->length - at) || fchmod(out, v->mode) || fsync(out) ||
                fstatat(dir, leaf, &current, AT_SYMLINK_NOFOLLOW) || !same(&current, &st) || renameat(dir, temp, dir, leaf) || fsync(dir))) result = -1;
            if (out >= 0) close(out);
            if (result) unlinkat(dir, temp, 0);
        } else if (!result && fchmod(fd, v->mode)) result = -1;
        if (fd >= 0) close(fd); if (dir >= 0) close(dir);
        if (result) break;
    }
    destroy(&c, true);
    *report = game_verify(root, cancel, error, cap);
    if (!*report || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(*report,"complete")) || cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(*report,"fixCount")) > 0) result = -1;
    if (result) copy_text(error, cap, "Some repairs could not finish. Check the game again; configuration backups were kept.");
    return result;
}
