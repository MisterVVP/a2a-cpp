#pragma once
#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace job_tutorial {
inline constexpr char kCoordinatorDefault[] = "127.0.0.1:8080";
inline constexpr char kAnalystDefault[] = "127.0.0.1:8081";
inline constexpr char kRestPath[] = "/a2a";
[[nodiscard]] a2a::core::Result<std::string> ReadFile(std::string_view path);
[[nodiscard]] a2a::core::Result<lf::a2a::v1::SendMessageResponse> Send(std::string_view base_url,
                                                                       const lf::a2a::v1::SendMessageRequest& request);
[[nodiscard]] lf::a2a::v1::SendMessageRequest JobRequest(std::string_view resume, std::string_view job);
[[nodiscard]] int RunAgentServer(std::string_view endpoint, std::string_view public_url, bool coordinator,
                                 std::string_view specialist_url);
[[nodiscard]] std::string Render(const lf::a2a::v1::SendMessageResponse& response);
}  // namespace job_tutorial
