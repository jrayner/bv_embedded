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

// MS VC++ does not appear to like this in an ifdef
#include "stdafx.h"

#include "band_if.h"

#define READ_DIAG_OUTPUT

#define DEBUG(X) if (debug_flag) { X }
//#define DEBUG(X)

const unsigned char ENDPOINT_DOWN = 0x2;
const unsigned char ENDPOINT_UP = 0x81;

#define FINISHED 1

#define DEFAULT_READ_TIMEOUT 50000
#define CMJ_USB_DEBUG 0


// --------------------------------------------------------------------------

// conversion funcs

double convAccValueToGValue(uint8_t*& aSamplePtr) {
	
	// values are big-endian order
	// assume accelerometer is set for +/-8g scale // TODO ***** enable other full G scales ***
	int16_t v=(int16_t) *aSamplePtr++;
	v<<=8;
	v&=0xff00;
	v|=(uint8_t) *aSamplePtr++;
	double float_val = (double)v / 4096;
		
	return float_val;
}

double convADCToVoltage(uint16_t adc) {
	
	/* The ADC input voltage is (10/32)*battery voltage.
	 * The ADC reference voltage is 2.8V or thereabouts
	 * ADC runs in right-aligned mode, 12 bits, so full scale is 4096
	 * ADC reading = 4096 *((voltage*10/32)/2.8)
	 * ADC reading = voltage * 457.14
	 */
	double voltage = (double)adc / 457.14;
		
	return voltage;
}

double convTempBinToCelsius(uint16_t temp_bin_val) {
	
	double celsius;
	
	uint16_t v = temp_bin_val;
	v >>= 2;
	
	if (v & 0x2000) {
		v = v & 0x1FFF;
		v = (~v) & 0x1FFF;
		v += 1;
		celsius = -(v * 0.03125);
	} else {
		celsius = v * 0.03125;
	}
		
	return celsius;
}

time_t convTicksToTime(time_t start_time, uint32_t number_ticks) {
	time_t ret = start_time;
	
	uint32_t secs = (RTC_SCALAR * number_ticks)/ RTC_CLOCK_BASE;
	ret += secs;
	
	return ret;
}

void convHwFwVerToBytes(uint8_t ver_raw, uint8_t& hwb, uint8_t& fwb) {
	hwb = ver_raw & HW_MASK;
	fwb = ver_raw;
	fwb >>= HW_VER_BITS;
	fwb &= FW_MASK;
}

// --------------------------------------------------------------------------

void MDataObserver::reset() {
	crc_ok = false;
	raw_sample_list.clear();
	status_raw = UNUSED_PAGE;
	current_tick = 0;
	temperature_raw = 0;
	dbg_raw.clear();
	additional_present = false;
	battery_raw = 0;
	band_id.clear();
	subject_id.clear();
	test_id.clear();
	centre_id.clear();
	for (int loop = 0; loop < MAX_CALIBRATION_DATA_LEN; loop++)
		calibration_data[loop] = 0;
	req_total_num_samples = 0;
	collect_start_time = 0;
	accel_conf_raw = 0;
	fw_hw_version_raw = 0;
	status_present = false;
	first_downloaded = 0;
	end_tick = 0;
	num_samples = 0;
}

void MDataObserver::addRawSample(uint8_t* bytesPtr) {
	
	sample s;
	memcpy(s.sample_raw, bytesPtr, BYTES_PER_SAMPLE);
	raw_sample_list.push_back(s);
}

void MDataObserver::sample::giveGValues(double& x, double& y, double& z) {
	uint8_t* ptr = sample_raw;
	x = convAccValueToGValue(ptr);
	y = convAccValueToGValue(ptr);
	z = convAccValueToGValue(ptr);
}

// --------------------------------------------------------------------------

BioBandIf::BioBandIf():
	current_state(ESTABLISH_LINK),
	dev_handle(NULL),
	iBadBlocksPtr(NULL),
	iLevelsPtr(NULL),
	read_page_ptr(NULL),
	raw_fd(NULL),
	rate_and_g_scale(0),
	iDebugDataPtr(NULL) {
		
	data_count = 1;
	page_idx = 0;
	pg_count = 0;
	crc_hi = 0xFF;
	crc_lo = 0xFF;
	wait_for_start = 1;
	total_rec = 0;
	
	memset(&sent_config_data,0,sizeof(sent_config_data));
	sent_config_data.mode = 1;
	sent_config_data.flash_ok = 1;
	
	memset(&received_config_data,0,sizeof(received_config_data));
	full_config_defined = 0;
	
	new_band_id_length = 0;
	read_timeout = DEFAULT_READ_TIMEOUT;
	
	debug_flag = 0;
	first_download = 0;
}

BioBandIf::~BioBandIf() {
	if (dev_handle) {
		closeUsb();
	}
	if (read_page_ptr)
		delete[] read_page_ptr;
}

void BioBandIf::setBandDebugLeds() {
	sent_config_data.mode = 2; // 0 = unknown
}

// -----------------------------------------------------------------------------

// USB methods

#ifdef USBLIB1

// Libusb 1.0 code

int BioBandIf::getBandSerialNumbers(list<string>& serial_numbers) {
	int band_count = 0;
	libusb_device **devs;
	ssize_t cnt;

    if (NULL != dev_handle) {
        fprintf(stderr, "Device already connected\n");
        return -E_BB_BAND_ALREADY_CONNECTED;
	}
	
    if (libusb_init(&ctx) < 0) {
        fprintf(stderr, "libusb_init failed\n");
        return -E_BB_USB_GENERIC_FAILURE;
    }
    
	DEBUG(libusb_set_debug(ctx, 3);)

    cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        fprintf(stderr, "Failed to retrieve usb devices %d\n",(int) cnt);
        return (int) cnt;
	}
	if (!cnt) {
        fprintf(stderr, "No Band found\n");
        return -E_BB_BAND_NOT_CONNECTED;
	}
	
	for (int i = 0; i < cnt; i++) {
		libusb_device *dev = devs[i];
		if (dev) {
			struct libusb_device_descriptor dev_desc;
			DEBUG(printf("reading device descriptor...\n");)
			if (!libusb_get_device_descriptor(dev, &dev_desc)) {
				if (dev_desc.idVendor == VENDOR
						&& dev_desc.idProduct == PRODUCT) {
					band_count++;
					if (dev_desc.iSerialNumber) {
						char buffer[100];
						libusb_device_handle *quick_check;
						libusb_open(dev, &quick_check);
						buffer[0] = 0;
						if (libusb_get_string_descriptor_ascii(quick_check,
							dev_desc.iSerialNumber, (unsigned char*) buffer,
							100) < 0) {
							DEBUG(printf("failed to read serial string\n");)
						} else {
							DEBUG(printf("Found band serial num <%s>\n",
								buffer);)
							serial_numbers.push_back(buffer);
						}
						libusb_close(quick_check);
					}
				}
			}
		}
	}
	libusb_free_device_list(devs, 1);

	return band_count;
}

int BioBandIf::connectUsb(const char* aSerialNum) {
	int retval = 0;
	libusb_device **devs;
	ssize_t cnt;

    if (NULL != dev_handle) {
        fprintf(stderr, "Device already connected\n");
        return -E_BB_BAND_ALREADY_CONNECTED;
	}
	
    retval = libusb_init(&ctx);
    if (retval < 0) {
        fprintf(stderr, "libusb_init failed %d\n",retval);
        return retval;
    }
    
	DEBUG(libusb_set_debug(ctx, 3);)

    cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        fprintf(stderr, "Failed to retrieve usb devices %d\n",(int) cnt);
        return (int) cnt;
	}
	if (!cnt) {
        fprintf(stderr, "No Band found\n");
        return -E_BB_BAND_NOT_CONNECTED;
	}
	
	if (aSerialNum) {
		for (int i = 0; i < cnt; i++) {
			libusb_device *dev = devs[i];
			if (dev) {
				struct libusb_device_descriptor dev_desc;
				DEBUG(printf("reading device descriptor...\n");)
				if (!libusb_get_device_descriptor(dev, &dev_desc)) {
					if (dev_desc.idVendor == VENDOR
							&& dev_desc.idProduct == PRODUCT) {
						if (dev_desc.iSerialNumber) {
							char buffer[100];
							libusb_device_handle *quick_check;
							libusb_open(dev, &quick_check);
							buffer[0] = 0;
							if (libusb_get_string_descriptor_ascii(quick_check,
								dev_desc.iSerialNumber, (unsigned char*) buffer,
								100) < 0) {
								DEBUG(printf("failed to read serial string\n");)
							} else {
								DEBUG(printf("Found band serial num <%s>\n",
									buffer);)
								if (aSerialNum && !strcmp(aSerialNum,buffer)) {
									DEBUG(printf(
										"Found specific band serial num <%s>\n",
										buffer);)
									dev_handle = quick_check;
									break;
								}
							}
							libusb_close(quick_check);
						}
					}
				}
			}
		}
	} else {
		dev_handle = libusb_open_device_with_vid_pid(ctx, VENDOR, PRODUCT);
	}
	libusb_free_device_list(devs, 1);

    if (dev_handle==NULL) {
		libusb_exit(ctx);
    	return -E_BB_GENERIC_FAILURE;
	}
	
	if(libusb_kernel_driver_active(dev_handle, 0) == 1) {
		// just in case something bizarre has happened
		fprintf(stderr, "kernel driver active\n");
		
		if(libusb_detach_kernel_driver(dev_handle, 0) == 0) //detach it
			printf("kernel driver detached!\n");
	}
	
	retval = libusb_claim_interface(dev_handle, 0);
	if(retval < 0) {
			fprintf(stderr, "cannot claim interface\n");
			libusb_exit(ctx);
			return retval;
	}
	DEBUG(printf("claimed interface\n");)

	return retval;
}

int BioBandIf::writeUsb(const int len) {
	int actual = 0;
	
	int r = libusb_bulk_transfer(dev_handle, ENDPOINT_DOWN,
								(unsigned char*) writechars, len, &actual, 10);
	if( r < 0 ) {
		perror("USB bulk write");
		DEBUG(printf("Write failed %d\n", r);)
	}
	return actual;
}

int BioBandIf::readUsb() {
	int rdlen, retval = libusb_bulk_transfer(dev_handle, ENDPOINT_UP,
						(unsigned char*) readchars, SIMPLE_TX_DATA_SIZE,
						&rdlen, read_timeout);
	if (!retval)
		retval = rdlen;
	return retval;
}

void BioBandIf::flushUsb() {
	char c;
	int retval=0,count=0;
	
	do {
		retval=libusb_bulk_transfer(dev_handle, ENDPOINT_UP,
			(unsigned char *)&c, 1, &count, 1);
		DEBUG(printf("flushUsb() %d\n",count);)
	} while (!retval);
}

