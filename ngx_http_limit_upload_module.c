/*
 * Author: cfsego
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    size_t                    limit_rate;
    size_t                    limit_rate_after;
    ngx_uint_t                log_level;
} ngx_http_limit_upload_conf_t;


typedef struct {
    off_t                     received;
    size_t                    limit_rate;
    ngx_http_event_handler_pt read_event_handler;
    ngx_http_event_handler_pt write_event_handler;
} ngx_http_limit_upload_ctx_t;


static ngx_int_t ngx_http_limit_upload_add_variable(ngx_conf_t *cf);
static ngx_int_t ngx_http_limit_upload_init(ngx_conf_t *cf);
static void *ngx_http_limit_upload_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_limit_upload_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);

static ngx_int_t ngx_http_variable_limit_upload_get_size(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void ngx_http_variable_limit_upload_set_size(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);

static ngx_int_t ngx_http_limit_upload_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_limit_upload_input_body_filter(ngx_http_request_t *r,
    ngx_buf_t *buf);

static void ngx_http_limit_req_delay(ngx_http_request_t *r);


static ngx_conf_enum_t ngx_http_limit_upload_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_command_t ngx_http_limit_upload_commands[] = {

    { ngx_string("limit_upload_rate"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                        |NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, limit_rate),
      NULL },

    { ngx_string("limit_upload_rate_after"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                        |NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, limit_rate_after),
      NULL },

    { ngx_string("limit_upload_rate_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_limit_upload_conf_t, log_level),
      &ngx_http_limit_upload_log_levels },

      ngx_null_command
};


static ngx_http_module_t ngx_http_limit_upload_module_ctx = {
    ngx_http_limit_upload_add_variable,       /* preconfiguration */
    ngx_http_limit_upload_init,               /* postconfiguration */

    NULL,                                     /* create main configuration */
    NULL,                                     /* init main configuration */

    NULL,                                     /* create server configuration */
    NULL,                                     /* merge server configuration */

    ngx_http_limit_upload_create_loc_conf,    /* create location configration */
    ngx_http_limit_upload_merge_loc_conf      /* merge location configration */
};


ngx_module_t ngx_http_limit_upload_module = {
    NGX_MODULE_V1,
    &ngx_http_limit_upload_module_ctx,        /* module context */
    ngx_http_limit_upload_commands,           /* module directives */
    NGX_HTTP_MODULE,                          /* module type */
    NULL,                                     /* init master */
    NULL,                                     /* init module */
    NULL,                                     /* init process */
    NULL,                                     /* init thread */
    NULL,                                     /* exit thread */
    NULL,                                     /* exit process */
    NULL,                                     /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_str_t  ngx_http_limit_upload_var_name
                                            = ngx_string("limit_upload_rate");

static ngx_http_input_body_filter_pt  ngx_http_next_input_body_filter;


static ngx_int_t
ngx_http_limit_upload_input_body_filter(ngx_http_request_t *r, ngx_buf_t *buf)
{
    off_t                          excess;
    ngx_int_t                      rc;
    ngx_msec_t                     delay;
    ngx_http_limit_upload_ctx_t   *ctx;
    ngx_http_limit_upload_conf_t  *llcf;

    rc = ngx_http_next_input_body_filter(r, buf);
    if (rc != NGX_OK) {
        return rc;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_limit_upload_module);

    llcf = ngx_http_get_module_loc_conf(r, ngx_http_limit_upload_module);

    if (ctx->limit_rate == 0) {
        ctx->limit_rate = llcf->limit_rate;
    }

    if (ctx->limit_rate) {
        ctx->received += ngx_buf_size(buf);

        excess = ctx->received - llcf->limit_rate_after
               - ctx->limit_rate * (ngx_time() - r->start_sec + 1);

        ngx_log_debug5(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "limit upload: ctx=%p, received=%O, "
                       "excess=%O, limit_rate=%z, period=%M",
                       ctx, ctx->received, excess, ctx->limit_rate,
                       ngx_time() - r->start_sec + 1);

        if (excess > 0) {
            if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            delay = excess * 1000 / ctx->limit_rate;

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "limit upload: delay=%M", delay);

            ctx->read_event_handler = r->read_event_handler;
            ctx->write_event_handler = r->write_event_handler;
            r->read_event_handler = ngx_http_test_reading;
            r->write_event_handler = ngx_http_limit_req_delay;
            ngx_add_timer(r->connection->write, delay);

            return NGX_AGAIN;
        }
    }

    return NGX_OK;
}


