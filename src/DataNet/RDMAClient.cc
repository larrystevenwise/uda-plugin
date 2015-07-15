
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

#include <stdio.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <malloc.h>
#include <netdb.h>
#include <errno.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "RDMAClient.h"
#include "../Merger/InputClient.h"
#include <IOUtility.h>
#include <UdaUtil.h>
using namespace std;

extern int netlev_dbg_flag;
extern merging_state_t merging_sm; 
extern uint32_t wqes_perconn;

#define RECONNECT_TRIES 5

static void client_comp_ibv_recv(netlev_wqe_t *wqe)
{
	struct ibv_send_wr *bad_sr;
	struct ibv_recv_wr *bad_rr;

	netlev_msg_t  *h = (netlev_msg_t *)wqe->data;
	netlev_conn_t *conn = wqe->conn;

	/* credit flow */
	pthread_mutex_lock(&conn->lock);
	conn->credits += h->credits; /* credits from peer */

	/* sanity check */
	if (conn->credits > wqes_perconn - 1) {
		log(lsERROR, "credit overflow. credits are %d", conn->credits);
		conn->credits = wqes_perconn - 1;
	}

	h->credits = 0;

	/* empty backlog */
	while (conn->credits > 0 && !list_empty(&conn->backlog)) {
		netlev_msg_backlog_t *back = list_entry(conn->backlog.next, typeof(*back), list);
		list_del(&back->list);
		netlev_msg_t h_back;
		h_back.credits = conn->returning;
		conn->returning = 0;

		h_back.type = back->type;
		h_back.tot_len = back->len;
		h_back.src_req = back->src_req ? back->src_req : 0;
		memcpy(h_back.msg, back->msg, back->len);
		int len = sizeof(netlev_msg_t)-(NETLEV_FETCH_REQSIZE-back->len);
		struct ibv_send_wr send_wr;
		ibv_sge sg ;
		bool send_signal = !(conn->sent_counter%SIGNAL_INTERVAL);
		conn->sent_counter++;
		init_wqe_send(&send_wr, &sg, &h_back, len, send_signal, back->context, conn);

		conn->credits--;

		log(lsTRACE, "removing a wqe from backlog");
		if (ibv_post_send(conn->qp_hndl, &send_wr, &bad_sr)) {
			output_stderr("[%s,%d] Error posting send\n",
					__FILE__,__LINE__);
		}
		free(back->msg);
		free(back);
	}
	pthread_mutex_unlock(&conn->lock);

	if ( h->type == MSG_RTS ) {
		client_part_req_t *req = (client_part_req_t*) (long2ptr(h->src_req));
		memcpy(req->recvd_msg, h->msg, h->tot_len);

		log(lsTRACE, "Client received RDMA completion for fetch request: jobid=%s, mapid=%s, reducer_id=%s, total_fetched_compressed=%lld, total_read_uncompress=%lld (not updated for this comp)",
				req->info->params[1], req->info->params[2], req->info->params[3], req->mop->fetched_len_rdma, req->mop->fetched_len_uncompress);
		req->mop->task->client->comp_fetch_req(req);
	}
	else {
		log(lsDEBUG, "received a noop");
	}


	/* put the receive wqe back */
	init_wqe_recv(wqe, NETLEV_FETCH_REQSIZE, conn->mem->mr->lkey, conn);
	// TODO: this log might be too verbose
	log(lsTRACE, "ibv_post_recv");
	if (ibv_post_recv(conn->qp_hndl, &wqe->desc.rr, &bad_rr)) {
		output_stderr("[%s,%d] ibv_post_recv failed\n",
				__FILE__,__LINE__);
	}
	pthread_mutex_lock(&conn->lock);
	if (h->type != MSG_NOOP)
		conn->returning++;
	pthread_mutex_unlock(&conn->lock);

	/* Send a no_op for credit flow */
	if (conn->returning >= (conn->peerinfo.credits >> 1)) {
		netlev_msg_t h;
		log(lsDEBUG, "sending a noop");
		netlev_post_send(&h,  0, 0, NULL, conn, MSG_NOOP);
	}
}