void BioBandIf::closeUsb() {
	if(dev_handle) {
		libusb_close(dev_handle);
		dev_handle=NULL;
		libusb_exit(ctx);
	}
}

#else

// Libusb 0.1 code

int BioBandIf::getBandSerialNumbers(list<string>& serial_numbers) {
	int band_count = 0;
    struct usb_bus* bus;
    struct usb_device* dev;

    if (NULL != dev_handle) {
        fprintf(stderr, "Device already connected\n");
        return -E_BB_BAND_ALREADY_CONNECTED;
	}
	
    usb_init();
    
	DEBUG(usb_set_debug(4);)

    if (usb_find_busses() < 0) {
        fprintf(stderr, "Failed to find usb busses\n");
        return -E_BB_USB_GENERIC_FAILURE;
	}
	
    if (usb_find_devices() < 0) {
        fprintf(stderr, "Failed to find usb devices\n");
        return -E_BB_USB_GENERIC_FAILURE;
	}
	
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor == VENDOR
                    && dev->descriptor.idProduct == PRODUCT) {
				band_count++;
				if (dev->descriptor.iSerialNumber) {
					char buffer[100];
					struct usb_dev_handle* quick_check = usb_open(dev);
					if (usb_get_string_simple(quick_check,
						dev->descriptor.iSerialNumber,
						buffer, sizeof(buffer)) < 0) {
						DEBUG(printf("failed to read serial string\n");)
					} else {
						//string tmp = buffer;
						DEBUG(printf("Found band serial num <%s>\n", buffer);)
						serial_numbers.push_back(buffer);
					}
					usb_close(quick_check);
				}
           }
        }
    }
    return band_count;
}

int BioBandIf::connectUsb(const char* aSerialNum) {
	int retval = 0;
	int band_count = 0;
    struct usb_bus* bus;
    struct usb_device* dev;
    struct usb_device* found_dev = NULL;

    if (NULL != dev_handle) {
        fprintf(stderr, "Device already connected\n");
        return -E_BB_BAND_ALREADY_CONNECTED;
	}
	
    usb_init();
    
	DEBUG(usb_set_debug(4);)

    retval = usb_find_busses();
    if (retval < 0) {
        fprintf(stderr, "Failed to find usb busses %d\n",retval);
        return retval;
	}
	
    retval = usb_find_devices();
    if (retval < 0) {
        fprintf(stderr, "Failed to find usb devices %d\n",retval);
        return retval;
	}
	
    for (bus = usb_get_busses(); bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor == VENDOR
                    && dev->descriptor.idProduct == PRODUCT) {
				if (!found_dev && !aSerialNum)
					found_dev = dev;
				band_count++;
				if (dev->descriptor.iSerialNumber) {
					char buffer[100];
					struct usb_dev_handle* quick_check = usb_open(dev);
					if (usb_get_string_simple(quick_check,
						dev->descriptor.iSerialNumber,
						buffer, sizeof(buffer)) < 0) {
						DEBUG(printf("failed to read serial string\n");)
					} else {
						if (aSerialNum && !strcmp(aSerialNum,buffer)) {
							DEBUG(printf("Found specific band serial num <%s>\n",
								buffer);)
							found_dev = dev;
						}
						DEBUG(printf("Band <%s>\n", buffer);)
					}
					usb_close(quick_check);
				}
           }
        }
    }

	if (!band_count) {
        fprintf(stderr, "Failed to find a connected BioBand\n");
		return -E_BB_BAND_NOT_CONNECTED;
	}
	
	if (found_dev) {
		dev_handle = usb_open(found_dev);
	} else {	
		fprintf(stderr, "Found %d connected BioBands, no serial number match\n",
			band_count);
		return -E_BB_NO_SERIAL_NUMBER_MATCH;
	}
	
#ifdef _WIN32

	/* Microsoft appears to require the following */
    if (dev_handle) {
		if (usb_set_configuration(dev_handle, MY_CONFIG) < 0)
		{
			fprintf(stderr, "error: setting config #%d failed\n",MY_CONFIG);
			usb_close(dev_handle);
			dev_handle = NULL;
			return -E_BB_USB_GENERIC_FAILURE;
		}
		else
		{
			DEBUG(printf("success: set configuration #%d\n", MY_CONFIG);)
		}
	}
#endif

    if (dev_handle) {
		if (usb_claim_interface(dev_handle, 0) < 0)
		{
			fprintf(stderr, "error: claiming interface #%d failed\n", MY_INTF);
			usb_close(dev_handle);
			dev_handle = NULL;
			retval = -E_BB_USB_GENERIC_FAILURE;
		}
		else
		{
			DEBUG(printf("success: claim_interface #%d\n", MY_INTF);)
		}
	}

	if(dev_handle)
		flushUsb();
		
	return retval;
}

int BioBandIf::writeUsb(const int len) {
	
#if CMJ_USB_DEBUG
	printf("BioBandIf::writeUsb(\"");
	for(int n=0;n<len;n++) {
		printf("%c",writechars[n]);
	}
	printf("\")\n");
#endif

	int r = usb_bulk_write(dev_handle, ENDPOINT_DOWN, writechars, len, 10);

	if( r < 0 ) {
		perror("USB bulk write");
		DEBUG(printf("Write failed %d\n", r);)
	}
	return len;
}

int BioBandIf::readUsb() {
	int retval;
	
	retval= usb_bulk_read(dev_handle, ENDPOINT_UP, readchars,
		SIMPLE_TX_DATA_SIZE, read_timeout);

#if CMJ_USB_DEBUG
	printf("BioBandIf::readUsb(\"");
	if(retval>0) {
		for(int n=0;n<retval;n++) {
			if(readchars[n]>31 && readchars[n]<127) {
				printf("%c",readchars[n]);
			} else {
				printf("<%02x>",(unsigned char)readchars[n]);
			}
		}
	} else {
		printf("----%d----",retval);
	}

	printf("\")\n");
#endif
	
	return retval;
}

void BioBandIf::flushUsb() {
	char c;
	int count=0;
	
	do {
		count=usb_bulk_read(dev_handle, ENDPOINT_UP, &c, 1, 1);
		DEBUG(printf("flushUsb() %d\n",count);)
	} while (count>0);
}

void BioBandIf::closeUsb() {
	if(dev_handle) {
		/* do a write to avoid strange hangup (with libusb 0.1) */
		writeUsb(0);

		usb_release_interface(dev_handle, 0);
		usb_close(dev_handle);
		dev_handle=NULL;
	}
}

#endif // !USBLIB1

// -----------------------------------------------------------------------------

// Protocol methods

void BioBandIf::cleanup() {
	if(dev_handle) {
		writechars[0] = RETURN_CHAR;
		writechars[1] = 0;
		writeUsb(2);
		
		closeUsb();
	}
}

void BioBandIf::displayTime(time_t* aTimePtr, bool isGuaranteed) {
	// debug use
	if (*aTimePtr) {
		struct tm *tmp = localtime(aTimePtr);
		if (tmp) {
			printf("%ld (local: %02d:%02d:%02d %02d:%02d:%02d)\n",
				*aTimePtr,
				tmp->tm_hour,tmp->tm_min,tmp->tm_sec,
				tmp->tm_mday,tmp->tm_mon+1,tmp->tm_year+1900);
		} else {
			printf("failed to convert\n");
		}
	} else {
		if (isGuaranteed)
			printf("no value\n");
		else
			printf("no value (not a guaranteed value)\n");
	}
}

void BioBandIf::displayConfig(struct config_info* aConfigPtr, int last_run) {
	
	// debug use
	if (!last_run) {
		printf("  standby time\t\t%d mins\n",
			aConfigPtr->standby_before_collection_time_mins);
	}
	if (last_run) {
		printf("  data present\t\t%d\n",
			aConfigPtr->max_samples);
	} else {
		printf("  max samples\t\t%d\n",
			aConfigPtr->max_samples);
	}
	if (aConfigPtr->subject_id[0]) {
		uint8_t tmp_subject_id[MAX_ID_LEN + 1];
		memset(tmp_subject_id,0,MAX_ID_LEN + 1);
		memcpy(tmp_subject_id, aConfigPtr->subject_id, MAX_ID_LEN);
		printf("  subject id\t\t%s\n",tmp_subject_id);
	} else  {
		printf("  subject id\t\tnot set?\n");
	}
	if (aConfigPtr->test_id[0]) {
		uint8_t tmp_test_id[MAX_ID_LEN + 1];
		memset(tmp_test_id,0,MAX_ID_LEN + 1);
		memcpy(tmp_test_id, aConfigPtr->test_id, MAX_ID_LEN);
		printf("  test id\t\t%s\n",tmp_test_id);
	} else  {
		printf("  test id\t\tnot set?\n");
	}
	if (aConfigPtr->centre_id[0]) {
		uint8_t tmp_centre_id[MAX_CENTRE_ID_LEN + 1];
		memset(tmp_centre_id,0,MAX_CENTRE_ID_LEN + 1);
		memcpy(tmp_centre_id, aConfigPtr->centre_id, MAX_CENTRE_ID_LEN);
		printf("  centre id\t\t%s\n",tmp_centre_id);
	} else  {
		printf("  centre id\t\tnot set?\n");
	}
	
	printf("  actioned time\t\t");
	time_t t_val = (time_t) aConfigPtr->actioned_time;
	displayTime(&t_val, false);
	
	if (last_run) {
		printf("  collect start\t\t");
		t_val = (time_t) aConfigPtr->collect_start_time;
		displayTime(&t_val, true);
		printf("  number of ticks\t%d\n", aConfigPtr->number_of_ticks);
	}
	
	switch (aConfigPtr->mode) {
	case 2: printf("  mode\t\t\tdebug\n"); break;
	case 1: printf("  mode\t\t\treal\n"); break;
	default: printf("  mode\t\t\tunknown (not a persistent value)\n"); break;
	}
	if (last_run) {
		printf("Board:\n");
		printf("  stored calibration (gain,offset): ");
		uint16_t* val = (uint16_t*) aConfigPtr->cali_data;
		printf("x(%u, ",*val);
		val++;
		printf("%u) ",*val);
		val++;
		printf("y(%u, ",*val);
		val++;
		printf("%u) ",*val);
		val++;
		printf("z(%u, ",*val);
		val++;
		printf("%u)\n",*val);
		
		printf("  battery level\t\t%d (%.02fV)\n",
			aConfigPtr->battery_level,
			convADCToVoltage(aConfigPtr->battery_level));
			
		if (aConfigPtr->band_id[0]) {
			uint8_t tmp_band_id[MAX_ID_LEN + 1];
			memset(tmp_band_id,0,MAX_ID_LEN + 1);
			memcpy(tmp_band_id, aConfigPtr->band_id, MAX_ID_LEN);
			printf("  band id\t\t%s\n",tmp_band_id);
		} else { 
			printf("  band id\t\tnot set?\n");
		}
		if (aConfigPtr->subject_id[0]) {
			uint8_t tmp_subject_id[MAX_ID_LEN + 1];
			memset(tmp_subject_id,0,MAX_ID_LEN + 1);
			memcpy(tmp_subject_id, aConfigPtr->subject_id, MAX_ID_LEN);
			printf("  subject id\t\t%s\n",tmp_subject_id);
		} else { 
			printf("  subject id\t\tnot set?\n");
		}
		if (aConfigPtr->test_id[0]) {
			uint8_t tmp_test_id[MAX_ID_LEN + 1];
			memset(tmp_test_id,0,MAX_ID_LEN + 1);
			memcpy(tmp_test_id, aConfigPtr->test_id, MAX_ID_LEN);
			printf("  test id\t\t%s\n",tmp_test_id);
		} else { 
			printf("  test id\t\tnot set?\n");
		}
		if (aConfigPtr->centre_id[0]) {
			uint8_t tmp_centre_id[MAX_CENTRE_ID_LEN + 1];
			memset(tmp_centre_id,0,MAX_CENTRE_ID_LEN + 1);
			memcpy(tmp_centre_id, aConfigPtr->centre_id, MAX_CENTRE_ID_LEN);
			printf("  centre id\t\t%s\n",tmp_centre_id);
		} else  {
			printf("  centre id\t\tnot set?\n");
		}
		printf("  flash\t\t\t");
		if (aConfigPtr->flash_ok)
			printf("OK\n");
		else
			printf("ERROR\n");
	}
}

