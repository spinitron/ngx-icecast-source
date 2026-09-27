/*
 * Copy Icecast SOURCE and PUT bodies that have no Content-Length.
 * GET and HEAD stay on proxy_pass. BSD-2-Clause, the nginx license.
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_event_connect.h>


#define NGX_HTTP_ICECAST_SOURCE_BUFFER           16384
#define NGX_HTTP_ICECAST_SOURCE_CONNECT_TIMEOUT  60000
#define NGX_HTTP_ICECAST_SOURCE_IDLE_TIMEOUT     3600000


typedef struct {
    ngx_addr_t   *addrs;
    ngx_uint_t    naddrs;
} ngx_http_icecast_source_loc_conf_t;


typedef struct {
    ngx_http_request_t     *request;
    ngx_peer_connection_t   peer;

    /* Rebuilt request line, headers, and any body bytes already in memory. */
    ngx_buf_t              *request_buf;

    /* One unread chunk from the source client, waiting to go upstream. */
    ngx_buf_t              *client_buf;

    /* One unread chunk from Icecast, waiting to go to the source client. */
    ngx_buf_t              *upstream_buf;

    unsigned                upstream_connected:1;
    unsigned                client_eof:1;
    unsigned                upstream_eof:1;
    unsigned                upstream_shut:1;
    unsigned                sent_to_client:1;
    unsigned                finished:1;
} ngx_http_icecast_source_ctx_t;


static ngx_int_t ngx_http_icecast_source_is_method(ngx_http_request_t *r);
static ngx_int_t ngx_http_icecast_source_rewrite(ngx_http_request_t *r);
static ngx_int_t ngx_http_icecast_source_content(ngx_http_request_t *r);
static ngx_buf_t *ngx_http_icecast_source_request_buf(ngx_http_request_t *r);
static void ngx_http_icecast_source_client_handler(ngx_http_request_t *r);
static void ngx_http_icecast_source_upstream_write(ngx_event_t *ev);
static void ngx_http_icecast_source_upstream_read(ngx_event_t *ev);
static void ngx_http_icecast_source_pump(ngx_http_icecast_source_ctx_t *ctx);
static void ngx_http_icecast_source_timeout(ngx_http_icecast_source_ctx_t *ctx);
static void ngx_http_icecast_source_ignore_event(ngx_event_t *ev);
static void ngx_http_icecast_source_finish(ngx_http_icecast_source_ctx_t *ctx,
    ngx_int_t rc);
static void ngx_http_icecast_source_touch(ngx_http_icecast_source_ctx_t *ctx);
static ngx_int_t ngx_http_icecast_source_send(ngx_connection_t *c,
    u_char **pos, u_char *last);
