/******************************************************************************************
* senseless — macOS serial backend for LibSensel
*
* Copyright (c) 2013-2017 Sensel, Inc. (original POSIX backend, sensel_serial_linux.c)
* Copyright (c) 2026 senseless contributors (macOS rewrite)
* MIT License (see LICENSE.sensel / LICENSE).
*
* The Morph enumerates as a USB CDC-ACM device, which macOS drives with its built-in
* AppleUSBCDCACM driver — no kernel extension or DriverKit driver is required. This
* backend talks to it through the /dev/cu.* callout node.
*
* Differences from the upstream POSIX backend:
*   - Devices are discovered through IOKit by USB vendor ID (0x2C2F) instead of by
*     scanning /dev names, so each Morph is listed exactly once (upstream matched both
*     tty.usbmodem* and cu.usbmodem* and therefore listed every device twice).
*     Probing every /dev/cu.usbmodem* is opt-in (SENSELESS_PROBE_ALL=1); SENSEL_PORT
*     forces a specific port.
*   - Writes loop until all bytes are sent (the fd is non-blocking, so a large write —
*     e.g. a flash packet — can be partial).
*   - The port is opened with TIOCEXCL so a second process cannot interleave traffic.
*   - Stale input (e.g. a previous process that died while scanning) is flushed and
*     the magic probe retried once.
*   - Read timeouts are silent unless SENSEL_DEBUG is defined.
******************************************************************************************/

#include "sensel.h"
#include "sensel_device.h"
#include "sensel_serial.h"
#include "sensel_register.h"
#include "sensel_register_map.h"

#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#define SENSEL_USB_VID           0x2C2F
#define SENSEL_SERIAL_DIR        "/dev/"
#define SENSEL_READ_TIMEOUT_US   (500 * 1000)
#define SENSEL_WRITE_TIMEOUT_US  (1000 * 1000)
#define MAX_CANDIDATES           32
#define SERIAL_PATH_LEN          128

#ifdef SENSEL_DEBUG
  #define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
  #define DBG(...) do {} while (0)
#endif

// Upstream API: the device list from the last scan is used by the Open*ByX functions.
static unsigned char    devices_scanned = 0;
static SenselDeviceList devlist;

////////////////////////////////////////////////////////////////////////////////
// I/O primitives

unsigned char senselSerialWrite(SenselSerialHandle *data, unsigned char *buf, int buf_len)
{
  int written = 0;

  while (written < buf_len)
  {
    ssize_t wr = write(data->serial_fd, buf + written, buf_len - written);
    if (wr > 0)
    {
      written += (int)wr;
      continue;
    }
    if (wr < 0 && errno != EAGAIN && errno != EINTR)
      return 0;

    fd_set write_fds;
    struct timeval timeout = { 0, SENSEL_WRITE_TIMEOUT_US };
    FD_ZERO(&write_fds);
    FD_SET(data->serial_fd, &write_fds);
    if (select(data->serial_fd + 1, NULL, &write_fds, NULL, &timeout) <= 0)
      return 0;
  }
  return 1;
}

int senselSerialReadAvailable(SenselSerialHandle *data, unsigned char *buf, int buf_len)
{
  for (;;)
  {
    fd_set read_fds;
    struct timeval timeout = { 0, SENSEL_READ_TIMEOUT_US };
    int ret;

    FD_ZERO(&read_fds);
    FD_SET(data->serial_fd, &read_fds);

    ret = select(data->serial_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ret < 0)
    {
      if (errno == EINTR)
        continue;
      DBG("sensel: select() failed: %s\n", strerror(errno));
      return -1;
    }
    if (ret == 0)
    {
      DBG("sensel: [fd:%d] read timed out\n", data->serial_fd);
      return 0;
    }

    ret = (int)read(data->serial_fd, buf, buf_len);
    if (ret < 0 && (errno == EAGAIN || errno == EINTR))
      continue;
    if (ret < 0)
      DBG("sensel: read() failed: %s\n", strerror(errno));
    return ret;
  }
}

unsigned char senselSerialReadBytes(SenselSerialHandle *data, unsigned char *buf, int buf_len)
{
  int bytes_left = buf_len;

  while (bytes_left > 0)
  {
    int bytes_read = senselSerialReadAvailable(data, buf, bytes_left);
    if (bytes_read <= 0)
      return 0;
    buf        += bytes_read;
    bytes_left -= bytes_read;
  }
  return 1;
}

int senselSerialGetAvailable(SenselSerialHandle *data)
{
  int bytes_avail = 0;

  if (ioctl(data->serial_fd, FIONREAD, &bytes_avail) < 0)
    return 0;
  return bytes_avail;
}

