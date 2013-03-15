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
 * Functions for read/write bytes to Nand Flash memory
 * 
 * Hynix NAND HY27UF(08/16)4G2B Series have 512Mx8bit with spare 16Mx8 bit
 * capacity. The device contains 4096 blocks, composed by 64 pages.
 * 
 * A program operation allows to write the 2112-byte page in typical 200us.
 * Data in the page can be read out at 25ns cycle time per byt (x8).
 * The I/O pins serve as the ports for address and data input/output as well as
 * command input.
 * 
 * Each page is used to hold 2046 data bytes, 2 bytes data CRC, 1 byte flash
 * block status and 63 spare bytes used as detailed in spare area format in
 * shared.h
 */

#include "stm32f10x.h"
#include "hw_config.h"
#include "nand_cwa.h"
#include "shared.h"
#include "debug.h"

// ----

// Nand flash commands
#define READ_PAGE_CYCLE1	0x00
#define READ_PAGE_CYCLE2	0x30

#define PROGRAM_PAGE_CYCLE1	0x80
#define PROGRAM_PAGE_CYCLE2	0x10

#define READ_STATUS_COMMAND	0x70
#define READ_ID_COMMAND		0x90

#define ERASE_BLOCK_SETUP	0x60
#define ERASE_COMMAND		0xD0

// ----

// Defines
#define FIRST_PAGE				0
#define SECOND_PAGE				1

#define FIRST_BLOCK				0

// ----

// Page debug area size definitions

// -1 for the dbg len at start
const uint8_t page0_dbg_size = FLASH_PAGE - VERSION_ADDR - VERSION_SIZE - 1;
const uint8_t page1_dbg_size =
	FLASH_PAGE - ACTION_EPOC_ADDR - EPOC_TIME_SIZE - 1;
const uint8_t pageN_dbg_size =
	FLASH_PAGE - TEMP_LEVEL_ADDR - TEMP_LEVEL_SIZE - 1;

// ----

// Global variables

uint8_t version_byte;

extern uint8_t data_values[FLASH_PAGE];
extern struct config_info current_config;

extern uint32_t start_page_tick;

extern uint16_t currentBatteryVoltage;
extern uint16_t currentTemperature;

extern uint8_t accel_rate_and_g_scale;

// ----

// Nand state variables

uint16_t current_block = 0;
uint16_t current_page = 0;

uint16_t read_block = 0;
uint8_t read_page = 0;

uint8_t last_page_in_block = 0;

uint8_t status = OK_USED_STATUS;

GPIO_InitTypeDef NAND_GPIO_InitStructure;

// ----

// Core Nand functions

static void nand_cwa_data_bus_write_mode() {
	NAND_GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	NAND_GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	NAND_GPIO_InitStructure.GPIO_Pin =
		NAND_D7 | NAND_D6 | NAND_D5 | NAND_D4 | NAND_D3 | NAND_D2 | NAND_D1 |
		NAND_D0;
	GPIO_Init(NAND_DATA_PORT, &NAND_GPIO_InitStructure);
}

static void nand_cwa_data_bus_read_mode() {
	NAND_GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	NAND_GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	NAND_GPIO_InitStructure.GPIO_Pin =
		NAND_D7 | NAND_D6 | NAND_D5 | NAND_D4 | NAND_D3 | NAND_D2 | NAND_D1 |
		NAND_D0;
	GPIO_Init(NAND_DATA_PORT, &NAND_GPIO_InitStructure);
}

void nand_cwa_init() {
	// Enable clocks to GPIO otherwise nothing happens
	RCC_APB2PeriphClockCmd(NAND_RCC_CONTROL_PORT, ENABLE);
	RCC_APB2PeriphClockCmd(NAND_RCC_DATA_PORT, ENABLE);
	
	// Make sure everything comes up in a sensible state to avoid corruption
	GPIO_SetBits(NAND_CONTROL_PORT, NAND_NCE | NAND_NWE | NAND_NRE);
	GPIO_ResetBits(NAND_CONTROL_PORT, NAND_ALE | NAND_CLE);
	GPIO_ResetBits(NAND_DATA_PORT, NAND_D7 | NAND_D6 | NAND_D5 | NAND_D4 |
		NAND_D3 | NAND_D2 | NAND_D1 | NAND_D0);
		
	// Init pin I/O modes
	NAND_GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	NAND_GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
	NAND_GPIO_InitStructure.GPIO_Pin =
		NAND_NCE | NAND_NWE | NAND_NRE | NAND_CLE | NAND_ALE;
	GPIO_Init(NAND_CONTROL_PORT, &NAND_GPIO_InitStructure);
	
	// NAND_RNB is a pulled-up input
	NAND_GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	NAND_GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	NAND_GPIO_InitStructure.GPIO_Pin = NAND_RNB;
	GPIO_Init(NAND_CONTROL_PORT, &NAND_GPIO_InitStructure);
	
	// Start data bus as an output to stop it floating around
	nand_cwa_data_bus_write_mode();
}

static void nandWriteByte(uint8_t aByte) {
	// There must be an easier way
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NCE,Bit_RESET);
	
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NWE,Bit_RESET);

	GPIO_Write(NAND_DATA_PORT,
		(GPIO_ReadOutputData(NAND_DATA_PORT)&0x00ff)|(aByte<<8));

	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NWE,Bit_SET);

	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NCE,Bit_SET);
}

