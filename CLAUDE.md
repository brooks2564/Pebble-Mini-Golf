# Pebble Mini Golf — Claude Code Instructions

## Project Overview
**Pebble Mini Golf** is a Pebble watchapp (not watchface) — a fully playable mini golf game.
UUID: `d7e8f9a0-b1c2-4d3e-a4f5-b6c7d8e9f0a1`
GitHub: `brooks2564/Pebble-Mini-Golf`
Author: `Brooman Inks`

## Build & Install
```bash
pebble build
cp build/Pebble-Mini-Golf.pbw Pebble-Mini-Golf.pbw
pebble install --phone 192.168.0.238
git add Pebble-Mini-Golf.pbw
git commit -m "Update PBW"
git push
```

## Game Design

### Main Menu
Two options:
1. **18-Hole Course** — hand-crafted holes, fixed layout every time
2. **Random Course** — 9 procedurally generated holes, seeded by current time

Menu also shows:
- Best scores (18-hole and 9-hole)
- Achievement stats (aces, eagles, under-par rounds) — shown once at least one is earned
- Resume option if a saved 18-hole game exists

### Gameplay Flow (per hole)
1. **Hole intro** — shows hole number and par for 1.5s; SELECT skips
2. **Aim phase** — dotted shot guide shows direction; UP/DOWN changes angle, SELECT locks it
3. **Power phase** — power bar on right side (green→yellow→red); UP increases, DOWN decreases, SELECT shoots
4. **Ball animation** — ball travels via AppTimer at 50ms, decelerates with friction, bounces off walls
5. **Hole out** — when ball enters cup radius, overlay shows score term (color-coded), advance on SELECT
6. **Scorecard** — shown after final hole; two-column layout (1–9 left, 10–18 right), total vs par diff

### Controls
- **UP**: rotate aim counter-clockwise (aim phase) / increase power (power phase)
- **DOWN**: rotate aim clockwise (aim phase) / decrease power (power phase)
- **SELECT**: confirm aim → power phase; confirm power → shoot; hole-out → next hole
- **BACK**: power → aim; in-game → save & return to menu; menu → exit app

### Score Terminology & Feedback
| Strokes vs Par | Term         | Overlay Color | Haptic          |
|----------------|--------------|---------------|-----------------|
| 1 stroke total | Hole in One! | Gold          | Triple pulse    |
| -2             | Eagle!       | Green         | Double pulse    |
| -1             | Birdie!      | Green         | Long pulse      |
|  0             | Par          | White         | Long pulse      |
| +1             | Bogey        | Red           | Long pulse      |
| +2             | Double Bogey | Red           | Long pulse      |
| +3+            | Triple+      | Red           | Long pulse      |
| MAX_STROKES(8) | pick up rule | —             | Double pulse    |

### Lip-out Mechanic
If ball enters cup radius at speed > 96 FP units, it bounces back out (dot-product reflection off cup-to-ball normal, 40% energy loss, nudge 2px outside cup). Short haptic tap.

### Achievements (persistent storage)
- `PKEY_ACH_HIO` (key 20) — total holes-in-one across all rounds
- `PKEY_ACH_EAGLES` (key 21) — total eagles across all rounds
- `PKEY_ACH_UNDER_PAR` (key 22) — total complete rounds finished under par

### Color vs B&W
- `#ifdef PBL_COLOR`: fairway = GColorIslamicGreen, border = GColorDarkGreen, ball = GColorWhite, cup = black+white ring, power bar green→yellow→red
- B&W (aplite, diorite, flint): fairway = GColorLightGray, border = GColorBlack, ball = GColorBlack

### Round Display (`#ifdef PBL_ROUND`)
Chalk and gabbro are round displays. Play area is constrained:
- `PW = 127`, `PH = 130` (fits within inscribed rectangle of round display)
- All 18 hole coordinates fit within this (max x=126, max y=120)

## Course Data Structure
```c
typedef struct {
  GPoint tee;           // ball start position (play area coords)
  GPoint cup;           // hole position (play area coords)
  uint8_t par;          // 2 or 3
  uint8_t num_walls;    // number of wall segments
  GPoint walls[12][2];  // wall segment endpoints [i][0]=start [i][1]=end
} HoleData;
```

