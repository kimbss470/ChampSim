#include "cache.h"
#include "set.h"

#include "ooo_cpu.h"
#include "uncore.h"

uint64_t l2pf_access = 0;

void CACHE::handle_fill()
{
    // handle fill
    uint32_t fill_cpu = (MSHR.next_fill_index == MSHR_SIZE) ? NUM_CPUS : MSHR.entry[MSHR.next_fill_index].cpu;
    if (fill_cpu == NUM_CPUS)
        return;

    if (MSHR.next_fill_cycle <= current_core_cycle[fill_cpu]) {

#ifdef SANITY_CHECK
        if (MSHR.next_fill_index >= MSHR.SIZE)
            assert(0);
#endif

        uint32_t mshr_index = MSHR.next_fill_index;

        // find victim
        uint32_t set = get_set(MSHR.entry[mshr_index].address), way;
        if (cache_type == IS_LLC) {
            way = llc_find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);
        }
        else
            way = find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);

        uint8_t  do_fill = 1;

#ifdef EXCLUSIVE_CACHE
        // LLC is not filled in exclusive cache hierarchies
        if (cache_type == IS_LLC) {
            // send data/instruction to higher level cache
            if (MSHR.entry[mshr_index].fill_level < fill_level) {
                if (MSHR.entry[mshr_index].instruction) 
                    upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                else // data
                    upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
            }

            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();

            return;
        }

        // L2 evictions are "copied" back into LLC
        else if (cache_type == IS_L2C) {
            // do not writeback dirty blocks now, they will be handled later
            if (block[set][way].valid && !block[set][way].dirty) {
                if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {
                    // lower level WQ is full, cannot replace this victim
                    do_fill = 0;
                    lower_level->increment_WQ_FULL(block[set][way].address);
                    STALL[MSHR.entry[mshr_index].type]++;

                    DP ( if (warmup_complete[fill_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                    cout << " lower level wq is full!" << " fill_addr: " << hex << MSHR.entry[mshr_index].address;
                    cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                }
                else {
                    PACKET writeback_packet;

                    writeback_packet.fill_level = fill_level << 1;
                    writeback_packet.cpu = fill_cpu;
                    writeback_packet.address = block[set][way].address;
                    writeback_packet.full_addr = block[set][way].full_addr;
                    writeback_packet.data = block[set][way].data;
                    writeback_packet.instr_id = MSHR.entry[mshr_index].instr_id;
                    writeback_packet.ip = 0; // writeback does not have ip
                    //writeback_packet.ip = MSHR.entry[mshr_index].ip;
					writeback_packet.type = WRITEBACK;
                    writeback_packet.event_cycle = current_core_cycle[fill_cpu];
                    writeback_packet.dirty_block = block[set][way].dirty;

                    lower_level->add_wq(&writeback_packet);
                }
            }
        }
#endif

#ifdef LLC_BYPASS
        if ((cache_type == IS_LLC) && (way == LLC_WAY)) { // this is a bypass that does not fill the LLC
            // update replacement policy
            if (cache_type == IS_LLC) {
                llc_update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, 0, MSHR.entry[mshr_index].type, 0);

            }
            else
                update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, 0, MSHR.entry[mshr_index].type, 0);

            // COLLECT STATS
            sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
            sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

            // check fill level
            if (MSHR.entry[mshr_index].fill_level < fill_level) {

                if (MSHR.entry[mshr_index].instruction) 
                    upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                else // data
                    upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
            }

            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();

            return; // return here, no need to process further in this function
        }
#endif

        // is this dirty?
#ifdef INCLUSIVE_CACHE
        if (block[set][way].valid && higher_level_dirty(block[set][way].address)) {
#else
        if (block[set][way].valid && block[set][way].dirty) {
#endif

            // check if the lower level WQ has enough room to keep this writeback request
            if (lower_level) {
                if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {

                    // lower level WQ is full, cannot replace this victim
                    do_fill = 0;
                    lower_level->increment_WQ_FULL(block[set][way].address);
                    STALL[MSHR.entry[mshr_index].type]++;

                    DP ( if (warmup_complete[fill_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                    cout << " lower level wq is full!" << " fill_addr: " << hex << MSHR.entry[mshr_index].address;
                    cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                }
                else {
#ifdef INCLUSIVE_CACHE
                    back_invalidate(block[set][way].address);
#endif
                    PACKET writeback_packet;

                    writeback_packet.fill_level = fill_level << 1;
                    writeback_packet.cpu = fill_cpu;
                    writeback_packet.address = block[set][way].address;
                    writeback_packet.full_addr = block[set][way].full_addr;
                    writeback_packet.data = block[set][way].data;
                    writeback_packet.instr_id = MSHR.entry[mshr_index].instr_id;
                    writeback_packet.ip = 0; // writeback does not have ip
                    //writeback_packet.ip = MSHR.entry[mshr_index].ip;
					writeback_packet.type = WRITEBACK;
                    writeback_packet.event_cycle = current_core_cycle[fill_cpu];
                    writeback_packet.dirty_block = block[set][way].dirty;

                    lower_level->add_wq(&writeback_packet);
                }
            }
#ifdef SANITY_CHECK
            else {
                // sanity check
                if (cache_type != IS_STLB)
                    assert(0);
            }
#endif
        }

        if (do_fill) {
            // update prefetcher
            if (cache_type == IS_L1D)
                l1d_prefetcher_cache_fill(MSHR.entry[mshr_index].full_addr, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].full_addr);
            if  (cache_type == IS_L2C)
                l2c_prefetcher_cache_fill(MSHR.entry[mshr_index].full_addr, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].full_addr);

            // update replacement policy
            if (cache_type == IS_LLC) {
                llc_update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);

#ifdef PRINT_MLP
                // update MLP data
                if (is_leading_load_ongoing && (leading_load_address == MSHR.entry[mshr_index].full_addr)) {
                    is_leading_load_ongoing = false;
                }
#endif
            }
            else
                update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);

            // COLLECT STATS
            sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
            sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

            fill_cache(set, way, &MSHR.entry[mshr_index]);

            // RFO marks cache line dirty
            if (cache_type == IS_L1D) {
                if (MSHR.entry[mshr_index].type == RFO)
                    block[set][way].dirty = 1;
            }

            // check fill level
            if (MSHR.entry[mshr_index].fill_level < fill_level) {

                if (MSHR.entry[mshr_index].instruction) 
                    upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                else // data
                    upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
            }

            // update processed packets
            if (cache_type == IS_ITLB) { 
                MSHR.entry[mshr_index].instruction_pa = block[set][way].data;
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }
            else if (cache_type == IS_DTLB) {
                MSHR.entry[mshr_index].data_pa = block[set][way].data;
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }
            else if (cache_type == IS_L1I) {
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }
            //else if (cache_type == IS_L1D) {
            else if ((cache_type == IS_L1D) && (MSHR.entry[mshr_index].type != PREFETCH)) {
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                    PROCESSED.add_queue(&MSHR.entry[mshr_index]);
            }

            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();
        }
    }
}

