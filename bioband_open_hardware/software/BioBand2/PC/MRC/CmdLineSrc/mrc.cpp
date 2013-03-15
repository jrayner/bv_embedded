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

#define SAMPLE_KEY 'S'
#define SAMPLE_AND_TEMP_KEY 'T'
#define TEXT_KEY 'H'

#define BL_EXT ".bl"
#define TL_EXT ".tl"
#define DBG_EXT ".dbg"
#define CSV_EXT ".csv"
#define RAW_EXT ".raw"

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
	printf("Cmd line params & default values:\n");
	printf("Raw file command options\n");
	printf("\t-raw [filename] read raw image of data from band to file or screen\n");
	printf("\t-uraw read raw image from band and produce a uniquely named raw file\n");
	printf("\t-fraw <filename> read raw image of band data from file\n");
	printf("\t-rbl <filename> store raw battery levels to file\n");
	printf("\t-rtl <filename> store raw temperature levels to file\n");
	printf("\t-rdbg <filename> store raw debug to file\n");
	printf("\t-rcsv <filename> store csv output to file\n");
	printf("\t-rsum produce summary of raw data\n");
	printf("\t-uall create uniquely named files for bl,tl,dbg & csv from band\n");
	printf("Band sampling command options\n");
	printf("\t-l collection time (in mins)\n");
	printf("\t-p standby time (in mins)\n");
	printf("\t-z set mode (0 = real, 1 = debug)\n");
	printf("\t-sj set subject id (max %d chars)\n", MAX_ID_LEN);
	printf("\t-tst set test id (max %d chars)\n", MAX_ID_LEN);
	printf("\t-cen set centre id (max %d chars)\n", MAX_CENTRE_ID_LEN);
	printf("Other band commands\n");
	//printf("\t-csv [filename] stream stored samples to csv file or screen\n");
	printf("\t-id set band id (max %d chars)\n", MAX_ID_LEN);
	printf("\t-bl read all the stored battery level measurements\n");
	printf("\t-tl read all the stored temperature level measurements\n");
	//printf("\t-sm accelerometer xyz value streaming mode\n");
	printf("\t-gdt current device time\n");
	printf("\t-gfdt get first download time\n");
	printf("\t-ef erase flash (remember this wipes id and subject as well)\n");
	printf("\t-gi get band id\n");
	printf("\t-gsj get subject id\n");
	printf("\t-gt get test id\n");
	printf("\t-gcen get centre id\n");
	printf("\t-gsb get stored samples size in bytes\n");
	printf("\t-gsp get stored samples size in pages\n");
	printf("\t-rp <page number (between 1 and value given by -gsp)> read page "
		"number\n");
	printf("\t-gbl get current battery level\n");
	printf("\t-gv get the versions\n");
	//printf("\t-gm get accelerometer measurement\n");
	printf("\t-sac <rate 50,100,400,1000> <scale 2,4,8> set accelerometer config\n");
	printf("\t-rac read current accelerometer data rate and g scale\n");
	printf("\t-raf read accelerometer data rate and g scale for previous run\n");
	printf("\t-scal set calibration data, 2 * unsigned values (gain & offset)"
		" for each axis\n");
	printf("\t-gts go to sleep\n");
	printf("Diag commands\n");
	printf("\t-bb read bad block info\n");
	//printf("\t-dd set up 5 days worth of dummy data\n");
	//printf("\t-rd quick read of all stored samples for performance stats\n");
	//printf("\t-drd debug read of all stored accelerometer values to screen\n");
	printf("\t-nobkp reset bkp domain on band\n");
	printf("\t-bug read dbg info from band\n");
	//printf("\t-rdbg read debug from band\n");
	printf("\n");
}

