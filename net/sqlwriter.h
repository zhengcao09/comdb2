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

#ifndef INCLUDED_SQLWRITER_H
#define INCLUDED_SQLWRITER_H

#include <stdint.h>
#include <pthread.h>

struct evbuffer;
struct event_base;
struct iovec;
struct sqlclntstate;
struct sqlwriter;

int  sql_flush(struct sqlwriter *);
//void sql_enable_flush(struct sqlwriter *);
//void sql_disable_flush(struct sqlwriter *);

void sql_enable_heartbeat(struct sqlwriter *);
void sql_disable_heartbeat(struct sqlwriter *);

void sql_enable_timeout(struct sqlwriter *, int);
void sql_disable_timeout(struct sqlwriter *);

//void sql_enable_trickle(struct sqlwriter *);
//void sql_disable_trickle(struct sqlwriter *);

int sql_write(struct sqlwriter *, int, void *, int);
int sql_writev(struct sqlwriter *, struct iovec *, int);
int sql_write_buffer(struct sqlwriter *, struct evbuffer *);

typedef int(sql_pack_fn)(uint8_t *out, void *pack_arg);
struct sqlwriter_arg {
    int fd;
    struct sqlclntstate *clnt;
    sql_pack_fn *pack;
    sql_pack_fn *pack_hb;
    size_t hb_sz;
};
struct sqlwriter *sqlwriter_new(struct sqlwriter_arg *);
void sqlwriter_free(struct sqlwriter *);
void sql_reset(struct sqlwriter *);

int sql_done(struct sqlwriter *);
int sql_peer_check(struct sqlwriter *);

struct event_base *sql_wrbase(struct sqlwriter *);
struct evbuffer *sql_wrbuf(struct sqlwriter *);

int recover_deadlock_evbuffer(struct sqlclntstate *);

void sql_enable_ssl(struct sqlwriter *, SSL *);
void sql_disable_ssl(struct sqlwriter *);

#endif /* INCLUDED_SQLWRITER_H */
