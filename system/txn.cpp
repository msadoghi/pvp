#include "txn.h"
#include "wl.h"
#include "query.h"
#include "thread.h"
#include "mem_alloc.h"
#include "msg_queue.h"
#include "pool.h"
#include "message.h"
#include "ycsb_query.h"
#include "array.h"
#include "timer.h"
#include "work_queue.h"

void TxnStats::init()
{
    starttime = 0;
    wait_starttime = get_sys_clock();
    total_process_time = 0;
    process_time = 0;
    total_local_wait_time = 0;
    local_wait_time = 0;
    total_remote_wait_time = 0;
    remote_wait_time = 0;
    write_cnt = 0;
    abort_cnt = 0;

    total_work_queue_time = 0;
    work_queue_time = 0;
    total_work_queue_cnt = 0;
    work_queue_cnt = 0;
    total_msg_queue_time = 0;
    msg_queue_time = 0;
    total_abort_time = 0;
    time_start_pre_prepare = 0;
    time_start_prepare = 0;
    time_start_commit = 0;
    time_start_execute = 0;

    clear_short();
}

void TxnStats::clear_short()
{
    work_queue_time_short = 0;
    cc_block_time_short = 0;
    cc_time_short = 0;
    msg_queue_time_short = 0;
    process_time_short = 0;
    network_time_short = 0;
}

void TxnStats::reset()
{
    wait_starttime = get_sys_clock();
    total_process_time += process_time;
    process_time = 0;
    total_local_wait_time += local_wait_time;
    local_wait_time = 0;
    total_remote_wait_time += remote_wait_time;
    remote_wait_time = 0;
    write_cnt = 0;

    total_work_queue_time += work_queue_time;
    work_queue_time = 0;
    total_work_queue_cnt += work_queue_cnt;
    work_queue_cnt = 0;
    total_msg_queue_time += msg_queue_time;
    msg_queue_time = 0;

    clear_short();
}

void TxnStats::abort_stats(uint64_t thd_id)
{
    total_process_time += process_time;
    total_local_wait_time += local_wait_time;
    total_remote_wait_time += remote_wait_time;
    total_work_queue_time += work_queue_time;
    total_msg_queue_time += msg_queue_time;
    total_work_queue_cnt += work_queue_cnt;
    assert(total_process_time >= process_time);
}

void TxnStats::commit_stats(uint64_t thd_id, uint64_t txn_id, uint64_t batch_id, uint64_t timespan_long,
                            uint64_t timespan_short)
{
    total_process_time += process_time;
    total_local_wait_time += local_wait_time;
    total_remote_wait_time += remote_wait_time;
    total_work_queue_time += work_queue_time;
    total_msg_queue_time += msg_queue_time;
    total_work_queue_cnt += work_queue_cnt;
    assert(total_process_time >= process_time);

    if (IS_LOCAL(txn_id))
    {
        PRINT_LATENCY("lat_s %ld %ld %f %f %f %f\n", txn_id, work_queue_cnt, (double)timespan_short / BILLION, (double)work_queue_time / BILLION, (double)msg_queue_time / BILLION, (double)process_time / BILLION);
    }
    else
    {
        PRINT_LATENCY("lat_rs %ld %ld %f %f %f %f\n", txn_id, work_queue_cnt, (double)timespan_short / BILLION, (double)total_work_queue_time / BILLION, (double)total_msg_queue_time / BILLION, (double)total_process_time / BILLION);
    }

    if (!IS_LOCAL(txn_id))
    {
        return;
    }
}

void Transaction::init()
{
    txn_id = UINT64_MAX;
    batch_id = UINT64_MAX;

    reset(0);
}

void Transaction::reset(uint64_t pool_id)
{
    rc = RCOK;
}

void Transaction::release(uint64_t pool_id)
{
    DEBUG("Transaction release\n");
}

