#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <iostream>
#include <memory>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/grpc_transport.h"

int main() {
  a2a::client::ResolvedInterface iface;
  iface.transport = a2a::client::PreferredTransport::kGrpc;
  iface.url = "localhost:50051";

  auto channel = grpc::CreateChannel(iface.url, grpc::InsecureChannelCredentials());
  auto transport = std::make_unique<a2a::client::GrpcTransport>(iface, channel);
  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("example-task-id");

  const auto response = client.GetTask(request);
  if (!response.ok()) {
    std::cerr << "GetTask failed: " << response.error().message() << '\n';
    return 1;
  }

  std::cout << "Task id: " << response.value().id() << '\n';
  return 0;
}
