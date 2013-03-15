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
 */

#include "stm32f10x.h"
#include "usb_lib.h"
#include "usb_desc.h"
#include "hw_config.h"
#include "usb_pwr.h"
#include "nand_cwa.h"
#include "shared.h"
#include "accel.h"
#include "tempsensor.h"
#include "debug.h"

// ----

// Leave space, H & Fs in this string
char version_str[] = " HHHHFFFF" __DATE__ " " __TIME__;

// ----

// * UPDATE these enums and getVerFloats when upgrading the fw and/or hw *

// Note the space and therefore the number of versions is limited for fw & hw
// but this only concerns bands in the field. If you ensure that very old bands
// are reflashed or discarded then this system should be sufficient, i.e numbers
// can be reused if required.

typedef enum _firmware_version
{
	CWA_FW_1_0 = 0x00,
	CWA_FW_1_1 = 0x01, 	// Change of architecture
	CWA_FW_1_2 = 0x02, 	// Bugfixes from knobbly long running tests
	CWA_FW_1_3 = 0x03, 	// Added isCompleteCapture api & removed PC/Band version 
						// dependency
	// Five bits, space for 28 more firmware versions in the field
	CW_CURRENT_FW = CWA_FW_1_3,
} firmware_version;

typedef enum _hardware_version
{
	CWA_HW_1_2 = 0x0,
	// Three bits, space for 7 hardware versions in the field
	CW_CURRENT_HW = CWA_HW_1_2,
} hardware_version;

void getVerFloats(float* hw_ptr, float* fw_ptr) {
	switch(CW_CURRENT_FW) {
		case CWA_FW_1_0: *fw_ptr = 1.0; break;
		case CWA_FW_1_1: *fw_ptr = 1.1; break;
		case CWA_FW_1_2: *fw_ptr = 1.2; break;
		case CWA_FW_1_3: *fw_ptr = 1.3; break;
		default: // unknown?
			*fw_ptr = 0.0;
			break;
	}
	switch(CW_CURRENT_HW) {
		case CWA_HW_1_2: *hw_ptr = 1.2; break;
		default: // unknown?
			*hw_ptr = 0.0;
			break;
	}
}

// ----

// Defines

#define SIXTY_SECONDS 60

#define INITIAL_TRANSFER_SIZE 60

#define MAX_CMD_WAIT_SECS 300

// Ensure collect isn't interrupted if plugged into the cable
#define IGNORE_WAKEUP_DURING_COLLECT

// ----

// Trigger points during collection (see Accelerometer interrupt handler)

#define READ_BATT_AND_TEMP (SAMPLE_FULL_PAGE - BYTES_PER_SAMPLE)

// ADC trigger point:
// give the ADC one sample period to take a battery reading
#define START_BATTERY_MEASUREMENT (READ_BATT_AND_TEMP - BYTES_PER_SAMPLE)

// Temperature trigger points (including additional 10%):
// The thermometer takes up to 228ms to read the temperature, so we start it
// that long before writing a page
#define START_TEMPERATURE_MEASUREMENT_50HZ \
	(READ_BATT_AND_TEMP - (13 * BYTES_PER_SAMPLE))
#define START_TEMPERATURE_MEASUREMENT_100HZ \
	(READ_BATT_AND_TEMP - (26 * BYTES_PER_SAMPLE))
#define START_TEMPERATURE_MEASUREMENT_400HZ \
	(READ_BATT_AND_TEMP - (102 * BYTES_PER_SAMPLE))
#define START_TEMPERATURE_MEASUREMENT_1000HZ \
	(READ_BATT_AND_TEMP - (252 * BYTES_PER_SAMPLE))

// ----

// Global variables

struct config_info current_config;
struct config_info last_config;

extern __IO uint32_t count_in; // defined in usb_endp.c
extern uint8_t buffer_in[SIMPLE_RX_DATA_SIZE];

bool led_state = 1;
bool usb_configured = 0;

volatile uint32_t data_count = 0;
volatile bool proceed_as_normal = 1;

volatile bool lastHalfPage = 0;
volatile bool firstHalfPage = 0;

uint16_t currentBatteryVoltage = 0;
uint16_t currentTemperature = 0;

int b_index = 0;
uint8_t data_values[FLASH_PAGE];

uint32_t start_page_tick = 0;

// Wait for config standby count
volatile uint16_t standby_count_secs;

volatile int transfer_size;

volatile bool simple_tx_read_check = 0;

uint16_t readBlockAddr = 0;
uint8_t pageAddr = 0;
uint8_t lastPageAddr = 0;

extern uint8_t accel_rate_and_g_scale; // accel.c
extern volatile accel_data_rate current_accel_rate; // accel.c

extern uint8_t version_byte; // nand_cwa.c

bool adc_configured = 0;

// Standby for collect remaining minutes
volatile uint32_t remaining_mins = 0;

// ----

// States

typedef enum _cwa_state
{
	CWA_UNKNOWN,
	CWA_STANDBY_FOR_CONFIG,
	CWA_CONFIG,
	CWA_READ_PAGE,
	CWA_READ_RAW,
	CWA_READ_BATTERY_LEVELS,
	CWA_READ_TEMPERATURE_LEVELS,
	CWA_READ_DBG,
	CWA_READ_BAD_BLOCKS,
	CWA_STANDBY_FOR_COLLECT,
	CWA_COLLECT,
	CWA_SLEEP,
	CWA_NUM_OF_STATES
} cwa_state;

volatile cwa_state current_state = CWA_UNKNOWN;

// ----

// Simple helper functions

static void powerDown() {
	PWR_EnterSTOPMode(PWR_Regulator_LowPower, PWR_STOPEntry_WFI);
}

static void enableUSB() {
	if (!usb_configured) {
		USB_ConfigSerialNum(current_config.band_id);
		Set_USBClock();
		//PowerOn();
		USB_Interrupts_Config();
		USB_Init();
		USB_Cable_Config(ENABLE);
		usb_configured = 1;
	}
}

static void send_ok_result() {
	USB_Send_Data(OK_MSG, OK_MSG_LEN);
}

