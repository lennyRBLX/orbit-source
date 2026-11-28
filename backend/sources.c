#include "orbit.h"
#include "remote_sources.h"
#include <string.h>

bool catalogue_source_known(const char *id) {
    return !strcmp(id, "archive") || !strcmp(id, "vikingfile") || !strcmp(id, "akirabox") ||
           !strcmp(id, "datanodes") || !strcmp(id, "1fichier") || !strcmp(id, "remote");
}

bool source_enabled_locked(const Release *r) {
    if (!r)
        return false;
    /* The pinned connection diagnostic is not a game source. */
    if (!strcmp(r->id, "orbit-network-check") || !strcmp(r->source, ORBIT_DIRECT_SOURCE))
        return true;
    if (!strcmp(r->source, "remote")) return remote_source_enabled_locked(r);
    /* A link the user pasted is not a catalogue source. */
    if (!strcmp(r->source, "link"))
        return true;
    return catalogue_source_known(r->source);
}

/* Older clients use this read-only response to open the catalogue. */
cJSON *sources_json_locked(void) {
    static const struct {
        const char *id, *label;
    } providers[] = {{"vikingfile", "Vikingfile"}, {"archive", "Archive.org"},
                     {"akirabox", "AkiraBox"}, {"datanodes", "DataNodes"},
                     {"remote", "Community catalogues"}};
    cJSON *o = cJSON_CreateObject(), *enabled = cJSON_AddArrayToObject(o, "enabled"),
          *options = cJSON_AddArrayToObject(o, "options");
    cJSON_AddTrueToObject(o, "acknowledged");
    cJSON_AddNumberToObject(o, "noticeVersion", 1);
    cJSON_AddNumberToObject(o, "remoteGeneration", remote_sources_generation_locked());
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++) {
        cJSON_AddItemToArray(enabled, cJSON_CreateString(providers[i].id));
        unsigned count = 0, addon_count = 0;
        for (size_t j = 0; j < orbit.release_count; j++)
            if (!strcmp(orbit.releases[j].source, providers[i].id)) {
                if (orbit.releases[j].addon_id[0]) addon_count++;
                else count++;
            }
        if (!strcmp(providers[i].id, "remote")) count = (unsigned)orbit.remote_release_count;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", providers[i].id);
        cJSON_AddStringToObject(p, "label", providers[i].label);
        cJSON_AddNumberToObject(p, "releaseCount", count);
        cJSON_AddNumberToObject(p, "addonReleaseCount", addon_count);
        cJSON_AddTrueToObject(p, "downloadReady");
        cJSON_AddItemToArray(options, p);
    }
    return o;
}
