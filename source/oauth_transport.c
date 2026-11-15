#include "oauth_transport.h"
#include "deviantart_config.h"
#include "art_page.h"
#include "art_content.h"
#include "oauth_token.h"
#include "oauth_pkce.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <strings.h>

/* Keep libcurl's connection cache alive between image requests. Preview downloads
 * are serialized by the application's single network worker, so one easy handle
 * is sufficient and lets curl reuse DNS/TLS/HTTP connections when hosts match. */
static CURL *preview_curl;
static struct curl_slist *preview_headers;

void oauth_transport_cleanup(void) {
    if (preview_curl) {
        curl_easy_cleanup(preview_curl);
        preview_curl = NULL;
    }
    if (preview_headers) {
        curl_slist_free_all(preview_headers);
        preview_headers = NULL;
    }
}

size_t oauth_response_write(void *data, size_t size, size_t count, void *context) {
    OAuthResponseBuffer *b = context;
    if (!b || !b->data || !b->capacity || b->used >= b->capacity ||
        (size && count > SIZE_MAX / size)) return 0;
    size_t bytes = size * count;
    if (bytes > b->capacity - b->used - 1) { b->overflow = true; return 0; }
    if (bytes && !data) return 0;
    if (bytes) memcpy(b->data + b->used, data, bytes);
    b->used += bytes;
    b->data[b->used] = 0;
    return bytes;
}

static int oauth_progress(void *context, curl_off_t download_total, curl_off_t downloaded,
                          curl_off_t upload_total, curl_off_t uploaded) {
    (void)upload_total; (void)uploaded;
    OAuthTransfer *transfer = context;
    if (!transfer) return 0;
    transfer->total_bytes = download_total <= 0 ? 0U :
        (download_total > UINT_MAX ? UINT_MAX : (unsigned)download_total);
    transfer->downloaded_bytes = downloaded <= 0 ? 0U :
        (downloaded > UINT_MAX ? UINT_MAX : (unsigned)downloaded);
    return transfer->cancel && *transfer->cancel ? 1 : 0;
}

bool oauth_post_token(const char *form, const char *ca_data, size_t ca_length,
                      OAuthResponseBuffer *response, long *http_status,
                      OAuthTransfer *transfer) {
    if (http_status) *http_status = 0;
    if (!form || !ca_data || !ca_length || !response || !response->data ||
        response->capacity < 2 || !http_status) return false;
    size_t form_length = strnlen(form, 8193);
    if (!form_length || form_length > 8192) return false;
    response->used = 0; response->overflow = false; response->data[0] = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_blob ca = { .data = (void *)ca_data, .len = ca_length, .flags = CURL_BLOB_NOCOPY };
    CURLcode opt = CURLE_OK;
#define SET(option, value) do { if (opt == CURLE_OK) opt = curl_easy_setopt(curl, option, value); } while (0)
    SET(CURLOPT_URL, SHIELD_DEVIANTART_TOKEN_URL);
    SET(CURLOPT_USERAGENT, "ShieldOnlineArt/0.1 (Nintendo 3DS)");
    SET(CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    SET(CURLOPT_POST, 1L);
    SET(CURLOPT_POSTFIELDS, form);
    SET(CURLOPT_POSTFIELDSIZE, (long)form_length);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CAINFO_BLOB, &ca);
    SET(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_CONNECTTIMEOUT, 15L);
    SET(CURLOPT_TIMEOUT, 60L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, oauth_response_write);
    SET(CURLOPT_WRITEDATA, response);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, oauth_progress);
    SET(CURLOPT_XFERINFODATA, transfer);
#undef SET
    CURLcode result = opt == CURLE_OK ? curl_easy_perform(curl) : opt;
    if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    curl_easy_cleanup(curl);
    return result == CURLE_OK && !response->overflow && *http_status == 200;
}

bool oauth_get_tag(const char *tag, const char *token, const char *ca_data, size_t ca_length,
                   OAuthResponseBuffer *response, long *http_status,
                   OAuthTransfer *transfer) {
    char url[512], encoded_tag[193], authorization[OAUTH_ACCESS_TOKEN_CAPACITY + 16];
    struct curl_slist *headers = NULL;
    if (http_status) *http_status = 0;
    if (!tag || !token || !ca_data || !ca_length || !response || !response->data ||
        response->capacity < 2 || response->capacity > ART_PAGE_MAX_BYTES + 1 || !http_status)
        return false;
    size_t tag_len = strnlen(tag, 65), token_len = strnlen(token, OAUTH_ACCESS_TOKEN_CAPACITY);
    if (!tag_len || tag_len >= 65 || !token_len || token_len >= OAUTH_ACCESS_TOKEN_CAPACITY) return false;
    static const char hex[] = "0123456789ABCDEF";
    size_t encoded_length = 0;
    for (size_t i = 0; i < tag_len; i++) {
        unsigned char c = (unsigned char)tag[i];
        if (c < 0x20 || c == 0x7f) return false;
        bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (unreserved) encoded_tag[encoded_length++] = (char)c;
        else {
            if (encoded_length + 3 >= sizeof(encoded_tag)) return false;
            encoded_tag[encoded_length++] = '%';
            encoded_tag[encoded_length++] = hex[c >> 4];
            encoded_tag[encoded_length++] = hex[c & 15];
        }
    }
    encoded_tag[encoded_length] = '\0';
    int n = snprintf(url, sizeof(url), "%s?tag=%s&limit=50&with_session=false",
                     SHIELD_DEVIANTART_BROWSE_URL, encoded_tag);
    if (n < 0 || (size_t)n >= sizeof(url)) return false;
    n = snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", token);
    if (n < 0 || (size_t)n >= sizeof(authorization)) return false;
    struct curl_slist *next = curl_slist_append(headers, authorization);
    if (!next) goto cleanup;
    headers = next;
    next = curl_slist_append(headers, "Accept: application/json");
    if (!next) goto cleanup;
    headers = next;
    next = curl_slist_append(headers, "dA-minor-version: 20240701");
    if (!next) goto cleanup;
    headers = next;
    response->used = 0; response->overflow = false; response->data[0] = 0;
    CURL *curl = curl_easy_init();
    if (!curl) goto cleanup;
    struct curl_blob ca = { .data = (void *)ca_data, .len = ca_length, .flags = CURL_BLOB_NOCOPY };
    CURLcode opt = CURLE_OK;
#define SET(option, value) do { if (opt == CURLE_OK) opt = curl_easy_setopt(curl, option, value); } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_USERAGENT, "ShieldOnlineArt/0.1 (Nintendo 3DS)");
    SET(CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CAINFO_BLOB, &ca);
    SET(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_CONNECTTIMEOUT, 15L);
    SET(CURLOPT_TIMEOUT, 60L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, oauth_response_write);
    SET(CURLOPT_WRITEDATA, response);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, oauth_progress);
    SET(CURLOPT_XFERINFODATA, transfer);
