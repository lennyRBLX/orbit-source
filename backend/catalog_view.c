#include "remote_sources.h"
/* Catalogue and add-on lists for the browser and TV app. Building one means
 * copying every visible entry and printing about 10 MB of JSON; doing that for
 * each request stalled the single HTTP thread (issue 185). Each body is built
 * once, compressed once for clients that accept gzip, and shared by reference
 * until the catalogue, the sources, the firmware or an invalidation changes it. */
#include "orbit.h"
#include "catalog_view.h"
#include "firmware.h"
#include "pspkg.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

struct CatalogBody {
    atomic_uint refs;
    size_t size;
    unsigned char *data;
};
/* Firmware is compared directly so a missed invalidation can't serve a stale list. */
typedef struct {
    unsigned long generation, catalog;
    uint32_t firmware;
} ViewKey;
typedef struct {
    bool valid;
    ViewKey key;
    CatalogBody *plain, *gzip; /* Each holds one reference for the cache. */
} View;
static pthread_mutex_t view_mutex = PTHREAD_MUTEX_INITIALIZER; /* After orbit.mutex. */
static View views[2];
static atomic_ulong view_generation;
static unsigned long replacements; /* orbit.mutex */

void catalog_view_invalidate_locked(void) { atomic_fetch_add(&view_generation, 1); }
void catalog_replaced_locked(void) {
    replacements++;
    catalog_view_invalidate_locked();
}
unsigned long catalog_generation_locked(void) { return replacements; }
const unsigned char *catalog_body_data(const CatalogBody *body) { return body->data; }
size_t catalog_body_size(const CatalogBody *body) { return body->size; }
void catalog_body_release(void *opaque) {
    CatalogBody *body = opaque;
    if (body && atomic_fetch_sub(&body->refs, 1) == 1) {
        free(body->data);
        free(body);
    }
}
static CatalogBody *retain(CatalogBody *body) {
    if (body) atomic_fetch_add(&body->refs, 1);
    return body;
}
/* Takes ownership of data, also when it fails. */
static CatalogBody *body_new(unsigned char *data, size_t size) {
    CatalogBody *body = data ? malloc(sizeof *body) : NULL;
    if (!body) {
        free(data);
        return NULL;
    }
    atomic_init(&body->refs, 1);
    body->size = size;
    body->data = data;
    return body;
}
static void key_locked(ViewKey *key) {
    *key = (ViewKey){.generation = atomic_load(&view_generation),
                     .catalog = replacements,
                     .firmware = orbit.firmware};
}
static bool same_key(const ViewKey *a, const ViewKey *b) {
    return a->generation == b->generation && a->catalog == b->catalog && a->firmware == b->firmware;
}
/* Index the curated title identities once per view. Prefer the first curated
 * entry when multiple options share a title ID. */
typedef struct {
    const cJSON *entry;
    const char *title_id;
    size_t order;
} TitleEntry;

static int compare_titles(const void *a, const void *b) {
    const TitleEntry *x = a, *y = b;
    int compare = strcmp(x->title_id, y->title_id);
    if (compare) return compare;
    return (x->order > y->order) - (x->order < y->order);
}

static const cJSON *match_title(const TitleEntry *titles, size_t count, const char *id) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(titles[mid].title_id, id) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < count && !strcmp(titles[lo].title_id, id) ? titles[lo].entry : NULL;
}

static int compare_mirrors(const void *a, const void *b) {
    const Release *x = *(const Release *const *)a, *y = *(const Release *const *)b;
    int key = strcmp(remote_mirror_group_locked(x), remote_mirror_group_locked(y));
    if (key) return key;
    bool x_missing = remote_link_missing_locked(x), y_missing = remote_link_missing_locked(y);
    return x_missing == y_missing ? strcmp(x->id, y->id) : x_missing ? 1 : -1;
}

