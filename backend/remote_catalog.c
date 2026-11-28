#include "remote_sources.h"
#include "provider_network.h"
#include "padrino.h"
#include "pspkg.h"

#include <ctype.h>
#include <math.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

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

static cJSON *padrino_snapshot;
static const cJSON **padrino_titles;
static size_t padrino_title_count;

static const char *pspkg_platform(const char *source_id) {
    if (!strcmp(source_id, PSPKG_PS5_SOURCE)) return "PS5";
    if (!strcmp(source_id, PSPKG_PS4_SOURCE)) return "PS4";
    return NULL;
}

/* The scanner publishes one feed containing both general sections and a
 * separate Arabic PS5 import. Match the original general listing URL so an
 * Arabic entry cannot enter either built-in source by its category alone. */
static bool source_package(const cJSON *entry, const char *source_id) {
    const char *platform = pspkg_platform(source_id);
    if (!platform) return true;
    const char *id = json_text(entry, "id");
    const char *description = json_text(entry, "description");
    if (strncmp(id, "pspkg-", 6) || !strncmp(id, "pspkg-arabic-", 13) ||
        strcmp(json_text(entry, "category"), platform)) return false;
#ifdef ORBIT_TEST
    const char *local = strstr(description, "\nSource: http://127.0.0.1:");
    if (local) {
        const char *path = strchr(local + strlen("\nSource: http://127.0.0.1:"), '/');
        if (path && !strncasecmp(path + 1, platform, 3) && path[4] == '/') return true;
    }
#endif
    return strstr(description, !strcmp(platform, "PS5")
        ? "\nSource: https://pspkg.com/ps5/" : "\nSource: https://pspkg.com/ps4/") != NULL;
}

static bool image_format(const Release *r) {
    return !strcmp(r->format, "exFAT") || !strcmp(r->format, "FFPFSC");
}

static int file_format(Release *r, const char *name, bool community, const char *detected);

/* Include the full source id in the digest even when its displayed prefix is shortened. */
static void identify(Release *r) {
    char identity[sizeof r->remote_catalog_id + sizeof r->url + sizeof r->sha256 +
                  sizeof r->filename + sizeof r->title_id + sizeof r->remote_version + sizeof r->remote_region + sizeof r->remote_edition + sizeof r->remote_choice +
                  sizeof r->remote_format_hint + sizeof r->remote_compression_hint + 24];
    if (r->remote_page)
        snprintf(identity, sizeof identity, "%s/%s\n%s", r->remote_catalog_id, r->browser_url, r->title_id);
    else
        snprintf(identity, sizeof identity, "%s/%s\n%s\n%s\n%s",
                 r->remote_catalog_id, r->url, r->filename, r->title_id, r->sha256);
    if (r->remote_edition[0] || r->remote_version[0] || r->remote_region[0]) {
        size_t used = strlen(identity);
        used += (size_t)snprintf(identity + used, sizeof identity - used, "\n%s\n%s", r->remote_version, r->remote_region);
        if (r->remote_edition[0] && used < sizeof identity)
            snprintf(identity + used, sizeof identity - used, "\n%s", r->remote_edition);
    }
    if (r->remote_format_hint[0] || r->remote_compression_hint[0]) {
        size_t used = strlen(identity);
        snprintf(identity + used, sizeof identity - used, "\n%s\n%s",
                 r->remote_format_hint, r->remote_compression_hint);
    }
    if (r->remote_choice[0]) {
        size_t used = strlen(identity);
        snprintf(identity + used, sizeof identity - used, "\n%s", r->remote_choice);
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)identity, strlen(identity), hash);
    snprintf(r->id, sizeof r->id, "rs-%.6s-%02x%02x%02x%02x%02x%02x",
             r->remote_catalog_id, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
    snprintf(r->game_id, sizeof r->game_id, "rs-%s-%s", r->remote_catalog_id, r->title_id);
}

void remote_pspkg_identify_choice(Release *r, const char *url, size_t index) {
    char key[2304];
    snprintf(key, sizeof key, "%s\n%s\n%zu", r->id, url, index);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)key, strlen(key), digest);
    for (size_t i = 0; i < sizeof digest; i++)
        snprintf(r->remote_choice + 2 * i, 3, "%02x", digest[i]);
    identify(r);
}

