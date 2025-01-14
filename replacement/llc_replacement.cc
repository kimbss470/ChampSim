//Glider Replacement Algorithm//

#include "cache.h"
#include <map>
#include <iostream>
#include <fstream>
#include <string>

using namespace std;


#define NUM_CORE 1
#define LLC_SETS NUM_CORE*2048
#define LLC_WAYS 16

//3-bit RRIP counters or all lines
#define maxRRPV 7
uint32_t rrpv[LLC_SETS][LLC_WAYS];


#define TIMER_SIZE 1024
uint64_t perset_mytimer[LLC_SETS];

uint64_t signatures[LLC_SETS][LLC_WAYS];
#include "glider_predictor_ver2.h"
GLIDER_PC_PREDICTOR* glider_predictor;  

#define OPTGEN_VECTOR_SIZE 128
#include "optgen.h"
OPTgen perset_optgen[LLC_SETS]; // per-set occupancy vectors; we only use 64 of these

#include <math.h>
#define bitmask(l) (((l) == 64) ? (unsigned long long)(-1LL) : ((1LL << (l))-1LL))
#define bits(x, i, l) (((x) >> (i)) & bitmask(l))
//Sample 64 sets per core
#define SAMPLED_SET(set) (bits(set, 0 , 6) == bits(set, ((unsigned long long)log2(LLC_SETS) - 6), 6) )

// Sampler to track 8x cache history for sampled sets
#define SAMPLED_CACHE_SIZE 2800
#define SAMPLER_WAYS 8
#define SAMPLER_SETS SAMPLED_CACHE_SIZE/SAMPLER_WAYS		
vector<map<uint64_t, ADDR_INFO> > addr_history; // Sampler

// sacusa: structures for additional data
uint64_t num_of_evictions;
uint64_t num_of_cache_friendly_evictions;

// initialize replacement state
void CACHE::llc_initialize_replacement()
{
    cout << "---------------------this is glider------------------------" << endl;
	for (int i=0; i<LLC_SETS; i++) {
        for (int j=0; j<LLC_WAYS; j++) {
            rrpv[i][j] = maxRRPV;
			signatures[i][j] = 0;
        }
        perset_mytimer[i] = 0;
        perset_optgen[i].init(LLC_WAYS);	//LLC_WAYS - 2 --> LLC_WAYS    //OPTgen의 CACHE_SIZE를 LLC_WAYS로 init. CACHE_SIZE는 occup. vector each entry의 limit
    }

    addr_history.resize(SAMPLER_SETS);
    for (int i=0; i<SAMPLER_SETS; i++) 
        addr_history[i].clear();

    glider_predictor = new GLIDER_PC_PREDICTOR();

    cout << "Initialize glider state" << endl;

    num_of_evictions = 0;
    num_of_cache_friendly_evictions = 0;
}

// find replacement victim
// return value should be 0 ~ 15 or 16 (bypass)
uint32_t CACHE::llc_find_victim (uint32_t cpu, uint64_t instr_id, uint32_t set, const BLOCK *current_set, uint64_t PC, uint64_t paddr, uint32_t type)
{
    // sacusa: data to compute percentage of cache friendly evictions
    num_of_evictions++;

    // look for the maxRRPV line
    for (uint32_t i=0; i<LLC_WAYS; i++)
        if (rrpv[set][i] == maxRRPV)
            return i;

    //If we cannot find a cache-averse line, we evict the oldest cache-friendly line
    num_of_cache_friendly_evictions++;
    uint32_t max_rrip = 0;
    int32_t lru_victim = -1;
    for (uint32_t i=0; i<LLC_WAYS; i++)
    {
        if (rrpv[set][i] >= max_rrip)
        {
            max_rrip = rrpv[set][i];
            lru_victim = i;
        }
    }

    assert (lru_victim != -1);
    //The glider_predictor is trained negatively on LRU evictions
    if( SAMPLED_SET(set) )												//???????????????????????
    {
        glider_predictor->decrement(signatures[set][lru_victim]);
    }
    return lru_victim;

    // WE SHOULD NOT REACH HERE
    assert(0);
    return 0;
}

void replace_addr_history_element(unsigned int sampler_set)
{
    uint64_t lru_addr = 0;
    
    for(map<uint64_t, ADDR_INFO>::iterator it=addr_history[sampler_set].begin(); it != addr_history[sampler_set].end(); it++)
    {

        if((it->second).lru == (SAMPLER_WAYS-1))
        {
            lru_addr = it->first;
            break;
        }
    }

    addr_history[sampler_set].erase(lru_addr);
}

void update_addr_history_lru(unsigned int sampler_set, unsigned int curr_lru)
{
    for(map<uint64_t, ADDR_INFO>::iterator it=addr_history[sampler_set].begin(); it != addr_history[sampler_set].end(); it++)
    {
        if((it->second).lru < curr_lru)
        {
            (it->second).lru++;
            assert((it->second).lru < SAMPLER_WAYS); 
        }
    }
}


