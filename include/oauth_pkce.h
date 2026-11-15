#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Entropy must come from the console's cryptographic random service. A failed
 * service call must abort authorization, never fall back to a timer or rand(). */
#define OAUTH_VERIFIER_ENTROPY 32u
#define OAUTH_STATE_ENTROPY 16u
#define OAUTH_VERIFIER_CAPACITY 44u
#define OAUTH_CHALLENGE_CAPACITY 44u
#define OAUTH_STATE_CAPACITY 23u
#define OAUTH_AUTH_URL_CAPACITY 1024u
#define OAUTH_MAX_CODE_LENGTH 256u

typedef enum {
    OAUTH_CALLBACK_OK = 0,
    OAUTH_CALLBACK_DENIED,
    OAUTH_CALLBACK_INVALID,
    OAUTH_CALLBACK_WRONG_STATE,
    OAUTH_CALLBACK_OUTPUT_TOO_SMALL
} OAuthCallbackStatus;

bool oauth_pkce_verifier(const uint8_t *entropy, size_t length,
                         char output[OAUTH_VERIFIER_CAPACITY]);
bool oauth_pkce_challenge(const char *verifier, size_t length,
                          char output[OAUTH_CHALLENGE_CAPACITY]);
bool oauth_pkce_state(const uint8_t *entropy, size_t length,
                      char output[OAUTH_STATE_CAPACITY]);
bool oauth_pkce_state_matches(const char *expected, size_t expected_length,
                              const char *received, size_t received_length);
bool oauth_pkce_build_url(const char *redirect, size_t redirect_length,
                          const char *state, const char *challenge,
                          char *output, size_t output_capacity);
OAuthCallbackStatus oauth_pkce_parse_callback(const char *query, size_t query_length,
                                             const char *expected_state,
                                             char *code, size_t code_capacity);
void oauth_pkce_clear(void *buffer, size_t length);
