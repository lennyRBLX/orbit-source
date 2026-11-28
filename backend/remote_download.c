#include "remote_sources.h"
#include "archive_account.h"
#include "image_format.h"
#include "provider_network.h"
#include "ca.h"
#include "pspkg.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PAGE_LIMIT (1024U * 1024U)
enum { ARCHIVE_FILE_LOGIN_REQUIRED = -4 };
#define MIN_SAMPLE_BYTES 8
#define MAX_SAMPLE_BYTES (DOWNLOAD_UFS_MAGIC_OFFSET + sizeof ((DownloadSample *)0)->ufs_magic)

typedef struct {
    char *body;
    size_t used;
    bool page, collect_page, truncated, direct, challenge, json, error_page, redirect_rejected;
    bool metadata_only, redirect_status, range, plain_text, html_only, archive_session, use_http1;
    const char *page_token;
    const char *initial_url;
    CURLcode code;
    long status;
    char effective_host[254];
    DownloadSample sample;
    char url[2048], name[160];
    int64_t size;
    atomic_bool *cancel;
    CURL *handle;
} Response;

static bool cancelled(const Response *r) {
    return atomic_load(&orbit.stop) || (r->cancel && atomic_load(r->cancel));
}

static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return cancelled(context);
}

static bool resolve_href(const char *base, const char *href, char *out, size_t cap);
static const char *prefix_suffix(const DownloadSample *sample, size_t length);

static bool metadata_origin(const char *initial, const char *next) {
    if (!strcmp(initial, next)) return true;
    const char *provider = remote_browser_provider(initial);
    if (!strcmp(provider, "pspkg")) return pspkg_page_redirect(initial, next);
    if (*provider && !strcmp(provider, remote_browser_provider(next))) return true;
    const char *initial_host = strstr(initial, "://"), *next_host = strstr(next, "://");
    if (initial_host && next_host && initial_host - initial == next_host - next &&
        !strncasecmp(initial, next, (size_t)(initial_host - initial))) {
        initial_host += 3;
        next_host += 3;
        size_t n = strcspn(initial_host, "/?");
        if (n && n == strcspn(next_host, "/?") && !strncasecmp(initial_host, next_host, n)) return true;
    }
#ifdef ORBIT_TEST
    if (!strncmp(initial, "http://127.0.0.1:", 17) && !strncmp(next, "http://127.0.0.1:", 17)) {
        size_t n = strcspn(initial + 17, "/");
        return n == strcspn(next + 17, "/") && !strncmp(initial + 17, next + 17, n);
    }
#endif
    return false;
}

static int before_request(void *context, char *address, char *local, int port, int local_port) {
    (void)address; (void)local; (void)port; (void)local_port;
    Response *r = context;
    char *url = NULL;
    curl_easy_getinfo(r->handle, CURLINFO_EFFECTIVE_URL, &url);
    bool allowed = (r->direct ? direct_http_url(url) : remote_https_url(url)) &&
                   (!r->metadata_only || metadata_origin(r->initial_url, url));
    if (!allowed) r->redirect_rejected = true;
    return allowed ? CURL_PREREQFUNC_OK : CURL_PREREQFUNC_ABORT;
}

static curl_socket_t response_socket(void *context, curlsocktype purpose, struct curl_sockaddr *address) {
    Response *r = context;
    curl_socket_t socket = provider_open_socket(NULL, purpose, address);
    if (socket == CURL_SOCKET_BAD && r->redirect_status) r->redirect_rejected = true;
    return socket;
}

static size_t headers(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    size_t n = a * b;
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
        r->used = 0;
        memset(&r->sample, 0, sizeof r->sample);
        r->page = r->truncated = r->challenge = r->json = false;
        r->error_page = r->redirect_status = r->plain_text = false;
        free(r->body);
        r->body = NULL;
        const char *status = memchr(data, ' ', n);
        if (status && data + n - status >= 4) {
            r->error_page = status[1] == '4' || status[1] == '5';
            r->redirect_status = status[1] == '3';
        }
    } else if (r->metadata_only && r->redirect_status && n > 9 && !strncasecmp(data, "Location:", 9)) {
        size_t start = 9;
        while (start < n && isspace((unsigned char)data[start])) start++;
        size_t end = n;
        while (end > start && isspace((unsigned char)data[end - 1])) end--;
        char location[2048], resolved[2048], *base = NULL;
        if (end - start >= sizeof location) { r->redirect_rejected = true; return 0; }
        memcpy(location, data + start, end - start);
        location[end - start] = 0;
        curl_easy_getinfo(r->handle, CURLINFO_EFFECTIVE_URL, &base);
        if (!base || !resolve_href(base, location, resolved, sizeof resolved) ||
            !metadata_origin(r->initial_url, resolved) ||
            (r->html_only && strcmp(remote_browser_provider(r->initial_url), remote_browser_provider(resolved)))) {
            r->redirect_rejected = true;
            return 0;
        }
    } else if (r->html_only && n >= 20 && !strncasecmp(data, "Content-Disposition:", 20)) {
        return 0;
    } else if (n >= 13 && !strncasecmp(data, "cf-mitigated:", 13)) {
        size_t offset = 13;
        while (offset < n && (data[offset] == ' ' || data[offset] == '\t')) offset++;
        r->challenge = n - offset >= 9 && !strncasecmp(data + offset, "challenge", 9);
    } else if (n >= 13 && !strncasecmp(data, "Content-Type:", 13)) {
        size_t offset = 13;
        while (offset < n && isspace((unsigned char)data[offset])) offset++;
        size_t length = n - offset;
        r->json = length >= 16 && !strncasecmp(data + offset, "application/json", 16);
        r->plain_text = length >= 10 && !strncasecmp(data + offset, "text/plain", 10);
        r->page = (length >= 9 && !strncasecmp(data + offset, "text/html", 9)) ||
                  (length >= 21 && !strncasecmp(data + offset, "application/xhtml+xml", 21)) ||
                  r->json ||
                  (r->collect_page && length >= 10 && !strncasecmp(data + offset, "text/plain", 10));
    }
    return n;
}

static size_t body(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    size_t n = a * b;
    if (cancelled(r) || (r->html_only && (!r->page || r->json || r->plain_text))) return 0;
    bool collect = r->collect_page && (r->page || r->error_page || r->plain_text);
    if (!collect && r->collect_page && !r->used) {
        size_t offset = 0;
        while (offset < n && isspace((unsigned char)data[offset])) offset++;
        collect = r->page = offset < n && data[offset] == '<';
    }
    size_t limit = collect ? PAGE_LIMIT : MAX_SAMPLE_BYTES;
    if (collect && !r->body) {
        r->body = malloc(PAGE_LIMIT + 1);
        if (!r->body) return 0;
    }
    size_t take = n <= limit - r->used ? n : limit - r->used;
    if (collect) memcpy(r->body + r->used, data, take);
    if (r->used < sizeof r->sample.prefix) {
        size_t remaining = sizeof r->sample.prefix - r->used;
        memcpy(r->sample.prefix + r->used, data, take < remaining ? take : remaining);
    }
    if (r->used < MAX_SAMPLE_BYTES && r->used + take > DOWNLOAD_UFS_MAGIC_OFFSET) {
        size_t start = r->used > DOWNLOAD_UFS_MAGIC_OFFSET ? r->used : DOWNLOAD_UFS_MAGIC_OFFSET;
        size_t end = r->used + take < MAX_SAMPLE_BYTES ? r->used + take : MAX_SAMPLE_BYTES;
        memcpy(r->sample.ufs_magic + start - DOWNLOAD_UFS_MAGIC_OFFSET, data + start - r->used, end - start);
    }
    r->used += take;
    if (collect) r->body[r->used] = 0;
    r->truncated = take != n || (!collect && prefix_suffix(&r->sample, r->used));
    return r->truncated ? 0 : n;
}

static bool fetch(const char *url, Response *r) {
    if (!(r->direct ? direct_http_url(url) : remote_https_url(url)) || cancelled(r)) return false;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    r->handle = curl;
    r->initial_url = url;
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, response_socket);
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, r);
    curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, before_request);
    curl_easy_setopt(curl, CURLOPT_PREREQDATA, r);
    /* The Rootz page token belongs only to the origin that issued it. */
    struct curl_slist *headers_out = NULL;
    if (r->page_token) {
        headers_out = curl_slist_append(NULL, r->page_token);
        if (!headers_out) { curl_easy_cleanup(curl); r->handle = NULL; return false; }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_out);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, r->page_token ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
#ifndef ORBIT_TEST
    const char *protocols = r->direct && !strncasecmp(url, "http://", 7) ? "http,https" : "https";
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
    /* BuzzHeavier can challenge HTTP/2 requests, including after a PSPKG redirect. */
    if (r->use_http1 || (r->collect_page && !r->range && !strcmp(remote_browser_provider(url), "buzzheavier")))
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    if (r->archive_session) archive_account_apply(curl);
    if (r->collect_page && !r->range) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (!r->collect_page || r->range) {
        char range[24];
        snprintf(range, sizeof range, "0-%zu", MAX_SAMPLE_BYTES - 1);
        curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, r);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode code = r->code = curl_easy_perform(curl);
    long status = 0;
    char *effective = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    r->status = status;
    if (effective) {
        CURLU *parts = curl_url();
        char *host = NULL;
        if (parts && !curl_url_set(parts, CURLUPART_URL, effective, CURLU_DISALLOW_USER) &&
            !curl_url_get(parts, CURLUPART_HOST, &host, 0))
            copy_text(r->effective_host, sizeof r->effective_host, host);
        curl_free(host);
        curl_url_cleanup(parts);
    }
    bool ok = !cancelled(r) && (code == CURLE_OK || (code == CURLE_WRITE_ERROR && r->truncated)) &&
              (status == 200 || status == 206) && effective && strlen(effective) < sizeof r->url;
    if (effective && strlen(effective) < sizeof r->url) copy_text(r->url, sizeof r->url, effective);
    if (ok) {
        struct curl_header *header = NULL;
        if (curl_easy_header(curl, "Content-Disposition", 0, CURLH_HEADER, -1, &header) == CURLHE_OK) {
            if (!provider_header_filename(header->value, r->name, sizeof r->name) &&
                !strncasecmp(header->value, "attachment", 10) && strchr(header->value, ';'))
                ok = false;
        }
        if (status == 206 && curl_easy_header(curl, "Content-Range", 0, CURLH_HEADER, -1, &header) == CURLHE_OK) {
            long long end, total;
            char trailing;
            if (sscanf(header->value, "bytes 0-%lld/%lld%c", &end, &total, &trailing) == 2 &&
                end >= MIN_SAMPLE_BYTES - 1 && total > end) r->size = total;
        } else if (status == 200) {
            curl_off_t size = -1;
            if (!curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size)) r->size = size;
        }
        if (r->range && curl_easy_header(curl, "Content-Encoding", 0, CURLH_HEADER, -1, &header) == CURLHE_OK &&
            strcasecmp(header->value, "identity")) r->size = 0;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers_out);
    r->handle = NULL;
    return ok;
}

