/*
   Copyright 2021 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <event2/buffer.h>
#include <event2/event.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <bdb_api.h>
#include <hostname_support.h>
#include <intern_strings.h>
#include <net_appsock.h>
#include <net_int.h>
#include <rtcpu.h>
#include <sql.h>
#include <sqlwriter.h>
#include <ssl_glue.h>
#include <str0.h>
#include <timer_util.h>

#include <newsql.h>

extern int gbl_nid_dbname;
extern SSL_CTX *gbl_ssl_ctx;
extern ssl_mode gbl_client_ssl_mode;

struct ping_pong {
    int status;
    struct event *ev;
    struct timeval start;
};

struct ssl_data {
    SSL *ssl;
    X509 *cert;
};

struct newsql_appdata_evbuffer {
    NEWSQL_APPDATA_COMMON /* Must be first */

    int fd;
    struct sqlclntstate clnt;
    struct newsqlheader hdr;
    struct ping_pong *ping;

    struct evbuffer *rd_buf;
    struct event *rd_hdr_ev;
    struct event *rd_payload_ev;

    unsigned active : 1; /* We count against MAXAPPSOCKSLIMIT */
    unsigned initial : 1; /* New connection or called newsql_reset */
    unsigned local : 1;

    struct sqlwriter *writer;
    struct ssl_data *ssl_data;

    void (*add_rd_event_fn)(struct newsql_appdata_evbuffer *, struct event *, struct timeval *);
    int (*rd_evbuffer_fn)(struct newsql_appdata_evbuffer *);
    void (*wr_dbinfo_fn)(struct newsql_appdata_evbuffer *);
};

static void add_rd_event(struct newsql_appdata_evbuffer *, struct event *, struct timeval *);
static void rd_hdr(int, short, void *);
static int rd_evbuffer_plaintext(struct newsql_appdata_evbuffer *);
static int rd_evbuffer_ssl(struct newsql_appdata_evbuffer *);
static void disable_ssl_evbuffer(struct newsql_appdata_evbuffer *);
static int newsql_write_hdr_evbuffer(struct sqlclntstate *, int, int);

static void free_newsql_appdata_evbuffer(int dummyfd, short what, void *arg)
{
    check_appsock_rd_thd();
    struct newsql_appdata_evbuffer *appdata = arg;
    int fd = appdata->fd;
    struct sqlclntstate *clnt = &appdata->clnt;
    if (appdata->ping) {
        event_free(appdata->ping->ev);
        free(appdata->ping);
        appdata->ping = NULL;
    }
    if (appdata->rd_hdr_ev) {
        event_free(appdata->rd_hdr_ev);
        appdata->rd_hdr_ev = NULL;
    }
    if (appdata->rd_payload_ev) {
        event_free(appdata->rd_payload_ev);
        appdata->rd_payload_ev = NULL;
    }
    if (appdata->rd_buf) {
        evbuffer_free(appdata->rd_buf);
        appdata->rd_buf = NULL;
    }
    rem_sql_evbuffer(clnt);
    rem_lru_evbuffer(clnt);
    if (appdata->active) {
        rem_appsock_connection_evbuffer(clnt);
    }
    if (appdata->ssl_data) {
        disable_ssl_evbuffer(appdata);
        free(appdata->ssl_data);
    }
    sqlwriter_free(appdata->writer);
    free_newsql_appdata(clnt);
    free(appdata);
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

static void newsql_cleanup(int dummyfd, short what, void *arg)
{
    check_appsock_timer_thd();
    struct newsql_appdata_evbuffer *appdata = arg;
    sql_disable_heartbeat(appdata->writer);
    sql_disable_timeout(appdata->writer);
    evtimer_once(appsock_rd_base, free_newsql_appdata_evbuffer, appdata);
}

static int newsql_flush_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    return sql_flush(appdata->writer);
}

static void newsql_read_hdr(int dummyfd, short what, void *arg)
{
    struct newsql_appdata_evbuffer *appdata = arg;
    add_lru_evbuffer(&appdata->clnt);
    rd_hdr(-1, 0, appdata);
}