static uint8_t nandReadByte() {
	// There must be an easier way
	uint8_t val = 0;
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NCE,Bit_RESET);
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NRE,Bit_RESET);

	val=(GPIO_ReadInputData(NAND_DATA_PORT)>>8)&0xff;

	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NRE,Bit_SET);
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_NCE,Bit_SET);
	return val;
}

static void nandWriteCmd(uint8_t aCmd) {
	// Assumes NCE already reset and data bus in write mode
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_CLE,Bit_SET);
	nandWriteByte(aCmd);
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_CLE,Bit_RESET);
}

static void nandWriteAddr(uint8_t* aAddrCyclePtr, uint8_t aNumofCycles) {
	// Assumes NCE already reset and data bus in write mode
	uint8_t loop;
	uint8_t* ptr = aAddrCyclePtr;
	
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_ALE,Bit_SET);
	for (loop = 0; loop < aNumofCycles; loop++) {
		nandWriteByte(*ptr++);
	}
	GPIO_WriteBit(NAND_CONTROL_PORT,NAND_ALE,Bit_RESET);
}

void readDeviceId(struct nand_identity* aResult) {
	uint8_t addr = 0x00;
	
	nandWriteCmd(READ_ID_COMMAND);
	nandWriteAddr(&addr,1);
	
	nand_cwa_data_bus_read_mode();
	aResult->maker  = nandReadByte();
	aResult->device = nandReadByte();
	aResult->third  = nandReadByte();
	aResult->fourth = nandReadByte();
	aResult->fifth  = nandReadByte();
	nand_cwa_data_bus_write_mode();
	
}

static uint8_t readStatus() {
	uint8_t result;
	
	while (GPIO_ReadInputDataBit(NAND_CONTROL_PORT,NAND_RNB) == Bit_RESET) {}
	
	nandWriteCmd(READ_STATUS_COMMAND);
	
	nand_cwa_data_bus_read_mode();
	result = nandReadByte();
	nand_cwa_data_bus_write_mode();
	
	return result;
}

/*
 * The Nand address cycles: (L = must be set low)
 *           IO7 IO6 IO5 IO4 IO3 IO2 IO1 IO0
 * 1st Cycle A7  A6  A5  A4  A3  A2  A1  A0		< Column 1
 * 2nd Cycle L   L   L   L	 A11 A10 A9  A8 	< Column 2
 * 3rd Cycle A19 A18 A17 A16 A15 A14 A13 A12	< Row 1
 * 4th Cycle A27 A26 A25 A24 A23 A22 A21 A20	< Row 2
 * 5th Cycle L   L   L   L   L   L	 A29 A28 	< Row 3
 */

static void setAddress(uint16_t aBlockAddr, uint16_t aPageAddr,
	uint16_t aByteAddrInPage, uint8_t* aAddress) {
	aAddress[0] =  aByteAddrInPage & 0xff;
	aAddress[1] = (aByteAddrInPage & 0xf00) >> 8;
	aAddress[2] = ((aBlockAddr & 0x03) << 6) | (aPageAddr & 0x3f);
	aAddress[3] = (aBlockAddr >> 2) & 0xff;
	aAddress[4] = (aBlockAddr >> 10) & 0x03;
}

static void readData(uint16_t aBlockAddr, uint16_t aPageAddr,
	uint16_t aByteAddrInPage, uint8_t* aDestBuffer, uint16_t aNumBytes) {
	uint8_t fiveCycleAddress[5];
	uint16_t loop;
	uint8_t* ptr = aDestBuffer;
	
	setAddress(aBlockAddr, aPageAddr, aByteAddrInPage, fiveCycleAddress);
	
	nandWriteCmd(READ_PAGE_CYCLE1);
	nandWriteAddr(fiveCycleAddress,5);
	nandWriteCmd(READ_PAGE_CYCLE2);
	
	// Wait until the page is loaded into the data registers
	// TODO check is there anyway this could loop forever?
	while (GPIO_ReadInputDataBit(NAND_CONTROL_PORT,NAND_RNB) == Bit_RESET) {}
	
	nand_cwa_data_bus_read_mode();
	for(loop = 0; loop < aNumBytes; loop++) {
		*ptr = nandReadByte();
		ptr++;
	}
	nand_cwa_data_bus_write_mode();
	
}

// End of Core Nand functions

// ----

// BioBand specific checking & data functions

static void programConfigData() {
	uint16_t loop;
	uint8_t* byte_ptr;
	
	// Band id
	for(loop = 0; loop < MAX_ID_LEN; loop++) {
		nandWriteByte(current_config.band_id[loop]);
	}
	
	// Subject id
	for(loop = 0; loop < MAX_ID_LEN; loop++) {
		nandWriteByte(current_config.subject_id[loop]);
	}
	
	// Test id
	for(loop = 0; loop < MAX_ID_LEN; loop++) {
		nandWriteByte(current_config.test_id[loop]);
	}
	
	// Centre id
	for(loop = 0; loop < MAX_CENTRE_ID_LEN; loop++) {
		nandWriteByte(current_config.centre_id[loop]);
	}
	
	// Calibration data
	for(loop = 0; loop < MAX_CALIBRATION_DATA_LEN; loop++) {
		nandWriteByte(current_config.cali_data[loop]);
	}
	
	// Store max samples
	byte_ptr = (uint8_t*) &current_config.max_samples;
	for(loop = 0; loop < MAX_SAMPLES_SIZE; loop++) {
		nandWriteByte(*byte_ptr++);
	}

	if (current_config.collect_start_time) {
		// Store rest only if collect has started (i.e start time has a value)
		byte_ptr = (uint8_t*) &current_config.collect_start_time;
		for(loop = 0; loop < EPOC_TIME_SIZE; loop++) {
			nandWriteByte(*byte_ptr++);
		}

		// Store accelerometer config
		nandWriteByte(accel_rate_and_g_scale);
		
		// Store fw/hw versions
		nandWriteByte(version_byte);
	}
}

