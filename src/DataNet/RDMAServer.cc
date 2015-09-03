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
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <malloc.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include "RDMAServer.h"
#include "../MOFServer/MOFServlet.h"
#include "../include/IOUtility.h"
#include <IOUtility.h>
#include <UdaUtil.h>

using namespace std;

extern supplier_state_t state_mac;
extern uint32_t wqes_perconn;


static void server_comp_ibv_recv(netlev_wqe_t *wqe)
{
	struct ibv_send_wr *bad_sr;
	struct ibv_recv_wr *bad_rr;
	int rc=0;

	netlev_msg_t *h = (netlev_msg_t*)wqe->data;
	netlev_conn_t *conn = wqe->conn;

	/* Credit flow */
	pthread_mutex_lock(&conn->lock);
	conn->credits += h->credits; /* Credits from peer */

	/* sanity check */
	if (conn->credits > wqes_perconn - 1) {
		log(lsERROR, "credit overflow. credits are %d", conn->credits);
		conn->credits = wqes_perconn - 1;
	}
	h->credits = 0;

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
		init_wqe_send(&send_wr, &sg, &h_back, len, 1, back->context, conn);

                conn->sent_counter++;  
		conn->credits--;

		log(lsTRACE, "removing message from backlog");
		if ((rc = ibv_post_send(conn->qp_hndl, &send_wr, &bad_sr)) != 0) {
			log(lsERROR, "Error posting send. rc=%d",rc);
		}

		log(lsTRACE, "After ibv_post_send. conn=%p CREDIT=%d RETURNING=%d", conn, conn->credits, conn->returning );

		free(back->msg);
		free(back);
	}
	pthread_mutex_unlock(&conn->lock);

	if (h->type == MSG_RTS) {
		shuffle_req_t *data_req;
		string param(h->msg, h->tot_len);
		/* XXX: create a free list of request to avoid malloc/free */
		data_req = get_shuffle_req(param);

		if (!data_req) {
			log(lsERROR, "Error in parsing request, request %s will not be processed", param.c_str());
		} else {
			log(lsTRACE, "request as received by server: jobid=%s, map_id=%s, reduceID=%d, map_offset=%lld, qpnum=%d, offset=%lld, path=%s",data_req->m_jobid.c_str(), data_req->m_map.c_str(), data_req->reduceID, (long long)data_req->map_offset, conn->qp_hndl->qp_num, (long long)data_req->record->offset, data_req->record->path.c_str());
			data_req->conn = conn;
			conn->received_counter ++;

			/* pass to parent and wake up other threads for processing */
			log(lsTRACE, "server received RDMA fetch request: jobid=%s, map_id=%s, reduceID=%d, map_offset=%d",data_req->m_jobid.c_str(), data_req->m_map.c_str(), data_req->reduceID, data_req->map_offset);
			state_mac.mover->insert_incoming_req(data_req);
		}
		/* data_req is freed by AIOHandler (in aio_completion_handler callback) or by DataEngine
		   (in start(), if error occurred before callback)*/
	} else {
		log(lsDEBUG, "received a noop" );
	}


	/* re-post wqe */
	/* Return the wqe of a noop message, recv */
	init_wqe_recv(wqe, NETLEV_FETCH_REQSIZE, conn->mem->mr->lkey, conn);
	log(lsTRACE, "calling ibv_post_recv");
	rc = ibv_post_recv(conn->qp_hndl, &wqe->desc.rr, &bad_rr);
	if (rc) {
		log(lsERROR, "ibv_post_recv failed: rc=%d", rc);
	}

	pthread_mutex_lock(&conn->lock);
	if (h->type != MSG_NOOP)
		conn->returning ++;
	pthread_mutex_unlock(&conn->lock);

	/* Send a no_op for credit flow */
	/* if (conn->returning >= (conn->peerinfo.credits >> 1)) {
		netlev_msg_t h;
		netlev_post_send(&h,  0, 0, NULL, conn, MSG_NOOP);
	} */
}

