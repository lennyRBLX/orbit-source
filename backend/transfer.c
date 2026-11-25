#include "ca.h"
#include "orbit.h"
#include "power.h"
#include "firmware.h"
#include "packages.h"
#include "archive_account.h"
#include "viking_session.h"
#include "debrid.h"
#include "extract.h"
#include <openssl/crypto.h>
#include "provider_network.h"
#include "remote_sources.h"
#include "transfer_writer.h"
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef ORBIT_BENCHMARK
#include "transfer_benchmark.h"
#endif
_Static_assert(sizeof(off_t) >= 8, "Orbit requires 64-bit file offsets");
#define PARALLEL_MIN_REMAINING (32LL * 1024 * 1024)
typedef struct Transfer {
    Job *job;
    Release *release;
    int fd;
    long status;
    int64_t length, start, end, total, offset, written, limit;
    int64_t live[ORBIT_MAX_RANGES];
    TransferWriter *writer;
    unsigned char *window_buffer;
    unsigned connections; /* this download's share of the setting, before retries halve it */
    struct Transfer *owner;
    int segment;
    char etag[256], modified[128], type[128], error[256];
    time_t retry_at;
    bool accepted, probe, range_probe, unsupported_range;
    bool capture_probe;
    atomic_bool *cancel_signal;
    CURL *handle;
    char disposition[512];
    double started;
    DownloadPolicy policy;
    DownloadHealth health;
    int64_t network_bytes;
    bool stalled;
    char *direct;    /* Owner only: where a piece was finally served from, for later pieces. */
    bool via_direct; /* This piece asked that saved address, not the provider's link. */
#ifdef ORBIT_BENCHMARK
    int64_t sample_span;
    double deadline;
    long socket_buffer, curl_buffer;
    int socket_before, socket_after, socket_error;
    cJSON *metrics;
    cJSON *speed_samples;
    double last_sample;
    int64_t enqueued;
#endif
} Transfer;
static bool torbox(const Transfer *t) { return !strcmp(t->job->delivery, "torbox"); }
static bool remote_transfer(const Transfer *t) { return !strcmp(t->release->source, "remote"); }
static bool direct_transfer(const Transfer *t) { return !strcmp(t->release->source, ORBIT_DIRECT_SOURCE); }
static bool get_verified_file(const Transfer *t) {
    return direct_transfer(t) || (remote_transfer(t) && t->release->remote_page);
}
static bool guarded(const Transfer *t) {
    return torbox(t) || remote_transfer(t) || direct_transfer(t) || provider_browser_source(t->release->source);
}
static bool address_allowed(const Transfer *t, const char *url) {
    if (torbox(t)) return debrid_transfer_url_allowed(url);
    if (direct_transfer(t)) return direct_http_url(url);
    return remote_transfer(t) ? remote_https_url(url) : provider_transfer_url_allowed(url);
}
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int64_t transfer_span(const Transfer *t) {
#ifdef ORBIT_BENCHMARK
    const Transfer *owner = t->owner ? t->owner : t;
    if (owner->sample_span) return owner->sample_span;
#endif
    return t->release->size;
}
static void header_value(char *out, size_t cap, const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' '))
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}
static bool strong_etag(const char *value) {
    size_t length = strlen(value);
    if (length < 2 || value[0] != '"' || value[length - 1] != '"') return false;
    for (size_t i = 1; i + 1 < length; i++)
        if ((unsigned char)value[i] < 33 || value[i] == '"' || (unsigned char)value[i] == 127) return false;
    return true;
}
static bool disposition_named(const char *header) {
    for (const char *p = strchr(header, ';'); p; p = strchr(p + 1, ';')) {
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t') q++;
        if (!strncasecmp(q, "filename=", 9) || !strncasecmp(q, "filename*=", 10)) return true;
    }
    return false;
}
static bool disposition_matches(const char *header, const char *expected) {
    bool plain = false, extended = false, plain_match = false, extended_match = false;
    const char *p = strchr(header, ';');
    while (p && *p) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        bool star = !strncasecmp(p, "filename*=", 10);
        bool filename = star || !strncasecmp(p, "filename=", 9);
        if (!filename) { p = strchr(p, ';'); continue; }
        p += star ? 10 : 9;
        while (*p == ' ' || *p == '\t') p++;
        bool quoted = *p == '"';
        if (quoted) p++;
        char value[512]; size_t n = 0;
        while (*p && (quoted ? *p != '"' : *p != ';')) {
            if (quoted && *p == '\\') { p++; if (!*p) return false; }
            if (n + 1 >= sizeof value) return false;
            value[n++] = *p++;
        }
        if (quoted) {
            if (*p != '"') return false;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != ';') return false;
        } else while (n && (value[n-1] == ' ' || value[n-1] == '\t')) n--;
        value[n] = 0;
        if (star) {
            if (extended || strncasecmp(value, "UTF-8'", 6)) return false;
            const char *encoded = strchr(value + 6, '\'');
            if (!encoded) return false;
            int length = 0;
            char *decoded = curl_easy_unescape(NULL, encoded + 1, 0, &length);
            extended = true;
            extended_match = decoded && length == (int)strlen(expected) &&
                             !memcmp(decoded, expected, (size_t)length);
            curl_free(decoded);
        } else {
            if (plain) return false;
            plain = true; plain_match = !strcmp(value, expected);
        }
        if (!*p) break;
    }
    return extended ? extended_match : plain && plain_match;
}
static bool controlled(Transfer *t) {
    /* These flags are atomic: a writer saving queue state must not block the
     * network callback on orbit.mutex. Capture probes still check source policy. */
    if (atomic_load(&orbit.stop) || atomic_load(&t->job->pause) || atomic_load(&t->job->cancel) ||
        (t->cancel_signal && atomic_load(t->cancel_signal))) return true;
    Transfer *owner = t->owner ? t->owner : t;
#ifdef ORBIT_BENCHMARK
    if (owner->deadline && seconds() >= owner->deadline) return true;
#endif
    if (transfer_writer_failed(owner->writer)) return true;
    if (t->capture_probe) {
        pthread_mutex_lock(&orbit.mutex);
        bool enabled = source_enabled_locked(t->release);
        pthread_mutex_unlock(&orbit.mutex);
        return !enabled;
    }
    return false;
}
static size_t headers(char *s, size_t a, size_t b, void *arg) {
    Transfer *t = arg;
    size_t n = a * b;
    if (n > 5 && !strncmp(s, "HTTP/", 5)) {
        char line[100];
        header_value(line, sizeof line, s, n);
        char *p = strchr(line, ' ');
        t->status = p ? strtol(p + 1, NULL, 10) : 0;
        t->length = t->start = t->end = t->total = -1;
        /* libcurl calls us for every response in a redirect chain. None of the
         * previous response's error or retry metadata describes the new one. */
        t->etag[0] = t->modified[0] = t->type[0] = t->error[0] = t->disposition[0] = 0;
        t->retry_at = 0;
        t->accepted = false;
    } else if (n > 15 && !strncasecmp(s, "Content-Length:", 15)) {
        t->length = strtoll(s + 15, NULL, 10);
    } else if (n > 14 && !strncasecmp(s, "Content-Range:", 14)) {
        long long x, y, z;
        if (sscanf(s + 14, " bytes %lld-%lld/%lld", &x, &y, &z) == 3) {
            t->start = x;
            t->end = y;
            t->total = z;
        }
    } else if (n > 5 && !strncasecmp(s, "ETag:", 5)) {
        header_value(t->etag, sizeof t->etag, s + 5, n - 5);
    } else if (n > 14 && !strncasecmp(s, "Last-Modified:", 14)) {
        header_value(t->modified, sizeof t->modified, s + 14, n - 14);
    } else if (n > 13 && !strncasecmp(s, "Content-Type:", 13)) {
        header_value(t->type, sizeof t->type, s + 13, n - 13);
        for (char *p = t->type; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else if (n > 9 && !strncasecmp(s, "Location:", 9) &&
               guarded(t)) {
        char location[8193], *base = NULL, *resolved = NULL;
        if (n - 9 >= sizeof location) return 0;
        header_value(location, sizeof location, s + 9, n - 9);
        CURLU *url = curl_url();
        curl_easy_getinfo(t->handle, CURLINFO_EFFECTIVE_URL, &base);
        bool allowed = url && base && !curl_url_set(url, CURLUPART_URL, base, 0) &&
            !curl_url_set(url, CURLUPART_URL, location, 0) &&
            !curl_url_get(url, CURLUPART_URL, &resolved, 0) && address_allowed(t, resolved);
        curl_free(resolved); curl_url_cleanup(url);
        if (!allowed) {
            copy_text(t->error, sizeof t->error, "Provider redirected to an unsupported download host.");
            return 0;
        }
    } else if (n > 20 && !strncasecmp(s, "Content-Disposition:", 20)) {
        header_value(t->disposition, sizeof t->disposition, s + 20, n - 20);
    } else if (n > 12 && !strncasecmp(s, "Retry-After:", 12)) {
        char v[100];
        header_value(v, sizeof v, s + 12, n - 12);
        char *end = NULL;
        long long wait = strtoll(v, &end, 10);
        time_t now = time(NULL);
        t->retry_at = (end && !*end && wait >= 0 && wait < 31536000) ? now + (time_t)wait
                                                                     : curl_getdate(v, NULL);
    } else if ((n == 2 && s[0] == '\r') || (n == 1 && s[0] == '\n')) {
        /* Redirects commonly have an HTML body. Validate the file response,
         * after libcurl follows Location, rather than rejecting that page. */
        if (t->probe || (t->status >= 100 && t->status < 200) ||
            (t->status >= 300 && t->status < 400))
            return n;
        if (t->status == 429 || t->status == 500 || t->status == 502 ||
            t->status == 503 || t->status == 504) {
            copy_text(t->error, sizeof t->error, "The download provider is temporarily unavailable.");
            return 0;
        }
        if (!t->capture_probe && guarded(t) && (t->status == 401 || t->status == 403 || t->status == 404 || t->status == 410)) {
            copy_text(t->error, sizeof t->error, torbox(t)
                ? "TorBox rejected the download link. Resume to request a fresh link. Your partial file is kept."
                : remote_transfer(t) ? "The community catalogue download link was rejected."
                : direct_transfer(t) ? "The download URL was rejected. Check that the link is still valid."
                : "The saved download link was rejected. Choose Refresh download link to verify with the provider again. Your partial file is kept.");
            return 0;
        }
        if (t->capture_probe && t->status != 200 && t->status != 206) {
            snprintf(t->error, sizeof t->error, "Provider rejected file verification (HTTP %ld). Open its download page again.", t->status);
            return 0;
        }
        if (t->status == 200 && t->offset == 0 && t->limit == t->release->size &&
            t->length == t->release->size && !t->owner)
            t->accepted = true;
        if (t->status == 206 && t->start == t->offset && t->total == t->release->size &&
            t->end == t->limit - 1 && t->length == t->limit - t->offset)
            t->accepted = true;
        if ((t->accepted || (t->range_probe && t->status == 200)) &&
            ((t->job->etag[0] && strcmp(t->job->etag, t->etag)) ||
             (!t->job->etag[0] && t->job->modified[0] && strcmp(t->job->modified, t->modified)))) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error,
                      "Source changed during the request. Partial file preserved.");
        }
        if (strcmp(t->release->id, "orbit-network-check") &&
            (strstr(t->type, "text/") || strstr(t->type, "json") || strstr(t->type, "xml"))) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error,
                      "Provider returned a page instead of a downloadable file.");
        }
        bool generated_provider = !strcmp(t->release->source, "akirabox") ||
                                  !strcmp(t->release->source, "datanodes") || !strcmp(t->release->source, "1fichier");
        bool named = t->disposition[0] && disposition_named(t->disposition);
        if ((t->capture_probe && generated_provider && t->accepted && !named)) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error, "Provider did not identify the selected filename.");
        }
        /* Vikingfile's signed route already identifies the exact filename.
         * Its storage server may return only a bare attachment disposition. */
        bool viking_route = t->capture_probe && !torbox(t) && !strcmp(t->release->source, "vikingfile");
        bool full_probe = get_verified_file(t) && t->range_probe && t->status == 200;
        if ((t->capture_probe || torbox(t) || ((remote_transfer(t) || direct_transfer(t)) && named)) &&
            (t->accepted || full_probe) && t->disposition[0] && (named || !viking_route)) {
            if (!disposition_matches(t->disposition, t->release->filename)) {
                t->accepted = false;
                copy_text(t->error, sizeof t->error, "Provider returned a different filename.");
            }
        }
        if (t->range_probe && t->status == 200 && t->length == t->release->size && !t->error[0]) {
            /* Abort at headers; do not consume a full file for a one-byte probe. */
            t->unsupported_range = true;
            return 0;
        }
    }
    return n;
}
static size_t body(char *data, size_t a, size_t b, void *arg) {
    Transfer *t = arg;
    size_t n = a * b;
    if (t->status >= 300 && t->status < 400)
        return n;
    if (!t->accepted) {
        if (!t->error[0])
            copy_text(t->error, sizeof t->error,
                      "Invalid HTTP range or file size. Partial file preserved.");
        return 0;
    }
    if (controlled(t))
        return 0;
    if (t->offset + t->written + (int64_t)n > t->limit) {
        copy_text(t->error, sizeof t->error, "Provider sent more bytes than expected.");
        return 0;
    }
    if (t->range_probe) {
        t->written += (int64_t)n;
        return n;
    }
    /* USB ranges stay in a bounded memory window until the entire response
     * validates. Only adjacent, verified bytes are handed to the disk writer. */
    Transfer *owner = t->owner ? t->owner : t;
    owner->network_bytes += (int64_t)n;
    if (t->window_buffer) {
        memcpy(t->window_buffer + t->written, data, n);
        t->written += (int64_t)n;
        return n;
    }
    if (owner->error[0])
        return 0;
    double before_write = seconds();
    if (transfer_writer_enqueue(owner->writer, (unsigned)t->segment, data, n)) return 0;
    if (seconds() - before_write > 0.25)
        owner->health = (DownloadHealth){seconds(), owner->network_bytes};
    t->written += (int64_t)n;
    unsigned count = t->job->range_count;
    owner->live[t->segment] = t->offset + t->written -
                             range_boundary(transfer_span(t), count, (unsigned)t->segment);
#ifdef ORBIT_BENCHMARK
    owner->enqueued += (int64_t)n;
    if (owner->speed_samples && seconds() - owner->last_sample >= 1) {
        owner->last_sample = seconds();
        cJSON *sample = cJSON_CreateObject();
        cJSON_AddNumberToObject(sample, "seconds", owner->last_sample - owner->started);
        cJSON_AddNumberToObject(sample, "bytes", (double)owner->enqueued);
        cJSON_AddItemToArray(owner->speed_samples, sample);
    }
#endif
    return n;
}
static int progress(void *arg, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt;
    (void)dn;
    (void)ut;
    (void)un;
    Transfer *t = arg, *owner = t->owner ? t->owner : t;
    if (controlled(t)) return 1;
    if (!owner->probe && owner->network_bytes < transfer_span(owner) - owner->offset &&
        owner->policy.timeout && download_stalled(&owner->health, seconds(), owner->network_bytes, &owner->policy)) {
        owner->stalled = true;
        copy_text(owner->error, sizeof owner->error, "Download stayed below the minimum speed. Partial file preserved.");
        return 1;
    }
    return owner->stalled ? 1 : 0;
}
static int before_request(void *arg, char *remote, char *local, int remote_port, int local_port) {
    (void)remote; (void)local; (void)remote_port; (void)local_port;
    Transfer *t = arg;
    char *url = NULL;
    curl_easy_getinfo(t->handle, CURLINFO_EFFECTIVE_URL, &url);
    if (controlled(t)) return CURL_PREREQFUNC_ABORT;
    if (address_allowed(t, url)) return CURL_PREREQFUNC_OK;
    copy_text(t->error, sizeof t->error, "Provider returned an unsupported download address.");
    return CURL_PREREQFUNC_ABORT;
}
#if defined(ORBIT_BENCHMARK) || !defined(ORBIT_DESKTOP)
static int configure_receive_buffer(curl_socket_t fd, long requested, int *before, int *after) {
    int original=0, actual=0, error=0;
    socklen_t length=sizeof original;
    if (getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&original,&length)) error=errno;
    /* Never reduce a larger system buffer or fail a transfer if tuning is denied. */
    if (!error && requested>original) {
        int desired=(int)requested;
        if (setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&desired,sizeof desired)) error=errno;
    }
    length=sizeof actual;
    if (getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&actual,&length) && !error) error=errno;
    if(before) *before=original;
    if(after) *after=actual;
    return error;
}
static int transfer_socket(void *arg, curl_socket_t fd, curlsocktype purpose) {
    (void)purpose;
#ifdef ORBIT_BENCHMARK
    Transfer *t = arg, *owner = t->owner ? t->owner : t;
    t->socket_error=configure_receive_buffer(fd,owner->socket_buffer,&t->socket_before,&t->socket_after);
#else
    (void)arg;
    /* Console measurements: 64 KiB defaults restrict high-latency throughput.
     * This console accepts a 1 MiB request as 512 KiB; the OS owns its cap. */
    (void)configure_receive_buffer(fd,1024*1024,NULL,NULL);
#endif
    return CURL_SOCKOPT_OK;
}
#endif
static CURL *request(Transfer *t) {
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    t->handle = c;
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    /* Once a piece has been redirected, later pieces ask the storage server
     * directly instead of hitting the provider's link for every piece. */
    const Transfer *o = t->owner ? t->owner : t;
    t->via_direct = o->direct && o->direct[0];
    curl_easy_setopt(c, CURLOPT_URL, t->via_direct ? o->direct : t->release->url);
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ORBIT_TEST
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    const char *protocols = direct_transfer(t) && !strncasecmp(t->release->url, "http://", 7) ? "http,https" : "https";
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (t->owner || !t->probe) ? 0L : 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "OrbitStore/" ORBIT_VERSION);
    /* AkiraBox's file servers require the referring file-page origin. Keep
     * this identical for the one-byte check, every range and resumed requests.
     * Only the public origin is sent, never a signed URL or browser cookies. */
    if (!torbox(t) && !strcmp(t->release->source, "akirabox"))
        curl_easy_setopt(c, CURLOPT_REFERER, "https://akirabox.to/");
    /* Login-only Archive.org files need its session. The cookies are scoped to
     * archive.org over HTTPS, so no redirect elsewhere can carry them. */
    if (!torbox(t) && !strcmp(t->release->source, "archive"))
        archive_account_apply(c);
    if (!torbox(t) && !strcmp(t->release->source, "vikingfile")) {
        curl_easy_setopt(c, CURLOPT_USERAGENT, "Mozilla/5.0 OrbitStore/" ORBIT_VERSION);
        curl_easy_setopt(c, CURLOPT_REFERER, "https://vikingfile.com/");
        if (!viking_apply_download(c, t->release)) {
            copy_text(t->error, sizeof t->error, "Vikingfile sign-in is unavailable. Sign in again and refresh the download link.");
            curl_easy_cleanup(c); t->handle = NULL; return NULL;
        }
    }
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, t);
#if defined(ORBIT_BENCHMARK) || !defined(ORBIT_DESKTOP)
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, transfer_socket);
    curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, t);