void TxnManager::init(uint64_t pool_id, Workload *h_wl)
{
    if (!txn)
    {
        DEBUG_M("Transaction alloc\n");
        txn_pool.get(pool_id, txn);
    }
#if !BANKING_SMART_CONTRACT
    if (!query)
    {
        DEBUG_M("TxnManager::init Query alloc\n");
        qry_pool.get(pool_id, query);
        // this->query->init();
    }
#endif
    sem_init(&rsp_mutex, 0, 1);
    return_id = UINT64_MAX;

    this->h_wl = h_wl;

    txn_ready = true;

    prepared = false;

    committed_local = false;
    prep_rsp_cnt = 2 * g_min_invalid_nodes;
    commit_rsp_cnt = prep_rsp_cnt + 1;
    chkpt_cnt = 1;
    chkpt_flag = false;

#if CONSENSUS == HOTSTUFF
    prepare_vote_cnt = 2 * g_min_invalid_nodes;
    precommit_vote_cnt = 2 * g_min_invalid_nodes;
    commit_vote_cnt = 2 * g_min_invalid_nodes;
    new_view_vote_cnt = 2 * g_min_invalid_nodes + 1;
    generic_received = false;
#endif

#if RING_BFT
    this->is_cross_shard = false;
#endif
#if SHARPER
    this->is_cross_shard = false;
    for (uint64_t i = 0; i < g_shard_cnt; i++)
    {
        prep_rsp_cnt_arr[i] = 2 * g_min_invalid_nodes;
        commit_rsp_cnt_arr[i] = 2 * g_min_invalid_nodes;
    }
#endif

    batchreq = NULL;

    txn_stats.init();
}

// reset after abort
void TxnManager::reset()
{
    rsp_cnt = 0;
    aborted = false;
    return_id = UINT64_MAX;
    //twopl_wait_start = 0;

    assert(txn);
#if BANKING_SMART_CONTRACT
    //assert(smart_contract);
#else
    assert(query);
#endif
    txn->reset(get_thd_id());

    // Stats
    txn_stats.reset();
}

void TxnManager::release(uint64_t pool_id)
{

#if RING_BFT
    if (!this->hash.empty() && this->is_cross_shard)
    {
        // cout << ccm_directory.size() << "   " << rcm_directory.size() << endl;
        // digest_directory.remove(hash);
        CommitCertificateMessage *temp1 = ccm_directory.pop(hash);
        if (temp1)
            Message::release_message(temp1);
    }
#endif

    uint64_t tid = get_txn_id();

#if BANKING_SMART_CONTRACT
    delete this->smart_contract;
#else
    qry_pool.put(pool_id, query);
    query = NULL;
#endif
    txn_pool.put(pool_id, txn);
    txn = NULL;

    txn_ready = true;

    hash.clear();
    prepared = false;

    prep_rsp_cnt = 2 * g_min_invalid_nodes;
    commit_rsp_cnt = prep_rsp_cnt + 1;
    chkpt_cnt = 1;
    chkpt_flag = false;

#if CONSENSUS == HOTSTUFF
    precommited = false;
    new_viewed = false;
    prepare_vote_cnt = 2 * g_min_invalid_nodes;
    precommit_vote_cnt = 2 * g_min_invalid_nodes;
    commit_vote_cnt = 2 * g_min_invalid_nodes;
    new_view_vote_cnt = 2 * g_min_invalid_nodes + 1;
    generic_received = false;
#endif

    release_all_messages(tid);

#if RING_BFT
    this->is_cross_shard = false;
    this->client_id = -1;
#endif
#if SHARPER
    this->is_cross_shard = false;
    for (uint64_t i = 0; i < g_shard_cnt; i++)
    {
        prep_rsp_cnt_arr[i] = 2 * g_min_invalid_nodes;
        commit_rsp_cnt_arr[i] = prep_rsp_cnt_arr[i];
    }
#endif

}

void TxnManager::reset_query()
{
#if !BANKING_SMART_CONTRACT
    ((YCSBQuery *)query)->reset();
#endif
}