static void programSpareArea() {
	
	// Handles the writing to all bytes past the bad block marker
	
	uint8_t max_dbg_sz = pageN_dbg_size;
	uint8_t dbg_sz;
	uint16_t loop;
	uint8_t* byte_ptr;
	
#ifdef ENABLE_TEMPERATURE
	// Temp is read in accelIntrHandler (main.c)
	uint16_t temperature=currentTemperature;
#else
	uint16_t temperature=current_page;
#endif

	// ** NOTE if alter this code, check that byte addresses are still correct
	// in shared.h **
	
	// Spare page bytes
	byte_ptr = (uint8_t*) &start_page_tick;
	
	// Status
	nandWriteByte(status);
	status = OK_USED_STATUS;
	
	// Store current tick
	for(loop = 0; loop < BIOBAND_TICK_SIZE; loop++) {
		nandWriteByte(*byte_ptr++);
	}
	
	// Store temperature
	// if the number of bytes is changed, remember to change area addresses!
	byte_ptr = (uint8_t*) &temperature;
	for(loop = 0; loop < TEMP_LEVEL_SIZE; loop++) {
		nandWriteByte(*byte_ptr++);
	}
		
	if (FIRST_PAGE == current_page) {
		// First page of block, so store the power level, tag & subject ids
		
		// Power level
#ifdef ENABLE_BATTERY_LEVEL
		// Battery is read in main.c RTCIntrHandler()
		uint16_t val = currentBatteryVoltage;
#else
		uint16_t val = 0;
#endif
		byte_ptr = (uint8_t*) &val;
		for(loop = 0; loop < BATTERY_LEVEL_SIZE; loop++) {
			nandWriteByte(*byte_ptr++);
		}
		
		programConfigData();
		
		max_dbg_sz = page0_dbg_size;
		
	} else if (SECOND_PAGE == current_page) {
		// Second page of block, only reserve space since status details written
		// later (i.e use 0xFF so flash bytes not altered)
		
		// First downloaded time
		for(loop = 0; loop < EPOC_TIME_SIZE; loop++) {
			nandWriteByte(0xFF);
		}
		
		// End tick time
		for(loop = 0; loop < BIOBAND_TICK_SIZE; loop++) {
			nandWriteByte(0xFF);
		}
		
		// End number of samples
		for(loop = 0; loop < MAX_SAMPLES_SIZE; loop++) {
			nandWriteByte(0xFF);
		}
		
		// Actioned time (can store here since not a guaranteed valid data item)
		byte_ptr = (uint8_t*) &current_config.actioned_time;
		for(loop = 0; loop < EPOC_TIME_SIZE; loop++) {
			nandWriteByte(*byte_ptr++);
		}
		
		max_dbg_sz = page1_dbg_size;
	}
	
	// Store any debugging
	dbg_sz = getReadSize(max_dbg_sz);
	
	if (!dbg_sz && max_dbg_sz > 6) {
		// No dbg to store & enough space for block/page info in raw bNpN format
		
		// Store length
		nandWriteByte(6);
		
		nandWriteByte('B');
		nandWriteByte(current_block);
		nandWriteByte(current_block>>8);
		nandWriteByte('P');
		nandWriteByte(current_page);
		nandWriteByte(current_page>>8);
		
			
	} else {
		// Debug data to store
		
		// Store length
		nandWriteByte(dbg_sz);
			
		if (dbg_sz) {
			uint16_t lp;
			uint8_t stop = 0;
			
			// Store the dbg info
			for (lp = 0; lp < dbg_sz; lp++) {
				uint8_t byte = readDbgByte(&stop);
				if (stop)
					break;
				nandWriteByte(byte);
			}
		}
	}
}

static int markBadBlock(uint16_t aBlockAddr, uint8_t aMarker) {
	uint8_t fiveCycleAddress[5];
	uint16_t loop;
	int retval = 0;
	
	writeStr("Mkbb");
	
	// Any block where the 1st Byte in the spare area of the 1st or 2nd page (if
	// the 1st page is Bad) does not contain FFh it's a Bad Block.
	
	for (loop = 0; loop < 2; loop++) {
	
		setAddress(aBlockAddr, loop, PAGE_SIZE, fiveCycleAddress);
		
		nandWriteCmd(PROGRAM_PAGE_CYCLE1);
		nandWriteAddr(fiveCycleAddress,5);
		
		nandWriteByte(aMarker);

		nandWriteCmd(PROGRAM_PAGE_CYCLE2);
		
		retval = readStatus() & 0x01;
		
		if (!retval)
			break;
	}
	
	return retval;
}