Walls are line segments. Boundary walls checked separately from hole walls.
Each hole is defined in `static const HoleData s_holes[18]` — stored in flash.

## 18-Hole Course Design (hand-crafted)
Play area: PW×PH (130×138 rect, 127×130 round). HUD: top 16px.

1.  Straight shot — par 2
2.  One wall gap — par 2
3.  Two wall gaps — par 2
4.  Vertical wall dogleg — par 3
5.  Two offset walls — par 2
6.  S-curve (3 walls) — par 3
7.  Vertical wall opposite side — par 3
8.  Static + moving obstacle — par 3 (moving wall oscillates via AppTimer)
9.  Diagonal wall — par 2
10. Two diagonal walls — par 3
11. Box obstacle (4 walls) — par 3
12. Vertical wall mid-right — par 2
13. Two offset walls — par 3
14. Three staggered walls — par 3
15. L-shape wall — par 2
16. Two angled walls — par 3
17. Four staircase walls — par 3
18. S-curve grand finale (3 alternating walls) — par 3

Total par for 18-hole course: 46

## Procedural 9-Hole Course
Seed = `time(NULL)` at app launch.
Generation per hole:
1. Tee in top-left quadrant, cup in bottom-right quadrant (min distance enforced)
2. 1–3 random horizontal or vertical wall segments
3. Par 3 if distance > ~78px or 3 walls, else par 2
Cup y bounded to `rand_range(85, PH - 10)` for platform compatibility.

## Persistent Storage Keys
| Key | Purpose |
|-----|---------|
| 1   | Best 18-hole score |
| 2   | Best 9-hole score |
| 10  | Save exists flag |
| 11  | Saved hole index |
| 12  | Saved scores array |
| 20  | Achievement: holes-in-one count |
| 21  | Achievement: eagles count |
| 22  | Achievement: under-par rounds count |

## File Structure
```
Pebble-Mini-Golf/
├── package.json                 ← Pebble manifest (7 platforms)
├── wscript                      ← Build script
├── CLAUDE.md                    ← This file
├── README.md                    ← GitHub readme
├── Pebble-Mini-Golf.pbw         ← Compiled binary (updated after each build)
├── resources/images/
│   ├── gball_clean.png          ← 68×68 ball graphic (menu screen)
│   ├── menu_icon.png            ← 25×25 launcher icon
│   ├── icon_80x80.png           ← App store icon
│   ├── icon_144x144.png         ← App store icon
│   └── banner_720x320.png       ← App store banner
└── src/c/
    └── main.c                   ← All game code (~1160 lines)
```
No JS needed (pure C game, no phone communication).

## Target Platforms
- aplite (B&W, 144×168) — 24KB RAM
- basalt (color, 144×168)
- chalk (color, 180×180 round) — `#ifdef PBL_ROUND` play area
- diorite (B&W, 144×168)
- emery (color, 200×228)
- flint (B&W, 144×168)
- gabbro (color, 260×260 round) — `#ifdef PBL_ROUND` play area

## Key Implementation Notes
- Ball animation via `app_timer_register(50, ball_tick, NULL)` (20 fps)
- Fixed-point: `#define FP 16` — multiply all positions/velocities by FP, divide to render
- Wall collision: dot-product reflection off wall normal, 7/8 energy retained
- `graphics_fill_circle` for ball (r=3) and cup (r=5, white ring at r=3, black dot at r=1)
- Procedural holes in `static HoleData s_proc_holes[9]` built at game start
- Scorecard: `static int8_t s_scores[18]` (strokes per hole, -1 = not played)
- Shot guide: 3 dots at distances {9, 21, 33}px, r=2, drawn in both AIM and POWER states
- Fading hint strip: AppTimer at 120ms, HINT_FADE_STEPS=3, HINT_HOLD_STEPS=18
- Bitmap tiling bug: `graphics_draw_bitmap_in_rect` tiles if rect ≠ bitmap size — always draw gball_clean at exactly 68×68
