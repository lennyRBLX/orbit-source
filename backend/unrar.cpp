// RAR archives through RARLAB's UnRAR (see unrar.h). UnRAR runs in test mode:
// it unpacks into its own memory and hands every block to Orbit's callbacks,
// which check names, write, flush and publish everything themselves. It reads
// only the descriptor Orbit opened, never a path, and never opens volumes.
#include "unrar.h"
#include "rar.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" HANDLE PASCAL OrbitRAROpenArchiveFd(struct RAROpenArchiveDataEx *r, int fd, unsigned int threads);

namespace {
const char *const kMemory = "Not enough memory to extract this archive on the PS5. Use Orbit Zero to extract it on a computer.";
const char *const kUnreadable = "Could not read the archive from the drive. Check the drive, then retry.";

struct Entry {
    std::string path;
    int64_t size;
    bool directory, included;
};
struct Context {
    const SevenCallbacks *cb;
    void *user;
    std::wstring password;
    bool passwordAsked = false;
    bool writing = false; // Data belongs to an entry Orbit is extracting.
    int result = 0;
};

int CALLBACK callback(UINT msg, LPARAM user, LPARAM p1, LPARAM p2) {
    Context *c = reinterpret_cast<Context *>(user);
    switch (msg) {
    case UCM_PROCESSDATA:
        if (c->cb->stopped(c->user)) { c->result = 1; return -1; }
        if (c->writing && (c->result = c->cb->write(c->user, reinterpret_cast<const void *>(p1), (size_t)p2)))
            return -1;
        return 1;
    case UCM_NEEDPASSWORDW: {
        c->passwordAsked = true;
        wchar_t *out = reinterpret_cast<wchar_t *>(p1);
        if (c->password.empty() || c->password.size() >= (size_t)p2) return -1;
        wmemcpy(out, c->password.c_str(), c->password.size() + 1);
        return 1;
    }
    case UCM_NEEDPASSWORD:
        c->passwordAsked = true;
        return -1; // Orbit's password is given in the wide form above.
    case UCM_CHANGEVOLUME:
    case UCM_CHANGEVOLUMEW:
        return -1; // Multi-part archives aren't supported.
    case UCM_LARGEDICT:
        return -1; // Larger than UnRAR's 4 GB default: refused.
    }
    return 0;
}

// Closes an open archive on every path, an exception included: a leaked
// handle would keep UnRAR's unpacking buffers until Orbit restarts.
struct ArchiveHandle {
    HANDLE h = nullptr;
    ~ArchiveHandle() { close(); }
    void close() { if (h) RARCloseArchive(h); h = nullptr; }
};

// The header of the next entry, with its full name. 0, ERAR_END_ARCHIVE or an error.
struct Header {
    RARHeaderDataEx data;
    wchar_t name[4096];
    int read(HANDLE h) {
        memset(&data, 0, sizeof data);
        data.FileNameEx = name;
        data.FileNameExSize = sizeof name / sizeof name[0];
        name[0] = 0;
        return RARReadHeaderEx(h, &data);
    }
};

int open_archive(int fd, unsigned mode, unsigned threads, Context &ctx, HANDLE *out, char *error, size_t cap) {
    *out = nullptr;
    RAROpenArchiveDataEx r;
    memset(&r, 0, sizeof r);
    wchar_t name[] = L"archive.rar"; // Only for UnRAR's messages; the file is fd.
    r.ArcNameW = name;
    r.OpenMode = mode;
    r.Callback = callback;
    r.UserData = reinterpret_cast<LPARAM>(&ctx);
    if (lseek(fd, 0, SEEK_SET) < 0) { snprintf(error, cap, "%s", kUnreadable); return -1; }
    HANDLE h = OrbitRAROpenArchiveFd(&r, fd, threads);
    if (!h) {
        if (ctx.result) return ctx.result;
        switch (r.OpenResult) {
        case ERAR_NO_MEMORY: snprintf(error, cap, "%s", kMemory); return -1;
        case ERAR_MISSING_PASSWORD:
        case ERAR_BAD_PASSWORD: return -2;
        case ERAR_EREAD: snprintf(error, cap, "%s", kUnreadable); return -1;
        default:
            if (ctx.passwordAsked) return -2; // Encrypted headers that don't decode.
            snprintf(error, cap, "This download is not a readable %s archive.", "rar");
            return -1;
        }
    }
    if (r.Flags & ROADF_VOLUME) {
        RARCloseArchive(h);
        snprintf(error, cap, "Multi-part archives aren't supported.");
        return -1;
    }
    *out = h;
    return 0;
}

// Why UnRAR stopped on an entry, as Orbit says it.
int failure(int code, bool encrypted, const Context &ctx, char *error, size_t cap) {
    switch (code) {
    case ERAR_MISSING_PASSWORD:
    case ERAR_BAD_PASSWORD: return -2;
    case ERAR_BAD_DATA:
        // RAR4 can't tell a wrong password from damaged data.
        if (encrypted && ctx.passwordAsked) return -2;
        snprintf(error, cap, "The archive is damaged: an extracted file failed its checksum.");
        return -1;
    case ERAR_UNKNOWN_FORMAT: snprintf(error, cap, "The archive uses a compression method Orbit can't extract."); return -1;
    case ERAR_NO_MEMORY:
    case ERAR_LARGE_DICT: snprintf(error, cap, "%s", kMemory); return -1;
    case ERAR_EREAD: snprintf(error, cap, "%s", kUnreadable); return -1;
    case ERAR_EOPEN:
    case ERAR_BAD_ARCHIVE: snprintf(error, cap, "The archive is incomplete: it ends before all of its files."); return -1;
    }
    return -1;
}

int decode(int fd, const char *password, unsigned threads, const SevenCallbacks *cb, void *user,
           char *error, size_t cap, bool inspect) {
    unsigned char signature[7] = {};
    if (pread(fd, signature, sizeof signature, 0) != (ssize_t)sizeof signature ||
        memcmp(signature, "Rar!\x1a\x07", 6) || (signature[6] != 0 && signature[6] != 1)) {
        snprintf(error, cap, "This download is not a %s archive.", "rar");
        return -1;
    }
    Context ctx{cb, user};
    if (password && *password && !UtfToWide(password, ctx.password)) return -1;
    std::unique_ptr<Header> header(new Header);

    // Every entry is checked before anything is written. Listing reads
    // headers only, even in a solid archive.
    ArchiveHandle arc;
    int rc = open_archive(fd, RAR_OM_LIST, 1, ctx, &arc.h, error, cap);
    if (rc) return rc;
    HANDLE h = arc.h;
    std::vector<Entry> entries;
    bool any = false;
    for (;;) {
        if (cb->stopped(user)) { rc = 1; break; }
        int code = header->read(h);
        if (code == ERAR_END_ARCHIVE) break;
        if (code) {
            rc = code == ERAR_MISSING_PASSWORD || code == ERAR_BAD_PASSWORD || (ctx.passwordAsked && code == ERAR_BAD_DATA) ? -2 : -1;
            if (rc == -1) snprintf(error, cap, "The archive is damaged: its file list can't be read.");
            break;
        }
        const RARHeaderDataEx &d = header->data;
        if (entries.size() >= ORBIT_ARCHIVE_MAX_ENTRIES) {
            snprintf(error, cap, "The archive has too many entries."); rc = -1; break;
        }
        if (d.Flags & (RHDF_SPLITBEFORE | RHDF_SPLITAFTER)) {
            snprintf(error, cap, "Multi-part archives aren't supported."); rc = -1; break;
        }
        std::string path;
        WideToUtf(std::wstring(header->name), path);
        bool directory = (d.Flags & RHDF_DIRECTORY) != 0;
        int64_t size = directory ? 0 : (int64_t)(((uint64_t)d.UnpSizeHigh << 32) | d.UnpSize);
        if (size < 0) { rc = -1; break; }
        unsigned type = d.HostOS == 3 ? (d.FileAttr & S_IFMT) : 0; // Unix: a st_mode.
        bool unsafe = d.RedirType != 0 || (type && type != S_IFREG && type != S_IFDIR);
        int check = cb->check(user, path.c_str(), size, directory, unsafe);
        if (check && check != 2) { rc = check; break; }
        entries.push_back({path, size, directory, !check});
        any = any || !check;
        if ((code = RARProcessFile(h, RAR_SKIP, nullptr, nullptr))) { rc = failure(code, false, ctx, error, cap); break; }
    }
    arc.close();
    if (rc || inspect || !any) return rc;

    rc = open_archive(fd, RAR_OM_EXTRACT, threads, ctx, &arc.h, error, cap);
    if (rc) return rc;
    h = arc.h;
    size_t index = 0;
    for (;;) {
        if (cb->stopped(user)) { rc = 1; break; }
        int code = header->read(h);
        if (code == ERAR_END_ARCHIVE) break;
        if (code) { rc = failure(code, true, ctx, error, cap); break; }
        std::string path;
        WideToUtf(std::wstring(header->name), path);
        // The same archive as listed: anything else is refused.
        if (index >= entries.size() || entries[index].path != path) { rc = -1; break; }
        const Entry &e = entries[index++];
        bool encrypted = (header->data.Flags & RHDF_ENCRYPTED) != 0;
        if (!e.included || e.directory) {
            if (e.included && (rc = cb->begin(user, e.path.c_str(), 0, 1))) break;
            if (e.included && (rc = cb->end(user))) break;
            // In a solid archive skipping unpacks too, so it can be cancelled.
            code = RARProcessFile(h, RAR_SKIP, nullptr, nullptr);
            if (ctx.result) { rc = ctx.result; break; }
            if (code) { rc = failure(code, encrypted, ctx, error, cap); break; }
            continue;
        }
        if ((rc = cb->begin(user, e.path.c_str(), e.size, 0))) break;
        ctx.writing = true;
        code = RARProcessFileW(h, RAR_TEST, nullptr, nullptr);
        ctx.writing = false;
        if (ctx.result) { rc = ctx.result; break; }
        if (code) { rc = failure(code, encrypted, ctx, error, cap); break; }
        if ((rc = cb->end(user))) break;
    }
    arc.close();
    if (!rc && index != entries.size()) rc = -1;
    return rc;
}

int invoke(int fd, const char *password, unsigned threads, const SevenCallbacks *cb, void *user,
           char *error, size_t cap, bool inspect) {
    int result;
    error[0] = 0;
    try { result = decode(fd, password, threads, cb, user, error, cap, inspect); }
    catch (const std::bad_alloc &) { snprintf(error, cap, "%s", kMemory); return -1; }
    catch (...) { result = -1; }
    if (result == -2) snprintf(error, cap, "The archive password is missing or incorrect. The downloaded archive was kept.");
    else if (result == -1 && !*error) snprintf(error, cap, "The archive is damaged or uses an unsupported compression method.");
    return result;
}
} // namespace

extern "C" unsigned unrar_threads(void) {
#ifdef ORBIT_DESKTOP
    // Host benchmarks and tests compare thread counts.
    if (const char *forced = getenv("ORBIT_UNRAR_THREADS")) {
        int n = atoi(forced);
        if (n >= 1 && n <= 8) return (unsigned)n;
    }
#endif
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online < 1 ? 4 : online > 4 ? 4 : (unsigned)online;
}
extern "C" int unrar_inspect(int fd, const char *password, const SevenCallbacks *cb, void *user,
                             char *error, size_t cap) {
    return invoke(fd, password, 1, cb, user, error, cap, true);
}
extern "C" int unrar_extract(int fd, const char *password, unsigned threads, const SevenCallbacks *cb,
                             void *user, char *error, size_t cap) {
    return invoke(fd, password, threads ? threads : unrar_threads(), cb, user, error, cap, false);
}
