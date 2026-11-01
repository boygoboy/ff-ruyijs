/* Cleartext socket and HTTP/1 exchange.
   Same export names as necko_core.dll. The caller owns header policy.
   This file does not open the donor socket process and does not speak TLS. */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET necko_fd;
#define NECKO_EXPORT __declspec(dllexport)
#define NECKO_BAD_FD INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int necko_fd;
#define NECKO_EXPORT __attribute__((visibility("default")))
#define NECKO_BAD_FD (-1)
#define closesocket close
#endif

#define NECKO_OK 0
#define NECKO_FRAMING 1
#define NECKO_IO 2
#define NECKO_TIMEOUT 5
#define NECKO_CALLBACK 6

/* necko_exchange_read rejects chunk metadata when its length is at least
   0x10001. The failure text in the DLL is "Chunk metadata too large". */
#define NECKO_CHUNK_META_LIMIT 0x10000
#define NECKO_HEADER_LIMIT (1024 * 1024)

typedef int (*necko_read_fn)(void *, uint8_t *, uint32_t, uint32_t *);
typedef int (*necko_write_fn)(void *, const uint8_t *, uint32_t);
typedef int (*necko_cancel_fn)(void *);

typedef struct necko_socket {
    necko_fd fd;
    int interrupted;
} necko_socket;

typedef struct necko_exchange {
    necko_read_fn read_cb;
    necko_write_fn write_cb;
    void *closure;
    char *headers;
    unsigned char *pending;
    size_t pending_len;
    size_t pending_cap;
    int chunked;
    int until_close;
    int reusable;
    int complete;
    long content_length;
    long remaining;
    int in_chunk_data;
    long chunk_left;
    int skip_crlf;
} necko_exchange;

static void startup_once(void) {
#ifdef _WIN32
    static int ready = 0;
    WSADATA data;
    if (!ready) {
        if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
            ready = 1;
        }
    }
#endif
}

static int set_nonblock(necko_fd fd, int enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags) == 0 ? 0 : -1;
#endif
}

static int wait_fd(necko_fd fd, int write, uint32_t timeout_ms) {
    if (timeout_ms == 0xFFFFFFFFu) {
        timeout_ms = 0x7FFFFFFFu;
    }
#ifdef _WIN32
    fd_set set;
    struct timeval tv;
    int ready;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    ready = select(0, write ? NULL : &set, write ? &set : NULL, NULL,
                   timeout_ms == 0 ? &tv : &tv);
    if (ready == 0) {
        return NECKO_TIMEOUT;
    }
    if (ready < 0) {
        return NECKO_IO;
    }
    return NECKO_OK;
#else
    struct pollfd pollfd;
    int ready;
    pollfd.fd = fd;
    pollfd.events = write ? POLLOUT : POLLIN;
    ready = poll(&pollfd, 1, timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms);
    if (ready == 0) {
        return NECKO_TIMEOUT;
    }
    if (ready < 0) {
        return NECKO_IO;
    }
    if (pollfd.revents & (write ? POLLOUT : POLLIN)) {
        return NECKO_OK;
    }
    if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return NECKO_IO;
    }
    return NECKO_OK;
#endif
}

static int connect_one(struct addrinfo *info, uint32_t timeout_ms, necko_cancel_fn cancel,
                       void *cancel_closure, necko_fd *out) {
    necko_fd fd;
    int rc;
    fd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (fd == NECKO_BAD_FD) {
        return NECKO_IO;
    }
    if (set_nonblock(fd, 1) != 0) {
        closesocket(fd);
        return NECKO_IO;
    }
    rc = connect(fd, info->ai_addr, (int)info->ai_addrlen);
    if (rc != 0) {
#ifdef _WIN32
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL) {
            closesocket(fd);
            return NECKO_IO;
        }
#else
        if (errno != EINPROGRESS && errno != EINTR) {
            closesocket(fd);
            return NECKO_IO;
        }
#endif
        if (cancel != NULL && cancel(cancel_closure)) {
            closesocket(fd);
            return NECKO_IO;
        }
        rc = wait_fd(fd, 1, timeout_ms);
        if (rc != NECKO_OK) {
            closesocket(fd);
            return rc;
        }
        {
            int so_error = 0;
            socklen_t length = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &length) != 0 ||
                so_error != 0) {
                closesocket(fd);
                return NECKO_IO;
            }
        }
    }
    {
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));
    }
    if (set_nonblock(fd, 0) != 0) {
        closesocket(fd);
        return NECKO_IO;
    }
    *out = fd;
    return NECKO_OK;
}

