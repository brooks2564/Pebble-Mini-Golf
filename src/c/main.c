#include <pebble.h>

// ---- Constants ----
#define FP              16
#define BALL_R           3
#define CUP_R            5
#define ARROW_LEN       16
#define MAX_STROKES      8
#define TIMER_MS        50    // ball tick interval ms
#define ANIM_MS        100    // animation/obstacle tick ms
#define INTRO_MS      1500    // hole intro display ms
#define MAX_WALLS       12
#define STOP_THRESH      6
#define ANGLE_STEP      10
#define POWER_STEP       5
#ifdef PBL_ROUND
  #define PW 127
  #define PH 130
#else
  #define PW 130
  #define PH 138
#endif
#define HUD_H           16

// Persistent storage keys
#define PKEY_BEST_18     1
#define PKEY_BEST_9      2
#define PKEY_SAVE_EXISTS 10
#define PKEY_SAVE_HOLE   11
#define PKEY_SAVE_SCORES 12

// ---- Types ----
typedef enum {
  STATE_MENU,
  STATE_HOLE_INTRO,
  STATE_AIM,
  STATE_POWER,
  STATE_ROLLING,
  STATE_HOLE_OUT,
  STATE_SCORECARD,
} GameState;

typedef struct {
  GPoint tee;
  GPoint cup;
  uint8_t par;
  uint8_t num_walls;
  GPoint walls[MAX_WALLS][2];
} HoleData;

// ---- Hole Definitions ----
// Hole 8 (index 7) has 1 static wall; the horizontal moving wall is dynamic.
static const HoleData s_holes[18] = {
  { {65,120}, {65,18}, 2, 0, {} },

  { {65,120}, {65,18}, 2, 1,
    { {{30,72},{100,72}} } },

  { {65,120}, {65,18}, 2, 2,
    { {{4,80},{50,80}}, {{80,80},{126,80}} } },

  { {20,120}, {110,18}, 3, 1,
    { {{65,20},{65,95}} } },

  { {65,120}, {65,18}, 2, 2,
    { {{4,90},{70,90}}, {{60,50},{126,50}} } },

  { {65,120}, {65,18}, 3, 3,
    { {{4,104},{60,104}}, {{70,72},{126,72}}, {{4,40},{60,40}} } },

  { {110,120}, {20,18}, 3, 1,
    { {{65,130},{65,45}} } },

  // Hole 8: one static vertical wall; moving horizontal added dynamically
  { {65,120}, {65,18}, 3, 1,
    { {{65,90},{65,54}} } },

  { {20,120}, {110,18}, 2, 1,
    { {{20,95},{110,45}} } },

  { {65,120}, {65,18}, 3, 2,
    { {{15,100},{75,60}}, {{55,75},{115,35}} } },

  { {65,120}, {65,18}, 3, 4,
    { {{45,50},{85,50}}, {{85,50},{85,90}},
      {{85,90},{45,90}}, {{45,90},{45,50}} } },

  { {20,120}, {110,60}, 2, 1,
    { {{65,30},{65,100}} } },

  { {20,120}, {110,18}, 3, 2,
    { {{4,90},{80,90}}, {{50,50},{126,50}} } },

  { {65,120}, {65,18}, 3, 3,
    { {{4,100},{65,100}}, {{65,70},{126,70}}, {{4,40},{65,40}} } },

  { {20,120}, {20,18}, 2, 1,
    { {{4,70},{90,70}} } },

  { {65,120}, {65,18}, 3, 2,
    { {{20,88},{50,54}}, {{80,88},{110,54}} } },

  { {20,120}, {110,18}, 3, 4,
    { {{4,104},{60,104}}, {{70,80},{126,80}},
      {{4,60},{80,60}},  {{40,40},{126,40}} } },

  // Hole 18: S-curve grand finale — 3 walls with alternating gaps, par 3
  // Path: right through gap@y=90 → left through gap@y=60 → right through gap@y=30 → cup
  { {15,120}, {115,18}, 3, 3,
    { {{4,90},{85,90}},
      {{45,60},{126,60}},
      {{4,30},{85,30}} } },
};

// Forward declaration
static void start_hint(const char *line1, const char *line2);

// Hint animation constants
#define HINT_MS         120
#define HINT_FADE_STEPS 3
#define HINT_HOLD_STEPS 18
#define HINT_TOTAL      (HINT_FADE_STEPS + HINT_HOLD_STEPS + HINT_FADE_STEPS)

// ---- Globals ----
static Window    *s_window;
static Layer     *s_layer;
static GBitmap   *s_gball_bmp;
static AppTimer  *s_ball_timer;
static AppTimer  *s_anim_timer;
static AppTimer  *s_intro_timer;
static AppTimer  *s_hint_timer;
static int8_t     s_hint_frame;
static char       s_hint_line1[24];
static char       s_hint_line2[24];
static GameState  s_state;
static bool       s_random_mode;
static int        s_current_hole;
static int        s_total_holes;
static int8_t     s_scores[18];
static int        s_strokes;