static const char *prefix_suffix(const DownloadSample *sample, size_t length) {
    const unsigned char *p = sample->prefix;
    if (length < MIN_SAMPLE_BYTES) return NULL;
    if (!memcmp(p, "PK\3\4", 4)) return ".zip";
    if (!memcmp(p, "7z\xbc\xaf\x27\x1c", 6)) return ".7z";
    if (!memcmp(p, "Rar!\x1a\x07", 6) && (p[6] == 0 || (p[6] == 1 && p[7] == 0))) return ".rar";
    if (!memcmp(p, "\x7f" "CNT", 4) || !memcmp(p, "\x7f" "FIH", 4)) return ".pkg";
    if (!memcmp(p, "PFSC", 4)) return ".ffpfsc";
    if (!memcmp(p + 3, "EXFAT", 5)) return ".exfat";
    /* A PFS header starts with a little-endian 64-bit version and magic. */
    if (length >= sizeof sample->prefix && (p[0] == 1 || p[0] == 2) &&
        !memcmp(p + 1, "\0\0\0\0\0\0\0\x0b\x2a\x33\x01\0\0\0\0", 15)) return ".ffpfs";
    return NULL;
}

static const char *sample_suffix(const DownloadSample *sample, size_t length) {
    const char *suffix = prefix_suffix(sample, length);
    if (!suffix && length >= MAX_SAMPLE_BYTES && !memcmp(sample->ufs_magic, "\x19\x01\x54\x19", 4))
        return ".ffpkg";
    return suffix;
}

bool download_file_magic(const Release *r, const DownloadSample *sample, size_t length) {
    const char *suffix = sample_suffix(sample, length);
    if (!suffix) return false;
    if (r->compression[0]) return !strcmp(suffix + 1, r->compression);
    const char *expected = image_suffix_for_format(r->format);
    return !strcmp(suffix, expected ? expected : ".pkg");
}

static bool safe_page_name(const char *name);

static int file_from_response(Release *r, const Response *response, const char *url, const char *page_name) {
    char name[sizeof r->filename] = "";
    const char *actual = response->name;
    if (!*actual && !(r->remote_page && (provider_disposition_name(response->url, name, sizeof name) ||
                                        provider_disposition_name(url, name, sizeof name)))) {
        name[0] = 0;
        if (page_name && *page_name) copy_text(name, sizeof name, page_name);
        else {
            remote_url_filename(response->url, name, sizeof name);
            if (r->filename[0] && strcmp(r->filename, name)) {
                name[0] = 0;
                remote_url_filename(url, name, sizeof name);
            }
        }
    }
    if (!*actual) actual = name;
    if (!response->direct && !safe_page_name(actual)) return -1;
    if (r->filename[0] && !release_filename_matches(r, actual)) return -1;
    Release file = *r;
    const char *suffix = sample_suffix(&response->sample, response->used);
    int format = response->direct ? direct_file_format(&file, actual) :
        remote_provider_file_format(&file, actual, r->remote_page ? suffix : NULL);
    if (format) return format;
    if (r->compression[0] && strcmp(r->compression, file.compression)) return 1;
    if (response->page || response->size < MIN_SAMPLE_BYTES || response->size > 9007199254740991LL ||
        (r->size && r->size != response->size) ||
        !download_file_magic(&file, &response->sample, response->used)) return -1;
    copy_text(file.url, sizeof file.url, url);
    file.size = response->size;
    *r = file;
    return 0;
}

static bool response_missing(const Response *response);
static bool safe_page_name(const char *name);

int remote_download_verify(Release *r, const char *url, atomic_bool *cancel, char *error, size_t cap) {
    bool direct = !strcmp(r->source, ORBIT_DIRECT_SOURCE);
    bool archive = !direct && remote_archive_url(url);
    Response response = {.cancel = cancel, .direct = direct, .range = true, .archive_session = archive,
                         .collect_page = archive || !strcmp(r->source, "remote") || !strcmp(r->source, "pspkg")};
    bool ok = fetch(url, &response);
    int code = response_missing(&response) ? REMOTE_DOWNLOAD_MISSING :
               ok ? file_from_response(r, &response, url, NULL) : -1;
    free(response.body);
    if (code) copy_text(error, cap, code == REMOTE_DOWNLOAD_MISSING ? REMOTE_DOWNLOAD_REMOVED :
                                      response.direct ? "This URL did not return a downloadable PKG, FPKG, ZIP, 7z or RAR with a filename and size."
                                      : code > 0 ? "This option uses an unsupported download format."
                                             : "The provider did not return a verifiable PKG, FPKG, RAR or ZIP.");
    return code;
}

/* Only read names and links present in HTML. The console browser still
 * handles interactive provider verification when a filename can be pinned. */
static bool html_text(const char *start, size_t length, char *out, size_t cap) {
    while (length && isspace((unsigned char)*start)) { start++; length--; }
    while (length && isspace((unsigned char)start[length - 1])) length--;
    size_t used = 0;
    const struct { const char *entity; char value; } entities[] = {
        {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''},
        {"&#x27;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&nbsp;", ' '}, {"&#160;", ' '}
    };
    for (size_t i = 0; i < length; i++) {
        if (used + 1 == cap) return false;
        char c = start[i];
        if (c == '&') {
            for (size_t j = 0; j < sizeof entities / sizeof *entities; j++) {
                size_t n = strlen(entities[j].entity);
                if (n > length - i || memcmp(start + i, entities[j].entity, n)) continue;
                c = entities[j].value;
                i += n - 1;
                break;
            }
        }
        out[used++] = c;
    }
    out[used] = 0;
    return used > 0;
}

static bool tag_attribute(const char *tag, const char *end, const char *attribute, char *out, size_t cap) {
    out[0] = 0;
    const char *p = tag;
    while (p < end && !isspace((unsigned char)*p)) p++;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        const char *key = p;
        while (p < end && *p != '=' && !isspace((unsigned char)*p)) p++;
        size_t length = (size_t)(p - key);
        if (!length) return false;
        bool match = length == strlen(attribute) && !strncasecmp(key, attribute, length);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p == end || *p != '=') {
            if (match) return true;
            continue;
        }
        p++;
        while (p < end && isspace((unsigned char)*p)) p++;
        char quote = p < end && (*p == '"' || *p == '\'') ? *p++ : 0;
        const char *value = p;
        while (p < end && (quote ? *p != quote : !isspace((unsigned char)*p))) p++;
        if (quote && p == end) return false;
        if (match) return p == value || html_text(value, (size_t)(p - value), out, cap);
        if (quote) p++;
    }
    return false;
}

static bool tag_value(const char *tag, const char *end, const char *attribute, char *out, size_t cap) {
    return tag_attribute(tag, end, attribute, out, cap) && *out;
}

static const char *find_text(const char *text, const char *word) {
    size_t n = strlen(word);
    for (; *text; text++) if (!strncasecmp(text, word, n)) return text;
    return NULL;
}

