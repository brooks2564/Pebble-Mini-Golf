#include <pebble.h>

// ---- Constants ----
#define FP              16    // fixed-point scale
#define BALL_R           3    // ball radius px
#define CUP_R            5    // cup radius px
#define ARROW_LEN       16    // aim arrow length px
#define MAX_STROKES      8    // max shots per hole
#define TIMER_MS        50    // ball update interval ms
#define MAX_WALLS       12    // internal walls per hole
#define STOP_THRESH      6    // FP speed threshold to stop
#define ANGLE_STEP      10    // degrees per button press
#define POWER_STEP       5    // power % per button press

// Play area in basalt/aplite coordinate space
#define PW             130
#define PH             138
#define HUD_H           16

// Persistent storage keys
#define PKEY_BEST_18     1    // best total strokes on 18-hole
#define PKEY_BEST_9      2    // best total strokes on 9-hole
#define PKEY_SAVE_EXISTS 10   // bool: saved game present
#define PKEY_SAVE_HOLE   11   // int: hole index to resume
#define PKEY_SAVE_SCORES 12   // data: int8_t s_scores[18]

// ---- Types ----
typedef enum {
  STATE_MENU,
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

// ---- 18 Hole Definitions (play-area-relative coords, 0-129 x, 0-137 y) ----
static const HoleData s_holes[18] = {
  // 1: Open straight — par 2
  { {65,120}, {65,18}, 2, 0, {} },

  // 2: Center blocker — par 2
  { {65,120}, {65,18}, 2, 1,
    { {{30,72},{100,72}} } },

  // 3: Narrow slot — par 2
  { {65,120}, {65,18}, 2, 2,
    { {{4,80},{50,80}}, {{80,80},{126,80}} } },

  // 4: Dogleg right wall — par 3
  { {20,120}, {110,18}, 3, 1,
    { {{65,20},{65,95}} } },

  // 5: Two-wall slalom — par 2
  { {65,120}, {65,18}, 2, 2,
    { {{4,90},{70,90}}, {{60,50},{126,50}} } },

  // 6: Three staggered walls — par 3
  { {65,120}, {65,18}, 3, 3,
    { {{4,104},{60,104}}, {{70,72},{126,72}}, {{4,40},{60,40}} } },

  // 7: Left dogleg — par 3
  { {110,120}, {20,18}, 3, 1,
    { {{65,130},{65,45}} } },

  // 8: Cross (+) obstacle — par 3
  { {65,120}, {65,18}, 3, 2,
    { {{30,72},{100,72}}, {{65,90},{65,54}} } },

  // 9: Diagonal barrier — par 2
  { {20,120}, {110,18}, 2, 1,
    { {{20,95},{110,45}} } },

  // 10: Double diagonal — par 3
  { {65,120}, {65,18}, 3, 2,
    { {{15,100},{75,60}}, {{55,75},{115,35}} } },

  // 11: Box obstacle — par 3
  { {65,120}, {65,18}, 3, 4,
    { {{45,50},{85,50}}, {{85,50},{85,90}},
      {{85,90},{45,90}}, {{45,90},{45,50}} } },

  // 12: Side pocket (cup in alcove) — par 2
  { {20,120}, {110,60}, 2, 1,
    { {{65,30},{65,100}} } },

  // 13: Z-wall — par 3
  { {20,120}, {110,18}, 3, 2,
    { {{4,90},{80,90}}, {{50,50},{126,50}} } },

  // 14: Three-wall labyrinth — par 3
  { {65,120}, {65,18}, 3, 3,
    { {{4,100},{65,100}}, {{65,70},{126,70}}, {{4,40},{65,40}} } },

  // 15: Bank shot — par 2
  { {20,120}, {20,18}, 2, 1,
    { {{4,70},{90,70}} } },

  // 16: Pinball bumpers — par 3
  { {65,120}, {65,18}, 3, 2,
    { {{20,88},{50,54}}, {{80,88},{110,54}} } },

  // 17: Four-wall maze — par 3
  { {20,120}, {110,18}, 3, 4,
    { {{4,104},{60,104}}, {{70,80},{126,80}},
      {{4,60},{80,60}},  {{40,40},{126,40}} } },

  // 18: Grand finale — par 3
  { {65,120}, {65,18}, 3, 6,
    { {{4,110},{70,110}}, {{60,88},{126,88}},
      {{4,68},{80,68}},  {{40,48},{126,48}},
      {{4,28},{60,28}},  {{45,108},{45,40}} } },
};

// ---- Globals ----
static Window    *s_window;
static Layer     *s_layer;
static AppTimer  *s_ball_timer;
static GameState  s_state;
static bool       s_random_mode;
static int        s_current_hole;
static int        s_total_holes;
static int8_t     s_scores[18];   // strokes per hole (-1 = not played)
static int        s_strokes;      // strokes on current hole

// Ball physics (fixed-point, play-area coords × FP)
static int32_t s_bx, s_by;
static int32_t s_vx, s_vy;

// Aim/power state
static int s_angle;   // 0–359 degrees (0=up, clockwise)
static int s_power;   // 0–100

// Procedural holes
static HoleData s_proc_holes[9];

// Current hole pointer
static const HoleData *s_cur_hole;

// Play area screen offset
static int s_px, s_py;

// Persistent / best scores
static int  s_best_18;   // 0 = no record
static int  s_best_9;
static bool s_has_save;  // 18-hole game in progress

// ---- Helpers ----
static GPoint to_screen(GPoint p) {
  return GPoint(p.x + s_px, p.y + s_py);
}

static int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

// ---- Persistent Storage ----
static void load_persistent_data(void) {
  s_best_18  = persist_read_int(PKEY_BEST_18);
  s_best_9   = persist_read_int(PKEY_BEST_9);
  s_has_save = persist_read_bool(PKEY_SAVE_EXISTS);
}

static void save_game_progress(void) {
  if (s_random_mode) return;  // random mode doesn't save
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
  for (int i = 0; i < s_total_holes; i++) {
    if (s_scores[i] >= 0) total += s_scores[i];
  }
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
    h->cup.y = (int16_t)rand_range(85, 128);
    int dx = h->cup.x - h->tee.x;
    int dy = h->cup.y - h->tee.y;
    if (dx*dx + dy*dy < 4000) { h->cup.x = 110; h->cup.y = 120; }
    int nw = rand_range(1, 3);
    h->num_walls = (uint8_t)nw;
    h->par = (uint8_t)((dx*dx + dy*dy > 6000 || nw >= 3) ? 3 : 2);
    for (int w = 0; w < nw; w++) {
      int cx = rand_range(20, 110);
      int cy = rand_range(25, 115);
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
      h->walls[w][0] = GPointZero;
      h->walls[w][1] = GPointZero;
    }
  }
}

