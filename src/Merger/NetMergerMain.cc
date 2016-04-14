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
** This file has been modified by Chelsio Communications to enable iWARP.
** For any queries regarding the changes made, please contact
** support@chelsio.com<mailto:support@chelsio.com>.
**
*/

#include <errno.h>
#include <stdlib.h>
#include <set>
#include <string>
#include <sys/socket.h>

#include "C2JNexus.h"
#include "DataNet/RDMAClient.h"
#include "Merger/reducer.h"
#include "include/IOUtility.h"
#include "CompareFunc.h"
using namespace std;


int netlev_dbg_flag = 0;
uint32_t wqes_perconn = 400;


/* merger state machine */
merging_state_t merging_sm;

hadoop_cmp_func g_cmp_func;

//COVERITY: UNCAUGHT_EXCEPT, RM#189300. false alarm
int MergeManager_main(int argc, char* argv[])  throw (UdaException*)
{
	int  ret;
    struct netlev_option op;
    ret = parse_options(argc, argv, &op);
    if (ret) {
	log(lsERROR, " MergeManager_main: parse option error");
	return -EIO;
    } 

    startLogNetMerger();

	log(lsDEBUG, "======== pid=%d ========", getpid() );

    /* PLEASE DON'T CHANGE THE FOLLOWING LINE - THE AUTOMATION PARSE IT */
    log (lsINFO, "UDA version is %s",STR(VERSION_UDA));
    log (lsINFO, "Compiled on the %s, %s\n", __DATE__, __TIME__);

    log (lsINFO, "size of rdma buffer as passed from java is %d\n", op.buf_size);

    /* initalize merging_sm */
    memset(&merging_sm, 0, sizeof(merging_state_t));
//    merging_sm.stop = 0;
    merging_sm.online = op.online;
    merging_sm.data_port = op.data_port;

//    pthread_mutex_init(&merging_sm.lock, NULL);
//    pthread_cond_init(&merging_sm.cond, NULL);

    /* Create a Fetcher
     * -- an event-driven thread responsible for
     * -- create a network connections with the server
     * -- round-robin to process segment requests from all reducers
     */
	spawn_reduce_task();

    return 0;
}



/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=4 sw=4 hlsearch cindent expandtab 
 */