static void newsql_read_again(int dummyfd, short what, void *arg)
{
    struct newsql_appdata_evbuffer *appdata = arg;
    sql_disable_heartbeat(appdata->writer);
    sql_disable_timeout(appdata->writer);
    struct sqlclntstate *clnt = &appdata->clnt;
    if (clnt->query_rc) {
        if (in_client_trans(clnt)) {
            clnt->had_errors = 1;
        } else {
            /* newsql over sbuf and fastsql drop the connection in this
             * situation - we can do better; connections are expensive */
            reset_clnt_flags(clnt);
        }
    }
    newsql_read_hdr(-1, 0, appdata);
}

static void newsql_reset_evbuffer(struct newsql_appdata_evbuffer *appdata)
{
    appdata->initial = 1;
    newsql_reset(&appdata->clnt);
}

static int newsql_done_cb(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    if (clnt->query_rc == CDB2ERR_IO_ERROR) { /* dispatch timed out */
        return evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
    }
    if (clnt->osql.replay == OSQL_RETRY_DO) {
        clnt->done_cb = NULL;
        srs_tran_replay_inline(clnt);
        clnt->done_cb = newsql_done_cb;
    } else if (clnt->osql.history && clnt->ctrl_sqlengine == SQLENG_NORMAL_PROCESS) {
        clnt->sql = NULL;
        srs_tran_destroy(clnt);
    } else if (appdata->query) {
        clnt->sql = NULL;
        cdb2__query__free_unpacked(appdata->query, NULL);
        appdata->query = NULL;
    }
    if (sql_done(appdata->writer) == 0) {
        newsql_read_again(-1, 0, appdata);
    } else {
        evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
    }
    return 0;
}

static int newsql_get_fileno_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    return appdata->fd;
}

static int newsql_get_x509_attr_evbuffer(struct sqlclntstate *clnt, int nid, void *out, int outsz)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    if (!appdata->ssl_data || !appdata->ssl_data->cert) return EINVAL;
    return ssl_x509_get_attr(appdata->ssl_data->cert, nid, out, outsz);
}

static int newsql_has_ssl_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    if (appdata->ssl_data && appdata->ssl_data->ssl) return 1;
    return 0;
}

static int newsql_has_x509_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    if (appdata->ssl_data && appdata->ssl_data->cert) return 1;
    return 0;
}

static int newsql_local_check_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    return appdata->local;
}

static int newsql_peer_check_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    return sql_peer_check(appdata->writer);
}

static int newsql_set_timeout_evbuffer(struct sqlclntstate *clnt, int timeout_ms)
{
    return 0; /* nop */
}

static int rd_evbuffer(struct newsql_appdata_evbuffer *appdata)
{
    return appdata->rd_evbuffer_fn(appdata); /* rd_evbuffer_plaintext */
}

static void ping_pong_cb(int fd, short what, void *arg)
{
    struct newsql_appdata_evbuffer *appdata = arg;
    struct ping_pong *ping = appdata->ping;
    struct event_base *wrbase = sql_wrbase(appdata->writer);
    int n = rd_evbuffer(appdata);
    if (n <= 0 && (what & EV_READ)) {
        ping->status = -2;
        event_base_loopbreak(wrbase);
        return;
    }
    struct newsqlheader hdr;
    if (evbuffer_get_length(appdata->rd_buf) < sizeof(hdr)) {
        struct timeval now;
        gettimeofday(&now, NULL);
        struct timeval timeout;
        timersub(&now, &ping->start, &timeout);
        if (timeout.tv_sec) {
            event_base_loopbreak(wrbase);
        } else {
            add_rd_event(appdata, ping->ev, &timeout);
        }
        return;
    }
    evbuffer_remove(appdata->rd_buf, &hdr, sizeof(hdr));
    if (ntohl(hdr.type) == RESPONSE_HEADER__SQL_RESPONSE_PONG) {
        ping->status = 0;
    } else {
        ping->status = -3;
    }
    event_base_loopbreak(wrbase);
}

/**
 * Called by `recv_ping_response()` in lua/sp.c
 * @retval  0: Received response
 * @retval -1: Timeout (1 second)
 * @retval -2: Read error
 * @retval -3: Unexpected respnose
 */
static int newsql_ping_pong_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    struct event_base *wrbase = sql_wrbase(appdata->writer);
    if (!appdata->ping) {
        appdata->ping = malloc(sizeof(struct ping_pong));
        int flags = EV_READ | EV_TIMEOUT;
        appdata->ping->ev = event_new(wrbase, appdata->fd, flags, ping_pong_cb, appdata);
    }
    struct ping_pong *ping = appdata->ping;
    ping->status = -1;
    gettimeofday(&ping->start, NULL);
    struct timeval onesec = {.tv_sec = 1};
    add_rd_event(appdata, ping->ev, &onesec);
    event_base_dispatch(wrbase);
    return ping->status;
}

