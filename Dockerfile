FROM ubuntu:22.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
    libcurl4-openssl-dev libsqlite3-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN cmake -S . -B build \
 && cmake --build build -j

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 libsqlite3-0 ca-certificates \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /app/build/catch_the_letter /app/catch_the_letter
COPY config /app/config

EXPOSE 8080
ENTRYPOINT ["/app/catch_the_letter","--config","/app/config/app.json"]
