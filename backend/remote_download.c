#include "remote_sources.h"
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
#define SAMPLE_BYTES 8

typedef struct {
    char *body;
    size_t used;
    bool page, collect_page, truncated, direct, challenge, json, error_page, redirect_rejected;
    bool metadata_only, redirect_status, plain_text;
    const char *initial_url;
    CURLcode code;
    long status;
    char effective_host[254];
    unsigned char sample[SAMPLE_BYTES];
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

static bool metadata_origin(const char *initial, const char *next) {
    if (!strcmp(initial, next)) return true;
    const char *provider = remote_browser_provider(initial);
    if (*provider && !strcmp(provider, remote_browser_provider(next))) {
        if (strcmp(provider, "pspkg")) return true;
        size_t length = strcspn(initial, "?");
        return length == strcspn(next, "?") && !memcmp(initial, next, length);
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

static size_t headers(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    size_t n = a * b;
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
        r->used = 0;
        r->page = r->truncated = r->challenge = r->json = false;
        r->error_page = r->redirect_status = r->plain_text = false;
        if (r->body) r->body[0] = 0;
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
            !metadata_origin(r->initial_url, resolved)) {
            r->redirect_rejected = true;
            return 0;
        }
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
                  (length >= 16 && !strncasecmp(data + offset, "application/json", 16));
    }
    return n;
}

static size_t body(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    size_t n = a * b;
    if (cancelled(r)) return 0;
    bool collect = r->collect_page && (r->page || r->error_page || r->plain_text);
    if (!collect && r->collect_page && !r->used) {
        size_t offset = 0;
        while (offset < n && isspace((unsigned char)data[offset])) offset++;
        collect = r->page = offset < n && data[offset] == '<';
    }
    size_t limit = collect ? PAGE_LIMIT : SAMPLE_BYTES;
    if (collect && !r->body) {
        r->body = malloc(PAGE_LIMIT + 1);
        if (!r->body) return 0;
    }
    size_t take = n <= limit - r->used ? n : limit - r->used;
    memcpy((collect ? (unsigned char *)r->body : r->sample) + r->used, data, take);
    if (collect && r->used < SAMPLE_BYTES) {
        size_t remaining = SAMPLE_BYTES - r->used;
        memcpy(r->sample + r->used, data, take < remaining ? take : remaining);
    }
    r->used += take;
    if (collect) r->body[r->used] = 0;
    r->truncated = take != n;
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
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, provider_open_socket);
    curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, before_request);
    curl_easy_setopt(curl, CURLOPT_PREREQDATA, r);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
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
    if (r->collect_page) curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (!r->collect_page) curl_easy_setopt(curl, CURLOPT_RANGE, "0-7");
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
                end >= SAMPLE_BYTES - 1 && total > end) r->size = total;
        } else if (status == 200) {
            curl_off_t size = -1;
            if (!curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &size)) r->size = size;
        }
    }
    curl_easy_cleanup(curl);
    r->handle = NULL;
    return ok;
}

static const char *sample_suffix(const unsigned char *p, size_t length) {
    if (length < SAMPLE_BYTES) return NULL;
    if (!memcmp(p, "PK\3\4", 4)) return ".zip";
    if (!memcmp(p, "7z\xbc\xaf\x27\x1c", 6)) return ".7z";
    if (!memcmp(p, "Rar!\x1a\x07", 6) && (p[6] == 0 || (p[6] == 1 && p[7] == 0))) return ".rar";
    if (!memcmp(p, "\x7f" "CNT", 4) || !memcmp(p, "\x7f" "FIH", 4)) return ".pkg";
    if (!memcmp(p, "PFSC", 4)) return ".ffpfsc";
    if (!memcmp(p + 3, "EXFAT", 5)) return ".exfat";
    return NULL;
}

bool download_file_magic(const Release *r, const unsigned char *p, size_t length) {
    const char *suffix = sample_suffix(p, length);
    const char *expected = r->compression[0] ? r->compression : "pkg";
    if (!r->compression[0]) {
        if (!strcmp(r->format, "exFAT")) expected = "exfat";
        else if (!strcmp(r->format, "FFPFSC")) expected = "ffpfsc";
    }
    return suffix && !strcmp(suffix + 1, expected);
}

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
    if (r->filename[0] && strcmp(r->filename, actual)) return -1;
    Release file = *r;
    const char *suffix = sample_suffix(response->sample, response->used);
    int format = response->direct ? direct_file_format(&file, actual) :
        remote_file_detected_format(&file, actual, r->remote_page ? suffix : NULL);
    if (format) return format;
    if (r->compression[0] && strcmp(r->compression, file.compression)) return 1;
    if (response->page || response->size < SAMPLE_BYTES || response->size > 9007199254740991LL ||
        (r->size && r->size != response->size) ||
        !download_file_magic(&file, response->sample, response->used)) return -1;
    copy_text(file.url, sizeof file.url, url);
    file.size = response->size;
    *r = file;
    return 0;
}

