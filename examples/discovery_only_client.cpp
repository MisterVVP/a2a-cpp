#include <iostream>

#include "a2a/client/discovery.h"
#include "a2a/core/protojson.h"
#include "example_support.h"

int main() {
  const auto card_json = a2a::core::MessageToJson(
      a2a::examples::BuildRestAgentCard("Discovery Example Agent", "http://agent.local/a2a"));
  if (!card_json.ok()) {
    std::cerr << "failed to prepare fixture: " << card_json.error().message() << '\n';
    return 1;
  }

  a2a::client::DiscoveryClient discovery([&card_json](std::string_view url) {
    (void)url;
    return a2a::client::HttpResponse{.status_code = 200, .body = card_json.value()};
  });

  const auto card = discovery.Fetch("http://agent.local");
  if (!card.ok()) {
    std::cerr << "discovery failed: " << card.error().message() << '\n';
    return 1;
  }

  std::cout << "discovered agent: " << card.value().name() << '\n';
  return 0;
}
