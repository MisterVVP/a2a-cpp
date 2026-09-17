FROM debian:12.11-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake ninja-build libcurl4-openssl-dev libprotobuf-dev protobuf-compiler libgrpc++-dev protobuf-compiler-grpc && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build-sdk -G Ninja -DA2A_ENABLE_TESTING=OFF -DA2A_BUILD_EXAMPLES=OFF -DA2A_ENABLE_POSTGRES_STORE=OFF -DCMAKE_INSTALL_PREFIX=/install && cmake --build /build-sdk --parallel && cmake --install /build-sdk
RUN cmake -S examples/tutorials/customer_support_copilot -B /build-tutorial -G Ninja -DCMAKE_PREFIX_PATH=/install && cmake --build /build-tutorial --parallel
FROM debian:12.11-slim
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4 libprotobuf32 libgrpc++1.51 ca-certificates curl && rm -rf /var/lib/apt/lists/* && useradd --system --uid 10001 tutorial
COPY --from=build /build-tutorial/support_client /build-tutorial/support_coordinator /build-tutorial/support_specialist /opt/tutorial/
COPY examples/tutorials/customer_support_copilot/samples /opt/tutorial/samples
USER tutorial
WORKDIR /opt/tutorial
