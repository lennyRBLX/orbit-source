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
#define REMOTE_MIRROR_RESERVE 2048
#define REMOTE_SCAN_LIMIT (3 * REMOTE_PACKAGE_LIMIT)
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

static int file_format(Release *r, const char *name, bool community, const char *detected);

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
        ((r->browser_url[0] || r->remote_page) && !remote_https_url(r->browser_url)) ||
        ((!r->remote_page || r->url[0]) && !remote_https_url(r->url))) return false;
    if (remote_page_pending(r) && !r->filename[0])
        return (!r->compression[0] || !strcmp(r->compression, "zip") || !strcmp(r->compression, "rar")) &&
            !r->extracted_size && !r->sha256[0] &&
            (!strcmp(r->format, "Unknown") || !strcmp(r->format, "Game folder") ||
             !strcmp(r->format, "FPKG") || !strcmp(r->format, "PKG"));
    if (!filename_valid(r->filename) ||
        (strcmp(r->format, "PKG") && strcmp(r->format, "FPKG") &&
         (strcmp(r->format, "Game folder") || !r->compression[0]))) return false;
    if (r->remote_page) {
        Release checked = *r;
        const char *suffix = !strcmp(r->compression, "zip") ? ".zip" :
                             !strcmp(r->compression, "rar") ? ".rar" : !r->compression[0] ? ".pkg" : NULL;
        if (!suffix || file_format(&checked, r->filename, true, suffix) ||
            strcmp(checked.compression, r->compression) || strcmp(checked.format, r->format)) return false;
    } else {
        const char *suffix = strrchr(r->filename, '.');
        if (!suffix || strcmp(compression(suffix), r->compression) ||
            (!r->compression[0] && strcasecmp(suffix, ".pkg") && strcasecmp(suffix, ".fpkg"))) return false;
    }
    if (!r->compression[0] && r->extracted_size) return false;
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
        !FIELD("mirrorKey", mirror_key) || (r->mirror_key[0] &&
            (strlen(r->mirror_key) != 64 || strspn(r->mirror_key, "0123456789abcdef") != 64)) ||
        !FIELD("titleId", title_id) || !FIELD("platform", platform) ||
        !FIELD("format", format) || !FIELD("filename", filename) || !FIELD("url", url) ||
        !FIELD("browserUrl", browser_url) || !FIELD("remoteLabel", remote_label) ||
        (r->remote_page && !*r->browser_url) || !FIELD("sha256", sha256) || !FIELD("compression", compression) ||
        !bytes(entry, "sizeBytes", &r->size) || !bytes(entry, "extractedBytes", &r->extracted_size) ||
        !bytes(entry, "sizeEstimateBytes", &r->display_size) ||
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

bool job_needs_browser(const Release *r) {
    return remote_page_pending(r) || (!provider_supported(r) && provider_browser_supported(r));
}

/* Expose the provider domain without returning the private catalogue URL. */
static bool url_host(const char *url, char *host, size_t cap) {
    CURLU *parts = curl_url();
    char *value = NULL;
    bool ok = parts && !curl_url_set(parts, CURLUPART_URL, url, CURLU_DISALLOW_USER) &&
        !curl_url_get(parts, CURLUPART_HOST, &value, 0);
    if (ok) {
        const char *name = !strncasecmp(value, "www.", 4) ? value + 4 : value;
        ok = *name && strlen(name) < cap;
        if (ok) {
            copy_text(host, cap, name);
            for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
        }
    }
    curl_free(value);
    curl_url_cleanup(parts);
    return ok;
}

bool remote_link_host(const Release *r, char *host, size_t cap) {
    return url_host(provider_torbox_url(r), host, cap);
}

bool remote_metadata_url(const char *url) {
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *path = strchr(url + 17, '/');
        if (path && !strncmp(path, "/metadata/", 10)) return true;
    }
