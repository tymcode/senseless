/******************************************************************************************
* senseless — open reimplementation of Sensel's force-frame decompression
*
* Copyright (c) 2026 senseless contributors. MIT License (see LICENSE).
*
* This header declares the same four entry points that Sensel's closed-source
* libSenselDecompress exported, with the same signatures, so that LibSensel built
* with SENSEL_PRESSURE (and any third-party code that called these directly)
* links against this implementation unchanged.
******************************************************************************************/

#ifndef __SENSEL_DECOMPRESS_H__
#define __SENSEL_DECOMPRESS_H__

#include "sensel.h"

#ifdef __cplusplus
extern "C" {
#endif

  /*!
   * Allocates a decompression context inside the device handle and initialises it
   * from the raw COMPRESSION_METADATA register contents (register 0x1C).
   */
  SENSEL_API
  SenselStatus WINAPI senselInitDecompressionHandle(SENSEL_HANDLE handle, unsigned char *data);

  /*!
   * Frees the decompression context held by the device handle.
   */
  SENSEL_API
  SenselStatus WINAPI senselFreeDecompressionHandle(SENSEL_HANDLE handle);

  /*!
   * Re-initialises the decompression context after a scan-detail change, using freshly
   * read COMPRESSION_METADATA.
   */
  SENSEL_API
  SenselStatus WINAPI senselDecompressionTriggerDetailChange(SENSEL_HANDLE handle, unsigned char *data);

  /*!
   * Decompresses the pressure and/or label payload of one frame into data->force_array
   * and data->labels_array (both sized sensor_info.num_rows * num_cols).
   * *decompress_bytes_read receives the number of payload bytes consumed.
   */
  SENSEL_API
  SenselStatus WINAPI senselDecompressFrame(SENSEL_HANDLE handle, unsigned char *frame_data, int data_size,
                                            unsigned char content_mask, SenselFrameData *data,
                                            unsigned int *decompress_bytes_read);

#ifdef __cplusplus
}
#endif

#endif //__SENSEL_DECOMPRESS_H__