static void wr_dbinfo(int fd, short what, void *arg)
{
    struct newsql_appdata_evbuffer *appdata = arg;
    appdata->wr_dbinfo_fn(appdata); /* wr_dbinfo_plaintext */
}

static void wr_dbinfo_int(struct newsql_appdata_evbuffer *appdata, int write_result)
{
    if (write_result <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
        return;
    }
    struct evbuffer *wr_buf = sql_wrbuf(appdata->writer);
    if (evbuffer_get_length(wr_buf) == 0) {
        newsql_read_hdr(-1, 0, appdata);
        return;
    }
    event_base_once(appsock_timer_base, appdata->fd, EV_WRITE, wr_dbinfo, appdata, NULL);
}

static void wr_dbinfo_ssl(struct newsql_appdata_evbuffer *appdata)
{
    struct evbuffer *wr_buf = sql_wrbuf(appdata->writer);
    int len = evbuffer_get_length(wr_buf);
    if (len > KB(16)) len = KB(16);
    const void *buf = evbuffer_pullup(wr_buf, len);
    int rc = SSL_write(appdata->ssl_data->ssl, buf, len);
    if (rc > 0) evbuffer_drain(wr_buf, rc);
    wr_dbinfo_int(appdata, rc);
}

static void wr_dbinfo_plaintext(struct newsql_appdata_evbuffer *appdata)
{
    struct evbuffer *wr_buf = sql_wrbuf(appdata->writer);
    int rc = evbuffer_write(wr_buf, appdata->fd);
    wr_dbinfo_int(appdata, rc);
}

static void process_dbinfo_int(struct newsql_appdata_evbuffer *appdata, struct evbuffer *buf)
{
    CDB2DBINFORESPONSE__Nodeinfo *master = NULL;
    CDB2DBINFORESPONSE__Nodeinfo *nodes[REPMAX];
    CDB2DBINFORESPONSE__Nodeinfo same_dc[REPMAX], diff_dc[REPMAX];
    int num_same_dc = 0, num_diff_dc = 0;
    host_node_type *hosts[REPMAX];
    int num_hosts = get_hosts_evbuffer(REPMAX, hosts);
    int my_dc = machine_dc(gbl_myhostname);
    int process_incoherent = bdb_amimaster(thedb->bdb_env);
    for (int i = 0; i < num_hosts; ++i) {
        CDB2DBINFORESPONSE__Nodeinfo *node;
        int dc = machine_dc(hosts[i]->host);
        node = (dc == my_dc) ?  &same_dc[num_same_dc++] : &diff_dc[num_diff_dc++];
        cdb2__dbinforesponse__nodeinfo__init(node);
        node->has_room = 1;
        node->room = dc;
        node->has_port = 1;
        node->port = hosts[i]->port;
        node->name = hosts[i]->host;
        node->incoherent = process_incoherent ? is_incoherent(thedb->bdb_env, node->name) : 0;
        const char *who = bdb_whoismaster(thedb->bdb_env);
        if (who && strcmp(who, node->name) == 0) {
            master = node;
        }
    }
    int j = 0;
    for (int i = 0; i < num_same_dc; ++i, ++j) {
        nodes[j] = &same_dc[i];
        nodes[j]->number = j;
    }
    for (int i = 0; i < num_diff_dc; ++i, ++j) {
        nodes[j] = &diff_dc[i];
        nodes[j]->number = j;
    }
    CDB2DBINFORESPONSE response = CDB2__DBINFORESPONSE__INIT;
    fill_ssl_info(&response);
    response.n_nodes = num_hosts;
    response.master = master;
    response.nodes = nodes;

    int len = cdb2__dbinforesponse__get_packed_size(&response);
    struct newsqlheader hdr = {0};
    hdr.type = htonl(RESPONSE_HEADER__DBINFO_RESPONSE);
    hdr.length = htonl(len);
    uint8_t out[len];
    cdb2__dbinforesponse__pack(&response, out);
    evbuffer_add(buf, &hdr, sizeof(hdr));
    evbuffer_add(buf, out, len);
}