#endif
    const char *hosts[] = {"akirabox.com", "akirabox.to", "rootz.so", "vikingfile.com",
        "vik1ngfile.site", "vik1ngf1le.online", "gofile.io", "datanodes.to", "datanodes.co", "filekeeper.net", "1fichier.com"};
    char host[254];
    if (!url_host(url, host, sizeof host)) return false;
    for (size_t i = 0; i < sizeof hosts / sizeof *hosts; i++)
        if (!strcmp(host, hosts[i])) return true;
    return false;
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
static int file_format(Release *r, const char *name, bool community, const char *detected) {
    if (!filename_valid(name) || strlen(name) >= sizeof r->filename) return -1;
    if (community && unsupported_label(name)) return 1;
    const char *suffix = strrchr(name, '.');
    if (community && suffix && !strcasecmp(suffix, ".7z")) return 1;
    const char *excluded[] = {".iso", ".img", ".tar", ".gz", ".bz2", ".xz", ".001", ".r00", ".rename"};
    for (size_t i = 0; i < sizeof excluded / sizeof *excluded; i++)
        if (suffix && !strcasecmp(suffix, excluded[i])) return 1;
    if (suffix && !strcasecmp(suffix, ".rar")) {
        const char *volume = suffix;
        while (volume > name && isdigit((unsigned char)volume[-1])) volume--;
        if (volume < suffix && volume - name >= 5 && !strncasecmp(volume - 5, ".part", 5)) return 1;
    }
    if (!suffix || (!*compression(suffix) && strcasecmp(suffix, ".pkg") && strcasecmp(suffix, ".fpkg")))
        suffix = detected;
    if (!suffix) return -1;
    const char *archive = compression(suffix);
    if (community && !strcmp(archive, "7z")) return 1;
    bool fpkg = !strcasecmp(suffix, ".fpkg");
    bool pkg = !strcasecmp(suffix, ".pkg");
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

int remote_file_format(Release *r, const char *name) { return file_format(r, name, true, NULL); }

int remote_file_detected_format(Release *r, const char *name, const char *suffix) {
    return file_format(r, name, true, suffix);
}

int direct_file_format(Release *r, const char *name) {
    int result = file_format(r, name, false, NULL);
    if (!result) {
        copy_text(r->title, sizeof r->title, name);
        for (size_t n = strlen(r->title); n && !utf8_text(r->title);) r->title[--n] = 0;
    }
    return result;
}

bool direct_release_init(Release *r, const char *url) {
    memset(r, 0, sizeof *r);
    if (!url || strlen(url) >= sizeof r->url || !direct_http_url(url)) return false;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)url, strlen(url), hash);
    snprintf(r->id, sizeof r->id, "url-%02x%02x%02x%02x%02x%02x%02x%02x%02x",
             hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8]);
    copy_text(r->source, sizeof r->source, ORBIT_DIRECT_SOURCE);
    copy_text(r->platform, sizeof r->platform, "ps5");
    copy_text(r->url, sizeof r->url, url);
    return true;
}

