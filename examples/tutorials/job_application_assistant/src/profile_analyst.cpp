#include <cstdlib>
#include <string>

#include "tutorial.h"
int main(int argc, char** argv) {
  const std::string endpoint = argc > 1 ? std::string(argv[1]) : std::string(job_tutorial::kAnalystDefault);
  const char* configured_public_url = std::getenv("A2A_TUTORIAL_PUBLIC_URL");
  const std::string public_url = configured_public_url != nullptr ? configured_public_url : "http://127.0.0.1:8081/a2a";
  return job_tutorial::RunAgentServer(endpoint, public_url, false, "");
}