static char *ngx_http_icecast_source(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_http_icecast_source_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_icecast_source_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_icecast_source_init(ngx_conf_t *cf);


static ngx_command_t ngx_http_icecast_source_commands[] = {

    { ngx_string("icecast_source"),
      NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_icecast_source,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t ngx_http_icecast_source_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_icecast_source_init,          /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_icecast_source_create_loc_conf, /* create location configuration */
    ngx_http_icecast_source_merge_loc_conf   /* merge location configuration */
};


ngx_module_t ngx_http_icecast_source_module = {
    NGX_MODULE_V1,
    &ngx_http_icecast_source_module_ctx,   /* module context */
    ngx_http_icecast_source_commands,      /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_icecast_source_is_method(ngx_http_request_t *r)
{
    if (r->method == NGX_HTTP_PUT) {
        return 1;
    }

    if (r->method == NGX_HTTP_UNKNOWN
        && r->method_name.len == sizeof("SOURCE") - 1
        && ngx_strncmp(r->method_name.data, (u_char *) "SOURCE",
                       sizeof("SOURCE") - 1)
           == 0)
    {
        return 1;
    }

    return 0;
}


/*
 * proxy_pass has already installed its content handler. Swap it only for
 * SOURCE and PUT. Returning NGX_DECLINED does not fall through to the proxy
 * from inside a content handler, so the swap has to happen before that phase.
 */
static ngx_int_t
ngx_http_icecast_source_rewrite(ngx_http_request_t *r)
{
    ngx_http_icecast_source_loc_conf_t  *slcf;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_icecast_source_module);

    if (slcf->addrs == NULL || r != r->main) {
        return NGX_DECLINED;
    }

    if (r->http_version > NGX_HTTP_VERSION_11) {
        return NGX_DECLINED;
    }

    if (!ngx_http_icecast_source_is_method(r)) {
        return NGX_DECLINED;
    }

    r->content_handler = ngx_http_icecast_source_content;

    return NGX_DECLINED;
}


static ngx_buf_t *
ngx_http_icecast_source_request_buf(ngx_http_request_t *r)
{
    size_t             size, rest;
    ngx_buf_t         *b;
    ngx_list_part_t   *part;
    ngx_table_elt_t   *h;
    ngx_uint_t         i;

    if (r->method_name.len == 0 || r->unparsed_uri.len == 0
        || r->http_protocol.len == 0)
    {
        return NULL;
    }

    rest = 0;
    if (r->header_in != NULL && r->header_in->pos < r->header_in->last) {
        rest = r->header_in->last - r->header_in->pos;
    }

    size = r->method_name.len + 1 + r->unparsed_uri.len + 1
           + r->http_protocol.len + 2;

    part = &r->headers_in.headers.part;
    while (part != NULL) {
        h = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash == 0) {
                continue;
            }

            size += h[i].key.len + 2 + h[i].value.len + 2;
        }

        part = part->next;
    }

    size += 2 + rest;

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NULL;
    }

    b->last = ngx_cpymem(b->last, r->method_name.data, r->method_name.len);
    *b->last++ = ' ';
    b->last = ngx_cpymem(b->last, r->unparsed_uri.data, r->unparsed_uri.len);
    *b->last++ = ' ';
    b->last = ngx_cpymem(b->last, r->http_protocol.data, r->http_protocol.len);
    *b->last++ = CR;
    *b->last++ = LF;

    part = &r->headers_in.headers.part;
    while (part != NULL) {
        h = part->elts;
        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash == 0) {
                continue;
            }

            b->last = ngx_cpymem(b->last, h[i].key.data, h[i].key.len);
            *b->last++ = ':';
            *b->last++ = ' ';
            b->last = ngx_cpymem(b->last, h[i].value.data, h[i].value.len);
            *b->last++ = CR;
            *b->last++ = LF;
        }

        part = part->next;
    }

    *b->last++ = CR;
    *b->last++ = LF;

    /*
     * nginx already read these body bytes with the headers. They have to
     * go upstream before the next recv, or the start of the stream is lost.
     */
    if (rest > 0) {
        b->last = ngx_cpymem(b->last, r->header_in->pos, rest);
    }

    if ((size_t) (b->last - b->start) != size) {
        return NULL;
    }

    return b;
}


static ngx_int_t
ngx_http_icecast_source_content(ngx_http_request_t *r)
{
    ngx_int_t                            rc;
    ngx_connection_t                    *c;
    ngx_http_icecast_source_ctx_t       *ctx;
    ngx_http_icecast_source_loc_conf_t  *slcf;

    slcf = ngx_http_get_module_loc_conf(r, ngx_http_icecast_source_module);

    if (slcf->addrs == NULL || slcf->naddrs != 1) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_icecast_source_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->request_buf = ngx_http_icecast_source_request_buf(r);
    ctx->client_buf = ngx_create_temp_buf(r->pool,
                                          NGX_HTTP_ICECAST_SOURCE_BUFFER);
    ctx->upstream_buf = ngx_create_temp_buf(r->pool,
                                            NGX_HTTP_ICECAST_SOURCE_BUFFER);
    if (ctx->request_buf == NULL || ctx->client_buf == NULL
        || ctx->upstream_buf == NULL)
    {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->request = r;
    ngx_http_set_ctx(r, ctx, ngx_http_icecast_source_module);

    ctx->peer.sockaddr = slcf->addrs[0].sockaddr;
    ctx->peer.socklen = slcf->addrs[0].socklen;
    ctx->peer.name = &slcf->addrs[0].name;
    ctx->peer.tries = 1;
    ctx->peer.get = ngx_event_get_peer;
    ctx->peer.log = r->connection->log;
    ctx->peer.log_error = NGX_ERROR_ERR;
    ctx->peer.type = SOCK_STREAM;

    r->keepalive = 0;
    r->lingering_close = 0;

    rc = ngx_event_connect_peer(&ctx->peer);

    if (rc != NGX_OK && rc != NGX_AGAIN) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "icecast_source: connect to %V failed", ctx->peer.name);
        return NGX_HTTP_BAD_GATEWAY;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "icecast_source: relaying to %V", ctx->peer.name);

    c = ctx->peer.connection;
    c->data = ctx;
    c->write->handler = ngx_http_icecast_source_upstream_write;
    c->read->handler = ngx_http_icecast_source_upstream_read;

    r->read_event_handler = ngx_http_icecast_source_client_handler;
    r->write_event_handler = ngx_http_icecast_source_client_handler;

    /*
     * The content phase finalizes whatever this function returns. count++
     * keeps the request open across that NGX_DONE. The matching decrement
     * happens when ngx_http_icecast_source_finish() finalizes for real.
     */
    r->main->count++;

    if (rc == NGX_OK) {
        ctx->upstream_connected = 1;
        if (!c->write->posted) {
            ngx_post_event(c->write, &ngx_posted_events);
        }
    }

    ngx_http_icecast_source_touch(ctx);

    return NGX_DONE;
}


