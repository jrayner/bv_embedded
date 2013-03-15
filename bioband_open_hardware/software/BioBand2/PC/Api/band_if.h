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

#ifndef _BAND_IF
#define _BAND_IF

// MS VC++ does not appear to like this in an ifdef
#include "stdafx.h"

#ifdef _WIN32
	#include "stdint.h"
#else // LINUX
	#include <stdint.h>
	#include <unistd.h>
	#include <termios.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h> 
#include <time.h>

#ifdef _WIN32

#include "usb.h"

// force VC++ to pick up libusb 0.1
#pragma comment(lib,"setupapi")
#pragma comment( lib, "libusb.lib" )

#define MY_CONFIG 1

#else // LINUX

#ifdef USBLIB1
#include <libusb-1.0/libusb.h>
#else
#include <usb.h>
#endif // USBLIB1

#endif

#define MY_INTF 0

#include "shared.h" // for shared defn of config_info
#include "usb_desc.h" // for tx/rx size defns

#include <string>
#include <queue>
#include <list>
using namespace std;

const int max_transfer_page = PAGE_LEADER + FLASH_PAGE;

double convAccValueToGValue(uint8_t*& aSamplePtr);
double convADCToVoltage(uint16_t adc);
double convTempBinToCelsius(uint16_t temp_bin_val);
time_t convTicksToTime(time_t start_time, uint32_t number_ticks);
void convHwFwVerToBytes(uint8_t ver_raw, uint8_t& hwb, uint8_t& fwb);

// Error codes with specific meanings, otherwise use errno.h
#define BB_SUCCESS 0
#define E_BB_GENERIC_FAILURE -1000
#define E_BB_BAD_PARAM -1001
#define E_BB_FILE_EXISTS -1002
#define E_BB_FAILED_TO_OPEN_FILE_FOR_WRITE -1003
#define E_BB_BAD_ID -1004
#define E_BB_BAND_NOT_CONNECTED -1005
#define E_BB_BAND_ALREADY_CONNECTED -1006
#define E_BB_NO_SERIAL_NUMBER_MATCH -1007
#define E_BB_USB_GENERIC_FAILURE -1008
#define E_BB_USB_READ_FAILURE -1009
#define E_BB_NOT_DATA_ON_BAND -1010
#define E_BB_UNEXPECTED_DATA_ON_LINK -1011
#define E_BB_REQUEST_FAILED -1012
#define E_BB_NO_RESPONSE_FROM_BAND -1013
#define E_BB_FAILED_TO_OPEN_FILE_FOR_READ -1014
#define E_BB_MISSING_CALLBACK_PTR -1015


#define BB_UNKNOWN_STR "Unknown"

/**
 * Data callback interface
 */
struct MDataObserver
{
	MDataObserver() { reset(); }
	
	/**
	 * Notifies the client of a new group of samples
	 * \return true if wish to stop the processing of further samples
	 */
	virtual bool evSamplesCallback() = 0;
	
	/**
	 * Notifies the client of completion
	 */
	virtual void evDoneCallback() = 0;
	
	virtual void reset();
	
	virtual void addRawSample(uint8_t* bytesPtr);
	
	struct sample {
		void giveGValues(double& x, double& y, double& z);
		uint8_t sample_raw[BYTES_PER_SAMPLE];
	};
	
	bool crc_ok;	
	list<sample> raw_sample_list;
	uint8_t status_raw;
	uint32_t current_tick;
	uint16_t temperature_raw;
	list<uint8_t> dbg_raw;
	
	// following data present only if additional_present is true
	bool additional_present;
	uint16_t battery_raw;
	string band_id;
	string subject_id;
	string test_id;
	string centre_id;
	uint8_t calibration_data[MAX_CALIBRATION_DATA_LEN];
	uint32_t req_total_num_samples;
	uint32_t collect_start_time;
	uint8_t accel_conf_raw;
	uint8_t fw_hw_version_raw;
	
	// following data present only if status_present is true
	// Note: this data is only stored once and should not be relied upon being
	// available
	bool status_present;
	uint32_t first_downloaded;
	uint32_t end_tick;
	uint32_t num_samples;
	uint32_t actioned_time;
};

class BioBandIf 
{ 
public: 
	/**
	 * Creates a new instance of the class.
	 */
	BioBandIf();
	
