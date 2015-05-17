//  local.c
//  proximac
//
//  Created by jedihy on 15-5-12.
//  Copyright (c) 2015年 jedihy. All rights reserved.
//

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>

#include <arpa/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <uv.h>
#include <unistd.h>
#include <getopt.h>
#include "jconf.h"
#include "local.h"
#include "utils.h"
#include "socks5.h"

conf_t conf;
FILE * logfile    = NULL;
uv_loop_t *loop = NULL;
int log_to_file = 1;
int gSocket = -1;
int gSocket_for_release = -1;

#define MYBUNDLEID		"com.proximac.kext"

// callback functions
static void server_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void server_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void remote_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void remote_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void remote_write_cb(uv_write_t *req, int status);
static void server_after_close_cb(uv_handle_t* handle);
static void connect_to_remote_cb(uv_connect_t* req, int status);
static void server_accept_cb(uv_stream_t *server, int status);
static void remote_after_close_cb(uv_handle_t* handle);
static void connect_to_remote_cb(uv_connect_t* req, int status);
static void final_after_close_cb(uv_handle_t* handle);

// pid rb-tree structure
/* Red-black tree of pid to be Hooked for proximac */


struct pid_tree pid_list;

static inline int
pid_cmp(const struct pid *tree_a, const struct pid *tree_b)
{
    if (tree_a->pid == tree_b->pid)
        return 0;
    return tree_a->pid < tree_b->pid? -1:1;
}

RB_GENERATE(pid_tree, pid, rb_link, pid_cmp);
static void final_after_close_cb(uv_handle_t* handle) {
    LOGW("final_after_close_cb");
    server_ctx_t* server_ctx = handle->data;
    if (server_ctx->buf != NULL)
        free(server_ctx->buf);
    free(server_ctx);
}

static void server_after_close_cb(uv_handle_t* handle) {

    server_ctx_t* server_ctx = handle->data;
    uv_read_stop((uv_stream_t*) &server_ctx->remote_handle);
    uv_close((uv_handle_t*)(void *)&server_ctx->remote_handle, final_after_close_cb);
}

static void remote_after_close_cb(uv_handle_t* handle) {
    server_ctx_t* server_ctx = handle->data;
    uv_read_stop((uv_stream_t*) &server_ctx->server_handle);
    uv_close((uv_handle_t*)(void *)&server_ctx->server_handle, final_after_close_cb);
}

static void remote_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
    *buf = uv_buf_init((char*) malloc(BUF_SIZE), BUF_SIZE);
    assert(buf->base != NULL);
}

