
#include "main.h"
#include "fonts.h"
#include "ssd1306.h"
#include <stdio.h>
#include <string.h>


I2C_HandleTypeDef hi2c1;
static uint32_t last_activity_ms = 0;


#define W 128
#define H 64

#define PADDLE_W 2
#define PADDLE_H 14
#define BALL_SZ 3

#define FPS 60
#define FRAME_MS (1000 / FPS)

#define BTN_PRESSED(port, pin) (HAL_GPIO_ReadPin((port), (pin)) == GPIO_PIN_SET)


typedef struct { int y; int h; } Paddle;
typedef struct { int x,y,vx,vy,size; } Ball;

static Paddle p1, p2;
static Ball ball;

static int score1 = 0, score2 = 0;
static float ball_speed = 1.0f;
static const float BALL_SPEED_INC = 0.15f;
static const float BALL_SPEED_MAX = 5.0f;

static int clamp(int v, int lo, int hi){ return (v<lo)?lo:(v>hi)?hi:v; }

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);

static void tetris_init(void);
static void tetris_update(uint8_t pressed_mask, uint8_t cur_mask);
static void tetris_draw(void);

static void pongplus_init(void);
static void pongplus_update(uint8_t pressed_mask, uint8_t cur_mask);
static void pongplus_draw(void);

static void maze_init(void);
static void maze_update(uint8_t pressed_mask, uint8_t cur_mask);
static void maze_draw(void);

static void invaders_init(void);
static void invaders_update(uint8_t pressed_mask, uint8_t cur_mask);
static void invaders_draw(void);

static void breakout_init(void);
static void breakout_update(uint8_t pressed_mask, uint8_t cur_mask);
static void breakout_draw(void);

static void rogue_init(void);
static void rogue_update(uint8_t pressed_mask, uint8_t cur_mask);
static void rogue_draw(void);

static void flappy_init(void);
static void flappy_update(uint8_t pressed_mask, uint8_t cur_mask);
static void flappy_draw(void);

static inline uint8_t any_button_pressed(void)
{
  return  BTN_PRESSED(BUTTON1_GPIO_Port, BUTTON1_Pin) ||
          BTN_PRESSED(BUTTON2_GPIO_Port, BUTTON2_Pin) ||
          BTN_PRESSED(BUTTON3_GPIO_Port, BUTTON3_Pin) ||
          BTN_PRESSED(BUTTON4_GPIO_Port, BUTTON4_Pin);
}

// ===================== MENU / APP STATE =====================
typedef enum {
  APP_MENU = 0,
  APP_PONG,
  APP_PONG_PLUS,
  APP_TETRIS,
  APP_MAZE,
  APP_INVADERS,
  APP_BREAKOUT,
  APP_ROGUE,
  APP_FLAPPY
} AppState;

typedef struct {
  const char *name;
  AppState state;
  uint8_t soon;   // 1 = show "SOON" tag
} MenuItem;

static const MenuItem menu_items[] = {
  {"PONG",      APP_PONG,      0},
  {"TETRIS",    APP_TETRIS,    0},
  {"PONG+",     APP_PONG_PLUS, 1},
  {"MAZE",      APP_MAZE,      0},
  {"ROGUE80",   APP_ROGUE,     0},
  {"FLAPPY",    APP_FLAPPY,    0},
  {"INVADERS",  APP_INVADERS,  0},
  {"BREAKOUT",  APP_BREAKOUT,  0},
};

#define MENU_COUNT   ((int)(sizeof(menu_items)/sizeof(menu_items[0])))
#define MENU_VISIBLE 2   // 2 big windows visible, scroll for the rest

static AppState app_state = APP_MENU;
static int menu_sel = 0;                  // 0 = Pong, 1 = Pong+
static uint8_t prev_btn_mask = 0;
static uint32_t exit_combo_t0 = 0;

#define BTN1_M (1u<<0)
#define BTN2_M (1u<<1)
#define BTN3_M (1u<<2)
#define BTN4_M (1u<<3)

static inline uint8_t read_buttons_mask(void)
{
  uint8_t m = 0;
  if (BTN_PRESSED(BUTTON1_GPIO_Port, BUTTON1_Pin)) m |= BTN1_M;
  if (BTN_PRESSED(BUTTON2_GPIO_Port, BUTTON2_Pin)) m |= BTN2_M;
  if (BTN_PRESSED(BUTTON3_GPIO_Port, BUTTON3_Pin)) m |= BTN3_M;
  if (BTN_PRESSED(BUTTON4_GPIO_Port, BUTTON4_Pin)) m |= BTN4_M;
  return m;
}

// --------- simple pixel primitives (no dependency on extra SSD1306 APIs)
static void draw_hline(int x0, int x1, int y, uint8_t c)
{
  if (y < 0 || y >= H) return;
  if (x0 > x1) { int t=x0; x0=x1; x1=t; }
  x0 = clamp(x0, 0, W-1);
  x1 = clamp(x1, 0, W-1);
  for (int x=x0; x<=x1; x++) SSD1306_DrawPixel(x, y, c);
}

static void draw_vline(int x, int y0, int y1, uint8_t c)
{
  if (x < 0 || x >= W) return;
  if (y0 > y1) { int t=y0; y0=y1; y1=t; }
  y0 = clamp(y0, 0, H-1);
  y1 = clamp(y1, 0, H-1);
  for (int y=y0; y<=y1; y++) SSD1306_DrawPixel(x, y, c);
}

static void draw_rect(int x, int y, int w, int h, uint8_t c)
{
  if (w <= 0 || h <= 0) return;
  draw_hline(x, x+w-1, y, c);
  draw_hline(x, x+w-1, y+h-1, c);
  draw_vline(x, y, y+h-1, c);
  draw_vline(x+w-1, y, y+h-1, c);
}

static void fill_rect(int x, int y, int w, int h, uint8_t c)
{
  if (w <= 0 || h <= 0) return;
  int x0 = clamp(x, 0, W-1);
  int y0 = clamp(y, 0, H-1);
  int x1 = clamp(x+w-1, 0, W-1);
  int y1 = clamp(y+h-1, 0, H-1);

  for (int yy = y0; yy <= y1; yy++)
    for (int xx = x0; xx <= x1; xx++)
      SSD1306_DrawPixel(xx, yy, c);
}

// Hold ALL 4 buttons ~1s to return to menu (safe "escape" combo)
static uint8_t exit_combo_detect(uint8_t cur_mask)
{
  const uint8_t ALL = BTN1_M | BTN2_M | BTN3_M | BTN4_M;

  if ((cur_mask & ALL) == ALL) {
    if (exit_combo_t0 == 0) exit_combo_t0 = HAL_GetTick();
    if ((HAL_GetTick() - exit_combo_t0) >= 1000u) {
      exit_combo_t0 = 0;
      return 1;
    }
  } else {
    exit_combo_t0 = 0;
  }
  return 0;
}

static int menu_get_top(void)
{
  if (MENU_COUNT <= MENU_VISIBLE) return 0;

  int max_top = MENU_COUNT - MENU_VISIBLE;
  int top = 0;

  // Keep selection visible in the 2-slot window
  if (menu_sel >= MENU_VISIBLE) top = menu_sel - (MENU_VISIBLE - 1);
  if (top > max_top) top = max_top;
  if (top < 0) top = 0;
  return top;
}

static void menu_draw_icon(AppState st, int x, int y, uint8_t fg)
{
  if (st == APP_PONG) {
    // Pong: two paddles + ball
    fill_rect(x + 0,  y + 3, 2, 10, fg);
    fill_rect(x + 18, y + 3, 2, 10, fg);
    fill_rect(x + 9,  y + 7, 3, 3,  fg);

  } else if (st == APP_TETRIS) {
    // Tetris: little block
    fill_rect(x + 2,  y + 8, 4, 4, fg);
    fill_rect(x + 6,  y + 8, 4, 4, fg);
    fill_rect(x + 10, y + 8, 4, 4, fg);
    fill_rect(x + 6,  y + 4, 4, 4, fg);

  } else if (st == APP_PONG_PLUS) {
    // Pong+: plus sign
    fill_rect(x + 9, y + 4, 3, 10, fg);
    fill_rect(x + 4, y + 8, 12, 3, fg);

  } else if (st == APP_MAZE) {
    // Maze: a tiny maze box
    draw_rect(x + 2, y + 2, 18, 12, fg);
    // inner walls
    draw_hline(x + 4,  x + 16, y + 5, fg);
    draw_vline(x + 6,  y + 5,  y + 11, fg);
    draw_hline(x + 6,  x + 14, y + 9, fg);
    // entrance gap + exit gap (stylish)
    SSD1306_DrawPixel(x + 2,  y + 4, 0); // entrance notch
    SSD1306_DrawPixel(x + 19, y + 9, 0); // exit notch
  } else if (st == APP_INVADERS) {
	// Invaders: tiny alien
	// head
	fill_rect(x + 6,  y + 4, 8, 4, fg);
	// eyes gap (only if fg=1, we can carve with 0; if inverted it still looks ok)
	SSD1306_DrawPixel(x + 8, y + 5, fg ? 0 : 1);
	SSD1306_DrawPixel(x +10, y + 5, fg ? 0 : 1);
	// legs
	fill_rect(x + 5,  y + 8, 2, 3, fg);
	fill_rect(x + 9,  y + 8, 2, 3, fg);
	fill_rect(x +13,  y + 8, 2, 3, fg);
  } else if (st == APP_BREAKOUT) {
	  // bricks + paddle + ball
	  fill_rect(x + 3, y + 3, 14, 3, fg);   // bricks row
	  fill_rect(x + 6, y + 12, 10, 2, fg);  // paddle
	  fill_rect(x + 11, y + 9, 2, 2, fg);   // ball
  } else if (st == APP_ROGUE) {
    // Tiny dungeon room + player + stairs
    draw_rect(x + 2, y + 2, 18, 12, fg);
    fill_rect(x + 5, y + 4, 3, 3, fg);      // player
    draw_hline(x + 8, x + 14, y + 8, fg);   // corridor
    draw_vline(x + 16, y + 8, y + 11, fg);  // '>'
    draw_hline(x + 14, x + 16, y + 11, fg);
  } else if (st == APP_FLAPPY) {
    // pipe
    fill_rect(x + 2,  y + 2, 4, 5, fg);
    fill_rect(x + 2,  y + 10, 4, 4, fg);

    // bird
    fill_rect(x + 10, y + 7, 4, 3, fg);
    SSD1306_DrawPixel(x + 11, y + 6, fg);
    SSD1306_DrawPixel(x + 12, y + 6, fg);
    SSD1306_DrawPixel(x + 14, y + 8, fg);
  }

}
static void menu_draw(void)
{
  SSD1306_Fill(0);

  // Frame
  draw_rect(0, 0, W, H, 1);

  // Title
  SSD1306_GotoXY(22, 2);
  SSD1306_Puts("ARCADE", &Font_11x18, 1);
  draw_hline(8, W - 9, 22, 1);

  // Big windows
  const int box_x = 10;
  const int box_w = 108;
  const int box_h = 16;   // smaller tiles
  const int y0 = 26;
  const int gap = 18;     // tighter spacing

  int top = menu_get_top();

  for (int slot = 0; slot < MENU_VISIBLE; slot++) {
    int idx = top + slot;
    if (idx >= MENU_COUNT) break;

    int y = y0 + slot * gap;
    uint8_t sel = (idx == menu_sel);

    if (sel) fill_rect(box_x, y, box_w, box_h, 1);
    draw_rect(box_x, y, box_w, box_h, 1);

    // Arrow
    if (sel && (((HAL_GetTick() / 250) & 1) == 0)) {
      SSD1306_GotoXY(box_x + 4, y + 4);
      SSD1306_Puts(">", &Font_7x10, 0);
    }

    // Text
    SSD1306_GotoXY(box_x + 18, y + (box_h - 10) / 2);
    SSD1306_Puts((char*)menu_items[idx].name, &Font_7x10, sel ? 0 : 1);

//    // "SOON" tag
//    if (menu_items[idx].soon) {
//      SSD1306_GotoXY(box_x + box_w - 32, y + 6);
//      SSD1306_Puts("SOON", &Font_7x10, sel ? 0 : 1);
//    }

    // Icon on right
    uint8_t fg = sel ? 0 : 1;
    menu_draw_icon(menu_items[idx].state, box_x + box_w - 24, y + 1, fg);
  }

  // Scrollbar (only if needed)
  if (MENU_COUNT > MENU_VISIBLE) {
    int max_top = MENU_COUNT - MENU_VISIBLE;
    int track_x = W - 4;      // inside border
    int track_y = y0;
    int track_h = (MENU_VISIBLE * box_h); // 40

    draw_vline(track_x, track_y, track_y + track_h - 1, 1);

    int knob_h = (track_h * MENU_VISIBLE) / MENU_COUNT;
    if (knob_h < 8) knob_h = 8;

    int knob_y = track_y + ((track_h - knob_h) * top) / max_top;
    fill_rect(track_x - 1, knob_y, 3, knob_h, 1);
  }

  SSD1306_UpdateScreen();
}

static void reset_ball(int dir) {
  ball_speed = 1.0f;            // reset speed each point (or remove if you want it to keep increasing across points)
  ball.x = W/2;
  ball.y = H/2;
  ball.vx = dir;           // +1 or -1
  ball.vy = (dir > 0) ? 1 : -1; // small angle to start
  ball.size = BALL_SZ;
}

static void game_init(void){
  p1.y = (H - PADDLE_H)/2; p1.h = PADDLE_H;
  p2.y = (H - PADDLE_H)/2; p2.h = PADDLE_H;
  reset_ball(+1);
}

static void menu_update(uint8_t pressed_mask)
{
  // UP (B1)
  if (pressed_mask & BTN1_M) {
    menu_sel--;
    if (menu_sel < 0) menu_sel = MENU_COUNT - 1;
  }

  // DOWN (B2)
  if (pressed_mask & BTN2_M) {
    menu_sel++;
    if (menu_sel >= MENU_COUNT) menu_sel = 0;
  }

  // SELECT (B3)
  if (pressed_mask & BTN3_M) {
    AppState st = menu_items[menu_sel].state;

    if (st == APP_PONG) {
      score1 = 0; score2 = 0;
      game_init();
      app_state = APP_PONG;
    } else if (st == APP_TETRIS) {
      tetris_init();
      app_state = APP_TETRIS;
    } else if (st == APP_PONG_PLUS) {
	  pongplus_init();
      app_state = APP_PONG_PLUS;
    } else if (st == APP_MAZE) {
      maze_init();
      app_state = APP_MAZE;
    } else if (st == APP_INVADERS) {
	  invaders_init();
	  app_state = APP_INVADERS;
	} else if (st == APP_BREAKOUT) {
	  breakout_init();
	  app_state = APP_BREAKOUT;
    } else if (st == APP_ROGUE) {
      rogue_init();
      app_state = APP_ROGUE;
    } else if (st == APP_FLAPPY) {
      flappy_init();
      app_state = APP_FLAPPY;
    }
  }
}

/* =========================== PONG+ (POWER UPS) =========================== */

typedef struct {
  int y;
  int h;

  float speed_factor;      // 1.0 = normal
  uint32_t speed_end_ms;

  float size_factor;       // 1.0 = normal (affects height)
  uint32_t size_end_ms;

  uint32_t shield_end_ms;  // if active, prevents scoring on that side
  uint32_t reverse_end_ms; // if active, controls are reversed
} PaddlePlus;

typedef struct {
  float x, y;
  float vx, vy;
  int size;
} BallPlus;

static PaddlePlus pp1, pp2;
static BallPlus   pb;

static int pplus_score1 = 0;
static int pplus_score2 = 0;

static int pplus_last_touch = 1; // 1 or 2 (who last hit the ball)