void CACHE::handle_writeback()
{
    // handle write
    uint32_t writeback_cpu = WQ.entry[WQ.head].cpu;
    if (writeback_cpu == NUM_CPUS)
        return;

    // handle the oldest entry
    if ((WQ.entry[WQ.head].event_cycle <= current_core_cycle[writeback_cpu]) && (WQ.occupancy > 0)) {
        int index = WQ.head;

        // access cache
        uint32_t set = get_set(WQ.entry[index].address);
        int way = check_hit(&WQ.entry[index]);
        
        if (way >= 0) { // writeback hit (or RFO hit for L1D)

            if (cache_type == IS_LLC) {
                llc_update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);

            }
            else
                update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);

            // COLLECT STATS
            sim_hit[writeback_cpu][WQ.entry[index].type]++;
            sim_access[writeback_cpu][WQ.entry[index].type]++;

            // mark dirty
#ifdef EXCLUSIVE_CACHE
            block[set][way].dirty = WQ.entry[index].dirty_block;
#else
            block[set][way].dirty = 1;
#endif

            if (cache_type == IS_ITLB)
                WQ.entry[index].instruction_pa = block[set][way].data;
            else if (cache_type == IS_DTLB)
                WQ.entry[index].data_pa = block[set][way].data;
            else if (cache_type == IS_STLB)
                WQ.entry[index].data = block[set][way].data;

            // check fill level
            if (WQ.entry[index].fill_level < fill_level) {

                if (WQ.entry[index].instruction) 
                    upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                else // data
                    upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
            }

            HIT[WQ.entry[index].type]++;
            ACCESS[WQ.entry[index].type]++;

            // remove this entry from WQ
            WQ.remove_queue(&WQ.entry[index]);
        }
        else { // writeback miss (or RFO miss for L1D)
            
            DP ( if (warmup_complete[writeback_cpu]) {
            cout << "[" << NAME << "] " << __func__ << " type: " << +WQ.entry[index].type << " miss";
            cout << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
            cout << " full_addr: " << WQ.entry[index].full_addr << dec;
            cout << " cycle: " << WQ.entry[index].event_cycle << endl; });

            if (cache_type == IS_L1D) { // RFO miss

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&WQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss

                    // add it to mshr (RFO miss)
                    add_mshr(&WQ.entry[index]);

                    // add it to the next level's read queue
                    //if (lower_level) // L1D always has a lower level cache
                        lower_level->add_rq(&WQ.entry[index]);
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[WQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                        // update fill_level
                        if (WQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                            MSHR.entry[mshr_index].fill_level = WQ.entry[index].fill_level;

                        // update request
                        if (MSHR.entry[mshr_index].type == PREFETCH) {
                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
			    MSHR.entry[mshr_index] = WQ.entry[index];

                            // in case request is already returned, we should keep event_cycle and retunred variables
                            MSHR.entry[mshr_index].returned = prior_returned;
                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                        }

                        MSHR_MERGED[WQ.entry[index].type]++;

                        DP ( if (warmup_complete[writeback_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << WQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << WQ.entry[index].address;
                        cout << " full_addr: " << WQ.entry[index].full_addr << dec;
                        cout << " cycle: " << WQ.entry[index].event_cycle << endl; });
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {

                    MISS[WQ.entry[index].type]++;
                    ACCESS[WQ.entry[index].type]++;

                    // remove this entry from WQ
                    WQ.remove_queue(&WQ.entry[index]);
                }

            }
            else {
                // find victim
                uint32_t set = get_set(WQ.entry[index].address), way;
                if (cache_type == IS_LLC) {
                    way = llc_find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);
                }
                else
                    way = find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);

                uint8_t  do_fill = 1;

#ifdef EXCLUSIVE_CACHE
                // L2 evictions are "copied back" into LLC
                if (cache_type == IS_L2C) {
                    // do not writeback dirty blocks now, they will be handled later
                    if (block[set][way].valid && !block[set][way].dirty) {
                        if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {
                            // lower level WQ is full, cannot replace this victim
                            do_fill = 0;
                            lower_level->increment_WQ_FULL(block[set][way].address);
                            STALL[WQ.entry[index].type]++;

                            DP ( if (warmup_complete[writeback_cpu]) {
                            cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                            cout << " lower level wq is full!" << " fill_addr: " << hex << WQ.entry[index].address;
                            cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                        }
                        else { 
                            PACKET writeback_packet;

                            writeback_packet.fill_level = fill_level << 1;
                            writeback_packet.cpu = writeback_cpu;
                            writeback_packet.address = block[set][way].address;
                            writeback_packet.full_addr = block[set][way].full_addr;
                            writeback_packet.data = block[set][way].data;
                            writeback_packet.instr_id = WQ.entry[index].instr_id;
                            writeback_packet.ip = 0;
                            //writeback_packet.ip = WQ.entry[index].ip;
                            writeback_packet.type = WRITEBACK;
                            writeback_packet.event_cycle = current_core_cycle[writeback_cpu];
                            writeback_packet.dirty_block = block[set][way].dirty;

                            lower_level->add_wq(&writeback_packet);
                        }
                    }
                }
#endif
#ifdef LLC_BYPASS
                if ((cache_type == IS_LLC) && (way == LLC_WAY)) {
                    cerr << "LLC bypassing for writebacks is not allowed!" << endl;
                    assert(0);
                }
#endif

                // is this dirty?
#ifdef INCLUSIVE_CACHE
                if (block[set][way].valid && higher_level_dirty(block[set][way].address)) {
#else
                if (block[set][way].valid && block[set][way].dirty) {
#endif

                    // check if the lower level WQ has enough room to keep this writeback request
                    if (lower_level) { 
                        if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) {

                            // lower level WQ is full, cannot replace this victim
                            do_fill = 0;
                            lower_level->increment_WQ_FULL(block[set][way].address);
                            STALL[WQ.entry[index].type]++;

                            DP ( if (warmup_complete[writeback_cpu]) {
                            cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                            cout << " lower level wq is full!" << " fill_addr: " << hex << WQ.entry[index].address;
                            cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                        }
                        else {
#ifdef INCLUSIVE_CACHE
                            back_invalidate(block[set][way].address);
#endif
                            PACKET writeback_packet;

                            writeback_packet.fill_level = fill_level << 1;
                            writeback_packet.cpu = writeback_cpu;
                            writeback_packet.address = block[set][way].address;
                            writeback_packet.full_addr = block[set][way].full_addr;
                            writeback_packet.data = block[set][way].data;
                            writeback_packet.instr_id = WQ.entry[index].instr_id;
                            writeback_packet.ip = 0;
                            //writeback_packet.ip = WQ.entry[index].ip;
                            writeback_packet.type = WRITEBACK;
                            writeback_packet.event_cycle = current_core_cycle[writeback_cpu];
                            writeback_packet.dirty_block = block[set][way].dirty;

                            lower_level->add_wq(&writeback_packet);
                        }
                    }
#ifdef SANITY_CHECK
                    else {
                        // sanity check
                        if (cache_type != IS_STLB)
                            assert(0);
                    }
#endif
                }

                if (do_fill) {
                    // update prefetcher
                    if (cache_type == IS_L1D)
                        l1d_prefetcher_cache_fill(WQ.entry[index].full_addr, set, way, 0, block[set][way].full_addr);
                    else if (cache_type == IS_L2C)
                        l2c_prefetcher_cache_fill(WQ.entry[index].full_addr, set, way, 0, block[set][way].full_addr);

                    // update replacement policy
                    if (cache_type == IS_LLC) {
                        llc_update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);

                    }
                    else
                        update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);

                    // COLLECT STATS
                    sim_miss[writeback_cpu][WQ.entry[index].type]++;
                    sim_access[writeback_cpu][WQ.entry[index].type]++;

                    fill_cache(set, way, &WQ.entry[index]);

                    // mark dirty
#ifdef EXCLUSIVE_CACHE
                    block[set][way].dirty = WQ.entry[index].dirty_block;
#else
                    block[set][way].dirty = 1;
#endif

                    // check fill level
                    if (WQ.entry[index].fill_level < fill_level) {

                        if (WQ.entry[index].instruction) 
                            upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                        else // data
                            upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
                    }

                    MISS[WQ.entry[index].type]++;
                    ACCESS[WQ.entry[index].type]++;

                    // remove this entry from WQ
                    WQ.remove_queue(&WQ.entry[index]);
                }
            }
        }
    }
}

void CACHE::handle_read()
{
    // handle read
    uint32_t read_cpu = RQ.entry[RQ.head].cpu;
    if (read_cpu == NUM_CPUS)
        return;

    for (uint32_t i=0; i<MAX_READ; i++) {

        // handle the oldest entry
        if ((RQ.entry[RQ.head].event_cycle <= current_core_cycle[read_cpu]) && (RQ.occupancy > 0)) {
            int index = RQ.head;            

            // access cache
            uint32_t set = get_set(RQ.entry[index].address);
            int way = check_hit(&RQ.entry[index]);
            
            if (way >= 0) { // read hit

                if (cache_type == IS_ITLB) {
                    RQ.entry[index].instruction_pa = block[set][way].data;
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }
                else if (cache_type == IS_DTLB) {
                    RQ.entry[index].data_pa = block[set][way].data;
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }
                else if (cache_type == IS_STLB) 
                    RQ.entry[index].data = block[set][way].data;
                else if (cache_type == IS_L1I) {
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }
                //else if (cache_type == IS_L1D) {
                else if ((cache_type == IS_L1D) && (RQ.entry[index].type != PREFETCH)) {
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                        PROCESSED.add_queue(&RQ.entry[index]);
                }

                // update prefetcher on load instruction
                if (RQ.entry[index].type == LOAD) {
                    if (cache_type == IS_L1D) 
                        l1d_prefetcher_operate(block[set][way].full_addr, RQ.entry[index].ip, 1, RQ.entry[index].type);
                    else if (cache_type == IS_L2C)
                        l2c_prefetcher_operate(block[set][way].full_addr, RQ.entry[index].ip, 1, RQ.entry[index].type);

                    total_access_count++;
                    uint64_t block_address = RQ.entry[index].address;
                
#ifdef PRINT_REUSE_STATS
                    collect_reuse_distance(block_address);
#endif
#ifdef PRINT_STRIDE_DISTRIBUTION
                    collect_stride_distribution(RQ.entry[index].ip, block_address);
#endif
#ifdef PRINT_OFFSET_PATTERN
                    if (cache_type == IS_L2C) {
                        // collect offset pattern (for L2 only)
                        collect_offset_pattern(block_address);
                    }
#endif
                }

                // update replacement policy
                if (cache_type == IS_LLC) {
                    llc_update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);

                }
                else
                    update_replacement_state(read_cpu, set, way, block[set][way].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type, 1);

                // COLLECT STATS
                sim_hit[read_cpu][RQ.entry[index].type]++;
                sim_access[read_cpu][RQ.entry[index].type]++;

                // check fill level
                if (RQ.entry[index].fill_level < fill_level) {

                    if (RQ.entry[index].instruction) 
                        upper_level_icache[read_cpu]->return_data(&RQ.entry[index]);
                    else // data
                        upper_level_dcache[read_cpu]->return_data(&RQ.entry[index]);
                }

                // update prefetch stats and reset prefetch bit
                if (block[set][way].prefetch) {
                    pf_useful++;
                    block[set][way].prefetch = 0;
                }
                block[set][way].used = 1;

                HIT[RQ.entry[index].type]++;
                ACCESS[RQ.entry[index].type]++;
                
                // remove this entry from RQ
                RQ.remove_queue(&RQ.entry[index]);

#ifdef EXCLUSIVE_CACHE
                // invalidate LLC block on read hit
                if (cache_type == IS_LLC) {
                    block[set][way].valid = 0;
                }
#endif
            }
            else { // read miss

                DP ( if (warmup_complete[read_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " read miss";
                cout << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
                cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                cout << " cycle: " << RQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&RQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss
#ifdef PRINT_ACCESS_PATTERN
                    if (RQ.entry[index].type == LOAD) {
                        // update access pattern (except for L1I and ITLB)
                        if (!((cache_type == IS_L1I) || (cache_type == IS_ITLB))) {
                            access_pattern.insert(pair <uint64_t, uint64_t> (total_access_count, RQ.entry[index].address));
                        }
                    }
#endif

#ifdef PRINT_MLP
                    // collect MLP at LLC
                    if (cache_type == IS_LLC) {
                        total_loads_to_mem++;

                        if (is_leading_load_ongoing) {
                            total_parallel_loads++;
                        }
                        else {
                            is_leading_load_ongoing = true;
                            leading_load_address = RQ.entry[index].full_addr;
                            total_leading_loads++;
                        }
                    }
#endif

                    // add it to mshr (read miss)
                    add_mshr(&RQ.entry[index]);

                    // add it to the next level's read queue
                    if (lower_level)
                        lower_level->add_rq(&RQ.entry[index]);
                    else { // this is the last level
                        if (cache_type == IS_STLB) {
                            // TODO: need to differentiate page table walk and actual swap

                            // emulate page table walk
                            uint64_t pa = va_to_pa(read_cpu, RQ.entry[index].instr_id, RQ.entry[index].full_addr, RQ.entry[index].address);

                            RQ.entry[index].data = pa >> LOG2_PAGE_SIZE; 
                            RQ.entry[index].event_cycle = current_core_cycle[read_cpu];
                            return_data(&RQ.entry[index]);
                        }
                    }
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[RQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                        // mark merged consumer
                        if (RQ.entry[index].type == RFO) {

                            if (RQ.entry[index].tlb_access) {
                                uint32_t sq_index = RQ.entry[index].sq_index;
                                MSHR.entry[mshr_index].store_merged = 1;
                                MSHR.entry[mshr_index].sq_index_depend_on_me.insert (sq_index);
				MSHR.entry[mshr_index].sq_index_depend_on_me.join (RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                            }

                            if (RQ.entry[index].load_merged) {
                                //uint32_t lq_index = RQ.entry[index].lq_index; 
                                MSHR.entry[mshr_index].load_merged = 1;
                                //MSHR.entry[mshr_index].lq_index_depend_on_me[lq_index] = 1;
				MSHR.entry[mshr_index].lq_index_depend_on_me.join (RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                            }
                        }
                        else {
                            if (RQ.entry[index].instruction) {
                                uint32_t rob_index = RQ.entry[index].rob_index;
                                MSHR.entry[mshr_index].instr_merged = 1;
                                MSHR.entry[mshr_index].rob_index_depend_on_me.insert (rob_index);

                                DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                cout << " merged rob_index: " << rob_index << " instr_id: " << RQ.entry[index].instr_id << endl; });

                                if (RQ.entry[index].instr_merged) {
				    MSHR.entry[mshr_index].rob_index_depend_on_me.join (RQ.entry[index].rob_index_depend_on_me, ROB_SIZE);
                                    DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                    cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                    cout << " merged rob_index: " << i << " instr_id: N/A" << endl; });
                                }
                            }
                            else 
                            {
                                uint32_t lq_index = RQ.entry[index].lq_index;
                                MSHR.entry[mshr_index].load_merged = 1;
                                MSHR.entry[mshr_index].lq_index_depend_on_me.insert (lq_index);

                                DP (if (warmup_complete[read_cpu]) {
                                cout << "[DATA_MERGED] " << __func__ << " cpu: " << read_cpu << " instr_id: " << RQ.entry[index].instr_id;
                                cout << " merged rob_index: " << RQ.entry[index].rob_index << " instr_id: " << RQ.entry[index].instr_id << " lq_index: " << RQ.entry[index].lq_index << endl; });
				MSHR.entry[mshr_index].lq_index_depend_on_me.join (RQ.entry[index].lq_index_depend_on_me, LQ_SIZE);
                                if (RQ.entry[index].store_merged) {
                                    MSHR.entry[mshr_index].store_merged = 1;
				    MSHR.entry[mshr_index].sq_index_depend_on_me.join (RQ.entry[index].sq_index_depend_on_me, SQ_SIZE);
                                }
                            }
                        }

                        // update fill_level
                        if (RQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                            MSHR.entry[mshr_index].fill_level = RQ.entry[index].fill_level;

                        // update request
                        if (MSHR.entry[mshr_index].type == PREFETCH) {
                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
                            MSHR.entry[mshr_index] = RQ.entry[index];
                            
                            // in case request is already returned, we should keep event_cycle and retunred variables
                            MSHR.entry[mshr_index].returned = prior_returned;
                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                        }

                        MSHR_MERGED[RQ.entry[index].type]++;

                        DP ( if (warmup_complete[read_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << RQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << RQ.entry[index].address;
                        cout << " full_addr: " << RQ.entry[index].full_addr << dec;
                        cout << " cycle: " << RQ.entry[index].event_cycle << endl; });
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {
                    // update prefetcher on load instruction
                    if (RQ.entry[index].type == LOAD) {
                        if (cache_type == IS_L1D) 
                            l1d_prefetcher_operate(RQ.entry[index].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type);
                        if (cache_type == IS_L2C)
                            l2c_prefetcher_operate(RQ.entry[index].full_addr, RQ.entry[index].ip, 0, RQ.entry[index].type);

                        total_access_count++;
                        uint64_t block_address = RQ.entry[index].address;
                
#ifdef PRINT_REUSE_STATS
                        collect_reuse_distance(block_address);
#endif
#ifdef PRINT_STRIDE_DISTRIBUTION
                        collect_stride_distribution(RQ.entry[index].ip, block_address);
#endif
#ifdef PRINT_OFFSET_PATTERN
                        if (cache_type == IS_L2C) {
                            // collect offset pattern (for L2 only)
                            collect_offset_pattern(block_address);
                        }
#endif
                    }

                    MISS[RQ.entry[index].type]++;
                    ACCESS[RQ.entry[index].type]++;

                    // remove this entry from RQ
                    RQ.remove_queue(&RQ.entry[index]);
                }
            }
        }
    }
}

void CACHE::handle_prefetch()
{
    // handle prefetch
    uint32_t prefetch_cpu = PQ.entry[PQ.head].cpu;
    if (prefetch_cpu == NUM_CPUS)
        return;

    for (uint32_t i=0; i<MAX_READ; i++) {

        // handle the oldest entry
        if ((PQ.entry[PQ.head].event_cycle <= current_core_cycle[prefetch_cpu]) && (PQ.occupancy > 0)) {
            int index = PQ.head;

            // access cache
            uint32_t set = get_set(PQ.entry[index].address);
            int way = check_hit(&PQ.entry[index]);
            
            if (way >= 0) { // prefetch hit

                // update replacement policy
                if (cache_type == IS_LLC) {
                    llc_update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);

                }
                else
                    update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);

                // COLLECT STATS
                sim_hit[prefetch_cpu][PQ.entry[index].type]++;
                sim_access[prefetch_cpu][PQ.entry[index].type]++;

                // check fill level
                if (PQ.entry[index].fill_level < fill_level) {
                    if (PQ.entry[index].instruction) 
                        upper_level_icache[prefetch_cpu]->return_data(&PQ.entry[index]);
                    else // data
                        upper_level_dcache[prefetch_cpu]->return_data(&PQ.entry[index]);
                }

                HIT[PQ.entry[index].type]++;
                ACCESS[PQ.entry[index].type]++;
                
                // remove this entry from PQ
                PQ.remove_queue(&PQ.entry[index]);
            }
            else { // prefetch miss

                DP ( if (warmup_complete[prefetch_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " prefetch miss";
                cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&PQ.entry[index]);

                if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) { // this is a new miss

                    DP ( if (warmup_complete[PQ.entry[index].cpu]) {
                    cout << "[" << NAME << "_PQ] " <<  __func__ << " want to add instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
                    cout << " occupancy: " << lower_level->get_occupancy(3, PQ.entry[index].address) << " SIZE: " << lower_level->get_size(3, PQ.entry[index].address) << endl; });

                    // first check if the lower level PQ is full or not
                    // this is possible since multiple prefetchers can exist at each level of caches
                    if (lower_level) {
                        if (cache_type == IS_LLC) {
                            if (lower_level->get_occupancy(1, PQ.entry[index].address) == lower_level->get_size(1, PQ.entry[index].address))
                                miss_handled = 0;
                            else {
                                // add it to MSHRs if this prefetch miss will be filled to this cache level
                                if (PQ.entry[index].fill_level <= fill_level)
                                    add_mshr(&PQ.entry[index]);
                                lower_level->add_rq(&PQ.entry[index]); // add it to the DRAM RQ
                            }
                        }
                        else {
                            if (lower_level->get_occupancy(3, PQ.entry[index].address) == lower_level->get_size(3, PQ.entry[index].address))
                                miss_handled = 0;
                            else {
                                // add it to MSHRs if this prefetch miss will be filled to this cache level
                                if (PQ.entry[index].fill_level <= fill_level)
                                    add_mshr(&PQ.entry[index]);

                                lower_level->add_pq(&PQ.entry[index]); // add it to lower level PQ
                            }
                        }
                    }
                }
                else {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) { // not enough MSHR resource

                        // TODO: should we allow prefetching with lower fill level at this case?
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[PQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) { // already in-flight miss

                        // no need to update request except fill_level
                        // update fill_level
                        if (PQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                            MSHR.entry[mshr_index].fill_level = PQ.entry[index].fill_level;

                        MSHR_MERGED[PQ.entry[index].type]++;

                        DP ( if (warmup_complete[prefetch_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << PQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << PQ.entry[index].address;
                        cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << MSHR.entry[mshr_index].fill_level;
                        cout << " cycle: " << MSHR.entry[mshr_index].event_cycle << endl; });
                    }
                    else { // WE SHOULD NOT REACH HERE
                        cerr << "[" << NAME << "] MSHR errors" << endl;
                        assert(0);
                    }
                }

                if (miss_handled) {

                    DP ( if (warmup_complete[prefetch_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << " prefetch miss handled";
                    cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                    cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

                    MISS[PQ.entry[index].type]++;
                    ACCESS[PQ.entry[index].type]++;

                    // remove this entry from PQ
                    PQ.remove_queue(&PQ.entry[index]);
                }
            }
        }
    }
}

void CACHE::operate()
{
    handle_fill();
    handle_writeback();
    handle_read();

    if (PQ.occupancy && (RQ.occupancy == 0))
        handle_prefetch();
}

uint32_t CACHE::get_set(uint64_t address)
{
    return (uint32_t) (address & ((1 << lg2(NUM_SET)) - 1));
}

uint32_t CACHE::get_way(uint64_t address, uint32_t set)
{
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == address)) 
            return way;
    }

    return NUM_WAY;
}

void CACHE::fill_cache(uint32_t set, uint32_t way, PACKET *packet)
{
#ifdef SANITY_CHECK
    if (cache_type == IS_ITLB) {
        if (packet->data == 0)
            assert(0);
    }

    if (cache_type == IS_DTLB) {
        if (packet->data == 0)
            assert(0);
    }

    if (cache_type == IS_STLB) {
        if (packet->data == 0)
            assert(0);
    }
#endif
    if (block[set][way].valid && block[set][way].prefetch && (block[set][way].used == 0))
        pf_useless++;

    if (block[set][way].valid == 0)
        block[set][way].valid = 1;
    block[set][way].dirty = 0;
    block[set][way].prefetch = (packet->type == PREFETCH) ? 1 : 0;
    block[set][way].used = 0;

    if (block[set][way].prefetch)
        pf_fill++;

    block[set][way].delta = packet->delta;
    block[set][way].depth = packet->depth;
    block[set][way].signature = packet->signature;
    block[set][way].confidence = packet->confidence;

    block[set][way].tag = packet->address;
    block[set][way].address = packet->address;
    block[set][way].full_addr = packet->full_addr;
    block[set][way].data = packet->data;
    block[set][way].cpu = packet->cpu;
    block[set][way].instr_id = packet->instr_id;

    DP ( if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "] " << __func__ << " set: " << set << " way: " << way;
    cout << " lru: " << block[set][way].lru << " tag: " << hex << block[set][way].tag << " full_addr: " << block[set][way].full_addr;
    cout << " data: " << block[set][way].data << dec << endl; });
}

int CACHE::check_hit(PACKET *packet)
{
    uint32_t set = get_set(packet->address);
    int match_way = -1;

    if (NUM_SET < set) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
        cerr << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
        cerr << " event: " << packet->event_cycle << endl;
        assert(0);
    }

    // hit
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == packet->address)) {

            match_way = way;

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "] " << __func__ << " instr_id: " << packet->instr_id << " type: " << +packet->type << hex << " addr: " << packet->address;
            cout << " full_addr: " << packet->full_addr << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru;
            cout << " event: " << packet->event_cycle << " cycle: " << current_core_cycle[cpu] << endl; });

            break;
        }
    }

    return match_way;
}

