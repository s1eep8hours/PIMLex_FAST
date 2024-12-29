#ifndef _DRAM_INDEX_H_
#define _DRAM_INDEX_H_

#include "./pgm_index.hpp"
#include <iostream>
#include "../support/common.h"
#include "../support/concurrent.h"
#include <mutex>
#include <shared_mutex>
#include "../support/typedefine.h"
#include "../support/opt_overflow_tree.h"


#define RECORD_TRANSMIT 0
std::atomic<int64_t> cputopim(0);


struct entry_t {
    DTYPE buf_key;
    string_payload* buf_payload;
};

#define BUF_SIZE 16
class buf_page{
    public:
    uint32_t page_count;
    uint32_t lock_;
    DTYPE buf_key[BUF_SIZE];
    string_payload* buf_payload[BUF_SIZE];
    buf_page(int i){
        page_count = 0;
        lock_ = 0;
    }

    inline void init(){
       page_count = 0;
        lock_ = 0; 
    }

    buf_page(){
    }

    /*** concurrency management **/
    inline void get_lock() {
    uint32_t new_value = 0;
    uint32_t old_value = 0;
    do {
        while (true) {
        old_value = __atomic_load_n(&lock_, __ATOMIC_ACQUIRE);
        if (!(old_value & lockSet)) {
            old_value &= lockMask;
            break;
        }
        }
        new_value = old_value | lockSet;
    } while (!CAS(&lock_, &old_value, new_value));
    }

    inline void release_lock() {
    uint32_t v = lock_;
    __atomic_store_n(&lock_, v + 1 - lockSet, __ATOMIC_RELEASE);
    }

    /*if the lock is set, return true*/
    inline bool test_lock_set(uint32_t &version) {
    version = __atomic_load_n(&lock_, __ATOMIC_ACQUIRE);
    return (version & lockSet) != 0;
    }

    // test whether the version has change, if change, return true
    inline bool test_lock_version_change(uint32_t old_version) {
    auto value = __atomic_load_n(&lock_, __ATOMIC_ACQUIRE);
    return (old_version != value);
    }

    bool insert_page(DTYPE& key, string_payload* val){
        get_lock();
        if(page_count >= BUF_SIZE){
            release_lock();
            return false;
        }
        if(page_count == 0){
            buf_key[0] = key;
            buf_payload[0] = new string_payload(); 
            *buf_payload[0] = *val;
            page_count++;
            release_lock();
            return true;
        }
        __builtin_prefetch(buf_payload, 0);

        int pl = 0, pr = page_count, pmid;
        while(pl < pr){
            #if RECORD_TRANSMIT
            cputodram += 8;
            #endif
            pmid = pl + (pr - pl) / 2;
            if(buf_key[pmid] <= key){
                pl = pmid + 1;
            }else{
                pr = pmid;
            }
        }

        memmove(&(buf_key[pl + 1]), &(buf_key[pl]), sizeof(DTYPE) * (page_count - pl));
        memmove(&(buf_payload[pl + 1]), &(buf_payload[pl]), sizeof(string_payload*) * (page_count - pl));
        buf_key[pl] = key;
        buf_payload[pl] = new string_payload(); 
        *buf_payload[pl] = *val;
        page_count++;
        release_lock();
        return true;
    }

    bool find_key_page(DTYPE& key, string_payload* val){
        int pl, pr, pmid;

        get_lock();
        if(page_count == 0){
            release_lock();
            return false;
        }
        pl = 0;
        pr = page_count;
        while(pl < pr){
            pmid = pl + (pr - pl) / 2;
            if(buf_key[pmid] <= key){
                pl = pmid + 1;
            }else{
                pr = pmid;
            }
        }
        val = buf_payload[pl - 1];
        release_lock();
        return true;
    }

    bool update_key_page(DTYPE& key, string_payload* val){
        get_lock();
        if(page_count == 0){
            release_lock();
            return false;
        }
        int pl = 0, pr = page_count, pmid;
        while(pl < pr){
            pmid = pl + (pr - pl) / 2;
            if(buf_key[pmid] <= key){
                pl = pmid + 1;
            }else{
                pr = pmid;
            }
        }
        *buf_payload[pl - 1]  = *val;
        release_lock();
        return true;
    }

    bool is_sort(){
        int flag = 0;
        for(uint64_t start = 1; start < page_count; start++){
            if(buf_key[start] < buf_key[start - 1])
                flag = 1;
        }
        if(flag == 1){
            return false;
        }
        return true;
    }

};

#define NR_PARTITION 128
class DRAM_index{
    public:
    DTYPE min_key;
    DTYPE* partial_keys;
    DTYPE* keys;
    string_payload* payloads;
    pgm_model_t* transfer_model[NR_DPUS];
    pgm::PGMIndex<DTYPE>* pgm_dram_index[NR_DPUS];
    size_t partiton_epsilon[NR_DPUS];
    uint64_t total_size;
    uint64_t partial_total_size;
    uint32_t dram_start_pos[NR_DPUS];
    buf_page* buffer_pages;
    uint64_t num_buf_pages; 
    OptBTree* opt_overflow_trees;
    std::vector<DTYPE> collect_overflow_keys;
    std::vector<string_payload> collect_overflow_val;
    OptBTree temp_dram_tree;
    #if USE_LUT
    lut_model_t* indram_lut_model[NR_DPUS];
    #endif

    DRAM_index(){
        opt_overflow_trees = new OptBTree[NR_PARTITION]; 
    }

    void get_payload_direct(int pos, string_payload* ret){
        *ret = payloads[pos];
    }
};

#endif