static void delete_connection(struct netlev_ctx *ctx, struct netlev_conn *conn)
{
	log(lsDEBUG, "freeing connection conn=%p",conn);
	pthread_mutex_lock(&ctx->lock);
	list_del(&conn->list);
	pthread_mutex_unlock(&ctx->lock);
	netlev_disconnect(conn);
}

static void server_cq_handler(progress_event_t *pevent, void *data)
{
	int ne = 0;
	int loop_count = 0;
	struct ibv_wc desc;
	netlev_wqe_t *wqe = NULL;
	struct netlev_dev *dev = (netlev_dev_t *)data;
	struct ibv_cq *cq;
	void *ctx;

	if (ibv_get_cq_event(dev->cq_channel, &cq, &ctx) != 0) {
		log(lsERROR, "notification, but no CQ event");
		goto error_event;
	}

	ibv_ack_cq_events(cq, 1);


	do {
		ne = ibv_poll_cq(cq, 1, &desc);

		if ( ne < 0) {
			log(lsERROR, "ibv_poll_cq failed ne=%d, (errno=%d %m)", ne, errno);
			return;
		}

		if (ne) {
			if (desc.status != IBV_WC_SUCCESS) {
				if (desc.status == IBV_WC_WR_FLUSH_ERR) {
					log(lsDEBUG, "Operation: %s (%d). Dev %p wr (0x%llx) flush err. quitting...",
							netlev_stropcode(desc.opcode), desc.opcode, dev, (uint64_t)desc.wr_id);
				} else {
					log(lsERROR, "Operation: %s (%d). Dev %p, Bad WC %s (%d) for wr_id 0x%llx",
							netlev_stropcode(desc.opcode), desc.opcode, dev, ibv_wc_status_str(desc.status) ,desc.status, (uint64_t)desc.wr_id);
					netlev_conn *conn = netlev_conn_find_by_qp((uint32_t) desc.qp_num, &dev->ctx->hdr_conn_list);
					if (conn) {
						conn->bad_conn = true;
						delete_connection(dev->ctx, conn);
					} else {
						log(lsWARN, "After WC ERROR, can't find connection to clean. qp_num = %d",desc.qp_num);
					}
				}
				//even if there was an error, must release the chunk
				chunk_t *chunk = (chunk_t*) (long2ptr(desc.wr_id)); //ptr_type_t is at offset 0 at chunk_t.
				if (chunk) {
					if (chunk->type == PTR_CHUNK) {
							state_mac.data_mac->release_chunk(chunk);
							log(lsDEBUG, "releasing chunk in case of an error");
					}
				}
				goto error_event;

			} else {

				switch (desc.opcode) {

				case IBV_WC_SEND:
					{
						chunk_t *chunk = (chunk_t*) (long2ptr(desc.wr_id)); //ptr_type_t is at offset 0 at chunk_t.
						if (chunk){
							log(lsTRACE, "got %s cq event: ACK_MSG_COMP chunk=%p", netlev_stropcode(desc.opcode), chunk);
							state_mac.data_mac->release_chunk(chunk);				
						}
						else {
							log(lsTRACE, "got %s cq event: NOOP_COMP", netlev_stropcode(desc.opcode));
						}
					}
					break;

				case IBV_WC_RECV:
					{
						wqe = (netlev_wqe_t *) (long2ptr(desc.wr_id));
						if (wqe) {
							log(lsTRACE, "got %s cq event. data=%s", netlev_stropcode(desc.opcode), wqe->data);
							server_comp_ibv_recv(wqe);
						}
						else {
							log(lsERROR, "got %s cq event with NULL wqe", netlev_stropcode(desc.opcode));
						}
					}
					break;
				case IBV_WC_RDMA_WRITE: // we send the RDAM_WRITE with flag=0 (not signaled)
				default:
					log(lsERROR, "got unhandled cq event: id %llx status %s (%d) opcode %s (%d)", desc.wr_id, ibv_wc_status_str(desc.status), desc.status, netlev_stropcode(desc.opcode), desc.opcode);
					break;
				}
			}
		}
		loop_count++;
		if (loop_count > 200) {
			log(lsDEBUG, "WARN: already handling %d cq events in a single loop", loop_count);
			break;
		}

	} while (ne);

error_event:
	if (ibv_req_notify_cq(cq, 0)) {
		log(lsERROR, "ibv_req_notify_cq failed");
	}
	return;
}