int CACHE::invalidate_entry(uint64_t inval_addr)
{
    uint32_t set = get_set(inval_addr);
    int match_way = -1;

    if (NUM_SET < set) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
        cerr << " inval_addr: " << hex << inval_addr << dec << endl;
        assert(0);
    }

    // invalidate
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == inval_addr)) {

            block[set][way].valid = 0;

            match_way = way;

            DP ( if (warmup_complete[cpu]) {
            cout << "[" << NAME << "] " << __func__ << " inval_addr: " << hex << inval_addr;  
            cout << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru << " cycle: " << current_core_cycle[cpu] << endl; });

            break;
        }
    }

    return match_way;
}

int CACHE::add_rq(PACKET *packet)
{
    // check for the latest wirtebacks in the write queue
    int wq_index = WQ.check_queue(packet);
    if (wq_index != -1) {
        
        // check fill level
        if (packet->fill_level < fill_level) {

            packet->data = WQ.entry[wq_index].data;
            if (packet->instruction) 
                upper_level_icache[packet->cpu]->return_data(packet);
            else // data
                upper_level_dcache[packet->cpu]->return_data(packet);
        }

#ifdef SANITY_CHECK
        if (cache_type == IS_ITLB)
            assert(0);
        else if (cache_type == IS_DTLB)
            assert(0);
        else if (cache_type == IS_L1I)
            assert(0);
#endif
        // update processed packets
        if ((cache_type == IS_L1D) && (packet->type != PREFETCH)) {
            if (PROCESSED.occupancy < PROCESSED.SIZE)
                PROCESSED.add_queue(packet);

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_RQ] " << __func__ << " instr_id: " << packet->instr_id << " found recent writebacks";
            cout << hex << " read: " << packet->address << " writeback: " << WQ.entry[wq_index].address << dec;
            cout << " index: " << MAX_READ << " rob_signal: " << packet->rob_signal << endl; });
        }

        HIT[packet->type]++;
        ACCESS[packet->type]++;

        WQ.FORWARD++;
        RQ.ACCESS++;

        return -1;
    }

    // check for duplicates in the read queue
    int index = RQ.check_queue(packet);
    if (index != -1) {
        
        if (packet->instruction) {
            uint32_t rob_index = packet->rob_index;
            RQ.entry[index].rob_index_depend_on_me.insert (rob_index);
            RQ.entry[index].instr_merged = 1;

            DP (if (warmup_complete[packet->cpu]) {
            cout << "[INSTR_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << RQ.entry[index].instr_id;
            cout << " merged rob_index: " << rob_index << " instr_id: " << packet->instr_id << endl; });
        }
        else 
        {
            // mark merged consumer
            if (packet->type == RFO) {

                uint32_t sq_index = packet->sq_index;
                RQ.entry[index].sq_index_depend_on_me.insert (sq_index);
                RQ.entry[index].store_merged = 1;
            }
            else {
                uint32_t lq_index = packet->lq_index; 
                RQ.entry[index].lq_index_depend_on_me.insert (lq_index);
                RQ.entry[index].load_merged = 1;

                DP (if (warmup_complete[packet->cpu]) {
                cout << "[DATA_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << RQ.entry[index].instr_id;
                cout << " merged rob_index: " << packet->rob_index << " instr_id: " << packet->instr_id << " lq_index: " << packet->lq_index << endl; });
            }
        }

        RQ.MERGED++;
        RQ.ACCESS++;

        return index; // merged index
    }

    // check occupancy
    if (RQ.occupancy == RQ_SIZE) {
        RQ.FULL++;

        return -2; // cannot handle this request
    }

    // if there is no duplicate, add it to RQ
    index = RQ.tail;