static void markUsed() {
	
	// Mark the page as being used (aids end identification in the last used
	// block if needing to sequentially look through the data).

	int retval = 0;
	uint8_t fiveCycleAddress[5];
	
	writeStr("MkUsd");
	
	setAddress(current_block, current_page, PAGE_STATUS_ADDR, fiveCycleAddress);
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE1);
	nandWriteAddr(fiveCycleAddress,5);
	
	programSpareArea();
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE2);
	
	retval = readStatus() & 0x01;
	
	if (retval) {
		// Mark block as bad, use page as marker to indicate where we got to
		markBadBlock(current_block, current_page + OUR_BAD_BLOCK_INDICATOR);
		
		current_page = MAX_PAGES_PER_BLOCK;
	}
}

static int checkBadBlock(uint16_t aBlock, uint8_t* aByte, uint8_t* aPage) {
	
	// Check first page in block
	readData(aBlock, FIRST_PAGE, BAD_BLOCK_ADDR, aByte, 1);
	if (VALID_BLOCK_INDICATOR != *aByte) {
		*aPage = FIRST_PAGE;
		return -1;
	}
	// Check second page in block
	readData(aBlock, SECOND_PAGE, BAD_BLOCK_ADDR, aByte, 1);
	if (VALID_BLOCK_INDICATOR != *aByte) {
		*aPage = SECOND_PAGE;
		return -1;
	}
	
	// Block is good
	return 0;
}

uint16_t findNextBadBlock(uint16_t aStartBlockAddr,uint8_t* aByte,
	uint8_t* aPage) {
	// Sequential search for the next bad block
	
	uint16_t found = MAX_BLOCKS;
	uint16_t loop;
	
	// Note that this func does not flag up if the block was partially written
	
	for (loop = aStartBlockAddr; loop < MAX_BLOCKS; loop++) {	
		if (checkBadBlock(loop,aByte,aPage)) {
			found = loop;
			break;
		}
	}
	return found;
}

static uint16_t findNextUsedBlock(uint16_t aStartBlockAddr) {
	
	// Sequential search for the next used block
	
	uint16_t block = aStartBlockAddr;
	uint8_t byte, dummy;
	
	while (block < MAX_BLOCKS) {
		
		if (!checkBadBlock(block, &byte, &dummy)) {
			// Block is valid
			
			readData(block, FIRST_PAGE, PAGE_STATUS_ADDR, &byte, 1);
			if (UNUSED_PAGE == byte) {
				// Unused & last block in sequence
				return MAX_BLOCKS;
			}
			
			// First page is used
			break;
		}
		
		block++;
	}
	return block;
}

static uint16_t findNextUsedBlock2(uint16_t aStartBlockAddr,
	uint8_t* aLastPage) {
	
	// Sequential search for the next used block, return last used page if bad
	// block, marked by the BioBand, encountered
	
	uint16_t block = aStartBlockAddr;
	uint8_t byte, dummy;
	
	*aLastPage = MAX_PAGES_PER_BLOCK;
	
	while (block < MAX_BLOCKS) {
		
		int bad = checkBadBlock(block, &byte, &dummy);
		if (bad) {
			if (byte > OUR_BAD_BLOCK_INDICATOR) {
				// Potentially our marking (indicates which page we got to)
				if (byte <= MAX_PAGES_PER_BLOCK) {
					*aLastPage = byte - OUR_BAD_BLOCK_INDICATOR;
					bad = 0;
				}
			}
		}
		
		if (!bad) {
			// Block is valid
			
			readData(block, FIRST_PAGE, PAGE_STATUS_ADDR, &byte, 1);
			if (UNUSED_PAGE == byte) {
				// Unused
				return MAX_BLOCKS;
			}
			
			// First page is used
			break;
		}
		
		block++;
	}
	return block;
}

uint16_t findFirstUsedBlock(uint8_t* aLastPage) {
	return findNextUsedBlock2(FIRST_BLOCK, aLastPage);
}

static int eraseBlock(uint16_t aBlockAddr) {
	uint8_t threeCycleAddress[3];
	int loop;
	int retval =0;
	
	if (aBlockAddr >= MAX_BLOCKS)
		return -1;
		
	/*
	 * 3rd Cycle A19 A18 A17 A16 A15 A14 A13 A12	< Row 1
	 * 4th Cycle A27 A26 A25 A24 A23 A22 A21 A20	< Row 2
	 * 5th Cycle L   L   L   L   L   L	 A29 A28 	< Row 3
	 */

	// Only cycle addresses A18 to A29 are valid while A12 to A17 are ignored
	threeCycleAddress[0] = (aBlockAddr & 0x03) << 6;
	threeCycleAddress[1] = (aBlockAddr >> 2) & 0xff;
	threeCycleAddress[2] = (aBlockAddr >> 10) & 0x03;
	
	nandWriteCmd(ERASE_BLOCK_SETUP);
	nandWriteAddr(threeCycleAddress,3);
	nandWriteCmd(ERASE_COMMAND);
	
	retval = readStatus() & 0x01;
	
	if (retval) {
		// Attempt to mark the block as bad
		markBadBlock(aBlockAddr, OUR_BAD_BLOCK_INDICATOR);
	}
	
	return retval;
}