static int ascii_case_equal(const char *left, const char *right, size_t length) {
    size_t index;
    for (index = 0; index < length; index++) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (tolower(a) != tolower(b)) {
            return 0;
        }
    }
    return 1;
}

static int contains_token(const char *value, const char *token) {
    size_t token_len = strlen(token);
    size_t index;
    size_t value_len;
    if (value == NULL) {
        return 0;
    }
    value_len = strcspn(value, "\r\n");
    if (value_len < token_len) {
        return 0;
    }
    for (index = 0; index + token_len <= value_len; index++) {
        if (ascii_case_equal(value + index, token, token_len)) {
            return 1;
        }
    }
    return 0;
}

static int open_socket(const char *host, uint16_t port, uint32_t timeout_ms,
                       necko_cancel_fn cancel, void *cancel_closure, necko_socket **out) {
    char service[8];
    struct addrinfo hints;
    struct addrinfo *list = NULL;
    struct addrinfo *item;
    necko_socket *socket_state;
    necko_fd fd = NECKO_BAD_FD;
    int rc = NECKO_IO;
    int saw_timeout = 0;
    if (host == NULL || host[0] == '\0') {
        return NECKO_FRAMING;
    }
    startup_once();
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    if (getaddrinfo(host, service, &hints, &list) != 0) {
        return NECKO_IO;
    }
    for (item = list; item != NULL; item = item->ai_next) {
        rc = connect_one(item, timeout_ms, cancel, cancel_closure, &fd);
        if (rc == NECKO_OK) {
            break;
        }
        if (rc == NECKO_TIMEOUT) {
            saw_timeout = 1;
        }
    }
    freeaddrinfo(list);
    if (rc != NECKO_OK) {
        return saw_timeout ? NECKO_TIMEOUT : rc;
    }
    socket_state = calloc(1, sizeof(*socket_state));
    if (socket_state == NULL) {
        closesocket(fd);
        return NECKO_IO;
    }
    socket_state->fd = fd;
    *out = socket_state;
    return NECKO_OK;
}

NECKO_EXPORT int necko_socket_open(const char *host, uint16_t port, uint32_t timeout_ms,
                                   necko_socket **out) {
    return open_socket(host, port, timeout_ms, NULL, NULL, out);
}

NECKO_EXPORT int necko_socket_open_cancellable(const char *host, uint16_t port,
                                              uint32_t timeout_ms, necko_cancel_fn cancel,
                                              void *cancel_closure, necko_socket **out) {
    return open_socket(host, port, timeout_ms, cancel, cancel_closure, out);
}

NECKO_EXPORT void necko_socket_interrupt(necko_socket *socket_state) {
    if (socket_state == NULL || socket_state->fd == NECKO_BAD_FD) {
        return;
    }
    socket_state->interrupted = 1;
#ifdef _WIN32
    shutdown(socket_state->fd, SD_BOTH);
#else
    shutdown(socket_state->fd, SHUT_RDWR);
#endif
}

NECKO_EXPORT void necko_socket_close(necko_socket *socket_state) {
    if (socket_state == NULL) {
        return;
    }
    if (socket_state->fd != NECKO_BAD_FD) {
        closesocket(socket_state->fd);
    }
    free(socket_state);
}

NECKO_EXPORT int necko_socket_idle(necko_socket *socket_state) {
    if (socket_state == NULL || socket_state->interrupted || socket_state->fd == NECKO_BAD_FD) {
        return 0;
    }
    return wait_fd(socket_state->fd, 0, 0) == NECKO_TIMEOUT ? 1 : 0;
}

