/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static void ngx_http_read_client_request_body_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_do_read_client_request_body(ngx_http_request_t *r);
static ngx_int_t ngx_http_write_request_body(ngx_http_request_t *r,
    ngx_chain_t *body);
static ngx_int_t ngx_http_read_discarded_request_body(ngx_http_request_t *r);
static ngx_int_t ngx_http_test_expect(ngx_http_request_t *r);


/*
 * on completion ngx_http_read_client_request_body() adds to
 * r->request_body->bufs one or two bufs:
	 当一个buf可以放下时，读到的post body将放在 r->header_in里 
 *    *) one memory buf that was preread in r->header_in; 
	 当一个buf放不下时，读到的post body一部分放在 r->header_in里，剩下的放在另外的buf里或文件中 
 *    *) one memory or file buf that contains the rest of the body
 */

ngx_int_t
ngx_http_read_client_request_body(ngx_http_request_t *r,
    ngx_http_client_body_handler_pt post_handler)
{
    size_t                     preread;
    ssize_t                    size;
    ngx_buf_t                 *b, buf;
    ngx_int_t                  rc;
    ngx_chain_t               *cl, **next;
    ngx_http_request_body_t   *rb;
    ngx_http_core_loc_conf_t  *clcf;
    /* 增加主请求的引用数，这个字段主要是在ngx_http_finalize_request调用的一些结束请求和 
       连接的函数中使用 */ 
    r->main->count++;

    /*
     *	typedef struct {
     *    ngx_temp_file_t                  * temp_file; 
     *    ngx_chain_t                      * bufs;  消息体都保存在这个chain里面  
     *    ngx_buf_t                        * buf;   用作临时存储的buf，在ngx_http_read_client_request_body和
                                                      ngx_http_do_read_client_request_body中用得到 
     *    off_t                             rest;   剩余部分偏移
     *    ngx_chain_t                      * to_write;
     *    ngx_http_client_body_handler_pt   post_handler;
     *  }ngx_http_request_body_t;
     */
    
    /*如果r->request_body已存在或已设置为忽略的话则直接调用post_handler*/
    if (r->request_body || r->discard_body) {
        post_handler(r);
        return NGX_OK;
    }
    /*处理Http 1.1 的expect请求头,如请求头中有 expect: "100-continue",则向客户端发送continue应答*/
    if (ngx_http_test_expect(r) != NGX_OK) {
        rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        goto done;
    }
    
    rb = ngx_pcalloc(r->pool, sizeof(ngx_http_request_body_t));
    if (rb == NULL) {
        rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        goto done;
    }
    
    r->request_body = rb;
    
    if (r->headers_in.content_length_n < 0) {
        post_handler(r);
        return NGX_OK;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    /*content_length为0表示body为空，nginx会只去新建一个temp_file*/
    if (r->headers_in.content_length_n == 0) {
        /*r->request_body_in_file_only 表示设定为每个body都存放到临时文件里*/
        if (r->request_body_in_file_only) {
	    /*此处函数调用的第二个形参body对应为NULL,所以只会新建一个temp_file*/
            if (ngx_http_write_request_body(r, NULL) != NGX_OK) {
                rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
                goto done;
            }
        }

        post_handler(r);
	/*conten_length为0时，结束body处理*/
        return NGX_OK;
    }

    rb->post_handler = post_handler;

    /*
     * set by ngx_pcalloc():
     *
     *     rb->bufs = NULL;
     *     rb->buf = NULL;
     *     rb->rest = 0;
     */
    /*preread为已读到的数据长度*/
    preread = r->header_in->last - r->header_in->pos;

    if (preread) {

        /* there is the pre-read part of the request body */

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "http client request body preread %uz", preread);
        /*这个b最终会挂到rb->bufs链中去*/
        b = ngx_calloc_buf(r->pool);
        if (b == NULL) {
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
            goto done;
        }

        b->temporary = 1;
        /*让b指向header_in的ngx_buf_t结构体*/
        b->start = r->header_in->pos;
        b->pos = r->header_in->pos;
        b->last = r->header_in->last;
        b->end = r->header_in->end;
	/*让buf也指向header_in结构体，注意：buf的last字段指向的是content_length表示的有效内存区,截断了preread多出来的部分*/
        ngx_memzero(&buf, sizeof(ngx_buf_t));
        buf.memory = 1;
        buf.start = r->header_in->pos;
        buf.pos = r->header_in->pos;
        buf.last = (off_t) preread >= r->headers_in.content_length_n
                 ? r->header_in->pos + (size_t) r->headers_in.content_length_n
                 : r->header_in->last;
        buf.end = r->header_in->end;

        rb->bufs = ngx_alloc_chain_link(r->pool);
        if (rb->bufs == NULL) {
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
            goto done;
        }
	/*把b挂到链中去*/
        rb->bufs->buf = b;
        rb->bufs->next = NULL;
	/*rb->buf是一个临时用的buf指针*/
        rb->buf = b;
	/*TODO:开始进入input_body_filter处理链？*/
        rc = ngx_http_top_input_body_filter(r, &buf);
        if (rc != NGX_OK) {
            if (rc > NGX_OK && rc < NGX_HTTP_SPECIAL_RESPONSE) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "input filter: return code 1xx or 2xx "
                              "will cause trouble and is converted to 500");
            }

            /**
             * NGX_OK: success and continue;
             * NGX_ERROR: failed and exit;
             * NGX_AGAIN: not ready and retry later.
             */

            if (rc < NGX_HTTP_SPECIAL_RESPONSE && rc != NGX_AGAIN) {
                rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
            }

            return rc;
        }

        if ((off_t) preread >= r->headers_in.content_length_n) {

            /* the whole request body was pre-read */
	    /*把pos移到读到的body的最后位置，并更新各个数据*/
            r->header_in->pos += (size_t) r->headers_in.content_length_n;
            r->request_length += r->headers_in.content_length_n;
            b->last = r->header_in->pos;

            if (r->request_body_in_file_only) {
                /*很明显的函数名字，将rb->bufs中的内容写入r->temp_file中*/
                if (ngx_http_write_request_body(r, rb->bufs) != NGX_OK) {
                    rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
                    goto done;
                }
            }
            /*到此时body已接收完整，调用post_handler*/
            post_handler(r);

            return NGX_OK;
        }

        /*
         * to not consider the body as pipelined request in
         * ngx_http_set_keepalive()
         */
        r->header_in->pos = r->header_in->last;
	/*request_length为已读取到body的长度,加上新读到的prereads个字节*/
        r->request_length += preread;
        
        rb->rest = r->headers_in.content_length_n - preread;
        /*判断剩余的数据是否比一个buf的剩余空间还大*/
        if (rb->rest <= (off_t) (b->end - b->last)) {
            /*如果小于或等于,则所有body可放在一个buf里*/
            /* the whole request body may be placed in r->header_in */

            rb->to_write = rb->bufs;
            /*因为还没读完，所以还需要再注册一次读数据的回调事件*/
            r->read_event_handler = ngx_http_read_client_request_body_handler;

            rc = ngx_http_do_read_client_request_body(r);
            goto done;
        }
        /*如果剩余数据过大，一个buf里放不下的话*/
        next = &rb->bufs->next;

    } else {
        b = NULL;
        rb->rest = r->headers_in.content_length_n;
        next = &rb->bufs;
    }

    size = clcf->client_body_buffer_size;
    size += size >> 2;

    if (rb->rest < size) {
        size = (ssize_t) rb->rest;

        if (r->request_body_in_single_buf) {
            size += preread;
        }

    } else {
        size = clcf->client_body_buffer_size;

        /* disable copying buffer for r->request_body_in_single_buf */
        b = NULL;
    }

    rb->buf = ngx_create_temp_buf(r->pool, size);
    if (rb->buf == NULL) {
        rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        goto done;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        goto done;
    }

    cl->buf = rb->buf;
    cl->next = NULL;

    if (b && r->request_body_in_single_buf) {
        size = b->last - b->pos;
        ngx_memcpy(rb->buf->pos, b->pos, size);
        rb->buf->last += size;

        next = &rb->bufs;
    }

    *next = cl;

    if (r->request_body_in_file_only || r->request_body_in_single_buf) {
        rb->to_write = rb->bufs;

    } else {
        rb->to_write = rb->bufs->next ? rb->bufs->next : rb->bufs;
    }

    r->read_event_handler = ngx_http_read_client_request_body_handler;

    rc = ngx_http_do_read_client_request_body(r);

