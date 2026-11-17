// senseless-mpe — turn a Sensel Morph into an MPE controller on macOS
// Copyright (c) 2026 senseless contributors. MIT License.
//
// Reads contacts through LibSensel and publishes a CoreMIDI virtual source that speaks
// MPE (MIDI Polyphonic Expression, lower zone). Each finger gets its own member channel:
//   x  -> per-note pitch bend (relative to where the finger landed)
//   y  -> CC74 (timbre / "slide")
//   z  -> channel pressure
// The surface is laid out as an isomorphic grid (default: semitones across, fourths up),
// so it works with or without an overlay on the Morph.

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sensel.h"

#define MAX_CONTACTS 64
#define MAX_MEMBERS  15

typedef enum { PITCH_RELATIVE, PITCH_ABSOLUTE } PitchMode;
typedef enum { Y_CELL, Y_RELATIVE } YMode;

typedef struct {
  char      name[64];
  int       cols, rows;          // grid; 0 = derive from cell_mm
  float     cell_mm;
  int       base_note;           // bottom-left cell
  int       row_interval;        // semitones per row
  int       bend_range;          // member-channel pitch-bend range, semitones
  int       members;             // member channels (lower zone: 2 .. 1+members)
  float     pressure_max_g;
  float     pressure_curve;
  float     velocity_min_g;
  float     velocity_max_g;
  float     velocity_curve;
  int       velocity_frames;     // frames to wait for velocity before note-on
  float     smoothing;           // 0 = none, 0.9 = heavy
  PitchMode pitch_mode;
  YMode     y_mode;
  int       flip_y;
  int       send_mcm;
  int       frame_rate;
  int       verbose;
  int       demo;                // play a synthetic gesture instead of reading a device
} Config;

typedef struct {
  int      active;
  int      sounding;             // note-on sent
  int      pending_frames;
  unsigned long start_tick;
  int      channel;              // 0-based MIDI channel
  int      note;
  float    x0, y0;
  float    cell_x0, cell_y0;     // origin of the cell the note started in
  float    x, y, force;
  float    peak_force;
  int      last_bend, last_cc74, last_pressure;
} Voice;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static Config       cfg;
static MIDIClientRef g_client;
static MIDIEndpointRef g_source;
static Voice        voices[MAX_CONTACTS];
static int          channel_busy[16];
static unsigned long channel_released_at[16];
static unsigned long g_tick = 0;
static float        g_width = 230.0f, g_height = 130.0f, g_cell_w, g_cell_h;

// ---------------------------------------------------------------- MIDI output

typedef struct {
  Byte           storage[4096];
  MIDIEventList *list;
  MIDIEventPacket *packet;
} Batch;

static void batch_begin(Batch *b)
{
  b->list = (MIDIEventList *)b->storage;
  b->packet = MIDIEventListInit(b->list, kMIDIProtocol_1_0);
}

static void batch_flush(Batch *b)
{
  if (b->list->numPackets > 0)
    MIDIReceivedEventList(g_source, b->list);
  batch_begin(b);
}

static void send3(Batch *b, int status, int d1, int d2)
{
  UInt32 word = 0x20000000u | ((UInt32)(status & 0xFF) << 16) | ((UInt32)(d1 & 0x7F) << 8) | (UInt32)(d2 & 0x7F);
  MIDIEventPacket *p = MIDIEventListAdd(b->list, sizeof(b->storage), b->packet, 0, 1, &word);
  if (!p) {               // list full: flush and retry
    batch_flush(b);
    p = MIDIEventListAdd(b->list, sizeof(b->storage), b->packet, 0, 1, &word);
  }
  if (p) b->packet = p;
}

static void send2(Batch *b, int status, int d1) { send3(b, status, d1, 0); }

static void send_cc(Batch *b, int ch, int cc, int v)       { send3(b, 0xB0 | ch, cc, v); }
static void send_bend(Batch *b, int ch, int v)             { send3(b, 0xE0 | ch, v & 0x7F, (v >> 7) & 0x7F); }
static void send_pressure(Batch *b, int ch, int v)         { send2(b, 0xD0 | ch, v); }
static void send_note(Batch *b, int on, int ch, int n, int v) { send3(b, (on ? 0x90 : 0x80) | ch, n, v); }