static float pb_base_speed = 1.2f;   // ramps up as game goes
static const float PB_SPEED_INC = 0.15f;
static const float PB_SPEED_MAX = 6.0f;

// Ball timed effects
static float pb_speed_factor = 1.0f;     // used as multiplier in movement step
static uint32_t pb_speed_end_ms = 0;

static uint8_t pb_visible = 1;
static uint32_t pb_invis_end_ms = 0;

// Orb / powerup spawn
static uint8_t pu_active = 0;
static int pu_cx = 0, pu_cy = 0;
static uint32_t pu_end_ms = 0;
static uint32_t pu_next_spawn_ms = 0;

// UI message when powerup is collected
static char pu_msg[18] = {0};
static uint32_t pu_msg_end_ms = 0;

// RNG (independent from tetris)
static uint32_t pp_rng = 0xC0FFEEu;

static inline uint8_t time_expired(uint32_t now, uint32_t end_ms)
{
  return (end_ms != 0u) && ((int32_t)(now - end_ms) >= 0);
}

static uint32_t pp_rand_u32(void)
{
  pp_rng = pp_rng * 1664525u + 1013904223u;
  return pp_rng;
}

static int pp_rand_range(int lo, int hi) // inclusive
{
  if (hi <= lo) return lo;
  return lo + (int)(pp_rand_u32() % (uint32_t)(hi - lo + 1));
}

static float pp_rand_frange(float lo, float hi)
{
  uint32_t r = (pp_rand_u32() >> 8) & 0xFFFFu; // 0..65535
  float t = (float)r / 65535.0f;
  return lo + (hi - lo) * t;
}

static float f_abs(float v) { return (v < 0) ? -v : v; }

static void pu_set_msg(const char *s)
{
  strncpy(pu_msg, s, sizeof(pu_msg) - 1);
  pu_msg[sizeof(pu_msg) - 1] = 0;
  pu_msg_end_ms = HAL_GetTick() + 1500u;
}

static void pongplus_reset_ball(int dir)
{
  pb_base_speed = 1.2f;
  pb_speed_factor = 1.0f;
  pb_speed_end_ms = 0;

  pb_visible = 1;
  pb_invis_end_ms = 0;

  pb.size = BALL_SZ;
  pb.x = (float)(W / 2);
  pb.y = (float)(H / 2);

  // small random angle
  float vy0 = (pp_rand_u32() & 1u) ? 0.8f : -0.8f;

  pb.vx = (dir > 0) ? pb_base_speed : -pb_base_speed;
  pb.vy = vy0;
}

typedef enum {
  PU_BALL_SPEED = 0,
  PU_BALL_SLOW,
  PU_PADDLE_SPEED,
  PU_SHIELD,
  PU_WIDE_PADDLE,
  PU_INVIS_BALL,
  PU_REVERSE_CONTROLS,
  PU_TINY_PADDLE,
  PU_COUNT
} PowerUpType;

static void pongplus_apply_powerup(int collector) // collector: 1 or 2
{
  uint32_t now = HAL_GetTick();
  PowerUpType t = (PowerUpType)(pp_rand_u32() % (uint32_t)PU_COUNT);

  PaddlePlus *me  = (collector == 1) ? &pp1 : &pp2;
  PaddlePlus *opp = (collector == 1) ? &pp2 : &pp1;

  switch (t)
  {
    case PU_BALL_SPEED: {
      pb_speed_factor = pp_rand_frange(1.5f, 2.0f);
      pb_speed_end_ms = now + (uint32_t)pp_rand_range(3000, 5000);
      pu_set_msg((collector == 1) ? "P1 BALL FAST" : "P2 BALL FAST");
    } break;

    case PU_BALL_SLOW: {
      pb_speed_factor = pp_rand_frange(0.5f, 0.7f);
      pb_speed_end_ms = now + (uint32_t)pp_rand_range(3000, 5000);
      pu_set_msg((collector == 1) ? "P1 SLOW MO" : "P2 SLOW MO");
    } break;

    case PU_PADDLE_SPEED: {
      me->speed_factor = 1.8f;
      me->speed_end_ms = now + (uint32_t)pp_rand_range(3000, 5000);
      pu_set_msg((collector == 1) ? "P1 SPD UP" : "P2 SPD UP");
    } break;

    case PU_SHIELD: {
      me->shield_end_ms = now + 3000u;
      pu_set_msg((collector == 1) ? "P1 SHIELD" : "P2 SHIELD");
    } break;

    case PU_WIDE_PADDLE: {
      me->size_factor = 2.0f;
      me->size_end_ms = now + (uint32_t)pp_rand_range(3000, 5000);
      pu_set_msg((collector == 1) ? "P1 BIG PAD" : "P2 BIG PAD");
    } break;

    case PU_INVIS_BALL: {
      pb_visible = 0;
      pb_invis_end_ms = now + (uint32_t)pp_rand_range(1000, 2000);
      pu_set_msg((collector == 1) ? "P1 INVIS" : "P2 INVIS");
    } break;

    case PU_REVERSE_CONTROLS: {
      opp->reverse_end_ms = now + (uint32_t)pp_rand_range(3000, 5000);
      pu_set_msg((collector == 1) ? "P2 REVERSE" : "P1 REVERSE");
    } break;

    case PU_TINY_PADDLE: {
      opp->size_factor = 0.6f;
      opp->size_end_ms = now + (uint32_t)pp_rand_range(3000, 5000);
      pu_set_msg((collector == 1) ? "P2 TINY" : "P1 TINY");
    } break;

    default: break;
  }
}

static void pongplus_spawn_orb(uint32_t now)
{
  pu_active = 1;

  // Spawn near center
  pu_cx = W / 2;
  pu_cy = pp_rand_range(12, H - 12);

  pu_end_ms = now + (uint32_t)pp_rand_range(5000, 8000);
}

static void pongplus_update_effects(uint32_t now)
{
  // Ball effects
  if (time_expired(now, pb_speed_end_ms)) {
    pb_speed_factor = 1.0f;
    pb_speed_end_ms = 0;
  }
  if (time_expired(now, pb_invis_end_ms)) {
    pb_visible = 1;
    pb_invis_end_ms = 0;
  }

  // Paddle effects (p1)
  if (time_expired(now, pp1.speed_end_ms)) {
    pp1.speed_factor = 1.0f;
    pp1.speed_end_ms = 0;
  }
  if (time_expired(now, pp1.size_end_ms)) {
    pp1.size_factor = 1.0f;
    pp1.size_end_ms = 0;
  }
  if (time_expired(now, pp1.shield_end_ms)) pp1.shield_end_ms = 0;
  if (time_expired(now, pp1.reverse_end_ms)) pp1.reverse_end_ms = 0;

  // Paddle effects (p2)
  if (time_expired(now, pp2.speed_end_ms)) {
    pp2.speed_factor = 1.0f;
    pp2.speed_end_ms = 0;
  }
  if (time_expired(now, pp2.size_end_ms)) {
    pp2.size_factor = 1.0f;
    pp2.size_end_ms = 0;
  }
  if (time_expired(now, pp2.shield_end_ms)) pp2.shield_end_ms = 0;
  if (time_expired(now, pp2.reverse_end_ms)) pp2.reverse_end_ms = 0;

  // Recompute paddle heights while keeping them clamped nicely
  {
    int c1 = pp1.y + pp1.h / 2;
    pp1.h = clamp((int)(PADDLE_H * pp1.size_factor + 0.5f), 6, H - 2);
    pp1.y = clamp(c1 - pp1.h / 2, 0, H - pp1.h);

    int c2 = pp2.y + pp2.h / 2;
    pp2.h = clamp((int)(PADDLE_H * pp2.size_factor + 0.5f), 6, H - 2);
    pp2.y = clamp(c2 - pp2.h / 2, 0, H - pp2.h);
  }
}

static void pongplus_init(void)
{
  pp_rng ^= HAL_GetTick();

  pplus_score1 = 0;
  pplus_score2 = 0;

  pp1.y = (H - PADDLE_H) / 2;
  pp1.h = PADDLE_H;
  pp1.speed_factor = 1.0f; pp1.speed_end_ms = 0;
  pp1.size_factor  = 1.0f; pp1.size_end_ms  = 0;
  pp1.shield_end_ms = 0;
  pp1.reverse_end_ms = 0;

  pp2 = pp1;

  pplus_last_touch = 1;

  pongplus_reset_ball((pp_rand_u32() & 1u) ? +1 : -1);

  // Powerup orb scheduling
  pu_active = 0;
  pu_next_spawn_ms = HAL_GetTick() + (uint32_t)pp_rand_range(1500, 3000);
  pu_end_ms = 0;

  pu_msg[0] = 0;
  pu_msg_end_ms = 0;
}

static void pongplus_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  (void)pressed_mask;

  uint32_t now = HAL_GetTick();

  pongplus_update_effects(now);

  // Spawn/despawn orb
  if (!pu_active && (int32_t)(now - pu_next_spawn_ms) >= 0) {
    pongplus_spawn_orb(now);
  }
  if (pu_active && (int32_t)(now - pu_end_ms) >= 0) {
    pu_active = 0;
    pu_next_spawn_ms = now + (uint32_t)pp_rand_range(2000, 4000);
  }

  // Controls:
  // P1 uses BTN3 (up) / BTN4 (down)
  // P2 uses BTN1 (up) / BTN2 (down)
  int p1_up = (cur_mask & BTN3_M) != 0;
  int p1_dn = (cur_mask & BTN4_M) != 0;
  int p2_up = (cur_mask & BTN1_M) != 0;
  int p2_dn = (cur_mask & BTN2_M) != 0;

  // Reverse controls effect
  if (pp1.reverse_end_ms != 0) { int t = p1_up; p1_up = p1_dn; p1_dn = t; }
  if (pp2.reverse_end_ms != 0) { int t = p2_up; p2_up = p2_dn; p2_dn = t; }

  // Paddle speeds
  int sp1 = (int)(2.0f * pp1.speed_factor + 0.5f); if (sp1 < 1) sp1 = 1;
  int sp2 = (int)(2.0f * pp2.speed_factor + 0.5f); if (sp2 < 1) sp2 = 1;

  if (p1_up) pp1.y -= sp1;
  if (p1_dn) pp1.y += sp1;
  if (p2_up) pp2.y -= sp2;
  if (p2_dn) pp2.y += sp2;

  pp1.y = clamp(pp1.y, 0, H - pp1.h);
  pp2.y = clamp(pp2.y, 0, H - pp2.h);

  // --- Ball movement with substeps to avoid tunneling at high speed ---
  float dx = pb.vx * pb_speed_factor;
  float dy = pb.vy * pb_speed_factor;

  float adx = f_abs(dx), ady = f_abs(dy);
  float m = (adx > ady) ? adx : ady;
  int steps = (int)(m / 2.0f) + 1;  // each substep ~<=2px

  float sx = dx / (float)steps;
  float sy = dy / (float)steps;

  for (int s = 0; s < steps; s++)
  {
    pb.x += sx;
    pb.y += sy;

    // Top/bottom bounce
    if (pb.y <= 0) { pb.y = 0; pb.vy = -pb.vy; break; }
    if (pb.y >= (float)(H - pb.size)) { pb.y = (float)(H - pb.size); pb.vy = -pb.vy; break; }

    // Orb collision (ball hits orb)
    if (pu_active) {
      const int R = 3;
      int ox0 = pu_cx - R, ox1 = pu_cx + R;
      int oy0 = pu_cy - R, oy1 = pu_cy + R;

      int bx0 = (int)pb.x;
      int by0 = (int)pb.y;
      int bx1 = bx0 + pb.size - 1;
      int by1 = by0 + pb.size - 1;

      if (bx1 >= ox0 && bx0 <= ox1 && by1 >= oy0 && by0 <= oy1) {
        pu_active = 0;
        pu_next_spawn_ms = now + (uint32_t)pp_rand_range(2000, 4000);

        // collector = who last hit the ball
        pongplus_apply_powerup(pplus_last_touch);
      }
    }

    // Paddle collision LEFT (P1)
    if (pb.vx < 0 && pb.x <= (float)PADDLE_W)
    {
      if ((pb.y + pb.size) >= pp1.y && pb.y <= (pp1.y + pp1.h)) {
        pb.x = (float)PADDLE_W;

        // increase base speed
        pb_base_speed += PB_SPEED_INC;
        if (pb_base_speed > PB_SPEED_MAX) pb_base_speed = PB_SPEED_MAX;

        // compute bounce angle by hit position
        float pcy = pp1.y + pp1.h * 0.5f;
        float bcy = pb.y + pb.size * 0.5f;
        float rel = (bcy - pcy) / (pp1.h * 0.5f);
        if (rel < -1.0f) rel = -1.0f;
        if (rel >  1.0f) rel =  1.0f;

        pb.vx = +pb_base_speed;
        pb.vy = rel * (pb_base_speed * 0.9f);
        if (f_abs(pb.vy) < 0.35f) pb.vy = (pb.vy < 0) ? -0.35f : 0.35f;

        pplus_last_touch = 1;
        break;
      }
    }

    // Paddle collision RIGHT (P2)
    if (pb.vx > 0 && (pb.x + pb.size) >= (float)(W - PADDLE_W))
    {
      if ((pb.y + pb.size) >= pp2.y && pb.y <= (pp2.y + pp2.h)) {
        pb.x = (float)(W - PADDLE_W - pb.size);

        pb_base_speed += PB_SPEED_INC;
        if (pb_base_speed > PB_SPEED_MAX) pb_base_speed = PB_SPEED_MAX;

        float pcy = pp2.y + pp2.h * 0.5f;
        float bcy = pb.y + pb.size * 0.5f;
        float rel = (bcy - pcy) / (pp2.h * 0.5f);
        if (rel < -1.0f) rel = -1.0f;
        if (rel >  1.0f) rel =  1.0f;

        pb.vx = -pb_base_speed;
        pb.vy = rel * (pb_base_speed * 0.9f);
        if (f_abs(pb.vy) < 0.35f) pb.vy = (pb.vy < 0) ? -0.35f : 0.35f;

        pplus_last_touch = 2;
        break;
      }
    }

    // Shield: prevent scoring on that side (bounce off edge)
    if (pp1.shield_end_ms != 0 && pb.x < 0.0f) {
      pb.x = 0.0f;
      if (pb.vx < 0) pb.vx = -pb.vx;
      break;
    }
    if (pp2.shield_end_ms != 0 && (pb.x + pb.size) > (float)W) {
      pb.x = (float)(W - pb.size);
      if (pb.vx > 0) pb.vx = -pb.vx;
      break;
    }

    // Scoring (only when fully out)
    if (pp1.shield_end_ms == 0 && (pb.x + pb.size) < 0.0f) {
      pplus_score2++;
      pongplus_reset_ball(+1);
      break;
    }
    if (pp2.shield_end_ms == 0 && pb.x > (float)W) {
      pplus_score1++;
      pongplus_reset_ball(-1);
      break;
    }
  }
}

