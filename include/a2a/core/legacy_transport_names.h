#pragma once

#include <string_view>

namespace a2a::core::legacy_transport_names {

inline constexpr std::string_view kJsonRpc = "jsonrpc";
inline constexpr std::string_view kRest = "rest";
inline constexpr std::string_view kGrpc = "grpc";
inline constexpr std::string_view kEndpointField = "endpoint";
inline constexpr std::string_view kPreferredTransportField = "preferredTransport";
inline constexpr std::string_view kAdditionalInterfacesField = "additionalInterfaces";
inline constexpr std::string_view kTransportField = "transport";

}  // namespace a2a::core::legacy_transport_names