RC TxnManager::commit()
{
    DEBUG("Commit %ld\n", get_txn_id());

    commit_stats();
    return Commit;
}

RC TxnManager::start_commit()
{
    RC rc = RCOK;
    DEBUG("%ld start_commit RO?\n", get_txn_id());
    return rc;
}

int TxnManager::received_response(RC rc)
{
    assert(txn->rc == RCOK);
    if (txn->rc == RCOK)
        txn->rc = rc;

    --rsp_cnt;

    return rsp_cnt;
}

bool TxnManager::waiting_for_response()
{
    return rsp_cnt > 0;
}

void TxnManager::commit_stats()
{
    uint64_t commit_time = get_sys_clock();
    uint64_t timespan_short = commit_time - txn_stats.restart_starttime;
    uint64_t timespan_long = commit_time - txn_stats.starttime;
    INC_STATS(get_thd_id(), total_txn_commit_cnt, 1);

    if (!IS_LOCAL(get_txn_id()))
    {
        txn_stats.commit_stats(get_thd_id(), get_txn_id(), get_batch_id(), timespan_long, timespan_short);
        return;
    }
    
    INC_STATS(get_thd_id(), txn_run_time, timespan_long);
    INC_STATS(get_thd_id(), single_part_txn_cnt, 1);
    txn_stats.commit_stats(get_thd_id(), get_txn_id(), get_batch_id(), timespan_long, timespan_short);
}

void TxnManager::register_thread(Thread *h_thd)
{
    this->h_thd = h_thd;
}

void TxnManager::set_txn_id(txnid_t txn_id)
{
    txn->txn_id = txn_id;
}

txnid_t TxnManager::get_txn_id()
{
    return txn->txn_id;
}

Workload *TxnManager::get_wl()
{
    return h_wl;
}

uint64_t TxnManager::get_thd_id()
{
    if (h_thd)
        return h_thd->get_thd_id();
    else
        return 0;
}

BaseQuery *TxnManager::get_query()
{
#if !BANKING_SMART_CONTRACT
    return query;
#else
    return NULL;
#endif
}

void TxnManager::set_query(BaseQuery *qry)
{
#if !BANKING_SMART_CONTRACT
    query = qry;
#endif
}

uint64_t TxnManager::incr_rsp(int i)
{
    uint64_t result;
    sem_wait(&rsp_mutex);
    result = ++this->rsp_cnt;
    sem_post(&rsp_mutex);
    return result;
}

uint64_t TxnManager::decr_rsp(int i)
{
    uint64_t result;
    sem_wait(&rsp_mutex);
    result = --this->rsp_cnt;
    sem_post(&rsp_mutex);
    return result;
}

RC TxnManager::validate()
{
    return RCOK;
}

/* Generic Helper functions. */

string TxnManager::get_hash()
{
    return hash;
}

void TxnManager::set_hash(string hsh)
{
    hash = hsh;
    hashSize = hash.length();
}

uint64_t TxnManager::get_hashSize()
{
    return hashSize;
}

void TxnManager::set_primarybatch(BatchRequests *breq)
{
    char *buf = create_msg_buffer(breq);
    Message *deepMsg = deep_copy_msg(buf, breq);
    batchreq = (BatchRequests *)deepMsg;
    delete_msg_buffer(buf);
}

bool TxnManager::is_chkpt_ready()
{
    return chkpt_flag;
}

void TxnManager::set_chkpt_ready()
{
    chkpt_flag = true;
}

uint64_t TxnManager::decr_chkpt_cnt()
{
    chkpt_cnt--;
    return chkpt_cnt;
}

uint64_t TxnManager::get_chkpt_cnt()
{
    return chkpt_cnt;
}

/* Helper functions for PBFT. */
void TxnManager::set_prepared()
{
    //printf("set_prepared %ld at %lf\n", this->txn->txn_id, simulation->seconds_from_start(get_sys_clock()));
    prepared = true;
}