static void draw_powerup_orb(int cx, int cy)
{
  // 5-pixel diameter orb (radius = 2)

  // center row
  SSD1306_DrawPixel(cx-3, cy, 1);
  SSD1306_DrawPixel(cx-2, cy, 1);
  SSD1306_DrawPixel(cx-1, cy, 1);
  SSD1306_DrawPixel(cx,   cy, 1);
  SSD1306_DrawPixel(cx+1, cy, 1);
  SSD1306_DrawPixel(cx+2, cy, 1);
  SSD1306_DrawPixel(cx+3, cy, 1);

  // row above
  SSD1306_DrawPixel(cx-3, cy-1, 1);
  SSD1306_DrawPixel(cx-2, cy-1, 1);
  SSD1306_DrawPixel(cx-1, cy-1, 1);
  SSD1306_DrawPixel(cx,   cy-1, 1);
  SSD1306_DrawPixel(cx+1, cy-1, 1);
  SSD1306_DrawPixel(cx+2, cy-1, 1);
  SSD1306_DrawPixel(cx+3, cy-1, 1);

  SSD1306_DrawPixel(cx-1, cy-2, 1);
  SSD1306_DrawPixel(cx,   cy-2, 1);
  SSD1306_DrawPixel(cx+1, cy-2, 1);

  // row below
  SSD1306_DrawPixel(cx-3, cy+1, 1);
  SSD1306_DrawPixel(cx-2, cy+1, 1);
  SSD1306_DrawPixel(cx-1, cy+1, 1);
  SSD1306_DrawPixel(cx,   cy+1, 1);
  SSD1306_DrawPixel(cx+1, cy+1, 1);
  SSD1306_DrawPixel(cx+2, cy+1, 1);
  SSD1306_DrawPixel(cx+3, cy+1, 1);

  SSD1306_DrawPixel(cx-1, cy+2, 1);
  SSD1306_DrawPixel(cx,   cy+2, 1);
  SSD1306_DrawPixel(cx+1, cy+2, 1);


}

static void pongplus_draw(void)
{
  SSD1306_Fill(0);

  // center dashed line
  for (int y = 0; y < H; y += 6) {
    SSD1306_DrawPixel(W/2, y, 1);
    SSD1306_DrawPixel(W/2, y+1, 1);
  }

  // shields indicator (visual)
  if (pp1.shield_end_ms != 0) draw_vline(3, 0, H-1, 1);
  if (pp2.shield_end_ms != 0) draw_vline(W-4, 0, H-1, 1);

  // paddles
  for (int y = 0; y < pp1.h; y++)
    for (int x = 0; x < PADDLE_W; x++)
      SSD1306_DrawPixel(x, pp1.y + y, 1);

  for (int y = 0; y < pp2.h; y++)
    for (int x = 0; x < PADDLE_W; x++)
      SSD1306_DrawPixel(W - PADDLE_W + x, pp2.y + y, 1);

  // orb (blink)
  if (pu_active) {
    if (((HAL_GetTick() / 500u) & 1u) == 0u) {
      draw_powerup_orb(pu_cx, pu_cy);
    }
  }

  // ball (invisible effect)
  if (pb_visible) {
    int bx = (int)(pb.x + 0.5f);
    int by = (int)(pb.y + 0.5f);

    for (int y = 0; y < pb.size; y++)
      for (int x = 0; x < pb.size; x++)
        SSD1306_DrawPixel(bx + x, by + y, 1);
  }

  // score
  char buf[8];

  SSD1306_GotoXY((W/2) - 18, 0);
  sprintf(buf, "%d", pplus_score1);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY((W/2) + 10, 0);
  sprintf(buf, "%d", pplus_score2);
  SSD1306_Puts(buf, &Font_7x10, 1);

  // powerup message
  uint32_t now = HAL_GetTick();
  if (!time_expired(now, pu_msg_end_ms) && pu_msg[0] != 0) {
    int len = (int)strlen(pu_msg);
    int x = (W - len * 7) / 2; // Font_7x10 is 7px wide typically
    if (x < 0) x = 0;
    SSD1306_GotoXY(x, H - 10);
    SSD1306_Puts(pu_msg, &Font_7x10, 1);
  }

  SSD1306_UpdateScreen();
}

//====================== END OF PONG+ =================

static void EnterStopMode(void)
{
  // Optional: turn off OLED to save power (if your lib has it)
  SSD1306_OFF();


  // Avoid immediate wake from a pending EXTI
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON1_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON2_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON3_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON4_Pin);

  HAL_SuspendTick(); // stop SysTick interrupt (saves power)

  // Enter STOP; wake on any enabled EXTI line (button press)
  HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

  // ---- CPU resumes here after wakeup ----
  SystemClock_Config();  // REQUIRED after STOP on STM32
  HAL_ResumeTick();

  // Re-init peripherals that depend on clocks if needed
  MX_I2C1_Init();
  SSD1306_Init();
  SSD1306_ON();

  //game_init();
  score1 = 0;
  score2 = 0;

  // After wake: go back to menu
  app_state = APP_MENU;
  menu_sel = 0;

  // Avoid accidental "select" right after wake if a button is still held
  prev_btn_mask = read_buttons_mask();

  // Draw menu immediately
  menu_draw();

  last_activity_ms = HAL_GetTick();
}


static void update_paddles(void){
  // Replace these with your real input reads:
  // Example: two buttons per paddle
  const int speed = 2;

  int p1_up = BTN_PRESSED(BUTTON3_GPIO_Port, BUTTON3_Pin);
  int p1_dn = BTN_PRESSED(BUTTON4_GPIO_Port, BUTTON4_Pin);
  int p2_up = BTN_PRESSED(BUTTON1_GPIO_Port, BUTTON1_Pin);
  int p2_dn = BTN_PRESSED(BUTTON2_GPIO_Port, BUTTON2_Pin);

  // TODO: read GPIO pins here (HAL_GPIO_ReadPin)

  if (p1_up) p1.y -= speed;
  if (p1_dn) p1.y += speed;
  if (p2_up) p2.y -= speed;
  if (p2_dn) p2.y += speed;

  p1.y = clamp(p1.y, 0, H - p1.h);
  p2.y = clamp(p2.y, 0, H - p2.h);
}

static void draw_frame(void){
	SSD1306_Fill(0);

  // center dashed line
  for (int y=0; y<H; y+=6) {
    // draw 1px vertical segment
	SSD1306_DrawPixel(W/2, y, 1);
	SSD1306_DrawPixel(W/2, y+1, 1);
  }

  // paddles (draw filled rectangles if your lib supports it)
  for(int y=0; y<p1.h; y++)
    for(int x=0; x<PADDLE_W; x++)
    	SSD1306_DrawPixel(x, p1.y + y, 1);

  for(int y=0; y<p2.h; y++)
    for(int x=0; x<PADDLE_W; x++)
    	SSD1306_DrawPixel(W - PADDLE_W + x, p2.y + y, 1);

  // ball
  for(int y=0; y<ball.size; y++)
    for(int x=0; x<ball.size; x++)
    	SSD1306_DrawPixel(ball.x + x, ball.y + y, 1);

  // score
  char buf[4];

  // Left player score (near middle-left)
  SSD1306_GotoXY((W/2) - 18, 0);           // adjust -30 to taste
  sprintf(buf, "%d", score1);
  SSD1306_Puts(buf, &Font_7x10, 1);

  // Right player score (near middle-right)
  SSD1306_GotoXY((W/2) + 10, 0);           // adjust +10 to taste
  sprintf(buf, "%d", score2);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_UpdateScreen();
}

static void update_ball(void){
  ball.x += ball.vx;
  ball.y += ball.vy;

  // Top/bottom bounce
  if (ball.y <= 0) { ball.y = 0; ball.vy = -ball.vy; }
  if (ball.y >= H - ball.size) { ball.y = H - ball.size; ball.vy = -ball.vy; }

  // Paddle collision (left paddle at x=0..PADDLE_W-1)
  if (ball.x <= PADDLE_W) {
    if (ball.y + ball.size >= p1.y && ball.y <= p1.y + p1.h) {
      ball.x = PADDLE_W;      // prevent sticking
      ball.vx = -ball.vx;

      // Add “spin” based on hit position
      int hit = (ball.y + ball.size/2) - (p1.y + p1.h/2);
      if (hit < -3) ball.vy = -2;
      else if (hit > 3) ball.vy = 2;
      else if (ball.vy == 0) ball.vy = 1;

      // ---- SPEED INCREASE (put it HERE) ----
      ball_speed += BALL_SPEED_INC;
      if (ball_speed > BALL_SPEED_MAX) ball_speed = BALL_SPEED_MAX;

      // keep directions, update magnitudes
      ball.vx = (ball.vx > 0) ? (int)(ball_speed + 0.5f) : -(int)(ball_speed + 0.5f);
      ball.vy = (ball.vy > 0) ? (int)(ball_speed/2 + 0.5f) : -(int)(ball_speed/2 + 0.5f);
      if (ball.vy == 0) ball.vy = (ball.vx > 0) ? 1 : -1;
      // --------------------------------------
    }
  }

  // Paddle collision (right paddle at x=W-PADDLE_W..W-1)
  if (ball.x + ball.size >= W - PADDLE_W) {
    if (ball.y + ball.size >= p2.y && ball.y <= p2.y + p2.h) {
      ball.x = W - PADDLE_W - ball.size;
      ball.vx = -ball.vx;

      int hit = (ball.y + ball.size/2) - (p2.y + p2.h/2);
      if (hit < -3) ball.vy = -2;
      else if (hit > 3) ball.vy = 2;
      else if (ball.vy == 0) ball.vy = -1;

      // ---- SPEED INCREASE (put it HERE too) ----
      ball_speed += BALL_SPEED_INC;
      if (ball_speed > BALL_SPEED_MAX) ball_speed = BALL_SPEED_MAX;

      // keep directions, update magnitudes
      ball.vx = (ball.vx > 0) ? (int)(ball_speed + 0.5f) : -(int)(ball_speed + 0.5f);
      ball.vy = (ball.vy > 0) ? (int)(ball_speed/2 + 0.5f) : -(int)(ball_speed/2 + 0.5f);
      if (ball.vy == 0) ball.vy = (ball.vx > 0) ? 1 : -1;
      // ------------------------------------------
    }
  }

  // Scoring
  if (ball.x < 0) { score2++; reset_ball(+1); }
  if (ball.x > W) { score1++; reset_ball(-1); }
}




/* =========================== TETRIS (CENTERED BOARD) ===========================
   Board is centered horizontally.
   NEXT piece on LEFT panel.
   SCORE/LEVEL/LINES on RIGHT panel.
*/
#define TBW 10
#define TBH 16
#define TCELL 4

#define TBOARD_W (TBW * TCELL)
#define TBOARD_H (TBH * TCELL)

#define TBOARD_X ((W - TBOARD_W) / 2)   // centered horizontally (should be 44)
#define TBOARD_Y ((H - TBOARD_H) / 2)   // centered vertically (should be 0)

#define TNEXT_W  (TBOARD_X)             // left area width
#define TSTAT_X  (TBOARD_X + TBOARD_W)  // right area start x

static uint8_t  t_board[TBH][TBW];
static int      t_piece_id = 0;   // 0..6
static int      t_rot = 0;        // 0..3
static int      t_x = 3, t_y = 0; // in board coords (top-left of 4x4)
static int      t_next_id = 0;

static int      t_score = 0;
static int      t_lines = 0;
static int      t_level = 0;
static uint8_t  t_game_over = 0;

static uint32_t t_rng = 1;
static uint32_t t_last_drop_ms = 0;

static uint8_t t_bag[7];
static int t_bag_index = 7;   // start empty so it refills on first use



// Order: I, O, T, S, Z, J, L
static const uint16_t t_base[7] = {
  0x00F0, // I
  0x0660, // O
  0x0072, // T
  0x0036, // S
  0x0063, // Z
  0x0071, // J
  0x0074  // L
};

static uint32_t t_rand_u32(void)
{
  t_rng = t_rng * 1664525u + 1013904223u;
  return t_rng;
}

static void t_refill_bag(void)
{
  // fill bag with 0..6
  for (int i = 0; i < 7; i++)
    t_bag[i] = i;

  // Fisher–Yates shuffle
  for (int i = 6; i > 0; i--)
  {
    int j = (int)(t_rand_u32() % (i + 1));

    uint8_t tmp = t_bag[i];
    t_bag[i] = t_bag[j];
    t_bag[j] = tmp;
  }

  t_bag_index = 0;
}

static int t_rand7(void)
{
  if (t_bag_index >= 7)
    t_refill_bag();

  return t_bag[t_bag_index++];
}

static inline uint8_t t_mask_cell(uint16_t mask, int r, int c)
{
  return (uint8_t)((mask >> (r*4 + c)) & 1u);
}

static uint8_t t_piece_cell(int id, int rot, int r, int c)
{
  int rr = r, cc = c;
  switch (rot & 3) {
    case 0: rr = r;     cc = c;     break;
    case 1: rr = 3 - c; cc = r;     break;
    case 2: rr = 3 - r; cc = 3 - c; break;
    default:rr = c;     cc = 3 - r; break;
  }
  return t_mask_cell(t_base[id], rr, cc);
}

static uint8_t t_collides(int nx, int ny, int nrot)
{
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!t_piece_cell(t_piece_id, nrot, r, c)) continue;

      int bx = nx + c;
      int by = ny + r;

      if (bx < 0 || bx >= TBW || by < 0 || by >= TBH) return 1;
      if (t_board[by][bx]) return 1;
    }
  }
  return 0;
}

static uint32_t t_drop_interval_ms(void)
{
  int base = 600 - (t_level * 40);
  if (base < 120) base = 120;
  return (uint32_t)base;
}

static void t_clear_lines(void)
{
  int cleared = 0;

  for (int y = TBH - 1; y >= 0; y--) {
    int full = 1;
    for (int x = 0; x < TBW; x++) {
      if (t_board[y][x] == 0) { full = 0; break; }
    }

    if (full) {
      cleared++;

      for (int yy = y; yy > 0; yy--) {
        memcpy(t_board[yy], t_board[yy - 1], TBW);
      }
      memset(t_board[0], 0, TBW);

      y++;
    }
  }

  if (cleared > 0) {
    static const int line_score[5] = {0, 40, 100, 300, 1200};
    t_score += line_score[cleared] * (t_level + 1);
    t_lines += cleared;
    t_level = t_lines / 10;
  }
}

static void t_spawn_piece(void)
{
  t_piece_id = t_next_id;
  t_next_id = t_rand7();

  t_rot = 0;
  t_x = (TBW - 4) / 2; // 3
  t_y = 0;

  if (t_collides(t_x, t_y, t_rot)) {
    t_game_over = 1;
  }
}

static void t_lock_piece(void)
{
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!t_piece_cell(t_piece_id, t_rot, r, c)) continue;

      int bx = t_x + c;
      int by = t_y + r;
      if (bx >= 0 && bx < TBW && by >= 0 && by < TBH) {
        t_board[by][bx] = (uint8_t)(t_piece_id + 1);
      }
    }
  }

  t_clear_lines();
  t_spawn_piece();
}

static void t_try_rotate(void)
{
  int nr = (t_rot + 1) & 3;

  if (!t_collides(t_x, t_y, nr)) { t_rot = nr; return; }

  if (!t_collides(t_x - 1, t_y, nr)) { t_x--; t_rot = nr; return; }
  if (!t_collides(t_x + 1, t_y, nr)) { t_x++; t_rot = nr; return; }
  if (!t_collides(t_x - 2, t_y, nr)) { t_x -= 2; t_rot = nr; return; }
  if (!t_collides(t_x + 2, t_y, nr)) { t_x += 2; t_rot = nr; return; }
}

static void tetris_init(void)
{
  memset(t_board, 0, sizeof(t_board));

  t_rng ^= HAL_GetTick();
  t_bag_index = 7;   // force new bag

  t_score = 0;
  t_lines = 0;
  t_level = 0;
  t_game_over = 0;

  t_next_id = t_rand7();
  t_spawn_piece();

  t_last_drop_ms = HAL_GetTick();
}