static size_t first_mirror(const Release *const *rows, size_t count, const char *key) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(remote_mirror_group_locked(rows[mid]), key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool describe_mirrors(cJSON *entry, const Release *r, const Release *const *group, size_t count) {
    char host[254];
    bool restricted = remote_link_archive_requires_login_locked(r);
    if (restricted && count < 2 && !cJSON_AddArrayToObject(entry, "torboxHosts")) return false;
    if (!restricted && remote_link_listed_locked(r) && remote_link_host(r, host, sizeof host) &&
        !cJSON_AddStringToObject(entry, "torboxHost", host)) return false;
    bool metadata = remote_metadata_option(r);
    size_t available = remote_link_missing_locked(r) ? 0 : 1;
    if (count > 1) {
        cJSON *hosts = cJSON_AddArrayToObject(entry, "torboxHosts");
        if (!hosts) return false;
        available = 0;
        for (size_t i = 0; i < count; i++) {
            const Release *mirror = group[i];
            metadata |= remote_metadata_option(mirror);
            if (!remote_link_listed_locked(mirror)) continue;
            if (!remote_link_missing_locked(mirror)) available++;
            if (remote_link_archive_requires_login_locked(mirror) || !remote_link_host(mirror, host, sizeof host)) continue;
            bool duplicate = false;
            const cJSON *item;
            cJSON_ArrayForEach(item, hosts)
                if (!strcmp(item->valuestring, host)) { duplicate = true; break; }
            if (duplicate) continue;
            cJSON *name = cJSON_CreateString(host);
            if (!name || !cJSON_AddItemToArray(hosts, name)) { cJSON_Delete(name); return false; }
        }
    }
    return (available <= 1 || cJSON_AddNumberToObject(entry, "mirrorCount", (double)available)) &&
           (available || cJSON_AddTrueToObject(entry, "providerMissing")) &&
           (!metadata || cJSON_AddTrueToObject(entry, "providerMetadata"));
}

bool catalog_add_resolved_version(cJSON *entry, const Release *r, const char *filename) {
    const char *version = r->remote_version;
    char inferred[sizeof r->remote_version];
    if (!*version) {
        if (!*filename && !r->remote_filename_synthetic) filename = r->remote_filename;
        pspkg_display_version("", filename, inferred, sizeof inferred);
        version = inferred;
    }
    return !*version || cJSON_AddStringToObject(entry, "resolvedVersion", version);
}

cJSON *catalog_remote_choice_locked(const Release *r, const Release *const *group, size_t count) {
    cJSON *entry = release_json(r);
    if (!entry) return NULL;
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "url");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "browserUrl");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "remoteCatalogId");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "remoteLabel");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "sourcePage");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "providerPage");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "sourceChoice");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "syntheticFilename");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "mirrorKey");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "remotePage");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "sha256");
    if (r->archive_password[0]) cJSON_AddTrueToObject(entry, "hasArchivePassword");
    cJSON_DeleteItemFromObjectCaseSensitive(entry, "archivePassword");
    bool ok = cJSON_AddStringToObject(entry, "provider", r->remote_label[0] ? r->remote_label : "Community catalogues") &&
         cJSON_AddBoolToObject(entry, "browserAvailable", remote_page_pending(r)) &&
         cJSON_AddBoolToObject(entry, "directAvailable", !remote_page_pending(r));
    if (ok) ok = describe_mirrors(entry, r, group, count);
    if (ok && !r->remote_version[0]) ok = catalog_add_resolved_version(entry, r, r->filename);
    firmware_describe(entry, orbit.firmware);
    if (!ok) { cJSON_Delete(entry); return NULL; }
    return entry;
}

