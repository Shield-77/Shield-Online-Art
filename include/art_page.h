#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ART_PAGE_MAX_BYTES (256u * 1024u)
#define ART_PAGE_MAX_ITEMS 50u

typedef struct {
    char id[64];
    char title[128];
    char artist[64];
    char page_url[512];
    char preview_url[1024];
    uint32_t preview_width, preview_height;
} ArtPageItem;

typedef struct {
    uint32_t count, has_more, has_next_offset, next_offset;
    char next_cursor[256];
    ArtPageItem items[ART_PAGE_MAX_ITEMS];
    /* Optional, higher-detail variants are fetched only after the user zooms. */
    char zoom_urls[ART_PAGE_MAX_ITEMS][1024];
} ArtPage;

typedef enum {
    ART_PAGE_OK = 0,
    ART_PAGE_INVALID,
    ART_PAGE_TOO_LARGE
} ArtPageStatus;

/* Parses metadata only; never requests or stores image bytes. For filtered
 * sessions unclassified or mature entries are excluded before returning URLs.
 * The caller owns the bounded JSON input and output and must check the result. */
ArtPageStatus art_parse_page(const char *json, size_t length,
                             bool filter_enabled, ArtPage *out);
