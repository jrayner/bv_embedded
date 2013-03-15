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

#define RAW_EXT ".raw"
#define DBG_BUF_EXT ".dbuf"

// TODO check why cleanup needed, destructor not being called
BioBandIf bandif;

char version_str[] = __DATE__ " " __TIME__;

static void quitProc(int a) {
	printf("Cleaning up\n");
	bandif.cleanup();
	exit(a);
}

static void displayHelp() {
	printf("Version: %s\n\n",version_str);
	printf("Optional cmd line params:\n");
	printf("\t-snum <serial number>\tspecific Bioband serial number to connect to\n");
	printf("\t-autodown\t\talways download if data not already downloaded\n");
	printf("\t-downdir <dir_path>\tdirectory to place the download files in\n");
	printf("\t-nocoll\t\tdon't ask collect question\n");
	printf("\t-centre <id>\t\tset the centre id to use for any sample collection\n");
	printf("\n");
}

static int getLocalTime(char* aString, size_t aLen,
	const time_t aGmtTime) {
	int retval = 0;
    struct tm *tmp = localtime(&aGmtTime);
    if (!tmp) {
		fprintf(stderr, "localtime failed\n");
        retval = -E_BB_GENERIC_FAILURE;
    } else if (strftime(aString, aLen, "%d/%m/%y %H:%M:%S", tmp) == 0) {
		fprintf(stderr, "strftime failed\n");
        retval = -E_BB_GENERIC_FAILURE;
    }
	return retval;
}

FILE* openUniqueFile(string& filename) {
	FILE* fd_ptr = NULL;
	if (filename.size()) {
		string original = filename;
		int val = 1;
		fd_ptr = fopen(filename.c_str(),"r");
		while (fd_ptr) {
			char tmp[10];
			filename = original;
			fclose(fd_ptr);
			sprintf(tmp,"_%d",val);
			filename += tmp;
			fd_ptr = fopen(filename.c_str(),"r");
			val++;
		}
		fd_ptr = fopen(filename.c_str(),"w");
	}
	return fd_ptr;
}

// ----

struct rawData : public MDataObserver {
	
	rawData();
	virtual ~rawData() {}
	
	void outputSummary();
	
	virtual bool evSamplesCallback();
	virtual void evDoneCallback();
	
	bool summary_output;
	time_t start_time;
	uint32_t num_samples_received;
	uint32_t expected_total;
	uint32_t last_percentage;
	uint32_t percentage;
};

rawData::rawData() :
	summary_output(false),
	start_time(0),
	num_samples_received(0),
	expected_total(0),
	last_percentage(0),
	percentage(0) {
}