static size_t tag_name_length(const char *tag) {
    return strspn(tag, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-");
}

static bool tag_is(const char *tag, const char *name) {
    size_t length = strlen(name);
    return tag_name_length(tag) == length && !strncasecmp(tag, name, length);
}

/* Ignore markup that can contain empty states, sample filenames or quotas
 * for other pages. Block boundaries remain visible to the status parser. */
static const char *skip_markup(const char *tag) {
    if (!strncmp(tag, "<!--", 4)) {
        const char *close = strstr(tag + 4, "-->");
        return close ? close + 3 : tag + strlen(tag);
    }
    const char *hidden[] = {"script", "style", "template", "nav", "footer"};
    for (size_t i = 0; i < sizeof hidden / sizeof *hidden; i++) {
        if (!tag_is(tag + 1, hidden[i])) continue;
        char marker[16];
        snprintf(marker, sizeof marker, "</%s", hidden[i]);
        const char *close = find_text(tag + 1, marker);
        const char *end = close ? strchr(close, '>') : NULL;
        return end ? end + 1 : tag + strlen(tag);
    }
    return tag;
}

static void visible_text(const char *start, const char *end, char *out, size_t cap) {
    size_t used = 0;
    char separator = 0;
    while (start < end && used + 1 < cap) {
        if (*start == '<') {
            const char *next = skip_markup(start);
            if (next == start) {
                const char *close = strchr(start, '>');
                if (!close || close >= end) break;
                const char *name = start + 1 + (start[1] == '/');
                const char *blocks[] = {"title", "p", "div", "section", "article", "li", "br",
                                        "header", "footer", "nav", "tr", "td", "dt", "dd", "h1", "h2", "h3", "h4"};
                for (size_t i = 0; i < sizeof blocks / sizeof *blocks; i++)
                    if (tag_is(name, blocks[i])) separator = '\n';
                next = close + 1;
            }
            if (!separator) separator = ' ';
            start = next;
            continue;
        }
        char c = *start++;
        if (c == '&') {
            const char *semi = memchr(start, ';', (size_t)(end - start) < 8 ? (size_t)(end - start) : 8);
            char decoded[2];
            if (semi && html_text(start - 1, (size_t)(semi - start + 2), decoded, sizeof decoded)) {
                c = decoded[0];
                start = semi + 1;
            }
        } else if ((unsigned char)c == 0xc2 && start < end && (unsigned char)*start == 0xa0) {
            c = ' ';
            start++;
        }
        if (isspace((unsigned char)c)) {
            if (!separator) separator = ' ';
            continue;
        }
        if (used && separator) {
            if (used + 2 >= cap) break;
            out[used++] = separator;
        }
        separator = 0;
        out[used++] = c;
    }
    out[used] = 0;
}

static bool phrase(const char *text, const char *word) {
    size_t length = strlen(word);
    for (const char *p = text; (p = find_text(p, word)); p += length)
        if ((p == text || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[length]) &&
            p[length] != '.') return true;
    return false;
}

static bool status_phrase(const char *text, const char *message) {
    for (const char *p = text; *p; p++) {
        if (p != text && isalnum((unsigned char)p[-1])) continue;
        const char *word = message, *end = p;
        while (*word && *end && (*word == ' ' ? isspace((unsigned char)*end) :
               *word == tolower((unsigned char)*end))) {
            word++;
            end++;
        }
        if (!*word && !isalnum((unsigned char)*end) && (*end != '.' || !isalnum((unsigned char)end[1])))
            return true;
    }
    return false;
}

static bool missing_text(const char *text) {
    const char *messages[] = {"file not found", "file missing", "file is missing",
        "file removed", "file was removed", "file has been removed",
        "file deleted", "file was deleted", "file has been deleted", "file does not exist",
        "file doesn't exist", "file is no longer available", "this item is no longer available", "status: deleted", "no such key"};
    /* A standalone status is common, but 'deleted' in a filename or a
     * retention policy is not evidence that this file has been removed. */
    for (const char *p = text; *p; ) {
        size_t length = strcspn(p, "\n");
        if (length >= 7 && !strncasecmp(p, "deleted", 7) &&
            (length == 7 || (length == 8 && strchr(".!", p[7])))) return true;
        p += length;
        if (*p) p++;
    }
    for (size_t i = 0; i < sizeof messages / sizeof *messages; i++)
        if (status_phrase(text, messages[i])) return true;
    return false;
}

static bool page_challenge(const char *body) {
    return body && (find_text(body, "_cf_chl_opt") || find_text(body, "<title>just a moment") ||
           find_text(body, "<title>attention required"));
}

static bool response_challenge(const Response *response) {
    return response->challenge || page_challenge(response->body);
}

static bool page_has_download(const char *html);
static bool element_text(const char *tag, const char *end, char *out, size_t cap);
static void page_size(const char *html, const char *anchor, const char *name, bool buzzheavier,
                      RemoteFileMetadata *file);

static bool page_message_missing(const char *body, size_t length, bool json_body, bool buzzheavier) {
    char *text = malloc(length + 1);
    if (!text) return false;
    bool missing = false;
    if (json_body) {
        const char *fields[] = {"status", "message", "error", "detail"};
        cJSON *json = cJSON_Parse(body);
        for (size_t i = 0; i < sizeof fields / sizeof *fields && !missing; i++) {
            const char *value = json_text(json, fields[i]);
            visible_text(value, value + strlen(value), text, length + 1);
            missing = missing_text(text);
        }
        cJSON_Delete(json);
    } else {
        visible_text(body, body + length, text, length + 1);
        if (buzzheavier && status_phrase(text, "try recovering this file")) {
            free(text);
            return true;
        }
        missing = missing_text(text);
        for (const char *tag = body; (tag = strchr(tag, '<')) != NULL; tag++) {
            const char *next = skip_markup(tag);
            if (next != tag) {
                if (!*next) break;
                tag = next - 1;
                continue;
            }
            if (!tag_is(tag + 1, "h1") && !tag_is(tag + 1, "title")) continue;
            const char *end = strchr(tag, '>');
            char heading[128];
            if (end && element_text(tag, end, heading, sizeof heading) && !strcasecmp(heading, "deleted")) {
                free(text);
                return !page_has_download(body);
            }
        }
        for (const char *tag = body; !missing && (tag = strchr(tag, '<')); ) {
            const char *next = skip_markup(tag);
            if (next != tag) { tag = next; continue; }
            const char *end = strchr(tag, '>');
            if (!end) break;
            const char *close = strchr(end + 1, '<');
            if (close && close - end < 128) {
                char status[16];
                visible_text(end + 1, close, status, sizeof status);
                missing = !strcasecmp(status, "deleted");
            }
            tag = end + 1;
        }
    }
    free(text);
    if (!missing || page_has_download(body)) return false;
    RemoteFileMetadata file = {0};
    page_size(body, NULL, "", buzzheavier, &file);
    return !file.confidence;
}

static bool response_missing(const Response *response) {
    if (cancelled(response) || response_challenge(response)) return false;
    if (response->status == 404 || response->status == 410) return true;
    if (!response->body || !response->used || response->truncated ||
        response->code != CURLE_OK) return false;
    bool buzzheavier = !strcasecmp(response->effective_host, "buzzheavier.com");
#ifdef ORBIT_TEST
    buzzheavier = buzzheavier || !strcmp(remote_browser_provider(response->url), "buzzheavier");
#endif
    if (response->status == 200 || (buzzheavier && response->status == 403))
        return page_message_missing(response->body, response->used, response->json, buzzheavier);
    if ((response->status != 401 && response->status != 403) || !remote_archive_url(response->url)) return false;
    char *text = malloc(response->used + 1);
    if (!text) return false;
    visible_text(response->body, response->body + response->used, text, response->used + 1);
    bool missing = status_phrase(text, "the item is not available due to issues with the item's content");
    free(text);
    return missing;
}

static bool archive_login_required(const Response *response) {
    if (!remote_archive_url(response->url) || response_challenge(response)) return false;
    return response->status == 401 || response->status == 403 ||
        (response->status == 200 && response->body && !response->truncated &&
         find_text(response->body, "you must log in to view this content"));
}

/* Probe public access first. Only retry denied Archive.org routes with the
 * saved cookies, so the catalogue knows which results require sign-in. */
static bool fetch_with_archive_login(const char *url, Response *response, bool *requires_login) {
    Response initial = *response;
    bool ok = fetch(url, response);
    *requires_login = archive_login_required(response);
    if (!*requires_login || !archive_account_signed_in()) return ok;

    free(response->body);
    *response = initial;
    response->archive_session = true;
    return fetch(url, response);
}

bool remote_download_html_missing(long status, const char *html, bool challenge) {
    if (challenge || page_challenge(html)) return false;
    if (status == 404 || status == 410) return true;
    return status == 200 && html && *html && page_message_missing(html, strlen(html), false, false);
}

bool remote_download_confirm_missing(const char *url, atomic_bool *cancel) {
    bool archive = remote_archive_url(url);
    Response page = {.cancel = cancel, .collect_page = true, .metadata_only = !archive,
                     .range = archive, .archive_session = archive};
    fetch(url, &page);
    bool missing = response_missing(&page);
    free(page.body);
    return missing;
}

static bool safe_page_name(const char *name) {
    if (!*name || *name == '.' || strpbrk(name, "/\\:") || strstr(name, "..") || !utf8_text(name))
        return false;
    char normalized[160];
    visible_text(name, name + strlen(name), normalized, sizeof normalized);
    return *normalized && !missing_text(normalized);
}

static const char *element_end(const char *tag, const char *end) {
    size_t length = tag_name_length(tag + 1);
    if (!length || end[-1] == '/') return NULL;
    unsigned depth = 1;
    for (const char *p = end + 1; (p = strchr(p, '<')); ) {
        const char *next = skip_markup(p);
        if (next != p) { p = next; continue; }
        bool closing = p[1] == '/';
        const char *name = p + 1 + closing;
        next = strchr(name, '>');
        if (!next || next - end > 4096) return NULL;
        if (tag_name_length(name) == length && !strncasecmp(name, tag + 1, length)) {
            if (closing && --depth == 0) return p;
            if (!closing && next[-1] != '/') depth++;
        }
        p = next + 1;
    }
    return NULL;
}

static bool element_text(const char *tag, const char *end, char *out, size_t cap) {
    const char *close = element_end(tag, end);
    if (!close || close - end > 4096) return false;
    visible_text(end + 1, close, out, cap);
    return *out != 0;
}

static bool page_has_download(const char *html) {
    for (const char *tag = html; (tag = strchr(tag, '<'));) {
        const char *skip = skip_markup(tag);
        if (skip != tag) { tag = skip; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        const char *current = tag;
        tag = end + 1;
        char label[160], href[2048];
        bool anchor = tag_is(current + 1, "a");
        bool input = tag_is(current + 1, "input");
        if (!anchor && !input && !tag_is(current + 1, "button")) continue;
        if (tag_attribute(current, end, "hidden", label, sizeof label) ||
            tag_attribute(current, end, "disabled", label, sizeof label) ||
            (tag_value(current, end, "aria-disabled", label, sizeof label) && !strcasecmp(label, "true")))
            continue;
        if (anchor && (!tag_value(current, end, "href", href, sizeof href) ||
                      (!strcmp(href, "#") && !tag_value(current, end, "onclick", label, sizeof label)))) continue;
        if (anchor && tag_attribute(current, end, "download", label, sizeof label)) return true;
        if (input && (!tag_value(current, end, "type", label, sizeof label) ||
                      (strcasecmp(label, "submit") && strcasecmp(label, "button")))) continue;
        if ((input ? tag_value(current, end, "value", label, sizeof label)
                   : element_text(current, end, label, sizeof label)) && phrase(label, "download"))
            return true;
        if (tag_value(current, end, "aria-label", label, sizeof label) && phrase(label, "download")) return true;
        if (tag_value(current, end, "title", label, sizeof label) && phrase(label, "download")) return true;
    }
    return false;
}

static bool title_filename(char *name, Release *file) {
    if (safe_page_name(name)) {
        int format = remote_provider_file_format(file, name, NULL);
        if (format >= 0) return format == 0;
    }
    if (!strncasecmp(name, "Download ", 9)) {
        memmove(name, name + 9, strlen(name + 9) + 1);
        if (*name == '"') memmove(name, name + 1, strlen(name));
    }
    if (safe_page_name(name) && !remote_provider_file_format(file, name, NULL)) return true;
    const char *separators[] = {" - ", " | ", " · ", " — ", "\""};
    for (size_t i = 0; i < sizeof separators / sizeof *separators; i++) {
        for (char *suffix = name; (suffix = strstr(suffix, separators[i])); suffix++) {
            char saved = *suffix;
            *suffix = 0;
            if (safe_page_name(name) && !remote_provider_file_format(file, name, NULL)) return true;
            *suffix = saved;
        }
    }
    return false;
}

static bool has_label(const char *labels, const char *name) {
    size_t n = strlen(name);
    for (const char *p = labels; (p = find_text(p, name)); p += n)
        if ((p == labels || isspace((unsigned char)p[-1])) &&
            (!p[n] || isspace((unsigned char)p[n]))) return true;
    return false;
}

static bool filename_label(const char *label) {
    return has_label(label, "filename") || has_label(label, "file-name") || has_label(label, "tier-name");
}

static bool page_filename(const Response *page, const Release *r, char *name, size_t cap, const char **anchor) {
    Release file = *r;
    bool datavaults = !strcmp(remote_browser_provider(page->url), "datavaults");
    char heading[sizeof file.filename] = "", title[512] = "";
    const char *heading_tag = NULL;
    for (const char *tag = page->body; (tag = strchr(tag, '<')); ) {
        const char *next = skip_markup(tag);
        if (next != tag) { tag = next; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        char label[128], value[512];
        bool named = (tag_is(tag + 1, "a") && tag_value(tag, end, "download", name, cap)) ||
                     tag_value(tag, end, "data-filename", name, cap);
        if (!named && datavaults && tag_is(tag + 1, "input") &&
            tag_value(tag, end, "type", label, sizeof label) && !strcasecmp(label, "hidden") &&
            tag_value(tag, end, "name", label, sizeof label) && !strcasecmp(label, "fname")) {
            named = tag_value(tag, end, "value", name, cap);
        }
        if (!named && ((tag_value(tag, end, "id", label, sizeof label) && filename_label(label)) ||
                       (tag_value(tag, end, "class", label, sizeof label) && filename_label(label)))) {
            named = tag_value(tag, end, "title", name, cap) || element_text(tag, end, name, cap);
        }
        if (named && safe_page_name(name)) {
            if (anchor) *anchor = tag;
            return true;
        }
        if (!heading_tag && tag_is(tag + 1, "h1") &&
            element_text(tag, end, heading, sizeof heading) && title_filename(heading, &file)) heading_tag = tag;
        if (tag_is(tag + 1, "meta") && tag_value(tag, end, "property", label, sizeof label) &&
            !strcasecmp(label, "og:title") && tag_value(tag, end, "content", value, sizeof value) &&
            title_filename(value, &file) && strlen(value) < cap) copy_text(title, sizeof title, value);
        if (!title[0] && tag_is(tag + 1, "title") &&
            element_text(tag, end, value, sizeof value) && title_filename(value, &file) &&
            strlen(value) < cap) copy_text(title, sizeof title, value);
        tag = end + 1;
    }
    if (anchor && (!title[0] || !strcasecmp(title, heading))) *anchor = heading_tag;
    copy_text(name, cap, *title ? title : heading_tag ? heading : "");
    return *name != 0;
}

/* A decimal separator denotes a decimal, except where a three-digit
 * suffix after a nonzero integer can also be a thousands grouping. */
static const char *size_value(const char *text, bool bytes_allowed, int64_t *size, bool *exact) {
    static const struct { const char *name; uint64_t factor; } units[] = {
        {"KiB", 1024}, {"MiB", 1048576}, {"GiB", 1073741824}, {"TiB", 1099511627776},
        {"KB", 1000}, {"MB", 1000000}, {"GB", 1000000000}, {"TB", 1000000000000}, {"B", 1},
    };
    const char *p = text;
    if (!isdigit((unsigned char)*p)) return NULL;
    double value = 0;
    while (isdigit((unsigned char)*p)) value = value * 10 + (*p++ - '0');
    bool decimal = (*p == '.' || *p == ',') && isdigit((unsigned char)p[1]);
    if (decimal) {
        bool comma = *p == ',';
        const char *from = ++p;
        double scale = 1;
        for (; isdigit((unsigned char)*p); p++) scale /= 10, value += (*p - '0') * scale;
        if (comma && p - from == 3 && text[0] != '0') return NULL;
    }
    while (isspace((unsigned char)*p)) p++;
    *exact = bytes_allowed && !decimal && (!*p || (*p == 'B' && !p[1]));
    if (!*exact) {
        size_t i = 0;
        for (; i < sizeof units / sizeof *units; i++) {
            size_t n = strlen(units[i].name);
            if (!strncasecmp(p, units[i].name, n) &&
                !isalnum((unsigned char)p[n]) && p[n] != '/') {
                value *= units[i].factor;
                p += n;
                break;
            }
        }
        if (i == sizeof units / sizeof *units) return NULL;
    } else if (*p) p++;
    if (!(value >= 1 && value <= 9007199254740991.0)) return NULL;
    *size = (int64_t)(value + 0.5);
    return p;
}

static bool size_label(const char *label) {
    return phrase(label, "size") || phrase(label, "filesize") || phrase(label, "file_size") ||
        phrase(label, "contentSize") || phrase(label, "tier-feat");
}

static bool unrelated_size(const char *text) {
    const char *terms[] = {"premium", "plan", "plans", "pricing", "quota", "storage", "speed",
        "bandwidth", "demo", "example", "maximum", "max upload", "upload limit", "transfer limit",
        "daily limit", "per day", "per second", "advertisement", "traffic"};
    for (size_t i = 0; i < sizeof terms / sizeof *terms; i++)
        if (phrase(text, terms[i])) return true;
    return false;
}

typedef struct {
    int rank;
    int64_t size;
    bool exact, ambiguous;
} SizeChoice;

static void offer_size(SizeChoice *best, RemoteFileMetadata *file, const char *text,
                       int rank, bool structured, bool whole) {
    if (!structured && unrelated_size(text)) return;
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p > text && (isalnum((unsigned char)p[-1]) ||
            p[-1] == '.' || p[-1] == ',' || p[-1] == '-'))) continue;
        int64_t size;
        bool exact;
        const char *end = size_value(p, structured, &size, &exact);
        if (!end) continue;
        const char *tail = end;
        while (isspace((unsigned char)*tail)) tail++;
        if (*tail == '/' || (whole && (p != text || *tail))) continue;
        int score = exact ? 4 : rank;
        if (file->candidate_count < sizeof file->candidates / sizeof *file->candidates)
            file->candidates[file->candidate_count++] = size;
        if (score > best->rank) *best = (SizeChoice){score, size, exact, false};
        else if (score == best->rank && size != best->size) best->ambiguous = true;
        if (!*end) break;
        p = end - 1;
    }
}