static int singleParam(char* argv[], int arg_idx, int argc, const char* check) {
	
	int retval = 0;
	if (!strcmp(check,argv[arg_idx])) {
		retval = 1;
		if (arg_idx+1 < argc) {
			// single param - no other args expected
			if ('-' == argv[arg_idx+1][0]) {
				printf("Note %s overrides all config params <e.g %s>\n",
					check,argv[arg_idx+1]);
			} else {
				arg_idx++;
				printf("no param required for %s\n",check);
			}
		}
	}
	return retval;
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

// ----

FILE* openUniqueFile(const char* aFilename) {
	FILE* fd_ptr = NULL;
	if (aFilename) {
		string filename = aFilename;
		int val = 1;
		bool show_name = false;
		fd_ptr = fopen(filename.c_str(),"r");
		while (fd_ptr) {
			char tmp[10];
			filename = aFilename;
			show_name = true;
			fclose(fd_ptr);
			sprintf(tmp,"_%d",val);
			filename += tmp;
			fd_ptr = fopen(filename.c_str(),"r");
			val++;
		}
		if (show_name) {
			fprintf(stderr,"* Note: Using <%s> as file <%s> already exists *\n",
				filename.c_str(),aFilename);
		}
		fd_ptr = fopen(filename.c_str(),"w");
	}
	return fd_ptr;
}

// ----

struct rawData : public MDataObserver {
	
	rawData();
	virtual ~rawData();
	
	void outputHeader();
	void outputSummary();
	
	virtual bool evSamplesCallback();
	virtual void evDoneCallback();
	
	int setBatteryLevelFilename(const char* aFilename);
	int setTemperatureLevelFilename(const char* aFilename);
	int setDebugFilename(const char* aFilename);
	int setCsvFilename(const char* aFilename);

	FILE* raw_out_bl;
	FILE* raw_out_tl;
	FILE* raw_out_dbg;
	FILE* raw_out_csv;
	bool header_output;
	bool summary_output;
	time_t start_time;
	uint32_t num_samples_received;
	uint32_t expected_total;
	uint32_t last_percentage;
	uint32_t percentage;
};

rawData::rawData() :
	raw_out_bl(NULL),
	raw_out_tl(NULL),
	raw_out_dbg(NULL),
	raw_out_csv(NULL),
	header_output(false),
	summary_output(false),
	start_time(0),
	num_samples_received(0),
	expected_total(0),
	last_percentage(0),
	percentage(0) {
}

rawData::~rawData() {
	if (raw_out_bl) {
		fclose(raw_out_bl);
	}
	if (raw_out_tl) {
		fclose(raw_out_tl);
	}
	if (raw_out_dbg) {
		fclose(raw_out_dbg);
	}
	if (raw_out_csv) {
		fclose(raw_out_csv);
	}
}

void rawData::outputHeader() {
	
	if (raw_out_csv) {
		FILE* template_file = fopen("template.csv","r");
		if (template_file) {
			// output a load of non band related csv header info
			char in_str[100];
			while (!feof(template_file)) {
				if (fgets(in_str, 100, template_file))
					fprintf(raw_out_csv,"%s",in_str);
			}
			fclose(template_file);
		}

		char upload_time_str[20];
		time_t actioned_time = 0;
		time_t sample_start_time = 0;
		time_t sample_end_time = 0;
		string actioned_time_str = "Unknown";
		string start_time_str = "Unknown";
		string end_time_str = "Unknown";
		char time_str[20];
		
		getLocalTime(upload_time_str, 20, time(NULL));
		
		if (bandif.isValid()) {
			
			// Note: these calls should occur before direct band raw file
			// creation starts otherwise will confuse the band interface.
			
			if (!bandif.getSampleTimings(actioned_time, sample_start_time,
				sample_end_time)) {
				if (actioned_time && !getLocalTime(time_str, 20, actioned_time))
					actioned_time_str = time_str;
				if (sample_start_time &&
					!getLocalTime(time_str, 20, sample_start_time))
					start_time_str = time_str;
				if (sample_end_time &&
					!getLocalTime(time_str, 20, sample_end_time))
					end_time_str = time_str;
			}
			
			fprintf(raw_out_csv, ",,,,,,Volunteer Number,%s\n",
				bandif.getSubjectDetails().c_str());
			fprintf(raw_out_csv, ",,,,,,Initialisation Time,%s\n",
				actioned_time_str.c_str());
			fprintf(raw_out_csv, ",,,,,,Start Time,%s\n",start_time_str.c_str());
			fprintf(raw_out_csv, ",,,,,,End Time (start of last page),%s\n",
				end_time_str.c_str());
			fprintf(raw_out_csv, ",,,,,,Serial Number,%s\n",
				bandif.getBandId().c_str());
			fprintf(raw_out_csv, ",,,,,,Battery Voltage,%.02fV\n",
				convADCToVoltage(bandif.getBatteryVoltage()));
			fprintf(raw_out_csv, ",,,,,,Upload Time,%s\n",upload_time_str);
			fprintf(raw_out_csv, ",,,,,,Firmware version,");
			float fw, hw;
			string fwdate;
			int ret = bandif.getHwFwVersions(hw, fw, fwdate);
			if (ret) {
				fprintf(raw_out_csv, "UNKNOWN");
			} else {
				fprintf(raw_out_csv,"hw:%.01f fw:%.01f(%s)\n",
					hw,fw,fwdate.c_str());
			}
			fprintf(raw_out_csv, "\n\n\n\n");
				
		} else if (additional_present) {
			
			// if getting the details directly from the band is not possible
			start_time = collect_start_time;
			if (!getLocalTime(time_str, 20, collect_start_time))
				start_time_str = time_str;
				
			fprintf(raw_out_csv, ",,,,,,Volunteer Number,%s\n", subject_id.c_str());
			fprintf(raw_out_csv, ",,,,,,Initialisation Time,%s\n",
				start_time_str.c_str());
			fprintf(raw_out_csv, ",,,,,,Start Time,%s\n",start_time_str.c_str());
			fprintf(raw_out_csv, ",,,,,,End Time (start of last page),UNKNOWN\n");
			fprintf(raw_out_csv, ",,,,,,Serial Number,%s\n", band_id.c_str());
			fprintf(raw_out_csv, ",,,,,,Battery Voltage,%.02fV\n",
				convADCToVoltage(battery_raw));
			// TODO upload time can be taken from the file creation time
			fprintf(raw_out_csv, ",,,,,,Upload Time,%s\n",upload_time_str);
			fprintf(raw_out_csv, ",,,,,,Firmware version,UNKNOWN\n\n\n\n");
		} else {
			fprintf(raw_out_csv, ",,,,,,Volunteer Number,UNKNOWN\n");
			fprintf(raw_out_csv, ",,,,,,Initialisation Time,UNKNOWN\n");
			fprintf(raw_out_csv, ",,,,,,Start Time,UNKNOWN\n");
			fprintf(raw_out_csv,
				",,,,,,End Time (start of last page),UNKNOWN\n");
			fprintf(raw_out_csv, ",,,,,,Serial Number,UNKNOWN\n");
			fprintf(raw_out_csv, ",,,,,,Battery Voltage,UNKNOWN\n");
			// TODO upload time can be taken from the file creation time
			fprintf(raw_out_csv, ",,,,,,Upload Time,%s\n",upload_time_str);
			fprintf(raw_out_csv, ",,,,,,Firmware version,UNKNOWN\n\n\n\n");
		}
			
		fflush(raw_out_csv);
	}
	header_output = true;
}

void rawData::outputSummary() {

	string start_time_str = "Unknown";
	char time_str[20];
	
	printf("\npage crc:\t0x%02x (", (int) crc_ok);
	if (crc_ok)
		printf("OK");
	else
		printf("FAILED");
	printf(")\n");
	printf("page status:\t0x%02x (", status_raw);
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
	printf("current_tick:\t%d\n",current_tick);
	
	time_t curr_time = convTicksToTime(start_time, current_tick);
	if (!getLocalTime(time_str, 20, curr_time))
		printf("page time:\t%ld (local:%s)\n",curr_time, time_str);
			
	double tl_fl = convTempBinToCelsius(temperature_raw);
	printf("temp level:\t0x%04x (%.02fC)\n",temperature_raw,tl_fl);
			
	if (additional_present) {
		
		double bl_fl = convADCToVoltage(battery_raw);
		printf("battery:\t0x%04x (%.02fV)\n",battery_raw,bl_fl);
	
		if (band_id.empty()) {
			printf("no band id!\n");
		} else {
			printf("band id:\t%s\n",band_id.c_str());
		}
		
		if (subject_id.empty()) {
			printf("no subject id!\n");
		} else {
			printf("subject id:\t%s\n",subject_id.c_str());
		}
		
		if (test_id.empty()) {
			printf("no test id!\n");
		} else {
			printf("test id:\t%s\n",test_id.c_str());
		}
		
		if (centre_id.empty()) {
			printf("no centre id!\n");
		} else {
			printf("centre id:\t%s\n",centre_id.c_str());
		}
		
		// calibration defined as 6 unsigned 16bit values
		printf("calibration (gain,offset): ");
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
		
		printf("mx samples:\t%d\n",req_total_num_samples);
		
		if (!getLocalTime(time_str, 20, collect_start_time))
			printf("collect start:\t%ld (local:%s)\n",
				(time_t) collect_start_time, time_str);
			
		accel_data_rate rate;
		accel_g_scale scale;
		int ret = decodeRateAndGscale(accel_conf_raw, &rate, &scale);
		printf("accel conf:\t0x%02x (",accel_conf_raw);
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
					printf("Unknown");
					break;
			}
			printf(" scale:");
			switch(scale) {
				case CWA_2G: printf("2g"); break;
				case CWA_4G: printf("4g"); break;
				case CWA_8G: printf("8g"); break;
				default:
					printf("Unknown");
					break;
			}
		}
		printf(")\n");

		uint8_t fw_ver, hw_ver;
		convHwFwVerToBytes(fw_hw_version_raw, hw_ver, fw_ver);
		printf("fwhw ver:\t0x%02x (",fw_hw_version_raw);
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
	printf("\n\nnum_samples_received %d\n",num_samples_received);
	printf("expected_total %d\n",expected_total);
}