#ifdef SANITY_CHECK
    if (RQ.entry[index].address != 0) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
        cerr << " address: " << hex << RQ.entry[index].address;
        cerr << " full_addr: " << RQ.entry[index].full_addr << dec << endl;
        assert(0);
    }
#endif

    RQ.entry[index] = *packet;

    // ADD LATENCY
    if (RQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
        RQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        RQ.entry[index].event_cycle += LATENCY;

    RQ.occupancy++;
    RQ.tail++;
    if (RQ.tail >= RQ.SIZE)
        RQ.tail = 0;

    DP ( if (warmup_complete[RQ.entry[index].cpu]) {
    cout << "[" << NAME << "_RQ] " <<  __func__ << " instr_id: " << RQ.entry[index].instr_id << " address: " << hex << RQ.entry[index].address;
    cout << " full_addr: " << RQ.entry[index].full_addr << dec;
    cout << " type: " << +RQ.entry[index].type << " head: " << RQ.head << " tail: " << RQ.tail << " occupancy: " << RQ.occupancy;
    cout << " event: " << RQ.entry[index].event_cycle << " current: " << current_core_cycle[RQ.entry[index].cpu] << endl; });

    if (packet->address == 0)
        assert(0);

    RQ.TO_CACHE++;
    RQ.ACCESS++;

    return -1;
}