// ---- Wall Collision ----
static bool segments_cross(int ax, int ay, int bx, int by,
                            int cx, int cy, int dx, int dy) {
  int d1x = bx - ax, d1y = by - ay;
  int d2x = dx - cx, d2y = dy - cy;
  int cross = d1x * d2y - d1y * d2x;
  if (cross == 0) return false;
  int ex = cx - ax, ey = cy - ay;
  int t_num = ex * d2y - ey * d2x;
  int u_num = ex * d1y - ey * d1x;
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
  int wx = w2.x - w1.x;
  int wy = w2.y - w1.y;
  int nx = -wy, ny = wx;
  int len_sq = nx*nx + ny*ny;
  if (len_sq == 0) return;
  int32_t dot = (int32_t)s_vx * nx + (int32_t)s_vy * ny;
  s_vx -= (int32_t)2 * dot * nx / len_sq;
  s_vy -= (int32_t)2 * dot * ny / len_sq;
  s_vx = s_vx * 7 / 8;
  s_vy = s_vy * 7 / 8;
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
    GPoint w1 = s_cur_hole->walls[i][0];
    GPoint w2 = s_cur_hole->walls[i][1];
    if (segments_cross(ox, oy, nx, ny, w1.x, w1.y, w2.x, w2.y)) {
      reflect_off_wall(w1, w2);
      return;
    }
  }
}

