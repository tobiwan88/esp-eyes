/* Keyframe animations (port of the browser prototype). */
#include "cat_eyes_internal.h"
#include <math.h>
#include <string.h>

/* add a track: pairs = {t0,v0, t1,v1, ...} */
static void trk(ce_tracks_t *e, int param, int n, int linear, const float *pairs)
{
    if (e->n >= CE_MAX_TRACKS || n > CE_MAX_KEYS) return;
    ce_track_t *t = &e->tr[e->n++];
    t->param = (uint8_t)param; t->n = (uint8_t)n; t->linear = (uint8_t)linear;
    for (int i = 0; i < n; i++) { t->k[i].t = (uint16_t)pairs[2 * i]; t->k[i].v = pairs[2 * i + 1]; }
}
#define K(e, p, ...) do { const float _a[] = {__VA_ARGS__}; \
    trk((e), (p), (int)(sizeof(_a) / sizeof(float) / 2), 0, _a); } while (0)

float ce_track_eval(const ce_track_t *t, uint32_t ms)
{
    if (ms <= t->k[0].t) return t->k[0].v;
    for (int i = 1; i < t->n; i++) {
        if (ms <= t->k[i].t) {
            float u = (float)(ms - t->k[i - 1].t) / (float)(t->k[i].t - t->k[i - 1].t);
            if (!t->linear) u = u * u * (3.f - 2.f * u);
            return t->k[i - 1].v + (t->k[i].v - t->k[i - 1].v) * u;
        }
    }
    return t->k[t->n - 1].v;
}

static float sgn(void) { return (ce_rand() & 1) ? 1.f : -1.f; }