static void client_cq_handler(progress_event_t *pevent, void *data)
{
	int rc=0;
	int ne = 0;
	int loop_count = 0;
	struct ibv_wc desc;
	void *ctx;
	netlev_dev_t *dev = (netlev_dev_t *) pevent->data;

	if ((rc = ibv_get_cq_event(dev->cq_channel, &dev->cq, &ctx)) != 0) {
		output_stderr("[%s,%d] notification, but no CQ event or bad rc=%d \n", __FILE__,__LINE__, rc);
		goto error_event;
	}

	ibv_ack_cq_events(dev->cq, 1);

	do {
		ne = ibv_poll_cq(dev->cq, 1, &desc);

		if ( ne < 0) {
			log(lsERROR, "ibv_poll_cq failed ne=%d, (errno=%d %m)", ne, errno);
			return;
		}

		if (ne) {
			if (desc.status != IBV_WC_SUCCESS) {
				if (desc.status == IBV_WC_WR_FLUSH_ERR) {
					log(lsERROR,"Operation: %s (%d). Dev %p wr (0x%llx) flush err. quitting...",
							netlev_stropcode(desc.opcode), desc.opcode, dev, (uint64_t)desc.wr_id);
					goto error_event;
				} else {
					log(lsERROR,"Operation: %s (%d). Dev %p, Bad WC %s (%d) for wr_id 0x%llx",
							netlev_stropcode(desc.opcode), desc.opcode, dev, ibv_wc_status_str(desc.status) ,desc.status, (uint64_t)desc.wr_id);
					goto error_event;
				}
			} else {

				/* output_stdout("Detect cq event wqe=%p, opcode=%d",
                              wqe, desc.opcode); */

				switch (desc.opcode) {

				case IBV_WC_SEND:
					{
						client_part_req_t* freq = (client_part_req_t *) (long2ptr(desc.wr_id));
						if (freq)
							log(lsTRACE, "got %s cq event: FETCH_REQ_COMP JOBID=%s MAPID=%s REDUCEID=%s ", netlev_stropcode(desc.opcode), freq->info->params[1], freq->info->params[2], freq->info->params[3]);
						else
							log(lsTRACE, "got %s cq event: NOOP_COMP", netlev_stropcode(desc.opcode));
					}
					break;

				case IBV_WC_RECV:
					{
						netlev_wqe_t *wqe = (netlev_wqe_t *) (long2ptr(desc.wr_id));
						if (wqe) {
							log(lsTRACE, "got %s cq event.", netlev_stropcode(desc.opcode));
							client_comp_ibv_recv(wqe);
						}
						else {
							log(lsERROR, "got %s cq event with NULL wqe", netlev_stropcode(desc.opcode));
							throw new UdaException("got IBV_WC_RECV cq event with NULL wqe (wr_id=NULL)");
						}
					}
					break;

				default:
					log(lsERROR, "got unhandled cq event: id %llx status %s (%d) opcode %s (%d)", desc.wr_id, ibv_wc_status_str(desc.status), desc.status, netlev_stropcode(desc.opcode), desc.opcode);
					break;
				}
			}
		}
		loop_count++;
		if (loop_count > 1000) {
			log(lsDEBUG, "WARN: already handling %d cq events in a single loop", loop_count);
			loop_count = 0;
		}

	} while (ne);

error_event:
	if (ibv_req_notify_cq(dev->cq, 0) != 0) {
		log(lsERROR,"ibv_req_notify_cq failed");
	}

	return;
}

