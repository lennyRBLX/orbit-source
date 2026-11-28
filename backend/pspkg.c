#include "pspkg.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool origin(const char *url, const char **path) {
    const char *prefix = "https://pspkg.com";
    size_t n = strlen(prefix);
    if (!strncmp(url, prefix, n) && url[n] == '/') { *path = url + n; return true; }
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *slash = strchr(url + 17, '/');
        if (slash) { *path = slash; return true; }
    }
#endif
    return false;
}

static bool section(const char *path, const char **platform, const char **name) {
    if (!strncmp(path, "/ps5/", 5)) *platform = "ps5";
    else if (!strncmp(path, "/ps4/", 5)) *platform = "ps4";
    else return false;
    *name = path + 5;
    return true;
}

static bool game_name(const char *name, const char **digits, const char **suffix) {
    *suffix = strstr(name, ".html");
    if (!*suffix || (*suffix)[5] || *suffix == name || strchr(name, '?') || strchr(name, '#') ||
        strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.") != strlen(name) ||
        *name == '.' || strstr(name, "..")) return false;
    *digits = *suffix;
    while (*digits > name && isdigit((unsigned char)(*digits)[-1])) (*digits)--;
    return *digits != *suffix && *digits > name && (*digits)[-1] == '-' &&
        *suffix - *digits <= 12;
}

bool pspkg_download_page(const char *game, char *out, size_t cap) {
    if (!game || !out || !cap) return false;
    const char *path, *platform, *name, *digits, *suffix;
    if (!origin(game, &path) || !section(path, &platform, &name) ||
        !game_name(name, &digits, &suffix) || !strncmp(name, "download-", 9)) return false;
    int n = snprintf(out, cap, "%.*sdownload-%s", (int)(name - game), game, name);
    return n > 0 && (size_t)n < cap;
}

bool pspkg_page_matches(const char *game, const char *page) {
    char expected[2048];
    if (!page || !pspkg_download_page(game, expected, sizeof expected)) return false;
    if (!strcmp(expected, page)) return true;
    const char *path, *platform, *name, *digits, *suffix;
    if (!origin(game, &path) || !section(path, &platform, &name) || !game_name(name, &digits, &suffix)) return false;
    size_t length = strlen(expected);
    return !strncmp(expected, page, length) && !strncmp(page + length, "?id=", 4) &&
        strlen(page + length + 4) == (size_t)(suffix - digits) &&
        !strncmp(page + length + 4, digits, (size_t)(suffix - digits));
}

bool pspkg_page_identity(const char *page, char *platform, size_t platform_cap,
                         char *game_id, size_t id_cap) {
    if (!page || !platform || !game_id || platform_cap < 4 || id_cap < 2) return false;
    const char *path, *part, *name, *digits, *suffix;
    if (!origin(page, &path) || !section(path, &part, &name) ||
        strncmp(name, "download-", 9)) return false;
    size_t prefix = (size_t)(name - page), length = strcspn(name + 9, "?");
    char game[2048];
    if (prefix + length >= sizeof game) return false;
    memcpy(game, page, prefix);
    memcpy(game + prefix, name + 9, length);
    game[prefix + length] = 0;
    if (!game_name(game + prefix, &digits, &suffix) || !pspkg_page_matches(game, page)) return false;
    length = (size_t)(suffix - digits);
    if (length >= id_cap) return false;
    memcpy(platform, part, 4);
    memcpy(game_id, digits, length);
    game_id[length] = 0;
    return true;
}

static bool read_char(const unsigned char *bytes, size_t length, size_t *at,
                      size_t step, unsigned char *out) {
    if (*at + step > length || (step == 2 && bytes[*at + 1])) return false;
    *out = bytes[*at];
    *at += step;
    return true;
}

static bool literal(const unsigned char *bytes, size_t length, size_t *at,
                    size_t step, const char *value) {
    for (const char *p = value; *p; p++) {
        unsigned char c;
        if (!read_char(bytes, length, at, step, &c) || c != (unsigned char)*p) return false;
    }
    return true;
}

static bool digits(const unsigned char *bytes, size_t length, size_t *at,
                   size_t step, char *out, size_t cap) {
    size_t used = 0;
    while (*at + step <= length && used + 1 < cap) {
        size_t next = *at;
        unsigned char c;
        if (!read_char(bytes, length, &next, step, &c) || !isdigit(c)) break;
        out[used++] = (char)c;
        *at = next;
    }
    out[used] = 0;
    return used > 0 && used <= 12;
}

static bool separator(const unsigned char *bytes, size_t length, size_t *at, size_t step) {
    size_t next = *at;
    if (literal(bytes, length, &next, step, "&amp;")) { *at = next; return true; }
    next = *at;
    if (literal(bytes, length, &next, step, "&")) { *at = next; return true; }
    return false;
}

void pspkg_scan_links(const unsigned char *bytes, size_t length, const char *platform,
                      const char *game_id, PspkgLink *links, size_t *count, size_t cap) {
    if (!bytes || !links || !count || !game_id || !*game_id ||
        (strcmp(platform, "ps5") && strcmp(platform, "ps4"))) return;
    if (cap > PSPKG_LINK_LIMIT) cap = PSPKG_LINK_LIMIT;
    for (size_t step = 1; step <= 2; step++) {
        for (size_t i = 0; i + 22 * step <= length && *count < cap; i++) {
            if (bytes[i] != 'd' || (step == 2 && bytes[i + 1])) continue;
            size_t at = i;
            char game[13], edition[13], link[13];
            if (!literal(bytes, length, &at, step, "download.php?game_id=") ||
                !digits(bytes, length, &at, step, game, sizeof game) ||
                strcmp(game, game_id) || !separator(bytes, length, &at, step) ||
                !literal(bytes, length, &at, step, "edition_id=") ||
                !digits(bytes, length, &at, step, edition, sizeof edition) ||
                !separator(bytes, length, &at, step) ||
                !literal(bytes, length, &at, step, "link_id=") ||
                !digits(bytes, length, &at, step, link, sizeof link)) continue;
            size_t boundary = at;
            unsigned char next;
            if (read_char(bytes, length, &boundary, step, &next) && isdigit(next)) continue;
            /* Preserve the remainder of the revealed href, including any
             * query parameters. Never synthesize browser/session parameters. */
            char tail[1800] = "";
            size_t used = 0;
            while (at + step <= length) {
                size_t end = at;
                unsigned char c;
                if (!read_char(bytes, length, &end, step, &c) ||
                    c <= 32 || c >= 127 || strchr("\"'<>`\\#", c)) break;
                if (used + 1 >= sizeof tail) break;
                size_t escaped = at;
                if (c == '&' && literal(bytes, length, &escaped, step, "&amp;")) end = escaped;
                tail[used++] = (char)c;
                at = end;
            }
            if (used + 1 >= sizeof tail) continue;
            bool duplicate = false;
            for (size_t j = 0; j < *count; j++)
                if (!strcmp(links[j].link_id, link) && !strcmp(links[j].edition_id, edition))
                    duplicate = true;
            if (duplicate) continue;
            PspkgLink *target = &links[(*count)++];
            memcpy(target->edition_id, edition, strlen(edition) + 1);
            memcpy(target->link_id, link, strlen(link) + 1);
            snprintf(target->url, sizeof target->url,
                     "https://pspkg.com/%s/download.php?game_id=%s&edition_id=%s&link_id=%s%s",
                     platform, game_id, edition, link, tail);
            target->intermediate = true;
            snprintf(target->label, sizeof target->label, "PSPKG");
        }
    }
}
