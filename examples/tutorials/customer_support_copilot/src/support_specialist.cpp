#include <cstdlib>
#include <string>

#include "tutorial.h"
int main(int argc, char** argv) {
  const std::string endpoint = argc > 1 ? std::string(argv[1]) : std::string(support_tutorial::kAnalystDefault);
  const char* configured_public_url = std::getenv("A2A_TUTORIAL_PUBLIC_URL");
  const char* configured_mcp_url = std::getenv("A2A_TUTORIAL_MCP_URL");
  const std::string public_url = configured_public_url != nullptr ? configured_public_url : "http://127.0.0.1:8181/a2a";
  return support_tutorial::RunAgentServer(
      endpoint, public_url, false, "",
      configured_mcp_url != nullptr ? configured_mcp_url : "http://127.0.0.1:8190/mcp");
}
