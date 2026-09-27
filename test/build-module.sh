#!/bin/sh
# Build ngx_http_icecast_source_module.so against the nginx package
# installed on this machine. Requires Ubuntu/Debian source entries (deb-src).
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
mod="$root/src"

if ! command -v nginx >/dev/null 2>&1; then
    echo "nginx is not installed" >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT

cd "$workdir"
apt-get update
apt-get install -y --no-install-recommends dpkg-dev
apt-get build-dep -y nginx
apt-get source nginx

src=$(find . -maxdepth 1 -type d -name 'nginx-*')
cd "$src"

args=$(nginx -V 2>&1 | sed -n 's/^configure arguments: //p')
eval "./configure $args --add-dynamic-module=$mod"
make -j"$(nproc)" modules
mkdir -p /usr/lib/nginx/modules
cp objs/ngx_http_icecast_source_module.so /usr/lib/nginx/modules/
echo "installed /usr/lib/nginx/modules/ngx_http_icecast_source_module.so"