// Controls (same as your previous version):
// B3 = LEFT, B4 = RIGHT, B1 = ROTATE, B2 = DROP (hold for faster)
static void tetris_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  if (t_game_over) {
    if (pressed_mask & BTN4_M) {
      tetris_init();
    }
    return;
  }

  if (pressed_mask & BTN4_M) { // LEFT
    if (!t_collides(t_x - 1, t_y, t_rot)) t_x--;
  }

  if (pressed_mask & BTN2_M) { // RIGHT
    if (!t_collides(t_x + 1, t_y, t_rot)) t_x++;
  }

  if (pressed_mask & BTN1_M) { // ROTATE
    t_try_rotate();
  }

  if (pressed_mask & BTN3_M) { // DROP step
    if (!t_collides(t_x, t_y + 1, t_rot)) t_y++;
    else t_lock_piece();
  }

  uint32_t now = HAL_GetTick();
  uint32_t interval = t_drop_interval_ms();

  if (cur_mask & BTN3_M) {
    interval /= 6;
    if (interval < 50) interval = 50;
  }

  if (now - t_last_drop_ms >= interval) {
    t_last_drop_ms = now;

    if (!t_collides(t_x, t_y + 1, t_rot)) t_y++;
    else t_lock_piece();
  }
}

static void t_draw_block_px(int px, int py, int cell, uint8_t filled)
{
  if (filled) fill_rect(px + 1, py + 1, cell - 1, cell - 1, 1);
}

static void tetris_draw(void)
{
  SSD1306_Fill(0);

  // Optional panel separators for aesthetics
  if (TBOARD_X >= 3) draw_vline(TBOARD_X - 2, 0, H - 1, 1);
  if (TSTAT_X + 1 < W) draw_vline(TSTAT_X + 1, 0, H - 1, 1);

  // ---- LEFT PANEL: NEXT ----
  SSD1306_GotoXY(6, 0);
  SSD1306_Puts("NEXT", &Font_7x10, 1);

  const int pc = 4; // preview cell size
  const int preview_w = 4 * pc + 2;
  const int preview_h = 4 * pc + 2;

  int nbx = (TNEXT_W - preview_w) / 2;
  if (nbx < 1) nbx = 1;
  int nby = 12;

  draw_rect(nbx, nby, preview_w, preview_h, 1);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      if (!t_piece_cell(t_next_id, 0, r, c)) continue;
      fill_rect(nbx + 1 + c * pc, nby + 1 + r * pc, pc, pc, 1);
    }
  }

  // ---- CENTER: BOARD ----
  // placed blocks
  for (int y = 0; y < TBH; y++) {
    for (int x = 0; x < TBW; x++) {
      if (t_board[y][x]) {
        int px = TBOARD_X + x * TCELL;
        int py = TBOARD_Y + y * TCELL;
        t_draw_block_px(px, py, TCELL, 1);
      }
    }
  }

  // current piece
  if (!t_game_over) {
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        if (!t_piece_cell(t_piece_id, t_rot, r, c)) continue;
        int bx = t_x + c;
        int by = t_y + r;
        if (bx < 0 || bx >= TBW || by < 0 || by >= TBH) continue;

        int px = TBOARD_X + bx * TCELL;
        int py = TBOARD_Y + by * TCELL;
        t_draw_block_px(px, py, TCELL, 1);
      }
    }
  }

  draw_rect(TBOARD_X, TBOARD_Y, TBOARD_W, TBOARD_H, 1);

  // ---- RIGHT PANEL: SCORE/LEVEL/LINES ----
  char buf[20];
  int sx = TSTAT_X + 4;

  SSD1306_GotoXY(sx, 0);
  sprintf(buf, "S:%d", t_score);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY(sx, 12);
  sprintf(buf, "LVL:%d", t_level);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY(sx, 24);
  sprintf(buf, "LN:%d", t_lines);
  SSD1306_Puts(buf, &Font_7x10, 1);

  // Game Over overlay
  if (t_game_over) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(28, 22);
    SSD1306_Puts("GAME OVER", &Font_7x10, 0);

    SSD1306_GotoXY(20, 34);
    SSD1306_Puts("B3 RESTART", &Font_7x10, 0);
  }

  SSD1306_UpdateScreen();
}

/* =========================== MAZE FPS (RAYCAST) =========================== */

#define MAZE_W 16
#define MAZE_H 16
#define MAZE_LEVELS 5

// Render fewer rays for speed (2 = 64 rays). Try 1 for sharper but slower.
#define MAZE_RENDER_STEP 2

// Tuning
#define MAZE_MOVE_SPEED 0.100f   // was 0.050f, now 2x faster
#define MAZE_ROT_SIN    0.0348994967f  // sin(2°)
#define MAZE_ROT_COS    0.9993908270f  // cos(2°)
#define MAZE_PLANE      0.66f    // FOV ~ 66 degrees

static const char * const maze_levels[MAZE_LEVELS][MAZE_H] = {
  { // Level 1
    "################",
    "#..............#",
    "#.###.######.#.#",
    "#.#.......##.#.#",
    "#.#.#####.##.#.#",
    "#.#.#...#....#.#",
    "#.#.#.#.####.#.#",
    "#...#.#....#.#.#",
    "#####.####.#.#.#",
    "#.....#....#.#.#",
    "#.#####.####.#.#",
    "#.#.....#..#.#.#",
    "#.#.#####..#.#.#",
    "#.#.......##...#",
    "#.###########.>#",
    "################",
  },
  { // Level 2
    "################",
    "#...#......#...#",
    "#.#.#.####.#.#.#",
    "#.#...#....#.#.#",
    "#.#####.####.#.#",
    "#.....#....#.#.#",
    "#####.####.#.#.#",
    "#.....#..#.#.#.#",
    "#.#####..#.#.#.#",
    "#.#......#...#.#",
    "#.#.##########.#",
    "#.#............#",
    "#.######.#######",
    "#......#.......#",
    "######.#######>#",
    "################",
  },
  { // Level 3
    "################",
    "#.....#........#",
    "#####.#.######.#",
    "#.....#......#.#",
    "#.##########.#.#",
    "#.#........#.#.#",
    "#.#.######.#.#.#",
    "#.#.#....#.#.#.#",
    "#...#.##.#...#.#",
    "###.#.##.#####.#",
    "#...#....#.....#",
    "#.#######.#.####",
    "#.......#.#....#",
    "#.#####.#.####.#",
    "#.....#.......>#",
    "################",
  },
  { // Level 4
    "################",
    "#........#.....#",
    "#.######.#.###.#",
    "#.#......#...#.#",
    "#.#.########.#.#",
    "#.#........#.#.#",
    "#.########.#.#.#",
    "#........#.#...#",
    "######.#.#.###.#",
    "#......#.#...#.#",
    "#.######.###.#.#",
    "#.#....#.....#.#",
    "#.#.##.#######.#",
    "#...##.........#",
    "##############>#",
    "################",
  },
  { // Level 5
    "################",
    "#...#..........#",
    "#.#.#.########.#",
    "#.#.#........#.#",
    "#.#.########.#.#",
    "#.#........#.#.#",
    "#.########.#.#.#",
    "#........#.#.#.#",
    "########.#.#.#.#",
    "#......#.#.#.#.#",
    "#.####.#.#.#.#.#",
    "#.#....#.#.#.#.#",
    "#.#.####.#.#.#.#",
    "#.#......#...#.#",
    "#.############>#",
    "################",
  }
};

static float mz_posX, mz_posY;
static float mz_dirX, mz_dirY;
static float mz_planeX, mz_planeY;

static uint8_t  mz_win = 0;
static uint32_t mz_win_end_ms = 0;

static uint8_t  mz_level = 0;
static uint8_t  mz_prev_level = 0xFF;
static int      mz_exit_x = 14;
static int      mz_exit_y = 14;
static uint32_t mz_rng = 0x62C4A38Fu;

static inline float mz_absf(float v) { return (v < 0) ? -v : v; }

static uint32_t mz_rand_u32(void)
{
  mz_rng = mz_rng * 1664525u + 1013904223u;
  return mz_rng;
}

static int mz_rand_range(int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + (int)(mz_rand_u32() % (uint32_t)(hi - lo + 1));
}

static inline uint8_t maze_is_wall(int x, int y)
{
  if (x < 0 || x >= MAZE_W || y < 0 || y >= MAZE_H) return 1;
  return (maze_levels[mz_level][y][x] == '#');
}

static void maze_init(void)
{
  mz_rng ^= HAL_GetTick();

  if (MAZE_LEVELS <= 1) {
    mz_level = 0;
  } else {
    uint8_t nl = (uint8_t)mz_rand_range(0, MAZE_LEVELS - 1);

    if (mz_prev_level != 0xFF && nl == mz_prev_level) {
      nl = (uint8_t)((nl + 1u + (mz_rand_u32() % (MAZE_LEVELS - 1))) % MAZE_LEVELS);
    }

    mz_level = nl;
    mz_prev_level = nl;
  }

  // find exit marker '>'
  mz_exit_x = 14;
  mz_exit_y = 14;
  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      if (maze_levels[mz_level][y][x] == '>') {
        mz_exit_x = x;
        mz_exit_y = y;
      }
    }
  }

  // Start in open space
  mz_posX = 1.5f;
  mz_posY = 1.5f;

  // Facing east
  mz_dirX = 1.0f;
  mz_dirY = 0.0f;

  // Camera plane
  mz_planeX = 0.0f;
  mz_planeY = MAZE_PLANE;

  mz_win = 0;
  mz_win_end_ms = 0;
}

static void maze_rotate(float sinA, float cosA)
{
  // rotate direction
  float oldDirX = mz_dirX;
  mz_dirX = mz_dirX * cosA - mz_dirY * sinA;
  mz_dirY = oldDirX * sinA + mz_dirY * cosA;

  // rotate plane
  float oldPlaneX = mz_planeX;
  mz_planeX = mz_planeX * cosA - mz_planeY * sinA;
  mz_planeY = oldPlaneX * sinA + mz_planeY * cosA;
}

static void maze_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  (void)pressed_mask;

  uint32_t now = HAL_GetTick();

  // Win pause
  if (mz_win) {
    if ((int32_t)(now - mz_win_end_ms) >= 0) {
      maze_init();
    }
    return;
  }

  // Controls:
  // BTN3 forward, BTN4 back, BTN1 turn left, BTN2 turn right
  uint8_t forward  = (cur_mask & BTN3_M) != 0;
  uint8_t backward = (cur_mask & BTN4_M) != 0;
  uint8_t turnL    = (cur_mask & BTN1_M) != 0;
  uint8_t turnR    = (cur_mask & BTN2_M) != 0;

  if (turnL) maze_rotate(+MAZE_ROT_SIN, MAZE_ROT_COS);
  if (turnR) maze_rotate(-MAZE_ROT_SIN, MAZE_ROT_COS);

  // Move (with collision)
  if (forward) {
    float nx = mz_posX + mz_dirX * MAZE_MOVE_SPEED;
    float ny = mz_posY + mz_dirY * MAZE_MOVE_SPEED;

    if (!maze_is_wall((int)nx, (int)mz_posY)) mz_posX = nx;
    if (!maze_is_wall((int)mz_posX, (int)ny)) mz_posY = ny;
  }

  if (backward) {
    float nx = mz_posX - mz_dirX * MAZE_MOVE_SPEED;
    float ny = mz_posY - mz_dirY * MAZE_MOVE_SPEED;

    if (!maze_is_wall((int)nx, (int)mz_posY)) mz_posX = nx;
    if (!maze_is_wall((int)mz_posX, (int)ny)) mz_posY = ny;
  }

  // Reached exit cell?
  if ((int)mz_posX == mz_exit_x && (int)mz_posY == mz_exit_y) {
	  mz_win = 1;
    mz_win_end_ms = now + 1800u;
  }
}

static void maze_draw(void)
{
  // Limit rendering to ~30fps (raycast is heavier than Pong/Tetris)
  static uint32_t last_draw_ms = 0;
  uint32_t now = HAL_GetTick();
  if ((uint32_t)(now - last_draw_ms) < 33u) return;
  last_draw_ms = now;

  SSD1306_Fill(0);

  // Simple floor + ceiling dotted texture (adds depth)
  for (int y = 0; y < H/2; y += 8) {
    for (int x = (y & 8) ? 1 : 3; x < W; x += 8) SSD1306_DrawPixel(x, y, 1);
  }
  for (int y = H/2; y < H; y += 4) {
    for (int x = (y & 4) ? 0 : 2; x < W; x += 4) SSD1306_DrawPixel(x, y, 1);
  }

  // Raycast
  for (int x = 0; x < W; x += MAZE_RENDER_STEP)
  {
    float cameraX = 2.0f * ((float)x / (float)W) - 1.0f;
    float rayDirX = mz_dirX + mz_planeX * cameraX;
    float rayDirY = mz_dirY + mz_planeY * cameraX;

    int mapX = (int)mz_posX;
    int mapY = (int)mz_posY;

    float deltaDistX = (rayDirX == 0.0f) ? 1e30f : mz_absf(1.0f / rayDirX);
    float deltaDistY = (rayDirY == 0.0f) ? 1e30f : mz_absf(1.0f / rayDirY);

    int stepX, stepY;
    float sideDistX, sideDistY;

    if (rayDirX < 0) { stepX = -1; sideDistX = (mz_posX - mapX) * deltaDistX; }
    else            { stepX =  1; sideDistX = (mapX + 1.0f - mz_posX) * deltaDistX; }

    if (rayDirY < 0) { stepY = -1; sideDistY = (mz_posY - mapY) * deltaDistY; }
    else            { stepY =  1; sideDistY = (mapY + 1.0f - mz_posY) * deltaDistY; }

    int hit = 0;
    int side = 0;

    // DDA
    for (int i = 0; i < 64 && !hit; i++) {
      if (sideDistX < sideDistY) {
        sideDistX += deltaDistX;
        mapX += stepX;
        side = 0;
      } else {
        sideDistY += deltaDistY;
        mapY += stepY;
        side = 1;
      }
      if (maze_is_wall(mapX, mapY)) hit = 1;
    }

    // Distance
    float perpWallDist;
    if (side == 0) perpWallDist = (mapX - mz_posX + (1 - stepX) * 0.5f) / (rayDirX == 0.0f ? 1e-6f : rayDirX);
    else           perpWallDist = (mapY - mz_posY + (1 - stepY) * 0.5f) / (rayDirY == 0.0f ? 1e-6f : rayDirY);

    if (perpWallDist < 0.12f) perpWallDist = 0.12f;

    int lineHeight = (int)((float)H / perpWallDist);
    int drawStart = -lineHeight/2 + H/2;
    int drawEnd   =  lineHeight/2 + H/2;

    if (drawStart < 0) drawStart = 0;
    if (drawEnd >= H) drawEnd = H - 1;

    // Shading by distance + side (monochrome dithering)
    int shade = 0;
    if (perpWallDist > 5.0f) shade = 3;
    else if (perpWallDist > 3.0f) shade = 2;
    else if (perpWallDist > 1.6f) shade = 1;
    if (side == 1) shade++; // darker if Y-side hit
    if (shade > 3) shade = 3;

    for (int sx = 0; sx < MAZE_RENDER_STEP; sx++)
    {
      int col = x + sx;
      if (col >= W) break;

      // outline edges (helps visibility)
      SSD1306_DrawPixel(col, drawStart, 1);
      SSD1306_DrawPixel(col, drawEnd, 1);

      for (int y = drawStart + 1; y < drawEnd; y++)
      {
        uint8_t on = 0;
        switch (shade) {
          case 0: on = 1; break;
          case 1: on = ((y & 1) == 0); break;
          case 2: on = (((col + y) & 3) == 0); break;
          default:on = (((col + y) & 7) == 0); break;
        }
        if (on) SSD1306_DrawPixel(col, y, 1);
      }
    }
  }

  // Crosshair
  SSD1306_DrawPixel(W/2,   H/2,   1);
  SSD1306_DrawPixel(W/2-1, H/2,   1);
  SSD1306_DrawPixel(W/2+1, H/2,   1);
  SSD1306_DrawPixel(W/2,   H/2-1, 1);
  SSD1306_DrawPixel(W/2,   H/2+1, 1);

  // Win overlay
  if (mz_win) {
    fill_rect(22, 22, 84, 20, 1);
    draw_rect(22, 22, 84, 20, 0);
    SSD1306_GotoXY(34, 27);
    SSD1306_Puts("ESCAPED!", &Font_7x10, 0);
  }

  SSD1306_UpdateScreen();
}

