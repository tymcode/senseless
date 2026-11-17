// senseless — .senselmap parser and overlay-flash compiler
// Copyright (c) 2026 senseless contributors. MIT License.
//
// Reimplements what SenselApp did when you pressed "Flash to Morph": parse the text
// .senselmap format, apply the app's version migrations, and lay the map out in the
// Morph's overlay-map flash area (register 0xC5). See docs/OVERLAY_MAPS.md.

#ifndef SENSELESS_SENSELMAP_H
#define SENSELESS_SENSELMAP_H

#include <stddef.h>
#include <stdint.h>

#define SENSELMAP_VERSION_CURRENT  19
#define SENSELMAP_FLASH_HEADER_LEN 96
#define SENSELMAP_FLASH_SIZE       17408

typedef enum {
  OVERLAY_KIND_GENERIC,    // 7 value bytes + 3 LED bytes per region, then the map label
  OVERLAY_KIND_INNOVATOR,  // + 4 fixed-point positions + 2 setting bytes per region, no label
} OverlayKind;

typedef struct {
  const char  *name;        // overlay name as written in .senselmap files
  int          slot;        // byte offset of this overlay's entry in the flash header
  int          page;        // 256-byte page where the map body is written
  int          body_size;   // bytes written for the map body
  int          regions;     // region count SenselApp expects
  OverlayKind  kind;
} OverlayInfo;

typedef struct {
  int       version;
  int       n_values, n_value_cols;
  uint8_t  *values;            // n_values x n_value_cols
  char      overlay[64];
  char      name[128];
  int       n_led, n_led_cols;
  uint8_t  *led;               // n_led x n_led_cols
  int       n_pos, n_pos_cols;
  float    *positions;         // n_pos x n_pos_cols
  int       n_settings, n_settings_cols;
  uint8_t  *settings;          // n_settings x n_settings_cols
  char      label[128];        // what SenselApp flashed as the label: file name sans extension
} SenselMap;

const OverlayInfo *senselmap_overlay_by_name(const char *name);
const OverlayInfo *senselmap_overlay_by_slot(int slot);
const OverlayInfo *senselmap_overlays(int *count);

// Parse a .senselmap file. Returns 0 on success, otherwise writes a message to err.
int  senselmap_load(const char *path, SenselMap *map, char *err, size_t err_len);
void senselmap_free(SenselMap *map);

// Apply SenselApp's in-place version migrations (UpgradeValues). Returns number of changes.
int  senselmap_upgrade(SenselMap *map);

// Compile the map body for flashing. `out` must hold info->body_size bytes.
int  senselmap_compile_body(const SenselMap *map, const OverlayInfo *info, uint8_t *out, char *err, size_t err_len);

// Build the 96-byte flash header from the current header (may be NULL or all 0xFF when the
// device has no maps) with `info`'s slot marked present.
void senselmap_build_header(const uint8_t *current, const OverlayInfo *info, uint8_t out[SENSELMAP_FLASH_HEADER_LEN]);

#endif
