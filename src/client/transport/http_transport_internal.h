// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <memory>

#include "a2a/client/http_json_transport.h"
#include "a2a/http/http_client.h"

namespace a2a::client::http_transport_internal {

using OwnedHttpAsyncRequester = std::function<core::Result<core::AsyncHandle>(
    HttpRequest request, core::Completion<HttpClientResponse> completion)>;

[[nodiscard]] a2a::http::Request ToSharedHttpRequest(const HttpRequest& request);
[[nodiscard]] HttpClientResponse ToClientHttpResponse(a2a::http::Response response);
[[nodiscard]] HttpRequester MakeHttpRequesterForClient(std::shared_ptr<http::Client> client);
[[nodiscard]] OwnedHttpAsyncRequester MakeOwnedHttpAsyncRequester(std::shared_ptr<http::Client> client);

}  // namespace a2a::client::http_transport_internal
