/******************************************************************************************
* senseless — open reimplementation of Sensel's force-frame decompression
*
* Copyright (c) 2026 senseless contributors. MIT License (see LICENSE).
*
* Sensel shipped force-frame decompression only as a closed x86_64/x86 binary
* (libSenselDecompress). This file reimplements it from the documented behaviour of
* that binary so that LibSensel can be built natively for Apple Silicon with full
* force-image support. See docs/PROTOCOL.md for the wire format.
*
* Pipeline per frame:
*   1. Pressure payload: varint run-length code over the coarse sensor grid
*      (e.g. 47 x 27 on the Morph). First varint is a base offset; afterwards the
*      stream alternates "zero-run length" and a sequence of non-zero values
*      (value + base) terminated by a 0.  Values are divided by the device's
*      force_value_scale to give grams.
*   2. Labels payload: varint run lengths, alternating NULL-label runs and labelled
*      runs; each labelled run is followed by a label byte whose bit 7 means
*      "another labelled run follows".
*   3. Both coarse images are upsampled by the interpolation factors from the
*      COMPRESSION_METADATA register (4x4 on the Morph -> 185 x 105) with a separable
*      4-tap kernel whose coefficients are computed at init time.
******************************************************************************************/

#include <stdlib.h>
#include <string.h>

#include "sensel.h"
#include "sensel_device.h"
#include "sensel_decompress.h"

#define LABEL_NULL        0xFF
#define LABEL_THRESHOLD   (10.0f / 4095.0f)

typedef struct
{
  unsigned char metadata[6];   // version major/minor, active cols/rows, interp cols/rows

  int   ncols;                 // coarse grid
  int   nrows;
  int   ncols_us;              // upsampled grid
  int   nrows_us;
  int   col_factor;
  int   row_factor;

  float (*col_coeff)[4];       // [col_factor + 1][4]
  float (*row_coeff)[4];       // [row_factor + 1][4]

  float         *forces;       // coarse force image, ncols * nrows
  unsigned char *labels;       // coarse label image, ncols * nrows
  float         *scratch;      // (row_factor + 1) * (col_factor + 1), used by label upsampling
} SenselDecompressCtx;

////////////////////////////////////////////////////////////////////////////////
// Interpolation kernel

static float blob(float x)
{
  x *= x;
  if (x >= 1.0f)
    return 0.0f;
  x = 1.0f - x;
  return x * x;
}

static float triangle(float x)
{
  if (x < 0.0f)
    x = -x;
  if (x >= 1.0f)
    return 0.0f;
  return 1.0f - x;
}

// Numerically convolve a triangle (linear interp) basis with a smooth blob window.
static float integrate(float step, float val_x, float sample_x)
{
  const float window = 1.0f;
  const float scale  = 1.0f;
  float num = 0.0f;
  float den = 0.0f;
  float t;

  for (t = -window; t <= 1.0f + window; t += step)
  {
    float w = blob(window * (sample_x - t));
    num += triangle(scale * (val_x - t)) * w;
    den += w;
  }
  return num / den;
}