#endif
#ifdef ORBIT_BENCHMARK
    Transfer *owner = t->owner ? t->owner : t;
    if (owner->curl_buffer) curl_easy_setopt(c, CURLOPT_BUFFERSIZE, owner->curl_buffer);
    if (owner->deadline) {
        long left = (long)((owner->deadline - seconds()) * 1000);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, left > 0 ? left : 1L);
    }
#endif
    if (guarded(t)) {
        curl_easy_setopt(c, CURLOPT_PROXY, "");
        curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, provider_open_socket);
        curl_easy_setopt(c, CURLOPT_PREREQFUNCTION, before_request);
        curl_easy_setopt(c, CURLOPT_PREREQDATA, t);
    }
    return c;
}
static void set_range(CURL *c, int64_t offset, int64_t limit, struct curl_slist *conditions) {
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", (long long)offset, (long long)(limit - 1));
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, conditions);
}
/* One bounded GET verifies actual range support, not an optional HEAD hint.
 * A valid full-file 200 means use the existing single-connection path. */
static int range_support(Transfer *owner, struct curl_slist *conditions, CURLcode *result) {
    Transfer probe = {
        .job = owner->job, .release = owner->release, .fd = -1, .limit = 1, .range_probe = true,
        .capture_probe = owner->capture_probe, .cancel_signal = owner->cancel_signal};
    CURL *c = request(&probe);
    if (!c) {
        copy_text(owner->error, sizeof owner->error, "Could not initialize range check.");
        return -1;
    }
    set_range(c, 0, 1, conditions);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    *result = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (probe.unsupported_range) {
        if (owner->probe && get_verified_file(owner)) {
            owner->status = probe.status;
            owner->length = probe.length;
            copy_text(owner->etag, sizeof owner->etag, probe.etag);
            copy_text(owner->modified, sizeof owner->modified, probe.modified);
        }
        *result = CURLE_OK;
        return 0;
    }
    if (*result == CURLE_OK && probe.accepted && probe.written == 1) {
        if (owner->probe && guarded(owner)) {
            owner->status = probe.status;
            owner->length = probe.total;
            copy_text(owner->etag, sizeof owner->etag, probe.etag);
            copy_text(owner->modified, sizeof owner->modified, probe.modified);
        }
        return 1;
    }
    owner->status = probe.status;
    owner->retry_at = probe.retry_at;
    if (probe.error[0])
        copy_text(owner->error, sizeof owner->error, probe.error);
    else
        snprintf(owner->error, sizeof owner->error, "Range check failed (HTTP %ld, %s).",
                 probe.status, curl_easy_strerror(*result));
    return -1;
}
int transfer_validate_capture(Release *release, atomic_bool *cancel, char *etag,
                              char *modified, char *error, size_t cap) {
    if (!provider_capture_url(release, release->url)) {
        copy_text(error, cap, "Captured link does not match this download option."); return -1;
    }
    Job job = {.release = *release};
    Transfer owner = {.job = &job, .release = release, .fd = -1, .probe = true,
                      .capture_probe = true, .cancel_signal = cancel};
    CURLcode rc = CURLE_OK;
    if (range_support(&owner, NULL, &rc) != 1) {
        copy_text(error, cap, owner.error[0] ? owner.error : "Provider did not return the expected file byte.");
        return -1;
    }
    bool strong = strong_etag(owner.etag);
    if (!strong && (!owner.modified[0] || curl_getdate(owner.modified, NULL) < 0)) {
        copy_text(error, cap, "The provider did not supply a stable file identity. Nothing was queued.");
        return -1;
    }
    copy_text(etag, 256, strong ? owner.etag : "");
    copy_text(modified, 128, owner.modified);
    return 0;
}
/* Shared validation/cancellation for full-file ranges and bounded USB windows. */
/* The address a piece was finally served from, kept for later pieces when it
 * isn't the provider's link and passes the same host checks. Memory only: a
 * resumed download asks the link again. */
