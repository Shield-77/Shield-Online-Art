#pragma once
#include "oauth_session.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The caller owns an initialized SOC service, the session and the clock.
 * This listener holds at most one client and never blocks the UI thread. */
typedef struct {
    int listener, client;
    uint64_t connected_ms;
    char line[1025];
    size_t used;
} AuthListener;

bool auth_listener_open(AuthListener *server, uint32_t bind_ip_network_order);
OAuthCallbackStatus auth_listener_poll(AuthListener *server, OAuthSession *session, uint64_t now_ms);
void auth_listener_close(AuthListener *server);