static int isValidErasedBlock(uint16_t aBlock) {
	uint8_t retval = 0;
	uint8_t byte, dummy;
	
	if (checkBadBlock(aBlock, &byte, &dummy)) {
		// Block has been marked bad
		if ((byte > OUR_BAD_BLOCK_INDICATOR) && (byte <= MAX_PAGES_PER_BLOCK)) {
			// Take this opportunity to reset the bad block marker to our
			// default so it wont be accessed by any future reads
			if (!eraseBlock(aBlock))
				markBadBlock(aBlock, OUR_BAD_BLOCK_INDICATOR);
		}
	} else {
		// Valid block
		retval = !eraseBlock(aBlock);
	}
	return retval;
}

void resetAllValidBlocks() {
	uint16_t resetlp;
	for (resetlp = 0; resetlp < MAX_BLOCKS; resetlp++) {
		isValidErasedBlock(resetlp);
	}
}

int resetWriteIterator() {
	
	// Reset the current block and page indexes
	
	for (current_block = 0; current_block < MAX_BLOCKS; current_block++) {
		if (isValidErasedBlock(current_block)) {
			// Successfully found the first valid block and erased it
			break;
		}
	}
	if (MAX_BLOCKS == current_block) {
		// No viable blocks - end of life
		return -1;
	}
		
	current_page = FIRST_PAGE;
	backupFlashIndexs();
	return 0;
}

static int programPage(uint16_t startIdx, uint16_t endIdx) {
	
	// Handles the programming of the Nand page according to the supplied start
	// and end indexes
	
	int retval = 0;
	uint8_t fiveCycleAddress[5];
	uint16_t loop;
	
	setAddress(current_block, current_page, startIdx, fiveCycleAddress);
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE1);
	nandWriteAddr(fiveCycleAddress,5);
	
	for(loop = startIdx; loop < endIdx; loop++) {
		nandWriteByte(data_values[loop]);
	}
	
	if (PAGE_SIZE == endIdx) {
		// For simplicity, always reserve next byte as bad block indicator
		nandWriteByte(VALID_BLOCK_INDICATOR);
		
		programSpareArea();		
	}
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE2);
	
	retval = readStatus() & 0x01;
	
	if (retval) {
		// Mark block as bad, use page as marker to indicate where we got to
		markBadBlock(current_block, current_page + OUR_BAD_BLOCK_INDICATOR);
		
		current_page = MAX_PAGES_PER_BLOCK;
	}
	return retval;
}

static void provisionNextBlock() {
	
	// Find and erase the next valid block
	
	current_block++;
	
	while (current_block < MAX_BLOCKS) {
		if (isValidErasedBlock(current_block)) {
			// Successfully found a valid block and erased it
			break;
		}
		current_block++;
	}
	current_page = FIRST_PAGE;
	backupFlashIndexs();
}

int writeFirstHalfPageToFlash() {
	
	// Store the first half of the sample data to Nand flash
	
	int retval = -1;	
    
	while (retval && (current_block < MAX_BLOCKS)) {
	
		if (MAX_PAGES_PER_BLOCK == current_page) {
			// Used up all the pages in the block, provision the next block
			provisionNextBlock();
			
			if (MAX_BLOCKS == current_block) {
				// We can try no further
				break;
			}
		}
		
		retval = programPage(0, SAMPLE_HALF_PAGE);
	}
	
	return retval;
}

int writeFinalHalfPageToFlash() {
	
	// Store the remainder of the sample data to Nand flash
	
	int retval = -1;
	
	while (retval && (current_block < MAX_BLOCKS)) {
		
		retval = programPage(SAMPLE_HALF_PAGE, PAGE_SIZE);
		if (retval) {
			// Second half of page failed, attempt with a new block
			
			// Potential that the first half page has already been tainted by
			// new data but that's the risk we have to take
			
			writeFirstHalfPageToFlash(); // <- we deliberately do not set retval
		}
	}
	
	if (!retval) {
		// Increment next page to use
		current_page++;
		
		if (MAX_PAGES_PER_BLOCK == current_page) {
			// Used up all the pages in the block, provision the next block
			provisionNextBlock();			
		} else {
			backupFlashIndexs();
		}
	}
	
	return retval;
}

int resetReadIterator() {
	
	// Reset the read block and page indexes
	
	read_block = findFirstUsedBlock(&last_page_in_block);
	if (MAX_BLOCKS == read_block) {
		// No viable blocks - nothing to read
		return -1;
	}
	
	read_page = FIRST_PAGE;
	return 0;
}

int readPageToMemory(bool* aDataEnd) {

	// Read the Nand flash page to memory
	
	*aDataEnd = 0;
	
	if (MAX_BLOCKS <= read_block)
		return -1;
		
	readData(read_block, read_page, 0, data_values, (uint16_t) FLASH_PAGE);
	if (UNUSED_PAGE == data_values[PAGE_STATUS_ADDR]) {
		// End of data
		*aDataEnd = 1;
		return -2;
	}
	read_page++;
	
	if (last_page_in_block == read_page) {
		read_page = FIRST_PAGE;

		// Find the next used block
		read_block = findNextUsedBlock2(++read_block, &last_page_in_block);
		if (MAX_BLOCKS == read_block) {
			// Nothing more to read
			return -1;
		}
	}	
	return 0;
}

