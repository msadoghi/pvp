#include "global.h"
#include "message.h"
#include "thread.h"
#include "worker_thread.h"
#include "txn.h"
#include "wl.h"
#include "query.h"
#include "ycsb_query.h"
#include "math.h"
#include "helper.h"
#include "msg_thread.h"
#include "msg_queue.h"
#include "work_queue.h"
#include "message.h"
#include "timer.h"
#include "chain.h"

#if SHARPER
/**
 * Processes an incoming client batch and sends a Pre-prepare message to al replicas.
 *
 * This function assumes that a client sends a batch of transactions and 
 * for each transaction in the batch, a separate transaction manager is created. 
 * Next, this batch is forwarded to all the replicas as a BatchRequests Message, 
 * which corresponds to the Pre-Prepare stage in the PBFT protocol.
 *
 * @param msg Batch of Transactions of type CientQueryBatch from the client.
 * @return RC
 */
RC WorkerThread::process_client_batch(Message *msg)
{

    ClientQueryBatch *clbtch = (ClientQueryBatch *)msg;

    // printf("Client Request: %ld, Shards: %s :: client: %ld ::\n", msg->txn_id, clbtch->is_cross_shard ? "Cross" : "Single", msg->return_node_id);
    // fflush(stdout);

    // Authenticate the client signature.
    validate_msg(clbtch);

#if VIEW_CHANGES
    // If message forwarded to the non-primary.
    if (g_node_id != get_current_view(get_thd_id()))
    {
        client_query_check(clbtch);
        return RCOK;
    }

    // Partial failure of Primary 0.
    // fail_primary(msg, 9);
#endif
    // Initialize all transaction mangers and Send BatchRequests message.
    if (clbtch->is_cross_shard)
        create_and_send_batchreq_cross(clbtch, clbtch->txn_id);
    else
        create_and_send_batchreq(clbtch, clbtch->txn_id);

    return RCOK;
}

void WorkerThread::create_and_send_batchreq_cross(ClientQueryBatch *msg, uint64_t tid)
{
    // Creating a new BatchRequests Message.
    Message *bmsg = Message::create_message(SUPER_PROPOSE);
    SuperPropose *breq = (SuperPropose *)bmsg;
    breq->init(get_thd_id());
    breq->is_cross_shard = true;
    for (uint64_t i = 0; i < g_shard_cnt; i++)
        breq->involved_shards[i] = msg->involved_shards[i];

    // Starting index for this batch of transactions.
    next_set = tid;

    // String of transactions in a batch to generate hash.
    string batchStr;

    // Allocate transaction manager for all the requests in batch.
    for (uint64_t i = 0; i < get_batch_size(); i++)
    {
        uint64_t txn_id = get_next_txn_id() + i;

        //cout << "Txn: " << txn_id << " :: Thd: " << get_thd_id() << "\n";
        //fflush(stdout);
        txn_man = get_transaction_manager(txn_id, 0);

        // Unset this txn man so that no other thread can concurrently use.
        unset_ready_txn(txn_man);

        txn_man->register_thread(this);
        txn_man->return_id = msg->return_node;

        // Fields that need to updated according to the specific algorithm.
        algorithm_specific_update(msg, i);

        init_txn_man(msg->cqrySet[i]);

        // Append string representation of this txn.
        batchStr += msg->cqrySet[i]->getString();

        // Setting up data for BatchRequests Message.
        breq->copy_from_txn(txn_man, msg->cqrySet[i]);

        // Reset this txn manager.
        bool ready = txn_man->set_ready();
        assert(ready);
    }

    // Now we need to unset the txn_man again for the last txn of batch.
    unset_ready_txn(txn_man);

    // Generating the hash representing the whole batch in last txn man.
    txn_man->set_hash(calculateHash(batchStr));
    txn_man->hashSize = txn_man->hash.length();

    breq->copy_from_txn(txn_man);

    // Storing the BatchRequests message.
    txn_man->set_primarybatch(breq);

    if (breq->is_cross_shard)
    {
        digest_directory.add(breq->hash, breq->txn_id / get_batch_size());
        // cout << "digest setted " << digest_directory.get(breq->hash) << endl;
        txn_man->is_cross_shard = true;
        for (uint64_t i = 0; i < g_shard_cnt; i++)
        {
            txn_man->involved_shards[i] = breq->involved_shards[i];
        }
    }

    // Storing all the signatures.
    vector<uint64_t> dest;

    for (uint64_t i = 0; i < g_node_cnt; i++)
    {
        // Do not send to itself and not involved shards
        if (i == g_node_id || !msg->involved_shards[get_shard_number(i)])
        {
            continue;
        }
        // Do not send to other shards replicas
        if (!is_in_same_shard(i, g_node_id) && !is_primary_node(get_thd_id(), i))
        {
            continue;
        }
        dest.push_back(i);
        breq->sign(i);
    }

    msg_queue.enqueue(get_thd_id(), breq, dest);
}

