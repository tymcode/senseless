/******************************************************************************************
* senseless — protocol extensions beyond the public LibSensel API
*
* Copyright (c) 2026 senseless contributors. MIT License (see LICENSE).
* Wire format documented in docs/PROTOCOL.md ("Variable-size transfers").
******************************************************************************************/

#include <stdio.h>
#include <string.h>

#include "sensel.h"
#include "sensel_device.h"
#include "sensel_register.h"
#include "sensel_serial.h"
#include "senseless_ext.h"

#ifndef SENSELESS_VERSION
  #define SENSELESS_VERSION "0.0.0"
#endif

#define RW_READ   0x81   // DEFAULT_BOARD_ADDR | read bit
#define RW_WRITE  0x01
#define VS_CONTINUE_ACK 0x01
#define VS_MORE_FLAG    0x8000
#define VS_LEN_MASK     0x7FFF
#define VS_WRITE_CHUNK  512

SENSEL_API
SenselStatus WINAPI senselessReadRegVSStream(SENSEL_HANDLE handle, unsigned char reg, unsigned char *buf,
                                             unsigned int buf_size, unsigned int *read_size)
{
  SenselDevice       *device = (SenselDevice *)handle;
  SenselSerialHandle *serial;
  unsigned char      cmd[3] = { RW_READ, reg, 0 };
  unsigned char      ack[3];
  unsigned char      cont   = VS_CONTINUE_ACK;
  unsigned int       total  = 0;
  int                more   = 1;

  if (!device || !buf)
    return SENSEL_ERROR;
  if (device->scanning_active)
    return SENSEL_ERROR;
  serial = &device->sensor_serial;
  if (read_size)
    *read_size = 0;

  if (!senselSerialWrite(serial, cmd, sizeof(cmd)))
    return SENSEL_ERROR;
  if (!senselSerialReadBytes(serial, ack, sizeof(ack)))
    return SENSEL_ERROR;

  while (more)
  {
    unsigned char  hdr[2];
    unsigned short word;
    unsigned int   n, i;
    unsigned char  checksum = 0, resp_checksum;

    if (!senselSerialReadBytes(serial, hdr, 2))
      return SENSEL_ERROR;
    word = (unsigned short)(hdr[0] | (hdr[1] << 8));
    more = (word & VS_MORE_FLAG) != 0;
    n    = word & VS_LEN_MASK;

    if (total + n > buf_size)
      return SENSEL_ERROR;
    if (n && !senselSerialReadBytes(serial, buf + total, (int)n))
      return SENSEL_ERROR;
    if (!senselSerialReadBytes(serial, &resp_checksum, 1))
      return SENSEL_ERROR;

    for (i = 0; i < n; i++)
      checksum += buf[total + i];
    if (checksum != resp_checksum)
      return SENSEL_ERROR;

    if (n == 0 && total == 0)
      break;   // empty register

    // SenselApp acknowledges every packet except a lone single-packet response.
    if (total != 0 || more)
      if (!senselSerialWrite(serial, &cont, 1))
        return SENSEL_ERROR;

    total += n;
  }

  if (read_size)
    *read_size = total;
  return SENSEL_OK;
}

SENSEL_API
SenselStatus WINAPI senselessWriteRegVSOffset(SENSEL_HANDLE handle, unsigned char reg, const unsigned char *buf,
                                              unsigned int size, unsigned int offset)
{
  SenselDevice       *device = (SenselDevice *)handle;
  SenselSerialHandle *serial;
  unsigned char      hdr[13];
  unsigned char      resp[2];
  unsigned char      packet[VS_WRITE_CHUNK + 3];
  unsigned int       sent = 0;
  int                i;

  if (!device || (!buf && size))
    return SENSEL_ERROR;
  if (device->scanning_active)
    return SENSEL_ERROR;
  serial = &device->sensor_serial;

  hdr[0] = RW_WRITE;
  hdr[1] = reg;
  hdr[2] = 0;
  hdr[3] = 8;                       // VS header size: 4-byte length + 4-byte offset
  for (i = 0; i < 4; i++)
  {
    hdr[4 + i] = (unsigned char)(size   >> (8 * i));
    hdr[8 + i] = (unsigned char)(offset >> (8 * i));
  }
  hdr[12] = 0;
  for (i = 4; i < 12; i++)
    hdr[12] += hdr[i];

  if (!senselSerialWrite(serial, hdr, sizeof(hdr)))
    return SENSEL_ERROR;
  if (!senselSerialReadBytes(serial, resp, 2))
    return SENSEL_ERROR;
  if (resp[0] == PT_WVS_NACK)
    return SENSEL_ERROR;

  while (sent < size)
  {
    unsigned int  n = (size - sent < VS_WRITE_CHUNK) ? size - sent : VS_WRITE_CHUNK;
    unsigned char checksum = 0;
    unsigned char ack;
    unsigned int  j;

    packet[0] = (unsigned char)(n & 0xFF);
    packet[1] = (unsigned char)(n >> 8);
    for (j = 0; j < n; j++)
    {
      packet[2 + j] = buf[sent + j];
      checksum += buf[sent + j];
    }
    packet[2 + n] = checksum;

    if (!senselSerialWrite(serial, packet, (int)(n + 3)))
      return SENSEL_ERROR;
    if (!senselSerialReadBytes(serial, &ack, 1))
      return SENSEL_ERROR;
    if (ack == PT_WVS_NACK)
      return SENSEL_ERROR;

    sent += n;
  }
  return SENSEL_OK;
}

SENSEL_API
SenselStatus WINAPI senselessReadOverlayFlash(SENSEL_HANDLE handle, unsigned char *buf, unsigned int buf_size,
                                              unsigned int *read_size)
{
  return senselessReadRegVSStream(handle, SENSELESS_REG_FLASH_OVERLAYS, buf, buf_size, read_size);
}

SENSEL_API
SenselStatus WINAPI senselessApplyAndReset(SENSEL_HANDLE handle)
{
  unsigned char one = 1;
  SenselDevice  *device = (SenselDevice *)handle;

  if (!device)
    return SENSEL_ERROR;
  // The device reboots after acknowledging (or sometimes before); a failed ack read is expected.
  _senselWriteReg(handle, &device->sensor_serial, SENSELESS_REG_FLASH_APP_RESET, 1, &one);
  return SENSEL_OK;
}

SENSEL_API
const char * WINAPI senselessLibraryVersion(void)
{
  return "senseless " SENSELESS_VERSION " (libsensel 0.8.2 API)";
}