// Ball physics (fixed-point)
static int32_t s_bx, s_by, s_vx, s_vy;

// Aim/power
static int s_angle, s_power;

// Animation frame (drives moving obstacle on hole 8)
static int s_anim_frame;

// Procedural holes
static HoleData s_proc_holes[9];

static const HoleData *s_cur_hole;
static int s_px, s_py;

// Persistent
static int  s_best_18, s_best_9;
static bool s_has_save;

// ---- Haptic patterns ----
static const uint32_t s_bounce_segs[] = { 20 };
static const VibePattern s_bounce_pat = {
  .durations    = s_bounce_segs,
  .num_segments = ARRAY_LENGTH(s_bounce_segs),
};

// ---- Helpers ----
static GPoint to_screen(GPoint p) {
  return GPoint(p.x + s_px, p.y + s_py);
}
static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

static int running_vs_par(void) {
  int total = 0;
  for (int i = 0; i < s_current_hole; i++) {
    const HoleData *h = s_random_mode ? &s_proc_holes[i] : &s_holes[i];
    if (s_scores[i] >= 0) total += s_scores[i] - h->par;
  }
  return total;
}

// Moving obstacle on hole 8 (index 7)
static void get_hole8_wall(GPoint *w1, GPoint *w2) {
  int period = 80;           // 80 × 100ms = 8-second full cycle
  int phase  = s_anim_frame % period;
  int travel = (phase < period / 2) ? phase : (period - phase);
  int wy = 48 + travel;      // oscillates y=48 to y=88
  *w1 = GPoint(18, wy);
  *w2 = GPoint(112, wy);
}

// ---- Persistent Storage ----
static void load_persistent_data(void) {
  s_best_18  = persist_read_int(PKEY_BEST_18);
  s_best_9   = persist_read_int(PKEY_BEST_9);
  s_has_save = persist_read_bool(PKEY_SAVE_EXISTS);
}

static void save_game_progress(void) {
  if (s_random_mode) return;
  persist_write_bool(PKEY_SAVE_EXISTS, true);
  persist_write_int(PKEY_SAVE_HOLE, s_current_hole);
  persist_write_data(PKEY_SAVE_SCORES, s_scores, sizeof(s_scores));
  s_has_save = true;
}

static void clear_saved_game(void) {
  persist_delete(PKEY_SAVE_EXISTS);
  persist_delete(PKEY_SAVE_HOLE);
  persist_delete(PKEY_SAVE_SCORES);
  s_has_save = false;
}

static void check_and_save_best(void) {
  int total = 0;
  for (int i = 0; i < s_total_holes; i++)
    if (s_scores[i] >= 0) total += s_scores[i];
  if (s_random_mode) {
    if (s_best_9 == 0 || total < s_best_9) {
      s_best_9 = total;
      persist_write_int(PKEY_BEST_9, total);
    }
  } else {
    if (s_best_18 == 0 || total < s_best_18) {
      s_best_18 = total;
      persist_write_int(PKEY_BEST_18, total);
    }
    clear_saved_game();
  }
}

// ---- Procedural Generation ----
static uint32_t s_rand_seed;

static uint32_t next_rand(void) {
  s_rand_seed = s_rand_seed * 1664525u + 1013904223u;
  return s_rand_seed;
}
static int rand_range(int lo, int hi) {
  int range = hi - lo + 1;
  if (range <= 0) return lo;
  return lo + (int)(next_rand() % (uint32_t)range);
}

static void generate_proc_holes(void) {
  s_rand_seed = (uint32_t)time(NULL);
  for (int i = 0; i < 9; i++) {
    HoleData *h = &s_proc_holes[i];
    h->tee.x = (int16_t)rand_range(10, 50);
    h->tee.y = (int16_t)rand_range(10, 50);
    h->cup.x = (int16_t)rand_range(75, 120);
    h->cup.y = (int16_t)rand_range(85, PH - 10);
    int dx = h->cup.x - h->tee.x, dy = h->cup.y - h->tee.y;
    if (dx*dx + dy*dy < 4000) { h->cup.x = 110; h->cup.y = 120; }
    int nw = rand_range(1, 3);
    h->num_walls = (uint8_t)nw;
    h->par = (uint8_t)((dx*dx + dy*dy > 6000 || nw >= 3) ? 3 : 2);
    for (int w = 0; w < nw; w++) {
      int cx = rand_range(20, 110), cy = rand_range(25, 115);
      int len = rand_range(20, 45);
      if (next_rand() & 1) {
        h->walls[w][0] = GPoint(cx - len/2, cy);
        h->walls[w][1] = GPoint(cx + len/2, cy);
      } else {
        h->walls[w][0] = GPoint(cx, cy - len/2);
        h->walls[w][1] = GPoint(cx, cy + len/2);
      }
    }
    for (int w = nw; w < MAX_WALLS; w++) {
      h->walls[w][0] = h->walls[w][1] = GPointZero;
    }
  }
}

