#ifndef ORBIT_IMAGE_FORMAT_H
#define ORBIT_IMAGE_FORMAT_H

#include <string.h>
#include <strings.h>

/* ShadowMountPlus disk images. PKG/FPKG installers are a separate format. */
static const struct {
    const char *format;
    const char *suffix;
} image_formats[] = {
    {"FFPKG", ".ffpkg"},
    {"exFAT", ".exfat"},
    {"FFPFS", ".ffpfs"},
    {"FFPFSC", ".ffpfsc"},
};

static inline const char *image_format_for_suffix(const char *suffix) {
    if (suffix)
        for (size_t i = 0; i < sizeof image_formats / sizeof *image_formats; i++)
            if (!strcasecmp(suffix, image_formats[i].suffix)) return image_formats[i].format;
    return NULL;
}

static inline const char *image_suffix_for_format(const char *format) {
    for (size_t i = 0; i < sizeof image_formats / sizeof *image_formats; i++)
        if (!strcmp(format, image_formats[i].format)) return image_formats[i].suffix;
    return NULL;
}

#endif
