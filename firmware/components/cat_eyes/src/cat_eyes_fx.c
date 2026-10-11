/*
 * Effects layer: sprites and small scenes drawn above the eye (CE_L_FX shapes
 * plus an optional whole-eye tint).  Each effect is a normal cat_anim_t, so the
 * random idle, the JSON API and the web page treat it like wink or roll.
 *
 * Every effect is ONE function  fx_xxx(eye, t, p, f)  that is called twice per
 * eye and frame:
 *   - p != NULL, f == NULL : adjust the eye parameters (gaze, lids, pupil)
 *   - p == NULL, f != NULL : add shapes / tint to the frame that was just built
 * so the positions are computed in one place and used for both.
 *
 * World coordinates: both displays share one x axis, so an object can fly from
 * the left eye into the right one.
 *     eye 0 = x   0 .. 240   |  gap 56 (invisible)  |  eye 1 = x 296 .. 536
 * Effects that travel use a "dir" (+1 left to right, -1 right to left) chosen
 * at random for every run.
 *
 * Cost: shapes are flat-colour primitives (circle, ellipse, capsule, triangle,
 * rect, ring, 5x7 glyph), at most CE_MAX_FX per eye; all floating point is per
 * frame, never per pixel.  Effects with a tint or a full-screen scene redraw
 * the whole eye every frame (like the snow of the christmas style).
 */
#include "cat_eyes_internal.h"
#include <math.h>
#include <stddef.h>

#define PI_F 3.14159265f
#define TAU_F 6.2831853f
#define FX_GAP 56
#define FX_OFF (CAT_EYES_W + FX_GAP)       /* world x of the right eye's left edge */
#define FX_WORLD (2 * CAT_EYES_W + FX_GAP) /* 536                                  */
#define CXF 120                            /* centre of the eye opening            */
#define CYF 118
#define FXL CE_L_FX

/* ---------------- helpers ---------------- */
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float lerpf(float a, float b, float u) { return a + (b - a) * u; }
static float sstep(float a, float b, float t)
{
    float u = clampf((t - a) / (b - a), 0.f, 1.f);
    return u * u * (3.f - 2.f * u);
}
static float backo(float u) /* ease-out with overshoot, 0..1 */
{
    u = clampf(u, 0.f, 1.f);
    const float c1 = 1.70158f, c3 = c1 + 1.f, v = u - 1.f;
    return 1.f + c3 * v * v * v + c1 * v * v;
}
static int R(float v) { return (int)lrintf(v); }
static int rr(float v) { int r = R(v); return r < 1 ? 1 : r; } /* radius, at least 1 */
static uint32_t hsh(uint32_t a, uint32_t b)
{
    uint32_t x = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;
    x ^= x >> 15; x *= 0xC2B2AE35u; x ^= x >> 13;
    return x;
}
static float hf(uint32_t a, uint32_t b) { return (float)(hsh(a, b) & 0xffffu) / 65535.f; }
static uint16_t mix565(int r0, int g0, int b0, int r1, int g1, int b1, float u)
{
    u = clampf(u, 0.f, 1.f);
    return RGB565(R(r0 + (r1 - r0) * u), R(g0 + (g1 - g0) * u), R(b0 + (b1 - b0) * u));
}
static float scr(int eye, float wx) { return eye ? wx - FX_OFF : wx; }         /* world -> eye x */
static float wmap(float u, int dir) { return dir > 0 ? u : FX_WORLD - u; }      /* travel coordinate -> world x */
static float follow(float wx) { return clampf((wx - FX_WORLD * .5f) / 190.f, -1.f, 1.f) * .85f; }

typedef struct { int x, y; } pt_t;
static pt_t xf(float cx, float cy, float a, float k, float lx, float ly)
{
    const float c = cosf(a), s = sinf(a);
    pt_t o = { R(cx + (lx * c - ly * s) * k), R(cy + (lx * s + ly * c) * k) };
    return o;
}

#define CIRC(x, y, r, c) ce_sh_circle(f, FXL, R(x), R(y), R(r), (c))
#define ELL(x, y, rx, ry, c) ce_sh_ellipse(f, FXL, R(x), R(y), R(rx), R(ry), (c))
#define CAP(x0, y0, x1, y1, r, c) ce_sh_capsule(f, FXL, R(x0), R(y0), R(x1), R(y1), (r), (c))
#define TRI(x0, y0, x1, y1, x2, y2, c) ce_sh_tri(f, FXL, R(x0), R(y0), R(x1), R(y1), R(x2), R(y2), (c))
#define RECT(x0, y0, x1, y1, c) ce_sh_rect(f, FXL, R(x0), R(y0), R(x1), R(y1), (c))
#define RING(x, y, ro, ri, c) ce_sh_ring(f, FXL, R(x), R(y), R(ro), R(ri), (c))
#define GLYPH(x, y, sc, g, c) ce_sh_glyph(f, FXL, R(x), R(y), (sc), (g), (c))

static void sparkle(ce_frame_t *f, float x, float y, float s, uint16_t c)
{
    if (s < 2.f) return;
    CAP(x, y - s, x, y + s, 1, c);
    CAP(x - s, y, x + s, y, 1, c);
    if (s > 7.f) CIRC(x, y, s * .28f, c);
}
static void tint(ce_frame_t *f, int shift, uint16_t add) { f->tint_shift = (uint8_t)shift; f->tint_add = add; }

/* ======================================================================== */
/* FOOD: pizza / donut / hot dog / burger flies from one eye to the other    */
/* ======================================================================== */
static void food_item(ce_frame_t *f, int kind, float x, float y, float a, float k)
{
#define XF(lx, ly) xf(x, y, a, k, (lx), (ly))
    switch (kind) {
    case 0: { /* pizza slice, crust on the +x side */
        pt_t o0 = XF(-37, 0), o1 = XF(31, -30), o2 = XF(31, 30);
        ce_sh_tri(f, FXL, o0.x, o0.y, o1.x, o1.y, o2.x, o2.y, RGB565(90, 52, 20));
        pt_t a0 = XF(-30, 0), a1 = XF(27, -24), a2 = XF(27, 24);
        ce_sh_tri(f, FXL, a0.x, a0.y, a1.x, a1.y, a2.x, a2.y, RGB565(248, 196, 64));
        pt_t c0 = XF(28, -25), c1 = XF(28, 25);
        ce_sh_capsule(f, FXL, c0.x, c0.y, c1.x, c1.y, rr(6 * k), RGB565(205, 140, 60));
        static const float pp[3][2] = {{2, -9}, {13, 9}, {-12, 3}};
        for (int i = 0; i < 3; i++) {
            pt_t q = XF(pp[i][0], pp[i][1]);
            ce_sh_circle(f, FXL, q.x, q.y, rr(5.5f * k), RGB565(200, 50, 43));
        }
        break;
    }
    case 1: { /* donut with sprinkles */
        ce_sh_ring(f, FXL, R(x), R(y), rr(27 * k), rr(9 * k), RGB565(217, 160, 102));
        ce_sh_ring(f, FXL, R(x), R(y), rr(23 * k), rr(9 * k), RGB565(240, 111, 160));
        static const uint16_t sp[3] = {RGB565(255, 255, 255), RGB565(80, 200, 255), RGB565(255, 230, 60)};
        for (int i = 0; i < 6; i++) {
            const float ph = i * 1.047f, cx = 15.f * cosf(ph), cy = 15.f * sinf(ph);
            const float tx = -sinf(ph + i * .9f) * 3.f, ty = cosf(ph + i * .9f) * 3.f;
            pt_t p0 = XF(cx - tx, cy - ty), p1 = XF(cx + tx, cy + ty);
            ce_sh_capsule(f, FXL, p0.x, p0.y, p1.x, p1.y, 1, sp[i % 3]);
        }
        break;
    }
    case 2: { /* hot dog */
        pt_t b0 = XF(-22, 6), b1 = XF(22, 6), s0 = XF(-31, -3), s1 = XF(31, -3);
        ce_sh_capsule(f, FXL, b0.x, b0.y, b1.x, b1.y, rr(14 * k), RGB565(227, 184, 115));
        ce_sh_capsule(f, FXL, s0.x, s0.y, s1.x, s1.y, rr(10 * k), RGB565(190, 80, 40));
        static const float zz[5][2] = {{-22, -6}, {-12, -12}, {-2, -6}, {8, -12}, {18, -6}};
        for (int i = 0; i < 4; i++) {
            pt_t p0 = XF(zz[i][0], zz[i][1]), p1 = XF(zz[i + 1][0], zz[i + 1][1]);
            ce_sh_capsule(f, FXL, p0.x, p0.y, p1.x, p1.y, 1, RGB565(250, 210, 40));
        }
        break;
    }
    default: { /* burger (does not rotate) */
        ce_sh_capsule(f, FXL, R(x - 22 * k), R(y + 15 * k), R(x + 22 * k), R(y + 15 * k), rr(7 * k), RGB565(227, 176, 100));
        ce_sh_capsule(f, FXL, R(x - 26 * k), R(y + 5 * k), R(x + 26 * k), R(y + 5 * k), rr(6 * k), RGB565(92, 51, 23));
        ce_sh_rect(f, FXL, R(x - 25 * k), R(y - 2 * k), R(x + 25 * k), R(y + 1 * k), RGB565(247, 208, 56));
        ce_sh_capsule(f, FXL, R(x - 24 * k), R(y - 5 * k), R(x + 24 * k), R(y - 5 * k), rr(3 * k), RGB565(109, 190, 69));
        ce_sh_ellipse(f, FXL, R(x), R(y - 16 * k), rr(28 * k), rr(13 * k), RGB565(224, 160, 80));
        static const float ss[4][2] = {{-10, -20}, {0, -24}, {10, -20}, {-3, -16}};
        for (int i = 0; i < 4; i++) ce_sh_circle(f, FXL, R(x + ss[i][0] * k), R(y + ss[i][1] * k), rr(2 * k), RGB565(255, 240, 200));
        break;
    }
    }
#undef XF
}