static void send_rpn(Batch *b, int ch, int msb, int lsb, int value_msb, int value_lsb)
{
  send_cc(b, ch, 101, msb);
  send_cc(b, ch, 100, lsb);
  send_cc(b, ch, 6, value_msb);
  if (value_lsb >= 0) send_cc(b, ch, 38, value_lsb);
  send_cc(b, ch, 101, 127);
  send_cc(b, ch, 100, 127);
}

static void send_mpe_config(void)
{
  Batch b;
  batch_begin(&b);
  send_rpn(&b, 0, 0, 6, cfg.members, -1);          // MPE Configuration Message, lower zone
  send_rpn(&b, 0, 0, 0, 2, 0);                      // manager channel bend range: 2 semitones
  for (int ch = 1; ch <= cfg.members; ch++)
    send_rpn(&b, ch, 0, 0, cfg.bend_range, 0);      // member bend range
  batch_flush(&b);
}

static int midi_init(void)
{
  CFStringRef name = CFStringCreateWithCString(NULL, cfg.name, kCFStringEncodingUTF8);
  OSStatus st = MIDIClientCreate(name, NULL, NULL, &g_client);
  if (st == noErr)
    st = MIDISourceCreateWithProtocol(g_client, name, kMIDIProtocol_1_0, &g_source);
  if (st == noErr) {
    // A stable unique ID lets DAWs remember the port between runs.
    SInt32 uid = 0x53454E53;   // 'SENS'
    for (const char *p = cfg.name; *p; p++) uid = uid * 31 + *p;
    MIDIObjectSetIntegerProperty(g_source, kMIDIPropertyUniqueID, uid);
    MIDIObjectSetStringProperty(g_source, kMIDIPropertyManufacturer, CFSTR("senseless"));
  }
  CFRelease(name);
  if (st != noErr) {
    fprintf(stderr, "CoreMIDI setup failed (OSStatus %d)\n", (int)st);
    return 0;
  }
  return 1;
}

// ---------------------------------------------------------------- mapping

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static int curve127(float value, float max, float curve, int min_out)
{
  float n = clampf(value / max, 0.0f, 1.0f);
  int v = (int)lroundf(powf(n, curve) * 127.0f);
  return clampi(v, min_out, 127);
}

// Velocity is measured on a log scale: on a real Morph, strike onsets span roughly
// 5 g (feather tap) to 1.5 kg (hard strike), and a linear/power scale crushes the
// soft end. min_g maps to velocity 1, max_g to 127; curve shapes the log position.
static int velocity127(float grams)
{
  if (grams <= cfg.velocity_min_g) return 1;
  float n = logf(grams / cfg.velocity_min_g) / logf(cfg.velocity_max_g / cfg.velocity_min_g);
  n = clampf(n, 0.0f, 1.0f);
  return clampi((int)lroundf(powf(n, cfg.velocity_curve) * 127.0f), 1, 127);
}

// Grid row 0 is at the bottom of the surface (nearest the player).
static void cell_of(float x, float y, int *col, int *row)
{
  float yy = cfg.flip_y ? y : g_height - y;
  *col = clampi((int)floorf(x / g_cell_w), 0, cfg.cols - 1);
  *row = clampi((int)floorf(yy / g_cell_h), 0, cfg.rows - 1);
}

static int bend_for(const Voice *v)
{
  float semis;
  if (cfg.pitch_mode == PITCH_RELATIVE)
    semis = (v->x - v->x0) / g_cell_w;
  else
    semis = (v->x - (v->cell_x0 + 0.5f * g_cell_w)) / g_cell_w;
  return clampi((int)lroundf(8192.0f + semis / (float)cfg.bend_range * 8192.0f), 0, 16383);
}

static int cc74_for(const Voice *v)
{
  float yy  = cfg.flip_y ? v->y : g_height - v->y;
  float yy0 = cfg.flip_y ? v->y0 : g_height - v->y0;
  float n;
  if (cfg.y_mode == Y_CELL)
    n = (yy - v->cell_y0) / g_cell_h;
  else
    n = 0.5f + (yy - yy0) / g_cell_h;
  return clampi((int)lroundf(n * 127.0f), 0, 127);
}