// called on every cache hit and cache fill
void CACHE::llc_update_replacement_state (uint32_t cpu, uint32_t set, uint32_t way, uint64_t paddr, uint64_t PC, uint64_t victim_addr, uint32_t type, uint8_t hit)
{
    paddr = (paddr >> 6) << 6;
	
	if(type == WRITEBACK) 
		return;


    //If we are sampling, OPTgen will only see accesses from sampled sets
    if(SAMPLED_SET(set))
    {
        //The current timestep 
        uint64_t curr_quanta = perset_mytimer[set] % OPTGEN_VECTOR_SIZE;

        uint32_t sampler_set = (paddr >> 6) % SAMPLER_SETS; 
        uint64_t sampler_tag = CRC(paddr >> 12) % 256;
        assert(sampler_set < SAMPLER_SETS);

        // This line has been used before. Since the right end of a usage interval is always 
        //a demand, ignore prefetches
        if((addr_history[sampler_set].find(sampler_tag) != addr_history[sampler_set].end()) && (type != PREFETCH))
        {
            unsigned int curr_timer = perset_mytimer[set];
            if(curr_timer < addr_history[sampler_set][sampler_tag].last_quanta)
               curr_timer = curr_timer + TIMER_SIZE;
            bool wrap =  ((curr_timer - addr_history[sampler_set][sampler_tag].last_quanta) > OPTGEN_VECTOR_SIZE);
            uint64_t last_quanta = addr_history[sampler_set][sampler_tag].last_quanta % OPTGEN_VECTOR_SIZE;
            
			if( !wrap && perset_optgen[set].should_cache(curr_quanta, last_quanta))
            {
                glider_predictor->increment(PC);
            }
            else
            {
                glider_predictor->decrement(PC);
            }
            //Some maintenance operations for OPTgen
            perset_optgen[set].add_access(curr_quanta);
            update_addr_history_lru(sampler_set, addr_history[sampler_set][sampler_tag].lru);

            //Since this was a demand access, mark the prefetched bit as false
            addr_history[sampler_set][sampler_tag].prefetched = false;
        }
        // This is the first time we are seeing this line (could be demand or prefetch)
        else if(addr_history[sampler_set].find(sampler_tag) == addr_history[sampler_set].end())
        {
            // Find a victim from the sampled cache if we are sampling
            if(addr_history[sampler_set].size() == SAMPLER_WAYS) 
                replace_addr_history_element(sampler_set);	//addr_history[sampler_set]중에서 lru가 MAX(SAMPLER_WAYS - 1)인 놈을 erase

            assert(addr_history[sampler_set].size() < SAMPLER_WAYS);
            //Initialize a new entry in the sampler
            addr_history[sampler_set][sampler_tag].init(0);
            
			perset_optgen[set].add_access(curr_quanta);
            update_addr_history_lru(sampler_set, SAMPLER_WAYS-1);
        }
        
        
		// Update the sampler with the timestamp, PC and our prediction
        addr_history[sampler_set][sampler_tag].update(perset_mytimer[set], PC, false);		
        addr_history[sampler_set][sampler_tag].lru = 0;
        //Increment the set timer
        perset_mytimer[set] = (perset_mytimer[set]+1) % TIMER_SIZE;
    }

    int new_prediction = glider_predictor->get_prediction (PC);
	
    signatures[set][way] = PC;	
    //Set RRPV
 	switch (new_prediction) {
		
		case CACHE_AVERSE:
			rrpv[set][way] = maxRRPV;
			break;

		case CACHE_FRIENDLY:
			rrpv[set][way] = 0;
			if(!hit){
				bool saturated = false;
				for(uint32_t i=0; i<LLC_WAYS; i++){
					if (rrpv[set][i] == maxRRPV-1)
						saturated = true;
				}
				
				for(uint32_t i=0; i<LLC_WAYS; i++){
					if (!saturated && rrpv[set][i] < maxRRPV-1)
						rrpv[set][i]++;
				}


			}
			rrpv[set][way] = 0;
			break;
		
		case CACHE_FRIENDLY_LOW:
			rrpv[set][way] = 2;
			break;	
		
		default:
			break;
	}
}

// use this function to print out your own stats at the end of simulation
void CACHE::llc_replacement_final_stats()
{
    unsigned int hits = 0;
    unsigned int demand_accesses = 0;
    unsigned int prefetch_accesses = 0;
    for(unsigned int i=0; i<LLC_SETS; i++)
    {
        demand_accesses += perset_optgen[i].demand_access;
        prefetch_accesses += perset_optgen[i].prefetch_access;
        hits += perset_optgen[i].get_num_opt_hits();
    }

    std::cout << "OPTgen demand accesses: " << demand_accesses << std::endl;
    std::cout << "OPTgen prefetch accesses: " << prefetch_accesses << std::endl;
    std::cout << "OPTgen hits: " << hits << std::endl;
    std::cout << "OPTgen hit rate: " << 100*(double)hits/((double)demand_accesses + (double)prefetch_accesses) << std::endl;
    std::cout << "Number of evictions: " << num_of_evictions << std::endl;
    std::cout << "Number of cache-friendly evictions: " << num_of_cache_friendly_evictions << std::endl;

    cout << endl << endl;
    return;
}