static void
ngx_http_icecast_source_client_handler(ngx_http_request_t *r)
{
    ngx_connection_t                *c;
    ngx_http_icecast_source_ctx_t   *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_icecast_source_module);
    c = r->connection;

    if (ctx == NULL || ctx->finished) {
        return;
    }

    if (c->read->timedout || c->write->timedout) {
        ngx_http_icecast_source_timeout(ctx);
        return;
    }

    ngx_http_icecast_source_pump(ctx);
}


static void
ngx_http_icecast_source_upstream_write(ngx_event_t *ev)
{
    int                              err;
    socklen_t                        len;
    ngx_connection_t                *c;
    ngx_http_icecast_source_ctx_t   *ctx;

    c = ev->data;
    ctx = c->data;

    if (ctx->finished) {
        return;
    }

    if (ev->timedout) {
        ngx_http_icecast_source_timeout(ctx);
        return;
    }

    if (!ctx->upstream_connected) {
        err = 0;
        len = sizeof(err);

        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len)
            == -1)
        {
            err = ngx_errno;
        }

        if (err) {
            ngx_log_error(NGX_LOG_ERR, c->log, err,
                          "icecast_source: connect() failed");
            ngx_http_icecast_source_finish(ctx, NGX_HTTP_BAD_GATEWAY);
            return;
        }

        ctx->upstream_connected = 1;
    }

    ngx_http_icecast_source_pump(ctx);
}


static void
ngx_http_icecast_source_upstream_read(ngx_event_t *ev)
{
    ngx_connection_t                *c;
    ngx_http_icecast_source_ctx_t   *ctx;

    c = ev->data;
    ctx = c->data;

    if (ctx->finished) {
        return;
    }

    if (ev->timedout) {
        ngx_http_icecast_source_timeout(ctx);
        return;
    }

    if (!ctx->upstream_connected) {
        return;
    }

    ngx_http_icecast_source_pump(ctx);
}


static ngx_int_t
ngx_http_icecast_source_send(ngx_connection_t *c, u_char **pos, u_char *last)
{
    ssize_t  n;

    while (*pos < last) {
        n = c->send(c, *pos, last - *pos);

        if (n == NGX_ERROR) {
            return NGX_ERROR;
        }

        if (n == NGX_AGAIN || n == 0) {
            return NGX_AGAIN;
        }

        *pos += n;
    }

    return NGX_OK;
}