int CACHE::add_wq(PACKET *packet)
{
    // check for duplicates in the write queue
    int index = WQ.check_queue(packet);
    if (index != -1) {

        WQ.MERGED++;
        WQ.ACCESS++;

        return index; // merged index
    }

    // sanity check
    if (WQ.occupancy >= WQ.SIZE)
        assert(0);

    // if there is no duplicate, add it to the write queue
    index = WQ.tail;
    if (WQ.entry[index].address != 0) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
        cerr << " address: " << hex << WQ.entry[index].address;
        cerr << " full_addr: " << WQ.entry[index].full_addr << dec << endl;
        assert(0);
    }

    WQ.entry[index] = *packet;

    // ADD LATENCY
    if (WQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
        WQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        WQ.entry[index].event_cycle += LATENCY;

    WQ.occupancy++;
    WQ.tail++;
    if (WQ.tail >= WQ.SIZE)
        WQ.tail = 0;

    DP (if (warmup_complete[WQ.entry[index].cpu]) {
    cout << "[" << NAME << "_WQ] " <<  __func__ << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
    cout << " full_addr: " << WQ.entry[index].full_addr << dec;
    cout << " head: " << WQ.head << " tail: " << WQ.tail << " occupancy: " << WQ.occupancy;
    cout << " data: " << hex << WQ.entry[index].data << dec;
    cout << " event: " << WQ.entry[index].event_cycle << " current: " << current_core_cycle[WQ.entry[index].cpu] << endl; });

    WQ.TO_CACHE++;
    WQ.ACCESS++;

    return -1;
}

