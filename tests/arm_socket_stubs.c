/* Test-only fake sockets. No part of this file is linked into the 3DS CIA. */
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>

static int error_value, accepts, closes;
static char input[1024];
static size_t available, cursor;
int *__errno(void) { return &error_value; }
void mock_net_reset(void) { error_value = accepts = closes = 0; available = cursor = 0; }
void mock_net_input(const char *data, size_t length) {
    if (length > sizeof(input)) length = sizeof(input);
    memcpy(input, data, length); available = length; cursor = 0;
}
int mock_net_closes(void) { return closes; }
int socket(int d, int t, int p) { (void)d; (void)t; (void)p; return 7; }
int fcntl(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
int bind(int fd, const struct sockaddr *addr, socklen_t len) { (void)fd; (void)addr; (void)len; return 0; }
int listen(int fd, int backlog) { (void)fd; (void)backlog; return 0; }
int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    (void)fd; (void)addr; (void)len;
    if (accepts++) { error_value = EAGAIN; return -1; }
    return 8;
}
ssize_t recv(int fd, void *buf, size_t len, int flags) {
    (void)fd; (void)flags;
    if (cursor == available) { error_value = EAGAIN; return -1; }
    size_t n = available - cursor;
    if (n > len) n = len;
    memcpy(buf, input + cursor, n); cursor += n; return (ssize_t)n;
}
ssize_t send(int fd, const void *buf, size_t len, int flags) {
    (void)fd; (void)buf; (void)flags; return (ssize_t)len;
}
int closesocket(int fd) { (void)fd; closes++; return 0; }