static void remember_storage(Transfer *owner, CURL *handle) {
    char *url = NULL;
    if (curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url) != CURLE_OK || !url ||
        !strcmp(url, owner->release->url) || strlen(url) >= 8192 ||
        (guarded(owner) && !address_allowed(owner, url))) return;
    if (!owner->direct && !(owner->direct = malloc(8192))) return;
    copy_text(owner->direct, 8192, url);
}
static void forget_storage(Transfer *owner) {
    if (!owner->direct) return;
    OPENSSL_cleanse(owner->direct, 8192);
    free(owner->direct);
    owner->direct = NULL;
}
static CURLcode perform_ranges(Transfer *owner, Transfer *parts, CURL **handles,
                               CURLM *multi, unsigned count, struct curl_slist *conditions) {
    CURLcode result = CURLE_OK;
    time_t again[ORBIT_MAX_RANGES] = {0};
    unsigned attempts[ORBIT_MAX_RANGES];
    for (unsigned i = 0; i < count; i++) attempts[i] = owner->job->attempts;
    for (;;) {
        if (controlled(owner) || owner->error[0]) {
            result = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        bool waiting = false;
        for (unsigned i = 0; i < count; i++) if (again[i]) {
            waiting = true;
            if (time(NULL) < again[i]) continue;
            Transfer *part = &parts[i];
            curl_multi_remove_handle(multi, handles[i]);
            /* USB windows commit only after every response validates. Other
             * ranges continue after the bytes already handed to the writer. */
            if (!part->window_buffer) part->offset += part->written;
            part->written = 0; part->accepted = false; part->error[0] = 0;
            set_range(handles[i], part->offset, part->limit, conditions);
            if (curl_multi_add_handle(multi, handles[i]) != CURLM_OK) return CURLE_FAILED_INIT;
            again[i] = 0;
        }
        int running = 0;
        CURLMcode rc = curl_multi_perform(multi, &running);
        if (rc != CURLM_OK) {
            result = CURLE_FAILED_INIT;
            break;
        }
        int remaining;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &remaining))) {
            if (message->msg != CURLMSG_DONE)
                continue;
            for (unsigned i = 0; i < count; i++) {
                Transfer *part = &parts[i];
                if (message->easy_handle != handles[i])
                    continue;
                if (message->data.result != CURLE_OK || !part->accepted ||
                    part->offset + part->written != part->limit) {
                    /* The saved storage address expired or stopped answering:
                     * ask the provider's link for this piece once more. */
                    if (part->via_direct && !controlled(owner)) {
                        owner->direct[0] = 0;
                        part->via_direct = false;
                        curl_easy_setopt(handles[i], CURLOPT_URL, part->release->url);
                        again[i] = time(NULL);
                        waiting = true;
                        continue;
                    }
                    /* A valid 206 cut short can resume just this request.
                     * Changed identity, HTTP errors and disk failures instead
                     * take the job-level recovery path. Siblings keep running. */
                    bool short_read = part->accepted && part->status == 206 && !part->error[0] &&
                        part->offset + part->written < part->limit &&
                        (message->data.result == CURLE_OK || message->data.result == CURLE_PARTIAL_FILE ||
                         message->data.result == CURLE_RECV_ERROR) && (owner->job->etag[0] || owner->job->modified[0]);
                    pthread_mutex_lock(&orbit.mutex);
                    bool retry = short_read && attempts[i] < orbit.download_policy.retries &&
                        orbit.download_policy.delay <= 30;
                    if (retry) {
                        attempts[i]++;
                        if (owner->job->attempts < attempts[i]) owner->job->attempts = attempts[i];
                        again[i] = time(NULL) + orbit.download_policy.delay;
                        state_save_locked();
                    }
                    pthread_mutex_unlock(&orbit.mutex);
                    if (retry) { waiting = true; continue; }
                    if (part->retry_at > owner->retry_at)
                        owner->retry_at = part->retry_at;
                    /* A sibling cancelled after a failure must not replace the
                     * provider's throttling status with its callback error. */
                    if (result != CURLE_OK && part->status != 429 && part->status != 503)
                        continue;
                    result = message->data.result == CURLE_OK
                                 ? (part->accepted ? CURLE_PARTIAL_FILE : CURLE_RANGE_ERROR)
                                 : message->data.result;
                    owner->status = part->status;
                    if (!owner->error[0]) {
                        if (part->error[0])
                            copy_text(owner->error, sizeof owner->error, part->error);
                        else
                            snprintf(owner->error, sizeof owner->error,
                                     "Transfer failed (HTTP %ld, %s).", part->status,
                                     curl_easy_strerror(result));
                    }
                } else if (!part->via_direct)
                    remember_storage(owner, handles[i]);
            }
        }
        if (result != CURLE_OK || (!running && !waiting))
            break;
        if (!running && waiting) owner->health = (DownloadHealth){seconds(), owner->network_bytes};
        if (curl_multi_poll(multi, NULL, 0, 200, NULL) != CURLM_OK) {
            result = CURLE_FAILED_INIT;
            break;
        }
    }
    return result;
}
static CURLcode run_ranges(Transfer *owner, struct curl_slist *conditions) {
    /* Independent bounded requests on one network worker; a separate writer
     * coalesces their buffers and owns all writes/checkpoints. */
    CURLM *multi = curl_multi_init();
    unsigned count = owner->job->range_count;
    CURL *handles[ORBIT_MAX_RANGES] = {0};
    bool added[ORBIT_MAX_RANGES] = {false};
    Transfer parts[ORBIT_MAX_RANGES] = {0};
    CURLcode result = CURLE_OK;
    if (!multi) {
        copy_text(owner->error, sizeof owner->error, "Could not initialize parallel transfer.");
        return CURLE_FAILED_INIT;
    }
    const unsigned connections = owner->job->transfer_connections ? owner->job->transfer_connections : 1;
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)connections);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)connections);
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, 0L);
    for (unsigned i = 0; i < count; i++) {
        int64_t base = range_boundary(transfer_span(owner), count, i);
        int64_t limit = range_boundary(transfer_span(owner), count, i + 1);
        parts[i] = (Transfer){.job = owner->job,
                              .release = owner->release,
                              .fd = owner->fd,
                              .owner = owner,
                              .segment = i,
                              .offset = base + owner->live[i],
                              .limit = limit};
        if (parts[i].offset == limit)
            continue;
        handles[i] = request(&parts[i]);
        if (!handles[i]) {
            result = CURLE_FAILED_INIT;
            goto cleanup;
        }
        set_range(handles[i], parts[i].offset, limit, conditions);
    }
    for (unsigned first = 0; first < count; first += connections) {
        unsigned end = first + connections < count ? first + connections : count;
        for (unsigned i = first; i < end; i++) {
            if (!handles[i]) continue;
            if (curl_multi_add_handle(multi, handles[i]) != CURLM_OK) {
                result = CURLE_FAILED_INIT;
                goto cleanup;
            }
            added[i] = true;
        }
        result = perform_ranges(owner, parts, handles, multi, count, conditions);
        for (unsigned i = first; i < end; i++) {
            if (added[i]) curl_multi_remove_handle(multi, handles[i]);
            added[i] = false;
        }
        if (result != CURLE_OK) break;
    }
cleanup:
    for (unsigned i = 0; i < count; i++) {
#ifdef ORBIT_BENCHMARK
        if (handles[i] && owner->metrics) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddItemToArray(owner->metrics, m);
            cJSON_AddNumberToObject(m, "range", i);
            cJSON_AddNumberToObject(m, "bytes", (double)parts[i].written);
            cJSON_AddNumberToObject(m, "httpStatus", parts[i].status);
            cJSON_AddBoolToObject(m, "validated", parts[i].accepted);
            cJSON_AddNumberToObject(m, "socketBufferBefore", parts[i].socket_before);
            cJSON_AddNumberToObject(m, "socketBufferAfter", parts[i].socket_after);
            cJSON_AddNumberToObject(m, "socketOptionError", parts[i].socket_error);
            double value = 0;
            curl_easy_getinfo(handles[i], CURLINFO_NAMELOOKUP_TIME, &value);
            cJSON_AddNumberToObject(m, "dnsSeconds", value);
            curl_easy_getinfo(handles[i], CURLINFO_CONNECT_TIME, &value);
            cJSON_AddNumberToObject(m, "connectSeconds", value);
            curl_easy_getinfo(handles[i], CURLINFO_STARTTRANSFER_TIME, &value);
            cJSON_AddNumberToObject(m, "firstByteSeconds", value);
            char *url = NULL, *host = NULL;
            CURLU *parsed = curl_url();
            curl_easy_getinfo(handles[i], CURLINFO_EFFECTIVE_URL, &url);
            if (parsed && url && !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
                !curl_url_get(parsed, CURLUPART_HOST, &host, 0))
                cJSON_AddStringToObject(m, "server", host);
            curl_free(host); curl_url_cleanup(parsed);
        }
#endif
        if (added[i])
            curl_multi_remove_handle(multi, handles[i]);
        if (handles[i])
            curl_easy_cleanup(handles[i]);
    }
    curl_multi_cleanup(multi);
    if (result != CURLE_OK && !owner->error[0])
        snprintf(owner->error, sizeof owner->error, "Transfer interrupted: %s.",
                 curl_easy_strerror(result));
    return result;
}

/* exFAT USB writes cannot cheaply leave holes. Requesting eighths of a large
 * game writes tens of GB ahead and can block the entire volume while the gap
 * is allocated. Keep eight network requests, but bound their combined span to
 * 8 MiB and feed the existing writer in ascending order. The multi handle keeps
 * connections reusable across windows. Memory is capped at 8 MiB in addition
 * to the writer's 8 MiB queue; checkpoints still cover flushed bytes only. */
