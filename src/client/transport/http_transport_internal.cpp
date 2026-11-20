// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "http_transport_internal.h"

#include <string>
#include <string_view>
#include <utility>

namespace a2a::client::http_transport_internal {
namespace {

constexpr std::string_view kUnsupportedMtls =
    "default libcurl HTTP requester does not support mTLS options; inject a custom requester for mTLS";

}  // namespace

a2a::http::Request ToSharedHttpRequest(const HttpRequest& request) {
  a2a::http::Request converted;
  converted.tls = request.tls;
  converted.method = request.method;
  converted.url = request.url;
  converted.body = request.body;
  converted.timeout = request.timeout;
  converted.headers.reserve(request.headers.size());
  for (const auto& [name, value] : request.headers) {
    converted.headers.push_back({.name = name, .value = value});
  }
  return converted;
}

HttpClientResponse ToClientHttpResponse(a2a::http::Response response) {
  HttpClientResponse converted;
  converted.status_code = response.status_code;
  converted.body = std::move(response.body);
  converted.headers.reserve(response.headers.size());
  for (auto& header : response.headers) {
    converted.headers.insert_or_assign(std::move(header.name), std::move(header.value));
  }
  return converted;
}

HttpRequester MakeHttpRequesterForClient(std::shared_ptr<http::Client> client) {
  return [client = std::move(client)](const HttpRequest& request) -> core::Result<HttpClientResponse> {
    if (request.mtls) {
      return core::Error::Validation(std::string(kUnsupportedMtls));
    }
    auto response = client->SendRequest(ToSharedHttpRequest(request));
    if (!response.ok()) {
      return response.error();
    }
    return ToClientHttpResponse(std::move(response.value()));
  };
}

OwnedHttpAsyncRequester MakeOwnedHttpAsyncRequester(std::shared_ptr<http::Client> client) {
  return [client = std::move(client)](
             HttpRequest input, core::Completion<HttpClientResponse> completion) -> core::Result<core::AsyncHandle> {
    if (input.mtls) {
      return core::Error::Validation(std::string(kUnsupportedMtls));
    }
    http::Request request;
    request.tls = std::move(input.tls);
    request.method = std::move(input.method);
    request.url = std::move(input.url);
    request.body = std::move(input.body);
    request.timeout = input.timeout;
    request.headers.reserve(input.headers.size());
    while (!input.headers.empty()) {
      auto header = input.headers.extract(input.headers.begin());
      request.headers.push_back({.name = std::move(header.key()), .value = std::move(header.mapped())});
    }
    return client->SendRequestAsync(std::move(request),
                                    [completion = std::move(completion)](core::Result<http::Response> response) {
                                      if (!response.ok()) {
                                        completion(response.error());
                                      } else {
                                        completion(ToClientHttpResponse(std::move(response.value())));
                                      }
                                    });
  };
}

}  // namespace a2a::client::http_transport_internal
