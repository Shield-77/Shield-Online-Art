#include "auth_lan.h"
#include <string.h>

OAuthCallbackStatus auth_lan_accept_line(OAuthSession *session, const char *line, size_t length) {
    const char prefix[] = "GET /callback?";
    const char http10[] = " HTTP/1.0\r\n";
    const char http11[] = " HTTP/1.1\r\n";
    const size_t prefix_len = sizeof(prefix) - 1;
    const size_t suffix_len = sizeof(http10) - 1;
    if (!session || !line || length > 1024 || length <= prefix_len + suffix_len ||
        memcmp(line, prefix, prefix_len) != 0 ||
        (memcmp(line + length - suffix_len, http10, suffix_len) != 0 &&
         memcmp(line + length - suffix_len, http11, suffix_len) != 0))
        return OAUTH_CALLBACK_INVALID;
    const char *query = line + prefix_len;
    const size_t query_len = length - prefix_len - suffix_len;
    for (size_t i = 0; i < query_len; i++) {
        unsigned char c = (unsigned char)query[i];
        if (c <= 0x20 || c >= 0x7f || c == '#') return OAUTH_CALLBACK_INVALID;
    }
    return oauth_session_accept(session, query, query_len);
}