static bool usb_destination(const Job *j) {
#ifdef ORBIT_TEST
    const char *usb = getenv("ORBIT_TEST_USB_STORAGE");
    if (usb && !strcmp(usb, "1")) return true;
#endif
    return !strncmp(j->storage_id, "usb", 3) && strlen(j->storage_id) == 4 &&
           j->storage_id[3] >= '0' && j->storage_id[3] <= '7' &&
           !strncmp(j->root, "/mnt/usb", 8) && strlen(j->root) == 9 &&
           j->root[8] == j->storage_id[3];
}
static CURLcode run_usb_ranges(Transfer *owner, struct curl_slist *conditions) {
    const unsigned connections = owner->job->transfer_connections ? owner->job->transfer_connections : 1;
    /* Each request's window shares the 8 MiB, and fits a saved range's writer
     * buffer; retries send fewer requests, not bigger ones. */
    const unsigned most = owner->connections > connections ? owner->connections : connections;
    const unsigned share = most > owner->job->range_count ? most : owner->job->range_count;
    const size_t chunk = TRANSFER_BUFFER_BYTES / share;
    unsigned char *memory = malloc(TRANSFER_BUFFER_BYTES);
    CURLM *multi = curl_multi_init();
    CURLcode result = CURLE_OK;
    if (!memory || !multi) { result = CURLE_OUT_OF_MEMORY; goto done; }
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)connections);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)connections);
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, 0L);
    /* Keep old segmented partials intact. Fill each saved range from its own
     * durable cursor; never reinterpret st_size as downloaded progress. */
    for (unsigned segment = 0; segment < owner->job->range_count; segment++) {
        int64_t base = range_boundary(transfer_span(owner), owner->job->range_count, segment);
        int64_t limit = range_boundary(transfer_span(owner), owner->job->range_count, segment + 1);
        int64_t cursor = base + owner->live[segment];
        while (cursor < limit) {
            if (controlled(owner)) { result = CURLE_ABORTED_BY_CALLBACK; goto done; }
            CURL *handles[ORBIT_MAX_RANGES] = {0};
            bool added[ORBIT_MAX_RANGES] = {false};
            Transfer parts[ORBIT_MAX_RANGES] = {0};
            unsigned count = 0;
            int64_t next = cursor;
            for (; count < connections && next < limit; count++) {
                int64_t end = limit - next < (int64_t)chunk ? limit : next + (int64_t)chunk;
                parts[count] = (Transfer){.job = owner->job, .release = owner->release,
                    .fd = owner->fd, .owner = owner, .segment = (int)segment,
                    .offset = next, .limit = end, .window_buffer = memory + count * chunk};
                handles[count] = request(&parts[count]);
                if (!handles[count]) { result = CURLE_FAILED_INIT; break; }
                set_range(handles[count], next, end, conditions);
                if (curl_multi_add_handle(multi, handles[count]) != CURLM_OK) {
                    result = CURLE_FAILED_INIT; break;
                }
                added[count] = true;
                next = end;
            }
            if (result == CURLE_OK)
                result = perform_ranges(owner, parts, handles, multi, count, conditions);
            for (unsigned i = 0; i < ORBIT_MAX_RANGES; i++) {
                if (added[i]) curl_multi_remove_handle(multi, handles[i]);
                if (handles[i]) curl_easy_cleanup(handles[i]);
            }
            if (result != CURLE_OK) goto done;
            double before_write = seconds();
            for (unsigned i = 0; i < count; i++) {
                if (transfer_writer_enqueue(owner->writer, segment, parts[i].window_buffer,
                                            (size_t)parts[i].written)) {
                    result = CURLE_ABORTED_BY_CALLBACK; goto done;
                }
                owner->live[segment] += parts[i].written;
            }
            if (seconds() - before_write > 0.25)
                owner->health = (DownloadHealth){seconds(), owner->network_bytes};
            cursor = next;
        }
        /* A prior layout can have no saved bytes in its next range. Finish
         * pending writes before crossing that boundary, even for a slow disk. */
        if (transfer_writer_drain(owner->writer)) {
            result = CURLE_ABORTED_BY_CALLBACK; goto done;
        }
    }
done:
    if (multi) curl_multi_cleanup(multi);
    free(memory);
    if (result != CURLE_OK && !owner->error[0])
        snprintf(owner->error, sizeof owner->error, "Transfer interrupted: %s.", curl_easy_strerror(result));
    return result;
}

#ifdef ORBIT_BENCHMARK
static void sample_hash(int fd, int64_t bytes, cJSON *out) {
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    if (!ctx) return;
    bool ok=EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)==1;
    unsigned char block[65536], hash[32];
    int64_t offset=0;
    while(ok && offset<bytes) {
        size_t amount=bytes-offset<(int64_t)sizeof block ? (size_t)(bytes-offset) : sizeof block;
        ssize_t n=pread(fd,block,amount,offset);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { ok=false; break; }
        ok=EVP_DigestUpdate(ctx,block,(size_t)n)==1; offset+=n;
    }
    unsigned length=0;
    if(ok) ok=EVP_DigestFinal_ex(ctx,hash,&length)==1 && length==32;
    EVP_MD_CTX_free(ctx);
    if(ok) {
        char hex[65];
        for(unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",hash[i]);
        cJSON_AddStringToObject(out,"sampleSha256",hex);
    }
}
/* Bounded prefix sampling with the production request/validation/range engine,
 * writer and state checkpoints. Only the isolated benchmark ELF exposes this.
 * The caller owns an exclusive temporary fd and an isolated Orbit state directory.
 * release.size remains the real remote size for HTTP validation; job.total is
 * the bounded local prefix so tail ranges never create a game-sized sparse file. */
cJSON *transfer_benchmark(Job *j, int fd, unsigned count, int64_t bytes, unsigned duration,
                          long socket_buffer, long curl_buffer) {
    cJSON *out = cJSON_CreateObject();
    Transfer t = {.job=j, .release=&j->release, .fd=fd, .limit=j->release.size,
                  .length=-1, .probe=true, .socket_buffer=socket_buffer, .curl_buffer=curl_buffer};
    CURLcode rc = CURLE_OK;
    struct curl_slist *conditions = NULL;
    double began = seconds();
    if ((count != 2 && count != 4 && count != 8 && count != 16) || bytes < 1024*1024 || bytes > 512LL*1024*1024 ||
        bytes >= j->release.size || duration < 1 || duration > 90 ||
        (socket_buffer && socket_buffer != 256*1024 && socket_buffer != 1024*1024) ||
        (curl_buffer && curl_buffer != 256*1024) || !provider_supported(&j->release)) {
        copy_text(t.error, sizeof t.error, "Invalid bounded benchmark configuration.");
        goto finish_benchmark;
    }
    if (provider_browser_source(j->release.source)) {
        if (range_support(&t, NULL, &rc) != 1) goto finish_benchmark;
    } else {
        CURL *c = request(&t);
        if (!c) { rc = CURLE_FAILED_INIT; goto finish_benchmark; }
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK || t.status != 200) goto finish_benchmark;
    }
    if (t.length != j->release.size || !strong_etag(t.etag)) {
        copy_text(t.error, sizeof t.error, "Benchmark requires the expected file size and strong ETag.");
        goto finish_benchmark;
    }
    copy_text(j->etag, sizeof j->etag, t.etag);
    char identity[300]; snprintf(identity, sizeof identity, "If-Match: %s", t.etag);
    conditions = curl_slist_append(NULL, identity);
    if (!conditions) { rc = CURLE_OUT_OF_MEMORY; goto finish_benchmark; }
    if (!strcmp(j->release.source, "archive") && range_support(&t, conditions, &rc) != 1)
        goto finish_benchmark;
    j->range_count = count; j->segmented = true; j->total = bytes; j->transfer_connections = count;
    t.sample_span = bytes; t.probe = false;
    if (fsync(fd) || state_save_locked()) {
        copy_text(t.error, sizeof t.error, "Benchmark state could not be saved.");
        goto finish_benchmark;
    }
    cJSON_AddNumberToObject(out, "preflightSeconds", seconds() - began);
    t.started = seconds(); t.deadline = t.started + duration;
    t.writer = transfer_writer_start(j, fd, t.started, t.error, sizeof t.error);
    if (!t.writer) goto finish_benchmark;
    t.metrics = cJSON_AddArrayToObject(out, "ranges");
    t.speed_samples = cJSON_AddArrayToObject(out, "speedSamples");
    t.last_sample = t.started;
    cJSON *initial_sample = cJSON_CreateObject();
    cJSON_AddNumberToObject(initial_sample, "seconds", 0);
    cJSON_AddNumberToObject(initial_sample, "bytes", 0);
    cJSON_AddItemToArray(t.speed_samples, initial_sample);
    rc = run_ranges(&t, conditions);
    /* curl's millisecond timeout is rounded down when handles are prepared. */
    cJSON_AddBoolToObject(out, "timeLimitReached", seconds() >= t.deadline ||
        (rc == CURLE_OPERATION_TIMEDOUT && seconds() + .01 >= t.deadline));
    cJSON_AddNumberToObject(out, "networkSeconds", seconds() - t.started);
    cJSON *last_sample = cJSON_CreateObject();
    cJSON_AddNumberToObject(last_sample, "seconds", seconds() - t.started);
    cJSON_AddNumberToObject(last_sample, "bytes", (double)t.enqueued);
    cJSON_AddItemToArray(t.speed_samples, last_sample);
    bool writer_ok = transfer_writer_finish(t.writer, t.error, sizeof t.error) == 0;
    cJSON_AddBoolToObject(out, "writerSucceeded", writer_ok);
    if (!writer_ok) rc = CURLE_WRITE_ERROR;
    t.writer = NULL;
    cJSON_AddNumberToObject(out, "transferSeconds", seconds() - t.started);
    cJSON_AddNumberToObject(out, "MBps", j->received / (seconds() - t.started) / 1000000.0);
    if(rc==CURLE_OK && j->received==bytes) sample_hash(fd,bytes,out);