static void server_cm_handler(progress_event_t *pevent, void *data)
{
	int ret;

	struct netlev_ctx    *ctx;
	struct rdma_cm_event *cm_event;
	struct netlev_conn   *conn = 0;
	struct netlev_dev    *dev;
	RdmaServer           *server;

	server = (RdmaServer *)data;
	ctx    = &(server->ctx);

	struct rdma_event_channel *cm_channel = ctx->cm_channel;

	if (rdma_get_cm_event(cm_channel, &cm_event)) {
		output_stderr("[%s:%d] rdma_get_cm_event err", __FILE__, __LINE__);
		//TODO: consider throw exception
		return;
	}

	switch (cm_event->event) {
		case RDMA_CM_EVENT_CONNECT_REQUEST:
		{
			log(lsINFO, "got RDMA_CM_EVENT_CONNECT_REQUEST (on cma_id=%x)", cm_event->id);
			dev = netlev_dev_find(cm_event->id, &ctx->hdr_dev_list);

			if (!dev) {
				log(lsERROR, "device not found");
				//TODO: replace this exception with (verbs) rdma_reject();
				throw new UdaException("device not found");
			}
			else {
				log(lsDEBUG, "found dev=%x", dev);
			}

			conn = netlev_init_conn(cm_event, dev);
			log(lsTRACE, "conn=%p", conn);
			if (!cm_event->param.conn.private_data ||
					(cm_event->param.conn.private_data_len < sizeof(conn->peerinfo)))
			{
				log(lsERROR, "bad private data len %d", cm_event->param.conn.private_data_len);
			}
			else
			{
				memcpy(&conn->peerinfo, cm_event->param.conn.private_data,
						sizeof(conn->peerinfo));
			}
			conn->credits = conn->peerinfo.credits;
			log(lsTRACE,"Server conn->credits in the beginning is %d", conn->credits);
			conn->returning = 0;

			conn->state = NETLEV_CONN_RTR;
			pthread_mutex_lock(&ctx->lock);
			list_add_tail(&conn->list, &ctx->hdr_conn_list);
			pthread_mutex_unlock(&ctx->lock);
		}
		break;

		case RDMA_CM_EVENT_ESTABLISHED:
		{
			log(lsDEBUG,"got RDMA_CM_EVENT_ESTABLISHED (on cma_id=%x)", cm_event->id);
			conn = netlev_conn_established(cm_event, &ctx->hdr_conn_list);
			log(lsDEBUG,"netlev_conn_established returned conn=%p (QPN connection in server is %d)", conn, conn->qp_hndl->qp_num);
		}
		break;

		case RDMA_CM_EVENT_DISCONNECTED:
		{
			log(lsDEBUG, "got RDMA_CM_EVENT_DISCONNECTED (on cma_id=%x)", cm_event->id);
			conn = netlev_conn_find_by_qp(cm_event->id->qp->qp_num, &ctx->hdr_conn_list);
			log(lsTRACE, "calling rdma_ack_cm_event for event=%d", cm_event->event);
			ret = rdma_ack_cm_event(cm_event);
			if (ret) {
				log(lsWARN, "ack cm event failed");
			}
			conn->bad_conn = true;
			if (!conn->received_counter) {
				delete_connection(ctx, conn);
			}
		}
		// don't break here to avoid ack after disconnect
		return;

		case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		{
			log(lsWARN, "got RDMA_CM_EVENT_TIMEWAIT_EXIT (on cma_id=%x)", cm_event->id);
			// avner: don't bail out
			// TODO: consider cleanup
		}
		break;

		default:
		{
			log(lsERROR, "Unhandled RDMA_CM event %s (%d), status=%d (on cma_id=%x)", rdma_event_str(cm_event->event), cm_event->event, cm_event->status, cm_event->id);
	#if 0
			// Disregard the unknown event
			// not the best but definitely not good to bail out
			// Since we don't know the event we can't be sure if we can destory the cmd_id or not

			if (cm_event->id) {
				if (rdma_destroy_id(cm_event->id)) {
					log(lsERROR, "rdma_destroy_id failed");
				}
			}
			/* XXX: Trigger the exit of all threads */
			throw new UdaException("Server is bailing out, because of an RDMA unknown event");
	#endif
		}
		break;
	}

	log(lsTRACE, "calling rdma_ack_cm_event for event=%d", cm_event->event);
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		log(lsWARN, "ack cm event failed");
	}
}

