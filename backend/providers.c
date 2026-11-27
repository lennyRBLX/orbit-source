#include "orbit.h"
#include "provider_network.h"
#include "links.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
bool provider_browser_source(const char *source) {
    return source && (!strcmp(source, "vikingfile") || !strcmp(source, "akirabox") ||
                      !strcmp(source, "datanodes") || !strcmp(source, "1fichier"));
}
static bool browser_format(const Release *r) {
    if (r->archive_unverified && (strcmp(r->format, "Unknown") || r->extracted_size || !r->compression[0])) return false;
    const char *suffix = !strcmp(r->format, "exFAT") ? ".exfat" :
                         !strcmp(r->format, "FFPFSC") ? ".ffpfsc" :
                         (!strcmp(r->format, "FPKG") || !strcmp(r->format, "PKG")) ? ".pkg" : NULL;
    if (!suffix && !r->archive_unverified && (strcmp(r->format, "Game folder") || !r->compression[0] ||
        strncmp(r->title_id, "PPSA", 4))) return false;
    if (r->compression[0]) {
        if (r->extracted_size <= 0 && !r->archive_unverified) return false;
        suffix = !strcmp(r->compression, "zip") ? ".zip" :
                 !strcmp(r->compression, "7z") ? ".7z" :
                 !strcmp(r->compression, "rar") ? ".rar" : NULL;
    }
    size_t n = strlen(r->filename);
    /* The provider filename remains exact; compression selects the actual
     * RAR decoder even when the published filename has a .7z suffix. */
    if (suffix && !strcmp(r->compression, "rar") && n > 3 &&
        !strcasecmp(r->filename + n - 3, ".7z"))
        suffix = ".7z";
    return suffix && n > strlen(suffix) && !strcasecmp(r->filename + n - strlen(suffix), suffix);
}
static bool browser_file(const Release *r) {
    if (!provider_browser_source(r->source) || strlen(r->title_id) != 9 ||
        (strncmp(r->title_id, "PPSA", 4) && strncmp(r->title_id, "CUSA", 4)) || !r->filename[0] ||
        strchr(r->filename, '/') || strchr(r->filename, '\\') || strstr(r->filename, ".."))
        return false;
    for (size_t i = 4; i < 9; i++)
        if (!isdigit((unsigned char)r->title_id[i])) return false;
    for (const unsigned char *p = (const void *)r->filename; *p; p++)
        if (*p < 32 || *p == 127) return false;
    /* Community locators pin the name. Their GET verifies the signature. */
    if (!r->remote_page && !browser_format(r)) return false;
    /* A source may use a descriptive filename without its title ID. If an ID
     * is present it must agree with the catalogue; matching remains exact. */
    const char *prefixes[] = {"PPSA", "CUSA"};
    for (size_t i = 0; i < 2; i++)
        for (const char *p = strstr(r->filename, prefixes[i]); p; p = strstr(p + 4, prefixes[i]))
            if (strncmp(p, r->title_id, 9) || isdigit((unsigned char)p[9])) return false;
    return true;
}
static bool browser_page(const Release *r, const char *url) {
    if (!strcmp(r->source, "1fichier")) {
        const char *prefix = "https://1fichier.com/?";
        size_t n = strlen(prefix);
        if (strncmp(url, prefix, n) || strlen(url + n) != 20) return false;
        for (const char *p = url + n; *p; p++)
            if (!isalnum((unsigned char)*p)) return false;
        return true;
    }
    if (strcmp(r->source, "vikingfile")) {
        const char *prefixes[] = {"https://akirabox.com/", "https://akirabox.to/", "https://datanodes.to/"};
        for (size_t i = 0; i < 3; i++) {
            if ((i == 2) != !strcmp(r->source, "datanodes")) continue;
            size_t n = strlen(prefixes[i]);
            if (strncmp(url, prefixes[i], n)) continue;
            const char *p = url + n;
            if (strlen(p) != (i == 2 ? 12U : 17U)) return false;
            for (size_t j = 0; j < 12; j++) if (!isalnum((unsigned char)p[j])) return false;
            return i == 2 || !strcmp(p + 12, "/file");
        }
        return false;
    }
    const char *prefixes[] = {"https://vik1ngfile.site/f/", "https://vikingfile.com/f/", "https://vik1ngf1le.online/f/"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(url, prefixes[i], n) || strlen(url + n) != 10) continue;
        for (const char *p = url + n; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9'))) return false;
        return true;
    }
    return false;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Browser-generated routes remain provider-bound and filename-bound. Tokens
 * are opaque; they are neither constructed nor retained in the catalogue. */
static bool generated_route(const Release *r, const char *url) {
    if (strncmp(url, "https://", 8)) return false;
    const char *host = url + 8, *path = strchr(host, '/');
    if (!path || path - host > 253 || strchr(url, '#')) return false;
    size_t n = (size_t)(path - host);
    bool akira = !strcmp(r->source, "akirabox"), data = !strcmp(r->source, "datanodes");
    bool fichier = !strcmp(r->source, "1fichier");
    bool gateway = data && n == strlen("tunnel5.dlproxy.uk") && !memcmp(host, "tunnel5.dlproxy.uk", n);
    bool allowed = akira && ((n == 12 && !strncmp(host, "akirabox.com", n)) ||
                            (n == 11 && !strncmp(host, "akirabox.to", n)));
    if (data) {
        const char *base = "datanodes.to"; size_t b = strlen(base);
        allowed = gateway || (n >= b && !memcmp(host + n - b, base, b) &&
                  (n == b || host[n - b - 1] == '.'));
    }
    /* Generated file servers only, never the file page, API or account host.
     * The opaque token is a candidate; capture must verify the exact response. */
    if (fichier) {
        const char *suffix = ".1fichier.com"; size_t b = strlen(suffix);
        if (n <= b + 2 || memcmp(host, "a-", 2) || memcmp(host + n - b, suffix, b)) return false;
        for (size_t i = 2; i < n - b; i++)
            if (!isalnum((unsigned char)host[i]) && host[i] != '-') return false;
        size_t token = strlen(path + 1);
        if (token < 10 || token > 256) return false;
        for (const char *p = path + 1; *p; p++)
            if (!isalnum((unsigned char)*p)) return false;
        return true;
    }
    if (!allowed) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)host[i]) && host[i] != '.' && host[i] != '-') return false;
    if (akira && strncmp(path, "/download/", 10)) return false;
    const char *end = strchr(path, '?');
    if (!end) end = url + strlen(url);
    /* Reject path normalization tricks before curl interprets the address. */
    for (const char *p = path + 1; p < end;) {
        char segment[2048]; size_t used = 0;
        while (p < end && *p != '/') {
            unsigned char c = (unsigned char)*p++;
            if (c == '%') {
                if (end - p < 2 || hex(p[0]) < 0 || hex(p[1]) < 0) return false;
                c = (unsigned char)(hex(p[0]) * 16 + hex(p[1])); p += 2;
            }
            if (!c || c < 32 || c == 127 || c == '/' || c == '\\' || used + 1 >= sizeof segment) return false;
            segment[used++] = (char)c;
        }
        segment[used] = 0;
        if (!used || !strcmp(segment, ".") || !strcmp(segment, "..")) return false;
        if (p < end) p++;
    }
    const char *name = end;
    while (name > path && name[-1] != '/') name--;
    if (name == path + 1 || name == end) return false;
    if (gateway) {
        /* Opaque routes need the same exact Content-Disposition/size/identity
         * validation as filename routes. Only this observed gateway is allowed. */
        if (name - path != 10 || end - name < 32 || end - name > 1024 ||
            strncmp(end, "?sig=", 5) || !end[5] || strchr(end + 5, '&')) return false;
        for (const unsigned char *p = (const void *)url; *p; p++)
            if (*p <= 32 || *p >= 127 || *p == '\\') return false;
        return true;
    }
    const unsigned char *expected = (const void *)r->filename;
    for (const char *p = name; p < end; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') {
            if (end - p < 3 || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
            c = (unsigned char)(hex(p[1]) * 16 + hex(p[2])); p += 2;
        } else if (akira && c == '+') {
            /* AkiraBox form-encodes the filename in its ready-button path.
             * Decode only for identity comparison; preserve the signed URL.
             * A literal plus is %2B and must not be decoded a second time. */
            c = ' ';
        }
        if (!c || !*expected || c != *expected++) return false;
    }
    if (*expected) return false;
    for (const unsigned char *p = (const void *)url; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '\\') return false;
    return true;
}
/* Stable storage hosts are supplied by Archive's public item metadata.
 * A storage route stays flat and exactly filename-bound. */