static void process_dbinfo(struct newsql_appdata_evbuffer *appdata)
{
    process_dbinfo_int(appdata, sql_wrbuf(appdata->writer));
    event_base_once(appsock_timer_base, appdata->fd, EV_WRITE, wr_dbinfo, appdata, NULL);
}

static void process_get_effects(struct newsql_appdata_evbuffer *appdata)
{
    CDB2EFFECTS effects = CDB2__EFFECTS__INIT;
    CDB2SQLRESPONSE response = CDB2__SQLRESPONSE__INIT;
    newsql_effects(&response, &effects, &appdata->clnt);
    int len = cdb2__sqlresponse__get_packed_size(&response);
    struct newsqlheader hdr = {0};
    hdr.type = htonl(RESPONSE_HEADER__SQL_EFFECTS);
    hdr.length = htonl(len);
    uint8_t out[len];
    cdb2__sqlresponse__pack(&response, out);
    struct evbuffer *wr_buf = sql_wrbuf(appdata->writer);
    evbuffer_add(wr_buf, &hdr, sizeof(hdr));
    evbuffer_add(wr_buf, out, len);
    event_base_once(appsock_timer_base, appdata->fd, EV_WRITE, wr_dbinfo, appdata, NULL);
}

static int ssl_check(struct newsql_appdata_evbuffer *appdata, CDB2QUERY *query)
{
    if (appdata->ssl_data && appdata->ssl_data->ssl) return 0;
    for (int i = 0; i < query->sqlquery->n_features; ++i) {
        if (CDB2_CLIENT_FEATURES__SSL == query->sqlquery->features[i]) {
            newsql_write_hdr_evbuffer(&appdata->clnt, RESPONSE_HEADER__SQL_RESPONSE_SSL, 0);
            return 1;
        }
    }
    write_response(&appdata->clnt, RESPONSE_ERROR, "database requires SSL connections", CDB2ERR_CONNECT_ERROR);
    return -1;
}

static void process_query(struct newsql_appdata_evbuffer *appdata, CDB2QUERY *query)
{
    int do_read = 0;
    int commit_rollback;
    if (gbl_client_ssl_mode >= SSL_REQUIRE) {
        switch (ssl_check(appdata, query)) {
        case 0: break;
        case 1: do_read = 1; // fallthrough
        default: goto out;
        }
    }
    appdata->query = query;
    CDB2SQLQUERY *sqlquery = appdata->sqlquery = query->sqlquery;
    struct sqlclntstate *clnt = &appdata->clnt;
    if (sqlquery == NULL)
        goto out;
    if (!appdata->active) {
        if (add_appsock_connection_evbuffer(clnt) != 0) {
            add_lru_evbuffer(clnt);
            exhausted_appsock_connections(clnt);
            goto out;
        }
        appdata->active = 1;
    }
    if (appdata->initial) {
        if (newsql_first_run(clnt, sqlquery) != 0) {
            goto out;
        }
        appdata->initial = 0;
    }
    if (newsql_loop(clnt, sqlquery) != 0) {
        goto out;
    }
    if (newsql_should_dispatch(clnt, &commit_rollback) != 0) {
        do_read = 1;
        goto out;
    }
    sql_reset(appdata->writer);
    if (clnt->query_timeout) {
        sql_enable_timeout(appdata->writer, clnt->query_timeout);
    }
    sql_enable_heartbeat(appdata->writer);
    if (dispatch_sql_query_no_wait(clnt) == 0) {
        return;
    }
out:cdb2__query__free_unpacked(query, NULL);
    if (do_read) {
        evtimer_once(appsock_rd_base, newsql_read_again, appdata);
    } else {
        evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
    }
}

static void process_cdb2query(struct newsql_appdata_evbuffer *appdata, CDB2QUERY *query)
{
    rem_lru_evbuffer(&appdata->clnt);
    CDB2DBINFO *dbinfo = query->dbinfo;
    if (!dbinfo) {
        process_query(appdata, query);
        return;
    }
    if (dbinfo->has_want_effects && dbinfo->want_effects) {
        process_get_effects(appdata);
    } else {
        process_dbinfo(appdata);
    }
    cdb2__query__free_unpacked(query, NULL);
}

static void add_rd_event_ssl(struct newsql_appdata_evbuffer *appdata, struct event *ev, struct timeval *t)
{
    if (SSL_pending(appdata->ssl_data->ssl)) {
        evtimer_once(event_get_base(ev), event_get_callback(ev), event_get_callback_arg(ev));
    } else {
        event_add(ev, t);
    }
}

