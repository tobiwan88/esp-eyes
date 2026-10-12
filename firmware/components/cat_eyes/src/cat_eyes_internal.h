#pragma once
/* Internal shared state of the cat_eyes component. Not part of the API. */
#include "cat_eyes.h"
#include <stdbool.h>
#include <stdint.h>

#define CE_CX 120       /* eye centre on the 240x240 display */
#define CE_CY 128
#define CE_EYE_HALF_W 104
#define CE_MAX_KEYS 14
#define CE_MAX_TRACKS 8
#define CE_MAX_SHAPES 48
#define CE_MAX_FX 40       /* shapes in the effects layer (drawn last, above everything) */
#define CE_MAX_HL 5
#define CE_SNOW 24
#define CE_BATS 3

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* raw 5/6/5-bit channel values; used for the per-eye tint (see ce_frame_t.tint_*) */
#define T565(r5, g6, b5) ((uint16_t)(((r5) << 11) | ((g6) << 5) | (b5)))

enum { P_GX, P_GY, P_UPPER, P_LOWER, P_PUPIL, P_TILT, P_NICT, P_FX, P_SMILE, P_COUNT };

typedef enum { IRIS_GRADIENT, IRIS_FLAT, IRIS_RING, IRIS_ANIME, IRIS_CUTE, IRIS_FIRE } ce_iris_kind_t;
typedef enum { PUPIL_SLIT, PUPIL_OVAL, PUPIL_ROUND } ce_pupil_kind_t;
typedef enum { HL_SOFT, HL_ANIME, HL_CUTE, HL_STAR, HL_DIM } ce_hl_kind_t;

typedef struct {
    const char *name;
    const char *special_name;      /* NULL = no special animation */
    int8_t yo, yi;                 /* outer / inner corner height (px, +down) */
    uint8_t ht, hb;                /* upper / lower lid curve height */
    float shape_exp;               /* almond exponent: lower = rounder */
    uint8_t iris_r;
    uint32_t bg, ink, opening;     /* RGB888 */
    ce_iris_kind_t iris;
    ce_pupil_kind_t pupil;
    ce_hl_kind_t hl;
    float pupil_min;               /* pupil = min + (1-min)*pupil */
    float pupil_cap;               /* <1: clamp then scale (halloween slit) */
    float tilt_bias, upper_bias;
    uint8_t top_ink, bot_ink, closed_ink;
    bool fibres, lid_shadow, corner_flick, lashes, demon_brow, santa_trim;
    bool blush, smile_arcs, moon_bats, snow, fur;
    int8_t fixed_palette;          /* -1 = follow iris setting */
    uint32_t nict_rgb; uint8_t nict_alpha;
} ce_style_t;

typedef struct { uint32_t light, mid, dark; } ce_palette_t;

extern const ce_style_t g_ce_styles[CAT_STYLE_COUNT];
extern const ce_palette_t g_ce_palettes[]; /* index: cat_iris_t (0..4), 5 fire, 6 xmas */
#define CE_PAL_FIRE 5
#define CE_PAL_XMAS 6

/* ---- animation ---- */
typedef struct { uint16_t t; float v; } ce_key_t;
typedef struct { uint8_t param, n, linear; ce_key_t k[CE_MAX_KEYS]; } ce_track_t;
typedef struct { uint8_t n; ce_track_t tr[CE_MAX_TRACKS]; } ce_tracks_t;
typedef struct { cat_anim_t id; uint16_t dur; bool jitter; ce_tracks_t eye[2]; } ce_anim_t;

/* ---- per-eye frame description built by tick, consumed by render ---- */
/* SH_RECT: x0,y0..x1,y1 inclusive.  SH_RING: centre x0,y0, outer r, inner r2.
 * SH_GLYPH: top-left x0,y0, pixel scale r, glyph index r2 (5x7 font).
 * clip != 0: rows outside [x2, y2] are not drawn (all kinds except SH_TRI).  */
typedef enum { SH_CIRCLE, SH_ELLIPSE, SH_CAPSULE, SH_TRI, SH_RECT, SH_RING, SH_GLYPH } ce_shape_kind_t;
typedef struct {
    uint8_t kind, clip; uint16_t color;
    int16_t x0, y0, x1, y1, x2, y2, r, r2;
    int16_t bx0, by0, bx1, by1;
} ce_shape_t;
enum { CE_L_UNDER = 0, CE_L_OVER = 1, CE_L_FX = 2 };

typedef struct {
    uint8_t kind;         /* 0 ellipse/circle, 1 star mask */
    int16_t x, y, rx, ry; /* screen */
    uint16_t color; uint16_t alpha; /* 0..256 */
} ce_hl_t;

