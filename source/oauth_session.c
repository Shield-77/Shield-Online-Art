#include "oauth_session.h"

void oauth_session_clear(OAuthSession *session) {
    if (session) oauth_pkce_clear(session, sizeof(*session));
}

bool oauth_session_begin(OAuthSession *session, const uint8_t *entropy, size_t entropy_length,
                         const char *redirect, size_t redirect_length,
                         char *url, size_t url_capacity) {
    char challenge[OAUTH_CHALLENGE_CAPACITY] = {0};
    bool ok = false;
    if (!session) return false;
    oauth_session_clear(session);
    if (url && url_capacity) url[0] = '\0';
    if (!entropy || entropy_length != OAUTH_VERIFIER_ENTROPY + OAUTH_STATE_ENTROPY)
        goto done;
    if (!oauth_pkce_verifier(entropy, OAUTH_VERIFIER_ENTROPY, session->verifier) ||
        !oauth_pkce_state(entropy + OAUTH_VERIFIER_ENTROPY, OAUTH_STATE_ENTROPY, session->state) ||
        !oauth_pkce_challenge(session->verifier, OAUTH_VERIFIER_CAPACITY - 1, challenge) ||
        !oauth_pkce_build_url(redirect, redirect_length, session->state, challenge, url, url_capacity))
        goto done;
    session->phase = 1;
    ok = true;
done:
    oauth_pkce_clear(challenge, sizeof(challenge));
    if (!ok) oauth_session_clear(session);
    return ok;
}

OAuthCallbackStatus oauth_session_accept(OAuthSession *session, const char *query, size_t length) {
    OAuthCallbackStatus status;
    if (!session || session->phase != 1) return OAUTH_CALLBACK_INVALID;
    status = oauth_pkce_parse_callback(query, length, session->state, session->code, sizeof(session->code));
    if (status == OAUTH_CALLBACK_OK) session->phase = 2;
    else if (status == OAUTH_CALLBACK_DENIED) oauth_session_clear(session);
    return status;
}