static void add_rd_event_plaintext(struct newsql_appdata_evbuffer *appdata, struct event *ev, struct timeval *t)
{
    event_add(ev, t);
}

static void add_rd_event(struct newsql_appdata_evbuffer *appdata, struct event *ev, struct timeval *timeout)
{
    appdata->add_rd_event_fn(appdata, ev, timeout); /* add_rd_event_plaintext */
}

static int verify_dbname(X509 *cert)
{
    return cert ? ssl_verify_dbname(cert, gbl_dbname, gbl_nid_dbname) : -1;
}

static int verify_hostname(struct newsql_appdata_evbuffer *appdata, X509 *cert)
{
    return cert ? ssl_verify_hostname(cert, appdata->fd) : -1;
}

static int verify_ssl(struct newsql_appdata_evbuffer *appdata)
{
    X509 *cert = appdata->ssl_data->cert = SSL_get_peer_certificate(appdata->ssl_data->ssl);
    switch (gbl_client_ssl_mode) {
    case SSL_VERIFY_DBNAME: if (verify_dbname(cert) != 0) return -1; // fallthrough
    case SSL_VERIFY_HOSTNAME: if (verify_hostname(appdata, cert) != 0) return -1; // fallthrough
    case SSL_VERIFY_CA: if (!cert) return -1; // fallthrough
    default: return 0;
    }
}

static int enable_ssl_evbuffer(struct newsql_appdata_evbuffer *appdata)
{
    int rc = verify_ssl(appdata);
    /* Enable SSL even if verification failed so we can return error */
    appdata->wr_dbinfo_fn = wr_dbinfo_ssl;
    appdata->rd_evbuffer_fn = rd_evbuffer_ssl;
    appdata->add_rd_event_fn = add_rd_event_ssl;
    sql_enable_ssl(appdata->writer, appdata->ssl_data->ssl);
    ssl_set_clnt_user(&appdata->clnt);
    return rc;
}

static void disable_ssl_evbuffer(struct newsql_appdata_evbuffer *appdata)
{
    appdata->wr_dbinfo_fn = wr_dbinfo_plaintext;
    appdata->rd_evbuffer_fn = rd_evbuffer_plaintext;
    appdata->add_rd_event_fn = add_rd_event_plaintext;
    sql_disable_ssl(appdata->writer);
    SSL *ssl = appdata->ssl_data ? appdata->ssl_data->ssl : NULL;
    if (!ssl) return;
    appdata->ssl_data->ssl = NULL;
    X509 *cert = appdata->ssl_data->cert;
    appdata->ssl_data->cert = NULL;
    SSL_shutdown(ssl);
    SSL_free(ssl);
    X509_free(cert);
}

static void ssl_accept_evbuffer(int fd, short what, void *arg)
{
    struct newsql_appdata_evbuffer *appdata = arg;
    SSL *ssl = appdata->ssl_data->ssl;
    int rc = SSL_do_handshake(ssl);
    if (rc == 1) {
        if (enable_ssl_evbuffer(appdata) == 0) {
            rd_hdr(-1, 0, appdata);
            return;
        }
        goto error;
    }
    int err = SSL_get_error(ssl, rc);
    switch (err) {
    case SSL_ERROR_WANT_READ:
        event_base_once(appsock_rd_base, fd, EV_READ, ssl_accept_evbuffer, appdata, NULL);
        return;
    case SSL_ERROR_WANT_WRITE:
        event_base_once(appsock_rd_base, fd, EV_READ, ssl_accept_evbuffer, appdata, NULL);
        return;
    case SSL_ERROR_SYSCALL:
        logmsg(LOGMSG_ERROR, "%s:%d SSL_do_handshake rc:%d err:%d errno:%d [%s]\n",
               __func__, __LINE__, rc, err, errno, strerror(errno));
        break;
    default:
        logmsg(LOGMSG_ERROR, "%s:%d SSL_do_handshake rc:%d err:%d [%s]\n",
               __func__, __LINE__, rc, err, ERR_error_string(err, NULL));
        break;
    }
error:
    write_response(&appdata->clnt, RESPONSE_ERROR, "Client certificate authentication failed", CDB2ERR_CONNECT_ERROR);
    evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
}

