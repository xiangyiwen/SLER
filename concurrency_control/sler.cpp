//
// Created by root on 2022/9/22.
//
#include "txn.h"
#include "row.h"
#include "row_sler.h"
#include "manager.h"
#include <mm_malloc.h>


#if CC_ALG == SLER

RC txn_man::validate_sler(RC rc) {
    uint64_t starttime = get_sys_clock();
    while(true){
        uint64_t span = get_sys_clock() - starttime;
        if(span > 10000000){
            printf("txn_id:%lu,validate_time: %lu\n",sler_txn_id,span);
            abort_process(this);
            return Abort;
        }

        // Abort myself actively
        if(status == ABORTED || rc == Abort){
            abort_process(this);
            return Abort;
        }
        if(sler_semaphore == 0){
            break;
        }
    }

    /**
     * Update status.
     */
//    while(!ATOM_CAS(status_latch, false, true))
//        PAUSE
//    // Abort myself actively  [this shouldn't happen]
//    if(status == ABORTED){
//        status_latch = false;
//        abort_process(this);
//        return Abort;
//    }
//    else if(status == RUNNING){
//        status = validating;
//    }
//    else
//        assert(false);
//    status_latch = false;

    // Abort myself actively
    if(status == ABORTED){
        abort_process(this);
        return Abort;
    }
    else if(status == RUNNING){
        int i = 0;
        if(!ATOM_CAS(status, RUNNING, validating)) {
            abort_process(this);
            return Abort;
        }
    }
    else {
        assert(status == ABORTED);
        abort_process(this);
        return Abort;
    }


    /**
     * Validate the read & write set
     */
    uint64_t min_next_begin = UINT64_MAX;
    uint64_t serial_id = 0;

    for(int rid = 0; rid < row_cnt; rid++){
        /**
         * BUG: this causes all begin_ts and end_ts = 0
         *
         */
//        if(accesses[rid]->type == WR){
//            continue;
//        }

        // Caculate serial_ID
        Version* current_version = (Version*)accesses[rid]->tuple_version;
        if(serial_id <= current_version->begin_ts) {

            if(current_version->begin_ts == UINT64_MAX){
                printf("You should wait!\n");
            }
            serial_id = current_version->begin_ts + 1;
        }

        if(accesses[rid]->type == WR){
            continue;
        }

        // Check RW dependency
        Version* newer_version = current_version->prev;
        if(newer_version){

            txn_man* newer_version_txn = newer_version->retire;
            // New version is uncommitted
            if(newer_version_txn){

                while(!ATOM_CAS(newer_version_txn->status_latch, false, true)){
                    PAUSE
                }
                status_t temp_status = newer_version_txn->status;

                if(temp_status == RUNNING){
                    if(newer_version->begin_ts != UINT64_MAX){
                        assert(newer_version->retire == nullptr);
                        min_next_begin = std::min(min_next_begin,newer_version->begin_ts);
                    }
                    else{
                        assert(newer_version->begin_ts == UINT64_MAX);

                        // Record RW dependency
                        newer_version_txn->SemaphoreAddOne();
                        PushDependency(newer_version_txn,newer_version_txn->get_sler_txn_id(),DepType::READ_WRITE_);

//                    // Update waiting set
//                    newer_version_txn->UnionWaitingSet(sler_waiting_set);
//
//                    //更新依赖链表中所有事务的 waiting_set
//                    auto deps = newer_version_txn->sler_dependency;
//                    for(auto dep_pair :deps){
//                        txn_man* txn_dep = dep_pair.dep_txn;
//
//                        txn_dep->UnionWaitingSet(newer_version_txn->sler_waiting_set);
//                    }
                        // 11-8 ---------------------------------------------------------------------
                    }
                }
                else if(temp_status == writing || temp_status == committing || temp_status == COMMITED){
                    // Treat next tuple version as committed(Do nothing here)
                    min_next_begin = std::min(min_next_begin,newer_version_txn->sler_serial_id);
                }
                else if(temp_status == validating){
                    newer_version_txn->status_latch = false;
                    abort_process(this);
                    return Abort;

                    //Todo: maybe we can wait until newer_version_txn commit/abort,but how can it inform me before that thread start next txn
                    /*
                    // abort in advance
                    if(serial_id >= std::min(min_next_begin,newer_version_txn->sler_serial_id)){
                        abort_process(this);
                        return Abort;
                    }
                     // wait until newer_version_txn commit / abort
                     */
                }
                else if(temp_status == ABORTED){
                    // next_version = NULL
                    newer_version_txn->status_latch = false;
                    continue;
                }

                newer_version_txn->status_latch = false;
            }
            // new version is committed
            else{
                min_next_begin = std::min(min_next_begin,newer_version->begin_ts);
            }

            if(serial_id >= min_next_begin){
                abort_process(this);
                return Abort;
            }
        }
    }


    /**
     * Writing phase
     */
     this->sler_serial_id = serial_id;

//     if(this->sler_serial_id != 1) printf("%d\n",this->sler_serial_id);
//     if(this->sler_serial_id == 0) printf("ERROR.\n");
//     if(this->sler_serial_id == UINT64_MAX) printf("MAX serial_ID!\n");

     // Update status.
     while(!ATOM_CAS(status_latch, false, true))
         PAUSE
     status = writing;
     status_latch = false;

     for(int rid = 0; rid < row_cnt; rid++){
         if(accesses[rid]->type == RD){
             continue;
         }

         Version* old_version = (Version*)accesses[rid]->tuple_version;
         Version* new_version = old_version->prev;
//
         while (!ATOM_CAS(accesses[rid]->orig_row->manager->blatch, false, true)){
             PAUSE
         }

//         while (!ATOM_CAS(old_version->version_latch, false, true)){
//             PAUSE
//         }
//         old_version->end_ts = this->sler_serial_id;
//         old_version->version_latch = false;

//         while (!ATOM_CAS(new_version->version_latch, false, true)){
//             PAUSE
//         }

//         assert(new_version->begin_ts = UINT64_MAX && new_version->retire == this);
         assert(new_version->begin_ts == UINT64_MAX && new_version->retire == this);

         old_version->end_ts = this->sler_serial_id;
         new_version->begin_ts = this->sler_serial_id;
         new_version->retire = nullptr;

         accesses[rid]->orig_row->manager->blatch = false;
//         new_version->version_latch = false;
     }


     /**
      * Releasing Dependency
      */
     // Update status.
     while(!ATOM_CAS(status_latch, false, true))
         PAUSE

     status = committing;
     status_latch = false;

     auto deps = sler_dependency;
//     for(auto dep_pair :deps){
//         txn_man *txn_dep = dep_pair.first;
//         uint64_t origin_txn_id = dep_pair.second.origin_txn_id;
//         DepType type = dep_pair.second.dep_type;
//
//         // only inform the txn which wasn't aborted
//         if(txn_dep->get_sler_txn_id() == origin_txn_id){
//             int num = type;
//             while(num != 0){
//                 if((num & 1) == 1){
//                     txn_dep->SemaphoreSubOne();
//                 }
//                 num = num >> 1;
//             }
//
//             // if there is a RW dependency
//             if((type & READ_WRITE_) != 0){
//                 // serialize other threads to concurrently modify the serial_ID
//                 while(!ATOM_CAS(txn_dep->serial_id_latch, false, true))
//                     PAUSE
//                 txn_dep->sler_serial_id = max(txn_dep->sler_serial_id, this->sler_serial_id + 1);
//                 txn_dep->serial_id_latch = false;
//             }
//         }
//     }

     for(auto dep_pair :deps){
         txn_man *txn_dep = dep_pair.dep_txn;
         uint64_t origin_txn_id = dep_pair.dep_txn_id;
         DepType type = dep_pair.dep_type;

         // only inform the txn which wasn't aborted and really depend on me
         if(txn_dep->status != ABORTED && txn_dep->get_sler_txn_id() == origin_txn_id ){

             // if there is a RW dependency
             if(type == READ_WRITE_){
                 uint64_t origin_serial_ID;
                 uint64_t new_serial_ID;
                 do {
                     origin_serial_ID = txn_dep->sler_serial_id;
                     new_serial_ID = this->sler_serial_id + 1;
                 } while (origin_serial_ID < new_serial_ID && !ATOM_CAS(txn_dep->sler_serial_id, origin_serial_ID, new_serial_ID));

//                 // serialize other threads to concurrently modify the serial_ID
//                 while(!ATOM_CAS(txn_dep->serial_id_latch, false, true))
//                     PAUSE
//                     txn_dep->sler_serial_id = max(txn_dep->sler_serial_id, this->sler_serial_id + 1);
//                 txn_dep->serial_id_latch = false;
             }

             txn_dep->SemaphoreSubOne();
         }
     }


    // Update status.
    while(!ATOM_CAS(status_latch, false, true))
        PAUSE
        status = COMMITED;
    status_latch = false;

    return rc;
}



