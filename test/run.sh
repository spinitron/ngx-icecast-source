#!/bin/sh
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)

cleanup() {
    if [ -f /tmp/nginx.pid ]; then
        nginx -c "$ROOT/nginx.conf" -s stop >/dev/null 2>&1 || true
    fi
    for dir in /proc/[0-9]*; do
        [ -r "$dir/cmdline" ] || continue
        cmd=$(tr '\0' ' ' < "$dir/cmdline" 2>/dev/null || true)
        case "$cmd" in
            *icecast2*) kill "${dir#/proc/}" >/dev/null 2>&1 || true ;;
        esac
    done
}
trap cleanup EXIT

mkdir -p /var/log/icecast2 /tmp
chown icecast2:icecast /var/log/icecast2
: > /var/log/icecast2/error.log
chown icecast2:icecast /var/log/icecast2/error.log

icecast2 -c "$ROOT/icecast.xml" -b

wait_port() {
    n=0
    while [ "$n" -lt 50 ]; do
        if python3 -c "import socket; socket.create_connection(('127.0.0.1', $1), 1).close()"; then
            return 0
        fi
        n=$((n + 1))
        sleep 0.1
    done
    echo "port $1 did not open" >&2
    return 1
}

wait_port 8000
nginx -c "$ROOT/nginx.conf"
wait_port 8080

set +e
python3 "$ROOT/probe.py"
status=$?
set -e

echo "--- icecast error log ---"
tail -n 40 /var/log/icecast2/error.log || true
echo "--- nginx error log ---"
tail -n 40 /tmp/nginx-error.log || true
exit "$status"
