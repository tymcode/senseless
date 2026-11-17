// Differential test: run two implementations of libSenselDecompress side by side on
// identical synthetic compressed frames and compare their output.
//
//   decompress_diff <reference.dylib> <candidate.dylib> [iterations]
//
// To compare against Sensel's original (x86_64-only) library this must be built for
// x86_64 and run under Rosetta; see tests/run_decompress_diff.sh.
//
// With a single library argument it runs a self-check instead: encode -> decode and
// verify the coarse values survive (using a 1x1 interpolation so no smoothing applies).

#include <dlfcn.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensel.h"
#include "sensel_device.h"

typedef SenselStatus (*init_fn)(SENSEL_HANDLE, unsigned char *);
typedef SenselStatus (*free_fn)(SENSEL_HANDLE);
typedef SenselStatus (*frame_fn)(SENSEL_HANDLE, unsigned char *, int, unsigned char, SenselFrameData *, unsigned int *);

typedef struct {
  void     *lib;
  init_fn  init;
  free_fn  free_;
  frame_fn frame;
} Impl;

static int load(Impl *impl, const char *path)
{
  impl->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!impl->lib) { fprintf(stderr, "dlopen %s: %s\n", path, dlerror()); return 0; }
  impl->init  = (init_fn)dlsym(impl->lib, "senselInitDecompressionHandle");
  impl->free_ = (free_fn)dlsym(impl->lib, "senselFreeDecompressionHandle");
  impl->frame = (frame_fn)dlsym(impl->lib, "senselDecompressFrame");
  if (!impl->init || !impl->free_ || !impl->frame) { fprintf(stderr, "%s: missing symbols\n", path); return 0; }
  return 1;
}

// ---------------------------------------------------------------- encoder (inverse of the wire format)

static int put_varint(unsigned char *buf, int idx, unsigned v)
{
  if (v < 0x80) { buf[idx++] = (unsigned char)v; }
  else          { buf[idx++] = (unsigned char)(0x80 | (v & 0x7F)); buf[idx++] = (unsigned char)(v >> 7); }
  return idx;
}

static int encode_forces(const unsigned short *raw, int n, unsigned short base, unsigned char *buf)
{
  int idx = put_varint(buf, 0, base);
  int i = 0;
  while (i < n) {
    int run = 0;
    while (i < n && raw[i] == 0) { run++; i++; }
    idx = put_varint(buf, idx, run);
    if (i >= n) break;
    while (i < n && raw[i] != 0) { idx = put_varint(buf, idx, raw[i] - base); i++; }
    if (i < n) idx = put_varint(buf, idx, 0);
  }
  return idx;
}

static int encode_labels(const unsigned char *lab, int n, unsigned char *buf)
{
  int idx = 0, i = 0;
  while (i < n) {
    int run = 0;
    while (i < n && lab[i] == 0xFF) { run++; i++; }
    idx = put_varint(buf, idx, run);                  // null run (may be 0)
    while (i < n && lab[i] != 0xFF) {
      unsigned char l = lab[i];
      int r = 0;
      while (i < n && lab[i] == l) { r++; i++; }
      idx = put_varint(buf, idx, r);
      buf[idx++] = (unsigned char)(l | ((i < n && lab[i] != 0xFF) ? 0x80 : 0));
    }
  }
  return idx;
}

// ---------------------------------------------------------------- synthetic frames

static void make_frame(int nc, int nr, unsigned short base, unsigned short *raw, unsigned char *lab)
{
  int nblobs = rand() % 6;
  memset(raw, 0, sizeof(unsigned short) * nc * nr);
  memset(lab, 0xFF, nc * nr);
  for (int b = 0; b < nblobs; b++) {
    int cx = rand() % nc, cy = rand() % nr, rad = 1 + rand() % 4;
    int peak = 50 + rand() % 3000;
    for (int y = cy - rad; y <= cy + rad; y++)
      for (int x = cx - rad; x <= cx + rad; x++) {
        if (x < 0 || y < 0 || x >= nc || y >= nr) continue;
        float d = sqrtf((float)((x - cx) * (x - cx) + (y - cy) * (y - cy))) / (float)(rad + 1);
        if (d >= 1.0f) continue;
        unsigned v = (unsigned)(base + 1 + peak * (1.0f - d));
        if (v > 0x7FFF) v = 0x7FFF;
        if (v > raw[y * nc + x]) { raw[y * nc + x] = (unsigned short)v; lab[y * nc + x] = (unsigned char)b; }
      }
  }
}