static bool valid(const Release *r) {
    if (!identifier(r->remote_catalog_id) || !identifier(r->title_id) || !r->title[0] ||
        (!strcmp(r->platform, "ps4") && !strncmp(r->title_id, "PPSA", 4)) ||
        (!strcmp(r->platform, "ps5") && !strncmp(r->title_id, "CUSA", 4)) ||
        (strcmp(r->platform, "ps4") && strcmp(r->platform, "ps5")) ||
        ((r->browser_url[0] || r->remote_page) && !remote_https_url(r->browser_url)) ||
        ((!r->remote_page || r->url[0]) && !remote_https_url(r->url))) return false;
    if (r->remote_choice[0] && (!r->remote_page || !pspkg_platform(r->remote_catalog_id) ||
        strlen(r->remote_choice) != 64 || strspn(r->remote_choice, "0123456789abcdef") != 64)) return false;
    if (r->remote_provider_page[0]) {
        const char *provider = remote_browser_provider(r->remote_provider_page);
        if (!r->remote_page || !pspkg_platform(r->remote_catalog_id) ||
            strcmp(remote_browser_provider(r->browser_url), "pspkg") ||
            !*provider || !strcmp(provider, "pspkg")) return false;
    }
    if (r->archive_password[0] && (!remote_page_pending(r) && !r->compression[0]))
        return false;
    if (remote_page_pending(r) && !r->filename[0])
        return (!r->compression[0] || !strcmp(r->compression, "zip") || !strcmp(r->compression, "rar") ||
                !strcmp(r->compression, "7z")) &&
            !r->extracted_size && !r->sha256[0] &&
            (!strcmp(r->format, "Unknown") || !strcmp(r->format, "Game folder") ||
             !strcmp(r->format, "FPKG") || !strcmp(r->format, "PKG") || image_format(r));
    if (!filename_valid(r->filename) ||
        (strcmp(r->format, "PKG") && strcmp(r->format, "FPKG") && !image_format(r) &&
         (strcmp(r->format, "Game folder") || !r->compression[0]))) return false;
    if (r->remote_page) {
        Release checked = *r;
        const char *suffix = !strcmp(r->compression, "zip") ? ".zip" :
                             !strcmp(r->compression, "rar") ? ".rar" :
                             !strcmp(r->compression, "7z") ? ".7z" :
                             !r->compression[0] ? (!strcmp(r->format, "exFAT") ? ".exfat" :
                                                   !strcmp(r->format, "FFPFSC") ? ".ffpfsc" : ".pkg") : NULL;
        if (!suffix || file_format(&checked, r->filename, true, suffix) ||
            strcmp(checked.compression, r->compression) || strcmp(checked.format, r->format)) return false;
    } else {
        const char *suffix = strrchr(r->filename, '.');
        if (!suffix || strcmp(compression(suffix), r->compression) ||
            (!r->compression[0] && strcasecmp(suffix, ".pkg") && strcasecmp(suffix, ".fpkg") &&
             !(image_format(r) && !strcasecmp(suffix + 1, r->format)))) return false;
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
    r->remote_filename_synthetic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "syntheticFilename"));
    if (!FIELD("sourceId", source) || strcmp(r->source, "remote") ||
        !FIELD("remoteCatalogId", remote_catalog_id) || !FIELD("title", title) ||
        !FIELD("mirrorKey", mirror_key) || (r->mirror_key[0] &&
            (strlen(r->mirror_key) != 64 || strspn(r->mirror_key, "0123456789abcdef") != 64)) ||
        !FIELD("titleId", title_id) || !FIELD("platform", platform) ||
        !FIELD("format", format) || !FIELD("filename", filename) || !FIELD("url", url) ||
        !FIELD("browserUrl", browser_url) || !FIELD("remoteLabel", remote_label) ||
        !FIELD("providerPage", remote_provider_page) || !FIELD("sourceChoice", remote_choice) ||
        !FIELD("version", remote_version) || !FIELD("region", remote_region) || !FIELD("edition", remote_edition) ||
        !FIELD("formatHint", remote_format_hint) || !FIELD("compressionHint", remote_compression_hint) ||
        !FIELD("filenameHint", remote_filename) || !FIELD("sourcePage", remote_source_page) ||
        (r->remote_page && !*r->browser_url) || !FIELD("sha256", sha256) || !FIELD("compression", compression) ||
        !FIELD("archivePassword", archive_password) ||
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

bool remote_archive_url(const char *url) {
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *path = strchr(url + 17, '/');
        return path && !strncmp(path, "/pspkg-provider/archive.org/", sizeof "/pspkg-provider/archive.org/" - 1);
    }
#endif
    char host[254];
    if (strncmp(url, "https://", 8) || !url_host(url, host, sizeof host)) return false;
    size_t length = strlen(host), suffix = sizeof ".archive.org" - 1;
    return !strcmp(host, "archive.org") || (length > suffix && !strcmp(host + length - suffix, ".archive.org"));
}

bool remote_link_host(const Release *r, char *host, size_t cap) {
    const char *url = provider_torbox_url(r);
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *path = strchr(url + 17, '/');
        static const char prefix[] = "/pspkg-provider/";
        if (path && !strncmp(path, prefix, sizeof prefix - 1)) {
            path += sizeof prefix - 1;
            size_t n = strcspn(path, "/");
            if (!n || n >= cap) return false;
            memcpy(host, path, n);
            host[n] = 0;
            return true;
        }
    }
#endif
    return url_host(url, host, cap);
}

bool remote_metadata_option(const Release *r) {
    return remote_https_url(provider_torbox_url(r));
}

static int mirror_provider(const char *host);

static bool hosted_page_hint(const char *url) {
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *path = strchr(url + 17, '/');
        if (path && !strncmp(path, "/metadata/", 10)) return true;
    }
#endif
    if (*remote_browser_provider(url)) return true;
    char host[254];
    if (!url_host(url, host, sizeof host)) return false;
    if (!strcmp(host, "archive.org")) return !strncmp(url, "https://archive.org/details/", 28);
    return mirror_provider(host) >= 0;
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
           token(label, "zstandard") || token(label, "pfs");
}

static bool multipart_label(const char *label) {
    for (const char *p = label; *p; p++) {
        if (p != label && isalnum((unsigned char)p[-1])) continue;
        size_t length = !strncasecmp(p, "part", 4) ? 4 : !strncasecmp(p, "volume", 6) ? 6 : 0;
        if (length && isdigit((unsigned char)p[length + strspn(p + length, " ._-")])) return true;
    }
    return false;
}

static const char *label_format(const char *label) {
    if (token(label, "fpkg")) return "FPKG";
    if (token(label, "pkg")) return "PKG";
    if (token(label, "exfat")) return "exFAT";
    if (token(label, "ffpfsc")) return "FFPFSC";
    if (token(label, "folder") || token(label, "rar") || token(label, "zip") ||
        token(label, "7z")) return "Game folder";
    return "";
}

/* The payload filename determines the container. A link's PKG/FPKG label
 * describes what is inside a supported archive. Unlabeled archives are folders. */