void rawData::outputSummary() {

	string start_time_str = BB_UNKNOWN_STR;
	char time_str[20];
	
	printf("\n\tSummary of the first page of raw file:\n");
	
	printf("\tpage crc:\t0x%02x (", (int) crc_ok);
	if (crc_ok)
		printf("OK");
	else
		printf("FAILED");
	printf(")\n");
	printf("\tpage status:\t0x%02x (", status_raw);
	if (status_raw == OK_USED_STATUS) {
		printf("OK");
	} else {
		if (!(status_raw & COLLECT_OK_MASK)) {
			if (status_raw & PAGE_OK_MASK) {
				printf("Potential data loss at this point");
			} else {
				printf("Partial page - Definate data loss at this point");
			}
		} else {
			printf("failed to decode\n");
		}
	}
	printf(")\n");
	printf("\tcurrent_tick:\t%d\n",current_tick);
	
	time_t curr_time = convTicksToTime(start_time, current_tick);
	if (!getLocalTime(time_str, 20, curr_time))
		printf("\tpage time:\t%ld (local:%s)\n",curr_time, time_str);
			
	double tl_fl = convTempBinToCelsius(temperature_raw);
	printf("\ttemp level:\t0x%04x (%.02fC)\n",temperature_raw,tl_fl);
			
	if (additional_present) {
		
		double bl_fl = convADCToVoltage(battery_raw);
		printf("\tbattery:\t0x%04x (%.02fV)\n",battery_raw,bl_fl);
	
		if (band_id.empty()) {
			printf("\tno band id!\n");
		} else {
			printf("\tband id:\t%s\n",band_id.c_str());
		}
		
		if (subject_id.empty()) {
			printf("\tno subject id!\n");
		} else {
			printf("\tsubject id:\t%s\n",subject_id.c_str());
		}
		
		if (test_id.empty()) {
			printf("\tno test id!\n");
		} else {
			printf("\ttest id:\t%s\n",test_id.c_str());
		}
		
		if (centre_id.empty()) {
			printf("\tno centre id!\n");
		} else {
			printf("\tcentre id:\t%s\n",centre_id.c_str());
		}
		
		// calibration defined as 6 unsigned 16bit values
		printf("\tcalibration (gain,offset): ");
		uint16_t* val = (uint16_t*) calibration_data;
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
		
		printf("\tmx samples:\t%d\n",req_total_num_samples);
		
		if (!getLocalTime(time_str, 20, collect_start_time))
			printf("\tcollect start:\t%ld (local:%s)\n",
				(time_t) collect_start_time, time_str);
			
		accel_data_rate rate;
		accel_g_scale scale;
		int ret = decodeRateAndGscale(accel_conf_raw, &rate, &scale);
		printf("\taccel conf:\t0x%02x (",accel_conf_raw);
		if (ret) {
			printf("* WARNING - failed to decode *");
		} else {
			printf("rate:");
			switch(rate) {
				case CWA_50HZ: printf("50Hz"); break;
				case CWA_100HZ: printf("100Hz"); break;
				case CWA_400HZ: printf("400Hz"); break;
				case CWA_1000HZ: printf("1000Hz"); break;
				default:
					printf(BB_UNKNOWN_STR);
					break;
			}
			printf(" scale:");
			switch(scale) {
				case CWA_2G: printf("2g"); break;
				case CWA_4G: printf("4g"); break;
				case CWA_8G: printf("8g"); break;
				default:
					printf(BB_UNKNOWN_STR);
					break;
			}
		}
		printf(")\n");

		uint8_t fw_ver, hw_ver;
		convHwFwVerToBytes(fw_hw_version_raw, hw_ver, fw_ver);
		printf("\tfwhw ver:\t0x%02x (",fw_hw_version_raw);
		printf("hw:0x%02x fw:0x%02x)\n\n",hw_ver,fw_ver);
	}
}

void rawData::evDoneCallback() {
	if (num_samples_received == expected_total) {
		printf("100%%");
	} else {
		expected_total++;
		// TODO warning if expected_total > 89 million, may have run out of room 
	}
	printf("\n\n\tnum samples received:\t%d\n",num_samples_received);
	printf("\texpected total:\t\t%d\n",expected_total);
	// TODO Warning if totals are greater 5% different
}

bool rawData::evSamplesCallback() {
	
	if (additional_present) {
		if (!expected_total) {
			start_time = collect_start_time;
			expected_total = req_total_num_samples;
		}
	}
	
	if (status_raw != OK_USED_STATUS) {
		char time_str[20];
		fprintf(stderr,"\n");
		time_t curr_time = convTicksToTime(start_time, current_tick);
		if (!getLocalTime(time_str, 20, curr_time))
			fprintf(stderr,"%s ",time_str);

		fprintf(stderr," Status: 0x%02x\n", status_raw);
		if (!(status_raw & COLLECT_OK_MASK)) {
			if (status_raw & PAGE_OK_MASK) {
				fprintf(stderr,
					"Potential data loss just before this time\n");
			} else {
				fprintf(stderr,"Partial page\n");
				fprintf(stderr,
					"Definate data loss part way at this time\n");
			}
		}
	}
	
	if (summary_output) {
		outputSummary();
		summary_output = false;
	}
	
	if (!raw_sample_list.empty()) {
		num_samples_received += raw_sample_list.size();
		if (expected_total) {
			uint64_t expected_num = expected_total;
			uint64_t tmp_num = num_samples_received;
			tmp_num *= 100;
			percentage = tmp_num / expected_num;
			uint diff_percent = percentage - last_percentage;
			if (diff_percent) {
				for (uint loop = last_percentage; loop < percentage; loop++) {
					if (loop % 10)
						printf(".");
					else
						printf("%d%%",loop);
				}
				fflush(stdout);
				last_percentage = percentage;
			}
		}
	}
	
	reset();
	
	return false;
}

