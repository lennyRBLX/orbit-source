#include "remote_sources.h"
#include "provider_network.h"

#include <ctype.h>
#include <math.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define REMOTE_PACKAGE_LIMIT 2048
#define MAX_SAFE_BYTES 9007199254740991.0

static bool identifier(const char *id) {
    return *id && strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == strlen(id);
}

static bool filename_valid(const char *name) {
    return *name && name[0] != '.' && utf8_text(name) && !strpbrk(name, "/\\:") && !strstr(name, "..");
}

static bool field(const cJSON *entry, const char *key, char *out, size_t cap) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, key);
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= cap || !utf8_text(value->valuestring)) return false;
    copy_text(out, cap, value->valuestring);
    return true;
}

static bool bytes(const cJSON *entry, const char *key, int64_t *out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, key);
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsNumber(value) || !(value->valuedouble >= 0 && value->valuedouble <= MAX_SAFE_BYTES) ||
        floor(value->valuedouble) != value->valuedouble) return false;
    *out = (int64_t)value->valuedouble;
    return true;
}

static const char *compression(const char *suffix) {
    if (!strcasecmp(suffix, ".zip")) return "zip";
    if (!strcasecmp(suffix, ".7z")) return "7z";
    if (!strcasecmp(suffix, ".rar")) return "rar";
    return "";
}

/* Include the full source id in the digest even when its displayed prefix is shortened. */
static void identify(Release *r) {
    char identity[sizeof r->remote_catalog_id + sizeof r->url + sizeof r->sha256 +
                  sizeof r->filename + sizeof r->title_id];
    if (r->remote_page)
        snprintf(identity, sizeof identity, "%s/%s\n%s", r->remote_catalog_id, r->browser_url, r->title_id);
    else
        snprintf(identity, sizeof identity, "%s/%s\n%s\n%s\n%s",
                 r->remote_catalog_id, r->url, r->filename, r->title_id, r->sha256);
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)identity, strlen(identity), hash);
    snprintf(r->id, sizeof r->id, "rs-%.6s-%02x%02x%02x%02x%02x%02x",
             r->remote_catalog_id, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
    snprintf(r->game_id, sizeof r->game_id, "rs-%s-%s", r->remote_catalog_id, r->title_id);
}

static bool valid(const Release *r) {
    if (!identifier(r->remote_catalog_id) || !identifier(r->title_id) || !r->title[0] ||
        (!strcmp(r->platform, "ps4") && !strncmp(r->title_id, "PPSA", 4)) ||
        (!strcmp(r->platform, "ps5") && !strncmp(r->title_id, "CUSA", 4)) ||
        (strcmp(r->platform, "ps4") && strcmp(r->platform, "ps5")) ||
        !filename_valid(r->filename) || !remote_https_url(r->url) ||
        (r->browser_url[0] && !remote_https_url(r->browser_url)) || !orbit_format_allowed(r)) return false;
    const char *suffix = strrchr(r->filename, '.');
    if (!suffix || strcmp(compression(suffix), r->compression) ||
        (!r->compression[0] && (strcasecmp(suffix, ".pkg") && strcasecmp(suffix, ".fpkg"))) ||
        (!r->compression[0] && r->extracted_size)) return false;
    return !r->sha256[0] || (strlen(r->sha256) == 64 &&
        strspn(r->sha256, "0123456789abcdefABCDEF") == 64);
}

int remote_release_parse(const cJSON *entry, Release *r) {
    memset(r, 0, sizeof *r);
    if (!cJSON_IsObject(entry)) return -1;
#define FIELD(key, member) field(entry, key, r->member, sizeof r->member)
    const cJSON *page = cJSON_GetObjectItemCaseSensitive(entry, "remotePage");
    if (page && !cJSON_IsTrue(page)) return -1;
    r->remote_page = cJSON_IsTrue(page);
    if (!FIELD("sourceId", source) || strcmp(r->source, "remote") ||
        !FIELD("remoteCatalogId", remote_catalog_id) || !FIELD("title", title) ||
        !FIELD("titleId", title_id) || !FIELD("platform", platform) ||
        !FIELD("format", format) || !FIELD("filename", filename) || !FIELD("url", url) ||
        !FIELD("browserUrl", browser_url) || !FIELD("remoteLabel", remote_label) ||
        (r->remote_page && !*r->browser_url) || !FIELD("sha256", sha256) || !FIELD("compression", compression) ||
        !bytes(entry, "sizeBytes", &r->size) || !bytes(entry, "extractedBytes", &r->extracted_size) ||
        !valid(r)) return -1;
#undef FIELD
    identify(r);
    return strcmp(r->id, json_text(entry, "id")) || strcmp(r->game_id, json_text(entry, "gameId")) ? -1 : 0;
}

