#pragma once

#include "oauth_pkce.h"

/* One authorization attempt, held in RAM only. Never serialize this object. */
typedef struct {
    char verifier[OAUTH_VERIFIER_CAPACITY];
    char state[OAUTH_STATE_CAPACITY];
    char code[OAUTH_MAX_CODE_LENGTH + 1];
    unsigned phase;
} OAuthSession;

/* The caller must obtain 48 bytes from a cryptographically secure RNG. */
bool oauth_session_begin(OAuthSession *session, const uint8_t *entropy, size_t entropy_length,
                         const char *redirect, size_t redirect_length,
                         char *url, size_t url_capacity);
OAuthCallbackStatus oauth_session_accept(OAuthSession *session, const char *query, size_t length);
void oauth_session_clear(OAuthSession *session);
