/*
 * Copyright (c) 2011, Medical Research Council
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *    * Redistributions of source code must retain the above copyright notice,
 * 		this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 * 		notice, this list of conditions and the following disclaimer in the
 * 		documentation and/or other materials provided with the distribution.
 *    * Neither the name of the MRC Epidemiology Unit nor the names of its
 * 		contributors may be used to endorse or promote products derived from
 * 		this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * 
 * 
 * Very simplistic circular buffer used to hold debug info until can be stored
 * into flash
 * 
 * To be driven by main thread only (not in interrupt handlers) since no locking
 * as yet
 */

#include "debug.h"
#include "hw_config.h"

#define MAX_DBG_BUFFER 240 // current max possible is 255 (i.e use of uint8_t)
#define BUF_OVERFLOW_CHAR '~'

uint8_t dbg_circ_buffer[MAX_DBG_BUFFER];
uint8_t dbg_write = 0;
uint8_t dbg_read = 0;
uint8_t no_more_reads = 1;

// ----

static int wDbg(uint8_t c) {
	if (no_more_reads) {
		no_more_reads = 0;
	} else {
		if (dbg_write) {
			if (dbg_write == dbg_read) {
				dbg_circ_buffer[dbg_write - 1] = BUF_OVERFLOW_CHAR;
				return -1;
			}
		} else if (!dbg_read) {
			// Mark buf overflow
			dbg_circ_buffer[MAX_DBG_BUFFER - 1] = BUF_OVERFLOW_CHAR;
			return -1;
		}
	}
	
	dbg_circ_buffer[dbg_write++] = c;
	if (MAX_DBG_BUFFER == dbg_write) {
		dbg_write = 0;
	}
	return 0;
}

static void writeDbgStr(const char* dbg_txt) {
	uint8_t len = 0;
	const char* tptr = dbg_txt;
	while (len < MAX_DBG_BUFFER && *tptr) {
		if (wDbg((uint8_t) *tptr++))
			break;
		len++;
	}
	wDbg(0);
}

static void writeDbgHex8(uint8_t n) {
	if (wDbg(1)) return;
	wDbg(n);
}

static void writeDbgHex16(uint16_t n) {
	if (wDbg(2)) return;
	if (wDbg(n>>8)) return;
	wDbg(n);
}

static void writeDbgHex32(uint32_t n) {
	if (wDbg(3)) return;
	if (wDbg(n>>24)) return;
	if (wDbg(n>>16)) return;
	if (wDbg(n>>8)) return;
	wDbg(n);
}

uint8_t getReadSize(uint8_t max_size) {
	uint8_t sz = 0;
	if (!no_more_reads) {
		if (dbg_write == dbg_read) {
			sz = MAX_DBG_BUFFER;
		} else if (dbg_write > dbg_read) {
			sz = dbg_write - dbg_read;
		} else {
			sz = MAX_DBG_BUFFER - (dbg_read - dbg_write);
		}
		if (sz > max_size)
			sz = max_size;
	}
	return sz;
}

uint8_t readDbgByte(uint8_t* stop) {
	uint8_t val;
	*stop = 0;
	if (no_more_reads) {
		*stop = 1;
		return 0;
	}
	val = dbg_circ_buffer[dbg_read++];
	if (MAX_DBG_BUFFER == dbg_read) {
		dbg_read = 0;
	}
	if (dbg_write == dbg_read) {
		no_more_reads = 1;
	}
	return val;
}

// ----

void resetDbg() {
	dbg_write = 0;
	dbg_read = 0;
	no_more_reads = 1;
}

void writeStr(const char* dbg_txt) {
	
#ifdef CWA_USART_DEBUG
	SendStringUart1(dbg_txt);
	SendStringUart1("\r\n");
#else
	writeDbgStr(dbg_txt);
#endif	
	
}

void writeHex8(uint8_t n) {
#ifdef CWA_USART_DEBUG
	SendHex8Uart1(n);
#else
	writeDbgHex8(n);
#endif	
}

void writeHex16(uint16_t n) {
#ifdef CWA_USART_DEBUG
	SendHex16Uart1(n);
#else
	writeDbgHex16(n);
#endif	
}

void writeHex32(uint32_t n) {
#ifdef CWA_USART_DEBUG
	SendHex32Uart1(n);
#else
	writeDbgHex32(n);
#endif	
}

// EOF