/**
 * Process incoming BatchRequests message from the Primary.
 *
 * This function is used by the non-primary or backup replicas to process an incoming
 * BatchRequests message sent by the primary replica. This processing would require 
 * sending messages of type PBFTPrepMessage, which correspond to the Prepare phase of 
 * the PBFT protocol. Due to network delays, it is possible that a repica may have 
 * received some messages of type PBFTPrepMessage and PBFTCommitMessage, prior to 
 * receiving this BatchRequests message.
 *
 * @param msg Batch of Transactions of type BatchRequests from the primary.
 * @return RC
 */
RC WorkerThread::process_batch(Message *msg)
{
    uint64_t cntime = get_sys_clock();

    BatchRequests *breq = (BatchRequests *)msg;

    // printf("BatchRequests: %ld : %s : from: %ld \n", breq->txn_id / get_batch_size(), breq->is_cross_shard ? "Cross" : "Single", breq->return_node_id);
    // fflush(stdout);

    // Assert that only a non-primary replica has received this message.
    assert(g_node_id != get_current_view(get_thd_id()));

    // Check if the message is valid.
    validate_msg(breq);

#if VIEW_CHANGES
    // Store the batch as it could be needed during view changes.
    store_batch_msg(breq);
#endif

    // Allocate transaction managers for all the transactions in the batch.
    set_txn_man_fields(breq, 0);

#if TIMER_ON
    // The timer for this client batch stores the hash of last request.
    add_timer(breq, txn_man->get_hash());
#endif

    if (breq->is_cross_shard)
    {
        digest_directory.add(breq->hash, breq->txn_id / get_batch_size());
        // cout << "digest setted " << digest_directory.get(breq->hash) << endl;
        txn_man->is_cross_shard = true;
        for (uint64_t i = 0; i < g_shard_cnt; i++)
        {
            txn_man->involved_shards[i] = breq->involved_shards[i];
        }
    }

    // Storing the BatchRequests message.
    txn_man->set_primarybatch(breq);

    // Send Prepare messages.
    txn_man->send_pbft_prep_msgs();

    // End the counter for pre-prepare phase as prepare phase starts next.
    double timepre = get_sys_clock() - cntime;
    INC_STATS(get_thd_id(), time_pre_prepare, timepre);

    // Only when BatchRequests message comes after some Prepare message.
    for (uint64_t i = 0; i < txn_man->info_prepare.size(); i++)
    {
        if (breq->is_cross_shard)
        {
            txn_man->decr_prep_rsp_cnt(get_shard_number(txn_man->info_prepare[i]));
            bool i_prep = true;
            for (uint64_t i = 0; i < g_shard_cnt; i++)
            {
                if (txn_man->prep_rsp_cnt_arr[i] != 0)
                    i_prep = false;
            }
            if (i_prep)
            {
                txn_man->set_prepared();
                break;
            }
        }
        else
        {
            // Decrement.
            uint64_t num_prep = txn_man->decr_prep_rsp_cnt();
            if (num_prep == 0)
            {
                txn_man->set_prepared();
                break;
            }
        }
    }

    // If enough Prepare messages have already arrived.
    if (txn_man->is_prepared())
    {
        // Send Commit messages.
        txn_man->send_pbft_commit_msgs();

        double timeprep = get_sys_clock() - txn_man->txn_stats.time_start_prepare - timepre;
        INC_STATS(get_thd_id(), time_prepare, timeprep);
        double timediff = get_sys_clock() - cntime;

        // Check if any Commit messages arrived before this BatchRequests message.
        for (uint64_t i = 0; i < txn_man->info_commit.size(); i++)
        {
            uint64_t num_comm = txn_man->decr_commit_rsp_cnt();
            if (num_comm == 0)
            {
                txn_man->set_committed();
                break;
            }
        }

        // If enough Commit messages have already arrived.
        if (txn_man->is_committed())
        {
#if TIMER_ON
            // End the timer for this client batch.
            remove_timer(txn_man->hash);
#endif
            // Proceed to executing this batch of transactions.
            send_execute_msg();

            // End the commit counter.
            INC_STATS(get_thd_id(), time_commit, get_sys_clock() - txn_man->txn_stats.time_start_commit - timediff);
        }
    }
    else
    {
        // Although batch has not prepared, still some commit messages could have arrived.
        for (uint64_t i = 0; i < txn_man->info_commit.size(); i++)
        {
            txn_man->decr_commit_rsp_cnt();
        }
    }

    // Release this txn_man for other threads to use.
    bool ready = txn_man->set_ready();
    assert(ready);

    // UnSetting the ready for the txn id representing this batch.
    txn_man = get_transaction_manager(msg->txn_id, 0);
    unset_ready_txn(txn_man);

    return RCOK;
}