static int file_format(Release *r, const char *name, bool community, const char *detected) {
    if (!filename_valid(name) || strlen(name) >= sizeof r->filename) return -1;
    if (community && unsupported_label(name)) return 1;
    const char *suffix = strrchr(name, '.');
    if (community && suffix && ((strlen(suffix) == 4 && strspn(suffix + 1, "0123456789") == 3) ||
        (strlen(suffix) == 4 && strchr("rRzZ", suffix[1]) && strspn(suffix + 2, "0123456789") == 2))) return 2;
    const char *excluded[] = {".iso", ".img", ".tar", ".gz", ".bz2", ".xz", ".rename"};
    for (size_t i = 0; i < sizeof excluded / sizeof *excluded; i++)
        if (suffix && !strcasecmp(suffix, excluded[i])) return 1;
    if (suffix && !strcasecmp(suffix, ".rar")) {
        const char *volume = suffix;
        while (volume > name && isdigit((unsigned char)volume[-1])) volume--;
        if (volume < suffix && volume - name >= 5 && !strncasecmp(volume - 5, ".part", 5)) return 2;
    }
    if (!suffix || (!*compression(suffix) && strcasecmp(suffix, ".pkg") && strcasecmp(suffix, ".fpkg") &&
        strcasecmp(suffix, ".exfat") && strcasecmp(suffix, ".ffpfsc")))
        suffix = detected;
    if (!suffix || (!community && (!strcasecmp(suffix, ".exfat") || !strcasecmp(suffix, ".ffpfsc")))) return -1;
    const char *archive = compression(suffix);
    if (community && image_format(r) && strcasecmp(r->format, suffix + 1)) return 1;
    bool fpkg = !strcasecmp(suffix, ".fpkg");
    bool pkg = !strcasecmp(suffix, ".pkg");
    const char *format = "Game folder";
    if (community && !strcasecmp(suffix, ".exfat")) format = "exFAT";
    else if (community && !strcasecmp(suffix, ".ffpfsc")) format = "FFPFSC";
    else if (fpkg || !strcmp(r->format, "FPKG")) format = "FPKG";
    else if (pkg || !strcmp(r->format, "PKG")) format = "PKG";
    memset(r->format, 0, sizeof r->format);
    copy_text(r->format, sizeof r->format, format);
    memset(r->compression, 0, sizeof r->compression);
    copy_text(r->compression, sizeof r->compression, archive);
    if (!*archive) memset(r->archive_password, 0, sizeof r->archive_password);
    copy_text(r->filename, sizeof r->filename, name);
    return 0;
}

int remote_file_format(Release *r, const char *name) { return file_format(r, name, true, NULL); }

int remote_file_detected_format(Release *r, const char *name, const char *suffix) {
    return file_format(r, name, true, suffix);
}

