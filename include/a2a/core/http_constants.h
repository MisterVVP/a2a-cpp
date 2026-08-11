// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace a2a::core::http {

inline constexpr char kCarriageReturn = static_cast<char>(0x0D);
inline constexpr char kLineFeed = static_cast<char>(0x0A);
inline constexpr std::array<char, 2> kLineTerminatorBytes{kCarriageReturn, kLineFeed};
inline constexpr std::array<char, 4> kHeaderDelimiterBytes{kCarriageReturn, kLineFeed, kCarriageReturn, kLineFeed};
inline constexpr std::string_view kLineTerminator{kLineTerminatorBytes.data(), kLineTerminatorBytes.size()};
inline constexpr std::string_view kHeaderDelimiter{kHeaderDelimiterBytes.data(), kHeaderDelimiterBytes.size()};
inline constexpr std::string_view kHeaderNameValueSeparator = ": ";
inline constexpr char kContentTypeParameterSeparator = ';';

inline constexpr std::string_view kMethodGet = "GET";
inline constexpr std::string_view kMethodPost = "POST";
inline constexpr std::string_view kMethodDelete = "DELETE";

inline constexpr std::string_view kHttpScheme = "http://";
inline constexpr std::string_view kHttpsScheme = "https://";
inline constexpr std::string_view kHttpVersion11 = "HTTP/1.1";
inline constexpr std::string_view kHttpVersion20 = "HTTP/2.0";
inline constexpr std::string_view kHttpVersion30 = "HTTP/3.0";
inline constexpr std::string_view kDefaultPushHttpVersion = kHttpVersion20;
inline constexpr std::string_view kContentTypeApplicationJson = "application/json";
inline constexpr std::string_view kContentTypeTextEventStream = "text/event-stream";
inline constexpr std::string_view kSseHeartbeat = ": keep-alive\n\n";
inline constexpr std::chrono::seconds kSseHeartbeatInterval{15};
inline constexpr std::string_view kContentLengthHeader = "content-length";
inline constexpr std::string_view kContentLengthHeaderName = "Content-Length";
inline constexpr std::string_view kConnectionHeader = "connection";
inline constexpr std::string_view kConnectionHeaderName = "Connection";
inline constexpr std::string_view kConnectionCloseHeaderName = kConnectionHeaderName;
inline constexpr std::string_view kConnectionCloseHeaderValue = "close";
inline constexpr std::string_view kConnectionKeepAliveHeaderValue = "keep-alive";
inline constexpr std::string_view kTransferEncodingHeader = "transfer-encoding";
inline constexpr std::string_view kTransferEncodingHeaderName = "Transfer-Encoding";
inline constexpr std::string_view kTransferEncodingChunked = "chunked";
inline constexpr std::string_view kHostHeaderName = "Host";
inline constexpr std::string_view kContentTypeHeaderName = "Content-Type";
inline constexpr std::string_view kCacheControlHeaderName = "Cache-Control";
inline constexpr std::string_view kCacheControlNoCache = "no-cache";
inline constexpr std::string_view kAuthorizationHeaderName = "Authorization";

inline constexpr int kDefaultHttpPort = 80;
inline constexpr int kDefaultHttpsPort = 443;
inline constexpr int kStatusOk = 200;
inline constexpr int kStatusCreated = 201;
inline constexpr int kStatusAccepted = 202;
inline constexpr int kStatusNoContent = 204;
inline constexpr int kStatusBadRequest = 400;
inline constexpr int kStatusUnauthorized = 401;
inline constexpr int kStatusForbidden = 403;
inline constexpr int kStatusNotFound = 404;
inline constexpr int kStatusMethodNotAllowed = 405;
inline constexpr int kStatusConflict = 409;
inline constexpr int kStatusPayloadTooLarge = 413;
inline constexpr int kStatusUnsupportedMediaType = 415;
inline constexpr int kStatusUnprocessableEntity = 422;
inline constexpr int kStatusTooManyRequests = 429;
inline constexpr int kStatusInternalServerError = 500;
inline constexpr int kStatusNotImplemented = 501;
inline constexpr int kStatusBadGateway = 502;
inline constexpr int kStatusServiceUnavailable = 503;
inline constexpr int kSuccessStatusMin = 200;
inline constexpr int kSuccessStatusMax = 299;
inline constexpr long kMillisecondsPerSecond = 1000;
inline constexpr long kMicrosecondsPerMillisecond = 1000;
inline constexpr std::size_t kAuthorizationHeaderReserveOverhead = 18;
inline constexpr std::size_t kReceiveBufferSize = 1024;
inline constexpr std::size_t kDecimalBase = 10;

}  // namespace a2a::core::http
