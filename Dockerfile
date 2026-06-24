FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
       build-essential \
       ca-certificates \
       cmake \
       libgrpc++-dev \
       libprotobuf-dev \
       ninja-build \
       protobuf-compiler \
       protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build/container -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DENABLE_GRPC=ON \
      -DENABLE_CLANG_TIDY=OFF \
      -DWARNINGS_AS_ERRORS=ON \
    && cmake --build build/container --parallel 2 \
      --target nebulakv_node nebulakv_cli nebulakv_workload \
    && install -d /opt/nebulakv/bin \
    && install -m 0755 build/container/nebulakv-node /opt/nebulakv/bin/ \
    && install -m 0755 build/container/nebulakv-cli /opt/nebulakv/bin/ \
    && install -m 0755 build/container/nebulakv-benchmark /opt/nebulakv/bin/ \
    && rm -rf /src/build

WORKDIR /var/lib/nebulakv
ENV PATH="/opt/nebulakv/bin:${PATH}"

EXPOSE 5001 9100
ENTRYPOINT ["/opt/nebulakv/bin/nebulakv-node"]