/* =========================== SPACE INVADERS =========================== */

#define INV_ROWS   4
#define INV_COLS   11

#define INV_LEVELS 3

#define INV_CELL_W 10
#define INV_CELL_H 8

#define INV_SPR_W  8
#define INV_SPR_H  5
#define INV_OX     ((INV_CELL_W - INV_SPR_W)/2)  // 1
#define INV_OY     1

#define INV_FORM_W (INV_COLS * INV_CELL_W)
#define INV_FORM_H (INV_ROWS * INV_CELL_H)

#define INV_START_X ((W - INV_FORM_W)/2)   // 9
#define INV_START_Y 12

#define INV_SHIP_W  8
#define INV_SHIP_H  5
#define INV_SHIP_Y  (H - INV_SHIP_H - 1)   // bottom area
#define INV_FIRE_COOLDOWN_MS 180u

typedef struct { int x, y; uint8_t active; } InvBullet;

static uint16_t inv_rowmask[INV_ROWS];
static int inv_x, inv_y;
static int inv_dir;                 // -1 left, +1 right
static uint8_t inv_anim;            // 0/1
static uint32_t inv_next_step_ms;
static uint32_t inv_step_ms;

static int inv_ship_x;
static InvBullet inv_pbullet;
static InvBullet inv_abullet;

static int inv_score;
static int inv_lives;
static uint8_t inv_game_over;
static uint8_t inv_win;

static uint32_t inv_next_shot_ms;
static uint32_t inv_next_fire_ms;
static uint32_t inv_rng = 0x13579BDFu;

static const uint16_t inv_level_masks[INV_LEVELS][INV_ROWS] = {
  {0x07FF, 0x07FF, 0x07FF, 0x07FF}, // level 1: full wall
  {0x03FE, 0x01FC, 0x03FE, 0x01FC}, // level 2: compact diamond-ish
  {0x0555, 0x02AA, 0x0555, 0x02AA}, // level 3: checker
};

static const uint16_t inv_level_step_base[INV_LEVELS] = {
  520u, 430u, 340u
};

static const uint16_t inv_level_shot_min[INV_LEVELS] = {
  500u, 380u, 260u
};

static const uint16_t inv_level_shot_max[INV_LEVELS] = {
  1200u, 900u, 650u
};

static uint8_t  inv_level_id = 0;
static uint8_t  inv_prev_level = 0xFF;
static uint16_t inv_level_base_step_ms = 520u;
static uint16_t inv_level_shot_min_ms  = 500u;
static uint16_t inv_level_shot_max_ms  = 1200u;
static int      inv_level_total_aliens = 44;

static uint32_t inv_rand_u32(void)
{
  inv_rng = inv_rng * 1664525u + 1013904223u;
  return inv_rng;
}

static int inv_rand_range(int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + (int)(inv_rand_u32() % (uint32_t)(hi - lo + 1));
}

static int inv_popcount16(uint16_t v)
{
  int c = 0;
  while (v) { c += (v & 1u); v >>= 1; }
  return c;
}

static int inv_count_alive(void)
{
  int c = 0;
  for (int r = 0; r < INV_ROWS; r++) c += inv_popcount16(inv_rowmask[r]);
  return c;
}

static void inv_bounds(int *minc, int *maxc)
{
  int mn = INV_COLS;
  int mx = -1;
  for (int r = 0; r < INV_ROWS; r++) {
    uint16_t m = inv_rowmask[r];
    for (int c = 0; c < INV_COLS; c++) {
      if (m & (1u << c)) {
        if (c < mn) mn = c;
        if (c > mx) mx = c;
      }
    }
  }
  *minc = mn;
  *maxc = mx;
}

static void inv_draw_sprite8(int x, int y, const uint8_t *rows, int h)
{
  for (int r = 0; r < h; r++) {
    uint8_t bits = rows[r];
    for (int c = 0; c < 8; c++) {
      if (bits & (1u << (7 - c))) {
        int px = x + c;
        int py = y + r;
        if (px >= 0 && px < W && py >= 0 && py < H)
          SSD1306_DrawPixel(px, py, 1);
      }
    }
  }
}

// Alien animation frames (8x5)
static const uint8_t inv_alienA[INV_SPR_H] = {
  0x18, // 00011000
  0x3C, // 00111100
  0x7E, // 01111110
  0xDB, // 11011011
  0x24  // 00100100
};

static const uint8_t inv_alienB[INV_SPR_H] = {
  0x18,
  0x3C,
  0x7E,
  0xBD, // 10111101
  0x42  // 01000010
};

// Ship sprite (8x5)
static const uint8_t inv_shipSpr[INV_SHIP_H] = {
  0x18,
  0x3C,
  0x7E,
  0xFF,
  0x24
};

static void inv_reset_wave(void)
{
  for (int r = 0; r < INV_ROWS; r++) {
    inv_rowmask[r] = inv_level_masks[inv_level_id][r];
  }

  inv_x = INV_START_X;
  inv_y = INV_START_Y;
  inv_dir = +1;
  inv_anim = 0;

  inv_pbullet.active = 0;
  inv_abullet.active = 0;

  inv_step_ms = inv_level_base_step_ms;
  inv_next_step_ms = HAL_GetTick() + inv_step_ms;

  inv_level_total_aliens = inv_count_alive();

  inv_next_shot_ms = HAL_GetTick() +
      (uint32_t)inv_rand_range(inv_level_shot_min_ms, inv_level_shot_max_ms);
}

static void invaders_init(void)
{
  inv_rng ^= HAL_GetTick();

  inv_score = 0;
  inv_lives = 3;
  inv_game_over = 0;
  inv_win = 0;

  inv_ship_x = (W - INV_SHIP_W) / 2;
  inv_next_fire_ms = 0;

  if (INV_LEVELS <= 1) {
    inv_level_id = 0;
  } else {
    uint8_t nl = (uint8_t)inv_rand_range(0, INV_LEVELS - 1);

    if (inv_prev_level != 0xFF && nl == inv_prev_level) {
      nl = (uint8_t)((nl + 1u + (inv_rand_u32() % (INV_LEVELS - 1))) % INV_LEVELS);
    }

    inv_level_id = nl;
    inv_prev_level = nl;
  }

  inv_level_base_step_ms = inv_level_step_base[inv_level_id];
  inv_level_shot_min_ms  = inv_level_shot_min[inv_level_id];
  inv_level_shot_max_ms  = inv_level_shot_max[inv_level_id];

  inv_reset_wave();
}

static void inv_try_fire(uint32_t now, uint8_t cur_mask, uint8_t pressed_mask)
{
  // Fire button: BTN3 (hold to auto fire). Also allow BTN1 tap.
  uint8_t fire_hold = (cur_mask & BTN3_M) != 0;
  uint8_t fire_tap  = (pressed_mask & BTN1_M) != 0;

  if (inv_pbullet.active) return;

  if ((fire_hold && (int32_t)(now - inv_next_fire_ms) >= 0) || fire_tap) {
    inv_pbullet.active = 1;
    inv_pbullet.x = inv_ship_x + INV_SHIP_W / 2;
    inv_pbullet.y = INV_SHIP_Y - 1;
    inv_next_fire_ms = now + INV_FIRE_COOLDOWN_MS;
  }
}

static int inv_check_player_bullet_hit(void)
{
  if (!inv_pbullet.active) return 0;

  // Quick reject: if above/below formation
  if (inv_pbullet.y < inv_y || inv_pbullet.y >= inv_y + INV_FORM_H) return 0;

  int col = (inv_pbullet.x - inv_x) / INV_CELL_W;
  int row = (inv_pbullet.y - inv_y) / INV_CELL_H;

  if (col < 0 || col >= INV_COLS || row < 0 || row >= INV_ROWS) return 0;

  // Make sure bullet is inside sprite bounds (not just cell)
  int sx = inv_x + col * INV_CELL_W + INV_OX;
  int sy = inv_y + row * INV_CELL_H + INV_OY;

  if (inv_pbullet.x < sx || inv_pbullet.x >= sx + INV_SPR_W) return 0;
  if (inv_pbullet.y < sy || inv_pbullet.y >= sy + INV_SPR_H) return 0;

  uint16_t bit = (uint16_t)(1u << col);
  if (inv_rowmask[row] & bit) {
    inv_rowmask[row] &= (uint16_t)~bit;
    inv_score += 10;

    inv_pbullet.active = 0;
    return 1;
  }
  return 0;
}

static void inv_spawn_alien_bullet(uint32_t now)
{
  if (inv_abullet.active) return;
  if ((int32_t)(now - inv_next_shot_ms) < 0) return;

  // pick a random column that has any alive alien
  int tries = 24;
  int col = -1;
  while (tries--) {
    int c = inv_rand_range(0, INV_COLS - 1);
    uint16_t bit = (uint16_t)(1u << c);
    int any = 0;
    for (int r = 0; r < INV_ROWS; r++) if (inv_rowmask[r] & bit) { any = 1; break; }
    if (any) { col = c; break; }
  }

  if (col >= 0) {
    // bottom-most alive in that column
    int row = -1;
    uint16_t bit = (uint16_t)(1u << col);
    for (int r = INV_ROWS - 1; r >= 0; r--) {
      if (inv_rowmask[r] & bit) { row = r; break; }
    }

    if (row >= 0) {
      int sx = inv_x + col * INV_CELL_W + INV_OX;
      int sy = inv_y + row * INV_CELL_H + INV_OY;

      inv_abullet.active = 1;
      inv_abullet.x = sx + INV_SPR_W / 2;
      inv_abullet.y = sy + INV_SPR_H + 1;
    }
  }

  inv_next_shot_ms = now + (uint32_t)inv_rand_range(inv_level_shot_min_ms,
                                                    inv_level_shot_max_ms);
}

static void inv_update_aliens(uint32_t now)
{
  if ((int32_t)(now - inv_next_step_ms) < 0) return;

  int alive = inv_count_alive();
  if (alive <= 0) {
    inv_win = 1;
    return;
  }

  int dead = inv_level_total_aliens - alive;

  // faster as more aliens die, based on chosen level
  int sp = (int)inv_level_base_step_ms - dead * 10;
  if (sp < 80) sp = 80;
  if (sp > (int)inv_level_base_step_ms) sp = (int)inv_level_base_step_ms;
  inv_step_ms = (uint32_t)sp;

  int minc, maxc;
  inv_bounds(&minc, &maxc);
  if (maxc < 0) { inv_win = 1; return; }

  int left_px  = inv_x + minc * INV_CELL_W;
  int right_px = inv_x + (maxc + 1) * INV_CELL_W; // end of right-most cell

  int step = 2 * inv_dir;

  // bounce on edges
  if (inv_dir > 0 && (right_px + step) >= (W - 1)) {
    inv_dir = -1;
    inv_y += 2;
  } else if (inv_dir < 0 && (left_px + step) <= 1) {
    inv_dir = +1;
    inv_y += 2;
  } else {
    inv_x += step;
  }

  inv_anim ^= 1u;

  // If aliens reach player zone -> game over
  if (inv_y + INV_FORM_H >= (INV_SHIP_Y - 2)) {
    inv_game_over = 1;
  }

  inv_next_step_ms = now + inv_step_ms;
}

static void inv_update_bullets(uint32_t now)
{
  (void)now;

  // player bullet
  if (inv_pbullet.active) {
    inv_pbullet.y -= 4;
    if (inv_pbullet.y < 0) inv_pbullet.active = 0;
    else inv_check_player_bullet_hit();
  }

  // alien bullet
  if (inv_abullet.active) {
    inv_abullet.y += 3;
    if (inv_abullet.y >= H) inv_abullet.active = 0;

    // hit ship?
    if (inv_abullet.active) {
      if (inv_abullet.x >= inv_ship_x &&
          inv_abullet.x <  inv_ship_x + INV_SHIP_W &&
          inv_abullet.y >= INV_SHIP_Y &&
          inv_abullet.y <  INV_SHIP_Y + INV_SHIP_H)
      {
        inv_abullet.active = 0;
        inv_lives--;

        // reset ship position on hit
        inv_ship_x = (W - INV_SHIP_W) / 2;
        inv_pbullet.active = 0;

        if (inv_lives <= 0) inv_game_over = 1;
      }
    }
  }
}

static void invaders_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  uint32_t now = HAL_GetTick();

  if (inv_game_over || inv_win) {
    // restart with BTN3 or BTN1
    if ((pressed_mask & BTN3_M) || (pressed_mask & BTN1_M)) {
      invaders_init();
    }
    return;
  }

  // Ship movement (BTN4 left, BTN2 right)
  int sp = 2;
  if (cur_mask & BTN4_M) inv_ship_x -= sp;
  if (cur_mask & BTN2_M) inv_ship_x += sp;
  inv_ship_x = clamp(inv_ship_x, 0, W - INV_SHIP_W);

  // Fire
  inv_try_fire(now, cur_mask, pressed_mask);

  // Alien movement + alien shooting + bullets
  inv_update_aliens(now);
  inv_spawn_alien_bullet(now);
  inv_update_bullets(now);

  // Win?
  if (inv_count_alive() <= 0) inv_win = 1;
}

static void invaders_draw(void)
{
  SSD1306_Fill(0);

  // HUD
  char buf[18];
  SSD1306_GotoXY(0, 0);
  sprintf(buf, "S:%d", inv_score);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY(86, 0);
  sprintf(buf, "L:%d", inv_lives);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY(42, 0);
  sprintf(buf, "LV:%d", inv_level_id + 1);
  SSD1306_Puts(buf, &Font_7x10, 1);

  // Aliens
  const uint8_t *spr = inv_anim ? inv_alienB : inv_alienA;

  for (int r = 0; r < INV_ROWS; r++) {
    uint16_t m = inv_rowmask[r];
    for (int c = 0; c < INV_COLS; c++) {
      if (m & (1u << c)) {
        int x = inv_x + c * INV_CELL_W + INV_OX;
        int y = inv_y + r * INV_CELL_H + INV_OY;
        inv_draw_sprite8(x, y, spr, INV_SPR_H);
      }
    }
  }

  // Ship
  inv_draw_sprite8(inv_ship_x, INV_SHIP_Y, inv_shipSpr, INV_SHIP_H);

  // Bullets
  if (inv_pbullet.active) {
    SSD1306_DrawPixel(inv_pbullet.x, inv_pbullet.y, 1);
    SSD1306_DrawPixel(inv_pbullet.x, inv_pbullet.y + 1, 1);
    SSD1306_DrawPixel(inv_pbullet.x, inv_pbullet.y + 2, 1);
  }
  if (inv_abullet.active) {
    SSD1306_DrawPixel(inv_abullet.x, inv_abullet.y, 1);
    SSD1306_DrawPixel(inv_abullet.x, inv_abullet.y + 1, 1);
    SSD1306_DrawPixel(inv_abullet.x, inv_abullet.y + 2, 1);
  }

  // Ground line (nice visual)
  draw_hline(0, W-1, INV_SHIP_Y - 1, 1);

  // Overlays
  if (inv_game_over) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(28, 22);
    SSD1306_Puts("GAME OVER", &Font_7x10, 0);

    SSD1306_GotoXY(22, 34);
    SSD1306_Puts("B3 RESTART", &Font_7x10, 0);
  }

  if (inv_win) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(36, 22);
    SSD1306_Puts("YOU WIN!", &Font_7x10, 0);

    SSD1306_GotoXY(22, 34);
    SSD1306_Puts("B3 RESTART", &Font_7x10, 0);
  }

  SSD1306_UpdateScreen();
}