int remote_provider_file_format(Release *r, const char *name, const char *detected) {
    Release file = *r;
    const char *suffix = strrchr(name, '.');
    /* A feed can label the contents as an image while the provider serves an archive.
     * Use the archive's actual format so the normal inspection verifies its contents. */
    if (file.remote_page && image_format(&file) &&
        ((suffix && *compression(suffix)) || (detected && *compression(detected))))
        copy_text(file.format, sizeof file.format, "Unknown");
    int result = file_format(&file, name, true, detected);
    if (!result) *r = file;
    return result;
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

static size_t page_id(const char *text, bool punctuation) {
    return strspn(text, punctuation
        ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
        : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
}

const char *remote_browser_provider(const char *url) {
    if (!url || strlen(url) >= sizeof ((Release *)0)->browser_url) return "";
    for (const unsigned char *p = (const void *)url; *p; p++)
        if (*p <= 32 || *p >= 127 || *p == '\\' || *p == '#') return "";
    char platform[4], game_id[13];
    if (pspkg_page_identity(url, platform, sizeof platform, game_id, sizeof game_id)) return "pspkg";
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) {
        const char *path = strchr(url + 17, '/');
        if (path && !strncmp(path, "/pspkg-provider/vikingfile.com/", sizeof "/pspkg-provider/vikingfile.com/" - 1)) return "vikingfile";
        if (path && !strncmp(path, "/pspkg-provider/buzzheavier.com/", sizeof "/pspkg-provider/buzzheavier.com/" - 1)) return "buzzheavier";
        if (path && !strncmp(path, "/pspkg-provider/datavaults.co/", sizeof "/pspkg-provider/datavaults.co/" - 1)) return "datavaults";
    }
#endif
    if (strncmp(url, "https://", 8)) return "";
    const char *host = url + 8, *path = strchr(host, '/');
    if (!path) return "";
    static const struct {
        const char *host, *prefix, *suffix, *provider;
        unsigned min, max;
        bool punctuation;
    } pages[] = {
        {"buzzheavier.com", "/", "", "buzzheavier", 6, 64, true},
        {"akirabox.com", "/", "/file", "akirabox", 12, 12, false},
        {"akirabox.to", "/", "/file", "akirabox", 12, 12, false},
        {"vikingfile.com", "/f/", "", "vikingfile", 10, 10, false},
        {"vik1ngfile.site", "/f/", "", "vikingfile", 10, 10, false},
        {"vik1ngf1le.online", "/f/", "", "vikingfile", 10, 10, false},
        {"1fichier.com", "/?", "", "1fichier", 20, 20, false},
        {"rootz.so", "/d/", "", "rootz", 4, 64, true},
        {"www.rootz.so", "/d/", "", "rootz", 4, 64, true},
        {"rapidgator.net", "/file/", "/", "rapidgator", 32, 32, false},
        {"datanodes.to", "/", "", "datanodes", 12, 12, false},
        {"datanodes.co", "/", "", "datanodes", 12, 12, false},
        {"filekeeper.net", "/f/", "", "filekeeper", 32, 32, false},
        {"datavaults.co", "/f/", "", "datavaults", 32, 32, false},
        {"filekeeper.net", "/", "", "filekeeper", 6, 64, true},
        {"datavaults.co", "/", "", "datavaults", 6, 64, true},
        {"archive.org", "/details/", "", "archive", 1, 120, true},
    };
    for (size_t i = 0; i < sizeof pages / sizeof *pages; i++) {
        size_t prefix = strlen(pages[i].prefix);
        if ((size_t)(path - host) != strlen(pages[i].host) ||
            memcmp(host, pages[i].host, (size_t)(path - host)) ||
            strncmp(path, pages[i].prefix, prefix)) continue;
        const char *id = path + prefix;
        size_t n = page_id(id, pages[i].punctuation);
        if (!strcmp(pages[i].provider, "archive"))
            n = strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-");
        if (n < pages[i].min || n > pages[i].max ||
            (!strcmp(pages[i].provider, "archive") && (*id == '.' || strstr(id, "..")))) return "";
        if (strcmp(pages[i].provider, "rapidgator"))
            return !strcmp(id + n, pages[i].suffix) ? pages[i].provider : "";
        if (strspn(id, "0123456789abcdefABCDEF") != n || id[n] != '/') return "";
        const char *name = id + n + 1;
        size_t length = strlen(name);
        char decoded[160];
        if (length <= 5 || strcmp(name + length - 5, ".html") ||
            strpbrk(name, "/?#") || !provider_percent_decode(name, length, decoded, sizeof decoded) ||
            !filename_valid(decoded) || strpbrk(decoded, "?#%")) return "";
        return pages[i].provider;
    }
    return "";
}

static bool legacy_opaque_url(const char *url) {
#ifdef ORBIT_TEST
    if (!strncmp(url, "http://127.0.0.1:", 17)) return true;
#endif
    char host[254];
    if (!url_host(url, host, sizeof host)) return false;
    const char *legacy[] = {"mediafire.com", "pixeldrain.com", "gofile.io", "mega.nz", "gamedrive.org"};
    for (size_t i = 0; i < sizeof legacy / sizeof *legacy; i++)
        if (!strcmp(host, legacy[i])) return true;
    return false;
}

static void canonical_page(char *url) {
    if (strchr(url, '?')) return;
    size_t n = strlen(url);
    if (n && url[n - 1] == '/') {
        url[n - 1] = 0;
        if (!*remote_browser_provider(url)) url[n - 1] = '/';
    }
    const char *prefixes[] = {"https://filekeeper.net/", "https://datavaults.co/", "https://datanodes.to/"};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
        size_t length = strlen(prefixes[i]);
        if (strncmp(url, prefixes[i], length)) continue;
        char *decoration = strchr(url + length, '/');
        if (!decoration || strchr(decoration + 1, '/')) return;
        char filename[160];
        Release file = {0};
        if (!remote_url_filename(url, filename, sizeof filename) || remote_file_format(&file, filename)) return;
        *decoration = 0;
        if (!*remote_browser_provider(url)) *decoration = '/';
        return;
    }
}