void BioBandIf::writeSingleChar(char val) {
	int written;
	writechars[0] = val;
	written = writeUsb(1);
	if (written != 1) {
		fprintf(stderr,"Unexpected write result %d\n",written);
	}
}

int BioBandIf::instructTheTag(struct config_info* msg) {
	
	switch (user_op) {

	case READ_BAD_BLOCKS_OP:
		writeSingleChar(READ_BAD_BLOCKS_CHAR);
		current_state = READ_BAD_BLOCKS;						
		break;

#if 0
	case TEST_FLASH_OP:
		writeSingleChar(TEST_FLASH_CHAR);
		current_state = WAIT_FOR_CONFIRM;
		break;
#endif

	case SET_ID_OP:
#if 0
		// TODO move id check callback to client code
		if (msg->band_id[0]) {
			char answer;
			uint8_t tmp_band_id[MAX_ID_LEN + 1];
			memcpy(tmp_band_id, msg->band_id, MAX_ID_LEN);
			tmp_band_id[MAX_ID_LEN] = 0;
			printf("\n** Warning: band id is already configured"
					" as <%s> **\n", tmp_band_id);
			printf("Press Y if you wish to overwrite with %s\n",
				new_band_id);
			answer = getchar();
			if (answer != 'Y')
				return -E_BB_BAD_PARAM;
		}
#endif
		writechars[0] = ID_CHAR;
		writechars[new_band_id_length + 1] = 0;
		memcpy(writechars + 1,new_band_id, new_band_id_length);
		writeUsb(new_band_id_length + 2);
		current_state = SET_IDENTITY;
		break;

#if 0
	case READ_STREAM_OP:
		if (msg && msg->max_samples) {
			DEBUG(printf("Data present\n");)
			page_temperature_value = 0;
			page_temp_defined = 0;
			writeSingleChar(READ_DATA_CHAR);
			current_state = READ_AS_CSV;
			wait_for_start = 1;
			pg_count = 0;
			page_idx = 0;
			total_rec = 0;
			collect_time = received_config_data.collect_start_time;
		} else {
			DEBUG(printf("No data stored on the device\n");)
			return -E_BB_NOT_DATA_ON_BAND;
		}
		break;
#endif

	case READ_RAW_OP:
		if (msg && msg->max_samples) {
			DEBUG(printf("Data present\n");)
			page_num = 0;
			writeSingleChar(READ_RAW_CHAR);
			current_state = READ_RAW;
			status_page_found = false;
			wait_for_start = 1;
			pg_count = 0;
			page_idx = 0;
			buffer_idx = 0;
			total_rec = 0;
			collect_time = received_config_data.collect_start_time;
		} else {
			DEBUG(printf("No data stored on the device\n");)
			return -E_BB_NOT_DATA_ON_BAND;
		}
		break;

	case READ_BATTERY_LEVEL_OP:
		if (msg && msg->max_samples) {
			writeSingleChar(BATTERY_LEVELS_CHAR);
			current_state = READ_BATTERY_LEVELS;
		} else {
			DEBUG(printf("No levels stored on the device\n");)
			return -E_BB_NOT_DATA_ON_BAND;
		}
		break;

	case READ_TEMPERATURE_LEVEL_OP:
		if (msg && msg->max_samples) {
			writeSingleChar(TEMPERATURE_LEVELS_CHAR);
			current_state = READ_TEMPERATURE_LEVELS;
		} else {
			DEBUG(printf("No levels stored on the device\n");)
			return -E_BB_NOT_DATA_ON_BAND;
		}
		break;

#if 0
	case STREAMING_MODE_OP:
		writeSingleChar(STREAMING_CHAR);
		current_state = STREAMING_MODE;
		break;

	case BATTERY_BURN_OP:
		writeSingleChar(BATTERY_BURN_CHAR);
		current_state = BATTERY_BURN_MODE;
		break;
#endif

	case COLLECT_OP: {
			// send config data
			char* msg_ptr = (char *) &sent_config_data;
			int writelen = sizeof(sent_config_data);
			
			if (writelen >= SIMPLE_RX_DATA_SIZE) {
				// sanity check
				fprintf(stderr,
					"Config struct is greater than buf size");
				return -E_BB_GENERIC_FAILURE;
			}
			
			writechars[0] = CONFIG_CHAR;
			sent_config_data.actioned_time = (uint32_t) time(NULL);
			memcpy(&writechars[1],msg_ptr,writelen);
			writeUsb(writelen+1);
			current_state = SET_CONFIG;
		} break;

	case GET_VERSION_OP:
		writeSingleChar(GET_VERSION_CHAR);
		current_state = GET_VERSION;
		break;
		
	case GET_DEVICE_TIME_OP:
		writeSingleChar(GET_DEVICE_TIME_CHAR);
		current_state = GET_DEVICE_TIME;
		break;
		
#if 0
	case GET_MEASUREMENT_OP:
		writeSingleChar(GET_MEASUREMENT_CHAR);
		current_state = GET_MEASUREMENT;
		break;
#endif
		
	case ERASE_FLASH_OP:
		writeSingleChar(ERASE_FLASH_CHAR);
		current_state = ERASE_FLASH;
		// erase takes a long time, so extend the normal read timeout
		read_timeout = 1000000;
		//keep_going = 0;
		break;
		
	case SET_LED_COLOUR_OP:
		writechars[0] = SET_LED_COLOUR_CHAR;
		writechars[1] = led_setting;
		DEBUG(printf("led setting %d\n",led_setting);)
		writeUsb(2);
		current_state = SET_LED_COLOUR;
		break;
		
	case GO_TO_SLEEP_OP:
		writeSingleChar(GO_TO_SLEEP_CHAR);
		keep_going = 0;
		break;
		
	case NO_BKP_OP:
		writeSingleChar(WIPE_BKP_CHAR);
		current_state = WAIT_FOR_CONFIRM;
		break;
		
	case READ_DBG_OP:
		writeSingleChar(READ_DBG_CHAR);
		current_state = READ_DBG;
		break;
		
	case READ_PAGE_OP:
		writechars[0] = READ_PAGE_CHAR;
		writechars[1] = (uint8_t) ((read_page_num >> 8) & 0xff);
		writechars[2] = (uint8_t) (read_page_num & 0xff);
		DEBUG(printf("read_page_num %d\n",read_page_num);)
		writeUsb(3);
		current_state = READ_PAGE;
		page_temperature_value = 0;
		if (!read_page_ptr) {
			read_page_ptr = new uint8_t[SAMPLES_PER_PAGE * BYTES_PER_SAMPLE];
			current_page_ptr = read_page_ptr;
		}
		break;
		
	case READ_NEXT_PAGE_OP:
		writeSingleChar(READ_NEXT_PAGE_CHAR);
		current_state = READ_PAGE;
		page_temperature_value = 0;
		if (!read_page_ptr) {
			read_page_ptr = new uint8_t[SAMPLES_PER_PAGE * BYTES_PER_SAMPLE];
			current_page_ptr = read_page_ptr;
		}
		break;
		
#if 0
	case SET_DUMMY_DATA_OP:
		writeSingleChar(SETUP_DUMMY_DATA_CHAR);
		current_state = WAIT_FOR_CONFIRM;
		break;
#endif
				
	case SET_ACCEL_CONFIG_OP:
		writechars[0] = SET_ACCEL_CONFIG_CHAR;
		writechars[1] = rate_and_g_scale;
		DEBUG(printf("led setting %d\n",rate_and_g_scale);)
		writeUsb(2);
		current_state = WAIT_FOR_CONFIRM;
		break;
		
	case READ_ACCEL_CONFIG_OP:
		writeSingleChar(READ_ACCEL_CONFIG_CHAR);
		current_state = READ_ACCEL_CONFIG;
		break;
		
	case READ_FLASH_ACCEL_CONFIG_OP:
		writeSingleChar(READ_FLASH_ACCEL_CHAR);
		current_state = READ_ACCEL_CONFIG;
		break;
		
	case SET_CALIB_DATA_OP:
		writechars[0] = SET_CALIB_CHAR;
		memcpy(writechars + 1,calibration_data,MAX_CALIBRATION_DATA_LEN);
		writeUsb(MAX_CALIBRATION_DATA_LEN+1);
		current_state = WAIT_FOR_CONFIRM;
		break;
		
	case SET_FIRST_DOWNLD_OP:
		writechars[0] = SET_FIRST_DOWNLD_CHAR;
		memcpy(writechars + 1,&first_download,5);
		writeUsb(5);
		current_state = WAIT_FOR_CONFIRM;
		break;
		
	case GET_FIRST_DOWNLD_OP:
		writeSingleChar(GET_FIRST_DOWNLD_CHAR);
		current_state = GET_FIRST_DOWNLD;
		break;
		
	case GET_IS_COMPLETE_OP:
		writeSingleChar(GET_IS_COMPLETE_CHAR);
		current_state = GET_IS_COMPLETE;
		break;
		
	default:
		fprintf(stderr,"Unknown user_op %d\n",user_op);
		return -E_BB_GENERIC_FAILURE;
	}
	return 0;
}