static void fx_food(int eye, float t, float *p, ce_frame_t *f)
{
    const int dir = g_ce.fxs.dir, kind = g_ce.fxs.var & 3;
    const float fl = clampf((t - 250.f) / 1700.f, 0.f, 1.f);
    const float u = lerpf(-70.f, 416.f, sstep(250.f, 1950.f, t));
    const float wx = wmap(u, dir);
    const float y = 112.f - 20.f * sinf(PI_F * fl) + 7.f * sinf(t / 120.f) * (1.f - sstep(1700.f, 1950.f, t));
    if (p) {
        p[P_GX] = t < 250.f ? -dir * .7f : follow(wx);
        p[P_GY] = clampf((y - 118.f) / 90.f, -.4f, .4f);
        if (t >= 1950.f && t < 2950.f) { /* chewing */
            const float c = fmaxf(0.f, sinf((t - 1950.f) / 300.f * TAU_F));
            p[P_UPPER] = .12f + .2f * c; p[P_LOWER] = .06f + .14f * c;
        }
        const float s = sstep(2950.f, 3150.f, t) * (1.f - sstep(3250.f, 3450.f, t)); /* happy squint */
        if (s > 0.f) { p[P_UPPER] = lerpf(.12f, .4f, s); p[P_LOWER] = lerpf(.06f, .5f, s); p[P_SMILE] = s > .5f ? 1.f : 0.f; }
        return;
    }
    const float kc = t < 2250.f ? 1.f : t < 2550.f ? .72f : t < 2850.f ? .44f : t < 2950.f ? .2f : 0.f;
    if (kc <= 0.f || t < 200.f) return;
    const float sx = scr(eye, wx);
    if (sx < -70.f || sx > 310.f) return;
    float a = 0.f;
    if (kind == 0) a = 4.f * PI_F * fl; else if (kind == 1) a = 8.8f * fl; else if (kind == 2) a = TAU_F * fl;
    food_item(f, kind, sx, y, a, 1.3f * kc);
}

/* ======================================================================== */
/* FLY: zigzag over both eyes, the pupils chase it, SNAP                     */
/* ======================================================================== */
static void fly_pos(float t, int dir, int var, float *ox, float *oy)
{
    const float tx = (var & 1) ? 120.f : 416.f; /* where it finally sits (travel frame) */
    const float k = sstep(1700.f, 2350.f, t);
    const float A = 8.f + 244.f * (1.f - k);
    const float x = lerpf(FX_WORLD * .5f, tx, k) + A * sinf(t / 800.f - 1.4f);
    const float ya = 4.f + 34.f * (1.f - k);
    *ox = wmap(x, dir);
    *oy = 112.f + ya * sinf(t / 190.f + 1.f) + .4f * ya * sinf(t / 83.f);
}

static void fx_fly(int eye, float t, float *p, ce_frame_t *f)
{
    const int dir = g_ce.fxs.dir, var = g_ce.fxs.var;
    float wx, y;
    fly_pos(t, dir, var, &wx, &y);
    if (p) {
        p[P_GX] = follow(wx); p[P_GY] = clampf((y - 118.f) / 80.f, -.5f, .5f);
        p[P_PUPIL] = .7f; p[P_UPPER] = .06f;
        const float s = sstep(2700.f, 2780.f, t) * (1.f - sstep(2900.f, 3050.f, t)); /* the snap */
        if (s > 0.f) { p[P_UPPER] = lerpf(.06f, 1.f, s); p[P_LOWER] = .3f * s; }
        if (t >= 2950.f) p[P_PUPIL] = lerpf(.95f, .22f, sstep(3000.f, 3500.f, t));
        return;
    }
    if (t >= 2790.f) return;
    for (int i = 3; i >= 1; i--) { /* ghost trail */
        float tx, ty;
        if (t - 60.f * i < 0.f) continue;
        fly_pos(t - 60.f * i, dir, var, &tx, &ty);
        tx = scr(eye, tx);
        if (tx > -20.f && tx < 260.f) CIRC(tx, ty, 3, RGB565(120, 120, 120));
    }
    const float sx = scr(eye, wx);
    if (sx < -30.f || sx > 270.f) return;
    const int fl = ((int)(t / 45.f)) & 1;
    float wx2, y2;
    fly_pos(t + 40.f, dir, var, &wx2, &y2);
    const int face = wx2 >= wx ? 1 : -1;
    const float F = 1.7f; /* the fly is small on a 1.28" panel: draw it big */
    ELL(sx - 5 * F, y - (8 + 4 * fl) * F, 11 * F, (3 + fl) * F, RGB565(215, 230, 245));
    ELL(sx + 5 * F, y - (8 + 4 * fl) * F, 11 * F, (3 + fl) * F, RGB565(215, 230, 245));
    ELL(sx, y, 10 * F, 7 * F, RGB565(30, 24, 20));
    CIRC(sx + face * 10 * F, y + F, 5 * F, RGB565(30, 24, 20));
    CIRC(sx + face * 12 * F, y - F, 1.8f * F, RGB565(220, 40, 40));
}

/* ======================================================================== */
/* PAPER PLANE: glides over, loops in the second eye, flies off              */
/* ======================================================================== */
static void plane_pos(float t, float *u, float *y)
{
    if (t < 1600.f) { *u = lerpf(-70.f, 330.f, t / 1600.f); *y = 100.f + 20.f * sinf(t / 380.f + .5f); return; }
    const float y0 = 100.f + 20.f * sinf(1600.f / 380.f + .5f);
    if (t < 2500.f) {
        const float k = (t - 1600.f) / 900.f, ph = TAU_F * (k * k * (3.f - 2.f * k));
        *u = 330.f + 35.f * k + 34.f * sinf(ph); *y = y0 - 34.f * (1.f - cosf(ph));
        return;
    }
    *u = 365.f + (t - 2500.f) * .35f; *y = y0 - (t - 2500.f) * .03f;
}