RC WorkerThread::process_super_propose(Message *msg)
{

    SuperPropose *breq = (SuperPropose *)msg;
    assert(breq->is_cross_shard);
    assert(breq->involved_shards[get_shard_number(g_node_id)]);
    // printf("SuperPropose: %ld : from: %ld\n", breq->txn_id, breq->return_node_id);
    // fflush(stdout);

    // Assert that only a non-primary replica has received this message.
    assert(is_primary_node(get_thd_id(), g_node_id));
    assert(breq->is_cross_shard);
    assert(breq->involved_shards[get_shard_number(g_node_id)]);
    // Check if the message is valid.
    ///////////////////////////////////////

    // Starting index for this batch of transactions.
    next_set = breq->txn_id;
    breq->index.release();
    breq->index.init(get_batch_size());

    // Allocate transaction manager for all the requests in batch.
    for (uint64_t i = 0; i < get_batch_size(); i++)
    {
        uint64_t txn_id = get_next_txn_id() + i;

        //cout << "Txn: " << txn_id << " :: Thd: " << get_thd_id() << "\n";
        //fflush(stdout);
        txn_man = get_transaction_manager(txn_id, 0);

        // Unset this txn man so that no other thread can concurrently use.
        unset_ready_txn(txn_man);

        txn_man->register_thread(this);
        txn_man->return_id = msg->return_node_id;

        // Fields that need to updated according to the specific algorithm.
        algorithm_specific_update(msg, i);

        init_txn_man(breq->requestMsg[i]);
        breq->index.add(txn_man->get_txn_id());

        // Reset this txn manager.
        bool ready = txn_man->set_ready();
        assert(ready);
    }

    // Now we need to unset the txn_man again for the last txn of batch.
    unset_ready_txn(txn_man);

    // Generating the hash representing the whole batch in last txn man.
    txn_man->set_hash(breq->hash);
    txn_man->hashSize = txn_man->hash.length();

    // Storing the BatchRequests message.
    txn_man->set_primarybatch((BatchRequests *)(breq));

    if (breq->is_cross_shard)
    {
        txn_man->is_cross_shard = true;
        for (uint64_t i = 0; i < g_shard_cnt; i++)
        {
            txn_man->involved_shards[i] = breq->involved_shards[i];
        }
    }

    //////////////////////////////////////
    breq->txn_id = txn_man->get_txn_id() - 2;
    digest_directory.add(breq->hash, breq->txn_id / get_batch_size());
    // cout << "digest setted " << digest_directory.get(breq->hash) << endl;
    char *buf = create_msg_buffer(breq);
    Message *deepMsg = deep_copy_msg(buf, breq);
    BatchRequests *batch_req_message = (BatchRequests *)deepMsg;
    batch_req_message->rtype = BATCH_REQ;
    delete_msg_buffer(buf);
    vector<uint64_t> dest;

    for (uint64_t i = 0; i < g_node_cnt; i++)
    {
        if (i == g_node_id || !is_in_same_shard(i, g_node_id))
        {
            continue;
        }
        dest.push_back(i);

        batch_req_message->sign(i);
    }
    msg_queue.enqueue(get_thd_id(), batch_req_message, dest);
    return RCOK;
}

/**
 * Processes incoming Prepare message.
 *
 * This functions precessing incoming messages of type PBFTPrepMessage. If a replica 
 * received 2f identical Prepare messages from distinct replicas, then it creates 
 * and sends a PBFTCommitMessage to all the other replicas.
 *
 * @param msg Prepare message of type PBFTPrepMessage from a replica.
 * @return RC
 */