static void send_error_result() {
	USB_Send_Data(ERROR_MSG, ERROR_MSG_LEN);
}

static void send_done() {
	USB_Send_Data("Done", 4);
}


// ----

// Interrupt handlers

// Simplistic accelerometer interrupt count
volatile uint8_t tcount = 0;

void accelIntrHandler(void) {
	
	/* 
	 * Accelerometer interrupt handler
	 * 
	 * Accelerometer interrupts are generated only when the device has entered
	 * collect state.
	 *  
	 * The b_index variable is used to co-ordinate the temperature and battery
	 * level sensor measurements need to be triggered at the correct times to
	 * allow for settling times of the sensors. In the same way, sample data is
	 * written to flash in half page quantities to allow new samples to be
	 * recorded without the need for an additional buffer.
	 */
	
	bool enable_thermometer = 0;

	if (CWA_TEST_MODE == current_config.mode) {
		tcount++;
		if (tcount == 50) {
			BlueLed(1);
		} else if (tcount >= 100) {
			tcount = 0;
			BlueLed(0);
		}
	}
	
	// Note high byte is read first in this mode
	accelReadingBuf((uint8_t*)&(data_values[b_index]));

	b_index+=6;
	data_count++;
	
	// A 'task list' for things to do while we're recording data
	switch(b_index) {
	case SAMPLE_HALF_PAGE:
		firstHalfPage = 1;
		break;
	case SAMPLE_FULL_PAGE:
		b_index = 0;
		lastHalfPage = 1;
		break;
		
#ifdef ENABLE_TEMPERATURE
	case START_TEMPERATURE_MEASUREMENT_50HZ:
		enable_thermometer = (CWA_50HZ == current_accel_rate);
		break;
	case START_TEMPERATURE_MEASUREMENT_100HZ:
		enable_thermometer = (CWA_100HZ == current_accel_rate);
		break;
	case START_TEMPERATURE_MEASUREMENT_400HZ:
		enable_thermometer = (CWA_400HZ == current_accel_rate);
		break;
	case START_TEMPERATURE_MEASUREMENT_1000HZ:
		enable_thermometer = (CWA_1000HZ == current_accel_rate);
		break;
#endif
		
#ifdef ENABLE_BATTERY_LEVEL
	case START_BATTERY_MEASUREMENT:
		ADC1_Configuration();
		break;
#endif
		
	case READ_BATT_AND_TEMP:
#ifdef ENABLE_BATTERY_LEVEL
		currentBatteryVoltage=getADC1Channel();
#endif
#ifdef ENABLE_TEMPERATURE
		SPI_Configuration_TempSensor();
		currentTemperature=tempReading();
		// Disable thermometer to save power
		tempDisable();
		SPI_Configuration_Accelerometer(0);
#endif
#ifdef ENABLE_BATTERY_LEVEL
		// Shut down ADC
		ADC1_Shutdown();
#endif
		break;
	}
	
	if (enable_thermometer) {
		// Enable thermometer
		SPI_Configuration_TempSensor();
		tempEnable();
		SPI_Configuration_Accelerometer(0);
	}
}

void RTCIntrHandler(void) {
	
	/*
	 * Second/Alarm RTC interrupt handler
	 * 
	 * The RTC interrupts are enabled when the device is in full power mode
	 * (i.e not in collect mode).
	 * The interrupts drive the diagnostic LED flashing and simple timeouts
	 * (second or minute based depending on the configuration of the RTC at that
	 * point in time).
	 */
	
	// Simple timeouts
	if (CWA_STANDBY_FOR_CONFIG == current_state) {
		standby_count_secs++;
	} else if (CWA_STANDBY_FOR_COLLECT == current_state) {
		if (remaining_mins) {
			remaining_mins--;
			saveValue(BKP_DR3, remaining_mins);
		}
	}

	// Simplistic diagnostic leds
	if (CWA_TEST_MODE == current_config.mode) {
		GreenLed(0);
		RedLed(0);
		BlueLed(0);
	
		if (!led_state) {
			led_state = 1;
		} else {
			led_state = 0;
			if (CWA_CONFIG == current_state) {
				GreenLed(1);
			} else if (CWA_STANDBY_FOR_CONFIG == current_state) {
				RedLed(1);
			} else if (CWA_STANDBY_FOR_COLLECT == current_state) {
				RedLed(1);
				BlueLed(1);
			}
		}
	}
}

void usbWakeUpIntrHandler() {
	
	// USB lead insertion interrupt handler
	
#ifdef IGNORE_WAKEUP_DURING_COLLECT
	// Ignore usb wakeup if currently standby or collecting data
	if (CWA_COLLECT == current_state ||
		CWA_STANDBY_FOR_COLLECT == current_state) {
		writeStr("IgnUWk");
		return;
	}
#endif
	// Ensure clocks working ok for USB transfer
	setUpClocks();
	
	// And for all the other peripherals
	clocksNormal();
	
	data_count = doesDataExist();

#ifdef ENABLE_BATTERY_LEVEL
	// Configure adc before needed
	if (!adc_configured) {
		ADC1_Configuration();
		adc_configured = 1;
	}
#endif

	writeStr("UUp");	
	proceed_as_normal = 0;
}

// End of interrupt handlers

// ----

// State definitions

static cwa_state waitForConfig() {
	
	// A full power state. Wait for go char from USB comms otherwise go back
	// to sleep if no comms in defined period.
	
	writeStr("w8C");
	proceed_as_normal = 1;

	enableUSB();
	
#ifdef ENABLE_BATTERY_LEVEL
	// configure adc before needed
	if (!adc_configured) {
		ADC1_Configuration();
		adc_configured = 1;
	}
#endif

	RTC_ITConfig(RTC_IT_SEC, ENABLE);
    RTC_WaitForLastTask();
    
	standby_count_secs = 0;

	while (standby_count_secs < MAX_CMD_WAIT_SECS) {
		
		if (bDeviceState == CONFIGURED && count_in != 0) {
			writeStr(">");
			USB_Send_Data(buffer_in, count_in);
			if (buffer_in[0] == GO_CHAR) {
				writeStr("*");
				standby_count_secs = 0;
				break;
			} else {
				writeStr("!");
			}
			count_in = 0;
		}
	}
	
	if (proceed_as_normal && standby_count_secs >= MAX_CMD_WAIT_SECS) {
		// Go to sleep if no go char in alotted time
		return CWA_SLEEP;
	}
	proceed_as_normal = 1;
	
	return CWA_CONFIG;
}