static void fx_plane(int eye, float t, float *p, ce_frame_t *f)
{
    const int dir = g_ce.fxs.dir;
    float u, y, u2, y2;
    plane_pos(t, &u, &y);
    if (p) {
        p[P_GX] = follow(wmap(u, dir)); p[P_GY] = clampf((y - 118.f) / 90.f, -.6f, .6f);
        return;
    }
    if (u > FX_WORLD + 50.f) return;
    plane_pos(t + 20.f, &u2, &y2);
    float uu, yy; plane_pos(t - 20.f, &uu, &yy);
    const float th = atan2f(y2 - yy, u2 - uu), k = 1.9f;
    const float c = cosf(th), s = sinf(th);
#define PP(lx, ly, X, Y) do { float ux = u + ((lx) * c - (ly) * s) * k, uy = y + ((lx) * s + (ly) * c) * k; \
    X = R(scr(eye, wmap(ux, dir))); Y = R(uy); } while (0)
    for (int i = 4; i >= 1; i--) { /* trail */
        float tu, ty;
        if (t - 70.f * i < 0.f) continue;
        plane_pos(t - 70.f * i, &tu, &ty);
        const float tx = scr(eye, wmap(tu, dir));
        if (tx > -10.f && tx < 250.f) CIRC(tx, ty, 2, RGB565(190, 205, 230));
    }
    int nx, ny, lx_, ly_, rx_, ry_, mx, my;
    PP(26, 0, nx, ny); PP(-22, -14, lx_, ly_); PP(-22, 14, rx_, ry_); PP(-12, 0, mx, my);
    ce_sh_tri(f, FXL, nx, ny, lx_, ly_, mx, my, RGB565(250, 250, 255));
    ce_sh_tri(f, FXL, nx, ny, rx_, ry_, mx, my, RGB565(190, 200, 222));
    ce_sh_capsule(f, FXL, nx, ny, mx, my, 1, RGB565(120, 130, 155));
#undef PP
}

/* ======================================================================== */
/* BUBBLE: drifts over, the eyes cross to watch it, it pops                  */
/* ======================================================================== */
static void fx_bubble(int eye, float t, float *p, ce_frame_t *f)
{
    const int dir = g_ce.fxs.dir;
    const float prog = sstep(100.f, 2900.f, t);
    const float u = lerpf(-24.f, 416.f, prog), wx = wmap(u, dir);
    const float y = lerpf(150.f, 92.f, prog) + 10.f * sinf(t / 330.f);
    const float rad = lerpf(20.f, 34.f, prog) + 1.5f * sinf(t / 110.f);
    if (p) {
        const float cross = sstep(2200.f, 2700.f, t) * (1.f - sstep(2900.f, 3050.f, t));
        p[P_GX] = lerpf(follow(wx) * .9f, eye ? -.6f : .6f, cross);
        p[P_GY] = clampf((y - 118.f) / 100.f, -.4f, .3f);
        p[P_PUPIL] = lerpf(.22f, .1f, cross);
        if (t >= 2900.f) {
            const float s = 1.f - sstep(3000.f, 3500.f, t);
            p[P_PUPIL] = lerpf(.22f, .95f, s); p[P_UPPER] = lerpf(.12f, 0.f, s); p[P_GX] = 0.f;
        }
        return;
    }
    const float sx = scr(eye, wx);
    if (t < 2900.f) {
        if (sx < -50.f || sx > 290.f) return;
        RING(sx, y, rad, rad - 4, RGB565(170, 225, 255));
        RING(sx, y, rad - 4, rad - 7, RGB565(255, 200, 240));
        ELL(sx - rad * .42f, y - rad * .42f, rad * .2f, rad * .1f + 1, 0xFFFF);
        CIRC(sx + rad * .4f, y + rad * .4f, 3, 0xFFFF);
        return;
    }
    const float tp = (t - 2900.f) / 280.f;
    if (tp >= 1.f) return;
    const float sx2 = scr(eye, wmap(416.f, dir));
    if (tp < .6f) RING(sx2, 92, 34 * (1 + .6f * tp), 34 * (1 + .6f * tp) - 4 * (1 - tp), RGB565(200, 235, 255));
    for (int i = 0; i < 8; i++) {
        const float a = i * PI_F / 4.f + .39f, rin = 34.f * (.9f + tp), rout = rin + 12.f * (1.f - tp);
        CAP(sx2 + cosf(a) * rin, 92 + sinf(a) * rin, sx2 + cosf(a) * rout, 92 + sinf(a) * rout, 1, RGB565(190, 235, 255));
    }
}

/* ======================================================================== */
/* SPIRAL: hypnosis pupils, one eye reverses                                 */
/* ======================================================================== */
static void fx_spiral(int eye, float t, float *p, ce_frame_t *f)
{
    if (p) { p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .02f; p[P_LOWER] = .02f; return; }
    float s = t < 350.f ? backo(t / 350.f) : 1.f;
    s *= 1.f - sstep(2750.f, 3100.f, t);
    if (s <= .05f) return;
    const float base = 5.65f * t / 1000.f; /* 0.9 turns per second */
    const float rot = (eye == 1 && t > 1500.f) ? 2.f * 5.65f * 1.5f - base : base;
    const uint16_t ink = RGB565(30, 14, 70);
    CIRC(CXF, CYF, 63 * s, ink);
    CIRC(CXF, CYF, 59 * s, RGB565(250, 248, 255));
    const int N = 27;
    const float rmax = 53.f * s;
    float px = CXF, py = CYF;
    for (int i = 1; i <= N; i++) {
        const float th = (float)i / N * 4.5f * PI_F, r = rmax * (float)i / N;
        const float x = CXF + r * cosf(th + rot), y = CYF + r * sinf(th + rot);
        CAP(px, py, x, y, rr(4.5f * s), ink);
        px = x; py = y;
    }
}

/* ======================================================================== */
/* SLOTS: three reels per eye, jackpot (or a near miss)                      */
/* ======================================================================== */
#define WIN_W 38
#define WIN_Y0 90
#define WIN_H 56
static const int k_win_x[3] = {58, 101, 144};

static void slot_sym(ce_frame_t *f, int id, float cx, float cy)
{
    const int first = f->n_fx;
    switch (id & 3) {
    case 0: /* cherries */
        CAP(cx - 7, cy - 1, cx + 2, cy - 21, 1, RGB565(46, 125, 50));
        CAP(cx + 9, cy + 1, cx + 2, cy - 21, 1, RGB565(46, 125, 50));
        CIRC(cx - 9, cy + 8, 9, RGB565(215, 38, 61));
        CIRC(cx + 9, cy + 10, 9, RGB565(215, 38, 61));
        CIRC(cx - 12, cy + 5, 2, RGB565(255, 200, 200));
        break;
    case 1: /* lucky seven */
        GLYPH(cx - 13, cy - 19, 6, 7, RGB565(70, 10, 10));
        GLYPH(cx - 15, cy - 21, 6, 7, RGB565(224, 33, 46));
        break;
    case 2: /* bell */
        ELL(cx, cy - 2, 14, 15, RGB565(242, 194, 48));
        CAP(cx - 17, cy + 13, cx + 17, cy + 13, 3, RGB565(242, 194, 48));
        CIRC(cx, cy + 20, 4, RGB565(200, 140, 20));
        ELL(cx - 5, cy - 8, 3, 6, RGB565(255, 236, 150));
        break;
    default: /* lemon */
        ELL(cx, cy, 15, 13, RGB565(245, 224, 80));
        CIRC(cx - 16, cy, 3, RGB565(245, 224, 80));
        CIRC(cx + 16, cy, 3, RGB565(245, 224, 80));
        ELL(cx - 5, cy - 4, 5, 3, RGB565(255, 250, 190));
        break;
    }
    ce_sh_clip(f, FXL, first, WIN_Y0, WIN_Y0 + WIN_H - 1);
}

static float reel_pos(float t, int i, int target)
{
    static const float stop[3] = {1300.f, 1750.f, 2200.f};
    const float u = clampf((t - 200.f) / (stop[i] - 200.f), 0.f, 1.f), rounds = 3.f + i, k = 1.f - u;
    return target + 4.f * rounds * k * k;
}

