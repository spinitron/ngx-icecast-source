# ngx-icecast-source

The nginx package in Debian and Ubuntu cannot reverse-proxy an Icecast source. This module corrects that problem.

nginx treats a `SOURCE` or `PUT` that has no `Content-Length` as a request with no body, so the encoded audio never reaches Icecast. The source then stays connected sending audio until Icecast times its connection out. 

nginx's handling of listeners is a different problem: a listener is the response, and `proxy_buffering off` already streams that.

So this module handles `SOURCE` and `PUT` requests only. It sends those to the upstream Icecast and copies bytes both ways until the connection closes. `GET` and `HEAD` requests continue to use `proxy_pass`.

This is not an Xiph project.

## Limits

The module is specific for clients that: are HTTP source clients, send `SOURCE` or `PUT`, send no `Content-Length`, and send a raw body. Clients behaving this way include:
- Liquidsoap's default `output.icecast`
- BUTT
- libshout
- FFmpeg (libavformat/icecast.c)

The module uses the existing `listen` and `location`. It does not open a second port.

A Shoutcast source that is not HTTP never arrives here. nginx has no request line to dispatch.

Chunked request bodies are copied as the client sent them. This module does not decode them.

No admin UI. No stats. No listener features.

## License

BSD-2-Clause, the nginx license. Copyright (C) 2026 Spinitron, LLC.

## Build

Build on the host whose nginx package will load the module. That host needs its deb-src entries enabled.

```
sudo test/build-module.sh
```

The script installs `/usr/lib/nginx/modules/ngx_http_icecast_source_module.so`. Load it from the top of the nginx configuration:

```
load_module /usr/lib/nginx/modules/ngx_http_icecast_source_module.so;
```

## Test

`test/` is an Ubuntu 24.04 image with that distribution's nginx and icecast2 packages. It sends a `SOURCE` with no `Content-Length`, and the audio in the same write as the headers.

```
docker build -f test/Dockerfile -t ngx-icecast-source .
docker run --rm ngx-icecast-source
```

Icecast relays the source to a listener. So does nginx, with the module loaded. The run exits 0.

`icecast_source` names the Icecast host and port. `GET` and `HEAD` in that location stay on `proxy_pass`.

## Status

No release yet. The module is in `src/`.

The test runs nginx 1.24.0-2ubuntu7.18 and icecast2 2.4.4 from Ubuntu 24.04. Ubuntu 22.04 and Icecast-KH have not been run.

Packages, when they exist, will be Debian packages for the nginx shipped by Ubuntu 22.04 and 24.04, amd64, published from a Launchpad PPA. Each package matches one nginx build. A `.so` built for a different nginx will not load.

The servers this project will claim are Debian `icecast2` and Icecast-KH.