typedef struct {
    float p[P_COUNT];                 /* final params used for this frame */
    const ce_style_t *st;
    int side;                         /* +1 eye0, -1 eye1 (mirrored) */
    uint16_t bg, ink, opening;
    /* lid column tables (screen y), valid where col_ok */
    int16_t top[CAT_EYES_W], bot[CAT_EYES_W], cl[CAT_EYES_W];
    int16_t top_lo[CAT_EYES_W], top_hi[CAT_EYES_W], bot_lo[CAT_EYES_W], bot_hi[CAT_EYES_W];
    int16_t smile[CAT_EYES_W];
    uint8_t col_ok[CAT_EYES_W];
    int16_t x_min, x_max;
    bool open, smile_mode;
    /* iris */
    int16_t icx, icy, ir;
    uint32_t ir2, inv_ir2;            /* (64<<16)/r^2 */
    uint16_t lut[64];
    uint16_t c_light, c_mid, c_dark;
    /* pupil: half width per |dy|; -1 outside */
    int16_t pup_a[96], pup_b[96];     /* outer / inner (anime) */
    int16_t pup_hh;
    bool heart; int16_t heart_half;  uint16_t heart_color;
    uint16_t pup_fill, pup_inner, pup_outline;
    /* anime / cute two-tone ellipses (relative to iris centre) */
    int16_t e1y, e1rx, e1ry, e2y, e2rx, e2ry;
    /* highlights */
    uint8_t n_hl; ce_hl_t hl[CE_MAX_HL];
    /* third eyelid */
    bool nict_on; int16_t nict_xe; uint16_t nict_color, nict_edge; uint16_t nict_alpha;
    /* shapes drawn behind / above the eye */
    uint8_t n_under, n_over;
    ce_shape_t under[CE_MAX_SHAPES], over[CE_MAX_SHAPES];
    /* effects layer (cat_eyes_fx.c): shapes above everything, plus an optional
     * whole-eye tint  out = ((c >> shift) & mask) + add  (shift 1 or 2, 0 = off).
     * add must not overflow: per channel <= max - (max >> shift).             */
    uint8_t n_fx; ce_shape_t fx[CE_MAX_FX];
    uint8_t tint_shift; uint16_t tint_add;
    int16_t fx_lo, fx_hi;
    /* dirty rows */
    int16_t y_lo, y_hi;
} ce_frame_t;

/* per-run randomisation of the effect animations */
typedef struct { int dir; int var; uint32_t seed; } ce_fxs_t;

typedef struct {
    bool inited, auto_idle, swap;
    cat_style_t style; cat_iris_t iris;
    uint32_t rng;
    uint32_t now, last_now, frame_no;
    /* animation */
    bool anim_on; ce_anim_t anim; uint32_t anim_start; cat_anim_t last_anim;
    uint32_t next_anim, next_blink, blink_start, next_sacc;
    float gx, gy, tgx, tgy; uint32_t look_until;
    float pupil_s[2];
    /* background particles */
    struct { float x, y, v, r; } snow[2][CE_SNOW];
    struct { float a, r, v; } bats[CE_BATS];
    ce_frame_t f[2];
    int16_t prev_lo[2], prev_hi[2];
    uint32_t prev_hash[2];
    bool force_full[2];
    bool prev_fx[2], prev_tint[2];
    int16_t prev_fx_lo[2], prev_fx_hi[2];
    ce_fxs_t fxs;
} ce_state_t;

extern ce_state_t g_ce;

/* helpers */
uint32_t ce_rand(void);
float ce_randf(float a, float b);
uint16_t ce_rgb(uint32_t rgb888);
uint16_t ce_blend(uint16_t a, uint16_t b, uint16_t alpha256); /* a->b */
uint32_t ce_isqrt(uint32_t v);

/* anim */
void ce_anim_build(cat_anim_t id, ce_anim_t *out);
float ce_track_eval(const ce_track_t *t, uint32_t ms);

/* render */
void ce_render_init(void);
void ce_build_frame(int eye, const float *params, uint32_t now);

/* shape primitives (layer = CE_L_UNDER / CE_L_OVER / CE_L_FX); degenerate sizes are ignored */
void ce_sh_circle(ce_frame_t *f, int layer, int x, int y, int r, uint16_t c);
void ce_sh_ellipse(ce_frame_t *f, int layer, int x, int y, int rx, int ry, uint16_t c);
void ce_sh_capsule(ce_frame_t *f, int layer, int x0, int y0, int x1, int y1, int r, uint16_t c);
void ce_sh_tri(ce_frame_t *f, int layer, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);
void ce_sh_rect(ce_frame_t *f, int layer, int x0, int y0, int x1, int y1, uint16_t c);
void ce_sh_ring(ce_frame_t *f, int layer, int x, int y, int r_out, int r_in, uint16_t c);
void ce_sh_glyph(ce_frame_t *f, int layer, int x, int y, int scale, int glyph, uint16_t c);
/* clip the shapes [first, n) of a layer to rows y0..y1 (not for triangles) */
void ce_sh_clip(ce_frame_t *f, int layer, int first, int y0, int y1);

/* 5x7 font: digits 0-9, '%', euro sign, 8 pseudo-katakana (matrix rain) */
#define CE_GLYPH_PCT 10
#define CE_GLYPH_EURO 11
#define CE_GLYPH_KANA0 12
#define CE_GLYPH_COUNT 20
extern const uint8_t g_ce_font[CE_GLYPH_COUNT][7];

/* custom effects (cat_eyes_efx.c): played as a pseudo animation id one past the enum */
#define CE_ANIM_EFX ((cat_anim_t)CAT_ANIM_COUNT)
#define CE_IS_FX(id) (((id) >= CAT_ANIM_FX_FIRST && (id) <= CAT_ANIM_FX_LAST) || (id) == CE_ANIM_EFX)
uint16_t ce_efx_duration(void);
void ce_efx_params(float t_ms, float *p);
void ce_efx_draw(int eye, float t_ms, ce_frame_t *f);
const char *ce_efx_name(void);
void ce_start_anim(cat_anim_t id);   /* cat_eyes.c: no range check */

/* effects (cat_eyes_fx.c) */
uint16_t ce_fx_duration(cat_anim_t id);
void ce_fx_begin(cat_anim_t id);                                   /* randomise a new run   */
void ce_fx_params(cat_anim_t id, int eye, float t_ms, float *p);   /* gaze / lids / pupil   */
void ce_fx_draw(cat_anim_t id, int eye, float t_ms, ce_frame_t *f);/* shapes + tint         */
