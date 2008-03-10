/* ISO14443A Miller decoder for OpenPICC, working with differential time samples
 *  
 * Copyright 2007 Milosch Meriac <meriac@bitmanufaktur.de>
 * Copyright 2007 Karsten Nohl <honk98@web.de>
 * Copyright 2007,2008 Henryk Plötz <henryk@ploetzli.ch>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by 
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <openpicc.h>
#include <FreeRTOS.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "iso14443.h"
#include "iso14443a_diffmiller.h"
#include "usb_print.h"

#include "performance.h"

#define DEBUGP (void)
#define printf usb_print_string

/*
 * Decoding Methodology: We'll only see the edges for the start of modulation pauses and not
 * all symbols generate modulation pauses at all. Two phases:
 *  + Old state and next edge delta to sequence of symbols (might be more than one symbol per edge)
 *  + Symbols to EOF/SOF marker and bits
 * 
 * These are the possible old-state/delta combinations and the symbols they yield:
 * 
 * old_state  delta (in bit_len/4)  symbol(s)
 * none       3                     ZZ
 * none       5                     ZX
 * X          3                     X
 * X          5                     YZ
 * X          7                     YX
 * X          >=9                   YY
 * Y          3                     ZZ
 * Y          5                     ZX
 * Z          3                     Z
 * Z          5                     X
 * Z          >=7                   Y
 * 
 * All other combinations are invalid and likely en- or decoding errors. (Note that old_state
 * Y is exactly the same as old_state none.)
 * 
 * The mapping from symbol sequences to SOF/EOF/bit is as follows:
 *         X: 1
 * 0, then Y: EOF
 *   other Y: 0
 *   first Z: SOF
 *   other Z: 0
 */

#define BIT_LEN 128
/* The theoretical error margin for the timing measurement is about 7 (note: that is a jitter of 7 in
 * total, e.g. +/- 3.5), but we'll round that up to +/- 8. However, the specification allows pause
 * times from 2us to 3us, e.g. 1us difference, so we'll add another 13.
 */ 
#define BIT_LEN_ERROR_MAX (8+13)
#define PAUSE_LEN 20
/* Subtract the mean error margin (about 4, see comment above) from the bit length
 * Also subtract the pause length for the case when the clock is not counting during
 * a pause. Will subtract this length below for the when the clock *is* counting during
 * pauses.
 */
#define BIT_OFFSET (-4 -PAUSE_LEN)
#define BIT_LEN_3 ((BIT_LEN*3)/4 +BIT_OFFSET)
#define BIT_LEN_5 ((BIT_LEN*5)/4 +BIT_OFFSET)
#define BIT_LEN_7 ((BIT_LEN*7)/4 +BIT_OFFSET)
#define BIT_LEN_9 ((BIT_LEN*9)/4 +BIT_OFFSET)

#define ALMOST_EQUAL(a,b) ( abs(a-b) <= BIT_LEN_ERROR_MAX )
#define MUCH_GREATER_THAN(a,b) ( a > (b+BIT_LEN_ERROR_MAX) )
#define ALMOST_GREATER_THAN_OR_EQUAL(a,b) (a >= (b-BIT_LEN_ERROR_MAX))

enum symbol {NO_SYM=0, sym_x, sym_y, sym_z};
enum bit { BIT_ERROR, BIT_SOF, BIT_0, BIT_1, BIT_EOF };

struct diffmiller_state {
	int initialized, pauses_count;
	enum symbol old_state;
	enum bit last_bit;
	iso14443_frame *frame;
	u_int32_t counter;
	u_int16_t byte,crc;
	u_int8_t parity;
	struct {
		u_int8_t in_frame:1;
		u_int8_t frame_finished:1;
		u_int8_t overflow:1;
		u_int8_t error:1;
	} flags;
};

struct diffmiller_state _state;

inline void start_frame(struct diffmiller_state * const state)
{
	state->counter=0;
	state->byte=0;
	state->parity=0;
	state->crc=0x6363;
	
	performance_set_checkpoint("start_frame before memset");
	memset(&state->flags, 0, sizeof(state->flags));
	state->flags.in_frame = 1;
	
	//memset(state->frame, 0, sizeof(*state->frame));
	memset(state->frame, 0, (u_int32_t)&(((iso14443_frame*)0)->data) );
	performance_set_checkpoint("start_frame after memset");
}

static inline void append_to_frame(struct diffmiller_state *const state,
		const u_int8_t byte, const u_int8_t parity, const u_int8_t valid_bits) {

	iso14443_frame * const f = state->frame;
	
	if(f->numbytes >= sizeof(f->data)/sizeof(f->data[0])-1) { /* -1, because the last byte may be half filled */
		state->flags.overflow = 1;
		return;
	}

	if(f->numbits != 0) {
		DEBUGP("Appending to a frame with incomplete byte");
	}

	f->data[f->numbytes] = byte & 0xff;
	f->parity[f->numbytes/8] |= ((parity&1)<<(f->numbytes%8));

	if(valid_bits == 8) {
		f->numbytes++;
	} else {
		f->numbits += valid_bits;
	}
}


static inline void end_frame(struct diffmiller_state * const state)
{
	if(state->frame != NULL) {
		if(state->counter > 0) {
			append_to_frame(state, state->byte, 0, state->counter);
		}
		
		if(!state->crc)
			state->frame->parameters.a.crc = CRC_OK;
		else
			state->frame->parameters.a.crc = CRC_ERROR;
		
		state->flags.frame_finished = 1;
	}
}