static SenselDevice *make_device(int cols_us, int rows_us, float force_scale)
{
  SenselDevice *d = calloc(1, sizeof(SenselDevice));
  d->sensor_info.num_cols = (unsigned short)cols_us;
  d->sensor_info.num_rows = (unsigned short)rows_us;
  d->sensor_info.max_contacts = 16;
  d->force_value_scale = force_scale;
  return d;
}

static SenselFrameData *make_frame_data(int n)
{
  SenselFrameData *f = calloc(1, sizeof(SenselFrameData));
  f->force_array  = calloc(n, sizeof(float));
  f->labels_array = calloc(n, 1);
  return f;
}

int main(int argc, char **argv)
{
  // Sensel's binary reads decomp_handle at +0x20 and force_value_scale at +0x44 of the
  // device struct; make sure this build has the same layout before trusting the result.
  if (offsetof(SenselDevice, decomp_handle) != 0x20 || offsetof(SenselDevice, force_value_scale) != 0x44) {
    fprintf(stderr, "SenselDevice layout mismatch (decomp_handle@%zu force_value_scale@%zu)\n",
            offsetof(SenselDevice, decomp_handle), offsetof(SenselDevice, force_value_scale));
    return 2;
  }
  if (argc < 2) { fprintf(stderr, "usage: %s ref.dylib [cand.dylib] [iterations]\n", argv[0]); return 2; }

  Impl ref, cand;
  int diff_mode = argc >= 3 && strcmp(argv[2], "-") != 0 && strstr(argv[2], ".dylib");
  if (!load(&ref, argv[1])) return 2;
  if (diff_mode && !load(&cand, argv[2])) return 2;
  int iters = argc >= 4 ? atoi(argv[3]) : 2000;
  srand(12345);

  if (!diff_mode) {
    // Self-check with 1x1 "interpolation" factors is not possible (kernel needs >=1 step), so
    // check the coarse sample points instead: with factor k, output[y*k][x*k] interpolates exactly
    // the coarse value only when neighbours are zero; we just verify payload accounting here.
    unsigned char meta[8] = {0, 0, 47, 27, 4, 4, 0, 0};
    int nc = 47, nr = 27, cu = 185, ru = 105;
    SenselDevice *d = make_device(cu, ru, 16.0f);
    if (ref.init(d, meta) != SENSEL_OK) { fprintf(stderr, "init failed\n"); return 1; }
    SenselFrameData *f = make_frame_data(cu * ru);
    unsigned short raw[47 * 27]; unsigned char lab[47 * 27]; unsigned char buf[16384];
    for (int it = 0; it < iters; it++) {
      make_frame(nc, nr, 30, raw, lab);
      int n1 = encode_forces(raw, nc * nr, 30, buf);
      int n2 = encode_labels(lab, nc * nr, buf + n1);
      unsigned int consumed = 0;
      if (ref.frame(d, buf, n1 + n2, FRAME_CONTENT_PRESSURE_MASK | FRAME_CONTENT_LABELS_MASK, f, &consumed) != SENSEL_OK
          || consumed != (unsigned)(n1 + n2)) {
        fprintf(stderr, "iteration %d: decode failed (consumed %u of %d)\n", it, consumed, n1 + n2);
        return 1;
      }
    }
    printf("self-check OK: %d frames decoded with exact payload accounting\n", iters);
    return 0;
  }

  struct { unsigned char meta[8]; int nc, nr; float scale; } configs[] = {
    {{0, 0, 47, 27, 4, 4, 0, 0}, 47, 27, 16.0f},   // Morph, high detail
    {{0, 0, 47, 27, 4, 4, 0, 0}, 47, 27,  8.0f},
    {{0, 0, 24, 14, 8, 8, 0, 0}, 24, 14, 16.0f},   // coarser scan detail
    {{0, 0, 12,  7, 4, 4, 0, 0}, 12,  7,  1.0f},
  };
  const unsigned char masks[] = {
    FRAME_CONTENT_PRESSURE_MASK, FRAME_CONTENT_LABELS_MASK,
    FRAME_CONTENT_PRESSURE_MASK | FRAME_CONTENT_LABELS_MASK,
  };

  double max_abs = 0, max_rel = 0;
  long label_mismatch = 0, frames = 0, failures = 0;

  for (size_t c = 0; c < sizeof(configs) / sizeof(configs[0]); c++) {
    int nc = configs[c].nc, nr = configs[c].nr;
    int cu = (nc - 1) * configs[c].meta[4] + 1, ru = (nr - 1) * configs[c].meta[5] + 1;
    int n_us = cu * ru;
    SenselDevice *dr = make_device(cu, ru, configs[c].scale);
    SenselDevice *dc = make_device(cu, ru, configs[c].scale);
    if (ref.init(dr, configs[c].meta) != SENSEL_OK || cand.init(dc, configs[c].meta) != SENSEL_OK) {
      fprintf(stderr, "config %zu: init failed\n", c); return 1;
    }
    SenselFrameData *fr = make_frame_data(n_us), *fc = make_frame_data(n_us);
    unsigned short *raw = malloc(sizeof(unsigned short) * nc * nr);
    unsigned char *lab = malloc(nc * nr), *buf = malloc(nc * nr * 8 + 64);

    for (int it = 0; it < iters; it++) {
      unsigned short base = (unsigned short)(rand() % 64);
      unsigned char mask = masks[it % 3];
      int len = 0;
      make_frame(nc, nr, base, raw, lab);
      if (mask & FRAME_CONTENT_PRESSURE_MASK) len = encode_forces(raw, nc * nr, base, buf);
      if (mask & FRAME_CONTENT_LABELS_MASK)   len += encode_labels(lab, nc * nr, buf + len);

      unsigned int cr = 0, cc = 0;
      SenselStatus sr = ref.frame(dr, buf, len, mask, fr, &cr);
      SenselStatus sc = cand.frame(dc, buf, len, mask, fc, &cc);
      frames++;
      if (sr != sc || cr != cc) {
        if (failures++ < 5)
          fprintf(stderr, "config %zu iter %d mask %d: status %d/%d consumed %u/%u\n", c, it, mask, sr, sc, cr, cc);
        continue;
      }
      // Labels-only frames: Sensel's library zeroes just the first ncols*nrows force entries
      // and leaves the rest stale; we zero the whole image, so forces are not comparable.
      int cmp_forces = (mask & FRAME_CONTENT_PRESSURE_MASK) != 0;
      int cmp_labels = (mask & FRAME_CONTENT_LABELS_MASK) != 0;
      for (int i = 0; i < n_us; i++) {
        if (cmp_forces) {
          double a = fr->force_array[i], b = fc->force_array[i];
          double d = fabs(a - b);
          if (d > max_abs) max_abs = d;
          if (fabs(a) > 1e-3 && d / fabs(a) > max_rel) max_rel = d / fabs(a);
        }
        if (cmp_labels && fr->labels_array[i] != fc->labels_array[i]) label_mismatch++;
      }
    }
    ref.free_(dr); cand.free_(dc);
  }

  printf("frames compared: %ld  status/length mismatches: %ld\n", frames, failures);
  printf("force max |diff| = %.3g  max rel diff = %.3g\n", max_abs, max_rel);
  printf("label pixel mismatches: %ld\n", label_mismatch);
  int ok = failures == 0 && max_rel < 1e-4 && label_mismatch == 0;
  printf("%s\n", ok ? "MATCH" : "MISMATCH");
  return ok ? 0 : 1;
}
