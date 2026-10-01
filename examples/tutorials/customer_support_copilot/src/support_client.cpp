#include <iostream>
#include <string>
#include <string_view>

#include "tutorial.h"
int main(int argc, char** argv) {
  std::string url = "http://127.0.0.1:8180";
  std::string ticket_resource;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (i + 1 >= argc) {
      std::cerr << "missing option value\n";
      return 2;
    }
    if (arg == "--coordinator-url") {
      url = argv[++i];
    } else if (arg == "--ticket-resource") {
      ticket_resource = argv[++i];
    } else {
      std::cerr << "unknown option\n";
      return 2;
    }
  }
  if (ticket_resource.empty()) {
    std::cerr << "--ticket-resource is required\n";
    return 2;
  }
  auto response = support_tutorial::Send(url, support_tutorial::TicketRequest(ticket_resource));
  if (!response.ok()) {
    std::cerr << response.error().message() << '\n';
    return 1;
  }
  std::cout << support_tutorial::Render(response.value());
  return 0;
}