bool TxnManager::is_prepared()
{
    return prepared;
}

uint64_t TxnManager::decr_prep_rsp_cnt()
{
    prep_rsp_cnt--;
    return prep_rsp_cnt;
}

uint64_t TxnManager::get_prep_rsp_cnt()
{
    return prep_rsp_cnt;
}

/************************************/

/* Helper functions for PBFT. */

void TxnManager::set_committed()
{
    //printf("set_committed %ld at %lf\n", this->txn->txn_id, simulation->seconds_from_start(get_sys_clock()));
    committed_local = true;
#if TEMP_QUEUE
    work_queue.reenqueue(instance_id);
#endif
}

bool TxnManager::is_committed()
{
    return committed_local;
}

void TxnManager::add_commit_msg(PBFTCommitMessage *pcmsg)
{
    char *buf = create_msg_buffer(pcmsg);
    Message *deepMsg = deep_copy_msg(buf, pcmsg);
    commit_msgs.push_back((PBFTCommitMessage *)deepMsg);
    delete_msg_buffer(buf);
}

uint64_t TxnManager::decr_commit_rsp_cnt()
{
    commit_rsp_cnt--;
    return commit_rsp_cnt;
}

uint64_t TxnManager::get_commit_rsp_cnt()
{
    return commit_rsp_cnt;
}

/*****************************/
/* Helper functions for SHARPER. */
#if SHARPER
uint64_t TxnManager::decr_prep_rsp_cnt(int shard)
{
    if (prep_rsp_cnt_arr[shard])
        prep_rsp_cnt_arr[shard]--;
    return prep_rsp_cnt_arr[shard];
}

uint64_t TxnManager::get_prep_rsp_cnt(int shard)
{
    return prep_rsp_cnt_arr[shard];
}

uint64_t TxnManager::decr_commit_rsp_cnt(int shard)
{
    if (commit_rsp_cnt_arr[shard])
        commit_rsp_cnt_arr[shard]--;
    return commit_rsp_cnt_arr[shard];
}

uint64_t TxnManager::get_commit_rsp_cnt(int shard)
{
    return commit_rsp_cnt_arr[shard];
}
#endif

/*****************************/

//broadcasts prepare message to all nodes
void TxnManager::send_pbft_prep_msgs()
{
    //printf("%ld Send PBFT_PREP_MSG message to %d nodes\n", get_txn_id(), g_node_cnt - 1);
    //fflush(stdout);

    Message *msg = Message::create_message(this, PBFT_PREP_MSG);
    PBFTPrepMessage *pmsg = (PBFTPrepMessage *)msg;

#if LOCAL_FAULT == true || VIEW_CHANGES
    if (get_prep_rsp_cnt() > 0)
    {
        decr_prep_rsp_cnt();
    }
#endif

    vector<uint64_t> dest;
    for (uint64_t i = 0; i < g_node_cnt; i++)
    {
#if SHARPER
        if (this->is_cross_shard)
        {
            pmsg->is_cross_shard = true;
            if (!this->involved_shards[get_shard_number(i)])
            {
                continue;
            }
        }
        else if (!is_in_same_shard(i, g_node_id))
        {
            continue;
        }
#elif RING_BFT
        if (!is_in_same_shard(i, g_node_id))
        {
            continue;
        }
#endif
        if (i == g_node_id)
        {
            continue;
        }
        dest.push_back(i);
    }

    msg_queue.enqueue(get_thd_id(), pmsg, dest);
    dest.clear();
}

