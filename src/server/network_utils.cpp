// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/network_utils.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <charconv>
#include <limits>
#include <sstream>
#include <string>

namespace a2a::server {
namespace {
constexpr int kDefaultMaxPort = 65535;
constexpr int kMinPort = 1;
}  // namespace

void CloseSocketCrossPlatform(int fd) noexcept {
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

std::string BuildHttpUrl(std::string_view host, int port, std::string_view path) {
  std::ostringstream stream;
  stream << "http://" << host << ':' << port << path;
  return stream.str();
}

a2a::core::Result<HostPortEndpoint> ParseHostPortEndpoint(std::string_view endpoint, int max_port) {
  if (max_port < kMinPort || max_port > kDefaultMaxPort) {
    return a2a::core::Error::Validation("max_port must be between 1 and 65535");
  }

  const auto pos = endpoint.rfind(':');
  if (pos == std::string_view::npos || pos == 0 || pos + 1 >= endpoint.size()) {
    std::ostringstream message;
    message << "Invalid endpoint format: '" << endpoint << "'. Expected <host>:<port>.";
    return a2a::core::Error::Validation(message.str());
  }

  const std::string_view port_text = endpoint.substr(pos + 1);
  int parsed_port = 0;
  const char* begin = port_text.data();
  const char* end = begin + port_text.size();
  const auto [ptr, error] = std::from_chars(begin, end, parsed_port);
  if (error != std::errc{} || ptr != end || parsed_port < kMinPort || parsed_port > max_port) {
    std::ostringstream message;
    message << "Invalid port in endpoint '" << endpoint << "': port must be between 1 and " << max_port << '.';
    return a2a::core::Error::Validation(message.str());
  }

  return HostPortEndpoint{.host = std::string(endpoint.substr(0, pos)), .port = parsed_port};
}

bool SetSocketNonBlocking(int fd) noexcept {
#ifdef _WIN32
  u_long mode = 1UL;
  return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool SetSocketNoDelay(int fd) noexcept {
  const int enabled = 1;
#ifdef _WIN32
  return setsockopt(static_cast<SOCKET>(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                    static_cast<int>(sizeof(enabled))) == 0;
#else
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, static_cast<socklen_t>(sizeof(enabled))) == 0;
#endif
}

}  // namespace a2a::server
