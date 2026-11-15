#pragma once
#include "oauth_session.h"

/* Parses one bounded HTTP request line only; the caller owns the listener,
 * must impose timeouts and close the socket. No token is sent over this link. */
OAuthCallbackStatus auth_lan_accept_line(OAuthSession *session, const char *line, size_t length);