#undef SET
    CURLcode result = opt == CURLE_OK ? curl_easy_perform(curl) : opt;
    if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    oauth_pkce_clear(authorization, sizeof(authorization));
    return result == CURLE_OK && !response->overflow && *http_status == 200;
cleanup:
    if (headers) curl_slist_free_all(headers);
    oauth_pkce_clear(authorization, sizeof(authorization));
    return false;
}

static bool preview_url_allowed(const char *url) {
    static const char prefix[] = "https://";
    if (!url || strncmp(url, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *host = url + sizeof(prefix) - 1;
    size_t n = 0;
    while (host[n] && host[n] != '/' && host[n] != '?' && host[n] != '#') {
        unsigned char c = (unsigned char)host[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        if (++n > 253) return false;
    }
    if (!n || host[0] == '.' || host[n - 1] == '.') return false;
    for (size_t i = 1; i < n; i++)
        if (host[i] == '.' && host[i - 1] == '.') return false;
    const char *domains[] = {"deviantart.com", "deviantart.net", "wixmp.com"};
    bool allowed = false;
    for (unsigned d = 0; d < sizeof(domains) / sizeof(domains[0]); d++) {
        size_t m = strlen(domains[d]);
        if (n >= m && strncasecmp(host + n - m, domains[d], m) == 0 &&
            (n == m || host[n - m - 1] == '.')) allowed = true;
    }
    if (!allowed) return false;
    for (const char *p = host + n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c >= 0x7f || c == '\\' || c == '#') return false;
    }
    return true;
}

bool oauth_get_preview(const char *url, const char *ca_data, size_t ca_length,
                       OAuthResponseBuffer *response, char *content_type, size_t content_type_capacity,
                       OAuthTransfer *transfer) {
    if (!preview_url_allowed(url) || !ca_data || !ca_length || !response ||
        !response->data || response->capacity < 2 ||
        response->capacity > ART_PREVIEW_MAX_BYTES + 1 || !content_type ||
        content_type_capacity < 2) return false;
    response->used = 0; response->overflow = false; response->data[0] = 0;
    content_type[0] = 0;
    if (!preview_curl) preview_curl = curl_easy_init();
    if (!preview_curl) return false;
    if (!preview_headers) {
        preview_headers = curl_slist_append(NULL, "Accept: image/jpeg, image/png");
        if (!preview_headers) return false;
    }
    struct curl_blob ca = { .data = (void *)ca_data, .len = ca_length, .flags = CURL_BLOB_NOCOPY };
    CURLcode opt = CURLE_OK;
#define SET(option, value) do { if (opt == CURLE_OK) opt = curl_easy_setopt(curl, option, value); } while (0)
    CURL *curl = preview_curl;
    SET(CURLOPT_URL, url);
    SET(CURLOPT_USERAGENT, "ShieldOnlineArt/0.1 (Nintendo 3DS)");
    SET(CURLOPT_HTTPGET, 1L);
    SET(CURLOPT_ACCEPT_ENCODING, "identity");
    /* Art previews are public assets; never send the user's OAuth token to a CDN. */
    SET(CURLOPT_HTTPHEADER, preview_headers);
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CAINFO_BLOB, &ca);
    SET(CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    SET(CURLOPT_BUFFERSIZE, 32768L);
    SET(CURLOPT_CONNECTTIMEOUT, 15L);
    SET(CURLOPT_TIMEOUT, 60L);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_WRITEFUNCTION, oauth_response_write);
    SET(CURLOPT_WRITEDATA, response);
    SET(CURLOPT_NOPROGRESS, 0L);
    SET(CURLOPT_XFERINFOFUNCTION, oauth_progress);
    SET(CURLOPT_XFERINFODATA, transfer);
#undef SET
    CURLcode result = opt == CURLE_OK ? curl_easy_perform(curl) : opt;
    long http_status = 0;
    char *mime = NULL;
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &mime);
        if (mime) snprintf(content_type, content_type_capacity, "%s", mime);
    }
    return result == CURLE_OK && !response->overflow && http_status == 200 && content_type[0];
}