int BioBandIf::stateMachine(int rdlen) {
	
	if (rdlen > 2 && current_state != ESTABLISH_LINK) {
		if (!strncmp("CWA",readchars,3)) {
			return 0;
		}
	}
	switch (current_state) {

	case ESTABLISH_LINK: {
			int loop;
			for (loop = 0; loop <= rdlen - 3; loop++) { 
			
				if (!strncmp("CWA",readchars + loop,3)) {
					DEBUG(printf("Link established %s\n",readchars);)
					if (READ_CONFIG_ONLY == user_op)
						writechars[0] = SEND_FULL_META_CHAR;
					else
						writechars[0] = SEND_META_CHAR;
					writeUsb(1);
					current_state = READ_CONFIG;
					break;
				}
			}
			if(rdlen>0) {
				if ((current_state != READ_CONFIG) &&
						(GO_CHAR != readchars[0])) {
					fprintf(stderr,"establish link - unexpected <%s>\n",
						readchars);
					return -E_BB_UNEXPECTED_DATA_ON_LINK;
				}
			}
		} break;

	case READ_CONFIG: {
			struct config_info* msg = NULL;
			
			if(rdlen>0) {
				if (CONFIG_CHAR == readchars[0]) {
					
					DEBUG(printf("Last run:\n");)
					if (rdlen > (int) sizeof(struct config_info)) {
						msg = (struct config_info*) &readchars[1];
						
						DEBUG(displayConfig(msg,1);)
						
						memcpy(&received_config_data,msg,
							sizeof(struct config_info));
						
						
					} else {
						fprintf(stderr,"No config data?\n");
					}
				
					if (READ_CONFIG_ONLY == user_op) {
						full_config_defined = 1;
						
						// nothing more to do except put the tag back in wait
						// for config mode
						writechars[0] = RETURN_CHAR;
						writechars[1] = 0;
						writeUsb(2);
						return FINISHED;
					}
				
					// after reading the config, instruct the tag what to do
					int ret = instructTheTag(msg);
					if (ret) {
						return ret;
					}
				} else {
					fprintf(stderr,"read config - unexpected <%s>\n",readchars);
					return -E_BB_UNEXPECTED_DATA_ON_LINK;
				}
			}
		} break;

	case READ_BAD_BLOCKS: {
			int loop = 0;
			char* ptr = readchars;
			
			while (loop < rdlen) {
				if ((*ptr == 'b')&&
					(loop + 5 < rdlen)&&
					(*(ptr+5)=='\n')) {
					uint16_t val = (*(ptr+1) << 8) + *(ptr+2);
					if (0x1000 == val) {
						DEBUG(printf("done\n");)
						return FINISHED;
					} else {
						bad_block* bad_block_ptr = new bad_block;
						if (bad_block_ptr) {
							bad_block_ptr->block = val;
							bad_block_ptr->page = *(ptr+3);
							bad_block_ptr->marker = *(ptr+4);
							iBadBlocksPtr->push(bad_block_ptr);
						}
						ptr += 6;
						loop += 6;
					}
				} else {
					// unexpected
					DEBUG(printf("unexpected 0x%02x ",*ptr++);)
					loop++;
				}
			}
		} break;
		
	case READ_DBG:
		if (!strcmp(readchars,"Done")) {
			DEBUG(printf("\nDone\n");)
			return FINISHED;
		} else {
			uint16_t loop;
			char* ptr = readchars;
			for (loop = 0; loop < rdlen; loop++) {
				uint8_t val = *ptr++;
				iDebugDataPtr->push(val);
				if (debug_flag) {
					if (val) {
						printf("0x%02x",val);
						if (val > 0x20 && val < 0x7F)
							printf("(%c)",(char) val);
						printf(" ");
					} else {
						printf("0x00 | ");
					}
				}
			}
		}
		break;

	case READ_TEMPERATURE_LEVELS:
	case READ_BATTERY_LEVELS:
		if (!strcmp(readchars,"Done")) {
			return FINISHED;
		} else {
			uint16_t loop;
			char* ptr = readchars;
			for (loop = 0; loop < (rdlen / 2); loop++) {
				uint16_t val;
				uint8_t* byte = (uint8_t*) &val;
				*byte++ = (uint8_t) ptr[0];
				*byte = (uint8_t) ptr[1];
				iLevelsPtr->push(val);
				DEBUG(printf("\t%d (%d)\n", val, rdlen);)
				ptr = ptr + 2;
			}
		}
		break;

	case SET_IDENTITY:
		if(rdlen>1) {
			if (!strcmp(readchars,"OK")) {
				DEBUG(printf("\nnew band id: %s\n",new_band_id);)
			}
			DEBUG(printf("result <%s>\n",readchars);)
			return FINISHED;
		}
		break;
	
#if 0
	case STREAMING_MODE:
		// simplistic streaming
		// TODO call MSampleObserver to process the data, don't stdout here
		if (!(rdlen % 6)) {
			char* ptr = readchars;
			uint16_t loop;
			for (loop = 0; loop < (rdlen / 6); loop++) {
				printf("%02d 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x (%d)\n",
					data_count,
					(uint8_t) ptr[0],(uint8_t) ptr[1],
					(uint8_t) ptr[2],(uint8_t) ptr[3],
					(uint8_t) ptr[4],(uint8_t) ptr[5],
					rdlen);							
				data_count++;
			}
		} else {
			if (!strcmp(readchars,"Done")) {
				return FINISHED;
			} else {
				fprintf(stderr,"unexpected <rdlen %d>\n",rdlen);
				//return -E_BB_UNEXPECTED_DATA_ON_LINK;
			}
		}
		break;
#endif
		
	case READ_PAGE: {
			uint16_t loop2;
			int read_idx = 0;
			
			total_rec += rdlen;
			while (read_idx < rdlen) {
				
				if (!page_idx && wait_for_start) {
					uint8_t zero_count = 0;
					uint8_t byte_count = 0;
					uint8_t ok = 1;
					if (!strcmp(readchars + read_idx,"Done")) {
						return FINISHED;
					}
					
					// simplistic start of page checking, we expect 00tt00 in
					// one read from the serial buf (where t = page temp byte)
					while (read_idx < rdlen) {
						uint8_t val = (uint8_t) readchars[read_idx++];
						if ((zero_count == 2) && (byte_count < 4)) {
							page_temperature_value <<= 8;
							page_temperature_value &= 0xff00;
							page_temperature_value |= val;
							page_temp_defined = 1;
						} else if (val) {
							zero_count = 0;
							ok = 0;
							fprintf(stderr, "ignored 0x%02x ",val);
						} else {
							zero_count++;
							if (4 == zero_count)
								break;
						}
						byte_count++;
					}
					if (!ok) {
						// Failed to find start of page during this read
						fprintf(stderr," ** Ignored (failed to find start)\n");
						break;
					}
					
					wait_for_start = 0;
					pg_count++;
					if (read_idx == rdlen)
						break;
				}

				if (page_idx < 341) {

					for (loop2 = 0; loop2 < 6; loop2++) {
						uint8_t v = (uint8_t) readchars[read_idx++];
						if (current_page_ptr) {
							*current_page_ptr = v;
							current_page_ptr++;
						}
						uint8_t i = crc_hi ^ v;
						crc_hi = crc_lo ^ table_crc_hi[i];
						crc_lo = table_crc_lo[i];
					}
					data_count++;
					page_idx++;
				}

				if ((341 == page_idx)&&(read_idx < rdlen)) {
					int diff = rdlen - read_idx;
					bool error = true;
					// TODO need to buffer and process
					if (diff >= 4) {
						// process checksum
						uint8_t high = (uint8_t) readchars[read_idx++];
						uint8_t low = (uint8_t) readchars[read_idx++];
						uint8_t badblock = (uint8_t) readchars[read_idx++];
						uint8_t status = (uint8_t) readchars[read_idx++];
						if (status != OK_USED_STATUS) {
							fprintf(stderr,"Bad status: 0x%02x\n",status);
							if (!(status & COLLECT_OK_MASK)) {
								if (status & PAGE_OK_MASK) {
									fprintf(stderr,
										"Potential data loss at this point\n");
								} else {
									fprintf(stderr,"Partial page\n");
									fprintf(stderr,
										"Definate data loss at this point\n");
								}
							} else {
								fprintf(stderr,"Programming error\n");
							}
						} else if (badblock != VALID_BLOCK_INDICATOR) {
							fprintf(stderr,"Bad block!\n");
						} else if ((crc_hi == high)&&(crc_lo == low)) {
							error = false;
						} else {
							fprintf(stderr,
								" CRC ERROR [0x%02x 0x%02x, 0x%02x 0x%02x]"
								" pg %d\n", crc_hi,crc_lo,high,low,pg_count);
						}
					} else {
						fprintf(stderr,"Read CRC/status failure\n");
					}
					if (error) {
						delete[] read_page_ptr;
						read_page_ptr = NULL;
						return FINISHED;
					}
					crc_hi = 0xFF;
					crc_lo = 0xFF;
					page_idx = 0;
					wait_for_start = 1;
				}
			}
		} break;
		
#if 0
	case READ_AS_CSV: {
			uint16_t loop2, loop3;
			int read_idx = 0;
			
			total_rec += rdlen;
			while (read_idx < rdlen) {
				
				if (!page_idx && wait_for_start) {
					uint8_t zero_count = 0;
					uint8_t byte_count = 0;
					uint8_t ok = 1;
					if (!strcmp(readchars + read_idx,"Done")) {
						
						DEBUG(printf("\n%d pages\n",pg_count);)
						DEBUG(printf("%d bytes processed\n", total_rec);)
						return FINISHED;
					}
					
					// simplistic start of page checking, we expect 00tt00 in
					// one read from the serial buf (where t = page temp byte)
					while (read_idx < rdlen) {
						uint8_t val = (uint8_t) readchars[read_idx++];
						if ((zero_count == 2) && (byte_count < 4)) {
							page_temperature_value <<= 8;
							page_temperature_value &= 0xff00;
							page_temperature_value |= val;
							page_temp_defined = 1;
						} else if (val) {
							zero_count = 0;
							ok = 0;
						} else {
							zero_count++;
							if (4 == zero_count)
								break;
						}
						byte_count++;
					}
					if (!ok) {
						// Failed to find start of page during this read
						fprintf(stderr," ** Ignored (failed to find start)\n");
						break;
					}
					
					start_page_time = collect_time;
					wait_for_start = 0;
					pg_count++;
					if (read_idx == rdlen)
						break;
				}

				if (page_idx < 341) {

					sample_obs_ptr->addRawSample(
						(uint8_t*) readchars + read_idx);
						
					if (page_temp_defined) {
						sample_obs_ptr->temperature_raw =
							page_temperature_value;
						page_temp_defined = 0;
						page_temperature_value = 0;
					}

					for (loop2 = 0; loop2 < 3; loop2++) {
						for (loop3 = 0; loop3 < 2; loop3++) {
						
							uint8_t v1 = (uint8_t) readchars[read_idx++];							
							uint8_t cv = crc_hi ^ v1;
							crc_hi = crc_lo ^ table_crc_hi[cv];
							crc_lo = table_crc_lo[cv];
						}
					}
					data_count++;
					page_idx++;
				}

				if ((341 == page_idx)&&(read_idx < rdlen)) {
					int diff = rdlen - read_idx;
					// TODO need to buffer and process
					if (diff >= 4) {
						// process checksum
						uint8_t high = (uint8_t) readchars[read_idx++];
						uint8_t low = (uint8_t) readchars[read_idx++];
						uint8_t badblock = (uint8_t) readchars[read_idx++];
						uint8_t status = (uint8_t) readchars[read_idx++];
						sample_obs_ptr->status_raw = status;
						if (status != OK_USED_STATUS) {
							fprintf(stderr,"Bad status: 0x%02x\n",status);
							if (!(status & COLLECT_OK_MASK)) {
								if (status & PAGE_OK_MASK) {
									fprintf(stderr,
										"Potential data loss at this point\n");
								} else {
									fprintf(stderr,"Partial page\n");
									fprintf(stderr,
										"Definate data loss at this point\n");
								}
							} else {
								fprintf(stderr,"Programming error\n");
							}
						} else if (badblock != VALID_BLOCK_INDICATOR) {
							fprintf(stderr,"Bad block!\n");
							fprintf(stderr,"Programming error\n");
						} else if ((crc_hi == high)&&(crc_lo == low)) {
							//printf(" CRC OK");
							sample_obs_ptr->crc_ok = true;
						} else {
							fprintf(stderr,
								" CRC ERROR [0x%02x 0x%02x, 0x%02x 0x%02x]"
								" pg %d\n", crc_hi,crc_lo,high,low,pg_count);
							sample_obs_ptr->crc_ok = false;
						}
					} else {
						fprintf(stderr,"Read CRC/status failure\n");
					}
					crc_hi = 0xFF;
					crc_lo = 0xFF;
					page_idx = 0;
					wait_for_start = 1;
				}				
			}
		} break;
#endif

	case READ_RAW: {
			
			total_rec += rdlen;
			if (raw_fd && raw_fd != stdout) {
				fwrite(readchars,1,rdlen,raw_fd);
			}
			if (buffer_idx + rdlen < max_transfer_page) {
				memcpy(data_buffer + buffer_idx, readchars, rdlen);
				
				buffer_idx += rdlen;
				checkRawStart();
				
			} else {
				int diff1 = max_transfer_page - buffer_idx;
				
				if (diff1) {
					memcpy(data_buffer + buffer_idx, readchars, diff1);
#if 0
					printf("diff1 %d\n",diff1);
					char* ptr = data_buffer + buffer_idx;
					if (raw_fd == stdout) {
						printf("~~0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
							(uint8_t) ptr[0],(uint8_t) ptr[1],
							(uint8_t) ptr[2],(uint8_t) ptr[3],
							(uint8_t) ptr[4],(uint8_t) ptr[5]);
					}
#endif
				}
				
				processRawData();
				if (sample_obs_ptr->evSamplesCallback()) {
					// TODO - need to send a command to the band to stop it
					// sending further data
				}
				
				// copy left over to start of buffer
				buffer_idx = 0;
				int move_val = rdlen - diff1;
				if (move_val) {
					memcpy(data_buffer, readchars + diff1, move_val);
					buffer_idx += move_val;
#if 0
					char* ptr = data_buffer;
					if (raw_fd == stdout) {
						printf("**");
						for(int mlp = 0; mlp < move_val; mlp++) {
							printf("0x%02x ", (uint8_t) ptr[mlp]);
						}
						printf("\n");
					}
#endif
				}
				
				if (!strcmp(data_buffer,"Done")) {
					
					DEBUG(printf("\n%d pages\n",pg_count);)
					DEBUG(printf("%d bytes processed\n", total_rec);)
					sample_obs_ptr->evDoneCallback();
					return FINISHED;
				}
			}
		} break;

	case SET_CONFIG:
		if(rdlen>0) {
			DEBUG(printf("New config:\n");)
			DEBUG(displayConfig(&sent_config_data,0);)
			DEBUG(printf("Configured <%s>\n",readchars);)
			
			// start
			writeSingleChar(START_CHAR);
			current_state = START;
		}
		break;

	case BATTERY_BURN_MODE:
	case START:
		if(rdlen>0) {
			DEBUG(printf("Started <%s>\n",readchars);)
			return FINISHED;
		}
		if(rdlen==-ENODEV) {
			DEBUG(printf("Device no longer present, assumed sleeping\n");)
			return FINISHED;
		}
		keep_going = 0;
		break;

	case WAIT_FOR_CONFIRM:
		if(rdlen>0) {
			DEBUG(printf("result <%s>\n",readchars);)
			return FINISHED;
		}
		if(rdlen==-ENODEV) {
			DEBUG(printf("Device no longer present\n");)
			return FINISHED;
		}
		break;
		
	case GET_VERSION:
		if(rdlen >= 1) {
			if (readchars[0]) {
				// Temporarily maintain backwards compatibility fw < 1.3
				uint8_t ver_raw = readchars[0];
				uint8_t hwb, fwb;
				DEBUG(printf("fw_hw_version <0x%02x>\n",ver_raw);)
				convHwFwVerToBytes(ver_raw, hwb, fwb);
				// hardcode known fw/hw used before this api change
				switch(fwb) {
					case 0x00: fw_ver = 1.0; break;
					case 0x01: fw_ver = 1.1; break;
					case 0x02: fw_ver = 1.2; break;
					default:
						fprintf(stderr,"Unexpected Fw version\n");
						fw_ver = 0;
						break;
				}
				switch(hwb) {
					case 0: hw_ver= 1.2; break;
					default:
						fprintf(stderr,"Unexpected Hw version\n");
						hw_ver = 0;
						break;
				}
				fw_ver_date_time = readchars+1;
				DEBUG(printf("date/time <%s>\n",readchars+1);)
				return FINISHED;
			} else if(rdlen >= 8) {
				memcpy(&hw_ver,readchars + 1, 4);
				memcpy(&fw_ver,readchars + 5, 4);
				DEBUG(printf("hw_ver <%f>\n",hw_ver);)
				DEBUG(printf("fw_ver <%f>\n",fw_ver);)
				fw_ver_date_time = readchars+9;
				DEBUG(printf("date/time <%s>\n",readchars+9);)
				return FINISHED;
			}
		}
		break;
	
	case GET_DEVICE_TIME:
		if (rdlen >= (int) sizeof(uint32_t)) {
			memcpy(&device_time,readchars, sizeof(uint32_t));
			DEBUG(displayTime(&device_time, false);)
			return FINISHED;
		} else {
//			DEBUG(printf("Failed to receive device time (rdlen %d < %ld)\n",
//				rdlen,sizeof(time_t));)
//			device_time = 0;
		}
		break;
		
#if 0
	case GET_MEASUREMENT:
		if (rdlen >= BYTES_PER_SAMPLE) {
			memcpy(measurement, readchars, BYTES_PER_SAMPLE);
			return FINISHED;
		} else {
//			DEBUG(printf("Failed to receive measurement\n");)
//			memset(measurement, 0, BYTES_PER_SAMPLE);
		}
		break;
#endif
		
	case ERASE_FLASH:
		if(rdlen>1) {
			DEBUG(printf("result <%s>\n",readchars);)
			if (strcmp("OK",readchars))
				return -E_BB_REQUEST_FAILED;
			read_timeout = DEFAULT_READ_TIMEOUT;
			return FINISHED;
		}
		break;
		
	case SET_LED_COLOUR:
		if(rdlen>1) {
			DEBUG(printf("led set result <%s>\n",readchars);)
			if (strcmp("OK",readchars))
				return -E_BB_REQUEST_FAILED;
			return FINISHED;
		}
		break;
		
	case READ_ACCEL_CONFIG:
		if(rdlen >= 1) {
			rate_and_g_scale = readchars[0];
			DEBUG(printf("rate_and_g_scale <0x%02x>\n",rate_and_g_scale);)
			return FINISHED;
		}
		break;
		
	case GET_FIRST_DOWNLD:
		if (rdlen >= (int) sizeof(uint32_t)) {
			memcpy(&first_download,readchars, sizeof(uint32_t));
			DEBUG(displayTime(((time_t*) &first_download), false);)
			return FINISHED;
		}
		break;
		
	case GET_IS_COMPLETE:
		if(rdlen >= 1) {
			is_complete = readchars[0];
			DEBUG(printf("is_complete <0x%02x>\n",is_complete);)
			return FINISHED;
		}
		break;
		
	default:
		fprintf(stderr,"Unknown state %d rec <%s>\n",current_state,readchars);
		break;
	}
	
	return 0;
}
		
