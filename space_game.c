/*
 *  PicoCalc Space Game  -  steerable ship + torpedoes + aliens
 *  ---------------------------------------------------------------------------
 *  Author: Thomas Dzubin
 *
 *  A 304x228 play box (4:3, like an old TV) is anchored to the top-left of the
 *  PicoCalc's 320x320 LCD, drawn with a 3-pixel border.  Inside it a random
 *  starfield scrolls past and a small filled-triangle "ship" sits permanently
 *  at the centre of the box.  Square "aliens" drift in and hunt the ship; you
 *  steer, thrust, cloak, and fire torpedoes at them before they ram you or
 *  shoot you down.
 *
 *  Uses lcd / southbridge / font-8x10 / audio from picocalc-text-starter by
 *  Blair Leduc.  Full game / control / status / rendering / audio notes are in
 *  GAME-DETAILS.txt next to this file.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"

#include "drivers/lcd.h"          /* lcd_init(), lcd_solid_rectangle(), RGB()   */
#include "drivers/southbridge.h"  /* sb_init(), sb_read_keyboard()              */
#include "drivers/keyboard.h"     /* KEY_* codes, KEY_STATE_* values            */
#include "drivers/audio.h"        /* audio_init(), audio_play_sound(), _stop()  */

#include "space_game.h"           /* tunables, alien_t, every prototype for this file */

#ifdef PICOCALC
#include "space_game_screenshot.h"  /* F5 screenshot - delete file + this line to drop it */
#endif

/* keyboard.c is not built; it declares this symbol as extern, but nothing we
 * link references it.  Defined here anyway so the intent is obvious. */
volatile bool user_interrupt = false;

/* Every #define, the alien_t model and every forward declaration live in
 * space_game.h (included above).  This file is just function bodies: main()
 * first, then run_game(), then the helpers. */

_Static_assert(BOX_W <= WIDTH && BOX_H <= HEIGHT,
               "the play box must fit the panel - shrink BOX_W / BOX_H (and set "
               "SHOW_STATUS 0) when porting to a smaller screen");

/* xorshift32 PRNG - seeded from the boot clock in main().  Kept up here with
 * its helpers so main() can be the first function in the file. */
static uint32_t rng_state = 0x9E3779B9u;
static inline uint32_t rng_next(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}
static inline float rng_range(float lo, float hi)
{
    return lo + (hi - lo) * ((float)rng_next() / 4294967296.0f);
}

/* =========================================================================
 *  main - hardware bring-up once, then a fresh game each time run_game()
 *  returns (which it does when the player restarts from GAME OVER).
 * ===================================================================== */
int main(void)
{
    sb_init();                   /* south-bridge I2C (keyboard); no bg poll */
    lcd_init();                  /* turns the display on */
    lcd_enable_cursor(false);    /* no blinking text cursor */
    audio_init();                /* PIO square-wave voice on GP26 / GP27 */

    rng_state = time_us_32() | 1u;

    lcd_set_foreground(COL_STATUS_TXT);    /* gauge readout text colour         */
    lcd_set_background(COL_SPACE);      /* ... on black, so digits self-erase */

    for (;;)
    {
        int level = splash_screen();       /* title + difficulty pick, each game */
        if (controls_screen())             /* key legend + 10 s countdown ...  */
            run_game(level);               /* ... unless ESC -> back to splash */
    }
}

/* =========================================================================
 *  Tunables note: all #defines are in space_game.h.  FLAME_COL below is the
 *  one bit of palette that has to be data (an array), not a macro.
 * ===================================================================== */

static const uint16_t FLAME_COL[2] = {
    RGB(255, 140, 0),                       /* bright orange                */
    RGB(255, 70, 0),                        /* deep orange (flicker)       */
};

/* Enemy-alien state.  The `alien_t` type and every helper prototype now live
 * in space_game.h, so this file is nothing but function bodies. */
static alien_t aliens[ALIEN_ONSCREEN];
static int     aliens_spawned;     /* running total ever placed (<= alien_max) */

/* Runtime alien tunables - start at the *_INIT values from space_game.h, but
 * are plain variables so they can be set per chosen difficulty (future). */
static int   alien_max        = ALIEN_MAX_INIT;        /* total that ever spawn - run_game() sets it per level (ALIEN_MAX_L1/L2/L3) */
static float alien_speed      = ALIEN_SPEED_INIT;      /* default per-alien speed (each alien_t copies it at spawn) */
static float alien_spawn_max  = ALIEN_SPAWN_MAX_INIT;  /* farthest spawn distance */

/* When false (the default), a loaded alien (torpedoes > 0) is frozen and only
 * fires; once empty it charges to ram.  When true, a loaded alien may ALSO
 * move / hunt / ram while it still has torpedoes.  Meant for a future high
 * difficulty - leave it false for now. */
static bool  alien_ram_and_fire = false;

/* Draw (or, with col == COL_SPACE, erase) one hint caption on the shared bottom
 * row: 1 = "Next Alien This Way", 2 = "Use S for Shields", 3 = "SPACE key to
 * fire", 4 = "Arrow Keys to Move", 5 = "Press Q to Quit".  Defined down in the
 * helpers; forward-declared so run_game() (up here) can use it. */
static void hint_caption(int which, uint16_t col);

/* ---------------------------------------------------------------------------
 *  One playthrough - returns when the player asks to restart after GAME OVER.
 *  Sits here, right after main(), on purpose: this is the game.  Every helper
 *  it calls is declared in space_game.h and defined below.
 * ------------------------------------------------------------------------- */