RdmaServer::RdmaServer(int port, int rdma_buf_size, void *state)
{
	supplier_state_t *smac = (supplier_state_t *)state;

	memset(&this->ctx, 0, sizeof(netlev_ctx_t));
	pthread_mutex_init(&this->ctx.lock, NULL);
	INIT_LIST_HEAD(&this->ctx.hdr_event_list);
	INIT_LIST_HEAD(&this->ctx.hdr_dev_list);
	INIT_LIST_HEAD(&this->ctx.hdr_conn_list);

	this->data_port = port;
	this->parent   = smac->mover;
	this->data_mac = smac->data_mac;
	memset(&this->helper, 0, sizeof(this->helper));

	this->rdma_total_len = NETLEV_RDMA_MEM_CHUNKS_NUM * ((unsigned long)rdma_buf_size + 2*AIO_ALIGNMENT);
	this->rdma_chunk_len = rdma_buf_size + 2*AIO_ALIGNMENT;
	log(lsDEBUG, "rdma_buf_size inside RdmaServer is %d", rdma_buf_size);
	this->rdma_mem = NULL;
	log(lsDEBUG, "memalign successed - %llu bytes", this->rdma_total_len);
}

RdmaServer::~RdmaServer()
{
	log(lsTRACE,"QQ server dtor");
	pthread_mutex_destroy(&this->ctx.lock);
	this->parent  = NULL;
	this->data_mac = NULL;
	int contigPagesEnabler =  ::atoi(UdaBridge_invoke_getConfData_callback ("mapred.rdma.mem.use.contig.pages", "1").c_str());
	if (!contigPagesEnabler)
	{
		free(this->rdma_mem);
	}
}

void RdmaServer::start_server()
{
	errno = 0;
	netlev_thread_t *th;

	this->ctx.epoll_fd = epoll_create(4096);
	if (this->ctx.epoll_fd < 0) {
		log(lsERROR, "cannot create epoll fd, (errno=%d %m)",errno);
		throw new UdaException("cannot create epoll fd");
	}

	/* Start a new thread */
	th = &this->helper;
	th->stop = 0;
	th->pollfd = this->ctx.epoll_fd;

	this->create_listener();

	pthread_attr_init(&th->attr);
	pthread_attr_setdetachstate(&th->attr, PTHREAD_CREATE_JOINABLE);
	uda_thread_create(&th->thread, &th->attr, event_processor, th);

	// mapping and registering memory for all RDMA capable device
	map_ib_devices(&ctx, server_cq_handler, &rdma_mem, rdma_total_len);
}

void RdmaServer::stop_server()
{
	struct netlev_conn *conn;
	struct netlev_dev  *dev;
	void *pstatus;

	this->destroy_listener();

	pthread_mutex_lock(&this->ctx.lock);

	while (!list_empty(&this->ctx.hdr_conn_list)) {
		conn = list_entry(this->ctx.hdr_conn_list.next, typeof(*conn), list);
		log(lsDEBUG,"DD Server conn->credits is %d", conn->credits);
		list_del(&conn->list);
		netlev_conn_free(conn);
	}
	log(lsDEBUG, "all connections are released");

	/* kill event thread before we destroy the cq in netlev_dev_release() */
	this->helper.stop = 1;
	pthread_attr_destroy(&this->helper.attr);
	pthread_join(this->helper.thread, &pstatus); log(lsDEBUG, "THREAD JOINED");

	while (!list_empty(&this->ctx.hdr_dev_list)) {
		dev = list_entry(this->ctx.hdr_dev_list.next, typeof(*dev), list);
		list_del(&dev->list);
		netlev_event_del(this->ctx.epoll_fd, dev->cq_channel->fd,
				&this->ctx.hdr_event_list);
		netlev_dev_release(dev);
		free(dev);
	}
	log(lsDEBUG, "all devices are released");

	pthread_mutex_unlock(&this->ctx.lock);

	close(this->ctx.epoll_fd);
	log(lsDEBUG,"RDMA server stopped");
}