static bool download_label(const char *label) {
    return phrase(label, "download") || phrase(label, "fileinfo") || phrase(label, "file-info");
}

static void page_size(const char *html, const char *anchor, const char *name, bool buzzheavier,
                      RemoteFileMetadata *file) {
    SizeChoice best = {0}, button_size = {0};
    const char *button_end = NULL;
    const char *first_tag = strchr(html, '<');
    if (!first_tag) offer_size(&best, file, html, 1, false, false);
    const char *fields[] = {"data-size-bytes", "data-size", "data-filesize", "data-file-size", "contentSize"};
    for (const char *tag = html; (tag = strchr(tag, '<'));) {
        const char *skip = skip_markup(tag);
        if (skip != tag) { tag = skip; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        const char *current = tag;
        tag = end + 1;
        if (!tag_name_length(current + 1)) continue;
        char label[160] = "", id[96] = "", value[512], property[64];
        tag_value(current, end, "class", label, sizeof label);
        tag_value(current, end, "id", id, sizeof id);
        char content[512] = "";
        bool has_content = element_text(current, end, content, sizeof content);
        bool leaf_context = tag_is(current + 1, "p") || tag_is(current + 1, "span") ||
            tag_is(current + 1, "td") || tag_is(current + 1, "li");
        if (unrelated_size(label) || unrelated_size(id) ||
            (has_content && (leaf_context || tag_value(current, end, "data-size", value, sizeof value)) && unrelated_size(content))) {
            const char *close = element_end(current, end);
            if (close) {
                const char *after = strchr(close, '>');
                if (after) tag = after + 1;
            }
            continue;
        }
        bool specific = size_label(label) || size_label(id);
        if (buzzheavier && tag_is(current + 1, "a") && phrase(label, "download-btn"))
            button_end = element_end(current, end);
        if (button_end && current < button_end && has_content &&
            tag_is(current + 1, "span") && phrase(label, "size"))
            offer_size(&button_size, file, content, 3, false, true);
        bool bound = current == anchor || download_label(label) || download_label(id);
        bool nearby = anchor && current > anchor && current - anchor < 256;
        if (tag_value(current, end, "itemprop", property, sizeof property) && size_label(property)) {
            specific = true;
            if (tag_value(current, end, "content", value, sizeof value))
                offer_size(&best, file, value, bound ? 3 : 2, true, true);
        }
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++)
            if (tag_value(current, end, fields[i], value, sizeof value))
                offer_size(&best, file, value, bound ? 3 : 2, true, true);
        if (tag_value(current, end, "property", property, sizeof property) ||
            tag_value(current, end, "name", property, sizeof property)) {
            bool description = !strcasecmp(property, "description") || !strcasecmp(property, "og:description");
            if (tag_value(current, end, "content", value, sizeof value) &&
                (size_label(property) || (description && (*name || phrase(value, "file")))))
                offer_size(&best, file, value, size_label(property) ? 2 : 1, size_label(property), false);
        }
        bool text_element = tag_is(current + 1, "span") || tag_is(current + 1, "p") ||
                            tag_is(current + 1, "dd") || tag_is(current + 1, "td") || tag_is(current + 1, "tr") ||
                            tag_is(current + 1, "li") || tag_is(current + 1, "b") ||
                            tag_is(current + 1, "strong") || tag_is(current + 1, "div") || tag_is(current + 1, "main") ||
                            tag_is(current + 1, "a") || tag_is(current + 1, "button");
        if (has_content && (specific || bound || text_element)) {
            const char *file_size = find_text(content, "file size");
            bool named = *name && find_text(content, name);
            bool download = download_label(content);
            if (!strncasecmp(content, "size:", 5)) {
                content[strcspn(content, "\n")] = 0;
                offer_size(&best, file, content + 5, 3, false, false);
            } else if (file_size && !unrelated_size(content)) {
                offer_size(&best, file, file_size, 3, false, false);
            } else if (specific || bound || named || nearby || download)
                offer_size(&best, file, content, bound || named || nearby ? 3 : 2, false, false);
            else if (!anchor && !*name) offer_size(&best, file, content, 1, false, true);
        }
        /* Text directly under body/main or split by formatting also serves
         * providers that don't put their size in a named element. */
        const char *next = strchr(tag, '<');
        if ((specific || bound || nearby) && (!next || next - tag < (ptrdiff_t)sizeof value) &&
            html_text(tag, next ? (size_t)(next - tag) : strlen(tag), value, sizeof value))
            offer_size(&best, file, value, specific ? 2 : 1, false, false);
    }
    const char *json_fields[] = {"sizeBytes", "fileSize", "contentSize"};
    for (const char *script = html; (script = find_text(script, "<script")); script += 7) {
        const char *end = strchr(script, '>');
        char type[48];
        if (!end || !tag_value(script, end, "type", type, sizeof type) ||
            (strcasecmp(type, "application/ld+json") && strcasecmp(type, "application/json"))) continue;
        const char *close = find_text(end + 1, "</script>");
        if (!close || close - end > 32768) continue;
        cJSON *json = cJSON_ParseWithLength(end + 1, (size_t)(close - end - 1));
        const char *json_name = json_text(json, "name");
        if (!*json_name || !*name || !strcmp(json_name, name)) {
            for (size_t i = 0; i < sizeof json_fields / sizeof *json_fields; i++) {
                const cJSON *field = cJSON_GetObjectItemCaseSensitive(json, json_fields[i]);
                char value[32];
                if (cJSON_IsString(field)) offer_size(&best, file, field->valuestring, 2, true, true);
                else if (cJSON_IsNumber(field) && field->valuedouble > 0 &&
                         field->valuedouble <= 9007199254740991.0 && floor(field->valuedouble) == field->valuedouble) {
                    snprintf(value, sizeof value, "%.0f", field->valuedouble);
                    offer_size(&best, file, value, 2, true, true);
                }
            }
        }
        cJSON_Delete(json);
    }
    /* Prefer the file's badge to estimates in surrounding page text. */
    if (button_size.rank && !best.exact) best = button_size;
    file->status = best.ambiguous ? REMOTE_METADATA_AMBIGUOUS : best.rank ? REMOTE_METADATA_FOUND : REMOTE_METADATA_NO_SIZE;
    file->confidence = best.rank;
    if (!best.ambiguous) {
        file->size = best.size;
        file->exact = best.exact;
    }
}

static bool resolve_href(const char *base, const char *href, char *out, size_t cap) {
    CURLU *parts = curl_url();
    char *url = NULL;
    bool ok = parts && !curl_url_set(parts, CURLUPART_URL, base, 0) &&
              !curl_url_set(parts, CURLUPART_URL, href, CURLU_ALLOW_SPACE | CURLU_URLENCODE | CURLU_DISALLOW_USER) &&
              !curl_url_get(parts, CURLUPART_URL, &url, 0) &&
              strlen(url) < cap && remote_https_url(url);
    if (ok) copy_text(out, cap, url);
    curl_free(url);
    curl_url_cleanup(parts);
    return ok;
}

typedef struct {
    char url[2048], filename[160];
    bool pinned, recognized;
} PageLink;

