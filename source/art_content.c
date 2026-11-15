#include "art_content.h"

bool art_content_allowed(bool filter_enabled, uint32_t flags) {
    if (!(flags & ART_FLAG_DELETED_KNOWN) || (flags & ART_FLAG_DELETED)) return false;
    if (filter_enabled && (!(flags & ART_FLAG_MATURE_KNOWN) || (flags & ART_FLAG_MATURE)))
        return false;
    return true;
}

static unsigned lower_ascii(unsigned c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool mime_equals(const char *mime, size_t length, const char *expected) {
    size_t first = 0, last = 0, i;
    while (first < length && (mime[first] == ' ' || mime[first] == '\t')) first++;
    last = first;
    while (last < length && mime[last] != ';') last++;
    while (last > first && (mime[last - 1] == ' ' || mime[last - 1] == '\t')) last--;
    for (i = 0; first + i < last && expected[i]; i++)
        if (lower_ascii((unsigned char)mime[first + i]) != (unsigned char)expected[i]) return false;
    return first + i == last && expected[i] == '\0';
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static bool acceptable_dimensions(uint32_t width, uint32_t height) {
    return width && height && width <= ART_PREVIEW_MAX_DIMENSION &&
           height <= ART_PREVIEW_MAX_DIMENSION &&
           (uint64_t)width * height <= ART_PREVIEW_MAX_PIXELS;
}

static ArtPreviewStatus check_png(const uint8_t *b, size_t n, ArtPreviewInfo *out) {
    if (n < 33 || read_be32(b + 8) != 13 || b[12] != 'I' || b[13] != 'H' ||
        b[14] != 'D' || b[15] != 'R') return ART_PREVIEW_INVALID;
    uint32_t width = read_be32(b + 16), height = read_be32(b + 20);
    if (!acceptable_dimensions(width, height)) return ART_PREVIEW_DIMENSIONS;
    unsigned depth = b[24], color = b[25];
    bool valid_depth = (color == 0 && (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)) ||
                       (color == 2 && (depth == 8 || depth == 16)) ||
                       (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                       ((color == 4 || color == 6) && (depth == 8 || depth == 16));
    if (!valid_depth || b[26] != 0 || b[27] != 0) return ART_PREVIEW_INVALID;
    if (b[28] != 0) return ART_PREVIEW_UNSUPPORTED;
    out->width = width; out->height = height; out->type = ART_IMAGE_PNG;
    return ART_PREVIEW_OK;
}

static bool is_jpeg_frame(unsigned marker) {
    return (marker >= 0xC0 && marker <= 0xC3) ||
           (marker >= 0xC5 && marker <= 0xC7) ||
           (marker >= 0xC9 && marker <= 0xCB) ||
           (marker >= 0xCD && marker <= 0xCF);
}

static ArtPreviewStatus check_jpeg(const uint8_t *b, size_t n, ArtPreviewInfo *out) {
    size_t p = 2;
    unsigned markers = 0;
    /* A preview should not spend seconds scanning megabytes of metadata. */
    while (p < n && p <= 256u * 1024u && ++markers <= 1024) {
        if (b[p++] != 0xFF) return ART_PREVIEW_INVALID;
        while (p < n && p <= 256u * 1024u && b[p] == 0xFF) p++;
        if (p > 256u * 1024u) return ART_PREVIEW_UNSUPPORTED;
        if (p >= n) return ART_PREVIEW_INVALID;
        unsigned marker = b[p++];
        if (marker == 0 || marker == 0xD8 || marker == 0xD9 || marker == 0xDA)
            return ART_PREVIEW_INVALID;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (n - p < 2) return ART_PREVIEW_INVALID;
        size_t segment = ((size_t)b[p] << 8) | b[p + 1];
        if (segment < 2 || segment > n - p) return ART_PREVIEW_INVALID;
        if (is_jpeg_frame(marker)) {
            if (marker != 0xC0 && marker != 0xC1 && marker != 0xC2)
                return ART_PREVIEW_UNSUPPORTED;
            if (segment < 8 || b[p + 2] != 8) return ART_PREVIEW_INVALID;
            uint32_t height = ((uint32_t)b[p + 3] << 8) | b[p + 4];
            uint32_t width = ((uint32_t)b[p + 5] << 8) | b[p + 6];
            unsigned components = b[p + 7];
            if (components != 1 && components != 3) return ART_PREVIEW_UNSUPPORTED;
            if (segment < 8u + 3u * components) return ART_PREVIEW_INVALID;
            if (!acceptable_dimensions(width, height)) return ART_PREVIEW_DIMENSIONS;
            out->width = width; out->height = height; out->type = ART_IMAGE_JPEG;
            return ART_PREVIEW_OK;
        }
        p += segment;
    }
    return ART_PREVIEW_INVALID;
}

ArtPreviewStatus art_check_preview(const uint8_t *bytes, size_t length,
                                  const char *mime, size_t mime_length,
                                  ArtPreviewInfo *out) {
    static const uint8_t png_sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (!out) return ART_PREVIEW_INVALID;
    out->width = out->height = 0; out->type = ART_IMAGE_NONE;
    if (length > ART_PREVIEW_MAX_BYTES) return ART_PREVIEW_TOO_LARGE;
    if (!bytes || !mime || !length || !mime_length) return ART_PREVIEW_INVALID;
    if (mime_length > 256) return ART_PREVIEW_BAD_MIME;
    bool jpeg = mime_equals(mime, mime_length, "image/jpeg");
    bool png = mime_equals(mime, mime_length, "image/png");
    if (!jpeg && !png) return ART_PREVIEW_BAD_MIME;
    if (jpeg) {
        if (length < 3 || bytes[0] != 0xFF || bytes[1] != 0xD8 || bytes[2] != 0xFF)
            return ART_PREVIEW_TYPE_MISMATCH;
        return check_jpeg(bytes, length, out);
    }
    if (length < sizeof(png_sig)) return ART_PREVIEW_TYPE_MISMATCH;
    for (unsigned i = 0; i < sizeof(png_sig); i++)
        if (bytes[i] != png_sig[i]) return ART_PREVIEW_TYPE_MISMATCH;
    return check_png(bytes, length, out);
}