static int pressure_for(const Voice *v) { return curve127(v->force, cfg.pressure_max_g, cfg.pressure_curve, 0); }

static int alloc_channel(void)
{
  int best = -1;
  unsigned long best_age = 0;
  for (int ch = 1; ch <= cfg.members; ch++) {
    if (channel_busy[ch]) continue;
    unsigned long age = g_tick - channel_released_at[ch];
    if (best < 0 || age > best_age) { best = ch; best_age = age; }
  }
  return best;   // -1 when every member channel is sounding
}

static void voice_off(Batch *b, Voice *v)
{
  if (v->sounding) {
    send_note(b, 0, v->channel, v->note, 64);
    if (cfg.verbose) printf("off ch%-2d note %3d\n", v->channel + 1, v->note);
  }
  if (v->channel > 0) {
    channel_busy[v->channel] = 0;
    channel_released_at[v->channel] = g_tick;
  }
  memset(v, 0, sizeof(*v));
}

static void voice_sound(Batch *b, Voice *v)
{
  int vel = velocity127(v->peak_force);
  v->last_bend = bend_for(v);
  v->last_cc74 = cc74_for(v);
  v->last_pressure = pressure_for(v);
  // MPE: per-note expression state goes out before the note-on.
  send_bend(b, v->channel, v->last_bend);
  send_cc(b, v->channel, 74, v->last_cc74);
  send_pressure(b, v->channel, v->last_pressure);
  send_note(b, 1, v->channel, v->note, vel);
  v->sounding = 1;
  if (cfg.verbose) printf("on  ch%-2d note %3d vel %3d\n", v->channel + 1, v->note, vel);
}

static void voice_update(Batch *b, Voice *v)
{
  int bend = bend_for(v), cc = cc74_for(v), pr = pressure_for(v);
  if (bend != v->last_bend) { send_bend(b, v->channel, bend); v->last_bend = bend; }
  if (cc != v->last_cc74)   { send_cc(b, v->channel, 74, cc); v->last_cc74 = cc; }
  if (pr != v->last_pressure) { send_pressure(b, v->channel, pr); v->last_pressure = pr; }
}

static void handle_contact(Batch *b, const SenselContact *c)
{
  Voice *v = &voices[c->id % MAX_CONTACTS];
  float a = cfg.smoothing;

  if (c->state == CONTACT_START) {
    int col, row, ch;
    if (v->active) voice_off(b, v);
    ch = alloc_channel();
    if (ch < 0) {
      // Steal the voice that has been sounding longest.
      Voice *oldest = NULL;
      for (int i = 0; i < MAX_CONTACTS; i++)
        if (voices[i].active && &voices[i] != v && (!oldest || voices[i].start_tick < oldest->start_tick))
          oldest = &voices[i];
      if (!oldest) return;
      voice_off(b, oldest);
      ch = alloc_channel();
      if (ch < 0) return;
    }
    cell_of(c->x_pos, c->y_pos, &col, &row);
    v->active = 1;
    v->start_tick = g_tick;
    v->channel = ch;
    channel_busy[ch] = 1;
    v->note = clampi(cfg.base_note + col + row * cfg.row_interval, 0, 127);
    v->x = v->x0 = c->x_pos;
    v->y = v->y0 = c->y_pos;
    v->cell_x0 = col * g_cell_w;
    v->cell_y0 = row * g_cell_h;
    v->force = v->peak_force = c->total_force;
    v->pending_frames = cfg.velocity_frames;
    if (v->pending_frames == 0) voice_sound(b, v);
    return;
  }

  if (!v->active) return;

  if (c->state == CONTACT_END) {
    voice_off(b, v);
    return;
  }

  // CONTACT_MOVE
  v->x = a * v->x + (1.0f - a) * c->x_pos;
  v->y = a * v->y + (1.0f - a) * c->y_pos;
  v->force = a * v->force + (1.0f - a) * c->total_force;
  if (!v->sounding) {
    if (c->total_force > v->peak_force) v->peak_force = c->total_force;
    if (--v->pending_frames <= 0) voice_sound(b, v);
    return;
  }
  voice_update(b, v);
}

