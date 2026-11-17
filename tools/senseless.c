// senseless — command-line utility for Sensel Morph devices
// Copyright (c) 2026 senseless contributors. MIT License.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sensel.h"
#include "senseless_ext.h"
#include "senselmap.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void usage(void)
{
  fprintf(stderr,
    "usage: senseless <command> [options]\n"
    "\n"
    "Device:\n"
    "  list                          list connected Sensel devices\n"
    "  info                          firmware / sensor information\n"
    "  contacts                      stream contacts (Ctrl-C to stop)\n"
    "  forces [--map]                stream total force, or an ASCII force map\n"
    "\n"
    "Overlay maps (firmware-side MIDI/MPE/HID layouts):\n"
    "  overlays                      list maps stored on the device\n"
    "  backup <file.bin>             save the device's overlay-map flash to a file\n"
    "  map-check <file.senselmap>... parse and compile maps offline (no device needed)\n"
    "  map-flash <file.senselmap>    flash a map to the device (backs up first)\n"
    "  map-clear                     erase all stored maps (factory default behaviour)\n"
    "  restore <file.bin>            write a backup made with `backup` to the device\n"
    "\n"
    "Options:\n"
    "  -d <index>                    device index from `list` (default 0)\n"
    "  -x                            map-check: hex-dump the compiled body\n"
    "  --yes                         confirm commands that write to device flash\n");
}

// ---------------------------------------------------------------- device helpers

static SENSEL_HANDLE open_device(int idx)
{
  SenselDeviceList list;
  SENSEL_HANDLE h = NULL;

  memset(&list, 0, sizeof(list));
  senselGetDeviceList(&list);
  if (list.num_devices == 0) {
    fprintf(stderr, "No Sensel device found. Is the Morph plugged in (and SenselApp / other clients closed)?\n");
    return NULL;
  }
  if (idx >= list.num_devices) {
    fprintf(stderr, "Device index %d out of range (%d found)\n", idx, list.num_devices);
    return NULL;
  }
  if (senselOpenDeviceByID(&h, list.devices[idx].idx) != SENSEL_OK || !h) {
    fprintf(stderr, "Failed to open %s\n", list.devices[idx].com_port);
    return NULL;
  }
  return h;
}

static int cmd_list(void)
{
  SenselDeviceList list;
  memset(&list, 0, sizeof(list));
  senselGetDeviceList(&list);
  if (list.num_devices == 0) {
    printf("No Sensel devices found.\n");
    return 1;
  }
  for (int i = 0; i < list.num_devices; i++)
    printf("%d  serial %-16s  %s\n", list.devices[i].idx, list.devices[i].serial_num, list.devices[i].com_port);
  return 0;
}

static int cmd_info(int idx)
{
  SENSEL_HANDLE h = open_device(idx);
  SenselFirmwareInfo fw;
  SenselSensorInfo si;
  unsigned char content = 0, leds = 0;
  unsigned short max_rate = 0, max_bright = 0;

  if (!h) return 1;
  senselGetFirmwareInfo(h, &fw);
  senselGetSensorInfo(h, &si);
  senselGetSupportedFrameContent(h, &content);
  senselGetMaxFrameRate(h, &max_rate);
  senselGetNumAvailableLEDs(h, &leds);
  senselGetMaxLEDBrightness(h, &max_bright);

  printf("library        %s\n", senselessLibraryVersion());
  printf("firmware       %d.%d.%d (protocol %d, release %d)\n", fw.fw_version_major, fw.fw_version_minor,
         fw.fw_version_build, fw.fw_protocol_version, fw.fw_version_release);
  printf("device         id 0x%04X, revision %d\n", fw.device_id, fw.device_revision);
  printf("sensor         %.1f x %.1f mm, %d x %d elements, max %d contacts\n", si.width, si.height,
         si.num_cols, si.num_rows, si.max_contacts);
  printf("frame content  %s%s%s%s\n", (content & FRAME_CONTENT_PRESSURE_MASK) ? "pressure " : "",
         (content & FRAME_CONTENT_LABELS_MASK) ? "labels " : "",
         (content & FRAME_CONTENT_CONTACTS_MASK) ? "contacts " : "",
         (content & FRAME_CONTENT_ACCEL_MASK) ? "accel " : "");
  printf("max frame rate %d Hz\n", max_rate);
  printf("LEDs           %d (max brightness %d)\n", leds, max_bright);
  senselClose(h);
  return 0;
}