int CACHE::prefetch_line(uint64_t ip, uint64_t base_addr, uint64_t pf_addr, int fill_level)
{
    pf_requested++;

    if (PQ.occupancy < PQ.SIZE) {
        if ((base_addr>>LOG2_PAGE_SIZE) == (pf_addr>>LOG2_PAGE_SIZE)) {
            
            PACKET pf_packet;
            pf_packet.fill_level = fill_level;
            pf_packet.cpu = cpu;
            //pf_packet.data_index = LQ.entry[lq_index].data_index;
            //pf_packet.lq_index = lq_index;
            pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
            pf_packet.full_addr = pf_addr;
            //pf_packet.instr_id = LQ.entry[lq_index].instr_id;
            //pf_packet.rob_index = LQ.entry[lq_index].rob_index;
            pf_packet.ip = ip;
            pf_packet.type = PREFETCH;
            pf_packet.event_cycle = current_core_cycle[cpu];

            // give a dummy 0 as the IP of a prefetch
            add_pq(&pf_packet);

            pf_issued++;

            return 1;
        }
    }

    return 0;
}

int CACHE::kpc_prefetch_line(uint64_t base_addr, uint64_t pf_addr, int fill_level, int delta, int depth, int signature, int confidence)
{
    if (PQ.occupancy < PQ.SIZE) {
        if ((base_addr>>LOG2_PAGE_SIZE) == (pf_addr>>LOG2_PAGE_SIZE)) {
            
            PACKET pf_packet;
            pf_packet.fill_level = fill_level;
            pf_packet.cpu = cpu;
            //pf_packet.data_index = LQ.entry[lq_index].data_index;
            //pf_packet.lq_index = lq_index;
            pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
            pf_packet.full_addr = pf_addr;
            //pf_packet.instr_id = LQ.entry[lq_index].instr_id;
            //pf_packet.rob_index = LQ.entry[lq_index].rob_index;
            pf_packet.ip = 0;
            pf_packet.type = PREFETCH;
            pf_packet.delta = delta;
            pf_packet.depth = depth;
            pf_packet.signature = signature;
            pf_packet.confidence = confidence;
            pf_packet.event_cycle = current_core_cycle[cpu];

            // give a dummy 0 as the IP of a prefetch
            add_pq(&pf_packet);

            pf_issued++;

            return 1;
        }
    }

    return 0;
}

int CACHE::add_pq(PACKET *packet)
{
    // check for the latest wirtebacks in the write queue
    int wq_index = WQ.check_queue(packet);
    if (wq_index != -1) {
        
        // check fill level
        if (packet->fill_level < fill_level) {

            packet->data = WQ.entry[wq_index].data;
            if (packet->instruction) 
                upper_level_icache[packet->cpu]->return_data(packet);
            else // data
                upper_level_dcache[packet->cpu]->return_data(packet);
        }

        HIT[packet->type]++;
        ACCESS[packet->type]++;

        WQ.FORWARD++;
        PQ.ACCESS++;

        return -1;
    }

    // check for duplicates in the PQ
    int index = PQ.check_queue(packet);
    if (index != -1) {
        if (packet->fill_level < PQ.entry[index].fill_level)
            PQ.entry[index].fill_level = packet->fill_level;

        PQ.MERGED++;
        PQ.ACCESS++;

        return index; // merged index
    }

    // check occupancy
    if (PQ.occupancy == PQ_SIZE) {
        PQ.FULL++;

        DP ( if (warmup_complete[packet->cpu]) {
        cout << "[" << NAME << "] cannot process add_pq since it is full" << endl; });
        return -2; // cannot handle this request
    }

    // if there is no duplicate, add it to PQ
    index = PQ.tail;

#ifdef SANITY_CHECK
    if (PQ.entry[index].address != 0) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
        cerr << " address: " << hex << PQ.entry[index].address;
        cerr << " full_addr: " << PQ.entry[index].full_addr << dec << endl;
        assert(0);
    }
#endif

    PQ.entry[index] = *packet;

    // ADD LATENCY
    if (PQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
        PQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        PQ.entry[index].event_cycle += LATENCY;

    PQ.occupancy++;
    PQ.tail++;
    if (PQ.tail >= PQ.SIZE)
        PQ.tail = 0;

    DP ( if (warmup_complete[PQ.entry[index].cpu]) {
    cout << "[" << NAME << "_PQ] " <<  __func__ << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
    cout << " type: " << +PQ.entry[index].type << " head: " << PQ.head << " tail: " << PQ.tail << " occupancy: " << PQ.occupancy;
    cout << " event: " << PQ.entry[index].event_cycle << " current: " << current_core_cycle[PQ.entry[index].cpu] << endl; });

    if (packet->address == 0)
        assert(0);

    PQ.TO_CACHE++;
    PQ.ACCESS++;

    return -1;
}

void CACHE::return_data(PACKET *packet)
{
    // check MSHR information
    int mshr_index = check_mshr(packet);

    // sanity check
    if (mshr_index == -1) {
        cerr << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id << " cannot find a matching entry!";
        cerr << " full_addr: " << hex << packet->full_addr;
        cerr << " address: " << packet->address << dec;
        cerr << " event: " << packet->event_cycle << " current: " << current_core_cycle[packet->cpu] << endl;
        assert(0);
    }

    // MSHR holds the most updated information about this request
    // no need to do memcpy
    MSHR.num_returned++;
    MSHR.entry[mshr_index].returned = COMPLETED;
    MSHR.entry[mshr_index].data = packet->data;

    // ADD LATENCY
    if (MSHR.entry[mshr_index].event_cycle < current_core_cycle[packet->cpu])
        MSHR.entry[mshr_index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        MSHR.entry[mshr_index].event_cycle += LATENCY;

    update_fill_cycle();

    DP (if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[mshr_index].instr_id;
    cout << " address: " << hex << MSHR.entry[mshr_index].address << " full_addr: " << MSHR.entry[mshr_index].full_addr;
    cout << " data: " << MSHR.entry[mshr_index].data << dec << " num_returned: " << MSHR.num_returned;
    cout << " index: " << mshr_index << " occupancy: " << MSHR.occupancy;
    cout << " event: " << MSHR.entry[mshr_index].event_cycle << " current: " << current_core_cycle[packet->cpu] << " next: " << MSHR.next_fill_cycle << endl; });
}