static void calcInterpCoeff(int steps, float (*coeff)[4])
{
  float inv = 1.0f / (float)(steps - 1);
  int i;

  for (i = 0; i < steps; i++)
  {
    float x = (float)i * inv;
    float sum;

    coeff[i][0] = integrate(0.001f, -1.0f, x);
    coeff[i][1] = integrate(0.001f,  0.0f, x);
    coeff[i][2] = integrate(0.001f,  1.0f, x);
    coeff[i][3] = integrate(0.001f,  2.0f, x);

    sum = coeff[i][0] + coeff[i][1] + coeff[i][2] + coeff[i][3];
    coeff[i][0] /= sum;
    coeff[i][1] /= sum;
    coeff[i][2] /= sum;
    coeff[i][3] /= sum;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Context management

static void ctxFreeBuffers(SenselDecompressCtx *ctx)
{
  free(ctx->col_coeff); ctx->col_coeff = NULL;
  free(ctx->row_coeff); ctx->row_coeff = NULL;
  free(ctx->forces);    ctx->forces    = NULL;
  free(ctx->labels);    ctx->labels    = NULL;
  free(ctx->scratch);   ctx->scratch   = NULL;
}

static SenselStatus ctxInit(SenselDevice *device, SenselDecompressCtx *ctx, const unsigned char *metadata)
{
  ctxFreeBuffers(ctx);
  memcpy(ctx->metadata, metadata, sizeof(ctx->metadata));

  ctx->ncols      = metadata[2];
  ctx->nrows      = metadata[3];
  ctx->col_factor = metadata[4];
  ctx->row_factor = metadata[5];

  // upsamplePressure() keeps per-column scratch on the stack, sized for factors < 64.
  if (ctx->ncols < 2 || ctx->nrows < 2 ||
      ctx->col_factor < 1 || ctx->row_factor < 1 || ctx->col_factor > 63 || ctx->row_factor > 63)
    return SENSEL_ERROR;

  ctx->ncols_us = (ctx->ncols - 1) * ctx->col_factor + 1;
  ctx->nrows_us = (ctx->nrows - 1) * ctx->row_factor + 1;

  // The upsampled image is written into SenselFrameData buffers that LibSensel sizes from
  // the sensor's reported row/column count; refuse metadata that would overflow them.
  if (device && (ctx->ncols_us * ctx->nrows_us >
                 (int)device->sensor_info.num_cols * (int)device->sensor_info.num_rows))
    return SENSEL_ERROR;

  ctx->col_coeff = malloc(sizeof(float[4]) * (ctx->col_factor + 1));
  ctx->row_coeff = malloc(sizeof(float[4]) * (ctx->row_factor + 1));
  ctx->forces    = calloc((size_t)ctx->ncols * ctx->nrows, sizeof(float));
  ctx->labels    = malloc((size_t)ctx->ncols * ctx->nrows);
  ctx->scratch   = malloc(sizeof(float) * (ctx->col_factor + 1) * (ctx->row_factor + 1));

  if (!ctx->col_coeff || !ctx->row_coeff || !ctx->forces || !ctx->labels || !ctx->scratch)
  {
    ctxFreeBuffers(ctx);
    return SENSEL_ERROR;
  }
  memset(ctx->labels, LABEL_NULL, (size_t)ctx->ncols * ctx->nrows);

  calcInterpCoeff(ctx->col_factor + 1, ctx->col_coeff);
  calcInterpCoeff(ctx->row_factor + 1, ctx->row_coeff);
  return SENSEL_OK;
}

////////////////////////////////////////////////////////////////////////////////
// Payload decoders

static int readVarint(const unsigned char *buf, int size, int *idx, unsigned short *out)
{
  unsigned short v;

  if (*idx >= size)
    return 0;
  v = buf[(*idx)++];
  if (v & 0x80)
  {
    if (*idx >= size)
      return 0;
    v = (unsigned short)((v & 0x7F) | ((unsigned short)buf[(*idx)++] << 7));
  }
  *out = v;
  return 1;
}

static int decodeForces(const unsigned char *buf, int size, float force_scale,
                        float *out, int total, int *bytes_read)
{
  int            idx       = 0;
  int            n         = 0;
  int            zero_run  = 1;
  unsigned short base      = 0;
  unsigned short v;

  if (idx < size && !readVarint(buf, size, &idx, &base))
    return 0;

  if (force_scale == 0.0f)
    force_scale = 1.0f;

  while (n < total)
  {
    if (!readVarint(buf, size, &idx, &v))
      return 0;

    if (zero_run)
    {
      if (n + v > total)
        return 0;
      memset(&out[n], 0, sizeof(float) * v);
      n += v;
      zero_run = 0;
    }
    else if (v > 0)
    {
      out[n++] = (float)(v + base) / force_scale;
    }
    else
    {
      zero_run = 1;
    }
  }

  *bytes_read = idx;
  return 1;
}

static int decodeLabels(const unsigned char *buf, int size, unsigned char *out, int total, int *bytes_read)
{
  int            idx       = 0;
  int            n         = 0;
  int            null_run  = 1;
  unsigned short run;
  unsigned char  label;

  while (n < total)
  {
    if (!readVarint(buf, size, &idx, &run))
      return 0;

    if (null_run)
    {
      label    = LABEL_NULL;
      null_run = 0;
    }
    else
    {
      unsigned char b;
      if (idx >= size)
        return 0;
      b     = buf[idx++];
      label = b & 0x7F;
      if (!(b & 0x80))
        null_run = 1;
    }

    if (n + run > total)
      return 0;
    memset(&out[n], label, run);
    n += run;
  }

  *bytes_read = idx;
  return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Upsampling

static void upsampleColumn(const SenselDecompressCtx *ctx, const float *pm, int x, int y,
                           int first, int last, int uy_iters, float *col)
{
  const int nc = ctx->ncols;
  float v0 = first ? 0.0f : pm[(y - 1) * nc + x];
  float v1 = pm[y * nc + x];
  float v2 = pm[(y + 1) * nc + x];
  float v3 = last  ? 0.0f : pm[(y + 2) * nc + x];
  int i;

  for (i = 0; i < uy_iters; i++)
    col[i] = v0 * ctx->row_coeff[i][0] + v1 * ctx->row_coeff[i][1] +
             v2 * ctx->row_coeff[i][2] + v3 * ctx->row_coeff[i][3];
}

static void upsamplePressure(const SenselDecompressCtx *ctx, const float *pm, float *out)
{
  const int nc    = ctx->ncols;
  const int nr    = ctx->nrows;
  const int nc_us = ctx->ncols_us;
  const int cf    = ctx->col_factor;
  const int rf    = ctx->row_factor;
  const int total = ctx->ncols_us * ctx->nrows_us;
  float     cols[4][64];
  float     *c0 = cols[0], *c1 = cols[1], *c2 = cols[2], *c3 = cols[3];
  int       i, j, k, l;

  for (i = 0; i < nr - 1; i++)
  {
    int first    = (i == 0);
    int last     = (i == nr - 2);
    int uy_iters = last ? rf + 1 : rf;

    memset(c0, 0, sizeof(float) * uy_iters);
    memset(c1, 0, sizeof(float) * uy_iters);
    upsampleColumn(ctx, pm, 0, i, first, last, uy_iters, c2);
    upsampleColumn(ctx, pm, 1, i, first, last, uy_iters, c3);

    for (j = 0; j < nc - 1; j++)
    {
      int   last_col = (j == nc - 2);
      int   ux_iters = last_col ? cf + 1 : cf;
      float *tmp     = c0;
      int   base;

      c0 = c1; c1 = c2; c2 = c3; c3 = tmp;
      if (last_col)
        memset(c3, 0, sizeof(float) * uy_iters);
      else
        upsampleColumn(ctx, pm, j + 2, i, first, last, uy_iters, c3);

      base = (i * rf) * nc_us + j * cf;
      for (k = 0; k < uy_iters; k++)
        for (l = 0; l < ux_iters; l++)
          out[base + k * nc_us + l] = c0[k] * ctx->col_coeff[l][0] + c1[k] * ctx->col_coeff[l][1] +
                                      c2[k] * ctx->col_coeff[l][2] + c3[k] * ctx->col_coeff[l][3];
    }
  }

  for (i = 0; i < total; i++)
    if (out[i] < 0.0f)
      out[i] = 0.0f;
}

static void upsampleLabels(const SenselDecompressCtx *ctx, const float *pm, const unsigned char *labels,
                           unsigned char *labels_us)
{
  const int nc    = ctx->ncols;
  const int nr    = ctx->nrows;
  const int nc_us = ctx->ncols_us;
  const int cf    = ctx->col_factor;
  const int rf    = ctx->row_factor;
  const int ux_n  = cf + 1;
  float     *best = ctx->scratch;
  int       y, x;

  memset(labels_us, LABEL_NULL, (size_t)ctx->ncols_us * ctx->nrows_us);

  for (y = 0; y < nr - 1; y++)
  {
    for (x = 0; x < nc - 1; x++)
    {
      float         vals[4][4];
      unsigned char labs[4][4];
      unsigned char uniq[16];
      int           n_uniq = 0;
      int           uy_iters, ux_iters, base, r, c, u, a, b;

      for (r = 0; r < 4; r++)
      {
        for (c = 0; c < 4; c++)
        {
          int yy = y + r - 1;
          int xx = x + c - 1;
          unsigned char lab;

          if (xx < 0 || xx > nc - 1 || yy < 0 || yy > nr - 1)
          {
            vals[r][c] = 0.0f;
            labs[r][c] = LABEL_NULL;
            continue;
          }
          vals[r][c] = pm[yy * nc + xx];
          lab = labs[r][c] = labels[yy * nc + xx];
          if (lab == LABEL_NULL)
            continue;
          for (u = 0; u < n_uniq; u++)
            if (uniq[u] == lab)
              break;
          if (u == n_uniq)
            uniq[n_uniq++] = lab;
        }
      }

      if (n_uniq == 0)
        continue;

      uy_iters = (y != nr - 2) ? rf : rf + 1;
      ux_iters = (x != nc - 2) ? cf : cf + 1;
      for (r = 0; r < uy_iters; r++)
        for (c = 0; c < ux_iters; c++)
          best[r * ux_n + c] = 0.0f;

      base = (y * rf) * nc_us + x * cf;
      for (u = 0; u < n_uniq; u++)
      {
        unsigned char lab = uniq[u];
        float masked[4][4];

        for (a = 0; a < 4; a++)
          for (b = 0; b < 4; b++)
            masked[a][b] = (labs[a][b] == lab) ? vals[a][b] : 0.0f;

        for (r = 0; r < uy_iters; r++)
        {
          float row[4];
          for (b = 0; b < 4; b++)
            row[b] = masked[0][b] * ctx->row_coeff[r][0] + masked[1][b] * ctx->row_coeff[r][1] +
                     masked[2][b] * ctx->row_coeff[r][2] + masked[3][b] * ctx->row_coeff[r][3];

          for (c = 0; c < ux_iters; c++)
          {
            float v = row[0] * ctx->col_coeff[c][0] + row[1] * ctx->col_coeff[c][1] +
                      row[2] * ctx->col_coeff[c][2] + row[3] * ctx->col_coeff[c][3];
            if (v >= LABEL_THRESHOLD && v > best[r * ux_n + c])
            {
              best[r * ux_n + c] = v;
              labels_us[base + r * nc_us + c] = lab;
            }
          }
        }
      }
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// Public API (ABI-compatible with Sensel's libSenselDecompress)

SENSEL_API
SenselStatus WINAPI senselInitDecompressionHandle(SENSEL_HANDLE handle, unsigned char *data)
{
  SenselDevice        *device = (SenselDevice *)handle;
  SenselDecompressCtx *ctx;
  SenselStatus        status;

  if (!device || !data)
    return SENSEL_ERROR;

  if (device->decomp_handle)
    senselFreeDecompressionHandle(handle);

  ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return SENSEL_ERROR;

  status = ctxInit(device, ctx, data);
  if (status != SENSEL_OK)
  {
    ctxFreeBuffers(ctx);
    free(ctx);
    return status;
  }

  device->decomp_handle = ctx;
  return SENSEL_OK;
}

SENSEL_API
SenselStatus WINAPI senselFreeDecompressionHandle(SENSEL_HANDLE handle)
{
  SenselDevice        *device = (SenselDevice *)handle;
  SenselDecompressCtx *ctx;

  if (!device)
    return SENSEL_ERROR;

  ctx = (SenselDecompressCtx *)device->decomp_handle;
  if (ctx)
  {
    ctxFreeBuffers(ctx);
    free(ctx);
  }
  device->decomp_handle = NULL;
  return SENSEL_OK;
}

SENSEL_API
SenselStatus WINAPI senselDecompressionTriggerDetailChange(SENSEL_HANDLE handle, unsigned char *data)
{
  SenselDevice *device = (SenselDevice *)handle;

  if (!device || !device->decomp_handle || !data)
    return SENSEL_ERROR;

  return ctxInit(device, (SenselDecompressCtx *)device->decomp_handle, data);
}

SENSEL_API
SenselStatus WINAPI senselDecompressFrame(SENSEL_HANDLE handle, unsigned char *frame_data, int data_size,
                                          unsigned char content_mask, SenselFrameData *data,
                                          unsigned int *decompress_bytes_read)
{
  SenselDevice        *device = (SenselDevice *)handle;
  SenselDecompressCtx *ctx;
  unsigned char       *ptr       = frame_data;
  int                 remaining  = data_size;
  int                 coarse     = 0;
  int                 out_total;
  int                 consumed;

  if (!device || !device->decomp_handle || !data || !decompress_bytes_read)
    return SENSEL_ERROR;

  ctx       = (SenselDecompressCtx *)device->decomp_handle;
  coarse    = ctx->ncols * ctx->nrows;
  out_total = (int)device->sensor_info.num_rows * (int)device->sensor_info.num_cols;
  *decompress_bytes_read = 0;

  if (content_mask & FRAME_CONTENT_PRESSURE_MASK)
  {
    consumed = 0;
    if (!decodeForces(ptr, remaining, device->force_value_scale, ctx->forces, coarse, &consumed))
      return SENSEL_ERROR;
    ptr       += consumed;
    remaining -= consumed;
    *decompress_bytes_read = consumed;

    if (data->force_array)
      upsamplePressure(ctx, ctx->forces, data->force_array);
  }
  else
  {
    // NOTE: ctx->forces is intentionally left as-is: like Sensel's library, a labels-only
    // frame is upsampled using the most recent coarse pressure image.
    if (data->force_array)
      memset(data->force_array, 0, sizeof(float) * out_total);
  }

  if (content_mask & FRAME_CONTENT_LABELS_MASK)
  {
    consumed = 0;
    if (!decodeLabels(ptr, remaining, ctx->labels, coarse, &consumed))
      return SENSEL_ERROR;
    *decompress_bytes_read += consumed;

    if (data->labels_array)
      upsampleLabels(ctx, ctx->forces, ctx->labels, data->labels_array);
  }
  else if (data->labels_array)
  {
    memset(data->labels_array, LABEL_NULL, out_total);
  }

  return SENSEL_OK;
}
