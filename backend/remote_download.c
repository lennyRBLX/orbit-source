#include "remote_sources.h"
#include "provider_network.h"
#include "ca.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PAGE_LIMIT (1024U * 1024U)
#define SAMPLE_BYTES 8

typedef struct {
    char *body;
    size_t used;
    bool page, collect_page, truncated, direct;
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

static int before_request(void *context, char *address, char *local, int port, int local_port) {
    (void)address; (void)local; (void)port; (void)local_port;
    const Response *r = context;
    char *url = NULL;
    curl_easy_getinfo(r->handle, CURLINFO_EFFECTIVE_URL, &url);
    bool allowed = r->direct ? direct_http_url(url) : remote_https_url(url);
    return allowed ? CURL_PREREQFUNC_OK : CURL_PREREQFUNC_ABORT;
}

static size_t headers(char *data, size_t a, size_t b, void *context) {
    Response *r = context;
    size_t n = a * b;
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) {
        r->used = 0;
        r->page = r->truncated = false;
    } else if (n >= 13 && !strncasecmp(data, "Content-Type:", 13)) {
        size_t offset = 13;
        while (offset < n && isspace((unsigned char)data[offset])) offset++;
        size_t length = n - offset;
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
    bool collect = r->page && r->collect_page;
    size_t limit = collect ? PAGE_LIMIT : SAMPLE_BYTES;
    if (collect && !r->body) {
        r->body = malloc(PAGE_LIMIT + 1);
        if (!r->body) return 0;
    }
    size_t take = n <= limit - r->used ? n : limit - r->used;
    memcpy((collect ? (unsigned char *)r->body : r->sample) + r->used, data, take);
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
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    char *effective = NULL;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    bool ok = !cancelled(r) && (code == CURLE_OK || (code == CURLE_WRITE_ERROR && r->truncated)) &&
              (status == 200 || status == 206) && effective && strlen(effective) < sizeof r->url;
    if (ok) {
        copy_text(r->url, sizeof r->url, effective);
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

bool download_file_magic(const Release *r, const unsigned char *p, size_t length) {
    if (length < SAMPLE_BYTES) return false;
    if (!strcmp(r->compression, "zip"))
        return !memcmp(p, "PK\3\4", 4);
    if (!strcmp(r->compression, "rar"))
        return !memcmp(p, "Rar!\x1a\x07", 6) && (p[6] == 0 || (p[6] == 1 && p[7] == 0));
    return !memcmp(p, "\x7f" "CNT", 4) || !memcmp(p, "\x7f" "FIH", 4);
}

static int file_from_response(Release *r, const Response *response, const char *url) {
    char name[sizeof r->filename] = "";
    if (!response->name[0]) {
        remote_url_filename(response->url, name, sizeof name);
        if (r->filename[0] && strcmp(r->filename, name))
            remote_url_filename(url, name, sizeof name);
    }
    const char *actual = response->name[0] ? response->name : name;
    if (r->filename[0] && strcmp(r->filename, actual)) return -1;
    Release file = *r;
    int format = response->direct ? direct_file_format(&file, actual) : remote_file_format(&file, actual);
    if (format) return format;
    if (response->page || response->size < SAMPLE_BYTES || response->size > 9007199254740991LL ||
        !download_file_magic(&file, response->sample, response->used)) return -1;
    copy_text(file.url, sizeof file.url, url);
    file.size = response->size;
    *r = file;
    return 0;
}

int remote_download_verify(Release *r, const char *url, atomic_bool *cancel, char *error, size_t cap) {
    Response response = {.cancel = cancel, .direct = !strcmp(r->source, ORBIT_DIRECT_SOURCE)};
    int code = fetch(url, &response) ? file_from_response(r, &response, url) : -1;
    if (code) copy_text(error, cap, response.direct ? "This URL did not return a downloadable PKG, FPKG, ZIP or RAR with a filename and size."
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

static bool tag_value(const char *tag, const char *end, const char *attribute, char *out, size_t cap) {
    size_t n = strlen(attribute);
    for (const char *p = tag; p < end; p++) {
        if (!isspace((unsigned char)*p) || end - p <= (ptrdiff_t)n + 2 ||
            strncasecmp(p + 1, attribute, n) || p[n + 1] != '=') continue;
        p += n + 2;
        if (*p != '"' && *p != '\'') return false;
        const char *close = memchr(p + 1, *p, (size_t)(end - p - 1));
        return close && html_text(p + 1, (size_t)(close - p - 1), out, cap);
    }
    return false;
}

static bool page_filename(const char *html, const char *provider, char *name, size_t cap) {
    if (!strcmp(provider, "vikingfile")) {
        const char *tag = strstr(html, "id=\"filename\"");
        const char *text = tag ? strchr(tag, '>') : NULL;
        const char *end = text ? strchr(++text, '<') : NULL;
        return end && html_text(text, (size_t)(end - text), name, cap);
    }
    for (const char *tag = html; (tag = strstr(tag, "<meta")); tag += 5) {
        const char *end = strchr(tag, '>');
        if (!end) break;
        char property[32];
        if (tag_value(tag, end, "property", property, sizeof property) && !strcmp(property, "og:title") &&
            tag_value(tag, end, "content", name, cap)) return true;
    }
    const char *tag = strstr(html, "<title>");
    const char *end = tag ? strstr(tag += strlen("<title>"), "</title>") : NULL;
    if (!end || !html_text(tag, (size_t)(end - tag), name, cap)) return false;
    const char *suffixes[] = {" - DataNodes", " · AkiraBox", " - 1fichier.com"};
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
        char *suffix = strstr(name, suffixes[i]);
        if (suffix && !strcmp(suffix, suffixes[i])) { *suffix = 0; break; }
    }
    return true;
}

static int64_t display_bytes(const char *text, bool bare, bool *exact) {
    const struct { const char *name; int64_t multiplier; } units[] = {
        {"bytes", 1}, {"byte", 1}, {"B", 1},
        {"KB", 1000}, {"MB", 1000000}, {"GB", 1000000000LL}, {"TB", 1000000000000LL},
        {"KiB", 1024}, {"MiB", 1048576}, {"GiB", 1073741824LL}, {"TiB", 1099511627776LL}
    };
    for (const char *p = text; *p; p++) {
        if (!isdigit((unsigned char)*p) || (p != text &&
                (isalnum((unsigned char)p[-1]) || p[-1] == '.' || p[-1] == ','))) continue;
        char number[32];
        const char *end = p;
        size_t n = 0;
        while ((isdigit((unsigned char)*end) || *end == '.' ||
               (*end == ',' && isdigit((unsigned char)end[1]))) && n + 1 < sizeof number) {
            if (*end != ',') number[n++] = *end;
            end++;
        }
        number[n] = 0;
        char *tail = NULL;
        double count = strtod(number, &tail);
        if (!tail || *tail) continue;
        while (isspace((unsigned char)*end)) end++;
        int64_t scale = 0;
        for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
            size_t length = strlen(units[i].name);
            if (!strncasecmp(end, units[i].name, length) && !isalnum((unsigned char)end[length])) {
                scale = units[i].multiplier;
                break;
            }
        }
        if (bare && !*end) scale = 1;
        double bytes = count * scale;
        if (!(bytes >= 1 && bytes <= 9007199254740991.0)) continue;
        *exact = scale == 1 && !strchr(number, '.');
        return (int64_t)bytes;
    }
    return 0;
}

static bool size_label(const char *name) {
    const char *markers[] = {"fileSize", "filesize", "file-size", "file_size", "file_size_info",
        "fsize", "download-size", "downloadSize", "contentSize"};
    if (!strcasecmp(name, "size")) return true;
    for (size_t i = 0; i < sizeof markers / sizeof *markers; i++)
        if (strstr(name, markers[i])) return true;
    return false;
}

static void offer_size(RemoteFileMetadata *metadata, const char *text, bool bare) {
    bool exact = false;
    int64_t size = display_bytes(text, bare, &exact);
    if (size && (!metadata->size || (exact && !metadata->exact))) {
        metadata->size = size;
        metadata->exact = exact;
    }
}

static void page_metadata(const char *html, const char *provider, RemoteFileMetadata *metadata) {
    if (!page_filename(html, provider, metadata->filename, sizeof metadata->filename) &&
        !page_filename(html, "", metadata->filename, sizeof metadata->filename))
        metadata->filename[0] = 0;
    for (const char *tag = html; (tag = strchr(tag, '<')); tag++) {
        const char *end = strchr(tag, '>');
        if (!end) break;
        if (end - tag > 2048) { tag = end; continue; }
        char label[128], value[512];
        const char *attributes[] = {"data-file-size", "data-filesize", "data-size-bytes", "data-size"};
        for (size_t i = 0; i < sizeof attributes / sizeof *attributes; i++)
            if (tag_value(tag, end, attributes[i], value, sizeof value))
                offer_size(metadata, value, true);
        if ((tag_value(tag, end, "property", label, sizeof label) ||
             tag_value(tag, end, "name", label, sizeof label) ||
             tag_value(tag, end, "itemprop", label, sizeof label)) &&
             tag_value(tag, end, "content", value, sizeof value)) {
            if (size_label(label)) offer_size(metadata, value, true);
            else if (!strcmp(label, "og:description") || !strcmp(label, "description"))
                offer_size(metadata, value, false);
        }
        if ((tag_value(tag, end, "id", label, sizeof label) && size_label(label)) ||
            (tag_value(tag, end, "class", label, sizeof label) && size_label(label)) ||
            (tag_value(tag, end, "itemprop", label, sizeof label) && size_label(label))) {
            const char *close = strchr(end + 1, '<');
            if (close && html_text(end + 1, (size_t)(close - end - 1), value, sizeof value))
                offer_size(metadata, value, true);
        }
        tag = end;
    }
    const char *fields[] = {"\"sizeBytes\"", "\"fileSize\"", "\"contentSize\""};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        const char *field = strstr(html, fields[i]);
        if (!field || strstr(field + strlen(fields[i]), fields[i])) continue;
        field += strlen(fields[i]);
        while (isspace((unsigned char)*field)) field++;
        if (*field++ != ':') continue;
        while (isspace((unsigned char)*field)) field++;
        bool quoted = *field == '"';
        if (quoted) field++;
        char value[64];
        size_t length = strcspn(field, quoted ? "\"" : ",}]");
        if (html_text(field, length, value, sizeof value)) offer_size(metadata, value, true);
    }
    const char *name = metadata->filename;
    if (*name == '.' || strpbrk(name, "/\\:") || strstr(name, "..") || !utf8_text(name))
        metadata->filename[0] = 0;
}

bool remote_download_metadata(const Release *r, RemoteFileMetadata *metadata, atomic_bool *cancel) {
    memset(metadata, 0, sizeof *metadata);
    if (!remote_page_pending(r) || !remote_metadata_url(r->browser_url)) return false;
    Response page = {.cancel = cancel, .collect_page = true};
    if (fetch(r->browser_url, &page)) {
        if (!page.page) {
            Release file = *r;
            if (!file_from_response(&file, &page, r->browser_url)) {
                copy_text(metadata->filename, sizeof metadata->filename, file.filename);
                metadata->size = file.size;
                metadata->exact = true;
            }
        } else if (page.body && remote_metadata_url(page.url)) {
            page_metadata(page.body, remote_browser_provider(page.url), metadata);
        }
    }
    free(page.body);
    return metadata->size > 0;
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

static int page_link(const Response *page, Release *r, char *url, size_t cap) {
    char href[2048], candidate[2048], filename[sizeof r->filename];
    for (const char *tag = page->body; (tag = strstr(tag, "<a ")); tag += 3) {
        const char *end = strchr(tag, '>');
        if (!end) break;
        Release file = *r;
        if (!tag_value(tag, end, "href", href, sizeof href) ||
            !resolve_href(page->url, href, candidate, sizeof candidate) ||
            !strcmp(candidate, page->url) || *remote_browser_provider(candidate) ||
            !remote_url_filename(candidate, filename, sizeof filename) || remote_file_format(&file, filename)) continue;
        if (url[0] && strcmp(url, candidate)) return -2;
        copy_text(url, cap, candidate);
    }
    return url[0] ? 0 : -1;
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

/* 0: verified direct file, 1: continue with Orbit's supported provider browser,
 * -1: no supported file can be determined from the selected option. */
int remote_download_resolve(Release *r, Release *locator, atomic_bool *cancel, char *error, size_t cap) {
    Response page = {.cancel = cancel, .collect_page = true};
    int result = -1;
    if (!fetch(r->browser_url, &page)) goto done;
    if (!page.page) {
        result = file_from_response(r, &page, r->browser_url) ? -1 : 0;
        goto done;
    }
    if (!page.body || !page.used) goto done;
    const char *provider = remote_browser_provider(page.url);
    if (*provider) {
        char name[sizeof r->filename];
        if (!page_filename(page.body, provider, name, sizeof name)) goto done;
        *locator = *r;
        if (remote_file_format(locator, name)) goto done;
        copy_text(locator->source, sizeof locator->source, provider);
        /* The private matcher still enforces the provider's selected file route. */
        if (strlen(page.url) >= sizeof locator->browser_url) goto done;
        copy_text(locator->browser_url, sizeof locator->browser_url, page.url);
        copy_text(locator->url, sizeof locator->url, page.url);
        if (!provider_browser_supported(locator)) goto done;
        result = 1;
    } else {
        char direct[sizeof r->url] = "";
        int link = !strncmp(page.url, "https://archive.org/details/", 28)
            ? archive_link(&page, r, direct, sizeof direct)
            : page.truncated ? -1 : page_link(&page, r, direct, sizeof direct);
        if (!link && !remote_download_verify(r, direct, cancel, error, cap)) result = 0;
        if (link == -2) copy_text(error, cap, "This option lists several files. Choose a mirror that identifies one download.");
    }
done:
    free(page.body);
    if (result < 0 && !error[0])
        copy_text(error, cap, "This option did not identify a supported PKG, FPKG, RAR or ZIP download.");
    return result;
}