static bool append_remote_locked(cJSON *out) {
    if (!orbit.remote_release_count) return true;
    size_t count = (size_t)cJSON_GetArraySize(orbit.catalog), i = 0;
    TitleEntry *titles = malloc((count ? count : 1) * sizeof *titles);
    const Release **mirrors = malloc(orbit.remote_release_count * sizeof *mirrors);
    if (!titles || !mirrors) { free(titles); free(mirrors); return false; }
    size_t mirror_count = 0;
    for (i = 0; i < orbit.remote_release_count; i++) {
        const Release *r = &orbit.remote_releases[i];
        if (!r->remote_alias[0] && *remote_mirror_group_locked(r) && source_enabled_locked(r))
            mirrors[mirror_count++] = r;
    }
    qsort(mirrors, mirror_count, sizeof *mirrors, compare_mirrors);
    i = 0;
    const cJSON *curated;
    cJSON_ArrayForEach(curated, orbit.catalog) {
        titles[i] = (TitleEntry){curated, json_text(curated, "titleId"), i};
        i++;
    }
    qsort(titles, count, sizeof *titles, compare_titles);
    bool ok = true;
    for (i = 0; i < orbit.remote_release_count && ok; i++) {
        const Release *r = &orbit.remote_releases[i];
        if (r->remote_alias[0] || !source_enabled_locked(r)) continue;
        bool pspkg = !strcmp(r->remote_catalog_id, PSPKG_PS5_SOURCE) || !strcmp(r->remote_catalog_id, PSPKG_PS4_SOURCE);
        size_t begin = 0, end = 0;
        const char *group = remote_mirror_group_locked(r);
        if (*group) {
            begin = first_mirror(mirrors, mirror_count, group);
            if (begin == mirror_count || mirrors[begin] != r) continue;
            end = begin + 1;
            while (end < mirror_count && !strcmp(remote_mirror_group_locked(mirrors[end]), group)) end++;
        }
        if (!pspkg && !remote_link_listed_locked(r)) {
            bool listed = false;
            for (size_t j = begin; j < end; j++) listed |= remote_link_listed_locked(mirrors[j]);
            if (!listed) continue;
        }
        cJSON *entry = catalog_remote_choice_locked(r, mirrors + begin, end - begin);
        if (!entry) { ok = false; break; }
        const cJSON *match = match_title(titles, count, r->title_id);
        if (match) {
            const char *const keys[] = {"gameId", "title", "genre", "tagline", "description", "cover", "hero",
                "coverFallback", "heroFallback", "artworkLayout", "publisher", "releaseDate", "addedAt",
                "newOnOrbitRank", "insights", "tags"};
            for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
                const cJSON *value = cJSON_GetObjectItemCaseSensitive(match, keys[k]);
                if (!value) continue;
                cJSON *copy = cJSON_Duplicate(value, true);
                cJSON_DeleteItemFromObjectCaseSensitive(entry, keys[k]);
                if (!copy || !cJSON_AddItemToObject(entry, keys[k], copy)) {
                    cJSON_Delete(copy);
                    ok = false;
                    break;
                }
            }
        } else if (!strcmp(r->remote_catalog_id, PADRINO_SOURCE)) {
            const cJSON *game = padrino_game(r->title_id);
            ok = cJSON_AddStringToObject(entry, "cover", json_text(game, "cover")) &&
                 cJSON_AddStringToObject(entry, "hero", json_text(game, "hero")) &&
                 cJSON_AddStringToObject(entry, "genre", json_text(game, "genre")) &&
                 cJSON_AddStringToObject(entry, "publisher", json_text(game, "publisher")) &&
                 cJSON_AddStringToObject(entry, "releaseDate", json_text(game, "releaseDate")) &&
                 cJSON_AddStringToObject(entry, "artworkLayout", "ambient");
        } else {
            ok = cJSON_AddBoolToObject(entry, "homebrew", true) &&
                 cJSON_AddStringToObject(entry, "cover", "") &&
                 cJSON_AddStringToObject(entry, "hero", "") &&
                 cJSON_AddStringToObject(entry, "artworkLayout", "ambient");
        }
        if (ok) ok = cJSON_AddItemToArray(out, entry);
        if (!ok) cJSON_Delete(entry);
    }
    free(titles);
    free(mirrors);
    return ok;
}

/* The visible entries without their private links, with what Orbit's
 * providers and this console make of each. orbit.mutex held. */
