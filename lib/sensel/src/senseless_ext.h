/******************************************************************************************
* senseless — protocol extensions beyond the public LibSensel API
*
* Copyright (c) 2026 senseless contributors. MIT License (see LICENSE).
*
* SenselApp used two variable-size (VS) register transfers that the published LibSensel
* does not implement: a VS write with an explicit byte offset (8-byte VS header) and a
* multi-packet VS read with per-packet continuation acks. Both are needed to read and
* write the Morph's overlay map storage (register 0xC5, FLASH_OVERLAYS).
*
* All functions here must be called while scanning is stopped.
******************************************************************************************/

#ifndef __SENSELESS_EXT_H__
#define __SENSELESS_EXT_H__

#include "sensel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSELESS_REG_FLASH_OVERLAYS   0xC5
#define SENSELESS_REG_FLASH_APP_RESET  0xC8
#define SENSELESS_OVERLAY_FLASH_SIZE   17408   // 68 pages x 256 bytes

/*!
 * Multi-packet VS read. Reads up to buf_size bytes from `reg` into buf; *read_size receives
 * the number of bytes the device returned (0 if the register is empty).
 */
SENSEL_API
SenselStatus WINAPI senselessReadRegVSStream(SENSEL_HANDLE handle, unsigned char reg, unsigned char *buf,
                                             unsigned int buf_size, unsigned int *read_size);

/*!
 * VS write of `size` bytes at byte `offset` within the register's address space.
 */
SENSEL_API
SenselStatus WINAPI senselessWriteRegVSOffset(SENSEL_HANDLE handle, unsigned char reg, const unsigned char *buf,
                                              unsigned int size, unsigned int offset);

/*!
 * Reads the whole overlay-map storage area (SENSELESS_OVERLAY_FLASH_SIZE bytes).
 */
SENSEL_API
SenselStatus WINAPI senselessReadOverlayFlash(SENSEL_HANDLE handle, unsigned char *buf, unsigned int buf_size,
                                              unsigned int *read_size);

/*!
 * Asks the device to apply flashed overlay data and reboot (what SenselApp does after
 * writing maps). The USB device disconnects and re-enumerates; close the handle afterwards.
 */
SENSEL_API
SenselStatus WINAPI senselessApplyAndReset(SENSEL_HANDLE handle);

/*!
 * Returns the senseless library version string, e.g. "senseless 0.1.0 (libsensel 0.8.2)".
 */
SENSEL_API
const char * WINAPI senselessLibraryVersion(void);

#ifdef __cplusplus
}
#endif

#endif // __SENSELESS_EXT_H__
