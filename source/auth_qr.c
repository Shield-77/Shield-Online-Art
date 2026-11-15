#include "auth_qr.h"
#include "oauth_pkce.h"
#include <string.h>

bool auth_qr_encode(const char *url, size_t length, uint8_t *work, uint8_t *result) {
    if (!url || !work || !result || work == result || !length || length >= OAUTH_AUTH_URL_CAPACITY ||
        length > AUTH_QR_BUFFER_SIZE) return false;
    /* The caller passes a bounded URL, without the terminating NUL. */
    for (size_t i = 0; i < length; i++) {
        if ((unsigned char)url[i] < 0x21 || (unsigned char)url[i] > 0x7e) return false;
    }
    memcpy(work, url, length);
    bool ok = qrcodegen_encodeBinary(work, length, result, qrcodegen_Ecc_LOW,
                                    qrcodegen_VERSION_MIN, AUTH_QR_MAX_VERSION,
                                    qrcodegen_Mask_AUTO, true);
    oauth_pkce_clear(work, AUTH_QR_BUFFER_SIZE);
    if (!ok) oauth_pkce_clear(result, AUTH_QR_BUFFER_SIZE);
    return ok;
}

bool auth_qr_draw_top(uint8_t *framebuffer, size_t length, const uint8_t *qr) {
    if (!framebuffer || length < 400u * 240u * 3u || !qr) return false;
    int size = qr[0];
    if (size < 21 || size > 109 || (size - 17) % 4 != 0) return false;
    const int scale = 2;
    const int side = (size + 8) * scale;
    const int left = (400 - side) / 2;
    const int top = (240 - side) / 2;
    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            int mx = x / scale - 4, my = y / scale - 4;
            uint8_t color = qrcodegen_getModule(qr, mx, my) ? 0 : 255;
            size_t offset = ((size_t)(left + x) * 240u + (239u - (unsigned)(top + y))) * 3u;
            framebuffer[offset] = framebuffer[offset + 1] = framebuffer[offset + 2] = color;
        }
    }
    return true;
}
