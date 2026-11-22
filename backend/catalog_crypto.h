#ifndef ORBIT_CATALOG_CRYPTO_H
#define ORBIT_CATALOG_CRYPTO_H
#include <stddef.h>

/* Scraping resistance only: the key is distributed with the application and
 * corresponding source. This is not a signing key or an access-control boundary. */
#define CATALOG_SEAL_OVERHEAD 40u
#define CATALOG_SEAL_LIMIT (16u * 1024u * 1024u)
/* The repository's own games, releases and add-ons envelopes are read only by the
 * catalogue tools, never by the console, so they may be larger than a feed. */
#define CATALOG_SOURCE_LIMIT (64u * 1024u * 1024u)
#define CATALOG_PURPOSE_FEED 1u
#define CATALOG_PURPOSE_GAMES 2u
#define CATALOG_PURPOSE_RELEASES 3u
#define CATALOG_PURPOSE_ADDONS 4u

/* Allocated results belong to the caller. Open authenticates before returning
 * plaintext and appends a NUL outside its reported length for JSON parsing. */
int catalog_seal(const void *data, size_t size, unsigned purpose,
                 unsigned char **out, size_t *length);
int catalog_open(const void *data, size_t size, unsigned purpose,
                 unsigned char **out, size_t *length);
#endif
