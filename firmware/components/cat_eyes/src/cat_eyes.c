/* Public API, idle scheduler and per-frame parameter evaluation. */
#include "cat_eyes_internal.h"
#include <math.h>
#include <string.h>

ce_state_t g_ce;

static const char *const k_anim_names[CAT_ANIM_COUNT] = {
    "wink", "slow_blink", "roll", "side_eye", "hunt", "startle",
    "nictitate", "sleepy", "cross_eyed", "happy", "glance",
    "food", "fly", "paper_plane", "bubble", "spiral", "slots", "euro", "fish", "rain", "snow",
    "leaf", "day_night", "paw", "spider", "loading", "pacman", "matrix", "disco", "pumpkin",
    "special",
};
/* relative frequency in random idle; rare ones stay surprising.
 * The effects are big set pieces, so each one is rare (together about 40 %). */
static const float k_weights[CAT_ANIM_COUNT] = {
    2, 2, 1.5f, 1.5f, 1.2f, .6f, 1, .8f, .6f, 1.5f, 3,
    .8f, .7f, .6f, .6f, .5f, .5f, .5f, .5f, .6f, .5f,
    .6f, .5f, .6f, .5f, .5f, .5f, .5f, .5f, .5f,
    1.4f,
};
/* seasonal affinity: some effects come up more often in matching styles */
static float affinity(int anim, cat_style_t st)
{
    if (st == CAT_STYLE_HALLOWEEN) {
        if (anim == CAT_ANIM_PUMPKIN) return 6.f;
        if (anim == CAT_ANIM_SPIDER) return 4.f;
        if (anim == CAT_ANIM_RAIN) return 2.f;
    }
    if (st == CAT_STYLE_CHRISTMAS && anim == CAT_ANIM_SNOW) return 6.f;
    if (st != CAT_STYLE_HALLOWEEN && anim == CAT_ANIM_PUMPKIN) return .35f;
    if (st != CAT_STYLE_CHRISTMAS && anim == CAT_ANIM_SNOW) return .6f;
    return 1.f;
}
static const char *const k_iris_names[CAT_IRIS_COUNT] = {"amber", "green", "blue", "copper", "violet", "odd"};
static const float k_base[P_COUNT] = {0, 0, .12f, .06f, .22f, 0, 0, 0, 0};

/* ---------------- helpers ---------------- */
uint32_t ce_rand(void)
{
    uint32_t x = g_ce.rng ? g_ce.rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_ce.rng = x;
}
float ce_randf(float a, float b) { return a + (b - a) * (float)(ce_rand() & 0xffffff) / 16777215.f; }

static float blink_curve(int32_t dt)
{
    if (dt < 0 || dt > 280) return 0;
    if (dt < 80) { float u = dt / 80.f; return u * u * (3 - 2 * u); }
    if (dt < 120) return 1;
    float u = (dt - 120) / 160.f; u = u * u * (3 - 2 * u); return 1 - u;
}

/* ---------------- API ---------------- */
void cat_eyes_init(uint32_t seed)
{
    memset(&g_ce, 0, sizeof(g_ce));
    g_ce.rng = seed | 1u;
    g_ce.auto_idle = true;
    g_ce.swap = true;
    g_ce.style = CAT_STYLE_REALISTIC;
    g_ce.iris = CAT_IRIS_AMBER;
    g_ce.last_anim = CAT_ANIM_NONE;
    g_ce.pupil_s[0] = g_ce.pupil_s[1] = .22f;
    for (int e = 0; e < 2; e++)
        for (int i = 0; i < CE_SNOW; i++) {
            g_ce.snow[e][i].x = ce_randf(0, 240); g_ce.snow[e][i].y = ce_randf(0, 240);
            g_ce.snow[e][i].v = ce_randf(.3f, .9f); g_ce.snow[e][i].r = ce_randf(1.2f, 3.f);
        }
    for (int i = 0; i < CE_BATS; i++) {
        g_ce.bats[i].a = ce_randf(0, 6.28f); g_ce.bats[i].r = ce_randf(70, 100); g_ce.bats[i].v = ce_randf(.004f, .009f);
    }
    g_ce.force_full[0] = g_ce.force_full[1] = true;
    ce_render_init();
    g_ce.inited = true;
}