/* Create a RDMA listener for incoming connection requests */
int RdmaServer::create_listener()
{
	errno = 0;
	struct sockaddr_in sin;

	this->ctx.cm_channel = rdma_create_event_channel();
	if (!this->ctx.cm_channel) {
		log(lsERROR, "rdma_create_event_channel failed, (errno=%d %m)",errno);
		goto err_listener;
	}

	if (rdma_create_id(this->ctx.cm_channel,
			&this->ctx.cm_id,
			NULL, RDMA_PS_TCP)) {
		log(lsERROR, "rdma_create_id failed, (errno=%d %m)",errno);
		goto err_listener;
	}

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(this->data_port);
	sin.sin_addr.s_addr = INADDR_ANY; /* any device */

	if (rdma_bind_addr(this->ctx.cm_id, (struct sockaddr *) &sin)) {
		log(lsERROR, "rdma_bind_addr failed, (errno=%d %m)",errno);
		goto err_listener;
	}

	/* 0 == maximum backlog. XXX: not yet bind to any device */
	if (rdma_listen(this->ctx.cm_id, NETLEV_LISTENER_BACKLOG)) {
		log(lsERROR, "rdma_listen failed, (errno=%d %m)",errno);
		goto err_listener;
	}

	log(lsDEBUG, "Server listens on cm_channel=%p", this->ctx.cm_channel);

	/* XXX: add the cm_event channel to the epoll descriptor */
	pthread_mutex_lock(&this->ctx.lock);
	netlev_event_add(this->ctx.epoll_fd,
			this->ctx.cm_id->channel->fd, EPOLLIN,
			server_cm_handler, this,
			&this->ctx.hdr_event_list);
	pthread_mutex_unlock(&this->ctx.lock);
	return 0;

err_listener:
	throw new UdaException("error on create rdma listener");
	return -1;
}

int RdmaServer::destroy_listener()
{
	log(lsDEBUG, "Closing server fd=%d cm_channel=%p",
			this->ctx.cm_id->channel->fd,
			this->ctx.cm_channel);

	/* Remove the read descriptor from the polling set */
	pthread_mutex_lock(&this->ctx.lock);
	netlev_event_del(this->ctx.epoll_fd, this->ctx.cm_id->channel->fd,
			&this->ctx.hdr_event_list);

	/* XXX: closing such things from the thread */
	if (rdma_destroy_id(this->ctx.cm_id)){
		log(lsERROR, "rdma_destroy_id failed");
	}
	rdma_destroy_event_channel(this->ctx.cm_channel);
	pthread_mutex_unlock(&this->ctx.lock);
	return 0;
}