void run_game(int level)
{
    lcd_clear_screen();          /* wipe the last game (harmless on first run) */
    sound_silence();             /* reset the sound latch                     */

    /* Per-difficulty alien tunables (the rest keep their *_INIT defaults). */
    alien_max = (level == 1) ? ALIEN_MAX_L1
              : (level == 2) ? ALIEN_MAX_L2
              : (level == 3) ? ALIEN_MAX_L3
                             : ALIEN_MAX_INIT;

    draw_border();
#if SHOW_STATUS
    draw_status_frames();
    { char lb[8]; snprintf(lb, sizeof lb, "Lv %d", level);
      lcd_putstr(KILLS_TXT_COL, LVL_TXT_ROW, lb); }    /* chosen difficulty, once */
#endif
    stars_init();
    torpedoes_init();
    atorpedoes_init();
    aliens_init(level, UNIVERSE_HALF, UNIVERSE_HALF);   /* ship starts dead centre */

    /* Ship state. */
    float angle = 0.0f;                 /* heading, radians, 0 = up          */
    float vx = 0.0f, vy = 0.0f;         /* velocity vector, px/frame (inertia) */

    int   fuel       = FUEL_MAX;        /* manoeuvring fuel; at 0 nothing moves */
    int   fuel_shown = -1;             /* last fuel drawn (-1 = force draw)  */
    int   ammo       = AMMO_MAX;        /* rounds left; at 0 SPACE does nothing */
    int   ammo_shown = -1;             /* last ammo drawn (-1 = force draw)  */
    int   pwr        = POWER_MAX;       /* power: raises shields / feeds cloak;
                                        * at 0 life support fails -> game over */
    int   pwr_shown  = -1;             /* last power drawn (-1 = force draw) */
    int   damage     = 0;              /* hull damage 0..100; DMG_MAX = dead */
    int   dmg_shown  = -1;             /* last Damage gauge value drawn (-1 force) */
    uint32_t dmg_tick_ms = 0;          /* next boot-ms to mend 1% (ship+aliens) */
#if !SHOW_STATUS
    signed char warn_shown[5] = { 0, 0, 0, 0, 0 };  /* right-edge low-resource
                                    * warnings (fuel/pwr/ammo/shld/hull): last
                                    * level drawn per row, 0/1/2 (screen starts
                                    * cleared, so no force-blank needed)        */
#endif

    int   score       = 0;             /* alien ships destroyed             */
    int   score_shown = -1;            /* last score drawn (-1 = force draw) */
    uint32_t game_start_ms = 0;        /* MM:SS clock origin (shifted over pauses) */
    int   clk_shown   = -1;            /* last whole-second value drawn (-1 force) */
    bool  game_over   = false;
    int   go_flash    = 0;             /* death-flash frames left            */
    bool  go_prompt   = false;         /* "press any key" is on screen      */
    bool  quit_prompt = false;         /* Q pressed - waiting for Y / other  */
    bool  help_pause  = false;         /* ? pressed - show legend, freeze   */
#ifdef PICOCALC
    bool     shot_pending    = false;  /* F5 pressed - grab a screenshot     */
    bool     shot_msg_shown  = false;  /* is the "Saved ..." line on screen? */
    uint32_t shot_msg_until_ms = 0;    /* ... shown until this boot-ms       */
    char     shot_msg[24]    = { 0 };  /* the status text to flash           */
#endif

    /* Position on the universal pixel grid (starts dead centre).  int32 whole
     * pixels + a float carry for the sub-pixel remainder of each frame's move. */
    int32_t univ_x = UNIVERSE_HALF, univ_y = UNIVERSE_HALF;
    float   univ_fx = 0.0f, univ_fy = 0.0f;
    int32_t univ_x_shown = -1, univ_y_shown = -1;

    uint32_t flame_until_ms = 0;        /* main flame lit until this boot-ms   */
    uint32_t brake_until_ms = 0;        /* brake flames lit until this boot-ms */
    uint32_t pew_until_ms   = 0;        /* player-fire chirp owns the voice till */
    uint32_t apew_until_ms  = 0;        /* alien-fire (reversed) chirp until    */
    uint32_t chime_until_ms = 0;        /* "alien destroyed" chime until (top priority) */
    uint32_t hiss_until_ms  = 0;        /* thruster/turn hiss plays until      */

    bool k_left = false, k_right = false;
    int  rot_fuel_frames = 0;           /* frames turned since the last -1 fuel */
    uint32_t last_key_ms = 0;           /* boot-ms of the last key event; drives *
                                        * the IDLE_ROOKIE_MS idle-nudge (0 = set *
                                        * on the first frame)                    */

    int   fnb_state      = FNB_OFF;     /* ENTER flip-and-burn autopilot     */
    float fnb_target     = 0.0f;        /* retrograde heading to flip to     */
    int   fnb_burn_left  = 0;           /* burn frames remaining             */
    bool  fnb_prev       = false;       /* was a flip-and-burn running last frame? */

    bool    auto_mode   = false;       /* A toggle: arrows = autopilot hops */
    bool    auto_shown  = false;       /* is the AUTOPILOT/MANUAL text up?  */
    uint32_t manual_txt_until_ms = 0;  /* "MANUAL" flash after leaving auto */
    int     auto_state  = AUTO_IDLE;   /* per-hop autopilot phase           */
    int     auto_dir    = 0;           /* 0 up / 1 down / 2 left / 3 right   */
    float   auto_target = 0.0f;        /* cardinal heading for the hop      */
    int32_t auto_start_ux = 0;         /* univ pos when the hop's move began */
    int32_t auto_start_uy = 0;

    int      shield_lvl    = 0;         /* 0..100 percent; 0 = shield down     */
    int      shield_shown  = -1;        /* last shield gauge value drawn (-1 force) */

    bool     cloaked      = false;      /* C toggle: invisible + unhuntable  */
    bool     cloak_shown  = false;      /* is the "CLOAKED" indicator up?    */
    uint32_t cloak_tick_ms = 0;         /* next boot-ms to dock cloak power  */

    int   sx[3], sy[3];                 /* current ship vertices    */
    int   psx[3], psy[3];               /* previous (for erase)     */
    ship_verts(angle, sx, sy);
    for (int i = 0; i < 3; i++) { psx[i] = sx[i]; psy[i] = sy[i]; }
    tri_fill(sx, sy, COL_SHIP);
    bool had_ship = true;               /* was the hull drawn last frame?    */

    int  pfx[3], pfy[3];               /* previous main-flame vertices  */
    bool had_flame = false;

    int  pblx[3] = {0}, pbly[3] = {0}; /* previous brake-flame vertices */
    int  pbrx[3] = {0}, pbry[3] = {0};
    bool had_brake = false;

    int  prcx[3] = {0}, prcy[3] = {0}; /* previous attitude-jet vertices */
    bool had_rcs = false;

    int  shx[6], shy[6];                     /* current shield-ring vertices */
    int  pshx[6] = {0}, pshy[6] = {0};       /* previous (for erase)         */
    bool had_shield = false;                 /* was the ring drawn last frame? */

    /* LEVEL-1 HINT state (difficulty 1 only - see space_game.h HINTS section). */
    bool     hint_fnb_on    = false;  /* was a flip-and-burn running last frame? */
    int      hint_cap_shown = 0;      /* caption row: 0 none / 1 next-alien / 2 shields / 3 fire / 4 move / 5 quit */
    /* "Next Alien This Way": grey arrow + caption toward the nearest off-box
     * alien when the box is clear (or a flip-and-burn just ended, box clear). */
    bool     hint_na_armed  = false;
    uint32_t hint_na_until  = 0;
    int      hint_na_vx[3] = {0}, hint_na_vy[3] = {0};   /* last arrow (for erase) */
    bool     hint_na_drawn  = false;
    /* "Use S for Shields": shown when the hull just became "wounded" (shield down). */
    bool     hint_s_armed   = false;
    uint32_t hint_s_until   = 0;
    /* "SPACE key to fire": shown once at HINT_FIRE_AFTER_MS if still no shot fired. */
    bool     hint_fire_armed = false;
    uint32_t hint_fire_until = 0;
    /* "Arrow Keys to Move": levels 0 and 1, once as the game starts. */
    bool     hint_move_armed = false;
    uint32_t hint_move_until = 0;
    /* "Press Q to Quit": level 0 only, at the 1- and 2-minute marks. */
    int      hint_q_stage    = 0;     /* 0 none / 1 shown at 1 min / 2 shown at 2 min */
    uint32_t hint_q_until    = 0;

    for (;;)
    {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (last_key_ms == 0)
            last_key_ms = now_ms;               /* start the idle clock */
        if (game_start_ms == 0)
            game_start_ms = now_ms;             /* start the MM:SS clock */

        /* ---- input: drain the key FIFO -> rotation flags + thrust/brake ---- */
        for (int n = 0; n < INPUT_DRAIN_MAX; n++)
        {
            uint16_t e = sb_read_keyboard();
            if (!e)
                break;

            uint8_t st   = (e >> 8) & 0xFF;
            uint8_t code = e & 0xFF;

            bool down;
            if (st == KEY_STATE_PRESSED || st == KEY_STATE_HOLD)
                down = true;
            else if (st == KEY_STATE_RELEASED)
                down = false;
            else
                continue;

            bool pressed = (st == KEY_STATE_PRESSED);   /* the leading edge only */

            last_key_ms = now_ms;              /* any key traffic = not idle */

            /* GAME OVER: once the prompt is up, any fresh key (bar ~, which
             * still reboots to BOOTSEL) returns to the start screen. */
            if (go_prompt && pressed && code != '~')
                return;

            /* Q-QUIT confirmation is up: Y -> back to the start screen,
             * anything else cancels it. */
            if (quit_prompt && pressed)
            {
                if (code == 'y' || code == 'Y')
                    return;                             /* -> splash_screen() */
                quit_prompt = false;
                lcd_putstr(QUIT_TXT_COL,     QUIT_TXT_ROW,     "                ");
                lcd_putstr(QUIT_TXT_COL,     QUIT_TXT_ROW + 1, "                ");
                continue;                               /* this key is consumed */
            }

            /* While an autopilot runs (flip-and-burn, or an autopilot hop) the
             * manual flight keys are locked out - the ship is not yours to
             * steer. */
            bool manual = (fnb_state == FNB_OFF && auto_state == AUTO_IDLE);

            switch (code)
            {
                /* ENTER: start a flip-and-burn - turn to face retrograde, then
                 * burn the main engine to kill speed.  Needs fuel, some actual
                 * velocity, and no cloak; ignored if one is already running. */
                case KEY_ENTER:
                case KEY_RETURN:
                    /* Works even mid autopilot-hop: it's the bail-out when the
                     * autopilot course is about to run you into an alien.  It
                     * cancels the hop and flips to a retrograde stop. */
                    if (pressed && fnb_state == FNB_OFF && !cloaked && fuel > 0)
                    {
                        float m = sqrtf(vx * vx + vy * vy);
                        if (m >= FNB_MIN_SPEED)
                        {
                            auto_state = AUTO_IDLE;        /* bail out of a hop */
                            fnb_target = atan2f(-vx, vy);   /* nose -> -v        */
                            if (fnb_target < 0.0f) fnb_target += 6.2831853f;
                            fnb_state = FNB_FLIP;
                        }
                    }
                    break;

                /* LEFT / RIGHT turn the hull only - they never touch the
                 * velocity vector, so any existing travel is unchanged.  The
                 * attitude jets burn fuel by TIME HELD, not per key event -
                 * that drain lives in the rotation block below (FUEL_ROT_FRAMES)
                 * so it can't ride on the keyboard's repeat rate.
                 * In AUTOPILOT the arrows launch a one-box hop instead
                 * (J / K become the hand-rotate keys). */
                case KEY_LEFT:
                    if (auto_mode)
                    {
                        if (pressed && auto_state == AUTO_IDLE && !cloaked && fuel > 0)
                        { auto_dir = 2; auto_target = 4.71238898f; auto_state = AUTO_TURN; }
                    }
                    else
                        k_left = down && manual;
                    break;
                case KEY_RIGHT:
                    if (auto_mode)
                    {
                        if (pressed && auto_state == AUTO_IDLE && !cloaked && fuel > 0)
                        { auto_dir = 3; auto_target = 1.57079633f; auto_state = AUTO_TURN; }
                    }
                    else
                        k_right = down && manual;
                    break;

                /* J / K: hand-rotate CCW / CW - AUTOPILOT only, and only
                 * between hops.  They feed the same k_left/k_right the normal
                 * LEFT/RIGHT do, so the rotation block and the RCS jet + hiss
                 * all just work. */
                case 'j': case 'J':
                    if (auto_mode && auto_state == AUTO_IDLE) k_left  = down;
                    break;
                case 'k': case 'K':
                    if (auto_mode && auto_state == AUTO_IDLE) k_right = down;
                    break;

                /* UP: add a THRUST_IMPULSE vector along the nose to the
                 * velocity (turn-and-thrust composes by vector addition), then
                 * cap the magnitude at SPEED_MAX; relight the main flame.
                 * ONE impulse per distinct key-down - holding UP does nothing
                 * (auto-repeat HOLD events are ignored).  Dead at 0 fuel. */
                case KEY_UP:
                    if (auto_mode)
                    {
                        if (pressed && auto_state == AUTO_IDLE && !cloaked && fuel > 0)
                        { auto_dir = 0; auto_target = 0.0f; auto_state = AUTO_TURN; }
                    }
                    else if (pressed && manual && fuel > 0)
                    {
                        vx +=  sinf(angle) * THRUST_IMPULSE;
                        vy += -cosf(angle) * THRUST_IMPULSE;
                        float m = sqrtf(vx * vx + vy * vy);
                        if (m > SPEED_MAX)
                        {
                            vx *= SPEED_MAX / m;
                            vy *= SPEED_MAX / m;
                        }
                        flame_until_ms = now_ms + FLAME_HOLD_MS;
                        hiss_until_ms  = now_ms + HISS_MS;
                        fuel -= FUEL_COST_FWD;
                    }
                    break;

                /* DOWN: the mirror of UP - add BRAKE_IMPULSE (half a THRUST_
                 * IMPULSE) along the OPPOSITE of the nose, then cap |v| at
                 * SPEED_MAX.  Not a brake: from a standstill it drives the ship
                 * backwards at half the up-thrust rate.  One impulse per
                 * distinct key-down; needs fuel; lights the front flames. */
                case KEY_DOWN:
                    if (auto_mode)
                    {
                        if (pressed && auto_state == AUTO_IDLE && !cloaked && fuel > 0)
                        { auto_dir = 1; auto_target = 3.14159265f; auto_state = AUTO_TURN; }
                    }
                    else if (pressed && manual && fuel > 0)
                    {
                        vx += -sinf(angle) * BRAKE_IMPULSE;
                        vy +=  cosf(angle) * BRAKE_IMPULSE;
                        float m = sqrtf(vx * vx + vy * vy);
                        if (m > SPEED_MAX)
                        {
                            vx *= SPEED_MAX / m;
                            vy *= SPEED_MAX / m;
                        }
                        brake_until_ms = now_ms + BRAKE_HOLD_MS;
                        hiss_until_ms  = now_ms + HISS_MS;
                        fuel -= FUEL_COST_REV;
                    }
                    break;

                /* SPACE: one torpedo per press (not on auto-repeat), while ammo
                 * lasts and NOT cloaked.  Only spend a round if one actually
                 * left the muzzle (all MAX_TORPEDOES slots could be in flight). */
                case KEY_SPACE:
                    if (pressed && !cloaked && ammo > 0 && torpedo_fire(angle))
                    {
                        ammo--;
                        pew_until_ms = now_ms + PEW_MS;   /* chirp the shot */
                    }
                    break;
                /* S: toggle shields.  Raising: needs SHIELD_PWR_COST power and no
                 * cloak; sets the level to SHIELD_START and spends that power
                 * once (no timer, no per-second drain - it only falls to hits).
                 * A second S drops it (no power back). */
                case 's':
                case 'S':
                    if (pressed)
                    {
                        if (shield_lvl > 0)
                            shield_lvl = 0;
                        else if (!cloaked && pwr >= SHIELD_PWR_COST)
                        {
                            shield_lvl = SHIELD_START;
                            pwr       -= SHIELD_PWR_COST;
                        }
                    }
                    break;
                /* C: toggle the cloak.  On: the hull vanishes, aliens lose the
                 * ship (each hunter keeps drifting on its last heading), and
                 * shields drop.  While cloaked: no firing, no shields, thrust
                 * capped at the slowest crawl, CLOAK_DRAIN power per second. */
                case 'c':
                case 'C':
                    if (pressed && manual)
                    {
                        if (cloaked)
                            cloaked = false;
                        else if (pwr > 0)
                        {
                            cloaked       = true;
                            shield_lvl    = 0;         /* cloak drops the shield */
                            cloak_tick_ms = now_ms + CLOAK_DRAIN_MS;
                            for (int a = 0; a < ALIEN_ONSCREEN; a++)
                            {
                                alien_t *al = &aliens[a];
                                if (al->state != 1 || !al->hunting)
                                    continue;
                                float rx = univ_delta(al->ux, univ_x);
                                float ry = univ_delta(al->uy, univ_y);
                                float d  = sqrtf(rx * rx + ry * ry);
                                if (d > 0.5f)             /* keep its charge heading */
                                {
                                    al->dirx = -rx / d;
                                    al->diry = -ry / d;
                                }
                                al->hunting    = false;   /* back to blind drift */
                                al->spot_pause = 0;
                            }
                        }
                    }
                    break;
                /* A: toggle AUTOPILOT.  M: leave AUTOPILOT for MANUAL nav (no-op
                 * if already manual).  Either aborts a hop in progress and drops
                 * any held J/K/arrow turn; leaving autopilot flashes "MANUAL". */
                case 'a':
                case 'A':
                    if (pressed && fnb_state == FNB_OFF)
                    {
                        bool was_auto = auto_mode;
                        auto_mode  = !auto_mode;
                        auto_state = AUTO_IDLE;
                        k_left = k_right = false;
                        if (was_auto)
                            manual_txt_until_ms = now_ms + MANUAL_TXT_MS;
                    }
                    break;
                case 'm':
                case 'M':
                    if (pressed && auto_mode)
                    {
                        auto_mode  = false;
                        auto_state = AUTO_IDLE;
                        k_left = k_right = false;
                        manual_txt_until_ms = now_ms + MANUAL_TXT_MS;
                    }
                    break;
                /* Q: quit to the start screen, but ask first.  The confirmation
                 * is handled by the quit_prompt check above; here we just raise
                 * it (and freeze the game while it is up). */
                case 'q':
                case 'Q':
                    if (pressed && !game_over && !quit_prompt)
                    {
                        quit_prompt = true;
                        lcd_putstr(QUIT_TXT_COL, QUIT_TXT_ROW,     "Quit?         ");
                        lcd_putstr(QUIT_TXT_COL, QUIT_TXT_ROW + 1, "Y = yes  N = no");
                    }
                    break;
                /* ?: freeze and bring up the key legend until any key resumes. */
                case '?':
                    if (pressed && !game_over && !quit_prompt)
                        help_pause = true;
                    break;
#ifdef PICOCALC
                /* F5: save a screenshot of the LCD to the SD card. */
                case KEY_F5:
                    if (pressed && !game_over && !quit_prompt)
                        shot_pending = true;
                    break;
#endif
                case KEY_ESC:   if (pressed) return;  break;    /* quit -> start screen */
                case '~':       if (down) go_bootsel(); break;  /* only ~ -> BOOTSEL   */
                default: break;
            }
        }

        /* ---- ? pressed: show the legend, freeze until a key, then rebuild --- */
        if (help_pause)
        {
            help_pause = false;
            sound_silence();
            uint32_t t0 = to_ms_since_boot(get_absolute_time());

            lcd_clear_screen();
            draw_key_legend();
            lcd_putstr(4, 30, "PAUSED - press any key to resume");

            while (sb_read_keyboard())        /* drop the '?' and anything queued */
                ;
            (void)wait_key();                 /* block here (~ still reboots)      */

            /* rebuild the playfield: border + status frames now, everything else is
             * flagged to repaint on the next frame. */
            lcd_clear_screen();
            draw_border();
#if SHOW_STATUS
            draw_status_frames();
            { char lb[8]; snprintf(lb, sizeof lb, "Lv %d", level);
              lcd_putstr(KILLS_TXT_COL, LVL_TXT_ROW, lb); }
            fuel_shown = ammo_shown = pwr_shown = -1;
            shield_shown = dmg_shown = score_shown = -1;
            univ_x_shown = univ_y_shown = -1;
            cloak_shown  = !cloaked;
            clk_shown    = -1;
#endif
            for (int i = 0; i < ALIEN_ONSCREEN; i++)
            { aliens[i].pdrawn = false; aliens[i].lrs_drawn = false; }
            had_ship = had_flame = had_brake = had_rcs = had_shield = false;
            auto_shown = false;

            /* push every wall-clock deadline forward by the paused span so no
             * timer "catches up" (thrust flames, hiss, cloak/repair ticks). */
            uint32_t paused = to_ms_since_boot(get_absolute_time()) - t0;
            flame_until_ms += paused;  brake_until_ms += paused;
            pew_until_ms   += paused;  apew_until_ms  += paused;
            chime_until_ms += paused;  hiss_until_ms  += paused;
            manual_txt_until_ms += paused;  last_key_ms += paused;
            game_start_ms += paused;
            if (hint_na_until)   hint_na_until   += paused;
            if (hint_s_until)    hint_s_until    += paused;
            if (hint_fire_until) hint_fire_until += paused;
            if (hint_move_until) hint_move_until += paused;
            if (hint_q_until)    hint_q_until    += paused;
            if (cloak_tick_ms) cloak_tick_ms += paused;
            if (dmg_tick_ms)   dmg_tick_ms   += paused;

            continue;                        /* resume next frame */
        }

#ifdef PICOCALC
        /* ---- F5: freeze, read the LCD out to a BMP on the SD card, then
         *      flash a one-line status.  The capture only READS the panel, so
         *      the frozen frame is exactly what lands in the file. */
        if (shot_pending)
        {
            shot_pending = false;
            sound_silence();
            uint32_t t0 = to_ms_since_boot(get_absolute_time());

            snprintf(shot_msg, sizeof shot_msg, "%s", screenshot_to_sd());

            uint32_t paused = to_ms_since_boot(get_absolute_time()) - t0;
            flame_until_ms += paused;  brake_until_ms += paused;
            pew_until_ms   += paused;  apew_until_ms  += paused;
            chime_until_ms += paused;  hiss_until_ms  += paused;
            manual_txt_until_ms += paused;  last_key_ms += paused;
            game_start_ms += paused;
            if (hint_na_until)   hint_na_until   += paused;
            if (hint_s_until)    hint_s_until    += paused;
            if (hint_fire_until) hint_fire_until += paused;
            if (hint_move_until) hint_move_until += paused;
            if (hint_q_until)    hint_q_until    += paused;
            if (cloak_tick_ms) cloak_tick_ms += paused;
            if (dmg_tick_ms)   dmg_tick_ms   += paused;
            shot_msg_until_ms = to_ms_since_boot(get_absolute_time()) + SHOT_MSG_MS;

            continue;                        /* status shows from next frame */
        }
#endif

        /* ---- Q-quit confirmation up: freeze the game until it is answered -- */
        if (quit_prompt)
        {
            sound_silence();
            sleep_ms(FRAME_MS);
            continue;
        }

        /* ---- YOU WON: every alien on a level that has them is dead -------- */
        if (level > 0 && !game_over && score >= alien_max)
        {
            sound_silence();

            uint32_t es = (now_ms - game_start_ms) / 1000u;   /* time TAKEN to win */
            if (es > GAME_LEN_S) es = GAME_LEN_S;
            char clk[8];
            snprintf(clk, sizeof clk, "%02u:%02u",
                     (unsigned)(es / 60), (unsigned)(es % 60));

            lcd_clear_screen();
            big_text(WIN_L1_X, WIN_L1_Y, WIN_SCALE_TXT, COL_TORPEDO, "YOU WON IN");
            big_text(WIN_L2_X, WIN_L2_Y, WIN_SCALE_CLK, COL_SHIP,    clk);

            play_win_song();                     /* ~WIN_SONG_MS, blocking */

            lcd_putstr(WIN_MSG_COL, WIN_MSG_ROW, "Press any key to continue");
            while (sb_read_keyboard())            /* drop keys hit during the song */
                ;
            (void)wait_key();                    /* ~ still reboots            */
            return;                              /* -> splash_screen()        */
        }

        /* ---- time's up: the 15-minute countdown reached 00:00 -> GAME OVER,
         *      on every level (level 0 has no other way to end). ---- */
        if (!game_over && game_start_ms != 0 &&
            (now_ms - game_start_ms) / 1000u >= GAME_LEN_S)
        {
            game_over = true;
            go_flash  = GAMEOVER_FLASH;
        }

        /* ---- game over: flash the rocket, print the message, wait for a key -- */
        if (game_over)
        {
            sound_silence();             /* kill any hiss/pew frozen on death */
            if (go_flash > 0)
            {
                go_flash--;
                tri_fill(sx, sy, (go_flash & 1) ? COL_SHIP_FLASH : COL_SPACE);
                if (go_flash == 0)
                {
                    tri_fill(sx, sy, COL_SPACE);               /* the rocket is gone */
                    lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW,     "         ");
                    lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW + 1, "                             ");
                    if (hint_na_drawn)                         /* wipe any level-1 hint */
                    {
                        tri_fill(hint_na_vx, hint_na_vy, COL_SPACE);
                        hint_na_drawn = false;
                    }
                    if (hint_cap_shown)
                    {
                        hint_caption(hint_cap_shown, COL_SPACE);
                        hint_cap_shown = 0;
                    }
                    lcd_putstr(GO_TXT_COL, GO_TXT_ROW, "GAME OVER");
                    lcd_putstr(GO_MSG_COL, GO_MSG_ROW, "press any key to restart");
                    go_prompt = true;     /* the input loop now accepts a restart */
                }
            }
            sleep_ms(FRAME_MS);
            continue;                    /* ~ still reboots to BOOTSEL */
        }

        if (fuel < 0)
            fuel = 0;                   /* spent on manoeuvring above          */
        /* ammo only ever decrements behind an `ammo > 0` guard, so it can't go
         * negative - no clamp needed.  The shield only changes on S / cloak /
         * damage - no per-frame upkeep. */

        /* ---- cloak burns CLOAK_DRAIN power per CLOAK_DRAIN_MS; drops at 0 --- */
        while (cloaked && now_ms >= cloak_tick_ms)
        {
            pwr -= CLOAK_DRAIN;
            if (pwr < 0) pwr = 0;
            cloak_tick_ms += CLOAK_DRAIN_MS;
        }
        if (cloaked && pwr == 0)
            cloaked = false;

        /* ---- power at 0 = life support gone = game over ---- */
        if (pwr <= 0 && !game_over)
        {
            game_over = true;
            go_flash  = GAMEOVER_FLASH;
        }

        /* ---- crew repair: -DMG_REPAIR_STEP off hull damage (and every live
         *      alien's internal damage) once every DMG_REPAIR_MS ---- */
        if (dmg_tick_ms == 0)
            dmg_tick_ms = now_ms + DMG_REPAIR_MS;
        while (now_ms >= dmg_tick_ms)
        {
            damage -= DMG_REPAIR_STEP;
            if (damage < 0) damage = 0;
            aliens_heal();
            dmg_tick_ms += DMG_REPAIR_MS;
        }

        /* ---- idle nudge: on a level that has aliens, no key for
         *      IDLE_ROOKIE_MS -> get the player moving.  If the ship is still
         *      coasting, flip-and-burn it to a halt FIRST (don't touch the
         *      timer - this block fires again, ship stopped, once the FnB
         *      ends).  Stopped: if a rookie is still alive, reload it with a
         *      full clip and make it aim perfectly; otherwise spawn a fresh
         *      one. ---- */
        if (level >= 1 && fnb_state == FNB_OFF &&
            now_ms - last_key_ms >= IDLE_ROOKIE_MS)
        {
            float spd = sqrtf(vx * vx + vy * vy);
            if (spd >= FNB_MIN_SPEED && fuel > 0 && !cloaked)
            {
                auto_state = AUTO_IDLE;                 /* bail any hop */
                fnb_target = atan2f(-vx, vy);           /* nose -> retrograde */
                if (fnb_target < 0.0f) fnb_target += 6.2831853f;
                fnb_state  = FNB_FLIP;
            }
            else
            {
                alien_t *rk = NULL;
                for (int i = 0; i < ALIEN_ONSCREEN; i++)
                    if (aliens[i].state == 1 && aliens[i].rookie)
                    { rk = &aliens[i]; break; }
                if (rk)
                {
                    /* Still around (usually emptied and charging by now): hand it
                     * another clip so it holds position and shoots again.  Its
                     * `shots` count carries over, so the aim ramp does too. */
                    rk->torpedoes  += ALIEN_TORPEDOES;
                    rk->next_fire_ms = now_ms;
                }
                else
                    alien_spawn_rookie(univ_x, univ_y, false);
                last_key_ms = now_ms;                    /* restart the idle clock */
            }
        }

        /* ---- while cloaked, the ship can only crawl (slowest speed) ---- */
        if (cloaked)
        {
            float m = sqrtf(vx * vx + vy * vy);
            if (m > BRAKE_IMPULSE)
            {
                vx *= BRAKE_IMPULSE / m;
                vy *= BRAKE_IMPULSE / m;
            }
        }

        /* ---- flip-and-burn autopilot (ENTER) ---- *
         *  FLIP: swing `angle` toward `fnb_target` (nose = retrograde) at the
         *  manual turn rate.  BURN: fire the main engine along the (now
         *  retrograde) nose to bleed off velocity, then stop.  Runs out of
         *  fuel -> abort; alien contact still ends it the hard way. */
        if (fnb_state == FNB_FLIP)
        {
            if (fuel <= 0)
                fnb_state = FNB_OFF;
            else
            {
                float d = fnb_target - angle;
                while (d >  3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                if (fabsf(d) <= FNB_ROT_STEP)
                {
                    angle = fnb_target;
                    fnb_state = FNB_BURN;
                    fnb_burn_left = FNB_BURN_FRAMES;
                }
                else
                    angle += (d > 0.0f) ? FNB_ROT_STEP : -FNB_ROT_STEP;
                if (++rot_fuel_frames >= FUEL_ROT_FRAMES)   /* time-based drain */
                {
                    rot_fuel_frames = 0;
                    fuel--;
                }
                hiss_until_ms = now_ms + HISS_MS;   /* autopilot jets hiss too */
            }
        }
        else if (fnb_state == FNB_BURN)
        {
            if (fuel <= 0)
                fnb_state = FNB_OFF;
            else
            {
                float m = sqrtf(vx * vx + vy * vy);
                if (m > 0.05f)
                {
                    float nx = sinf(angle), ny = -cosf(angle);   /* retrograde */
                    float imp = (m < THRUST_IMPULSE) ? m : THRUST_IMPULSE;
                    vx += nx * imp;
                    vy += ny * imp;
                    if (vx * nx + vy * ny > 0.0f) { vx = 0.0f; vy = 0.0f; }
                    fuel -= FUEL_COST_FWD;   /* main engine = forward cost */
                }
                flame_until_ms = now_ms + FLAME_HOLD_MS;   /* light the tail */
                hiss_until_ms  = now_ms + HISS_MS;          /* ... and hiss    */
                if (--fnb_burn_left <= 0)
                    fnb_state = FNB_OFF;
            }
        }

        /* ---- Autopilot hop (in autopilot, an arrow starts one) ---- *
         *  AUTO_TURN: swing `angle` to the screen cardinal `auto_target` (the
         *  side attitude jet shows via auto_turn_side, below), then zero
         *  velocity and anchor the start point.  AUTO_MOVE: accelerate to
         *  AUTO_CRUISE (main engine) then coast.  AUTO_BRAKE: retro-pulse
         *  (front engine ONLY - never the main engine) to a stop, then snap
         *  exactly onto the line.  It is a strict TURN -> MOVE -> BRAKE march:
         *  once braking starts the forward engine never lights again.
         *  Runs out of fuel -> abort (ship just coasts). */
        int auto_turn_side = 0;      /* !=0 while AUTO_TURN is rotating (jet side) */

        if (auto_state == AUTO_TURN)
        {
            if (fuel <= 0)
                auto_state = AUTO_IDLE;
            else
            {
                float d = auto_target - angle;
                while (d >  3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                if (fabsf(d) <= ROT_STEP)
                {
                    angle        = auto_target;
                    vx = vy      = 0.0f;          /* clean 1-D hop from here  */
                    auto_start_ux = univ_x;
                    auto_start_uy = univ_y;
                    univ_fx = univ_fy = 0.0f;
                    auto_state = AUTO_MOVE;
                }
                else
                {
                    angle += (d > 0.0f) ? ROT_STEP : -ROT_STEP;
                    auto_turn_side = (d > 0.0f) ? -1 : +1;  /* CW -> port jet   */
                    if (++rot_fuel_frames >= FUEL_ROT_FRAMES)
                    {
                        rot_fuel_frames = 0;
                        fuel--;
                    }
                    hiss_until_ms = now_ms + HISS_MS;
                }
            }
        }
        else if (auto_state == AUTO_MOVE)
        {
            float nx = sinf(auto_target), ny = -cosf(auto_target);   /* hop dir */
            float prog = univ_delta(univ_x, auto_start_ux) * nx +
                         univ_delta(univ_y, auto_start_uy) * ny;
            float rem  = (float)AUTO_DIST - prog;
            float w    = vx * nx + vy * ny;                          /* axis speed */
            /* distance it would cover if it began braking now (impulsive
             * BRAKE_IMPULSE per frame): ~ v^2/2a + v/2 */
            float d_brake = (w > 0.0f)
                ? (w * w / (2.0f * BRAKE_IMPULSE) + w * 0.5f) : 0.0f;

            if (fuel <= 0)
                auto_state = AUTO_IDLE;
            else if (rem <= d_brake + 0.5f || w > AUTO_CRUISE + 0.01f)
                auto_state = AUTO_BRAKE;               /* time to slow - stop the main engine */
            else
            {
                if (w < AUTO_CRUISE - 0.01f)           /* accel toward cruise ("fire twice") */
                {
                    w += THRUST_IMPULSE;
                    if (w > AUTO_CRUISE) w = AUTO_CRUISE;
                    flame_until_ms = now_ms + FLAME_HOLD_MS;
                    hiss_until_ms  = now_ms + HISS_MS;
                    fuel -= FUEL_COST_FWD;
                }
                /* else: coasting - silent, no flame */
                vx = nx * w;                           /* keep it purely axial */
                vy = ny * w;
            }
        }
        else if (auto_state == AUTO_BRAKE)
        {
            float nx = sinf(auto_target), ny = -cosf(auto_target);
            float w  = vx * nx + vy * ny;

            if (fuel <= 0)
                auto_state = AUTO_IDLE;                /* abort - coast on what's left */
            else if (w <= 0.01f)
            {
                /* stopped - snap exactly onto the target line and finish */
                vx = vy = 0.0f;
                univ_fx = univ_fy = 0.0f;
                switch (auto_dir)
                {
                    case 0:  univ_y = univ_wrap(auto_start_uy - AUTO_DIST); break;
                    case 1:  univ_y = univ_wrap(auto_start_uy + AUTO_DIST); break;
                    case 2:  univ_x = univ_wrap(auto_start_ux - AUTO_DIST); break;
                    default: univ_x = univ_wrap(auto_start_ux + AUTO_DIST); break;
                }
                auto_state = AUTO_IDLE;
            }
            else
            {
                w -= BRAKE_IMPULSE;                    /* retro pulse - FRONT flames only */
                if (w < 0.0f) w = 0.0f;
                brake_until_ms = now_ms + BRAKE_HOLD_MS;
                hiss_until_ms  = now_ms + HISS_MS;
                fuel -= FUEL_COST_REV;
                vx = nx * w;
                vy = ny * w;
            }
        }

        /* ---- rotation (manual; orientation only; dead at 0 fuel or during
         *      an autopilot).  Attitude fuel is charged by TIME HELD: -1 per
         *      FUEL_ROT_FRAMES frames actually turning, so the cost is the same
         *      on any keyboard regardless of key-repeat rate.  A turn also fires
         *      the attitude jet on the side OPPOSITE the turn (turn_side) and
         *      runs the hiss.  In autopilot this is driven by J / K. */
        bool turning   = (fnb_state == FNB_OFF && auto_state == AUTO_IDLE &&
                          k_left != k_right && fuel > 0);
        int  turn_side = 0;
        if (turning)
        {
            angle += k_right ? ROT_STEP : -ROT_STEP;
            turn_side = k_right ? -1 : +1;     /* yaw right -> port jet, etc. */
            if (++rot_fuel_frames >= FUEL_ROT_FRAMES)
            {
                rot_fuel_frames = 0;
                fuel--;
            }
            hiss_until_ms = now_ms + HISS_MS;
        }
        else if (fnb_state == FNB_OFF && auto_state == AUTO_IDLE)
            rot_fuel_frames = 0;               /* not turning - reset the meter */

        /* AUTO_TURN turns the ship too - show its side attitude jet the same
         * way a manual J/K turn does (auto_turn_side was set above). */
        if (auto_turn_side != 0)
        {
            turning   = true;
            turn_side = auto_turn_side;
        }

        if (angle >= 6.2831853f) angle -= 6.2831853f;
        else if (angle < 0.0f)   angle += 6.2831853f;

        /* The field scrolls opposite the ship's velocity - it keeps drifting
         * while coasting, and the ship may be pointing any which way. */
        float scroll_x = -vx;
        float scroll_y = -vy;

        /* ---- advance the ship's spot on the universal grid by +v ---- */
        univ_fx += vx;
        univ_fy += vy;
        int univ_step_x = (int)univ_fx;   /* whole pixels moved this frame */
        int univ_step_y = (int)univ_fy;
        univ_fx -= (float)univ_step_x;
        univ_fy -= (float)univ_step_y;
        univ_x = univ_wrap(univ_x + univ_step_x);
        univ_y = univ_wrap(univ_y + univ_step_y);

        /* ---- aliens hunt the ship; contact -> shield loss or hull damage ---- */
        bool ship_dead = false;
        aliens_update(univ_x, univ_y, level, &shield_lvl, &damage, cloaked, &score, &ship_dead);
        aliens_fire(univ_x, univ_y, level, cloaked, now_ms, &apew_until_ms);
        if (ship_dead)
        {
            game_over = true;
            go_flash  = GAMEOVER_FLASH;
        }

        /* ---- render ---- */
        ship_verts(angle, sx, sy);

        bool ship_moved = false;
        for (int i = 0; i < 3; i++)
            if (sx[i] != psx[i] || sy[i] != psy[i])
                ship_moved = true;

        /* Cloak hides the hull and its flames entirely. */
        bool draw_ship  = !cloaked;
        bool shield_on  = (shield_lvl > 0);
        /* "Wounded": hull damage over SHIP_WOUND_ON, or power under that percent
         * (thresholds shared with the yellow-hull tint below). */
        bool ship_wounded = (damage > SHIP_WOUND_ON) ||
                            (pwr * 100 < POWER_MAX * SHIP_WOUND_ON);

        /* Main flame for a beat after each UP; brake flames for a beat after
         * each DOWN.  The main flame re-flickers in length every frame. */
        bool thrusting = (now_ms < flame_until_ms) && draw_ship;
        bool braking   = (now_ms < brake_until_ms) && draw_ship;

        int  fx[3], fy[3];
        if (thrusting)
        {
            int len = FLAME_LEN + (int)(rng_next() % (uint32_t)(FLAME_FLICKER + 1));
            flame_verts(angle, len, fx, fy);
        }

        int  blx[3], bly[3], brx[3], bry[3];
        if (braking)
        {
            brake_verts(angle, -1, blx, bly);
            brake_verts(angle, +1, brx, bry);
        }

        /* Attitude jet: a side flare while LEFT / RIGHT turns the hull, on the
         * side opposite the turn (turn_side, set in the rotation block). */
        int  rcx[3], rcy[3];
        bool rcs_on = turning && draw_ship;
        if (rcs_on)
            rcs_verts(angle, turn_side, rcx, rcy);

        if (shield_on)
            shield_verts(angle, shx, shy);

        /* Erase last frame's ship body, flames and shield ring before the
         * stars redraw over the vacated pixels.  The hull is erased when it
         * moved OR when the cloak just hid it. */
        if (had_ship && (ship_moved || !draw_ship))
            tri_fill(psx, psy, COL_SPACE);
        if (had_flame)
            tri_outline(pfx, pfy, COL_SPACE);
        if (had_brake)
        {
            tri_outline(pblx, pbly, COL_SPACE);
            tri_outline(pbrx, pbry, COL_SPACE);
        }
        if (had_rcs)
            tri_outline(prcx, prcy, COL_SPACE);
        if (had_shield)
            shield_draw(pshx, pshy, COL_SPACE);
        aliens_erase();                          /* clear last frame's squares  */
        lrs_erase();                             /* ... and last frame's LRS blips */

        stars_update(scroll_x, scroll_y);       /* stars pass behind          */
        torpedoes_update();                       /* player torpedoes over the stars */
        atorpedoes_update(univ_x, univ_y);        /* alien torpedoes, same layer  */
        torpedoes_hit_aliens(univ_x, univ_y, now_ms, &chime_until_ms);  /* player tracers vs squares */
        atorpedoes_hit_ship(univ_x, univ_y, &shield_lvl, &damage, cloaked, &ship_dead);  /* -> shield or hull */
        if (ship_dead && !game_over)
        {
            game_over = true;
            go_flash  = GAMEOVER_FLASH;
        }

        /* Flames (behind the hull), the alien squares, then the hull - always
         * redrawn so a star or square that erased a shared pixel can't leave a
         * gap - then the shield ring on top.  A live shield tints the hull. */
        if (thrusting)
            tri_outline(fx, fy, FLAME_COL[rng_next() & 1u]);
        if (braking)
        {
            tri_outline(blx, bly, COL_BRAKE);
            tri_outline(brx, bry, COL_BRAKE);
        }
        if (rcs_on)
            tri_outline(rcx, rcy, COL_BRAKE);
        aliens_draw(univ_x, univ_y);
        aliens_respawn(univ_x, univ_y, level);   /* only after the erase above */
        lrs_draw(univ_x, univ_y);               /* Long Range Sensors: off-box aliens */
        if (draw_ship)
            tri_fill(sx, sy, shield_on     ? COL_SHIELD
                           : ship_wounded  ? COL_SHIP_WOUND
                                           : COL_SHIP);
        if (shield_on)
            shield_draw(shx, shy, COL_SHIELD);

        if (draw_ship)
            for (int i = 0; i < 3; i++) { psx[i] = sx[i]; psy[i] = sy[i]; }
        if (thrusting)
            for (int i = 0; i < 3; i++) { pfx[i] = fx[i]; pfy[i] = fy[i]; }
        if (braking)
            for (int i = 0; i < 3; i++)
            {
                pblx[i] = blx[i]; pbly[i] = bly[i];
                pbrx[i] = brx[i]; pbry[i] = bry[i];
            }
        if (rcs_on)
            for (int i = 0; i < 3; i++) { prcx[i] = rcx[i]; prcy[i] = rcy[i]; }
        if (shield_on)
            for (int i = 0; i < 6; i++) { pshx[i] = shx[i]; pshy[i] = shy[i]; }
        had_ship   = draw_ship;
        had_flame  = thrusting;
        had_brake  = braking;
        had_rcs    = rcs_on;
        had_shield = shield_on;

        /* ---- audio: the "alien destroyed" chime wins, then the player "pew",
         *      then the alien "pew" (reversed), then the thruster/turn hiss,
         *      else silence ------------------------------------------------- */
        if (now_ms < chime_until_ms)
            sound_chime(CHIME_MS - (int)(chime_until_ms - now_ms));
        else if (now_ms < pew_until_ms)
            sound_pew(PEW_MS - (int)(pew_until_ms - now_ms));
        else if (now_ms < apew_until_ms)
            sound_apew(PEW_MS - (int)(apew_until_ms - now_ms));
        else if (now_ms < hiss_until_ms)
            sound_hiss();
        else
            sound_silence();

#if SHOW_STATUS
        /* ---- status: repaint each gauge only on the frames its value moved ---- */
        if (fuel != fuel_shown)
        {
            gauge_draw(FUEL_GAUGE_Y, FUEL_TXT_ROW, COL_GAUGE_FUEL, "Fuel", fuel, FUEL_MAX, true);
            fuel_shown = fuel;
        }
        if (pwr != pwr_shown)
        {
            gauge_draw(PWR_GAUGE_Y, PWR_TXT_ROW, COL_GAUGE_POWER, "Power", pwr, POWER_MAX, true);
            pwr_shown = pwr;
        }
        if (ammo != ammo_shown)
        {
            gauge_draw(AMMO_GAUGE_Y, AMMO_TXT_ROW, COL_GAUGE_AMMO, "Ammo", ammo, AMMO_MAX, false);
            ammo_shown = ammo;
        }
        if (shield_lvl != shield_shown)
        {
            gauge_draw(SHIELD_GAUGE_Y, SHIELD_GTXT_ROW, COL_GAUGE_SHIELD,
                       "Shields", shield_lvl, 100, true);
            shield_shown = shield_lvl;
        }
        if (damage != dmg_shown)
        {
            gauge_draw(DMG_GAUGE_Y, DMG_GTXT_ROW, COL_GAUGE_DMG,
                       "Damage", damage, DMG_MAX, true);
            dmg_shown = damage;
        }
        if (score != score_shown)
        {
            char sb[16];
            if (level == 0)                      /* level 0 has no aliens at all */
                snprintf(sb, sizeof sb, "No Aliens");
            else
                snprintf(sb, sizeof sb, "Kills %2d/%d", score, alien_max);
            lcd_putstr(KILLS_TXT_COL, SCORE_VAL_ROW, sb);
            score_shown = score;
        }
        if (cloaked != cloak_shown)
        {
            lcd_putstr(CLK_TXT_COL, CLK_TXT_ROW, cloaked ? "CLOAKED" : "       ");
            cloak_shown = cloaked;
        }

        /* ---- MM:SS countdown next to "Lv N": starts at 15:00, ticks down to
         *      00:00 = GAME OVER (handled above).  Frozen across ? / F5. ---- */
        {
            uint32_t el = (now_ms - game_start_ms) / 1000u;
            int rem = (el >= GAME_LEN_S) ? 0 : (int)(GAME_LEN_S - el);
            if (rem != clk_shown)
            {
                char cb[8];
                snprintf(cb, sizeof cb, "%02d:%02d", rem / 60, rem % 60);
                lcd_putstr(CLOCK_TXT_COL, CLOCK_TXT_ROW, cb);
                clk_shown = rem;
            }
        }

        /* ---- universal coordinates, bottom-right, only when they change ---- */
        if (univ_x != univ_x_shown || univ_y != univ_y_shown)
        {
            char buf[32];
            snprintf(buf, sizeof buf, "X:%05ld Y:%05ld",   /* 5-wide field; universe is now 4 digits */
                     (long)univ_x, (long)univ_y);
            lcd_putstr(UNIV_TXT_COL, UNIV_TXT_ROW, buf);
            univ_x_shown = univ_x;
            univ_y_shown = univ_y;
        }
#else   /* !SHOW_STATUS: the gauge/readout locals are then unused - sink them */
        (void)fuel_shown; (void)ammo_shown;   (void)pwr_shown;
        (void)score_shown;(void)cloak_shown;  (void)shield_shown; (void)dmg_shown;
        (void)univ_x_shown;(void)univ_y_shown; (void)clk_shown;
#endif  /* SHOW_STATUS */

        /* ---- the ONE autopilot banner in the box (redraw every frame so the
         *      scrolling starfield can't nibble it, blank once when it goes).
         *      Flip-and-burn is an autopilot too, so it shares this banner;
         *      when it ends, the banner reverts to whatever mode we were in
         *      (AUTOPILOT, or a brief "MANUAL" flash). */
        if (fnb_prev && fnb_state == FNB_OFF && !auto_mode)
            manual_txt_until_ms = now_ms + MANUAL_TXT_MS;   /* FnB done -> back to manual */
        fnb_prev = (fnb_state != FNB_OFF);

        /* Second banner line is 29 wide so each state fully overwrites the last
         * (the "press J,K..." idle hint is the longest). */
        if (fnb_state != FNB_OFF)
        {
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW,     "AUTOPILOT");
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW + 1, "flip-and-burn                ");
            auto_shown = true;
        }
        else if (auto_mode)
        {
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW, "AUTOPILOT");
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW + 1,
                       auto_state != AUTO_IDLE ? "engaged                      "
                                               : "press J,K keys to rotate ship");
            auto_shown = true;
        }
        else if (now_ms < manual_txt_until_ms)   /* brief "MANUAL" on leaving autopilot */
        {
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW,     "MANUAL   ");
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW + 1, "                             ");
            auto_shown = true;
        }
        else if (auto_shown)
        {
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW,     "         ");
            lcd_putstr(AUTO_TXT_COL, AUTO_TXT_ROW + 1, "                             ");
            auto_shown = false;
        }