static void fx_slots(int eye, float t, float *p, ce_frame_t *f)
{
    (void)eye;
    const int var = g_ce.fxs.var, sym = var & 3, miss = ((var >> 2) & 3) == 0;
    int tgt[3] = {sym, sym, miss ? (sym + 1 + ((var >> 4) % 3)) & 3 : sym};
    if (p) {
        p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .02f; p[P_LOWER] = .02f;
        if (miss && t > 2300.f) { const float s = sstep(2300.f, 2700.f, t); p[P_UPPER] = lerpf(.02f, .5f, s); p[P_GY] = .35f * s; }
        return;
    }
    const bool win = !miss && t > 2350.f && t < 3350.f;
    const bool flash = win && (((int)((t - 2350.f) / 100.f)) & 1);
    if (win && flash) tint(f, 1, T565(15, 24, 0));
    RECT(51, 81, 189, 155, flash ? 0xFFFF : RGB565(232, 185, 35));
    RECT(54, 84, 186, 152, RGB565(122, 16, 32));
    for (int i = 0; i < 3; i++) RECT(k_win_x[i], WIN_Y0, k_win_x[i] + WIN_W, WIN_Y0 + WIN_H - 1, RGB565(255, 246, 220));
    for (int i = 0; i < 3; i++) {
        const float o = reel_pos(t, i, tgt[i]), base = floorf(o), fr = o - base;
        const int cur = ((int)base) & 3, cx = k_win_x[i] + WIN_W / 2;
        const float yc = WIN_Y0 + WIN_H * .5f;
        if (fr < .93f) slot_sym(f, cur, (float)cx, yc + fr * WIN_H);
        if (fr > .07f) slot_sym(f, (cur + 1) & 3, (float)cx, yc + (fr - 1.f) * WIN_H);
    }
    if (win) {
        const float s = 12.f + 4.f * sinf(t / 70.f);
        sparkle(f, 51, 81, s, 0xFFFF); sparkle(f, 189, 81, s, 0xFFFF);
        sparkle(f, 51, 155, s, 0xFFFF); sparkle(f, 189, 155, s, 0xFFFF);
    }
}

/* ======================================================================== */
/* EURO: the pupils turn into shiny coins                                    */
/* ======================================================================== */
static void fx_euro(int eye, float t, float *p, ce_frame_t *f)
{
    if (p) { p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .02f; p[P_LOWER] = .04f; return; }
    const float te = t - eye * 80.f;
    if (te < 0.f) return;
    float s = te < 350.f ? backo(te / 350.f) : 1.f;
    s *= 1.f - sstep(2250.f, 2600.f, te);
    if (s <= .05f) return;
    const float cy = 121.f; /* a little low and big: the slit pupil must not peek out below */
    CIRC(CXF, cy, 63 * s, RGB565(176, 122, 18));
    CIRC(CXF, cy, 58 * s, RGB565(242, 194, 48));
    RING(CXF, cy, 50 * s, 47 * s, RGB565(222, 165, 28));
    int sc = R(8.f * s); if (sc < 1) sc = 1;
    GLYPH(CXF - 2.5f * sc + sc / 2, cy - 3.5f * sc + sc / 2, sc, CE_GLYPH_EURO, RGB565(255, 226, 120)); /* soft highlight */
    GLYPH(CXF - 2.5f * sc, cy - 3.5f * sc, sc, CE_GLYPH_EURO, RGB565(138, 90, 10));
    const float ph = eye * .35f;
    sparkle(f, 86, 88, 22.f * powf(fmaxf(0.f, sinf(te / 420.f + ph)), 3.f), 0xFFFF);
    sparkle(f, 156, 146, 16.f * powf(fmaxf(0.f, sinf(te / 330.f + 2.f + ph)), 3.f), 0xFFFF);
    if (te > 700.f && te < 960.f) sparkle(f, 92, 88, 34.f * sinf(PI_F * (te - 700.f) / 260.f), 0xFFFF); /* cha-ching */
}

/* ======================================================================== */
/* FISH: one eye is a fish tank, a second fish joins the first               */
/* ======================================================================== */
static void fish(ce_frame_t *f, float x, float y, int face, float s, float wag, uint16_t body, uint16_t stripe)
{
    TRI(x - face * 10 * s, y, x - face * 21 * s, y - 8 * s + wag, x - face * 21 * s, y + 8 * s + wag, body);
    TRI(x - 4 * face * s, y - 6 * s, x + 3 * face * s, y - 6 * s, x - face * s, y - 15 * s, body);
    ELL(x, y, 13 * s, 8 * s, body);
    CAP(x + face * 2 * s, y - 7 * s, x + face * 2 * s, y + 7 * s, rr(1.6f * s), stripe);
    CIRC(x + face * 8 * s, y - 2 * s, 3 * s, 0xFFFF);
    CIRC(x + face * 8.5f * s, y - 2 * s, 1.5f * s, 0);
}

static void fx_fish(int eye, float t, float *p, ce_frame_t *f)
{
    const int tank = g_ce.fxs.var & 1;
    if (p) {
        if (eye == tank) { p[P_GX] = 0.f; p[P_GY] = 0.f; }
        else { p[P_GX] = (tank ? .6f : -.6f) + .25f * sinf(t / 900.f); p[P_GY] = .05f * sinf(t / 600.f); p[P_PUPIL] = .35f; }
        return;
    }
    if (eye != tank) return;
    float s = backo(t / 300.f) * (1.f - sstep(3300.f, 3650.f, t));
    if (s <= .05f) return;
    CIRC(CXF, CYF, 62 * s, RGB565(20, 74, 128));
    CIRC(CXF, CYF, 60 * s, RGB565(42, 143, 207));
    CIRC(CXF, CYF - 6, 40 * s, RGB565(62, 168, 230));
    ELL(CXF, 164, 28 * s, 7 * s, RGB565(232, 216, 160));
    CAP(96, 166, 96 + 4 * sinf(t / 430.f) * s, 136, 2, RGB565(46, 155, 78));
    CAP(146, 166, 146 + 4 * sinf(t / 430.f + 1.f) * s, 142, 2, RGB565(46, 155, 78));
    for (int i = 0; i < 3; i++) {
        const float by = 168.f - fmodf(t * .04f + i * 40.f, 110.f);
        if (by > 64.f && s > .9f) RING(138 + 8 * sinf(t / 300.f + i * 2.f), by, 5, 3, RGB565(200, 235, 255));
    }
    const float th1 = t / 380.f, wag = 3.f * sinf(t / 60.f);
    fish(f, CXF + 32 * cosf(th1), 114 + 18 * sinf(th1), sinf(th1) < 0 ? 1 : -1, s, wag, RGB565(255, 138, 31), 0xFFFF);
    if (t > 1700.f) {
        const float s2 = backo((t - 1700.f) / 350.f) * s, th2 = t / 310.f + 2.f;
        fish(f, 122 + 26 * cosf(th2), 126 + 14 * sinf(th2), sinf(th2) < 0 ? 1 : -1, s2, -wag, RGB565(245, 208, 32), RGB565(45, 91, 216));
    }
}

/* ======================================================================== */
/* RAIN: drops, a gloomy tint and a lightning flash on both eyes at once     */
/* ======================================================================== */
static bool storm_flash(float t)
{
    return (t >= 900.f && t < 960.f) || (t >= 1010.f && t < 1080.f) || (t >= 2100.f && t < 2170.f);
}