static void all_off(void)
{
  Batch b;
  batch_begin(&b);
  for (int i = 0; i < MAX_CONTACTS; i++)
    if (voices[i].active) voice_off(&b, &voices[i]);
  batch_flush(&b);
}

// ---------------------------------------------------------------- device loop

static SENSEL_HANDLE open_morph(void)
{
  SenselDeviceList list;
  SENSEL_HANDLE h = NULL;
  memset(&list, 0, sizeof(list));
  senselGetDeviceList(&list);
  if (list.num_devices == 0) return NULL;
  if (senselOpenDeviceByID(&h, list.devices[0].idx) != SENSEL_OK) return NULL;
  return h;
}

static void layout_from_sensor(SENSEL_HANDLE h)
{
  SenselSensorInfo si;
  if (senselGetSensorInfo(h, &si) == SENSEL_OK && si.width > 0 && si.height > 0) {
    g_width = si.width;
    g_height = si.height;
  }
  if (cfg.cols <= 0) cfg.cols = (int)fmaxf(1.0f, floorf(g_width / cfg.cell_mm));
  if (cfg.rows <= 0) cfg.rows = (int)fmaxf(1.0f, floorf(g_height / cfg.cell_mm));
  g_cell_w = g_width / cfg.cols;
  g_cell_h = g_height / cfg.rows;
}

static void print_layout(void)
{
  static const char *names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
  printf("surface %.0f x %.0f mm -> %d x %d cells of %.1f x %.1f mm\n", g_width, g_height, cfg.cols,
         cfg.rows, g_cell_w, g_cell_h);
  for (int r = cfg.rows - 1; r >= 0; r--) {
    printf("  ");
    for (int c = 0; c < cfg.cols; c++) {
      int n = cfg.base_note + c + r * cfg.row_interval;
      printf("%-3s%-2d", names[n % 12], n / 12 - 1);
    }
    printf("\n");
  }
}

// Synthetic two-finger gesture for checking the MIDI/MPE setup without a Morph:
// finger A lands, slides up a semitone and swells; finger B adds a note with a CC74 sweep.
static void run_demo(void)
{
  SenselContact c[2];
  g_width = 230.0f; g_height = 130.0f;
  if (cfg.cols <= 0) cfg.cols = (int)floorf(g_width / cfg.cell_mm);
  if (cfg.rows <= 0) cfg.rows = (int)floorf(g_height / cfg.cell_mm);
  g_cell_w = g_width / cfg.cols;
  g_cell_h = g_height / cfg.rows;
  print_layout();
  usleep(1500000);   // give MIDI monitors / DAWs a moment to connect before configuring MPE
  if (cfg.send_mcm) send_mpe_config();
  printf("demo: playing a synthetic gesture on \"%s\" (Ctrl-C to stop)\n", cfg.name);

  while (!g_stop) {
    for (int t = 0; t <= 200 && !g_stop; t++) {
      Batch b;
      int n = 0;
      float u = t / 200.0f;
      memset(c, 0, sizeof(c));
      c[n].id = 0;
      c[n].state = t == 0 ? CONTACT_START : t == 200 ? CONTACT_END : CONTACT_MOVE;
      c[n].x_pos = 2.5f * g_cell_w + u * g_cell_w;                 // +1 semitone over the gesture
      c[n].y_pos = g_height - 1.5f * g_cell_h;
      c[n].total_force = 50.0f + 500.0f * sinf(3.14159f * u);
      n++;
      if (t >= 50 && t <= 150) {
        float w = (t - 50) / 100.0f;
        c[n].id = 1;
        c[n].state = t == 50 ? CONTACT_START : t == 150 ? CONTACT_END : CONTACT_MOVE;
        c[n].x_pos = 6.5f * g_cell_w;
        c[n].y_pos = g_height - (2.0f + w * 0.98f) * g_cell_h;      // sweep CC74 up the cell
        c[n].total_force = 300.0f;
        n++;
      }
      g_tick++;
      batch_begin(&b);
      for (int i = 0; i < n; i++) handle_contact(&b, &c[i]);
      batch_flush(&b);
      usleep(10000);
    }
    usleep(500000);
  }
}