static const char *state_name(unsigned int s)
{
  switch (s) {
    case CONTACT_START: return "start";
    case CONTACT_MOVE:  return "move ";
    case CONTACT_END:   return "end  ";
    default:            return "?    ";
  }
}

static int stream(int idx, int forces, int ascii_map)
{
  SENSEL_HANDLE h = open_device(idx);
  SenselFrameData *frame = NULL;
  SenselSensorInfo si;

  if (!h) return 1;
  senselGetSensorInfo(h, &si);
  senselSetFrameContent(h, forces ? FRAME_CONTENT_PRESSURE_MASK : FRAME_CONTENT_CONTACTS_MASK);
  senselAllocateFrameData(h, &frame);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  if (senselStartScanning(h) != SENSEL_OK) {
    fprintf(stderr, "Failed to start scanning\n");
    senselClose(h);
    return 1;
  }

  while (!g_stop) {
    unsigned int n = 0;
    if (senselReadSensor(h) != SENSEL_OK) continue;
    senselGetNumAvailableFrames(h, &n);
    for (unsigned int f = 0; f < n; f++) {
      senselGetFrame(h, frame);
      if (!forces) {
        for (int c = 0; c < frame->n_contacts; c++) {
          SenselContact *k = &frame->contacts[c];
          printf("contact %2d %s  x %6.1f mm  y %6.1f mm  force %7.1f g  area %5.0f\n", k->id,
                 state_name(k->state), k->x_pos, k->y_pos, k->total_force, k->area);
        }
        fflush(stdout);
      } else if (!ascii_map) {
        float total = 0;
        for (int i = 0; i < si.num_rows * si.num_cols; i++) total += frame->force_array[i];
        printf("\rtotal force %9.1f g   ", total);
        fflush(stdout);
      } else {
        static const char shades[] = " .:-=+*#%@";
        printf("\033[H\033[2J");
        for (int y = 0; y < si.num_rows; y += 4) {
          for (int x = 0; x < si.num_cols; x += 2) {
            float v = frame->force_array[y * si.num_cols + x];
            int s = v <= 0 ? 0 : v < 1 ? 1 : v < 2 ? 2 : v < 4 ? 3 : v < 8 ? 4 : v < 16 ? 5 : v < 32 ? 6 : v < 64 ? 7 : v < 128 ? 8 : 9;
            putchar(shades[s]);
          }
          putchar('\n');
        }
        fflush(stdout);
      }
    }
  }

  printf("\n");
  senselStopScanning(h);
  senselFreeFrameData(h, frame);
  senselClose(h);
  return 0;
}

// ---------------------------------------------------------------- overlay maps

static int read_flash(SENSEL_HANDLE h, uint8_t *buf, unsigned int *len)
{
  memset(buf, 0xFF, SENSELESS_OVERLAY_FLASH_SIZE);
  if (senselessReadOverlayFlash(h, buf, SENSELESS_OVERLAY_FLASH_SIZE, len) != SENSEL_OK) {
    fprintf(stderr, "Failed to read overlay flash\n");
    return 0;
  }
  return 1;
}

static int write_file(const char *path, const uint8_t *buf, size_t len)
{
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(buf, 1, len, f) != len) {
    if (f) fclose(f);
    fprintf(stderr, "Failed to write %s\n", path);
    return 0;
  }
  fclose(f);
  return 1;
}

static int has_magic(const uint8_t *buf)
{
  return buf[0] == 0xCA && buf[1] == 0xFE && buf[2] == 0xFE && buf[3] == 0xED;
}

