/*
 * Custom effects (".efx" files): data-driven overlays uploaded to the device.
 * Format: docs/efx-format.md.  A file is a few shapes with keyframe tracks
 * (position, size, colour) plus an optional gaze/pupil/lid track for the eyes.
 * Shapes are drawn into the same CE_L_FX list as the built-in effects, so a
 * custom effect costs no more than e.g. "pumpkin".  No heap, no per-pixel float.
 *
 * Layout (little endian)
 *   header 16: "EFX1", ver=1, flags, u16 duration_ms, u8 n_shapes, u8 n_gaze,
 *              u16 payload_len, u16 crc16(payload, CCITT-FALSE), u16 reserved=0
 *   payload  : n_gaze x gaze key {u16 t, i8 gx, i8 gy, u8 pupil, u8 lid}
 *              n_shapes x {u8 kind, u8 mask, u8 flags, u8 aux, u8 n_keys,
 *                          n_keys x key {u16 t, i16 x, i16 y, i16 size, u16 rgb565}}
 */
#include "cat_eyes_internal.h"
#include <math.h>
#include <string.h>

#define HDR 16
#define GAZE_SZ 6
#define SHAPE_HDR 5
#define KEY_SZ 10
#define FX_OFF (CAT_EYES_W + 56)
#define FX_WORLD (2 * CAT_EYES_W + 56)

static const uint8_t *s_data;
static size_t s_len;
static char s_name[32];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static const char *const k_err[] = {
    "ok", "not an .efx file (bad header)", "unsupported efx version", "file length does not match header",
    "checksum mismatch", "bad shape or gaze count", "bad gaze key", "bad shape field",
    "key times must increase", "value out of range",
};

const char *cat_eyes_efx_error(int code)
{
    return (code >= 0 && code < (int)(sizeof k_err / sizeof k_err[0])) ? k_err[code] : "unknown error";
}

int cat_eyes_efx_check(const uint8_t *d, size_t len)
{
    if (!d || len < HDR || memcmp(d, "EFX1", 4) != 0) return 1;
    if (len > CAT_EFX_MAX_BYTES) return 3;
    if (d[4] != 1) return 2;
    const uint16_t dur = rd16(d + 6), plen = rd16(d + 10);
    const int ns = d[8], ng = d[9];
    if (len != (size_t)HDR + plen) return 3;
    if (crc16(d + HDR, plen) != rd16(d + 12)) return 4;
    if (dur < 200 || dur > 30000 || ns < 1 || ns > CAT_EFX_MAX_SHAPES || ng > CAT_EFX_MAX_GAZE) return 5;
    const uint8_t *p = d + HDR, *end = d + len;
    int last = -1;
    for (int i = 0; i < ng; i++) {
        if (end - p < GAZE_SZ) return 3;
        const int t = rd16(p);
        if (t <= last) return 8;
        last = t;
        p += GAZE_SZ;
    }
    for (int i = 0; i < ns; i++) {
        if (end - p < SHAPE_HDR) return 3;
        const int kind = p[0], mask = p[1], aux = p[3], nk = p[4];
        if (kind > 8 || (mask & 3) == 0 || (mask & ~7) || (p[2] >> 4) > 5) return 7;
        if (kind == 6 && aux >= CE_GLYPH_COUNT) return 7;
        if (nk < 1 || nk > CAT_EFX_MAX_KEYS) return 5;
        p += SHAPE_HDR;
        if (end - p < nk * KEY_SZ) return 3;
        int lt = -1;
        for (int k = 0; k < nk; k++, p += KEY_SZ) {
            const int t = rd16(p), x = rds16(p + 2), y = rds16(p + 4), s = rds16(p + 6);
            if (t <= lt) return 8;
            lt = t;
            if (x < -1000 || x > 1000 || y < -1000 || y > 1000 || s < 0 || s > 400) return 9;
        }
    }
    return p == end ? 0 : 3;
}

uint16_t ce_efx_duration(void) { return s_data ? rd16(s_data + 6) : 1; }

/* ---------------- evaluation ---------------- */
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float ease(int mode, float u)
{
    u = clampf(u, 0.f, 1.f);
    switch (mode) {
    case 1: return u * u * (3.f - 2.f * u);
    case 2: return u * u;
    case 3: return 1.f - (1.f - u) * (1.f - u);
    case 4: { const float c1 = 1.70158f, c3 = c1 + 1.f, v = u - 1.f; return 1.f + c3 * v * v * v + c1 * v * v; }
    case 5: return u >= 1.f ? 1.f : 0.f;
    default: return u;
    }
}
static uint16_t mixc(uint16_t a, uint16_t b, float u)
{
    const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)((((int)lrintf(ar + (br - ar) * u)) << 11) | (((int)lrintf(ag + (bg - ag) * u)) << 5) | (int)lrintf(ab + (bb - ab) * u));
}

/* interpolated state of one shape at time t; false = not visible */
typedef struct { float x, y, s; uint16_t c; } st_t;
static bool eval_shape(const uint8_t *keys, int nk, int flags, float t, st_t *o)
{
    const float t0 = rd16(keys), tl = rd16(keys + (nk - 1) * KEY_SZ);
    if (t < t0) return false;
    if (t > tl) {
        if (flags & 1) { const float span = tl - t0; t = span > 0.f ? t0 + fmodf(t - t0, span) : t0; }
        else if (!(flags & 2)) return false;
        else t = tl;
    }
    int i = 0;
    while (i < nk - 2 && t >= rd16(keys + (i + 1) * KEY_SZ)) i++;
    const uint8_t *a = keys + i * KEY_SZ, *b = nk > 1 ? a + KEY_SZ : a;
    float u = 0.f;
    if (nk > 1) {
        const float ta = rd16(a), tb = rd16(b);
        u = ease(flags >> 4, (t - ta) / (tb - ta));
    }
    o->x = rds16(a + 2) + (rds16(b + 2) - rds16(a + 2)) * u;
    o->y = rds16(a + 4) + (rds16(b + 4) - rds16(a + 4)) * u;
    o->s = rds16(a + 6) + (rds16(b + 6) - rds16(a + 6)) * u;
    o->c = mixc(rd16(a + 8), rd16(b + 8), clampf(u, 0.f, 1.f));
    return true;
}