//broadcasts commit message to all nodes
void TxnManager::send_pbft_commit_msgs()
{
    //cout << "Send PBFT_COMMIT_MSG messages " << get_txn_id() << "\n";
    //fflush(stdout);

    Message *msg = Message::create_message(this, PBFT_COMMIT_MSG);
    PBFTCommitMessage *cmsg = (PBFTCommitMessage *)msg;

#if LOCAL_FAULT == true || VIEW_CHANGES
    if (get_commit_rsp_cnt() > 0)
    {
        decr_commit_rsp_cnt();
    }
#endif

#if RING_BFT
    if (this->is_cross_shard)
        cmsg->is_cross_shard = true;
    else
        cmsg->is_cross_shard = false;
#endif

    vector<uint64_t> dest;
    for (uint64_t i = 0; i < g_node_cnt; i++)
    {
#if SHARPER
        if (this->is_cross_shard)
        {
            cmsg->is_cross_shard = true;
            if (!this->involved_shards[get_shard_number(i)])
            {
                continue;
            }
        }
        else if (!is_in_same_shard(i, g_node_id))
        {
            continue;
        }
#elif RING_BFT
        if (!is_in_same_shard(i, g_node_id))
        {
            continue;
        }
#endif
        if (i == g_node_id)
        {
            continue;
        }
        dest.push_back(i);
    }

    msg_queue.enqueue(get_thd_id(), cmsg, dest);
    dest.clear();
}

#if !TESTING_ON

void TxnManager::release_all_messages(uint64_t txn_id)
{
    if ((txn_id + 1) % get_batch_size() == 0)
    {
        info_prepare.clear();
        info_commit.clear();
#if RING_BFT
        if (!this->is_cross_shard)
            Message::release_message(batchreq);

#elif CONSENSUS == HOTSTUFF
        vote_prepare.clear();
        vote_precommit.clear();
        vote_commit.clear();
        vote_new_view.clear(); 
    #if THRESHOLD_SIGNATURE
        preparedQC.signature_share_map.clear();
        precommittedQC.signature_share_map.clear();
        committedQC.signature_share_map.clear();
        #if CHAINED
        genericQC.release();
        highQC.release();
        #endif
    #endif
        // Message::release_message(prepmsg);
        if(prepmsg){
            Message::release_message(prepmsg, 5);
        }
        if(propmsg){
            Message::release_message(propmsg, 6);
        }
#else
        // Message::release_message(batchreq);
        if(batchreq){
             Message::release_message(batchreq);
         }
#endif

        PBFTCommitMessage *cmsg;
        while (commit_msgs.size() > 0)
        {
            cmsg = (PBFTCommitMessage *)this->commit_msgs[0];
            commit_msgs.erase(commit_msgs.begin());
            Message::release_message(cmsg);
        }
    }
}

#endif // !TESTING

//broadcasts checkpoint message to all nodes
void TxnManager::send_checkpoint_msgs()
{
    DEBUG("%ld Send PBFT_CHKPT_MSG message to %d\n nodes", get_txn_id(), g_node_cnt - 1);
    Message *msg = Message::create_message(this, PBFT_CHKPT_MSG);
    CheckpointMessage *ckmsg = (CheckpointMessage *)msg;
    // vector<uint64_t> dest;
    // for (uint64_t i = 0; i < g_node_cnt; i++)
    // {
    //     if (i == g_node_id)
    //     {
    //         continue;
    //     }
    //     dest.push_back(i);
    // }

    // msg_queue.enqueue(get_thd_id(), ckmsg, dest);
    // dest.clear();
    work_queue.enqueue(get_thd_id(), ckmsg, false);
}


#if CONSENSUS == HOTSTUFF
bool TxnManager::is_precommitted(){
    return precommited;
}

void TxnManager::set_precommitted(){
    precommited = true;
}

bool TxnManager::is_new_viewed(){
    return new_viewed;
}

void TxnManager::set_new_viewed(){
    new_viewed = true;
}

void TxnManager::set_primarybatch(HOTSTUFFPrepareMsg *prep){
    char *buf = create_msg_buffer(prep);
    Message *deepMsg = deep_copy_msg(buf, prep); 
    prepmsg = (HOTSTUFFPrepareMsg*)deepMsg;
    delete_msg_buffer(buf);
}