	/**
	 * The class destructor disconnects any attached drive and de-allocates all
	 * internal memory.
	 */
	virtual ~BioBandIf();
	
	/**
	 * The object attempts to connect to an band accelerometer.
	 * \param aSerialNum of band to connect to or can leave NULL if know only
	 * one band is connected
	 * \return 0 indicates a successful connect otherwise < 0 if there is an
	 * error
	 */
	int connectUsb(const char* aSerialNum = NULL);
	
	/**
	 * Request the retrieval of all the known bands connected to USB
	 * \param serial_numbers is the list construct to be populated
	 * \return the number of bands found otherwise < 0 if there is an error
	 */
	int getBandSerialNumbers(list<string>& serial_numbers);

	/**
	 * Returns the state of the connection
	 * \return true if a device is connected
	 */
	bool isValid();
	
	/**
	 * Textual error description of the last error detected
	 * \return empty string if not an error
	 */
	const string errorToString(int aErrCode);
	
	/**
	 * Get the hardware & firmware versions of the connected band
	 * \param aHw is the hardware version
	 * \param aFw is the firmware version
	 * \param aFwDate is the compile date and time
	 * \return 0 if successful otherwise < 0 if there is an error
	 */
	const int getHwFwVersions(float& aHw,float& aFw, string& aFwDate);
	
	/**
	 * Get the bioband api version
	 * \param aApi is the api version
	 */
	const void getBbApiVersion(float& aVer);
	
	/**
	 * Retrieve the current voltage of the band battery
	 * \return the battery level or less than zero if there is an error
	 */
	unsigned int getBatteryVoltage();
	
	/**
	 * Textual identifying string for the particular bioband.
	 * \return the identifying string for the particular physical device
	 * attached or Unknown if there is a problem
	 */
	const string getBandId();
	
	/**
	 * Textual identifying string for the text that was stored on the device
	 * with setSubjectDetails().
	 * \return the identifying string for the subject or Unknown if there is a
	 * problem
	 */
	const string getSubjectDetails();
	
	/**
	 * Textual identifying string for the text that was stored on the device
	 * with setTestDetails().
	 * \return the identifying string for the test or Unknown if there is a
	 * problem
	 */
	const string getTestDetails();
	
	/**
	 * Two character string that was stored on the device with setCentreId().
	 * \return the identifying string for the centre or Unknown if there is a
	 * problem
	 */
	const string getCentreId();
	
	
	// TODO Given the use of ticks and the reality that the battery will usually
	// be flat after an extended collect is this api worth having? 
	/**
	 * Retrieve the current GMT time according to the device. If the battery
	 * has failed and/or no collect has been run then the time will show
	 * seconds operating since powered using epoch, 1/1/1970, as the base.
	 * \param device_time is the time to populate
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	const int getGmtDeviceTime(time_t& gmt_time);
	
	/**
	 * Retrieve the total size (in bytes) of measurement data stored (not
	 * including temperature measurements)
	 * Note that the request can take a long time to complete due to the need
	 * to sequentially check the stored pages to calculate the size.
	 * 
	 * \return the size of the data otherwise < 0 if there is an error
	 */
	int getStoredSize();
	
	/**
	 * Retrieve the timing info for start and end of the stored samples and when
	 * the sampling was actioned (note the action time does not persist if the
	 * band is reset or battery power is lost)
	 * Note that the request can take a long time to complete due to the need
	 * to sequentially check the stored pages to find the end time.
	 * 
	 * \param actioned_time references the action time_t value to populate
	 * \param sample_start_time references the start time_t value to populate
	 * \param sample_end_time references the sample end time_t value to populate
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getSampleTimings(time_t& actioned_time, time_t& sample_start_time,
		time_t& sample_end_time);

	// ----
	
	/**
	 * Retrieve the number of pages of measurement data stored
	 * Note that the request can take a long time to complete due to the need
	 * to sequentially check the stored pages to calculate the size.
	 * 
	 * \return the number of pages otherwise < 0 if there is an error
	 */
	int getPageCount();
	