// ---- Ball Update ----
static void ball_tick(void *context) {
  if (s_state != STATE_ROLLING) return;

  int ox = (int)(s_bx / FP);
  int oy = (int)(s_by / FP);
  int nx = (int)((s_bx + s_vx) / FP);
  int ny = (int)((s_by + s_vy) / FP);

  check_collisions(ox, oy, nx, ny);

  s_bx += s_vx;
  s_by += s_vy;

  if (s_bx < (int32_t)BALL_R * FP) {
    s_bx = (int32_t)BALL_R * FP;
    s_vx = iabs32(s_vx) * 7 / 8;
  } else if (s_bx > (int32_t)(PW - BALL_R) * FP) {
    s_bx = (int32_t)(PW - BALL_R) * FP;
    s_vx = -iabs32(s_vx) * 7 / 8;
  }
  if (s_by < (int32_t)BALL_R * FP) {
    s_by = (int32_t)BALL_R * FP;
    s_vy = iabs32(s_vy) * 7 / 8;
  } else if (s_by > (int32_t)(PH - BALL_R) * FP) {
    s_by = (int32_t)(PH - BALL_R) * FP;
    s_vy = -iabs32(s_vy) * 7 / 8;
  }

  // Friction
  s_vx = s_vx * 15 / 16;
  s_vy = s_vy * 15 / 16;

  // Cup check
  int bpx = (int)(s_bx / FP);
  int bpy = (int)(s_by / FP);
  int cdx = bpx - s_cur_hole->cup.x;
  int cdy = bpy - s_cur_hole->cup.y;
  if (cdx*cdx + cdy*cdy <= CUP_R*CUP_R) {
    s_scores[s_current_hole] = (int8_t)s_strokes;
    s_state = STATE_HOLE_OUT;
    layer_mark_dirty(s_layer);
    return;
  }

  // Stop check
  int32_t speed = iabs32(s_vx) + iabs32(s_vy);
  if (speed < STOP_THRESH) {
    s_vx = s_vy = 0;
    if (s_strokes >= MAX_STROKES) {
      s_scores[s_current_hole] = MAX_STROKES;
      s_state = STATE_HOLE_OUT;
    } else {
      s_state = STATE_AIM;
    }
    layer_mark_dirty(s_layer);
    return;
  }

  layer_mark_dirty(s_layer);
  s_ball_timer = app_timer_register(TIMER_MS, ball_tick, NULL);
}

// ---- Drawing ----
static void draw_menu(GContext *ctx, GRect bounds) {
  int cx = bounds.size.w / 2;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Ball graphic (smaller to make room for scores)
  int ball_r = 22;
  int ball_cy = ball_r + 8;
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorIslamicGreen);
  graphics_fill_circle(ctx, GPoint(cx, ball_cy), ball_r);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(cx, ball_cy), 6);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(cx, ball_cy), 2);
