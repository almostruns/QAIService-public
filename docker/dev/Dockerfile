FROM ubuntu:24.04 AS dev

ARG DEBIAN_FRONTEND=noninteractive
ARG MUDUO_VERSION=2.0.3
ARG MUDUO_SHA256=5e90c2f07074ed5ab9347959edc8d387e40a57fbffb92e4a99cfb9a230851371

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        libboost-dev \
        libcurl4-openssl-dev \
        libmariadb-dev \
        libpoppler-cpp-dev \
        librabbitmq-dev \
        libsodium-dev \
        libspdlog-dev \
        libxml2-dev \
        libzip-dev \
        ninja-build \
        nlohmann-json3-dev \
    && curl --location --fail --silent --show-error \
        "https://github.com/chenshuo/muduo/archive/refs/tags/v${MUDUO_VERSION}.tar.gz" \
        --output /tmp/muduo.tar.gz \
    && echo "${MUDUO_SHA256}  /tmp/muduo.tar.gz" | sha256sum --check - \
    && mkdir -p /tmp/muduo-source /tmp/muduo-build \
    && tar --extract --gzip --file /tmp/muduo.tar.gz \
        --directory /tmp/muduo-source --strip-components=1 \
    && cmake \
        -S /tmp/muduo-source \
        -B /tmp/muduo-build \
        -DMUDUO_BUILD_EXAMPLES=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/muduo-build --parallel \
    && cmake --install /tmp/muduo-build \
    && rm -rf /tmp/muduo.tar.gz /tmp/muduo-source /tmp/muduo-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]

FROM dev AS build

COPY . /workspace

RUN cmake \
        -S /workspace \
        -B /opt/qaiservice-build \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/qaiservice-build --parallel

FROM ubuntu:24.04 AS server

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        ca-certificates \
        curl \
        libmariadb3 \
        libpoppler-cpp0t64 \
        librabbitmq4 \
        libsodium23 \
        libspdlog1.12 \
        libxml2 \
        libzip4t64 \
        tzdata \
    && groupadd --system qaiservice \
    && useradd --system --gid qaiservice --no-create-home qaiservice \
    && mkdir -p /var/lib/qaiservice/files \
    && chown -R qaiservice:qaiservice /var/lib/qaiservice \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/qaiservice-build/qaiservice /usr/local/bin/qaiservice
COPY web /opt/qaiservice/web

USER qaiservice

EXPOSE 8080

HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
    CMD curl --fail --silent --show-error http://127.0.0.1:8080/health >/dev/null || exit 1

CMD ["/usr/local/bin/qaiservice"]
