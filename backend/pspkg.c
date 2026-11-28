#include "pspkg.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

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

bool pspkg_page_redirect(const char *page, const char *redirect) {
    const char *path, *next;
    if (!origin(page, &path) || !origin(redirect, &next)) return false;
    size_t host = (size_t)(path - page);
    if (host != (size_t)(next - redirect) || memcmp(page, redirect, host)) return false;
    /* The general page can make a same-game round trip through the locale route. */
    if (!strncmp(next, "/ar/", 4)) next += 3;
    size_t length = strcspn(path, "?");
    return length == strcspn(next, "?") && !memcmp(path, next, length);
}

bool pspkg_link_matches(const char *url, const char *page, const char *platform,
                        const char *game_id, const char *edition_id) {
    const char *path, *page_path, *part, *name;
    if (!origin(url, &path) || !origin(page, &page_path) ||
        path - url != page_path - page || memcmp(url, page, (size_t)(path - url)) ||
        !section(path, &part, &name) || strcmp(part, platform)) return false;
    size_t edition_length = strspn(edition_id, "0123456789");
    if (!edition_length || edition_length > 12 || edition_id[edition_length]) return false;
    char prefix[80];
    int length = snprintf(prefix, sizeof prefix, "download.php?game_id=%s&edition_id=%s&link_id=",
                          game_id, edition_id);
    if (length < 0 || (size_t)length >= sizeof prefix || strncmp(name, prefix, (size_t)length))
        return false;
    name += length;
    size_t link_length = strspn(name, "0123456789");
    return link_length && link_length <= 12 &&
        (!name[link_length] || !strcmp(name + link_length, "&route=browser"));
}

const char *pspkg_title_id(const char *text) {
    for (const char *p = text; *p; p++) {
        if (p > text && isalnum((unsigned char)p[-1])) continue;
        if (strncasecmp(p, "PPSA", 4) && strncasecmp(p, "CUSA", 4)) continue;
        if (strspn(p + 4, "0123456789") == 5 && !isalnum((unsigned char)p[9])) return p;
    }
    return NULL;
}

static bool version_next_to_id(const char *start, const char *end, const char *id) {
    if (!id) return false;
    const char *separators = " \t._-:()[]";
    return (end < id && end + strspn(end, separators) == id) ||
           (id + 9 < start && id + 9 + strspn(id + 9, separators) == start);
}

static bool version_size_suffix(const char *end) {
    const char *unit = end + strspn(end, " \t");
    const char *units[] = {"KB", "MB", "GB", "TB", "KiB", "MiB", "GiB", "TiB"};
    for (size_t i = 0; i < sizeof units / sizeof *units; i++) {
        size_t n = strlen(units[i]);
        if (!strncasecmp(unit, units[i], n) && !isalnum((unsigned char)unit[n])) return true;
    }
    return false;
}

static bool find_version(const char *text, bool filename, bool display, char *out, size_t cap) {
    enum { BARE_FILENAME = 1, PADDED_FILENAME, MARKED_VERSION, TITLE_VERSION, MARKED_TITLE_VERSION };
    const char *id = pspkg_title_id(text);
    unsigned best = 0;
    for (const char *start = text; *start; start++) {
        if (start > text && isalnum((unsigned char)start[-1])) continue;
        bool prefixed = *start == 'v' || *start == 'V';
        const char *number = start + prefixed;
        if (!isdigit((unsigned char)*number)) continue;

        const char *end = number;
        size_t width[3] = {0}, parts = 0;
        do {
            if (parts) end++;
            size_t digits = strspn(end, "0123456789");
            if (parts < 3) width[parts] = digits;
            parts++;
            end += digits;
        } while (*end == '.' && isdigit((unsigned char)end[1]) &&
                 !(filename && !strcasecmp(end, ".7z")));
        const char *token = start;
        // Skip the whole numeric token, including invalid versions and title years.
        start = end - 1;
        if (isalnum((unsigned char)*end) || version_size_suffix(end)) continue;

        bool adjacent = parts > 1 && version_next_to_id(token, end, id);
        bool extended = parts > 3 || width[0] > 3 || width[1] > 3 || width[2] > 3;
        // Longer values need a version prefix or a neighboring title ID.
        if (extended && !(display && parts > 1 && (prefixed || adjacent))) continue;
        unsigned rank = 0;
        if (adjacent) rank = prefixed ? MARKED_TITLE_VERSION : TITLE_VERSION;
        else if (prefixed) rank = MARKED_VERSION;
        else if (filename && parts > 1 && width[0] <= 2)
            rank = width[0] == 2 && width[1] == 3 ? PADDED_FILENAME : BARE_FILENAME;
        if (rank <= best) continue;

        if (!display) {
            // Keep the existing version string used to identify queued choices.
            while (*number == '0' && isdigit((unsigned char)number[1])) number++;
            if (parts == 2 && width[1] == 3 && end[-1] == '0') end--;
        }
        size_t length = (size_t)(end - number);
        if (length >= cap) continue;
        memcpy(out, number, length);
        out[length] = 0;
        best = rank;
    }
    return best > 0;
}

static void read_version(const char *edition, const char *filename, bool display, char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!find_version(edition, false, display, out, cap)) find_version(filename, true, display, out, cap);
}

void pspkg_version(const char *edition, const char *filename, char *out, size_t cap) {
    read_version(edition, filename, false, out, cap);
}

void pspkg_display_version(const char *edition, const char *filename, char *out, size_t cap) {
    read_version(edition, filename, true, out, cap);
}

/* Compare numeric components without truncating long version strings. */
int pspkg_compare_versions(const char *a, const char *b) {
    if (!*a || !*b) return !!*a - !!*b;
    while (*a || *b) {
        a += strspn(a, "0");
        b += strspn(b, "0");
        size_t a_len = strspn(a, "0123456789"), b_len = strspn(b, "0123456789");
        if (a_len != b_len) return a_len > b_len ? 1 : -1;
        int order = strncmp(a, b, a_len);
        if (order) return order;
        a += a_len;
        b += b_len;
        if (*a && *a != '.') return 0;
        if (*b && *b != '.') return 0;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

static size_t read_patch_components(const char *text, unsigned values[3], size_t widths[3]) {
    for (size_t i = 0; i < 3; i++) {
        widths[i] = strspn(text, "0123456789");
        if (!widths[i] || widths[i] > 3) return 0;
        for (size_t digit = 0; digit < widths[i]; digit++)
            values[i] = values[i] * 10 + (unsigned)(text[digit] - '0');
        text += widths[i];
        if (!*text) return i + 1;
        if (*text++ != '.') return 0;
    }
    return 0;
}

bool pspkg_patch_matches(const char *version, const char *patch, bool ps5) {
    unsigned v[3] = {0}, p[3] = {0};
    size_t widths[3], patch_widths[3];
    size_t count = read_patch_components(version, v, widths);
    size_t patch_count = read_patch_components(patch, p, patch_widths);
    if (count < 2 || patch_count != (ps5 ? 3U : 2U) || v[0] != p[0]) return false;
    if (count == patch_count) return v[1] == p[1] && v[2] == p[2];
    if (!ps5 || count != 2) return false;
    /* PS5 filenames sometimes shorten 02.120 to 2.12. The caller requires a
     * unique match across patches before filling in the missing component. */
    unsigned shortened = v[1];
    for (size_t i = widths[1]; i < patch_widths[1]; i++) shortened *= 10;
    return v[1] == p[1] || shortened == p[1];
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