int readLogicalPage(uint16_t pagenum) {
	
	// Find a particular page number (absolute rather than relative to the
	// block being read)
	
	int count = 1;
	uint8_t last_page, byte, page;
	uint16_t block = findFirstUsedBlock(&last_page);
	
	if (pagenum < 1)
		return -1;
	
	while (block < MAX_BLOCKS) {

		for (page = FIRST_PAGE; page < last_page; page++) {
			if (count == pagenum) {
				// Found the required page
				bool dummy = 0;
				read_block = block;
				read_page = page;
				return readPageToMemory(&dummy);
			} else {
				readData(block, page, PAGE_STATUS_ADDR, &byte, 1);
				if (UNUSED_PAGE == byte) {
					// Unused page, failed to find required page
					return -1;
				}
				count++;
			}
		}
		
		block = findNextUsedBlock2(++block, &last_page);
	}
	return -1;
}

int doesDataExist() {
	
	// Fast check for any data, i.e stored in the first valid page in flash
	
	int exists = 0;
	uint8_t last_page, byte;
	uint16_t block = findFirstUsedBlock(&last_page);
	
	if (block < MAX_BLOCKS) {
		
		readData(block, FIRST_PAGE, PAGE_STATUS_ADDR, &byte, 1);
		if (UNUSED_PAGE != byte) {
			exists = 1;
		}
	}

	return exists;
}

// ----

// Specific storage/retrieval functions

void retrieveCurrentConfig() {
	
	// Retrieve config data from the spare area of the first page

	uint16_t block = FIRST_BLOCK;
	uint8_t byte, dummy;
	
	// Find the first good block but don't check if used or not
	while (block < MAX_BLOCKS) {
		if (!checkBadBlock(block, &byte, &dummy)) {
			// block is valid			
			break;
		}	
		block++;
	}
	
	if (block < MAX_BLOCKS) {
		// Note: rejig this if data sizes/order changes in shared.h
		uint16_t data_len = (MAX_ID_LEN * 3) + MAX_CENTRE_ID_LEN +
			MAX_CALIBRATION_DATA_LEN + MAX_SAMPLES_SIZE + EPOC_TIME_SIZE;
		uint8_t* byte_ptr = data_values;
		uint8_t loop;
		
		readData(block, FIRST_PAGE, BAND_ID_ADDR, data_values, data_len);
		if (*byte_ptr != 0xFF) {
			// Something exists, assumption here is if band id exists then the
			// rest of config data should
			for (loop = 0; loop < MAX_ID_LEN; loop++)
				current_config.band_id[loop] = *byte_ptr++;
				
			for (loop = 0; loop < MAX_ID_LEN; loop++)
				current_config.subject_id[loop] = *byte_ptr++;
				
			for (loop = 0; loop < MAX_ID_LEN; loop++)
				current_config.test_id[loop] = *byte_ptr++;
				
			for (loop = 0; loop < MAX_CENTRE_ID_LEN; loop++)
				current_config.centre_id[loop] = *byte_ptr++;
				
			for (loop = 0; loop < MAX_CALIBRATION_DATA_LEN; loop++)
				current_config.cali_data[loop] = *byte_ptr++;
				
			current_config.max_samples = *((uint32_t*) byte_ptr);
			byte_ptr += MAX_SAMPLES_SIZE;
			
			current_config.collect_start_time = *((uint32_t*) byte_ptr);
			// Check start time is valid
			if (current_config.collect_start_time > 0x7FFFFFFF)
				current_config.collect_start_time = 0;
		}
	} else {
		// Current_config should already be null from zeroConfigs call
	}	
}

void storeCurrentConfig() {
	
	// Store current config data to the spare area of the first page
	int retval;
	uint16_t loop;
	uint8_t* byte_ptr;
	uint8_t fiveCycleAddress[5];
	
	setAddress(current_block, current_page, BAND_ID_ADDR, fiveCycleAddress);
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE1);
	nandWriteAddr(fiveCycleAddress,5);
	
	programConfigData();
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE2);
	
	retval = readStatus() & 0x01;
	
	if (retval) {
		// Mark block as bad, use page as marker to indicate where we got to
		markBadBlock(current_block, current_page + OUR_BAD_BLOCK_INDICATOR);
		
		current_page = MAX_PAGES_PER_BLOCK;
	}
}

void retrieveStartEpocTime(uint32_t* aCollectStartTime) {
	uint16_t block = findNextUsedBlock(FIRST_BLOCK);	
	if (block < MAX_BLOCKS) {
		uint8_t* byte_ptr = (uint8_t*) aCollectStartTime;
		// Start collect epoc time is stored in the spare area of the first page
		readData(block, FIRST_PAGE, START_EPOC_ADDR, byte_ptr,
			EPOC_TIME_SIZE);
	} else {
		*aCollectStartTime = 0;
	}
}

uint8_t retrieveAccelConfig() {
	uint8_t retval;
	uint16_t block = findNextUsedBlock(FIRST_BLOCK);	
	if (block < MAX_BLOCKS) {
		readData(block, FIRST_PAGE, ACCEL_CONFIG_ADDR, &retval,
			ACCEL_CONFIG_SIZE);
	} else {
		// Default result to unknown settings if cannot find in flash
		retval = CWA_UNKNOWN_GS | CWA_UNKNOWN_DR;
	}
	return retval;
}