RC WorkerThread::process_pbft_prep_msg(Message *msg)
{
    // cout << "prep: " << msg->txn_id / get_batch_size() << "\tFROM: " << msg->return_node_id << (msg->is_cross_shard ? " Cross " : " Single   ") << txn_man->get_txn_id() << endl;
    // fflush(stdout);

    // Check if the incoming message is valid.
    PBFTPrepMessage *pmsg = (PBFTPrepMessage *)msg;
    validate_msg(pmsg);

    // Check if sufficient number of Prepare messages have arrived.
    if (prepared(pmsg))
    {
        // cout << "prepared  " << msg->txn_id / get_batch_size() << endl;
        // Send Commit messages.
        txn_man->send_pbft_commit_msgs();

        // End the prepare counter.
        INC_STATS(get_thd_id(), time_prepare, get_sys_clock() - txn_man->txn_stats.time_start_prepare);
    }

    return RCOK;
}

/**
 * Checks if the incoming PBFTCommitMessage can be accepted.
 *
 * This functions checks if the hash and view of the commit message matches that of 
 * the Pre-Prepare message. Once 2f+1 messages are received it returns a true and 
 * sets the `is_committed` flag for furtue identification.
 *
 * @param msg PBFTCommitMessage.
 * @return bool True if the transactions of this batch can be executed.
 */
bool WorkerThread::committed_local(PBFTCommitMessage *msg)
{
    // cout << "Check Commit: TID: " << txn_man->get_txn_id() << "\n";
    // fflush(stdout);

    // Once committed is set for this transaction, no further processing.
    if (txn_man->is_committed())
    {
        return false;
    }

    // If BatchRequests messages has not arrived, then hash is empty; return false.
    if (txn_man->get_hash().empty())
    {
        //cout << "hash empty: " << txn_man->get_txn_id() << "\n";
        //fflush(stdout);
        txn_man->info_commit.push_back(msg->return_node);
        return false;
    }
    else
    {
        if (!checkMsg(msg))
        {
            // If message did not match.
            //cout << txn_man->get_hash() << " :: " << msg->hash << "\n";
            //cout << get_current_view(get_thd_id()) << " :: " << msg->view << "\n";
            //fflush(stdout);
            return false;
        }
    }
    if (msg->is_cross_shard)
    {
        // cout << "counting cross shard commits " << msg->return_node_id << "  " << msg->txn_id << endl;
        txn_man->decr_commit_rsp_cnt(get_shard_number(msg->return_node_id));
        for (uint64_t i = 0; i < g_shard_cnt; i++)
        {
            if (txn_man->commit_rsp_cnt_arr[i] != 0 && txn_man->involved_shards[i])
                return false;
        }
        txn_man->set_committed();
        return true;
    }
    uint64_t comm_cnt = txn_man->decr_commit_rsp_cnt();
    if (comm_cnt == 0 && txn_man->is_prepared())
    {
        txn_man->set_committed();
        return true;
    }

    return false;
}

/**
 * Processes incoming Commit message.
 *
 * This functions precessing incoming messages of type PBFTCommitMessage. If a replica 
 * received 2f+1 identical Commit messages from distinct replicas, then it asks the 
 * execute-thread to execute all the transactions in this batch.
 *
 * @param msg Commit message of type PBFTCommitMessage from a replica.
 * @return RC
 */
RC WorkerThread::process_pbft_commit_msg(Message *msg)
{
    // cout << "comm: " << msg->txn_id / get_batch_size() << "\tfrom: " << msg->return_node_id << (msg->is_cross_shard ? " Cross " : " Single   ") << txn_man->get_txn_id() << endl;
    // fflush(stdout);

    if (txn_man->commit_rsp_cnt == 2 * g_min_invalid_nodes + 1)
    {
        txn_man->txn_stats.time_start_commit = get_sys_clock();
    }

    // Check if message is valid.
    PBFTCommitMessage *pcmsg = (PBFTCommitMessage *)msg;
    validate_msg(pcmsg);

    txn_man->add_commit_msg(pcmsg);
    txn_man->is_cross_shard = pcmsg->is_cross_shard;
    // Check if sufficient number of Commit messages have arrived.
    if (committed_local(pcmsg))
    {
        // cout << "commited  " << msg->txn_id / get_batch_size() << endl;
#if TIMER_ON
        // End the timer for this client batch.
        remove_timer(txn_man->hash);
#endif

        // Add this message to execute thread's queue.
        send_execute_msg();

        INC_STATS(get_thd_id(), time_commit, get_sys_clock() - txn_man->txn_stats.time_start_commit);
    }

    return RCOK;
}
#endif