#if !SHOW_STATUS
        /* ---- small-screen (no status strip) low-resource warnings, down the box's
         *      right edge, over the starfield.  Yellow at <20% (fuel/power/
         *      ammo/shield) or >70% (damage), red at <10% / >85%.  Shield only
         *      warns while actually raised (0 = simply not deployed).  Redrawn
         *      every frame so the stars can't erode them; each row blanked
         *      once when its warning clears. */
        {
            static const char *const WARN_LBL[5] =
                { "FUEL", "PWR ", "AMMO", "SHLD", "HULL" };
            int wfp = fuel * 100 / FUEL_MAX;
            int wpp = pwr  * 100 / POWER_MAX;
            int wap = ammo * 100 / AMMO_MAX;
            int wlvl[5], wval[5];
            wlvl[0] = wfp < 10 ? 2 : (wfp < 20 ? 1 : 0);   wval[0] = wfp;
            wlvl[1] = wpp < 10 ? 2 : (wpp < 20 ? 1 : 0);   wval[1] = wpp;
            wlvl[2] = wap < 10 ? 2 : (wap < 20 ? 1 : 0);   wval[2] = wap;
            wlvl[3] = shield_lvl <= 0 ? 0
                    : (shield_lvl < 10 ? 2 : (shield_lvl < 20 ? 1 : 0));
            wval[3] = shield_lvl;
            wlvl[4] = damage > 85 ? 2 : (damage > 70 ? 1 : 0);   wval[4] = damage;

            for (int i = 0; i < 5; i++)
            {
                if (wlvl[i])
                {
                    char wb[10];
                    snprintf(wb, sizeof wb, "%s%3d%%", WARN_LBL[i], wval[i]);
                    lcd_set_foreground(wlvl[i] == 2 ? COL_WARN_CRIT : COL_WARN_LOW);
                    lcd_putstr(WARN_TXT_COL, WARN_TXT_ROW0 + i, wb);
                    warn_shown[i] = (signed char)wlvl[i];
                }
                else if (warn_shown[i] != 0)
                {
                    lcd_putstr(WARN_TXT_COL, WARN_TXT_ROW0 + i, "        ");
                    warn_shown[i] = 0;
                }
            }
            lcd_set_foreground(COL_STATUS_TXT);   /* restore for banners / GAME OVER */
        }