// ---- Wall Collision ----
static bool segments_cross(int ax, int ay, int bx, int by,
                            int cx, int cy, int dx, int dy) {
  int d1x = bx-ax, d1y = by-ay, d2x = dx-cx, d2y = dy-cy;
  int cross = d1x*d2y - d1y*d2x;
  if (cross == 0) return false;
  int ex = cx-ax, ey = cy-ay;
  int t_num = ex*d2y - ey*d2x;
  int u_num = ex*d1y - ey*d1x;
  if (cross > 0) {
    if (t_num < 0 || t_num > cross) return false;
    if (u_num < 0 || u_num > cross) return false;
  } else {
    if (t_num > 0 || t_num < cross) return false;
    if (u_num > 0 || u_num < cross) return false;
  }
  return true;
}

static void reflect_off_wall(GPoint w1, GPoint w2) {
  int wx = w2.x - w1.x, wy = w2.y - w1.y;
  int nx = -wy, ny = wx;
  int len_sq = nx*nx + ny*ny;
  if (len_sq == 0) return;
  int32_t dot = (int32_t)s_vx * nx + (int32_t)s_vy * ny;
  s_vx -= (int32_t)2 * dot * nx / len_sq;
  s_vy -= (int32_t)2 * dot * ny / len_sq;
  s_vx = s_vx * 7 / 8;
  s_vy = s_vy * 7 / 8;
  // Brief haptic tap on wall bounce
  vibes_enqueue_custom_pattern(s_bounce_pat);
}

static const GPoint s_boundary[4][2] = {
  {{0,0},{PW,0}}, {{PW,0},{PW,PH}},
  {{PW,PH},{0,PH}}, {{0,PH},{0,0}},
};

static void check_collisions(int ox, int oy, int nx, int ny) {
  for (int i = 0; i < 4; i++) {
    if (segments_cross(ox, oy, nx, ny,
                       s_boundary[i][0].x, s_boundary[i][0].y,
                       s_boundary[i][1].x, s_boundary[i][1].y)) {
      reflect_off_wall(s_boundary[i][0], s_boundary[i][1]);
      return;
    }
  }
  for (int i = 0; i < s_cur_hole->num_walls; i++) {
    GPoint w1 = s_cur_hole->walls[i][0], w2 = s_cur_hole->walls[i][1];
    if (segments_cross(ox, oy, nx, ny, w1.x, w1.y, w2.x, w2.y)) {
      reflect_off_wall(w1, w2);
      return;
    }
  }
  // Hole 8 moving obstacle
  if (s_current_hole == 7) {
    GPoint mw1, mw2;
    get_hole8_wall(&mw1, &mw2);
    if (segments_cross(ox, oy, nx, ny, mw1.x, mw1.y, mw2.x, mw2.y)) {
      reflect_off_wall(mw1, mw2);
    }
  }
}

// ---- Timer callbacks ----
static void anim_tick(void *context) {
  s_anim_frame++;
  if (s_current_hole == 7) layer_mark_dirty(s_layer);
  s_anim_timer = app_timer_register(ANIM_MS, anim_tick, NULL);
}

static void intro_timer_cb(void *context) {
  s_intro_timer = NULL;
  s_state = STATE_AIM;
  start_hint("UP/DN: rotate aim", "SEL: lock direction");
  layer_mark_dirty(s_layer);
}