static int rd_evbuffer_ssl(struct newsql_appdata_evbuffer *appdata)
{
    SSL *ssl = appdata->ssl_data->ssl;
    int len = KB(16);
    struct iovec v = {0};
    if (evbuffer_reserve_space(appdata->rd_buf, len, &v, 1) == -1) {
        return -1;
    }
    int rc = SSL_read(ssl, v.iov_base, len);
    if (rc > 0) {
        v.iov_len = rc;
        evbuffer_commit_space(appdata->rd_buf, &v, 1);
        return rc;
    }
    int err = SSL_get_error(ssl, rc);
    switch (err) {
    case SSL_ERROR_ZERO_RETURN: disable_ssl_evbuffer(appdata); // fallthrough
    case SSL_ERROR_WANT_READ: return 1;
    case SSL_ERROR_SYSCALL:
        logmsg(LOGMSG_ERROR, "%s:%d SSL_read rc:%d err:%d errno:%d [%s]\n",
               __func__, __LINE__, rc, err, errno, strerror(errno));
        break;
    default:
        logmsg(LOGMSG_ERROR, "%s:%d SSL_read rc:%d err:%d [%s]\n",
               __func__, __LINE__, rc, err, ERR_error_string(err, NULL));
        break;
    }
    return rc;
}

static int rd_evbuffer_plaintext(struct newsql_appdata_evbuffer *appdata)
{
    return evbuffer_read(appdata->rd_buf, appdata->fd, -1);
}

static void process_ssl_request(struct newsql_appdata_evbuffer *appdata)
{
    if (appdata->ssl_data && appdata->ssl_data->ssl) {
        struct sqlclntstate *clnt = &appdata->clnt;
        logmsg(LOGMSG_ERROR,
               "%s fd:%d already have ssl clnt:%s pid:%d host:%d\n", __func__,
               appdata->fd, clnt->argv0, clnt->conninfo.pid,
               clnt->conninfo.node);
        goto cleanup;
    }
    int rc;
    char ssl_response = (gbl_client_ssl_mode >= SSL_ALLOW) ? 'Y' : 'N';
    if ((rc = write(appdata->fd, &ssl_response, 1)) != 1) {
        logmsg(LOGMSG_ERROR, "%s write fd:%d ssl_response:%c rc:%d err:%s\n",
               __func__, appdata->fd, ssl_response, rc, strerror(errno));
        goto cleanup;
    }
    if (ssl_response != 'Y') {
        goto cleanup;
    }
    if (!appdata->ssl_data) {
        appdata->ssl_data = calloc(1, sizeof(struct ssl_data));
    }
    SSL *ssl = appdata->ssl_data->ssl = SSL_new(gbl_ssl_ctx);
    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_set_fd(ssl, appdata->fd);
    SSL_set_accept_state(ssl);
    ssl_accept_evbuffer(appdata->fd, EV_READ, appdata);
    return;

cleanup:
    evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
}

static void process_newsql_payload(struct newsql_appdata_evbuffer *appdata, CDB2QUERY *query)
{
    switch (appdata->hdr.type) {
    case CDB2_REQUEST_TYPE__CDB2QUERY:
        process_cdb2query(appdata, query);
        break;
    case CDB2_REQUEST_TYPE__RESET:
        newsql_reset_evbuffer(appdata);
        rd_hdr(-1, 0, appdata);
        break;
    case CDB2_REQUEST_TYPE__SSLCONN:
        process_ssl_request(appdata);
        break;
    default:
        logmsg(LOGMSG_ERROR, "%s bad type:%d fd:%d\n", __func__, appdata->hdr.type, appdata->fd);
        evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
        break;
    }
}

static void rd_payload(int dummyfd, short what, void *arg)
{
    CDB2QUERY *query = NULL;
    struct newsql_appdata_evbuffer *appdata = arg;
    if (evbuffer_get_length(appdata->rd_buf) >= appdata->hdr.length) {
        goto payload;
    }
    if (rd_evbuffer(appdata) <= 0 && (what & EV_READ)) {
        evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
        return;
    }
    if (evbuffer_get_length(appdata->rd_buf) < appdata->hdr.length) {
        add_rd_event(appdata, appdata->rd_payload_ev, NULL);
        return;
    }
payload:
    if (appdata->hdr.length) {
        int len = appdata->hdr.length;
        void *data = evbuffer_pullup(appdata->rd_buf, len);
        if (data == NULL || (query = cdb2__query__unpack(NULL, len, data)) == NULL) {
            evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
            return;
        }
        evbuffer_drain(appdata->rd_buf, len);
    }
    process_newsql_payload(appdata, query);
}