	/**
	 * Request to read a particular page of accelerometer values from the band
	 * \param page is the number of logical page
	 * \param page_mem is the pointer to the start of the page, the invoker is
	 * responsible for deleting the memory after use. Values are packed as 16
	 * bits in x,y,z,x ... order to the size of the page (see
	 * getSamplesPerPage()) which may vary between devices.
	 * \param temp_val is the stored temperature value
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int readPage(const uint16_t page, uint8_t*& page_mem, uint16_t& temp_val);
	
	/**
	 * Request to read next page after an initial readPage request. This is a
	 * faster alternative to using readPage when doing successive reads.
	 * \param page_mem is the pointer to the start of the page, the invoker is
	 * responsible for deleting the memory after use. Values are packed as 16
	 * bits in x,y,z,x ... order to the size of the page (see
	 * getSamplesPerPage()) which may vary between devices.
	 * \param temp_val is the stored temperature value
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int readNextPage(uint8_t*& page_mem, uint16_t& temp_val);
	
	// ----
	
	/**
	 * Read all the samples invoking the callback for each sample
	 * \param aCallbackPtr is the callback pointer
	 * \return < 0 if there is an error otherwise total bytes received
	 */
	//int readAllSamples(MDataObserver* aCallbackPtr); - replaced by raw
	
	// ----
	
	// RAW
	
	/**
	 * Read all data from the band (allows for off-line processing of data)
	 * \param write_fd_ptr is the optional file descriptor to write to
	 * \return < 0 if there is an error otherwise total bytes received
	 */
	int readRawFromBand(FILE* write_fd_ptr);
	
	/**
	 * Read the data from a previous extracted band raw file.
	 * \param read_fd_ptr is the mandatory file descriptor to read from
	 * \return < 0 if there is an error otherwise total bytes received
	 */
	int readRawFromFile(FILE* read_fd_ptr);
	
	/**
	 * Set the callback to be invoked for each sample from the raw file
	 * \param aCallbackPtr is the callback pointer
	 * \return 0 indicates successful setting, < 0 if there is an error
	 */
	int setRawDataCallbackPtr(MDataObserver* aCallbackPtr);

	// ----
	