static int transfer(necko_socket *socket_state, int write, uint8_t *buffer, uint32_t capacity,
                    uint32_t timeout_ms, uint32_t *moved) {
    int rc;
    int count;
    if (socket_state == NULL || socket_state->fd == NECKO_BAD_FD || socket_state->interrupted) {
        return NECKO_IO;
    }
    if (timeout_ms == 0) {
        if (set_nonblock(socket_state->fd, 1) != 0) {
            return NECKO_IO;
        }
    }
    rc = wait_fd(socket_state->fd, write, timeout_ms);
    if (rc != NECKO_OK) {
        if (timeout_ms == 0) {
            set_nonblock(socket_state->fd, 0);
        }
        return rc;
    }
    if (write) {
        count = send(socket_state->fd, (char *)buffer, (int)capacity, 0);
    } else {
        count = recv(socket_state->fd, (char *)buffer, (int)capacity, 0);
    }
    if (timeout_ms == 0) {
        set_nonblock(socket_state->fd, 0);
    }
    if (count < 0) {
        return NECKO_IO;
    }
    *moved = (uint32_t)count;
    return NECKO_OK;
}

NECKO_EXPORT int necko_socket_read(necko_socket *socket_state, uint8_t *buffer, uint32_t capacity,
                                   uint32_t timeout_ms, uint32_t *length) {
    if (length == NULL) {
        return NECKO_FRAMING;
    }
    *length = 0;
    return transfer(socket_state, 0, buffer, capacity, timeout_ms, length);
}

NECKO_EXPORT int necko_socket_write(necko_socket *socket_state, const uint8_t *buffer,
                                    uint32_t length, uint32_t timeout_ms) {
    uint32_t sent = 0;
    while (sent < length) {
        uint32_t count = 0;
        int rc = transfer(socket_state, 1, (uint8_t *)buffer + sent, length - sent, timeout_ms,
                          &count);
        if (rc != NECKO_OK) {
            return rc;
        }
        if (count == 0) {
            return NECKO_IO;
        }
        sent += count;
    }
    return NECKO_OK;
}

NECKO_EXPORT int necko_socket_write_some(necko_socket *socket_state, const uint8_t *buffer,
                                         uint32_t length, uint32_t *written) {
    if (written == NULL) {
        return NECKO_FRAMING;
    }
    *written = 0;
    return transfer(socket_state, 1, (uint8_t *)buffer, length, 0, written);
}

NECKO_EXPORT int necko_socket_peer(necko_socket *socket_state, char *host, uint32_t host_cap,
                                   uint16_t *port) {
    struct sockaddr_storage storage;
    socklen_t length = sizeof(storage);
    if (socket_state == NULL || host == NULL || host_cap < 8 || port == NULL) {
        return NECKO_FRAMING;
    }
    if (getpeername(socket_state->fd, (struct sockaddr *)&storage, &length) != 0) {
        return NECKO_IO;
    }
    if (storage.ss_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)&storage;
        if (inet_ntop(AF_INET, &ipv4->sin_addr, host, host_cap) == NULL) {
            return NECKO_IO;
        }
        *port = ntohs(ipv4->sin_port);
        return NECKO_OK;
    }
    if (storage.ss_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&storage;
        if (inet_ntop(AF_INET6, &ipv6->sin6_addr, host, host_cap) == NULL) {
            return NECKO_IO;
        }
        *port = ntohs(ipv6->sin6_port);
        return NECKO_OK;
    }
    return NECKO_IO;
}

static int append_bytes(necko_exchange *exchange, const unsigned char *data, size_t length) {
    if (exchange->pending_len + length > exchange->pending_cap) {
        size_t cap = exchange->pending_cap ? exchange->pending_cap * 2 : 4096;
        unsigned char *grown;
        while (cap < exchange->pending_len + length) {
            cap *= 2;
        }
        grown = realloc(exchange->pending, cap);
        if (grown == NULL) {
            return NECKO_IO;
        }
        exchange->pending = grown;
        exchange->pending_cap = cap;
    }
    memcpy(exchange->pending + exchange->pending_len, data, length);
    exchange->pending_len += length;
    return NECKO_OK;
}