static cwa_state readBatteryLevels() {
	unsigned int number = 0;
	
	// A full power state. Simplistic transfer of battery level details to the
	// PC.
	
	if (readBlockAddr < MAX_BLOCKS) {
		
		number = readNextSetOfBatteryLevels(transfer_size,
			&readBlockAddr);
		if (number) {
			USB_Send_Data(data_values, number);
		}
	}
	if (!number) {
		current_state = CWA_STANDBY_FOR_CONFIG;
		send_done();
	}
	return current_state;
}

static cwa_state readTemperatureLevels() {
	unsigned int number = 0;
	
	// A full power state. Simplistic transfer of temperature level details to
	// the PC.
	
	if (readBlockAddr < MAX_BLOCKS) {
		
		number = readNextSetOfTempLevels(transfer_size, &pageAddr,
			&lastPageAddr, &readBlockAddr);
		if (number) {
			USB_Send_Data(data_values, number);
		}
	}
	if (!number) {
		current_state = CWA_STANDBY_FOR_CONFIG;
		send_done();
	}
	return current_state;
}

static cwa_state readBadBlocks() {
	
	// A full power state. Simplistic transfer of bad block details to the PC.
	
	if (readBlockAddr < MAX_BLOCKS) {
		
		uint8_t byte = 0;
		uint8_t page = 0;
		uint8_t tmp_data[6];
		readBlockAddr = findNextBadBlock(readBlockAddr,&byte,&page);
		
		tmp_data[0] = 'b';
		tmp_data[1] = (readBlockAddr >> 8) & 0xff;
		tmp_data[2] = readBlockAddr & 0xff;
		tmp_data[3] = page;
		tmp_data[4] = byte;
		tmp_data[5] = '\n';
		USB_Send_Data(tmp_data, 6);
		
		readBlockAddr++;
	} else {
		current_state = CWA_STANDBY_FOR_CONFIG;
	}
	
	return current_state;
}

static cwa_state readDebug() {
	
	// A full power state. Simplistic transfer of debug buffer details to the PC
	
	uint8_t rsz = getReadSize(INITIAL_TRANSFER_SIZE);
	
	if (rsz) {
		uint8_t tmp_data[INITIAL_TRANSFER_SIZE];
		uint8_t loop, stop = 0;
		for (loop = 0; loop < rsz; loop++) {
			tmp_data[loop] = readDbgByte(&stop);
			if (stop)
				break;
		}
		USB_Send_Data(tmp_data, loop);
	} else {
		current_state = CWA_STANDBY_FOR_CONFIG;
		send_done();
	}
	
	return current_state;
}