#if SEPARATE
void TxnManager::set_primarybatch(HOTSTUFFProposalMsg *prop){
    char *buf = create_msg_buffer(prop);
    Message *deepMsg = deep_copy_msg(buf, prop); 
    propmsg = (HOTSTUFFProposalMsg*)deepMsg;
    delete_msg_buffer(buf);
}
#endif

void TxnManager::setPreparedQC(HOTSTUFFPreCommitMsg *pcmsg){
    this->preparedQC = pcmsg->PreparedQC;
    string batch_hash = this->get_hash();
#if !PVP
    hash_QC_lock.lock();
    if(!hash_to_QC.count(batch_hash)){
        hash_to_QC.insert(make_pair<string&,QuorumCertificate&>(batch_hash, this->preparedQC));
    }else if(hash_to_QC[batch_hash].type < this->preparedQC.type){
        hash_to_QC[batch_hash] = this->preparedQC;
    }
    hash_QC_lock.unlock();
#else
    uint64_t instance_id = pcmsg->instance_id;
    hash_QC_lock[instance_id].lock();
    if(!hash_to_QC[instance_id].count(batch_hash)){
        hash_to_QC[instance_id].insert(make_pair<string&,QuorumCertificate&>(batch_hash, this->preparedQC));
    }else if(hash_to_QC[instance_id][batch_hash].type < this->preparedQC.type){
        hash_to_QC[instance_id][batch_hash] = this->preparedQC;
    }
    hash_QC_lock[instance_id].unlock();
#endif
    assert(this->preparedQC.type == PREPARE);
}

void TxnManager::setPreCommittedQC(HOTSTUFFCommitMsg *cmsg){
    this->precommittedQC = cmsg->PreCommittedQC;
    string batch_hash = this->get_hash();
#if !PVP
    hash_QC_lock.lock();
    if(!hash_to_QC.count(batch_hash)){
        hash_to_QC.insert(make_pair<string&,QuorumCertificate&>(batch_hash, this->precommittedQC));
    }
    else if(hash_to_QC[batch_hash].type < this->precommittedQC.type){
        hash_to_QC[batch_hash] = this->precommittedQC;
    }
    hash_QC_lock.unlock();
#else
    uint64_t instance_id = cmsg->instance_id;
    hash_QC_lock[instance_id].lock();
    if(!hash_to_QC[instance_id].count(batch_hash)){
        hash_to_QC[instance_id].insert(make_pair<string&,QuorumCertificate&>(batch_hash, this->precommittedQC));
    }else if(hash_to_QC[instance_id][batch_hash].type < this->precommittedQC.type){
        hash_to_QC[instance_id][batch_hash] = this->precommittedQC;
    }
    hash_QC_lock[instance_id].unlock();
#endif
    assert(this->precommittedQC.type == PRECOMMIT);
}

void TxnManager::setCommittedQC(HOTSTUFFDecideMsg *dmsg){
    this->committedQC = dmsg->CommittedQC;
    string batch_hash = this->get_hash();
#if !PVP
    hash_QC_lock.lock();
    if(!hash_to_QC.count(batch_hash)){
        hash_to_QC.insert(make_pair<string&,QuorumCertificate&>(batch_hash, this->committedQC));
    }
    else if(hash_to_QC[batch_hash].type < this->committedQC.type){
        hash_to_QC[batch_hash] = this->committedQC;
    }
    hash_QC_lock.unlock();
#else
    uint64_t instance_id = dmsg->instance_id;
    hash_QC_lock[instance_id].lock();
    if(!hash_to_QC[instance_id].count(batch_hash)){
        hash_to_QC[instance_id].insert(make_pair<string&,QuorumCertificate&>(batch_hash, this->committedQC));
    }
    else if(hash_to_QC[instance_id][batch_hash].type < this->committedQC.type){
        hash_to_QC[instance_id][batch_hash] = this->committedQC;
    }
    hash_QC_lock[instance_id].unlock();
#endif
    assert(this->committedQC.type == COMMIT);
}