#endif  /* !SHOW_STATUS */

#ifdef PICOCALC
        /* ---- F5 screenshot status line: redraw every frame while up (the
         *      starfield would nibble it), blank once when it expires. */
        if (now_ms < shot_msg_until_ms)
        {
            lcd_set_foreground(COL_SHIP);
            lcd_putstr(SHOT_MSG_COL, SHOT_MSG_ROW, shot_msg);
            lcd_set_foreground(COL_STATUS_TXT);
            shot_msg_shown = true;
        }
        else if (shot_msg_shown)
        {
            lcd_putstr(SHOT_MSG_COL, SHOT_MSG_ROW, "                    ");
            shot_msg_shown = false;
        }
#endif

        /* ---- HINTS (difficulty 0 and 1 only).  Grey big_text captions along
         *      the bottom of the box, each armed for HINT_MS; on level 1 the
         *      "next alien" one also draws a grey arrow toward the nearest
         *      off-box alien.  Drawn last so they sit on top; the arrow erases
         *      its previous frame so it tracks the ship; the active caption is
         *      redrawn every frame so passing stars can't erode it, and only
         *      ONE caption uses the row at a time (priority move > next-alien >
         *      shields > fire > quit). ------------------------------------- */
        if (level <= 1)
        {
            uint32_t el_ms = now_ms - game_start_ms;    /* play time so far (ms) */
            bool na_show = false, s_show = false, fire_show = false;

            /* HINT 4 - "Arrow Keys to Move": levels 0 and 1, once as play starts. */
            if (!hint_move_armed)
            {
                hint_move_armed = true;
                hint_move_until = now_ms + HINT_MS;
            }
            bool move_show = now_ms < hint_move_until;

            /* HINT 5 - "Press Q to Quit": level 0 only, at the 1- and 2-min marks. */
            bool quit_show = false;
            if (level == 0)
            {
                if (hint_q_stage == 0 && el_ms >= HINT_Q_AT_1_MS)
                { hint_q_stage = 1; hint_q_until = now_ms + HINT_MS; }
                else if (hint_q_stage == 1 && el_ms >= HINT_Q_AT_2_MS)
                { hint_q_stage = 2; hint_q_until = now_ms + HINT_MS; }
                quit_show = now_ms < hint_q_until;
            }

            if (level == 1)
            {
                /* survey: nearest alien, and is any alien square on the box? */
                float bdx = 0.0f, bdy = 0.0f, bdd = 1e30f;
                bool  any_alive = false, any_on_box = false;
                for (int i = 0; i < ALIEN_ONSCREEN; i++)
                {
                    if (aliens[i].state != 1)
                        continue;
                    any_alive = true;
                    float ax = univ_delta(aliens[i].ux, univ_x);
                    float ay = univ_delta(aliens[i].uy, univ_y);
                    int   sxp = CENTRE_X + (int)ax, syp = CENTRE_Y + (int)ay;
                    if (sxp >= IN_LO && sxp < IN_HI_X && syp >= IN_LO && syp < IN_HI_Y)
                        any_on_box = true;
                    float dd = ax * ax + ay * ay;
                    if (dd < bdd) { bdd = dd; bdx = ax; bdy = ay; }
                }
                bool fnb_ended = (hint_fnb_on && fnb_state == FNB_OFF);
                hint_fnb_on = (fnb_state != FNB_OFF);

                /* HINT 1 - "Next Alien This Way": armed when the box is clear of
                 * aliens (some still alive); re-armed by a flip-and-burn ending. */
                bool cleared = (any_alive && !any_on_box && score < alien_max);
                if (!cleared)
                {
                    hint_na_armed = false;
                    hint_na_until = 0;
                }
                else if (!hint_na_armed || fnb_ended)
                {
                    hint_na_armed = true;
                    hint_na_until = now_ms + HINT_MS;
                }
                na_show = cleared && now_ms < hint_na_until;

                if (hint_na_drawn)                     /* erase last frame's arrow */
                {
                    tri_fill(hint_na_vx, hint_na_vy, COL_SPACE);
                    hint_na_drawn = false;
                }
                if (na_show)
                {
                    float m = sqrtf(bdx * bdx + bdy * bdy);
                    if (m < 1.0f) m = 1.0f;
                    float ux = bdx / m, uy = bdy / m;  /* unit toward nearest alien */
                    float rx = -uy, ry = ux;           /* perpendicular            */
                    hint_na_vx[0] = CENTRE_X + (int)(ux * HINT_ARROW_TIP);
                    hint_na_vy[0] = CENTRE_Y + (int)(uy * HINT_ARROW_TIP);
                    hint_na_vx[1] = CENTRE_X + (int)(-ux * HINT_ARROW_BACK + rx * HINT_ARROW_HALF);
                    hint_na_vy[1] = CENTRE_Y + (int)(-uy * HINT_ARROW_BACK + ry * HINT_ARROW_HALF);
                    hint_na_vx[2] = CENTRE_X + (int)(-ux * HINT_ARROW_BACK - rx * HINT_ARROW_HALF);
                    hint_na_vy[2] = CENTRE_Y + (int)(-uy * HINT_ARROW_BACK - ry * HINT_ARROW_HALF);
                    tri_fill(hint_na_vx, hint_na_vy, COL_HINT);
                    hint_na_drawn = true;
                }

                /* HINT 2 - "Use S for Shields": armed the moment the hull goes
                 * "wounded" with no shield up; re-armed when that clears. */
                bool nag_shields = ship_wounded && !shield_on;
                if (!nag_shields)
                {
                    hint_s_armed = false;
                    hint_s_until = 0;
                }
                else if (!hint_s_armed)
                {
                    hint_s_armed = true;
                    hint_s_until = now_ms + HINT_MS;
                }
                s_show = nag_shields && now_ms < hint_s_until;

                /* HINT 3 - "SPACE key to fire": once, if no torpedo has been
                 * fired (ammo untouched) after HINT_FIRE_AFTER_MS of play. */
                bool never_fired = (ammo == AMMO_MAX);
                if (never_fired && !hint_fire_armed && el_ms >= HINT_FIRE_AFTER_MS)
                {
                    hint_fire_armed = true;            /* one-shot */
                    hint_fire_until = now_ms + HINT_MS;
                }
                fire_show = never_fired && now_ms < hint_fire_until;
            }

            /* one caption row, priority move > next-alien > shields > fire >
             * quit: erase the outgoing caption once, (re)draw the active one
             * every frame so passing stars can't erode it. */
            int cap = move_show ? 4 : na_show ? 1 : s_show ? 2
                    : fire_show ? 3 : quit_show ? 5 : 0;
            if (hint_cap_shown && hint_cap_shown != cap)
                hint_caption(hint_cap_shown, COL_SPACE);
            if (cap)
                hint_caption(cap, COL_HINT);
            hint_cap_shown = cap;
        }

        sleep_ms(FRAME_MS);
    }
}