static cwa_state configure() {
	
	/*
	 * A full power state. Allow the user to configure the device and
	 * potentially. This simplistic protocol could do with refactoring to
	 * improve readability and error handling.
	 */
	
	cwa_state move_to_state = CWA_CONFIG;
	
	GreenLed(0);
	RedLed(0);
	
	RTC_ITConfig(RTC_IT_SEC, ENABLE);
    RTC_WaitForLastTask();
	
	// Send current meta data & configuration to PC
	if (data_count && CWA_REAL_MODE == current_config.mode) {
		USB_Send_Data("CWA1\n", 5);
	} else {
		USB_Send_Data("CWAz\n", 5);
	}
	
	// Process commands from the PC
	while (proceed_as_normal) {
		
		if (bDeviceState == CONFIGURED && count_in != 0) {
			int break_out = 1;
			
			writeHex8(buffer_in[0]);
			
			switch(buffer_in[0]) {
				
			case CONFIG_CHAR:
				if (count_in > sizeof(struct config_info)) {
					// Assumption that data is a config_info struct
					uint16_t bkp_mode;
					struct config_info* msg =
						(struct config_info*) &buffer_in[1];
					current_config.max_samples = msg->max_samples;
					current_config.standby_before_collection_time_mins =
						msg->standby_before_collection_time_mins;
					current_config.actioned_time = msg->actioned_time;
					
					// Set the RTC with current time
					RTC_SetCounter(current_config.actioned_time);
					RTC_WaitForLastTask();
					
					current_config.mode = msg->mode;
					bkp_mode = current_config.mode;
					BKP_WriteBackupRegister(BKP_DR9, bkp_mode);
					if (msg->subject_id[0]) {
						uint8_t loop;
						for (loop = 0; loop < MAX_ID_LEN; loop++) {
							current_config.subject_id[loop] =
								msg->subject_id[loop];
						}
					}
					if (msg->test_id[0]) {
						uint8_t loop;
						for (loop = 0; loop < MAX_ID_LEN; loop++) {
							current_config.test_id[loop] =
								msg->test_id[loop];
						}
					}
					if (msg->centre_id[0]) {
						uint8_t loop;
						for (loop = 0; loop < MAX_CENTRE_ID_LEN; loop++) {
							current_config.centre_id[loop] =
								msg->centre_id[loop];
						}
					}
					if (CWA_TEST_MODE == current_config.mode)
						gpioEnableLeds();
					send_ok_result();
				} else {
					send_error_result();
				}
				
				// Expect another command to follow
				break_out = 0;
				break;
				
			case SEND_FULL_META_CHAR:
			case SEND_META_CHAR: {
					// Send meta data 
					char metachars[SIMPLE_TX_DATA_SIZE];
					uint8_t* last_conf_ptr = (uint8_t *) &last_config;
					uint8_t* current_ptr = (uint8_t *) &current_config;
					int rdlen = sizeof(last_config);
					int loop;
					uint8_t* ptr = &metachars[1];
					metachars[0] = CONFIG_CHAR;
					
					for (loop = 0; loop < rdlen; loop++) {
						*last_conf_ptr++ = *current_ptr++;
					}

#ifdef ENABLE_BATTERY_LEVEL
					last_config.battery_level = getADC1Channel();
#else
					last_config.battery_level = 0;
#endif
					
					if (buffer_in[0] == SEND_FULL_META_CHAR) {
						// Slower option - if unexpected ending then may need
						// to search through all the flash data for the end
						data_count = retrieveStartTimeTicksAndSamples(
							&last_config.collect_start_time,
							&last_config.number_of_ticks);
							
					} else {
						// Ensure the start time is populated if known
						retrieveStartEpocTime(
							&last_config.collect_start_time);
					}
					last_config.max_samples = data_count;
					
					last_conf_ptr = (uint8_t *) &last_config;
					for (loop = 0; loop < rdlen; loop++)
						*ptr++ = *last_conf_ptr++;
					USB_Send_Data(metachars, rdlen+1);
					
					// TODO check use of full meta is always a standalone op
					//if (buffer_in[0] == SEND_FULL_META_CHAR) {
					//	move_to_state = CWA_STANDBY_FOR_CONFIG;
					// } else {
						// expect another command to follow
						break_out = 0;
					// }
				} break;
	  		
			case START_CHAR:
				if (resetWriteIterator()) {
					send_error_result();
				} else {
					// Store current config to last
					int loop, rdlen = sizeof(last_config);
					uint8_t* current_ptr = (uint8_t *) &current_config;
					uint8_t* last_ptr = (uint8_t *) &last_config;
					for (loop = 0; loop < rdlen; loop++)
						*last_ptr++ = *current_ptr++;
					
					data_count = 0;
					
					// Store config into flash now in case ExtR or similar
					// before collect starts, but don't store the start time
					current_config.collect_start_time = 0;
					storeCurrentConfig();
	
					// Move to collection or standby for collection
					if (current_config.standby_before_collection_time_mins){
						USB_Send_Data("Standby", 7);
						remaining_mins =
							current_config.standby_before_collection_time_mins;
						
						// Move to CWA_STANDBY_FOR_COLLECT state
						move_to_state = CWA_STANDBY_FOR_COLLECT;
					} else {
						USB_Send_Data("Collecting", 10);
						
						// Move to CWA_COLLECT state
						move_to_state = CWA_COLLECT;
					}
					
					// Disable USB now since USB interrupts appear to effect the
					// collect process
					USB_Interrupts_Disable();
					
					// Switch off USB
					PowerOff();
					usb_configured = 0;
					
					// Go to low power state
					if (CWA_TEST_MODE != current_config.mode)
						gpioMinimumPower();
					clocksMinimumPower();
				}
				break;
				
			case READ_PAGE_CHAR: {
					// Page by page read
					uint16_t pagenum = (buffer_in[1] << 8) + buffer_in[2];
					transfer_size = INITIAL_TRANSFER_SIZE;
					b_index = 0;
					
					if (readLogicalPage(pagenum)) {
						send_error_result();
					} else {
						int loop;
						uint8_t tmp[6];
						for (loop = 0; loop < 6; loop++) {
							tmp[loop] = 0;
						}

						// Temperature data has been shoe horned as a late
						// requirement into the settling pattern at the start of
						// the page
						
						// Send initial data
						tmp[2] = temperatureHighByte();
						tmp[3] = temperatureLowByte();
						USB_Send_Data(tmp, 6);
						
						// Move to CWA_READ_PAGE state
						move_to_state = CWA_READ_PAGE;
					}	
				} break;

			case READ_NEXT_PAGE_CHAR: {
					// Slightly faster read next page if previously used
					// READ_PAGE_CHAR
					bool data_end = 0;					
					transfer_size = INITIAL_TRANSFER_SIZE;
					b_index = 0;
					
					if (readPageToMemory(&data_end)) {
						send_error_result();
					} else {
						int loop;
						uint8_t tmp[6];
						for (loop = 0; loop < 6; loop++) {
							tmp[loop] = 0;
						}

						// Send settling pattern & temperature data
						tmp[2] = temperatureHighByte();
						tmp[3] = temperatureLowByte();
						USB_Send_Data(tmp, 6);
						
						// Move to CWA_READ_PAGE state
						move_to_state = CWA_READ_PAGE;
					}	
				} break;

			case READ_RAW_CHAR: {
					// Read all the flash contents out to the PC
					bool data_end = 0;					
					transfer_size = INITIAL_TRANSFER_SIZE;
					b_index = 0;
					simple_tx_read_check = 0;			
					if (resetReadIterator()) {
	
						send_error_result();
	
					} else {
						int loop;
						uint8_t tmp[6];
						for (loop = 0; loop < 6; loop++) {
							tmp[loop] = 0;
						}

						// Send settling pattern & page number
						tmp[2] = readPageHighByte();
						tmp[3] = readPageLowByte();
						
						if (readPageToMemory(&data_end)) {

							send_error_result();
		
						} else {
							transfer_size = INITIAL_TRANSFER_SIZE;
						
							// Move to CWA_READ_RAW state
							move_to_state = CWA_READ_RAW;
							current_state = CWA_READ_RAW;
							USB_Send_Data(tmp, 6);
						}
					}
				} break;

			case BATTERY_LEVELS_CHAR:
				transfer_size = INITIAL_TRANSFER_SIZE;
				simple_tx_read_check = 0;			
				readBlockAddr = 0;			

				current_state = CWA_READ_BATTERY_LEVELS;
				// If successful, move to CWA_READ_BATTERY_LEVELS state
				move_to_state = readBatteryLevels();
				break;

			case TEMPERATURE_LEVELS_CHAR:
				transfer_size = INITIAL_TRANSFER_SIZE;
				simple_tx_read_check = 0;
				pageAddr = 0;
				lastPageAddr = 0;
				readBlockAddr = findFirstUsedBlock(&lastPageAddr);

				current_state = CWA_READ_TEMPERATURE_LEVELS;
				// If successful, move to CWA_READ_TEMPERATURE_LEVELS state
				move_to_state = readBatteryLevels();
				move_to_state = readTemperatureLevels();
				break;

			case READ_BAD_BLOCKS_CHAR:
				simple_tx_read_check = 0;			
				readBlockAddr = 0;	
						
				current_state = CWA_READ_BAD_BLOCKS;
				// If successful, move to CWA_READ_BAD_BLOCKS state
				move_to_state = readBadBlocks();
				break;

			case ID_CHAR: {
					// Set identity
					uint8_t loop;
					char* ptr = buffer_in + 1;
					for (loop = 0; loop < MAX_ID_LEN; loop++) {
						current_config.band_id[loop] = *ptr++;
					}
					// NOTE in order to write the id to flash, the user should
					// run a quick test capture
					move_to_state = CWA_STANDBY_FOR_CONFIG;
					send_ok_result();
				} break;

			case RETURN_CHAR:
				// Return to standby for config. Used if the PC end wishes to
				// cancel the current configured state and return to the start.
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				break;
				
			case GET_VERSION_CHAR: {
					int count = 0;
					float hw_ver, fw_ver;
					uint8_t* fl_ptr;
					uint8_t loop;
					char* ptr = version_str;
					while (ptr && *ptr) {
						count++;
						ptr++;
					}
					// Slightly overcomplicated version retrieval, due to having
					// older devices in the field when the versioning system was
					// altered.
					
					// Use first byte as marker (new version format, fw > 1.2)
					version_str[0] = 0;
					move_to_state = CWA_STANDBY_FOR_CONFIG;
					getVerFloats(&hw_ver, &fw_ver);
					fl_ptr = (uint8_t*) &hw_ver;
					for(loop = 1; loop < 5; loop++)
						version_str[loop] = *fl_ptr++;
					fl_ptr = (uint8_t*) &fw_ver;
					for(loop = 5; loop < 9; loop++)
						version_str[loop] = *fl_ptr++;
					USB_Send_Data(version_str, count+1);
					for(loop = 0; loop < 9; loop++)
						version_str[loop] = ' ';
				} break;
				
			case GET_DEVICE_TIME_CHAR: {
				// Relatively useless functionality as the device usually needs
				// a recharge before downloading data
				uint32_t current_time = RTC_GetCounter();
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				USB_Send_Data(&current_time, sizeof(uint32_t)+1);
				} break;
				
			case ERASE_FLASH_CHAR:
				// Move to CWA_STANDBY_FOR_CONFIG state
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				resetAllValidBlocks();
				send_ok_result();
				break;
				
			case SET_LED_COLOUR_CHAR: {
					// Simplistic setting of led colour according to data byte
					uint8_t led_setting = buffer_in[1];
					current_config.mode = CWA_REAL_MODE;
					gpioEnableLeds();
					BlueLed(0);
					GreenLed(0);
					RedLed(0);
					if (led_setting & 0x01)
						BlueLed(1);
					if (led_setting & 0x02)
						GreenLed(1);
					if (led_setting & 0x04)
						RedLed(1);
					send_ok_result();
					count_in = 0;
				}
				// Return here to avoid GreenLed(0) at end of the function
				return CWA_STANDBY_FOR_CONFIG;
				
			case GO_TO_SLEEP_CHAR:
				// Move to CWA_SLEEP state
				move_to_state = CWA_SLEEP;
				break;
				
			case WIPE_BKP_CHAR:
				// Simple action to reset the backup domain flag
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				BKP_WriteBackupRegister(BKP_DR1, 0xFF);
				send_ok_result();
				break;
				
			case READ_DBG_CHAR:
				simple_tx_read_check = 0;			
				current_state = CWA_READ_DBG;
				// If successful, move to CWA_READ_DBG state
				move_to_state = readDebug();
				break;
				
			case SET_ACCEL_CONFIG_CHAR:
				// Simple action to set the accelerometer & g scale config
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				accel_rate_and_g_scale = buffer_in[1];
				send_ok_result();
				break;
				
			case READ_ACCEL_CONFIG_CHAR:
				// Simple action to read the accelerometer & g scale config
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				USB_Send_Data(&accel_rate_and_g_scale, 1);
				break;
				
			case READ_FLASH_ACCEL_CHAR: {
				// Simple action to read the accelerometer & g scale config
				// for the data stored in flash
				uint8_t flash_accel_val = retrieveAccelConfig();
				move_to_state = CWA_STANDBY_FOR_CONFIG;
				USB_Send_Data(&flash_accel_val, 1);
				} break;
				
			case SET_CALIB_CHAR: {
					// Set calibration data
					uint8_t loop;
					uint8_t* ptr = (uint8_t*) (buffer_in + 1);
					for (loop = 0; loop < MAX_CALIBRATION_DATA_LEN; loop++) {						
						current_config.cali_data[loop] = *ptr++;
					}
					// NOTE in order to write the data to flash the user should
					// run a quick test capture
					move_to_state = CWA_STANDBY_FOR_CONFIG;
					send_ok_result();
				} break;
				
			case SET_FIRST_DOWNLD_CHAR: {
					// Simple action to set the first download time
					uint32_t check = 0;
					retrieveDownloadTime(&check);
					if (check) {
						// First downloaded time already set
						send_error_result();
					} else {
						uint32_t* ptr = (uint32_t*) (buffer_in + 1);
						if (storeDownloadTime(*ptr))
							send_error_result();
						else
							send_ok_result();
					}
					move_to_state = CWA_STANDBY_FOR_CONFIG;
				} break;
				
			case GET_FIRST_DOWNLD_CHAR: {
					// Simple action to read the first download time
					uint32_t dt = 0;
					retrieveDownloadTime(&dt);
					USB_Send_Data(&dt, sizeof(uint32_t)+1);
					move_to_state = CWA_STANDBY_FOR_CONFIG;
				} break;
				
			case GET_IS_COMPLETE_CHAR: {
					// Simple action to read whether the last capture completed
					// successfully
					uint8_t val = isCompleteCapture();
					move_to_state = CWA_STANDBY_FOR_CONFIG;
					USB_Send_Data(&val, 1);
				} break;
				
			case GO_CHAR:
				// TODO this should not be necessary
				// make sure we don't get stuck in this state if the CWA PC
				// software thinks we shouldn't be in it
				writeStr("CMJ");
				move_to_state = CWA_CONFIG;
				break;
				
			default:
				// Ignore unexpected char
				// TODO move_to_state = CWA_STANDBY_FOR_CONFIG;
				// & return an error?
				break;
	  		}
	  		if (break_out)
	  			break;
			count_in = 0;
		}
	}
	
	count_in = 0;
	GreenLed(0);

	return move_to_state;
}

