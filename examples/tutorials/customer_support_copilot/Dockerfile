FROM alpine:3.22 AS build
RUN apk add --no-cache build-base cmake curl-dev grpc-dev grpc-plugins ninja protobuf protobuf-dev
WORKDIR /src
COPY . .
RUN cmake -S . -B /build-sdk -G Ninja -DA2A_ENABLE_TESTING=OFF -DA2A_BUILD_EXAMPLES=OFF -DA2A_ENABLE_POSTGRES_STORE=OFF -DCMAKE_INSTALL_PREFIX=/install && cmake --build /build-sdk --parallel && cmake --install /build-sdk
RUN cmake -S examples/tutorials/customer_support_copilot -B /build-tutorial -G Ninja -DCMAKE_PREFIX_PATH=/install && cmake --build /build-tutorial --parallel
FROM alpine:3.22
RUN apk add --no-cache ca-certificates grpc grpc-cpp libcurl libprotobuf libstdc++ \
    && adduser -S -D -H -u 10001 tutorial
COPY --from=build /build-tutorial/support_client /build-tutorial/support_coordinator /build-tutorial/support_specialist /opt/tutorial/
COPY examples/tutorials/customer_support_copilot/samples /opt/tutorial/samples
USER tutorial
WORKDIR /opt/tutorial
