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
        strchr(name, '/')) return false;
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
    int n = snprintf(out, cap, "%.*sdownload-%s?id=%.*s",
                     (int)(name - game), game, name, (int)(suffix - digits), digits);
    return n > 0 && (size_t)n < cap;
}

bool pspkg_page_matches(const char *game, const char *page) {
    char expected[2048];
    return page && pspkg_download_page(game, expected, sizeof expected) && !strcmp(expected, page);
}

bool pspkg_page_identity(const char *page, char *platform, size_t platform_cap,
                         char *game_id, size_t id_cap) {
    if (!page || !platform || !game_id || platform_cap < 4 || id_cap < 2) return false;
    const char *path, *part, *name, *digits, *suffix;
    if (!origin(page, &path) || !section(path, &part, &name) ||
        strncmp(name, "download-", 9)) return false;
    const char *query = strstr(name, ".html?id=");
    if (!query || query[9] == 0) return false;
    size_t id_length = strlen(query + 9);
    if (id_length >= id_cap || id_length > 12 ||
        strspn(query + 9, "0123456789") != id_length) return false;
    char game[2048], expected[2048];
    int n = snprintf(game, sizeof game, "%.*s%s", (int)(name - page), page, name + 9);
    if (n <= 0 || (size_t)n >= sizeof game) return false;
    /* Strip the query while retaining the original game slug. */
    char *q = strchr(game, '?');
    if (q) *q = 0;
    if (!game_name(game + (name - page), &digits, &suffix) ||
        (size_t)(suffix - digits) != id_length ||
        strncmp(digits, query + 9, id_length) ||
        !pspkg_download_page(game, expected, sizeof expected) || strcmp(expected, page)) return false;
    memcpy(platform, part, 4);
    memcpy(game_id, query + 9, id_length);
    game_id[id_length] = 0;
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
            bool duplicate = false;
            for (size_t j = 0; j < *count; j++)
                if (!strcmp(links[j].link_id, link) && !strcmp(links[j].edition_id, edition))
                    duplicate = true;
            if (duplicate) continue;
            PspkgLink *target = &links[(*count)++];
            memcpy(target->edition_id, edition, strlen(edition) + 1);
            memcpy(target->link_id, link, strlen(link) + 1);
            snprintf(target->url, sizeof target->url,
                     "https://pspkg.com/%s/download.php?game_id=%s&edition_id=%s&link_id=%s&browser_type=other",
                     platform, game_id, edition, link);
        }
    }
}