#else
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_circle(ctx, GPoint(cx, ball_cy), ball_r);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(cx, ball_cy), 6);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, GPoint(cx, ball_cy), 2);
#endif

  int y = ball_cy + ball_r + 4;

  // Title
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Mini Golf",
    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(0, y, bounds.size.w, 28),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 30;

  // Best scores
  char best_buf[28];
  char b18[10], b9[8];
  if (s_best_18 > 0) snprintf(b18, sizeof(b18), "%d", s_best_18);
  else               snprintf(b18, sizeof(b18), "--");
  if (s_best_9 > 0)  snprintf(b9, sizeof(b9), "%d", s_best_9);
  else               snprintf(b9, sizeof(b9), "--");
  snprintf(best_buf, sizeof(best_buf), "Best 18H:%s  9H:%s", b18, b9);
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorChromeYellow);
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  graphics_draw_text(ctx, best_buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, y, bounds.size.w - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 18;

  graphics_context_set_text_color(ctx, GColorWhite);

  // Resume option (18-hole saves only)
  if (s_has_save) {
    int saved_hole = persist_read_int(PKEY_SAVE_HOLE);
    char res_buf[24];
    snprintf(res_buf, sizeof(res_buf), "SEL: Resume H%d/18", saved_hole + 1);
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorGreen);
#endif
    graphics_draw_text(ctx, res_buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(4, y, bounds.size.w - 8, 16),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    y += 18;
    graphics_context_set_text_color(ctx, GColorWhite);
  }

  graphics_draw_text(ctx, "UP: New 18-Hole",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, y, bounds.size.w - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  y += 18;

  graphics_draw_text(ctx, "DOWN: Random 9-Hole",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, y, bounds.size.w - 8, 16),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_scorecard(GContext *ctx, GRect bounds) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Scorecard",
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(0, 4, bounds.size.w, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  int total_par = 0, total_strokes = 0;
  int y = 28;
  int avail = bounds.size.h - 56;
  int row_h = avail / s_total_holes;
  if (row_h < 12) row_h = 12;

  for (int i = 0; i < s_total_holes; i++) {
    const HoleData *h = s_random_mode ? &s_proc_holes[i] : &s_holes[i];
    int st = (s_scores[i] >= 0) ? s_scores[i] : 0;
    total_strokes += st;
    total_par += h->par;
    int diff = st - h->par;
    char buf[24];
    snprintf(buf, sizeof(buf), "%2d. %d/%d %s",
             i + 1, st, h->par,
             diff < 0 ? "-" : (diff == 0 ? "=" : "+"));
    graphics_draw_text(ctx, buf,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(6, y, bounds.size.w - 12, row_h + 2),
      GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    y += row_h;
    if (y > bounds.size.h - 30) break;
  }

  char buf[32];
  // Flag if new best
  bool is_best = s_random_mode ? (s_best_9 > 0 && total_strokes == s_best_9)
                                : (s_best_18 > 0 && total_strokes == s_best_18);
  snprintf(buf, sizeof(buf), "Total %d  Par %d%s",
           total_strokes, total_par, is_best ? " NEW BEST!" : "");
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, is_best ? GColorChromeYellow : GColorWhite);
#endif
  graphics_draw_text(ctx, buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(4, bounds.size.h - 28, bounds.size.w - 8, 24),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "SELECT: Menu",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, bounds.size.h - 50, bounds.size.w - 8, 18),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_hud(GContext *ctx, int screen_w) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, screen_w, HUD_H), 0, GCornerNone);

  char buf[32];
  snprintf(buf, sizeof(buf), "H%d/%d  P%d  Strokes:%d",
           s_current_hole + 1, s_total_holes,
           s_cur_hole->par, s_strokes);
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
    GPoint w1 = to_screen(s_cur_hole->walls[i][0]);
    GPoint w2 = to_screen(s_cur_hole->walls[i][1]);
    graphics_draw_line(ctx, w1, w2);
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

static void draw_power_bar(GContext *ctx, GRect bounds) {
  int bar_w = 8;
  int bar_x = bounds.size.w - bar_w - 2;
  int bar_y = s_py + 8;
  int bar_h = PH - 16;
  int fill_h = bar_h * s_power / 100;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h), 0, GCornerNone);

#ifdef PBL_COLOR
  GColor fill_col = (s_power > 70) ? GColorRed :
                    (s_power > 40) ? GColorChromeYellow :
                                     GColorGreen;
  graphics_context_set_fill_color(ctx, fill_col);
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, GRect(bar_x, bar_y + bar_h - fill_h, bar_w, fill_h),
                     0, GCornerNone);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h));
}

static const char *score_term(int strokes, int par) {
  if (strokes == 1) return "Hole in One!";
  int d = strokes - par;
  if (d <= -2)  return "Eagle!";
  if (d == -1)  return "Birdie!";
  if (d ==  0)  return "Par";
  if (d ==  1)  return "Bogey";
  if (d ==  2)  return "Double Bogey";
  return               "Triple+";
}

