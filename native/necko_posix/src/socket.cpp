// POSIX implementation of the upstream standalone adapter's socket ABI.
#include "core.h"
#include <sys/socket.h>
#include <poll.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <memory>
namespace {
struct Socket { int fd = -1; ~Socket() { if (fd >= 0) close(fd); } };
using Clock = std::chrono::steady_clock;
int Wait(int fd, short events, Clock::time_point end, Cancel cancel = nullptr, void* user = nullptr) {
  for (;;) {
    if (cancel && cancel(user)) return 6;
    auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
    pollfd item{fd, events, 0};
    int result = poll(&item, 1, int(std::clamp<int64_t>(remain, 0, cancel ? 50 : INT32_MAX)));
    if (result > 0) return item.revents & (POLLERR | POLLNVAL) ? 6 : 0;
    if (result < 0 && errno != EINTR) return 6;
    if (Clock::now() >= end) return 5;
  }
}
Clock::time_point Deadline(uint32_t ms) { return Clock::now() + std::chrono::milliseconds(ms); }
int OpenStream(int family) {
#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  return ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
  const int fd = ::socket(family, SOCK_STREAM, 0);
  if (fd < 0) return fd;
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    close(fd);
    return -1;
  }
#ifdef SO_NOSIGPIPE
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return fd;
#endif
}
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
}
extern "C" {
int necko_socket_open_cancellable(const char* host, uint16_t port, uint32_t ms,
                                Cancel cancel, void* user, void** output) {
  if (!host || !output) return 1;
  *output = nullptr;
  addrinfo hints{}, *addresses = nullptr;
  hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  const auto service = std::to_string(port);
  const auto end = Deadline(ms);
  if (getaddrinfo(host, service.c_str(), &hints, &addresses)) return 6;
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(addresses, freeaddrinfo);
  int error = 6;
  for (auto* address = addresses; address; address = address->ai_next) {
    if (cancel && cancel(user)) return 6;
    auto socket = std::make_unique<Socket>();
    socket->fd = OpenStream(address->ai_family);
    if (socket->fd < 0) continue;
    if (connect(socket->fd, address->ai_addr, address->ai_addrlen)) {
      if (errno != EINPROGRESS) continue;
      error = Wait(socket->fd, POLLOUT, end, cancel, user);
      if (error) { if (error == 5) break; continue; }
      int code = 0; socklen_t size = sizeof(code);
      if (getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &code, &size) || code) continue;
    }
    *output = socket.release(); return 0;
  }
  return error;
}
int necko_socket_open(const char* host, uint16_t port, uint32_t ms, void** output) {
  return necko_socket_open_cancellable(host, port, ms, nullptr, nullptr, output);
}
void necko_socket_close(void* value) { delete static_cast<Socket*>(value); }
void necko_socket_interrupt(void* value) { if (value) shutdown(static_cast<Socket*>(value)->fd, SHUT_RDWR); }
int necko_socket_idle(void* value) {
  if (!value) return 0;
  char byte; const auto n = recv(static_cast<Socket*>(value)->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
  return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}
int necko_socket_read(void* value, uint8_t* data, uint32_t size, uint32_t ms, uint32_t* used) {
  if (!value || !used) return 1;
  *used = 0; if (!size) return 0;
  const auto end = Deadline(ms); const auto fd = static_cast<Socket*>(value)->fd;
  for (;;) {
    auto n = recv(fd, data, size, 0);
    if (n >= 0) { *used = uint32_t(n); return 0; }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return 6;
    if (int error = Wait(fd, POLLIN, end)) return error;
  }
}
int necko_socket_write_some(void* value, const uint8_t* data, uint32_t size, uint32_t* used) {
  if (!value || !used) return 1;
  *used = 0;
  ssize_t n;
  do { n = send(static_cast<Socket*>(value)->fd, data, size, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
  if (n >= 0) { *used = uint32_t(n); return 0; }
  return errno == EAGAIN || errno == EWOULDBLOCK ? 5 : 6;
}
int necko_socket_write(void* value, const uint8_t* data, uint32_t size, uint32_t ms) {
  const auto end = Deadline(ms);
  while (size) {
    uint32_t used; int error = necko_socket_write_some(value, data, size, &used);
    if (error && error != 5) return error;
    if (error == 5) { if (int ready = Wait(static_cast<Socket*>(value)->fd, POLLOUT, end)) return ready; }
    else { if (!used) return 6; data += used; size -= used; }
  }
  return 0;
}
int necko_socket_peer(void* value, char* host, uint32_t size, uint16_t* port) {
  if (!value || !port) return 1;
  sockaddr_storage address{}; socklen_t count = sizeof(address); char service[16]{};
  if (getpeername(static_cast<Socket*>(value)->fd, reinterpret_cast<sockaddr*>(&address), &count) ||
      getnameinfo(reinterpret_cast<sockaddr*>(&address), count, host, size, service, sizeof(service),
                  NI_NUMERICHOST | NI_NUMERICSERV)) return 6;
  *port = uint16_t(std::stoi(service)); return 0;
}
}