static void fx_rain(int eye, float t, float *p, ce_frame_t *f)
{
    if (p) {
        p[P_GX] = 0.f; p[P_GY] = -.35f; p[P_UPPER] = .06f;
        static const float fs[2] = {900.f, 2100.f};
        for (int i = 0; i < 2; i++)
            if (t >= fs[i] && t < fs[i] + 320.f) { const float k = (t - fs[i]) / 320.f; p[P_PUPIL] = lerpf(.95f, .22f, k); p[P_UPPER] = lerpf(.3f, .06f, k); }
        return;
    }
    const bool flash = storm_flash(t);
    tint(f, 1, flash ? T565(15, 31, 15) : T565(0, 6, 10));
    for (int i = 0; i < 16; i++) {
        const float x = (float)(hsh(i, eye + 7) % 260) - 10.f, spd = 420.f + (float)(hsh(i, 3) % 260);
        const float y = fmodf((float)(hsh(i, 5) % 320) + t * spd / 1000.f, 320.f) - 40.f;
        CAP(x, y, x - 3, y + 13, 1, RGB565(170, 205, 255));
    }
    for (int i = 0; i < 4; i++) { /* big drops running down the lens */
        const float x = 30.f + (float)(hsh(i, eye + 31) % 180), y = fmodf((float)(hsh(i, 9) % 260) + t * .11f, 260.f) - 10.f;
        CIRC(x, y, 3, RGB565(190, 220, 255));
        CAP(x, y - 3, x, y - 13, 1, RGB565(150, 185, 240));
    }
    if (flash) {
        const float xs = eye ? 44.f : 196.f;
        static const float bx[6] = {0, -16, 6, -14, 4, -18}, by[6] = {-2, 48, 72, 132, 152, 235};
        for (int i = 0; i < 5; i++) CAP(xs + bx[i], by[i], xs + bx[i + 1], by[i + 1], 5, RGB565(255, 240, 150));
        for (int i = 0; i < 5; i++) CAP(xs + bx[i], by[i], xs + bx[i + 1], by[i + 1], 2, RGB565(255, 255, 225));
    }
}

/* ======================================================================== */
/* SNOW: flakes, a snow bank and a snowman that pops up in the pupil         */
/* ======================================================================== */
static void fx_snow(int eye, float t, float *p, ce_frame_t *f)
{
    if (p) { p[P_GX] = .3f * sinf(t / 900.f); p[P_GY] = -.2f; p[P_PUPIL] = .45f; return; }
    tint(f, 1, T565(4, 10, 14));
    for (int i = 0; i < 16; i++) {
        const uint32_t h = hsh(i, eye * 13 + 1);
        const float spd = 35.f + (float)((h >> 8) % 45), x = (float)(h % 240) + 14.f * sinf(t / 700.f + i);
        const float y = fmodf((float)((h >> 16) % 270) + t * spd / 1000.f, 270.f) - 15.f;
        CIRC(x, y, 2 + (int)((h >> 4) % 3), 0xFFFF);
    }
    const float bank = sstep(400.f, 1000.f, t);
    ELL(CXF, 172, 76 * bank, 10 * bank, RGB565(240, 246, 255));
    float S = t < 1100.f ? 0.f : backo((t - 1100.f) / 450.f);
    S *= 1.f - sstep(3400.f, 3750.f, t);
    if (S <= .05f) return;
#define SX(lx) (CXF + (lx) * S)
#define SY(ly) (172 + (ly) * S)
    const uint16_t white = RGB565(246, 248, 255), line = RGB565(110, 125, 160), coal = RGB565(20, 20, 24), red = RGB565(200, 40, 50);
    CIRC(SX(0), SY(-27), 28 * S, line);
    CIRC(SX(0), SY(-70), 19 * S, line);
    CIRC(SX(0), SY(-27), 26 * S, white);
    CIRC(SX(0), SY(-70), 17 * S, white);
    RECT(SX(-20), SY(-91), SX(20), SY(-87), coal);
    RECT(SX(-12), SY(-108), SX(12), SY(-91), coal);
    RECT(SX(-12), SY(-95), SX(12), SY(-91), red);
    TRI(SX(0), SY(-70), SX(22), SY(-67), SX(0), SY(-64), RGB565(255, 140, 30));
    CIRC(SX(-6), SY(-75), 2.5f * S, coal); CIRC(SX(6), SY(-75), 2.5f * S, coal);
    RECT(SX(-16), SY(-55), SX(16), SY(-48), red);
    RECT(SX(6), SY(-48), SX(12), SY(-34), red);
    CIRC(SX(0), SY(-30), 3 * S, coal); CIRC(SX(0), SY(-18), 3 * S, coal);
#undef SX
#undef SY
}

/* ======================================================================== */
/* LEAF: two autumn leaves fall, the eyes follow the first one to the ground */
/* ======================================================================== */
static void leaf_pos(float t, int which, float *x, float *y, float *a)
{
    if (which == 0) {
        const float tl = (150.f + 34.f) / .070f;
        const float te = t < tl ? t : tl;
        *x = 120.f + 52.f * sinf(te / 470.f) * (1.f - .6f * sstep(2400.f, 3000.f, te));
        *y = -34.f + .070f * te;
        *a = .8f * sinf(te / 470.f + 1.4f) + te * .0004f;
        if (t > tl) *a += .08f * sinf((t - tl) / 120.f) * expf(-(t - tl) / 400.f);
    } else {
        const float tb = t - 900.f, tl = (150.f + 30.f) / .075f, te = tb < tl ? tb : tl;
        *x = 168.f + 40.f * sinf(te / 410.f + 2.f);
        *y = -30.f + .075f * te;
        *a = .9f * sinf(te / 410.f + 3.f) - te * .0005f;
        if (tb > tl) *a += .08f * sinf((tb - tl) / 110.f) * expf(-(tb - tl) / 400.f);
    }
}

static void leaf(ce_frame_t *f, float x, float y, float a, float k, uint16_t body, uint16_t vein)
{
#define XF(lx, ly) xf(x, y, a, k, (lx), (ly))
    pt_t tip = XF(0, -22), l = XF(-14, -2), r = XF(14, -2), b = XF(0, 16);
    ce_sh_tri(f, FXL, tip.x, tip.y, l.x, l.y, r.x, r.y, body);
    ce_sh_tri(f, FXL, b.x, b.y, l.x, l.y, r.x, r.y, body);
    pt_t l1 = XF(-24, -12), l2 = XF(-8, -10), r1 = XF(24, -12), r2 = XF(8, -10);
    ce_sh_tri(f, FXL, l.x, l.y, l1.x, l1.y, l2.x, l2.y, body);
    ce_sh_tri(f, FXL, r.x, r.y, r1.x, r1.y, r2.x, r2.y, body);
    pt_t v0 = XF(0, -18), v1 = XF(0, 22), s1 = XF(0, 28);
    ce_sh_capsule(f, FXL, v0.x, v0.y, v1.x, v1.y, 1, vein);
    ce_sh_capsule(f, FXL, v1.x, v1.y, s1.x, s1.y, 1, RGB565(110, 70, 30));
#undef XF
}

static void fx_leaf(int eye, float t, float *p, ce_frame_t *f)
{
    (void)eye;
    float xa, ya, aa;
    leaf_pos(t, 0, &xa, &ya, &aa);
    if (p) {
        p[P_GX] = clampf((xa - 120.f) / 60.f, -1.f, 1.f) * .7f;
        p[P_GY] = clampf((ya - 105.f) / 60.f, -1.f, 1.f) * .85f;
        return;
    }
    const float k = 1.f - sstep(3700.f, 4000.f, t);
    if (k <= .05f) return;
    leaf(f, xa, ya, aa, 1.5f * k, RGB565(226, 117, 27), RGB565(140, 50, 10));
    if (t > 900.f) {
        float xb, yb, ab;
        leaf_pos(t, 1, &xb, &yb, &ab);
        leaf(f, xb, yb, ab, 1.2f * k, RGB565(232, 185, 35), RGB565(150, 100, 10));
    }
}

