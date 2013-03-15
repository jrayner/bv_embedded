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

#include <stdint.h>

#ifndef _CWA_SHARED_DEFNS_H
#define _CWA_SHARED_DEFNS_H

#define BYTES_PER_SAMPLE 6
#define SAMPLES_PER_PAGE 341  // = 2046 / 6 bytes
#define SAMPLES_PER_SECOND 80

#define MAX_ID_LEN 8
#define MAX_CENTRE_ID_LEN 2

#define MAX_CALIBRATION_AXIS_PARAMS 6
#define MAX_CALIBRATION_DATA_LEN (MAX_CALIBRATION_AXIS_PARAMS * 2)

// USB vid pid

#define VENDOR_HI	0x04
#define PRODUCT_HI	0x60
#define VENDOR_LO	0x83
#define PRODUCT_LO	0x00

#define VENDOR		0x0483
#define PRODUCT 	0x6000

// ----

// Organisation

#define PAGE_SIZE 2048
#define SPARE_SIZE 64

#define MAX_PAGES_PER_BLOCK 64
#define MAX_BLOCKS			4096

#define FLASH_PAGE (PAGE_SIZE + SPARE_SIZE)
#define FLASH_BLOCK (MAX_PAGES_PER_BLOCK * FLASH_PAGE)

#define RTC_CLOCK_BASE		32768
#define RTC_SCALAR			33

// ----

// Spare area format

#define BAD_BLOCK_SIZE		1
#define PAGE_STATUS_SIZE	1
#define BIOBAND_TICK_SIZE	4
#define TEMP_LEVEL_SIZE		2
#define BATTERY_LEVEL_SIZE	2
#define MAX_SAMPLES_SIZE	4
#define EPOC_TIME_SIZE		4
#define ACCEL_CONFIG_SIZE	1
#define VERSION_SIZE		1

// Spare area addresses (all pages)
#define BAD_BLOCK_ADDR		PAGE_SIZE
#define PAGE_STATUS_ADDR	(BAD_BLOCK_ADDR + BAD_BLOCK_SIZE)
#define CURRENT_TICK_ADDR	(PAGE_STATUS_ADDR + PAGE_STATUS_SIZE)
#define TEMP_LEVEL_ADDR		(CURRENT_TICK_ADDR + BIOBAND_TICK_SIZE)

// Spare area addresses (additionals for page 0)
#define BATTERY_LEVEL_ADDR 	(TEMP_LEVEL_ADDR + TEMP_LEVEL_SIZE)
#define BAND_ID_ADDR		(BATTERY_LEVEL_ADDR + BATTERY_LEVEL_SIZE)
#define SUBJECT_ID_ADDR		(BAND_ID_ADDR + MAX_ID_LEN)
#define TEST_ID_ADDR		(SUBJECT_ID_ADDR + MAX_ID_LEN)
#define CENTRE_ID_ADDR		(TEST_ID_ADDR + MAX_ID_LEN)
#define CALIBRATION_ADDR	(CENTRE_ID_ADDR + MAX_CENTRE_ID_LEN)
#define MAX_SAMPLES_ADDR	(CALIBRATION_ADDR + MAX_CALIBRATION_DATA_LEN)
#define START_EPOC_ADDR		(MAX_SAMPLES_ADDR + MAX_SAMPLES_SIZE)
#define ACCEL_CONFIG_ADDR	(START_EPOC_ADDR + EPOC_TIME_SIZE)
#define VERSION_ADDR		(ACCEL_CONFIG_ADDR + ACCEL_CONFIG_SIZE)

// Spare area addresses (additionals for page 1)
#define DLOAD_EPOC_ADDR		(TEMP_LEVEL_ADDR + TEMP_LEVEL_SIZE)
#define END_TICK_ADDR		(DLOAD_EPOC_ADDR + EPOC_TIME_SIZE)
#define END_SAMPLES_ADDR	(END_TICK_ADDR + BIOBAND_TICK_SIZE)
#define ACTION_EPOC_ADDR	(END_SAMPLES_ADDR + MAX_SAMPLES_SIZE)

// Using 01 to distinguish from Hynix bad block marker (x00)
#define OUR_BAD_BLOCK_INDICATOR	0x01
#define VALID_BLOCK_INDICATOR	0xff

// ----

// Status byte
#define UNUSED_PAGE			0xFF // flash byte default, page hasn't been used
#define OK_USED_STATUS		0x12 // = used page, no loss, fully written

// Notionally bit 0 = used bit, set to 0 to indicate use

