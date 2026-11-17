// senseless — .senselmap parser and overlay-flash compiler
// Copyright (c) 2026 senseless contributors. MIT License.

#include "senselmap.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// slot = offset in the flash header; page = (slot - 8), except Innovator's and Thunder,
// which SenselApp stores at page == slot. Body sizes are the buffers SenselApp wrote.
static const OverlayInfo k_overlays[] = {
  { "QWERTY Keyboard",  12,  4, 1024,  79, OVERLAY_KIND_GENERIC   },
  { "Piano",            16,  8, 1024,  38, OVERLAY_KIND_GENERIC   },
  { "Music Production", 20, 12, 1024,  47, OVERLAY_KIND_GENERIC   },
  { "Drum Pad",         24, 16, 1024,  21, OVERLAY_KIND_GENERIC   },
  { "Art",              28, 20, 1024,  20, OVERLAY_KIND_GENERIC   },
  { "Gaming",           32, 24, 1024,  23, OVERLAY_KIND_GENERIC   },
  // slot 36 / page 28 holds device-wide "Morph" settings, not a map
  { "Video Editing",    40, 32, 1024,  66, OVERLAY_KIND_GENERIC   },
  { "Innovator's",      44, 44, 2048,  96, OVERLAY_KIND_INNOVATOR },
  { "No Overlay",       48, 40, 1024,   1, OVERLAY_KIND_GENERIC   },
  { "Buchla Thunder",   52, 52, 4096, 306, OVERLAY_KIND_GENERIC   },
};
#define N_OVERLAYS ((int)(sizeof(k_overlays) / sizeof(k_overlays[0])))

static const uint8_t k_default_header_prefix[12] = {
  0xCA, 0xFE, 0xFE, 0xED,   // magic
  0x00, 0x44, 0x00, 0x00,   // 0x44 = 68 pages of 256 bytes
  0x01, 0x00, 0x00, 0x00,
};

const OverlayInfo *senselmap_overlays(int *count)
{
  if (count) *count = N_OVERLAYS;
  return k_overlays;
}

const OverlayInfo *senselmap_overlay_by_name(const char *name)
{
  for (int i = 0; i < N_OVERLAYS; i++)
    if (strcmp(k_overlays[i].name, name) == 0)
      return &k_overlays[i];
  return NULL;
}

const OverlayInfo *senselmap_overlay_by_slot(int slot)
{
  for (int i = 0; i < N_OVERLAYS; i++)
    if (k_overlays[i].slot == slot)
      return &k_overlays[i];
  return NULL;
}

// ---------------------------------------------------------------- parsing

typedef struct {
  char  **lines;
  int     n;
  int     pos;
} Lines;

static void seterr(char *err, size_t len, const char *fmt, ...)
{
  va_list ap;
  if (!err || !len) return;
  va_start(ap, fmt);
  vsnprintf(err, len, fmt, ap);
  va_end(ap);
}

static const char *next_line(Lines *l)
{
  return l->pos < l->n ? l->lines[l->pos++] : NULL;
}

static int next_int(Lines *l, int *out)
{
  const char *s = next_line(l);
  char *end;
  long v;
  if (!s) return 0;
  v = strtol(s, &end, 10);
  if (end == s) return 0;
  *out = (int)v;
  return 1;
}

static int parse_byte_row(const char *s, uint8_t *row, int cols)
{
  for (int c = 0; c < cols; c++) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || v < 0 || v > 255) return 0;
    row[c] = (uint8_t)v;
    s = end;
    if (c < cols - 1) { if (*s != ',') return 0; s++; }
  }
  return 1;
}

static int parse_float_row(const char *s, float *row, int cols)
{
  for (int c = 0; c < cols; c++) {
    char *end;
    float v = strtof(s, &end);
    if (end == s) return 0;
    row[c] = v;
    s = end;
    if (c < cols - 1) { if (*s != ',') return 0; s++; }
  }
  return 1;
}

static int read_byte_matrix(Lines *l, uint8_t **out, int rows, int cols, int copies)
{
  // .senselmap stores each matrix twice (current + "original"); keep only the first.
  if (rows < 0 || cols <= 0 || rows > 4096 || cols > 64) return 0;
  *out = calloc((size_t)rows * cols, 1);
  if (!*out) return 0;
  for (int copy = 0; copy < copies; copy++)
    for (int r = 0; r < rows; r++) {
      const char *s = next_line(l);
      uint8_t scratch[64];
      if (!s || !parse_byte_row(s, copy == 0 ? &(*out)[r * cols] : scratch, cols)) return 0;
    }
  return 1;
}