/* =========================================================================
 *  Small helpers   (rng_state / rng_next / rng_range are up near main())
 * ===================================================================== */

static inline int iroundf(float v) { return (int)(v < 0.0f ? v - 0.5f : v + 0.5f); }

/* Universal-grid helpers: the grid is a torus of side UNIVERSE_SIZE. A single
 * +/- is enough because every per-frame move is tiny next to the grid. */
int32_t univ_wrap(int32_t v)
{
    if (v < 0)                    v += UNIVERSE_SIZE;
    else if (v >= UNIVERSE_SIZE)  v -= UNIVERSE_SIZE;
    return v;
}
/* Shortest signed distance a - b along one axis of the torus, as a float. */
float univ_delta(int32_t a, int32_t b)
{
    int32_t d = a - b;
    if      (d >  UNIVERSE_HALF) d -= UNIVERSE_SIZE;
    else if (d < -UNIVERSE_HALF) d += UNIVERSE_SIZE;
    return (float)d;
}

/* Fill a rectangle, clipped to the interior of the box so nothing ever
 * overwrites the border or spills outside the BOX_W x BOX_H play area. */
static void px_rect(uint16_t colour, int x, int y, int w, int h)
{
    if (x < IN_LO) { w -= (IN_LO - x); x = IN_LO; }
    if (y < IN_LO) { h -= (IN_LO - y); y = IN_LO; }
    if (x + w > IN_HI_X) w = IN_HI_X - x;
    if (y + h > IN_HI_Y) h = IN_HI_Y - y;
    if (w > 0 && h > 0)
        lcd_solid_rectangle(colour, (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
}

/* Bresenham line, one clipped pixel at a time (used for the ship edges). */
static void draw_line(int x0, int y0, int x1, int y1, uint16_t colour)
{
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;)
    {
        px_rect(colour, x0, y0, 1, 1);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* =========================================================================
 *  Starfield
 * ===================================================================== */

typedef struct {
    float    x, y;              /* sub-pixel head position in the interior   */
    float    d;                 /* depth, DEPTH_MIN..1 (higher = nearer)     */
    uint16_t col;               /* colour for this depth (recomputed on wrap) */
    int      ax, ay, bx, by;    /* exact segment last drawn (tail a -> head b) */
} star_t;

static star_t stars[STAR_COUNT];

/* Pick a fresh depth for a (re)spawned star and the grey it draws in. The
 * r*r weighting puts more stars far away, so the near "lane" stays sparse and
 * its bright streaks read clearly. */
static void star_setdepth(star_t *s)
{
    float r = (float)rng_next() / 4294967296.0f;
    s->d = DEPTH_MIN + (1.0f - DEPTH_MIN) * r * r;

    float t   = (s->d - DEPTH_MIN) / (1.0f - DEPTH_MIN);   /* 0 far .. 1 near */
    int   lum = STAR_LUM_MIN + (int)(t * (255 - STAR_LUM_MIN));
    int   blu = lum + (int)((1.0f - t) * STAR_FAR_TINT);
    if (blu > 255) blu = 255;
    s->col = RGB(lum, lum, blu);
}

/* Draw a star: a single clipped pixel when tail == head, else a short line. */
static void seg_draw(int ax, int ay, int bx, int by, uint16_t col)
{
    if (ax == bx && ay == by)
        px_rect(col, ax, ay, 1, 1);
    else
        draw_line(ax, ay, bx, by, col);
}

void stars_init(void)
{
    for (int i = 0; i < STAR_COUNT; i++)
    {
        star_t *s = &stars[i];
        s->x = rng_range(IN_LO, IN_HI_X);
        s->y = rng_range(IN_LO, IN_HI_Y);
        star_setdepth(s);
        s->ax = s->bx = iroundf(s->x);
        s->ay = s->by = iroundf(s->y);
        seg_draw(s->ax, s->ay, s->bx, s->by, s->col);
    }
}

/* Advance every star by d * (scroll_x, scroll_y), wrapping any that fall off
 * an edge to a fresh random spot + depth on the opposite edge. Each star
 * erases the exact segment it drew last frame, then draws a dot or - if it is
 * moving fast enough - a streak back along its travel. */
void stars_update(float scroll_x, float scroll_y)
{
    for (int i = 0; i < STAR_COUNT; i++)
    {
        star_t *s = &stars[i];

        seg_draw(s->ax, s->ay, s->bx, s->by, COL_SPACE);   /* erase last frame */

        float mvx = scroll_x * s->d;
        float mvy = scroll_y * s->d;

        s->x += mvx;
        s->y += mvy;

        bool wrapped = false;
        if (s->x <  IN_LO)   { s->x += IN_SPAN_X; s->y = rng_range(IN_LO, IN_HI_Y); wrapped = true; }
        if (s->x >= IN_HI_X) { s->x -= IN_SPAN_X; s->y = rng_range(IN_LO, IN_HI_Y); wrapped = true; }
        if (s->y <  IN_LO)   { s->y += IN_SPAN_Y; s->x = rng_range(IN_LO, IN_HI_X); wrapped = true; }
        if (s->y >= IN_HI_Y) { s->y -= IN_SPAN_Y; s->x = rng_range(IN_LO, IN_HI_X); wrapped = true; }
        if (wrapped)
            star_setdepth(s);

        int hx = iroundf(s->x), hy = iroundf(s->y);   /* head */
        int tx = hx, ty = hy;                          /* tail (dot by default) */

        float mv2 = mvx * mvx + mvy * mvy;
        if (mv2 >= STREAK_MIN * STREAK_MIN)
        {
            float m    = sqrtf(mv2);
            float len  = m * STREAK_GAIN;
            if (len > STREAK_MAX) len = STREAK_MAX;
            float back = len / m;                  /* scale move -> tail offset */
            tx = iroundf(s->x - mvx * back);
            ty = iroundf(s->y - mvy * back);
        }

        seg_draw(tx, ty, hx, hy, s->col);
        s->ax = tx; s->ay = ty; s->bx = hx; s->by = hy;
    }
}

/* =========================================================================
 *  Ship  (and the thrust flame - both are just rotated triangles)
 * ===================================================================== */

/* Rotate a triangle given in ship-local coords (nose towards -y, origin at the
 * ship centre) by "angle" (radians, 0 = nose up, growing clockwise) and drop
 * it onto the screen at the box centre. */
static void rot_tri(float angle, const float lx[3], const float ly[3],
                    int vx[3], int vy[3])
{
    const float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < 3; i++)
    {
        vx[i] = CENTRE_X + iroundf(lx[i] * c - ly[i] * s);
        vy[i] = CENTRE_Y + iroundf(lx[i] * s + ly[i] * c);
    }
}

/* The ship: nose ahead, two tail corners behind. */
void ship_verts(float angle, int vx[3], int vy[3])
{
    const float lx[3] = {        0.0f, -(float)SHIP_HALF, (float)SHIP_HALF };
    const float ly[3] = { -(float)SHIP_NOSE,  (float)SHIP_TAIL, (float)SHIP_TAIL };
    rot_tri(angle, lx, ly, vx, vy);
}

/* The flame: a short triangle hanging off the tail, pointing straight back
 * (opposite the nose). "len" is how far the tip sticks out past the tail and
 * is flickered frame to frame by the caller. */
void flame_verts(float angle, int len, int vx[3], int vy[3])
{
    const float lx[3] = { -(float)FLAME_HALF, (float)FLAME_HALF, 0.0f };
    const float ly[3] = {  (float)SHIP_TAIL,  (float)SHIP_TAIL,
                           (float)(SHIP_TAIL + len) };
    rot_tri(angle, lx, ly, vx, vy);
}

/* One retro-brake flame off a front side of the hull, pointing FORWARD (the
 * opposite end from the main flame) so it reads as "slowing down".  side = -1
 * for the left flame, +1 for the right. */
void brake_verts(float angle, int side, int vx[3], int vy[3])
{
    const float sgn = (float)side;
    const float lx[3] = { sgn * (float)(BRAKE_OFF_X + BRAKE_HALF),
                          sgn * (float)(BRAKE_OFF_X - BRAKE_HALF),
                          sgn * (float)(BRAKE_OFF_X + 1) };
    const float ly[3] = { (float)BRAKE_OFF_Y,
                          (float)BRAKE_OFF_Y,
                          (float)(BRAKE_OFF_Y - BRAKE_LEN) };
    rot_tri(angle, lx, ly, vx, vy);
}

/* One attitude (RCS) jet: a small triangle whose base sits on a front side of
 * the hull and whose tip points straight OUT to that side.  side = -1 for a
 * port (left) jet, +1 for starboard - the caller fires the jet on the side
 * OPPOSITE the requested turn so the reaction yaws the nose that way. */
void rcs_verts(float angle, int side, int vx[3], int vy[3])
{
    const float sgn = (float)side;
    const float lx[3] = { sgn * (float)RCS_OFF_X,
                          sgn * (float)RCS_OFF_X,
                          sgn * (float)(RCS_OFF_X + RCS_LEN) };
    const float ly[3] = { (float)(RCS_OFF_Y - RCS_HALF),
                          (float)(RCS_OFF_Y + RCS_HALF),
                          (float)RCS_OFF_Y };
    rot_tri(angle, lx, ly, vx, vy);
}

void tri_outline(const int vx[3], const int vy[3], uint16_t colour)
{
    draw_line(vx[0], vy[0], vx[1], vy[1], colour);
    draw_line(vx[1], vy[1], vx[2], vy[2], colour);
    draw_line(vx[2], vy[2], vx[0], vy[0], colour);
}

/* Solid triangle: for each scanline between the top and bottom vertex, fill the
 * span between where the edges cross that row.  Same vertex arrays as
 * tri_outline(); px_rect() clips every span to the box interior. */
void tri_fill(const int vx[3], const int vy[3], uint16_t colour)
{
    int ymin = vy[0], ymax = vy[0];
    for (int i = 1; i < 3; i++)
    {
        if (vy[i] < ymin) ymin = vy[i];
        if (vy[i] > ymax) ymax = vy[i];
    }
    for (int y = ymin; y <= ymax; y++)
    {
        int xl = 1 << 15, xr = -(1 << 15);
        for (int i = 0; i < 3; i++)
        {
            int j  = (i + 1) % 3;
            int ya = vy[i], yb = vy[j];
            if (ya == yb)                       continue;   /* horizontal edge */
            if ((y < ya && y < yb) || (y > ya && y > yb)) continue;
            int x = vx[i] + (vx[j] - vx[i]) * (y - ya) / (yb - ya);
            if (x < xl) xl = x;
            if (x > xr) xr = x;
        }
        if (xl <= xr)
            px_rect(colour, xl, y, xr - xl + 1, 1);
    }
}

/* The shield: a hexagon ring of radius SHIELD_R around the ship centre, spun
 * with the ship so it reads as "energised". */
void shield_verts(float angle, int vx[6], int vy[6])
{
    for (int i = 0; i < 6; i++)
    {
        float a = angle + (float)i * 1.04719755f;   /* i * PI/3 */
        vx[i] = CENTRE_X + iroundf((float)SHIELD_R * sinf(a));
        vy[i] = CENTRE_Y - iroundf((float)SHIELD_R * cosf(a));
    }
}

void shield_draw(const int vx[6], const int vy[6], uint16_t colour)
{
    for (int i = 0; i < 6; i++)
        draw_line(vx[i], vy[i], vx[(i + 1) % 6], vy[(i + 1) % 6], colour);
}

/* =========================================================================
 *  Torpedoes  (screen-space; faster than the ship can ever travel)
 * ===================================================================== */

typedef struct {
    float x, y;             /* sub-pixel head position in the box         */
    float dx, dy;           /* unit heading, fixed at spawn               */
    bool  live;
    int   ax, ay, bx, by;   /* exact segment last drawn (tail -> head)    */
} torpedo_t;

static torpedo_t torpedoes[MAX_TORPEDOES];

void torpedoes_init(void)
{
    for (int i = 0; i < MAX_TORPEDOES; i++)
        torpedoes[i].live = false;
}

/* Fire one torpedo from the nose along the current heading. Returns true if a
 * torpedo actually launched, false if all MAX_TORPEDOES slots are in flight (so
 * the caller only spends a round when one really left the muzzle). */
bool torpedo_fire(float angle)
{
    const float ux = sinf(angle), uy = -cosf(angle);

    for (int i = 0; i < MAX_TORPEDOES; i++)
    {
        torpedo_t *t = &torpedoes[i];
        if (t->live)
            continue;

        t->x  = (float)CENTRE_X + ux * (float)(SHIP_NOSE + 3);
        t->y  = (float)CENTRE_Y + uy * (float)(SHIP_NOSE + 3);
        t->dx = ux;
        t->dy = uy;
        t->live = true;
        t->ax = t->bx = iroundf(t->x);
        t->ay = t->by = iroundf(t->y);
        seg_draw(t->ax, t->ay, t->bx, t->by, COL_TORPEDO);
        return true;
    }
    return false;
}

/* Advance every live torpedo; kill (and stop drawing) any that leaves the box.
 * Like the stars, each torpedo erases the exact segment it drew last frame. */
void torpedoes_update(void)
{
    for (int i = 0; i < MAX_TORPEDOES; i++)
    {
        torpedo_t *t = &torpedoes[i];
        if (!t->live)
            continue;

        seg_draw(t->ax, t->ay, t->bx, t->by, COL_SPACE);   /* erase last frame */

        t->x += t->dx * TORPEDO_SPEED;
        t->y += t->dy * TORPEDO_SPEED;

        if (t->x < IN_LO || t->x >= IN_HI_X || t->y < IN_LO || t->y >= IN_HI_Y)
        {
            t->live = false;                               /* reached the edge */
            continue;
        }

        int hx = iroundf(t->x), hy = iroundf(t->y);
        int tx = iroundf(t->x - t->dx * (float)TORPEDO_LEN);
        int ty = iroundf(t->y - t->dy * (float)TORPEDO_LEN);
        seg_draw(tx, ty, hx, hy, COL_TORPEDO);
        t->ax = tx; t->ay = ty; t->bx = hx; t->by = hy;
    }
}

/* =========================================================================
 *  Alien torpedoes  (universe-grid; a shooter re-aims at the ship every shot,
 *  then the torpedo flies a fixed grid line - moving the ship dodges it)
 * ===================================================================== */

typedef struct {
    int32_t ux, uy;        /* universe position of the torpedo head         */
    float   fx, fy;        /* sub-pixel carry for the move step            */
    float   dx, dy;        /* unit heading, fixed at fire (toward the ship)*/
    bool    live;
    int     ax, ay, bx, by;/* exact screen segment last drawn (tail->head) */
    bool    drawn;         /* was it on the box last frame?                */
} atorpedo_t;

static atorpedo_t atorpedoes[ALIEN_TORPEDO_MAX];

void atorpedoes_init(void)
{
    for (int i = 0; i < ALIEN_TORPEDO_MAX; i++)
    {
        atorpedoes[i].live  = false;
        atorpedoes[i].drawn = false;
    }
}

/* Fire one torpedo from alien universe point (aux,auy) at the ship's universe
 * point (sux,suy) as it is right now, shifted `aim_below` px down-screen (the
 * rookie's shrinking miss; 0 = dead on).  The heading is fixed here for the
 * torpedo's whole life - it never curves - so the ship can slip a straight shot
 * by moving before it lands.  Returns true if a slot was free. */
static bool atorpedo_fire(int32_t aux, int32_t auy,
                         int32_t sux, int32_t suy, float aim_below)
{
    float dx = univ_delta(sux, aux);
    float dy = univ_delta(suy, auy) + aim_below;
    float m  = sqrtf(dx * dx + dy * dy);
    if (m < 0.001f)
        return false;
    dx /= m; dy /= m;

    for (int i = 0; i < ALIEN_TORPEDO_MAX; i++)
    {
        atorpedo_t *t = &atorpedoes[i];
        if (t->live)
            continue;
        t->ux = univ_wrap(aux + (int32_t)(dx * (float)(ALIEN_HALF + 2)));
        t->uy = univ_wrap(auy + (int32_t)(dy * (float)(ALIEN_HALF + 2)));
        t->fx = t->fy = 0.0f;
        t->dx = dx; t->dy = dy;
        t->live = true;
        int hx = CENTRE_X + (int)univ_delta(t->ux, sux);
        int hy = CENTRE_Y + (int)univ_delta(t->uy, suy);
        t->ax = t->bx = hx;
        t->ay = t->by = hy;
        t->drawn = true;
        seg_draw(hx, hy, hx, hy, COL_ATORPEDO);
        return true;
    }
    return false;
}

/* Advance every live alien torpedo along its fixed grid heading; kill any whose
 * screen point leaves the box.  Erase-then-redraw the screen segment, like the
 * player torpedoes and the stars. */
void atorpedoes_update(int32_t sux, int32_t suy)
{
    for (int i = 0; i < ALIEN_TORPEDO_MAX; i++)
    {
        atorpedo_t *t = &atorpedoes[i];
        if (!t->live)
            continue;

        if (t->drawn)
            seg_draw(t->ax, t->ay, t->bx, t->by, COL_SPACE);   /* erase last frame */

        t->fx += t->dx * ATORPEDO_SPEED;
        t->fy += t->dy * ATORPEDO_SPEED;
        int sx = (int)t->fx, sy = (int)t->fy;
        t->fx -= (float)sx;  t->fy -= (float)sy;
        t->ux = univ_wrap(t->ux + sx);
        t->uy = univ_wrap(t->uy + sy);

        int hx = CENTRE_X + (int)univ_delta(t->ux, sux);
        int hy = CENTRE_Y + (int)univ_delta(t->uy, suy);
        if (hx < IN_LO || hx >= IN_HI_X || hy < IN_LO || hy >= IN_HI_Y)
        {
            t->live  = false;
            t->drawn = false;
            continue;
        }

        int tx = hx - iroundf(t->dx * (float)TORPEDO_LEN);
        int ty = hy - iroundf(t->dy * (float)TORPEDO_LEN);
        seg_draw(tx, ty, hx, hy, COL_ATORPEDO);
        t->ax = tx; t->ay = ty; t->bx = hx; t->by = hy;
        t->drawn = true;
    }
}

/* Any live alien torpedo that reaches the ship: with the shield up it costs the
 * shield SHIELD_HIT_TORPEDO and the torpedo is spent; with it down the game is
 * over.  A cloaked ship can't be hit (aliens never aimed at it). */
void atorpedoes_hit_ship(int32_t sux, int32_t suy,
                              int *shield_lvl, int *damage,
                              bool cloaked, bool *ship_dead)
{
    if (cloaked)
        return;
    float r = (*shield_lvl > 0) ? (float)SHIELD_R : (float)SHIP_HALF;
    for (int i = 0; i < ALIEN_TORPEDO_MAX; i++)
    {
        atorpedo_t *t = &atorpedoes[i];
        if (!t->live)
            continue;
        float dx = univ_delta(t->ux, sux), dy = univ_delta(t->uy, suy);
        if (dx * dx + dy * dy < r * r)
        {
            if (t->drawn)
                seg_draw(t->ax, t->ay, t->bx, t->by, COL_SPACE);   /* erase tracer */
            t->live  = false;
            t->drawn = false;
            if (*shield_lvl > 0)
            {
                *shield_lvl -= SHIELD_HIT_TORPEDO;     /* the shield eats it */
                if (*shield_lvl < 0) *shield_lvl = 0;
            }
            else
            {
                *damage += SHIP_DMG_TORPEDO;          /* the hull takes 10% */
                if (*damage > DMG_MAX) *damage = DMG_MAX;
                if (*damage >= DMG_MAX) *ship_dead = true;
            }
        }
    }
}

/* =========================================================================
 *  Enemy aliens  (squares living on the universal grid; they hunt the ship)
 * ===================================================================== */

/* Solid square, clipped to the box by px_rect().  Used for both the draw and
 * the erase, so a moving alien leaves no trail. */
static void alien_fill(int x, int y, uint16_t col)
{
    px_rect(col, x, y, ALIEN_SIZE, ALIEN_SIZE);
}

/* Place an alien on the grid: a random bearing ALIEN_SPAWN_MIN..MAX px from the
 * ship, with its own random travel heading.  It drifts until it comes on
 * screen and spots the ship.  `count` = this is a NEW alien entering the game
 * (bumps aliens_spawned toward alien_max); pass false when merely relocating an
 * existing one (a drifter recycled by the cull), which must not eat the quota. */
static void alien_spawn(alien_t *a, int32_t sux, int32_t suy, bool count)
{
    float bearing = rng_range(0.0f, 6.2831853f);
    float dist    = rng_range(ALIEN_SPAWN_MIN, alien_spawn_max);
    a->ux = univ_wrap(sux + (int32_t)(cosf(bearing) * dist));
    a->uy = univ_wrap(suy + (int32_t)(sinf(bearing) * dist));

    float heading = rng_range(0.0f, 6.2831853f);
    a->dirx = cosf(heading);
    a->diry = sinf(heading);

    a->speed = alien_speed;         /* the current default; override per-alien later */
    a->fx = a->fy = 0.0f;
    a->hunting = false;
    a->spot_pause = 0;
    a->state = 1;
    a->flash = 0;
    a->pdrawn = false;
    a->lrs_drawn = false;
    /* Every ALIEN_SHOOTER_EVERY-th NEW alien is a stationary shooter with a
     * full clip; a merely-relocated one (count == false) is a plain drifter. */
    a->torpedoes = (count && (aliens_spawned % ALIEN_SHOOTER_EVERY) == 0)
                 ? ALIEN_TORPEDOES : 0;
    a->shots        = 0;
    a->next_fire_ms = 0;
    a->seen_ms      = 0;
    a->rookie       = false;
    a->dmg          = 0;
    a->wounded      = false;
    if (count)
        aliens_spawned++;
}

static void lrs_tri(int dir, int ax, int ay, uint16_t col);   /* defined in the LRS section below */

/* Drop a fresh "rookie" shooter onto the right edge of the box - the same
 * deliberately-bad-aim alien that levels 1-3 seed at the start.  run_game()
 * calls it (count = false) whenever the player has sat with no key pressed for
 * IDLE_ROOKIE_MS; aliens_init() calls it once (count = true) at the start.
 *
 * There is only ever ONE rookie alive: if one already is, this is a no-op.
 * aliens_respawn() keeps all ALIEN_ONSCREEN slots full, so an idle-nudge rookie
 * usually has no empty slot - it then recycles a non-shooter that hasn't locked
 * on (or, failing that, any non-shooter).  It gives up only if every slot holds
 * a shooter. */
void alien_spawn_rookie(int32_t sux, int32_t suy, bool count)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
        if (aliens[i].state != 0 && aliens[i].rookie)
            return;                                  /* one rookie at a time */

    int slot = -1;
    for (int i = 0; i < ALIEN_ONSCREEN && slot < 0; i++)      /* free slot   */
        if (aliens[i].state == 0) slot = i;
    for (int i = 0; i < ALIEN_ONSCREEN && slot < 0; i++)      /* calm drifter */
        if (aliens[i].state == 1 && !aliens[i].hunting && aliens[i].torpedoes == 0) slot = i;
    for (int i = 0; i < ALIEN_ONSCREEN && slot < 0; i++)      /* any non-shooter */
        if (aliens[i].state == 1 && aliens[i].torpedoes == 0) slot = i;
    if (slot < 0)
        return;                                       /* every slot is a shooter */

    /* Recycling a live alien: wipe whatever it had on screen now, because
     * alien_spawn() clears pdrawn / lrs_drawn and the frame's erase pass won't. */
    if (aliens[slot].pdrawn)
        alien_fill(aliens[slot].px, aliens[slot].py, COL_SPACE);
    if (aliens[slot].lrs_drawn)
        lrs_tri(aliens[slot].lrs_dir, aliens[slot].lrs_x, aliens[slot].lrs_y, COL_SPACE);

    float dy = rng_range(ALIEN_INTRO_YMIN, ALIEN_INTRO_YMAX);
    if (rng_next() & 1u)
        dy = -dy;
    alien_spawn(&aliens[slot], sux, suy, count);
    aliens[slot].ux        = univ_wrap(sux + ALIEN_INTRO_X);
    aliens[slot].uy        = univ_wrap(suy + (int32_t)dy);
    aliens[slot].torpedoes = ALIEN_TORPEDOES;   /* force a clip, whatever the count */
    aliens[slot].rookie    = true;              /* the bad shot that nudges you */
}

void aliens_init(int level, int32_t sux, int32_t suy)
{
    aliens_spawned = 0;
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
    {
        aliens[i].state    = 0;
        aliens[i].pdrawn   = false;
        aliens[i].lrs_drawn = false;
        aliens[i].speed    = alien_speed;
        aliens[i].torpedoes  = 0;
        aliens[i].shots    = 0;
        aliens[i].seen_ms  = 0;
        aliens[i].rookie   = false;
        aliens[i].dmg      = 0;
        aliens[i].wounded  = false;
    }

    /* Levels 1-3: seed one rookie shooter on the right edge of the box so it is
     * obvious what to shoot at, and a beginner gets eased in with soft shots.
     * (run_game() drops in another whenever the player idles too long.) */
    if (level >= 1)
        alien_spawn_rookie(sux, suy, true);
}

/* Per-frame: move each alien (drift, or - once it has spotted the ship -
 * charge it), resolve alien<->alien and alien<->ship contact, run the death
 * flash (which bumps *score on completion), and refill empty slots until
 * alien_max have been used up.  A ram always wrecks the alien; with the shield
 * up it costs *shield_lvl SHIELD_HIT_RAM, with it down it adds SHIP_DMG_RAM to
 * *damage and sets *ship_dead if that hits DMG_MAX. */
void aliens_update(int32_t sux, int32_t suy, int level,
                          int *shield_lvl, int *damage, bool cloaked,
                          int *score, bool *ship_dead)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)          /* move */
    {
        alien_t *a = &aliens[i];
        if (a->state != 1)
            continue;

        float rx = univ_delta(a->ux, sux);           /* alien offset from ship */
        float ry = univ_delta(a->uy, suy);
        float d  = sqrtf(rx * rx + ry * ry);

        /* A rookie that has slid off the play box (the ship flew out from under
         * it) is just removed - no death flash, no score. */
        if (a->rookie)
        {
            float rsx = (float)CENTRE_X + rx;
            float rsy = (float)CENTRE_Y + ry;
            if (rsx < IN_LO - ALIEN_HALF || rsx > IN_HI_X + ALIEN_HALF ||
                rsy < IN_LO - ALIEN_HALF || rsy > IN_HI_Y + ALIEN_HALF)
            {
                a->state = 0;
                continue;
            }
        }

        /* ---- A LOADED alien (torpedoes > 0) NEVER drifts, hunts or wanders -
         *      it holds its exact position and does nothing but fire (and
         *      aliens_fire() only fires while its square is on the play box with
         *      the ship).  Recycled only if the ship flies right off
         *      (d > ALIEN_CULL_DIST).
         *
         *      EXCEPTION: alien_ram_and_fire (off by default, a future
         *      high-difficulty knob) lets a loaded alien move / hunt / ram AND
         *      keep shooting - it just falls through to the normal logic. ---- */
        if (a->torpedoes > 0 && !alien_ram_and_fire)
        {
            a->hunting    = false;      /* a loaded alien is never a hunter */
            a->spot_pause = 0;
            if (d > ALIEN_CULL_DIST)
                alien_spawn(a, sux, suy, false);
            continue;
        }

        /* ---- from here the alien MAY drift / spot / hunt ---- */

        /* A spent shooter (fired at least one round, now empty) - or, with
         * alien_ram_and_fire on, a still-loaded shooter - abandons its post and
         * charges the ship to ram it, at ANY difficulty (level 1 included). */
        bool charges = (a->shots > 0) || (alien_ram_and_fire && a->torpedoes > 0);
        if (charges)
        {
            a->hunting    = true;
            a->spot_pause = 0;
        }

        if (level == 1 && !charges)   /* difficulty 1: inert aliens - never
                                    drift, spot or hunt.  They can still be shot
                                    or run into (resolved in the loops below). */
            continue;

        if (level == 3 && !cloaked)   /* difficulty 3: the whole universe charges
                                        the ship at once - no line of sight
                                        needed (a cloaked ship is still unseen). */
        {
            a->hunting    = true;
            a->spot_pause = 0;
        }

        if (!a->hunting)
        {
            float sxp = (float)CENTRE_X + rx;         /* where it is on screen */
            float syp = (float)CENTRE_Y + ry;
            bool on_screen = sxp > IN_LO - ALIEN_HALF && sxp < IN_HI_X + ALIEN_HALF &&
                             syp > IN_LO - ALIEN_HALF && syp < IN_HI_Y + ALIEN_HALF;
            if (on_screen && !cloaked)                /* a cloaked ship is unseen */
            {
                a->hunting    = true;                 /* it has seen us */
                a->spot_pause = SPOT_PAUSE;           /* ... stop a beat, then charge */
            }
            else if (d > ALIEN_CULL_DIST)
            {
                alien_spawn(a, sux, suy, false);      /* drifted off - recycle,
                                                        NOT a new alien */
                continue;
            }
        }

        float mx, my;                                 /* this frame's heading */
        if (!a->hunting)             { mx = a->dirx;  my = a->diry;  }
        else if (a->spot_pause > 0)  { a->spot_pause--; mx = my = 0.0f; }
        else if (d > 0.5f)           { mx = -rx / d;  my = -ry / d;  }
        else                        { mx = my = 0.0f; }

        a->fx += a->speed * mx;
        a->fy += a->speed * my;
        int stepx = (int)a->fx, stepy = (int)a->fy;
        a->fx -= (float)stepx;
        a->fy -= (float)stepy;
        a->ux = univ_wrap(a->ux + stepx);
        a->uy = univ_wrap(a->uy + stepy);
    }

    for (int i = 0; i < ALIEN_ONSCREEN; i++)          /* alien vs alien */
    {
        if (aliens[i].state != 1)
            continue;
        for (int j = i + 1; j < ALIEN_ONSCREEN; j++)
        {
            if (aliens[j].state != 1)
                continue;
            if (fabsf(univ_delta(aliens[i].ux, aliens[j].ux)) < ALIEN_SIZE &&
                fabsf(univ_delta(aliens[i].uy, aliens[j].uy)) < ALIEN_SIZE)
            {
                aliens[i].state = aliens[j].state = 2;
                aliens[i].flash = aliens[j].flash = ALIEN_FLASH;
            }
        }
    }

    for (int i = 0; i < ALIEN_ONSCREEN; i++)          /* alien vs ship */
    {
        alien_t *a = &aliens[i];
        if (a->state != 1)
            continue;
        float dx = univ_delta(a->ux, sux);
        float dy = univ_delta(a->uy, suy);
        bool  shielded = (*shield_lvl > 0);
        float reach = shielded ? (float)(ALIEN_HALF + SHIELD_R)
                               : (float)(ALIEN_HALF + SHIP_HALF);
        if (dx * dx + dy * dy < reach * reach)
        {
            a->state = 2;                             /* a ram wrecks the alien */
            a->flash = ALIEN_FLASH;
            if (shielded)
            {
                *shield_lvl -= SHIELD_HIT_RAM;        /* the shield eats it */
                if (*shield_lvl < 0) *shield_lvl = 0;
            }
            else
            {
                *damage += SHIP_DMG_RAM;             /* the hull takes 40% */
                if (*damage > DMG_MAX) *damage = DMG_MAX;
                if (*damage >= DMG_MAX) *ship_dead = true;
            }
        }
    }

    for (int i = 0; i < ALIEN_ONSCREEN; i++)          /* death flash -> gone */
    {
        alien_t *a = &aliens[i];
        if (a->state == 2 && --a->flash <= 0)
        {
            a->state = 0;
            (*score)++;
        }
    }
    /* NB: emptied slots are NOT refilled here - that has to wait until
     * aliens_erase() has wiped the dead alien's last square (see
     * aliens_respawn(), called after the erase in the render section).
     * Refilling here would run alien_spawn() -> pdrawn = false and orphan
     * the white outline on screen. */
}

