#!/usr/bin/env python3
"""Send an Icecast SOURCE the way BUTT does, and see if a listener hears it."""

import base64
import socket
import sys
import threading
import time

MARKER = b"NGXSRC01"
AUTH = base64.b64encode(b"source:probe-secret").decode()
HEAD = (
    "SOURCE /live HTTP/1.0\r\n"
    f"Authorization: Basic {AUTH}\r\n"
    "Content-Type: audio/mpeg\r\n"
    "\r\n"
).encode()


def mp3_frame():
    # MPEG-1 Layer III, 128 kbps, 44100 Hz, no padding, no CRC: 417 bytes.
    # Icecast's generic handler only releases a buffer to listeners at a frame sync.
    body = (MARKER * 52)[:413]
    return b"\xff\xfb\x90\x00" + body


FRAME = mp3_frame()
FIRST = HEAD + (FRAME * 2)


def status_line(sock, timeout):
    sock.settimeout(timeout)
    data = b""
    try:
        while b"\r\n" not in data and len(data) < 4096:
            chunk = sock.recv(1024)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    if not data:
        return ""
    return data.split(b"\r\n", 1)[0].decode("latin1", "replace")


def feed(sock, until):
    blob = FRAME * 4
    while time.monotonic() < until:
        try:
            sock.send(blob)
        except OSError:
            return
        time.sleep(0.2)


def source(port, hold):
    sock = socket.create_connection(("127.0.0.1", port), 2)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sent = sock.send(FIRST)
    until = time.monotonic() + hold
    thread = threading.Thread(target=feed, args=(sock, until), daemon=True)
    thread.start()
    line = status_line(sock, min(2.0, hold))
    thread.join(timeout=hold + 0.5)
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()
    return line, sent == len(FIRST)


def listen(port, seconds):
    time.sleep(0.4)
    sock = socket.create_connection(("127.0.0.1", port), 2)
    sock.sendall(
        b"GET /live HTTP/1.0\r\n"
        b"Host: localhost\r\n"
        b"Icy-MetaData: 0\r\n"
        b"\r\n"
    )
    sock.settimeout(0.5)
    data = b""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            chunk = sock.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        data += chunk
    sock.close()
    return data


def main():
    direct_heard = {}

    def grab_direct():
        direct_heard["data"] = listen(8000, 1.0)

    direct_thread = threading.Thread(target=grab_direct)
    direct_thread.start()
    direct, direct_one = source(8000, 1.2)
    direct_thread.join()
    direct_count = direct_heard.get("data", b"").count(MARKER)
    print(f"direct source: {direct or 'no response'}")
    print(f"direct listener marker count: {direct_count}")
    if not direct_one:
        print("direct source first send was split")
        return 2
    if "200" not in direct:
        print("icecast did not accept a source on loopback")
        return 2
    if direct_count == 0:
        sample = direct_heard.get("data", b"")[:180]
        print(f"direct listener bytes: {len(direct_heard.get('data', b''))} {sample!r}")
        print("icecast did not relay the payload to a listener")
        return 2

    time.sleep(0.5)
    heard = {}

    def grab():
        heard["data"] = listen(8080, 2.5)

    thread = threading.Thread(target=grab)
    thread.start()
    proxied, proxied_one = source(8080, 3.0)
    thread.join()
    count = heard.get("data", b"").count(MARKER)
    print(f"proxied source: {proxied or 'no response'}")
    print(f"proxied first send carried headers and audio: {proxied_one}")
    print(f"listener marker count: {count}")
    if "200" in proxied and count > 0:
        print("result: source audio passed through nginx")
        return 0
    print("result: source audio did not pass through nginx")
    return 1


if __name__ == "__main__":
    sys.exit(main())