static void ball_tick(void *context) {
  if (s_state != STATE_ROLLING) return;

  int ox = (int)(s_bx / FP), oy = (int)(s_by / FP);
  int nx = (int)((s_bx + s_vx) / FP), ny = (int)((s_by + s_vy) / FP);

  check_collisions(ox, oy, nx, ny);

  s_bx += s_vx;
  s_by += s_vy;

  if (s_bx < (int32_t)BALL_R * FP) {
    s_bx = (int32_t)BALL_R * FP; s_vx = iabs32(s_vx) * 7 / 8;
  } else if (s_bx > (int32_t)(PW - BALL_R) * FP) {
    s_bx = (int32_t)(PW - BALL_R) * FP; s_vx = -iabs32(s_vx) * 7 / 8;
  }
  if (s_by < (int32_t)BALL_R * FP) {
    s_by = (int32_t)BALL_R * FP; s_vy = iabs32(s_vy) * 7 / 8;
  } else if (s_by > (int32_t)(PH - BALL_R) * FP) {
    s_by = (int32_t)(PH - BALL_R) * FP; s_vy = -iabs32(s_vy) * 7 / 8;
  }

  s_vx = s_vx * 15 / 16;
  s_vy = s_vy * 15 / 16;

  // Cup check
  int bpx = (int)(s_bx / FP), bpy = (int)(s_by / FP);
  int cdx = bpx - s_cur_hole->cup.x, cdy = bpy - s_cur_hole->cup.y;
  if (cdx*cdx + cdy*cdy <= CUP_R*CUP_R) {
    int32_t speed = iabs32(s_vx) + iabs32(s_vy);
    if (speed > 96) {
      // Lip out — too fast, ball hits back of cup and bounces out
      int nx = cdx, ny = cdy;
      int len_sq = nx*nx + ny*ny;
      if (len_sq == 0) { nx = 1; ny = 0; len_sq = 1; }
      int32_t dot = (int32_t)s_vx * nx + (int32_t)s_vy * ny;
      s_vx -= (int32_t)2 * dot * nx / len_sq;
      s_vy -= (int32_t)2 * dot * ny / len_sq;
      // Energy absorbed by rim (~40% loss)
      s_vx = s_vx * 6 / 10;
      s_vy = s_vy * 6 / 10;
      // Nudge ball outside cup so it doesn't re-trigger immediately
      s_bx += (int32_t)nx * FP * 2;
      s_by += (int32_t)ny * FP * 2;
      vibes_enqueue_custom_pattern(s_bounce_pat);
    } else {
      // Ball drops in
      s_scores[s_current_hole] = (int8_t)s_strokes;
      s_state = STATE_HOLE_OUT;
      vibes_long_pulse();
      layer_mark_dirty(s_layer);
      return;
    }
  }

  // Stop check
  int32_t speed = iabs32(s_vx) + iabs32(s_vy);
  if (speed < STOP_THRESH) {
    s_vx = s_vy = 0;
    if (s_strokes >= MAX_STROKES) {
      s_scores[s_current_hole] = MAX_STROKES;
      s_state = STATE_HOLE_OUT;
      vibes_double_pulse();
    } else {
      s_state = STATE_AIM;
      start_hint("UP/DN: rotate aim", "SEL: lock direction");
    }
    layer_mark_dirty(s_layer);
    return;
  }

  layer_mark_dirty(s_layer);
  s_ball_timer = app_timer_register(TIMER_MS, ball_tick, NULL);
}

// ---- Hint animation ----

static void hint_tick(void *context) {
  if (s_hint_frame < HINT_TOTAL) {
    s_hint_frame++;
    layer_mark_dirty(s_layer);
    s_hint_timer = app_timer_register(HINT_MS, hint_tick, NULL);
  } else {
    s_hint_frame = 0;
    s_hint_timer = NULL;
    layer_mark_dirty(s_layer);
  }
}

static void start_hint(const char *line1, const char *line2) {
  if (s_hint_timer) { app_timer_cancel(s_hint_timer); s_hint_timer = NULL; }
  strncpy(s_hint_line1, line1, sizeof(s_hint_line1) - 1);
  s_hint_line1[sizeof(s_hint_line1) - 1] = '\0';
  strncpy(s_hint_line2, line2, sizeof(s_hint_line2) - 1);
  s_hint_line2[sizeof(s_hint_line2) - 1] = '\0';
  s_hint_frame = 1;
  layer_mark_dirty(s_layer);
  s_hint_timer = app_timer_register(HINT_MS, hint_tick, NULL);
}