bool remote_url_filename(const char *url, char *name, size_t cap) {
    CURLU *parsed = curl_url();
    char *path = NULL;
    bool ok = parsed && !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
              !curl_url_get(parsed, CURLUPART_PATH, &path, 0);
    const char *last = ok ? strrchr(path, '/') : NULL;
    ok = last != NULL;
    if (ok) {
        int length = 0;
        char *decoded = curl_easy_unescape(NULL, last + 1, 0, &length);
        ok = decoded && length > 0 && (size_t)length < cap && (size_t)length == strlen(decoded);
        if (ok) copy_text(name, cap, decoded);
        curl_free(decoded);
    }
    curl_free(path);
    curl_url_cleanup(parsed);
    return ok;
}

bool remote_page_pending(const Release *r) {
    return r->remote_page && !r->url[0];
}

static bool token(const char *label, const char *word) {
    size_t length = strlen(word);
    for (const char *p = label; *p; p++)
        if ((p == label || !isalnum((unsigned char)p[-1])) &&
            !strncasecmp(p, word, length) && !isalnum((unsigned char)p[length])) return true;
    return false;
}

static bool unsupported_label(const char *label) {
    return token(label, "lz4") || token(label, "zstd") || token(label, "zst") ||
           token(label, "exfat") || token(label, "ffpfsc") || token(label, "pfs") ||
           token(label, "7z");
}

static const char *label_format(const char *label) {
    if (token(label, "fpkg")) return "FPKG";
    if (token(label, "pkg")) return "PKG";
    if (token(label, "folder") || token(label, "rar") || token(label, "zip")) return "Game folder";
    return "";
}

/* The payload filename determines the container. A link's PKG/FPKG label
 * describes what is inside a supported archive. Unlabeled archives are folders. */
int remote_file_format(Release *r, const char *name) {
    if (!filename_valid(name) || strlen(name) >= sizeof r->filename) return -1;
    if (unsupported_label(name)) return 1;
    const char *suffix = strrchr(name, '.');
    if (!suffix) return -1;
    const char *archive = compression(suffix);
    const char *excluded[] = {".7z", ".iso", ".img", ".tar", ".gz", ".bz2", ".xz", ".001", ".r00", ".rename"};
    for (size_t i = 0; i < sizeof excluded / sizeof *excluded; i++)
        if (!strcasecmp(suffix, excluded[i])) return 1;
    bool fpkg = !strcasecmp(suffix, ".fpkg");
    bool pkg = !strcasecmp(suffix, ".pkg");
    if (!*archive && !pkg && !fpkg) return -1;
    if (!strcasecmp(suffix, ".rar")) {
        const char *volume = suffix;
        while (volume > name && isdigit((unsigned char)volume[-1])) volume--;
        if (volume < suffix && volume - name >= 5 && !strncasecmp(volume - 5, ".part", 5)) return 1;
    }
    const char *format = "Game folder";
    if (fpkg || !strcmp(r->format, "FPKG")) format = "FPKG";
    else if (pkg || !strcmp(r->format, "PKG")) format = "PKG";
    memset(r->format, 0, sizeof r->format);
    copy_text(r->format, sizeof r->format, format);
    memset(r->compression, 0, sizeof r->compression);
    copy_text(r->compression, sizeof r->compression, archive);
    copy_text(r->filename, sizeof r->filename, name);
    return 0;
}

const char *remote_browser_provider(const char *url) {
    const char *viking[] = {"https://vikingfile.com/f/", "https://vik1ngfile.site/f/", "https://vik1ngf1le.online/f/"};
    for (size_t i = 0; i < sizeof viking / sizeof *viking; i++)
        if (!strncmp(url, viking[i], strlen(viking[i]))) return "vikingfile";
    if (!strncmp(url, "https://akirabox.com/", 21) || !strncmp(url, "https://akirabox.to/", 20))
        return strstr(url + 20, "/file") ? "akirabox" : "";
    if (!strncmp(url, "https://datanodes.to/", 21) && strcspn(url + 21, "/?#") == 12)
        return "datanodes";
    if (!strncmp(url, "https://1fichier.com/?", 22)) return "1fichier";
    return "";
}