static void run(void)
{
  while (!g_stop) {
    SENSEL_HANDLE h = open_morph();
    SenselFrameData *frame = NULL;
    int errors = 0;

    if (!h) {
      static int warned = 0;
      if (!warned) { fprintf(stderr, "waiting for a Sensel Morph...\n"); warned = 1; }
      sleep(1);
      continue;
    }

    layout_from_sensor(h);
    print_layout();
    senselSetFrameContent(h, FRAME_CONTENT_CONTACTS_MASK);
    if (cfg.frame_rate > 0) senselSetMaxFrameRate(h, (unsigned short)cfg.frame_rate);
    senselAllocateFrameData(h, &frame);
    if (senselStartScanning(h) != SENSEL_OK) {
      fprintf(stderr, "failed to start scanning\n");
      senselClose(h);
      sleep(1);
      continue;
    }
    if (cfg.send_mcm) send_mpe_config();
    printf("MPE output on virtual MIDI source \"%s\" (lower zone, %d member channels, bend +/-%d)\n",
           cfg.name, cfg.members, cfg.bend_range);

    while (!g_stop) {
      unsigned int n = 0;
      if (senselReadSensor(h) != SENSEL_OK) {
        if (++errors > 20) { fprintf(stderr, "lost the device; reconnecting\n"); break; }
        continue;
      }
      errors = 0;
      senselGetNumAvailableFrames(h, &n);
      for (unsigned int f = 0; f < n; f++) {
        Batch b;
        if (senselGetFrame(h, frame) != SENSEL_OK) continue;
        g_tick++;
        batch_begin(&b);
        for (int i = 0; i < frame->n_contacts; i++) handle_contact(&b, &frame->contacts[i]);
        batch_flush(&b);
      }
    }

    all_off();
    senselStopScanning(h);
    senselFreeFrameData(h, frame);
    senselClose(h);
  }
}

// ---------------------------------------------------------------- CLI

static void usage(void)
{
  printf(
    "usage: senseless-mpe [options]\n\n"
    "  --name NAME            virtual MIDI source name (default \"Senseless Morph\")\n"
    "  --cols N --rows N      grid size (default: from --cell-mm)\n"
    "  --cell-mm MM           target cell size (default 19)\n"
    "  --base-note N          note of the bottom-left cell (default 36 = C2)\n"
    "  --row-interval N       semitones between rows (default 5 = fourths)\n"
    "  --bend-range N         member pitch-bend range in semitones (default 48)\n"
    "  --members N            member channels, 1-15 (default 15)\n"
    "  --pressure-max G       grams for full channel pressure (default 1000)\n"
    "  --pressure-curve X     exponent applied to pressure (default 0.7)\n"
    "  --velocity-min G       onset grams for velocity 1 (default 5)\n"
    "  --velocity-max G       onset grams for velocity 127 (default 1000)\n"
    "  --velocity-curve X     exponent on the log-scaled velocity (default 1)\n"
    "  --velocity-frames N    frames to sample before note-on (default 1, 0 = immediate)\n"
    "  --smoothing A          0..0.95 low-pass on x/y/force (default 0)\n"
    "  --pitch absolute       bend measured from the cell centre (default: relative to touch)\n"
    "  --y relative           CC74 relative to touch point (default: position within cell)\n"
    "  --flip-y               put row 0 at the other edge\n"
    "  --frame-rate HZ        request a scan rate from the device\n"
    "  --no-mcm               don't send the MPE configuration message on connect\n"
    "  --demo                 play a synthetic gesture (no device needed)\n"
    "  -v, --verbose          print note events\n");
}