/* Any live torpedo inside an alien square: wipe the torpedo, add ALIEN_HIT_DMG to
 * that alien's internal damage.  At >= ALIEN_DMG_MAX it starts the death flash
 * and arms the happy "alien destroyed" chime (*chime_until_ms); otherwise it
 * just goes (or stays) "wounded" - yellow once dmg > ALIEN_WOUND_ON.  One
 * torpedo per alien per frame (the break).  Called just after
 * torpedoes_update(), so tracers are current. */
void torpedoes_hit_aliens(int32_t sux, int32_t suy,
                          uint32_t now_ms, uint32_t *chime_until_ms)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
    {
        alien_t *a = &aliens[i];
        if (a->state != 1)
            continue;
        float asx = (float)CENTRE_X + univ_delta(a->ux, sux);
        float asy = (float)CENTRE_Y + univ_delta(a->uy, suy);
        for (int b = 0; b < MAX_TORPEDOES; b++)
        {
            torpedo_t *tp = &torpedoes[b];   /* b: torpedo slot index */
            if (!tp->live)
                continue;
            bool hit =
                (fabsf(tp->x - asx) <= ALIEN_HALF && fabsf(tp->y - asy) <= ALIEN_HALF) ||
                (fabsf((float)tp->ax - asx) <= ALIEN_HALF &&
                 fabsf((float)tp->ay - asy) <= ALIEN_HALF);
            if (hit)
            {
                seg_draw(tp->ax, tp->ay, tp->bx, tp->by, COL_SPACE);   /* erase tracer */
                tp->live = false;
                a->dmg += ALIEN_HIT_DMG;
                if (a->dmg >= ALIEN_DMG_MAX)
                {
                    a->state = 2;
                    a->flash = ALIEN_FLASH;
                    *chime_until_ms = now_ms + CHIME_MS;   /* happy chime: got one! */
                }
                else if (a->dmg > ALIEN_WOUND_ON)
                {
                    a->wounded = true;                /* red -> yellow */
                }
                break;
            }
        }
    }
}