static bool archive_storage_url(const Release *r) {
    const char *url = r->url;
    if (strncmp(url, "https://ia", 10) || strchr(url, '?') || strchr(url, '#') ||
        !r->filename[0] || strchr(r->filename, '/') || strchr(r->filename, '\\') || strstr(r->filename, ".."))
        return false;
    const char *at = url + 10;
    for (int i = 0; i < 6; i++) if (!isdigit((unsigned char)*at++)) return false;
    const char *us = ".us.archive.org/", *ca = ".ca.archive.org/";
    if (strncmp(at, us, strlen(us)) && strncmp(at, ca, strlen(ca))) return false;
    at += strlen(us);
    size_t volume = 0;
    while (isdigit((unsigned char)*at)) { if (++volume > 4) return false; at++; }
    if (!volume || strncmp(at, "/items/", 7)) return false;
    at += 7;
    const char *item = at;
    if (!isalnum((unsigned char)*at)) return false;
    while (*at && *at != '/') {
        if (!isalnum((unsigned char)*at) && *at != '_' && *at != '-' && *at != '.') return false;
        if (*at == '.' && at[1] == '.') return false;
        if (at - item >= 255) return false;
        at++;
    }
    if (*at++ != '/' || !*at) return false;
    const unsigned char *expected = (const void *)r->filename;
    while (*at) {
        unsigned char c = (unsigned char)*at++;
        if (c == '%') {
            if (!at[0] || !at[1] || hex(at[0]) < 0 || hex(at[1]) < 0) return false;
            c = (unsigned char)(hex(at[0]) * 16 + hex(at[1])); at += 2;
        } else if (c <= 32 || c >= 127) return false;
        if (c < 32 || c == 127 || c == '/' || c == '\\' || !*expected || c != *expected++) return false;
    }
    return !*expected;
}