static bool normalize_url(const char *raw, char *url, size_t cap) {
    if (strlen(raw) >= cap || !strncmp(raw, "https://1fichier.com?", 21) ||
        strpbrk(raw, "\\#") || strstr(raw, "/../") ||
        strstr(raw, "/./")) return false;
    for (const unsigned char *p = (const void *)raw; *p; p++)
        if (*p < 32 || *p == 127) return false;
    CURLU *parts = curl_url();
    char *normalized = NULL, *port = NULL;
    bool ok = parts && !strpbrk(raw, "\\#\t\r\n") &&
              !curl_url_set(parts, CURLUPART_URL, raw, CURLU_DISALLOW_USER | CURLU_ALLOW_SPACE | CURLU_URLENCODE) &&
              (curl_url_get(parts, CURLUPART_PORT, &port, 0) == CURLUE_NO_PORT
#ifdef ORBIT_TEST
               || !strncmp(raw, "http://127.0.0.1:", 17)
#endif
              ) &&
              !curl_url_get(parts, CURLUPART_URL, &normalized, 0) &&
              strlen(normalized) < cap && remote_https_url(normalized);
    if (ok) {
        copy_text(url, cap, normalized);
        canonical_page(url);
    }
    curl_free(port);
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

    const char *label = json_text(link, "label"), *provider = json_text(link, "name");
    const char *kind = json_text(link, "format");
    if (!*label) label = provider;
    if (multipart_label(label) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(link, "multipart")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "multipart"))) return 2;
    const char *part_fields[] = {"part", "volume", "partNumber", "volumeNumber"};
    for (size_t i = 0; i < sizeof part_fields / sizeof *part_fields; i++) {
        const cJSON *part = cJSON_GetObjectItemCaseSensitive(link, part_fields[i]);
        if (!part) part = cJSON_GetObjectItemCaseSensitive(entry, part_fields[i]);
        if ((cJSON_IsNumber(part) && part->valuedouble > 0) ||
            (cJSON_IsString(part) && *part->valuestring)) return 2;
    }
    const char *format = json_text(entry, "format"), *category = json_text(entry, "category");
    const char *edition = json_text(link, "edition");
    if (!*edition) edition = json_text(entry, "edition");
    if (multipart_label(edition) || multipart_label(provider)) return 2;
    if (unsupported_label(label) || unsupported_label(kind) || unsupported_label(edition) ||
        unsupported_label(format) || unsupported_label(category) || unsupported_label(provider) ||
        unsupported_label(json_text(link, "compression")) || unsupported_label(json_text(link, "assetCompression")) ||
        unsupported_label(json_text(entry, "compression")) || unsupported_label(json_text(entry, "assetCompression"))) return 1;
    const char *hint = label_format(kind);
    if (!*hint) hint = label_format(label);
    if (!*hint) hint = label_format(edition);
    if (!*hint) {
        if (unsupported_label(format) || unsupported_label(category)) return 1;
        hint = label_format(*format ? format : category);
        if (*format && !*hint && strcasecmp(format, "Unknown") && strcasecmp(format, "Unknown file type")) return 1;
    }
    copy_text(r->format, sizeof r->format, hint);
    if (*label && strlen(label) < sizeof r->remote_label && utf8_text(label))
        copy_text(r->remote_label, sizeof r->remote_label, label);

    if (*edition && utf8_text(edition)) copy_text(r->remote_edition, sizeof r->remote_edition, edition);
    if (!field(link, "version", r->remote_version, sizeof r->remote_version) ||
        (!r->remote_version[0] && !field(entry, "version", r->remote_version, sizeof r->remote_version)) ||
        !field(link, "region", r->remote_region, sizeof r->remote_region) ||
        (!r->remote_region[0] && !field(entry, "region", r->remote_region, sizeof r->remote_region))) return -1;
    const char *source_page = json_text(link, "sourcePage");
    if (!*source_page) source_page = json_text(entry, "downloadSource");
    if (*source_page && !strchr(source_page, '?') && remote_https_url(source_page) &&
        strlen(source_page) < sizeof r->remote_source_page)
        copy_text(r->remote_source_page, sizeof r->remote_source_page, source_page);
    char filename[sizeof r->filename] = "", path_name[sizeof r->filename] = "";
    if (!field(entry, "title", r->title, sizeof r->title) ||
        !field(entry, "titleId", r->title_id, sizeof r->title_id) ||
        !r->title[0] ||
        !field(link, "filename", filename, sizeof filename) ||
        (!*filename && !field(entry, "filename", filename, sizeof filename)) ||
        (*filename && !filename_valid(filename))) return -1;
    const char *pspkg = pspkg_platform(source_id);
    if (pspkg && !r->title_id[0]) {
        /* Public PSPKG pages often omit a PSN title ID. Keep their scanner ID
         * stable and separate from real CUSA/PPSA matches in Orbit's catalogue. */
        const char *id = json_text(entry, "id");
        if (strlen(id) != 18 || strncmp(id, "pspkg-", 6) ||
            strspn(id + 6, "0123456789abcdef") != 12) return -1;
        snprintf(r->title_id, sizeof r->title_id, "P%c%s", pspkg[2], id + 6);
    }
    if (!identifier(r->title_id)) return -1;
    if (*filename && unsupported_label(filename)) return 1;
    if (*filename && multipart_label(filename)) return 2;
    const char *platform = json_text(entry, "platform");
    if (!*platform && pspkg) platform = pspkg;
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
    if (has_path && multipart_label(path_name)) return 2;
    if (!strcmp(r->url, "https://datanodes.to/download")) return 1;
    const char *page_provider = remote_browser_provider(r->url);
    bool gated = pspkg && !strcmp(page_provider, "pspkg");
    if (gated) {
        const char *source = strstr(json_text(entry, "description"), "\nSource: ");
        if (!source) return -1;
        source += strlen("\nSource: ");
        const char *end = strchr(source, '\n');
        char game[sizeof r->browser_url];
        if (!end || end == source || end - source >= (ptrdiff_t)sizeof game) return -1;
        memcpy(game, source, (size_t)(end - source));
        game[end - source] = 0;
        if (!pspkg_page_matches(game, r->url)) return -1;
    }
    bool explicit_page = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(link, "page"));
    int result = remote_file_format(r, *filename ? filename : has_path ? path_name : "");
    bool hosted = gated || *page_provider || ((explicit_page || !has_path) && hosted_page_hint(r->url));
    if ((!hosted && explicit_page) || (!gated && !strcmp(page_provider, "pspkg"))) return -1;
    if (result > 0) return result;
    if (!hosted && result < 0 && !legacy_opaque_url(r->url)) return -1;
    if (result < 0 || hosted) {
        /* Opaque routes are resolved only after the option is selected. */
        if (strlen(r->url) >= sizeof r->browser_url) return -1;
        copy_text(r->browser_url, sizeof r->browser_url, r->url);
        memset(r->url, 0, sizeof r->url);
        if (*filename) copy_text(r->remote_filename, sizeof r->remote_filename, filename);
        r->remote_filename_synthetic = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(link, "syntheticFilename"));
        memset(r->filename, 0, sizeof r->filename);
        const char *archive = json_text(link, "compression");
        if (!*archive) archive = json_text(entry, "compression");
        if (!*archive) archive = token(kind, "7z") || token(label, "7z") ? "7z" :
                                 token(kind, "rar") || token(label, "rar") ? "rar" :
                                 token(kind, "zip") || token(label, "zip") ? "zip" : "";
        if (!strcasecmp(archive, "zip") || !strcasecmp(archive, "rar") || !strcasecmp(archive, "7z")) {
            copy_text(r->compression, sizeof r->compression, archive);
            for (char *p = r->compression; *p; p++) *p = (char)tolower((unsigned char)*p);
        }
        r->remote_page = true;
        if (!bytes(link, "sizeBytes", &r->display_size) ||
            (!r->display_size && !bytes(entry, "sizeBytes", &r->display_size))) return -1;
        if (*hint) copy_text(r->remote_format_hint, sizeof r->remote_format_hint, hint);
        if (r->compression[0]) copy_text(r->remote_compression_hint, sizeof r->remote_compression_hint, r->compression);
        if (!r->format[0]) copy_text(r->format, sizeof r->format, "Unknown");
    } else {
        const cJSON *file = *filename && !*json_text(link, "filename") ? entry : link;
        if (!field(file, "checksum", r->sha256, sizeof r->sha256) ||
            (!r->sha256[0] && !field(file, "sha256", r->sha256, sizeof r->sha256)) ||
            !bytes(file, "sizeBytes", &r->size) || !bytes(file, "extractedBytes", &r->extracted_size))
            return -1;
        if (!r->size && !bytes(entry, "sizeBytes", &r->display_size)) return -1;
        const char *page = json_text(entry, "downloadSource");
        if (*page && strlen(page) < sizeof r->browser_url && remote_https_url(page))
            copy_text(r->browser_url, sizeof r->browser_url, page);
        for (char *p = r->sha256; *p; p++)
            if (*p >= 'A' && *p <= 'F') *p += 'a' - 'A';
    }
    if (!bytes(link, "sizeEstimateBytes", &r->display_size) ||
        ((r->remote_page || r->compression[0]) &&
         !field(link, "archivePassword", r->archive_password, sizeof r->archive_password))) return -1;
    if (!valid(r)) return -1;
    identify(r);
    return 0;
}

