#pragma once
#include "oauth_session.h"

/* Creates the form body for an HTTPS POST. It must never be logged or put in
 * a URL. The caller clears the output immediately after the request. */
bool oauth_exchange_form(const OAuthSession *session, const char *redirect,
                         size_t redirect_length, char *output, size_t capacity);

/* Creates the OAuth refresh-token grant body. The caller clears the body
 * immediately after the request. */
bool oauth_refresh_form(const char *refresh_token, char *output, size_t capacity);