/* Vikingfile's file servers: a short name of lowercase letters or digits
 * under vikingfile.com (ia., ar., ko., ty. seen so far; ty. first on
 * 13 November 2026). Exactly n characters of host are checked. */
bool provider_viking_file_server(const char *host, size_t n) {
    const char *base = ".vikingfile.com";
    size_t b = strlen(base), label = n > b ? n - b : 0;
    if (label < 2 || label > 4 || memcmp(host + label, base, b)) return false;
    for (size_t i = 0; i < label; i++)
        if (!islower((unsigned char)host[i]) && !isdigit((unsigned char)host[i])) return false;
    return true;
}
/* Exactly "?md5=<token>&expires=<seconds>": nothing else may ride along. */
static bool viking_signature(const unsigned char *query) {
    if (strncmp((const char *)query, "?md5=", 5)) return false;
    const unsigned char *p = query + 5, *start = p;
    while (isalnum(*p) || *p == '-' || *p == '_') p++;
    if (p - start < 8 || p - start > 64 || strncmp((const char *)p, "&expires=", 9)) return false;
    p += 9; start = p;
    while (isdigit(*p)) p++;
    return p - start >= 9 && p - start <= 12 && !*p;
}

/* Percent-decode n bytes. Fails on a bad escape, a control byte or no room. */
static bool percent_decode(const char *src, size_t n, char *out, size_t cap) {
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (n - i < 3 || hex((unsigned char)src[i + 1]) < 0 || hex((unsigned char)src[i + 2]) < 0) return false;
            c = (unsigned char)(hex((unsigned char)src[i + 1]) * 16 + hex((unsigned char)src[i + 2]));
            i += 2;
        }
        if (c < 32 || c == 127 || used + 1 >= cap) return false;
        out[used++] = (char)c;
    }
    out[used] = 0;
    return true;
}
static const char *find_nocase(const char *text, const char *word) {
    for (size_t n = strlen(word); *text; text++)
        if (!strncasecmp(text, word, n)) return text;
    return NULL;
}
/* The file name in a Content-Disposition value: filename*=UTF-8''<encoded>
 * first, then filename="<name>" or filename=<name>. */
