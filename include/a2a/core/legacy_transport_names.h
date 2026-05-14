#pragma once

#include <string_view>

namespace a2a::core::legacy_transport_names {

inline constexpr std::string_view kEndpointField = "endpoint";
inline constexpr std::string_view kPreferredTransportField = "preferredTransport";
inline constexpr std::string_view kAdditionalInterfacesField = "additionalInterfaces";
inline constexpr std::string_view kTransportField = "transport";
inline constexpr std::string_view kLegacyRestTransport = "REST";
inline constexpr std::string_view kLegacyJsonRpcTransport = "JSONRPC";
inline constexpr std::string_view kLegacyGrpcTransport = "GRPC";

}  // namespace a2a::core::legacy_transport_names