/* =========================== BREAKOUT + POWERUPS =========================== */

#define BO_FP 256

#define BO_LEVELS 5

#define BO_COLS 10
#define BO_ROWS 6

#define BO_CELL_W 12
#define BO_CELL_H 6

#define BO_BR_W   10
#define BO_BR_H   4

#define BO_BR_X0  ((W - (BO_COLS * BO_CELL_W)) / 2)  // usually 4
#define BO_BR_Y0  12

#define BO_HUD_Y  0
#define BO_TOP_WALL 10   // keep ball out of HUD font area

#define BO_PADDLE_H 3
#define BO_PADDLE_Y (H - 4)

#define BO_PADDLE_W0 18
#define BO_PADDLE_WMAX 48
#define BO_PADDLE_WMIN 10

#define BO_BALL_SZ 2
#define BO_MAX_BALLS 2

#define BO_MAX_PU 2

typedef struct {
  int32_t x, y;    // Q8.8
  int32_t vx, vy;  // Q8.8 per frame at factor=1
  uint8_t active;
  uint8_t stuck;   // stuck to paddle (sticky)
} BoBall;

typedef struct {
  int x, y;        // pixels (center)
  uint8_t type;
  uint8_t active;
} BoPU;

typedef enum {
  BO_PU_WIDE = 0,
  BO_PU_SLOW,
  BO_PU_STICKY,
  BO_PU_MULTI,
  BO_PU_LIFE,
  BO_PU_COUNT
} BoPUType;

static uint16_t bo_bricks[BO_ROWS];
static BoBall bo_balls[BO_MAX_BALLS];
static BoPU   bo_pu[BO_MAX_PU];

static int bo_paddle_x = 0;
static int bo_paddle_w = BO_PADDLE_W0;

static int bo_score = 0;
static int bo_lives = 3;

static uint8_t bo_game_over = 0;
static uint8_t bo_win = 0;

// power-up timers
static uint32_t bo_wide_end_ms = 0;
static uint32_t bo_sticky_end_ms = 0;

static uint16_t bo_speed_q8 = 256;     // 256 = normal
static uint32_t bo_speed_end_ms = 0;

static uint8_t bo_pu_phase = 0;

static char bo_msg[10] = {0};
static uint32_t bo_msg_end_ms = 0;

static const uint16_t bo_level_masks[BO_LEVELS][BO_ROWS] = {
  {0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x3FF}, // level 1: solid
  {0x2AA, 0x155, 0x2AA, 0x155, 0x2AA, 0x155}, // level 2: checker
  {0x3FF, 0x201, 0x201, 0x201, 0x201, 0x3FF}, // level 3: frame
  {0x020, 0x070, 0x0F8, 0x1FC, 0x3FE, 0x3FF}, // level 4: pyramid
  {0x303, 0x186, 0x0CC, 0x0CC, 0x186, 0x303}, // level 5: cross/diamond
};

static uint8_t bo_level_id = 0;
static uint8_t bo_prev_level = 0xFF;

// RNG
static uint32_t bo_rng = 0xA5A5C3E1u;

static uint32_t bo_rand_u32(void)
{
  bo_rng = bo_rng * 1664525u + 1013904223u;
  return bo_rng;
}

static int bo_rand_range(int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + (int)(bo_rand_u32() % (uint32_t)(hi - lo + 1));
}

static inline int bo_abs_i(int v) { return (v < 0) ? -v : v; }
static inline int32_t bo_abs_fp(int32_t v) { return (v < 0) ? -v : v; }

static inline uint8_t bo_time_expired(uint32_t now, uint32_t end_ms)
{
  return (end_ms != 0u) && ((int32_t)(now - end_ms) >= 0);
}

static void bo_set_msg(const char *s)
{
  strncpy(bo_msg, s, sizeof(bo_msg) - 1);
  bo_msg[sizeof(bo_msg) - 1] = 0;
  bo_msg_end_ms = HAL_GetTick() + 1200u;
}

static int bo_bricks_left(void)
{
  int n = 0;
  for (int r = 0; r < BO_ROWS; r++) {
    uint16_t m = bo_bricks[r];
    while (m) { n += (m & 1u); m >>= 1; }
  }
  return n;
}

static void bo_reset_serve(void)
{
  // deactivate extra balls
  for (int i = 0; i < BO_MAX_BALLS; i++) {
    bo_balls[i].active = 0;
    bo_balls[i].stuck = 0;
  }

  // main ball stuck to paddle
  bo_balls[0].active = 1;
  bo_balls[0].stuck = 1;

  bo_balls[0].vx = 0;
  bo_balls[0].vy = -(330); // upward on launch (Q8.8-ish magnitude)

  // clear falling powerups
  for (int i = 0; i < BO_MAX_PU; i++) bo_pu[i].active = 0;

  // reset slow only (wide/sticky are kept if you want; here we reset slow)
  bo_speed_q8 = 256;
  bo_speed_end_ms = 0;
}

static void breakout_init(void)
{
  bo_rng ^= HAL_GetTick();

  bo_score = 0;
  bo_lives = 3;
  bo_game_over = 0;
  bo_win = 0;

  if (BO_LEVELS <= 1) {
    bo_level_id = 0;
  } else {
    uint8_t nl = (uint8_t)bo_rand_range(0, BO_LEVELS - 1);

    if (bo_prev_level != 0xFF && nl == bo_prev_level) {
      nl = (uint8_t)((nl + 1u + (bo_rand_u32() % (BO_LEVELS - 1))) % BO_LEVELS);
    }

    bo_level_id = nl;
    bo_prev_level = nl;
  }

  for (int r = 0; r < BO_ROWS; r++) {
    bo_bricks[r] = bo_level_masks[bo_level_id][r];
  }

  bo_paddle_w = BO_PADDLE_W0;
  bo_paddle_x = (W - bo_paddle_w) / 2;

  bo_wide_end_ms = 0;
  bo_sticky_end_ms = 0;
  bo_speed_q8 = 256;
  bo_speed_end_ms = 0;

  bo_msg[0] = 0;
  bo_msg_end_ms = 0;

  bo_reset_serve();
}

static void bo_spawn_powerup(int cx, int cy)
{
  // only sometimes
  if ((bo_rand_u32() % 5u) != 0u) return; // ~20%

  // find free slot
  int k = -1;
  for (int i = 0; i < BO_MAX_PU; i++) {
    if (!bo_pu[i].active) { k = i; break; }
  }
  if (k < 0) return;

  // weighted random
  uint32_t r = bo_rand_u32() % 100u;
  uint8_t t;
  if (r < 25) t = BO_PU_WIDE;
  else if (r < 45) t = BO_PU_SLOW;
  else if (r < 65) t = BO_PU_STICKY;
  else if (r < 85) t = BO_PU_MULTI;
  else t = BO_PU_LIFE;

  bo_pu[k].active = 1;
  bo_pu[k].type = t;
  bo_pu[k].x = cx;
  bo_pu[k].y = cy;
}

static void bo_apply_powerup(uint8_t type)
{
  uint32_t now = HAL_GetTick();

  switch ((BoPUType)type)
  {
    case BO_PU_WIDE:
      bo_paddle_w = clamp(BO_PADDLE_W0 * 2, BO_PADDLE_WMIN, BO_PADDLE_WMAX);
      bo_wide_end_ms = now + 8000u;
      bo_set_msg("WIDE");
      break;

    case BO_PU_SLOW:
      bo_speed_q8 = 160;              // ~0.625x speed
      bo_speed_end_ms = now + 7000u;
      bo_set_msg("SLOW");
      break;

    case BO_PU_STICKY:
      bo_sticky_end_ms = now + 8000u;
      bo_set_msg("STICK");
      break;

    case BO_PU_MULTI: {
      // spawn 2nd ball if available
      for (int i = 1; i < BO_MAX_BALLS; i++) {
        if (!bo_balls[i].active && bo_balls[0].active && !bo_balls[0].stuck) {
          bo_balls[i] = bo_balls[0];
          bo_balls[i].vx = -bo_balls[i].vx;
          bo_balls[i].active = 1;
          bo_balls[i].stuck = 0;
          bo_set_msg("MULTI");
          break;
        }
      }
    } break;

    case BO_PU_LIFE:
      if (bo_lives < 9) bo_lives++;
      bo_set_msg("+LIFE");
      break;

    default:
      break;
  }
}

static void bo_update_effects(uint32_t now)
{
  if (bo_time_expired(now, bo_wide_end_ms)) {
    bo_wide_end_ms = 0;
    bo_paddle_w = BO_PADDLE_W0;
  }
  if (bo_time_expired(now, bo_sticky_end_ms)) {
    bo_sticky_end_ms = 0;
  }
  if (bo_time_expired(now, bo_speed_end_ms)) {
    bo_speed_end_ms = 0;
    bo_speed_q8 = 256;
  }

  // keep paddle clamped if width changed
  bo_paddle_x = clamp(bo_paddle_x, 0, W - bo_paddle_w);
}

static void bo_launch_ball(BoBall *b)
{
  // launch upward; angle depends on where you hit on paddle
  int x_px = (int)(b->x >> 8);
  int ball_cx = x_px + (BO_BALL_SZ / 2);

  int pad_cx = bo_paddle_x + bo_paddle_w / 2;
  int dx = ball_cx - pad_cx;

  int half = bo_paddle_w / 2;
  if (half < 1) half = 1;

  int32_t speed = bo_rand_range(300, 360);
  int32_t vx = (int32_t)dx * speed / half;
  if (vx < -speed) vx = -speed;
  if (vx >  speed) vx =  speed;

  int32_t vy = -(speed - (bo_abs_fp(vx) / 3));
  if (vy > -120) vy = -120;

  if (vx == 0) vx = 80;

  b->vx = vx;
  b->vy = vy;
  b->stuck = 0;
}

static int bo_ball_brick_collision(BoBall *b)
{
  int x_px = (int)(b->x >> 8);
  int y_px = (int)(b->y >> 8);

  int x0 = x_px;
  int y0 = y_px;
  int x1 = x_px + BO_BALL_SZ - 1;
  int y1 = y_px + BO_BALL_SZ - 1;

  int bricks_y_end = BO_BR_Y0 + BO_ROWS * BO_CELL_H;

  if (y1 < BO_BR_Y0 || y0 >= bricks_y_end) return 0;

  int c0 = (x0 - BO_BR_X0) / BO_CELL_W;
  int c1 = (x1 - BO_BR_X0) / BO_CELL_W;
  int r0 = (y0 - BO_BR_Y0) / BO_CELL_H;
  int r1 = (y1 - BO_BR_Y0) / BO_CELL_H;

  if (c0 < 0) c0 = 0;
  if (r0 < 0) r0 = 0;
  if (c1 >= BO_COLS) c1 = BO_COLS - 1;
  if (r1 >= BO_ROWS) r1 = BO_ROWS - 1;

  for (int r = r0; r <= r1; r++) {
    for (int c = c0; c <= c1; c++) {
      if ((bo_bricks[r] & (1u << c)) == 0) continue;

      int rx = BO_BR_X0 + c * BO_CELL_W + 1;
      int ry = BO_BR_Y0 + r * BO_CELL_H + 1;

      // AABB overlap
      if (x1 < rx || x0 >= rx + BO_BR_W) continue;
      if (y1 < ry || y0 >= ry + BO_BR_H) continue;

      // remove brick
      bo_bricks[r] &= (uint16_t)~(1u << c);
      bo_score += (BO_ROWS - r) * 5;

      // spawn powerup (from brick center)
      bo_spawn_powerup(rx + BO_BR_W / 2, ry + BO_BR_H / 2);

      // bounce direction by minimal penetration
      int oL = x1 - rx + 1;
      int oR = (rx + BO_BR_W) - x0;
      int oT = y1 - ry + 1;
      int oB = (ry + BO_BR_H) - y0;

      int min = oL;
      if (oR < min) min = oR;
      if (oT < min) min = oT;
      if (oB < min) min = oB;

      if (min == oL || min == oR) b->vx = -b->vx;
      else b->vy = -b->vy;

      return 1;
    }
  }
  return 0;
}

static void breakout_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  uint32_t now = HAL_GetTick();

  if (bo_game_over || bo_win) {
    if ((pressed_mask & BTN3_M) || (pressed_mask & BTN1_M)) {
      breakout_init();
    }
    return;
  }

  bo_update_effects(now);

  // Paddle movement (BTN4 left, BTN2 right)
  int sp = 3;
  if (cur_mask & BTN4_M) bo_paddle_x -= sp;
  if (cur_mask & BTN2_M) bo_paddle_x += sp;
  bo_paddle_x = clamp(bo_paddle_x, 0, W - bo_paddle_w);

  // Powerups fall (30 px/sec-ish)
  bo_pu_phase++;
  uint8_t pu_move = ((bo_pu_phase & 1u) == 0u);

  for (int i = 0; i < BO_MAX_PU; i++) {
    if (!bo_pu[i].active) continue;

    if (pu_move) bo_pu[i].y += 1;

    // collected?
    int px0 = bo_pu[i].x - 2;
    int px1 = bo_pu[i].x + 2;
    int py0 = bo_pu[i].y - 2;
    int py1 = bo_pu[i].y + 2;

    if (px1 >= bo_paddle_x && px0 <= (bo_paddle_x + bo_paddle_w) &&
        py1 >= BO_PADDLE_Y && py0 <= (BO_PADDLE_Y + BO_PADDLE_H))
    {
      bo_apply_powerup(bo_pu[i].type);
      bo_pu[i].active = 0;
      continue;
    }

    if (bo_pu[i].y > H + 3) bo_pu[i].active = 0;
  }

  // Balls
  int active_balls = 0;

  for (int i = 0; i < BO_MAX_BALLS; i++) {
    BoBall *b = &bo_balls[i];
    if (!b->active) continue;
    active_balls++;

    // stuck to paddle (serve or sticky)
    if (b->stuck) {
      int bx = bo_paddle_x + bo_paddle_w/2 - BO_BALL_SZ/2;
      int by = BO_PADDLE_Y - BO_BALL_SZ - 1;

      b->x = (int32_t)bx * BO_FP;
      b->y = (int32_t)by * BO_FP;

      if (pressed_mask & (BTN3_M | BTN1_M)) {
        bo_launch_ball(b);
      }
      continue;
    }

    // move with slow factor
    int32_t dx = (b->vx * (int32_t)bo_speed_q8) >> 8;
    int32_t dy = (b->vy * (int32_t)bo_speed_q8) >> 8;

    b->x += dx;
    b->y += dy;

    int x_px = (int)(b->x >> 8);
    int y_px = (int)(b->y >> 8);

    // walls
    if (x_px <= 0) { b->x = 0; b->vx = -b->vx; }
    if (x_px >= W - BO_BALL_SZ) { b->x = (int32_t)(W - BO_BALL_SZ) * BO_FP; b->vx = -b->vx; }
    if (y_px <= BO_TOP_WALL) { b->y = (int32_t)BO_TOP_WALL * BO_FP; b->vy = -b->vy; }

    // paddle collision (only if moving down)
    x_px = (int)(b->x >> 8);
    y_px = (int)(b->y >> 8);

    if (b->vy > 0) {
      int bx0 = x_px;
      int bx1 = x_px + BO_BALL_SZ - 1;
      int by0 = y_px;
      int by1 = y_px + BO_BALL_SZ - 1;

      if (by1 >= BO_PADDLE_Y && by0 <= BO_PADDLE_Y + BO_PADDLE_H &&
          bx1 >= bo_paddle_x && bx0 <= bo_paddle_x + bo_paddle_w)
      {
        // bounce up
        b->y = (int32_t)(BO_PADDLE_Y - BO_BALL_SZ) * BO_FP;
        b->vy = -bo_abs_fp(b->vy);

        // angle
        int ball_cx = bx0 + BO_BALL_SZ/2;
        int pad_cx = bo_paddle_x + bo_paddle_w/2;
        int dxp = ball_cx - pad_cx;
        int half = bo_paddle_w/2; if (half < 1) half = 1;

        int32_t speed = 330;
        int32_t nvx = (int32_t)dxp * speed / half;
        if (nvx < -speed) nvx = -speed;
        if (nvx >  speed) nvx =  speed;

        b->vx = nvx;

        // sticky active?
        if (bo_sticky_end_ms != 0) {
          b->stuck = 1;
        }
      }
    }

    // brick collision
    bo_ball_brick_collision(b);

    // fell below screen?
    y_px = (int)(b->y >> 8);
    if (y_px >= H) {
      b->active = 0;
    }
  }

  // if no balls left -> lose life and serve again
  int still = 0;
  for (int i = 0; i < BO_MAX_BALLS; i++) if (bo_balls[i].active) still++;

  if (still == 0) {
    bo_lives--;
    if (bo_lives <= 0) bo_game_over = 1;
    else bo_reset_serve();
  }

  // win?
  if (bo_bricks_left() == 0) {
    bo_win = 1;
  }
}