static void draw_hint(GContext *ctx, GRect bounds) {
  if (s_hint_frame <= 0) return;

  // Calculate alpha 0..HINT_FADE_STEPS
  int alpha;
  if (s_hint_frame <= HINT_FADE_STEPS) {
    alpha = s_hint_frame;
  } else if (s_hint_frame <= HINT_FADE_STEPS + HINT_HOLD_STEPS) {
    alpha = HINT_FADE_STEPS;
  } else {
    int fade_out = s_hint_frame - HINT_FADE_STEPS - HINT_HOLD_STEPS;
    alpha = HINT_FADE_STEPS - fade_out;
  }
  if (alpha <= 0) return;

  int strip_h = 28;
  GRect strip = GRect(0, bounds.size.h - strip_h, bounds.size.w, strip_h);

#ifdef PBL_COLOR
  GColor strip_col = (alpha >= HINT_FADE_STEPS) ? GColorBlack : GColorDarkGray;
  GColor text_col  = (alpha >= HINT_FADE_STEPS) ? GColorWhite :
                     (alpha == 2)               ? GColorLightGray : GColorDarkGray;
#else
  GColor strip_col = GColorBlack;
  GColor text_col  = (alpha >= 2) ? GColorWhite : GColorLightGray;
#endif

  graphics_context_set_fill_color(ctx, strip_col);
  graphics_fill_rect(ctx, strip, 0, GCornerNone);
  graphics_context_set_text_color(ctx, text_col);
  graphics_draw_text(ctx, s_hint_line1,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(0, bounds.size.h - strip_h, bounds.size.w, 14),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, s_hint_line2,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(0, bounds.size.h - 14, bounds.size.w, 14),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// ---- Drawing ----

static void draw_menu(GContext *ctx, GRect bounds) {
  int cx = bounds.size.w / 2;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Golf ball image — centred between top of screen and "Mini Golf" title
  // IMPORTANT: draw size must exactly match bitmap size (68x68) or Pebble tiles it
  int title_y  = 74;
  int ball_size = 68;
  int ball_r    = ball_size / 2;
  int ball_cy   = title_y / 2;  // vertical centre of space above title

  if (s_gball_bmp) {
    graphics_draw_bitmap_in_rect(ctx, s_gball_bmp,
      GRect(cx - ball_r, ball_cy - ball_r, ball_size, ball_size));
  }

  int y = title_y;

  // Title
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Mini Golf",
    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(0, y, bounds.size.w, 28),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 27;

  // Best scores
  char b18[8], b9[8];
  if (s_best_18 > 0) snprintf(b18, sizeof(b18), "%d", s_best_18);
  else               snprintf(b18, sizeof(b18), "--");
  if (s_best_9  > 0) snprintf(b9,  sizeof(b9),  "%d", s_best_9);
  else               snprintf(b9,  sizeof(b9),  "--");
  char best_buf[28];
  snprintf(best_buf, sizeof(best_buf), "Best 18H:%s  9H:%s", b18, b9);
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorChromeYellow);
#endif
  graphics_draw_text(ctx, best_buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, y, bounds.size.w - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 16;

  graphics_context_set_text_color(ctx, GColorWhite);

  if (s_has_save) {
    int saved_hole = persist_read_int(PKEY_SAVE_HOLE);
    char res_buf[22];
    snprintf(res_buf, sizeof(res_buf), "SEL: Resume H%d/18", saved_hole + 1);
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorGreen);
#endif
    graphics_draw_text(ctx, res_buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(4, y, bounds.size.w - 8, 16),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y += 16;
    graphics_context_set_text_color(ctx, GColorWhite);
  }

  graphics_draw_text(ctx, "UP: New 18-Hole",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, y, bounds.size.w - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 16;
  graphics_draw_text(ctx, "DOWN: Random 9-Hole",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, y, bounds.size.w - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_hole_intro(GContext *ctx, GRect bounds) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorWhite);

  // "HOLE" on one line, number on the next — both fit at any width
  char num_buf[4];
  snprintf(num_buf, sizeof(num_buf), "%d", s_current_hole + 1);

  graphics_draw_text(ctx, "HOLE",
    fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
    GRect(0, 20, bounds.size.w, 48),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, num_buf,
    fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
    GRect(0, 66, bounds.size.w, 48),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorChromeYellow);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx,
    GPoint(bounds.size.w/2 - 28, 116),
    GPoint(bounds.size.w/2 + 28, 116));
#endif

  char buf[12];
  snprintf(buf, sizeof(buf), "Par  %d", s_cur_hole->par);
  graphics_draw_text(ctx, buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(0, 120, bounds.size.w, 28),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "SELECT to skip",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(0, bounds.size.h - 18, bounds.size.w, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_scorecard(GContext *ctx, GRect bounds) {
  int sw = bounds.size.w, sh = bounds.size.h;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);

  graphics_draw_text(ctx, "SCORECARD",
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(0, 2, sw, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  int total_par = 0, total_strokes = 0;
  int row_h = 14, start_y = 24;
  char buf[12];

  if (s_total_holes == 18) {
    // Divider line down the centre
    graphics_context_set_stroke_width(ctx, 1);
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorDarkGray);
#else
    graphics_context_set_stroke_color(ctx, GColorWhite);
#endif
    graphics_draw_line(ctx, GPoint(sw/2, start_y), GPoint(sw/2, sh - 20));

    for (int i = 0; i < 9; i++) {
      int y  = start_y + i * row_h;
      int li = i, ri = i + 9;
      const HoleData *hl = &s_holes[li];
      const HoleData *hr = &s_holes[ri];
      total_par += hl->par + hr->par;

      // Left column: holes 1-9
      if (s_scores[li] >= 0) {
        total_strokes += s_scores[li];
        snprintf(buf, sizeof(buf), "%d   %d", li+1, s_scores[li]);
        graphics_context_set_text_color(ctx, GColorWhite);
      } else {
        snprintf(buf, sizeof(buf), "%d  --", li+1);
#ifdef PBL_COLOR
        graphics_context_set_text_color(ctx, GColorDarkGray);
#endif
      }
      graphics_draw_text(ctx, buf,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(4, y, sw/2 - 6, row_h + 2),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

      // Right column: holes 10-18
      graphics_context_set_text_color(ctx, GColorWhite);
      if (s_scores[ri] >= 0) {
        total_strokes += s_scores[ri];
        snprintf(buf, sizeof(buf), "%d  %d", ri+1, s_scores[ri]);
        graphics_context_set_text_color(ctx, GColorWhite);
      } else {
        snprintf(buf, sizeof(buf), "%d --", ri+1);
#ifdef PBL_COLOR
        graphics_context_set_text_color(ctx, GColorDarkGray);
#endif
      }
      graphics_draw_text(ctx, buf,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(sw/2 + 4, y, sw/2 - 8, row_h + 2),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
  } else {
    // 9-hole random: single column
    for (int i = 0; i < 9; i++) {
      int y = start_y + i * row_h;
      const HoleData *hd = &s_proc_holes[i];
      total_par += hd->par;
      graphics_context_set_text_color(ctx, GColorWhite);
      if (s_scores[i] >= 0) {
        total_strokes += s_scores[i];
        snprintf(buf, sizeof(buf), "H%d   %d", i+1, s_scores[i]);
      } else {
        snprintf(buf, sizeof(buf), "H%d  --", i+1);
#ifdef PBL_COLOR
        graphics_context_set_text_color(ctx, GColorDarkGray);
#endif
      }
      graphics_draw_text(ctx, buf,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(4, y, sw - 8, row_h + 2),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
  }

  // Total line
  bool is_best = s_random_mode
    ? (s_best_9  > 0 && total_strokes == s_best_9)
    : (s_best_18 > 0 && total_strokes == s_best_18);
  char total_buf[32];
  snprintf(total_buf, sizeof(total_buf), "Total %d  Par %d%s",
           total_strokes, total_par, is_best ? " BEST!" : "");
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, is_best ? GColorChromeYellow : GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  graphics_draw_text(ctx, total_buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, sh - 18, sw - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_hud(GContext *ctx, int screen_w) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, screen_w, HUD_H), 0, GCornerNone);

  int rvp = running_vs_par();
  char rvp_str[7];
  if (rvp == 0)      snprintf(rvp_str, sizeof(rvp_str), "E");
  else if (rvp > 0)  snprintf(rvp_str, sizeof(rvp_str), "+%d", rvp);
  else               snprintf(rvp_str, sizeof(rvp_str), "%d", rvp);

  char buf[34];
  snprintf(buf, sizeof(buf), "H%d/%d P%d %s  %d",
           s_current_hole + 1, s_total_holes,
           s_cur_hole->par, rvp_str, s_strokes);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(2, 0, screen_w - 4, HUD_H),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_hole_field(GContext *ctx) {
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorIslamicGreen);
#else
  graphics_context_set_fill_color(ctx, GColorLightGray);
#endif
  graphics_fill_rect(ctx, GRect(s_px, s_py, PW, PH), 0, GCornerNone);

  GPoint cup = to_screen(s_cur_hole->cup);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, cup, CUP_R);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, cup, CUP_R - 2);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, cup, 1);

#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorDarkGreen);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  graphics_context_set_stroke_width(ctx, 3);
  for (int i = 0; i < s_cur_hole->num_walls; i++) {
    graphics_draw_line(ctx,
      to_screen(s_cur_hole->walls[i][0]),
      to_screen(s_cur_hole->walls[i][1]));
  }

  // Moving obstacle on hole 8
  if (s_current_hole == 7) {
    GPoint mw1, mw2;
    get_hole8_wall(&mw1, &mw2);
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorRed);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_context_set_stroke_width(ctx, 3);
    graphics_draw_line(ctx, to_screen(mw1), to_screen(mw2));
  }

#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorDarkGreen);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_rect(ctx, GRect(s_px, s_py, PW, PH));
}

static void draw_ball(GContext *ctx) {
  int bx = (int)(s_bx / FP) + s_px;
  int by = (int)(s_by / FP) + s_py;
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(bx, by), BALL_R);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, GPoint(bx, by), BALL_R);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(bx, by), BALL_R + 1);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(bx, by), BALL_R);
#endif
}

static void draw_arrow(GContext *ctx) {
  int bx = (int)(s_bx / FP) + s_px;
  int by = (int)(s_by / FP) + s_py;
  int32_t ta = DEG_TO_TRIGANGLE(s_angle);
  int dx = (int)(ARROW_LEN * sin_lookup(ta) / TRIG_MAX_RATIO);
  int dy = (int)(-ARROW_LEN * cos_lookup(ta) / TRIG_MAX_RATIO);
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorChromeYellow);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_line(ctx, GPoint(bx, by), GPoint(bx + dx, by + dy));
}