static int cmd_overlays(int idx)
{
  SENSEL_HANDLE h = open_device(idx);
  uint8_t *buf = malloc(SENSELESS_OVERLAY_FLASH_SIZE);
  unsigned int len = 0;
  int count, found = 0;
  const OverlayInfo *ov = senselmap_overlays(&count);

  if (!h || !buf) return 1;
  if (!read_flash(h, buf, &len)) { senselClose(h); return 1; }
  senselClose(h);

  printf("read %u bytes of overlay flash\n", len);
  if (len < SENSELMAP_FLASH_HEADER_LEN || !has_magic(buf)) {
    printf("No custom maps stored (factory defaults in use).\n");
    free(buf);
    return 0;
  }
  for (int i = 0; i < count; i++) {
    const uint8_t *e = &buf[ov[i].slot];
    if (e[0] == 0xFF) continue;
    unsigned int off = (unsigned int)e[1] << 8;
    found++;
    printf("  %-18s page %-3d", ov[i].name, e[1]);
    if (off < len && buf[off] >= 16 && buf[off] <= SENSELMAP_VERSION_CURRENT) {
      printf(" map v%d", buf[off]);
      if (ov[i].kind == OVERLAY_KIND_GENERIC) {
        // label sits after the region table between \r\r ... \r\r
        unsigned int p = off + 4 + (unsigned int)ov[i].regions * 10;
        if (p + 2 < len && buf[p] == '\r' && buf[p + 1] == '\r') {
          printf("  \"");
          for (p += 2; p < len && buf[p] != '\r'; p++) putchar(buf[p]);
          printf("\"");
        }
      }
    } else {
      printf(" (unrecognised body)");
    }
    printf("\n");
  }
  if (!found) printf("Header present but no maps stored.\n");
  free(buf);
  return 0;
}

static int cmd_backup(int idx, const char *path)
{
  SENSEL_HANDLE h = open_device(idx);
  uint8_t *buf = malloc(SENSELESS_OVERLAY_FLASH_SIZE);
  unsigned int len = 0;
  int ok;

  if (!h || !buf) return 1;
  ok = read_flash(h, buf, &len);
  senselClose(h);
  if (!ok) return 1;
  if (!write_file(path, buf, SENSELESS_OVERLAY_FLASH_SIZE)) return 1;
  printf("saved overlay flash (%u bytes reported, %d written) to %s\n", len, SENSELESS_OVERLAY_FLASH_SIZE, path);
  free(buf);
  return 0;
}

static void hexdump(const uint8_t *buf, int len)
{
  for (int i = 0; i < len; i += 16) {
    printf("  %04x ", i);
    for (int j = 0; j < 16 && i + j < len; j++) printf(" %02x", buf[i + j]);
    printf("\n");
  }
}

static int load_and_compile(const char *path, SenselMap *map, const OverlayInfo **info, uint8_t *body, int *body_len, int verbose)
{
  char err[256];
  int upgrades;

  if (senselmap_load(path, map, err, sizeof(err)) != 0) {
    fprintf(stderr, "%s: %s\n", path, err);
    return 0;
  }
  *info = senselmap_overlay_by_name(map->overlay);
  if (!*info) {
    fprintf(stderr, "%s: unknown overlay \"%s\"\n", path, map->overlay);
    senselmap_free(map);
    return 0;
  }
  upgrades = senselmap_upgrade(map);
  *body_len = senselmap_compile_body(map, *info, body, err, sizeof(err));
  if (*body_len < 0) {
    fprintf(stderr, "%s: %s\n", path, err);
    senselmap_free(map);
    return 0;
  }
  if (verbose)
    printf("%s\n  overlay %-18s map v%-2d regions %-3d -> page %d, %d of %d bytes%s\n", path, (*info)->name,
           map->version, map->n_values, (*info)->page, *body_len, (*info)->body_size,
           upgrades ? " (migrated from older map version)" : "");
  return 1;
}

static int cmd_map_check(int argc, char **argv, int hex)
{
  int failures = 0;
  uint8_t body[4096];
  for (int i = 0; i < argc; i++) {
    SenselMap map;
    const OverlayInfo *info;
    int len;
    if (!load_and_compile(argv[i], &map, &info, body, &len, 1)) { failures++; continue; }
    if (hex) hexdump(body, len);
    senselmap_free(&map);
  }
  return failures ? 1 : 0;
}

