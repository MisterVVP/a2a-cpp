// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_delivery.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protojson.h"
#include "a2a/server/server_utils.h"
#include "a2a/server/socket_utils.h"

namespace a2a::server {
namespace {

struct ParsedUrl final {
  std::string host;
  std::string port;
  std::string target;
  bool tls = false;
};

core::Result<ParsedUrl> ParseUrl(std::string_view url) {
  ParsedUrl parsed;
  std::string_view rest;
  if (url.starts_with(core::http::kHttpScheme)) {
    rest = url.substr(core::http::kHttpScheme.size());
    parsed.port = std::to_string(core::http::kDefaultHttpPort);
  } else if (url.starts_with(core::http::kHttpsScheme)) {
    rest = url.substr(core::http::kHttpsScheme.size());
    parsed.port = std::to_string(core::http::kDefaultHttpsPort);
    parsed.tls = true;
  } else {
    return core::Error::Validation("push notification URL must use http or https");
  }

  const auto path_pos = rest.find('/');
  std::string_view authority = path_pos == std::string_view::npos ? rest : rest.substr(0, path_pos);
  parsed.target = path_pos == std::string_view::npos ? "/" : std::string(rest.substr(path_pos));
  if (authority.empty()) {
    return core::Error::Validation("push notification URL host is required");
  }
  const auto port_pos = authority.rfind(':');
  if (port_pos != std::string_view::npos) {
    parsed.host = std::string(authority.substr(0, port_pos));
    parsed.port = std::string(authority.substr(port_pos + 1));
  } else {
    parsed.host = std::string(authority);
  }
  if (parsed.host.empty() || parsed.port.empty()) {
    return core::Error::Validation("push notification URL host and port are required");
  }
  return parsed;
}

core::Result<int> ConnectTcp(const ParsedUrl& url, std::chrono::milliseconds timeout) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  const int resolve_result = ::getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &addresses);
  if (resolve_result != 0) {
    return core::Error::Network("failed to resolve push notification host");
  }

  int fd = -1;
  for (addrinfo* candidate = addresses; candidate != nullptr; candidate = candidate->ai_next) {
    fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (fd < 0) {
      continue;
    }
#ifndef _WIN32
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / core::http::kMillisecondsPerSecond);
    tv.tv_usec = static_cast<long>((timeout.count() % core::http::kMillisecondsPerSecond) *
                                   core::http::kMicrosecondsPerMillisecond);
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) {
      break;
    }
    CloseSocketCrossPlatform(fd);
    fd = -1;
  }
  ::freeaddrinfo(addresses);
  if (fd < 0) {
    return core::Error::Network("failed to connect to push notification webhook");
  }
  return fd;
}

core::Result<void> SendAll(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto result = ::send(fd, data.data() + sent, data.size() - sent, 0);
    if (result <= 0) {
      return core::Error::Network("failed to send push notification request");
    }
    sent += static_cast<std::size_t>(result);
  }
  return {};
}

core::Result<std::string> ReceiveResponse(int fd) {
  std::string response;
  char buffer[core::http::kReceiveBufferSize]{};
  const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
  if (received <= 0) {
    return core::Error::Network("failed to receive push notification response");
  }
  response.assign(buffer, static_cast<std::size_t>(received));
  return response;
}

std::optional<int> ParseHttpStatus(std::string_view response) {
  const auto first_space = response.find(' ');
  if (first_space == std::string_view::npos || first_space + 4 > response.size()) {
    return std::nullopt;
  }
  int status = 0;
  const auto status_text = response.substr(first_space + 1, 3);
  const auto* begin = status_text.data();
  const auto* end = begin + status_text.size();
  const auto parsed = std::from_chars(begin, end, status);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return std::nullopt;
  }
  return status;
}

std::string BuildAuthorizationHeader(const lf::a2a::v1::TaskPushNotificationConfig& config) {
  if (config.authentication().scheme().empty()) {
    return {};
  }
  std::string header;
  header.reserve(config.authentication().scheme().size() + config.authentication().credentials().size() +
                 core::http::kAuthorizationHeaderReserveOverhead);
  header.append(core::http::kAuthorizationHeaderName);
  header.append(core::http::kHeaderNameValueSeparator);
  header.append(config.authentication().scheme());
  if (!config.authentication().credentials().empty()) {
    header.push_back(' ');
    header.append(config.authentication().credentials());
  }
  header.append(core::http::kLineTerminator);
  return header;
}