static int storeStatusU32(uint32_t aVal, uint16_t aByteAddrInPage) {
	int retval;
	uint8_t loop;
	uint8_t fiveCycleAddress[5];
	uint8_t* byte_ptr = (uint8_t*) &aVal;
	
	uint16_t block = findNextUsedBlock(FIRST_BLOCK);	
	
	// Store into reserved location
	// (note: can only be stored once, otherwise will corrupt what is already
	// there)
	setAddress(block, SECOND_PAGE, aByteAddrInPage, fiveCycleAddress);
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE1);
	nandWriteAddr(fiveCycleAddress,5);
	
	for(loop = 0; loop < sizeof(uint32_t); loop++) {
		nandWriteByte(*byte_ptr++);
	}
	
	nandWriteCmd(PROGRAM_PAGE_CYCLE2);
	
	retval = readStatus() & 0x01;
	
	if (retval) {
		// Don't set the block as bad since status info is not important
		// and want to preserve the collected data
	}
	return retval;
}

static void retrieveStatusU32(uint32_t* aVal, uint16_t aByteAddrInPage) {
	uint16_t block = findNextUsedBlock(FIRST_BLOCK);	
	if (block < MAX_BLOCKS) {
		uint8_t* byte_ptr = (uint8_t*) aVal;
		readData(block, SECOND_PAGE, aByteAddrInPage, byte_ptr,
			sizeof(uint32_t));
		if (*aVal == 0xFFFFFFFF) {
			// Assumption here is flash area not set
			*aVal = 0;
		}
	} else {
		*aVal = 0;
	}
}

int storeDownloadTime(uint32_t aDownloadTime) {
	return storeStatusU32(aDownloadTime, DLOAD_EPOC_ADDR);
}

void retrieveDownloadTime(uint32_t* aDownloadTimePtr) {
	retrieveStatusU32(aDownloadTimePtr, DLOAD_EPOC_ADDR);
}

int storeEndTick(uint32_t aEndTick) {
	return storeStatusU32(aEndTick, END_TICK_ADDR);
}

void retrieveEndTick(uint32_t* aEndTickPtr) {
	retrieveStatusU32(aEndTickPtr, END_TICK_ADDR);
}

int storeEndSamples(uint32_t aEndSamples) {
	return storeStatusU32(aEndSamples, END_SAMPLES_ADDR);
}

void retrieveEndSamples(uint32_t* aEndSamplesPtr) {
	retrieveStatusU32(aEndSamplesPtr, END_SAMPLES_ADDR);
}

void retrieveActionTime(uint32_t* aActionTimePtr) {
	retrieveStatusU32(aActionTimePtr, ACTION_EPOC_ADDR);
}

static bool isLastUsedBlock(uint16_t aBlock, uint8_t* last_page) {

	uint8_t max_page = MAX_PAGES_PER_BLOCK;
	uint8_t page, byte, dummy;
	
	// Determine max number of valid pages in this block
	int bad = checkBadBlock(aBlock, &byte, &dummy);
	if (bad) {
		if (byte > OUR_BAD_BLOCK_INDICATOR) {
			// Potentially our marking (indicates which page we got to)
			if (byte <= MAX_PAGES_PER_BLOCK) {
				max_page = byte - OUR_BAD_BLOCK_INDICATOR;
			}
		}
	}
		
	// Iterate through the pages until find first unused
	for (page = FIRST_PAGE + 1; page < max_page; page++) {
		readData(aBlock, page, PAGE_STATUS_ADDR, &byte, 1);
		if (UNUSED_PAGE == byte) {
			*last_page = page - 1;
			return 1;
		}
	}
	
	return 0;
}

uint8_t isCompleteCapture() {
	uint16_t max_samples = 0;
	uint32_t num_samples = 0;
	uint16_t block = findNextUsedBlock(FIRST_BLOCK);	
	if (block < MAX_BLOCKS) {
		uint8_t* byte_ptr = (uint8_t*) &max_samples;
		// Max samples is stored in the spare area of the first page
		readData(block, FIRST_PAGE, MAX_SAMPLES_ADDR, byte_ptr,
			MAX_SAMPLES_SIZE);
	} else {
		// No capture to check
		return NO_CAPTURE_TO_CHECK;
	}
	retrieveEndSamples(&num_samples);
	if (num_samples >= max_samples) {
		// Complete
		return COMPLETE_CAPTURE;
	}
	// Incomplete capture
	return INCOMPLETE_CAPTURE;
}

uint32_t retrieveStartTimeTicksAndSamples(uint32_t* aCollectStartTime,
	uint32_t* aNumTicks) {
	
	uint32_t num_samples = 0;
	bool found = 0;
		
	// Retrieve start collect epoc time & number of ticks and return the number
	// of samples
	
	*aCollectStartTime = 0;
	
	retrieveEndTick(aNumTicks);
	if (*aNumTicks) {
		retrieveEndSamples(&num_samples);
		if (num_samples) {
			// looks like collect ended ok
			retrieveStartEpocTime(aCollectStartTime);
			found = 1;
		}
	}
	
	if (!found) {
		uint8_t last_page;
		uint16_t block = findFirstUsedBlock(&last_page);
		
		*aNumTicks = 0;
		
		// Status details not stored - need to do it the long way ..
		
		// Do a count of the number of stored samples and scan along to the last
		// block to retrieve the end time
		
		if (block < MAX_BLOCKS) {
			uint32_t page_count = 0;
			uint8_t byte, page;
			uint16_t last_block;
			uint8_t last_used_page = MAX_PAGES_PER_BLOCK - 1;
			uint8_t* byte_ptr = (uint8_t*) aCollectStartTime;
			
			// Read the start collect epoc time
			readData(block, FIRST_PAGE, START_EPOC_ADDR, byte_ptr,
				EPOC_TIME_SIZE);
			
			// Find the last block
			while (block < MAX_BLOCKS) {
				
				last_block = block;
				
				// Count the used number of pages
				for (page = FIRST_PAGE; page < last_page; page++) {
					readData(block, page, PAGE_STATUS_ADDR, &byte, 1);
					if (UNUSED_PAGE == byte) {
						// Unused page, return the page count
						break;
					}
					page_count++;
				}
				
				if (isLastUsedBlock(last_block,&last_page))
					break;
					
				block = findNextUsedBlock2(++block, &last_page);
			}

			// Read the number of ticks from the last used page in the last used
			// block
			byte_ptr = (uint8_t*) aNumTicks;
			readData(last_block, last_page, CURRENT_TICK_ADDR, byte_ptr,
				BIOBAND_TICK_SIZE);
				
			num_samples = page_count * SAMPLES_PER_PAGE;
		}
	}

	return num_samples;
}