done:

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        r->main->count--;
    }

    return rc;
}


static void
ngx_http_read_client_request_body_handler(ngx_http_request_t *r)
{
    ngx_int_t  rc;

    if (r->connection->read->timedout) {
        r->connection->timedout = 1;
        ngx_http_finalize_request(r, NGX_HTTP_REQUEST_TIME_OUT);
        return;
    }

    rc = ngx_http_do_read_client_request_body(r);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        ngx_http_finalize_request(r, rc);
    }
}


static ngx_int_t
ngx_http_do_read_client_request_body(ngx_http_request_t *r)
{
    size_t                     size;
    ssize_t                    n;
    ngx_buf_t                 *b, buf;
    ngx_int_t                  rc;
    ngx_connection_t          *c;
    ngx_http_request_body_t   *rb;
    ngx_http_core_loc_conf_t  *clcf;

    c = r->connection;
    rb = r->request_body;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http read client request body");

    for ( ;; ) {
        for ( ;; ) {
            if (rb->buf->last == rb->buf->end) {
                /*当ngx_http_read_client_body调用时，这里的rb->to_write是指向rb->bufs链*/
                if (ngx_http_write_request_body(r, rb->to_write) != NGX_OK) {
                    return NGX_HTTP_INTERNAL_SERVER_ERROR;
                }
                /*rb->bufs链只有一个节点时，rb->to_write将为rb->bufs*/
                rb->to_write = rb->bufs->next ? rb->bufs->next : rb->bufs;
                /*清空rb->buf*/
                rb->buf->last = rb->buf->start;
            }
            /*buf空闲大小*/
            size = rb->buf->end - rb->buf->last;
            /*没读完的body是否大于buf的空闲空间,将size赋值为两者之间的较小者*/
            if ((off_t) size > rb->rest) {
                size = (size_t) rb->rest;
            }
            /*先将rb->buf填满或把body一下读完*/
            n = c->recv(c, rb->buf->last, size);

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                           "http client request body recv %z", n);
            /*设置了非阻塞读，可以先退出*/
            if (n == NGX_AGAIN) {
                break;
            }

            if (n == 0) {
                ngx_log_error(NGX_LOG_INFO, c->log, 0,
                              "client prematurely closed connection");
            }

            if (n == 0 || n == NGX_ERROR) {
                c->error = 1;
                return NGX_HTTP_BAD_REQUEST;
            }
            /*将buf赋值为rb->buf上面recv读入的body部分*/
            ngx_memzero(&buf, sizeof(ngx_buf_t));
            buf.memory = 1;
            buf.start = rb->buf->last;
            buf.pos = rb->buf->last;
            buf.last = buf.start + n;
            buf.end = buf.last;
            /*FIXME nginx为什么不在recv里面自动更新buf->last，这样不是更方便吗？*/
            rb->buf->last += n;
            rb->rest -= n;
            /*更新已读取的数据长度*/
            r->request_length += n;
            /*对新收到的数据再次调用过滤模块*/
            rc = ngx_http_top_input_body_filter(r, &buf);
            if (rc != NGX_OK) {
                if (rc > NGX_OK && rc < NGX_HTTP_SPECIAL_RESPONSE) {
                    ngx_log_error(NGX_LOG_ERR, c->log, 0,
                              "input filter: return code 1xx or 2xx "
                              "will cause trouble and is converted to 500");
                }

                if (rc < NGX_HTTP_SPECIAL_RESPONSE && rc != NGX_AGAIN) {
                    rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
                }

                return rc;
            }

            if (rb->rest == 0) {
                break;
            }

            if (rb->buf->last < rb->buf->end) {
                break;
            }
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http client request body rest %O", rb->rest);

        if (rb->rest == 0) {
            break;
        }
        /*当连接为不可读的时候*/
        if (!c->read->ready) {
            clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
            /*对read事情加超时机制,这里应该会设置下面接下来的c->read->timer_set*/
            ngx_add_timer(c->read, clcf->client_body_timeout);
            /*注册read event*/
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                return NGX_HTTP_INTERNAL_SERVER_ERROR;
            }
            /*说明read事件还没完成*/
            return NGX_AGAIN;
        }
    }

    if (c->read->timer_set) {
        /*删除超时*/
        ngx_del_timer(c->read);
    }
     
    /*处理完上面的各种情况，终于可以写文件保存起来了*/
    if (rb->temp_file || r->request_body_in_file_only) {

        /* save the last part */
        /*写文件*/
        if (ngx_http_write_request_body(r, rb->to_write) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        b = ngx_calloc_buf(r->pool);
        if (b == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        /*这个b将连上rb->bufs链，它里面保存着temp_file的文件信息，包括offset这些*/
        b->in_file = 1;
        b->file_pos = 0;
        b->file_last = rb->temp_file->file.offset;
        b->file = &rb->temp_file->file;
        /*TODO:为何这里只可能有两个buf*/
        if (rb->bufs->next) {
            rb->bufs->next->buf = b;

        } else {
            rb->bufs->buf = b;
        }
    }

    if (rb->bufs->next
        && (r->request_body_in_file_only || r->request_body_in_single_buf))
    {
        /*当设置了body放在单独文件中或内存中与rb->bufs中有两个buf时，丢弃前一个buf*/
        rb->bufs = rb->bufs->next;
    }

    r->read_event_handler = ngx_http_block_reading;
    /*已读完了所有数据，可调用回调函数了*/
    rb->post_handler(r);

    return NGX_OK;
}


static ngx_int_t
ngx_http_write_request_body(ngx_http_request_t *r, ngx_chain_t *body)
{
    ssize_t                    n;
    ngx_temp_file_t           *tf;
    ngx_http_request_body_t   *rb;
    ngx_http_core_loc_conf_t  *clcf;

    rb = r->request_body;
    /*temp_file未定义*/
    if (rb->temp_file == NULL) {
        tf = ngx_pcalloc(r->pool, sizeof(ngx_temp_file_t));
        if (tf == NULL) {
            return NGX_ERROR;
        }

        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        tf->file.fd = NGX_INVALID_FILE;
        tf->file.log = r->connection->log;
        /*TODO:r->client_body_tmep_path在编译时已写死?*/
        tf->path = clcf->client_body_temp_path;
        tf->pool = r->pool;
        tf->warn = "a client request body is buffered to a temporary file";
        tf->log_level = r->request_body_file_log_level;
	/*temp文件是否持久保存*/
        tf->persistent = r->request_body_in_persistent_file;
        tf->clean = r->request_body_in_clean_file;
	/*文件目录访问权限相关*/
        if (r->request_body_file_group_access) {
	    /*open 调用函数中的 mask*/
            tf->access = 0660;
        }

        rb->temp_file = tf;

        if (body == NULL) {
            /* empty body with r->request_body_in_file_only */
            /*创建temp文件，就返回了*/
            if (ngx_create_temp_file(&tf->file, tf->path, tf->pool,
                                     tf->persistent, tf->clean, tf->access)
                != NGX_OK)
            {
                return NGX_ERROR;
            }

            return NGX_OK;
        }
    }
    /*如果rb->temp_file != NULL*/
    n = ngx_write_chain_to_temp_file(rb->temp_file, body);

    /* TODO: n == 0 or not complete and level event */

    if (n == NGX_ERROR) {
        return NGX_ERROR;
    }
    /*更新写文件偏移*/
    rb->temp_file->offset += n;

    return NGX_OK;
}

/*以下为忽略body之类的函数，暂时用不上所以暂不分析了：）*/
ngx_int_t
ngx_http_discard_request_body(ngx_http_request_t *r)
{
    ssize_t       size;
    ngx_event_t  *rev;

    if (r != r->main || r->discard_body) {
        return NGX_OK;
    }

    if (ngx_http_test_expect(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rev = r->connection->read;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0, "http set discard body");

    if (rev->timer_set) {
        ngx_del_timer(rev);
    }

    if (r->headers_in.content_length_n <= 0 || r->request_body) {
        return NGX_OK;
    }

    size = r->header_in->last - r->header_in->pos;

    if (size) {
        if (r->headers_in.content_length_n > size) {
            r->header_in->pos += size;
            r->headers_in.content_length_n -= size;

        } else {
            r->header_in->pos += (size_t) r->headers_in.content_length_n;
            r->headers_in.content_length_n = 0;
            return NGX_OK;
        }
    }

    if (ngx_http_read_discarded_request_body(r) == NGX_OK) {
        r->lingering_close = 0;
        return NGX_OK;
    }

    /* == NGX_AGAIN */

    r->read_event_handler = ngx_http_discarded_request_body_handler;

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->count++;
    r->discard_body = 1;

    return NGX_OK;
}


void
ngx_http_discarded_request_body_handler(ngx_http_request_t *r)
{
    ngx_int_t                  rc;
    ngx_msec_t                 timer;
    ngx_event_t               *rev;
    ngx_connection_t          *c;
    ngx_http_core_loc_conf_t  *clcf;

    c = r->connection;
    rev = c->read;

    if (rev->timedout) {
        c->timedout = 1;
        c->error = 1;
        ngx_http_finalize_request(r, NGX_ERROR);
        return;
    }

    if (r->lingering_time) {
        timer = (ngx_msec_t) (r->lingering_time - ngx_time());

        if (timer <= 0) {
            r->discard_body = 0;
            r->lingering_close = 0;
            ngx_http_finalize_request(r, NGX_ERROR);
            return;
        }

    } else {
        timer = 0;
    }

    rc = ngx_http_read_discarded_request_body(r);

    if (rc == NGX_OK) {
        r->discard_body = 0;
        r->lingering_close = 0;
        ngx_http_finalize_request(r, NGX_DONE);
        return;
    }

    /* rc == NGX_AGAIN */

    if (ngx_handle_read_event(rev, 0) != NGX_OK) {
        c->error = 1;
        ngx_http_finalize_request(r, NGX_ERROR);
        return;
    }

    if (timer) {

        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        timer *= 1000;

        if (timer > clcf->lingering_timeout) {
            timer = clcf->lingering_timeout;
        }

        ngx_add_timer(rev, timer);
    }
}


static ngx_int_t
ngx_http_read_discarded_request_body(ngx_http_request_t *r)
{
    size_t   size;
    ssize_t  n;
    u_char   buffer[NGX_HTTP_DISCARD_BUFFER_SIZE];

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http read discarded body");

    for ( ;; ) {
        if (r->headers_in.content_length_n == 0) {
            r->read_event_handler = ngx_http_block_reading;
            return NGX_OK;
        }

        if (!r->connection->read->ready) {
            return NGX_AGAIN;
        }

        size = (r->headers_in.content_length_n > NGX_HTTP_DISCARD_BUFFER_SIZE) ?
                   NGX_HTTP_DISCARD_BUFFER_SIZE:
                   (size_t) r->headers_in.content_length_n;

        n = r->connection->recv(r->connection, buffer, size);

        if (n == NGX_ERROR) {
            r->connection->error = 1;
            return NGX_OK;
        }

        if (n == NGX_AGAIN) {
            return NGX_AGAIN;
        }

        if (n == 0) {
            return NGX_OK;
        }

        r->headers_in.content_length_n -= n;
    }
}

/*这里只是在判断有没有http 1.1的expect头并发送相应的回答*/
static ngx_int_t
ngx_http_test_expect(ngx_http_request_t *r)
{
    ngx_int_t   n;
    ngx_str_t  *expect;

    if (r->expect_tested
        || r->headers_in.expect == NULL
        || r->http_version < NGX_HTTP_VERSION_11)
    {
        return NGX_OK;
    }

    r->expect_tested = 1;

    expect = &r->headers_in.expect->value;

    if (expect->len != sizeof("100-continue") - 1
        || ngx_strncasecmp(expect->data, (u_char *) "100-continue",
                           sizeof("100-continue") - 1)
           != 0)
    {
        return NGX_OK;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "send 100 Continue");

    n = r->connection->send(r->connection,
                            (u_char *) "HTTP/1.1 100 Continue" CRLF CRLF,
                            sizeof("HTTP/1.1 100 Continue" CRLF CRLF) - 1);

    if (n == sizeof("HTTP/1.1 100 Continue" CRLF CRLF) - 1) {
        return NGX_OK;
    }

    /* we assume that such small packet should be send successfully */

    return NGX_ERROR;
}