// Dotted shot guide line in aim phase
static void draw_shot_guide(GContext *ctx) {
  int bx = (int)(s_bx / FP) + s_px;
  int by = (int)(s_by / FP) + s_py;
  int32_t ta  = DEG_TO_TRIGANGLE(s_angle);
  int     sin_a = (int)(sin_lookup(ta)  * 100 / TRIG_MAX_RATIO);
  int     cos_a = (int)(cos_lookup(ta)  * 100 / TRIG_MAX_RATIO);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorChromeYellow);
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif

  // 5 dots at 60% of original distances (reduced by 40%)
  static const int dists[] = { 9, 21, 33 };
  for (int i = 0; i < 3; i++) {
    int d     = dists[i];
    int dot_x = bx + d * sin_a / 100;
    int dot_y = by - d * cos_a / 100;
    if (dot_x >= s_px + 1 && dot_x < s_px + PW - 1 &&
        dot_y >= s_py + 1 && dot_y < s_py + PH - 1) {
      graphics_fill_circle(ctx, GPoint(dot_x, dot_y), 2);
    }
  }
}

static void draw_power_bar(GContext *ctx, GRect bounds) {
  int bar_w = 8, bar_x = bounds.size.w - bar_w - 2;
  int bar_y = s_py + 8, bar_h = PH - 16;
  int fill_h = bar_h * s_power / 100;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h), 0, GCornerNone);