// ----

void textError(int errorCode) {
	string errStr = bandif.errorToString(errorCode);
	printf("\tError: %s\n",errStr.c_str());
}
   		
string enterIdentity(const char* aForm, int len) {
   	uint loop;
   	char new_id[len + 1];
	string id;
	printf("\tPlease enter a %s identifier (%d chars maximum): ",
		aForm,len);
	while(1) {
		fflush(stdout);
		fflush(stdin);
		fgets(new_id , len+1, stdin);
		for (loop = 0; loop < strlen(new_id); loop++) {
			if (new_id[loop] == '\n') {
				new_id[loop] = 0;
				break;
			}
		}
		if (loop)
			break;
	}
	id = new_id;
	return id;
}

time_t enterTime() {
	time_t val;	
	char buff[15];	
	int day, month, year, hour, min;
	time_t rawtime;
	struct tm* timeinfo;

	while(1) {
		uint loop;
		fflush(stdout);
		fflush(stdin);
		fgets(buff, 15, stdin);
		for (loop = 0; loop < strlen(buff); loop++) {
			if (buff[loop] == '\n') {
				buff[loop] = 0;
				break;
			}
		}
		if (loop) {
			int num_read = sscanf(buff, "%d/%d/%d %d:%d",
				&day, &month, &year, &hour, &min);
			if (5 == num_read) {
				break;
			} else {
				printf("\tPlease enter in dd/mm/yy hh:mm format:\n");
			}
		}
	}

	// assume for now all times will be in the future
	year+= 100;
	
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	timeinfo->tm_year = year;
	timeinfo->tm_mon = month - 1;
	timeinfo->tm_mday = day;
	timeinfo->tm_hour = hour;
	timeinfo->tm_min = min;
	timeinfo->tm_sec = 0;
	
	val = mktime(timeinfo);
	return val;
}

void wait_for_key() {
	char input[4];
	printf("\n\tPress return to finish\n\n");
	fflush(stdin);
	fgets(input, 3, stdin);
}