void senselSerialFlushInput(SenselSerialHandle *data)
{
  unsigned char buf[512];

  tcflush(data->serial_fd, TCIFLUSH);
  // Drain anything still in flight without waiting the full read timeout per chunk.
  for (;;)
  {
    fd_set read_fds;
    struct timeval timeout = { 0, 50 * 1000 };
    FD_ZERO(&read_fds);
    FD_SET(data->serial_fd, &read_fds);
    if (select(data->serial_fd + 1, &read_fds, NULL, NULL, &timeout) <= 0)
      break;
    if (read(data->serial_fd, buf, sizeof(buf)) <= 0)
      break;
  }
}

void senselSerialClose(SenselSerialHandle *data)
{
  if (data->serial_fd != -1)
  {
    ioctl(data->serial_fd, TIOCNXCL);
    close(data->serial_fd);
    data->serial_fd = -1;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Open + probe

static int probeMagic(SenselSerialHandle *data)
{
  char magic[SENSEL_MAGIC_LEN + 1];

  memset(magic, 0, sizeof(magic));
  if (_senselReadReg(NULL, data, SENSEL_REG_MAGIC, SENSEL_MAGIC_LEN, (unsigned char *)magic) != SENSEL_OK)
    return 0;
  return strcmp(magic, SENSEL_MAGIC) == 0;
}

static unsigned char serialOpenPath(SenselSerialHandle *data, const char *path)
{
  struct termios options;

  data->serial_fd = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY);
  if (data->serial_fd == -1)
  {
    DBG("sensel: open(%s) failed: %s\n", path, strerror(errno));
    return 0;
  }

  if (ioctl(data->serial_fd, TIOCEXCL) == -1)
  {
    DBG("sensel: %s is in use by another process\n", path);
    close(data->serial_fd);
    data->serial_fd = -1;
    return 0;
  }

  tcgetattr(data->serial_fd, &options);
  cfmakeraw(&options);
  options.c_cflag |= (CLOCAL | CREAD);
  options.c_cflag &= ~(CRTSCTS | CSTOPB);
  options.c_cc[VMIN]  = 0;
  options.c_cc[VTIME] = 0;
  cfsetispeed(&options, B115200);   // ignored by CDC-ACM; kept for parity with upstream
  cfsetospeed(&options, B115200);
  tcsetattr(data->serial_fd, TCSANOW, &options);

  senselSerialFlushInput(data);
  if (probeMagic(data))
    return 1;

  // A previous client may have left the device streaming frames; drain and retry once.
  senselSerialFlushInput(data);
  if (probeMagic(data))
    return 1;

  senselSerialClose(data);
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Discovery

static int cfNumberToInt(CFTypeRef ref, int *out)
{
  if (!ref || CFGetTypeID(ref) != CFNumberGetTypeID())
    return 0;
  return CFNumberGetValue((CFNumberRef)ref, kCFNumberIntType, out);
}

// Fill `paths` with /dev/cu.* nodes whose USB ancestor has Sensel's vendor ID.
// Returns the number found, or -1 if the IOKit registry could not be queried.
static int findCandidatesIOKit(char paths[][SERIAL_PATH_LEN], int max)
{
  CFMutableDictionaryRef match;
  io_iterator_t          iter;
  io_object_t            service;
  int                    count = 0;

  match = IOServiceMatching(kIOSerialBSDServiceValue);
  if (!match)
    return -1;
  CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));

  if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &iter) != KERN_SUCCESS)
    return -1;

  while ((service = IOIteratorNext(iter)) && count < max)
  {
    CFTypeRef vid_ref = IORegistryEntrySearchCFProperty(service, kIOServicePlane, CFSTR("idVendor"),
                                                        kCFAllocatorDefault,
                                                        kIORegistryIterateRecursively | kIORegistryIterateParents);
    int vid = 0;

    if (cfNumberToInt(vid_ref, &vid) && vid == SENSEL_USB_VID)
    {
      CFTypeRef path_ref = IORegistryEntryCreateCFProperty(service, CFSTR(kIOCalloutDeviceKey),
                                                           kCFAllocatorDefault, 0);
      if (path_ref && CFGetTypeID(path_ref) == CFStringGetTypeID() &&
          CFStringGetCString((CFStringRef)path_ref, paths[count], sizeof(paths[count]), kCFStringEncodingUTF8))
      {
        count++;
      }
      if (path_ref)
        CFRelease(path_ref);
    }
    if (vid_ref)
      CFRelease(vid_ref);
    IOObjectRelease(service);
  }
  IOObjectRelease(iter);
  return count;
}

// Fallback: every /dev/cu.usbmodem* node (probed later with the magic register).
static int findCandidatesDev(char paths[][SERIAL_PATH_LEN], int max)
{
  DIR           *d = opendir(SENSEL_SERIAL_DIR);
  struct dirent *ent;
  int           count = 0;

  if (!d)
    return 0;
  while ((ent = readdir(d)) != NULL && count < max)
  {
    if (strncmp(ent->d_name, "cu.usbmodem", 11) == 0)
    {
      snprintf(paths[count], sizeof(paths[count]), "%s%s", SENSEL_SERIAL_DIR, ent->d_name);
      count++;
    }
  }
  closedir(d);
  return count;
}

