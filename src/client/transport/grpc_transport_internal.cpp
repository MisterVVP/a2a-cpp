// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "grpc_transport_internal.h"

#include <cctype>
#include <ranges>
#include <string>

#include "a2a/client/auth.h"
#include "a2a/core/extensions.h"
#include "request_headers.h"
namespace a2a::client::grpc_internal {
namespace {
constexpr const char* kInvalidInterface = "GrpcTransport requires a gRPC interface";
constexpr const char* kMissingUrl = "Resolved gRPC interface URL is required";
constexpr const char* kMissingStub = "gRPC stub is not configured";
constexpr const char* kTransportName = "grpc";
}  // namespace

ListTasksResponse DecodeListTasks(lf::a2a::v1::ListTasksResponse&& response) {
  ListTasksResponse result;
  result.tasks.reserve(static_cast<std::size_t>(response.tasks_size()));
  for (auto& task : *response.mutable_tasks()) {
    result.tasks.push_back(std::move(task));
  }
  result.next_page_token = std::move(*response.mutable_next_page_token());
  return result;
}

core::Result<std::unique_ptr<::grpc::ClientContext>> BuildContext(const ResolvedInterface& resolved_interface,
                                                                  bool configured,
                                                                  std::chrono::milliseconds default_timeout,
                                                                  const CallOptions& options) {
  if (resolved_interface.transport != PreferredTransport::kGrpc) {
    return core::Error::Validation(kInvalidInterface);
  }
  if (resolved_interface.url.empty()) {
    return core::Error::Validation(kMissingUrl);
  }
  if (!configured) {
    return core::Error::Internal(kMissingStub);
  }

  auto context = std::make_unique<::grpc::ClientContext>();
  const auto timeout = options.timeout.value_or(default_timeout);
  context->set_deadline(std::chrono::system_clock::now() + timeout);

  HeaderMap headers = options.headers;

  if (!options.extensions.empty()) {
    headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(headers);
  }
  if (options.credential_provider != nullptr) {
    const auto apply = ApplyCredentialProvider(*options.credential_provider, options.auth_context, &headers);
    if (!apply.ok()) {
      return apply.error();
    }
  }

  const auto version = internal::ApplyProtocolVersion(options, headers);
  if (!version.ok()) {
    return version.error();
  }

  for (const auto& [name, value] : headers) {
    std::string lowered_name = name;
    std::ranges::transform(lowered_name, lowered_name.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    context->AddMetadata(lowered_name, value);
  }
  return context;
}
core::Error BuildGrpcError(const ::grpc::Status& status) {
  core::Error error = core::Error::RemoteProtocol(status.error_message()).WithTransport(kTransportName);
  error = error.WithProtocolCode(std::to_string(static_cast<int>(status.error_code())));
  return error;
}
}  // namespace a2a::client::grpc_internal