int BioBandIf::enterEventLoop(op_state op) {
    
    int ret = 0;
	int rdlen;
    int num_reads = 0;
    int num_nothing = 0;
    
	if (!dev_handle) {
		fprintf(stderr,"Not connected to the device\n");
		return -E_BB_BAND_NOT_CONNECTED;
	}

	user_op = op;
    
    DEBUG(printf("Establishing link with Bioband (op %d)\n",op);)

	writechars[0] = 'g';
	writechars[1] = 0;
	writeUsb(2);
	
	keep_going = 1;
	
	// TODO need to rework this loop - sort into commands that do have responses
	// and those that don't (since enter sleep mode or similar). Also USB issues
	// on different hardware configurations, need consistent approach.
	
	while (keep_going) {
   
		rdlen = readUsb();
		if (rdlen < 0) {
			/* special case: if we're in state START */
			/* we're expecting the Band to disappear, so allow the error
			 * through */
			if (!((current_state==START || current_state==BATTERY_BURN_MODE)
				&& rdlen==-ENODEV)) {
				ret = rdlen;
				DEBUG(perror("USB bulk read failed");)
				DEBUG(printf("result %d",ret);)
				if (!num_reads) {
					printf("** No comms from the Bioband device (%d) **\n",ret);
					break;
				} else {
					DEBUG(printf("\n%d pages, %d reads\n",pg_count,num_reads);)
				}
				if (ret == -4) {
					perror("USB bulk read failed");
					fprintf(stderr,"result %d",ret);
					break;
				} else {
					continue;
				}
			}
		}
		num_reads++;
		if (rdlen>0) {
			
			readchars[rdlen] = '\0';
			num_nothing = 0;
			ret = stateMachine(rdlen);
			if (ret) {
				break;
			}
			if (current_state==START || current_state==BATTERY_BURN_MODE) {
				break;
			}
		} else if (!rdlen) {
			num_nothing++;
			if (num_nothing > 500) {
				ret = -E_BB_NO_RESPONSE_FROM_BAND;
				DEBUG(printf("current_state %d\n",current_state);)
				fprintf(stderr,
					"No response from band (press reset on breakout board)\n");
				break;
			}
		}
    	//printf("read <%s>(%d)\n",readchars,rdlen);
	   
	}
	if (ret > 0)
		ret = 0;
	
	flushUsb();
	
	current_state = ESTABLISH_LINK;

    return ret;
}