bool provider_header_filename(const char *value, char *out, size_t cap) {
    for (const char *p = value; (p = find_nocase(p, "filename*=")); p += 10) {
        if (p != value && p[-1] != ';' && p[-1] != ' ') continue;
        const char *v = p + 10, *quote = strstr(v, "''");
        if (!quote || strncasecmp(v, "UTF-8", 5)) return false;
        v = quote + 2;
        return percent_decode(v, strcspn(v, ";"), out, cap) && *out;
    }
    for (const char *p = value; (p = find_nocase(p, "filename=")); p += 9) {
        if (p != value && p[-1] != ';' && p[-1] != ' ') continue;
        const char *v = p + 9;
        size_t used = 0;
        if (*v == '"') {
            for (v++; *v && *v != '"'; v++) {
                if (*v == '\\' && v[1]) v++;
                if (used + 1 >= cap) return false;
                out[used++] = *v;
            }
            if (*v != '"') return false;
        } else {
            size_t n = strcspn(v, "; ");
            if (n >= cap) return false;
            memcpy(out, v, n); used = n;
        }
        out[used] = 0;
        return used > 0;
    }
    return false;
}
bool provider_content_filename(const char *disposition, char *out, size_t cap) {
    return provider_header_filename(disposition, out, cap);
}
bool provider_percent_decode(const char *src, size_t n, char *out, size_t cap) {
    return percent_decode(src, n, out, cap);
}
/* The file name a storage link's response-content-disposition asks for. The
 * value may be encoded once or twice. Also used for the kernel log. */
bool provider_disposition_name(const char *url, char *out, size_t cap) {
    const char *key = "response-content-disposition=", *query = url ? strchr(url, '?') : NULL;
    for (const char *p = query; p && *p; p = strchr(p + 1, '&')) {
        if (strncmp(p + 1, key, strlen(key))) continue;
        const char *v = p + 1 + strlen(key);
        char once[1024], twice[1024];
        if (!percent_decode(v, strcspn(v, "&#"), once, sizeof once)) return false;
        if (provider_header_filename(once, out, cap)) return true;
        return strchr(once, '%') && percent_decode(once, strlen(once), twice, sizeof twice) &&
               provider_header_filename(twice, out, cap);
    }
    return false;
}
/* Vikingfile's Download button can go straight to its OVH storage (seen on a
 * PS4 on 13 November 2026): a one-part object key signed with AWS SigV4. The
 * file's name is only in response-content-disposition and must be the
 * option's exact filename; the capture probe then checks the file itself. */