	/**
	 * Request the retrieval of the battery level details from the band for the
	 * last sampling session.
	 * \param levels_ptr is the list construct to be populated
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getBatteryLevels(queue<uint16_t>* levels_ptr);

	/**
	 * Request the retrieval of the temperature levels from the band for the
	 * last sampling session.
	 * \param levels_ptr is the list construct to be populated
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getTemperatureLevels(queue<uint16_t>* levels_ptr);

	/**
	 * Request the asynchronous retrieval of the current accelerometer
	 * measurement.
	 * \param x is the reference to the x axis parameter to populate
	 * \param y is the reference to the y axis parameter to populate
	 * \param z is the reference to the z axis parameter to populate
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	//int getMeasurement(uint16_t& x, uint16_t& y, uint16_t& z);

	/**
	 * Stores the band id into the band flash upon the next measurement cycle
	 * Note that the id will NOT be stored if no measurement cycle is executed
	 * or power on the device is lost between invocation and the measurement
	 * cycle
	 * \param id is the band identity to be stored
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setBandId(const string& id);
	
	/**
	 * Stores the subject id into the band flash upon the next measurement cycle
	 * Note that the id will NOT be stored if no measurement cycle is executed
	 * or power on the device is lost between invocation and the measurement
	 * cycle
	 * \param id is the subject identity to be stored
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setSubjectDetails(const string& id);
	
	/**
	 * Stores the test id into the band flash upon the next measurement cycle
	 * Note that the id will NOT be stored if no measurement cycle is executed
	 * or power on the device is lost between invocation and the measurement
	 * cycle
	 * \param id is the test identity to be stored
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setTestDetails(const string& id);
	
	/**
	 * Stores the two character centre id into the band flash upon the next
	 * measurement cycle.
	 * Note that the id will NOT be stored if no measurement cycle is executed
	 * or power on the device is lost between invocation and the measurement
	 * cycle
	 * \param id is the centre identity to be stored
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setCentreId(const string& id);
	
	/**
	 * Destructively erases all measurement and identity details (both user and
	 * device ids) stored on the band device
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int clearStoredMeasurements();
	
	/**
	 * Start the measurement collection. After invoking this method it will not
	 * be possible to communicate with the band again until the USB lead is next
	 * plugged into the device or reset is pressed on the breakout board.
	 * \param wait_mins is the number of minutes to wait until collection should
	 * begin
	 * \param run_mins is the length of time collection should run for
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int start(const unsigned int wait_mins, const unsigned int run_mins);
	
	// ----
	
	// Accelerometer Config
	
	/**
	 * Set accelerometer data rate and full G scale
	 * \param dr is the data rate
	 * \param gs is the full G scale
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setAccelConfig(accel_data_rate dr, accel_g_scale gs);
	
	/**
	 * Read the currently set accelerometer data rate and full G scale
	 * \param dr is the data rate
	 * \param gs is the full G scale
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int readAccelConfig(accel_data_rate* dr, accel_g_scale* gs);

	/**
	 * Read the accelerometer data rate and full G scale from the previous run
	 * stored in flash (will return unknown if no data in flash)
	 * \param dr is the data rate
	 * \param gs is the full G scale
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int readFlashAccelConfig(accel_data_rate* dr, accel_g_scale* gs);

	/**
	 * Record calibration gain and offset data in the flash memory of the band
	 * \param params is a set of six uint16_t, representing gain and offset in
	 * each axis xy&z.
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int recordCalibrationData(uint16_t* params);
	
	/**
	 * Retrieve calibration gain and offset data from the flash memory of the
	 * band
	 * \param params is a set of six uint16_t, representing gain and offset in
	 * each axis xy&z.
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getCalibrationData(uint16_t* params);
	
	// ----
	
	/**
	 * Set the first (ideally successful) download time into the band
	 * \param first_download_time is the epoc time
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setFirstDownloadTime(time_t first_download_time);
	
	/**
	 * Get the first download time from the band
	 * \param first_download_time is the epoc time to populate. If the time is
	 * zero then no download time currently set on the Bioband.
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getFirstDownloadTime(time_t& first_download_time);
	
	// ----
	
	/**
	 * Get whether a capture exists and whether it completed as expected
	 * \param exists is set to true if data does exist
	 * \param complete is set to true if the expected amount of data exists
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getIfCaptureExistsAndComplete(bool& exists, bool& complete);

	// ----
	
	typedef enum {
		NOTHING = 0,
		BLUE = 1,
		GREEN = 2,
		GREEN_BLUE = 3,
		RED = 4,
		RED_BLUE = 5,
		RED_GREEN = 6,
		WHITE = 7
	} led_colour;
	
	/**
	 * Set the LED to the colour required
	 * \param colour is the colour to set
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int setLed(led_colour colour);
	
	/**
	 * Forceably send the band to sleep
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int goToSleep();
	
	/**
	 * Alternative to object destructor
	 * Return the device to ready to receive state and close the USB connection
	 * 
	 * Note that connectUsb() will need to be called before attempting to call
	 * any further object methods after use.
	 * 
	 * This method is not recommended for normal use. Quick get around for
	 * Windows strangeness.
	 */
	void cleanup();
	
	/**
	 * Set the band into debug mode - the led on the band will indicate which
	 * mode it is in:
	 * Flashing Red = waiting for command
	 * Flashing Green = processing command
	 * Flashing Blue = sampling (at half rate of sampling freq)
	 * Use of setLed will switch off debug mode
	 * 
	 * Note for v1.2+ bands, the green led is also hardwired to light when the
	 * band is charging.
	 */
	void setBandDebugLeds();
	
	struct bad_block {
		unsigned int block;
		unsigned int page;
		unsigned int marker;
	};
	
	/**
	 * Wipe the backup domain information on the connected Bioband. Use
	 * inconjunction with clearStoredMeasurements if wish to forcibly return a
	 * band to completely clean state.
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int noBkp();
	
	/**
	 * Diagnostics only. Request the retrieval of the band badblock data
	 * \param bad_blocks_ptr is the list construct to be populated
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getBadBlocks(queue<bad_block*>* bad_blocks_ptr);

	/**
	 * Diagnostics only. Request the retrieval of the debug text codes from
	 * band's circular debug buffer and place in the supplied list.
	 * \param debug_buffer is the list to populate
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	int getDebugBuffer(queue<uint8_t>* debug_buffer_ptr);

	/**
	 * Testing only. Sets up 5 days worth of dummy data on the band.
	 * \return 0 indicates successful completion, < 0 if there is an error
	 */
	//int setUpDummyData();
	
	/**
	 * Testing only. Enable/disable extra debug output from bioband API
	 */
	void enableDebug() { debug_flag = 1; }
	void disableDebug() { debug_flag = 0; }
	
private:
	int writeUsb(const int len);
	int readUsb();
	void flushUsb();
	void closeUsb();

	void writeSingleChar(char val);
	
