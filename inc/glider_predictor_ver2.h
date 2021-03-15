//glider_predictor//

#ifndef PREDICTOR_H
#define PREDICTOR_H

using namespace std;

#include <iostream>

#include <math.h>
#include <set>
#include <vector>
#include <map>

#define NUM_UNIQUE_PC 7
#define NUM_ISVM 2048
#define NUM_WEIGHT 20

#define TRAIN_TH 30000

#define PREDICT_TH_HIGH 60
#define PREDICT_TH_LOW 0

#define CACHE_FRIENDLY 2
#define CACHE_FRIENDLY_LOW 1
#define CACHE_AVERSE 0

uint64_t CRC( uint64_t _blockAddress )
{
    static const unsigned long long crcPolynomial = 3988292384ULL;
    unsigned long long _returnVal = _blockAddress;
    for( unsigned int i = 0; i < 32; i++ )
        _returnVal = ( ( _returnVal & 1 ) == 1 ) ? ( ( _returnVal >> 1 ) ^ crcPolynomial ) : ( _returnVal >> 1 );
    return _returnVal;
}

int hash_ISVM( uint64_t pc )
{
	return pc%NUM_ISVM;
}

int hash_weight( uint64_t last_pc )
{
	return last_pc%NUM_WEIGHT;
}

class GLIDER_PC_PREDICTOR
{
    int ISVM_TABLE[NUM_ISVM][NUM_WEIGHT] = {{0}};	    //first : PC, second : weights
	map<uint64_t, int > PCHR;				//first : PC, second : lru 
	int access_time;
    
	public:
	
	GLIDER_PC_PREDICTOR(){
		
		PCHR.clear();
		access_time = 0;
		
	}
	
    void increment (uint64_t pc)
    {
        int ISVM_idx = hash_ISVM(pc);
		int sum = 0;
		for (map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++) {
			sum += ISVM_TABLE[ISVM_idx][hash_weight(it->first)];
		}
		if (sum < TRAIN_TH) {
			
			for (map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++) {
				ISVM_TABLE[ISVM_idx][hash_weight(it->first)]++;
			}  

		}

    }

    void decrement (uint64_t pc)
    {
		int ISVM_idx = hash_ISVM(pc);
		int sum = 0;
		for ( map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++ ) {
			sum += ISVM_TABLE[ISVM_idx][hash_weight(it->first)];
		}
		if ( sum > -TRAIN_TH ) {
			
			for ( map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++ ) {
				ISVM_TABLE[ISVM_idx][hash_weight(it->first)]--;
			}  

		}

    }

    int get_prediction (uint64_t pc)
    {
		int ISVM_idx = hash_ISVM(pc);
		int prediction = 0;
		int sum = 0;
		uint64_t old_pc; 
		
		for ( map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++ ) {
			sum += ISVM_TABLE[ISVM_idx][hash_weight(it->first)];
		}

		if ( sum >= PREDICT_TH_HIGH ) {
			prediction = CACHE_FRIENDLY;
		}
		else if ( sum < PREDICT_TH_LOW ) {
			prediction = CACHE_AVERSE;
		}
		else {
			prediction = CACHE_FRIENDLY_LOW;
			//prediction = CACHE_AVERSE;
		}

		//update PCHR
		if ( PCHR.find(pc) != PCHR.end() ) {
			
			for ( map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++ ) {
				
				if( it->second < PCHR[pc] ) {
					
					it->second++;
				} 
			} 
		}
		
		else if ( PCHR.size() < NUM_UNIQUE_PC ) {
			
			for ( map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++ ) {
			
				it->second++;
			}
		}
		else if ( PCHR.size() == NUM_UNIQUE_PC ) {
		
			for ( map<uint64_t, int>::iterator it = PCHR.begin(); it != PCHR.end(); it++ ) {
		
			 	it->second++;		
				if( it->second == NUM_UNIQUE_PC ) {
	
					old_pc = it->first;
				}
				

			}
			PCHR.erase(old_pc);
		} 
		assert(PCHR.size() <= NUM_UNIQUE_PC);
		PCHR[pc] = 0;
		return prediction;
		
    }
};

#endif