static int page_link(const Response *page, const Release *r, const Release *locator, PageLink *link) {
    for (const char *tag = page->body; (tag = strchr(tag, '<')); tag++) {
        const char *next = skip_markup(tag);
        if (next != tag) { tag = next - 1; continue; }
        if ((tag[1] != 'a' && tag[1] != 'A') || !isspace((unsigned char)tag[2])) continue;
        const char *end = strchr(tag, '>');
        if (!end) break;
        PageLink candidate = {0};
        char href[sizeof candidate.url], named[sizeof candidate.filename];
        Release file = *r;
        if (!tag_value(tag, end, "href", href, sizeof href) ||
            !resolve_href(page->url, href, candidate.url, sizeof candidate.url) ||
            !strcmp(candidate.url, page->url) || *remote_browser_provider(candidate.url)) continue;
        bool download = tag_attribute(tag, end, "download", named, sizeof named);
        if (!download) named[0] = 0;
        if (*named && !safe_page_name(named)) continue;
        if (*named) {
            copy_text(candidate.filename, sizeof candidate.filename, named);
            candidate.pinned = true;
        } else if (!provider_disposition_name(candidate.url, candidate.filename, sizeof candidate.filename)) {
            candidate.filename[0] = 0;
            remote_url_filename(candidate.url, candidate.filename, sizeof candidate.filename);
        }
        if (locator) {
            Release match = *locator;
            if (!match.filename[0] && safe_page_name(candidate.filename)) {
                int format = remote_provider_file_format(&match, candidate.filename, NULL);
                if (format > 0) continue;
                if (format < 0 && candidate.pinned)
                    copy_text(match.filename, sizeof match.filename, candidate.filename);
            }
            if (!provider_capture_url(&match, candidate.url)) continue;
            if (match.filename[0]) {
                copy_text(candidate.filename, sizeof candidate.filename, match.filename);
                candidate.pinned = true;
            }
        } else {
            int format = remote_provider_file_format(&file, candidate.filename, NULL);
            if (format > 0) continue;
            candidate.recognized = !format;
            if (!download && !candidate.recognized) {
                char label[96];
                if (!element_text(tag, end, label, sizeof label) ||
                    (strcasecmp(label, "Download") && strcasecmp(label, "Download file"))) continue;
            }
        }
        if (!safe_page_name(candidate.filename)) candidate.filename[0] = 0;
        if (link->url[0]) {
            if (strcmp(link->url, candidate.url) || strcmp(link->filename, candidate.filename)) return -2;
            candidate.pinned |= link->pinned;
        }
        *link = candidate;
    }
    return link->url[0] ? 0 : -1;
}

static void file_metadata(const Release *file, RemoteFileMetadata *metadata) {
    copy_text(metadata->filename, sizeof metadata->filename, file->filename);
    if (strcmp(file->format, "Unknown")) copy_text(metadata->format, sizeof metadata->format, file->format);
    copy_text(metadata->compression, sizeof metadata->compression, file->compression);
}

static const char *mime_suffix(const char *mime) {
    const struct { const char *mime, *suffix; } types[] = {
        {"application/vnd.rar", ".rar"}, {"application/x-rar-compressed", ".rar"},
        {"application/zip", ".zip"}, {"application/x-7z-compressed", ".7z"},
    };
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (!strcasecmp(mime, types[i].mime)) return types[i].suffix;
    return NULL;
}

static bool page_value(const char *html, const char *key, char *out, size_t cap) {
    for (const char *p = html; (p = find_text(p, key)); p += strlen(key)) {
        const char *value = p + strlen(key);
        if (*value == '"' || *value == '\'') value++;
        value += strspn(value, " \t\r\n");
        if (*value != ':' && *value != '=') continue;
        value++;
        value += strspn(value, " \t\r\n");
        if (*value != '"' && *value != '\'') continue;
        const char *end = strchr(value + 1, *value);
        if (end && end - value > 1 && (size_t)(end - value) <= cap)
            return html_text(value + 1, (size_t)(end - value - 1), out, cap);
    }
    for (const char *tag = html; (tag = strchr(tag, '<')); ) {
        const char *end = strchr(tag, '>');
        if (!end) break;
        char name[32];
        if (end - tag <= 2048 &&
            (tag_is(tag + 1, "meta") || tag_is(tag + 1, "input")) &&
            tag_value(tag, end, "name", name, sizeof name) && !strcasecmp(name, key) &&
            (tag_value(tag, end, "content", out, cap) || tag_value(tag, end, "value", out, cap)))
            return true;
        tag = end + 1;
    }
    return false;
}

static bool rootz_page(const char *url) {
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *path = strchr(url + 17, '/');
        return path && !strncmp(path, "/metadata/rootz/", 16);
    }
#endif
    return !strncasecmp(url, "https://rootz.so/", 17) || !strncasecmp(url, "https://www.rootz.so/", 21);
}

static bool rootz_page_token(const char *html, char *token, size_t cap) {
    if (page_value(html, "pageToken", token, cap) ||
        page_value(html, "page-token", token, cap) ||
        page_value(html, "page_token", token, cap) ||
        page_value(html, "__page_token__", token, cap) ||
        page_value(html, "X-Page-Token", token, cap)) return true;

    /* Next.js sends the page props inside a JSON-encoded Flight string. */
    const char *marker = "self.__next_f.push(";
    for (const char *p = html; (p = strstr(p, marker)); ) {
        p += strlen(marker);
        const char *end;
        cJSON *push = cJSON_ParseWithOpts(p, &end, false);
        const cJSON *type = cJSON_GetArrayItem(push, 0);
        const cJSON *flight = cJSON_GetArrayItem(push, 1);
        bool found = push && *end == ')' && cJSON_IsArray(push) &&
            cJSON_IsNumber(type) && type->valuedouble == 1 && cJSON_IsString(flight) &&
            page_value(flight->valuestring, "pageToken", token, cap);
        cJSON_Delete(push);
        if (found) return true;
    }
    return false;
}

static void rootz_metadata(const Response *page, const Release *r, RemoteFileMetadata *metadata) {
    char id[128], token[512], path[192], url[2048], header[sizeof token + 16];
    if (!rootz_page(page->url) || !page->body ||
        (!page_value(page->body, "shortId", id, sizeof id) &&
         !page_value(page->body, "short-id", id, sizeof id) &&
         !page_value(page->body, "short_id", id, sizeof id) &&
         !remote_url_filename(page->url, id, sizeof id)) ||
        !rootz_page_token(page->body, token, sizeof token)) return;
    if (strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(id) ||
        strspn(token, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.+/=") != strlen(token)) return;
    snprintf(path, sizeof path, "/api/files/download-by-short?shortId=%s", id);
    if (!resolve_href(page->url, path, url, sizeof url)) return;
    snprintf(header, sizeof header, "X-Page-Token: %s", token);
    Response api = {.cancel = page->cancel, .collect_page = true, .page_token = header};
    bool ok = fetch(url, &api);
    cJSON *json = api.body && !api.truncated && !response_challenge(&api) &&
        (ok || api.status == 404 || api.status == 410) ? cJSON_Parse(api.body) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!data) data = json;
    else if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "success"))) data = NULL;
    const char *status = json_text(data, "status");
    if (missing_text(status) || !strcasecmp(status, "removed") || !strcasecmp(status, "not_found") ||
        !strcasecmp(status, "file_not_found") ||
        response_missing(&api)) {
        metadata->status = REMOTE_METADATA_MISSING;
        metadata->size = 0;
        metadata->filename[0] = 0;
    } else if (ok && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "downloadAllowed"))) {
        RemoteFileMetadata exact = {0};
        const char *name = json_text(data, "fileName");
        const char *mime = json_text(data, "mimeType");
        Release file = *r;
        if (strlen(name) < sizeof file.filename && safe_page_name(name) &&
            !remote_file_detected_format(&file, name, mime_suffix(mime))) file_metadata(&file, &exact);
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(data, "size");
        if (cJSON_IsNumber(size) && size->valuedouble > 0 && size->valuedouble <= 9007199254740991.0 &&
            floor(size->valuedouble) == size->valuedouble) {
            exact.size = (int64_t)size->valuedouble;
            exact.exact = true;
        } else if (cJSON_IsString(size) && *size->valuestring &&
                   strspn(size->valuestring, "0123456789") == strlen(size->valuestring)) {
            size_value(size->valuestring, true, &exact.size, &exact.exact);
        }
        if (exact.filename[0] || exact.size) {
            if (!exact.size) { exact.size = metadata->size; exact.exact = metadata->exact; }
            exact.attempted = metadata->attempted;
            exact.http_status = metadata->http_status;
            copy_text(exact.effective_host, sizeof exact.effective_host, metadata->effective_host);
            exact.confidence = exact.exact ? 4 : metadata->confidence;
            exact.status = exact.size ? REMOTE_METADATA_FOUND : REMOTE_METADATA_NO_SIZE;
            *metadata = exact;
        }
    }
    cJSON_Delete(json);
    free(api.body);
}

enum { PSPKG_GAME_LABEL = 1, PSPKG_OTHER_LABEL = 2, PSPKG_CONTEXT_CHARS = 350 };

static unsigned pspkg_link_labels(const char *text) {
    unsigned labels = 0;
    if (phrase(text, "game") || phrase(text, "pkg") || phrase(text, "fpkg") ||
        strstr(text, "اللعبة") || strstr(text, "العبة"))
        labels |= PSPKG_GAME_LABEL;
    if (phrase(text, "update") || phrase(text, "updates") || phrase(text, "patch") ||
        phrase(text, "backport") || phrase(text, "dlc") || phrase(text, "dlcs") ||
        strstr(text, "تحديث") || strstr(text, "إضافات") || strstr(text, "الاضافات"))
        labels |= PSPKG_OTHER_LABEL;
    return labels;
}

typedef struct { const char *tag, *end; } PspkgParent;

static bool pspkg_void_element(const char *name) {
    const char *tags[] = {"area", "base", "br", "col", "embed", "hr", "img", "input",
                          "link", "meta", "param", "source", "track", "wbr"};
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++)
        if (tag_is(name, tags[i])) return true;
    return false;
}

static unsigned pspkg_nearby_labels(const PspkgParent *parents, size_t depth) {
    unsigned labels = 0;
    /* Match the PSPKG sync script's short, local text windows. */
    for (unsigned i = 0; depth && i < 3; i++) {
        const PspkgParent *parent = &parents[--depth];
        if (tag_is(parent->tag + 1, "body") || tag_is(parent->tag + 1, "html")) break;
        char text[PSPKG_CONTEXT_CHARS * 4 + 2];
        if (!element_text(parent->tag, parent->end, text, sizeof text)) continue;
        unsigned characters = 0;
        for (const unsigned char *p = (const unsigned char *)text; *p; p++)
            characters += (*p & 0xc0) != 0x80;
        if (characters <= PSPKG_CONTEXT_CHARS) labels |= pspkg_link_labels(text);
    }
    return labels;
}