static void remote_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    server_ctx_t *server_ctx = stream->data;
    if (nread <= 0) {
        if (nread == 0) {
            if (buf->len > 0)
                free(buf->base);
            return;
        }
        TRY_CLOSE(server_ctx, &server_ctx->remote_handle, remote_after_close_cb);
        
    } else {
        //        SHOW_BUFFER(buf->base, nread);
        if (server_ctx->remote_stage == 0) {
            if (buf->base[0] == 0x05 && buf->base[1] == 0x00) {
                LOGD("socks5 server response 05 00 (OK)");
            }
            
            write_req_t *wr = (write_req_t*) malloc(sizeof(write_req_t));
            wr->req.data    = server_ctx;
            int len = 4 + 1 + server_ctx->addrlen + sizeof(server_ctx->port);
            char* socks5req = malloc(len);
            socks5req[0] = 0x05;
            socks5req[1] = 0x01;
            socks5req[2] = 0x00;
            socks5req[3] = 0x03;
            socks5req[4] = server_ctx->addrlen;
            memcpy(socks5req + 5, server_ctx->remote_addr, server_ctx->addrlen);
            uint16_t port = htons(server_ctx->port);
            memcpy(socks5req + 5 + server_ctx->addrlen, &port, sizeof(server_ctx->port));
            wr->buf = uv_buf_init(socks5req, len);
            uv_write(&wr->req, (uv_stream_t*)&server_ctx->remote_handle, &wr->buf, 1, remote_write_cb);
            server_ctx->remote_stage = 1;
//            LOGW("server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
            
        } else if (server_ctx->remote_stage == 1) {
            if (buf->base[0] == 0x05) {
                LOGD("socks5 server works");
                server_ctx->server_stage = 1;
                write_req_t *wr = (write_req_t*) malloc(sizeof(write_req_t));
                if (server_ctx->buf_len) {
                    char* tmpbuf = malloc(server_ctx->buf_len);
                    memcpy(tmpbuf, server_ctx->buf, server_ctx->buf_len);
                    free(server_ctx->buf);
                    server_ctx->buf = NULL;
                    wr->req.data    = server_ctx;
                    wr->buf = uv_buf_init(tmpbuf, server_ctx->buf_len);
                        uv_write(&wr->req, (uv_stream_t*)&server_ctx->remote_handle, &wr->buf, 1, remote_write_cb);
                }
                uv_read_start((uv_stream_t*)&server_ctx->server_handle, server_alloc_cb, server_read_cb);
                server_ctx->remote_stage = 2;
//                LOGW("server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
            }
            
        } else if (server_ctx->remote_stage == 2) {
            write_req_t *wr = (write_req_t*) malloc(sizeof(write_req_t));
            wr->req.data    = server_ctx;
            wr->buf = uv_buf_init(buf->base, nread);
            uv_write(&wr->req, (uv_stream_t*)&server_ctx->server_handle, &wr->buf, 1, remote_write_cb);
//                LOGW("server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
        }
        
    }
    
}


static void remote_write_cb(uv_write_t *req, int status) {
    write_req_t* wr = (write_req_t*) req;
    server_ctx_t* server_ctx = req->data;
    if (status) {
        if (status != UV_ECANCELED) {
            LOGW("remote_write_cb TRY_CLOSE");
            if (req->handle == &server_ctx->server_handle) {
                TRY_CLOSE(server_ctx, &server_ctx->server_handle, server_after_close_cb);
            } else {
                TRY_CLOSE(server_ctx, &server_ctx->remote_handle, remote_after_close_cb);
            }
        }
    }
    
    assert(wr->req.type == UV_WRITE);
    /* Free the read/write buffer and the request */
    free(wr->buf.base);
    free(wr);
//        LOGW("server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
}

static void connect_to_remote_cb(uv_connect_t* req, int status) {
    server_ctx_t* server_ctx = (server_ctx_t *)req->data;
//    LOGW("4 server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
    if (status) {
        // cleanup
        if (status != UV_ECANCELED) {
            TRY_CLOSE(server_ctx, &server_ctx->remote_handle, remote_after_close_cb);
        }
        free(req);
        return;
    }
    
    uv_read_start(req->handle, remote_alloc_cb, remote_read_cb);
    write_req_t *wr = (write_req_t*) malloc(sizeof(write_req_t));
    wr->req.data    = server_ctx;
    char* socks5req = malloc(3);
    socks5req[0] = 0x05;
    socks5req[1] = 0x01;
    socks5req[2] = 0x00;
    wr->buf = uv_buf_init(socks5req, 3);
    uv_write(&wr->req, (uv_stream_t*)&server_ctx->remote_handle, &wr->buf, 1, remote_write_cb);
}


static void server_accept_cb(uv_stream_t *server, int status) {
    if (status) {
        LOGE("server accept failed!");
        return;
    }
    server_ctx_t *server_ctx = calloc(1, sizeof(server_ctx_t));
    // calloc set all members to zero!
    
    server_ctx->server_handle.data = server_ctx;
    server_ctx->remote_handle.data = server_ctx;
    server_ctx->remote_stage = 0;
    server_ctx->server_stage = 0;
    uv_tcp_init(loop, &server_ctx->server_handle);
    uv_tcp_init(loop, &server_ctx->remote_handle);
    uv_tcp_nodelay(&server_ctx->server_handle, 1);
    LOGW("server_accept_cb %x %d", server_ctx, server_ctx->remote_handle.type);
    int r = uv_accept(server, (uv_stream_t*) &server_ctx->server_handle);
    if (r) {
        fprintf(stderr, "accepting connection failed %d", r);
        uv_close((uv_handle_t*) &server_ctx->server_handle, NULL);
    }
    
   
    uv_read_start((uv_stream_t*) &server_ctx->server_handle, server_alloc_cb,
                  server_read_cb);
    
}

