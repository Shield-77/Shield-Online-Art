#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Limits for online previews, not for the existing local SD viewer. */
#define ART_PREVIEW_MAX_BYTES (1024u * 1024u)
#define ART_PREVIEW_MAX_DIMENSION 1024u
#define ART_PREVIEW_MAX_PIXELS (512u * 1024u)

enum {
    ART_FLAG_DELETED_KNOWN = 1u << 0,
    ART_FLAG_DELETED = 1u << 1,
    ART_FLAG_MATURE_KNOWN = 1u << 2,
    ART_FLAG_MATURE = 1u << 3
};

typedef enum {
    ART_PREVIEW_OK = 0,
    ART_PREVIEW_TOO_LARGE,
    ART_PREVIEW_BAD_MIME,
    ART_PREVIEW_TYPE_MISMATCH,
    ART_PREVIEW_INVALID,
    ART_PREVIEW_UNSUPPORTED,
    ART_PREVIEW_DIMENSIONS
} ArtPreviewStatus;

typedef enum { ART_IMAGE_NONE = 0, ART_IMAGE_JPEG, ART_IMAGE_PNG } ArtImageType;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t type; /* ArtImageType, fixed width across compiler ABI settings. */
} ArtPreviewInfo;

/* Must be called on the metadata before requesting any preview or thumbnail.
 * A missing or ill-typed field must be represented as *_KNOWN being unset. */
bool art_content_allowed(bool filter_enabled, uint32_t flags);

/* Header preflight only: the actual JPEG/PNG decoder must still validate the
 * complete file, including checksums. The buffer is always owned by the caller. */
ArtPreviewStatus art_check_preview(const uint8_t *bytes, size_t length,
                                  const char *mime, size_t mime_length,
                                  ArtPreviewInfo *out);
