#include "oauth_token.h"
#include "oauth_pkce.h"
#include <stdint.h>
#include <string.h>

typedef struct { const unsigned char *s; size_t n, p; } TokenJson;

static void skip_ws(TokenJson *j) {
    while (j->p < j->n && (j->s[j->p] == ' ' || j->s[j->p] == '\r' ||
           j->s[j->p] == '\n' || j->s[j->p] == '\t')) j->p++;
}

static bool token_string(TokenJson *j, char *out, size_t cap, size_t *length) {
    size_t used = 0;
    skip_ws(j);
    if (j->p >= j->n || j->s[j->p++] != '"') return false;
    while (j->p < j->n) {
        unsigned char c = j->s[j->p++];
        if (c == '"') {
            if (out) { if (used >= cap) return false; out[used] = 0; }
            if (length) *length = used;
            return true;
        }
        if (c == '\\') {
            if (j->p >= j->n) return false;
            c = j->s[j->p++];
            if (c == '"' || c == '\\' || c == '/') { }
            else if (c == 'b') c = 8;
            else if (c == 'f') c = 12;
            else if (c == 'n') c = 10;
            else if (c == 'r') c = 13;
            else if (c == 't') c = 9;
            else return false; /* token fields should be plain ASCII */
        }
        if (c < 0x21 || c > 0x7e) return false;
        if (out) { if (used + 1 >= cap) return false; out[used] = (char)c; }
        used++;
    }
    return false;
}

static bool token_skip(TokenJson *j, unsigned depth);
static bool token_skip_array(TokenJson *j, unsigned depth) {
    j->p++; skip_ws(j);
    if (j->p < j->n && j->s[j->p] == ']') { j->p++; return true; }
    for (;;) {
        if (!token_skip(j, depth + 1)) return false;
        skip_ws(j);
        if (j->p < j->n && j->s[j->p] == ']') { j->p++; return true; }
        if (j->p >= j->n || j->s[j->p++] != ',') return false;
    }
}
static bool token_skip_object(TokenJson *j, unsigned depth) {
    j->p++; skip_ws(j);
    if (j->p < j->n && j->s[j->p] == '}') { j->p++; return true; }
    for (;;) {
        if (!token_string(j, NULL, 0, NULL)) return false;
        skip_ws(j);
        if (j->p >= j->n || j->s[j->p++] != ':' || !token_skip(j, depth + 1)) return false;
        skip_ws(j);
        if (j->p < j->n && j->s[j->p] == '}') { j->p++; return true; }
        if (j->p >= j->n || j->s[j->p++] != ',') return false;
    }
}
static bool token_skip(TokenJson *j, unsigned depth) {
    if (depth > 12) return false;
    skip_ws(j);
    if (j->p >= j->n) return false;
    unsigned char c = j->s[j->p];
    if (c == '"') return token_string(j, NULL, 0, NULL);
    if (c == '{') return token_skip_object(j, depth);
    if (c == '[') return token_skip_array(j, depth);
    if (c == 't' && j->n - j->p >= 4 && memcmp(j->s + j->p, "true", 4) == 0) { j->p += 4; return true; }
    if (c == 'f' && j->n - j->p >= 5 && memcmp(j->s + j->p, "false", 5) == 0) { j->p += 5; return true; }
    if (c == 'n' && j->n - j->p >= 4 && memcmp(j->s + j->p, "null", 4) == 0) { j->p += 4; return true; }
    size_t start = j->p;
    if (c == '-') j->p++;
    while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') j->p++;
    if (j->p == start || (j->p == start + 1 && c == '-')) return false;
    if (j->p < j->n && (j->s[j->p] == '.' || j->s[j->p] == 'e' || j->s[j->p] == 'E')) return false;
    return true;
}

static bool token_uint(TokenJson *j, unsigned *out) {
    skip_ws(j);
    if (j->p >= j->n || j->s[j->p] < '0' || j->s[j->p] > '9') return false;
    if (j->s[j->p] == '0' && j->p + 1 < j->n && j->s[j->p + 1] >= '0' && j->s[j->p + 1] <= '9') return false;
    unsigned value = 0;
    while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') {
        unsigned d = j->s[j->p++] - '0';
        if (value > (UINT32_MAX - d) / 10) return false;
        value = value * 10 + d;
    }
    *out = value;
    return true;
}

bool oauth_token_parse(const char *json, size_t length, OAuthToken *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!json || !length || length > 8192) return false;
    TokenJson j = {(const unsigned char *)json, length, 0};
    char key[32], status[24] = {0}, type[24] = {0}, scope[128];
    unsigned seen = 0;
    bool have_status = false, have_access = false, have_type = false, have_expires = false;
    skip_ws(&j);
    if (j.p >= j.n || j.s[j.p++] != '{') goto fail;
    skip_ws(&j);
    if (j.p < j.n && j.s[j.p] == '}') goto fail;
    for (;;) {
        size_t key_len = 0;
        if (!token_string(&j, key, sizeof(key), &key_len)) goto fail;
        skip_ws(&j);
        if (j.p >= j.n || j.s[j.p++] != ':') goto fail;
        unsigned field = key_len == 6 && memcmp(key, "status", 6) == 0 ? 1u :
                         key_len == 12 && memcmp(key, "access_token", 12) == 0 ? 2u :
                         key_len == 10 && memcmp(key, "token_type", 10) == 0 ? 4u :
                         key_len == 10 && memcmp(key, "expires_in", 10) == 0 ? 8u :
                         key_len == 13 && memcmp(key, "refresh_token", 13) == 0 ? 16u :
                         key_len == 5 && memcmp(key, "scope", 5) == 0 ? 32u : 0u;
        if (field && (seen & field)) goto fail;
        seen |= field;
        if (field == 1) { if (!token_string(&j, status, sizeof(status), NULL)) goto fail; have_status = true; }
        else if (field == 2) { if (!token_string(&j, out->access, sizeof(out->access), NULL) || !out->access[0]) goto fail; have_access = true; }
        else if (field == 4) { if (!token_string(&j, type, sizeof(type), NULL)) goto fail; have_type = true; }
        else if (field == 8) { if (!token_uint(&j, &out->expires_in) || !out->expires_in || out->expires_in > 86400) goto fail; have_expires = true; }
        else if (field == 16) { if (!token_string(&j, out->refresh, sizeof(out->refresh), NULL)) goto fail; }
        else if (field == 32) { if (!token_string(&j, scope, sizeof(scope), NULL)) goto fail; }
        else if (!token_skip(&j, 1)) goto fail;
        skip_ws(&j);
        if (j.p < j.n && j.s[j.p] == '}') { j.p++; break; }
        if (j.p >= j.n || j.s[j.p++] != ',') goto fail;
    }
    skip_ws(&j);
    if (j.p != j.n || !have_status || !have_access || !have_type || !have_expires ||
        strcmp(status, "success") != 0 || strcmp(type, "Bearer") != 0) goto fail;
    return true;
fail:
    oauth_token_clear(out);
    return false;
}

void oauth_token_clear(OAuthToken *token) {
    if (token) oauth_pkce_clear(token, sizeof(*token));
}
