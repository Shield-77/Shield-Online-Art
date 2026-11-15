#include "oauth_exchange.h"
#include "deviantart_config.h"
#include <string.h>

typedef struct { char *out; size_t capacity, used; bool valid; } Form;
static void append(Form *f, const char *value, size_t length, bool encode) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length && f->valid; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) { f->valid = false; break; }
        bool plain = !encode || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
        size_t needed = plain ? 1 : 3;
        if (f->used + needed >= f->capacity) { f->valid = false; break; }
        if (plain) f->out[f->used++] = (char)c;
        else { f->out[f->used++] = '%'; f->out[f->used++] = hex[c >> 4]; f->out[f->used++] = hex[c & 15]; }
    }
}

bool oauth_exchange_form(const OAuthSession *s, const char *redirect, size_t redirect_length,
                         char *output, size_t capacity) {
    if (!output || !capacity) return false;
    output[0] = 0;
    if (!s || s->phase != 2 || !redirect || redirect_length < 9 || redirect_length > 400 ||
        memcmp(redirect, "https://", 8) != 0) return false;
    size_t code_length = strnlen(s->code, sizeof(s->code));
    size_t verifier_length = strnlen(s->verifier, sizeof(s->verifier));
    if (!code_length || code_length == sizeof(s->code) || verifier_length != OAUTH_VERIFIER_CAPACITY - 1) return false;
    Form f = {output, capacity, 0, true};
#define LITERAL(x) append(&f, x, sizeof(x) - 1, false)
    LITERAL("grant_type=authorization_code&client_id=" SHIELD_DEVIANTART_CLIENT_ID "&redirect_uri=");
    append(&f, redirect, redirect_length, true);
    LITERAL("&code="); append(&f, s->code, code_length, true);
    LITERAL("&code_verifier="); append(&f, s->verifier, verifier_length, true);
#undef LITERAL
    if (!f.valid) { oauth_pkce_clear(output, f.used); output[0] = 0; return false; }
    output[f.used] = 0;
    return true;
}

bool oauth_refresh_form(const char *refresh_token, char *output, size_t capacity) {
    if (!output || !capacity) return false;
    output[0] = 0;
    if (!refresh_token) return false;
    size_t token_length = strnlen(refresh_token, 2048);
    if (!token_length || token_length >= 2048) return false;
    Form f = {output, capacity, 0, true};
    static const char prefix[] =
        "grant_type=refresh_token&client_id=" SHIELD_DEVIANTART_CLIENT_ID "&refresh_token=";
    append(&f, prefix, sizeof(prefix) - 1, false);
    append(&f, refresh_token, token_length, true);
    if (!f.valid) {
        oauth_pkce_clear(output, f.used);
        output[0] = 0;
        return false;
    }
    output[f.used] = 0;
    return true;
}