static void draw_hole_out_overlay(GContext *ctx, GRect bounds) {
  int ow = bounds.size.w - 36;
  int ox = 18;
  int oy = bounds.size.h / 2 - 44;
  int oh = 88;

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(ox, oy, ow, oh), 6, GCornersAll);
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorChromeYellow);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, GRect(ox, oy, ow, oh), 6);
#endif

  char buf[24];
  snprintf(buf, sizeof(buf), "Hole %d", s_current_hole + 1);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(ox + 4, oy + 4, ow - 8, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  snprintf(buf, sizeof(buf), "%d stroke%s", s_strokes, s_strokes == 1 ? "" : "s");
  graphics_draw_text(ctx, buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(ox + 4, oy + 28, ow - 8, 18),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, score_term(s_strokes, s_cur_hole->par),
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(ox + 4, oy + 48, ow - 8, 22),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  graphics_draw_text(ctx, "SELECT to continue",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(ox + 4, oy + 70, ow - 8, 18),
    GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void layer_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  if (s_state == STATE_MENU) {
    draw_menu(ctx, bounds);
    return;
  }
  if (s_state == STATE_SCORECARD) {
    draw_scorecard(ctx, bounds);
    return;
  }

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  draw_hole_field(ctx);
  draw_ball(ctx);
  draw_hud(ctx, bounds.size.w);

  switch (s_state) {
    case STATE_AIM:
      draw_arrow(ctx);
      graphics_context_set_text_color(ctx, GColorWhite);
      graphics_draw_text(ctx, "AIM  UP/DN=rotate  SEL=lock",
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(0, bounds.size.h - 14, bounds.size.w - 12, 14),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
      break;
    case STATE_POWER:
      draw_arrow(ctx);
      draw_power_bar(ctx, bounds);
      graphics_context_set_text_color(ctx, GColorWhite);
      graphics_draw_text(ctx, "POWER  UP/DN=adj  SEL=shoot",
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(0, bounds.size.h - 14, bounds.size.w - 12, 14),
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
      break;
    case STATE_HOLE_OUT:
      draw_hole_out_overlay(ctx, bounds);
      break;
    default:
      break;
  }
}

// ---- Hole Setup ----
static void start_hole(void) {
  s_cur_hole = s_random_mode ? &s_proc_holes[s_current_hole] : &s_holes[s_current_hole];
  s_bx = (int32_t)s_cur_hole->tee.x * FP;
  s_by = (int32_t)s_cur_hole->tee.y * FP;
  s_vx = s_vy = 0;
  s_strokes = 0;
  s_angle = 0;
  s_power = 50;
  s_state = STATE_AIM;
  layer_mark_dirty(s_layer);
}

static void resume_saved_game(void) {
  s_random_mode = false;
  s_total_holes = 18;
  s_current_hole = persist_read_int(PKEY_SAVE_HOLE);
  persist_read_data(PKEY_SAVE_SCORES, s_scores, sizeof(s_scores));
  // If saved at HOLE_OUT (score recorded), advance to next hole
  if (s_current_hole < 18 && s_scores[s_current_hole] >= 0) {
    s_current_hole++;
  }
  if (s_current_hole >= 18) {
    check_and_save_best();
    s_state = STATE_SCORECARD;
    layer_mark_dirty(s_layer);
  } else {
    // Clear in-progress score for this hole (start it fresh)
    s_scores[s_current_hole] = -1;
    start_hole();
  }
}

// ---- Button Handlers ----
static void up_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_MENU:
      // Start fresh 18-hole game (discards any saved game)
      s_random_mode = false;
      s_total_holes = 18;
      s_current_hole = 0;
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
    default:
      break;
  }
}

static void down_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_MENU:
      s_random_mode = true;
      s_total_holes = 9;
      s_current_hole = 0;
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
    default:
      break;
  }
}

static void select_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_MENU:
      if (s_has_save) resume_saved_game();
      break;
    case STATE_AIM:
      s_state = STATE_POWER;
      layer_mark_dirty(s_layer);
      break;
    case STATE_POWER: {
      // Shoot — increased power multiplier (×4 instead of ×3/2)
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
    default:
      break;
  }
}

static void back_handler(ClickRecognizerRef ref, void *ctx) {
  (void)ref; (void)ctx;
  switch (s_state) {
    case STATE_POWER:
      // Return to aim phase — don't go to menu
      s_state = STATE_AIM;
      layer_mark_dirty(s_layer);
      break;
    case STATE_AIM:
    case STATE_ROLLING:
    case STATE_HOLE_OUT:
      // Save progress and return to menu
      if (s_ball_timer) { app_timer_cancel(s_ball_timer); s_ball_timer = NULL; }
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
    default:
      break;
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
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_px = (bounds.size.w - PW) / 2;
  s_py = HUD_H + (bounds.size.h - HUD_H - PH) / 2;

  s_layer = layer_create(bounds);
  layer_set_update_proc(s_layer, layer_update);
  layer_add_child(root, s_layer);

  s_state = STATE_MENU;
  s_ball_timer = NULL;
}

static void window_unload(Window *window) {
  if (s_ball_timer) { app_timer_cancel(s_ball_timer); s_ball_timer = NULL; }
  layer_destroy(s_layer);
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

static void deinit(void) {
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