finish_benchmark:
    curl_slist_free_all(conditions);
    cJSON_AddStringToObject(out, "error", t.error);
    cJSON_AddNumberToObject(out, "curlCode", rc);
    cJSON_AddNumberToObject(out, "preflightStatus", t.status);
    cJSON_AddNumberToObject(out, "retryAt", (double)t.retry_at);
    cJSON_AddNumberToObject(out, "bytes", (double)j->received);
    cJSON_AddNumberToObject(out, "connections", count);
    cJSON_AddNumberToObject(out, "socketBufferRequested", socket_buffer);
    cJSON_AddNumberToObject(out, "curlBufferRequested", curl_buffer);
    cJSON_AddNumberToObject(out, "peakBufferedBytes", (double)j->transfer_stats.peak_buffered);
    cJSON_AddNumberToObject(out, "bufferWaitSeconds", j->transfer_stats.buffer_wait_seconds);
    cJSON_AddNumberToObject(out, "writeSeconds", j->transfer_stats.write_seconds);
    cJSON_AddNumberToObject(out, "syncSeconds", j->transfer_stats.sync_seconds);
    cJSON_AddNumberToObject(out, "writeCalls", (double)j->transfer_stats.write_calls);
    cJSON_AddNumberToObject(out, "syncCalls", (double)j->transfer_stats.sync_calls);
    forget_storage(&t);
    return out;
}
#endif
static bool file_type_matches(const Transfer *t) {
    unsigned char sample[8];
    ssize_t size = pread(t->fd, sample, sizeof sample, 0);
    return size == (ssize_t)sizeof sample && download_file_magic(t->release, sample, (size_t)size);
}
static bool checksum(Transfer *t) {
    if (!t->release->sha256[0])
        return true;
    if (lseek(t->fd, 0, SEEK_SET) < 0)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    unsigned char block[65536]; /* Each download worker owns its checksum buffer. */
    unsigned char digest[32] = {0};
    unsigned length = 0;
    while (ok) {
        ssize_t n = read(t->fd, block, sizeof block);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        if (controlled(t)) {
            ok = false;
            break;
        }
        ok = EVP_DigestUpdate(ctx, block, (size_t)n) == 1;
    }
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, digest, &length) == 1;
    EVP_MD_CTX_free(ctx);
    char hex[65];
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return ok && length == 32 && !strcmp(hex, t->release->sha256);
}
static unsigned retry_source(const Job *j) {
    if (!strcmp(j->delivery, "torbox")) return 5;
    if (!strcmp(j->release.source, "vikingfile")) return 1;
    if (!strcmp(j->release.source, "akirabox")) return 2;
    if (!strcmp(j->release.source, "datanodes")) return 3;
    if (!strcmp(j->release.source, "1fichier")) return 4;
    if (!strcmp(j->release.source, "remote")) return 6;
    return 0;
}
static bool retryable(long status, CURLcode rc) {
    return status == 408 || status == 429 || status == 500 || status == 503 || status == 502 || status == 504 ||
           rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
           rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_RECV_ERROR || rc == CURLE_PARTIAL_FILE ||
           rc == CURLE_SEND_ERROR || rc == CURLE_GOT_NOTHING;
}
typedef struct {
    Job *job;
    double started;
    int64_t sampled;
} ExtractionProgress;
static void extraction_progress(void *context, int64_t written) {
    ExtractionProgress *p = context;
    double now = seconds(), elapsed = now - p->started;
    pthread_mutex_lock(&orbit.mutex);
    p->job->extracted = written;
    if (written < p->sampled) { p->sampled = 0; p->started = now; p->job->speed = 0; }
    else if (elapsed >= 0.5) {
        p->job->speed = (double)(written - p->sampled) / elapsed;
        p->sampled = written; p->started = now;
    }
    pthread_mutex_unlock(&orbit.mutex);
#ifdef ORBIT_TEST
    /* Lets tests pause, cancel or stop Orbit while an extraction is running. */
    const char *delay = getenv("ORBIT_TEST_EXTRACT_DELAY_MS");
    if (delay) {
        struct timespec pause = {.tv_sec = atoi(delay) / 1000, .tv_nsec = (atoi(delay) % 1000) * 1000000L};
        nanosleep(&pause, NULL);
    }
#endif
}
/* Unpacking needs room for the archive's files beside what other downloads still need.
 * Checked just before it starts, with the drive's own free space; 3 when it doesn't fit. */
static int unpack_room(Job *j, int dir, uint64_t bytes, char *error, size_t cap) {
    struct statvfs fs;
    uint64_t free_bytes = fstatvfs(dir, &fs) ? 0 : (uint64_t)fs.f_bavail * (fs.f_frsize ? fs.f_frsize : fs.f_bsize);
#ifdef ORBIT_TEST
    const char *limited = getenv("ORBIT_TEST_EXTRACT_FREE_BYTES");
    if (limited) free_bytes = strtoull(limited, NULL, 10);
#endif
    pthread_mutex_lock(&orbit.mutex);
    bool fits = job_unpack_fits_locked(j, free_bytes, bytes);
    pthread_mutex_unlock(&orbit.mutex);
    if (fits) return 0;
    copy_text(error, cap, "Not enough free space to unpack this download. Free up space on its drive, then select Resume. The downloaded archive was kept.");
    return 3;
}
/* The verified archive is still `partial`. Extract it, then delete it or keep it
 * under its own name, as the setting asks. 0 when complete, 1 when paused,
 * 3 when there is no room to unpack it (the archive is kept). */
static int extract_job(Job *j, int dir, const char *partial, char *note, size_t note_cap,
                       char *error, size_t cap) {
    char path[540], folder[160];
    snprintf(path, sizeof path, "%s/homebrew", j->root);
    /* Several top-level entries go into one folder named after the archive. */
    copy_text(folder, sizeof folder, j->filename);
    char *dot = strrchr(folder, '.');
    if (dot && dot != folder) *dot = 0;
    pthread_mutex_lock(&orbit.mutex);
    copy_text(j->status, sizeof j->status, "waiting-extraction");
    j->extracted = 0;
    j->archive_locked = false;
    j->speed = 0;
    j->output[0] = 0;
    bool keep = !orbit.delete_archives;
    state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    ExtractionProgress progress = {.job = j};
    Extraction x = {.dir = dir, .path = path, .archive = partial, .job_id = j->id,
                    .compression = j->release.compression, .folder = folder,
                    .password = j->archive_password[0] ? j->archive_password : j->release.archive_password,
                    .expected = j->release.extracted_size, .pause = &j->pause,
                    .cancel = &j->cancel, .stop = &orbit.stop,
                    .progress = extraction_progress, .context = &progress};
    while (pthread_mutex_trylock(&extraction_mutex)) {
        if (j->pause || j->cancel || orbit.stop) return 1;
        struct timespec wait = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&wait, NULL);
    }
    pthread_mutex_lock(&orbit.mutex);
    copy_text(j->status, sizeof j->status, j->release.archive_unverified ? "inspecting" : "extracting");
    state_save_locked();
    diagnostics_event_locked("extract-start", j, 0, 0);
    pthread_mutex_unlock(&orbit.mutex);
    int rc = 0;
    bool remote = !strcmp(j->release.source, "remote");
    if (j->release.archive_unverified || remote || !strcmp(j->release.source, ORBIT_DIRECT_SOURCE)) {
        int64_t inspected = 0;
        char format[12];
        rc = inspect_archive(&x, &inspected, format, error, cap);
        if (!rc && remote) {
            const char *expected = !strcmp(j->release.format, "FPKG") ? "PKG" : j->release.format;
            if (strcmp(format, expected) || (x.expected > 0 && x.expected != inspected)) {
                copy_text(error, cap, "Archive contents do not match the community catalogue.");
                rc = -1;
            }
        }
        if (!rc) {
            pthread_mutex_lock(&orbit.mutex);
            j->inspected_size = inspected;
            copy_text(j->inspected_format, sizeof j->inspected_format, format);
            state_save_locked();
            pthread_mutex_unlock(&orbit.mutex);
            rc = unpack_room(j, dir, inspected > 0 ? (uint64_t)inspected : 0, error, cap);
            if (!rc) {
                pthread_mutex_lock(&orbit.mutex);
                x.expected = inspected;
                copy_text(j->status, sizeof j->status, "extracting");
                state_save_locked();
                pthread_mutex_unlock(&orbit.mutex);
            }
        }
    } else if (j->release.extracted_size > 0) {
        rc = unpack_room(j, dir, (uint64_t)j->release.extracted_size, error, cap);
    }
    if (!rc) { progress.started = seconds(); rc = extract_archive(&x, error, cap); }
    pthread_mutex_unlock(&extraction_mutex);
    if (rc) {
        pthread_mutex_lock(&orbit.mutex);
        j->extracted = 0; /* The staging folder is gone; nothing is reserved by it. */
        j->archive_locked = rc == -2; /* only then is a password worth asking for */
        pthread_mutex_unlock(&orbit.mutex);
        return rc;
    }
    uint32_t required = !strcmp(j->release.platform, "ps5") ? firmware_extracted(dir, x.output) : 0;
    bool backport = !strcmp(j->release.platform, "ps5") && firmware_backport(dir, x.output);
    bool kept = keep && !extract_publish_file(dir, partial, dir, j->filename);
    if (!keep) unlinkat(dir, partial, 0);
    else if (!kept)
        snprintf(note, note_cap, "Extracted. %s was in use, so the archive was kept as %s.",
                 j->filename, partial);
    fsync(dir);
    pthread_mutex_lock(&orbit.mutex);
    copy_text(j->output, sizeof j->output, x.output);
    j->extracted_firmware = required;
    j->extracted_backport = backport;
    j->archive_kept = kept;
    j->archive_ready = false;
    pthread_mutex_unlock(&orbit.mutex);
    return 0;
}
static int prepare_torbox(Transfer *t, const Release *route) {
    Job *j = t->job;
    pthread_mutex_lock(&orbit.mutex);
    copy_text(j->delivery_phase, sizeof j->delivery_phase, "Preparing with TorBox");
    pthread_mutex_unlock(&orbit.mutex);
    int prepared = debrid_resolve(j, route, t->release, &t->retry_at, t->error, sizeof t->error);
    if (prepared == 0 && t->error[0]) return -1;
    if (prepared == 1) {
        t->limit = t->release->size;
        pthread_mutex_lock(&orbit.mutex);
        copy_text(j->delivery_phase, sizeof j->delivery_phase, "Downloading via TorBox");
        pthread_mutex_unlock(&orbit.mutex);
    }
    return prepared;
}

static bool fall_back_to_provider_locked(Job *j) {
    bool browser = job_needs_browser(&j->release);
    if ((!browser && !provider_supported(&j->release)) || orbit.state_failed ||
        job_delete_partial_locked(j, j->error, sizeof j->error)) return false;
    copy_text(j->delivery, sizeof j->delivery, "direct");
    j->delivery_phase[0] = j->debrid_account[0] = 0;
    j->debrid_id = j->debrid_file = -1;
    j->debrid_submitted = false;
    j->debrid_submitted_at = 0;
    j->debrid_progress = 0;
    j->mirror_id[0] = j->mirror_first[0] = 0;
    j->mirror_torbox = false;
    j->browser_pending = browser;
    j->attempts = 0;
    j->retry_at = 0;
    j->error[0] = 0;
    copy_text(j->status, sizeof j->status, "queued");
    return true;
}

/* The queued release keeps its original identity and output path. Only the
 * request URL changes, after rechecking the alternate against that file. */