// bit 1  [1 = ok (fully written), 0 = fail, partially written page]	
#define PAGE_FAIL_MASK			0xFD
#define PAGE_OK_MASK			0x02

// Bit 4  [1 = ok (no loss), 0 = fail, data loss before this point]	
#define COLLECT_LOSS_MASK		0xEF
#define COLLECT_OK_MASK			0x10

// ----

// 4 spaces and two data bytes (i.e 00dd00) at start of page
#define PAGE_LEADER				6

// ----

#define OK_MSG "OK"
#define OK_MSG_LEN 2

#define ERROR_MSG "ERROR"
#define ERROR_MSG_LEN 5

// ----

// Complete codes
#define NO_CAPTURE_TO_CHECK 0
#define INCOMPLETE_CAPTURE 1
#define COMPLETE_CAPTURE 2

// ----

#ifndef LINUX
#pragma pack(1)
#endif

struct config_info {
	uint32_t max_samples;
	uint32_t actioned_time;
	uint32_t collect_start_time;
	uint32_t number_of_ticks;
	uint16_t standby_before_collection_time_mins;
	uint16_t battery_level;
	uint8_t mode;
	uint8_t flash_ok;
	uint8_t band_id[MAX_ID_LEN];
	uint8_t subject_id[MAX_ID_LEN];
	uint8_t test_id[MAX_ID_LEN];
	uint8_t centre_id[MAX_CENTRE_ID_LEN];
	uint8_t cali_data[MAX_CALIBRATION_DATA_LEN];
#ifdef LINUX
}  __attribute__((packed));
#else
};
#endif

typedef enum _cwa_mode
{
	CWA_UNKNOWN_MODE,
	CWA_REAL_MODE,
	CWA_TEST_MODE,
	CWA_NUM_OF_MODES
} cwa_mode;

// Simplistic control chars
#define GO_CHAR					'g'
#define CONFIG_CHAR				'#'
#define SEND_META_CHAR			'M'
#define SEND_FULL_META_CHAR		'N'
#define START_CHAR				's'
#define READ_DATA_CHAR			'D'
#define SETUP_DUMMY_DATA_CHAR	'd'
#define BATTERY_LEVELS_CHAR		'L'
#define TEMPERATURE_LEVELS_CHAR 't'
#define READ_BAD_BLOCKS_CHAR	'B'
#define TEST_FLASH_CHAR			'F'
#define ID_CHAR					'I'
#define RETURN_CHAR				'r'
#define STREAMING_CHAR			'S'
#define BATTERY_BURN_CHAR		'Z'
#define GET_VERSION_CHAR		'V'
#define GET_DEVICE_TIME_CHAR	'T'
#define GET_MEASUREMENT_CHAR	'm'
#define ERASE_FLASH_CHAR		'X'
#define SET_LED_COLOUR_CHAR		'l'
#define GO_TO_SLEEP_CHAR		'z'
#define READ_PAGE_CHAR			'p'
#define READ_NEXT_PAGE_CHAR		'q'
#define READ_RAW_CHAR			'R'
#define WIPE_BKP_CHAR			'W'
#define READ_DBG_CHAR			'b'
#define SET_ACCEL_CONFIG_CHAR	'A'
#define READ_ACCEL_CONFIG_CHAR	'a'
#define READ_FLASH_ACCEL_CHAR	'f'
#define SET_CALIB_CHAR			'K'
#define SET_FIRST_DOWNLD_CHAR	'J'
#define GET_FIRST_DOWNLD_CHAR	'j'
#define GET_IS_COMPLETE_CHAR	'C'

// Table of CRC values for high-order byte
extern uint8_t table_crc_hi[256];

// Table of CRC values for low-order byte
extern uint8_t table_crc_lo[256];

typedef enum _accel_data_rate
{
	CWA_UNKNOWN_DR = 0x00,
	CWA_50HZ = 0x01,
	CWA_100HZ = 0x02,
	CWA_400HZ = 0x03,
	CWA_1000HZ = 0x04
} accel_data_rate;

typedef enum _accel_g_scale
{
	CWA_UNKNOWN_GS = 0x00,
	CWA_2G = 0x10,
	CWA_4G = 0x20,
	CWA_8G = 0x30
} accel_g_scale;

uint8_t encodeRateAndGscale(accel_data_rate rate, accel_g_scale scale);
int decodeRateAndGscale(uint8_t byte, accel_data_rate* rate,
	accel_g_scale* scale);
	
#define FW_VER_BITS		5
#define FW_MASK			0x1F
#define HW_VER_BITS		3
#define HW_MASK			0x07

#endif // _CWA_SHARED_DEFNS_H