// Candidate ports, in order of preference:
//   1. $SENSEL_PORT, if set (e.g. /dev/cu.usbmodem1234561) — used as-is.
//   2. Serial nodes whose USB device has Sensel's vendor ID (IOKit).
//   3. Only if IOKit cannot be queried, or $SENSELESS_PROBE_ALL=1: every /dev/cu.usbmodem*.
//      This sends a Sensel register read to each port, so it is not done by default —
//      other USB-serial devices (Arduinos, printers, ...) should not receive stray bytes.
static int findCandidates(char paths[][SERIAL_PATH_LEN], int max)
{
  const char *port = getenv("SENSEL_PORT");
  const char *probe_all = getenv("SENSELESS_PROBE_ALL");
  int n;

  if (port && *port && max > 0)
  {
    snprintf(paths[0], SERIAL_PATH_LEN, "%s", port);
    return 1;
  }
  n = findCandidatesIOKit(paths, max);
  if (n < 0 || (probe_all && strcmp(probe_all, "1") == 0))
    n = findCandidatesDev(paths, max);
  return n;
}

////////////////////////////////////////////////////////////////////////////////
// Upstream entry points

unsigned char senselSerialOpen2(SenselSerialHandle *data, char *file_name)
{
  return serialOpenPath(data, file_name);
}

unsigned char senselSerialOpen(SenselSerialHandle *data, char *com_port)
{
  char paths[MAX_CANDIDATES][SERIAL_PATH_LEN];
  int  n, i;

  if (com_port != NULL)
    return serialOpenPath(data, com_port);

  n = findCandidates(paths, MAX_CANDIDATES);
  for (i = 0; i < n; i++)
    if (serialOpenPath(data, paths[i]))
      return 1;
  return 0;
}

unsigned char senselSerialOpenDeviceByID(SenselSerialHandle *data, unsigned char idx)
{
  int i;

  if (!devices_scanned)
    return 0;
  for (i = 0; i < devlist.num_devices; i++)
    if (devlist.devices[i].idx == idx)
      return serialOpenPath(data, (char *)devlist.devices[i].com_port);
  return 0;
}

unsigned char senselSerialOpenDeviceBySerialNum(SenselSerialHandle *data, char *serial_num)
{
  int i;

  if (!devices_scanned)
    return 0;
  for (i = 0; i < devlist.num_devices; i++)
    if (!strcmp(serial_num, (char *)devlist.devices[i].serial_num))
      return serialOpenPath(data, (char *)devlist.devices[i].com_port);
  return 0;
}

unsigned char senselSerialOpenDeviceByComPort(SenselSerialHandle *data, char *com_port)
{
  int i;

  if (!devices_scanned)
    return 0;
  for (i = 0; i < devlist.num_devices; i++)
    if (!strcmp(com_port, (char *)devlist.devices[i].com_port))
      return serialOpenPath(data, (char *)devlist.devices[i].com_port);
  return 0;
}

unsigned char senselSerialScan(SenselDeviceList *list)
{
  char               paths[MAX_CANDIDATES][SERIAL_PATH_LEN];
  SenselSerialHandle serial;
  unsigned char      num_devices = 0;
  int                n, i, j;

  memset(&devlist, 0, sizeof(devlist));
  n = findCandidates(paths, MAX_CANDIDATES);

  for (i = 0; i < n && num_devices < SENSEL_MAX_DEVICES; i++)
  {
    SenselDeviceID *devid = &devlist.devices[num_devices];
    unsigned int   num_chars = 0;

    serial.serial_fd = -1;
    if (!serialOpenPath(&serial, paths[i]))
      continue;

    if (_senselReadRegVS(NULL, &serial, SENSEL_REG_DEVICE_SERIAL_NUMBER, sizeof(devid->serial_num) - 1,
                         devid->serial_num, &num_chars) == SENSEL_OK)
    {
      // Firmware reports a 16-byte serial of which only 13 bytes are valid; padding is 0xFF.
      for (j = 0; j < (int)num_chars; j++)
        if (devid->serial_num[j] == 0xFF)
          devid->serial_num[j] = 0;
      devid->serial_num[num_chars] = 0;
      devid->idx = num_devices;
      strncpy((char *)devid->com_port, paths[i], sizeof(devid->com_port) - 1);
      devlist.num_devices = ++num_devices;
    }
    senselSerialClose(&serial);
  }

  devices_scanned = 1;
  memcpy(list, &devlist, sizeof(SenselDeviceList));
  return num_devices;
}
