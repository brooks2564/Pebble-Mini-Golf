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

### Gameplay Flow (per hole)
1. **Aim phase** — arrow rotates around ball; UP/DOWN changes angle, SELECT locks it
2. **Power phase** — power bar on right side; UP increases, DOWN decreases, SELECT shoots
3. **Ball animation** — ball travels via AppTimer, decelerates with friction, bounces off walls
4. **Hole out** — when ball enters cup radius, show score term + stroke count, advance to next hole
5. **Scorecard** — shown after final hole; total strokes vs total par

### Controls
- **UP**: rotate aim clockwise (aim phase) / increase power (power phase)
- **DOWN**: rotate aim counter-clockwise (aim phase) / decrease power (power phase)
- **SELECT**: confirm aim → enter power phase; confirm power → shoot
- **BACK**: exits app (standard Pebble behavior)

### Ball Physics
- Position stored as fixed-point (×16 subpixel precision): `int16_t ball_x, ball_y`
- Velocity: `int16_t vel_x, vel_y` (pixels/tick × 16)
- Friction: multiply velocity by 0.95 each tick (~20 ticks/sec via AppTimer at 50ms)
- Stop threshold: `|vel_x| + |vel_y| < 8` (fixed-point)
- Wall collision: reflect velocity vector off wall normal (dot product reflection formula)
- Hole-in: distance from ball center to cup center < 6px

### Score Terminology
| Strokes vs Par | Term       |
|----------------|------------|
| -2             | Eagle      |
| -1             | Birdie     |
|  0             | Par        |
| +1             | Bogey      |
| +2             | Double Bogey |
| +3 or more     | Triple+ / "X" |
| 1 stroke total | Hole in One! |
| Max strokes    | 8 (pick up rule) |

### Color vs B&W
- `#ifdef PBL_COLOR`: fairway = GColorIslamicGreen, rough border = GColorDarkGreen, ball = GColorWhite, cup = GColorBlack with white ring, power bar = GColorChromeYellow → GColorRed
- B&W (aplite, diorite, flint): fairway = GColorLightGray, border = GColorBlack, ball = GColorBlack, cup = GColorBlack

## Course Data Structure
```c
typedef struct {
  GPoint tee;           // ball start position (screen coords)
  GPoint cup;           // hole position (screen coords)
  uint8_t par;          // 2 or 3
  uint8_t num_walls;    // number of wall segments
  GPoint walls[20][2];  // wall segment endpoints [i][0]=start [i][1]=end
} HoleData;
```

Walls are line segments. The outer fairway boundary is also stored as wall segments.
Each hole is defined in a static array in flash (not RAM).

## 18-Hole Course Design (hand-crafted)
Screen play area: ~130×130px centered on 144×168 display (leaving room for HUD).
HUD: top 16px (hole number, par, strokes so far); bottom 12px (mini scorecard dots).

Hole designs (increasing difficulty):
1.  Straight shot — par 2, wide fairway, cup at far end
2.  Slight dogleg right — par 2
3.  Narrow corridor — par 2
4.  L-shape — par 3, sharp 90° turn
5.  Bumper in center — par 2, round obstacle to avoid
6.  Island fairway — par 3, narrow bridge path
7.  S-curve — par 3
8.  Windmill/wall blocker — par 3, moving obstacle (AppTimer)
9.  Long straight with side pocket — par 3
10. Zigzag — par 3
11. Boomerang shape — par 3
12. Cross junction — par 3, fairway crosses itself
13. Small green, long approach — par 3
14. Pinball bumpers (2 round obstacles) — par 3
15. Spiral approach — par 3
16. Off-angle shot required — par 2
17. Bank shot hole — par 2 (you must bounce off a wall)
18. Grand finale — par 3, complex shape

Total par for 18-hole course: 46

## Procedural 9-Hole Course
Seed = `time(NULL)` XOR milliseconds at app launch.
Generation per hole:
1. Place tee at random position in top-left quadrant of play area
2. Place cup at random position in bottom-right quadrant (ensuring min distance 60px)
3. Generate 2–4 random wall segments as obstacles (not blocking direct line to cup completely)
4. Add outer boundary walls based on a randomly chosen fairway shape:
   - Rectangle, L-shape, T-shape, or Z-shape
5. Par: if distance tee→cup > 80px or 3+ obstacles → par 3, else par 2

## File Structure
```
Pebble-Mini-Golf/
├── package.json          ← Pebble manifest
├── wscript               ← Build script
├── CLAUDE.md             ← This file
├── Pebble-Mini-Golf.pbw  ← Compiled binary (updated after each build)
└── src/
    └── c/
        └── main.c        ← All game code (~700 lines)
```
No JS needed (pure C game, no phone communication).

## Target Platforms
- aplite (B&W, 144×168) — 24KB RAM, careful with data
- basalt (color, 144×168)
- diorite (B&W, 144×168)
- emery (color, 200×228) — adjust play area to 180×180
- flint (B&W, 144×168)

## Key Implementation Notes
- All 18 hole definitions in `static const HoleData s_holes[18]` — stored in flash
- Ball animation via `app_timer_register(50, ball_tick, NULL)` (20 fps)
- Fixed-point: `#define FP 16` — multiply all positions/velocities by FP, divide to render
- Wall collision: for each wall segment, check if ball crosses it this tick, reflect `vel` off wall normal
- `graphics_fill_circle` for ball (r=3) and cup (r=4 with inner dot)
- Procedural holes stored in `static HoleData s_proc_holes[9]` built at game start
- Scorecard: `static int8_t s_scores[18]` (strokes per hole, -1 = not played yet)