netlev_conn_t* RdmaClient::netlev_get_conn(unsigned long ipaddr, int port,
		netlev_ctx_t *ctx,
		list_head_t *registered_mem)
{
	netlev_conn_t         *conn;
	struct rdma_cm_event  *cm_event;
	struct rdma_cm_id     *cm_id;
	struct netlev_dev     *dev;
	struct sockaddr_in     sin;
	struct rdma_conn_param conn_param;
	struct connreq_data    xdata;

	errno = 0;
	int retryCount=0;
	bool connected = false;

	sin.sin_addr.s_addr = ipaddr;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);

	do{
		if (rdma_create_id(ctx->cm_channel, &cm_id, NULL, RDMA_PS_TCP) != 0) {
			log(lsERROR, "rdma_create_id failed, (errno=%d %m)",errno);
			throw new UdaException("rdma_create_id failed");
			return NULL;
		}
		if (rdma_resolve_addr(cm_id, NULL, (struct sockaddr*)&sin, NETLEV_TIMEOUT_MS)) {
			log(lsERROR, "rdma_resolve_addr failed, (errno=%d %m)",errno);
			throw new UdaException("rdma_resolve_addr failed");
			return NULL;
		}
		if (rdma_get_cm_event(ctx->cm_channel, &cm_event)) {
			log(lsERROR, "rdma_get_cm_event failed, (errno=%d %m)",errno);
			throw new UdaException("rdma_get_cm_event failed");
			return NULL;
		}
		if (cm_event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
			rdma_ack_cm_event(cm_event);
			log(lsERROR, "Unexpected RDMA_CM event %s (%d), status=%d (on cma_id=%x)", rdma_event_str(cm_event->event), cm_event->event, cm_event->status, cm_event->id);
			throw new UdaException("unexpected CM event");
			return NULL;
		}
		rdma_ack_cm_event(cm_event);
		if (rdma_resolve_route(cm_id, NETLEV_TIMEOUT_MS)) {
			log(lsERROR, "rdma_resolve_route failed, (errno=%d %m)",errno);
			throw new UdaException("rdma_resolve_route failed");
			return NULL;
		}
		if (rdma_get_cm_event(ctx->cm_channel, &cm_event)) {
			log(lsERROR, "rdma_get_cm_event failed, (errno=%d %m)",errno);
			throw new UdaException("rdma_get_cm_event failed");
			return NULL;
		}
		if (cm_event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
			rdma_ack_cm_event(cm_event);
			log(lsWARN, "Unexpected RDMA_CM event %s (%d), status=%d (on cma_id=%x)", rdma_event_str(cm_event->event), cm_event->event, cm_event->status, cm_event->id);
			//TODO: consider throw new UdaException("unexpected CM event");
			return NULL;
		}
		rdma_ack_cm_event(cm_event);
		dev = netlev_dev_find(cm_id, &ctx->hdr_dev_list);
		if (!dev) {
			log(lsERROR, "device not found");
			throw new UdaException("device not found");
		}
		else {
			log(lsDEBUG, "found dev=%x", dev);
		}

		conn = netlev_conn_alloc(dev, cm_id);
		if (!conn) {
			goto err_conn_alloc;
		}

		/* Save an extra one for credit flow */
		memset(&xdata, 0, sizeof(xdata));
		xdata.qp = cm_id->qp->qp_num;
		xdata.credits = wqes_perconn - 1;
		xdata.rdma_mem_rkey = dev->rdma_mem->mr->rkey;

		memset(&conn_param, 0, sizeof (conn_param));
		conn_param.responder_resources = 1;
		conn_param.initiator_depth = 1;
		conn_param.retry_count = RDMA_DEFAULT_RNR_RETRY;
		conn_param.rnr_retry_count = RDMA_DEFAULT_RNR_RETRY;
		conn_param.private_data = &xdata;
		conn_param.private_data_len = sizeof(xdata);

		if (rdma_connect(cm_id, &conn_param)) {
			log(lsERROR, "rdma_connect failed, (errno=%d %m)",errno);
			goto err_rdma_connect;
		}

		if (rdma_get_cm_event(ctx->cm_channel, &cm_event)) {
			log(lsERROR, "rdma_get_cm_event err, (errno=%d %m)",errno);
			goto err_rdma_connect;
		}

		if (cm_event->event != RDMA_CM_EVENT_ESTABLISHED) {
			log(lsERROR, "Unexpected RDMA_CM event %s (%d), status=%d (on cma_id=%x)", rdma_event_str(cm_event->event), cm_event->event, cm_event->status, cm_event->id);
			rdma_ack_cm_event(cm_event);
			//goto err_rdma_connect;
			netlev_conn_free(conn);
			log(lsINFO, "Failed to connect to server. Try #%d",retryCount);
			retryCount++;

		}else{
			log(lsINFO, "Successfully got RDMA_CM_EVENT_ESTABLISHED with peer %x:%d (on cma_id=%x)", (int)ipaddr, port, cm_event->id);
			conn->peerIPAddr = ipaddr;
			list_add_tail(&conn->list, &ctx->hdr_conn_list);

			if (!cm_event->param.conn.private_data ||
					(cm_event->param.conn.private_data_len < sizeof(conn->peerinfo))) {
				output_stderr("%s: bad private data len %d",
						__func__, cm_event->param.conn.private_data_len);
			}
			memcpy(&conn->peerinfo, cm_event->param.conn.private_data, sizeof(conn->peerinfo));
			conn->credits = conn->peerinfo.credits;
			log(lsDEBUG,"Client conn->credits in the beginning is %d", conn->credits);
			conn->returning = 0;
			rdma_ack_cm_event(cm_event);
			connected = true;
		}

	}while(retryCount<=RECONNECT_TRIES && !connected);

	if(retryCount>RECONNECT_TRIES){
		log(lsERROR, "Failed to connect to server %x. Tried for %d times",(int)ipaddr, RECONNECT_TRIES);
		goto err_conn_alloc;
	}

	return conn;

