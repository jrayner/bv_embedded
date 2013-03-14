/** 
 * @file nfc.c
 * @brief Dummy NFC library implementation to satisfy libfreefare
 */

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif // HAVE_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <sys/select.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>

#include <nfc/nfc.h>

#include <nfc/nfc-messages.h>

#ifdef DEBUG
#define _DBG(X) X
#else
#define _DBG(X)
#endif

void print_hex(const char* pcTag, const byte_t* pbtData, ssize_t szBytes) {
#if DEBUG
	do {
		size_t __szPos;
		fprintf(stderr," %s: ", pcTag);
		for (__szPos=0; __szPos < (size_t)(szBytes); __szPos++) {
			fprintf(stderr,"%02x  ",pbtData[__szPos]);
		}
		fprintf(stderr,"\n");
	 } while (0);
#endif
}

bool nfc_configure(nfc_device_t* pnd, const nfc_device_option_t ndo,
	const bool bEnable) {
	_DBG(fprintf(stderr,"nfc_configure\n");)
	return true;
}

bool nfc_initiator_init(const nfc_device_t* pnd) {
	_DBG(fprintf(stderr,"nfc_initiator_init\n");)
	return true;
}

bool nfc_initiator_select_passive_target(const nfc_device_t* pnd,
	const nfc_modulation_t nmInitModulation, const byte_t* pbtInitData,
	const size_t szInitDataLen, nfc_target_info_t* pnti) {
	_DBG(fprintf(stderr,"nfc_initiator_select_passive_target\n");)
	return true;
}

bool nfc_initiator_deselect_target(const nfc_device_t* pnd) {
	_DBG(fprintf(stderr,"nfc_initiator_deselect_target\n");)
	return true;
}

bool nfc_initiator_transceive_dep_bytes(const nfc_device_t* pnd,
	const byte_t* pbtTx, const size_t szTxLen, byte_t* pbtRx,
	size_t* pszRxLen) {
	_DBG(fprintf(stderr,"nfc_initiator_transceive_dep_bytes\n");)
	return true;
}

bool convert_char_to_hex(const byte_t val, byte_t* out) {
	bool res = false;
	if ((0x30 <= val)&&(val <= 0x39)) {
		*out = val - 0x30;
	} else if ((0x41 <= val)&&(val <= 0x46)) {
		*out = (val - 0x41) + 10;
	} else {
		_DBG(fprintf(stderr,"*** Non hex char %d ***\n",val);)
		*out = 0;
		res = true;
	}
	return res;
}

const struct timeval timeout = { 
	.tv_sec  =     0, // 0 second
	.tv_usec = 300000  // 300 ms
};

bool uart_receive(const nfc_device_t* pnd, byte_t* pbtRx, size_t* pszRxLen) {
	int res;
	int byteCount;
	fd_set rfds;
	struct timeval tv;
	byte_t v1, v2;

	int fd = pnd->ui8TxBits;

	// Reset the output count  
	*pszRxLen = 0;

	do {
		FD_ZERO(&rfds);
		FD_SET(fd,&rfds);
		tv = timeout;
		
		do {
			res = select(fd+1, &rfds, NULL, NULL, &tv);
			if (res < 0) {
				sigset_t sigset;
				sigemptyset(&sigset);
				_DBG(fprintf(stderr,"RX select error. %d\n",errno);)
				if(errno==EINTR) {
					_DBG(fprintf(stderr,"Interrupted system call, trying again...\n");)
					continue;
				}
				return false;
			}
		} while (res<0);

		// Read time-out
		if (res == 0) {
			if (*pszRxLen == 0) {
				_DBG(fprintf(stderr,"** RX time-out **\n");)
				return false;
			} else {
				// We received some data, but nothing more is available
				_DBG(fprintf(stderr,"No more\n");)
				return true;
			}
		}

		// Retrieve the count of the incoming bytes
		res = ioctl(fd, FIONREAD, &byteCount);
		if (res < 0) {
			_DBG(fprintf(stderr,"Failed to get byte count %d\n",res);)
			return false;
		}

		// There is something available, read the data
		res = read(fd,pbtRx+(*pszRxLen),byteCount);
		if (res <= 0) {
			_DBG(fprintf(stderr,"Failed to read %d\n",res);)
			return false;
		}

		*pszRxLen += res;
		
		if ((*pszRxLen > 4)&&(0x0d == pbtRx[*pszRxLen - 2])) {
			// 5553 gives length as first two chars and terminates with CR LF
			if (!convert_char_to_hex(pbtRx[0], &v1)) {
				if (!convert_char_to_hex(pbtRx[1], &v2)) {
					v1 = (v1 * 16) + v2;
					if ((v1 * 2) == (*pszRxLen - 4)) {
						//_DBG(fprintf(stderr,"Complete\n");)
						return true;
					}
				}
			}
		}

	} while (byteCount);

	return true;
}