void ready_for_tx_cb() {
	
	// Read helper, see waitForEndOfRead state below.
		
	if (!proceed_as_normal) {
		current_state = CWA_STANDBY_FOR_CONFIG;
		send_done();
		simple_tx_read_check = 0;
		return;
	}
	
	if (simple_tx_read_check) {
		// Just in case get a very quick tx callback
		RedLed(1);
		return;
	}
	simple_tx_read_check = 1;
	
	switch (current_state) {
		case CWA_READ_RAW:	
			if (b_index < 2100) {
				USB_Send_Data(data_values + b_index, transfer_size);
				b_index += transfer_size;
			} else {
				bool data_end = 0;
				// Last 12 bytes + (Done or leading zeroes)
				uint8_t loop, tmp[18];
				uint8_t* ptr = data_values + 2100;
				for (loop = 0; loop < 12; loop++) {
					tmp[loop] = *ptr++;
				}
				
				// Show data activity via blue led
				BlueLed(1);

				// Send settling pattern & page number
				tmp[14] = readPageHighByte();
				tmp[15] = readPageLowByte();
					
				b_index = 0;
				if (readPageToMemory(&data_end)) {
					// Last page
					current_state = CWA_STANDBY_FOR_CONFIG;
					tmp[12] = 'D';
					tmp[13] = 'o';
					tmp[14] = 'n';
					tmp[15] = 'e';
					USB_Send_Data(tmp, 16);
				} else {
					// Start of new page
					tmp[12] = 0;
					tmp[13] = 0;
					tmp[16] = 0;
					tmp[17] = 0;
					USB_Send_Data(tmp, 18);
				}
				BlueLed(0);
			}
			break;
		case CWA_READ_PAGE:	
			if (b_index < 2040) {
				USB_Send_Data(data_values + b_index, transfer_size);
				b_index += transfer_size;
			} else {
				// Last 6 bytes (+ 2 crc + bb + status bytes) + 'Done'
				uint8_t loop, tmp[16];
				uint8_t* ptr = data_values + 2040;
				for (loop = 0; loop < 10; loop++) {
					tmp[loop] = *ptr++;
				}
				b_index = 0;
				current_state = CWA_STANDBY_FOR_CONFIG;
				tmp[10] = 'D';
				tmp[11] = 'o';
				tmp[12] = 'n';
				tmp[13] = 'e';
				USB_Send_Data(tmp, 14);
			}
			break;
		case CWA_READ_BATTERY_LEVELS:
			readBatteryLevels();
			break;
		case CWA_READ_TEMPERATURE_LEVELS:
			readTemperatureLevels();
			break;
		case CWA_READ_BAD_BLOCKS:
			readBadBlocks();
			break;
		case CWA_READ_DBG:
			readDebug();
			break;
	}
	
	simple_tx_read_check = 0;
}