static void diagnostic_url(cJSON *row, const char *field, const char *url) {
    if (!*url) return;
    char host[254];
    if (*remote_browser_provider(url)) cJSON_AddStringToObject(row, field, url);
    else if (url_host(url, host, sizeof host)) cJSON_AddStringToObject(row, field, host);
    else cJSON_AddStringToObject(row, field, "[invalid URL]");
}

cJSON *remote_catalog_diagnostics_locked(const cJSON *entry, const char *source_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "source", source_id);
    cJSON_AddStringToObject(root, "discovery", entry ? "discovered" : "not-in-cached-index");
    cJSON *rows = cJSON_AddArrayToObject(root, "links");
    const cJSON *link, *links = cJSON_GetObjectItemCaseSensitive(entry, "downloadLinks");
    unsigned count = 0;
    cJSON_ArrayForEach(link, links) {
        if (count++ == 64) {
            cJSON_AddTrueToObject(root, "truncated");
            break;
        }
        cJSON *row = cJSON_CreateObject();
        if (!row) break;
        const char *url = json_text(link, "url");
        diagnostic_url(row, "rawUrl", url);
        char normalized[2048];
        bool normalized_ok = normalize_url(url, normalized, sizeof normalized);
        cJSON_AddBoolToObject(row, "normalized", normalized_ok);
        if (normalized_ok) diagnostic_url(row, "normalizedUrl", normalized);
        const char *fields[] = {"name", "label", "format", "compression", "assetCompression",
                                "part", "volume", "multipart", "version", "region", "filename", "sizeEstimateBytes"};
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(link, fields[i]);
            if (cJSON_IsNumber(value) || cJSON_IsBool(value) ||
                (cJSON_IsString(value) && strlen(value->valuestring) < 256))
                cJSON_AddItemToObject(row, fields[i], cJSON_Duplicate(value, true));
        }
        Release mapped;
        int result = source_package(entry, source_id) ? map_link(entry, link, source_id, &mapped) : 3;
        const char *reason = result == 2 ? "multipart" : result == 1 ? "unsupported-format" :
            result == 3 ? "wrong-platform-or-section" : result ? "invalid-url-or-fields" : "";
        const Release *installed = result ? NULL : release_find(mapped.id);
        bool enabled = installed && remote_source_enabled_locked(installed);
        bool visible = enabled && !installed->remote_alias && !remote_link_missing_locked(installed);
        if (visible && *remote_mirror_group_locked(installed)) {
            Release first;
            if (remote_next_mirror_locked(installed, "", &first) && strcmp(first.id, installed->id) < 0)
                visible = false;
        }
        cJSON_AddStringToObject(row, "importResult", "in-cache");
        cJSON_AddStringToObject(row, "filterReason", reason);
        cJSON_AddStringToObject(row, "mapResult", result ? "rejected" : installed ? "retained" : "not-retained");
        if (installed) {
            cJSON_AddStringToObject(row, "releaseId", installed->id);
            cJSON_AddStringToObject(row, "mirrorGroup", remote_mirror_group_locked(installed));
        }
        cJSON_AddBoolToObject(row, "visible", visible);
        cJSON_AddBoolToObject(row, "metadataEligible", enabled && remote_metadata_option(installed));
        cJSON_AddItemToArray(rows, row);
    }
    return root;
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

void padrino_catalog_clear(void) {
    free(padrino_titles);
    cJSON_Delete(padrino_snapshot);
    padrino_titles = NULL;
    padrino_snapshot = NULL;
    padrino_title_count = 0;
}

const cJSON *padrino_game(const char *title_id) {
    size_t first = 0, end = padrino_title_count;
    while (first < end) {
        size_t mid = first + (end - first) / 2;
        const cJSON *game = padrino_titles[mid];
        int order = strcmp(json_text(game, "titleId"), title_id);
        if (!order) return game;
        if (order < 0) first = mid + 1;
        else end = mid;
    }
    return NULL;
}

typedef struct { size_t package; const cJSON *link; } OptionOrigin;
static void identify_mirrors(Release *releases, const OptionOrigin *origin, size_t count);