static bool viking_storage_link(const Release *r) {
    const char *url = r->url, *suffix = ".s3.bhs.io.cloud.ovh.net";
    if (strncmp(url, "https://", 8) || strchr(url, '#')) return false;
    const char *host = url + 8, *path = strchr(host, '/');
    size_t n = path ? (size_t)(path - host) : 0, s = strlen(suffix);
    if (path && n > s && !memcmp(host + n - s, ".s3.sbg.io.cloud.ovh.net", s))
        suffix = ".s3.sbg.io.cloud.ovh.net";
    if (!path || n <= s || n > s + 63 || memcmp(host + n - s, suffix, s)) return false;
    for (size_t i = 0; i < n - s; i++)
        if (!islower((unsigned char)host[i]) && !isdigit((unsigned char)host[i]) && host[i] != '-') return false;
    const char *query = strchr(path, '?');
    if (!query || query == path + 1 || query - path > 512) return false;
    bool dots = true;
    for (const char *p = path + 1; p < query; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.' && *p != '_' && *p != '~' && *p != '%') return false;
        dots = dots && *p == '.';
    }
    if (dots) return false;
    static const char *const required[] = {"X-Amz-Algorithm", "X-Amz-Credential", "X-Amz-Date",
        "X-Amz-SignedHeaders", "X-Amz-Signature", "response-content-disposition"};
    const size_t count = sizeof required / sizeof *required;
    unsigned seen = 0;
    for (const char *q = query + 1; *q; ) {
        size_t len = strcspn(q, "&"), key = strcspn(q, "=&");
        if (!key || key >= len) return false;
        for (size_t i = key + 1; i < len; i++)
            if ((unsigned char)q[i] <= 32 || (unsigned char)q[i] >= 127 || q[i] == '\\') return false;
        bool known = false;
        for (size_t i = 0; i < count; i++)
            if (key == strlen(required[i]) && !strncmp(q, required[i], key)) {
                if (seen & 1u << i) return false;
                seen |= 1u << i; known = true;
            }
        if (!known && strncasecmp(q, "X-Amz-", 6) && strncmp(q, "response-", 9) &&
            !(key == 4 && !strncmp(q, "x-id", 4))) return false;
        if (key == 15 && !strncmp(q, "X-Amz-Algorithm", 15) &&
            (len != 32 || strncmp(q + 16, "AWS4-HMAC-SHA256", 16))) return false;
        q += len;
        if (*q == '&' && !*++q) return false;
    }
    char name[sizeof r->filename];
    return seen == (1u << count) - 1 && provider_disposition_name(url, name, sizeof name) &&
           !strcmp(name, r->filename);
}

/* Provider boundary: never turn metadata/page URLs into unverified downloads. */
bool provider_url_supported(const Release *release) {
    if (!strcmp(release->source, "remote")) return remote_https_url(release->url);
    /* A link the user pasted: any https host, guarded like provider downloads. */
    if (!strcmp(release->source, "link")) return link_url_allowed(release->url);
    if (!strcmp(release->id, "orbit-network-check"))
        return !strcmp(release->url,
                       "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/LICENSE");
    if (!strcmp(release->source, "archive"))
        return ((!strncmp(release->url, "https://archive.org/download/", 29) && strlen(release->url) > 29) || archive_storage_url(release)) &&
               strlen(release->url) > 29 &&
               (!strcmp(release->format, "FFPFSC") || !strcmp(release->format, "exFAT") ||
                !strcmp(release->format, "FPKG") || !strcmp(release->format, "PKG") ||
                (release->archive_unverified && release->compression[0]) ||
                (!strcmp(release->format, "Game folder") && !strncmp(release->title_id, "PPSA", 4) &&
                 release->extracted_size > 0 &&
                 (!strcmp(release->compression, "zip") || !strcmp(release->compression, "7z") || !strcmp(release->compression, "rar"))));
    /* Only exact filename-bound direct routes may enter the download queue.
     * Landing pages are a separate browser capability, never a transfer URL. */
    if (!browser_file(release)) return false;
    if (strcmp(release->source, "vikingfile")) return generated_route(release, release->url);
    if (viking_storage_link(release)) return true;
    /* vikingfile.com/d/ is the account route. The file page's Download button
     * can lead to a file server's /download/ route (ia., seen on PS4 through
     * Sony's browser window on 13 November 2026, or another file server),
     * signed by an md5 and expires query. */
    const char *prefix = "https://vikingfile.com/d/", *path = NULL;
    bool signed_route = false;
    if (!strncmp(release->url, "https://", 8)) {
        const char *host = release->url + 8, *slash = strchr(host, '/');
        if (slash && provider_viking_file_server(host, (size_t)(slash - host)) && !strncmp(slash, "/download/", 10)) {
            path = slash + 10; signed_route = true;
        }
    }
    if (!path && !strncmp(release->url, prefix, strlen(prefix))) path = release->url + strlen(prefix);
    /* Vikingfile's file pages moved to vik1ngf1le.online, so the same route
     * can be offered there. */
    const char *moved = "https://vik1ngf1le.online/d/";
    if (!path && !strncmp(release->url, moved, strlen(moved))) path = release->url + strlen(moved);
    if (!path) return false;
    if (strlen(path) < 12 || path[10] != '/')
        return false;
    for (size_t i = 0; i < 10; i++)
        if (!isalnum((unsigned char)path[i])) return false;
    const unsigned char *expected = (const void *)release->filename;
    const unsigned char *p = (const void *)(path + 11);
    for (; *p && !(signed_route && *p == '?'); p++) {
        unsigned char c = *p;
        if (c == '%') {
            if (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0) return false;
            c = (unsigned char)(hex(p[1]) * 16 + hex(p[2])); p += 2;
        } else if (c <= 32 || c >= 127 || c == '?' || c == '#' || c == '/' || c == '\\')
            return false;
        if (!*expected || c != *expected++) return false;
    }
    if (*expected) return false;
    if (!signed_route) return true;
    return viking_signature(p);
}