static bool response_missing(const Response *response);

int remote_download_verify(Release *r, const char *url, atomic_bool *cancel, char *error, size_t cap) {
    Response response = {.cancel = cancel, .direct = !strcmp(r->source, ORBIT_DIRECT_SOURCE)};
    bool ok = fetch(url, &response);
    int code = !response.direct && response_missing(&response) ? REMOTE_DOWNLOAD_MISSING :
               ok ? file_from_response(r, &response, url, NULL) : -1;
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

static bool missing_text(const char *text) {
    const char *messages[] = {"file not found", "file missing", "file is missing",
        "file removed", "file was removed", "file has been removed",
        "file deleted", "file was deleted", "file has been deleted", "file does not exist",
        "file doesn't exist", "file is no longer available", "status: deleted", "no such key"};
    /* A standalone status is common, but 'deleted' in a filename or a
     * retention policy is not evidence that this file has been removed. */
    for (const char *p = text; *p; ) {
        size_t length = strcspn(p, "\n");
        if (length >= 7 && !strncasecmp(p, "deleted", 7) &&
            (length == 7 || (length == 8 && strchr(".!", p[7])))) return true;
        p += length;
        if (*p) p++;
    }
    for (const char *p = text; *p; p++) {
        if (p != text && isalnum((unsigned char)p[-1])) continue;
        for (size_t i = 0; i < sizeof messages / sizeof *messages; i++) {
            const char *word = messages[i], *end = p;
            while (*word && *end && (*word == ' ' ? isspace((unsigned char)*end) :
                   *word == tolower((unsigned char)*end))) {
                word++;
                end++;
            }
            if (!*word && !isalnum((unsigned char)*end) && (*end != '.' || !isalnum((unsigned char)end[1])))
                return true;
        }
    }
    return false;
}

static bool page_challenge(const char *body) {
    return body && (find_text(body, "/cdn-cgi/challenge-platform/") ||
           find_text(body, "_cf_chl_opt") || find_text(body, "<title>just a moment") ||
           find_text(body, "<title>attention required"));
}

static bool response_challenge(const Response *response) {
    return response->challenge || page_challenge(response->body);
}

static bool page_has_download(const char *html);
static void page_size(const char *html, const char *anchor, const char *name, RemoteFileMetadata *file);

static bool page_message_missing(const char *body, size_t length, bool json_body) {
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
        missing = missing_text(text);
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
    page_size(body, NULL, "", &file);
    return !file.confidence;
}

static bool response_missing(const Response *response) {
    if (cancelled(response) || response_challenge(response)) return false;
    if (response->status == 404 || response->status == 410) return true;
    return response->status == 200 && response->body && response->used && !response->truncated &&
           response->code == CURLE_OK && page_message_missing(response->body, response->used, response->json);
}

bool remote_download_html_missing(long status, const char *html, bool challenge) {
    if (challenge || page_challenge(html)) return false;
    if (status == 404 || status == 410) return true;
    return status == 200 && html && *html && page_message_missing(html, strlen(html), false);
}

bool remote_download_confirm_missing(const char *url, atomic_bool *cancel) {
    Response page = {.cancel = cancel, .collect_page = true, .metadata_only = true};
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
        int format = remote_file_format(file, name);
        if (format >= 0) return format == 0;
    }
    if (!strncasecmp(name, "Download ", 9)) {
        memmove(name, name + 9, strlen(name + 9) + 1);
        if (*name == '"') memmove(name, name + 1, strlen(name));
    }
    if (safe_page_name(name) && !remote_file_format(file, name)) return true;
    const char *separators[] = {" - ", " | ", " · ", " — ", "\""};
    for (size_t i = 0; i < sizeof separators / sizeof *separators; i++) {
        for (char *suffix = name; (suffix = strstr(suffix, separators[i])); suffix++) {
            char saved = *suffix;
            *suffix = 0;
            if (safe_page_name(name) && !remote_file_format(file, name)) return true;
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

static bool page_filename(const char *html, const Release *r, char *name, size_t cap, const char **anchor) {
    Release file = *r;
    char heading[sizeof file.filename] = "", title[512] = "";
    const char *heading_tag = NULL;
    for (const char *tag = html; (tag = strchr(tag, '<')); ) {
        const char *next = skip_markup(tag);
        if (next != tag) { tag = next; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        char label[128], value[512];
        bool named = (tag_is(tag + 1, "a") && tag_value(tag, end, "download", name, cap)) ||
                     tag_value(tag, end, "data-filename", name, cap);
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
        "bandwidth", "transfer limit", "daily limit", "per day", "per second", "advertisement", "traffic"};
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

static void page_size(const char *html, const char *anchor, const char *name, RemoteFileMetadata *file) {
    SizeChoice best = {0};
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
        if (unrelated_size(label) || unrelated_size(id)) {
            const char *close = element_end(current, end);
            if (close) {
                const char *after = strchr(close, '>');
                if (after) tag = after + 1;
            }
            continue;
        }
        bool specific = size_label(label) || size_label(id);
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
                            tag_is(current + 1, "dd") || tag_is(current + 1, "td") ||
                            tag_is(current + 1, "li") || tag_is(current + 1, "b") ||
                            tag_is(current + 1, "strong") || tag_is(current + 1, "div") ||
                            tag_is(current + 1, "a") || tag_is(current + 1, "button");
        if ((specific || bound || text_element) && element_text(current, end, value, sizeof value)) {
            const char *file_size = find_text(value, "file size");
            bool named = *name && find_text(value, name);
            if (file_size) offer_size(&best, file, file_size, 2, false, false);
            offer_size(&best, file, value, bound || named || nearby ? 3 : specific ? 2 : 1, false, false);
        }
        /* Text directly under body/main or split by formatting also serves
         * providers that don't put their size in a named element. */
        const char *next = strchr(tag, '<');
        if ((!next || next - tag < (ptrdiff_t)sizeof value) &&
            html_text(tag, next ? (size_t)(next - tag) : strlen(tag), value, sizeof value))
            offer_size(&best, file, value, specific ? 2 : 1, false, false);
    }
    const char *json_fields[] = {"sizeBytes", "fileSize", "contentSize"};
    for (size_t i = 0; i < sizeof json_fields / sizeof *json_fields; i++) {
        char key[32];
        snprintf(key, sizeof key, "\"%s\"", json_fields[i]);
        const char *field = strstr(html, key);
        if (!field || strstr(field + strlen(key), key)) continue;
        field += strlen(key);
        while (isspace((unsigned char)*field)) field++;
        if (*field++ != ':') continue;
        while (isspace((unsigned char)*field)) field++;
        bool quoted = *field == '"';
        if (quoted) field++;
        char value[64];
        size_t length = strcspn(field, quoted ? "\"" : ",}\r\n");
        if (!quoted && field[length] == ',' && isdigit((unsigned char)field[length + 1])) continue;
        if (html_text(field, length, value, sizeof value)) offer_size(&best, file, value, 2, true, true);
    }
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
                int format = remote_file_format(&match, candidate.filename);
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
            int format = remote_file_format(&file, candidate.filename);
            if (format > 0) continue;
            candidate.recognized = !format;
            if (!download && !candidate.recognized) {
                char label[16];
                const char *close = strchr(end + 1, '<');
                if (!close || !html_text(end + 1, (size_t)(close - end - 1), label, sizeof label) ||
                    strcasecmp(label, "Download")) continue;
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

/* Keep only stable, independently validated public provider pages. */
cJSON *remote_pspkg_links(const char *html) {
    cJSON *links = cJSON_CreateArray();
    if (!links) return NULL;
    char edition[256] = "";
    bool base = true;
    for (const char *tag = html; (tag = strchr(tag, '<')) && cJSON_GetArraySize(links) < PSPKG_LINK_LIMIT;) {
        const char *next = skip_markup(tag);
        if (next != tag) { tag = next; continue; }
        const char *end = strchr(tag, '>');
        if (!end) break;
        const char *current = tag;
        tag = end + 1;
        if (tag_is(current + 1, "h2") || tag_is(current + 1, "h3") || tag_is(current + 1, "h4")) {
            element_text(current, end, edition, sizeof edition);
            base = !phrase(edition, "update") && !phrase(edition, "patch") &&
                   !phrase(edition, "backport") && !phrase(edition, "dlc");
            continue;
        }
        if (!tag_is(current + 1, "a")) continue;
        char href[2048], label[128] = "";
        if (!tag_value(current, end, "href", href, sizeof href)) continue;
        const char *provider = remote_browser_provider(href);
        if (!*provider || !strcmp(provider, "pspkg")) continue;
        element_text(current, end, label, sizeof label);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "url", href);
        cJSON_AddStringToObject(item, "name", provider);
        cJSON_AddStringToObject(item, "label", label);
        if (*edition) cJSON_AddStringToObject(item, "edition", edition);
        bool unsupported = phrase(edition, "zstd") || phrase(edition, "lz4") ||
                           phrase(label, "zstd") || phrase(label, "lz4") || phrase(edition, "pfs");
        bool multipart = phrase(edition, "part") || phrase(edition, "multipart") ||
                         phrase(label, "part") || phrase(label, "volume");
        const char *formats[] = {"FPKG", "PKG", "PFS", "exFAT", "FFPFSC"};
        for (size_t i = 0; i < sizeof formats / sizeof *formats; i++)
            if (phrase(edition, formats[i])) {
                cJSON_AddStringToObject(item, "format", formats[i]);
                break;
            }
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
        for (const char *at = edition; *at; at++) {
            if ((*at != 'v' && *at != 'V') || !isdigit((unsigned char)at[1]) ||
                (at > edition && isalnum((unsigned char)at[-1]))) continue;
            size_t n = strspn(at + 1, "0123456789.");
            char version[24];
            if (n < sizeof version && n > 1) {
                memcpy(version, at + 1, n);
                version[n] = 0;
                cJSON_AddStringToObject(item, "version", version);
            }
            break;
        }
        const char *part = find_text(label, "part");
        if (!part) part = find_text(edition, "part");
        if (part && multipart) {
            part += 4;
            part += strspn(part, " #._-");
            if (isdigit((unsigned char)*part)) cJSON_AddNumberToObject(item, "part", strtol(part, NULL, 10));
        }
        if (unsupported) cJSON_AddStringToObject(item, "filterReason", "unsupported-format");
        else if (multipart) cJSON_AddStringToObject(item, "filterReason", "multipart");
        else if (!base) cJSON_AddStringToObject(item, "filterReason", "unsupported-category");
        else cJSON_AddStringToObject(item, "filterReason", "");
        cJSON_AddBoolToObject(item, "metadataEligible", base && !unsupported && !multipart);
        SizeChoice size = {0};
        RemoteFileMetadata candidates = {0};
        offer_size(&size, &candidates, edition, 2, false, false);
        offer_size(&size, &candidates, label, 2, false, false);
        if (size.size && !size.ambiguous) cJSON_AddNumberToObject(item, "sizeEstimateBytes", (double)size.size);
        for (const char *at = edition; *at; at++) {
            if (strncasecmp(at, "CUSA", 4) && strncasecmp(at, "PPSA", 4)) continue;
            if (strspn(at + 4, "0123456789") != 5) continue;
            char id[10];
            memcpy(id, at, 9);
            id[9] = 0;
            cJSON_AddStringToObject(item, "titleId", id);
            break;
        }
        cJSON_AddItemToArray(links, item);
    }
    return links;
}

static void archive_metadata(const Release *release, RemoteFileMetadata *file) {
    static const char prefix[] = "https://archive.org/details/";
    if (strncmp(release->browser_url, prefix, sizeof prefix - 1)) return;
    char url[256];
    int n = snprintf(url, sizeof url, "https://archive.org/metadata/%s", release->browser_url + sizeof prefix - 1);
    if (n < 0 || (size_t)n >= sizeof url) return;
    Response api = {.collect_page = true, .metadata_only = true};
    cJSON *json = fetch(url, &api) && !api.truncated && api.body ? cJSON_Parse(api.body) : NULL;
    const cJSON *item, *files = cJSON_GetObjectItemCaseSensitive(json, "files");
    Release selected = {0};
    int64_t bytes = 0;
    cJSON_ArrayForEach(item, files) {
        const char *name = json_text(item, "name");
        const cJSON *private_file = cJSON_GetObjectItemCaseSensitive(item, "private");
        Release candidate = *release;
        if (strcmp(json_text(item, "source"), "original") || cJSON_IsTrue(private_file) ||
            !strcmp(json_text(item, "private"), "true") || remote_file_format(&candidate, name) ||
            (release->remote_filename[0] && !release->remote_filename_synthetic && strcmp(release->remote_filename, name))) continue;
        if (selected.filename[0]) { bytes = 0; break; }
        selected = candidate;
        const char *value = json_text(item, "size");
        bool exact = false;
        if (!size_value(value, true, &bytes, &exact) || !exact) bytes = 0;
    }
    if (bytes > 0) {
        file_metadata(&selected, file);
        file->size = bytes;
        file->exact = true;
        file->confidence = 4;
        file->status = REMOTE_METADATA_FOUND;
        if (file->candidate_count < sizeof file->candidates / sizeof *file->candidates)
            file->candidates[file->candidate_count++] = bytes;
    }
    cJSON_Delete(json);
    free(api.body);
}

const char *remote_metadata_status(RemoteMetadataStatus status) {
    static const char *const states[] = {
        "not-attempted", "found", "missing", "interactive-verification", "challenge", "timeout",
        "network-tls-failure", "http-error", "redirected-to-invalid-host", "page-too-large",
        "parse-no-size", "ambiguous-size", "unsupported-format",
    };
    return states[status];
}

static bool response_interactive(const Response *page) {
    if (page->status == 401) return true;
    if (!page->body) return false;
    const char *phrases[] = {"Awaiting verification", "verify you are human", "complete the captcha",
        "verification required", "login required", "sign in to download", "log in to download",
        "countdown", "please wait before downloading"};
    for (size_t i = 0; i < sizeof phrases / sizeof *phrases; i++)
        if (find_text(page->body, phrases[i])) return true;
    const char *password = find_text(page->body, "type=\"password\"");
    return password && (find_text(page->body, "log in") || find_text(page->body, "sign in"));
}

bool remote_download_metadata(const Release *r, RemoteFileMetadata *metadata, cJSON **links) {
    memset(metadata, 0, sizeof *metadata);
    if (!remote_metadata_option(r)) return false;
    Response page = {.collect_page = true, .metadata_only = true};
    bool ok = fetch(r->browser_url, &page);
    metadata->http_status = page.status;
    metadata->attempted = true;
    copy_text(metadata->effective_host, sizeof metadata->effective_host, page.effective_host);
    if (page.redirect_rejected || page.code == CURLE_TOO_MANY_REDIRECTS ||
        (page.status >= 300 && page.status < 400 && page.code != CURLE_OK))
        metadata->status = REMOTE_METADATA_REDIRECT;
    else if (response_challenge(&page)) metadata->status = REMOTE_METADATA_CHALLENGE;
    else if (response_missing(&page)) metadata->status = REMOTE_METADATA_MISSING;
    else if (response_interactive(&page)) metadata->status = REMOTE_METADATA_INTERACTIVE;
    else if (page.code == CURLE_OPERATION_TIMEDOUT) metadata->status = REMOTE_METADATA_TIMEOUT;
    else if (page.error_page || (page.status && !ok && page.code == CURLE_OK))
        metadata->status = REMOTE_METADATA_HTTP;
    else if (!ok) metadata->status = REMOTE_METADATA_NETWORK;
    else if (page.truncated && page.page) metadata->status = REMOTE_METADATA_TOO_LARGE;
    else if (!page.page) {
        Release file = *r;
        if (!file_from_response(&file, &page, r->browser_url, NULL)) {
            file_metadata(&file, metadata);
            metadata->size = file.size;
            metadata->exact = true;
            metadata->confidence = 4;
            metadata->status = REMOTE_METADATA_FOUND;
        } else metadata->status = REMOTE_METADATA_UNSUPPORTED;
    } else if (page.body) {
        Release file = *r;
        char name[sizeof file.filename] = "";
        const char *anchor = NULL;
        bool named = page_filename(page.body, r, name, sizeof name, &anchor);
        if (!named) {
            PageLink link = {0};
            if (!page_link(&page, r, NULL, &link) && (link.pinned || link.recognized)) {
                copy_text(name, sizeof name, link.filename);
                named = true;
            }
        }
        page_size(page.body, anchor, name, metadata);
        if (named) {
            int format = remote_file_format(&file, name);
            if (!format) file_metadata(&file, metadata);
            else if (format > 0) {
                metadata->status = REMOTE_METADATA_UNSUPPORTED;
                metadata->size = 0;
                metadata->confidence = 0;
                metadata->exact = false;
            } else if (safe_page_name(name)) copy_text(metadata->filename, sizeof metadata->filename, name);
        }
    } else metadata->status = REMOTE_METADATA_NO_SIZE;
    if (ok && (metadata->status == REMOTE_METADATA_FOUND || metadata->status == REMOTE_METADATA_NO_SIZE ||
               metadata->status == REMOTE_METADATA_AMBIGUOUS))
        archive_metadata(r, metadata);
    const bool pspkg = !strcmp(remote_browser_provider(r->browser_url), "pspkg");
    if (pspkg && links && page.body && !page.truncated && !response_challenge(&page) &&
        (ok || metadata->status == REMOTE_METADATA_INTERACTIVE)) {
        *links = remote_pspkg_links(page.body);
        cJSON *row;
        cJSON_ArrayForEach(row, *links) {
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "metadataEligible"))) {
                cJSON_AddBoolToObject(row, "getAttempted", false);
                continue;
            }
            Release provider = *r;
            copy_text(provider.browser_url, sizeof provider.browser_url, json_text(row, "url"));
            RemoteFileMetadata downstream;
            remote_download_metadata(&provider, &downstream, NULL);
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

static int archive_link(const Response *page, Release *r, char *url, size_t cap) {
    const char prefix[] = "https://archive.org/details/";
    if (strncmp(page->url, prefix, sizeof prefix - 1)) return -1;
    const char *id = page->url + sizeof prefix - 1;
    size_t n = strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.");
    if (!n || (id[n] && id[n] != '/' && id[n] != '?')) return -1;
    char path[2048];
    if (snprintf(path, sizeof path, "https://archive.org/metadata/%.*s", (int)n, id) >= (int)sizeof path)
        return -1;
    Response metadata = {.cancel = page->cancel, .collect_page = true};
    int result = -1;
    if (fetch(path, &metadata) && metadata.body && !metadata.truncated) {
        cJSON *root = cJSON_Parse(metadata.body);
        const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files"), *entry;
        cJSON_ArrayForEach(entry, files) {
            const char *name = json_text(entry, "name");
            Release file = *r;
            const cJSON *private_file = cJSON_GetObjectItemCaseSensitive(entry, "private");
            if (strcmp(json_text(entry, "source"), "original") ||
                cJSON_IsTrue(private_file) || !strcmp(json_text(entry, "private"), "true") ||
                remote_file_format(&file, name)) continue;
            if (result == 0) { result = -2; break; }
            char *encoded = curl_easy_escape(NULL, name, 0);
            int length = encoded ? snprintf(url, cap, "https://archive.org/download/%.*s/%s", (int)n, id, encoded) : -1;
            if (length > 0 && (size_t)length < cap) result = 0;
            curl_free(encoded);
        }
        cJSON_Delete(root);
    }
    free(metadata.body);
    return result;
}

/* Returns 0 for a verified direct file, 1 for an interactive provider page,
 * REMOTE_DOWNLOAD_MISSING for a removed file, and -1 for other failures. */
int remote_download_resolve(Release *r, Release *locator, atomic_bool *cancel, char *error, size_t cap) {
    Response page = {.cancel = cancel, .collect_page = true};
    int result = -1;
    const char *selected_page = provider_torbox_url(r);
    bool ok = fetch(selected_page, &page);
    if (response_missing(&page)) {
        result = REMOTE_DOWNLOAD_MISSING;
        copy_text(error, cap, REMOTE_DOWNLOAD_REMOVED);
        goto done;
    }
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
    const char *provider = remote_browser_provider(page.url);
    if (!provider_browser_source(provider)) provider = "";
    if (*provider) {
        *locator = *r;
        char name[sizeof r->filename];
        if (page_filename(page.body, r, name, sizeof name, NULL)) {
            if (remote_file_format(locator, name) > 0) goto done;
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
        int link = archive_link(&page, r, direct, sizeof direct);
        if (!link && !remote_download_verify(r, direct, cancel, error, cap)) result = 0;
        if (link == -2) copy_text(error, cap, "This option lists several files. Choose a mirror that identifies one download.");
        goto done;
    }
    PageLink link = {0};
    int found = page.truncated ? -1 : page_link(&page, r, *provider ? locator : NULL, &link);
    if (!found) {
        Release file = *r;
        Response response = {.cancel = cancel};
        char heading[sizeof file.filename];
        const char *name = NULL;
        if (link.pinned) {
            name = link.filename;
            copy_text(file.filename, sizeof file.filename, name);
        } else if (!*provider && !link.recognized && page_filename(page.body, r, heading, sizeof heading, NULL)) {
            name = heading;
        }
        if (fetch(link.url, &response) && !file_from_response(&file, &response, link.url, name)) {
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
    if (result < 0 && !error[0])
        copy_text(error, cap, "This option did not identify a supported PKG, FPKG, RAR or ZIP download.");
    return result;
}