static void *
ngx_http_limit_upload_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_limit_upload_conf_t  *conf;

    conf = ngx_palloc(cf->pool, sizeof(ngx_http_limit_upload_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->limit_rate = NGX_CONF_UNSET_SIZE;
    conf->limit_rate_after = NGX_CONF_UNSET_SIZE;
    conf->log_level = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_limit_upload_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_limit_upload_conf_t  *conf = child;
    ngx_http_limit_upload_conf_t  *prev = parent;

    ngx_conf_merge_size_value(conf->limit_rate, prev->limit_rate, 0);
    ngx_conf_merge_size_value(conf->limit_rate_after, prev->limit_rate_after,
                              0);
    ngx_conf_merge_uint_value(conf->log_level,
                              prev->log_level, NGX_LOG_ERR);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_limit_upload_add_variable(ngx_conf_t *cf)
{
    ngx_http_variable_t           *var;

    var = ngx_http_add_variable(cf, &ngx_http_limit_upload_var_name,
                                NGX_HTTP_VAR_CHANGEABLE
                                |NGX_HTTP_VAR_NOCACHEABLE
                                |NGX_HTTP_VAR_INDEXED);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->set_handler = ngx_http_variable_limit_upload_set_size;
    var->get_handler = ngx_http_variable_limit_upload_get_size;
    var->data = offsetof(ngx_http_limit_upload_ctx_t, limit_rate);

    if (ngx_http_get_variable_index(cf, &ngx_http_limit_upload_var_name)
        == NGX_ERROR)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_upload_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt           *h;
    ngx_http_core_main_conf_t     *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_POST_READ_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_limit_upload_handler;

    ngx_http_next_input_body_filter = ngx_http_top_input_body_filter;
    ngx_http_top_input_body_filter = ngx_http_limit_upload_input_body_filter;

    return NGX_OK;
}


static ngx_int_t
ngx_http_limit_upload_handler(ngx_http_request_t *r)
{
    ngx_http_limit_upload_ctx_t   *ctx;

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_limit_upload_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit upload: ctx=%p", ctx);

    ngx_http_set_ctx(r, ctx, ngx_http_limit_upload_module);

    return NGX_OK;
}


static ngx_int_t
ngx_http_variable_limit_upload_get_size(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    size_t                        *sp;
    ngx_http_limit_upload_ctx_t   *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_limit_upload_module);
    sp = (size_t *) ((char *) ctx + data);

    v->data = ngx_pnalloc(r->pool, NGX_SIZE_T_LEN);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    v->len = ngx_sprintf(v->data, "%uz", *sp) - v->data;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


static void
ngx_http_variable_limit_upload_set_size(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ssize_t                        s, *sp;
    ngx_str_t                      val;
    ngx_http_limit_upload_ctx_t   *ctx;

    val.len = v->len;
    val.data = v->data;

    s = ngx_parse_size(&val);

    if (s == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "invalid size \"%V\"", &val);
        return;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_limit_upload_module);
    sp = (ssize_t *) ((char *) ctx + data);

    *sp = s;

    return;
}


static void
ngx_http_limit_req_delay(ngx_http_request_t *r)
{
    ngx_event_t                   *wev;
    ngx_http_limit_upload_ctx_t   *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_limit_upload_module);

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "limit upload delay");

    wev = r->connection->write;

    if (!wev->timedout) {

        if (ngx_handle_write_event(wev, 0) != NGX_OK) {
            ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        }

        return;
    }

    wev->timedout = 0;

    if (ngx_handle_read_event(r->connection->read, 0) != NGX_OK) {
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    r->read_event_handler = ctx->read_event_handler;
    r->write_event_handler = ctx->write_event_handler;

    r->read_event_handler(r);
}