// ----

void BioBandIf::checkRawStart() {
	if (wait_for_start && buffer_idx >= PAGE_LEADER) {
		
		int read_idx = 0;
		uint8_t zero_count = 0;
		uint8_t byte_count = 0;
		bool ok = true;
		
		// simplistic start of page checking, we expect 00pp00
		//  (where p = page number)
		while (read_idx < buffer_idx) {
			uint8_t val = (uint8_t) data_buffer[read_idx++];
			if ((zero_count == 2) && (byte_count < 4)) {
				page_num <<= 8;
				page_num &= 0xff00;
				page_num |= val;
			} else if (val) {
				zero_count = 0;
				ok = false;
			} else {
				zero_count++;
				if (4 == zero_count)
					break;
			}
			byte_count++;
		}
		if (ok) {
			if (raw_fd == stdout)
				printf("\npg:%d\n",page_num);
			wait_for_start = 0;
			pg_count++;
		} else {
			// Failed to find start of page during this read
			fprintf(stderr," ** Ignored (failed to find start)\n");
		}
	}
}

void BioBandIf::processRawData() {
	int read_idx = 0;
	
	if (wait_for_start) {
		printf("Ignoring data\n");
	} else {
		bool validate_page = false;
		uint8_t badblock = (uint8_t)
			data_buffer[BAD_BLOCK_ADDR + PAGE_LEADER];
		uint8_t status = (uint8_t)
			data_buffer[PAGE_STATUS_ADDR + PAGE_LEADER];
		sample_obs_ptr->status_raw = status;
		if (status != OK_USED_STATUS) {
			DEBUG(printf("Page: %d Status: 0x%02x\n",page_num,status);)
			if (!(status & COLLECT_OK_MASK)) {
				if (status & PAGE_OK_MASK) {
					DEBUG(printf("Potential data loss before this page\n");)
					validate_page = true;
				} else {
					DEBUG(printf("Partial page\n");)
					DEBUG(printf(
						"Definate data loss part way through this page\n");)
						
					read_idx = PAGE_LEADER;
					page_idx = 0;
					while (page_idx < 341) {
						uint8_t ff_count = 0;
						char* ptr = data_buffer + read_idx;
						for (int loop2 = 0; loop2 < 6; loop2++) {
							if ((uint8_t) ptr[loop2] == 0xff)
								ff_count++;
						}
						if (ff_count == 6)
							break;
							
						sample_obs_ptr->addRawSample(
							(uint8_t*) data_buffer + read_idx);
					
						if (raw_fd == stdout) {
							printf("%02d 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
								data_count,
								(uint8_t) ptr[0],(uint8_t) ptr[1],
								(uint8_t) ptr[2],(uint8_t) ptr[3],
								(uint8_t) ptr[4],(uint8_t) ptr[5]);
						}
										
						data_count++;
						page_idx++;
						read_idx += BYTES_PER_SAMPLE;
					}
					int missing = 341 - page_idx;
					DEBUG(printf("Missing %d samples\n", missing);)
					data_count += missing;	

				}
			} else {
				fprintf(stderr,"Programming error\n");
			}
		} else if (badblock != VALID_BLOCK_INDICATOR) {
			fprintf(stderr,"Bad block!\n");
		} else {
			validate_page = true;
		}
		if (validate_page) {
			// check CRC
			read_idx = PAGE_LEADER;
			page_idx = 0;
			crc_hi = 0xFF;
			crc_lo = 0xFF;
			while (page_idx < 341) {

				char* ptr = data_buffer + read_idx;
				if (raw_fd == stdout) {
					printf("%02d 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\n",
						data_count,
						(uint8_t) ptr[0],(uint8_t) ptr[1],
						(uint8_t) ptr[2],(uint8_t) ptr[3],
						(uint8_t) ptr[4],(uint8_t) ptr[5]);
				}
								
				sample_obs_ptr->addRawSample((uint8_t*) data_buffer + read_idx);
				
				for (int loop2 = 0; loop2 < 3; loop2++) {
					for (int loop3 = 0; loop3 < 2; loop3++) {
					
						uint8_t v1 = (uint8_t) data_buffer[read_idx++];							
						uint8_t cv = crc_hi ^ v1;
						crc_hi = crc_lo ^ table_crc_hi[cv];
						crc_lo = table_crc_lo[cv];
					}
				}
				data_count++;
				page_idx++;
			}

			// process checksum
			uint8_t high = (uint8_t) data_buffer[read_idx++];
			uint8_t low = (uint8_t) data_buffer[read_idx++];
			if ((crc_hi == high)&&(crc_lo == low)) {
				sample_obs_ptr->crc_ok = true;
				if (raw_fd == stdout)
					printf("CRC Ok\n");
			} else {
				sample_obs_ptr->crc_ok = false;
				fprintf(stderr,
					" CRC ERROR [0x%02x 0x%02x, 0x%02x 0x%02x]"
					" pg %d\n", crc_hi,crc_lo,high,low,page_num);
			}
			
		}
		
		{
			// Here be dragons .. currently a maintenance issue
			
			// TODO This needs to be a tied to a file that is shared with the
			// Bioband, so both storing and retrieval code can be easily altered
			// in one place
			
			// It would be good if the future design could be abstracted from
			// the concept of pages as the underlying hardware is likely to
			// change with the eventual obsolescence of the current flash chips
			
			uint8_t* byte_ptr = (uint8_t*)
				&data_buffer[CURRENT_TICK_ADDR + PAGE_LEADER];
			
			uint32_t* ct_ptr = (uint32_t*) byte_ptr;
			sample_obs_ptr->current_tick = (uint32_t) *ct_ptr;
			if (raw_fd == stdout)
				printf("current_tick 0x%08x\n",*ct_ptr);
			byte_ptr += BIOBAND_TICK_SIZE;
			
			uint16_t* tl_ptr = (uint16_t*) byte_ptr;
			sample_obs_ptr->temperature_raw = *tl_ptr;
			if (raw_fd == stdout) {
				double tl_fl = convTempBinToCelsius(*tl_ptr);
				printf("tl 0x%04x (%.02fC)\n",*tl_ptr,tl_fl);
			}
			byte_ptr += TEMP_LEVEL_SIZE;
			
			if (!page_num) {
				
				sample_obs_ptr->additional_present = true;
				
				uint16_t* bl_ptr = (uint16_t*) byte_ptr;
				sample_obs_ptr->battery_raw = *bl_ptr;
				if (raw_fd == stdout) {
					double bl_fl = convADCToVoltage(*bl_ptr);
					printf("bl 0x%04x (%.02fV)\n",*bl_ptr,bl_fl);
				}
				byte_ptr += BATTERY_LEVEL_SIZE;
			
				if (*byte_ptr) {
					sample_obs_ptr->band_id = (char*) byte_ptr;
					if (raw_fd == stdout)
						printf("band <%s>\n",byte_ptr);
				} else {
					if (raw_fd == stdout)
						printf("no band id!\n");
				}
				byte_ptr += MAX_ID_LEN;
				
				if (*byte_ptr) {
					sample_obs_ptr->subject_id = (char*) byte_ptr;
					if (raw_fd == stdout)
						printf("sj <%s>\n",byte_ptr);
				} else {
					if (raw_fd == stdout)
						printf("no sj id!\n");
				}
				byte_ptr += MAX_ID_LEN;
				
				if (*byte_ptr) {
					sample_obs_ptr->test_id = (char*) byte_ptr;
					if (raw_fd == stdout)
						printf("test <%s>\n",byte_ptr);
				} else {
					if (raw_fd == stdout)
						printf("no test id!\n");
				}
				byte_ptr += MAX_ID_LEN;
				
				if (*byte_ptr) {
					sample_obs_ptr->centre_id = (char*) byte_ptr;
					if (raw_fd == stdout)
						printf("centre <%s>\n",byte_ptr);
				} else {
					if (raw_fd == stdout)
						printf("no centre id!\n");
				}
				byte_ptr += MAX_CENTRE_ID_LEN;
				
				for (int loop = 0; loop < MAX_CALIBRATION_DATA_LEN; loop++)
					sample_obs_ptr->calibration_data[loop] = *byte_ptr++;
				
				uint32_t* sm_ptr = (uint32_t*) byte_ptr;
				sample_obs_ptr->req_total_num_samples = *sm_ptr;
				// reduce by one
				sample_obs_ptr->req_total_num_samples--;
				if (raw_fd == stdout)
					printf("mx samples 0x%08x\n",*sm_ptr);
				byte_ptr += MAX_SAMPLES_SIZE;
			
				uint32_t* collect_start_ptr = (uint32_t*) byte_ptr;
				sample_obs_ptr->collect_start_time =
					(time_t) *collect_start_ptr;
				if (raw_fd == stdout)
					printf("collect_start_time 0x%08x\n",*collect_start_ptr);
				byte_ptr += EPOC_TIME_SIZE;
			
				sample_obs_ptr->accel_conf_raw = *byte_ptr;
				if (raw_fd == stdout)
					printf("accel config 0x%02x\n",*byte_ptr);
				byte_ptr += ACCEL_CONFIG_SIZE;
				
				sample_obs_ptr->fw_hw_version_raw = *byte_ptr;
				if (raw_fd == stdout)
					printf("fw hw version 0x%02x\n",*byte_ptr);
				byte_ptr += VERSION_SIZE;
				
			} else if (1 == page_num) {
				if (status_page_found) {
					// the status page is only stored once on the Bioband - so
					// ignore the unused bytes here from a later second page
					byte_ptr += EPOC_TIME_SIZE + BIOBAND_TICK_SIZE +
						MAX_SAMPLES_SIZE + EPOC_TIME_SIZE;
				} else {
				
					sample_obs_ptr->status_present = true;
					status_page_found = true;
					
					// these fields may or may not be present (assume 0xFFs =
					// not), likely due to loss of battery before data written
					// or flash block corrupted (& the data is not duplicated
					// like the first page info).
					
					uint32_t* download_time_ptr = (uint32_t*) byte_ptr;
					if (*byte_ptr == 0xFF) {
						// assume the data has not yet been downloaded
						if (raw_fd == stdout)
							printf("No download_time set\n");
					} else {
						sample_obs_ptr->first_downloaded =
							(time_t) *download_time_ptr;
						if (raw_fd == stdout)
							printf("first download_time 0x%08x\n",
								*download_time_ptr);
					}
					byte_ptr += EPOC_TIME_SIZE;
				
					uint32_t* etick_ptr = (uint32_t*) byte_ptr;
					if (*byte_ptr == 0xFF) {
						// likely the battery failed before collect finished
						if (raw_fd == stdout)
							printf("No end_tick set\n");
					} else {
						sample_obs_ptr->end_tick = (uint32_t) *etick_ptr;
						if (raw_fd == stdout)
							printf("end tick 0x%08x\n",*etick_ptr);
					}
					byte_ptr += BIOBAND_TICK_SIZE;
			
					uint32_t* num_samples_ptr = (uint32_t*) byte_ptr;
					if (*byte_ptr == 0xFF) {
						// likely the battery failed before collect finished
						if (raw_fd == stdout)
							printf("No num_samples set\n");
					} else {
						sample_obs_ptr->num_samples = (uint32_t)
							*num_samples_ptr;
						if (raw_fd == stdout)
							printf("num samples 0x%08x\n",*num_samples_ptr);
					}
					byte_ptr += MAX_SAMPLES_SIZE;
			
					uint32_t* actioned_time_ptr = (uint32_t*) byte_ptr;
					if (*byte_ptr == 0xFF) {
						// presumably the first flash block must have been
						// corrupted
						if (raw_fd == stdout)
							printf("No actioned time set\n");
					} else {
						sample_obs_ptr->actioned_time = (time_t)
							*actioned_time_ptr;
						if (raw_fd == stdout)
							printf("actioned_time 0x%08x\n",
								*actioned_time_ptr);
					}
					byte_ptr += EPOC_TIME_SIZE;
				}
			
			}
			
			if (*byte_ptr) {
				uint8_t dbg_sz = *byte_ptr++;
				if (raw_fd == stdout)
					printf("dbg %d: <",dbg_sz);
				for(uint8_t lp = 0; lp < dbg_sz; lp++) {
					uint8_t val = *byte_ptr++;
					sample_obs_ptr->dbg_raw.push_back(val);
					if (val > 0x1F && val < 0x7F) {
						if (raw_fd == stdout)
							printf("%c",val);
					} else {
						if (raw_fd == stdout) {
							if (val)
								printf("0x%02x ",val);
							else
								printf(" ");
						}
					}
				}
				if (raw_fd == stdout)
					printf(">\n");
			}
		}
	}

	wait_for_start = 1;
}

