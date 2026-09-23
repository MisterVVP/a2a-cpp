#pragma once
#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace support_tutorial {
inline constexpr std::string_view kCoordinatorDefault = "127.0.0.1:8180";
inline constexpr std::string_view kAnalystDefault = "127.0.0.1:8181";
inline constexpr std::string_view kRestPath = "/a2a";
[[nodiscard]] a2a::core::Result<std::string> ReadFile(std::string_view path);
[[nodiscard]] a2a::core::Result<lf::a2a::v1::SendMessageResponse> Send(std::string_view base_url,
                                                                       const lf::a2a::v1::SendMessageRequest& request);
[[nodiscard]] lf::a2a::v1::SendMessageRequest TicketRequest(std::string_view ticket, std::string_view unused);
[[nodiscard]] int RunAgentServer(std::string_view endpoint, std::string_view public_url, bool coordinator,
                                 std::string_view specialist_url);
[[nodiscard]] std::string Render(const lf::a2a::v1::SendMessageResponse& response);
}  // namespace support_tutorial