void cat_eyes_set_style(cat_style_t s)
{
    if ((unsigned)s >= CAT_STYLE_COUNT || s == g_ce.style) return;
    g_ce.style = s;
    if (g_ce.anim_on && g_ce.anim.id == CAT_ANIM_SPECIAL) g_ce.anim_on = false;
    g_ce.force_full[0] = g_ce.force_full[1] = true;
}
void cat_eyes_set_iris(cat_iris_t i)
{
    if ((unsigned)i >= CAT_IRIS_COUNT) return;
    g_ce.iris = i; g_ce.force_full[0] = g_ce.force_full[1] = true;
}
void cat_eyes_set_auto(bool on) { g_ce.auto_idle = on; }
void cat_eyes_set_swap_bytes(bool s) { g_ce.swap = s; }
cat_style_t cat_eyes_get_style(void) { return g_ce.style; }
cat_iris_t cat_eyes_get_iris(void) { return g_ce.iris; }
bool cat_eyes_get_auto(void) { return g_ce.auto_idle; }
void cat_eyes_force_full(int eye)
{
    if (eye < 0) g_ce.force_full[0] = g_ce.force_full[1] = true;
    else if (eye < 2) g_ce.force_full[eye] = true;
}

void cat_eyes_play(cat_anim_t id)
{
    if ((unsigned)id >= CAT_ANIM_COUNT) return;
    if (id == CAT_ANIM_SPECIAL && !g_ce_styles[g_ce.style].special_name) return;
    ce_start_anim(id);
}
void ce_start_anim(cat_anim_t id)
{
    ce_anim_build(id, &g_ce.anim);
    if (CE_IS_FX(id)) ce_fx_begin(id);
    g_ce.anim_on = true; g_ce.anim_start = g_ce.now; g_ce.last_anim = id;
}
void cat_eyes_blink(void) { g_ce.blink_start = g_ce.now; }
void cat_eyes_look(float gx, float gy, uint32_t hold_ms)
{
    g_ce.tgx = gx; g_ce.tgy = gy; g_ce.look_until = g_ce.now + hold_ms; g_ce.next_sacc = g_ce.look_until;
}

static int find(const char *const *tab, int n, const char *s)
{
    if (!s) return -1;
    for (int i = 0; i < n; i++) if (tab[i] && strcmp(tab[i], s) == 0) return i;
    return -1;
}
int cat_eyes_style_from_name(const char *n)
{
    for (int i = 0; i < CAT_STYLE_COUNT; i++) if (n && strcmp(g_ce_styles[i].name, n) == 0) return i;
    return -1;
}
int cat_eyes_iris_from_name(const char *n) { return find(k_iris_names, CAT_IRIS_COUNT, n); }
int cat_eyes_anim_from_name(const char *n)
{
    int i = find(k_anim_names, CAT_ANIM_COUNT, n);
    if (i < 0) for (int s = 0; s < CAT_STYLE_COUNT; s++)
        if (n && g_ce_styles[s].special_name && strcmp(g_ce_styles[s].special_name, n) == 0) return CAT_ANIM_SPECIAL;
    return i;
}
const char *cat_eyes_anim_name(cat_anim_t a)
{
    if (a == CE_ANIM_EFX) return ce_efx_name();
    if ((unsigned)a >= CAT_ANIM_COUNT) return "idle";
    if (a == CAT_ANIM_SPECIAL && g_ce_styles[g_ce.style].special_name) return g_ce_styles[g_ce.style].special_name;
    return k_anim_names[a];
}
cat_anim_t cat_eyes_current_anim(void) { return g_ce.anim_on ? g_ce.anim.id : CAT_ANIM_NONE; }