int RdmaServer::rdma_write_mof_send_ack(struct shuffle_req *req, uintptr_t laddr,
		uint64_t req_size, void* chunk,struct index_record *record)
{
	netlev_dev_t       *dev;
	int32_t             rdma_send_size, total_ack_len, lkey;
	size_t              ack_msg_len;
	int rc;
	struct ibv_send_wr  send_wr_rdma, send_wr_ack;
	struct ibv_sge      sge_rdma, sge_ack;
	struct ibv_send_wr *bad_wr;
	netlev_msg_t        h;

	netlev_conn_t *conn = req->conn;
	dev = conn->dev;

	lkey = dev->rdma_mem->mr->lkey;
	rdma_send_size = this->rdma_chunk_len > req_size ? req_size : this->rdma_chunk_len;
	ack_msg_len = snprintf(h.msg, sizeof(h.msg), "%lld:%lld:%d:%lld:%s:",
			(long long)record->rawLength,
			(long long)record->partLength,
			rdma_send_size,
			(long long)record->offset,
			record->path.c_str());
	if (ack_msg_len >= sizeof(h.msg)) {
	    	log(lsERROR, "trying to send a message too big. msg_len=%d, max=%d",ack_msg_len, sizeof(h.msg));
	    	throw new UdaException("trying to send a message too big");
	}
	conn->received_counter--;

	if (!conn->bad_conn){
		//locking to prevent destruction of the connection before ibv_post_send
		pthread_mutex_lock(&conn->lock);
		if (conn->credits>0){
			log(lsTRACE, "before sending it is now %d, conn is %d in the send, h.msg is %s, rdma_send_size is %d", conn->received_counter, conn->bad_conn,h.msg,rdma_send_size);
			init_wqe_rdmaw(&send_wr_rdma, &sge_rdma,
					(int)rdma_send_size,
					(void *)laddr,
					lkey,
					(void *)req->remote_addr,
					(uint32_t)conn->peerinfo.rdma_mem_rkey, &send_wr_ack);

			total_ack_len = sizeof(netlev_msg_t)-(NETLEV_FETCH_REQSIZE-ack_msg_len);

			h.type = MSG_RTS;
			h.tot_len = ack_msg_len;
			h.src_req = req->freq ? req->freq : 0;
 			if (conn->returning > 0) {
                                h.credits = 1;
                                conn->returning --;
                        } else {
                                h.credits = 0;
                                conn->returning = 0;
                        }
			init_wqe_send(&send_wr_ack, &sge_ack, &h, total_ack_len, 1, chunk, conn); //signal each time, to release the chunk

			if ((rc = ibv_post_send(conn->qp_hndl, &send_wr_rdma, &bad_wr)) != 0) {
				log(lsERROR, "ibv_post_send of rdma_write&send ack failed. error: errno=%d", rc);
				pthread_mutex_unlock(&conn->lock);
				return -1;
			}
			conn->credits--;
			conn->sent_counter += 2;

			log(lsTRACE, "After ibv_post_send. JOBID=%s, REDUCEID=%d, MAPID=%s, MAP_OFFSET=%lld, CONN(conn=%p CREDIT=%d RETURNING=%d) ", req->m_jobid.c_str(), req->reduceID , req->m_map.c_str(),  req->map_offset, conn, conn->credits, conn->returning );
			pthread_mutex_unlock(&conn->lock);
			return 0;
		} else {
			//send RDMA (do not take up recv wqe at client's end) and save ack in backlog
			log(lsTRACE, "there are no credits for ack. send only the rdma");
			init_wqe_rdmaw(&send_wr_rdma, &sge_rdma,
					(int)rdma_send_size,
					(void *)laddr,
					lkey,
					(void *)req->remote_addr,
					(uint32_t)conn->peerinfo.rdma_mem_rkey,NULL);

			//save as backlog
			netlev_msg_backlog_t *back = init_backlog_data(MSG_RTS, ack_msg_len, req->freq, chunk, h.msg);
			list_add_tail(&back->list, &conn->backlog);

			conn->sent_counter ++;

			if ((rc = ibv_post_send(conn->qp_hndl, &send_wr_rdma, &bad_wr)) != 0) {
				log(lsERROR, "ServerConn: RDMA Post Failed, with rc=%d", rc);
				pthread_mutex_unlock(&conn->lock);
				return -1;
			}
			pthread_mutex_unlock(&conn->lock);
			return -2;
		}

	} else {//connection does not exist anymore
		log(lsERROR, "connection does not exist anymore. releasing chunk");
		chunk_t *chunk_to_release = (chunk_t*) chunk;
		state_mac.data_mac->release_chunk(chunk_to_release);
		if (!conn->received_counter){
			log(lsINFO, "connection does not exist anymore, all related chunks are released. freeing connection");
			delete_connection(&this->ctx, conn);
		}
		return -1;
	}

}