static bool prepare_mirror(Transfer *t, Release *candidate, bool *source_failed) {
    Job *j = t->job;
    pthread_mutex_lock(&orbit.mutex);
    bool found = true;
    if (j->mirror_id[0]) found = remote_mirror_locked(&j->release, j->mirror_id, candidate);
    else *candidate = j->release;
    pthread_mutex_unlock(&orbit.mutex);
    if (!found) {
        *source_failed = true;
        copy_text(t->error, sizeof t->error, "This mirror is no longer available.");
        return false;
    }
    if (torbox(t)) return true;
    Release locator;
    int checked = remote_page_pending(candidate)
        ? remote_download_resolve(candidate, &locator, &j->cancel, t->error, sizeof t->error)
        : remote_download_verify(candidate, candidate->url, &j->cancel, t->error, sizeof t->error);
    if (checked || !remote_mirror_file(t->release, candidate)) {
        *source_failed = true;
        if (!t->error[0]) copy_text(t->error, sizeof t->error, "The mirror did not return the selected file.");
        return false;
    }
    if (!j->filename[0]) {
        Release file = *t->release;
        copy_text(file.filename, sizeof file.filename, candidate->filename);
        copy_text(file.format, sizeof file.format, candidate->format);
        copy_text(file.compression, sizeof file.compression, candidate->compression);
        file.size = candidate->size;
        pthread_mutex_lock(&orbit.mutex);
        bool assigned = job_assign_remote_file_locked(j, &file, t->error, sizeof t->error);
        pthread_mutex_unlock(&orbit.mutex);
        if (!assigned) return false;
        *t->release = file;
        t->limit = file.size;
    }
    copy_text(t->release->url, sizeof t->release->url, candidate->url);
    copy_text(t->release->filename, sizeof t->release->filename, candidate->filename);
    return true;
}

/* A partial file only belongs to the URL that wrote it. Clear its validators
 * and range map along with its bytes before switching mirrors. */
static bool advance_mirror_locked(Job *j) {
    Release candidate;
    if (j->archive_ready) return false;
    bool original_next = j->mirror_first[0] && !strcmp(j->mirror_id, j->mirror_first);
    if (original_next) {
        candidate = j->release;
    } else {
        char after[sizeof j->mirror_id];
        copy_text(after, sizeof after, j->mirror_id);
        do {
            if (!remote_next_mirror_locked(&j->release, after, &candidate)) return false;
            copy_text(after, sizeof after, candidate.id);
        } while (!strcmp(candidate.id, j->mirror_first));
    }
    if (job_delete_partial_locked(j, j->error, sizeof j->error)) return false;
    copy_text(j->mirror_id, sizeof j->mirror_id, original_next ? "" : candidate.id);
    j->mirror_torbox |= !strcmp(j->delivery, "torbox");
    if (!j->mirror_torbox || debrid_prepare_locked(&candidate, j, j->error, sizeof j->error) != 200)
        copy_text(j->delivery, sizeof j->delivery, "direct");
    j->debrid_submitted = false;
    j->debrid_submitted_at = 0;
    j->debrid_progress = 0;
    j->error[0] = j->delivery_phase[0] = 0;
    j->attempts = 0;
    j->retry_at = 0;
    copy_text(j->status, sizeof j->status, "queued");
    return true;
}