bool provider_supported(const Release *release) {
#ifdef ORBIT_TEST
    /* Test binaries may parse the catalogue, but may only transfer fixtures. */
    return !strncmp(release->url, "http://127.0.0.1:", 17) &&
           (!strcmp(release->source, "archive") || !strcmp(release->source, "remote") ||
            !strcmp(release->source, ORBIT_DIRECT_SOURCE) ||
            !strcmp(release->source, "link") || provider_browser_source(release->source));
#else
    if (!strcmp(release->source, ORBIT_DIRECT_SOURCE)) return direct_http_url(release->url);
    return provider_url_supported(release);
#endif
}

bool provider_browser_supported(const Release *release) {
    if (!release || !provider_browser_source(release->source)) return false;
#ifdef ORBIT_TEST
    if (!strncmp(release->browser_url, "http://127.0.0.1:", 17)) return true;
#endif
    return browser_page(release, release->browser_url) &&
        (browser_file(release) || (release->remote_page && !release->filename[0])) &&
        (!strcmp(release->url, release->browser_url) || provider_url_supported(release));
}

/* When the page omitted a filename, a captured path can supply it only if
 * its file ID is the same as the selected provider page's. Opaque routes
 * still need the page's exact filename before they can be matched. */
static bool page_bound_filename(Release *r) {
    if (!r->remote_page || !browser_page(r, r->browser_url) || strncmp(r->url, "https://", 8)) return false;
    const char *page = strchr(r->browser_url + 8, '/'), *path = strchr(r->url + 8, '/');
    if (!page || !path) return false;
    bool akira = !strcmp(r->source, "akirabox");
    const char *id = path + 1;
    size_t length = 12;
    if (!strcmp(r->source, "vikingfile")) {
        page += 3; /* /f/ */
        length = 10;
        if (!strncmp(path, "/d/", 3)) id = path + 3;
        else if (!strncmp(path, "/download/", 10)) id = path + 10;
        else return false;
    } else {
        page++;
        if (akira && !strncmp(path, "/download/", 10)) id = path + 10;
        else if (strcmp(r->source, "datanodes")) return false;
    }
    if (strncmp(page, id, length) || id[length] != '/') return false;
    const char *start = id + length + 1;
    size_t n = strcspn(start, "?#");
    char encoded[3 * sizeof r->filename];
    if (!n || n >= sizeof encoded || memchr(start, '/', n)) return false;
    for (size_t i = 0; i < n; i++) encoded[i] = akira && start[i] == '+' ? ' ' : start[i];
    return percent_decode(encoded, n, r->filename, sizeof r->filename);
}

bool provider_capture_url(const Release *release, const char *candidate) {
    if (!provider_browser_source(release->source) || !candidate || strlen(candidate) >= sizeof release->url) return false;
    Release resolved = *release;
    strcpy(resolved.url, candidate);
    if (!resolved.filename[0] && !page_bound_filename(&resolved)) return false;
    return provider_supported(&resolved);
}

const char *provider_torbox_url(const Release *r) {
    /* Submit the stable page for these providers, including after capture. */
    if (r->remote_page || ((!strcmp(r->source, "datanodes") || !strcmp(r->source, "1fichier") ||
        !strcmp(r->source, "akirabox")) && provider_browser_supported(r))) return r->browser_url;
    return r->url;
}
