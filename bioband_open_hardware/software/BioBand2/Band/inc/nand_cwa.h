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
 */

#ifndef _NAND_CWA_H
#define _NAND_CWA_H

#include "shared.h"

#define SAMPLE_HALF_PAGE 1026 // roughly half page in 6 byte units
#define SAMPLE_FULL_PAGE 2046

// ----

struct nand_identity {
	uint8_t maker;
	uint8_t device;
	uint8_t third;
	uint8_t fourth;
	uint8_t fifth;
};

// Init GPIO
void nand_cwa_init();

void readDeviceId(struct nand_identity* aResult);

uint16_t findNextBadBlock(uint16_t aStartBlockAddr,uint8_t* aByte,
	uint8_t* aPage);
uint16_t findFirstUsedBlock(uint8_t* aLastPageAddr);

int resetIfOurBadBlock(uint16_t aBlock);

void resetAllValidBlocks();

int resetWriteIterator();
int writeFirstHalfPageToFlash();
int writeFinalHalfPageToFlash();

int resetReadIterator();
int readPageToMemory(bool* aDataEnd);

int readLogicalPage(uint16_t pagenum);

int doesDataExist();
int countNumberUsedPages();

void retrieveCurrentConfig();
void storeCurrentConfig();

void retrieveStartEpocTime(uint32_t* aCollectStartTime);
uint8_t retrieveAccelConfig();
uint8_t retrieveVersionData();

int storeDownloadTime(uint32_t aDownloadTime);
void retrieveDownloadTime(uint32_t* aDownloadTimePtr);
int storeEndTick(uint32_t aEndTick);
void retrieveEndTick(uint32_t* aEndTickPtr);
int storeEndSamples(uint32_t aEndSamples);
void retrieveEndSamples(uint32_t* aEndSamplesPtr);
void retrieveActionTime(uint32_t* aActionTimePtr);

uint8_t isCompleteCapture();

uint32_t retrieveStartTimeTicksAndSamples(uint32_t* aCollectStartTime,
	uint32_t* aNumTicks);
unsigned int readNextSetOfBatteryLevels(int aMaxSize, uint16_t* aBlockAddr);

unsigned int readNextSetOfTempLevels(int aMaxSize, uint8_t* aPageAddr,
	uint8_t* aLastPageAddr, uint16_t* aBlockAddr);

uint8_t temperatureHighByte();
uint8_t temperatureLowByte();
uint8_t readPageHighByte();
uint8_t readPageLowByte();

void restoreFlashIndexs();
void backupFlashIndexs();

#endif //_NAND_CWA_H
