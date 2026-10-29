// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_request_context.h"

#include <utility>

#include "a2a/core/extensions.h"
#include "a2a/core/version.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/version_header_validator.h"

namespace a2a::server {
namespace {
constexpr std::string_view kExtensionsMetadataKey = "a2a-extensions";
}
void AddGrpcActivatedExtensions(::grpc::ServerContextBase* context,
                                const std::vector<std::string>& activated_extensions) {
  if (context == nullptr || activated_extensions.empty()) {
    return;
  }
  context->AddTrailingMetadata(std::string(kExtensionsMetadataKey), core::Extensions::Format(activated_extensions));
}

core::Result<ValidatedGrpcRequest> BuildGrpcRequestContext(const ::grpc::ServerContextBase& context,
                                                           const RequiredExtensionsValidator& extensions_validator) {
  RequestContext request_context;
  request_context.remote_address = context.peer();

  for (const auto& [key_ref, value_ref] : context.client_metadata()) {
    const std::string key(key_ref.data(), key_ref.length());
    const std::string value(value_ref.data(), value_ref.length());
    request_context.client_headers[key] = value;
  }
  request_context.auth_metadata = ExtractAuthMetadata(request_context.client_headers);
  const auto version = VersionHeaderValidator{}.Validate(request_context.client_headers);
  if (!version.ok()) {
    return version.error().WithTransport(std::string(GrpcServerTransport::kTransportName));
  }
  request_context.protocol_version = core::Version::kProtocolVersion;

  const auto extensions = extensions_validator.Validate(request_context.client_headers);
  if (!extensions.ok()) {
    return extensions.error().WithTransport(std::string(GrpcServerTransport::kTransportName));
  }
  return ValidatedGrpcRequest{.request_context = std::move(request_context),
                              .activated_extensions = extensions.value()};
}
}  // namespace a2a::server