static bool normalize_url(const char *raw, char *url, size_t cap) {
    CURLU *parts = curl_url();
    char *normalized = NULL;
    bool ok = parts && !strpbrk(raw, "\\#\t\r\n") &&
              !curl_url_set(parts, CURLUPART_URL, raw, CURLU_DISALLOW_USER | CURLU_ALLOW_SPACE | CURLU_URLENCODE) &&
              !curl_url_get(parts, CURLUPART_URL, &normalized, 0) &&
              strlen(normalized) < cap && remote_https_url(normalized);
    if (ok) copy_text(url, cap, normalized);
    curl_free(normalized);
    curl_url_cleanup(parts);
    return ok;
}

/* Returns 1 for an excluded format and -1 when an option cannot be interpreted. */
static int map_link(const cJSON *entry, const cJSON *link, const char *source_id, Release *r) {
    memset(r, 0, sizeof *r);
    if (!cJSON_IsObject(entry) || !cJSON_IsObject(link) || !identifier(source_id) ||
        strlen(source_id) >= sizeof r->remote_catalog_id) return -1;
    copy_text(r->remote_catalog_id, sizeof r->remote_catalog_id, source_id);
    copy_text(r->source, sizeof r->source, "remote");

    const char *label = json_text(link, "name"), *kind = json_text(link, "format");
    const char *format = json_text(entry, "format"), *category = json_text(entry, "category");
    if (unsupported_label(label) || unsupported_label(kind)) return 1;
    const char *hint = label_format(kind);
    if (!*hint) hint = label_format(label);
    if (!*hint) {
        if (unsupported_label(format) || unsupported_label(category)) return 1;
        hint = label_format(*format ? format : category);
        if (*format && !*hint) return 1;
    }
    copy_text(r->format, sizeof r->format, hint);
    if (*label && strlen(label) < sizeof r->remote_label && utf8_text(label))
        copy_text(r->remote_label, sizeof r->remote_label, label);

    char filename[sizeof r->filename] = "", path_name[sizeof r->filename] = "";
    if (!field(entry, "title", r->title, sizeof r->title) ||
        !field(entry, "titleId", r->title_id, sizeof r->title_id) ||
        !r->title[0] || !identifier(r->title_id) ||
        !field(link, "filename", filename, sizeof filename) ||
        (!*filename && !field(entry, "filename", filename, sizeof filename)) ||
        (*filename && !filename_valid(filename))) return -1;
    const char *platform = json_text(entry, "platform");
    if ((*platform && strcasecmp(platform, "PS4") && strcasecmp(platform, "PS5")) ||
        (!strcasecmp(platform, "PS4") && !strncmp(r->title_id, "PPSA", 4)) ||
        (!strcasecmp(platform, "PS5") && !strncmp(r->title_id, "CUSA", 4))) return -1;
    copy_text(r->platform, sizeof r->platform,
              !strcasecmp(platform, "PS4") || (!*platform && !strncmp(r->title_id, "CUSA", 4)) ? "ps4" : "ps5");

    const char *url = json_text(link, "url");
    if (strchr(url, '#')) return 1; /* Browser-only containers need a provider adapter. */
    if (!normalize_url(url, r->url, sizeof r->url)) return -1;
    bool has_path = remote_url_filename(r->url, path_name, sizeof path_name);
    if (has_path && unsupported_label(path_name)) return 1;
    if (!strcmp(r->url, "https://datanodes.to/download")) return 1;
    bool hosted = *remote_browser_provider(r->url) ||
                  !strncmp(r->url, "https://www.mediafire.com/file/", 31);
    int result = remote_file_format(r, *filename ? filename : has_path ? path_name : "");
    if (result > 0) return result;
    if (result < 0 || hosted) {
        /* Opaque routes are resolved only after the option is selected. */
        if ((*filename && result < 0) || strlen(r->url) >= sizeof r->browser_url) return -1;
        copy_text(r->browser_url, sizeof r->browser_url, r->url);
        memset(r->url, 0, sizeof r->url);
        memset(r->filename, 0, sizeof r->filename);
        r->remote_page = true;
        if (!r->format[0]) copy_text(r->format, sizeof r->format, "Unknown");
    } else {
        const cJSON *file = *filename ? entry : link;
        if (!field(file, "checksum", r->sha256, sizeof r->sha256) ||
            (!r->sha256[0] && !field(file, "sha256", r->sha256, sizeof r->sha256)) ||
            !bytes(file, "sizeBytes", &r->size) || !bytes(file, "extractedBytes", &r->extracted_size))
            return -1;
        const char *page = json_text(entry, "downloadSource");
        if (*page && strlen(page) < sizeof r->browser_url && remote_https_url(page))
            copy_text(r->browser_url, sizeof r->browser_url, page);
        if (!valid(r)) return -1;
        for (char *p = r->sha256; *p; p++)
            if (*p >= 'A' && *p <= 'F') *p += 'a' - 'A';
    }
    identify(r);
    return 0;
}