static void rd_hdr(int dummyfd, short what, void *arg)
{
    struct newsqlheader hdr;
    struct newsql_appdata_evbuffer *appdata = arg;
    if (evbuffer_get_length(appdata->rd_buf) >= sizeof(struct newsqlheader)) {
        goto hdr;
    }
    if (rd_evbuffer(appdata) <= 0 && (what & EV_READ)) {
        evtimer_once(appsock_timer_base, newsql_cleanup, appdata);
        return;
    }
    if (evbuffer_get_length(appdata->rd_buf) < sizeof(struct newsqlheader)) {
        add_rd_event(appdata, appdata->rd_hdr_ev, NULL);
        return;
    }
hdr:
    evbuffer_remove(appdata->rd_buf, &hdr, sizeof(struct newsqlheader));
    appdata->hdr.type = ntohl(hdr.type);
    appdata->hdr.compression = ntohl(hdr.compression);
    appdata->hdr.state = ntohl(hdr.state);
    appdata->hdr.length = ntohl(hdr.length);
    rd_payload(-1, 0, appdata);
}

static void *newsql_destroy_stmt_evbuffer(struct sqlclntstate *clnt, void *arg)
{
    struct newsql_stmt *stmt = arg;
    cdb2__query__free_unpacked(stmt->query, NULL);
    free(stmt);
    return NULL;
}

static int newsql_close_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    return shutdown(appdata->fd, SHUT_RDWR);
}

struct debug_cmd {
    struct newsql_appdata_evbuffer *appdata;
    struct event *ev;
    int need;
};

static void debug_cmd_cb(int fd, short what, void *arg)
{
    struct debug_cmd *cmd = arg;
    struct newsql_appdata_evbuffer *appdata = cmd->appdata;
    int rc = rd_evbuffer(appdata);
    if (rc <= 0 || evbuffer_get_length(appdata->rd_buf) >= cmd->need) {
        event_base_loopbreak(event_get_base(cmd->ev));
    } else {
        add_rd_event(appdata, cmd->ev, NULL);
    }
}

/* read interactive cmds for debugging a stored procedure */
static int newsql_read_evbuffer(struct sqlclntstate *clnt, void *b, int l, int n)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    int need = l * n;
    int have = evbuffer_get_length(appdata->rd_buf);
    if (have >= need) goto done;
    struct event_base *wrbase = sql_wrbase(appdata->writer);
    struct debug_cmd cmd;
    cmd.appdata = appdata;
    cmd.need = need;
    cmd.ev = event_new(wrbase, appdata->fd, EV_READ, debug_cmd_cb, &cmd);
    add_rd_event(appdata, cmd.ev, NULL);
    event_base_dispatch(wrbase);
    event_free(cmd.ev);
    have = evbuffer_get_length(appdata->rd_buf);
    if (need > have) {
        need = 0;
    }
done:
    evbuffer_remove(appdata->rd_buf, b, need);
    return need / l;
}

static int newsql_pack_hb(uint8_t *out, void *arg)
{
    struct sqlclntstate *clnt = arg;
    int state;
    if (is_pingpong(clnt))
        state = 1;
    else {
        state = (clnt->sqltick > clnt->sqltick_last_seen);
        clnt->sqltick_last_seen = clnt->sqltick;
    }
    struct newsqlheader *h = (struct newsqlheader *)out;
    memset(h, 0, sizeof(struct newsqlheader));
    h->type = htonl(RESPONSE_HEADER__SQL_RESPONSE_HEARTBEAT);
    h->state = htonl(state);
    return 0;
}

struct newsql_pack_arg {
    struct newsqlheader *hdr;
    const CDB2SQLRESPONSE *resp;
};

static int newsql_pack(uint8_t *out, void *data)
{
    struct newsql_pack_arg *arg = data;
    if (arg->hdr) {
        memcpy(out, arg->hdr, sizeof(struct newsqlheader));
        out += sizeof(struct newsqlheader);
    }
    if (arg->resp) {
        cdb2__sqlresponse__pack(arg->resp, out);
        if (arg->resp->response_type == RESPONSE_TYPE__LAST_ROW) {
            return 1;
        }
    }
    return 0;
}