QuorumCertificate TxnManager::get_preparedQC(){
    return preparedQC;
}

QuorumCertificate TxnManager::get_precommittedQC(){
    return precommittedQC;
}

QuorumCertificate TxnManager::get_committedQC(){
    return committedQC;
}

void TxnManager::send_hotstuff_prepare_vote(){
    // printf("%ld Send HOTSTUFF_PREP_VOTE_MSG message to primary %ld\n", get_txn_id(), return_id);
    // fflush(stdout);

    Message *msg = Message::create_message(this, HOTSTUFF_PREP_VOTE_MSG);
    HOTSTUFFPrepareVoteMsg *pvmsg = (HOTSTUFFPrepareVoteMsg *)msg;

    vector<uint64_t> dest;
    dest.push_back(return_id);

    msg_queue.enqueue(get_thd_id(), pvmsg, dest);
    dest.clear();

}

void TxnManager::send_hotstuff_precommit_msgs(){
    Message *msg = Message::create_message(this, HOTSTUFF_PRECOMMIT_MSG);
    HOTSTUFFPreCommitMsg *pcmsg = (HOTSTUFFPreCommitMsg *)msg;
    
    vector<uint64_t> dest;
    for(uint i=0; i<g_node_cnt; i++){
        if(i==g_node_id){
            pcmsg->sign(i);
#if THRESHOLD_SIGNATURE
            this->precommittedQC.signature_share_map[g_node_id] = pcmsg->sig_share;
#endif
            vote_precommit.push_back(i);
            continue;
        }
        dest.push_back(i);
    }
    msg_queue.enqueue(get_thd_id(), pcmsg, dest);
    dest.clear();
}

void TxnManager::send_hotstuff_precommit_vote(){
    // printf("%ld Send HOTSTUFF_PRECOMMIT_VOTE_MSG message to primary %ld\n", get_txn_id(), return_id);
    // fflush(stdout);

    Message *msg = Message::create_message(this, HOTSTUFF_PRECOMMIT_VOTE_MSG);
    HOTSTUFFPreCommitVoteMsg *pvmsg = (HOTSTUFFPreCommitVoteMsg *)msg;

    vector<uint64_t> dest;
    dest.push_back(return_id);

    msg_queue.enqueue(get_thd_id(), pvmsg, dest);
    dest.clear();
}

void TxnManager::send_hotstuff_commit_msgs(){
    Message *msg = Message::create_message(this, HOTSTUFF_COMMIT_MSG);
    HOTSTUFFCommitMsg *cmsg = (HOTSTUFFCommitMsg *)msg;
    
    vector<uint64_t> dest;
    for(uint i=0; i<g_node_cnt; i++){
        if(i == g_node_id){
            cmsg->sign(i);
#if THRESHOLD_SIGNATURE
            this->committedQC.signature_share_map[g_node_id] = cmsg->sig_share;
#endif
            vote_commit.push_back(i);
            continue;
        }
        dest.push_back(i);
    }
    msg_queue.enqueue(get_thd_id(), cmsg, dest);
    dest.clear();
}

void TxnManager::send_hotstuff_commit_vote(){
    // printf("%ld Send HOTSTUFF_COMMIT_VOTE_MSG message to primary %ld\n", get_txn_id(), return_id);
    // fflush(stdout);

    Message *msg = Message::create_message(this, HOTSTUFF_COMMIT_VOTE_MSG);
    HOTSTUFFCommitVoteMsg *pvmsg = (HOTSTUFFCommitVoteMsg *)msg;

    vector<uint64_t> dest;
    dest.push_back(return_id);

    msg_queue.enqueue(get_thd_id(), pvmsg, dest);
    dest.clear();
}