static cwa_state waitForEndOfRead() {
	
	// A full power state. The flash pages are read & sent by ready_for_tx_cb
		
	// A simple wait until have transferred all the data to the pc
	while (CWA_STANDBY_FOR_CONFIG != current_state) {}
	
	return CWA_STANDBY_FOR_CONFIG;
}

static cwa_state waitForCollectionTime() {
	
	// Low power state. Wait for time for collection to start.
	
	// Disable the RTC second interrupt
	RTC_ITConfig(RTC_IT_SEC, DISABLE);
	RTC_WaitForLastTask();
		
	// Enable the RTC alarm interrupt
	RTC_ITConfig(RTC_IT_ALR, ENABLE);
	RTC_WaitForLastTask();

	// If decide in future that diag leds not needed then could set alarm with
	// number of secs until collection to start rather than looping every minute
	while (remaining_mins) {

		// Wake up every minute to decrement remaining_mins
		RTC_SetAlarm(RTC_GetCounter()+ SIXTY_SECONDS);
		RTC_WaitForLastTask();

		powerDown();
	}
	
	RTC_ITConfig(RTC_IT_ALR, DISABLE);
	RTC_WaitForLastTask();
	
	return CWA_COLLECT;
}

static cwa_state collect() {
	
	// Low power state. Collection of xyz values until number of samples or
	// flash maximum reached.
	
	uint8_t crc_hi = 0xFF;
	uint8_t crc_lo = 0xFF;

	uint32_t tmp, max_samples = current_config.max_samples;
	uint16_t u16_dr10 = accel_rate_and_g_scale;
	
	saveValue(BKP_DR3, max_samples);
	
	// Disable the RTC second interrupt
	RTC_ITConfig(RTC_IT_SEC, DISABLE);

	// Define the collect time for both configs
	if (!current_config.collect_start_time) {
		last_config.collect_start_time = RTC_GetCounter();
		current_config.collect_start_time = last_config.collect_start_time;
		
		RTC_SetCounter(0);
	}
	// Must do this before RTC change
	RTC_WaitForLastTask();

	RedLed(0);
	GreenLed(0);
	BlueLed(0);
	
	// Change RTC to represent ticks
	RTC_SetPrescaler(RTC_SCALAR);
	RTC_WaitForLastTask();
		
	gpioEnableSpi();
	SPI_Configuration_Accelerometer(0);
	
	b_index = 0;
	lastHalfPage = 0;
	firstHalfPage = 0;
	
	BKP_WriteBackupRegister(BKP_DR10, u16_dr10);
	accelEnable();
	
	// Do an initial read to trigger the first call of the accelerometer
	// interrupt handler
	accelReadingBuf((uint8_t*)&(data_values[b_index]));
	b_index+=6;
	data_count++;

	while (data_count < max_samples) {

		// Each interrupt - check whether have enough data to write half a page
		// to flash
		if (lastHalfPage) {
			uint16_t loop;

			// Calculate final checksum
			for (loop = SAMPLE_HALF_PAGE; loop < SAMPLE_FULL_PAGE; loop++) {
				uint8_t i = crc_hi ^ data_values[loop];
				crc_hi = crc_lo ^ table_crc_hi[i];
				crc_lo = table_crc_lo[i];
			}
			
			data_values[SAMPLE_FULL_PAGE] = crc_hi;
			data_values[SAMPLE_FULL_PAGE + 1] = crc_lo;
    
			if (writeFinalHalfPageToFlash()) {
				writeStr("F1!");
				break;
			}
			lastHalfPage = 0;
			saveValue(BKP_DR5, data_count);
			start_page_tick = RTC_GetCounter();
		} else if (firstHalfPage) {
			uint16_t loop;

			crc_hi = 0xFF;
			crc_lo = 0xFF;
			
			// Calculate checksum for first half page
			for (loop = 0; loop < SAMPLE_HALF_PAGE; loop++) {
				uint8_t i = crc_hi ^ data_values[loop];
				crc_hi = crc_lo ^ table_crc_hi[i];
				crc_lo = table_crc_lo[i];
			}
			
			if (writeFirstHalfPageToFlash()) {
				writeStr("F0!");
				break;
			}
			firstHalfPage = 0;
		}

		powerDown();
	}
	
	saveValue(BKP_DR5, data_count);
	storeEndTick(RTC_GetCounter());
	storeEndSamples(data_count);
	
	SPI_Configuration_Accelerometer(0);
	accelDisable();
	SPI_Configuration_TempSensor();
	tempDisable();
	ADC1_Shutdown();
	
	// Move RTC back to seconds
	RTC_SetPrescaler(RTC_CLOCK_BASE);
	RTC_WaitForLastTask();
		
	tmp = (RTC_GetCounter() * RTC_SCALAR)/RTC_CLOCK_BASE;
	tmp += last_config.collect_start_time;
	
	RTC_SetCounter(tmp);
	RTC_WaitForLastTask();
	
	return CWA_SLEEP;
}