/* ======================================================================== */
/* DAY / NIGHT: a dial in the pupil, the sun sets and the moon rises         */
/* ======================================================================== */
static void fx_daynight(int eye, float t, float *p, ce_frame_t *f)
{
    (void)eye;
    if (p) { p[P_GX] = 0.f; p[P_GY] = -.3f; p[P_PUPIL] = .4f; return; }
    float s = backo(t / 350.f) * (1.f - sstep(3400.f, 3800.f, t));
    if (s <= .05f) return;
    const float A = PI_F * sstep(900.f, 2300.f, t), n = A / PI_F;
    const uint16_t sky = n < .5f ? mix565(74, 168, 255, 255, 138, 74, n / .5f) : mix565(255, 138, 74, 11, 18, 51, (n - .5f) / .5f);
    CIRC(CXF, CYF, 60 * s, RGB565(40, 50, 90));
    CIRC(CXF, CYF, 57 * s, sky);
    const float ss = clampf(1.5f * (1.f - n), 0.f, 1.f) * s, ms = clampf(1.5f * (n - .3333f), 0.f, 1.f) * s;
    const float sunx = CXF + 30.f * sinf(A) * s, suny = CYF - 30.f * cosf(A) * s;
    if (ss > .05f) {
        for (int i = 0; i < 8; i++) {
            const float a = i * PI_F / 4.f + t / 1500.f;
            CAP(sunx + cosf(a) * 21 * ss, suny + sinf(a) * 21 * ss, sunx + cosf(a) * 29 * ss, suny + sinf(a) * 29 * ss, 2, RGB565(255, 200, 40));
        }
        CIRC(sunx, suny, 16 * ss, RGB565(255, 216, 58));
    }
    if (ms > .05f) {
        const float mx = CXF - 30.f * sinf(A) * s, my = CYF + 30.f * cosf(A) * s;
        CIRC(mx, my, 16 * ms, RGB565(245, 241, 208));
        CIRC(mx + 8 * ms, my - 5 * ms, 13 * ms, sky);
        static const float st[6][2] = {{-34, -26}, {30, -34}, {-8, -42}, {38, 6}, {-40, 14}, {14, 36}};
        for (int i = 0; i < 6; i++) sparkle(f, CXF + st[i][0] * s, CYF + st[i][1] * s, ms * (4.f + 3.f * sinf(t / 200.f + i * 1.7f)), 0xFFFF);
    }
}

/* ======================================================================== */
/* PAW: a cat paw swats at the pupil, which dodges (twice)                   */
/* ======================================================================== */
static void paw_pos(float t, int side, float *x, float *y)
{
    /* keyframes: approach, wind up, swat left, recoil, wind up, swat right, recoil, retreat.
     * ks: 1 = x offset is mirrored by `side` (the arm's side), 0 = absolute */
    static const float kt[10] = {0, 450, 700, 780, 950, 1250, 1330, 1500, 1750, 2200};
    static const float kx[10] = {60, 14, 20, -12, -2, 8, 20, 4, 40, 60};
    static const float ky[10] = {300, 150, 160, 120, 150, 156, 118, 150, 200, 310};
    static const uint8_t ks[10] = {1, 1, 1, 0, 0, 0, 0, 0, 1, 1};
    int i = 0;
    while (i < 8 && t > kt[i + 1]) i++;
    const float u = sstep(kt[i], kt[i + 1], t);
    const float x0 = kx[i] * (ks[i] ? side : 1), x1 = kx[i + 1] * (ks[i + 1] ? side : 1);
    *x = 120.f + lerpf(x0, x1, u);
    *y = lerpf(ky[i], ky[i + 1], u);
}

static void fx_paw(int eye, float t, float *p, ce_frame_t *f)
{
    const int A = g_ce.fxs.var & 1, side = (g_ce.fxs.var & 2) ? 1 : -1;
    if (p) {
        if (eye == A) {
            const float dodge = sstep(700.f, 760.f, t) - sstep(1000.f, 1100.f, t) - (sstep(1260.f, 1320.f, t) - sstep(1500.f, 1620.f, t));
            p[P_GX] = .85f * dodge; p[P_GY] = .4f * sstep(0.f, 300.f, t) * (1.f - sstep(450.f, 600.f, t));
            p[P_PUPIL] = .5f;
        } else {
            p[P_GX] = A ? .65f : -.65f; p[P_GY] = .1f; p[P_PUPIL] = .3f;
        }
        return;
    }
    if (eye != A) return;
    float px, py;
    paw_pos(t, side, &px, &py);
    if (py > 300.f) return;
    const uint16_t fur = RGB565(230, 161, 91), line = RGB565(90, 52, 20), pad = RGB565(242, 156, 176);
    const float K = 1.3f, ax = px + side * 70.f;
    CAP(ax, 340, px, py + 12 * K, R(17 * K), line);
    CIRC(px, py, 25 * K, line);
    CIRC(px - 14 * K, py - 22 * K, 12 * K, line); CIRC(px, py - 28 * K, 12 * K, line); CIRC(px + 14 * K, py - 22 * K, 12 * K, line);
    CAP(ax, 340, px, py + 12 * K, R(15 * K), fur);
    CIRC(px, py, 22 * K, fur);
    CIRC(px - 14 * K, py - 22 * K, 10 * K, fur); CIRC(px, py - 28 * K, 10 * K, fur); CIRC(px + 14 * K, py - 22 * K, 10 * K, fur);
    CIRC(px, py + 4 * K, 10 * K, pad);
    CIRC(px - 14 * K, py - 20 * K, 4.5f * K, pad); CIRC(px, py - 26 * K, 4.5f * K, pad); CIRC(px + 14 * K, py - 20 * K, 4.5f * K, pad);
    if ((t >= 740.f && t < 860.f) || (t >= 1300.f && t < 1420.f)) { /* claws out */
        CAP(px - 14 * K, py - 31 * K, px - 15 * K, py - 40 * K, 2, 0xFFFF);
        CAP(px, py - 37 * K, px, py - 46 * K, 2, 0xFFFF);
        CAP(px + 14 * K, py - 31 * K, px + 15 * K, py - 40 * K, 2, 0xFFFF);
    }
}

/* ======================================================================== */
/* SPIDER: crawls across an eye, both eyes cross and shudder                 */
/* ======================================================================== */
static void fx_spider(int eye, float t, float *p, ce_frame_t *f)
{
    const int A = g_ce.fxs.var & 1, dir = g_ce.fxs.dir;
    if (p) {
        const float w = sstep(150.f, 500.f, t) * (1.f - sstep(3000.f, 3400.f, t));
        p[P_GX] = (eye ? -.62f : .62f) * w; p[P_GY] = .45f * w; p[P_PUPIL] = lerpf(.22f, .12f, w);
        const float sh = sstep(1450.f, 1500.f, t) * (1.f - sstep(1950.f, 2050.f, t)) + .12f * w;
        p[P_GX] += sinf(t / 21.f) * .1f * sh; p[P_UPPER] += .25f * sh; p[P_TILT] += sinf(t / 37.f) * .2f * sh;
        return;
    }
    if (eye != A || t < 280.f) return;
    float xd;
    if (t < 1300.f) xd = lerpf(-40.f, 105.f, (t - 300.f) / 1000.f);
    else if (t < 1900.f) xd = lerpf(105.f, 135.f, (t - 1300.f) / 600.f);
    else xd = lerpf(135.f, 280.f, (t - 1900.f) / 1300.f);
    if (t > 3200.f) return;
    const float x = dir > 0 ? xd : CAT_EYES_W - xd, y = 112.f + 10.f * sinf(t / 260.f);
    const int face = dir;
    const float F = 1.4f;
    const uint16_t black = RGB565(18, 12, 24);
    const float ph = t / 65.f;
    for (int sd = -1; sd <= 1; sd += 2)
        for (int k = 0; k < 4; k++) {
            const float sw = sinf(ph + k * 1.5708f + (sd > 0 ? PI_F : 0.f)) * 5.f;
            const float ax = x + face * (10.f - 5.f * k) * F, kx = ax + face * (6.f - 4.f * k) * F + sw * .4f * face;
            const float fx = ax + face * (12.f - 9.f * k) * F + sw * face;
            CAP(ax, y, kx, y + sd * 20 * F, 3, black);
            CAP(kx, y + sd * 20 * F, fx, y + sd * 34 * F, 3, black);
        }
    CIRC(x - face * 11 * F, y + 2, 14 * F, black);
    CIRC(x - face * 11 * F, y + 1, 4 * F, RGB565(232, 84, 26));
    CIRC(x + face * 9 * F, y, 9 * F, black);
    CIRC(x + face * 13 * F, y - 3 * F, 2.5f, 0xFFFF); CIRC(x + face * 13 * F, y + 3 * F, 2.5f, 0xFFFF);
}