int direct_release_parse(const cJSON *entry, Release *r) {
    if (!direct_release_init(r, json_text(entry, "url")) ||
        direct_file_format(r, json_text(entry, "filename")) ||
        !bytes(entry, "sizeBytes", &r->size) || r->size < 8 ||
        strcmp(r->id, json_text(entry, "id")) || strcmp(r->format, json_text(entry, "format")) ||
        strcmp(r->compression, json_text(entry, "compression"))) return -1;
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
        if (*format && !*hint && strcasecmp(format, "Unknown") && strcasecmp(format, "Unknown file type")) return 1;
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
    bool hosted = remote_metadata_url(r->url) || *remote_browser_provider(r->url) ||
                  !strncmp(r->url, "https://www.mediafire.com/file/", 31);
    int result = remote_file_format(r, *filename ? filename : has_path ? path_name : "");
    if (result > 0) return result;
    if (result < 0 || hosted) {
        /* Opaque routes are resolved only after the option is selected. */
        if (strlen(r->url) >= sizeof r->browser_url) return -1;
        copy_text(r->browser_url, sizeof r->browser_url, r->url);
        memset(r->url, 0, sizeof r->url);
        memset(r->filename, 0, sizeof r->filename);
        r->remote_page = true;
        if (!bytes(link, "sizeBytes", &r->display_size) ||
            (!r->display_size && !bytes(entry, "sizeBytes", &r->display_size))) return -1;
        if (!r->format[0]) copy_text(r->format, sizeof r->format, "Unknown");
    } else {
        const cJSON *file = *filename ? entry : link;
        if (!field(file, "checksum", r->sha256, sizeof r->sha256) ||
            (!r->sha256[0] && !field(file, "sha256", r->sha256, sizeof r->sha256)) ||
            !bytes(file, "sizeBytes", &r->size) || !bytes(file, "extractedBytes", &r->extracted_size))
            return -1;
        if (!r->size && !bytes(entry, "sizeBytes", &r->display_size)) return -1;
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

static void mirror_hash(Release *r, const char *identity) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)identity, strlen(identity), hash);
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof hash; i++) {
        r->mirror_key[2 * i] = hex[hash[i] >> 4];
        r->mirror_key[2 * i + 1] = hex[hash[i] & 15];
    }
    r->mirror_key[2 * sizeof hash] = 0;
}

/* Only remove a provider name that agrees with the actual URL. Preserve
 * edition, region, version, DLC, and explicitly combined variants. */
static const struct {
    const char *domains[4], *labels[6];
} mirror_providers[] = {
    {{"akirabox.com", "akirabox.to"}, {"akirabox com", "akirabox to", "akira box", "akirabox"}},
    {{"vikingfile.com", "vik1ngfile.site", "vik1ngf1le.online"},
        {"vikingfile com", "vik1ngfile site", "vik1ngf1le online", "vikingfile", "viking", "viki"}},
    {{"datanodes.to", "datanodes.co"}, {"datanodes to", "datanodes co", "data nodes", "datanodes", "data"}},
    {{"rootz.so"}, {"rootz so", "rootz"}},
    {{"gofile.io"}, {"gofile io", "gofile"}},
    {{"filekeeper.net"}, {"filekeeper net", "file keeper", "filekeeper"}},
    {{"1fichier.com"}, {"1fichier com", "1fichier"}},
    {{"archive.org"}, {"archive org", "archive"}},
    {{"mediafire.com"}, {"mediafire com", "mediafire"}},
};

static int mirror_variant(const Release *r, char *out, size_t cap) {
    char host[254];
    if (!remote_link_host(r, host, sizeof host)) return -1;
    size_t provider = 0;
    for (; provider < sizeof mirror_providers / sizeof *mirror_providers; provider++) {
        const char *const *domains = mirror_providers[provider].domains;
        size_t i = 0;
        while (i < sizeof mirror_providers[provider].domains / sizeof *domains && domains[i] &&
               strcmp(host, domains[i])) i++;
        if (i < sizeof mirror_providers[provider].domains / sizeof *domains && domains[i]) break;
    }
    if (provider == sizeof mirror_providers / sizeof *mirror_providers) return -1;

    size_t used = 0;
    bool space = false;
    for (const unsigned char *p = (const unsigned char *)r->remote_label; *p; p++) {
        if (!strncmp((const char *)p, "·", 2)) { p++; space = true; continue; }
        if (!strncmp((const char *)p, "—", 3) || !strncmp((const char *)p, "–", 3)) {
            p += 2;
            space = true;
            continue;
        }
        if (*p < 128 && !isalnum(*p) && *p != '+') { space = true; continue; }
        if (used + (space && used ? 2 : 1) >= cap) return -1;
        if (space && used) out[used++] = ' ';
        out[used++] = (char)tolower(*p);
        space = false;
    }
    out[used] = 0;
    const char *const *labels = mirror_providers[provider].labels;
    for (size_t i = 0; i < sizeof mirror_providers[provider].labels / sizeof *labels && labels[i]; i++) {
        size_t n = strlen(labels[i]);
        for (char *p = out; (p = strstr(p, labels[i])); p += n) {
            if ((p != out && p[-1] != ' ') || (p[n] && p[n] != ' ')) continue;
            char *end = p + n;
            if (*end == ' ') end++;
            else if (p != out) p--;
            memmove(p, end, strlen(end) + 1);
            return (int)provider;
        }
    }
    return -1;
}

