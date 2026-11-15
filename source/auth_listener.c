#include "auth_listener.h"
#include "auth_lan.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

static void close_client(AuthListener *server) {
    if (server->client >= 0) closesocket(server->client);
    server->client = -1;
    oauth_pkce_clear(server->line, sizeof(server->line));
    server->used = 0;
    server->connected_ms = 0;
}

void auth_listener_close(AuthListener *server) {
    if (!server) return;
    close_client(server);
    if (server->listener >= 0) closesocket(server->listener);
    server->listener = -1;
}

bool auth_listener_open(AuthListener *server, uint32_t ip) {
    if (!server) return false;
    memset(server, 0, sizeof(*server));
    server->client = server->listener = -1;
    uint32_t host = ntohl(ip);
    if (!((host >> 24) == 10 || (host >> 16) == 0xc0a8 ||
          ((host >> 20) == 0xac1))) return false;
    server->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listener < 0 || !nonblocking(server->listener)) goto fail;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(38741);
    address.sin_addr.s_addr = ip;
    if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server->listener, 1) < 0) goto fail;
    return true;
fail:
    auth_listener_close(server);
    return false;
}

OAuthCallbackStatus auth_listener_poll(AuthListener *server, OAuthSession *session, uint64_t now_ms) {
    if (!server || !session || server->listener < 0 || session->phase != 1)
        return OAUTH_CALLBACK_INVALID;
    if (server->client < 0) {
        server->client = accept(server->listener, NULL, NULL);
        if (server->client < 0) return OAUTH_CALLBACK_INVALID;
        if (!nonblocking(server->client)) { close_client(server); return OAUTH_CALLBACK_INVALID; }
        server->connected_ms = now_ms;
    }
    if (now_ms - server->connected_ms > 10000) {
        close_client(server);
        return OAUTH_CALLBACK_INVALID;
    }
    ssize_t received = recv(server->client, server->line + server->used,
                            sizeof(server->line) - 1 - server->used, 0);
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return OAUTH_CALLBACK_INVALID;
    if (received <= 0) { close_client(server); return OAUTH_CALLBACK_INVALID; }
    server->used += (size_t)received;
    for (size_t i = 0; i < server->used; i++) {
        if (server->line[i] == '\n') {
            OAuthCallbackStatus result = auth_lan_accept_line(session, server->line, i + 1);
            static const char response[] =
                "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
                "Retournez sur la 3DS pour voir le resultat.";
            (void)send(server->client, response, sizeof(response) - 1, 0);
            close_client(server);
            return result;
        }
    }
    if (server->used == sizeof(server->line) - 1) close_client(server);
    return OAUTH_CALLBACK_INVALID;
}