static int confirm(int yes, const char *what)
{
  if (yes) return 1;
  fprintf(stderr, "This will %s. Re-run with --yes to proceed.\n", what);
  return 0;
}

static void backup_path(char *out, size_t len)
{
  char stamp[32];
  time_t t = time(NULL);
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&t));
  snprintf(out, len, "morph-overlays-%s.bin", stamp);
}

static int cmd_map_flash(int idx, const char *path, int yes)
{
  SenselMap map;
  const OverlayInfo *info;
  uint8_t body[4096], header[SENSELMAP_FLASH_HEADER_LEN];
  uint8_t *flash = malloc(SENSELESS_OVERLAY_FLASH_SIZE);
  unsigned int len = 0;
  int body_len;
  char bpath[64];
  SENSEL_HANDLE h;
  SenselFirmwareInfo fw;

  if (!flash || !load_and_compile(path, &map, &info, body, &body_len, 1)) return 1;
  if (!confirm(yes, "write to the Morph's overlay-map flash")) return 1;

  h = open_device(idx);
  if (!h) return 1;
  senselGetFirmwareInfo(h, &fw);
  if (fw.fw_version_major == 0 && fw.fw_version_minor < SENSELMAP_VERSION_CURRENT) {
    fprintf(stderr, "Firmware %d.%d predates map format v%d; SenselApp required a firmware update first.\n",
            fw.fw_version_major, fw.fw_version_minor, SENSELMAP_VERSION_CURRENT);
    senselClose(h);
    return 1;
  }
  if (!read_flash(h, flash, &len)) { senselClose(h); return 1; }
  backup_path(bpath, sizeof(bpath));
  if (!write_file(bpath, flash, SENSELESS_OVERLAY_FLASH_SIZE)) { senselClose(h); return 1; }
  printf("backed up current overlay flash to %s\n", bpath);

  senselmap_build_header(len >= SENSELMAP_FLASH_HEADER_LEN ? flash : NULL, info, header);

  // Same order as SenselApp: body first, then header, then apply + reset.
  if (senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, body, (unsigned)info->body_size,
                                (unsigned)info->page << 8) != SENSEL_OK ||
      senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, header, sizeof(header), 0) != SENSEL_OK) {
    fprintf(stderr, "Flash write failed. Restore with: senseless restore %s --yes\n", bpath);
    senselClose(h);
    return 1;
  }
  senselessApplyAndReset(h);
  senselClose(h);
  printf("flashed \"%s\" to the %s slot; the Morph is rebooting.\n", map.label, info->name);
  senselmap_free(&map);
  free(flash);
  return 0;
}

static int cmd_map_clear(int idx, int yes)
{
  uint8_t ff[512];
  uint8_t *flash = malloc(SENSELESS_OVERLAY_FLASH_SIZE);
  unsigned int len = 0;
  char bpath[64];
  SENSEL_HANDLE h;

  if (!confirm(yes, "erase every overlay map stored on the Morph")) return 1;
  h = open_device(idx);
  if (!h || !flash) return 1;
  if (!read_flash(h, flash, &len)) { senselClose(h); return 1; }
  backup_path(bpath, sizeof(bpath));
  if (!write_file(bpath, flash, SENSELESS_OVERLAY_FLASH_SIZE)) { senselClose(h); return 1; }
  printf("backed up current overlay flash to %s\n", bpath);

  memset(ff, 0xFF, sizeof(ff));   // SenselApp's "Clear": 512 bytes of 0xFF over the header
  if (senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, ff, sizeof(ff), 0) != SENSEL_OK) {
    fprintf(stderr, "Clear failed.\n");
    senselClose(h);
    return 1;
  }
  senselessApplyAndReset(h);
  senselClose(h);
  printf("maps cleared; the Morph is rebooting.\n");
  free(flash);
  return 0;
}