#ifdef PBL_COLOR
  GColor fc = (s_power > 70) ? GColorRed :
              (s_power > 40) ? GColorChromeYellow : GColorGreen;
  graphics_context_set_fill_color(ctx, fc);
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx,
    GRect(bar_x, bar_y + bar_h - fill_h, bar_w, fill_h), 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h));
}

static const char *score_term(int strokes, int par) {
  if (strokes == 1) return "Hole in One!";
  int d = strokes - par;
  if (d <= -2) return "Eagle!";
  if (d == -1) return "Birdie!";
  if (d ==  0) return "Par";
  if (d ==  1) return "Bogey";
  if (d ==  2) return "Double Bogey";
  return              "Triple+";
}

static void draw_hole_out_overlay(GContext *ctx, GRect bounds) {
  int ow = bounds.size.w - 36, ox = 18;
  int oy = bounds.size.h / 2 - 44, oh = 88;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(ox, oy, ow, oh), 6, GCornersAll);
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorChromeYellow);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, GRect(ox, oy, ow, oh), 6);
#endif

  graphics_context_set_text_color(ctx, GColorWhite);
  char buf[24];
  snprintf(buf, sizeof(buf), "Hole %d", s_current_hole + 1);
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(ox+4, oy+4, ow-8, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  snprintf(buf, sizeof(buf), "%d stroke%s", s_strokes, s_strokes == 1 ? "" : "s");
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(ox+4, oy+28, ow-8, 18),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, score_term(s_strokes, s_cur_hole->par),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(ox+4, oy+48, ow-8, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "SELECT to continue",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(ox+4, oy+70, ow-8, 18),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void layer_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_state == STATE_MENU) { draw_menu(ctx, bounds); return; }
  if (s_state == STATE_SCORECARD) { draw_scorecard(ctx, bounds); return; }
  if (s_state == STATE_HOLE_INTRO) { draw_hole_intro(ctx, bounds); return; }

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  draw_hole_field(ctx);
  draw_ball(ctx);
  draw_hud(ctx, bounds.size.w);

  switch (s_state) {
    case STATE_AIM:
      draw_shot_guide(ctx);
      draw_hint(ctx, bounds);
      break;
    case STATE_POWER:
      draw_shot_guide(ctx);
      draw_power_bar(ctx, bounds);
      draw_hint(ctx, bounds);
      break;
    case STATE_HOLE_OUT:
      draw_hole_out_overlay(ctx, bounds);
      break;
    default:
      break;
  }
}

// ---- Hole Setup ----
static void cancel_timers(void) {
  if (s_ball_timer)  { app_timer_cancel(s_ball_timer);  s_ball_timer  = NULL; }
  if (s_anim_timer)  { app_timer_cancel(s_anim_timer);  s_anim_timer  = NULL; }
  if (s_intro_timer) { app_timer_cancel(s_intro_timer); s_intro_timer = NULL; }
  if (s_hint_timer)  { app_timer_cancel(s_hint_timer);  s_hint_timer  = NULL; s_hint_frame = 0; }
}

static void start_hole(void) {
  s_cur_hole = s_random_mode ? &s_proc_holes[s_current_hole] : &s_holes[s_current_hole];
  s_bx = (int32_t)s_cur_hole->tee.x * FP;
  s_by = (int32_t)s_cur_hole->tee.y * FP;
  s_vx = s_vy = 0;
  s_strokes = 0;
  s_angle   = 0;
  s_power   = 50;
  s_anim_frame = 0;

  cancel_timers();
  s_anim_timer  = app_timer_register(ANIM_MS, anim_tick, NULL);
  s_intro_timer = app_timer_register(INTRO_MS, intro_timer_cb, NULL);
  s_state = STATE_HOLE_INTRO;
  layer_mark_dirty(s_layer);
}

static void resume_saved_game(void) {
  s_random_mode  = false;
  s_total_holes  = 18;
  s_current_hole = persist_read_int(PKEY_SAVE_HOLE);
  persist_read_data(PKEY_SAVE_SCORES, s_scores, sizeof(s_scores));
  if (s_current_hole < 18 && s_scores[s_current_hole] >= 0)
    s_current_hole++;
  if (s_current_hole >= 18) {
    check_and_save_best();
    s_state = STATE_SCORECARD;
    layer_mark_dirty(s_layer);
  } else {
    s_scores[s_current_hole] = -1;
    start_hole();
  }
}