static int read_float_matrix(Lines *l, float **out, int rows, int cols, int copies)
{
  if (rows < 0 || cols <= 0 || rows > 4096 || cols > 64) return 0;
  *out = calloc((size_t)rows * cols, sizeof(float));
  if (!*out) return 0;
  for (int copy = 0; copy < copies; copy++)
    for (int r = 0; r < rows; r++) {
      const char *s = next_line(l);
      float scratch[64];
      if (!s || !parse_float_row(s, copy == 0 ? &(*out)[r * cols] : scratch, cols)) return 0;
    }
  return 1;
}

static void base_name(const char *path, char *out, size_t len)
{
  const char *slash = strrchr(path, '/');
  const char *b = slash ? slash + 1 : path;
  const char *dot = strrchr(b, '.');
  size_t n = dot ? (size_t)(dot - b) : strlen(b);
  if (n >= len) n = len - 1;
  memcpy(out, b, n);
  out[n] = 0;
}

int senselmap_load(const char *path, SenselMap *map, char *err, size_t err_len)
{
  FILE *f = fopen(path, "rb");
  char *text = NULL;
  long size;
  Lines l = {0};
  int ok = 0;

  memset(map, 0, sizeof(*map));
  if (!f) { seterr(err, err_len, "cannot open %s", path); return -1; }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0 || size > (16 << 20)) { fclose(f); seterr(err, err_len, "bad file size"); return -1; }
  text = malloc((size_t)size + 1);
  if (!text || fread(text, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(text); seterr(err, err_len, "read failed"); return -1; }
  fclose(f);
  text[size] = 0;

  // Split into lines (tolerate CRLF).
  for (long i = 0; i <= size; i++) if (i == size || text[i] == '\n') l.n++;
  l.lines = calloc((size_t)l.n, sizeof(char *));
  {
    int k = 0;
    char *start = text;
    for (long i = 0; i <= size; i++) {
      if (i == size || text[i] == '\n') {
        text[i] = 0;
        if (i > 0 && text[i - 1] == '\r') text[i - 1] = 0;
        l.lines[k++] = start;
        start = &text[i + 1];
      }
    }
  }

  do {
    const char *s = next_line(&l);
    int rows, cols;
    if (!s || strcmp(s, "SenselMap") != 0) {
      seterr(err, err_len, "not a text .senselmap (binary maps from SenselApp < 0.10 are not supported)");
      break;
    }
    if (!next_int(&l, &map->version)) { seterr(err, err_len, "missing version"); break; }

    if (!next_int(&l, &rows) || !next_int(&l, &cols)) { seterr(err, err_len, "missing value dimensions"); break; }
    map->n_values = rows; map->n_value_cols = cols;
    if (!read_byte_matrix(&l, &map->values, rows, cols, 2)) { seterr(err, err_len, "bad value rows (line %d)", l.pos); break; }

    if (!(s = next_line(&l))) { seterr(err, err_len, "missing overlay name"); break; }
    snprintf(map->overlay, sizeof(map->overlay), "%s", s);
    if (!(s = next_line(&l))) { seterr(err, err_len, "missing map name"); break; }
    snprintf(map->name, sizeof(map->name), "%s", s);

    if (!next_int(&l, &rows) || !next_int(&l, &cols)) { seterr(err, err_len, "missing LED dimensions"); break; }
    map->n_led = rows; map->n_led_cols = cols;
    if (!read_byte_matrix(&l, &map->led, rows, cols, 2)) { seterr(err, err_len, "bad LED rows (line %d)", l.pos); break; }

    if (!next_int(&l, &rows) || !next_int(&l, &cols)) { seterr(err, err_len, "missing position dimensions"); break; }
    map->n_pos = rows; map->n_pos_cols = cols;
    if (!read_float_matrix(&l, &map->positions, rows, cols, 2)) { seterr(err, err_len, "bad position rows (line %d)", l.pos); break; }

    if (!next_int(&l, &rows) || !next_int(&l, &cols)) { seterr(err, err_len, "missing settings dimensions"); break; }
    map->n_settings = rows; map->n_settings_cols = cols;
    if (!read_byte_matrix(&l, &map->settings, rows, cols, 1)) { seterr(err, err_len, "bad settings rows (line %d)", l.pos); break; }

    // Optional per-position strings (version >= 17) are display-only and not flashed.
    base_name(path, map->label, sizeof(map->label));
    ok = 1;
  } while (0);

  free(l.lines);
  free(text);
  if (!ok) { senselmap_free(map); return -1; }
  return 0;
}

void senselmap_free(SenselMap *map)
{
  free(map->values);
  free(map->led);
  free(map->positions);
  free(map->settings);
  map->values = map->led = map->settings = NULL;
  map->positions = NULL;
}

