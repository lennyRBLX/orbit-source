#ifndef ORBIT_PSPKG_H
#define ORBIT_PSPKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSPKG_LINK_LIMIT 64

typedef struct {
    char edition_id[13];
    char link_id[13];
    char url[2048], provider[256];
    char label[128], edition[128], title_id[16], version[32], region[32], format[24], compression[16];
    char group[128], filename[160], archive_password[129];
    char reason[32];
    int64_t size, torbox_size;
    unsigned torbox_cached_revision;
    bool intermediate, size_exact, archive_requires_login;
} PspkgLink;

/* Only the general PS4/PS5 game paths are accepted. No Arabic paths. */
bool pspkg_download_page(const char *game, char *out, size_t cap);
bool pspkg_page_matches(const char *game, const char *page);
bool pspkg_page_identity(const char *page, char *platform, size_t platform_cap,
                         char *game_id, size_t id_cap);
bool pspkg_page_redirect(const char *page, const char *redirect);
bool pspkg_link_matches(const char *url, const char *page, const char *platform,
                        const char *game_id, const char *edition_id);
const char *pspkg_title_id(const char *text);
void pspkg_version(const char *edition, const char *filename, char *out, size_t cap);
void pspkg_display_version(const char *edition, const char *filename, char *out, size_t cap);
/* Reads only PSPKG link IDs for the selected game from browser memory. */
void pspkg_scan_links(const unsigned char *bytes, size_t length, const char *platform,
                      const char *game_id, PspkgLink *links, size_t *count, size_t cap);

#endif