static int pull(necko_exchange *exchange, size_t minimum) {
    while (exchange->pending_len < minimum) {
        uint8_t buffer[8192];
        uint32_t count = 0;
        int rc = exchange->read_cb(exchange->closure, buffer, sizeof(buffer), &count);
        if (rc != 0) {
            return rc;
        }
        if (count == 0) {
            return NECKO_IO;
        }
        rc = append_bytes(exchange, buffer, count);
        if (rc != NECKO_OK) {
            return rc;
        }
    }
    return NECKO_OK;
}

static int find_marker(const unsigned char *data, size_t length, const char *marker, size_t *at) {
    size_t mark = strlen(marker);
    size_t index;
    if (mark == 0 || length < mark) {
        return 0;
    }
    for (index = 0; index + mark <= length; index++) {
        if (memcmp(data + index, marker, mark) == 0) {
            *at = index;
            return 1;
        }
    }
    return 0;
}

static const char *header_value(const char *headers, const char *name) {
    size_t name_len = strlen(name);
    const char *line = headers;
    while (line != NULL && *line) {
        const char *end = strstr(line, "\r\n");
        size_t span = end == NULL ? strlen(line) : (size_t)(end - line);
        if (span > name_len + 1 && ascii_case_equal(line, name, name_len) && line[name_len] == ':') {
            return line + name_len + 1;
        }
        if (end == NULL) {
            break;
        }
        line = end + 2;
    }
    return NULL;
}

static int header_has_token(const char *headers, const char *name, const char *token) {
    return contains_token(header_value(headers, name), token);
}

static int take_status(necko_exchange *exchange) {
    size_t at = 0;
    char *block;
    char *line_end;
    int code;
    const char *length_text;
    char *length_end;
    if (!find_marker(exchange->pending, exchange->pending_len, "\r\n\r\n", &at)) {
        return 0;
    }
    if (at > NECKO_HEADER_LIMIT) {
        return -NECKO_FRAMING;
    }
    block = malloc(at + 1);
    if (block == NULL) {
        return -NECKO_IO;
    }
    memcpy(block, exchange->pending, at);
    block[at] = '\0';
    memmove(exchange->pending, exchange->pending + at + 4, exchange->pending_len - (at + 4));
    exchange->pending_len -= at + 4;
    line_end = strstr(block, "\r\n");
    if (line_end != NULL) {
        *line_end = '\0';
    }
    if (sscanf(block, "HTTP/%*s %d", &code) != 1) {
        free(block);
        return -NECKO_FRAMING;
    }
    if (line_end != NULL) {
        *line_end = '\r';
    }
    if (code >= 100 && code < 200) {
        free(block);
        return 0;
    }
    /* Final headers. Callers treat a negative result as an error code. */
    exchange->headers = block;
    exchange->chunked = header_has_token(block, "Transfer-Encoding", "chunked");
    exchange->until_close = header_has_token(block, "Connection", "close") ||
                            strncmp(block, "HTTP/1.1", 8) != 0;
    length_text = header_value(block, "Content-Length");
    if (!exchange->chunked && length_text != NULL) {
        while (*length_text == ' ' || *length_text == '\t') {
            length_text++;
        }
        exchange->content_length = strtol(length_text, &length_end, 10);
        if (length_end == length_text || exchange->content_length < 0) {
            return -NECKO_FRAMING;
        }
        exchange->remaining = exchange->content_length;
    } else if (!exchange->chunked) {
        exchange->until_close = 1;
        exchange->remaining = -1;
    }
    exchange->reusable = !exchange->until_close && strncmp(block, "HTTP/1.1", 8) == 0;
    return 1;
}

