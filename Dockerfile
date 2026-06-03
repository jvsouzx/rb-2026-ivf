FROM debian:bookworm-slim AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG RB_IVF_NLIST=8192
ARG RB_IVF_SAMPLE_SIZE=200000
ARG RB_IVF_ITERATIONS=12

ENV RB_IVF_NLIST=${RB_IVF_NLIST}
ENV RB_IVF_SAMPLE_SIZE=${RB_IVF_SAMPLE_SIZE}
ENV RB_IVF_ITERATIONS=${RB_IVF_ITERATIONS}

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        curl \
        git \
        ninja-build \
        pkg-config \
        tar \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt
RUN git clone https://github.com/microsoft/vcpkg.git \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_DISABLE_METRICS=1

WORKDIR /src
COPY CMakeLists.txt vcpkg.json ./
COPY src ./src
COPY resources ./resources

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    && cmake --build build --target preprocess_references \
    && ./build/preprocess_references \
    && cmake --build build --target fraud_api

FROM debian:bookworm-slim AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY resources/mcc_risk.json resources/normalization.json ./resources/
COPY --from=build /src/resources/references.ivf ./resources/references.ivf
COPY --from=build /src/build/fraud_api ./fraud_api

EXPOSE 8080

CMD ["./fraud_api"]