typedef struct { size_t package; const cJSON *link; } OptionOrigin;
typedef struct {
    Release *release;
    OptionOrigin origin;
    char variant[128];
    unsigned provider;
} MirrorOption;

static int compare_mirror_options(const void *a, const void *b) {
    const MirrorOption *x = a, *y = b;
    if (x->origin.package != y->origin.package)
        return x->origin.package < y->origin.package ? -1 : 1;
    int variant = strcmp(x->variant, y->variant);
    return variant ? variant : strcmp(x->release->id, y->release->id);
}

static void group_package_mirrors(MirrorOption *rows, size_t count) {
    char identity[sizeof mirror_providers / sizeof *mirror_providers * 24 + 16] = "package/";
    size_t used = strlen(identity);
    const Release *first = rows[0].release;
    unsigned providers = 0;
    int64_t size = 0;
    const char *name = "", *compression = "";
    for (size_t i = 0; i < count; i++) {
        const Release *r = rows[i].release;
        if (i && !strcmp(rows[i - 1].release->id, r->id)) continue;
        unsigned provider = 1u << rows[i].provider;
        const char *filename = json_text(rows[i].origin.link, "filename");
        if ((providers & provider) || strcmp(first->format, r->format) ||
            (size && r->display_size && size != r->display_size) ||
            (*compression && r->compression[0] && strcmp(compression, r->compression)) ||
            (*name && *filename && strcmp(name, filename))) return;
        if (r->display_size) size = r->display_size;
        if (r->compression[0]) compression = r->compression;
        if (*filename) name = filename;
        providers |= provider;
        used += (size_t)snprintf(identity + used, sizeof identity - used, "%s/", r->id);
    }
    if (!(providers & (providers - 1))) return;
    for (size_t i = 0; i < count; i++) mirror_hash(rows[i].release, identity);
}

static void identify_mirrors(Release *releases, const OptionOrigin *origin, size_t count) {
    MirrorOption *opaque = calloc(count, sizeof *opaque);
    size_t pending = 0;
    for (size_t i = 0; i < count; i++) {
        Release *r = &releases[i];
        if (r->sha256[0]) {
            char identity[160];
            snprintf(identity, sizeof identity, "file/%s/%s/%s/%s/%lld",
                r->title_id, r->format, r->compression, r->sha256, (long long)r->size);
            mirror_hash(r, identity);
            continue;
        }
        const cJSON *link = origin[i].link;
        if (!opaque || !remote_page_pending(r) || cJSON_GetObjectItemCaseSensitive(link, "part") ||
            *json_text(link, "checksum") || *json_text(link, "sha256") || token(r->remote_label, "part")) continue;
        int provider = mirror_variant(r, opaque[pending].variant, sizeof opaque[pending].variant);
        if (provider < 0) continue;
        opaque[pending].release = r;
        opaque[pending].origin = origin[i];
        opaque[pending].provider = (unsigned)provider;
        pending++;
    }
    if (pending > 1) {
        qsort(opaque, pending, sizeof *opaque, compare_mirror_options);
        for (size_t i = 0; i < pending;) {
            size_t next = i + 1;
            while (next < pending && opaque[i].origin.package == opaque[next].origin.package &&
                   !strcmp(opaque[i].variant, opaque[next].variant)) next++;
            if (next - i > 1) group_package_mirrors(opaque + i, next - i);
            i = next;
        }
    }
    free(opaque);
}