NECKO_EXPORT int necko_exchange_open(const char *method, const char *target, const char *headers,
                                    necko_read_fn read_cb, necko_write_fn write_cb, void *closure,
                                    necko_exchange **out) {
    necko_exchange *exchange;
    char *request = NULL;
    size_t method_len;
    size_t target_len;
    size_t header_len;
    size_t total;
    int rc;
    if (method == NULL || target == NULL || headers == NULL || read_cb == NULL || write_cb == NULL ||
        out == NULL) {
        return NECKO_FRAMING;
    }
    if (strchr(method, '\n') || strchr(method, '\r') || strchr(target, '\n') || strchr(target, '\r')) {
        return NECKO_FRAMING;
    }
    method_len = strlen(method);
    target_len = strlen(target);
    header_len = strlen(headers);
    if (method_len == 0 || target_len == 0) {
        return NECKO_FRAMING;
    }
    if (header_len >= 2 && memcmp(headers + header_len - 2, "\r\n", 2) != 0) {
        return NECKO_FRAMING;
    }
    if (header_len == 1) {
        return NECKO_FRAMING;
    }
    total = method_len + 1 + target_len + strlen(" HTTP/1.1\r\n") + header_len + 2;
    request = malloc(total + 1);
    if (request == NULL) {
        return NECKO_IO;
    }
    snprintf(request, total + 1, "%s %s HTTP/1.1\r\n%s", method, target, headers);
    memcpy(request + strlen(request), "\r\n", 3);
    exchange = calloc(1, sizeof(*exchange));
    if (exchange == NULL) {
        free(request);
        return NECKO_IO;
    }
    exchange->read_cb = read_cb;
    exchange->write_cb = write_cb;
    exchange->closure = closure;
    exchange->content_length = -1;
    exchange->remaining = -1;
    rc = write_cb(closure, (const uint8_t *)request, (uint32_t)strlen(request));
    free(request);
    if (rc != 0) {
        free(exchange);
        return rc;
    }
    *out = exchange;
    return NECKO_OK;
}

NECKO_EXPORT int necko_exchange_write(necko_exchange *exchange, const uint8_t *data, uint32_t length) {
    if (exchange == NULL || exchange->write_cb == NULL) {
        return NECKO_FRAMING;
    }
    if (length == 0) {
        return NECKO_OK;
    }
    return exchange->write_cb(exchange->closure, data, length);
}

NECKO_EXPORT int necko_exchange_receive(necko_exchange *exchange) {
    int taken;
    if (exchange == NULL) {
        return NECKO_FRAMING;
    }
    while (exchange->headers == NULL) {
        size_t at = 0;
        int rc;
        if (!find_marker(exchange->pending, exchange->pending_len, "\r\n\r\n", &at)) {
            if (exchange->pending_len > NECKO_HEADER_LIMIT) {
                return NECKO_FRAMING;
            }
            rc = pull(exchange, exchange->pending_len + 1);
            if (rc != NECKO_OK) {
                return rc;
            }
            continue;
        }
        taken = take_status(exchange);
        if (taken < 0) {
            return -taken;
        }
    }
    return NECKO_OK;
}

NECKO_EXPORT const char *necko_exchange_headers(necko_exchange *exchange) {
    if (exchange == NULL || exchange->headers == NULL) {
        return "";
    }
    return exchange->headers;
}

static int consume(necko_exchange *exchange, size_t count) {
    if (count > exchange->pending_len) {
        return NECKO_IO;
    }
    memmove(exchange->pending, exchange->pending + count, exchange->pending_len - count);
    exchange->pending_len -= count;
    return NECKO_OK;
}

static int read_chunk_size(necko_exchange *exchange) {
    size_t at = 0;
    char line[NECKO_CHUNK_META_LIMIT + 1];
    char *end = NULL;
    unsigned long size;
    if (exchange->pending_len >= NECKO_CHUNK_META_LIMIT + 1 &&
        !find_marker(exchange->pending, exchange->pending_len, "\r\n", &at)) {
        return -NECKO_FRAMING;
    }
    if (!find_marker(exchange->pending, exchange->pending_len, "\r\n", &at)) {
        return 0;
    }
    if (at >= NECKO_CHUNK_META_LIMIT + 1) {
        return -NECKO_FRAMING;
    }
    memcpy(line, exchange->pending, at);
    line[at] = '\0';
    size = strtoul(line, &end, 16);
    if (end == line) {
        return -NECKO_FRAMING;
    }
    if (consume(exchange, at + 2) != NECKO_OK) {
        return -NECKO_IO;
    }
    if (size == 0) {
        exchange->complete = 1;
        return 1;
    }
    exchange->in_chunk_data = 1;
    exchange->chunk_left = (long)size;
    return 1;
}