/* The crew mends DMG_REPAIR_STEP off every live alien's internal damage each
 * repair tick (the ship's hull damage heals at the same rate, in run_game).
 * Yellow reverts to red once the damage drops below ALIEN_WOUND_OFF. */
void aliens_heal(void)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
    {
        alien_t *a = &aliens[i];
        if (a->state != 1 || a->dmg <= 0)
            continue;
        a->dmg -= DMG_REPAIR_STEP;
        if (a->dmg < 0) a->dmg = 0;
        if (a->dmg < ALIEN_WOUND_OFF)
            a->wounded = false;                       /* yellow -> red */
    }
}

/* Each loaded shooter alien whose square is on the play box launches one torpedo
 * at the ship + chirps the reversed "pew" - but not until ALIEN_ARM_MS after it
 * was first seen there ("loading up"), and then no faster than the re-fire gap:
 * ALIEN_FIRE_MS at level 3, doubled (half the rate) at the gentler levels 1-2.
 * Each shot is re-aimed here, so a torpedo already in flight keeps its own line
 * and the ship can move off the spot it was aimed at.  A cloaked ship is
 * unseen, so nobody fires. */
void aliens_fire(int32_t sux, int32_t suy, int level, bool cloaked,
                        uint32_t now_ms, uint32_t *apew_until_ms)
{
    if (cloaked)
        return;
    uint32_t refire_ms = (level <= 2) ? 2u * ALIEN_FIRE_MS : ALIEN_FIRE_MS;
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
    {
        alien_t *a = &aliens[i];
        if (a->state != 1 || a->torpedoes <= 0)
            continue;

        int asx = CENTRE_X + (int)univ_delta(a->ux, sux);
        int asy = CENTRE_Y + (int)univ_delta(a->uy, suy);
        bool on_box = asx > IN_LO - ALIEN_HALF && asx < IN_HI_X + ALIEN_HALF &&
                      asy > IN_LO - ALIEN_HALF && asy < IN_HI_Y + ALIEN_HALF;
        if (!on_box)
            continue;

        if (a->seen_ms == 0)                    /* first sighting: start arming */
            a->seen_ms = now_ms;
        if (now_ms < a->seen_ms + ALIEN_ARM_MS || now_ms < a->next_fire_ms)
            continue;                           /* still loading, or cooling down */

        /* A rookie's aim tightens with each round it has fired: shot 1 aims
         * ALIEN_ROOKIE_MISS px low, shrinking linearly to 0 by shot
         * ALIEN_ROOKIE_SHOTS, dead on from then on.  Keyed on the alien's own
         * `shots`, so it is unaffected by clip size or a reload. */
        float aim_below = 0.0f;
        if (a->rookie && a->shots < ALIEN_ROOKIE_SHOTS)
            aim_below = (float)ALIEN_ROOKIE_MISS
                      * (float)(ALIEN_ROOKIE_SHOTS - 1 - a->shots)
                      / (float)(ALIEN_ROOKIE_SHOTS - 1);

        if (atorpedo_fire(a->ux, a->uy, sux, suy, aim_below))
        {
            a->torpedoes--;
            a->shots++;
            a->next_fire_ms = now_ms + refire_ms;
            *apew_until_ms  = now_ms + PEW_MS;
        }
    }
}

void aliens_erase(void)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
        if (aliens[i].pdrawn)
            alien_fill(aliens[i].px, aliens[i].py, COL_SPACE);
}

/* Refill emptied slots.  Call this AFTER aliens_erase() each frame, so a slot
 * that emptied this frame has already had its last square wiped before
 * alien_spawn() clears pdrawn. */
void aliens_respawn(int32_t sux, int32_t suy, int level)
{
    if (level == 0)                       /* difficulty 0: an empty universe */
        return;
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
        if (aliens[i].state == 0 && aliens_spawned < alien_max)
            alien_spawn(&aliens[i], sux, suy, true);   /* a genuine new alien */
}

void aliens_draw(int32_t sux, int32_t suy)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
    {
        alien_t *a = &aliens[i];
        if (a->state == 0)
        {
            a->pdrawn = false;
            continue;
        }

        int x = CENTRE_X + (int)univ_delta(a->ux, sux) - ALIEN_HALF;
        int y = CENTRE_Y + (int)univ_delta(a->uy, suy) - ALIEN_HALF;

        if (x > IN_HI_X || y > IN_HI_Y || x + ALIEN_SIZE < IN_LO || y + ALIEN_SIZE < IN_LO)
        {
            a->pdrawn = false;                 /* off the play box - don't draw */
            continue;
        }

        uint16_t col;
        if (a->state == 2 && (a->flash & 1))
            col = COL_ALIEN_FLASH;
        else
            col = a->wounded ? COL_ALIEN_WOUND : COL_ALIEN;
        alien_fill(x, y, col);
        a->px = x; a->py = y; a->pdrawn = true;
    }
}

/* --- Long Range Sensors: a small triangular blip just inside the box edge,
 * pointing at an off-screen alien that lines up with the box on exactly one
 * axis (straight up / down / left / right of it) ------------------------- */

/* Outline triangle, apex at (ax, ay), pointing dir (1=up,2=down,3=left,4=right). */
static void lrs_tri(int dir, int ax, int ay, uint16_t col)
{
    switch (dir)
    {
        case 1:  /* up */
            draw_line(ax, ay, ax - LRS_HALF, ay + LRS_LEN, col);
            draw_line(ax, ay, ax + LRS_HALF, ay + LRS_LEN, col);
            draw_line(ax - LRS_HALF, ay + LRS_LEN, ax + LRS_HALF, ay + LRS_LEN, col);
            break;
        case 2:  /* down */
            draw_line(ax, ay, ax - LRS_HALF, ay - LRS_LEN, col);
            draw_line(ax, ay, ax + LRS_HALF, ay - LRS_LEN, col);
            draw_line(ax - LRS_HALF, ay - LRS_LEN, ax + LRS_HALF, ay - LRS_LEN, col);
            break;
        case 3:  /* left */
            draw_line(ax, ay, ax + LRS_LEN, ay - LRS_HALF, col);
            draw_line(ax, ay, ax + LRS_LEN, ay + LRS_HALF, col);
            draw_line(ax + LRS_LEN, ay - LRS_HALF, ax + LRS_LEN, ay + LRS_HALF, col);
            break;
        case 4:  /* right */
            draw_line(ax, ay, ax - LRS_LEN, ay - LRS_HALF, col);
            draw_line(ax, ay, ax - LRS_LEN, ay + LRS_HALF, col);
            draw_line(ax - LRS_LEN, ay - LRS_HALF, ax - LRS_LEN, ay + LRS_HALF, col);
            break;
    }
}

void lrs_erase(void)
{
    for (int i = 0; i < ALIEN_ONSCREEN; i++)
        if (aliens[i].lrs_drawn)
            lrs_tri(aliens[i].lrs_dir, aliens[i].lrs_x, aliens[i].lrs_y, COL_SPACE);
}

/* One sensor blip per LIVE alien whose screen position lines up with the box
 * on exactly one axis (straight above/below -> top/bottom edge; straight
 * left/right -> side edge).  On-screen aliens (both axes in range) and purely
 * diagonal ones (neither in range) get nothing. */