err_rdma_connect:
	netlev_conn_free(conn);
err_conn_alloc:
	//    rdma_destroy_id(cm_id); //unnecessary, since destroyed in netlev_conn_alloc and netlev_conn_free
	rdma_destroy_event_channel(ctx->cm_channel);
	log(lsERROR, "[%s,%d] connection failed", __FILE__,__LINE__);
	throw new UdaException("connection failed");
	return NULL;
}

RdmaClient::RdmaClient(int port, reduce_task_t* reduce_task) : parent(NULL)
{
	netlev_thread_t *th;

	memset(&this->ctx, 0, sizeof(netlev_ctx_t));
	pthread_mutex_init(&this->ctx.lock, NULL);
	INIT_LIST_HEAD(&this->ctx.hdr_event_list);
	INIT_LIST_HEAD(&this->ctx.hdr_dev_list);
	INIT_LIST_HEAD(&this->ctx.hdr_conn_list);
	INIT_LIST_HEAD(&this->register_mems_head);
	errno = 0;

	this->reduce_task = reduce_task;

	this->svc_port = port;
	this->ctx.cm_channel = rdma_create_event_channel();

	if (!this->ctx.cm_channel)  {
		log(lsERROR, "rdma_create_event_channel failed, (errno=%d %m)",errno);
		throw new UdaException("rdma_create_event_channel failed");
	}

	this->ctx.epoll_fd = epoll_create(4096);

	if (this->ctx.epoll_fd < 0) {
		log(lsERROR, "cannot create epoll fd, (errno=%d %m)",errno);
		throw new UdaException("cannot create epoll fd");
	}

	/* Start a new thread */
	memset(&this->helper, 0, sizeof(this->helper));
	th = &this->helper;
	th->stop = 0;
	th->pollfd = this->ctx.epoll_fd;
	pthread_attr_init(&th->attr);
	pthread_attr_setdetachstate(&th->attr, PTHREAD_CREATE_JOINABLE);
	uda_thread_create(&th->thread, &th->attr, event_processor, th);

	/* FIXME:
	 * When we consider disconnection we need to add
	 * the cm_event channel to the epoll descriptor.
	 */
}

RdmaClient::~RdmaClient()
{
	struct netlev_conn *conn;
	struct netlev_dev *dev;

	/* relase all connection */
	while(!list_empty(&this->ctx.hdr_conn_list)) {
		conn = list_entry(this->ctx.hdr_conn_list.next, typeof(*conn), list);
		log(lsDEBUG,"Client conn->credits is %d", conn->credits);
		list_del(&conn->list);
		netlev_conn_free(conn);
	}
	//DBGPRINT(DBG_CLIENT, "all connections are released\n");

	/* release all device */
	while(!list_empty(&this->ctx.hdr_dev_list)) {
		dev = list_entry(this->ctx.hdr_dev_list.next, typeof(*dev), list);
		list_del(&dev->list);
		netlev_event_del(this->ctx.epoll_fd, dev->cq_channel->fd,
				&this->ctx.hdr_event_list);
		netlev_dev_release(dev);
		free(dev);
	}
	//DBGPRINT(DBG_CLIENT, "all devices are released\n");

	this->helper.stop = 1;
	pthread_attr_destroy(&this->helper.attr);
	pthread_join(this->helper.thread, NULL); log(lsDEBUG, "THREAD JOINED");
	//DBGPRINT(DBG_CLIENT, "RDMAClient is shut down \n");

	rdma_destroy_event_channel(this->ctx.cm_channel);
	close(this->ctx.epoll_fd);
	pthread_mutex_destroy(&this->ctx.lock);
}

