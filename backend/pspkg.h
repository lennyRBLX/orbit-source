#ifndef ORBIT_PSPKG_H
#define ORBIT_PSPKG_H

#include <stdbool.h>
#include <stddef.h>

#define PSPKG_LINK_LIMIT 64

typedef struct {
    char edition_id[13];
    char link_id[13];
    char url[2048], provider[256];
    char label[128], edition[128], title_id[16], version[32], region[32], format[24], compression[16];
    char reason[32];
    bool intermediate;
} PspkgLink;

/* Only the general PS4/PS5 game paths are accepted. No Arabic paths. */
bool pspkg_download_page(const char *game, char *out, size_t cap);
bool pspkg_page_matches(const char *game, const char *page);
bool pspkg_page_identity(const char *page, char *platform, size_t platform_cap,
                         char *game_id, size_t id_cap);
/* Reads only PSPKG link IDs for the selected game from browser memory. */
void pspkg_scan_links(const unsigned char *bytes, size_t length, const char *platform,
                      const char *game_id, PspkgLink *links, size_t *count, size_t cap);

#endif