std::string BuildHeaderLine(std::string_view name, std::string_view value) {
  std::ostringstream header;
  header << name << core::http::kHeaderNameValueSeparator << value << core::http::kLineTerminator;
  return header.str();
}

struct RequestHttpVersion final {
  std::string_view value;
};

std::string BuildHttpRequest(const ParsedUrl& url, const lf::a2a::v1::TaskPushNotificationConfig& config,
                             std::string_view body, RequestHttpVersion http_version) {
  std::ostringstream request;
  request << "POST " << url.target << ' ' << http_version.value << core::http::kLineTerminator;
  request << BuildHeaderLine(core::http::kHostHeaderName, url.host);
  request << BuildHeaderLine(core::http::kContentTypeHeaderName, core::http::kContentTypeApplicationJson);
  request << BuildHeaderLine(core::http::kContentLengthHeaderName, std::to_string(body.size()));
  request << BuildHeaderLine(core::http::kConnectionCloseHeaderName, core::http::kConnectionCloseHeaderValue);
  request << BuildAuthorizationHeader(config);
  request << core::http::kLineTerminator;
  request << body;
  return request.str();
}

}  // namespace

HttpPushNotificationDeliveryClient::HttpPushNotificationDeliveryClient(HttpPushNotificationDeliveryOptions options)
    : options_(std::move(options)) {}

HttpPushNotificationDeliveryClient::HttpPushNotificationDeliveryClient(std::chrono::milliseconds timeout)
    : options_(HttpPushNotificationDeliveryOptions{.timeout = timeout}) {}

core::Result<PushDeliveryResult> DeliverHttpRequest(const PushDeliveryRequest& request, const ParsedUrl& url,
                                                    std::string_view body, std::string_view http_version,
                                                    std::chrono::milliseconds timeout) {
  const auto fd = ConnectTcp(url, timeout);
  if (!fd.ok()) {
    return fd.error();
  }
  const std::string http_request =
      BuildHttpRequest(url, request.config, body, RequestHttpVersion{.value = http_version});
  const auto send_result = SendAll(fd.value(), http_request);
  if (!send_result.ok()) {
    CloseSocketCrossPlatform(fd.value());
    return send_result.error();
  }
  const auto response = ReceiveResponse(fd.value());
  CloseSocketCrossPlatform(fd.value());
  if (!response.ok()) {
    return response.error();
  }
  const auto status = ParseHttpStatus(response.value());
  if (!status.has_value()) {
    return core::Error::RemoteProtocol("push notification webhook returned malformed HTTP response");
  }
  if (*status < core::http::kSuccessStatusMin || *status > core::http::kSuccessStatusMax) {
    PushDeliveryResult result{.http_status = *status, .error_message = "webhook returned non-2xx status"};
    return core::Error::RemoteProtocol(result.error_message);
  }
  return PushDeliveryResult{.http_status = *status, .error_message = {}};
}

core::Result<PushDeliveryResult> HttpPushNotificationDeliveryClient::Deliver(const PushDeliveryRequest& request) {
  const auto body = core::MessageToJson(request.payload);
  if (!body.ok()) {
    return body.error();
  }
  const auto url = ParseUrl(request.config.url());
  if (!url.ok()) {
    return url.error();
  }
  if (url.value().tls) {
    return core::Error::Network("HTTPS push notification delivery requires a TLS-enabled custom delivery client");
  }

  auto result = DeliverHttpRequest(request, url.value(), body.value(), options_.http_version, options_.timeout);
  if (result.ok() || options_.fallback_http_version.empty() ||
      options_.fallback_http_version == options_.http_version) {
    return result;
  }
  return DeliverHttpRequest(request, url.value(), body.value(), options_.fallback_http_version, options_.timeout);
}

}  // namespace a2a::server