void CACHE::update_fill_cycle()
{
    // update next_fill_cycle
    uint64_t min_cycle = UINT64_MAX;
    uint32_t min_index = MSHR.SIZE;
    for (uint32_t i=0; i<MSHR.SIZE; i++) {
        if ((MSHR.entry[i].returned == COMPLETED) && (MSHR.entry[i].event_cycle < min_cycle)) {
            min_cycle = MSHR.entry[i].event_cycle;
            min_index = i;
        }

        DP (if (warmup_complete[MSHR.entry[i].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " checking instr_id: " << MSHR.entry[i].instr_id;
        cout << " address: " << hex << MSHR.entry[i].address << " full_addr: " << MSHR.entry[i].full_addr;
        cout << " data: " << MSHR.entry[i].data << dec << " returned: " << +MSHR.entry[i].returned << " fill_level: " << MSHR.entry[i].fill_level;
        cout << " index: " << i << " occupancy: " << MSHR.occupancy;
        cout << " event: " << MSHR.entry[i].event_cycle << " current: " << current_core_cycle[MSHR.entry[i].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
    }
    
    MSHR.next_fill_cycle = min_cycle;
    MSHR.next_fill_index = min_index;
    if (min_index < MSHR.SIZE) {

        DP (if (warmup_complete[MSHR.entry[min_index].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[min_index].instr_id;
        cout << " address: " << hex << MSHR.entry[min_index].address << " full_addr: " << MSHR.entry[min_index].full_addr;
        cout << " data: " << MSHR.entry[min_index].data << dec << " num_returned: " << MSHR.num_returned;
        cout << " event: " << MSHR.entry[min_index].event_cycle << " current: " << current_core_cycle[MSHR.entry[min_index].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
    }
}

int CACHE::check_mshr(PACKET *packet)
{
    // search mshr
    for (uint32_t index=0; index<MSHR_SIZE; index++) {
        if (MSHR.entry[index].address == packet->address) {
            
            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_MSHR] " << __func__ << " same entry instr_id: " << packet->instr_id << " prior_id: " << MSHR.entry[index].instr_id;
            cout << " address: " << hex << packet->address;
            cout << " full_addr: " << packet->full_addr << dec << endl; });

            return index;
        }
    }

    DP ( if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " << __func__ << " new address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec << endl; });

    DP ( if (warmup_complete[packet->cpu] && (MSHR.occupancy == MSHR_SIZE)) { 
    cout << "[" << NAME << "_MSHR] " << __func__ << " mshr is full";
    cout << " instr_id: " << packet->instr_id << " mshr occupancy: " << MSHR.occupancy;
    cout << " address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec;
    cout << " cycle: " << current_core_cycle[packet->cpu] << endl; });

    return -1;
}

void CACHE::add_mshr(PACKET *packet)
{
    uint32_t index = 0;

    // search mshr
    for (index=0; index<MSHR_SIZE; index++) {
        if (MSHR.entry[index].address == 0) {
            
            MSHR.entry[index] = *packet;
            MSHR.entry[index].returned = INFLIGHT;
            MSHR.occupancy++;

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id;
            cout << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
            cout << " index: " << index << " occupancy: " << MSHR.occupancy << endl; });

            break;
        }
    }
}

uint32_t CACHE::get_occupancy(uint8_t queue_type, uint64_t address)
{
    if (queue_type == 0)
        return MSHR.occupancy;
    else if (queue_type == 1)
        return RQ.occupancy;
    else if (queue_type == 2)
        return WQ.occupancy;
    else if (queue_type == 3)
        return PQ.occupancy;

    return 0;
}

uint32_t CACHE::get_size(uint8_t queue_type, uint64_t address)
{
    if (queue_type == 0)
        return MSHR.SIZE;
    else if (queue_type == 1)
        return RQ.SIZE;
    else if (queue_type == 2)
        return WQ.SIZE;
    else if (queue_type == 3)
        return PQ.SIZE;

    return 0;
}

void CACHE::increment_WQ_FULL(uint64_t address)
{
    WQ.FULL++;
}

void CACHE::back_invalidate(uint64_t address)
{
    uint8_t upper_level_dirty = 0;
    uint64_t data = 0, full_addr = 0;
    int data_cache = FILL_DRAM;

    // get set and way
    uint32_t set = get_set(address), way;
    for (way = 0; way < NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == address)) {
            break;
        }
    }

    if (cache_type == IS_LLC) {
        // invalidate the block in all higher cache levels of all CPUs and fetch the latest data
        for (int cpu = 0; cpu < NUM_CPUS; cpu++) {
            upper_level_dirty = ((CACHE *)upper_level_icache[cpu])->invalidate_and_return_data(cpu, address, &data, &data_cache);
            
            if (upper_level_dcache[cpu] != upper_level_icache[cpu]) {
                upper_level_dirty = ((CACHE *)upper_level_dcache[cpu])->invalidate_and_return_data(cpu, address, &data, &data_cache);
            }

            // update cache if block is dirty
            if (upper_level_dirty) {
                block[set][way].dirty = 1;
                block[set][way].data = data;
            }
        }
    }

    else if (cache_type == IS_L2C) {
        // invalidate the block in all higher cache levels and fetch the latest data
        upper_level_dirty = ((CACHE *)upper_level_icache[cpu])->invalidate_and_return_data(cpu, address, &data, &data_cache);
            
        if (upper_level_dcache[cpu] != upper_level_icache[cpu]) {
            upper_level_dirty = ((CACHE *)upper_level_dcache[cpu])->invalidate_and_return_data(cpu, address, &data, &data_cache);
        }

        // update cache if block is dirty
        if (upper_level_dirty) {
            block[set][way].dirty = 1;
            block[set][way].data = data;
        }
    }
}

uint8_t CACHE::invalidate_and_return_data(uint32_t cpu, uint64_t address, uint64_t *data, int *data_cache)
{
    uint8_t upper_level_dirty = 0;

    if (upper_level_icache[cpu]) {
        upper_level_dirty = ((CACHE *)upper_level_icache[cpu])->invalidate_and_return_data(cpu, address, data, data_cache);

        if (upper_level_dcache[cpu] != upper_level_icache[cpu]) {
            upper_level_dirty = ((CACHE *)upper_level_dcache[cpu])->invalidate_and_return_data(cpu, address, data, data_cache);
        }
    }

    uint8_t dirty = 0;
    uint32_t set = get_set(address), way;

    // invalidate the block and record its data and fill level if it is dirty
    for (way = 0; way < NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == address)) {
            block[set][way].valid = 0;

            if (block[set][way].dirty && !upper_level_dirty && (fill_level <= *data_cache)) {
                dirty = 1;
                *data = block[set][way].data;
                *data_cache = fill_level;
            }

            break;
        }
    }

    return (dirty || upper_level_dirty);
}

uint8_t CACHE::higher_level_dirty(uint64_t address)
{
    if (cache_type == IS_LLC) {
        uint8_t upper_level_dirty = 0;

        for (int cpu = 0; cpu < NUM_CPUS; cpu++) {
            // icache is never dirty, so check dcache only
            upper_level_dirty = ((CACHE *)upper_level_dcache[cpu])->higher_level_dirty(address);

            if (upper_level_dirty) {
                return 1;
            }
        }

        uint32_t set = get_set(address), way;

        // check if the block is dirty
        for (way = 0; way < NUM_WAY; way++) {
            if (block[set][way].valid && (block[set][way].tag == address)) {
                return block[set][way].dirty;
            }
        }
    }

    else {
        uint8_t upper_level_dirty = 0;

        // icache is never dirty, so check dcache only
        if (upper_level_dcache[cpu]) {
            upper_level_dirty = ((CACHE *)upper_level_dcache[cpu])->higher_level_dirty(address);
        }

        if (upper_level_dirty) {
            return 1;
        }

        uint32_t set = get_set(address), way;

        // check if the block is dirty
        for (way = 0; way < NUM_WAY; way++) {
            if (block[set][way].valid && (block[set][way].tag == address)) {
                return block[set][way].dirty;
            }
        }
    }

    return 0;
}

uint8_t CACHE::get_reuse_distance_bin(uint16_t reuse_distance)
{
    if (reuse_distance <= 512) {
        return 0;
    }
    else if (reuse_distance <= 1024) {
        return 1;
    }
    else if (reuse_distance <= 2048) {
        return 2;
    }
    else if (reuse_distance <= 4096) {
        return 3;
    }
    else if (reuse_distance <= 8192) {
        return 4;
    }
    else if (reuse_distance <= 16384) {
        return 5;
    }
    else if (reuse_distance <= 32768) {
        return 6;
    }
    else if (reuse_distance <= 65536) {
        return 7;
    }
    else if (reuse_distance <= 131072) {
        return 8;
    }
    else if (reuse_distance <= 242144) {
        return 9;
    }

    return 10;
}