int main(int argc, char *argv[])
{
	bool band_id_exists = false;
	bool data_to_be_downloaded = false;
	int ret = 0;
	char answer[4];
	int arg_idx = 1;
	uint32_t standby_for_collect_mins = 0;
	uint32_t collect_mins = 0;
		
	string serial_number;
	string centre_id_param;
	string download_dir_path;
	bool auto_download = false;
	bool ask_collect = true;
	bool enable_debug = false;

	// check for non band direct commands
	while (arg_idx < argc) {
		if (!strcmp("?",argv[arg_idx])) {
			displayHelp();
			return 0;
		} else if (!strcmp("dbg",argv[arg_idx])) {
			bandif.enableDebug();
			enable_debug = true;
		} else if (!strcmp("-snum",argv[arg_idx])) {
			arg_idx++;
			if (arg_idx < argc) {
				serial_number = argv[arg_idx];
			} else {
				fprintf(stderr,"\tNo USB serial number specified for -snum\n");
				break;
			}
		} else if (!strcmp("-autodown",argv[arg_idx])) {
			auto_download = true;
		} else if (!strcmp("-nocoll",argv[arg_idx])) {
			ask_collect = false;
		} else if (!strcmp("-centre",argv[arg_idx])) {
			arg_idx++;
			if (arg_idx < argc) {
				centre_id_param = argv[arg_idx];
			} else {
				fprintf(stderr,"\tNo id specified for -centre\n");
				break;
			}
		} else if (!strcmp("-downdir",argv[arg_idx])) {
			arg_idx++;
			if (arg_idx < argc) {
				download_dir_path = argv[arg_idx];
			} else {
				fprintf(stderr,"\tNo download dir specified for -downdir\n");
				break;
			}
		}
		arg_idx++;
	}		

	signal(SIGINT, quitProc);
	
	// ----

	if (serial_number.size()) {
		// specific serial number specified on the command line
		if (bandif.connectUsb(serial_number.c_str()) < 0) {
			fprintf(stderr,
				"\tFailed to connect to serial number %s (-snum param)\n",
				serial_number.c_str());
			bandif.cleanup();
			wait_for_key();
			return -E_BB_BAND_NOT_CONNECTED;
		}
	} else {
		// check for several bands connected
		list<string> serial_numbers;
		int count = bandif.getBandSerialNumbers(serial_numbers);
		if (count <= 0) {
			fprintf(stderr,"\tFailed to detect band (%d)\n",count);
			bandif.cleanup();
			wait_for_key();
			return count;
		} else if (1 == count) {
			if (bandif.connectUsb(NULL) < 0) {
				printf("\tFailed to connect\n\n");
				bandif.cleanup();
				return -E_BB_BAND_NOT_CONNECTED;
			}
		} else {
			list<string>::iterator iter;
			int index = 65;
			printf("\t%d bands connected\n", count);
			printf("\tSelect the index letter of the band required:\n");
			printf("\t\tindex\tserial number\n");
			for(iter = serial_numbers.begin(); iter != serial_numbers.end();
				iter++ ) {
				printf("\t\t%c\t%s\n",index++,iter->c_str());
			}
			printf("\tLetter:");
			fflush(stdin);
			char answer = getchar();
			index = 65;
			for(iter = serial_numbers.begin(); iter != serial_numbers.end();
				iter++ ) {
				if (answer == index) {
					if (bandif.connectUsb(iter->c_str()) < 0) {
						fprintf(stderr,"Failed to connect to %s\n",
							iter->c_str());
						bandif.cleanup();
						wait_for_key();
						return -E_BB_BAND_NOT_CONNECTED;
					}
					break;
				}
				index++;
			}
		}
	}
	
	if (!bandif.isValid()) {
		printf("\tNo valid connection\n");
		wait_for_key();
		return -E_BB_BAND_NOT_CONNECTED;
	}
	
	// ----

	printf("\n\tBioband demo\n\n\tRequesting details from the band ...\n");
	
	float fw, hw;
	string fwdate;
	queue<uint8_t> debug_buffer;
	bool incomplete = false;
	int ver_ret = bandif.getHwFwVersions(hw, fw, fwdate);
	
	if (fw > 1.25) {
		// only bands with fw1.3+ support this call
		bool cap_exists, cap_complete;
		if (!bandif.getIfCaptureExistsAndComplete(cap_exists, cap_complete)) {
			if (cap_exists) {
				if (cap_complete) {
					printf("\n\tCapture complete\n");	
				} else {
					printf("\n\t** Incomplete capture **\n");
					incomplete = true;
					
					// Preserve the info from the debug log in case anything
					// useful for later analysis - important to do this now i.e
					// as early as possible in the operations
					bandif.getDebugBuffer(&debug_buffer);	
				}
			} else {
				printf("\n\tNo data\n");	
			}
		}
	}
	
	printf("\n\tIdentities:\n");	
	if (incomplete) {
		printf("\tRetrieving details (may take a while) ...\n");
	}
	string band_id_str = bandif.getBandId();
	band_id_exists = (band_id_str != BB_UNKNOWN_STR);
	fprintf(stdout, "\tband id:\t%s\n", band_id_str.c_str());
	fprintf(stdout, "\tsubject:\t%s\n", bandif.getSubjectDetails().c_str());
	fprintf(stdout, "\ttest:\t\t%s\n", bandif.getTestDetails().c_str());
	fprintf(stdout, "\tcentre:\t\t%s\n", bandif.getCentreId().c_str());
	fflush(stdout);

	// ----

	printf("\n\tSample data:\n");
	int stored_size = bandif.getStoredSize();
	if (stored_size < 0) {
		printf("\tfailed to retrieve stored size\n");
	} else if (stored_size) {
		time_t actioned_time = 0;
		time_t sample_start_time = 0;
		time_t sample_end_time = 0;
		char time_str[20];

		if (!bandif.getSampleTimings(actioned_time, sample_start_time,
			sample_end_time)) {
			string actioned_time_str = "Unknown";
			string start_time_str = "Error";
			string end_time_str = "Error";
			if (actioned_time && !getLocalTime(time_str, 20, actioned_time))
				actioned_time_str = time_str;
			if (sample_start_time &&
				!getLocalTime(time_str, 20, sample_start_time))
				start_time_str = time_str;
			if (sample_end_time &&
				!getLocalTime(time_str, 20, sample_end_time))
				end_time_str = time_str;
			fprintf(stdout, "\tactioned:\t%s\n", actioned_time_str.c_str());
			fprintf(stdout, "\tcollect started:%s\n",start_time_str.c_str());
			fprintf(stdout, "\tcollect ended:\t%s\n", end_time_str.c_str());
		}
		printf("\tnum of samples:\t%d\n", stored_size / BYTES_PER_SAMPLE);
		
		time_t dlt;
		if (!bandif.getFirstDownloadTime(dlt)) {
			printf("\t1st downloaded:\t");
			if (dlt) {
				char local_time_str[20];
				if (!getLocalTime(local_time_str, 20, dlt)) {
					printf("%s\n",local_time_str);
				}
			} else {
				printf("not set\n");
				data_to_be_downloaded = true;
			}
		}
	} else {
		printf("\tno sample data on device\n");
	}
	fflush(stdout);
	
	// ----

	printf("\n\tSettings:\n");		
	unsigned int battery_level = bandif.getBatteryVoltage();
	if (battery_level) {
		printf("\tbattery level:\t%.02fV",
			convADCToVoltage(battery_level));
		if (fw > 1.25) {
			// more accurate battery level detection with fw1.3+
			if (battery_level > 1920) // approx 4.2V
				printf(" (Good)");
			else if (battery_level > 1828) // approx 4V
				printf(" (Moderate)");
			else
				printf(" (Charging required)");
		}
		printf("\n");
	}

	fprintf(stdout, "\tversion:\t");
	if (ver_ret) {
		printf(BB_UNKNOWN_STR);
	} else {
		fprintf(stdout,"hw:%.01f fw:%.01f(%s)\n", hw,fw,fwdate.c_str());
	}

	time_t dt;
	if (!bandif.getGmtDeviceTime(dt)) {
		char local_time_str[20];
		if (!getLocalTime(local_time_str, 20, dt)) {
			printf("\tband time:\t%s\n",local_time_str);
			
			// TODO if epoc time - then probable drained battery during collect
		}
	}
	fflush(stdout);
	
	accel_data_rate rate;
	accel_g_scale scale;
	bandif.readAccelConfig(&rate, &scale);
	printf("\taccelerometer:\trate ");
	switch(rate) {
		case CWA_50HZ: printf("50Hz"); break;
		case CWA_100HZ: printf("100Hz"); break;
		case CWA_400HZ: printf("400Hz"); break;
		case CWA_1000HZ: printf("1000Hz"); break;
		default:
			// This has been a bad sign (incomplete initialisation) in the past,
			// hopefully shouldn't get this now but put a warning just in case
			fprintf(stdout, "** WARNING - accelerometer unconfigured **");
			break;
	}
	printf(", scale ");
	switch(scale) {
		case CWA_2G: printf("2g"); break;
		case CWA_4G: printf("4g"); break;
		case CWA_8G: printf("8g"); break;
		default:
			printf(BB_UNKNOWN_STR);
			break;
	}
	printf("\n");
	
	uint16_t cal_gain_offset[MAX_CALIBRATION_AXIS_PARAMS];
	if (!bandif.getCalibrationData(cal_gain_offset)) {
		uint16_t* val = cal_gain_offset;
		printf("\tcalibration:\t(gain,offset) ");
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
	fflush(stdout);
	
	// ----
	
	if (data_to_be_downloaded && debug_buffer.size()) {
		
		// incomplete capture & debug buffer not already stored (since the
		// data hasn't been downloaded) - so store the debug buffer in a
		// binary file.
		string filename;
		if (!download_dir_path.empty()) {
			filename = download_dir_path;
			filename += '/';
		}
		filename += bandif.getBandId();
		filename += "_";
		filename += bandif.getSubjectDetails();
		filename += "_";
		filename += bandif.getTestDetails();
		filename += "_";
		filename += bandif.getCentreId();
		filename += DBG_BUF_EXT;
		FILE* fd_ptr = openUniqueFile(filename);
		if (fd_ptr) {
			while (debug_buffer.size()) {
				uint8_t val = debug_buffer.front();
				fwrite(&val,1,1,fd_ptr);
				debug_buffer.pop();
			}
			printf("\n\tStored debug buffer: %s\n",filename.c_str());
			fclose(fd_ptr);
		}
	}
	
	// ----
	
	// questions
	
	printf("\n");
	
	if (!band_id_exists) {
		printf("\tNo band id exists\n");
   		string b_id = enterIdentity("unique band",MAX_ID_LEN);
		ret = bandif.setBandId(b_id);
		if (ret) {
			printf("\tFailed to set band id (ret %d)\n", ret);
			textError(ret);
			wait_for_key();
			return ret;
		} else {
     		printf("\tnew band id:\t%s\n\n",b_id.c_str());
		}
 	}
 	
 	if (data_to_be_downloaded) {
		if (!auto_download) {
			printf("\tThe data on the band has not been downloaded\n");
			printf("\tDo you want to download it now? [Y/n]\n\t");
			fflush(stdout);
			fflush(stdin);
			fgets(answer, 3, stdin);
		}
		if (auto_download ||
			*answer == 'Y' || *answer == 'y' || *answer == '\n') {
			string filename;
			if (!download_dir_path.empty()) {
				filename = download_dir_path;
				filename += '/';
			}
			filename += bandif.getBandId();
			filename += "_";
			filename += bandif.getSubjectDetails();
			filename += "_";
			filename += bandif.getTestDetails();
			filename += "_";
			filename += bandif.getCentreId();
			filename += RAW_EXT;
			FILE* fd_ptr = openUniqueFile(filename);
			if (fd_ptr) {
				rawData raw_samples;
				ret = bandif.setRawDataCallbackPtr(&raw_samples);
				if (enable_debug) {
					raw_samples.summary_output = true;
				}
				if (ret < 0) {
					printf("Failed to set data callback (%d)\n", ret);
				} else {
					printf("\tDownloading:\n\t");
					int total_rec = bandif.readRawFromBand(fd_ptr);
					if (total_rec < 0) {
						printf("\tRead failure (%d)\n",total_rec);
						textError(total_rec);
						wait_for_key();
						return total_rec;
					} else {
						printf("\n\tSuccessfully downloaded raw file: %s\n",
							filename.c_str());
						time_t current_time = time(NULL);
						bandif.setFirstDownloadTime(current_time);
						data_to_be_downloaded = false;
					}
				}
				fclose(fd_ptr);
			}
		}
		printf("\n");
	}
 	
 	if (ask_collect) {
		printf("\tDo you want to start a new collect? [Y/n]\n\t");
		fflush(stdout);
		fflush(stdin);
		fgets(answer, 3, stdin);
	} else {
		*answer = 'n';
	}
	if (*answer == 'Y' || *answer == 'y' || *answer == '\n') {
		
		if (data_to_be_downloaded) {
			printf("\tNote: This will wipe the current band data which has not"
				" yet been downloaded\n");
			printf("\tPress Ctrl C if wish to cancel\n\n");
		}
		
		printf("\tImmediate start? [Y/n]\n\t");
		fflush(stdout);
		fflush(stdin);
		fgets(answer, 3, stdin);
		if (*answer == 'Y' || *answer == 'y' || *answer == '\n') {
			
			// 29000 mins is about the maximum for 50Hz sampling with amount of
			// flash on the band, a bit of tolerance on the sampling & no bad
			// blocks. A well charged battery should last 11+ days, so use 16000
			// mins as upper limit for now.
			
			char collect_period[6];
			uint loop;
			while(1) {
				printf("\tPlease enter the number of minutes"
					" for the collect to run (from 1 to 16000): ");
				fgets(collect_period, 6, stdin);
				for (loop = 0; loop < strlen(collect_period); loop++) {
					if (collect_period[loop] == '\n') {
						collect_period[loop] = 0;
						break;
					}
				}
				if (loop) {
					collect_mins = atoi(collect_period);
					if (collect_mins && collect_mins < 16000)
						break;
				}
			}
				
		} else {
			
			time_t current_time = time(NULL);
			time_t start_time;
			time_t end_time;
			while(1) {
				
				printf("\tPlease enter a start date & time (dd/mm/yy hh:mm):"
					"\n\t");
				start_time = enterTime();
				if (start_time) {
					if (start_time < current_time) {
						printf("\tStart date must be in the future\n");
						continue;
					}
					printf("\tPlease enter an end date & time:\n\t");
					end_time = enterTime();
					if (end_time > start_time) {
						if ((end_time - start_time) < 60) {
							printf("\tLess than a minute difference between "
								"start and end\n");
						} else {
							if ((end_time - start_time) > 29000) {
								printf(
									"\tNote: likely to hit maximum of band\n");
							}
							break;
						}
						
					}
				}
			}
			
			current_time = time(NULL);
			standby_for_collect_mins = (start_time - current_time)/60;
			collect_mins = (end_time - start_time)/60;
			
			printf("\tStandby for %u mins\n",standby_for_collect_mins);	
		}
		
		printf("\tCollect for %u mins\n\n",collect_mins);
		
   		string sj_id = enterIdentity("subject",MAX_ID_LEN);
		ret = bandif.setSubjectDetails(sj_id);
		if (ret) {
			printf("\tFailed to set subject id (ret %d)\n", ret);
			textError(ret);
			wait_for_key();
			return ret;
		} else {
     		printf("\tsubject id:\t%s\n\n",sj_id.c_str());
		}
		
   		string tst_id = enterIdentity("test",MAX_ID_LEN);
		ret = bandif.setTestDetails(tst_id);
		if (ret) {
			printf("\tFailed to set test id (ret %d)\n", ret);
			textError(ret);
			wait_for_key();
			return ret;
		} else {
     		printf("\ttest id:\t%s\n\n",tst_id.c_str());
		}
		
		string centre_id = centre_id_param;
		if (centre_id.empty()) {
			centre_id = enterIdentity("centre",MAX_CENTRE_ID_LEN);
		}
		ret = bandif.setCentreId(centre_id);
		if (ret) {
			printf("\tFailed to set centre id (ret %d)\n", ret);
			textError(ret);
			wait_for_key();
			return ret;
		} else {
     		printf("\tcentre id:\t%s\n\n",centre_id.c_str());
		}
		
	} else if (!band_id_exists && !data_to_be_downloaded) {
		
		printf("\tA short collect is required to store the band id"
			" in the band\n");
		printf("\tIs it okay to start that now? [Y/n]\n");
		fflush(stdout);
		fflush(stdin);
		fgets(answer, 3, stdin);
		if (*answer == 'Y' || *answer == 'y' || *answer == '\n') {
			collect_mins = 1;
			
			// dummy ids for this quick collect
			bandif.setSubjectDetails("demo");
			bandif.setTestDetails("t1min");
			bandif.setCentreId("uk");

		} else {
			printf("\tPlease note, if the band is reset or loses power the"
				"new band id will may be lost unless a collect is run\n");
		}
	}
	
	if (collect_mins) {
		bool allow_collect = true;
		
		if (fw > 1.25 && (collect_mins > 60)) {
			bool show_warning = true;
			// more accurate battery level detection with fw1.3+
			
			// rough maths to see if battery level ok for collection run
			if (battery_level > 1691) { // 1691 = approx 3.7V
				int check_level = battery_level;
				// TODO currently guesttimate, improve this with actual calc
				check_level -= (collect_mins / 64);
				show_warning = (check_level < 1650); // 1650 = approx 3.6V
			}
			if (show_warning) {
				printf(
					"\t*** Battery potentially too low for full %d mins ***\n",
					collect_mins);
				printf("\tProceed? [N/y]\n");
				fflush(stdout);
				fflush(stdin);
				fgets(answer, 3, stdin);
				if (*answer == 'N' || *answer == 'n' || *answer == '\n') {
					allow_collect = false;
					printf("\n\tCancelled\n");
				}
			}
		}

		if (allow_collect) {
			ret = bandif.start(standby_for_collect_mins, collect_mins);
			if (ret) {
				printf("\n\tAn error has occurred\n");
				textError(ret);
			} else if (standby_for_collect_mins) {
				printf("\t* Entered standby for collect *\n");
			} else {
				printf("\t* Immediate collection for %d mins has started *\n",
					collect_mins);
			}
		}
	}
	
	if (!ret) {
		printf("\n\tAll done\n\n\tThe Bioband (%s) may now be unplugged\n",
			band_id_str.c_str());
	}
	
	// ----
	
	wait_for_key();

	return ret;
}

// EOF