bool rawData::evSamplesCallback() {
	
	if (additional_present) {	
		if (!expected_total) {
			printf("\n");
			start_time = collect_start_time;
			expected_total = req_total_num_samples;
		}
	}
	
	if (status_raw != OK_USED_STATUS) {
		char time_str[20];
		fprintf(stderr,"\n");
		time_t curr_time = convTicksToTime(start_time, current_tick);
		if (!getLocalTime(time_str, 20, curr_time))
			fprintf(stderr,"%s (tick %u)",time_str,current_tick);

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
		fflush(stderr);
	}
	
	if (summary_output) {
		outputSummary();
		summary_output = false;
		if (!raw_out_csv && !raw_out_bl && !raw_out_tl && !raw_out_dbg) {
			// only a summary requested
			return true;
		}
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
	
	if (raw_out_csv) {
		char time_str[20];
		double x,y,z;
		double base = RTC_CLOCK_BASE;
		double top = RTC_SCALAR * 1000;
		double factor = top / base;
		uint64_t start_millisecs = start_time * 1000;
		uint64_t millisecs_since_epoc = start_millisecs;
		double tick_millisecs = current_tick;
		tick_millisecs *= factor;
		millisecs_since_epoc += tick_millisecs;
		if (!header_output) {
			outputHeader();
		}
		// needs to be %ld
		fprintf(raw_out_csv,"%ld",millisecs_since_epoc); 
		if (!raw_sample_list.empty()) {
			sample s = raw_sample_list.front();
			s.giveGValues(x, y, z);
			raw_sample_list.pop_front();
			fprintf(raw_out_csv,",%.3f,%.3f,%.3f",x,y,z);
			fprintf(raw_out_csv,",%.02f",
				convTempBinToCelsius(temperature_raw));
			if (status_raw != OK_USED_STATUS) {
				if (!(status_raw & COLLECT_OK_MASK)) {
					if (status_raw & PAGE_OK_MASK) {
						fprintf(raw_out_csv,
							",Potential data loss just before this time\n");
					} else {
						fprintf(raw_out_csv,
							",Definate data loss occurred in this section\n");
					}
				} else {
					fprintf(raw_out_csv,",");
					if (!crc_ok)
						fprintf(raw_out_csv,"CRC Error & ");
					fprintf(raw_out_csv,"Unknown status 0x%02x", status_raw);
				}
			} else if (!crc_ok) {
				time_t err_time =
					convTicksToTime(start_time, current_tick);
				fprintf(raw_out_csv, ",CRC Error: at this time (%ld)",
					err_time);
				if (!getLocalTime(time_str, 20, err_time)) {
					fprintf(raw_out_csv,"(local: %s)",time_str);
				}
				fprintf(raw_out_csv,"\n");
			}
			fprintf(raw_out_csv,"\n");
			while (!raw_sample_list.empty()) {
				s = raw_sample_list.front();
				s.giveGValues(x, y, z);
				raw_sample_list.pop_front();
				fprintf(raw_out_csv,",%.3f,%.3f,%.3f\n",x,y,z);
			}
		} else {
			if (crc_ok) {
				fprintf(raw_out_csv,",,,,,Error: no samples?\n");
			} else {
				time_t err_time =
					convTicksToTime(start_time, current_tick);
				fprintf(raw_out_csv, ",,,,,CRC Error: at this time (%ld)",
					err_time);
				if (!getLocalTime(time_str, 20, err_time)) {
					fprintf(raw_out_csv,"(local: %s)",time_str);
				}
				fprintf(raw_out_csv,"\n");
			}
		}
		fflush(raw_out_csv);
	}
	if (raw_out_bl && additional_present) {
		double bl_fl = convADCToVoltage(battery_raw);
		fprintf(raw_out_bl,"%.02fV\n",bl_fl);
	}
	if (raw_out_tl) {
		double tl_fl = convTempBinToCelsius(temperature_raw);
		fprintf(raw_out_tl,"%.02fC\n",tl_fl);
	}
	if (raw_out_dbg) {
		bool block_page = false;
		fprintf(raw_out_dbg,"dbg %d: <",(int) dbg_raw.size());
		list<uint8_t>::iterator iter;
		iter = dbg_raw.begin();
		uint8_t val = *iter;
		if (6 == dbg_raw.size() && 'B' == val) {
			for(int iter_lp = 0; iter_lp < 3; iter_lp++)
				iter++;
			val = *iter;
			if ('P' == val) {
				// for now, assume that block & page info
				block_page = true;
				iter = dbg_raw.begin();
				for (int lp = 0; lp < 2; lp++) {
					uint16_t info;
					uint8_t* b_ptr = (uint8_t*) &info;
					val = *iter++;
					*b_ptr++ = *iter++;
					*b_ptr = *iter++;
					fprintf(raw_out_dbg,"%c%d",val,info);
				}
			}
		}
		if (!block_page) {
			for(iter = dbg_raw.begin(); iter != dbg_raw.end(); iter++) {
				uint8_t val = *iter;
				if (val > 0x1F && val < 0x7F) {
					fprintf(raw_out_dbg,"%c",val);
				} else {
					if (val)
						fprintf(raw_out_dbg,"0x%02x ",val);
					else
						fprintf(raw_out_dbg," ");
				}
			}
		}
		fprintf(raw_out_dbg,">\n");
	}
	
	reset();
	
	return false;
}

int rawData::setBatteryLevelFilename(const char* aFilename) {
	int retval = -E_BB_BAD_PARAM;
	if (aFilename) {
		string filename = aFilename;
		if (filename.rfind(BL_EXT) == string::npos)
			filename += BL_EXT;
    	printf("batt filename:\t%s\n",filename.c_str());
		retval = BB_SUCCESS;
		raw_out_bl = openUniqueFile(filename.c_str());
		if (!raw_out_bl) {
			printf("Failed to open file for write\n");
			retval = -E_BB_FAILED_TO_OPEN_FILE_FOR_WRITE;
		}
	}
	return retval;
}

int rawData::setTemperatureLevelFilename(const char* aFilename) {
	int retval = -E_BB_BAD_PARAM;
	if (aFilename) {
		string filename = aFilename;
		if (filename.rfind(TL_EXT) == string::npos)
			filename += TL_EXT;
    	printf("temp filename:\t%s\n",filename.c_str());
		retval = BB_SUCCESS;
		raw_out_tl = openUniqueFile(filename.c_str());
		if (!raw_out_tl) {
			printf("Failed to open file for write\n");
			retval = -E_BB_FAILED_TO_OPEN_FILE_FOR_WRITE;
		}
	}
	return retval;
}

int rawData::setDebugFilename(const char* aFilename) {
	int retval = -E_BB_BAD_PARAM;
	if (aFilename) {
		string filename = aFilename;
		if (filename.rfind(DBG_EXT) == string::npos)
			filename += DBG_EXT;
    	printf("debug filename:\t%s\n",filename.c_str());
		retval = BB_SUCCESS;
		raw_out_dbg = openUniqueFile(filename.c_str());
		if (!raw_out_dbg) {
			printf("Failed to open file for write\n");
			retval = -E_BB_FAILED_TO_OPEN_FILE_FOR_WRITE;
		}
	}
	return retval;
}

int rawData::setCsvFilename(const char* aFilename) {
	int retval = -E_BB_BAD_PARAM;
	if (aFilename) {
		string filename = aFilename;
		if (filename.rfind(CSV_EXT) == string::npos)
			filename += CSV_EXT;
    	printf("csv filename:\t%s\n",filename.c_str());
		retval = BB_SUCCESS;
		raw_out_csv = openUniqueFile(filename.c_str());
		if (!raw_out_csv) {
			printf("Failed to open file for write\n");
			retval = -E_BB_FAILED_TO_OPEN_FILE_FOR_WRITE;
		}
	}
	return retval;
}

// ----

void textError(int errorCode) {
	string errStr = bandif.errorToString(errorCode);
	printf("%s\n",errStr.c_str());
}

int main(int argc, char *argv[])
{
	signal(SIGINT, quitProc);
	
    int collection_time_in_mins = 0;
    int standby_before_collection_time_mins = 0;
	rawData raw_samples;

    printf("MRC\n");
    
    if (argc == 1) {
    	printf("\n** Please specify the params **\n\n"
    	"For example the command \"./mrc -l 10 -sj tst10min -z 1\"\n"
    	"would collect samples for 10 minutes, identified as tst10min with"
    	" the progress\nshown by the debug leds\n\n");
		displayHelp();
		return 0;
    } else {
		int arg_idx = 1;

		// check for non band direct commands
		while (arg_idx < argc) {
			if (!strcmp("?",argv[arg_idx])) {
				displayHelp();
				return 0;
			} else if (!strcmp("dbg",argv[arg_idx])) {
				bandif.enableDebug();
			} else if (!strcmp("-rbl",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int ret =
						raw_samples.setBatteryLevelFilename(argv[arg_idx]);
					if (ret < 0) {
						printf("Failed to set rbl %s filename (%d)\n",
							argv[arg_idx],ret);
					}
				} else {
					printf("Error -rbl missing filename\n");
					return -E_BB_BAD_PARAM;
				} 
			} else if (!strcmp("-rtl",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int ret =
						raw_samples.setTemperatureLevelFilename(argv[arg_idx]);
					if (ret < 0) {
						printf("Failed to set rtl %s filename (%d)\n",
							argv[arg_idx],ret);
					}
				} else {
					printf("Error -rtl missing filename\n");
					return -E_BB_BAD_PARAM;
				} 
			} else if (!strcmp("-rdbg",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int ret = raw_samples.setDebugFilename(argv[arg_idx]);
					if (ret < 0) {
						printf("Failed to set rdbg %s filename (%d)\n",
							argv[arg_idx],ret);
					}
				} else {
					printf("Error -rdbg missing filename\n");
					return -E_BB_BAD_PARAM;
				} 
			} else if (!strcmp("-rcsv",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int ret = raw_samples.setCsvFilename(argv[arg_idx]);
					if (ret < 0) {
						printf("Failed to set rcsv %s filename (%d)\n",
							argv[arg_idx],ret);
					}
				} else {
					printf("Error -rcsv missing filename\n");
					return -E_BB_BAD_PARAM;
				} 
			} else if (!strcmp("-rsum",argv[arg_idx])) {
				raw_samples.summary_output = true;
			} else if (!strcmp("-fraw",argv[arg_idx])) {
				FILE* fd_ptr = NULL;
				arg_idx++;
				if (arg_idx < argc) {
					fd_ptr = fopen(argv[arg_idx],"r");
					if (!fd_ptr) {
						printf("Failed to open %s for read\n",argv[arg_idx]);
						return -E_BB_FAILED_TO_OPEN_FILE_FOR_READ;
					}
				} else {
					printf("No filename specified for fraw\n");
					return -E_BB_BAD_PARAM;
				}
				int ret = bandif.setRawDataCallbackPtr(&raw_samples);
				if (ret < 0) {
					printf("Failed to set data callback (%d)\n", ret);
				} else {
					int total_rec = bandif.readRawFromFile(fd_ptr);
					if (total_rec < 0) {
						printf("Read failure (%d)\n",total_rec);
					}
					printf("\n");
				}
				if (fd_ptr)
					fclose(fd_ptr);
				return 0;
			}
			arg_idx++;
		}
		
		// check for several bands connected
		list<string> serial_numbers;
		int count = bandif.getBandSerialNumbers(serial_numbers);
		if (count <= 0) {
			printf("Failed to detect band (%d)\n",count);
			bandif.cleanup();
			return count;
		} else if (1 == count) {
			if (bandif.connectUsb(NULL) < 0) {
				printf("Failed to connect\n");
				bandif.cleanup();
				return -E_BB_BAND_NOT_CONNECTED;
			}
		} else {
			list<string>::iterator iter;
			int index = 65;
			printf("%d bands connected\n", count);
			printf("Select the index letter of the band required:\n");
			printf("\tindex\tserial number\n");
			for(iter = serial_numbers.begin(); iter != serial_numbers.end();
				iter++ ) {
				printf("\t%c\t%s\n",index++,iter->c_str());
			}
			printf("Letter:");
			char answer = getchar();
			index = 65;
			for(iter = serial_numbers.begin(); iter != serial_numbers.end();
				iter++ ) {
				if (answer == index) {
					if (bandif.connectUsb(iter->c_str()) < 0) {
						printf("Failed to connect to %s\n", iter->c_str());
						bandif.cleanup();
						return -E_BB_BAND_NOT_CONNECTED;
					}
					break;
				}
				index++;
			}
		}

		// check for band direct commands
		arg_idx = 1;
		while (arg_idx < argc) {
			
			//printf(" %s\n",argv[arg_idx]);

			if (!strcmp("dbg",argv[arg_idx])) {
				arg_idx++;
				continue;
			} else if (!strcmp("-rbl",argv[arg_idx])) {
				arg_idx += 2;
				continue;
			} else if (!strcmp("-rtl",argv[arg_idx])) {
				arg_idx += 2;
				continue;
			} else if (!strcmp("-rdbg",argv[arg_idx])) {
				arg_idx += 2;
				continue;
			} else if (!strcmp("-rcsv",argv[arg_idx])) {
				arg_idx += 2;
				continue;
			} else if (!strcmp("-rsum",argv[arg_idx])) {
				arg_idx++;
				continue;
			} else if (!strcmp("-p",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					char* tmp = argv[arg_idx];
					int val = atoi(tmp);
					standby_before_collection_time_mins = val;
				} else {
					printf("Missing standby mins value\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-l",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					char* tmp = argv[arg_idx];
					collection_time_in_mins = atoi(tmp);
				} else {
					printf("Missing collection mins value\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-z",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					char* tmp = argv[arg_idx];
					int val = atoi(tmp);
					if ((val == 1)) {
						bandif.setBandDebugLeds();
					}
				} else {
					printf("Missing mode value\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-id",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int sz = strlen(argv[arg_idx]);
					if (sz <= MAX_ID_LEN) {
						string tmp;
						tmp.append(argv[arg_idx]);
						int ret = bandif.setBandId(tmp);
						if (ret) {
							printf("Error: Failed to set band id"
								" (ret %d)\n", ret);
							textError(ret);
						} else {
							printf("OK\n");
						}
					} else {
						printf("Id <%s> more than max of %d chars\n",
							argv[arg_idx],MAX_ID_LEN);
						bandif.cleanup();
						return -E_BB_BAD_ID;
					}
				} else {
					printf("Missing id\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-sj",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int sz = strlen(argv[arg_idx]);
					if (sz <= MAX_ID_LEN) {
						string tmp;
						tmp.append(argv[arg_idx]);
						int ret = bandif.setSubjectDetails(tmp);
						if (ret) {
							printf("Error: Failed to set subject id"
								" (ret %d)\n", ret);
							textError(ret);
							return ret;
						}
					} else {
						printf("subject id <%s> more than max of %d chars\n",
							argv[arg_idx],MAX_ID_LEN);
						bandif.cleanup();
						return -E_BB_BAD_ID;
					}
				} else {
					printf("Missing subject id\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-tst",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int sz = strlen(argv[arg_idx]);
					if (sz <= MAX_ID_LEN) {
						string tmp;
						tmp.append(argv[arg_idx]);
						int ret = bandif.setTestDetails(tmp);
						if (ret) {
							printf("Error: Failed to set test id"
								" (ret %d)\n", ret);
							textError(ret);
							return ret;
						}
					} else {
						printf("test id <%s> more than max of %d chars\n",
							argv[arg_idx],MAX_ID_LEN);
						bandif.cleanup();
						return -E_BB_BAD_ID;
					}
				} else {
					printf("Missing test id\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-cen",argv[arg_idx])) {
				arg_idx++;
				if (arg_idx < argc) {
					int sz = strlen(argv[arg_idx]);
					if (sz <= MAX_CENTRE_ID_LEN) {
						string tmp;
						tmp.append(argv[arg_idx]);
						int ret = bandif.setCentreId(tmp);
						if (ret) {
							printf("Error: Failed to set centre id"
								" (ret %d)\n", ret);
							textError(ret);
							return ret;
						}
					} else {
						printf("centre id <%s> more than max of %d chars\n",
							argv[arg_idx],MAX_CENTRE_ID_LEN);
						bandif.cleanup();
						return -E_BB_BAD_ID;
					}
				} else {
					printf("Missing centre id\n");
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
			} else if (!strcmp("-raw",argv[arg_idx])) {
				FILE* fd_ptr = NULL;
				arg_idx++;
				if (arg_idx < argc) {
					string filename = argv[arg_idx];
					if (filename.rfind(RAW_EXT) == string::npos)
						filename += RAW_EXT;
    				printf("raw filename:\t%s\n",filename.c_str());
					fd_ptr = openUniqueFile(filename.c_str());
				}
				int ret = bandif.setRawDataCallbackPtr(&raw_samples);
				if (ret < 0) {
					printf("Failed to set data callback (%d)\n", ret);
					textError(ret);
				} else {
					raw_samples.outputHeader();
					int total_rec = bandif.readRawFromBand(fd_ptr);
					if (total_rec < 0) {
						printf("Read failure (%d)\n",total_rec);
					} else {
						time_t current_time = time(NULL);
						bandif.setFirstDownloadTime(current_time);
					}
					printf("\n");
				}
				if (fd_ptr)
					fclose(fd_ptr);
				return 0;
			} else if (!strcmp("-uall",argv[arg_idx])) {
				// Creates a common filename from band, subject & test details
				// and uses that to name the all the derived component filenames
				string common_filename = bandif.getBandId();
				common_filename += "_";
    			common_filename += bandif.getSubjectDetails();
				common_filename += "_";
    			common_filename += bandif.getTestDetails();
				common_filename += "_";
    			common_filename += bandif.getCentreId();
				int ret =
					raw_samples.setBatteryLevelFilename(common_filename.c_str());
				if (ret < 0) {
					printf("Failed to set rbl %s filename (%d)\n",
						common_filename.c_str(),ret);
				}
				ret = raw_samples.setTemperatureLevelFilename(
					common_filename.c_str());
				if (ret < 0) {
					printf("Failed to set rtl %s filename (%d)\n",
						common_filename.c_str(),ret);
				}
				ret = raw_samples.setDebugFilename(common_filename.c_str());
				if (ret < 0) {
					printf("Failed to set rdbg %s filename (%d)\n",
						common_filename.c_str(),ret);
				}
				ret = raw_samples.setCsvFilename(common_filename.c_str());
				if (ret < 0) {
					printf("Failed to set rcsv %s filename (%d)\n",
						common_filename.c_str(),ret);
				}
			} else if (!strcmp("-uraw",argv[arg_idx])) {
				// Same as -raw but creates a common filename from band, subject
				// & test details and uses that to name the raw file.
				string filename = bandif.getBandId();
				filename += "_";
    			filename += bandif.getSubjectDetails();
				filename += "_";
    			filename += bandif.getTestDetails();
 				filename += "_";
    			filename += bandif.getCentreId();
   				filename += RAW_EXT;
    			printf("raw filename:\t%s\n",filename.c_str());
				FILE* fd_ptr = openUniqueFile(filename.c_str());
				int ret = bandif.setRawDataCallbackPtr(&raw_samples);
				if (ret < 0) {
					printf("Failed to set data callback (%d)\n", ret);
				} else {
					raw_samples.outputHeader();
					int total_rec = bandif.readRawFromBand(fd_ptr);
					if (total_rec < 0) {
						printf("Read failure (%d)\n",total_rec);
					} else {
						time_t current_time = time(NULL);
						bandif.setFirstDownloadTime(current_time);
					}
					printf("\n");
				}
				if (fd_ptr)
					fclose(fd_ptr);
				return 0;
#if 0
			} else if (!strcmp("-csv",argv[arg_idx])) {
				outCsvData csv_data;
				arg_idx++;
				if (arg_idx < argc) {
					if (csv_data.openFile(argv[arg_idx]))
						printf("Missing or bad filename - using stdout\n");
				}
				csv_data.outputHeader();
				int total_rec = bandif.readAllSamples(&csv_data);
				if (total_rec < 0) {
					printf("Read failure (%d)\n",total_rec);
				}
				printf("\n");
				return 0;
#endif
			} else {
				
				// other simple ops
				while (1) {
#if 0
					if (singleParam(argv, arg_idx, argc, "-drd")) {
						debugData debug_data;
						int total_rec = bandif.readAllSamples(&debug_data);
						if (total_rec < 0) {
							printf("Read failure (%d)\n",total_rec);
						}
						printf("\n");
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-rd")) {
						time_t start_time = time(NULL);
						speedData speed_data;
						int total_rec = bandif.readAllSamples(&speed_data);
						if (total_rec < 0) {
							printf("Read failure (%d)\n",total_rec);
						} else {
							time_t end_time = time(NULL);
							int approx_elapsed = (int) end_time -
								(int) start_time;
							float seven_day_val = 7 * 24 * 3600 * 80 * 6;
							float prop = seven_day_val / total_rec;
							prop = prop * approx_elapsed;
							int total_secs = (int) prop;
							
							printf("\n%d bytes received in approx %d secs\n",
								total_rec, approx_elapsed);
							printf(
								"7 days of samples (%d bytes) would take approx"
								" %d secs (or approx %d mins)\n",
								(int) seven_day_val, total_secs,
								total_secs / 60);
						}
						printf("\n");
    					return 0;
					}
#endif
#if 0
					if (singleParam(argv, arg_idx, argc, "-gm")) {
						uint16_t x,y,z;
						int ret = bandif.getMeasurement(x,y,z);
						if (ret) {
							printf("Failed to retrieve measurement\n");
						} else {
							printf("Measurement: x 0x%04x y 0x%04x z 0x%04x\n",
								x,y,z);
						}
						return ret;
					}
#endif
					if (singleParam(argv, arg_idx, argc, "-gbl")) {
    					unsigned int val = bandif.getBatteryVoltage();
    					if (val)
							printf("current battery level: %.02fV\n",
								convADCToVoltage(val));
						else
							printf("Failed to retrieve the battery level\n");
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gi")) {
						printf("Retrieving the band id\n");
    					string bert = bandif.getBandId();
    					printf("device id: %s\n",bert.c_str());
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gsj")) {
						printf("Retrieving the subject id\n");
    					string bert = bandif.getSubjectDetails();
    					printf("user id: %s\n",bert.c_str());
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gt")) {
						printf("Retrieving the test id\n");
    					string bert = bandif.getTestDetails();
    					printf("test id: %s\n",bert.c_str());
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gcen")) {
						printf("Retrieving the centre id\n");
    					string bert = bandif.getCentreId();
    					printf("centre id: %s\n",bert.c_str());
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gsb")) {
						printf("Requires the pages to be counted"
							" - may take a while\n");
						fflush(stdout);
    					printf("bytes count: %d\n",bandif.getStoredSize());
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gsp")) {
						printf("Requires the pages to be counted"
							" - may take a while\n");
						fflush(stdout);
    					printf("page count: %d\n",bandif.getPageCount());
    					return 0;
					}
					if (!strcmp("-scal",argv[arg_idx])) {
						arg_idx++;
						uint16_t cal_gain_offset[MAX_CALIBRATION_AXIS_PARAMS];
						bool all_ok = true;
						for(int loop = 0; loop < MAX_CALIBRATION_AXIS_PARAMS;
							loop++) {
							if (arg_idx < argc) {
								char* tmp = argv[arg_idx];
								if (isdigit(tmp[0])) {
									int val = atoi(tmp);
									if ((val >= 0)&&(val < 65536)) {
										cal_gain_offset[loop] = val;
										printf("%u ",cal_gain_offset[loop]);
									} else {
										printf("Expected calibration param %s"
											" between 0 and 65535\n",tmp);
										all_ok = false;
										break;
									}
								} else {
									printf("Unexpected value <%s> for "
										"calibration param %d\n",tmp,loop);
									all_ok = false;
									break;
								}
							} else {
								printf("Missing calibration param %d\n",loop);
								all_ok = false;
								break;
							}
							arg_idx++;
						}
						printf("\n");
						if (all_ok) {
							int ret =
								bandif.recordCalibrationData(cal_gain_offset);
							if (ret) {
								printf("Error: Failed to set calibration data"
									" (ret %d)\n", ret);
								textError(ret);
							} else {
								printf("OK\n");
							}
						}
						return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-rcal")) {
						printf("Retrieving the calibration data\n");
						uint16_t cal_gain_offset[MAX_CALIBRATION_AXIS_PARAMS];
    					int ret = bandif.getCalibrationData(cal_gain_offset);
    					if (ret) {
							printf("Failed to retrieve the data (ret %d)\n",
								ret);
							textError(ret);
						} else {
							uint16_t* val = cal_gain_offset;
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
    					return 0;
					}
					if (!strcmp("-rp",argv[arg_idx])) {
						arg_idx++;
						if (arg_idx < argc) {
							char* tmp = argv[arg_idx];
							int val = atoi(tmp);
							if (val) {
								uint8_t* page_mem_ptr = NULL;
								uint16_t temp_val;
								int ret = bandif.readPage(val, page_mem_ptr,
									temp_val);
								if (ret) {
									printf("Error: Failed to read the page"
										" (ret %d)\n", ret);
									textError(ret);
								} else {
									if (page_mem_ptr) {
										uint8_t* mem_ptr = page_mem_ptr;
										for (int lp = 0; lp < SAMPLES_PER_PAGE;
											lp++) {
											printf("(%d) ",lp);
											for (int lpv = 0;
												lpv < BYTES_PER_SAMPLE; lpv++) {
												printf("0x%02x ",*mem_ptr++);
											}
											if (!lp) {
												printf("[temp 0x%04x]",
													temp_val);
											}
											printf("\n");
										}
										delete[] page_mem_ptr;
									} else {
										printf("Page mem ptr is null\n");
									}
								}
							} else {
								printf("Error: Not a page number (%s)\n",tmp);
							}
						} else {
							printf("Error: Missing page number\n");
							bandif.cleanup();
							return -E_BB_BAD_PARAM;
						}
    					return 0;
					}
					if (!strcmp("-sac",argv[arg_idx])) {
						accel_data_rate rate = CWA_50HZ;
						accel_g_scale scale = CWA_8G;
						int err = 0;
						arg_idx++;
						if (arg_idx < argc) {
							char* tmp = argv[arg_idx];
							int dr = atoi(tmp);
							if (dr) {
								switch (dr) {
									case 50: rate = CWA_50HZ; break;
									case 100: rate = CWA_100HZ; break;
									case 400: rate = CWA_400HZ; break;
									case 1000: rate = CWA_1000HZ; break;
									default:
										err = 1;
										break;
								}
							} else {
								printf("Unknown rate <%s>\n",tmp);
								err = 1;
							}
							if (!err) {
								arg_idx++;
								if (arg_idx < argc) {
									tmp = argv[arg_idx];
									int sc = atoi(tmp);
									if (sc) {
										switch (sc) {
											case 2: scale = CWA_2G; break;
											case 4: scale = CWA_4G; break;
											case 8: scale = CWA_8G; break;
											default:
												err = 1;
												break;
										}
									} else {
										printf("Unknown scale <%s>\n",tmp);
										err = 1;
									}
								} else {
									printf("Expecting scale value after rate\n");
									err = 1;
								}
							}
						} else {
							printf("Expecting rate value after -sac\n");
							err = 1;
						}
						if (err) {
							printf("Error:\nValid rates = 50,100,400,1000\n"
							 "Valid scales = 2,4,8\n");
						} else {
							int ret = bandif.setAccelConfig(rate, scale);
							if (ret) {
								printf("Error: Failed to set accel config"
									" (ret %d)\n", ret);
								textError(ret);
							} else {
								printf("OK\n");
							}
						}
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-rac")) {
						accel_data_rate rate;
						accel_g_scale scale;
						bandif.readAccelConfig(&rate, &scale);
						printf("rate: ");
						switch(rate) {
							case CWA_50HZ: printf("50Hz"); break;
							case CWA_100HZ: printf("100Hz"); break;
							case CWA_400HZ: printf("400Hz"); break;
							case CWA_1000HZ: printf("1000Hz"); break;
							default:
								printf("Unknown");
								break;
						}
						printf("\nscale: ");
						switch(scale) {
							case CWA_2G: printf("2g"); break;
							case CWA_4G: printf("4g"); break;
							case CWA_8G: printf("8g"); break;
							default:
								printf("Unknown");
								break;
						}
						printf("\n");
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-raf")) {
						accel_data_rate rate;
						accel_g_scale scale;
						bandif.readFlashAccelConfig(&rate, &scale);
						printf("rate: ");
						switch(rate) {
							case CWA_50HZ: printf("50Hz"); break;
							case CWA_100HZ: printf("100Hz"); break;
							case CWA_400HZ: printf("400Hz"); break;
							case CWA_1000HZ: printf("1000Hz"); break;
							default:
								printf("Unknown");
								break;
						}
						printf("\nscale: ");
						switch(scale) {
							case CWA_2G: printf("2g"); break;
							case CWA_4G: printf("4g"); break;
							case CWA_8G: printf("8g"); break;
							default:
								printf("Unknown");
								break;
						}
						printf("\n");
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gv")) {
						float fw, hw;
						string fwdate;
						int ret = bandif.getHwFwVersions(hw, fw, fwdate);
						if (ret) {
							printf(
								"Error: failed to retrieve the versions (%d)\n",
								ret);
							textError(ret);
						} else {
							float bb;
							bandif.getBbApiVersion(bb);
    						printf("versions hw:%.01f fw:%.01f(%s)"
    							" bb_api:%.01f(%s)\n",hw,fw,fwdate.c_str(),
    							bb,version_str);
						}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-gdt")) {
    					time_t dt;
    					if (bandif.getGmtDeviceTime(dt)) {
							printf("Error: failed to retrieve the band time\n");
						} else {
							char local_time_str[20];
							if (!getLocalTime(local_time_str, 20, dt)) {
								printf("local time:\t%s\n",local_time_str);
							} else {
								printf("Error: failed to convert to local\n");
							}
						}
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-gfdt")) {
    					time_t dlt;
    					if (bandif.getFirstDownloadTime(dlt)) {
							printf("Error: failed to retrieve the download"
								" time\n");
						} else if (dlt) {
							char local_time_str[20];
							if (!getLocalTime(local_time_str, 20, dlt)) {
								printf("first download time:\t%s\n",
									local_time_str);
							} else {
								printf("Error: failed to convert to local\n");
							}
						} else {
							printf("No first download time currently set\n");
						}
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-bl")) {
						queue<uint16_t> battery_levels;
						printf("Retrieving battery levels\n");
						int ret = bandif.getBatteryLevels(&battery_levels);
						if (!ret) {
							if (battery_levels.size()) {
								
								printf("Battery levels: (%d)\n",
									(int) battery_levels.size());
								while (battery_levels.size()) {

									uint16_t bl = battery_levels.front();
									printf("\t%.02fV\n",
										convADCToVoltage(bl));
									battery_levels.pop();
								}

							} else {
								printf("No battery levels (?)\n");
							}
						} else {
							printf("Error: Failed to receive battery levels"
								" (%d)\n", ret);
							textError(ret);
						}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-tl")) {
						queue<uint16_t> temp_levels;
						printf("Retrieving temperature levels\n");
						int ret = bandif.getTemperatureLevels(&temp_levels);
						if (!ret) {
							if (temp_levels.size()) {
								
								printf("Temperature levels: (%d)\n",
									(int) temp_levels.size());
								while (temp_levels.size()) {

									uint16_t tl = temp_levels.front();
									printf("\t%.02f\n",
										convTempBinToCelsius(tl));
									temp_levels.pop();
								}
								
							} else {
								printf("No temperature levels (?)\n");
							}
						} else {
							printf("Error: Failed to receive temp levels "
								"(%d)\n", ret);
							textError(ret);
						}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-ef")) {
    					int ret = bandif.clearStoredMeasurements();
						if (ret) {
							printf("Failed (ret %d)\n",ret);
							textError(ret);
						} else {
							printf("OK\n");
						}
    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-ld")) {
						int ret = 0;
						for (int ldlp = 0; ldlp < 8; ldlp++) {
    						ret = bandif.setLed((BioBandIf::led_colour) ldlp);
    						if (ret) {
								printf("Failed to setLed (ret %d)\n",ret);
								textError(ret);
								break;
							}
    						sleep(1);
    					}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-gts")) {
						int ret = bandif.goToSleep();
						if (ret) {
							printf("Failed (ret %d)\n",ret);
							textError(ret);
						} else {
							printf("OK\n");
						}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-nobkp")) {
						int ret = bandif.noBkp();
						if (ret) {
							printf("Failed (ret %d)\n",ret);
							textError(ret);
						} else {
							printf("OK"
								"\t(Now press reset on the breakout board)\n");
						}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-bug")) {
						queue<uint8_t> debug_list;
						int ret = bandif.getDebugBuffer(&debug_list);
						if (ret) {
							printf("Failed (ret %d)\n",ret);
							textError(ret);
						} else {
							if (debug_list.size()) {
								while (debug_list.size()) {

									uint8_t val = debug_list.front();
									if (val) {
										if (val > 0x20 && val < 0x7F)
											printf("%c",(char) val);
										else
											printf("0x%02x ",val);
									} else {
										printf("\n");
									}

									debug_list.pop();
								}
								printf("\n");
							} else {
								printf("No debug data\n");
							}
						}
    					return ret;
					}
					if (singleParam(argv, arg_idx, argc, "-eg")) {
						// example use of band_if class
						// TODO missing ret checks after each call
						string tmp;
						
						int ret = bandif.setLed(BioBandIf::WHITE);
						if (ret) {
							printf("Failed (ret %d)\n",ret);
							textError(ret);
							return ret;
						}
						
						sleep(2);
						
						bandif.setLed(BioBandIf::GREEN);
						sleep(2);
						
						bandif.setLed(BioBandIf::GREEN_BLUE);
						sleep(2);
						
						bandif.setLed(BioBandIf::RED);
						sleep(2);
						
						bandif.setLed(BioBandIf::RED_BLUE);
						sleep(2);
						
						bandif.setLed(BioBandIf::RED_GREEN);
						
						sleep(2);
						
						bandif.setLed(BioBandIf::NOTHING);
						
						// get some current info
    					printf("current page count: %d\n",
    						bandif.getPageCount());
    						
						// clear flash
						bandif.clearStoredMeasurements();

						// set up new ids - not necessary if device has already
						// ids - but worth showing in example
						tmp = "BioBand1";
						bandif.setBandId(tmp);

						tmp = "TestUser";
						bandif.setSubjectDetails(tmp);

						tmp = "Test1";
						bandif.setTestDetails(tmp);

						tmp = "UK";
						bandif.setCentreId(tmp);

						// Set debug just so can see end of collect during this
						// example
						bandif.setBandDebugLeds();
						
						// collect for one minute
						bandif.start(0,1);

    					return 0;
					}
					if (singleParam(argv, arg_idx, argc, "-bb")) {
						
						queue<BioBandIf::bad_block*> bad_block_list;
						printf("Retrieving bad blocks\n");
						int ret = bandif.getBadBlocks(&bad_block_list);
						
						if (!ret) {
							if (bad_block_list.size()) {
								
								printf("Bad blocks: (%d)\n"
									"\tblk\tpage\tmarker\n",
									(int) bad_block_list.size());
								while (bad_block_list.size()) {

									BioBandIf::bad_block* bad_block_ptr =
										bad_block_list.front();
									printf("\t0x%04x\t%d\t0x%02x\n",
										bad_block_ptr->block,
										bad_block_ptr->page,
										bad_block_ptr->marker);

									delete bad_block_ptr;
									bad_block_list.pop();
								}

							} else {
								printf("No bad blocks (?)\n");
							}
						} else {
							printf("Error: Failed to retrieve bad blocks "
								"(%d)\n", ret);
							textError(ret);
						}
						return ret;
					}
#if 0
					if (singleParam(argv, arg_idx, argc, "-dd")) {
						int ret = bandif.setUpDummyData();
						if (ret) {
							printf("Failed to set up dummy data\n");
						} else {
							printf("Dummy data creation started ok\n"
								"Give it a while (wait until the red light"
								" starts flashing again)\n");
						}
						return ret;
					}
#endif
					printf("\n** Unexpected param <%s> **\n\nAllowed params:\n",
						argv[arg_idx]);
					displayHelp();
					bandif.cleanup();
					return -E_BB_BAD_PARAM;
				}
				
			}
			arg_idx++;
		}
	}
	
	if (collection_time_in_mins) {
		if (bandif.start(standby_before_collection_time_mins,
			collection_time_in_mins))
			printf("ERROR\n");
		else
			printf("OK\n");

	}
	return 0;
}

// EOF
