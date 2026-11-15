#include "oauth_pkce.h"
#include "deviantart_config.h"
#include <mbedtls/sha256.h>

void oauth_pkce_clear(void *buffer, size_t length) {
    volatile uint8_t *p = (volatile uint8_t *)buffer;
    if (!p) return;
    while (length--) *p++ = 0;
}

static void base64url(const uint8_t *input, size_t length, char *output) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0, o = 0;
    while (length - i >= 3) {
        uint32_t n = ((uint32_t)input[i] << 16) | ((uint32_t)input[i + 1] << 8) | input[i + 2];
        output[o++] = alphabet[(n >> 18) & 63];
        output[o++] = alphabet[(n >> 12) & 63];
        output[o++] = alphabet[(n >> 6) & 63];
        output[o++] = alphabet[n & 63];
        i += 3;
    }
    if (length - i == 1) {
        uint32_t n = (uint32_t)input[i] << 16;
        output[o++] = alphabet[(n >> 18) & 63];
        output[o++] = alphabet[(n >> 12) & 63];
    } else if (length - i == 2) {
        uint32_t n = ((uint32_t)input[i] << 16) | ((uint32_t)input[i + 1] << 8);
        output[o++] = alphabet[(n >> 18) & 63];
        output[o++] = alphabet[(n >> 12) & 63];
        output[o++] = alphabet[(n >> 6) & 63];
    }
    output[o] = '\0';
}

bool oauth_pkce_verifier(const uint8_t *entropy, size_t length,
                         char output[OAUTH_VERIFIER_CAPACITY]) {
    if (!output) return false;
    output[0] = '\0';
    if (!entropy || length != OAUTH_VERIFIER_ENTROPY) return false;
    base64url(entropy, length, output);
    return true;
}

bool oauth_pkce_challenge(const char *verifier, size_t length,
                          char output[OAUTH_CHALLENGE_CAPACITY]) {
    if (!output) return false;
    output[0] = '\0';
    if (!verifier || length < 43 || length > 128) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)verifier[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~'))
            return false;
    }
    uint8_t digest[32];
    int result = mbedtls_sha256_ret((const unsigned char *)verifier, length, digest, 0);
    if (result == 0) base64url(digest, sizeof(digest), output);
    oauth_pkce_clear(digest, sizeof(digest));
    return result == 0;
}

bool oauth_pkce_state(const uint8_t *entropy, size_t length,
                      char output[OAUTH_STATE_CAPACITY]) {
    if (!output) return false;
    output[0] = '\0';
    if (!entropy || length != OAUTH_STATE_ENTROPY) return false;
    base64url(entropy, length, output);
    return true;
}

bool oauth_pkce_state_matches(const char *expected, size_t expected_length,
                              const char *received, size_t received_length) {
    if (!expected || !received || expected_length != OAUTH_STATE_CAPACITY - 1 ||
        received_length != expected_length) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < expected_length; i++)
        diff |= (unsigned char)expected[i] ^ (unsigned char)received[i];
    return diff == 0;
}

typedef struct { char *out; size_t capacity, used; bool ok; } UrlBuilder;

static void url_put(UrlBuilder *b, char c) {
    if (!b->ok || b->used + 1 >= b->capacity) { b->ok = false; return; }
    b->out[b->used++] = c;
}

static void url_literal(UrlBuilder *b, const char *text) {
    while (*text) url_put(b, *text++);
}

static bool unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

static void url_encoded(UrlBuilder *b, const char *text, size_t length) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (unreserved(c)) url_put(b, (char)c);
        else { url_put(b, '%'); url_put(b, hex[c >> 4]); url_put(b, hex[c & 15]); }
    }
}

static bool valid_redirect(const char *text, size_t length) {
    static const char prefix[] = "https://";
    if (!text || length < 9 || length > 512) return false;
    for (size_t i = 0; i < sizeof(prefix) - 1; i++)
        if (text[i] != prefix[i]) return false;
    size_t label_length = 0, host_length = 0;
    bool host_valid = false, in_path = false, previous_hyphen = false;
    for (size_t i = sizeof(prefix) - 1; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '/') {
            if (!in_path && (!label_length || previous_hyphen)) return false;
            host_valid = true;
            in_path = true;
            continue;
        }
        if (c <= 0x20 || c >= 0x7F || c == '@' || c == '\\' || c == '#' || c == '?' || c == '%')
            return false;
        if (!in_path) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
            if (++host_length > 253) return false;
            if (c == '.') {
                if (!label_length || previous_hyphen) return false;
                label_length = 0;
            } else {
                if ((!label_length && c == '-') || ++label_length > 63) return false;
            }
            previous_hyphen = c == '-';
        }
    }
    return host_valid || (label_length && !previous_hyphen);
}

static bool valid_base64url(const char *text, size_t length) {
    if (!text) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return text[length] == '\0';
}