static void
ngx_http_icecast_source_pump(ngx_http_icecast_source_ctx_t *ctx)
{
    ssize_t             n;
    ngx_int_t           rc;
    u_char             *start;
    ngx_buf_t          *b;
    ngx_connection_t   *client, *upstream;
    ngx_http_request_t *r;

    if (ctx->finished || !ctx->upstream_connected) {
        return;
    }

    r = ctx->request;
    client = r->connection;
    upstream = ctx->peer.connection;

    for (;;) {
        if (ctx->request_buf->pos < ctx->request_buf->last) {
            rc = ngx_http_icecast_source_send(upstream,
                                              &ctx->request_buf->pos,
                                              ctx->request_buf->last);
            if (rc == NGX_ERROR) {
                ngx_http_icecast_source_finish(ctx, NGX_HTTP_BAD_GATEWAY);
                return;
            }

            if (rc == NGX_AGAIN) {
                break;
            }
        }

        b = ctx->client_buf;
        if (b->pos < b->last) {
            rc = ngx_http_icecast_source_send(upstream, &b->pos, b->last);
            if (rc == NGX_ERROR) {
                ngx_http_icecast_source_finish(ctx, NGX_HTTP_BAD_GATEWAY);
                return;
            }

            if (rc == NGX_AGAIN) {
                break;
            }

            b->pos = b->start;
            b->last = b->start;
        }

        if (ctx->client_eof || !client->read->ready) {
            break;
        }

        n = client->recv(client, b->last, b->end - b->last);
        if (n == NGX_ERROR) {
            ngx_http_icecast_source_finish(ctx, NGX_HTTP_BAD_GATEWAY);
            return;
        }

        if (n == NGX_AGAIN || n == 0) {
            if (n == 0) {
                ctx->client_eof = 1;
            }
            break;
        }

        b->last += n;
    }

    if (ctx->finished) {
        return;
    }

    for (;;) {
        b = ctx->upstream_buf;

        if (b->pos < b->last) {
            start = b->pos;
            rc = ngx_http_icecast_source_send(client, &b->pos, b->last);
            if (b->pos > start) {
                ctx->sent_to_client = 1;
                r->header_sent = 1;
            }

            if (rc == NGX_ERROR) {
                ngx_http_icecast_source_finish(ctx, NGX_ERROR);
                return;
            }

            if (rc == NGX_AGAIN) {
                break;
            }

            b->pos = b->start;
            b->last = b->start;
        }

        if (ctx->upstream_eof || !upstream->read->ready) {
            break;
        }

        n = upstream->recv(upstream, b->last, b->end - b->last);
        if (n == NGX_ERROR) {
            ngx_http_icecast_source_finish(ctx, NGX_HTTP_BAD_GATEWAY);
            return;
        }

        if (n == NGX_AGAIN || n == 0) {
            if (n == 0) {
                ctx->upstream_eof = 1;
            }
            break;
        }

        b->last += n;
    }

    if (ctx->finished) {
        return;
    }

    /*
     * The source client closed. Icecast treats that close as the end of
     * the stream, once every byte we already accepted has been written.
     */
    if (ctx->client_eof && !ctx->upstream_shut
        && ctx->request_buf->pos == ctx->request_buf->last
        && ctx->client_buf->pos == ctx->client_buf->last)
    {
        if (ngx_shutdown_socket(upstream->fd, NGX_WRITE_SHUTDOWN) == -1) {
            ngx_log_error(NGX_LOG_ERR, upstream->log, ngx_socket_errno,
                          "icecast_source: shutdown() failed");
            ngx_http_icecast_source_finish(ctx, NGX_HTTP_BAD_GATEWAY);
            return;
        }

        ctx->upstream_shut = 1;
    }

    if (ctx->upstream_eof && ctx->upstream_buf->pos == ctx->upstream_buf->last) {
        ngx_http_icecast_source_finish(ctx,
            ctx->sent_to_client ? NGX_OK : NGX_HTTP_BAD_GATEWAY);
        return;
    }

    if (ngx_handle_read_event(client->read, 0) != NGX_OK
        || ngx_handle_write_event(client->write, 0) != NGX_OK
        || ngx_handle_read_event(upstream->read, 0) != NGX_OK
        || ngx_handle_write_event(upstream->write, 0) != NGX_OK)
    {
        ngx_http_icecast_source_finish(ctx, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    ngx_http_icecast_source_touch(ctx);
}


static void
ngx_http_icecast_source_ignore_event(ngx_event_t *ev)
{
    (void) ev;
}


static void
ngx_http_icecast_source_timeout(ngx_http_icecast_source_ctx_t *ctx)
{
    ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                  "icecast_source: connection timed out");
    ngx_http_icecast_source_finish(ctx, NGX_HTTP_REQUEST_TIME_OUT);
}


static void
ngx_http_icecast_source_finish(ngx_http_icecast_source_ctx_t *ctx, ngx_int_t rc)
{
    ngx_connection_t    *upstream;
    ngx_http_request_t  *r;

    if (ctx->finished) {
        return;
    }

    ctx->finished = 1;
    r = ctx->request;

    if (ctx->sent_to_client) {
        r->header_sent = 1;
        rc = NGX_OK;
    }

    r->read_event_handler = ngx_http_request_empty_handler;
    r->write_event_handler = ngx_http_request_empty_handler;

    upstream = ctx->peer.connection;
    if (upstream != NULL) {
        upstream->read->handler = ngx_http_icecast_source_ignore_event;
        upstream->write->handler = ngx_http_icecast_source_ignore_event;
        ngx_close_connection(upstream);
        ctx->peer.connection = NULL;
    }

    ngx_http_finalize_request(r, rc);
}


static void
ngx_http_icecast_source_set_timer(ngx_event_t *ev, ngx_msec_t timeout)
{
    if (ev->timer_set) {
        ngx_del_timer(ev);
    }

    ngx_add_timer(ev, timeout);
}


static void
ngx_http_icecast_source_touch(ngx_http_icecast_source_ctx_t *ctx)
{
    ngx_msec_t         timeout;
    ngx_connection_t  *client, *upstream;

    timeout = ctx->upstream_connected
              ? NGX_HTTP_ICECAST_SOURCE_IDLE_TIMEOUT
              : NGX_HTTP_ICECAST_SOURCE_CONNECT_TIMEOUT;

    client = ctx->request->connection;
    ngx_http_icecast_source_set_timer(client->read, timeout);
    ngx_http_icecast_source_set_timer(client->write, timeout);

    upstream = ctx->peer.connection;
    if (upstream != NULL) {
        ngx_http_icecast_source_set_timer(upstream->read, timeout);
        ngx_http_icecast_source_set_timer(upstream->write, timeout);
    }
}


static char *
ngx_http_icecast_source(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_icecast_source_loc_conf_t  *slcf = conf;

    ngx_str_t   *value, url;
    ngx_url_t    u;
    u_char      *slash;

    value = cf->args->elts;

    if (slcf->addrs != NULL) {
        return "is duplicate";
    }

    url = value[1];

    if (url.len >= 8
        && ngx_strncasecmp(url.data, (u_char *) "https://", 8) == 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "icecast_source does not take an https URL");
        return NGX_CONF_ERROR;
    }

    if (url.len >= 7
        && ngx_strncasecmp(url.data, (u_char *) "http://", 7) == 0)
    {
        url.data += 7;
        url.len -= 7;
    }

    slash = ngx_strlchr(url.data, url.data + url.len, '/');
    if (slash != NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "icecast_source \"%V\" must not include a path",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    ngx_memzero(&u, sizeof(ngx_url_t));
    u.url = url;
    u.default_port = 80;
    u.no_resolve = 0;

    if (ngx_parse_url(cf->pool, &u) != NGX_OK) {
        if (u.err) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "%s in icecast_source \"%V\"", u.err, &value[1]);
        }

        return NGX_CONF_ERROR;
    }

    if (u.naddrs != 1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "icecast_source \"%V\" resolved to %ui addresses",
                           &value[1], u.naddrs);
        return NGX_CONF_ERROR;
    }

    slcf->addrs = u.addrs;
    slcf->naddrs = u.naddrs;

    return NGX_CONF_OK;
}


static void *
ngx_http_icecast_source_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_icecast_source_loc_conf_t  *slcf;

    slcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_icecast_source_loc_conf_t));
    if (slcf == NULL) {
        return NULL;
    }

    return slcf;
}


static char *
ngx_http_icecast_source_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_icecast_source_loc_conf_t  *prev = parent;
    ngx_http_icecast_source_loc_conf_t  *conf = child;

    if (conf->addrs == NULL) {
        conf->addrs = prev->addrs;
        conf->naddrs = prev->naddrs;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_icecast_source_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_icecast_source_rewrite;

    return NGX_OK;
}