// -----------------------------------------------------------------------------

// Interface methods

bool BioBandIf::isValid() {
	return dev_handle;
}

const string BioBandIf::errorToString(int aErrCode) {
	string errStr;
	if (aErrCode < 0) {
		errStr = "Unknown Error";
		if (aErrCode < -E_BB_GENERIC_FAILURE) {
			errStr = strerror(-aErrCode);
		} else {
			// since just a limited number, hardcode the strings here
			switch (aErrCode) {
			case -E_BB_GENERIC_FAILURE:
				errStr = "Generic failure";
				break;
			case -E_BB_BAD_PARAM:
				errStr = "Bad parameter supplied";
				break;
			case -E_BB_FILE_EXISTS:
				errStr = "File already exists";
				break;
			case -E_BB_FAILED_TO_OPEN_FILE_FOR_WRITE:
				errStr = "Failed to open file for write";
				break;
			case -E_BB_BAD_ID:
				errStr = "Bad id";
				break;
			case -E_BB_BAND_NOT_CONNECTED:
				errStr = "Bioband is not connected";
				break;
			case -E_BB_BAND_ALREADY_CONNECTED:
				errStr = "Bioband is already connected";
				break;
			case -E_BB_NO_SERIAL_NUMBER_MATCH:
				errStr = "No match for the Bioband serial number";
				break;
			case -E_BB_USB_GENERIC_FAILURE:
				errStr = "USB generic failure";
				break;
			case -E_BB_USB_READ_FAILURE:
				errStr = "USB read failure";
				break;
			case -E_BB_NOT_DATA_ON_BAND:
				errStr = "No data on the Bioband";
				break;
			case -E_BB_UNEXPECTED_DATA_ON_LINK:
				errStr = "Unexpected data on the link";
				break;
			case -E_BB_REQUEST_FAILED:
				errStr = "Request failed";
				break;
			case -E_BB_NO_RESPONSE_FROM_BAND:
				errStr = "No response from the Bioband";
				break;
			case -E_BB_FAILED_TO_OPEN_FILE_FOR_READ:
				errStr = "Failed to open file for read";
				break;
			case -E_BB_MISSING_CALLBACK_PTR:
				errStr = "Programming error - Missing callback pointer";
				break;
			default:
				// programming error
				fprintf(stderr,"Unknown Bioband code (%d)\n",aErrCode);
				break;		
			}
		}
	}
	return errStr;
}

const int BioBandIf::getHwFwVersions(float& aHw,float& aFw, string& aFwDate) {
	
	int retval;
	aHw = 0;
	aFw = 0;
	retval =  enterEventLoop(GET_VERSION_OP);	
	if (!retval) {
		aHw = hw_ver;
		aFw = fw_ver;
		aFwDate = fw_ver_date_time;
	}
	return retval;
}

const void BioBandIf::getBbApiVersion(float& aVer) {
	// For now return 1.0
	aVer = 1.0;
}

unsigned int BioBandIf::getBatteryVoltage() {
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval) {
		DEBUG(printf("  battery level\t\t%d\n",
			received_config_data.battery_level);)
		return received_config_data.battery_level;
	}
	return 0;
}

const string BioBandIf::getBandId() {
	string val;
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval && received_config_data.band_id[0]) {
		char tmp_band_id[MAX_ID_LEN + 1];
		memset(tmp_band_id,0,MAX_ID_LEN + 1);
		memcpy(tmp_band_id, received_config_data.band_id, MAX_ID_LEN);
		val = tmp_band_id;
		DEBUG(printf("  band id\t\t%s\n",tmp_band_id);)
	} else { 
		DEBUG(printf("  band id\t\tnot set?\n");)
		val = BB_UNKNOWN_STR;
	}
	return val;
}

const string BioBandIf::getSubjectDetails() {
	string val;
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval && received_config_data.subject_id[0]) {
		char tmp_subject_id[MAX_ID_LEN + 1];
		memset(tmp_subject_id,0,MAX_ID_LEN + 1);
		memcpy(tmp_subject_id, received_config_data.subject_id, MAX_ID_LEN);
		val = tmp_subject_id;
		DEBUG(printf("  subject id\t\t%s\n",tmp_subject_id);)
	} else  {
		DEBUG(printf("  subject id\t\tnot set?\n");)
		val = BB_UNKNOWN_STR;
	}
	return val;
}

const string BioBandIf::getTestDetails() {
	string val;
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval && received_config_data.test_id[0]) {
		char tmp_test_id[MAX_ID_LEN + 1];
		memset(tmp_test_id,0,MAX_ID_LEN + 1);
		memcpy(tmp_test_id, received_config_data.test_id, MAX_ID_LEN);
		val = tmp_test_id;
		DEBUG(printf("  test id\t\t%s\n",tmp_test_id);)
	} else  {
		DEBUG(printf("  test id\t\tnot set?\n");)
		val = BB_UNKNOWN_STR;
	}
	return val;
}

const string BioBandIf::getCentreId() {
	string val;
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval && received_config_data.centre_id[0]) {
		char tmp_centre_id[MAX_CENTRE_ID_LEN + 1];
		memset(tmp_centre_id,0,MAX_CENTRE_ID_LEN + 1);
		memcpy(tmp_centre_id, received_config_data.centre_id,
			MAX_CENTRE_ID_LEN);
		val = tmp_centre_id;
		DEBUG(printf("  centre id\t\t%s\n",tmp_centre_id);)
	} else  {
		DEBUG(printf("  centre id\t\tnot set?\n");)
		val = BB_UNKNOWN_STR;
	}
	return val;
}

int BioBandIf::getStoredSize() {
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval) {
		DEBUG(printf("  samples recorded\t%d\n",
			received_config_data.max_samples);)
		retval = received_config_data.max_samples * BYTES_PER_SAMPLE;
	}
	return retval;
}

int BioBandIf::getPageCount() {

	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval) {
		DEBUG(printf("  samples recorded\t%d\n",
			received_config_data.max_samples);)
		retval = received_config_data.max_samples / SAMPLES_PER_PAGE;
	}
	return retval;
}

int BioBandIf::getSampleTimings(time_t& actioned_time,
	time_t& sample_start_time, time_t& sample_end_time) {
		
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval) {
		DEBUG(printf("  actioned_time\t%d\n",
			received_config_data.actioned_time);)
		DEBUG(printf("  collect_start_time\t%d\n",
			received_config_data.collect_start_time);)
		DEBUG(printf("  number_of_ticks\t%d\n",
			received_config_data.number_of_ticks);)
		actioned_time = (time_t) received_config_data.actioned_time;
		sample_start_time = (time_t) received_config_data.collect_start_time;
		sample_end_time = convTicksToTime(sample_start_time,
			received_config_data.number_of_ticks);
		DEBUG(printf("  end_time\t%ld\n",sample_end_time);)
	}
	return retval;
}

int BioBandIf::readPage(const uint16_t page, uint8_t*& page_mem,
	uint16_t& temp_val) {
	// Note pages are numbered from 1
	if (page < 1) {
		DEBUG(printf("Invalid page %d, the first page is 1\n",page);)
		return -E_BB_BAD_PARAM;
	}
	
	read_page_num = page;
	int retval = enterEventLoop(READ_PAGE_OP);
	if (!retval) {
		// pass ownership
		page_mem = read_page_ptr;
		read_page_ptr = NULL;
		temp_val = page_temperature_value;
	}
	return retval;
}

int BioBandIf::readNextPage(uint8_t*& page_mem, uint16_t& temp_val) {
	int retval = enterEventLoop(READ_NEXT_PAGE_OP);
	if (!retval) {
		// pass ownership
		page_mem = read_page_ptr;
		read_page_ptr = NULL;
		temp_val = page_temperature_value;
	}
	return retval;
}

#if 0
int BioBandIf::readAllSamples(MDataObserver* aCallbackPtr) {
	int retval = -E_BB_BAD_PARAM;
	if (aCallbackPtr) {
		sample_obs_ptr = aCallbackPtr;
		sample_obs_ptr->reset();
		retval = enterEventLoop(READ_STREAM_OP);
		if (!retval)
			retval = total_rec;
		sample_obs_ptr = NULL;
	} else {
		DEBUG(printf("Null callback ptr?\n");)
	}
	return retval;
}
#endif

int BioBandIf::readRawFromBand(FILE* write_fd_ptr) {
	int retval;
	raw_fd = NULL;
	if (debug_flag)
		raw_fd = stdout;
	if (write_fd_ptr) {
		raw_fd = write_fd_ptr;
	}
	if (!sample_obs_ptr) {
		return -E_BB_MISSING_CALLBACK_PTR;
	}
	DEBUG(printf("readRawFromBand\n");)
	retval = enterEventLoop(READ_RAW_OP);
	raw_fd = NULL;
	
	sample_obs_ptr = NULL;
	
	if (!retval)
		retval = total_rec;
	return retval;
}