void lrs_draw(int32_t sux, int32_t suy)
{
    const int lo_x = IN_LO + LRS_HALF + 1;    /* keep the triangle inside the box */
    const int hi_x = IN_HI_X - LRS_HALF - 1;
    const int lo_y = IN_LO + LRS_HALF + 1;
    const int hi_y = IN_HI_Y - LRS_HALF - 1;

    for (int i = 0; i < ALIEN_ONSCREEN; i++)
    {
        alien_t *a = &aliens[i];
        int dir = 0, ax = 0, ay = 0;

        if (a->state == 1)
        {
            int asx = CENTRE_X + (int)univ_delta(a->ux, sux);
            int asy = CENTRE_Y + (int)univ_delta(a->uy, suy);
            bool x_in = (asx >= IN_LO && asx < IN_HI_X);
            bool y_in = (asy >= IN_LO && asy < IN_HI_Y);

            if (x_in && !y_in)                       /* straight above / below */
            {
                ax = asx < lo_x ? lo_x : (asx > hi_x ? hi_x : asx);
                if (asy < IN_LO) { dir = 1; ay = IN_LO + 1; }
                else             { dir = 2; ay = IN_HI_Y - 2; }
            }
            else if (y_in && !x_in)                  /* straight left / right */
            {
                ay = asy < lo_y ? lo_y : (asy > hi_y ? hi_y : asy);
                if (asx < IN_LO) { dir = 3; ax = IN_LO + 1; }
                else             { dir = 4; ax = IN_HI_X - 2; }
            }
        }

        if (dir != 0)
        {
            lrs_tri(dir, ax, ay, COL_ALIEN);
            a->lrs_dir = dir; a->lrs_x = ax; a->lrs_y = ay; a->lrs_drawn = true;
        }
        else
            a->lrs_drawn = false;
    }
}

/* =========================================================================
 *  Audio
 * ===================================================================== */

/* Last pair handed to the driver, so a steady sound is set once and then left
 * alone (re-arming the PIO every frame would click). */
static uint16_t snd_l = 0, snd_r = 0;

static void sound_set(uint16_t l, uint16_t r)
{
    if (l == snd_l && r == snd_r)
        return;
    snd_l = l;
    snd_r = r;
    audio_play_sound(l, r);
}

/* Hand the voice back to silence (once), so a coasting ship is dead quiet. */
void sound_silence(void)
{
    if (snd_l == 0 && snd_r == 0)
        return;
    snd_l = snd_r = 0;
    audio_stop();
}

/* The player torpedo "pew": a fast pitch DROP from PEW_HI to PEW_LO over PEW_MS.
 * age_ms is 0..PEW_MS. */
void sound_pew(int age_ms)
{
    if (age_ms < 0)      age_ms = 0;
    if (age_ms > PEW_MS) age_ms = PEW_MS;
    int f = PEW_HI - (PEW_HI - PEW_LO) * age_ms / PEW_MS;
    sound_set((uint16_t)f, (uint16_t)(f - 15));   /* tiny L/R detune */
}

/* The alien torpedo "pew": the same chirp reversed - pitch RISES PEW_LO -> PEW_HI. */
void sound_apew(int age_ms)
{
    if (age_ms < 0)      age_ms = 0;
    if (age_ms > PEW_MS) age_ms = PEW_MS;
    int f = PEW_LO + (PEW_HI - PEW_LO) * age_ms / PEW_MS;
    sound_set((uint16_t)f, (uint16_t)(f - 15));
}

/* The "alien destroyed" chime: a rising 3-note major arpeggio (CHIME_N1 ->
 * N2 -> N3) split evenly across CHIME_MS.  age_ms is 0..CHIME_MS. */
void sound_chime(int age_ms)
{
    if (age_ms < 0) age_ms = 0;
    int step = age_ms * 3 / CHIME_MS;                 /* 0, 1, 2 (or 3 at the very end) */
    uint16_t f = (step <= 0) ? CHIME_N1 : (step == 1) ? CHIME_N2 : CHIME_N3;
    sound_set(f, (uint16_t)(f - 15));                 /* tiny L/R detune, like the pews */
}

/* The thruster "hiss": independent random L/R pitch high in the band every
 * frame - the closest a single square-wave voice gets to white noise.  Called
 * once per frame while the hiss window is open. */
void sound_hiss(void)
{
    uint16_t span = HISS_HI - HISS_LO + 1;
    uint16_t l = (uint16_t)(HISS_LO + rng_next() % span);
    uint16_t r = (uint16_t)(HISS_LO + rng_next() % span);
    sound_set(l, r);
}

/* A short (~WIN_SONG_MS) happy jingle - a rising major fanfare that resolves.
 * Played on the "YOU WON IN" screen, and once at program startup on the first
 * splash.  BLOCKS: sleep_ms() walks the note list, drops the voice at the end,
 * and resets the sound latch.  {Hz, ms}; Hz 0 == a rest. */
void play_win_song(void)
{
    static const uint16_t song[][2] = {
        { 784, 150}, { 784, 150}, { 784, 150}, {1047, 550}, {   0,  90},
        {1319, 150}, {1047, 150}, {1319, 150}, {1568, 650}, {   0,  90},
        { 784, 150}, { 880, 150}, { 988, 150}, {1047, 220}, {1175, 220}, {1319, 220},
        {1568, 300}, {1319, 300}, {1568, 990},
    };
    for (unsigned i = 0; i < sizeof song / sizeof song[0]; i++)
    {
        uint16_t f = song[i][0];
        if (f)
            audio_play_sound(f, (uint16_t)(f > 20 ? f - 12 : f));   /* tiny L/R detune */
        else
            audio_stop();
        sleep_ms(song[i][1]);
    }
    /* We drove the PIO with raw audio_play_sound(), so the latch is stale and
     * sound_silence() would no-op - kill the last note directly, then sync the
     * latch so the game's own sound code carries on from silence. */
    audio_stop();
    snd_l = snd_r = 0;
}

/* =========================================================================
 *  Setup
 * ===================================================================== */

void draw_border(void)
{
    lcd_solid_rectangle(COL_BORDER, 0,            0,            BOX_W,  BORDER); /* top    */
    lcd_solid_rectangle(COL_BORDER, 0,            BOX_H-BORDER, BOX_W,  BORDER); /* bottom */
    lcd_solid_rectangle(COL_BORDER, 0,            0,            BORDER, BOX_H);  /* left   */
    lcd_solid_rectangle(COL_BORDER, BOX_W-BORDER, 0,            BORDER, BOX_H);  /* right  */
}

/* The status gauges live in the black strip below the box, so they draw with
 * lcd_solid_rectangle() / lcd_putstr() directly (no px_rect clip).  The whole
 * block is compiled out when SHOW_STATUS == 0 (small-panel port). */
#if SHOW_STATUS

/* 1px outline - drawn once at startup. */
static void gauge_frame(int y)
{
    lcd_solid_rectangle(COL_GAUGE_FRAME, GAUGE_X,             y,            GAUGE_W, 1);
    lcd_solid_rectangle(COL_GAUGE_FRAME, GAUGE_X,             y + GAUGE_H-1, GAUGE_W, 1);
    lcd_solid_rectangle(COL_GAUGE_FRAME, GAUGE_X,             y,            1, GAUGE_H);
    lcd_solid_rectangle(COL_GAUGE_FRAME, GAUGE_X + GAUGE_W-1, y,            1, GAUGE_H);
}

/* Repaint one bar's interior (fill over drain) + its readout.  value is assumed
 * already clamped to 0..maxv.  as_pct: show a bare "nn%" (Fuel, Power) instead
 * of the "cur/max" count (Ammo). */
void gauge_draw(int y, int txt_row, uint16_t fill_col,
                       const char *label, int value, int maxv, bool as_pct)
{
    const int inner_w = GAUGE_W - 2;
    int fill_w = inner_w * value / maxv;
    if (fill_w > 0)
        lcd_solid_rectangle(fill_col,        GAUGE_X + 1,          y + 1,
                            fill_w, GAUGE_H - 2);
    if (fill_w < inner_w)
        lcd_solid_rectangle(COL_GAUGE_DRAIN, GAUGE_X + 1 + fill_w, y + 1,
                            inner_w - fill_w, GAUGE_H - 2);

    char buf[16];
    if (as_pct)
        snprintf(buf, sizeof buf, "%s %3d%%", label,
                 maxv ? value * 100 / maxv : 0);
    else
        snprintf(buf, sizeof buf, "%s %3d/%d", label, value, maxv);
    lcd_putstr(GAUGE_TXT_COL, txt_row, buf);
}

void draw_status_frames(void)
{
    gauge_frame(FUEL_GAUGE_Y);
    gauge_frame(PWR_GAUGE_Y);
    gauge_frame(AMMO_GAUGE_Y);
    gauge_frame(SHIELD_GAUGE_Y);
    gauge_frame(DMG_GAUGE_Y);
}

#endif /* SHOW_STATUS */

void go_bootsel(void)
{
    audio_stop();
    rom_reset_usb_boot(0, 0);
}

/* =========================================================================
 *  Start screen (title + difficulty) and controls screen
 * ===================================================================== */

/* There is no large font, so the title cards blow up the 8x10 one: every set
 * glyph pixel becomes a `scale` x `scale` block.  Top-left at (x, y), `col` on
 * the black screen already there.  ASCII only; spaces are skipped. */
void big_text(int x, int y, int scale, uint16_t col, const char *s)
{
    for (; *s; s++, x += 8 * scale)
    {
        if (*s == ' ')
            continue;
        const uint8_t *g = &font_8x10.glyphs[(uint8_t)*s * GLYPH_HEIGHT];
        for (int row = 0; row < GLYPH_HEIGHT; row++)
            for (int bit = 0; bit < 8; bit++)
                if (g[row] & (0x80 >> bit))
                    lcd_solid_rectangle(col,
                                        (uint16_t)(x + bit * scale),
                                        (uint16_t)(y + row * scale),
                                        (uint16_t)scale, (uint16_t)scale);
    }
}

/* One hint caption on the shared bottom row (forward-declared up by
 * run_game()).  col == COL_SPACE erases it. */
static void hint_caption(int which, uint16_t col)
{
    switch (which)
    {
        case 1: big_text(HINT_NA_TXT_X,   HINT_TXT_Y, HINT_TXT_SCALE, col, "Next Alien This Way"); break;
        case 2: big_text(HINT_S_TXT_X,    HINT_TXT_Y, HINT_TXT_SCALE, col, "Use S for Shields");   break;
        case 3: big_text(HINT_FIRE_TXT_X, HINT_TXT_Y, HINT_TXT_SCALE, col, "SPACE key to fire");   break;
        case 4: big_text(HINT_MOVE_TXT_X, HINT_TXT_Y, HINT_TXT_SCALE, col, "Arrow Keys to Move");  break;
        case 5: big_text(HINT_Q_TXT_X,    HINT_TXT_Y, HINT_TXT_SCALE, col, "Press Q to Quit");     break;
        default: break;
    }
}

/* Block until a key is pressed; return its ASCII code (0 for arrows / specials
 * that carry none).  Only ~ drops straight to BOOTSEL. */
uint8_t wait_key(void)
{
    for (;;)
    {
        uint16_t e = sb_read_keyboard();
        if (e)
        {
            uint8_t st   = (e >> 8) & 0xFF;
            uint8_t code = e & 0xFF;
            if (st == KEY_STATE_PRESSED)
            {
                if (code == '~')
                    go_bootsel();               /* does not return */
                return code;
            }
        }
        sleep_ms(FRAME_MS);
    }
}

/* The start screen: big wrapped title + author, then the four difficulty
 * choices (one line each) right below.  Waits for 0..3 and returns the level.
 * On the very first showing (program startup) it plays the same happy jingle
 * as the win screen.  ~ reboots to BOOTSEL (via wait_key); ESC does nothing here. */
int splash_screen(void)
{
    lcd_clear_screen();
    big_text(28,  40, 3, COL_SHIP,   "YET ANOTHER");
    big_text(28,  80, 3, COL_SHIP,   "SPACE ALIEN");
    big_text(16, 120, 3, COL_TORPEDO, "GAME (YASAG)");
    lcd_putstr(12, 16, VERSION_STR "  by Thomas Dzubin");

    lcd_putstr(0, 18, "Choose Difficulty");
    lcd_putstr(4, 19, "0   No aliens");
    lcd_putstr(4, 20, "1   Easy Aliens (with hints)");
    lcd_putstr(4, 21, "2   Regular Aliens");
    lcd_putstr(4, 22, "3   Hard Aliens");
    lcd_putstr(4, 24, "Press 0-3");

    static bool startup_song_played = false;
    if (!startup_song_played)          /* the win jingle, once, at program startup */
    {
        play_win_song();               /* blocks ~WIN_SONG_MS, splash stays up */
        startup_song_played = true;
    }

    for (;;)
    {
        uint8_t code = wait_key();
        if (code >= '0' && code <= '3')
            return code - '0';
    }
}

/* The key legend itself (title + rows).  Assumes the screen is already cleared.
 * Shared by the CONTROLS screen and the in-game '?' pause. */
void draw_key_legend(void)
{
    big_text(96, 6, 2, COL_SHIP, "CONTROLS");

    int r = 4;
    lcd_putstr(2, r++, "Left/Right    turn the ship");
    lcd_putstr(2, r++, "Up            fire main engine (tap)");
    lcd_putstr(2, r++, "Down          reverse engine (tap)");
    lcd_putstr(2, r++, "Space         shoot");
    lcd_putstr(2, r++, "S             shields on / off");
    lcd_putstr(2, r++, "C             cloak on / off");
    lcd_putstr(2, r++, "Enter         flip + burn (full stop)");
    lcd_putstr(2, r++, "A             autopilot on / off");
    lcd_putstr(2, r++, "M             back to manual flying");
    lcd_putstr(2, r++, "?             pause + show this page");
    r++;
    /* NB: F5 (screenshot to SD, PICOCALC only) is deliberately NOT listed here -
     * it's an undocumented function, discoverable only by reading the code. */
    lcd_putstr(2, r++, "While autopilot is on:");
    lcd_putstr(2, r++, "  Arrows      hop one screen that way");
    lcd_putstr(2, r++, "  J / K       turn the ship");
    r++;
    lcd_putstr(2, r++, "*** Enter (flip + burn) ALWAYS brings");
    lcd_putstr(2, r++, "    the ship to a DEAD STOP, whatever");
    lcd_putstr(2, r++, "    your speed or heading. ***");
    r++;
    lcd_putstr(2, r++, "Q             quit to start screen");
    /* Esc also returns to the start screen, but it is deliberately NOT listed
     * here - like ~, it is an undocumented shortcut. */
}

/* The key legend, shown between the start screen and the game.  A 10 -> 0
 * countdown along the bottom starts the game when it hits 0; any key press
 * starts it immediately.  Returns true to play, false if ESC was pressed
 * (caller loops back to the start screen).  ~ still reboots to BOOTSEL. */
bool controls_screen(void)
{
    lcd_clear_screen();
    draw_key_legend();

    while (sb_read_keyboard())        /* drop anything left over from the start screen */
        ;

    for (int secs = 10; secs >= 0; secs--)
    {
        char buf[40];
        snprintf(buf, sizeof buf, "Game starts in %2d - any key skips", secs);
        lcd_putstr(4, 30, buf);        /* bottom row of the screen */
        if (secs == 0)
            return true;              /* "0" shown; straight into the game */

        uint32_t until = to_ms_since_boot(get_absolute_time()) + 1000u;
        while (to_ms_since_boot(get_absolute_time()) < until)
        {
            uint16_t e = sb_read_keyboard();
            if (e)
            {
                uint8_t st   = (e >> 8) & 0xFF;
                uint8_t code = e & 0xFF;
                if (st == KEY_STATE_PRESSED)
                {
                    if (code == '~')
                        go_bootsel();          /* does not return */
                    if (code == KEY_ESC)
                        return false;          /* back to the start screen */
                    return true;               /* any other key -> start now */
                }
            }
            sleep_ms(FRAME_MS);
        }
    }
    return true;
}

