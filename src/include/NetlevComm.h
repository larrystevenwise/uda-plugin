/*
** Copyright (C) 2012 Auburn University
** Copyright (C) 2012 Mellanox Technologies
** 
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at:
**  
** http://www.apache.org/licenses/LICENSE-2.0
** 
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, 
** either express or implied. See the License for the specific language 
** governing permissions and  limitations under the License.
**
**
*/

#ifndef NETLEV_COMM_H
#define NETLEV_COMM_H

#include "LinkList.h"
#include "UdaBridge.h"  //avnerb - TEMP - will be removed
#include <limits.h> // for PATH_MAX
#include <sys/time.h>

#define NETLEV 1

#define NETLEV_TIMEOUT_MS        (5000)
#define NETLEV_FETCH_REQSIZE     (800)
#define NETLEV_MOF_PATH_MAX_SIZE     (600)

#define NETLEV_KV_POOL_EXPO         (20)

#define NETLEV_RDMA_MEM_CHUNKS_NUM	(1000)

#define ARRAY_SIZE(array) (sizeof array / sizeof array[0])
#define RDMA_TIMEOUT 2

#define long2ptr(p) (void *)(unsigned long)(p)
#define ptr2long(p) (u_int64_t)(unsigned long)(p)

enum {
    DBG_CLIENT = 0x1,
    DBG_SERVER = 0x2,
    DBG_CONN   = 0x4,
};

extern int netlev_dbg_flag;
extern unsigned int wqes_perconn;


struct progress_event;
typedef void (*event_handler_t) (struct progress_event *pevent, void *ctx);

typedef struct progress_event {
    int                 fd;
    struct list_head    list;
    event_handler_t     handler;
    void               *data;
    char                handler_name[30];
} progress_event_t;

typedef struct netlev_thread {
    pthread_t        thread;
    pthread_attr_t   attr;
    volatile int     stop;
    int	             pollfd;
    void            *context;
} netlev_thread_t;

void dprint(char *s, char *fmt, ...);

#define  DBGPRINT(flag, args...)     \
    do {                                 \
       	if (netlev_dbg_flag & (flag)) {  \
            char s0[32];                 \
            sprintf(s0, "[%d:%d:%s] ", getpid(), __LINE__, __FILE__); \
            dprint(s0, args);            \
       	}                                \
    } while (0)


int netlev_event_add(int poll_fd, int fd, int events, 
                     event_handler_t handler, void *data, 
	                 struct list_head *head, const char *name);

void netlev_event_del(int poll_fd, int fd, struct list_head *head);

void *event_processor(void *context);

#define timeit(s, func) ({ \
	struct timeval start, stop, delta; \
	void * ret; \
	gettimeofday(&start, NULL);  \
	ret = (void *)(func); \
	gettimeofday(&stop, NULL); \
	timersub(&stop, &start, &delta); \
	if (delta.tv_sec > 30) { \
		log(lsERROR, "Extra slow function %s - %lu seconds!", (s), delta.tv_sec); \
	} \
	ret; \
})

#define timeit_void(s, func) ({ \
	struct timeval start, stop, delta; \
	gettimeofday(&start, NULL);  \
	(func); \
	gettimeofday(&stop, NULL); \
	timersub(&stop, &start, &delta); \
	if (delta.tv_sec > 30) { \
		log(lsERROR, "Extra slow function %s - %lu seconds!", (s), delta.tv_sec); \
	} \
})
	

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sw=4 hlsearch cindent expandtab 
 */