uint8_t CACHE::get_stride_bin(uint64_t stride)
{
    if (stride == 0) {
        return 0;
    }
    else if (stride == 1) {
        return 1;
    }
    else if (stride < 4) {
        return 2;
    }
    else if (stride < 32) {
        return 3;
    }
    else if (stride < 128) {
        return 4;
    }
    else if (stride < 1024) {
        return 5;
    }
    else if (stride < 4096) {
        return 6;
    }

    return 7;
}

void CACHE::collect_reuse_distance(uint64_t block_address)
{
    std::vector<uint64_t>::iterator it;
    for (it = recent_accesses.begin(); it != recent_accesses.end(); it++) {
        if (*it == block_address) {
            break;
        }
    }

    if (it != recent_accesses.end()) {
        uint16_t reuse_distance = std::distance(recent_accesses.begin(), it) + 1;
        reuse_distance_bins[get_reuse_distance_bin(reuse_distance)]++;
        recent_accesses.erase(it);
    }
    else {
        if (num_of_unique_references == 262144) {
            recent_accesses.erase(recent_accesses.end() - 1);
            reuse_distance_bins[num_of_reuse_distance_bins - 1]++;
        }
        else {
            num_of_unique_references++;
        }
    }

    recent_accesses.insert(recent_accesses.begin(), block_address);
}

void CACHE::collect_stride_distribution(uint64_t ip, uint64_t block_address)
{
    uint8_t last_bin_index = num_of_stride_distribution_bins - 1;

    // update stats for local stride distribution
    map<uint64_t, uint64_t>::iterator lla_it = last_local_address.find(ip);
    
    if (lla_it != last_local_address.end()) {
        // update stride history and its index
        uint64_t stride;
        if (block_address > lla_it->second) {
            stride = block_address - lla_it->second;
        } else {
            stride = lla_it->second - block_address;
        }
        local_stride_history[ip][local_stride_history_index[ip]] = stride;
        local_stride_history_index[ip] = (local_stride_history_index[ip] == (stride_history_length - 1)) ? 0 : local_stride_history_index[ip] + 1;
        num_local_strides[ip]++;

        // update bins
        if (num_local_strides[ip] == stride_history_length) {
            uint8_t stride_bin_index = last_bin_index;

            if ((local_stride_history[ip][0] == local_stride_history[ip][1]) && \
                (local_stride_history[ip][1] == local_stride_history[ip][2])) {
                stride_bin_index = get_stride_bin(stride);
            }

            local_stride_distribution[stride_bin_index]++;
            num_local_strides[ip]--;    // to ensure this value stays == stride_history_length
        }
    }
    else {
        // first encounter with this address => initialize related structures
        num_local_strides[ip] = 0;
        local_stride_history[ip] = new uint64_t[stride_history_length];
        local_stride_history_index[ip] = 0;
    }
    last_local_address[ip] = block_address;

    // update stats for global stride distribution
    if (total_access_count > 1) {
        // update stride history and its index
        uint64_t stride;
        if (block_address > last_global_address) {
            stride = block_address - last_global_address;
        } else {
            stride = last_global_address - block_address;
        }
        global_stride_history[global_stride_history_index] = stride;
        global_stride_history_index = (global_stride_history_index == (stride_history_length - 1)) ? 0 : global_stride_history_index + 1;
        num_global_strides++;

        // update bins
        if (num_global_strides == stride_history_length) {
            uint8_t stride_bin_index = last_bin_index;

            if ((global_stride_history[0] == global_stride_history[1]) && \
                (global_stride_history[1] == global_stride_history[2])) {
                stride_bin_index = get_stride_bin(stride);
            }

            global_stride_distribution[stride_bin_index]++;
            num_global_strides--;    // to ensure this value stays == stride_history_length
        }
    }
    last_global_address = block_address;
}

void CACHE::collect_offset_pattern(uint64_t block_address)
{
    if (is_first_access) {
        last_address = block_address;
        is_first_access = false;
    }
    else {
        offset_pattern.push_back(block_address - last_address);
        last_address = block_address;
    }
}

void CACHE::check_inclusive()
{
    cout << "check_inclusive" << endl;
    for (int i = 0; i < NUM_CPUS; i++)
    {
        //cout<<"1";
        //L1I data should be present L2C
        for (int l1iset = 0; l1iset < L1I_SET; l1iset++)
            for (int l1iway = 0; l1iway < L1I_WAY; l1iway++)
                if (ooo_cpu[i].L1I.block[l1iset][l1iway].valid == 1)
                {
                    //cout<<"2";
                    int match = 0;
                    for (int l2cset = 0; l2cset < L2C_SET; l2cset++)
                        for (int l2cway = 0; l2cway < L2C_WAY; l2cway++)
                        {
                            if (ooo_cpu[i].L2C.block[l2cset][l2cway].tag == ooo_cpu[i].L1I.block[l1iset][l1iway].tag &&
                                ooo_cpu[i].L2C.block[l2cset][l2cway].full_addr == ooo_cpu[i].L1I.block[l1iset][l1iway].full_addr &&
                                //ooo_cpu[i].L2C.block[l2cset][l2cway].data == ooo_cpu[i].L1I.block[l1iset][l1iway].data &&
                                ooo_cpu[i].L2C.block[l2cset][l2cway].valid == 1)
                            {
                                match = 1;
                            }
                            //cout<<"3";
                        }
                    if (!match)
                    {
                        cout << "1 FAILED" << endl;
                    }
                }
        //L1D data should be present L2C
        for (int l1dset = 0; l1dset < L1D_SET; l1dset++)
            for (int l1dway = 0; l1dway < L1D_WAY; l1dway++)
                if (ooo_cpu[i].L1D.block[l1dset][l1dway].valid == 1)
                {
                    //cout<<"4";
                    int match = 0;
                    for (int l2cset = 0; l2cset < L2C_SET; l2cset++)
                        for (int l2cway = 0; l2cway < L2C_WAY; l2cway++)
                        {
                            if (ooo_cpu[i].L2C.block[l2cset][l2cway].tag == ooo_cpu[i].L1D.block[l1dset][l1dway].tag &&
                                ooo_cpu[i].L2C.block[l2cset][l2cway].full_addr == ooo_cpu[i].L1D.block[l1dset][l1dway].full_addr &&
                                //ooo_cpu[i].L2C.block[l2cset][l2cway].data == ooo_cpu[i].L1D.block[l1dset][l1dway].data &&
                                ooo_cpu[i].L2C.block[l2cset][l2cway].valid == 1)
                                match = 1;
                            //cout<<"5";
                        }
                    if (!match)
                    {
                        cout << "2 FAILED" << endl;
                    }
                }
        //L2C data should be present in LLC
        for (int l2cset = 0; l2cset < L2C_SET; l2cset++)
            for (int l2cway = 0; l2cway < L2C_WAY; l2cway++)
                if (ooo_cpu[i].L2C.block[l2cset][l2cway].valid == 1)
                {
                    //cout<<"5";
                    int match = 0;
                    for (int llcset = 0; llcset < LLC_SET; llcset++)
                        for (int llcway = 0; llcway < LLC_WAY; llcway++)
                            if (ooo_cpu[i].L2C.block[l2cset][l2cway].tag == uncore.LLC.block[llcset][llcway].tag &&
                                ooo_cpu[i].L2C.block[l2cset][l2cway].full_addr == uncore.LLC.block[llcset][llcway].full_addr &&
                                //ooo_cpu[i].L2C.block[l2cset][l2cway].data == uncore.LLC.block[llcset][llcway].data &&
                                uncore.LLC.block[llcset][llcway].valid == 1)
                            {
                                match = 1;
                                //		cout<<"6";
                            }
                    if (!match)
                    {
                        cout << "3 FAILED" << endl;
                    }
                }
    }
}