static void bo_draw_powerup(int cx, int cy, uint8_t type)
{
  // 5x5 capsule
  draw_rect(cx - 2, cy - 2, 5, 5, 1);

  // tiny inside mark per type (so you can “read” it)
  switch ((BoPUType)type) {
    case BO_PU_WIDE:
      draw_hline(cx - 1, cx + 1, cy, 1);
      break;
    case BO_PU_SLOW:
      SSD1306_DrawPixel(cx, cy, 1);
      SSD1306_DrawPixel(cx-1, cy+1, 1);
      SSD1306_DrawPixel(cx+1, cy-1, 1);
      break;
    case BO_PU_STICKY:
      fill_rect(cx - 1, cy - 1, 3, 3, 1);
      break;
    case BO_PU_MULTI:
      SSD1306_DrawPixel(cx-1, cy, 1);
      SSD1306_DrawPixel(cx+1, cy, 1);
      break;
    case BO_PU_LIFE:
      SSD1306_DrawPixel(cx, cy-1, 1);
      SSD1306_DrawPixel(cx, cy, 1);
      SSD1306_DrawPixel(cx, cy+1, 1);
      SSD1306_DrawPixel(cx-1, cy, 1);
      SSD1306_DrawPixel(cx+1, cy, 1);
      break;
    default:
      break;
  }
}

static void breakout_draw(void)
{
  SSD1306_Fill(0);

  // HUD
  char buf[16];
  SSD1306_GotoXY(0, 0);
  sprintf(buf, "S:%d", bo_score);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY(92, 0);
  sprintf(buf, "L:%d", bo_lives);
  SSD1306_Puts(buf, &Font_7x10, 1);

  uint32_t now = HAL_GetTick();

  if (bo_time_expired(now, bo_msg_end_ms) || !bo_msg[0]) {
    SSD1306_GotoXY(42, 0);
    sprintf(buf, "LV:%d", bo_level_id + 1);
    SSD1306_Puts(buf, &Font_7x10, 1);
  }

  // short center message (doesn't block play area)
  if (!bo_time_expired(now, bo_msg_end_ms) && bo_msg[0]) {
    int len = (int)strlen(bo_msg);
    int x = (W - len * 7) / 2;
    SSD1306_GotoXY(x, 0);
    SSD1306_Puts(bo_msg, &Font_7x10, 1);
  }

  // Bricks
  for (int r = 0; r < BO_ROWS; r++) {
    for (int c = 0; c < BO_COLS; c++) {
      if (bo_bricks[r] & (1u << c)) {
        int rx = BO_BR_X0 + c * BO_CELL_W + 1;
        int ry = BO_BR_Y0 + r * BO_CELL_H + 1;

        // alternate style by row (looks nicer on mono OLED)
        if (r & 1) draw_rect(rx, ry, BO_BR_W, BO_BR_H, 1);
        else       fill_rect(rx, ry, BO_BR_W, BO_BR_H, 1);
      }
    }
  }

  // Paddle
  fill_rect(bo_paddle_x, BO_PADDLE_Y, bo_paddle_w, BO_PADDLE_H, 1);

  // Balls
  for (int i = 0; i < BO_MAX_BALLS; i++) {
    if (!bo_balls[i].active) continue;
    int bx = (int)(bo_balls[i].x >> 8);
    int by = (int)(bo_balls[i].y >> 8);

    fill_rect(bx, by, BO_BALL_SZ, BO_BALL_SZ, 1);
  }

  // Powerups
  for (int i = 0; i < BO_MAX_PU; i++) {
    if (!bo_pu[i].active) continue;
    bo_draw_powerup(bo_pu[i].x, bo_pu[i].y, bo_pu[i].type);
  }

  // Overlays
  if (bo_game_over) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(28, 22);
    SSD1306_Puts("GAME OVER", &Font_7x10, 0);

    SSD1306_GotoXY(22, 34);
    SSD1306_Puts("B3 RESTART", &Font_7x10, 0);
  }

  if (bo_win) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(36, 22);
    SSD1306_Puts("YOU WIN!", &Font_7x10, 0);

    SSD1306_GotoXY(22, 34);
    SSD1306_Puts("B3 RESTART", &Font_7x10, 0);
  }

  SSD1306_UpdateScreen();
}

/* =========================== ROGUE80 (COMPACT ASCII ROGUELIKE) =========================== */
/*
   OLED-friendly Rogue-inspired mode.

   Controls:
   B1 = UP
   B2 = DOWN
   B3 = LEFT
   B4 = RIGHT

   Walk into M to attack.
   $ = gold
   ! = potion
   > = stairs to next floor
   Hold all 4 buttons = back to menu (handled by your main loop)
*/

#define ROG_W 32
#define ROG_H 16

#define ROG_VIEW_W 18   // 18 chars * 7 px  = 126 px
#define ROG_VIEW_H 5    // 5 rows  * 10 px = 50 px, with top HUD row

#define ROG_MAX_ROOMS 6
#define ROG_MAX_MON   12
#define ROG_MAX_ITEM  12

typedef struct { int x, y, w, h, cx, cy; } RogRoom;
typedef struct { int x, y; uint8_t alive; } RogMonster;
typedef struct { int x, y; uint8_t type; uint8_t active; } RogItem;

enum { ROG_TILE_WALL = 0, ROG_TILE_FLOOR = 1 };
enum { ROG_ITEM_GOLD = 0, ROG_ITEM_POTION = 1 };

static uint8_t rog_map[ROG_H][ROG_W];
static uint8_t rog_seen[ROG_H][ROG_W];

static RogRoom    rog_rooms[ROG_MAX_ROOMS];
static RogMonster rog_mon[ROG_MAX_MON];
static RogItem    rog_item[ROG_MAX_ITEM];

static int rog_room_count = 0;
static int rog_mon_count  = 0;
static int rog_item_count = 0;

static int rog_px = 0, rog_py = 0;
static int rog_stair_x = 0, rog_stair_y = 0;

static int rog_hp    = 5;
static int rog_gold  = 0;
static int rog_depth = 1;

static uint8_t  rog_game_over = 0;
static uint32_t rog_rng = 0x51A7E123u;

static int rog_absi(int v) { return (v < 0) ? -v : v; }

static uint32_t rog_rand_u32(void)
{
  rog_rng = rog_rng * 1664525u + 1013904223u;
  return rog_rng;
}

static int rog_rand_range(int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + (int)(rog_rand_u32() % (uint32_t)(hi - lo + 1));
}

static int rog_monster_at(int x, int y)
{
  for (int i = 0; i < rog_mon_count; i++) {
    if (rog_mon[i].alive && rog_mon[i].x == x && rog_mon[i].y == y) return i;
  }
  return -1;
}

static int rog_item_at(int x, int y)
{
  for (int i = 0; i < rog_item_count; i++) {
    if (rog_item[i].active && rog_item[i].x == x && rog_item[i].y == y) return i;
  }
  return -1;
}

static uint8_t rog_walkable(int x, int y)
{
  if (x < 0 || x >= ROG_W || y < 0 || y >= ROG_H) return 0;
  return (rog_map[y][x] == ROG_TILE_FLOOR);
}

static uint8_t rog_cell_free(int x, int y)
{
  if (!rog_walkable(x, y)) return 0;
  if (x == rog_px && y == rog_py) return 0;
  if (x == rog_stair_x && y == rog_stair_y) return 0;
  if (rog_monster_at(x, y) >= 0) return 0;
  if (rog_item_at(x, y) >= 0) return 0;
  return 1;
}

static void rog_carve_room(int x, int y, int w, int h)
{
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      rog_map[yy][xx] = ROG_TILE_FLOOR;
}

static void rog_carve_h(int x0, int x1, int y)
{
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; x++) rog_map[y][x] = ROG_TILE_FLOOR;
}

static void rog_carve_v(int x, int y0, int y1)
{
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; y++) rog_map[y][x] = ROG_TILE_FLOOR;
}

static uint8_t rog_room_overlaps(int x, int y, int w, int h)
{
  for (int i = 0; i < rog_room_count; i++) {
    RogRoom *r = &rog_rooms[i];

    if ((x - 1) < (r->x + r->w + 1) && (x + w + 1) > r->x &&
        (y - 1) < (r->y + r->h + 1) && (y + h + 1) > r->y)
      return 1;
  }
  return 0;
}

static void rog_reveal(void)
{
  for (int y = 0; y < ROG_H; y++) {
    for (int x = 0; x < ROG_W; x++) {
      if (rog_absi(x - rog_px) <= 6 && rog_absi(y - rog_py) <= 4)
        rog_seen[y][x] = 1;
    }
  }
}

static uint8_t rog_visible(int x, int y)
{
  return (rog_absi(x - rog_px) <= 6 && rog_absi(y - rog_py) <= 4);
}

static void rog_place_on_floor(int *out_x, int *out_y)
{
  for (int tries = 0; tries < 120; tries++) {
    RogRoom *r = &rog_rooms[rog_rand_range(0, rog_room_count - 1)];

    int x = rog_rand_range(r->x, r->x + r->w - 1);
    int y = rog_rand_range(r->y, r->y + r->h - 1);

    if (rog_cell_free(x, y)) {
      *out_x = x;
      *out_y = y;
      return;
    }
  }

  for (int y = 1; y < ROG_H - 1; y++) {
    for (int x = 1; x < ROG_W - 1; x++) {
      if (rog_cell_free(x, y)) {
        *out_x = x;
        *out_y = y;
        return;
      }
    }
  }

  *out_x = 1;
  *out_y = 1;
}

static void rog_build_floor(void)
{
  for (;;)
  {
    memset(rog_map,  0, sizeof(rog_map));
    memset(rog_seen, 0, sizeof(rog_seen));
    memset(rog_mon,  0, sizeof(rog_mon));
    memset(rog_item, 0, sizeof(rog_item));

    rog_mon_count  = 0;
    rog_item_count = 0;
    rog_room_count = 0;

    for (int tries = 0; tries < 48 && rog_room_count < ROG_MAX_ROOMS; tries++) {
      int rw = rog_rand_range(4, 8);
      int rh = rog_rand_range(3, 5);
      int rx = rog_rand_range(1, ROG_W - rw - 2);
      int ry = rog_rand_range(1, ROG_H - rh - 2);

      if (rog_room_overlaps(rx, ry, rw, rh)) continue;

      rog_carve_room(rx, ry, rw, rh);

      RogRoom *nr = &rog_rooms[rog_room_count];
      nr->x  = rx;
      nr->y  = ry;
      nr->w  = rw;
      nr->h  = rh;
      nr->cx = rx + rw / 2;
      nr->cy = ry + rh / 2;

      if (rog_room_count > 0) {
        RogRoom *pr = &rog_rooms[rog_room_count - 1];

        if (rog_rand_u32() & 1u) {
          rog_carve_h(pr->cx, nr->cx, pr->cy);
          rog_carve_v(nr->cx, pr->cy, nr->cy);
        } else {
          rog_carve_v(pr->cx, pr->cy, nr->cy);
          rog_carve_h(pr->cx, nr->cx, nr->cy);
        }
      }

      rog_room_count++;
    }

    if (rog_room_count >= 2) break;

    // Try again with a different RNG state
    rog_rng ^= 0x9E3779B9u;
  }

  rog_px = rog_rooms[0].cx;
  rog_py = rog_rooms[0].cy;

  rog_stair_x = rog_rooms[rog_room_count - 1].cx;
  rog_stair_y = rog_rooms[rog_room_count - 1].cy;

  // Monsters scale with depth
  int mon_target = 3 + rog_depth;
  if (mon_target > ROG_MAX_MON) mon_target = ROG_MAX_MON;

  for (int i = 0; i < mon_target; i++) {
    int x, y;
    rog_place_on_floor(&x, &y);

    rog_mon[rog_mon_count].x = x;
    rog_mon[rog_mon_count].y = y;
    rog_mon[rog_mon_count].alive = 1;
    rog_mon_count++;
  }

  // Gold
  int gold_target = 3 + (rog_depth / 2);
  if (gold_target > (ROG_MAX_ITEM - 1)) gold_target = (ROG_MAX_ITEM - 1);

  for (int i = 0; i < gold_target; i++) {
    int x, y;
    rog_place_on_floor(&x, &y);

    rog_item[rog_item_count].x = x;
    rog_item[rog_item_count].y = y;
    rog_item[rog_item_count].type = ROG_ITEM_GOLD;
    rog_item[rog_item_count].active = 1;
    rog_item_count++;
  }

  // One potion
  {
    int x, y;
    rog_place_on_floor(&x, &y);

    rog_item[rog_item_count].x = x;
    rog_item[rog_item_count].y = y;
    rog_item[rog_item_count].type = ROG_ITEM_POTION;
    rog_item[rog_item_count].active = 1;
    rog_item_count++;
  }

  rog_reveal();
}

static void rogue_init(void)
{
  rog_rng ^= HAL_GetTick();

  rog_hp = 5;
  rog_gold = 0;
  rog_depth = 1;
  rog_game_over = 0;

  rog_build_floor();
}

static void rog_next_floor(void)
{
  rog_depth++;
  if (rog_hp < 9) rog_hp++;   // tiny reward per level
  rog_build_floor();
}