int pegasus_entry_to_release(const cJSON *entry, const char *source_id, Release *r) {
    const cJSON *links = cJSON_GetObjectItemCaseSensitive(entry, "downloadLinks"), *link;
    if (!cJSON_IsArray(links)) return -1;
    bool unsupported = false;
    cJSON_ArrayForEach(link, links) {
        int result = map_link(entry, link, source_id, r);
        if (result == 0) return 0;
        if (result > 0) unsupported = true;
    }
    return unsupported ? 1 : -1;
}

static int compare_releases(const void *a, const void *b) {
    return strcmp(((const Release *)a)->id, ((const Release *)b)->id);
}

static int append_option(Release **releases, size_t *count, size_t *capacity, const Release *r) {
    if (*count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 16;
        Release *next = realloc(*releases, *capacity * sizeof *next);
        if (!next) return -1;
        *releases = next;
    }
    (*releases)[(*count)++] = *r;
    return 0;
}

int remote_catalog_parse(const char *body, size_t length, const char *source_id,
                         Release **releases, size_t *count, size_t *skipped) {
    *releases = NULL;
    *count = *skipped = 0;
    cJSON *root = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    const cJSON *packages = cJSON_GetObjectItemCaseSensitive(root, "packages"), *entry;
    if (!cJSON_IsArray(packages) || memchr(body, 0, length)) {
        cJSON_Delete(root);
        return -1;
    }
    size_t capacity = 0, package_count = (size_t)cJSON_GetArraySize(packages);
    struct { const cJSON *next, *first; } *remaining =
        calloc(package_count ? package_count : 1, sizeof *remaining);
    if (!remaining) goto failed;
    size_t i = 0;
    cJSON_ArrayForEach(entry, packages) {
        const cJSON *links = cJSON_GetObjectItemCaseSensitive(entry, "downloadLinks"), *link;
        bool unsupported = false;
        if (cJSON_IsArray(links) && *count < REMOTE_PACKAGE_LIMIT) {
            Release first;
            int best = -1;
            cJSON_ArrayForEach(link, links) {
                Release r;
                int result = map_link(entry, link, source_id, &r);
                if (result > 0) unsupported = true;
                if (result) continue;
                int rank = !remote_page_pending(&r) ? 2 : !strcmp(r.format, "Unknown") ? 0 : 1;
                if (rank > best) {
                    best = rank;
                    first = r;
                    remaining[i].first = link;
                }
                if (rank == 2) break;
            }
            if (best >= 0) {
                if (append_option(releases, count, &capacity, &first)) goto failed;
                remaining[i].next = links->child;
            } else if (!unsupported) (*skipped)++;
        } else (*skipped)++;
        i++;
    }
    /* Visit the mirrors in rounds, so the largest entries cannot use the
     * source limit before the other titles get a second choice. */
    while (*count < REMOTE_PACKAGE_LIMIT) {
        bool more = false;
        i = 0;
        cJSON_ArrayForEach(entry, packages) {
            const cJSON *link = remaining[i].next;
            for (; link; link = link->next) {
                Release r;
                if (link == remaining[i].first || map_link(entry, link, source_id, &r)) continue;
                if (append_option(releases, count, &capacity, &r)) goto failed;
                link = link->next;
                break;
            }
            remaining[i++].next = link;
            more |= link != NULL;
            if (*count == REMOTE_PACKAGE_LIMIT) break;
        }
        if (!more) break;
    }
    free(remaining);
    cJSON_Delete(root);
    if (*count > 1) qsort(*releases, *count, sizeof **releases, compare_releases);
    size_t unique = 0;
    for (i = 0; i < *count; i++) {
        if (unique && !strcmp((*releases)[unique - 1].id, (*releases)[i].id)) continue;
        (*releases)[unique++] = (*releases)[i];
    }
    *count = unique;
    return 0;
failed:
    free(remaining);
    free(*releases);
    *releases = NULL;
    cJSON_Delete(root);
    return -1;
}
