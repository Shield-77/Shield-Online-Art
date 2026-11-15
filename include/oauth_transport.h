#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t capacity, used;
    bool overflow;
} OAuthResponseBuffer;

/* The worker owns these counters; the UI samples the naturally atomic 32-bit
 * values to show progress without locking the rendering loop. */
typedef struct {
    volatile bool *cancel;
    volatile unsigned downloaded_bytes;
    volatile unsigned total_bytes;
} OAuthTransfer;

/* Exposed for isolated tests; bounded, always NUL-terminates on success. */
size_t oauth_response_write(void *data, size_t size, size_t count, void *context);

/* Call from a worker after SOC and libcurl initialization. Requires a trusted
 * CA bundle. Caller must clear the response and form after use. */
bool oauth_post_token(const char *form, const char *ca_data, size_t ca_length,
                      OAuthResponseBuffer *response, long *http_status,
                      OAuthTransfer *transfer);
bool oauth_get_tag(const char *tag, const char *token, const char *ca_data, size_t ca_length,
                   OAuthResponseBuffer *response, long *http_status,
                   OAuthTransfer *transfer);
bool oauth_get_preview(const char *url, const char *ca_data, size_t ca_length,
                       OAuthResponseBuffer *response, char *content_type, size_t content_type_capacity,
                       OAuthTransfer *transfer);

/* Releases the reusable preview connection before curl_global_cleanup(). */
void oauth_transport_cleanup(void);