NECKO_EXPORT int necko_exchange_read(necko_exchange *exchange, uint8_t *buffer, uint32_t capacity,
                                    uint32_t *length) {
    uint32_t filled = 0;
    if (length == NULL || exchange == NULL) {
        return NECKO_FRAMING;
    }
    *length = 0;
    if (exchange->headers == NULL) {
        return NECKO_FRAMING;
    }
    while (filled < capacity && !exchange->complete) {
        int rc;
        if (exchange->chunked) {
            if (exchange->skip_crlf) {
                if (exchange->pending_len < 2) {
                    rc = pull(exchange, 2);
                    if (rc != NECKO_OK) {
                        return rc;
                    }
                }
                if (exchange->pending_len < 2 || exchange->pending[0] != '\r' ||
                    exchange->pending[1] != '\n') {
                    return NECKO_FRAMING;
                }
                consume(exchange, 2);
                exchange->skip_crlf = 0;
                exchange->in_chunk_data = 0;
            }
            if (!exchange->in_chunk_data) {
                size_t at = 0;
                int parsed;
                if (!find_marker(exchange->pending, exchange->pending_len, "\r\n", &at)) {
                    rc = pull(exchange, exchange->pending_len + 1);
                    if (rc != NECKO_OK) {
                        return rc;
                    }
                }
                parsed = read_chunk_size(exchange);
                if (parsed < 0) {
                    return -parsed;
                }
                if (parsed == 0) {
                    continue;
                }
                if (exchange->complete) {
                    break;
                }
            }
            if (exchange->chunk_left > 0) {
                uint32_t room = capacity - filled;
                uint32_t take;
                if ((unsigned long)exchange->chunk_left < room) {
                    room = (uint32_t)exchange->chunk_left;
                }
                if (exchange->pending_len == 0) {
                    rc = pull(exchange, 1);
                    if (rc != NECKO_OK) {
                        return rc;
                    }
                }
                take = room;
                if ((size_t)take > exchange->pending_len) {
                    take = (uint32_t)exchange->pending_len;
                }
                memcpy(buffer + filled, exchange->pending, take);
                consume(exchange, take);
                filled += take;
                exchange->chunk_left -= take;
                if (exchange->chunk_left == 0) {
                    exchange->skip_crlf = 1;
                }
            }
            continue;
        }
        if (exchange->remaining == 0) {
            exchange->complete = 1;
            break;
        }
        if (exchange->pending_len == 0) {
            rc = pull(exchange, 1);
            if (rc != NECKO_OK) {
                if (exchange->until_close && filled > 0) {
                    exchange->complete = 1;
                    exchange->reusable = 0;
                    break;
                }
                if (exchange->until_close) {
                    exchange->complete = 1;
                    exchange->reusable = 0;
                    break;
                }
                return rc;
            }
        }
        {
            uint32_t take = capacity - filled;
            if ((size_t)take > exchange->pending_len) {
                take = (uint32_t)exchange->pending_len;
            }
            if (exchange->remaining > 0 && (long)take > exchange->remaining) {
                take = (uint32_t)exchange->remaining;
            }
            memcpy(buffer + filled, exchange->pending, take);
            consume(exchange, take);
            filled += take;
            if (exchange->remaining > 0) {
                exchange->remaining -= take;
                if (exchange->remaining == 0) {
                    exchange->complete = 1;
                }
            }
        }
    }
    *length = filled;
    return NECKO_OK;
}

NECKO_EXPORT int necko_exchange_reusable(necko_exchange *exchange) {
    if (exchange == NULL) {
        return 0;
    }
    return exchange->complete && exchange->reusable;
}

NECKO_EXPORT void necko_exchange_close(necko_exchange *exchange) {
    if (exchange == NULL) {
        return;
    }
    free(exchange->headers);
    free(exchange->pending);
    free(exchange);
}