int main(int argc, char **argv)
{
  static struct option opts[] = {
    {"name", required_argument, 0, 'n'}, {"cols", required_argument, 0, 'c'},
    {"rows", required_argument, 0, 'r'}, {"cell-mm", required_argument, 0, 'm'},
    {"base-note", required_argument, 0, 'b'}, {"row-interval", required_argument, 0, 'i'},
    {"bend-range", required_argument, 0, 'B'}, {"members", required_argument, 0, 'M'},
    {"pressure-max", required_argument, 0, 'p'}, {"pressure-curve", required_argument, 0, 'P'},
    {"velocity-min", required_argument, 0, 'g'}, {"velocity-max", required_argument, 0, 'e'}, {"velocity-curve", required_argument, 0, 'E'},
    {"velocity-frames", required_argument, 0, 'f'}, {"smoothing", required_argument, 0, 's'},
    {"pitch", required_argument, 0, 'x'}, {"y", required_argument, 0, 'y'},
    {"flip-y", no_argument, 0, 'F'}, {"frame-rate", required_argument, 0, 'R'},
    {"no-mcm", no_argument, 0, 'N'}, {"verbose", no_argument, 0, 'v'}, {"demo", no_argument, 0, 'D'}, {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}};
  int o;

  memset(&cfg, 0, sizeof(cfg));
  snprintf(cfg.name, sizeof(cfg.name), "Senseless Morph");
  cfg.cell_mm = 19.0f;
  cfg.base_note = 36;
  cfg.row_interval = 5;
  cfg.bend_range = 48;
  cfg.members = 15;
  cfg.pressure_max_g = 1000.0f;
  cfg.pressure_curve = 0.7f;
  cfg.velocity_min_g = 5.0f;
  cfg.velocity_max_g = 1000.0f;
  cfg.velocity_curve = 1.0f;
  cfg.velocity_frames = 1;
  cfg.send_mcm = 1;

  while ((o = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
    switch (o) {
      case 'n': snprintf(cfg.name, sizeof(cfg.name), "%s", optarg); break;
      case 'c': cfg.cols = atoi(optarg); break;
      case 'r': cfg.rows = atoi(optarg); break;
      case 'm': cfg.cell_mm = (float)atof(optarg); break;
      case 'b': cfg.base_note = atoi(optarg); break;
      case 'i': cfg.row_interval = atoi(optarg); break;
      case 'B': cfg.bend_range = clampi(atoi(optarg), 1, 96); break;
      case 'M': cfg.members = clampi(atoi(optarg), 1, MAX_MEMBERS); break;
      case 'p': cfg.pressure_max_g = (float)atof(optarg); break;
      case 'P': cfg.pressure_curve = (float)atof(optarg); break;
      case 'g': cfg.velocity_min_g = (float)atof(optarg); break;
      case 'e': cfg.velocity_max_g = (float)atof(optarg); break;
      case 'E': cfg.velocity_curve = (float)atof(optarg); break;
      case 'f': cfg.velocity_frames = clampi(atoi(optarg), 0, 20); break;
      case 's': cfg.smoothing = clampf((float)atof(optarg), 0.0f, 0.95f); break;
      case 'x': cfg.pitch_mode = strcmp(optarg, "absolute") == 0 ? PITCH_ABSOLUTE : PITCH_RELATIVE; break;
      case 'y': cfg.y_mode = strcmp(optarg, "relative") == 0 ? Y_RELATIVE : Y_CELL; break;
      case 'F': cfg.flip_y = 1; break;
      case 'R': cfg.frame_rate = atoi(optarg); break;
      case 'N': cfg.send_mcm = 0; break;
      case 'v': cfg.verbose = 1; break;
      case 'D': cfg.demo = 1; break;
      case 'h': usage(); return 0;
      default: usage(); return 2;
    }
  }
  if (cfg.cell_mm < 5.0f) cfg.cell_mm = 5.0f;
  if (cfg.pressure_max_g <= 0) cfg.pressure_max_g = 1000.0f;
  if (cfg.velocity_min_g <= 0) cfg.velocity_min_g = 5.0f;
  if (cfg.velocity_max_g <= cfg.velocity_min_g) cfg.velocity_max_g = 1000.0f;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  if (!midi_init()) return 1;
  if (cfg.demo) run_demo(); else run();
  all_off();
  MIDIEndpointDispose(g_source);
  MIDIClientDispose(g_client);
  return 0;
}