void ce_anim_build(cat_anim_t id, ce_anim_t *a)
{
    memset(a, 0, sizeof(*a));
    a->id = id;
    ce_tracks_t *e0 = &a->eye[0], *e1 = &a->eye[1];
    bool copy = true; /* both eyes share eye[0] tracks */

    switch (id) {
    case CAT_ANIM_WINK: {
        ce_tracks_t *w = (ce_rand() & 1) ? e1 : e0, *o = (w == e0) ? e1 : e0;
        K(w, P_UPPER, 0, .12f, 110, 1, 480, 1, 700, .12f);
        K(w, P_LOWER, 0, .06f, 110, .4f, 480, .4f, 700, .06f);
        K(o, P_UPPER, 0, .12f, 150, .22f, 480, .22f, 700, .12f);
        K(o, P_LOWER, 0, .06f, 150, .22f, 480, .22f, 700, .06f);
        a->dur = 750; copy = false; break;
    }
    case CAT_ANIM_SLOW_BLINK:
        K(e0, P_UPPER, 0, .12f, 700, .88f, 1500, .88f, 2300, .12f);
        K(e0, P_LOWER, 0, .06f, 700, .3f, 1500, .3f, 2300, .06f);
        K(e0, P_PUPIL, 0, .22f, 700, .38f, 2300, .25f);
        a->dur = 2400; break;
    case CAT_ANIM_ROLL: {
        /* gaze sweeps from left over the top to the right, then returns */
        float gxk[26], gyk[26];
        for (int i = 0; i <= 10; i++) {
            float u = i / 10.f; u = u * u * (3 - 2 * u);
            float ang = 3.14159f + .3f - u * (3.14159f + .6f);
            gxk[2 * i] = gyk[2 * i] = i * 110.f;
            gxk[2 * i + 1] = cosf(ang) * .85f;
            gyk[2 * i + 1] = -sinf(ang) * .75f;
        }
        gxk[22] = gyk[22] = 1300; gxk[23] = gxk[21] * .3f; gyk[23] = gyk[21] * .3f;
        gxk[24] = gyk[24] = 1500; gxk[25] = gyk[25] = 0;
        trk(e0, P_GX, 13, 1, gxk); trk(e0, P_GY, 13, 1, gyk);
        K(e0, P_UPPER, 0, .12f, 300, .25f, 1100, .3f, 1500, .5f, 2200, .5f, 2600, .12f);
        K(e0, P_LOWER, 0, .06f, 1500, .2f, 2200, .2f, 2600, .06f);
        a->dur = 2700; break;
    }
    case CAT_ANIM_SIDE_EYE: {
        float d = sgn() * .85f;
        for (int i = 0; i < 2; i++) {
            ce_tracks_t *e = &a->eye[i]; float s = i ? .08f : 0;
            K(e, P_GX, 0, 0, 120, d, 1800, d, 1950, 0);
            K(e, P_UPPER, 0, .12f, 250, .45f + s, 1700, .45f + s, 2000, .12f);
            K(e, P_LOWER, 0, .06f, 250, .26f + s / 2, 1700, .26f + s / 2, 2000, .06f);
            K(e, P_TILT, 0, 0, 250, .35f, 1700, .35f, 2000, 0);
        }
        a->dur = 2100; copy = false; break;
    }
    case CAT_ANIM_HUNT: {
        float x1 = ce_randf(-.7f, .7f), y1 = ce_randf(-.4f, .4f);
        float x2 = ce_randf(-.7f, .7f), y2 = ce_randf(-.4f, .4f);
        K(e0, P_PUPIL, 0, .22f, 250, .95f, 2400, .9f, 2900, .22f);
        K(e0, P_GX, 0, 0, 90, x1, 1150, x1, 1230, x2, 2500, x2, 2800, 0);
        K(e0, P_GY, 0, 0, 90, y1, 1150, y1, 1230, y2, 2500, y2, 2800, 0);
        K(e0, P_UPPER, 0, .12f, 200, .02f, 2500, .02f, 2800, .12f);
        K(e0, P_LOWER, 0, .06f, 200, .14f, 2500, .14f, 2800, .06f);
        a->jitter = true; a->dur = 3000; break;
    }
    case CAT_ANIM_STARTLE: {
        float x = ce_randf(-.6f, .6f), y = ce_randf(-.4f, .2f);
        K(e0, P_PUPIL, 0, .22f, 80, 1, 900, .95f, 1600, .22f);
        K(e0, P_UPPER, 0, .12f, 60, 0, 900, 0, 1300, .12f);
        K(e0, P_LOWER, 0, .06f, 60, 0, 1300, .06f);
        K(e0, P_GX, 0, 0, 60, x, 1100, x, 1400, 0);
        K(e0, P_GY, 0, 0, 60, y, 1100, y, 1400, 0);
        a->dur = 1700; break;
    }
    case CAT_ANIM_NICTITATE: {
        int mode = (int)(ce_rand() % 10); /* 6/10 both, else one eye */
        if (mode < 6) K(e0, P_NICT, 0, 0, 160, .75f, 260, .75f, 520, 0);
        else { K(&a->eye[mode & 1], P_NICT, 0, 0, 160, .75f, 260, .75f, 520, 0); copy = false; }
        a->dur = 560; break;
    }
    case CAT_ANIM_SLEEPY:
        K(e0, P_UPPER, 0, .12f, 1500, .7f, 2200, .66f, 3200, .93f, 3500, .93f, 3580, 0, 3900, .12f);
        K(e0, P_LOWER, 0, .06f, 1500, .25f, 3500, .3f, 3580, 0, 3900, .06f);
        K(e0, P_NICT, 0, 0, 2400, 0, 3000, .4f, 3500, .4f, 3580, 0);
        K(e0, P_GY, 0, 0, 1500, .3f, 3500, .4f, 3580, -.1f, 3900, 0);
        K(e0, P_PUPIL, 0, .22f, 3500, .28f, 3600, .65f, 4200, .22f);
        a->dur = 4300; break;
    case CAT_ANIM_CROSS_EYED:
        for (int i = 0; i < 2; i++) {
            float d = i ? -.65f : .65f; ce_tracks_t *e = &a->eye[i];
            K(e, P_GX, 0, 0, 400, d, 1600, d, 1900, 0);
            K(e, P_GY, 0, 0, 400, .45f, 1600, .45f, 1900, 0);
            K(e, P_PUPIL, 0, .22f, 400, .12f, 1600, .12f, 1900, .22f);
        }
        a->dur = 2000; copy = false; break;
    case CAT_ANIM_HAPPY:
        K(e0, P_UPPER, 0, .12f, 300, .38f, 1500, .38f, 1800, .12f);
        K(e0, P_LOWER, 0, .06f, 300, .52f, 1500, .52f, 1800, .06f);
        K(e0, P_PUPIL, 0, .22f, 300, .3f, 1800, .22f);
        K(e0, P_SMILE, 0, 0, 250, 1, 1500, 1, 1700, 0);
        a->dur = 1900; break;
    case CAT_ANIM_GLANCE: {
        float d = sgn() * .8f, y = ce_randf(-.2f, .2f);
        K(e0, P_GX, 0, 0, 70, d, 600, d, 680, 0);
        K(e0, P_GY, 0, 0, 70, y, 600, 0);
        a->dur = 720; break;
    }
    case CAT_ANIM_SPECIAL: {
        cat_style_t s = g_ce.style;
        K(e0, P_FX, 0, 0, 250, 1, 1600, 1, 2000, 0);
        if (s == CAT_STYLE_CUTE) K(e0, P_PUPIL, 0, .22f, 250, .9f, 1600, .9f, 2000, .22f);
        if (s == CAT_STYLE_HALLOWEEN) {
            K(e0, P_PUPIL, 0, .22f, 200, 0, 1600, 0, 2000, .22f);
            K(e0, P_TILT, 0, 0, 200, .6f, 1600, .6f, 2000, 0);
            K(e0, P_UPPER, 0, .12f, 200, .3f, 1600, .3f, 2000, .12f);
        }
        if (s == CAT_STYLE_ANIME) K(e0, P_GY, 0, 0, 250, -.3f, 1600, -.3f, 2000, 0);
        a->dur = 2100; break;
    }
    default:
        a->dur = CE_IS_FX(id) ? ce_fx_duration(id) : 1;
        break;
    }
    if (copy) a->eye[1] = a->eye[0];
}