bool uart_send(const nfc_device_t* pnd, const byte_t* pbtTx,
	const size_t szTxLen) {
	int32_t res;
	size_t szPos = 0;
	fd_set rfds;
	struct timeval tv;

	int fd = pnd->ui8TxBits;

	while (szPos < szTxLen) {
		FD_ZERO(&rfds);
		FD_SET(fd,&rfds);
		tv = timeout;
		res = select(fd+1, NULL, &rfds, NULL, &tv);

		if (res < 0) {
			_DBG(fprintf(stderr,"TX error.\n");)
			return false;
		}
		if (res == 0) {
			_DBG(fprintf(stderr,"TX time-out.\n");)
			return false;
		}

		res = write(fd,pbtTx+szPos,szTxLen-szPos);
		if (res <= 0) {
			_DBG(fprintf(stderr,"Failed to send %d\n",res);)
			return false;
		}

		szPos += res;
	}
	return true;
}

bool nfc_initiator_transceive_bytes(const nfc_device_t* pnd,
	const byte_t* pbtTx, const size_t szTxLen, byte_t* pbtRx,
	size_t* pszRxLen) {

	char abtTxBuf[100];
	byte_t abtRxBuf[100];
	size_t szRxBufLen = 100;
	size_t data_len;

	_DBG(print_hex("TX", pbtTx,szTxLen);)

	data_len = szTxLen;
	
	{
		char* dest_ptr = abtTxBuf+5;
		char* src_ptr = (char*) pbtTx;
		
		sprintf(abtTxBuf,"t%02x0F", (int) data_len);

		for (int loop = 0; loop < szTxLen; loop++) {
			// Convert the bytes into chars for 5553 to understand
			u_int8_t val = *src_ptr;
			sprintf(dest_ptr,"%02x", val);
			src_ptr++;
			dest_ptr += 2;
		}
		*dest_ptr = 0;
	}
	
	_DBG(print_hex("TX 5553", (byte_t*) abtTxBuf,(data_len * 2) + 5);)
	
	if (!uart_send(pnd,(byte_t*) abtTxBuf,(data_len * 2) + 5)) {
		_DBG(fprintf(stderr,"Unable to transmit data. (TX)\n");)
		return false;
	}

	if (!uart_receive(pnd,abtRxBuf,&szRxBufLen)) {
		_DBG(fprintf(stderr,"Unable to receive data. (RX)\n");)
		return false;
	}

	_DBG(print_hex("RX 5553", abtRxBuf,szRxBufLen);)

	// When the answer should be ignored, just return a successful result
	if(pbtRx == NULL || pszRxLen == NULL) return true;

	{
		int count = 0;
		byte_t* out_ptr = pbtRx;
		
		// ignore the size byte at start
		byte_t* rx_ptr = abtRxBuf + 2;
		int loop = 0;
		int finish = 0;
		
		while (loop < szRxBufLen) {
			byte_t final = 0;
			for (int bloop = 0; bloop < 2; bloop++) {
				byte_t val = *rx_ptr++;
				byte_t out = 0;
				final <<= 4;
				if ((0x0d == val)||(0x0a == val)) {
					finish = 1;
					break;
				}
				if (convert_char_to_hex(val, &out)) {
					// Non hex string from 5553
					finish = 1;
					break;
				}
				final |= out;
				loop++;
			}
			if (finish)
				break;
				
			*out_ptr = final;
			out_ptr++;
			count++;
		}
		
		*pszRxLen = count;
		if (count) {
			_DBG(print_hex("RX", pbtRx,count);)
		} else {
			_DBG(fprintf(stderr,"** Bad data **\n");)
			return false;
		}
	}

	return true;
}