static const uint8_t *shapes_start(const uint8_t *d) { return d + HDR + d[9] * GAZE_SZ; }

void ce_efx_params(float t, float *p)
{
    if (!s_data || s_data[9] == 0) return;
    const int ng = s_data[9];
    const uint8_t *g = s_data + HDR;
    int i = 0;
    while (i < ng - 1 && t >= rd16(g + (i + 1) * GAZE_SZ)) i++;
    const uint8_t *a = g + i * GAZE_SZ, *b = ng > 1 ? a + GAZE_SZ : a;
    float u = 0.f;
    if (ng > 1 && t > rd16(a)) u = ease(1, (t - rd16(a)) / (float)(rd16(b) - rd16(a)));
    if (t <= rd16(g)) u = 0.f;
    if (t >= rd16(g + (ng - 1) * GAZE_SZ)) { a = b = g + (ng - 1) * GAZE_SZ; u = 0.f; }
#define LERP(fa, fb) ((fa) + ((fb) - (fa)) * u)
    p[P_GX] = LERP((int8_t)a[2] / 127.f, (int8_t)b[2] / 127.f);
    p[P_GY] = LERP((int8_t)a[3] / 127.f, (int8_t)b[3] / 127.f);
    const float pa = a[4] ? a[4] / 255.f : .22f, pb = b[4] ? b[4] / 255.f : .22f;
    const float la = a[5] ? (a[5] - 1) / 254.f : .12f, lb = b[5] ? (b[5] - 1) / 254.f : .12f;
    p[P_PUPIL] = LERP(pa, pb);
    p[P_UPPER] = LERP(la, lb);
#undef LERP
}

#define R(v) ((int)lrintf(v))
void ce_efx_draw(int eye, float t, ce_frame_t *f)
{
    if (!s_data) return;
    const int ns = s_data[8], world_flip = (s_data[5] & 1) && g_ce.fxs.dir < 0;
    const uint8_t *p = shapes_start(s_data);
    for (int i = 0; i < ns; i++) {
        const int kind = p[0], mask = p[1], flags = p[2], aux = p[3], nk = p[4];
        const uint8_t *keys = p + SHAPE_HDR;
        p = keys + nk * KEY_SZ;
        if (!((mask >> eye) & 1)) continue;
        st_t s;
        if (!eval_shape(keys, nk, flags, t, &s) || s.s < 1.f) continue;
        float x = s.x;
        if (mask & 4) { if (world_flip) x = FX_WORLD - x; if (eye) x -= FX_OFF; }
        const float y = s.y, r = s.s;
        const uint16_t c = s.c;
        const int L = CE_L_FX;
        const float ang = aux * (6.2831853f / 256.f);
        switch (kind) {
        case 0: ce_sh_circle(f, L, R(x), R(y), R(r), c); break;
        case 1: ce_sh_ellipse(f, L, R(x), R(y), R(r), R(r * aux / 32.f), c); break;
        case 2: ce_sh_rect(f, L, R(x - r), R(y - r * aux / 32.f), R(x + r), R(y + r * aux / 32.f), c); break;
        case 3: ce_sh_ring(f, L, R(x), R(y), R(r), R(r * aux / 255.f), c); break;
        case 4:
            ce_sh_circle(f, L, R(x - r * .5f), R(y - r * .25f), R(r * .5f + .5f), c);
            ce_sh_circle(f, L, R(x + r * .5f), R(y - r * .25f), R(r * .5f + .5f), c);
            ce_sh_tri(f, L, R(x - r * .97f), R(y - r * .1f), R(x + r * .97f), R(y - r * .1f), R(x), R(y + r * .95f), c);
            break;
        case 5:
            ce_sh_capsule(f, L, R(x), R(y - r), R(x), R(y + r), 1, c);
            ce_sh_capsule(f, L, R(x - r), R(y), R(x + r), R(y), 1, c);
            if (r > 7.f) ce_sh_circle(f, L, R(x), R(y), R(r * .28f), c);
            break;
        case 6: ce_sh_glyph(f, L, R(x - 2.5f * R(r)), R(y - 3.5f * R(r)), R(r), aux, c); break;
        case 7: ce_sh_capsule(f, L, R(x - cosf(ang) * r), R(y - sinf(ang) * r), R(x + cosf(ang) * r), R(y + sinf(ang) * r), 2, c); break;
        default: {
            float vx[3], vy[3];
            for (int k = 0; k < 3; k++) {
                const float a = ang + k * 2.0943951f - 1.5707963f;
                vx[k] = x + cosf(a) * r; vy[k] = y + sinf(a) * r;
            }
            ce_sh_tri(f, L, R(vx[0]), R(vy[0]), R(vx[1]), R(vy[1]), R(vx[2]), R(vy[2]), c);
        }
        }
    }
}

bool cat_eyes_efx_play(const char *name, const uint8_t *data, size_t len)
{
    if (cat_eyes_efx_check(data, len) != 0) return false;
    s_data = data; s_len = len;
    strncpy(s_name, name ? name : "custom", sizeof s_name - 1);
    s_name[sizeof s_name - 1] = 0;
    ce_start_anim(CE_ANIM_EFX);
    return true;
}

const char *ce_efx_name(void) { return s_name; }