static cJSON *pspkg_page_links(const char *html, const char *page, const char *heading, bool public_page) {
    cJSON *links = cJSON_CreateArray();
    if (!links) return NULL;
    char edition[256] = "";
    char group[128] = "", password[sizeof ((Release *)0)->archive_password] = "";
    SizeChoice edition_size = {0};
    RemoteFileMetadata candidates = {0};
    if (heading) copy_text(edition, sizeof edition, heading);
    const Release locator = {.source = "pspkg"};
    const char *last_slash = strrchr(page, '/');
    size_t directory = last_slash ? (size_t)(last_slash + 1 - page) : 0;
    PspkgParent parents[64];
    size_t depth = 0, edition_depth = 0;
    for (const char *tag = html; (tag = strchr(tag, '<')) && cJSON_GetArraySize(links) < PSPKG_LINK_LIMIT;) {
        const char *next = skip_markup(tag);
        if (next != tag) { tag = next; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        const char *current = tag;
        tag = end + 1;
        const char *name = current + 1;
        if (*name == '/') {
            size_t length = tag_name_length(++name);
            for (size_t i = depth; length && i; i--)
                if (tag_name_length(parents[i - 1].tag + 1) == length &&
                    !strncasecmp(name, parents[i - 1].tag + 1, length)) {
                    depth = i - 1;
                    break;
                }
            if (depth < edition_depth) {
                copy_text(edition, sizeof edition, heading ? heading : "");
                edition_depth = 0;
            }
            continue;
        }
        bool anchor = tag_is(name, "a");
        if (!anchor && tag_name_length(name) && end[-1] != '/' && !pspkg_void_element(name)) {
            if (depth == sizeof parents / sizeof *parents) break;
            parents[depth++] = (PspkgParent){current, end};
        }
        char classes[128];
        if (heading && tag_value(current, end, "class", classes, sizeof classes)) {
            if (has_label(classes, "dpx-meta-size")) {
                char text[128];
                if (element_text(current, end, text, sizeof text))
                    offer_size(&edition_size, &candidates, text, 3, false, false);
            } else if (has_label(classes, "dpx-password")) {
                const char *close = element_end(current, end);
                if (!close || memchr(end + 1, '<', (size_t)(close - end - 1)) ||
                    !html_text(end + 1, (size_t)(close - end - 1), password, sizeof password) ||
                    !utf8_text(password)) password[0] = 0;
            } else if (has_label(classes, "dpx-group-label")) {
                group[0] = 0;
                element_text(current, end, group, sizeof group);
            }
        }
        if (tag_is(current + 1, "h2") || tag_is(current + 1, "h3") || tag_is(current + 1, "h4")) {
            edition[0] = 0;
            element_text(current, end, edition, sizeof edition);
            edition_depth = depth ? depth - 1 : 0;
            continue;
        }
        if (!anchor) continue;
        char raw[2048], href[2048], label[128] = "";
        if (!tag_value(current, end, "href", raw, sizeof raw) ||
            !resolve_href(page, raw, href, sizeof href)) continue;
        element_text(current, end, label, sizeof label);
        const char *provider = remote_browser_provider(href);
        bool intermediate = !public_page && directory && !strncmp(href, page, directory) &&
            !strncmp(href + directory, "download.php?", sizeof "download.php?" - 1);
        unsigned labels = pspkg_link_labels(edition);
        if (!intermediate) {
            if (!provider_capture_url(&locator, href)) continue;
            unsigned local = pspkg_link_labels(label) | pspkg_nearby_labels(parents, depth);
            if (heading) local |= pspkg_link_labels(heading);
            if (!local) continue;
            labels |= local;
        }
        bool base = !(labels & PSPKG_OTHER_LABEL);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "url", href);
        cJSON_AddStringToObject(item, "name", provider);
        cJSON_AddStringToObject(item, "label", label);
        if (*edition) cJSON_AddStringToObject(item, "edition", edition);
        if (*group) cJSON_AddStringToObject(item, "group", group);
        if (*password) cJSON_AddStringToObject(item, "archivePassword", password);
        char filename[sizeof locator.filename] = "";
        if (!tag_value(current, end, "download", filename, sizeof filename) &&
            !tag_value(current, end, "data-filename", filename, sizeof filename) && !*provider && !intermediate)
            remote_url_filename(href, filename, sizeof filename);
        Release detected = {0};
        if (safe_page_name(filename) && !remote_provider_file_format(&detected, filename, NULL))
            cJSON_AddStringToObject(item, "filename", filename);
        else filename[0] = 0;
        bool unsupported = phrase(edition, "zstd") || phrase(edition, "lz4") ||
                           phrase(label, "zstd") || phrase(label, "lz4");
        bool multipart = phrase(edition, "part") || phrase(edition, "multipart") ||
                         phrase(label, "part") || phrase(label, "volume");
        const char *format = remote_label_format(edition);
        if (!*format || !strcmp(format, "Game folder")) format = remote_label_format(group);
        if (!*format || !strcmp(format, "Game folder")) format = remote_label_format(label);
        if (image_suffix_for_format(detected.format)) format = detected.format;
        if (*format && strcmp(format, "Game folder")) cJSON_AddStringToObject(item, "format", format);
        const char *compressions[] = {"ZSTD", "LZ4", "RAR", "ZIP", "7z"};
        for (size_t i = 0; i < sizeof compressions / sizeof *compressions; i++)
            if (phrase(edition, compressions[i]) || phrase(label, compressions[i])) {
                cJSON_AddStringToObject(item, "compression", compressions[i]);
                break;
            }
        const char *regions[] = {"EUR", "USA", "JPN", "ASIA", "WORLD"};
        for (size_t i = 0; i < sizeof regions / sizeof *regions; i++)
            if (phrase(edition, regions[i])) {
                cJSON_AddStringToObject(item, "region", regions[i]);
                break;
            }
        char version[sizeof ((PspkgLink *)0)->version];
        pspkg_version(edition, filename, version, sizeof version);
        if (version[0]) cJSON_AddStringToObject(item, "version", version);
        const char *part = find_text(label, "part");
        if (!part) part = find_text(edition, "part");
        if (part && multipart) {
            part += 4;
            part += strspn(part, " #._-");
            if (isdigit((unsigned char)*part)) cJSON_AddNumberToObject(item, "part", strtol(part, NULL, 10));
        }
        if (intermediate) cJSON_AddTrueToObject(item, "intermediate");
        if (unsupported) cJSON_AddStringToObject(item, "filterReason", "unsupported-format");
        else if (multipart) cJSON_AddStringToObject(item, "filterReason", "multipart");
        else if (!base) cJSON_AddStringToObject(item, "filterReason", "unsupported-category");
        else cJSON_AddStringToObject(item, "filterReason", "");
        cJSON_AddBoolToObject(item, "metadataEligible", base && !unsupported && !multipart);
        SizeChoice size = edition_size;
        RemoteFileMetadata text_candidates = {0};
        offer_size(&size, &text_candidates, edition, 2, false, false);
        offer_size(&size, &text_candidates, label, 2, false, false);
        if (size.size && !size.ambiguous) cJSON_AddNumberToObject(item, "sizeEstimateBytes", (double)size.size);
        const char *title_id = pspkg_title_id(edition);
        if (title_id) {
            char id[10];
            snprintf(id, sizeof id, "%.9s", title_id);
            cJSON_AddStringToObject(item, "titleId", id);
        }
        cJSON_AddItemToArray(links, item);
    }
    return links;
}

cJSON *remote_pspkg_links(const char *html, const char *page) {
    return pspkg_page_links(html, page, NULL, true);
}

cJSON *remote_pspkg_session_links(const char *html, const char *page, const char *heading) {
    return pspkg_page_links(html, page, heading, false);
}