static int cmd_restore(int idx, const char *path, int yes)
{
  uint8_t *buf = malloc(SENSELESS_OVERLAY_FLASH_SIZE);
  FILE *f = fopen(path, "rb");
  int count, written = 0;
  const OverlayInfo *ov = senselmap_overlays(&count);
  SENSEL_HANDLE h;

  if (!f || !buf || fread(buf, 1, SENSELESS_OVERLAY_FLASH_SIZE, f) != SENSELESS_OVERLAY_FLASH_SIZE) {
    fprintf(stderr, "%s is not a %d-byte overlay backup\n", path, SENSELESS_OVERLAY_FLASH_SIZE);
    if (f) fclose(f);
    return 1;
  }
  fclose(f);
  if (!confirm(yes, "overwrite the Morph's overlay-map flash with this backup")) return 1;
  h = open_device(idx);
  if (!h) return 1;

  if (!has_magic(buf)) {
    uint8_t ff[512];
    memset(ff, 0xFF, sizeof(ff));
    printf("backup has no maps; clearing instead\n");
    if (senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, ff, sizeof(ff), 0) != SENSEL_OK) goto fail;
  } else {
    // Rewrite each present slot's body with the size SenselApp used, then the header.
    for (int i = 0; i < count; i++) {
      const uint8_t *e = &buf[ov[i].slot];
      unsigned int off;
      if (e[0] == 0xFF) continue;
      off = (unsigned int)e[1] << 8;
      if (off + (unsigned)ov[i].body_size > SENSELESS_OVERLAY_FLASH_SIZE) continue;
      if (senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, buf + off, (unsigned)ov[i].body_size, off) != SENSEL_OK)
        goto fail;
      written++;
    }
    if (buf[36] != 0xFF) {   // device-wide "Morph" settings page
      unsigned int off = (unsigned int)buf[37] << 8;
      if (off + 1024 <= SENSELESS_OVERLAY_FLASH_SIZE &&
          senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, buf + off, 1024, off) != SENSEL_OK)
        goto fail;
    }
    if (senselessWriteRegVSOffset(h, SENSELESS_REG_FLASH_OVERLAYS, buf, SENSELMAP_FLASH_HEADER_LEN, 0) != SENSEL_OK)
      goto fail;
  }
  senselessApplyAndReset(h);
  senselClose(h);
  printf("restored %d map(s) from %s; the Morph is rebooting.\n", written, path);
  free(buf);
  return 0;

fail:
  fprintf(stderr, "Write failed during restore.\n");
  senselClose(h);
  return 1;
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv)
{
  int idx = 0, yes = 0, hex = 0, ascii_map = 0;
  char *pos[64];
  int npos = 0;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-d") && i + 1 < argc) idx = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--yes")) yes = 1;
    else if (!strcmp(argv[i], "-x")) hex = 1;
    else if (!strcmp(argv[i], "--map")) ascii_map = 1;
    else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
    else if (npos < 64) pos[npos++] = argv[i];
  }
  if (npos == 0) { usage(); return 2; }

  const char *cmd = pos[0];
  if (!strcmp(cmd, "list"))      return cmd_list();
  if (!strcmp(cmd, "info"))      return cmd_info(idx);
  if (!strcmp(cmd, "contacts"))  return stream(idx, 0, 0);
  if (!strcmp(cmd, "forces"))    return stream(idx, 1, ascii_map);
  if (!strcmp(cmd, "overlays"))  return cmd_overlays(idx);
  if (!strcmp(cmd, "backup")    && npos == 2) return cmd_backup(idx, pos[1]);
  if (!strcmp(cmd, "map-check") && npos >= 2) return cmd_map_check(npos - 1, pos + 1, hex);
  if (!strcmp(cmd, "map-flash") && npos == 2) return cmd_map_flash(idx, pos[1], yes);
  if (!strcmp(cmd, "map-clear")) return cmd_map_clear(idx, yes);
  if (!strcmp(cmd, "restore")   && npos == 2) return cmd_restore(idx, pos[1], yes);
  usage();
  return 2;
}