static void server_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
    *buf = uv_buf_init((char*) malloc(BUF_SIZE), BUF_SIZE);
    assert(buf->base != NULL);
}

static void server_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    server_ctx_t *server_ctx = stream->data;
//    LOGW("1 server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
    if (nread <= 0) {
        if (nread == 0) {
            if (buf->len > 0)
                free(buf->base);
            return;
        }
//        LOGW("server_read_cb TRY_CLOSE");
        TRY_CLOSE(server_ctx, &server_ctx->server_handle, server_after_close_cb);
        
    } else {
        if (server_ctx->server_stage == 0) {
//            LOGW("2 server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);

            uv_read_stop(stream);
            server_ctx->buf = buf->base;
            server_ctx->buf_len = nread;
            server_ctx->addrlen = server_ctx->buf[0];
            memcpy(server_ctx->remote_addr, server_ctx->buf + sizeof(char), server_ctx->addrlen);
            memcpy(&server_ctx->port, server_ctx->buf + 1 + server_ctx->addrlen, sizeof(server_ctx->port));
            int tmpbuf_len = nread - server_ctx->addrlen - 1 - sizeof(server_ctx->port);
            if (tmpbuf_len) {
                char* tmpbuf = malloc(tmpbuf_len);
                memcpy(tmpbuf, server_ctx->buf + server_ctx->addrlen + 1 + sizeof(server_ctx->port), tmpbuf_len);
                
                server_ctx->buf_len = tmpbuf_len;
                server_ctx->buf = tmpbuf;
            }
            else {
                server_ctx->buf_len = NULL;
                server_ctx->buf = NULL;
            }

            free(buf->base);
            
            LOGD("server_ctx %x addrlen = %d addr = %s port = %d", server_ctx, server_ctx->addrlen, server_ctx->remote_addr, server_ctx->port);
            
            struct sockaddr_in remote_addr;
            memset(&remote_addr, 0, sizeof(remote_addr));
            int r = uv_ip4_addr(conf.local_address, conf.localport, &remote_addr);
            
            uv_connect_t *remote_conn_req = calloc(1, sizeof(uv_connect_t));
            remote_conn_req->data = server_ctx;
            uv_tcp_connect(remote_conn_req, &server_ctx->remote_handle, (struct sockaddr*)&remote_addr, connect_to_remote_cb);
//            LOGW("3 server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
        }
        else if (server_ctx->server_stage == 1) {
            write_req_t *wr = (write_req_t*) malloc(sizeof(write_req_t));
            wr->req.data    = server_ctx;
            wr->buf = uv_buf_init(buf->base, nread);
            uv_write(&wr->req, (uv_stream_t*)&server_ctx->remote_handle, &wr->buf, 1, remote_write_cb);
//            LOGW("server_after_close_cb %x %d", server_ctx, server_ctx->remote_handle.type);
            // let write_cb to release buf->base
        }
    }
    
}