void TxnManager::send_hotstuff_decide_msgs(){
    Message *msg = Message::create_message(this, HOTSTUFF_DECIDE_MSG);
    HOTSTUFFDecideMsg *dmsg = (HOTSTUFFDecideMsg *)msg;
    
    vector<uint64_t> dest;
    for(uint i=0; i<g_node_cnt; i++){
        if(i==g_node_id)
            continue;
        dest.push_back(i);
    }
    msg_queue.enqueue(get_thd_id(), dmsg, dest);
    dest.clear();
}


#if !STOP_NODE_SET
bool TxnManager::send_hotstuff_newview(){
#else
bool TxnManager::send_hotstuff_newview(bool &failednode){
#endif

#if !PVP
    uint64_t dest_node_id = get_view_primary(get_current_view(0) + 1);
#else
    uint64_t dest_node_id = get_view_primary(get_current_view(instance_id) + 1, instance_id);
#endif

#if STOP_NODE_SET
    failednode = false;
    stop_lock.lock();
    if(stop_node_set.count(dest_node_id)){
        failednode = true;
    }
    stop_lock.unlock();
    if(failednode)  return false;
#endif

    vector<uint64_t> dest;

    Message *msg = Message::create_message(this, HOTSTUFF_NEW_VIEW_MSG);
    HOTSTUFFNewViewMsg *nvmsg = (HOTSTUFFNewViewMsg *)msg;
    nvmsg->force = true;
    nvmsg->sig_empty = false;
    nvmsg->digital_sign();
    if(g_node_id != dest_node_id)
    {
        #if SEND_NEWVIEW_PRINT
        printf("%ld Send HOTSTUFF_NEW_VIEW_MSG message to %ld   %f\n", get_txn_id(), dest_node_id, simulation->seconds_from_start(get_sys_clock()));
        fflush(stdout);
        #endif
        dest.push_back(dest_node_id);   //send new view to the next primary 
        msg_queue.enqueue(get_thd_id(), nvmsg, dest);
        dest.clear();
    }else{
        this->genericQC.signature_share_map[g_node_id] = nvmsg->sig_share;
    }
    Message *msg2 = Message::create_message(this, HOTSTUFF_NEW_VIEW_MSG);
    HOTSTUFFNewViewMsg *nvmsg2 = (HOTSTUFFNewViewMsg *)msg2;
    for(uint64_t i = dest_node_id + 1; i < g_node_cnt; i++){
        if(i==g_node_id){
            continue;
        }
        dest.push_back(i);
    }
    for(uint64_t i = 0; i < dest_node_id; i++){
        if(i==g_node_id){
            continue;
        }
        dest.push_back(i);
    }
    if(nvmsg2->instance_id == 0){
        nvmsg2->force = true;
    }
    msg_queue.enqueue(get_thd_id(), nvmsg2, dest);
    dest.clear();

#if SEPARATE
    if(--this->new_view_vote_cnt == 0){
        this->set_new_viewed();
    }
#endif

#if TIMER_ON
    #if !PVP
    server_timer->waiting_prepare = true;
    server_timer->last_new_view_time = get_sys_clock();
    #else
    server_timer[instance_id]->waiting_prepare = true;
    server_timer[instance_id]->last_new_view_time = get_sys_clock();
    #endif
#endif

    return true;
}


#if SEPARATE
void TxnManager::send_hotstuff_generic(){
    Message *msg = Message::create_message(this, HOTSTUFF_GENERIC_MSG);
    HOTSTUFFGenericMsg *gene = (HOTSTUFFGenericMsg *)msg;
    assert(!this->get_hash().empty());
    this->highQC = gene->highQC = get_g_preparedQC(instance_id);
    #if MAC_VERSION
    gene->digital_sign();
    this->psig_share = gene->psig_share;
    #endif

    vector<uint64_t> dest = nodes_to_send(0, g_node_cnt);
    msg_queue.enqueue(get_thd_id(), gene, dest);
    dest.clear();
}
#endif

#endif