static void run_job(Job *j) {
    Release resolved = j->release;
    Release *r = &resolved;
    Transfer t = {.job = j,
                  .release = r,
                  .fd = -1,
                  .length = -1,
                  .start = -1,
                  .end = -1,
                  .total = -1,
                  .limit = r->size,
                  .probe = true};
    memcpy(t.live, j->range_received, sizeof t.live);
    pthread_mutex_lock(&orbit.mutex);
    t.policy = orbit.download_policy;
    pthread_mutex_unlock(&orbit.mutex);
    int dir = -1;
    char partial[192], note[256] = "";
    CURLcode rc = CURLE_OK;
    CURL *c = NULL;
    struct curl_slist *conditions = NULL;
    bool done = false, debrid_waiting = false, source_failed = false;
    int unpacked = -1; /* extract_job(): 3 when there was no room to unpack */
    if (controlled(&t)) goto finish;
    pthread_mutex_lock(&orbit.mutex);
    /* A drive the console remounted is recognised; an unmarked one gets its mark. */
    if (storage_rebind_locked(j) && storage_mark_locked(j)) (void)state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    Release mirror;
    const Release *route = &j->release;
    if ((j->mirror_id[0] || j->mirror_first[0]) && !j->archive_ready) {
        if (!prepare_mirror(&t, &mirror, &source_failed)) goto finish;
        if (j->mirror_id[0]) route = &mirror;
    }
    bool early_torbox = torbox(&t) && remote_transfer(&t) && (!r->filename[0] || !r->size);
    if (early_torbox) {
        if (!storage_matches(j)) {
            copy_text(t.error, sizeof t.error, "Destination is missing or not writable.");
            goto finish;
        }
        int prepared = prepare_torbox(&t, route);
        if (prepared != 1) {
            debrid_waiting = prepared == 0;
            source_failed = !debrid_waiting;
            goto finish;
        }
        if (controlled(&t)) goto finish;
    }
    if (remote_transfer(&t) && !get_verified_file(&t) && !torbox(&t) && !j->archive_ready) {
        c = request(&t);
        if (!c) {
            copy_text(t.error, sizeof t.error, "Could not initialize HTTPS.");
            goto finish;
        }
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        c = NULL;
        if (rc != CURLE_OK || t.status != 200 || t.length <= 0 || t.length > 9007199254740991LL ||
            strstr(t.type, "text/") || strstr(t.type, "json") || strstr(t.type, "xml")) {
            source_failed = true;
            copy_text(t.error, sizeof t.error, "Could not verify the community catalogue download.");
            goto finish;
        }
        if (r->size && r->size != t.length) {
            source_failed = true;
            copy_text(t.error, sizeof t.error, "Source size changed. Catalogue needs verification.");
            goto finish;
        }
        if (!r->size) {
            pthread_mutex_lock(&orbit.mutex);
            Storage drives[ORBIT_MAX_STORAGE];
            size_t count = storage_list(drives);
            const Storage *target = NULL;
            for (size_t i = 0; i < count; i++)
                if (!strcmp(drives[i].id, j->storage_id) && drives[i].device == j->device &&
                    drives[i].inode == j->inode) { target = &drives[i]; break; }
            if (!target || !job_space_fits_locked(target, (uint64_t)t.length)) {
                pthread_mutex_unlock(&orbit.mutex);
                copy_text(t.error, sizeof t.error, target ? "Not enough free space to finish this download."
                    : "Destination is missing or not writable.");
                goto finish;
            }
            j->total = j->release.size = r->size = t.limit = t.length;
            int saved = state_save_locked();
            if (saved) j->total = j->release.size = r->size = t.limit = 0;
            pthread_mutex_unlock(&orbit.mutex);
            if (saved) {
                copy_text(t.error, sizeof t.error, "Could not persist source identity.");
                goto finish;
            }
        }
        if (controlled(&t)) goto finish;
    }
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    dir = storage_open(j);
    if (dir < 0 || storage_check_writable(dir)) {
        copy_text(t.error, sizeof t.error, "Destination is missing or not writable.");
        goto finish;
    }
    struct stat st;
    if (j->archive_ready) {
        /* Downloaded and verified earlier; only the extraction is repeated. */
        int archive = openat(dir, partial, O_RDONLY | O_NOFOLLOW);
        bool intact = archive >= 0 && !fstat(archive, &st) && S_ISREG(st.st_mode) && st.st_size == r->size;
        if (archive >= 0) close(archive);
        if (!intact) {
            copy_text(t.error, sizeof t.error,
                      "The downloaded archive is missing or changed. Delete it and download again.");
            goto finish;
        }
        unpacked = extract_job(j, dir, partial, note, sizeof note, t.error, sizeof t.error);
        done = !unpacked;
        goto finish;
    }
    if (fstatat(dir, j->filename, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        copy_text(t.error, sizeof t.error,
                  "Destination file already exists. It will not be overwritten.");
        goto finish;
    }
    t.fd = openat(dir, partial, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (t.fd < 0 || fstat(t.fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        copy_text(t.error, sizeof t.error, "Cannot open a safe partial file.");
        goto finish;
    }
    /* O_CREAT may have replaced a manually deleted/renamed partial. An empty
     * file has no bytes to resume, even if the queue still has durable cursors.
     * Never infer this from the queue or reset a nonempty file: sparse tails
     * and intact partials must retain their original ranges and identity. */
    if (!st.st_size && (j->segmented || j->received || j->etag[0] || j->modified[0])) {
        pthread_mutex_lock(&orbit.mutex);
        j->received = 0;
        j->segmented = false;
        j->range_count = 0;
        memset(j->range_received, 0, sizeof j->range_received);
        j->etag[0] = j->modified[0] = 0;
        int saved = state_save_locked();
        pthread_mutex_unlock(&orbit.mutex);
        memset(t.live, 0, sizeof t.live);
        if (saved) {
            copy_text(t.error, sizeof t.error, "Could not persist the download ranges.");
            goto finish;
        }
    }
    t.offset = st.st_size;
    int64_t required_length = st.st_size;
    if (j->segmented) {
        t.offset = required_length = 0;
        for (unsigned i = 0; i < j->range_count; i++) {
            t.offset += t.live[i];
            int64_t end = range_boundary(r->size, j->range_count, i) + t.live[i];
            if (t.live[i] && end > required_length) required_length = end;
        }
    }
    if (st.st_size < required_length || st.st_size > r->size || t.offset < 0) {
        copy_text(t.error, sizeof t.error,
                  "Partial file size is invalid. Delete the partial file before retrying.");
        goto finish;
    }
    struct statvfs fs;
    uint64_t disk_remaining = (uint64_t)(r->size - t.offset);
    if (j->segmented && st.st_blocks > 0) {
        uint64_t allocated = (uint64_t)st.st_blocks * 512;
        uint64_t unallocated = allocated >= (uint64_t)r->size ? 0 : (uint64_t)r->size - allocated;
        if (unallocated < disk_remaining)
            disk_remaining = unallocated;
    }
    /* Only the download: room to unpack an archive is checked just before it is unpacked. */
    if (fstatvfs(t.fd, &fs) || (uint64_t)fs.f_bavail * (fs.f_frsize ? fs.f_frsize : fs.f_bsize) <
                                   disk_remaining + 16 * 1024 * 1024) {
        copy_text(t.error, sizeof t.error, "Not enough free space to finish this download.");
        goto finish;
    }
    if (torbox(&t) && !early_torbox) {
        int prepared = prepare_torbox(&t, route);
        if (prepared != 1) {
            debrid_waiting = prepared == 0;
            source_failed = !debrid_waiting;
            goto finish;
        }
    }
    bool viking = torbox(&t) || provider_browser_source(r->source);
    int direct_ranges = -1;
    if (viking) {
        /* Test the saved captured URL with one byte first. No provider page is
         * opened by Resume; expired links get an explicit refresh action. */
        if (range_support(&t, NULL, &rc) != 1) {
            source_failed = torbox(&t);
            if (!t.error[0])
                copy_text(t.error, sizeof t.error, "Source did not return a valid byte range.");
            goto finish;
        }
    } else if (get_verified_file(&t)) {
        direct_ranges = range_support(&t, NULL, &rc);
        if (direct_ranges < 0) {
            source_failed = remote_transfer(&t);
            goto finish;
        }
    } else if (!remote_transfer(&t)) {
        c = request(&t);
        if (!c) {
            copy_text(t.error, sizeof t.error, "Could not initialize HTTPS.");
            goto finish;
        }
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        c = NULL;
        if (rc != CURLE_OK || t.status != 200) {
            snprintf(t.error, sizeof t.error, "Source check failed (HTTP %ld, %s).", t.status,
                     curl_easy_strerror(rc));
            goto finish;
        }
    }
    if (t.length != r->size) {
        source_failed = torbox(&t);
        copy_text(t.error, sizeof t.error, direct_transfer(&t) ? "The file size changed after the download URL was checked."
                                                             : "Source size changed. Catalogue needs verification.");
        goto finish;
    }
    bool strong = strong_etag(t.etag);
    if ((t.offset || j->segmented) &&
        ((j->etag[0] && strcmp(j->etag, t.etag)) ||
         (!j->etag[0] && j->modified[0] && strcmp(j->modified, t.modified)) ||
         (!j->etag[0] && !j->modified[0]))) {
        source_failed = torbox(&t);
        copy_text(
            t.error, sizeof t.error,
            "Source identity cannot be verified for resume. Delete the partial file and retry.");
        goto finish;
    }
    pthread_mutex_lock(&orbit.mutex);
    if (!t.offset) {
        copy_text(j->etag, sizeof j->etag, strong ? t.etag : "");
        copy_text(j->modified, sizeof j->modified, t.modified);
    }
    j->received = t.offset;
    int saved = state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    if (saved) {
        copy_text(t.error, sizeof t.error, "Could not persist source identity.");
        goto finish;
    }
    char h[400];
    if (j->etag[0]) {
        snprintf(h, sizeof h, "If-Match: %s", j->etag);
        conditions = curl_slist_append(NULL, h);
    } else if (j->modified[0]) {
        snprintf(h, sizeof h, "If-Unmodified-Since: %s", j->modified);
        conditions = curl_slist_append(NULL, h);
    }
    if ((j->etag[0] || j->modified[0]) && !conditions) {
        copy_text(t.error, sizeof t.error, "Could not prepare source identity check.");
        goto finish;
    }
    bool usb = usb_destination(j), usb_parallel = false;
    bool single_connection = !strcmp(r->source, "1fichier") && strcmp(j->delivery, "torbox");
    pthread_mutex_lock(&orbit.mutex);
    /* The worker reserves this budget before starting. A later settings change
     * must not let another job overbook connections already in use. */
    unsigned setting = j->transfer_connections ? j->transfer_connections : 1;
    pthread_mutex_unlock(&orbit.mutex);
    t.connections = setting;
    if (!j->segmented) {
        /* The layout follows the setting when the download starts; a saved
         * partial keeps its own, and the setting only limits its requests. */
        unsigned count = setting;
#ifdef ORBIT_TEST
        const char *connections = getenv("ORBIT_TEST_CONNECTIONS");
        if (connections && !strcmp(connections, "2")) count = 2;
        else if (connections && !strcmp(connections, "4")) count = 4;
#endif
        bool parallel = false;
        if (!single_connection && j->etag[0] && t.offset < range_boundary(r->size, count, 1) &&
            r->size - t.offset >= PARALLEL_MIN_REMAINING) {
            int supported = viking ? 1 : direct_ranges >= 0 ? direct_ranges : range_support(&t, conditions, &rc);
            if (supported < 0) goto finish;
            parallel = supported == 1;
        }
        /* Persist the layout before starting the writer or writing any tail.
         * Single-connection downloads also use a durable cursor, not st_size. */
        if (fsync(t.fd)) {
            copy_text(t.error, sizeof t.error, "Could not flush the existing partial file.");
            goto finish;
        }
        t.live[0] = t.offset;
        pthread_mutex_lock(&orbit.mutex);
        j->segmented = true;
        j->range_count = parallel && !usb ? count : 1;
        usb_parallel = parallel && usb;
        j->range_received[0] = t.offset;
        saved = state_save_locked();
        pthread_mutex_unlock(&orbit.mutex);
        if (saved) {
            copy_text(t.error, sizeof t.error, "Could not persist the download ranges.");
            goto finish;
        }
    }
    /* A one-cursor USB job may have used either range windows or a host that
     * ignores ranges. Recheck on resume; do not require a new queue schema. */
    if (!single_connection && usb && !usb_parallel && j->etag[0] && t.offset < r->size) {
        int supported = viking ? 1 : direct_ranges >= 0 ? direct_ranges : range_support(&t, conditions, &rc);
        if (supported < 0) goto finish;
        usb_parallel = supported == 1;
    }
    if (t.offset < r->size) {
        t.probe = false;
        t.started = seconds();
        t.health = (DownloadHealth){t.started, 0};
        pthread_mutex_lock(&orbit.mutex);
        unsigned wanted = usb && (usb_parallel || j->range_count > 1) ? setting
                          : j->range_count < setting ? j->range_count : setting;
        j->transfer_connections = download_connections(wanted, j->attempts);
        pthread_mutex_unlock(&orbit.mutex);
        t.writer = transfer_writer_start(j, t.fd, t.started, t.error, sizeof t.error);
        if (!t.writer) goto finish;
        if (usb_parallel || j->range_count > 1) {
            rc = usb ? run_usb_ranges(&t, conditions) : run_ranges(&t, conditions);
            if (rc != CURLE_OK || t.error[0]) {
                source_failed = torbox(&t) && !transfer_writer_failed(t.writer);
                goto finish;
            }
        } else {
            c = request(&t);
            if (!c) {
                copy_text(t.error, sizeof t.error, "Could not initialize transfer.");
                goto finish;
            }
            set_range(c, t.offset, r->size, conditions);
            rc = curl_easy_perform(c);
            curl_easy_cleanup(c);
            c = NULL;
            if (rc != CURLE_OK || !t.accepted) {
                source_failed = torbox(&t) && !transfer_writer_failed(t.writer);
                if (!t.error[0])
                    snprintf(t.error, sizeof t.error, "Transfer failed (HTTP %ld, %s).", t.status,
                             curl_easy_strerror(rc));
                goto finish;
            }
        }
    }
    if (t.writer) {
        int failed = transfer_writer_finish(t.writer, t.error, sizeof t.error);
        t.writer = NULL;
        if (failed) { rc = CURLE_WRITE_ERROR; goto finish; }
    }
    int64_t durable = 0;
    for (unsigned i = 0; i < j->range_count; i++) durable += j->range_received[i];
    if (fstat(t.fd, &st) || st.st_size != r->size || durable != r->size) {
        copy_text(t.error, sizeof t.error, "Downloaded file size or disk flush failed validation.");
        goto finish;
    }
    pthread_mutex_lock(&orbit.mutex);
    copy_text(j->status, sizeof j->status, "verifying");
    j->speed = 0;
    state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    if (!checksum(&t)) {
        source_failed = true;
        copy_text(t.error, sizeof t.error, "Checksum verification failed. Partial file preserved.");
        goto finish;
    }
    if ((get_verified_file(&t) || (remote_transfer(&t) && (torbox(&t) || j->mirror_id[0] || j->mirror_first[0]))) && !file_type_matches(&t)) {
        source_failed = true;
        copy_text(t.error, sizeof t.error, "The downloaded file does not match the verified file type.");
        goto finish;
    }
    if (controlled(&t))
        goto finish;
    if (!storage_matches(j)) {
        copy_text(t.error, sizeof t.error, "Destination disconnected before finalization.");
        goto finish;
    }
    /* Kept packed: the archive is published under its own name like any file. */
    if (r->compression[0] && !j->keep_packed) {
        close(t.fd);
        t.fd = -1;
        pthread_mutex_lock(&orbit.mutex);
        j->archive_ready = true;
        int persisted = state_save_locked();
        pthread_mutex_unlock(&orbit.mutex);
        if (persisted) {
            copy_text(t.error, sizeof t.error, "Could not persist the verified archive.");
            goto finish;
        }
        unpacked = extract_job(j, dir, partial, note, sizeof note, t.error, sizeof t.error);
        done = !unpacked;
        goto finish;
    }
    if (linkat(dir, partial, dir, j->filename, 0)) {
        /* exFAT does not support hard links. Reserve the final path exclusively. */
        if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOSYS) {
            copy_text(t.error, sizeof t.error,
                      "Could not finalize without overwriting an existing file.");
            goto finish;
        }
        /* O_EXCL reserves the name; remove only our reservation if rename fails. */
        int reserve = openat(dir, j->filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (reserve < 0) {
            copy_text(t.error, sizeof t.error, "Destination filename is already in use.");
            goto finish;
        }
        close(reserve);
        if (renameat(dir, partial, dir, j->filename)) {
            unlinkat(dir, j->filename, 0);
            copy_text(t.error, sizeof t.error, "Could not finalize the completed file.");
            goto finish;
        }
    } else
        unlinkat(dir, partial, 0);
    fsync(dir);
    done = true;
finish:
    if (c)
        curl_easy_cleanup(c);
    curl_slist_free_all(conditions);
    if (t.writer) {
        if (transfer_writer_finish(t.writer, t.error, sizeof t.error)) {
            rc = CURLE_WRITE_ERROR;
            t.status = 0; /* A disk failure must not inherit a sibling's HTTP retry. */
            source_failed = false;
        }
        t.writer = NULL;
    }
    if (t.fd >= 0) {
        if (!j->segmented) fsync(t.fd);
        struct stat actual;
        if (!j->segmented && !fstat(t.fd, &actual)) {
            pthread_mutex_lock(&orbit.mutex);
            j->received = actual.st_size;
            pthread_mutex_unlock(&orbit.mutex);
        }
        close(t.fd);
    }
    /* Archive.org refuses a login-only file, and one it has taken down, with
     * 401 or 403. Say which and what to do, not an HTTP code. */
    if (!done && (t.status == 401 || t.status == 403) && !torbox(&t) && !strcmp(r->source, "archive")) {
        pthread_mutex_lock(&orbit.mutex);
        bool stopping = j->cancel || j->pause || orbit.stop;
        pthread_mutex_unlock(&orbit.mutex);
        if (!stopping) archive_account_refusal(r->url, t.error, sizeof t.error);
    }
    pthread_mutex_lock(&orbit.mutex);
    j->speed = 0;
    /* The TorBox progress watch lasts only while the job keeps waiting. */
    if (!debrid_waiting || j->cancel || j->pause || orbit.stop) j->debrid_watch = 0;
    bool can_switch = !done && !j->cancel && !j->pause && !orbit.stop && !debrid_waiting &&
        !j->archive_ready && unpacked == -1;
    bool used_torbox = torbox(&t);
    bool provider_switched = can_switch && used_torbox && source_failed && fall_back_to_provider_locked(j);
    /* An unavailable or rate-limited link should not cool down its other mirrors. */
    bool mirror_switched = can_switch && !used_torbox &&
        remote_transfer(&t) && (source_failed || t.status == 401 || t.status == 403 || t.status == 404 ||
            t.status == 410 || t.status == 429 || t.status == 503) && advance_mirror_locked(j);
    bool switched = provider_switched || mirror_switched;
    if (!switched && (t.status == 429 || t.status == 503)) {
        time_t until = time(NULL) + orbit.download_policy.delay;
        if (t.retry_at > until) until = t.retry_at;
        if (until > orbit.source_retry_at[retry_source(j)]) orbit.source_retry_at[retry_source(j)] = until;
    }
    if (done) {
        copy_text(j->status, sizeof j->status, "complete");
        copy_text(j->verification, sizeof j->verification, r->sha256[0] ? "sha256" : "size");
        copy_text(j->error, sizeof j->error, note);
    } else if (j->cancel) {
        copy_text(j->status, sizeof j->status, "cancelled");
        j->error[0] = 0;
        if (j->remove)
            job_delete_partial_locked(j, j->error, sizeof j->error);
    } else if (j->pause || orbit.stop) {
        copy_text(j->status, sizeof j->status, "paused");
        copy_text(j->error, sizeof j->error,
                  source_enabled_locked(r)
                      ? ""
                      : "Source disabled. Enable it in Sources before resuming.");
    } else if (unpacked == 3) {
        /* No room to unpack: it waits, paused, until the user frees space and resumes. */
        copy_text(j->status, sizeof j->status, "paused");
        copy_text(j->error, sizeof j->error, t.error);
    } else if (debrid_waiting) {
        copy_text(j->status, sizeof j->status, "queued");
        j->retry_at = t.retry_at; j->error[0] = 0;
    } else if (!switched && (retryable(t.status, rc) || (used_torbox && (t.status == 401 || t.status == 403 || t.status == 410)) ||
                (t.stalled && rc != CURLE_WRITE_ERROR)) &&
               j->attempts < orbit.download_policy.retries &&
               (!j->received || j->etag[0] || j->modified[0])) {
        j->attempts++;
        time_t fallback = time(NULL) + orbit.download_policy.delay;
        j->retry_at = t.retry_at > fallback ? t.retry_at : fallback;
        copy_text(j->status, sizeof j->status, "retrying");
        copy_text(j->error, sizeof j->error, t.error);
        /* A short hiccup must not swap games. Long provider cooldowns still
         * release the slot so an unrelated source can make progress. */
        j->retry_worker = j->retry_at <= time(NULL) + 30 &&
                          orbit.source_retry_at[retry_source(j)] <= time(NULL) + 30;
    } else if (!switched && !(!used_torbox && remote_transfer(&t) && (t.status >= 400 || retryable(t.status, rc)) &&
                                    advance_mirror_locked(j))) {
        copy_text(j->status, sizeof j->status, "error");
        copy_text(j->error, sizeof j->error, t.error[0] ? t.error : "Transfer interrupted.");
    }
    state_save_locked();
    if (done) { power_download_completed(); packages_completed(j, j->id, j->output[0] ? j->output : j->filename, true); }
    diagnostics_event_locked("transfer-end", j, t.status, (int)rc);
    char notice[512] = ""; /* a DLC title is "base · title · DLC", as in packages.c */
    if ((done || !strcmp(j->status, "error")) &&
        notify_wanted_locked(done ? NOTIFY_DOWNLOAD_FINISHED : NOTIFY_DOWNLOAD_FAILED))
        release_download_title(&j->release, notice, sizeof notice);
    pthread_cond_broadcast(&orbit.changed);
    pthread_mutex_unlock(&orbit.mutex);
    if (*notice) notify_download(notice, done);
    if (dir >= 0)
        close(dir);
    OPENSSL_cleanse(resolved.url, sizeof resolved.url);
    forget_storage(&t);
}
/* Called with the queue lock held. The saved range layout never changes. */
static unsigned connection_budget(const Job *j, unsigned reserved) {
    unsigned setting = orbit.download_connections ? orbit.download_connections : ORBIT_DEFAULT_CONNECTIONS;
    unsigned share = ORBIT_MAX_RANGES / (orbit.concurrent_downloads ? orbit.concurrent_downloads : 1);
    if (setting > share) setting = share ? share : 1;
    if (!strcmp(j->delivery, "torbox") && setting > 8) setting = 8;
    if (reserved >= ORBIT_MAX_RANGES) return 1;
    if (setting > ORBIT_MAX_RANGES - reserved) setting = ORBIT_MAX_RANGES - reserved;
    return setting;
}
static void run_with_short_retries(Job *j) {
    for (;;) {
        run_job(j);
        pthread_mutex_lock(&orbit.mutex);
        while (j->retry_worker && !j->pause && !j->cancel && !orbit.stop &&
               j->attempts <= orbit.download_policy.retries) {
            time_t now = time(NULL), until = j->retry_at;
            if (orbit.source_retry_at[retry_source(j)] > until) until = orbit.source_retry_at[retry_source(j)];
            if (until > now + 30) { j->retry_worker = false; break; }
            if (now >= until) break;
            struct timespec deadline = {.tv_sec = now + 1};
            pthread_cond_timedwait(&orbit.changed, &orbit.mutex, &deadline);
        }
        if (!j->retry_worker) {
            pthread_cond_broadcast(&orbit.changed);
            pthread_mutex_unlock(&orbit.mutex);
            break;
        }
        j->retry_worker = false;
        if (j->attempts > orbit.download_policy.retries && !j->pause && !j->cancel && !orbit.stop) {
            copy_text(j->status, sizeof j->status, "error");
            state_save_locked();
            pthread_cond_broadcast(&orbit.changed);
            pthread_mutex_unlock(&orbit.mutex);
            break;
        }
        unsigned reserved = 0;
        for (size_t i = 0; i < orbit.job_count; i++)
            if (&orbit.jobs[i] != j && job_active(&orbit.jobs[i]))
                reserved += orbit.jobs[i].transfer_connections ? orbit.jobs[i].transfer_connections : 1;
        j->transfer_connections = connection_budget(j, reserved);
        copy_text(j->status, sizeof j->status, "downloading");
        j->error[0] = 0; /* The failure that caused this retry is over. */
        /* run_job handles pause/cancel, including delete-partial, before I/O. */
        pthread_mutex_unlock(&orbit.mutex);
    }
}
/* A job the worker gives up on before it starts; its notice is sent after unlocking. */
static void fail_waiting_locked(Job *j, char (*notices)[512], size_t *count) {
    copy_text(j->status, sizeof j->status, "error");
    if (notify_wanted_locked(NOTIFY_DOWNLOAD_FAILED) && *count < 4)
        release_download_title(&j->release, notices[(*count)++], sizeof notices[0]);
}
void *download_worker(void *unused) {
    (void)unused;
    for (;;) {
        char notices[4][512];
        size_t notice_count = 0;
        pthread_mutex_lock(&orbit.mutex);
        if (orbit.stop) {
            pthread_mutex_unlock(&orbit.mutex);
            break;
        }
        Job *next = NULL;
        time_t now = time(NULL);
        unsigned active = 0, reserved = 0;
        for (size_t i = 0; i < orbit.job_count; i++)
            if (job_active(&orbit.jobs[i])) {
                active++;
                reserved += orbit.jobs[i].transfer_connections ? orbit.jobs[i].transfer_connections : 1;
            }
        /* Copies, Files actions and transfers hold their space and paths, so downloads start
         * beside them; only one whose names a running operation uses waits for it. */
        if (active < orbit.concurrent_downloads && reserved < ORBIT_MAX_RANGES && !orbit.state_failed)
            for (size_t i = 0; i < orbit.job_count; i++) {
                Job *j = &orbit.jobs[i];
                if (j->retry_worker || storage_hold_conflict_locked(j)) continue;
                if (!strcmp(j->status, "retrying") && j->attempts > orbit.download_policy.retries) {
                    fail_waiting_locked(j, notices, &notice_count);
                    j->retry_at = 0;
                    state_save_locked();
                    continue;
                }
                if (now < orbit.source_retry_at[retry_source(j)]) continue;
                if ((!strcmp(j->status, "queued") && now >= j->retry_at) ||
                    (!strcmp(j->status, "retrying") && now >= j->retry_at)) {
                    if (!source_enabled_locked(&j->release)) {
                        copy_text(j->status, sizeof j->status, "paused");
                        copy_text(j->error, sizeof j->error,
                                  "Source disabled. Enable it in Sources before resuming.");
                        state_save_locked();
                        continue;
                    }
                    /* Saved queues from older builds may contain colliding names.
                     * Never let a second worker share a partial or final path. */
                    bool conflict = false;
                    for (size_t k = 0; k < orbit.job_count; k++) {
                        Job *other = &orbit.jobs[k];
                        if (other != j && other->id[0] && strcmp(other->status, "complete") &&
                            strcmp(other->status, "cancelled") && job_paths_overlap(j, other)) {
                            conflict = true;
                            break;
                        }
                    }
                    if (conflict) {
                        fail_waiting_locked(j, notices, &notice_count);
                        copy_text(j->error, sizeof j->error,
                                  "Another download uses this filename on that drive. Open Downloads to manage it.");
                        state_save_locked();
                    } else if (!next || j->order < next->order) next = j;
                }
            }
        if (next) {
            if (next->browser_pending) {
                int started = browser_start_queued_locked(next, next->error, sizeof next->error);
                if (started > 0) {
                    next->retry_at = 0;
                    copy_text(next->status, sizeof next->status, "verifying");
                    if (state_save_locked()) {
                        fail_waiting_locked(next, notices, &notice_count);
                        copy_text(next->error, sizeof next->error, "Could not persist the download start.");
                    }
                } else if (started == 0) {
                    next->retry_at = now + 1;
                } else {
                    fail_waiting_locked(next, notices, &notice_count);
                    (void)state_save_locked();
                }
                pthread_mutex_unlock(&orbit.mutex);
                for (size_t i = 0; i < notice_count; i++) notify_download(notices[i], false);
                continue;
            }
            next->transfer_connections = connection_budget(next, reserved);
            copy_text(next->status, sizeof next->status, "downloading");
            next->error[0] = 0;
            if (state_save_locked()) {
                copy_text(next->status, sizeof next->status, "error");
                copy_text(next->error, sizeof next->error, "Could not persist the download start.");
                pthread_mutex_unlock(&orbit.mutex);
                for (size_t i = 0; i < notice_count; i++) notify_download(notices[i], false);
                continue;
            }
            diagnostics_event_locked("transfer-start", next, 0, 0);
            pthread_mutex_unlock(&orbit.mutex);
            for (size_t i = 0; i < notice_count; i++) notify_download(notices[i], false);
            run_with_short_retries(next);
        } else {
            struct timespec deadline = {.tv_sec = now + 1, .tv_nsec = 0};
            if (!notice_count) pthread_cond_timedwait(&orbit.changed, &orbit.mutex, &deadline);
            pthread_mutex_unlock(&orbit.mutex);
            for (size_t i = 0; i < notice_count; i++) notify_download(notices[i], false);
        }
    }
    return NULL;
}