int process_find() {
    FILE *fstream = NULL;
    char buff[1024];
    
    struct pid *pid_tmp = NULL;
    RB_FOREACH(pid_tmp, pid_tree, &pid_list) {
        int process_name_len = strlen(pid_tmp->name);
        memset(buff,0,sizeof(buff));
        char* template_str = "ps -e|grep \"\"|head -1|awk \'{if($4==\"ps\")print -1;else print $1}\'";
        int template_len = strlen(template_str);
        char* command = calloc(1, process_name_len + template_len + 1);
        sprintf(command, "ps -e|grep \"%s\"|head -1|awk \'{if($6==\"ps\")print -1;else print $1}\'", pid_tmp->name);
        if(NULL == (fstream = popen(command, "r"))) {
            fprintf(stderr, "execute command failed: %s", strerror(errno));
            return -1;
        }
        
        if(NULL != fgets(buff, sizeof(buff), fstream)) {
            pid_tmp->pid = atoi(buff);
            if (pid_tmp->pid == -1)
                FATAL("process not found");
        }
        else {
            FATAL("shell command execute failed");
        }
        
        LOGI("process to be hooked %s PID = %d", pid_tmp->name, pid_tmp->pid);
        
        pclose(fstream);
    
    }

    return 0;
}

int tell_kernel_to_unhook() {
    struct ctl_info ctl_info;
    struct sockaddr_ctl sc;
    errno_t retval = 0;
    int tmp = 0;

    int result = 0;
    int size = sizeof(result);
    retval = getsockopt(gSocket, SYSPROTO_CONTROL, PROXIMAC_OFF, &result, &size);
    if ( -1 == retval) {
        LOGI("getsockopt failure PROXIMAC_OFF");
        exit(EXIT_FAILURE);
    } else if (EINPROGRESS == retval) {
        LOGI("ERROR: Maybe Proximac is unregistering filters...");
    }
    
    if (result == EINPROGRESS) {
        LOGI("Proximac is unregistering filters...");
        LOGI("Wait a few sec to for Proximac to release kernel resources");

    }
    
    return 0;

}

int tell_kernel_to_hook() {
    struct ctl_info ctl_info;
    struct sockaddr_ctl sc;
    errno_t retval = 0;
    LOGI("tell kernel");
    
    gSocket = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (gSocket < 0) {
        LOGE("socket SYSPROTO_CONTROL");
        exit(EXIT_FAILURE);
    }
    
    bzero(&ctl_info, sizeof(struct ctl_info));

    strcpy(ctl_info.ctl_name, MYBUNDLEID);
    if (ioctl(gSocket, CTLIOCGINFO, &ctl_info) == -1) {
        LOGE("ioctl CTLIOCGINFO");
        exit(EXIT_FAILURE);
    }
    
    bzero(&sc, sizeof(struct sockaddr_ctl));
    sc.sc_len = sizeof(struct sockaddr_ctl);
    sc.sc_family = AF_SYSTEM;
    sc.ss_sysaddr = SYSPROTO_CONTROL;
    sc.sc_id = ctl_info.ctl_id;
    sc.sc_unit = 0;
    
    
    if (connect(gSocket, (struct sockaddr *)&sc, sizeof(struct sockaddr_ctl))) {
        LOGE("connect");
        exit(EXIT_FAILURE);
    }
    
   
    int tmp = 0;
    retval = setsockopt(gSocket, SYSPROTO_CONTROL, PROXIMAC_ON, &tmp, sizeof(tmp));
    if (retval) {
        LOGE("setsockopt failure PROXIMAC_ON");
        return retval;
    }
    
    struct pid *pid_tmp = NULL;
    int pidset_checksum = 0;
    RB_FOREACH(pid_tmp, pid_tree, &pid_list) {
        retval = setsockopt(gSocket, SYSPROTO_CONTROL, HOOK_PID, &pid_tmp->pid, sizeof(pid_tmp->pid));
        if (retval)
        {
            LOGE("setsockopt failure HOOK_PID");
            return retval;
        }
        pidset_checksum += pid_tmp->pid;
    }
    
    int pidget_checksum = 0;
    int size = sizeof(pidget_checksum);
    retval = getsockopt(gSocket, SYSPROTO_CONTROL, HOOK_PID, &pidget_checksum, &size);
    if (retval) {
        LOGE("getsockopt HOOK_PID failure");
        return retval;
    }
    
    if (pidget_checksum == pidset_checksum)
        LOGI("Hook Succeed!");
    else
        LOGI("Hook Fail! pidget_checksum = %d pidset_checksum = %d", pidget_checksum, pidset_checksum);
    
    int pid_num = 0;
    size = sizeof(pid_num);
    
    retval = getsockopt(gSocket, SYSPROTO_CONTROL, PIDLIST_STATUS, &pid_num, &size);
    if (retval)
    {
        LOGE("getsockopt PIDLIST_STATUS failure");
        return retval;
    }
    
    LOGI("pid_num = %d", pid_num);

    
    return retval;
}