static cJSON *pspkg_field_labels(const char *html) {
    cJSON *fields = cJSON_CreateObject();
    char edition[128] = "";
    for (const char *tag = html; (tag = strchr(tag, '<')) && cJSON_GetArraySize(fields) < PSPKG_LINK_LIMIT;) {
        const char *next = skip_markup(tag);
        if (next != tag) { tag = next; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        char label[96], id[32];
        if (tag_value(tag, end, "class", label, sizeof label)) {
            if (has_label(label, "dpx-edition-title")) element_text(tag, end, edition, sizeof edition);
            else if (has_label(label, "dpx-gate") && *edition &&
                     tag_value(tag, end, "data-field-id", id, sizeof id) &&
                     strspn(id, "0123456789") == strlen(id) &&
                     !cJSON_HasObjectItem(fields, id)) cJSON_AddStringToObject(fields, id, edition);
        }
        tag = end + 1;
    }
    return fields;
}

static bool response_interactive(const Response *page);

cJSON *remote_pspkg_public_fields(const char *url, RemoteMetadataStatus *status, atomic_bool *cancel) {
    Response page = {.cancel = cancel, .collect_page = true, .metadata_only = true};
    bool ok = fetch(url, &page);
    *status = REMOTE_METADATA_NETWORK;
    if (response_challenge(&page)) *status = REMOTE_METADATA_CHALLENGE;
    else if (page.status == 404 || page.status == 410 || response_missing(&page)) *status = REMOTE_METADATA_MISSING;
    else if (response_interactive(&page)) *status = REMOTE_METADATA_INTERACTIVE;
    else if (ok) *status = REMOTE_METADATA_NO_SIZE;
    const char *path = strstr(page.url, "://");
    path = path ? strchr(path + 3, '/') : NULL;
    cJSON *fields = ok && page.body && !page.truncated && path && strncmp(path, "/ar/", 4)
        ? pspkg_field_labels(page.body) : NULL;
    free(page.body);
    return fields;
}

static void download_page_metadata(const Response *page, const Release *r, RemoteFileMetadata *metadata) {
    Release file = *r;
    char name[sizeof file.filename] = "";
    const char *anchor = NULL;
    bool named = page_filename(page, r, name, sizeof name, &anchor);
    if (!named) {
        PageLink link = {0};
        if (!page_link(page, r, NULL, &link) && (link.pinned || link.recognized)) {
            copy_text(name, sizeof name, link.filename);
            named = true;
        }
    }
    page_size(page->body, anchor, name, !strcmp(remote_browser_provider(page->url), "buzzheavier"), metadata);
    const char *title = find_text(page->body, "<title>");
    char title_text[512];
    if (title && element_text(title, title + 6, title_text, sizeof title_text) &&
        (phrase(title_text, "LZ4") || phrase(title_text, "ZSTD"))) {
        metadata->status = REMOTE_METADATA_UNSUPPORTED;
        metadata->size = 0;
        metadata->exact = false;
    } else if (named) {
        int format = remote_provider_file_format(&file, name, NULL);
        if (!format && r->compression[0] && strcmp(r->compression, file.compression)) format = 1;
        if (!format) file_metadata(&file, metadata);
        else if (format > 0) {
            metadata->status = REMOTE_METADATA_UNSUPPORTED;
            metadata->size = 0;
            metadata->confidence = 0;
            metadata->exact = false;
        } else if (safe_page_name(name)) copy_text(metadata->filename, sizeof metadata->filename, name);
    }
}

static void archive_metadata(const Response *page, const Release *r, RemoteFileMetadata *metadata);
static int archive_file(const char *page, const Release *r, atomic_bool *cancel, char *url, size_t cap,
                        RemoteFileMetadata *file_info);

bool remote_pspkg_provider_metadata(const char *url, atomic_bool *cancel, RemoteFileMetadata *metadata) {
    const char *provider = remote_browser_provider(url);
    if (!*provider || !strcmp(provider, "pspkg")) return false;
    Response page = {.cancel = cancel, .collect_page = true, .metadata_only = true, .html_only = true};
    bool received = fetch_with_archive_login(url, &page, &metadata->archive_requires_login);
    if (archive_login_required(&page)) metadata->status = REMOTE_METADATA_ARCHIVE_LOGIN;
    else if (response_missing(&page)) metadata->status = REMOTE_METADATA_MISSING;
    bool ok = received && page.page && !page.json && !page.plain_text &&
        page.body && !page.truncated && !response_challenge(&page) &&
        metadata->status != REMOTE_METADATA_MISSING && metadata->status != REMOTE_METADATA_ARCHIVE_LOGIN &&
        !strcmp(provider, remote_browser_provider(page.url));
    if (ok) {
        Release file = {.remote_page = true};
        download_page_metadata(&page, &file, metadata);
        if (rootz_page(page.url)) rootz_metadata(&page, &file, metadata);
        if (!strcmp(provider, "archive")) archive_metadata(&page, &file, metadata);
        ok = metadata->status != REMOTE_METADATA_MISSING && metadata->status != REMOTE_METADATA_UNSUPPORTED &&
             metadata->status != REMOTE_METADATA_ARCHIVE_LOGIN;
    }
    free(page.body);
    return ok;
}

const char *remote_pspkg_route(const char *url, bool intermediate, atomic_bool *cancel,
                               char *out, size_t cap, RemoteFileMetadata *metadata) {
    if (!remote_https_url(url)) return "unsafe-url";
    bool file_candidate = !intermediate && !*remote_browser_provider(url);
    Response response = {.cancel = cancel, .collect_page = true, .range = file_candidate, .use_http1 = intermediate};
    bool requires_login;
    bool ok = fetch_with_archive_login(url, &response, &requires_login);
    const char *status = "";
    if (response.redirect_rejected || response.code == CURLE_TOO_MANY_REDIRECTS ||
        response.code == CURLE_UNSUPPORTED_PROTOCOL || response.code == CURLE_URL_MALFORMAT ||
        (response.status >= 300 && response.status < 400 && !ok)) status = "unsafe-url";
    else if (archive_login_required(&response)) status = "archive-login-required";
    else if (response_missing(&response)) status = "missing-page";
    else if (*remote_browser_provider(response.url) && strcmp(remote_browser_provider(response.url), "pspkg") &&
             (ok || response_challenge(&response) || response_interactive(&response))) {
        if (strlen(response.url) >= cap) status = "unsafe-url";
        else {
            copy_text(out, cap, response.url);
            if (response_challenge(&response)) status = "verify-required";
        }
    } else if (response_challenge(&response) || response_interactive(&response) ||
               (intermediate && (response.status == 401 || response.status == 403 ||
                strstr(response.url, "/download.php?")))) status = "verify-required";
    else if (!ok || !response.url[0] || strlen(response.url) >= cap) status = "provider-unavailable";
    else if (response.page && !*remote_browser_provider(response.url)) status = "unsupported-provider";
    else copy_text(out, cap, response.url);
    if (metadata && ok && !*status && !response_challenge(&response)) {
        Release file = {.remote_page = true};
        if (response.page && response.body && !response.truncated) {
            download_page_metadata(&response, &file, metadata);
            if (rootz_page(response.url)) rootz_metadata(&response, &file, metadata);
            if (remote_archive_url(response.url)) archive_metadata(&response, &file, metadata);
            if (metadata->status == REMOTE_METADATA_MISSING) status = "missing-page";
            else if (metadata->status == REMOTE_METADATA_ARCHIVE_LOGIN) status = "archive-login-required";
        } else if (!response.page && !file_from_response(&file, &response, url, NULL)) {
            file_metadata(&file, metadata);
            metadata->size = file.size;
            metadata->exact = true;
            metadata->status = REMOTE_METADATA_FOUND;
        }
    }
    if (metadata) metadata->archive_requires_login |= requires_login;
    free(response.body);
    return status;
}

const char *remote_metadata_status(RemoteMetadataStatus status) {
    static const char *const states[] = {
        "not-attempted", "found", "missing", "interactive-verification", "challenge", "timeout",
        "network-tls-failure", "http-error", "redirected-to-invalid-host", "page-too-large",
        "parse-no-size", "ambiguous-size", "unsupported-format", "archive-login-required",
    };
    return states[status];
}

static bool response_interactive(const Response *page) {
    if (page->status == 401) return true;
    if (!page->body || !page->used) return false;
    char *text = malloc(page->used + 1);
    if (!text) return false;
    visible_text(page->body, page->body + page->used, text, page->used + 1);
    const char *phrases[] = {"Awaiting verification", "verify you are human", "complete the captcha",
        "verification required", "login required", "sign in to download", "log in to download",
        "please wait before downloading"};
    bool interactive = false;
    for (size_t i = 0; i < sizeof phrases / sizeof *phrases && !interactive; i++)
        interactive = find_text(text, phrases[i]) != NULL;
    if (!interactive && find_text(page->body, "type=\"password\""))
        interactive = find_text(text, "log in") || find_text(text, "sign in");
    free(text);
    return interactive;
}

/* Share only the public transport response. Each row still interprets it
 * using its own filename, package format and compression policy. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    struct {
        char url[2048];
        bool range, loading, ok;
        time_t expires;
        Response response;
    } entries[8];
} metadata_gets = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

static bool metadata_get(const char *url, Response *response) {
    if (response->archive_session) return fetch(url, response);
    const size_t count = sizeof metadata_gets.entries / sizeof *metadata_gets.entries;
    if (strlen(url) >= sizeof metadata_gets.entries[0].url) return false;
    pthread_mutex_lock(&metadata_gets.mutex);
    size_t slot;
    for (;;) {
        size_t oldest = count, match = count;
        time_t now = time(NULL);
        for (size_t i = 0; i < count; i++) {
            if (metadata_gets.entries[i].range == response->range &&
                !strcmp(metadata_gets.entries[i].url, url) &&
                (metadata_gets.entries[i].loading || metadata_gets.entries[i].expires > now)) {
                match = i;
                break;
            }
            if (!metadata_gets.entries[i].loading &&
                (oldest == count || metadata_gets.entries[i].expires < metadata_gets.entries[oldest].expires))
                oldest = i;
        }
        if (match == count) { slot = oldest; break; }
        if (metadata_gets.entries[match].loading) {
            pthread_cond_wait(&metadata_gets.changed, &metadata_gets.mutex);
            continue;
        }
        *response = metadata_gets.entries[match].response;
        response->body = response->body ? strdup(response->body) : NULL;
        bool ok = metadata_gets.entries[match].ok;
        pthread_mutex_unlock(&metadata_gets.mutex);
        return ok;
    }
    if (slot < count) {
        free(metadata_gets.entries[slot].response.body);
        metadata_gets.entries[slot].response.body = NULL;
        copy_text(metadata_gets.entries[slot].url, sizeof metadata_gets.entries[slot].url, url);
        metadata_gets.entries[slot].range = response->range;
        metadata_gets.entries[slot].loading = true;
    }
    pthread_mutex_unlock(&metadata_gets.mutex);
    bool ok = fetch(url, response);
    if (slot < count) {
        pthread_mutex_lock(&metadata_gets.mutex);
        metadata_gets.entries[slot].response = *response;
        metadata_gets.entries[slot].response.body = response->body ? strdup(response->body) : NULL;
        metadata_gets.entries[slot].ok = ok;
        metadata_gets.entries[slot].expires = time(NULL) + 30;
        metadata_gets.entries[slot].loading = false;
        pthread_cond_broadcast(&metadata_gets.changed);
        pthread_mutex_unlock(&metadata_gets.mutex);
    }
    return ok;
}

bool remote_download_metadata(const Release *r, RemoteFileMetadata *metadata, cJSON **links) {
    memset(metadata, 0, sizeof *metadata);
    if (!remote_metadata_option(r)) return false;
    bool direct = !remote_page_pending(r);
    const char *url = provider_torbox_url(r);
    bool archive = remote_archive_url(url);
    Response page = {.collect_page = true, .metadata_only = !direct, .range = direct};
    bool ok = archive ? fetch_with_archive_login(url, &page, &metadata->archive_requires_login)
                      : metadata_get(url, &page);
    bool interactive = response_interactive(&page);
    metadata->http_status = page.status;
    metadata->attempted = true;
    copy_text(metadata->effective_host, sizeof metadata->effective_host, page.effective_host);
    if (page.redirect_rejected || page.code == CURLE_TOO_MANY_REDIRECTS ||
        (page.status >= 300 && page.status < 400 && page.code != CURLE_OK))
        metadata->status = REMOTE_METADATA_REDIRECT;
    else if (response_challenge(&page)) metadata->status = REMOTE_METADATA_CHALLENGE;
    else if (archive_login_required(&page)) metadata->status = REMOTE_METADATA_ARCHIVE_LOGIN;
    else if (response_missing(&page)) metadata->status = REMOTE_METADATA_MISSING;
    else if (interactive && !ok) metadata->status = REMOTE_METADATA_INTERACTIVE;
    else if (page.code == CURLE_OPERATION_TIMEDOUT) metadata->status = REMOTE_METADATA_TIMEOUT;
    else if (page.error_page || (page.status && !ok && page.code == CURLE_OK))
        metadata->status = REMOTE_METADATA_HTTP;
    else if (!ok) metadata->status = REMOTE_METADATA_NETWORK;
    else if (page.truncated && page.page) metadata->status = REMOTE_METADATA_TOO_LARGE;
    else if (!page.page) {
        Release file = *r;
        if (!file_from_response(&file, &page, url, NULL)) {
            file_metadata(&file, metadata);
            metadata->size = file.size;
            metadata->exact = true;
            metadata->confidence = 4;
            metadata->status = REMOTE_METADATA_FOUND;
        } else if (direct && page.size <= 0 && download_file_magic(r, &page.sample, page.used)) {
            metadata->status = REMOTE_METADATA_NO_SIZE;
        } else metadata->status = REMOTE_METADATA_UNSUPPORTED;
    } else if (direct) metadata->status = interactive ? REMOTE_METADATA_INTERACTIVE : REMOTE_METADATA_NO_SIZE;
    else if (page.body) {
        download_page_metadata(&page, r, metadata);
        if (interactive && !metadata->filename[0] && metadata->status == REMOTE_METADATA_NO_SIZE)
            metadata->status = REMOTE_METADATA_INTERACTIVE;
    } else metadata->status = REMOTE_METADATA_NO_SIZE;
    if (!ok && direct && archive && !metadata->archive_requires_login && !archive_account_signed_in() &&
        (metadata->status == REMOTE_METADATA_NETWORK || metadata->status == REMOTE_METADATA_TIMEOUT)) {
        /* The canonical item record can identify a restricted file even when
         * the storage server does not return a usable response. */
        RemoteFileMetadata file = {0};
        if (archive_file(url, r, NULL, NULL, 0, &file) == ARCHIVE_FILE_LOGIN_REQUIRED) {
            metadata->status = REMOTE_METADATA_ARCHIVE_LOGIN;
            metadata->archive_requires_login = true;
        }
    }
    if (ok && page.page && rootz_page(page.url)) rootz_metadata(&page, r, metadata);
    if (ok && (metadata->status == REMOTE_METADATA_FOUND || metadata->status == REMOTE_METADATA_NO_SIZE ||
               metadata->status == REMOTE_METADATA_AMBIGUOUS))
        archive_metadata(&page, r, metadata);
    const bool pspkg = !strcmp(remote_browser_provider(r->browser_url), "pspkg");
    if (pspkg && page.body && !page.truncated) {
        cJSON *fields = pspkg_field_labels(page.body);
        const cJSON *edition;
        size_t count = 0;
        cJSON_ArrayForEach(edition, fields) {
            const char *label = edition->valuestring;
            if (!label || count == sizeof metadata->pspkg_title_ids / sizeof *metadata->pspkg_title_ids) break;
            const char *p = pspkg_title_id(label);
            if (p)
                snprintf(metadata->pspkg_title_ids[count++], 10, "%.9s", p);
        }
        cJSON_Delete(fields);
    }
    if (pspkg && links && page.body && !page.truncated && !response_challenge(&page) &&
        (ok || metadata->status == REMOTE_METADATA_INTERACTIVE)) {
        *links = remote_pspkg_links(page.body, page.url);
        cJSON *row;
        cJSON_ArrayForEach(row, *links) {
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "metadataEligible"))) {
                cJSON_AddBoolToObject(row, "getAttempted", false);
                continue;
            }
            Release provider = *r;
            const char *target = json_text(row, "url");
            provider.remote_page = *remote_browser_provider(target) != 0;
            provider.remote_provider_page[0] = 0;
            provider.url[0] = provider.browser_url[0] = 0;
            if (provider.remote_page) copy_text(provider.browser_url, sizeof provider.browser_url, target);
            else copy_text(provider.url, sizeof provider.url, target);
            RemoteFileMetadata downstream;
            remote_download_metadata(&provider, &downstream, NULL);
            if (downstream.filename[0]) {
                char version[sizeof ((PspkgLink *)0)->version];
                pspkg_version(json_text(row, "edition"), downstream.filename, version, sizeof version);
                cJSON_DeleteItemFromObjectCaseSensitive(row, "version");
                if (version[0]) cJSON_AddStringToObject(row, "version", version);
            }
            cJSON_AddBoolToObject(row, "getAttempted", downstream.attempted);
            cJSON_AddStringToObject(row, "status", remote_metadata_status(downstream.status));
            cJSON_AddNumberToObject(row, "httpStatus", downstream.http_status);
            cJSON_AddStringToObject(row, "effectiveHost", downstream.effective_host);
            cJSON_AddNumberToObject(row, "sizeBytes", (double)downstream.size);
            cJSON_AddBoolToObject(row, "exact", downstream.exact);
        }
    }
    free(page.body);
    return true;

}