static cwa_state sleep() {
	
	// Go into lowest power state. If woken by USB lead inserted then return to
	// full power standby for config state.
		
	BlueLed(0);
	GreenLed(0);
	RedLed(0);

	// Switch off USB
	PowerOff();
	usb_configured = 0;
	gpioMinimumPower();
	clocksMinimumPower();
	
	// Disable the RTC second interrupt
	RTC_ITConfig(RTC_IT_SEC, DISABLE);
	RTC_WaitForLastTask();
	
	// Go to sleep until USB lead is inserted
	while (proceed_as_normal)
		powerDown();

	return CWA_STANDBY_FOR_CONFIG;
}

// End of states

// ----

// Helpers for main func

void zeroConfigs() {
	// Simplistic helper for zeroing of struct (no memset)
	int loop, rdlen = sizeof(struct config_info);
	
	uint8_t* current_ptr = (uint8_t *) &current_config;
	uint8_t* last_ptr = (uint8_t *) &last_config;
	for (loop = 0; loop < rdlen; loop++) {
		*last_ptr++ = 0;
		*current_ptr++ = 0;
	}
}

uint8_t encodeFwAndHwVersions(firmware_version fw, hardware_version hw) {
	uint8_t byte = (uint8_t) fw;
	byte <<= HW_VER_BITS;
	byte |= (hw & HW_MASK);
	return byte;
}

// ----

// Main entry point