unsigned int readNextSetOfBatteryLevels(int aMaxSize, uint16_t* aBlockAddr) {
	
	int retval = 0;
	uint8_t dummy;
	uint16_t block = findNextUsedBlock(*aBlockAddr);
	
	// Store a set of battery level values in memory ready for transfer by USB
	
	while ((block < MAX_BLOCKS) && (retval < aMaxSize)) {
		
		// Extract the battery level from spare area of the first page
		readData(block, FIRST_PAGE, BATTERY_LEVEL_ADDR, data_values + retval,
			BATTERY_LEVEL_SIZE);
		retval += BATTERY_LEVEL_SIZE;

		if (isLastUsedBlock(block,&dummy)) {
			block = MAX_BLOCKS;
			break;
		}
		
		block = findNextUsedBlock(++block);
	}
	*aBlockAddr = block;
	
	return retval;
}

unsigned int readNextSetOfTempLevels(int aMaxSize, uint8_t* aPageAddr,
	uint8_t* aLastPageAddr, uint16_t* aBlockAddr) {
	
	int retval = 0;
	bool finish = 0;
	uint16_t block = *aBlockAddr;
	uint8_t page = *aPageAddr;
	uint8_t byte;
	
	// Store a set of temperature level values in memory ready for transfer by
	// USB
	
	while ((block < MAX_BLOCKS) && (retval < aMaxSize)) {
		
		// Check the page is used
		readData(block, page, PAGE_STATUS_ADDR, &byte, 1);
		if (UNUSED_PAGE == byte) {
			// Unused page
			block = MAX_BLOCKS;
			break;
		}
		
		// Extract the temperature level from the spare area of the page
		readData(block, page, TEMP_LEVEL_ADDR, data_values + retval,
			TEMP_LEVEL_SIZE);
		retval += TEMP_LEVEL_SIZE;

		page++;
		if (page == *aLastPageAddr) {
			// Find the next block
			block = findNextUsedBlock2(++block, aLastPageAddr);
			page = 0;
		}
	}
		
	*aBlockAddr = block;
	*aPageAddr = page;
	
	return retval;
}

uint8_t temperatureHighByte() {
	return data_values[TEMP_LEVEL_ADDR+1];
}

uint8_t temperatureLowByte() {
	return data_values[TEMP_LEVEL_ADDR];
}

uint8_t readPageHighByte() {
	return (read_page & 0xff00) >> 8;
}

uint8_t readPageLowByte() {
	return read_page & 0xff;
}

// ----

// Restore/backup functions (battery backed storage)

void restoreFlashIndexs() {
	uint8_t byte;
	
	status = status & COLLECT_LOSS_MASK;
	
	current_block = BKP_ReadBackupRegister(BKP_DR7);
	current_page = BKP_ReadBackupRegister(BKP_DR8);
	
	// Quick sanity checks
	if (current_block > MAX_BLOCKS) {
		writeStr("InvBlk");
		return;
	}
	if (current_page > MAX_PAGES_PER_BLOCK) {
		writeStr("InvPg");
		return;
	}
	
	if (current_page < MAX_PAGES_PER_BLOCK) {
		uint8_t page_used = 0;
		uint16_t bloop;
		
		// Check state of the current page
		readData(current_block, current_page, 0, data_values,
			(uint16_t) FLASH_PAGE);
			
		// Crude check for if the page is used
		for(bloop = 0; bloop < FLASH_PAGE; bloop++) {
			if (data_values[bloop] != 0xff) {
				page_used = 1;
				break;
			}
		}
		
		if (page_used) {
	
			if (UNUSED_PAGE == data_values[PAGE_STATUS_ADDR]) {
				status = status & PAGE_FAIL_MASK;
				
				// Set status
				markUsed();
			}
			
			// Increment next page to use
			current_page++;
		} else {
			// Page not used
			writeStr("PgNU");
		}
	} else {
		// Have to assume that whatever reset the band happened after the
		// page had been incremented to max pages.
		writeStr("MaxPg");
	}
	
	if (current_page == MAX_PAGES_PER_BLOCK) {
		// Used up all the pages in the block, provision the next block
		provisionNextBlock();			
	} else {
		backupFlashIndexs();
	}
}

void backupFlashIndexs() {
	BKP_WriteBackupRegister(BKP_DR7, current_block);
	BKP_WriteBackupRegister(BKP_DR8, current_page);
}

// EOF
