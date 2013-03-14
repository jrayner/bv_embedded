/*-
 * Public platform independent Near Field Communication (NFC) library
 * 
 * Copyright (C) 2009, Roel Verdult
 * 
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

/**
 * @file nfc.h
 * @brief libnfc interface
 *
 * Provide all usefull functions (API) to handle NFC devices.
 */

#ifndef _LIBNFC_H_
#define _LIBNFC_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
  /* Windows platform */
  #ifndef _WINDLL
    /* CMake compilation */
    #ifdef nfc_EXPORTS
      #define  NFC_EXPORT __declspec(dllexport)
    #else /* nfc_EXPORTS */
      #define  NFC_EXPORT __declspec(dllimport)
    #endif /* nfc_EXPORTS */
  #else /* _WINDLL */
    /* Manual makefile */
    #define NFC_EXPORT
  #endif /* _WINDLL */
#else /* _WIN32 */
  #define NFC_EXPORT
#endif /* _WIN32 */

#include <nfc/nfc-types.h>

#ifdef __cplusplus 
    extern "C" {
#endif // __cplusplus

/* NFC Device/Hardware manipulation */
NFC_EXPORT bool nfc_configure(nfc_device_t* pnd, const nfc_device_option_t ndo, const bool bEnable);

/* NFC initiator: act as "reader" */
NFC_EXPORT bool nfc_initiator_init(const nfc_device_t* pnd);
NFC_EXPORT bool nfc_initiator_select_passive_target(const nfc_device_t* pnd, const nfc_modulation_t nmInitModulation, const byte_t* pbtInitData, const size_t szInitDataLen, nfc_target_info_t* pti);
NFC_EXPORT bool nfc_initiator_deselect_target(const nfc_device_t* pnd);
NFC_EXPORT bool nfc_initiator_transceive_bytes(const nfc_device_t* pnd, const byte_t* pbtTx, const size_t szTxLen, byte_t* pbtRx, size_t* pszRxLen);
NFC_EXPORT bool nfc_initiator_transceive_dep_bytes(const nfc_device_t* pnd, const byte_t* pbtTx, const size_t szTxLen, byte_t* pbtRx, size_t* pszRxLen);

/* Misc. functions */
NFC_EXPORT void iso14443a_crc(byte_t* pbtData, size_t szLen, byte_t* pbtCrc);
NFC_EXPORT void append_iso14443a_crc(byte_t* pbtData, size_t szLen);

#ifdef __cplusplus 
}
#endif // __cplusplus


#endif // _LIBNFC_H_