void RdmaClient::register_mem(struct memory_pool *mem_pool, double_buffer_t buffers)
{
	map_ib_devices(&ctx, client_cq_handler, (void**)&mem_pool->mem, mem_pool->total_size);

	int rc = split_mem_pool_to_pairs(mem_pool, buffers);
	if (rc) {
		log(lsERROR, "UDA critical error: failed on split_mem_pool_to_pairs , rc=%d ==> exit process", rc);
		throw new UdaException("failure in split_mem_pool_to_pairs");
	}

	/* PLEASE DON'T CHANGE THE FOLLOWING LINE - THE AUTOMATION PARSE IT */
	log(lsINFO, " After RDMA buffers registration: buffer1 = %d bytes , buffer2 = %d bytes , buffers count = %d , total = %lld bytes)", buffers.buffer1, buffers.buffer2, mem_pool->num, mem_pool->total_size);
}

void init_mem_desc(mem_desc_t *desc, char *addr, int32_t buf_len){
	desc->buff  = addr;
	desc->buf_len = buf_len;
	desc->status = INIT;
	desc->start = 0;
	desc->end = 0;
	pthread_mutex_init(&desc->lock, NULL);
	pthread_cond_init(&desc->cond, NULL);
}

int RdmaClient::split_mem_pool_to_pairs(memory_pool_t *pool, double_buffer_t buffers)
{
    pthread_mutex_init(&pool->lock, NULL);
    INIT_LIST_HEAD(&pool->free_descs);

    int num = pool->num;
    int size1 = buffers.buffer1;
    int size2 = buffers.buffer2;

    log (lsDEBUG, "buffer length1  is %d, buffer length2  is %d pool->total_size is %d\n", size1, size2, pool->total_size);

    mem_desc_t *desc_arr =  new mem_desc_t[num*2];
    mem_set_desc_t* pair_desc_arr = new mem_set_desc_t[num];

    log(lsTRACE, "sizeof(mem_desc_t)=%lld", sizeof(mem_desc_t));

    pthread_mutex_lock(&pool->lock);
    pool->desc_arr = desc_arr;
    pool->pair_desc_arr = pair_desc_arr;

    for (int i = 0; i < num; i++) {
    	//init mem_desc of the pair
		mem_desc_t *desc1 = &(desc_arr[2*i]);
		mem_desc_t *desc2 = &(desc_arr[2*i+1]);
		init_mem_desc(desc1, pool->mem + i * (size1 + size2), size1);
		init_mem_desc(desc2, pool->mem + i * (size1 + size2) + size1, size2);

		pair_desc_arr[i].buffer_unit[0] = desc1;
		pair_desc_arr[i].buffer_unit[1] = desc2;

        list_add_tail(&(pair_desc_arr[i].list), &pool->free_descs);
    }
    pthread_mutex_unlock(&pool->lock);
	log (lsTRACE, "After memory pool creation: %d X (buff1_size=%d buff2_size=%d)", num, size1, size2);
    return 0;
}

netlev_conn_t* RdmaClient::connect(const char *host, int port)
{
	netlev_conn_t *conn;
	unsigned long ipaddr;

	pthread_mutex_lock(&this->ctx.lock);
	ipaddr = get_hostip(host);
	if (!ipaddr) {
		output_stderr("get hostip error");
		pthread_mutex_unlock(&this->ctx.lock);
		return NULL;
	}
	conn = netlev_conn_find_by_ip(ipaddr, &this->ctx.hdr_conn_list);
	if (conn) {
		pthread_mutex_unlock(&this->ctx.lock);
		return conn;
	}

	output_stdout("RDMA Client: connecting to %s:%d" , host, port);

	conn = netlev_get_conn(ipaddr, port, &this->ctx, &this->register_mems_head);

	if (!conn) {
		log(lsERROR, "connection to %s failed", host);
	}

	pthread_mutex_unlock(&this->ctx.lock);

	return conn;
}

