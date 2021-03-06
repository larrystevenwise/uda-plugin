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

#include <dirent.h>
#include <memory>
#include "IOUtility.h"
#include "MOFServlet.h"

using namespace std;

/* Parse param into a shuffle_req_t */
shuffle_req_t* get_shuffle_req(const string &param)
{
    size_t start, end;
    shuffle_req_t *sreq = new shuffle_req_t();
    sreq->record = new index_record();
    auto_ptr<shuffle_req_t> my_auto_ptr ( sreq );

    end = param.find(':');
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->m_jobid.assign(param, 0, end);

    //COVERITY: RM#189303 NEGATIVE_RETURNS. false alarm
    start = ++end;
    end = param.find(':', start);
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->m_map.assign(param, start, end - start);

    //COVERITY: RM#189303 NEGATIVE_RETURNS. false alarm
    start = ++end;
    end = param.find(':', start);
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->map_offset = atoi(param.substr(start, end - start).c_str());

    //COVERITY: RM#189303 NEGATIVE_RETURNS. false alarm
    start = ++end;
    end = param.find(':', start);
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->reduceID = atoi(param.substr(start, end - start).c_str());

    //COVERITY: RM#189303 NEGATIVE_RETURNS. false alarm
    start = ++end;
    end = param.find(':', start);
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->remote_addr = atoll(param.substr(start, end - start).c_str());

    //COVERITY: RM#189303 NEGATIVE_RETURNS. false alarm
    start = ++end;
    end = param.find(':', start);
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->freq = atoll(param.substr(start, end - start).c_str());

    //COVERITY: RM#189303 NEGATIVE_RETURNS. false alarm
    start = ++end;
    end = param.find(':', start);
    if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
    sreq->chunk_size = atoi(param.substr(start, end - start).c_str());

    start = ++end;
	end = param.find(':', start);
	if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
	sreq->record->offset = atoll(param.substr(start, end - start).c_str());

	start = ++end;
	end = param.find(':', start);
	if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
	sreq->record->path.assign(param, start, end - start);

	start = ++end;
	end = param.find(':', start);
	if(end == param.npos) return NULL; /* if no ':' is found in shuffle request,  return NULL to calling request. */
	sreq->record->rawLength = atoll(param.substr(start, end - start).c_str());

	start = ++end;
	int param_length = param.length(); /* if no ':' is found in shuffle request,  return NULL to calling request. */
	sreq->record->partLength = atoll(param.substr(start, param_length - start).c_str());

    my_auto_ptr.release();
    return sreq;
}


OutputServer::OutputServer(int data_port, int mode, int rdma_buf_size,
                           supplier_state_t *state)
{
	this->data_port = data_port;
    this->rdma = NULL; 
    this->rdma_buf_size = rdma_buf_size;
    //this->tcp  = NULL;    AUBURN_DEAD_CODE
    this->state = state;
    INIT_LIST_HEAD(&this->incoming_req_list);

    pthread_mutex_init(&this->in_lock, NULL);
    pthread_mutex_init(&this->out_lock, NULL);
    pthread_cond_init(&this->in_cond, NULL);
    /* if (mode == STANDALONE) {
        list_fet_req(path);
    } */
}

#if _BullseyeCoverage
	#pragma BullseyeCoverage off
#endif
OutputServer::~OutputServer()
{
	output_stdout("OutputServer: D'tor");
    pthread_mutex_destroy(&this->in_lock);
    pthread_mutex_destroy(&this->out_lock);
    pthread_cond_destroy(&this->in_cond);
}
#if _BullseyeCoverage
	#pragma BullseyeCoverage on
#endif



void OutputServer::start_server()
{
    this->rdma = new RdmaServer(this->data_port, this->rdma_buf_size, this->state);
    this->rdma->start_server();
}

#if _BullseyeCoverage
	#pragma BullseyeCoverage off
#endif
void OutputServer::stop_server()
{
    this->rdma->stop_server();
    delete this->rdma;
}
#if _BullseyeCoverage
	#pragma BullseyeCoverage on
#endif


void OutputServer::insert_incoming_req(shuffle_req_t *req)
{
    pthread_mutex_lock(&in_lock);

    /* testing section */
    /* if (req->map_offset == 0) {
        int reduceid = req->reduceID;
        map<int, int>::iterator iter =
            recv_stat.find(reduceid);
        if (iter != recv_stat.end()) {
            recv_stat[reduceid] = (*iter).second + 1;
        } else {
            recv_stat[reduceid] = 1;
        }
        output_stdout("reducer : %d, First mop recv: %d",
                      reduceid, recv_stat[reduceid]);
    } */

    list_add_tail(&req->list, &incoming_req_list);

    pthread_cond_broadcast(&in_cond);
    pthread_mutex_unlock(&in_lock);
}

void OutputServer::start_outgoing_req(shuffle_req_t *req, index_record_t* record,  chunk_t *chunk, uint64_t length, int offsetAligment)
{
	uintptr_t local_addr = (uintptr_t)(chunk->buff + offsetAligment);
	this->rdma->rdma_write_mof_send_ack(req, local_addr, length,(void*)chunk, record);
}



/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sw=4 hlsearch cindent expandtab 
 */