void signal_handler_ctl_z(uv_signal_t *handle, int signum)
{
    LOGI("Ctrl+Z pressed, tell kernel to UnHook socket");
    tell_kernel_to_unhook();
    uv_loop_t* loop = handle->data;
    uv_signal_stop(handle);
    uv_stop(loop);
    exit(0);
}

void signal_handler_ctl_c(uv_signal_t *handle, int signum)
{
    LOGI("Ctrl+C pressed, tell kernel to UnHook socket");
    tell_kernel_to_unhook();
    uv_loop_t* loop = handle->data;
    uv_signal_stop(handle);
    uv_stop(loop);
    exit(0);
}


int main(int argc, char **argv) {
    int c, option_index = 0, daemon = 0;
    char* configfile = NULL;
    char* logfile_path = "/tmp/proximac.log";
    RB_INIT(&pid_list);
    opterr = 0;
    static struct option long_options[] =
    {
        {0, 0, 0, 0}
    };
    
    while ((c = getopt_long(argc, argv, "c:d",
                            long_options, &option_index)) != -1) {
        switch (c) {
            case 'd':
            {
                daemon = 1;
                break;
            }
            case 'c':
            {
                configfile = optarg;
                break;
            }
            default:
            {
                opterr = 1;
                break;
            }
        }
    }

    if (opterr == 1|| configfile == NULL) {
        fprintf(stderr, "No config file specified!\n");
        usage();
        exit(EXIT_FAILURE);
    }
    
    if (configfile) {
        read_conf(configfile, &conf);
    }
    
    int r = tell_kernel_to_hook();
    if (r) {
        if (r == EAGAIN)
            FATAL("Please wait a few seconds for Proximac release resources in kernel (normally in 10 sec)");
        else
            FATAL("kernel cannot hook this PID due to various reasons");
    }
    
    if (daemon == 1)
        init_daemon();
    
    if (log_to_file)
        USE_LOGFILE(logfile_path);
    
    struct sockaddr_in bind_addr;
    loop = malloc(sizeof *loop);
    uv_loop_init(loop);
    listener_t *listener      = calloc(1, sizeof(server_ctx_t));
    listener->handle.data     = listener;
    uv_tcp_init(loop, &listener->handle);
    uv_tcp_nodelay(&listener->handle, 1);
    
    r = uv_ip4_addr(conf.proximac_listen_address, conf.proximac_port, &bind_addr);
    if (r)
        LOGE("address error");
    r = uv_tcp_bind(&listener->handle, (struct sockaddr*)&bind_addr, 0);
    if (r)
        LOGI("bind error");
    r = uv_listen((uv_stream_t*) &listener->handle, 128 /*backlog*/, server_accept_cb);
    if (r)
        LOGI("listen error port");
    LOGI("Listening on %s:%d", conf.proximac_listen_address, conf.proximac_port);

    signal(SIGPIPE, SIG_IGN);
    uv_signal_t sigint,sigstp,sigkil;
    sigkil.data = loop;
    sigint.data = loop;
    sigstp.data = loop;
    int n = uv_signal_init(loop, &sigint);
    n = uv_signal_init(loop, &sigstp);
    n = uv_signal_init(loop, &sigkil);
    n = uv_signal_start(&sigint, signal_handler_ctl_z, SIGKILL);
    n = uv_signal_start(&sigint, signal_handler_ctl_c, SIGINT);
    n = uv_signal_start(&sigstp, signal_handler_ctl_z, SIGTSTP);
    
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_close(loop);
    free(loop);
    CLOSE_LOGFILE;
    return 0;
}