void RdmaClient::comp_fetch_req(client_part_req_t *req)
{
	if (parent==this){//there is no decompression thread ->must notify MergeManager directly
		if (req->mop){
			MergeManager *merge_man = req->mop->task->merge_man;
			merge_man->update_fetch_req(req);
			merge_man->mark_req_as_ready(req);
		} else {
			log(lsFATAL, "req->mop is null!"); //TODO might be related to key/value size bigger than rdma buffer size. see bug 89763
			exit (-1);
		}
	} else {
		parent->comp_fetch_req(req);
	}
}

RdmaClient* RdmaClient::getRdmaClient()
{
	return this;
}

void RdmaClient::start_client()
{
	this->parent = this->reduce_task->client; //problem here!!!!!
}

void RdmaClient::stop_client()
{
}

int RdmaClient::start_fetch_req(client_part_req_t *freq, char *buff, int32_t buf_len)
{
	size_t          msg_len;
	uint64_t        addr;
	netlev_conn_t  *conn;

	if (buf_len <= 0) {
		log(lsERROR, "illegal fetch request size of %d bytes", buf_len); //DO NOT CHANGE THIS LINE. THE REGRESSION IS PARSING IT
		throw new UdaException("illegal fetch request size of 0 or less bytes");
	}

	addr = (uint64_t)((uintptr_t)(buff));

	netlev_msg_t h;

	/* jobid:mapid:mop_offset:reduceid:mem_addr:req_prt:chunk_size:offset_in_file:mof_path */
	msg_len = snprintf(h.msg, sizeof(h.msg), "%s:%s:%lld:%s:%lu:%lu:%d:%lld:%s:%lld:%lld",
			freq->info->params[1],
			freq->info->params[2],
			(long long)freq->mop->fetched_len_rdma,
			freq->info->params[3],
			addr,
			(uint64_t) freq,
			buf_len,
			(long long)freq->mop->mofOffset,
			freq->mop->mofPath.c_str(),
			(long long)freq->mop->total_len_uncompress,
			(long long)freq->mop->total_len_rdma);

	if (msg_len >= sizeof(h.msg)) {
	    	log(lsERROR, "trying to fetch a message too big. msg_len=%d, max=%d",msg_len, sizeof(h.msg));
	    	throw new UdaException("trying to fetch a message too big");
	}

	conn = connect(freq->info->params[0], svc_port);
	if (!conn) {
		log(lsERROR, "could not connect to host %s on port %d", freq->info->params[0], svc_port);
		throw new UdaException("trying to fetch a message too big");
	}
	log(lsTRACE, "calling to netlev_post_send: mapid=%s, reduceid=%s, mapp_offset=%lld, qp=%d, hostname=%s, buf_len=%d, msg len=%d, offset=%lld", freq->info->params[2], freq->info->params[3], freq->mop->fetched_len_rdma, conn->qp_hndl->qp_num,freq->info->params[0],buf_len, msg_len, freq->mop->mofOffset);
	return netlev_post_send(&h,  msg_len, 0, freq, conn, MSG_RTS);
}

unsigned long RdmaClient::get_hostip(const char *host)
{
	string id(host);
	map<string, unsigned long>::iterator iter;
	iter = this->local_dns.find(id);
	if (iter != this->local_dns.end()) {
		return iter->second;
	}

	struct addrinfo *res;
	struct addrinfo  hints;
	unsigned long    ip;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((getaddrinfo(host, NULL, &hints, &res)) < 0 ) {
		output_stderr("%s: getaddr for %s",
				__func__, host);
		ip = 0;
	} else {
		ip = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	}
	freeaddrinfo(res);
	local_dns[id] = ip;
	return ip;
}

#if LCOV_AUBURN_DEAD_CODE
void
RdmaClient::disconnect(struct netlev_conn *conn)
{
	rdma_disconnect(conn->cm_id);
	netlev_conn_free(conn);
}
#endif
