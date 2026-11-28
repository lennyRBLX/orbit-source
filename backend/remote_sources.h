#ifndef ORBIT_REMOTE_SOURCES_H
#define ORBIT_REMOTE_SOURCES_H

#define PADRINO_SOURCE "padrino"
#define PSPKG_PS5_SOURCE "pspkg-ps5"
#define PSPKG_PS4_SOURCE "pspkg-ps4"
#define PADRINO_OPTION_LIMIT 24576
#define PSPKG_INDEX_LIMIT 8192
#define METADATA_REQUEST_LIMIT 32

#include "orbit.h"

/* Startup loads the built-in catalogues and their cache before reading the queue. */
int remote_sources_load(void);
#ifdef ORBIT_TEST
cJSON *remote_pspkg_preview(const char *source_id, const char *url,
                            size_t game_limit);
#endif
#ifdef ORBIT_REMOTE_TEST_CLOCK
cJSON *remote_sources_test_status_locked(void);
void remote_sources_test_refresh_locked(void);
#endif
cJSON *remote_pspkg_links(const char *html);
cJSON *remote_pspkg_session_links(const char *html, const char *page, const char *heading);
void remote_pspkg_identify_choice(Release *release, const char *url, size_t index);
const char *remote_pspkg_route(const char *url, bool intermediate, atomic_bool *cancel,
                               char *out, size_t cap);
int remote_sources_start(void);
void remote_sources_stop(void);
void remote_sources_tick(void);
bool remote_source_enabled_locked(const Release *release);
cJSON *remote_sources_diagnostics_locked(void);
unsigned long remote_sources_generation_locked(void);

/* Pegasus package adapter and validation of queued remote snapshots. */
int pegasus_entry_to_release(const cJSON *entry, const char *source_id, Release *release);
bool remote_page_pending(const Release *release);
const char *remote_browser_provider(const char *url);
bool remote_link_host(const Release *release, char *host, size_t cap);
bool remote_link_missing_locked(const Release *release);
void remote_mark_missing_locked(const Release *release);
/* The release must be a live catalogue entry. */
const char *remote_mirror_group_locked(const Release *release);
bool remote_next_mirror_locked(const Release *release, const char *after, Release *mirror);
bool remote_mirror_locked(const Release *release, const char *id, Release *mirror, bool torbox);
bool remote_torbox_route_locked(const Release *release, Release *route);
bool remote_link_listed_locked(const Release *release);
void remote_torbox_changed_locked(void);
/* Acquires orbit.mutex to check provider metadata. */
bool remote_mirror_file(const Release *selected, const Release *file);
bool remote_metadata_option(const Release *release);
bool remote_link_missing_locked(const Release *release);
void remote_mark_missing_locked(const Release *release);
bool remote_url_filename(const char *url, char *name, size_t cap);
int remote_file_format(Release *release, const char *name);
int remote_file_detected_format(Release *release, const char *name, const char *suffix);
int direct_file_format(Release *release, const char *name);
bool direct_release_init(Release *release, const char *url);
int direct_release_parse(const cJSON *entry, Release *release);
bool download_file_magic(const Release *release, const unsigned char *sample, size_t length);
enum { REMOTE_DOWNLOAD_MISSING = -2 };
#define REMOTE_DOWNLOAD_REMOVED "The source no longer has this file. Choose another download option if there is one."
bool remote_download_html_missing(long status, const char *html, bool challenge);
int remote_download_resolve(Release *release, Release *locator, atomic_bool *cancel,
                            char *error, size_t cap);
bool remote_download_confirm_missing(const char *url, atomic_bool *cancel);
int remote_download_verify(Release *release, const char *url, atomic_bool *cancel,
                           char *error, size_t cap);
typedef enum {
    REMOTE_METADATA_NONE, REMOTE_METADATA_FOUND, REMOTE_METADATA_MISSING, REMOTE_METADATA_INTERACTIVE,
    REMOTE_METADATA_CHALLENGE, REMOTE_METADATA_TIMEOUT, REMOTE_METADATA_NETWORK, REMOTE_METADATA_HTTP,
    REMOTE_METADATA_REDIRECT, REMOTE_METADATA_TOO_LARGE, REMOTE_METADATA_NO_SIZE,
    REMOTE_METADATA_AMBIGUOUS, REMOTE_METADATA_UNSUPPORTED,
} RemoteMetadataStatus;
const char *remote_metadata_status(RemoteMetadataStatus status);
cJSON *remote_pspkg_public_fields(const char *url, RemoteMetadataStatus *status, atomic_bool *cancel);
typedef struct {
    RemoteMetadataStatus status;
    unsigned confidence, candidate_count;
    long http_status;
    bool attempted;
    int64_t candidates[12];
    char effective_host[254];
    char pspkg_title_ids[12][10];
    char filename[160];
    char format[24], compression[16];
    int64_t size;
    bool exact;
} RemoteFileMetadata;
bool remote_download_metadata(const Release *release, RemoteFileMetadata *metadata, cJSON **links);
cJSON *remote_metadata_json_locked(const char *ids);
int remote_release_parse(const cJSON *entry, Release *release);
int remote_catalog_parse(const char *body, size_t length, const char *source_id,
                         Release **releases, size_t *count, size_t *skipped);
int remote_catalog_title(const char *body, size_t length, const char *source_id, const char *title_id,
                         Release **releases, size_t *count);
cJSON *remote_title_options_locked(const Release *selected);
Release *remote_title_find_locked(const char *id);
cJSON *remote_catalog_diagnostics_locked(const cJSON *entry, const char *source_id);
cJSON *remote_source_package_diagnostics_locked(const char *source, const char *package);
int padrino_catalog_load(Release **releases, size_t *count, time_t *snapshot_at);
const cJSON *padrino_game(const char *title_id);
void padrino_catalog_clear(void);

#endif