bool oauth_pkce_build_url(const char *redirect, size_t redirect_length,
                          const char *state, const char *challenge,
                          char *output, size_t output_capacity) {
    if (!output || !output_capacity) return false;
    output[0] = '\0';
    if (!valid_redirect(redirect, redirect_length) ||
        !valid_base64url(state, OAUTH_STATE_CAPACITY - 1) ||
        !valid_base64url(challenge, OAUTH_CHALLENGE_CAPACITY - 1)) return false;
    UrlBuilder b = { output, output_capacity, 0, true };
    url_literal(&b, SHIELD_DEVIANTART_AUTH_URL "?response_type=code&client_id=" SHIELD_DEVIANTART_CLIENT_ID "&redirect_uri=");
    url_encoded(&b, redirect, redirect_length);
    url_literal(&b, "&scope=" SHIELD_DEVIANTART_SCOPE "&state=");
    url_encoded(&b, state, OAUTH_STATE_CAPACITY - 1);
    url_literal(&b, "&code_challenge=");
    url_encoded(&b, challenge, OAUTH_CHALLENGE_CAPACITY - 1);
    url_literal(&b, "&code_challenge_method=S256");
    if (!b.ok) { output[0] = '\0'; return false; }
    output[b.used] = '\0';
    return true;
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool query_key(const char *key, size_t length, const char *expected) {
    size_t i = 0;
    while (i < length && expected[i] && key[i] == expected[i]) i++;
    return i == length && expected[i] == '\0';
}

static bool decode_value(const char *input, size_t length, char *output, size_t capacity,
                         size_t *written) {
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (length - i < 3) return false;
            int high = hex_digit((unsigned char)input[++i]);
            int low = hex_digit((unsigned char)input[++i]);
            if (high < 0 || low < 0) return false;
            c = (unsigned char)((high << 4) | low);
        } else if (c == '+') c = ' ';
        if (c <= 0x20 || c >= 0x7F || out + 1 >= capacity) return false;
        output[out++] = (char)c;
    }
    if (!capacity) return false;
    output[out] = '\0';
    *written = out;
    return true;
}

OAuthCallbackStatus oauth_pkce_parse_callback(const char *query, size_t query_length,
                                             const char *expected_state,
                                             char *code, size_t code_capacity) {
    char received_state[OAUTH_STATE_CAPACITY] = {0};
    char received_code[OAUTH_MAX_CODE_LENGTH + 1] = {0};
    char received_error[64] = {0};
    size_t state_length = 0, code_length = 0, error_length = 0;
    bool has_state = false, has_code = false, has_error = false;
    OAuthCallbackStatus result = OAUTH_CALLBACK_INVALID;
    if (!code || !code_capacity) return OAUTH_CALLBACK_OUTPUT_TOO_SMALL;
    code[0] = '\0';
    if (!query || !expected_state || query_length > 4096 ||
        !valid_base64url(expected_state, OAUTH_STATE_CAPACITY - 1)) return result;

    size_t pos = query_length && query[0] == '?' ? 1 : 0;
    unsigned fields = 0;
    while (pos < query_length && ++fields <= 32) {
        size_t end = pos, equal = pos;
        while (end < query_length && query[end] != '&') end++;
        while (equal < end && query[equal] != '=') equal++;
        if (equal == end || equal == pos) goto cleanup;
        const char *value = query + equal + 1;
        size_t len = end - equal - 1;
        if (query_key(query + pos, equal - pos, "state")) {
            if (has_state || !decode_value(value, len, received_state, sizeof(received_state), &state_length)) goto cleanup;
            has_state = true;
        } else if (query_key(query + pos, equal - pos, "code")) {
            if (has_code || !decode_value(value, len, received_code, sizeof(received_code), &code_length)) goto cleanup;
            has_code = true;
        } else if (query_key(query + pos, equal - pos, "error")) {
            if (has_error || !decode_value(value, len, received_error, sizeof(received_error), &error_length)) goto cleanup;
            has_error = true;
        }
        if (end == query_length) { pos = end; break; }
        pos = end + 1;
        if (pos == query_length) goto cleanup;
    }
    if (pos < query_length || !has_state || (has_code == has_error)) goto cleanup;
    if (!oauth_pkce_state_matches(expected_state, OAUTH_STATE_CAPACITY - 1,
                                  received_state, state_length)) {
        result = OAUTH_CALLBACK_WRONG_STATE;
        goto cleanup;
    }
    if (has_error) {
        result = error_length ? OAUTH_CALLBACK_DENIED : OAUTH_CALLBACK_INVALID;
        goto cleanup;
    }
    if (!code_length) goto cleanup;
    if (code_length + 1 > code_capacity) { result = OAUTH_CALLBACK_OUTPUT_TOO_SMALL; goto cleanup; }
    for (size_t i = 0; i <= code_length; i++) code[i] = received_code[i];
    result = OAUTH_CALLBACK_OK;
cleanup:
    oauth_pkce_clear(received_state, sizeof(received_state));
    oauth_pkce_clear(received_code, sizeof(received_code));
    oauth_pkce_clear(received_error, sizeof(received_error));
    return result;
}