// ---- Button Handlers ----
static void up_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_MENU:
      s_random_mode = false; s_total_holes = 18; s_current_hole = 0;
      memset(s_scores, -1, sizeof(s_scores));
      clear_saved_game();
      start_hole();
      break;
    case STATE_AIM:
      s_angle = (s_angle - ANGLE_STEP + 360) % 360;
      layer_mark_dirty(s_layer);
      break;
    case STATE_POWER:
      s_power = (s_power + POWER_STEP > 100) ? 100 : s_power + POWER_STEP;
      layer_mark_dirty(s_layer);
      break;
    default: break;
  }
}

static void down_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_MENU:
      s_random_mode = true; s_total_holes = 9; s_current_hole = 0;
      memset(s_scores, -1, sizeof(s_scores));
      generate_proc_holes();
      start_hole();
      break;
    case STATE_AIM:
      s_angle = (s_angle + ANGLE_STEP) % 360;
      layer_mark_dirty(s_layer);
      break;
    case STATE_POWER:
      s_power = (s_power - POWER_STEP < 0) ? 0 : s_power - POWER_STEP;
      layer_mark_dirty(s_layer);
      break;
    default: break;
  }
}

static void select_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_MENU:
      if (s_has_save) resume_saved_game();
      break;
    case STATE_HOLE_INTRO:
      // Skip countdown, go straight to aim
      if (s_intro_timer) { app_timer_cancel(s_intro_timer); s_intro_timer = NULL; }
      s_state = STATE_AIM;
      start_hint("UP/DN: rotate aim", "SEL: lock direction");
      layer_mark_dirty(s_layer);
      break;
    case STATE_AIM:
      s_state = STATE_POWER;
      start_hint("UP/DN: adjust power", "SEL: shoot");
      layer_mark_dirty(s_layer);
      break;
    case STATE_POWER: {
      s_strokes++;
      int32_t ta = DEG_TO_TRIGANGLE(s_angle);
      int32_t v0 = (int32_t)s_power * 4;
      s_vx = v0 * sin_lookup(ta) / TRIG_MAX_RATIO;
      s_vy = -v0 * cos_lookup(ta) / TRIG_MAX_RATIO;
      s_state = STATE_ROLLING;
      if (s_ball_timer) { app_timer_cancel(s_ball_timer); }
      s_ball_timer = app_timer_register(TIMER_MS, ball_tick, NULL);
      break;
    }
    case STATE_HOLE_OUT:
      s_current_hole++;
      if (s_current_hole >= s_total_holes) {
        check_and_save_best();
        s_state = STATE_SCORECARD;
        layer_mark_dirty(s_layer);
      } else {
        start_hole();
      }
      break;
    case STATE_SCORECARD:
      s_state = STATE_MENU;
      layer_mark_dirty(s_layer);
      break;
    default: break;
  }
}

static void back_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_POWER:
      s_state = STATE_AIM;
      start_hint("UP/DN: rotate aim", "SEL: lock direction");
      layer_mark_dirty(s_layer);
      break;
    case STATE_HOLE_INTRO:
    case STATE_AIM:
    case STATE_ROLLING:
    case STATE_HOLE_OUT:
      cancel_timers();
      save_game_progress();
      s_state = STATE_MENU;
      layer_mark_dirty(s_layer);
      break;
    case STATE_SCORECARD:
      s_state = STATE_MENU;
      layer_mark_dirty(s_layer);
      break;
    case STATE_MENU:
      window_stack_remove(s_window, true);
      break;
    default: break;
  }
}

static void click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   100, up_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
  window_single_click_subscribe(BUTTON_ID_BACK,   back_handler);
}

// ---- Window Lifecycle ----
static void window_load(Window *window) {
  Layer *root   = window_get_root_layer(window);
  GRect  bounds = layer_get_bounds(root);
  s_px = (bounds.size.w - PW) / 2;
  s_py = HUD_H + (bounds.size.h - HUD_H - PH) / 2;

  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, layer_update);
  layer_add_child(root, s_layer);

  s_gball_bmp = gbitmap_create_with_resource(RESOURCE_ID_GBALL_CLEAN);

  s_state = STATE_MENU;
  s_ball_timer = s_anim_timer = s_intro_timer = NULL;
}

static void window_unload(Window *window) {
  cancel_timers();
  layer_destroy(s_layer);
  if (s_gball_bmp) { gbitmap_destroy(s_gball_bmp); s_gball_bmp = NULL; }
}

// ---- App Lifecycle ----
static void init(void) {
  load_persistent_data();
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);
}

static void deinit(void) { window_destroy(s_window); }

int main(void) {
  init();
  app_event_loop();
  deinit();
}