static int newsql_write_evbuffer(struct sqlclntstate *clnt, int type, int state,
                                 const CDB2SQLRESPONSE *resp, int flush)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    int hdr_len = type ? sizeof(struct newsqlheader) : 0;
    int response_len = resp ? cdb2__sqlresponse__get_packed_size(resp) : 0;
    int total_len = hdr_len + response_len;
    struct newsql_pack_arg arg = {0};
    struct newsqlheader hdr;
    if (type) {
        hdr.type = htonl(type);
        hdr.compression = 0;
        hdr.state = htonl(state);
        hdr.length = htonl(response_len);
        arg.hdr = &hdr;
    }
    arg.resp = resp;
    return sql_write(appdata->writer, total_len, &arg, flush);

}

static int newsql_write_hdr_evbuffer(struct sqlclntstate *clnt, int h, int state)
{
    return newsql_write_evbuffer(clnt, h, state, 0, 1);
}

static int newsql_write_postponed_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    struct iovec v[2];

    v[0].iov_base = (char *)&appdata->postponed->hdr;
    v[0].iov_len = sizeof(struct newsqlheader);

    v[1].iov_base = (char *)appdata->postponed->row;
    v[1].iov_len = appdata->postponed->len;

    return sql_writev(appdata->writer, v, 2);
}

static int newsql_write_dbinfo_evbuffer(struct sqlclntstate *clnt)
{
    struct newsql_appdata_evbuffer *appdata = clnt->appdata;
    struct evbuffer *buf = evbuffer_new();
    process_dbinfo_int(appdata, buf);
    int rc = sql_write_buffer(appdata->writer, buf);
    evbuffer_free(buf);
    return rc || sql_flush(appdata->writer);
}

static int allow_admin(int local)
{
    extern int gbl_forbid_remote_admin;
    return (local || !gbl_forbid_remote_admin);
}

static void newsql_setup_clnt_evbuffer(struct appsock_handler_arg *arg, int admin)
{
    check_appsock_rd_thd();

    int local = 0;
    if (arg->addr.sin_addr.s_addr == gbl_myaddr.s_addr) {
        local = 1;
    } else if (arg->addr.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
        local = 1;
    }

    if (thedb->no_more_sql_connections || (admin && !allow_admin(local))) {
        evbuffer_free(arg->rd_buf);
        shutdown(arg->fd, SHUT_RDWR);
        close(arg->fd);
        return;
    }

    struct newsql_appdata_evbuffer *appdata = calloc(1, sizeof(*appdata));
    struct sqlclntstate *clnt = &appdata->clnt;

    reset_clnt(clnt, 1);
    char *origin = get_hostname_by_fileno(arg->fd);
    clnt->origin = origin ? origin : intern("???");
    clnt->appdata = appdata;
    clnt->done_cb = newsql_done_cb;

    newsql_setup_clnt(clnt);
    plugin_set_callbacks_newsql(evbuffer);

    clnt->admin = admin;
    appdata->initial = 1;
    appdata->local = local;
    appdata->fd = arg->fd;
    appdata->rd_buf = arg->rd_buf;
    appdata->rd_hdr_ev = event_new(appsock_rd_base, arg->fd, EV_READ, rd_hdr, appdata);
    appdata->rd_payload_ev = event_new(appsock_rd_base, arg->fd, EV_READ, rd_payload, appdata);
    struct sqlwriter_arg sqlwriter_arg = {
        .fd = arg->fd,
        .clnt = &appdata->clnt,
        .pack = newsql_pack,
        .pack_hb = newsql_pack_hb,
        .hb_sz = sizeof(struct newsqlheader),
    };
    appdata->writer = sqlwriter_new(&sqlwriter_arg);
    disable_ssl_evbuffer(appdata);

    add_sql_evbuffer(&appdata->clnt);
    newsql_read_hdr(-1, 0, appdata);
}

static void handle_newsql_request_evbuffer(int dummyfd, short what, void *data)
{
    newsql_setup_clnt_evbuffer(data, 0);
    free(data);
}

static void handle_newsql_admin_request_evbuffer(int dummyfd, short what, void *data)
{
    newsql_setup_clnt_evbuffer(data, 1);
    free(data);
}

void setup_newsql_evbuffer_handlers(void)
{
    add_appsock_handler("newsql\n", handle_newsql_request_evbuffer);
    add_appsock_handler("@newsql\n", handle_newsql_admin_request_evbuffer);
}