	int instructTheTag(struct config_info* msg);
	int stateMachine(int rdlen);
	
	void checkRawStart();
	void processRawData();
	
	typedef enum {
		COLLECT_OP,
		//READ_STREAM_OP,
		READ_BAD_BLOCKS_OP,
		//TEST_FLASH_OP,
		READ_BATTERY_LEVEL_OP,
		READ_TEMPERATURE_LEVEL_OP,
		//STREAMING_MODE_OP,
		SET_ID_OP,
		//BATTERY_BURN_OP,
		GET_VERSION_OP,
		GET_DEVICE_TIME_OP,
		//GET_MEASUREMENT_OP,
		ERASE_FLASH_OP,
		SET_LED_COLOUR_OP,
		READ_PAGE_OP,
		READ_NEXT_PAGE_OP,
		//SET_DUMMY_DATA_OP,
		GO_TO_SLEEP_OP,
		READ_CONFIG_ONLY,
		READ_RAW_OP,
		NO_BKP_OP,
		READ_DBG_OP,
		SET_ACCEL_CONFIG_OP,
		READ_ACCEL_CONFIG_OP,
		READ_FLASH_ACCEL_CONFIG_OP,
		SET_CALIB_DATA_OP,
		SET_FIRST_DOWNLD_OP,
		GET_FIRST_DOWNLD_OP,
		GET_IS_COMPLETE_OP //fw1.2
	} op_state;

	int enterEventLoop(op_state op);

	void displayTime(time_t* aTimePtr, bool isGuaranteed);
	void displayConfig(struct config_info* aConfigPtr, int last_run);

private:

	op_state user_op;

	typedef enum
	{
		ESTABLISH_LINK,
		READ_CONFIG,
		READ_AS_CSV,
		READ_BAD_BLOCKS,
		READ_BATTERY_LEVELS,
		READ_TEMPERATURE_LEVELS,
		STREAMING_MODE,
		BATTERY_BURN_MODE,
		WAIT_FOR_CONFIRM,
		SET_IDENTITY,
		SET_CONFIG,
		START,
		GET_VERSION,
		GET_DEVICE_TIME,
		GET_MEASUREMENT,
		ERASE_FLASH,
		SET_LED_COLOUR,
		READ_PAGE,
		DEBUG_OUTPUT,
		READ_RAW,
		READ_DBG,
		READ_ACCEL_CONFIG,
		GET_FIRST_DOWNLD,
		GET_IS_COMPLETE
	} ctrl_state;

	ctrl_state current_state;


	int data_count;
	int page_idx;
	int pg_count;
	uint8_t crc_hi;
	uint8_t crc_lo;

	uint8_t wait_for_start;

	int total_rec;

#ifdef USBLIB1
	libusb_device_handle *dev_handle;
	libusb_context *ctx;
#else
	struct usb_dev_handle *dev_handle;
#endif
	
	int full_config_defined;
	struct config_info received_config_data;
	struct config_info sent_config_data;

	int new_band_id_length;
	uint8_t new_band_id[MAX_ID_LEN + 1];

	char readchars[SIMPLE_TX_DATA_SIZE];
	char writechars[SIMPLE_RX_DATA_SIZE];
	
	char data_buffer[max_transfer_page];
	int buffer_idx;
	
	
	queue<bad_block*>* iBadBlocksPtr;
	queue<uint16_t>* iLevelsPtr;
	
	float hw_ver;
	float fw_ver;
	string fw_ver_date_time;
	
	time_t device_time;
	uint8_t measurement[BYTES_PER_SAMPLE+1];
	led_colour led_setting;
	
	unsigned int read_timeout;
	
	int keep_going;
	
	uint16_t read_page_num;
	uint8_t* read_page_ptr;
	uint8_t* current_page_ptr;

	time_t collect_time;
	time_t start_page_time;
	
	int page_temp_defined;
	uint16_t page_temperature_value;
	uint16_t page_num;
	
	int debug_flag;
	
	MDataObserver* sample_obs_ptr;
	
	FILE* raw_fd;
	
	uint8_t rate_and_g_scale;
	uint8_t calibration_data[MAX_CALIBRATION_DATA_LEN];
	
	uint32_t first_download;
	bool status_page_found;
	
	uint8_t is_complete;
	
	queue<uint8_t>* iDebugDataPtr;
};

#endif

// EOF
