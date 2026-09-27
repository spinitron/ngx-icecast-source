# docker build -f test/Dockerfile -t ngx-icecast-source .
# docker run --rm ngx-icecast-source
# Exit 0: the source audio reached a listener through nginx.
# Exit 1: Icecast relayed it directly, and nginx did not.
# Exit 2: Icecast itself did not relay the payload.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        gcc \
        icecast2 \
        make \
        nginx \
        nginx-dev \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/ /src/src/
COPY test/ /src/test/
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources \
    && chmod +x /src/test/build-module.sh /src/test/run.sh \
    && /src/test/build-module.sh

CMD ["/src/test/run.sh"]