/* ======================================================================== */
/* LOADING: buffering wheel, "99%", snap                                     */
/* ======================================================================== */
static void fx_loading(int eye, float t, float *p, ce_frame_t *f)
{
    (void)eye;
    if (p) {
        p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .02f; p[P_LOWER] = .04f;
        if (t >= 3300.f) {
            const float s = sstep(3300.f, 3350.f, t) * (1.f - sstep(3420.f, 3500.f, t));
            p[P_UPPER] = lerpf(.02f, 1.f, s); p[P_LOWER] = lerpf(.04f, .3f, s);
            p[P_PUPIL] = lerpf(.22f, .95f, 1.f - sstep(3500.f, 3900.f, t));
        }
        return;
    }
    if (t >= 3300.f) return;
    const float s = backo(t / 250.f);
    ELL(CXF, CYF, 88 * s, 54 * s, RGB565(60, 64, 76));
    ELL(CXF, CYF, 86 * s, 52 * s, 0);
    if (s < .95f) return;
    const bool text = (t >= 1700.f && t < 2350.f) || (t >= 2550.f && t < 3150.f);
    if (text) {
        GLYPH(69, 97, 6, 9, 0xFFFF); GLYPH(105, 97, 6, 9, 0xFFFF); GLYPH(141, 97, 6, CE_GLYPH_PCT, 0xFFFF);
        return;
    }
    const int head = (int)(t / 83.f) % 12;
    for (int i = 0; i < 12; i++) {
        const float a = i * TAU_F / 12.f - PI_F / 2.f;
        const int age = (head - i + 12) % 12; /* 0 = brightest */
        const int g = 255 - age * 17;
        CIRC(CXF + 34 * cosf(a), 112 + 34 * sinf(a), 6, RGB565(g, g, g));
    }
}

/* ======================================================================== */
/* PAC-MAN: chased by a ghost across both eyes, eats a pellet, turns around  */
/* ======================================================================== */
static void ghost(ce_frame_t *f, float x, float y, bool scared, int face)
{
    const uint16_t body = scared ? RGB565(40, 40, 255) : RGB565(255, 32, 32);
    CIRC(x, y - 4, 22, body);
    RECT(x - 22, y - 4, x + 22, y + 22, body);
    TRI(x - 12, y + 23, x - 2, y + 23, x - 7, y + 12, 0);
    TRI(x + 2, y + 23, x + 12, y + 23, x + 7, y + 12, 0);
    if (scared) {
        CIRC(x - 7, y - 6, 3, 0xFFFF); CIRC(x + 7, y - 6, 3, 0xFFFF);
        CAP(x - 14, y + 7, x - 7, y + 3, 1, 0xFFFF); CAP(x - 7, y + 3, x, y + 7, 1, 0xFFFF);
        CAP(x, y + 7, x + 7, y + 3, 1, 0xFFFF); CAP(x + 7, y + 3, x + 14, y + 7, 1, 0xFFFF);
    } else {
        ELL(x - 8, y - 6, 6, 7, 0xFFFF); ELL(x + 8, y - 6, 6, 7, 0xFFFF);
        CIRC(x - 8 + face * 3, y - 6, 3, RGB565(40, 40, 255)); CIRC(x + 8 + face * 3, y - 6, 3, RGB565(40, 40, 255));
    }
}

static void fx_pacman(int eye, float t, float *p, ce_frame_t *f)
{
    if (p) { p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .02f; p[P_LOWER] = .02f; return; }
    float s = backo(t / 200.f) * (1.f - sstep(3900.f, 4150.f, t));
    if (s <= .05f) return;
    ELL(CXF, CYF, 106 * s, 68 * s, 0);
    if (s < .95f) return;
    const uint16_t wall = RGB565(36, 56, 255);
    CAP(28, 88, 212, 88, 3, wall);
    CAP(28, 148, 212, 148, 3, wall);
    const bool back = t >= 2300.f;
    const float pac = back ? 430.f - .30f * (t - 2300.f) : -50.f + 480.f * t / 2300.f;
    const float gho = back ? 312.f - .34f * (t - 2300.f) : pac - 118.f;
    if (!back)
        for (int k = 0; k < 18; k++) {
            const float wx = 14.f + 30.f * k, sx = scr(eye, wx);
            if (wx > pac + 8.f && sx > 2.f && sx < 238.f) CIRC(sx, 118, 4, RGB565(255, 216, 176));
        }
    if (!back) {
        const float sx = scr(eye, 440.f);
        if (sx > 2.f && sx < 238.f && (((int)(t / 150.f)) & 1)) CIRC(sx, 118, 8, RGB565(255, 216, 176));
    }
    const float gx = scr(eye, gho);
    if (gx > -30.f && gx < 270.f) ghost(f, gx, 118, back, back ? -1 : 1);
    const float px = scr(eye, pac);
    if (px > -40.f && px < 280.f) {
        const int face = back ? -1 : 1;
        CIRC(px, 118, 26, RGB565(255, 224, 0));
        const float m = .1f + .55f * fabsf(sinf(t / 65.f));
        TRI(px, 118, px + face * 30 * cosf(m), 118 - 30 * sinf(m), px + face * 30 * cosf(m), 118 + 30 * sinf(m), 0);
    }
}

/* ======================================================================== */
/* MATRIX: green code rain, the right eye lags behind the left one           */
/* ======================================================================== */
static void fx_matrix(int eye, float t, float *p, ce_frame_t *f)
{
    const float te = t - eye * 380.f;
    if (p) {
        p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .02f; p[P_LOWER] = .04f; p[P_PUPIL] = .15f;
        return;
    }
    if (te < 0.f) return;
    tint(f, (te < 200.f || te > 3600.f) ? 1 : 2, (te < 200.f || te > 3600.f) ? T565(0, 10, 0) : T565(0, 6, 0));
    static const uint16_t col[5] = {RGB565(207, 255, 208), RGB565(77, 255, 110), RGB565(31, 216, 74), RGB565(18, 166, 52), RGB565(11, 110, 34)};
    for (int c = 0; c < 7; c++) {
        const float spd = 170.f + (float)(hsh(c, g_ce.fxs.seed) % 160), start = 90.f * c;
        if (te < start) continue;
        const float head = fmodf((te - start) * spd / 1000.f, 330.f) - 60.f;
        for (int k = 0; k < 5; k++) {
            const float gy = head - 24.f * k;
            if (gy < -22.f || gy > 240.f) continue;
            const int r = (int)(hsh(c * 8 + k, (uint32_t)(te / 140.f) + g_ce.fxs.seed) % 18);
            GLYPH(30 + c * 30 - 7, gy, 3, r < 10 ? r : r + 2, col[k]);
        }
    }
}