static int archive_file(const char *page, const Release *r, atomic_bool *cancel, char *url, size_t cap,
                        RemoteFileMetadata *file_info) {
    const char details[] = "https://archive.org/details/", download[] = "https://archive.org/download/";
    bool single = !strncmp(page, download, sizeof download - 1);
    char target[sizeof r->filename] = "";
    if (single && !remote_url_filename(page, target, sizeof target)) return -1;
    if (!single && strncmp(page, details, sizeof details - 1)) return -1;
    const char *id = page + (single ? sizeof download : sizeof details) - 1;
    size_t n = strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.");
    if (!n || (single ? id[n] != '/' : (id[n] && id[n] != '/' && id[n] != '?'))) return -1;
    char path[2048];
    if (snprintf(path, sizeof path, "https://archive.org/metadata/%.*s", (int)n, id) >= (int)sizeof path)
        return -1;
    Response metadata = {.cancel = cancel, .collect_page = true, .metadata_only = true};
    int result = -1;
    bool ok = fetch(path, &metadata);
    if (response_missing(&metadata)) result = REMOTE_DOWNLOAD_MISSING;
    else if (ok && metadata.body && !metadata.truncated) {
        cJSON *root = cJSON_Parse(metadata.body);
        bool signed_in = archive_account_signed_in(), needs_login = false;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "is_dark"))) result = REMOTE_DOWNLOAD_MISSING;
        const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files"), *entry;
        cJSON_ArrayForEach(entry, (result == REMOTE_DOWNLOAD_MISSING ? NULL : files)) {
            const char *name = json_text(entry, "name");
            Release file = *r;
            const cJSON *private_file = cJSON_GetObjectItemCaseSensitive(entry, "private");
            bool restricted = cJSON_IsTrue(private_file) || !strcmp(json_text(entry, "private"), "true");
            if ((single && strcmp(name, target)) || strcmp(json_text(entry, "source"), "original") ||
                remote_file_format(&file, name)) continue;
            if (restricted && !signed_in) { needs_login = true; continue; }
            if (result == 0) { result = -3; break; }
            if (file_info) {
                file_metadata(&file, file_info);
                file_info->archive_requires_login = restricted;
                const cJSON *size = cJSON_GetObjectItemCaseSensitive(entry, "size");
                if (cJSON_IsString(size)) size_value(size->valuestring, true, &file_info->size, &file_info->exact);
                file_info->confidence = file_info->size ? 4 : 0;
                file_info->status = file_info->size ? REMOTE_METADATA_FOUND : REMOTE_METADATA_NO_SIZE;
                result = 0;
                continue;
            }
            char *encoded = curl_easy_escape(NULL, name, 0);
            int length = encoded ? snprintf(url, cap, "https://archive.org/download/%.*s/%s", (int)n, id, encoded) : -1;
            if (length > 0 && (size_t)length < cap) result = 0;
            curl_free(encoded);
        }
        if (result == -1 && needs_login) result = ARCHIVE_FILE_LOGIN_REQUIRED;
        cJSON_Delete(root);
    }
    free(metadata.body);
    return result;
}

static void archive_metadata(const Response *page, const Release *r, RemoteFileMetadata *metadata) {
    static const char prefix[] = "https://archive.org/details/";
    if (strncmp(page->url, prefix, sizeof prefix - 1)) return;
    RemoteFileMetadata file = {0};
    int result = archive_file(page->url, r, page->cancel, NULL, 0, &file);
    if (!result) {
        file.attempted = metadata->attempted;
        file.archive_requires_login |= metadata->archive_requires_login;
        file.http_status = metadata->http_status;
        copy_text(file.effective_host, sizeof file.effective_host, metadata->effective_host);
        *metadata = file;
    }
    if (result == REMOTE_DOWNLOAD_MISSING) metadata->status = REMOTE_METADATA_MISSING;
    if (result == ARCHIVE_FILE_LOGIN_REQUIRED) {
        metadata->status = REMOTE_METADATA_ARCHIVE_LOGIN;
        metadata->archive_requires_login = true;
    }
}

/* Returns 0 for a verified direct file, 1 for an interactive provider page,
 * REMOTE_DOWNLOAD_MISSING for a removed file, and -1 for other failures. */
int remote_download_resolve(Release *r, Release *locator, atomic_bool *cancel, char *error, size_t cap) {
    Response page = {.cancel = cancel, .collect_page = true};
    int result = -1;
    const char *selected_page = provider_torbox_url(r);
    page.archive_session = remote_archive_url(selected_page);
    bool ok = fetch(selected_page, &page);
    if (archive_login_required(&page)) { archive_account_refusal(selected_page, error, cap); goto done; }
    if (response_missing(&page)) { result = REMOTE_DOWNLOAD_MISSING; goto done; }
    if (!ok || response_challenge(&page) || response_interactive(&page)) {
        if (response_challenge(&page) || response_interactive(&page)) {
            const char *provider = remote_browser_provider(selected_page);
            if (provider_browser_source(provider) && strcmp(provider, "pspkg")) {
                *locator = *r;
                copy_text(locator->source, sizeof locator->source, provider);
                copy_text(locator->browser_url, sizeof locator->browser_url, selected_page);
                copy_text(locator->url, sizeof locator->url, selected_page);
                if (provider_browser_supported(locator)) result = 1;
            }
            if (result < 0) copy_text(error, cap, "Open the provider page and complete its verification.");
        }
        goto done;
    }
    if (!page.page) {
        result = file_from_response(r, &page, selected_page, NULL) ? -1 : 0;
        goto done;
    }
    if (!page.body || !page.used) goto done;
    if (rootz_page(page.url)) {
        RemoteFileMetadata file = {0};
        rootz_metadata(&page, r, &file);
        if (file.status == REMOTE_METADATA_MISSING) { result = REMOTE_DOWNLOAD_MISSING; goto done; }
    }
    const char *provider = remote_browser_provider(page.url);
    if (!provider_browser_source(provider)) provider = "";
    if (*provider) {
        *locator = *r;
        char name[sizeof r->filename];
        if (page_filename(&page, r, name, sizeof name, NULL)) {
            if (remote_provider_file_format(locator, name, NULL) > 0) goto done;
            copy_text(locator->filename, sizeof locator->filename, name);
        }
        copy_text(locator->source, sizeof locator->source, provider);
        /* The private matcher still enforces the provider's selected file route. */
        if (strlen(page.url) >= sizeof locator->browser_url) goto done;
        copy_text(locator->browser_url, sizeof locator->browser_url, page.url);
        copy_text(locator->url, sizeof locator->url, page.url);
        if (!provider_browser_supported(locator)) goto done;
    }
    if (!strncmp(page.url, "https://archive.org/details/", 28)) {
        char direct[sizeof r->url] = "";
        int link = archive_file(page.url, r, page.cancel, direct, sizeof direct, NULL);
        if (link == ARCHIVE_FILE_LOGIN_REQUIRED) { archive_account_refusal(selected_page, error, cap); goto done; }
        if (!link) {
            int verified = remote_download_verify(r, direct, cancel, error, cap);
            if (!verified || verified == REMOTE_DOWNLOAD_MISSING) result = verified;
        }
        if (link == REMOTE_DOWNLOAD_MISSING) result = link;
        if (link == -3) copy_text(error, cap, "This option lists several files. Choose a mirror that identifies one download.");
        goto done;
    }
    PageLink link = {0};
    int found = page.truncated ? -1 : page_link(&page, r, *provider ? locator : NULL, &link);
    if (!found) {
        Release file = *r;
        Response response = {.cancel = cancel, .collect_page = true, .range = true};
        char heading[sizeof file.filename];
        const char *name = NULL;
        if (link.pinned) {
            name = link.filename;
            copy_text(file.filename, sizeof file.filename, name);
        } else if (!*provider && !link.recognized && page_filename(&page, r, heading, sizeof heading, NULL)) {
            name = heading;
        }
        bool received = fetch(link.url, &response);
        bool missing = response_missing(&response);
        free(response.body);
        if (missing) { result = REMOTE_DOWNLOAD_MISSING; goto done; }
        if (received && !file_from_response(&file, &response, link.url, name)) {
            *r = file;
            result = 0;
            goto done;
        }
    }
    if (*provider) result = 1;
    else if (found == -2)
        copy_text(error, cap, "This option lists several files. Choose a mirror that identifies one download.");
done:
    free(page.body);
    if (result == REMOTE_DOWNLOAD_MISSING) copy_text(error, cap, REMOTE_DOWNLOAD_REMOVED);
    else if (result < 0 && !error[0])
        copy_text(error, cap, "The provider file could not be verified.");
    return result;
}