int padrino_catalog_load(Release **releases, size_t *count, time_t *snapshot_at) {
    *releases = NULL;
    *count = 0;
    unsigned long length = PADRINO_JSON_SIZE;
    unsigned char *body = malloc(length + 1);
    if (!body) return -1;
    if (uncompress(body, &length, padrino_data, sizeof padrino_data) != Z_OK || length != PADRINO_JSON_SIZE) {
        free(body);
        return -1;
    }
    body[length] = 0;
    cJSON *snapshot = cJSON_ParseWithLengthOpts((char *)body, length + 1, NULL, true);
    free(body);
    cJSON *games = cJSON_GetObjectItemCaseSensitive(snapshot, "packages"), *game;
    int size = cJSON_GetArraySize(games);
    const cJSON **titles = NULL;
    Release *rows = NULL;
    OptionOrigin *origins = NULL;
    if (json_int(snapshot, "schemaVersion") != 1 || !cJSON_IsArray(games) || size < 1 || size > PADRINO_OPTION_LIMIT)
        goto fail;
    size_t options = 0, used = 0, title_count = 0;
    const char *previous = "";
    cJSON_ArrayForEach(game, games) {
        const cJSON *links = cJSON_GetObjectItemCaseSensitive(game, "downloadLinks");
        const char *id = json_text(game, "titleId");
        if (!cJSON_IsArray(links) || strcmp(previous, id) >= 0) goto fail;
        previous = id;
        options += (size_t)cJSON_GetArraySize(links);
        if (options > PADRINO_OPTION_LIMIT) goto fail;
    }
    if (!options) goto fail;
    titles = malloc((size_t)size * sizeof *titles);
    rows = calloc(options, sizeof *rows);
    origins = calloc(options, sizeof *origins);
    if (!titles || !rows || !origins) goto fail;
    cJSON_ArrayForEach(game, games) {
        cJSON *links = cJSON_GetObjectItemCaseSensitive(game, "downloadLinks"), *link;
        size_t before = used;
        cJSON_ArrayForEach(link, links) {
            if (map_link(game, link, PADRINO_SOURCE, &rows[used])) continue;
            origins[used++] = (OptionOrigin){title_count, link};
        }
        if (before < used) titles[title_count++] = game;
    }
    if (!used) goto fail;
    identify_mirrors(rows, origins, used);
    free(origins);
    qsort(rows, used, sizeof *rows, compare_releases);
    for (size_t i = 0; i < used; i++) {
        if (*count && !strcmp(rows[*count - 1].id, rows[i].id)) continue;
        if (*count != i) rows[*count] = rows[i];
        (*count)++;
    }
    padrino_snapshot = snapshot;
    padrino_titles = titles;
    padrino_title_count = title_count;
    *releases = rows;
    *snapshot_at = PADRINO_SNAPSHOT_AT;
    return 0;
fail:
    cJSON_Delete(snapshot);
    free(origins);
    free(titles);
    free(rows);
    return -1;
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
    {{"filekeeper.net"}, {"filekeeper net", "file keeper", "filekeeper", "filek"}},
    {{"1fichier.com"}, {"1fichier com", "1fichier"}},
    {{"archive.org"}, {"archive org", "archive"}},
    {{"mediafire.com"}, {"mediafire com", "mediafire"}},
    {{"buzzheavier.com"}, {"buzzheavier com", "buzzheavier"}},
    {{"rapidgator.net"}, {"rapidgator net", "rapidgator"}},
    {{"datavaults.co"}, {"datavaults co", "datavaults", "vault"}},
};

static int mirror_provider(const char *host) {
    for (size_t p = 0; p < sizeof mirror_providers / sizeof *mirror_providers; p++) {
        const char *const *domains = mirror_providers[p].domains;
        for (size_t i = 0; i < sizeof mirror_providers[p].domains / sizeof *domains && domains[i]; i++)
            if (!strcmp(host, domains[i])) return (int)p;
    }
    return -1;
}

static int mirror_variant(const Release *r, char *out, size_t cap) {
    char host[254];
    if (!remote_link_host(r, host, sizeof host)) return -1;
#ifdef ORBIT_TEST
    /* Fixtures use one loopback server while retaining independent provider
     * identities for the mirror grouping and queue tests. */
    const char *url = provider_torbox_url(r);
    if (!strcmp(host, "127.0.0.1")) {
        const char *path = strchr(url + 17, '/');
        if (path && !strncmp(path, "/metadata/", 10)) {
            path += 10;
            const size_t length = strcspn(path, "/");
            if (path[length] == '/' && length < sizeof host) {
                memcpy(host, path, length);
                host[length] = 0;
            }
        }
    }
#endif
    int provider = mirror_provider(host);
    if (provider < 0) return -1;

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

typedef struct {
    Release *release;
    OptionOrigin origin;
    char variant[128];
    unsigned provider;
} MirrorOption;

static int compare_mirror_variants(const MirrorOption *x, const MirrorOption *y) {
    if (x->origin.package != y->origin.package)
        return x->origin.package < y->origin.package ? -1 : 1;
    int variant = strcmp(x->variant, y->variant);
    if (variant || strcmp(x->release->remote_catalog_id, PADRINO_SOURCE)) return variant;
    variant = strcmp(x->release->remote_version, y->release->remote_version);
    if (variant || x->release->display_size == y->release->display_size) return variant;
    return x->release->display_size < y->release->display_size ? -1 : 1;
}

static int compare_mirror_options(const void *a, const void *b) {
    const MirrorOption *x = a, *y = b;
    int variant = compare_mirror_variants(x, y);
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
        const char *filename = r->remote_filename_synthetic ? "" : r->remote_filename;
        if ((providers & provider) || strcmp(first->format, r->format) ||
            strcmp(first->remote_version, r->remote_version) || strcmp(first->remote_region, r->remote_region) ||
            strcmp(first->remote_edition, r->remote_edition) ||
            (size && r->display_size && size != r->display_size) ||
            (i && (strcmp(compression, r->compression) || strcmp(name, filename)))) return;
        if (r->display_size) size = r->display_size;
        compression = r->compression;
        name = filename;
        providers |= provider;
        used += (size_t)snprintf(identity + used, sizeof identity - used, "%s/", r->id);
    }
    if (!(providers & (providers - 1))) return;
    for (size_t i = 0; i < count; i++) mirror_hash(rows[i].release, identity);
}