int main(void) {
	struct nand_identity nand_id;
	uint16_t u16_bkp_temp;
	
	// ----
	
	// Set up BioBand hardware
	Set_System();
	GPIO_Configuration();
	
	resetDbg();
	
#ifdef CWA_USART_DEBUG
	USART_Configuration1();
	writeStr("OK");
#endif

	EXTI_Configuration();
	NVIC_Configuration();
	TIM2_Configuration();
	gpioEnableSpi();
	SPI_Configuration_TempSensor();
	tempDisable();
	SPI_Configuration_Accelerometer(1);
	accelDisable();
	
	nand_cwa_init();
	
	readDeviceId(&nand_id);
	
	zeroConfigs();
	
	// End of hardware initialisation

	// ----
	
	// Restore current_config
	retrieveCurrentConfig();
	retrieveActionTime(&current_config.actioned_time);
	
	version_byte = encodeFwAndHwVersions(CW_CURRENT_FW, CW_CURRENT_HW);
	
	current_config.mode = CWA_TEST_MODE;
	
#if 0
	if ((nand_id.maker  == 0xad) &&
		(nand_id.device == 0xdc) &&
		(nand_id.third  == 0x10) &&
		(nand_id.fourth == 0x95) &&
		(nand_id.fifth  == 0x54)) {
		// Corresponds to identity for HY27UF084G2B
		current_config.flash_ok = 1;
	} else {
		current_config.flash_ok = 0;
	}
#endif

	current_config.flash_ok = 1;
	last_config.flash_ok = current_config.flash_ok;
	
	data_count = doesDataExist();
	
	current_state = CWA_STANDBY_FOR_CONFIG;

	// ----
	
	// Check if state & data needs to be restored

	// Determine the reason for (re-)entering main	
	if (RCC_GetFlagStatus(RCC_FLAG_PORRST) != RESET) {
		writeStr("PwR");
		// Assumption collect has finished or the device has been flashed for
		// the first time
		BKP_WriteBackupRegister(BKP_DR1, 0);
		GreenLed(1);
	} else if (RCC_GetFlagStatus(RCC_FLAG_PINRST) != RESET) {
		// Reset pressed on the breakout board
		writeStr("ExR");
		RedLed(1);
	} else if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET) {
		writeStr("SwR");
	} else {
		writeStr("OtR");
	}
	
	// Read register 1 from the battery backed store
	u16_bkp_temp = BKP_ReadBackupRegister(BKP_DR1);
 	
	SendHex16Uart1(u16_bkp_temp);

	// Determine if state needs to be restored due to re-entry into main
    if(u16_bkp_temp == version_byte) {
		
		accel_data_rate rate;
		accel_g_scale scale;
		bool restart_rtc_int = 1;
		uint16_t u16_c_state = BKP_ReadBackupRegister(BKP_DR2);
		uint16_t debug_state = BKP_ReadBackupRegister(BKP_DR9);
		
		uint16_t u16_dr10 = BKP_ReadBackupRegister(BKP_DR10);
		accel_rate_and_g_scale = u16_dr10 & 0x00FF;
		if (decodeRateAndGscale(accel_rate_and_g_scale, &rate, &scale)) {
			// Corrupt accel value - set to defaults
			writeStr("!ac");
			accel_rate_and_g_scale = CWA_8G | CWA_50HZ;
			current_accel_rate = CWA_50HZ;
		}
		
		if (debug_state < CWA_NUM_OF_MODES) {
			current_config.mode = debug_state;
		} else {
			// Corrupt - set to real mode
			current_config.mode = CWA_REAL_MODE;
		}
		
#if 0
		// No need to configure RTC, wait for RTC registers synchronisation
		RTC_WaitForSynchro();
		RTC_WaitForLastTask();
#else
		// TEMP - using full RTC config for now since appears to be a synchro
		// lock up after flashing
		RTC_Configuration();
#endif

		SendHex16Uart1(u16_c_state); // Debug
		
		if (u16_c_state < CWA_NUM_OF_STATES) {
			
			// Valid stored state
			if (CWA_COLLECT == u16_c_state) {
				
				// Check for unexpected external reset during collect state
				writeStr("lcl");
				current_config.max_samples = restoreValue(BKP_DR3);
				writeHex32(current_config.max_samples);
				data_count = restoreValue(BKP_DR5);
				writeHex32(data_count);
				
				if (data_count < current_config.max_samples) {
					// Collect has been affected by external reset or hardware
					// issue
					writeStr("rcl");
					current_state = CWA_COLLECT;
					start_page_tick = RTC_GetCounter();
					
					restoreFlashIndexs();
					restart_rtc_int = 0;
				}
			} else {
				
				// Check if waiting to enter collect state
				current_config.collect_start_time = 0;
				if (CWA_STANDBY_FOR_COLLECT == u16_c_state) {
					
					// Standby for collect has been affected
					remaining_mins = restoreValue(BKP_DR3);
					writeHex32(remaining_mins);
					if (remaining_mins) {
						writeStr("rby");
						current_state = CWA_STANDBY_FOR_COLLECT;
					} else {
						writeStr("gc0");
						current_state = CWA_COLLECT;
						restart_rtc_int = 0;
					}
				}
			}
		}
		
		if (restart_rtc_int) {
			// Only restart the RTC interrupt if in full power state
			
			// Debug
			writeHex32(RTC_GetCounter());
		
			// Re-enable the RTC Second
			RTC_ITConfig(RTC_IT_SEC, ENABLE);
			RTC_WaitForLastTask();
		}
    } else {
		uint16_t u16_dr10 = accel_rate_and_g_scale;
		
		// Reset backup domain
		BKP_DeInit();
        BKP_WriteBackupRegister(BKP_DR1, version_byte);
        BKP_WriteBackupRegister(BKP_DR9, current_config.mode);
		writeStr("!BKP");
		
		RTC_Configuration();
		
		// Current time unknown
		RTC_SetCounter(0);
		RTC_WaitForLastTask();

		// Set RTC prescaler: set RTC period to 1sec
		RTC_SetPrescaler(RTC_CLOCK_BASE);
		RTC_WaitForLastTask();

		// Enable the RTC Second interrupt
		RTC_ITConfig(RTC_IT_SEC, ENABLE);
		RTC_WaitForLastTask();
		
		current_config.collect_start_time = 0;
		BKP_WriteBackupRegister(BKP_DR10, u16_dr10);
    }

    // Clear reset flags
    RCC_ClearFlag();
    
    // End of state restore functionality
    
	// ----

	// Ensure LEDs are initially off
	RedLed(0);
	GreenLed(0);
	BlueLed(0);
		
 	// Enter into state machine
	while (1) {

		// Debug
		SendHex16Uart1(current_state);
		
		switch (current_state) {
		case CWA_CONFIG:
			current_state = configure();
			break;
		case CWA_READ_RAW:
			writeStr("RR");
			current_state = waitForEndOfRead();
			break;
		case CWA_READ_PAGE:
			writeStr("RP");
			current_state = waitForEndOfRead();
			break;
		case CWA_READ_BATTERY_LEVELS:
			writeStr("BL");
			current_state = waitForEndOfRead();
			break;
		case CWA_READ_TEMPERATURE_LEVELS:
			writeStr("TL");
			current_state = waitForEndOfRead();
			break;
		case CWA_READ_BAD_BLOCKS:
			current_state = waitForEndOfRead();
			break;
		case CWA_READ_DBG:
			current_state = waitForEndOfRead();
			break;
		case CWA_STANDBY_FOR_COLLECT:
#ifdef ENABLE_BATTERY_LEVEL
			if (adc_configured) {
				adc_configured = 0;
				ADC1_Shutdown();
			}
#endif
			writeStr("SC");
			current_state = waitForCollectionTime();
			break;
		case CWA_COLLECT:
#ifdef ENABLE_BATTERY_LEVEL
			if (adc_configured) {
				adc_configured = 0;
				ADC1_Shutdown();
			}
#endif
			writeStr("CL");
			current_state = collect();
			break;
		case CWA_SLEEP:
#ifdef ENABLE_BATTERY_LEVEL
			if (adc_configured) {
				adc_configured = 0;
				ADC1_Shutdown();
			}
#endif
			writeStr("SL");
			current_state = sleep();
			break;
		default: // CWA_STANDBY_FOR_CONFIG
			writeStr("SB");
			current_state = waitForConfig();
			break;
		}
		
		// Store current state
        BKP_WriteBackupRegister(BKP_DR2, current_state);
		
		if (!proceed_as_normal) {
			// Device has been awoken by the usb lead being inserted
			current_state = CWA_STANDBY_FOR_CONFIG;
		}
	}

}

// EOF