// ---------------------------------------------------------------- migrations

#define WIDGET_MPE 3

int senselmap_upgrade(SenselMap *map)
{
  int changes = 0;
  int c = map->n_value_cols;

  if (map->version <= 5 && c >= 6) {
    // MPE axis destinations were renumbered in map version 6.
    for (int r = 0; r < map->n_values; r++) {
      uint8_t *v = &map->values[r * c];
      if (v[0] != WIDGET_MPE) continue;
      for (int k = 3; k <= 5; k++) {
        if (v[k] == 142) { v[k] = 128; changes++; }
        else if (v[k] == 138) { v[k] = 129; changes++; }
        else if (v[k] == 141) { v[k] = 130; changes++; }
      }
    }
  }
  if (map->version <= 15 && c >= 7) {
    // Absolute-X MPE regions gained two companion flag bits in version 16.
    for (int r = 0; r < map->n_values; r++) {
      uint8_t *v = &map->values[r * c];
      if (v[0] == WIDGET_MPE && (v[6] & 2)) { v[6] |= 6; changes++; }
    }
  }
  return changes;
}

// ---------------------------------------------------------------- compilation

int senselmap_compile_body(const SenselMap *map, const OverlayInfo *info, uint8_t *out, char *err, size_t err_len)
{
  int n = 0;

  if (map->n_value_cols != 7) { seterr(err, err_len, "expected 7 value columns, got %d", map->n_value_cols); return -1; }
  if (map->n_led_cols != 3)   { seterr(err, err_len, "expected 3 LED columns, got %d", map->n_led_cols); return -1; }
  if (map->n_led < map->n_values) { seterr(err, err_len, "fewer LED rows than regions"); return -1; }
  if (map->n_values != info->regions) {
    seterr(err, err_len, "%s map has %d regions, the firmware layout has %d", info->name, map->n_values, info->regions);
    return -1;
  }

  memset(out, 0, (size_t)info->body_size);
  out[n++] = SENSELMAP_VERSION_CURRENT;
  out[n++] = 0; out[n++] = 0; out[n++] = 0;

  if (info->kind == OVERLAY_KIND_INNOVATOR) {
    if (map->n_pos < map->n_values || map->n_pos_cols < 4 || map->n_settings < map->n_values || map->n_settings_cols < 5) {
      seterr(err, err_len, "Innovator's map needs 4 positions and 5 settings per region");
      return -1;
    }
    for (int r = 0; r < map->n_values; r++) {
      if (n + 20 > info->body_size) { seterr(err, err_len, "map too large"); return -1; }
      memcpy(&out[n], &map->values[r * 7], 7); n += 7;
      memcpy(&out[n], &map->led[r * 3], 3);    n += 3;
      for (int k = 0; k < 4; k++) {
        // SenselApp wrote (byte)p and (byte)(p * 256): integer part, then fractional byte.
        float p = map->positions[r * map->n_pos_cols + k];
        int whole = (int)p;
        int fixed = (int)(p * 256.0f);
        out[n++] = (uint8_t)whole;
        out[n++] = (uint8_t)fixed;
      }
      out[n++] = map->settings[r * map->n_settings_cols + 0];
      out[n++] = map->settings[r * map->n_settings_cols + 4];
    }
  } else {
    size_t label_len = strlen(map->label);
    if (n + map->n_values * 10 + 4 + (int)label_len > info->body_size) {
      seterr(err, err_len, "map too large for %d-byte slot", info->body_size);
      return -1;
    }
    for (int r = 0; r < map->n_values; r++) {
      memcpy(&out[n], &map->values[r * 7], 7); n += 7;
      memcpy(&out[n], &map->led[r * 3], 3);    n += 3;
    }
    out[n++] = '\r'; out[n++] = '\r';
    for (size_t i = 0; i < label_len; i++)
      out[n++] = isprint((unsigned char)map->label[i]) ? (uint8_t)map->label[i] : '?';
    out[n++] = '\r'; out[n++] = '\r';
  }
  return n;
}

void senselmap_build_header(const uint8_t *current, const OverlayInfo *info, uint8_t out[SENSELMAP_FLASH_HEADER_LEN])
{
  if (current)
    memcpy(out, current, SENSELMAP_FLASH_HEADER_LEN);
  else
    memset(out, 0xFF, SENSELMAP_FLASH_HEADER_LEN);
  memcpy(out, k_default_header_prefix, sizeof(k_default_header_prefix));
  out[info->slot + 0] = 0;
  out[info->slot + 1] = (uint8_t)info->page;
  out[info->slot + 2] = 7;
  out[info->slot + 3] = 8;
}
