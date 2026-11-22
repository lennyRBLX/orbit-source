/* Catalogue and add-on lists for the browser and TV app. Building one means
 * copying every visible entry and printing about 10 MB of JSON; doing that for
 * each request stalled the single HTTP thread (issue 185). Each body is built
 * once, compressed once for clients that accept gzip, and shared by reference
 * until the catalogue, the sources, the firmware or an invalidation changes it. */
#include "orbit.h"
#include "catalog_view.h"
#include "firmware.h"
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
/* Everything a body depends on. Source settings and firmware are compared
 * directly, so a missed invalidation for them can't serve a stale list. */
typedef struct {
    unsigned long generation, catalog;
    unsigned enabled, notice_version;
    bool acknowledged;
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
                     .enabled = orbit.sources.enabled,
                     .notice_version = orbit.sources.notice_version,
                     .acknowledged = orbit.sources.acknowledged_at > 0,
                     .firmware = orbit.firmware};
}
static bool same_key(const ViewKey *a, const ViewKey *b) {
    return a->generation == b->generation && a->catalog == b->catalog && a->enabled == b->enabled &&
           a->notice_version == b->notice_version && a->acknowledged == b->acknowledged &&
           a->firmware == b->firmware;
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
        if (!source_enabled_locked(r)) continue;
        cJSON *entry = cJSON_Duplicate(x, true);
        if (!entry) {
            cJSON_Delete(a);
            return NULL;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "url");
        cJSON_DeleteItemFromObjectCaseSensitive(entry, "browserUrl");
        cJSON_AddBoolToObject(entry, "browserAvailable", provider_browser_supported(r));
        cJSON_AddBoolToObject(entry, "directAvailable", provider_url_supported(r));
        firmware_describe(entry, orbit.firmware);
        cJSON_AddItemToArray(a, entry);
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