static void rog_monsters_turn(void)
{
  for (int i = 0; i < rog_mon_count; i++) {
    RogMonster *m = &rog_mon[i];
    if (!m->alive) continue;

    int dx  = rog_px - m->x;
    int dy  = rog_py - m->y;
    int adx = rog_absi(dx);
    int ady = rog_absi(dy);

    // Attack if adjacent in 4 directions
    if ((adx + ady) == 1) {
      rog_hp--;
      if (rog_hp <= 0) {
        rog_hp = 0;
        rog_game_over = 1;
        return;
      }
      continue;
    }

    int sx = (dx > 0) - (dx < 0);
    int sy = (dy > 0) - (dy < 0);

    int tryx = m->x;
    int tryy = m->y;

    // Chase player when close
    if ((adx + ady) <= 8) {
      if (adx >= ady) {
        tryx = m->x + sx;
        tryy = m->y;

        if (!rog_walkable(tryx, tryy) || rog_monster_at(tryx, tryy) >= 0 ||
            (tryx == rog_px && tryy == rog_py)) {
          tryx = m->x;
          tryy = m->y + sy;
        }
      } else {
        tryx = m->x;
        tryy = m->y + sy;

        if (!rog_walkable(tryx, tryy) || rog_monster_at(tryx, tryy) >= 0 ||
            (tryx == rog_px && tryy == rog_py)) {
          tryx = m->x + sx;
          tryy = m->y;
        }
      }
    }
    // Occasionally wander
    else if ((rog_rand_u32() & 3u) == 0u) {
      static const int dd[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
      int k = rog_rand_range(0, 3);
      tryx = m->x + dd[k][0];
      tryy = m->y + dd[k][1];
    }

    if (tryx == m->x && tryy == m->y) continue;

    if (rog_walkable(tryx, tryy) &&
        rog_monster_at(tryx, tryy) < 0 &&
        !(tryx == rog_stair_x && tryy == rog_stair_y) &&
        !(tryx == rog_px && tryy == rog_py))
    {
      m->x = tryx;
      m->y = tryy;
    }
  }
}

static void rog_try_move(int dx, int dy)
{
  if (rog_game_over) return;

  int nx = rog_px + dx;
  int ny = rog_py + dy;

  if (!rog_walkable(nx, ny)) return;

  // Attack by walking into monster
  int mi = rog_monster_at(nx, ny);
  if (mi >= 0) {
    rog_mon[mi].alive = 0;
    rog_monsters_turn();
    rog_reveal();
    return;
  }

  // Move
  rog_px = nx;
  rog_py = ny;

  // Pickup
  int ii = rog_item_at(rog_px, rog_py);
  if (ii >= 0) {
    if (rog_item[ii].type == ROG_ITEM_GOLD) {
      rog_gold++;
    } else if (rog_item[ii].type == ROG_ITEM_POTION) {
      rog_hp += 2;
      if (rog_hp > 9) rog_hp = 9;
    }
    rog_item[ii].active = 0;
  }

  // Next floor
  if (rog_px == rog_stair_x && rog_py == rog_stair_y) {
    rog_next_floor();
    return;
  }

  rog_monsters_turn();
  rog_reveal();
}

static void rogue_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  (void)cur_mask;

  if (rog_game_over) {
    if (pressed_mask & BTN3_M) {
      rogue_init();
    }
    return;
  }

  if      (pressed_mask & BTN1_M) rog_try_move( 0, -1);
  else if (pressed_mask & BTN2_M) rog_try_move( 0, +1);
  else if (pressed_mask & BTN3_M) rog_try_move(-1,  0);
  else if (pressed_mask & BTN4_M) rog_try_move(+1,  0);
}

static void rogue_draw(void)
{
  SSD1306_Fill(0);

  // HUD
  char hud[20];
  sprintf(hud, "H:%d G:%d D:%d", rog_hp, rog_gold, rog_depth);
  SSD1306_GotoXY(0, 0);
  SSD1306_Puts(hud, &Font_7x10, 1);

  // Camera centered around player
  int cam_x = rog_px - (ROG_VIEW_W / 2);
  int cam_y = rog_py - (ROG_VIEW_H / 2);

  cam_x = clamp(cam_x, 0, ROG_W - ROG_VIEW_W);
  cam_y = clamp(cam_y, 0, ROG_H - ROG_VIEW_H);

  for (int vy = 0; vy < ROG_VIEW_H; vy++) {
    char row[ROG_VIEW_W + 1];

    for (int vx = 0; vx < ROG_VIEW_W; vx++) {
      int mx = cam_x + vx;
      int my = cam_y + vy;
      char ch = ' ';

      if (rog_seen[my][mx]) {
        ch = rog_map[my][mx] ? '.' : '#';

        if (mx == rog_stair_x && my == rog_stair_y) ch = '>';

        if (rog_visible(mx, my)) {
          int ii = rog_item_at(mx, my);
          if (ii >= 0)
            ch = (rog_item[ii].type == ROG_ITEM_GOLD) ? '$' : '!';

          int mi = rog_monster_at(mx, my);
          if (mi >= 0)
            ch = 'M';
        }
      }

      if (mx == rog_px && my == rog_py) ch = '@';
      row[vx] = ch;
    }

    row[ROG_VIEW_W] = 0;
    SSD1306_GotoXY(0, 10 + vy * 10);
    SSD1306_Puts(row, &Font_7x10, 1);
  }

  if (rog_game_over) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(28, 22);
    SSD1306_Puts("GAME OVER", &Font_7x10, 0);

    SSD1306_GotoXY(18, 34);
    SSD1306_Puts("B3 RESTART", &Font_7x10, 0);
  }

  SSD1306_UpdateScreen();
}

/* =========================== FLAPPY =========================== */

#define FB_FP          256

#define FB_PLAY_Y      10
#define FB_BIRD_X      24
#define FB_BIRD_W      5
#define FB_BIRD_H      4

#define FB_PIPE_W      12
#define FB_GAP_H       18
#define FB_PIPE_COUNT  3
#define FB_SPACING     44

#define FB_GRAVITY     18     // Q8.8 per frame
#define FB_FLAP_VY    -210    // Q8.8
#define FB_MAX_FALL    260    // Q8.8

typedef struct {
  int x;
  int gap_y;      // top of opening
  uint8_t scored;
} FlappyPipe;

static FlappyPipe fb_pipes[FB_PIPE_COUNT];

static int32_t fb_bird_y_q8 = 0;
static int32_t fb_bird_vy_q8 = 0;

static int fb_score = 0;
static int fb_best  = 0;

static uint8_t fb_started = 0;
static uint8_t fb_game_over = 0;
static uint8_t fb_scroll_div = 0;

static uint32_t fb_rng = 0x7F4A7C15u;

static uint32_t fb_rand_u32(void)
{
  fb_rng = fb_rng * 1664525u + 1013904223u;
  return fb_rng;
}

static int fb_rand_range(int lo, int hi)
{
  if (hi <= lo) return lo;
  return lo + (int)(fb_rand_u32() % (uint32_t)(hi - lo + 1));
}

static int fb_gap_min(void) { return FB_PLAY_Y + 4; }
static int fb_gap_max(void) { return H - FB_GAP_H - 4; }

static int fb_rightmost_x(void)
{
  int mx = fb_pipes[0].x;
  for (int i = 1; i < FB_PIPE_COUNT; i++) {
    if (fb_pipes[i].x > mx) mx = fb_pipes[i].x;
  }
  return mx;
}

static void flappy_reset_pipe(int i, int x)
{
  fb_pipes[i].x = x;
  fb_pipes[i].gap_y = fb_rand_range(fb_gap_min(), fb_gap_max());
  fb_pipes[i].scored = 0;
}

static void fb_die(void)
{
  fb_game_over = 1;
  if (fb_score > fb_best) fb_best = fb_score;
}

static void flappy_init(void)
{
  fb_rng ^= HAL_GetTick();

  fb_score = 0;
  fb_started = 0;
  fb_game_over = 0;
  fb_scroll_div = 0;

  fb_bird_y_q8 = (int32_t)(FB_PLAY_Y + ((H - FB_PLAY_Y) / 2) - (FB_BIRD_H / 2)) * FB_FP;
  fb_bird_vy_q8 = 0;

  int x0 = W + 18;
  for (int i = 0; i < FB_PIPE_COUNT; i++) {
    flappy_reset_pipe(i, x0 + i * FB_SPACING);
  }
}

static void flappy_update(uint8_t pressed_mask, uint8_t cur_mask)
{
  (void)cur_mask;

  uint8_t flap = (pressed_mask & (BTN1_M | BTN3_M)) != 0;

  if (fb_game_over) {
    if (flap) flappy_init();
    return;
  }

  if (!fb_started) {
    if (flap) {
      fb_started = 1;
      fb_bird_vy_q8 = FB_FLAP_VY;
    }
    return;
  }

  // flap
  if (flap) {
    fb_bird_vy_q8 = FB_FLAP_VY;
  }

  // bird physics
  fb_bird_vy_q8 += FB_GRAVITY;
  if (fb_bird_vy_q8 > FB_MAX_FALL) fb_bird_vy_q8 = FB_MAX_FALL;

  fb_bird_y_q8 += fb_bird_vy_q8;

  int by = (int)(fb_bird_y_q8 >> 8);

  // top / bottom collision
  if (by < FB_PLAY_Y) {
    fb_bird_y_q8 = (int32_t)FB_PLAY_Y * FB_FP;
    fb_die();
    return;
  }

  if (by > (H - FB_BIRD_H)) {
    fb_bird_y_q8 = (int32_t)(H - FB_BIRD_H) * FB_FP;
    fb_die();
    return;
  }

  // move pipes every 2nd frame (~30 px/sec at 60fps)
  if (((fb_scroll_div++) & 1u) == 0u) {
    for (int i = 0; i < FB_PIPE_COUNT; i++) {
      fb_pipes[i].x -= 1;
    }

    for (int i = 0; i < FB_PIPE_COUNT; i++) {
      if ((fb_pipes[i].x + FB_PIPE_W) < 0) {
        flappy_reset_pipe(i, fb_rightmost_x() + FB_SPACING);
      }
    }
  }

  // score + collision
  int bx0 = FB_BIRD_X;
  int bx1 = FB_BIRD_X + FB_BIRD_W - 1;
  int by0 = (int)(fb_bird_y_q8 >> 8);
  int by1 = by0 + FB_BIRD_H - 1;

  for (int i = 0; i < FB_PIPE_COUNT; i++) {
    int px0 = fb_pipes[i].x;
    int px1 = fb_pipes[i].x + FB_PIPE_W - 1;
    int gy0 = fb_pipes[i].gap_y;
    int gy1 = fb_pipes[i].gap_y + FB_GAP_H - 1;

    if (!fb_pipes[i].scored && px1 < FB_BIRD_X) {
      fb_pipes[i].scored = 1;
      fb_score++;
      if (fb_score > fb_best) fb_best = fb_score;
    }

    if (bx1 >= px0 && bx0 <= px1) {
      if (by0 < gy0 || by1 > gy1) {
        fb_die();
        return;
      }
    }
  }
}

static void fb_draw_bird(int x, int y)
{
  // simple tiny bird sprite
  fill_rect(x,     y + 1, 4, 3, 1);   // body
  SSD1306_DrawPixel(x + 1, y,     1); // top wing
  SSD1306_DrawPixel(x + 2, y,     1);
  SSD1306_DrawPixel(x + 4, y + 2, 1); // beak
}

static void flappy_draw(void)
{
  SSD1306_Fill(0);

  // HUD
  char buf[20];
  SSD1306_GotoXY(0, 0);
  sprintf(buf, "S:%d", fb_score);
  SSD1306_Puts(buf, &Font_7x10, 1);

  SSD1306_GotoXY(84, 0);
  sprintf(buf, "B:%d", fb_best);
  SSD1306_Puts(buf, &Font_7x10, 1);

  draw_hline(0, W - 1, FB_PLAY_Y - 1, 1);

  // pipes
  for (int i = 0; i < FB_PIPE_COUNT; i++) {
    int x = fb_pipes[i].x;
    int gy = fb_pipes[i].gap_y;

    // top pipe
    if (gy > FB_PLAY_Y) {
      fill_rect(x, FB_PLAY_Y, FB_PIPE_W, gy - FB_PLAY_Y, 1);
    }

    // bottom pipe
    int by = gy + FB_GAP_H;
    if (by < H) {
      fill_rect(x, by, FB_PIPE_W, H - by, 1);
    }
  }

  // bird
  fb_draw_bird(FB_BIRD_X, (int)(fb_bird_y_q8 >> 8));

  if (!fb_started && !fb_game_over) {
    fill_rect(18, 20, 92, 24, 1);
    draw_rect(18, 20, 92, 24, 0);

    SSD1306_GotoXY(34, 22);
    SSD1306_Puts("FLAPPY", &Font_7x10, 0);

    SSD1306_GotoXY(24, 34);
    SSD1306_Puts("B1/B3 GO", &Font_7x10, 0);
  }

  if (fb_game_over) {
    fill_rect(18, 18, 92, 28, 1);
    draw_rect(18, 18, 92, 28, 0);

    SSD1306_GotoXY(28, 22);
    SSD1306_Puts("GAME OVER", &Font_7x10, 0);

    SSD1306_GotoXY(14, 34);
    SSD1306_Puts("B1/B3 RETRY", &Font_7x10, 0);
  }

  SSD1306_UpdateScreen();
}


// ======================== MAIN ================================
int main(void)
{


  HAL_Init();
  SystemClock_Config();

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
  SSD1306_Init();
  SSD1306_Clear();
  // Start in menu
  app_state = APP_MENU;
  menu_sel = 0;
  prev_btn_mask = read_buttons_mask();
  menu_draw();

  uint32_t last = HAL_GetTick();
  last_activity_ms = HAL_GetTick();

  while (1)
  {
    uint8_t cur_mask = read_buttons_mask();

    // Activity tracking (press OR hold counts as activity)
    if (cur_mask) last_activity_ms = HAL_GetTick();

    // STOP after 30s idle
    if ((HAL_GetTick() - last_activity_ms) >= 30000u) {
      EnterStopMode();
      // EnterStopMode() returns after wake and re-draws menu
      last = HAL_GetTick();
      continue;
    }

    // Edge detect presses
    uint8_t pressed = (uint8_t)(cur_mask & (uint8_t)(~prev_btn_mask));
    prev_btn_mask = cur_mask;

    uint32_t now = HAL_GetTick();
    if (now - last >= FRAME_MS)
    {
      last += FRAME_MS;

      switch (app_state)
      {
        case APP_MENU:
          menu_update(pressed);
          menu_draw();
          break;

        case APP_PONG:
          // exit back to menu: hold ALL 4 buttons for ~1 second
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;  // avoid immediate select
            menu_draw();
            break;
          }
          update_paddles();
          update_ball();
          draw_frame();
          break;

        case APP_PONG_PLUS:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          pongplus_update(pressed, cur_mask);
          pongplus_draw();
          break;

        case APP_TETRIS:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          tetris_update(pressed, cur_mask);
          tetris_draw();
          break;

        case APP_MAZE:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          maze_update(pressed, cur_mask);
          maze_draw();
          break;

        case APP_INVADERS:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          invaders_update(pressed, cur_mask);
          invaders_draw();
          break;

        case APP_BREAKOUT:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          breakout_update(pressed, cur_mask);
          breakout_draw();
          break;

        case APP_ROGUE:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          rogue_update(pressed, cur_mask);
          rogue_draw();
          break;

        case APP_FLAPPY:
          if (exit_combo_detect(cur_mask)) {
            app_state = APP_MENU;
            prev_btn_mask = cur_mask;
            menu_draw();
            break;
          }
          flappy_update(pressed, cur_mask);
          flappy_draw();
          break;

        default:
          app_state = APP_MENU;
          menu_draw();
          break;
      }
    }
  }
}
/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
  PeriphClkInit.I2c1ClockSelection = RCC_I2C1CLKSOURCE_HSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x0010020A;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : BUTTON3_Pin BUTTON4_Pin */
  GPIO_InitStruct.Pin = BUTTON3_Pin|BUTTON4_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : BUTTON2_Pin BUTTON1_Pin */
  GPIO_InitStruct.Pin = BUTTON2_Pin|BUTTON1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  HAL_NVIC_SetPriority(EXTI0_1_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI0_1_IRQn);

  HAL_NVIC_SetPriority(EXTI2_3_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI2_3_IRQn);

  HAL_NVIC_SetPriority(EXTI4_15_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI4_15_IRQn);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == BUTTON1_Pin || GPIO_Pin == BUTTON2_Pin ||
      GPIO_Pin == BUTTON3_Pin || GPIO_Pin == BUTTON4_Pin)
  {
    // mark activity (works both in run mode and right after wake)
    last_activity_ms = HAL_GetTick();
  }
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
