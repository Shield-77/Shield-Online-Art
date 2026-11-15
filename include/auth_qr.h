#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../third_party/qrcodegen/qrcodegen.h"

/* At 2 pixels/module with a 4-module margin this fits the 240-pixel screen. */
#define AUTH_QR_MAX_VERSION 23
#define AUTH_QR_BUFFER_SIZE qrcodegen_BUFFER_LEN_FOR_VERSION(AUTH_QR_MAX_VERSION)
bool auth_qr_encode(const char *url, size_t length, uint8_t *work, uint8_t *result);
/* Draws a QR with its quiet zone into a 400 x 240 BGR8 3DS framebuffer. */
bool auth_qr_draw_top(uint8_t *framebuffer, size_t length, const uint8_t *qr);