/* ---------------- scheduler ---------------- */
static cat_anim_t pick(void)
{
    float tot = 0; bool sp = g_ce_styles[g_ce.style].special_name != NULL;
    for (int i = 0; i < CAT_ANIM_COUNT; i++)
        if (i != g_ce.last_anim && (i != CAT_ANIM_SPECIAL || sp)) tot += k_weights[i] * affinity(i, g_ce.style);
    float r = ce_randf(0, tot);
    for (int i = 0; i < CAT_ANIM_COUNT; i++) {
        if (i == g_ce.last_anim || (i == CAT_ANIM_SPECIAL && !sp)) continue;
        r -= k_weights[i] * affinity(i, g_ce.style);
        if (r <= 0) return (cat_anim_t)i;
    }
    return CAT_ANIM_GLANCE;
}

static bool anim_drives(int eye, int param)
{
    if (!g_ce.anim_on) return false;
    const ce_tracks_t *t = &g_ce.anim.eye[eye];
    for (int i = 0; i < t->n; i++) if (t->tr[i].param == param) return true;
    return false;
}

static uint32_t hash_frame(const ce_frame_t *f)
{
    /* params quantised so sub-pixel drift doesn't force a push */
    uint32_t h = 2166136261u;
    for (int i = 0; i < P_COUNT; i++) { int32_t q = (int32_t)lrintf(f->p[i] * 400.f); h = (h ^ (uint32_t)q) * 16777619u; }
    return h;
}

void cat_eyes_tick(uint32_t now)
{
    if (!g_ce.inited) cat_eyes_init(1);
    if (g_ce.frame_no == 0) {
        g_ce.last_now = now; g_ce.next_anim = now + 2000; g_ce.next_blink = now + 1200;
        g_ce.next_sacc = now + 800; g_ce.blink_start = now - 100000;
    }
    uint32_t dt = now - g_ce.last_now; if (dt > 100) dt = 100;
    g_ce.now = now; g_ce.last_now = now; g_ce.frame_no++;
    const ce_style_t *st = &g_ce_styles[g_ce.style];
    float frames = dt / 16.67f;

    if (g_ce.anim_on && now - g_ce.anim_start > g_ce.anim.dur) {
        g_ce.anim_on = false; g_ce.next_anim = now + (uint32_t)ce_randf(2500, 7000);
    }
    if (g_ce.auto_idle && !g_ce.anim_on && (int32_t)(now - g_ce.next_anim) > 0) cat_eyes_play(pick());
    const bool fx = g_ce.anim_on && CE_IS_FX(g_ce.anim.id);
    if ((int32_t)(now - g_ce.next_blink) > 0) {
        if (!anim_drives(0, P_UPPER) && !fx) g_ce.blink_start = now;
        g_ce.next_blink = now + ((ce_rand() % 100) < 15 ? 350 : (uint32_t)ce_randf(2500, 7000));
    }
    if ((int32_t)(now - g_ce.next_sacc) > 0) {
        g_ce.tgx = ce_randf(-.35f, .35f); g_ce.tgy = ce_randf(-.25f, .25f);
        g_ce.next_sacc = now + (uint32_t)ce_randf(700, 2800);
    }
    float kg = 1.f - powf(.7f, frames);
    g_ce.gx += (g_ce.tgx - g_ce.gx) * kg; g_ce.gy += (g_ce.tgy - g_ce.gy) * kg;
    float kp = 1.f - powf(.82f, frames);

    for (int e = 0; e < 2; e++) {
        float p[P_COUNT];
        memcpy(p, k_base, sizeof(p));
        p[P_GX] = g_ce.gx; p[P_GY] = g_ce.gy;
        uint32_t t = now - g_ce.anim_start;
        if (g_ce.anim_on) {
            const ce_tracks_t *tr = &g_ce.anim.eye[e];
            for (int i = 0; i < tr->n; i++) p[tr->tr[i].param] = ce_track_eval(&tr->tr[i], t);
            if (g_ce.anim.jitter) p[P_GX] += sinf(t / 37.f) * .03f;
            if (fx) ce_fx_params(g_ce.anim.id, e, (float)t, p);
        }
        /* style pupil remap */
        float tg = p[P_PUPIL];
        bool wide_ok = g_ce.anim_on && (g_ce.anim.id == CAT_ANIM_STARTLE || g_ce.anim.id == CAT_ANIM_HUNT);
        if (st->pupil_cap < 1.f && !wide_ok) tg = fminf(tg, st->pupil_cap) * st->pupil_cap;
        tg = st->pupil_min + (1.f - st->pupil_min) * tg;
        g_ce.pupil_s[e] += (tg + sinf(now / 1700.f) * .02f - g_ce.pupil_s[e]) * kp;
        p[P_PUPIL] = g_ce.pupil_s[e];
        p[P_TILT] += st->tilt_bias; p[P_UPPER] += st->upper_bias;
        p[P_UPPER] += p[P_GY] * .12f; /* lids follow gaze */
        float bv = blink_curve((int32_t)(now - g_ce.blink_start) - e * 16);
        if (bv > p[P_UPPER]) p[P_UPPER] = bv;
        if (bv * .25f > p[P_LOWER]) p[P_LOWER] = bv * .25f;
        ce_build_frame(e, p, now);
        if (fx) ce_fx_draw(g_ce.anim.id, e, (float)t, &g_ce.f[e]);
    }

    /* background particles */
    for (int e = 0; e < 2; e++)
        for (int i = 0; i < CE_SNOW; i++) {
            g_ce.snow[e][i].y += g_ce.snow[e][i].v * frames;
            g_ce.snow[e][i].x += sinf(now / 900.f + g_ce.snow[e][i].r) * .3f * frames;
            if (g_ce.snow[e][i].y > 244) { g_ce.snow[e][i].y = -4; g_ce.snow[e][i].x = ce_randf(0, 240); }
        }
}