static inline void Miller_Bit(struct diffmiller_state * const state, const int bit_value)
{
	
    if(state->counter<8) {
    	state->byte=state->byte | (bit_value<<state->counter);
    	if(bit_value)
    		state->parity++;
    } else DEBUGP((((~state->parity)&1)==bit_value) ? " ":"!" );

    if(++state->counter==9) {
    	state->counter=0;
    	append_to_frame(state, state->byte, bit_value, 8);

    	//DEBUGP(" ==0x%02X\n",byte);

    	state->byte=(state->byte ^ state->crc)&0xFF;
    	state->byte=(state->byte ^ state->byte<<4)&0xFF;
    	state->crc=((state->crc>>8)^(state->byte<<8)^(state->byte<<3)^(state->byte>>4))&0xFFFF;

    	//printf(" [%04X]\n",crc);

    	state->byte=0;        
    	state->parity=0;
    }
}

#define PRINT_BIT(a) if(0){(void)a;}
//#define PRINT_BIT(a) usb_print_string(a)

#define DO_BIT_0 { \
	Miller_Bit(state, 0); \
	PRINT_BIT(" 0"); \
	}

#define DO_BIT_1 { \
	Miller_Bit(state, 1); \
	PRINT_BIT(" 1"); \
	}

#define DO_SYMBOL_X \
	if(!in_frame) { \
		if(last_bit == BIT_0) DO_BIT_0; \
		error = 1; \
		end_frame(state); \
		PRINT_BIT(" ERROR\n"); \
		last_bit = BIT_ERROR; \
		in_frame = 0; \
	} else { \
		if(last_bit == BIT_0) DO_BIT_0; \
		DO_BIT_1; \
		last_bit = BIT_1; \
	}

#define DO_SYMBOL_Y \
	if(!in_frame) { \
		if(last_bit == BIT_0) DO_BIT_0; \
		error = 1; \
		end_frame(state); \
		PRINT_BIT(" ERROR\n"); \
		last_bit = BIT_ERROR; \
		in_frame = 0; \
	} else { \
		if(last_bit == BIT_0) { \
			end_frame(state); \
			PRINT_BIT(" EOF\n"); \
			last_bit = BIT_EOF; \
			in_frame = 0; \
		} else { \
			last_bit = BIT_0; \
		} \
	}

#define DO_SYMBOL_Z \
	if(!in_frame) { \
		if(last_bit == BIT_0) DO_BIT_0; \
		start_frame(state); \
		PRINT_BIT("SOF"); \
		in_frame = 1; \
		last_bit = BIT_ERROR; \
		last_bit = BIT_SOF; \
	} else { \
		if(last_bit == BIT_0) DO_BIT_0; \
		last_bit = BIT_0; \
	}


int iso14443a_decode_diffmiller(struct diffmiller_state * const state, iso14443_frame * const frame, 
	const u_int32_t buffer[], unsigned int * const offset, const unsigned int buflen)
{
	if(state == NULL || !state->initialized) return -EINVAL;
	if(state->frame != NULL && state->frame != frame) return -EINVAL;
	state->frame = frame;
	
	enum symbol old_state = state->old_state;
	enum bit last_bit = state->last_bit;
	int in_frame = state->flags.in_frame;
	int error = state->flags.error;
	
	for(; *offset < buflen; ) {
		int delta;
		if(state->pauses_count)
			delta = buffer[(*offset)++] - PAUSE_LEN;
		else
			delta = buffer[(*offset)++];

		switch(old_state) {
		case sym_x:
			if( ALMOST_EQUAL(delta, BIT_LEN_3) ) {
				DO_SYMBOL_X;
				old_state = sym_x;
			} else if( ALMOST_EQUAL(delta, BIT_LEN_5) ) {
				DO_SYMBOL_Y;
				DO_SYMBOL_Z;
				old_state = sym_z;
			} else if( ALMOST_EQUAL(delta, BIT_LEN_7) ) {
				DO_SYMBOL_Y;
				DO_SYMBOL_X;
				old_state = sym_x;
			} else if( ALMOST_GREATER_THAN_OR_EQUAL(delta, BIT_LEN_9)) {
				DO_SYMBOL_Y;
				DO_SYMBOL_Y;
				old_state = sym_y;
			}
			break;
		case NO_SYM: /* Fall-Through */
		case sym_y:
			if( ALMOST_EQUAL(delta, BIT_LEN_3) ) {
				DO_SYMBOL_Z;
				DO_SYMBOL_Z;
				old_state = sym_z;
			} else if( ALMOST_EQUAL(delta, BIT_LEN_5) ) {
				DO_SYMBOL_Z;
				DO_SYMBOL_X;
				old_state = sym_x;
			} 
			break;
		case sym_z:
			if( ALMOST_EQUAL(delta, BIT_LEN_3) ) {
				DO_SYMBOL_Z;
				old_state = sym_z;
			} else if( ALMOST_EQUAL(delta, BIT_LEN_5) ) {
				DO_SYMBOL_X;
				old_state = sym_x;
			} else if( ALMOST_GREATER_THAN_OR_EQUAL(delta, BIT_LEN_7)) {
				DO_SYMBOL_Y;
				old_state = sym_y;
			}
			break;
		}
		
		if(state->flags.frame_finished)  {
			state->flags.frame_finished = 0;
			state->old_state = old_state;
			state->last_bit = last_bit;
			state->flags.in_frame = in_frame;
			state->flags.error = error;
			performance_set_checkpoint("frame finished");
			return 0;
		}
	}
	
	state->old_state = old_state;
	state->last_bit = last_bit;
	state->flags.in_frame = in_frame;
	state->flags.error = error;
	
	return -EBUSY;
}

struct diffmiller_state *iso14443a_init_diffmiller(int pauses_count)
{
	if(_state.initialized) return NULL;
	struct diffmiller_state *state = &_state;
	state->initialized = 1;
	state->pauses_count = pauses_count;
	state->frame = NULL;
	state->flags.frame_finished = 0;
	
	return state;
}