/* ======================================================================== */
/* DISCO: mirror ball, colour wash on the beat, sparkles                     */
/* ======================================================================== */
static void fx_disco(int eye, float t, float *p, ce_frame_t *f)
{
    const float beat = 469.f; /* 128 bpm */
    const int bi = (int)(t / beat);
    const float ph = fmodf(t, beat) / beat;
    if (p) {
        p[P_GX] = .5f * sinf(t / beat * PI_F); p[P_GY] = .1f * cosf(t / beat * TAU_F);
        p[P_UPPER] = .12f + .08f * (1.f - ph); p[P_PUPIL] = .6f;
        return;
    }
    static const uint16_t wash[6] = {T565(15, 0, 15), T565(0, 30, 15), T565(15, 30, 0), T565(15, 4, 0), T565(2, 30, 2), T565(2, 6, 15)};
    tint(f, 1, ph < .12f ? T565(15, 31, 15) : wash[(bi + eye * 3) % 6]);
    const float fade = 1.f - sstep(3700.f, 4000.f, t), s = backo(t / 300.f) * fade;
    if (s <= .05f) return;
    const float bx = CXF, by = 112.f, br = 34.f * s;
    CAP(bx, 50, bx, by - br, 1, RGB565(150, 155, 165));
    CIRC(bx, by, br + 2, RGB565(60, 66, 80));
    CIRC(bx, by, br, RGB565(201, 206, 216));
    static const float rows[4] = {-20.f, -8.f, 8.f, 20.f};
    for (int i = 0; i < 4; i++) {
        const float dy = rows[i] * s, hw = sqrtf(fmaxf(0.f, br * br - dy * dy));
        RECT(bx - hw, by + dy - 1, bx + hw, by + dy + 1, RGB565(110, 118, 135));
    }
    const float rot = fmodf(t * .0022f, PI_F / 5.f);
    for (int k = 0; k < 5; k++) {
        const float x = br * cosf(rot + k * PI_F / 5.f), hh = sqrtf(fmaxf(0.f, br * br - x * x));
        CAP(bx + x, by - hh, bx + x, by + hh, 1, RGB565(110, 118, 135));
    }
    for (int i = 0; i < 5; i++) {
        const float a = hf(bi, i) * TAU_F, rad = 38.f + hf(bi, i + 9) * 18.f;
        sparkle(f, bx + cosf(a) * rad * 1.5f, by + sinf(a) * rad * .85f, 13.f * (1.f - ph) * fade, (i & 1) ? RGB565(255, 240, 150) : 0xFFFF);
    }
}

/* ======================================================================== */
/* PUMPKIN: jack-o'-lantern pupils, a bat flies by                           */
/* ======================================================================== */
static void fx_pumpkin(int eye, float t, float *p, ce_frame_t *f)
{
    if (p) { p[P_GX] = 0.f; p[P_GY] = 0.f; p[P_UPPER] = .04f; p[P_LOWER] = .04f; return; }
    const float te = t - eye * 90.f;
    tint(f, 1, T565(5, 0, 8));
    if (te < 0.f) return;
    const float S = 1.2f * backo(te / 450.f) * (1.f - sstep(3350.f, 3800.f, te));
    if (S > .06f) {
        const float wob = .08f * sinf(te / 150.f) * expf(-te / 700.f), sx = S * (1.f - wob), sy = S * (1.f + wob);
#define PX(lx) (CXF + (lx) * sx)
#define PY(ly) (126 + (ly) * sy)
        const uint16_t orange = RGB565(242, 138, 18), dark = RGB565(200, 100, 10), rib = RGB565(176, 86, 6), green = RGB565(74, 107, 31);
        static const uint16_t glow[4] = {RGB565(255, 214, 58), RGB565(255, 170, 30), RGB565(255, 226, 100), RGB565(255, 190, 40)};
        const uint16_t gl = glow[hsh((uint32_t)(te / 80.f), eye) & 3];
        ELL(PX(-26), PY(0), 28 * sx, 36 * sy, dark);
        ELL(PX(26), PY(0), 28 * sx, 36 * sy, dark);
        ELL(PX(0), PY(0), 32 * sx, 40 * sy, orange);
        CAP(PX(-16), PY(-30), PX(-16), PY(30), 1, rib);
        CAP(PX(16), PY(-30), PX(16), PY(30), 1, rib);
        RECT(PX(-5), PY(-52), PX(5), PY(-38), green);
        CAP(PX(5), PY(-49), PX(14), PY(-54), rr(2 * S), green);
        const float bl = fmodf(te, 1300.f) < 120.f ? .25f : 1.f; /* carved eyes blink */
        TRI(PX(-24), PY(-4), PX(-8), PY(-4), PX(-16), PY(-4 - 16 * bl), gl);
        TRI(PX(8), PY(-4), PX(24), PY(-4), PX(16), PY(-4 - 16 * bl), gl);
        TRI(PX(0), PY(2), PX(-5), PY(10), PX(5), PY(10), gl);
        ELL(PX(0), PY(20), 26 * sx, 10 * sy, gl);
        TRI(PX(-14), PY(11), PX(-5), PY(11), PX(-10), PY(19), orange);
        TRI(PX(6), PY(29), PX(15), PY(29), PX(10), PY(21), orange);
#undef PX
#undef PY
    }
    if (t > 1300.f && t < 2700.f) { /* a bat crosses both eyes */
        const float u = lerpf(-50.f, 590.f, (t - 1300.f) / 1400.f), bx = scr(eye, g_ce.fxs.dir > 0 ? u : FX_WORLD - u);
        const float by = 62.f + 14.f * sinf(t / 160.f), fl = 10.f * sinf(t / 55.f);
        if (bx > -30.f && bx < 270.f) {
            const uint16_t bat = RGB565(88, 44, 130);
            TRI(bx, by, bx - 22, by - 6 + fl, bx - 8, by + 6, bat);
            TRI(bx, by, bx + 22, by - 6 + fl, bx + 8, by + 6, bat);
            CIRC(bx, by + 1, 5, bat);
        }
    }
}

/* ---------------- dispatch ---------------- */
typedef void (*fx_fn)(int eye, float t, float *p, ce_frame_t *f);
static const fx_fn k_fx[CAT_ANIM_FX_LAST - CAT_ANIM_FX_FIRST + 1] = {
    fx_food, fx_fly, fx_plane, fx_bubble, fx_spiral, fx_slots, fx_euro, fx_fish, fx_rain, fx_snow,
    fx_leaf, fx_daynight, fx_paw, fx_spider, fx_loading, fx_pacman, fx_matrix, fx_disco, fx_pumpkin,
};
static const uint16_t k_dur[CAT_ANIM_FX_LAST - CAT_ANIM_FX_FIRST + 1] = {
    3500, 3600, 3300, 3500, 3150, 3600, 2700, 3650, 3200, 3900,
    4000, 3800, 2300, 3600, 3900, 4150, 3800, 4000, 3800,
};

uint16_t ce_fx_duration(cat_anim_t id)
{
    if (id == CE_ANIM_EFX) return ce_efx_duration();
    return (id >= CAT_ANIM_FX_FIRST && id <= CAT_ANIM_FX_LAST) ? k_dur[id - CAT_ANIM_FX_FIRST] : 1;
}

void ce_fx_begin(cat_anim_t id)
{
    (void)id;
    g_ce.fxs.dir = (ce_rand() & 1) ? 1 : -1;
    g_ce.fxs.var = (int)(ce_rand() >> 8);
    g_ce.fxs.seed = ce_rand();
}

void ce_fx_params(cat_anim_t id, int eye, float t, float *p)
{
    if ((unsigned)eye > 1) return;
    if (id == CE_ANIM_EFX) { ce_efx_params(t, p); return; }
    if (id < CAT_ANIM_FX_FIRST || id > CAT_ANIM_FX_LAST) return;
    k_fx[id - CAT_ANIM_FX_FIRST](eye, t, p, NULL);
}

void ce_fx_draw(cat_anim_t id, int eye, float t, ce_frame_t *f)
{
    if ((unsigned)eye > 1) return;
    if (id == CE_ANIM_EFX) ce_efx_draw(eye, t, f);
    else if (id >= CAT_ANIM_FX_FIRST && id <= CAT_ANIM_FX_LAST) k_fx[id - CAT_ANIM_FX_FIRST](eye, t, NULL, f);
    else return;
    /* rows touched by the shapes, for the dirty-row logic */
    int lo = CAT_EYES_H, hi = 0;
    for (int i = 0; i < f->n_fx; i++) {
        const ce_shape_t *s = &f->fx[i];
        int a = s->by0, b = s->by1;
        if (s->clip && s->kind != SH_TRI) { if (a < s->x2) a = s->x2; if (b > s->y2) b = s->y2; }
        if (a < lo) lo = a;
        if (b + 1 > hi) hi = b + 1;
    }
    f->fx_lo = (int16_t)(lo < 0 ? 0 : lo);
    f->fx_hi = (int16_t)(hi > CAT_EYES_H ? CAT_EYES_H : hi);
}