void txn_man::abort_process(txn_man * txn){
//    while(!ATOM_CAS(status_latch, false, true))
//        PAUSE
//        status = ABORTED;
//    status_latch = false;

    //11-18
//    if(row_cnt == 0){
//       cout << "type: " << accesses[0]->type << endl;
//       Version* newer = (Version*)accesses[0]->tuple_version;
//       newer = newer->prev;
//       if(newer) {
//           cout << "new version: " << newer << endl;
//           cout << "new version(retire txn): " << newer->retire << endl;
//           cout << "new version(retire txn - sler_txn_id): " << newer->retire->sler_txn_id << endl;
//           cout << "new version(retire ID): " << newer->retire_ID << endl;
//           cout << "txn: " << txn << endl;
//           cout << "txn(sler_txn_id): " << txn->sler_txn_id << endl;
//       }
//    }


    for(int rid = 0; rid < row_cnt; rid++){
        if(accesses[rid]->type == RD){
            continue;
        }

        Version* old_version = (Version*)accesses[rid]->tuple_version;
        Version* new_version = old_version->prev;

        while (!ATOM_CAS(accesses[rid]->orig_row->manager->blatch, false, true)){
            PAUSE
        }

//        while (!ATOM_CAS(old_version->version_latch, false, true)){
//            PAUSE
//        }
//        while (!ATOM_CAS(new_version->version_latch, false, true)){
//            PAUSE
//        }

        assert(new_version->begin_ts == UINT64_MAX && new_version->retire == this);

        Version* row_header = accesses[rid]->orig_row->manager->get_version_header();

        // new version is the newest version
        if(new_version == row_header){
            old_version->prev = NULL;
            accesses[rid]->orig_row->manager->version_header = old_version;
            new_version->next = NULL;
        }
        else{
            Version* pre_new = new_version->prev;
            pre_new->next = old_version;
            old_version->prev = pre_new;
            new_version->prev = NULL;
            new_version->next = NULL;
        }

//        old_version->version_latch = false;

//        new_version->row->free_row();
//        _mm_free(new_version->row);
//        new_version->row = NULL;
//        _mm_free(new_version->data);


        new_version->retire = nullptr;

//        new_version->version_latch = false;

        _mm_free(new_version);
        new_version = NULL;

        accesses[rid]->orig_row->manager->blatch = false;

    }

    /**
     * Cascading abort
     */
    auto deps = sler_dependency;
//    for(auto dep_pair :deps){
//        txn_man *txn_dep = dep_pair.first;
//        uint64_t origin_txn_id = dep_pair.second.origin_txn_id;
//        DepType type = dep_pair.second.dep_type;
//
//        // only inform the txn which wasn't aborted
//        if(txn_dep->get_sler_txn_id() == origin_txn_id){
//            if((type & WRITE_WRITE_) != 0 || (type & WRITE_READ_) != 0){
////                while(!ATOM_CAS(txn_dep->status_latch, false, true)){
////                    PAUSE
////                }
////                txn_dep->status = ABORTED;
////                txn_dep->status_latch = false;
//                txn_dep->set_abort();
//            }
//        }
//    }

    for(auto dep_pair :deps){
        txn_man *txn_dep = dep_pair.dep_txn;
        uint64_t origin_txn_id = dep_pair.dep_txn_id;
        DepType type = dep_pair.dep_type;

        // only inform the txn which wasn't aborted
        if(txn_dep->get_sler_txn_id() == origin_txn_id){
            if((type == WRITE_WRITE_) || (type == WRITE_READ_)){
                txn_dep->set_abort();
            }
        }
    }

//    while(!ATOM_CAS(status_latch, false, true))
//        PAUSE
//        status = ABORTED;
//    status_latch = false;
}


#endif