typedef struct {
    const char *key;
    size_t option;
} RetainedMirror;

static int compare_retained_mirrors(const void *a, const void *b) {
    const RetainedMirror *x = a, *y = b;
    int order = strcmp(x->key, y->key);
    if (order || x->option == y->option) return order;
    return x->option < y->option ? -1 : 1;
}

/* The original cap still covers visible choices. A separate reserve keeps
 * one alternate per group across the catalogue before retaining second ones. */
static size_t retain_choices(Release *releases, size_t count) {
    if (count <= REMOTE_PACKAGE_LIMIT) return count;
    RetainedMirror *mirrors = malloc(count * sizeof *mirrors);
    struct Selection { size_t first, rank; bool keep; } *selection = calloc(count, sizeof *selection);
    if (!mirrors || !selection) {
        free(mirrors);
        free(selection);
        return REMOTE_PACKAGE_LIMIT;
    }
    size_t grouped = 0;
    for (size_t i = 0; i < count; i++) {
        selection[i].first = i;
        if (releases[i].mirror_key[0]) mirrors[grouped++] = (RetainedMirror){releases[i].mirror_key, i};
    }
    qsort(mirrors, grouped, sizeof *mirrors, compare_retained_mirrors);
    for (size_t i = 0, first = 0; i < grouped; i++) {
        if (i && strcmp(mirrors[first].key, mirrors[i].key)) first = i;
        selection[mirrors[i].option].first = mirrors[first].option;
        selection[mirrors[i].option].rank = i - first;
    }
    size_t visible = 0, alternate = 0;
    for (size_t i = 0; i < count && visible < REMOTE_PACKAGE_LIMIT; i++)
        if (!selection[i].rank) { selection[i].keep = true; visible++; }
    for (size_t rank = 1; rank <= 2 && alternate < REMOTE_MIRROR_RESERVE; rank++)
        for (size_t i = 0; i < count && alternate < REMOTE_MIRROR_RESERVE; i++)
            if (selection[i].rank == rank && selection[selection[i].first].keep) {
                selection[i].keep = true;
                alternate++;
            }
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        if (selection[i].keep) releases[kept++] = releases[i];
    free(selection);
    free(mirrors);
    return kept;
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
    OptionOrigin *origins = calloc(REMOTE_SCAN_LIMIT, sizeof *origins);
    if (!remaining || !origins) goto failed;
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
                origins[*count] = (OptionOrigin){i, remaining[i].first};
                if (append_option(releases, count, &capacity, &first)) goto failed;
                remaining[i].next = links->child;
            } else if (!unsupported) (*skipped)++;
        } else (*skipped)++;
        i++;
    }
    /* Visit the mirrors in rounds, so the largest entries cannot use the
     * source limit before the other titles get a second choice. */
    while (*count < REMOTE_SCAN_LIMIT) {
        bool more = false;
        i = 0;
        cJSON_ArrayForEach(entry, packages) {
            const cJSON *link = remaining[i].next;
            for (; link; link = link->next) {
                Release r;
                if (link == remaining[i].first || map_link(entry, link, source_id, &r)) continue;
                origins[*count] = (OptionOrigin){i, link};
                if (append_option(releases, count, &capacity, &r)) goto failed;
                link = link->next;
                break;
            }
            remaining[i++].next = link;
            more |= link != NULL;
            if (*count == REMOTE_SCAN_LIMIT) break;
        }
        if (!more) break;
    }
    identify_mirrors(*releases, origins, *count);
    *count = retain_choices(*releases, *count);
    free(origins);
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
    free(origins);
    free(remaining);
    free(*releases);
    *releases = NULL;
    cJSON_Delete(root);
    return -1;
}