static void identify_mirrors(Release *releases, const OptionOrigin *origin, size_t count) {
    if (!count) return;
    bool bundled = !strcmp(releases[0].remote_catalog_id, PADRINO_SOURCE);
    MirrorOption *opaque = calloc(count, sizeof *opaque);
    size_t pending = 0;
    for (size_t i = 0; i < count; i++) {
        Release *r = &releases[i];
        if (r->sha256[0]) {
            char identity[384];
            snprintf(identity, sizeof identity, "file/%s/%s/%s/%s/%lld/%s/%s/%s",
                r->title_id, r->format, r->compression, r->sha256, (long long)r->size,
                r->remote_version, r->remote_region, r->filename);
            mirror_hash(r, identity);
            continue;
        }
        const cJSON *link = origin[i].link;
        if (!opaque || !remote_page_pending(r) || cJSON_GetObjectItemCaseSensitive(link, "part") ||
            *json_text(link, "checksum") || *json_text(link, "sha256") || token(r->remote_label, "part")) continue;
        /* Padrino collects multiple editions under one title. A provider
         * label by itself is insufficient evidence within that title. */
        if (bundled && (!r->remote_version[0] || !r->display_size)) continue;
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
            while (next < pending && !compare_mirror_variants(&opaque[i], &opaque[next])) next++;
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
    for (size_t rank = 1; rank < grouped && alternate < REMOTE_MIRROR_RESERVE; rank++)
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

static bool selected_package(const cJSON *entry, const char *source_id, const char *title_id) {
    return source_package(entry, source_id) && (!title_id || !strcmp(json_text(entry, "titleId"), title_id));
}

static int parse_catalog(const char *body, size_t length, const char *source_id, const char *title_id,
                         Release **releases, size_t *count, size_t *skipped) {
    *releases = NULL;
    *count = *skipped = 0;
    cJSON *root = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    const cJSON *packages = cJSON_GetObjectItemCaseSensitive(root, "packages"), *entry;
    if (!cJSON_IsArray(packages) || memchr(body, 0, length)) {
        cJSON_Delete(root);
        return -1;
    }
    size_t scan_limit = pspkg_platform(source_id) ? PSPKG_INDEX_LIMIT : REMOTE_SCAN_LIMIT;
    if (title_id) scan_limit = 128;
    size_t capacity = 0, package_count = (size_t)cJSON_GetArraySize(packages);
    struct { const cJSON *next, *first; } *remaining =
        calloc(package_count ? package_count : 1, sizeof *remaining);
    OptionOrigin *origins = calloc(scan_limit, sizeof *origins);
    size_t seen_cap = 2;
    while (seen_cap < scan_limit * 2) seen_cap *= 2;
    size_t *first_slots = calloc(seen_cap, sizeof *first_slots);
    if (!remaining || !origins || !first_slots) goto failed;
    size_t i = 0;
    cJSON_ArrayForEach(entry, packages) {
        if (!selected_package(entry, source_id, title_id)) { i++; continue; }
        const cJSON *links = cJSON_GetObjectItemCaseSensitive(entry, "downloadLinks"), *link;
        bool unsupported = false;
        if (cJSON_IsArray(links) && *count < scan_limit) {
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
                size_t slot = 0;
                for (const unsigned char *p = (const unsigned char *)first.title_id; *p; p++)
                    slot = slot * 31 + *p;
                slot &= seen_cap - 1;
                while (first_slots[slot] && strcmp((*releases)[first_slots[slot] - 1].title_id, first.title_id))
                    slot = (slot + 1) & (seen_cap - 1);
                remaining[i].next = links->child;
                if (!first_slots[slot]) {
                    first_slots[slot] = *count + 1;
                    origins[*count] = (OptionOrigin){i, remaining[i].first};
                    if (append_option(releases, count, &capacity, &first)) goto failed;
                } else remaining[i].first = NULL;
            } else if (!unsupported) (*skipped)++;
        } else (*skipped)++;
        i++;
    }
    /* Visit the mirrors in rounds, so the largest entries cannot use the
     * source limit before the other titles get a second choice. */
    while (*count < scan_limit) {
        bool more = false;
        i = 0;
        cJSON_ArrayForEach(entry, packages) {
            if (!selected_package(entry, source_id, title_id)) { i++; continue; }
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
            if (*count == scan_limit) break;
        }
        if (!more) break;
    }
    identify_mirrors(*releases, origins, *count);
    if (!title_id && !pspkg_platform(source_id)) *count = retain_choices(*releases, *count);
    free(first_slots);
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
    free(first_slots);
    free(origins);
    free(remaining);
    free(*releases);
    *releases = NULL;
    cJSON_Delete(root);
    return -1;
}

int remote_catalog_parse(const char *body, size_t length, const char *source_id,
                         Release **releases, size_t *count, size_t *skipped) {
    return parse_catalog(body, length, source_id, NULL, releases, count, skipped);
}

int remote_catalog_title(const char *body, size_t length, const char *source_id, const char *title_id,
                         Release **releases, size_t *count) {
    size_t skipped;
    return parse_catalog(body, length, source_id, title_id, releases, count, &skipped);
}