int BioBandIf::readRawFromFile(FILE* read_fd_ptr) {
	int retval = 0;
	raw_fd = NULL;
	if (debug_flag)
		raw_fd = stdout;
	if (!read_fd_ptr) {
		return -E_BB_FAILED_TO_OPEN_FILE_FOR_READ;
	}
	if (!sample_obs_ptr) {
		return -E_BB_MISSING_CALLBACK_PTR;
	}
	page_num = 0;
	wait_for_start = 1;
	pg_count = 0;
	page_idx = 0;
	buffer_idx = 0;
	total_rec = 0;
	collect_time = 0;
	status_page_found = false;
	
	while (!feof(read_fd_ptr)) {
		buffer_idx = fread(data_buffer,1,max_transfer_page,read_fd_ptr);
		DEBUG(printf("buffer_idx %d\n",buffer_idx);)
		total_rec += buffer_idx;
		if (buffer_idx < max_transfer_page) {
			if (!strcmp(data_buffer,"Done")) {			
				DEBUG(printf("\n%d pages\n",pg_count);)
				DEBUG(printf("%d bytes processed\n", total_rec);)
				sample_obs_ptr->evDoneCallback();
				break;
			}
		}
		checkRawStart();
		processRawData();
		if (sample_obs_ptr->evSamplesCallback())
			break;
	}
		
	sample_obs_ptr = NULL;

	if (!retval)
		retval = total_rec;
	return retval;
}

int BioBandIf::setRawDataCallbackPtr(MDataObserver* aCallbackPtr) {
	int retval = BB_SUCCESS;
	if (aCallbackPtr) {
		sample_obs_ptr = aCallbackPtr;
		sample_obs_ptr->reset();
	} else {
		DEBUG(printf("Null callback ptr?\n");)
		retval = -E_BB_BAD_PARAM;
	}
	return retval;
}

int BioBandIf::getBadBlocks(queue<bad_block*>* bad_blocks_ptr) {
	
	int retval = -E_BB_BAD_PARAM;
	if (bad_blocks_ptr) {
		iBadBlocksPtr = bad_blocks_ptr;
		retval = enterEventLoop(READ_BAD_BLOCKS_OP);
	}
	return retval;
}

int BioBandIf::getBatteryLevels(queue<uint16_t>* levels_ptr) {
	
	int retval = -E_BB_BAD_PARAM;
	if (levels_ptr) {
		iLevelsPtr = levels_ptr;
		retval = enterEventLoop(READ_BATTERY_LEVEL_OP);
	}
	return retval;
}

int BioBandIf::getTemperatureLevels(queue<uint16_t>* levels_ptr) {
	
	int retval = -E_BB_BAD_PARAM;
	if (levels_ptr) {
		iLevelsPtr = levels_ptr;
		retval = enterEventLoop(READ_TEMPERATURE_LEVEL_OP);
	}
	return retval;
}

const int BioBandIf::getGmtDeviceTime(time_t& gmt_time) {
	int retval;
	device_time = 0;
	retval = enterEventLoop(GET_DEVICE_TIME_OP);
	if (!retval)
		gmt_time = device_time;
	return retval;
}

#if 0
int BioBandIf::getMeasurement(uint16_t& x, uint16_t& y, uint16_t& z) {
	int retval = enterEventLoop(GET_MEASUREMENT_OP);
	x = *((uint16_t*) measurement);
	y = *((uint16_t*) measurement + 1);
	z = *((uint16_t*) measurement + 2);
	DEBUG(printf("x 0x%04x y 0x%04x z 0x%04x\n",x,y,z);)
	return retval;
}
#endif

int BioBandIf::setBandId(const string& id) {
	int retval = 0;
	
	new_band_id_length = id.size();
	if (new_band_id_length <= MAX_ID_LEN) {

		memset(new_band_id,0,MAX_ID_LEN + 1);
		memcpy(new_band_id, id.c_str(), new_band_id_length);

		retval = enterEventLoop(SET_ID_OP);
	} else {
		printf("Invalid band Id %s\n",id.c_str());
		retval = -E_BB_BAD_ID;
	}
	return retval;
}

int BioBandIf::setSubjectDetails(const string& id) {
	int retval = 0;
	int sz = id.size();
	
	if (sz <= MAX_ID_LEN) {
		
		memset(sent_config_data.subject_id,0,MAX_ID_LEN);
		memcpy(sent_config_data.subject_id, id.c_str(), sz);
		
	} else {
		printf("Invalid subject Id %s\n",id.c_str());
		retval = -E_BB_BAD_ID;
	}
	return retval;
}

int BioBandIf::setTestDetails(const string& id) {
	int retval = 0;
	int sz = id.size();
	
	if (sz <= MAX_ID_LEN) {
		
		memset(sent_config_data.test_id,0,MAX_ID_LEN);
		memcpy(sent_config_data.test_id, id.c_str(), sz);
		
	} else {
		printf("Invalid test Id %s\n",id.c_str());
		retval = -E_BB_BAD_ID;
	}
	return retval;
}

int BioBandIf::setCentreId(const string& id) {
	int retval = 0;
	int sz = id.size();
	
	if (sz <= MAX_CENTRE_ID_LEN) {
		
		memset(sent_config_data.centre_id,0,MAX_CENTRE_ID_LEN);
		memcpy(sent_config_data.centre_id, id.c_str(), sz);
		
	} else {
		printf("Invalid centre Id %s\n",id.c_str());
		retval = -E_BB_BAD_ID;
	}
	return retval;
}

int BioBandIf::clearStoredMeasurements() {
	return enterEventLoop(ERASE_FLASH_OP);
}

int BioBandIf::start(const unsigned int wait_mins,
	const unsigned int run_mins) {
		
	accel_data_rate dr;
	accel_g_scale gs;
	int retval = readAccelConfig(&dr, &gs);
	if (retval) {
		// for some reason cannot access the config
		dr = CWA_50HZ;
	}
	
	// build in some tolerance for rough accelerometer freq characteristics 
	int sample_per_sec_plus_ten_percent;
	switch(dr) {
		case CWA_100HZ: sample_per_sec_plus_ten_percent = 110; break;
		case CWA_400HZ: sample_per_sec_plus_ten_percent = 440; break;
		case CWA_1000HZ: sample_per_sec_plus_ten_percent = 1100; break;
		case CWA_50HZ:
		default:
			sample_per_sec_plus_ten_percent = 55;
			break;
	}
	
	// although trying to stay away from physical characteristics of the
	// band, it helps to capture whole numbers of pages since the band
	// can only download full pages.
	uint approx_num_samples =
		run_mins * 60 * sample_per_sec_plus_ten_percent;
	uint num_pages = approx_num_samples / SAMPLES_PER_PAGE;
	num_pages++;
	
	sent_config_data.max_samples = (num_pages * SAMPLES_PER_PAGE) + 1;
	sent_config_data.standby_before_collection_time_mins = wait_mins;

	retval = enterEventLoop(COLLECT_OP);
	
	// not possible to talk to the device any more
	closeUsb();

	return retval;
}

int BioBandIf::setLed(led_colour colour) {
	
	// switch off debug mode (since uses the led)
	sent_config_data.mode = 1;
	
	led_setting = colour;
	
	return enterEventLoop(SET_LED_COLOUR_OP);
}

int BioBandIf::goToSleep() {
	return enterEventLoop(GO_TO_SLEEP_OP);
}

#if 0
int BioBandIf::setUpDummyData() {
	return enterEventLoop(SET_DUMMY_DATA_OP);
}
#endif

int BioBandIf::noBkp() {
	return enterEventLoop(NO_BKP_OP);
}

int BioBandIf::getDebugBuffer(queue<uint8_t>* debug_buffer_ptr) {
	
	int retval = -E_BB_BAD_PARAM;
	if (debug_buffer_ptr) {
		iDebugDataPtr = debug_buffer_ptr;
		retval = enterEventLoop(READ_DBG_OP);
	}
	return retval;
}

int BioBandIf::setAccelConfig(accel_data_rate dr, accel_g_scale gs) {
	rate_and_g_scale = encodeRateAndGscale(dr, gs);
	return enterEventLoop(SET_ACCEL_CONFIG_OP);
}

int BioBandIf::readAccelConfig(accel_data_rate* dr, accel_g_scale* gs) {
	int retval = enterEventLoop(READ_ACCEL_CONFIG_OP);
	if (!retval) {
		retval = decodeRateAndGscale(rate_and_g_scale, dr, gs);
	}
	return retval;
}

int BioBandIf::readFlashAccelConfig(accel_data_rate* dr, accel_g_scale* gs) {
	int retval = enterEventLoop(READ_FLASH_ACCEL_CONFIG_OP);
	if (!retval) {
		retval = decodeRateAndGscale(rate_and_g_scale, dr, gs);
	}
	return retval;
}

int BioBandIf::recordCalibrationData(uint16_t* params) {
	memcpy(calibration_data,params,MAX_CALIBRATION_DATA_LEN);
	return enterEventLoop(SET_CALIB_DATA_OP);
}

int BioBandIf::setFirstDownloadTime(time_t first_download_time) {
	first_download = (uint32_t) first_download_time;
	return enterEventLoop(SET_FIRST_DOWNLD_OP);
}

int BioBandIf::getFirstDownloadTime(time_t& first_download_time) {
	int retval;
	first_download = 0;
	retval = enterEventLoop(GET_FIRST_DOWNLD_OP);
	if (!retval)
		first_download_time = (time_t) first_download;
	return retval;
}

int BioBandIf::getCalibrationData(uint16_t* params) {
	
	int retval = 0;
	if (!full_config_defined)
		retval = enterEventLoop(READ_CONFIG_ONLY);
	if (!retval) {
		if (debug_flag) {
			uint16_t* val = (uint16_t*) received_config_data.cali_data;
			printf("  stored calibration (gain,offset): ");
			printf("x(%u, ",*val);
			val++;
			printf("%u) ",*val);
			val++;
			printf("y(%u, ",*val);
			val++;
			printf("%u) ",*val);
			val++;
			printf("z(%u, ",*val);
			val++;
			printf("%u)\n",*val);
		}
		memcpy(params,received_config_data.cali_data,MAX_CALIBRATION_DATA_LEN);
	}
	return retval;
}

int BioBandIf::getIfCaptureExistsAndComplete(bool& exists, bool& complete) {
	int retval;
	is_complete = 0;
	exists = false;
	complete = false;
	retval = enterEventLoop(GET_IS_COMPLETE_OP);
	if (!retval) {
		if (NO_CAPTURE_TO_CHECK != is_complete) {
			exists = true;
			complete = (COMPLETE_CAPTURE == is_complete);
		}
	}
	return retval;
}



// EOF
