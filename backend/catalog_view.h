#ifndef ORBIT_CATALOG_VIEW_H
#define ORBIT_CATALOG_VIEW_H
#include "orbit.h"
#include <stdbool.h>
#include <stddef.h>
/* The /api/v1/catalog and /api/v1/addons bodies, built once and shared by
 * every request until something they show changes (issue 185). */
typedef struct CatalogBody CatalogBody;
/* Call with orbit.mutex held right after anything those bodies show changes
 * that the cache key doesn't cover, such as a flag firmware_describe() reads.
 * Already covered: catalogue replacement, community catalogue refresh and orbit.firmware.
 * Only bumps a counter: no I/O, no other lock. The next request rebuilds. */
void catalog_view_invalidate_locked(void);
/* catalog.c, with orbit.mutex held: orbit.catalog, orbit.addons and
 * orbit.releases were replaced. Invalidates the bodies too. */
void catalog_replaced_locked(void);
/* Changes with every catalog_replaced_locked(). Read with orbit.mutex held. */
unsigned long catalog_generation_locked(void);
/* A reference to the list's JSON. *gzip asks for a gzip body and says whether
 * this is one; a body that can't be compressed comes back plain. NULL when
 * memory runs out. Call without orbit.mutex. */
CatalogBody *catalog_view_get(bool addons, bool *gzip);
const unsigned char *catalog_body_data(const CatalogBody *body);
size_t catalog_body_size(const CatalogBody *body);
/* Drops one reference; the shape of MHD_ContentReaderFreeCallback. */
void catalog_body_release(void *body);
/* Frees the cached bodies at shutdown. Responses being sent keep theirs. */
void catalog_view_clear(void);
cJSON *catalog_remote_choice_locked(const Release *r, const Release *const *group, size_t count);

#endif