bool cat_eyes_dirty_rows(int eye, int *y0, int *y1)
{
    if ((unsigned)eye > 1) return false;
    const ce_frame_t *f = &g_ce.f[eye];
    const ce_style_t *st = f->st;
    uint32_t h = hash_frame(f);
    const bool fx_on = f->n_fx > 0, tint_on = f->tint_shift != 0;
    /* a whole-eye tint (or its removal) touches every row */
    bool full = g_ce.force_full[eye] || st->snow || st->moon_bats || st->iris == IRIS_FIRE || tint_on || g_ce.prev_tint[eye];
    if (!full && !fx_on && !g_ce.prev_fx[eye] && h == g_ce.prev_hash[eye]) return false;
    int lo = f->y_lo, hi = f->y_hi;
    if (full) { lo = 0; hi = CAT_EYES_H; }
    else {
        if (g_ce.prev_lo[eye] < lo) lo = g_ce.prev_lo[eye];
        if (g_ce.prev_hi[eye] > hi) hi = g_ce.prev_hi[eye];
        /* effect shapes move: redraw where they are now and where they were */
        if (fx_on) { if (f->fx_lo < lo) lo = f->fx_lo; if (f->fx_hi > hi) hi = f->fx_hi; }
        if (g_ce.prev_fx[eye]) { if (g_ce.prev_fx_lo[eye] < lo) lo = g_ce.prev_fx_lo[eye]; if (g_ce.prev_fx_hi[eye] > hi) hi = g_ce.prev_fx_hi[eye]; }
    }
    g_ce.prev_hash[eye] = h; g_ce.prev_lo[eye] = f->y_lo; g_ce.prev_hi[eye] = f->y_hi;
    g_ce.prev_fx[eye] = fx_on; g_ce.prev_fx_lo[eye] = f->fx_lo; g_ce.prev_fx_hi[eye] = f->fx_hi;
    g_ce.prev_tint[eye] = tint_on;
    g_ce.force_full[eye] = false;
    *y0 = lo < 0 ? 0 : lo; *y1 = hi > CAT_EYES_H ? CAT_EYES_H : hi;
    return *y1 > *y0;
}