static cJSON *snapshot_locked(bool addons) {
    cJSON *list = addons ? orbit.addons : orbit.catalog, *x;
    cJSON *a = cJSON_CreateArray();
    if (!a) return NULL;
    /* orbit.releases holds the games then the add-ons in feed order: look at
     * the matching slot before searching (tests may reorder slots). */
    size_t slot = addons ? (size_t)cJSON_GetArraySize(orbit.catalog) : 0;
    cJSON_ArrayForEach(x, list) {
        const char *id = json_text(x, "id");
        Release *r = slot < orbit.release_count && !strcmp(orbit.releases[slot].id, id)
                         ? &orbit.releases[slot]
                         : release_find(id);
        slot++;
        if (!source_enabled_locked(r) || !remote_link_listed_locked(r)) continue;
        cJSON *entry = cJSON_Duplicate(x, true);
        if (!entry) {
            cJSON_Delete(a);
            return NULL;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "url");
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "browserUrl");
        cJSON_AddBoolToObject(entry, "browserAvailable", provider_browser_supported(r));
        cJSON_AddBoolToObject(entry, "directAvailable", provider_url_supported(r));
        if (remote_metadata_option(r)) cJSON_AddTrueToObject(entry, "providerMetadata");
        char host[254];
        if (remote_link_missing_locked(r)) cJSON_AddTrueToObject(entry, "providerMissing");
        if (remote_link_archive_requires_login_locked(r)) cJSON_AddArrayToObject(entry, "torboxHosts");
        else if (remote_link_host(r, host, sizeof host)) cJSON_AddStringToObject(entry, "torboxHost", host);
        firmware_describe(entry, orbit.firmware);
        cJSON_AddItemToArray(a, entry);
    }
    if (!addons && !append_remote_locked(a)) {
        cJSON_Delete(a);
        return NULL;
    }
    return a;
}
static CatalogBody *compress_body(const CatalogBody *plain) {
    if (plain->size > UINT_MAX / 2) return NULL;
    z_stream z = {0};
    /* 15 + 16: a gzip wrapper, which every browser decodes. Level 6 measured
     * 9.3 MB to 1.7 MB; higher levels saved under 2% for far more time. */
    if (deflateInit2(&z, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    uLong bound = deflateBound(&z, (uLong)plain->size);
    unsigned char *out = bound <= UINT_MAX ? malloc(bound) : NULL;
    int rc = Z_MEM_ERROR;
    if (out) {
        z.next_in = plain->data;
        z.avail_in = (uInt)plain->size;
        z.next_out = out;
        z.avail_out = (uInt)bound;
        rc = deflate(&z, Z_FINISH);
    }
    size_t size = (size_t)z.total_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    unsigned char *fitted = realloc(out, size ? size : 1);
    return body_new(fitted ? fitted : out, size);
}
/* Keeps a body built for key, unless that key's body is already cached. */
static void keep_plain(View *view, const ViewKey *key, CatalogBody *plain) {
    pthread_mutex_lock(&view_mutex);
    if (!view->valid || !same_key(&view->key, key) || !view->plain) {
        catalog_body_release(view->plain);
        catalog_body_release(view->gzip);
        *view = (View){.valid = true, .key = *key, .plain = retain(plain)};
    }
    pthread_mutex_unlock(&view_mutex);
}
static void keep_gzip(View *view, const ViewKey *key, const CatalogBody *plain, CatalogBody *gzip) {
    pthread_mutex_lock(&view_mutex);
    if (view->valid && same_key(&view->key, key) && view->plain == plain && !view->gzip)
        view->gzip = retain(gzip);
    pthread_mutex_unlock(&view_mutex);
}
CatalogBody *catalog_view_get(bool addons, bool *gzip) {
    View *view = &views[addons ? 1 : 0];
    ViewKey key;
    CatalogBody *found = NULL, *plain = NULL;
    cJSON *snapshot = NULL;
    pthread_mutex_lock(&orbit.mutex);
    key_locked(&key);
    pthread_mutex_lock(&view_mutex);
    if (view->valid && same_key(&view->key, &key)) {
        found = retain(*gzip ? view->gzip : view->plain);
        if (!found) plain = retain(view->plain);
    }
    pthread_mutex_unlock(&view_mutex);
    if (!found && !plain) snapshot = snapshot_locked(addons);
    pthread_mutex_unlock(&orbit.mutex);
    if (found) return found;
    if (!plain) {
        /* Printing and compressing happen without orbit.mutex: downloads and
         * other requests carry on while the snapshot is serialised. */
        char *text = snapshot ? cJSON_PrintUnformatted(snapshot) : NULL;
        cJSON_Delete(snapshot);
        plain = text ? body_new((unsigned char *)text, strlen(text)) : NULL;
        if (!plain) return NULL;
        keep_plain(view, &key, plain);
    }
    if (!*gzip) return plain;
    CatalogBody *packed = compress_body(plain);
    if (!packed) {
        *gzip = false;
        return plain;
    }
    keep_gzip(view, &key, plain, packed);
    catalog_body_release(plain);
    return packed;
}
void catalog_view_clear(void) {
    pthread_mutex_lock(&view_mutex);
    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        catalog_body_release(views[i].plain);
        catalog_body_release(views[i].gzip);
        views[i] = (View){0};
    }
    pthread_mutex_unlock(&view_mutex);
}
