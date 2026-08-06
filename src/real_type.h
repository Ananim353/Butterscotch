#ifndef _BS_REAL_TYPE_H_
#define _BS_REAL_TYPE_H_

#include "common.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef USE_FLOAT_REALS

typedef float GMLReal;

// Trig call counter, built only with BS_TRIG_COUNT. Every trig user funnels through these
// macros — GML's sin/cos, lengthdir_x/y, point_direction, darctan2, and the rotated-box paths
// in collision.h — so one increment here counts everything, and the total lands in PERF as
// "trig=" per window.
//
// Measured 2026-07-30 on console: 2-3 calls per frame outdoors (240 per 2s window in
// room_town_north, mostly draw_shadowcast's lengthdir_x), ZERO indoors, peak ~13. That is why
// the VFPU's hardware vsin/vcos is NOT wired in: it would save thousandths of a millisecond,
// despite 1140 textual uses of sin() in the chapter's GML. Kept so the same question can be
// re-answered in a scene we have not profiled yet (battles, particle-heavy cutscenes) without
// rebuilding the instrumentation from scratch.
#if defined(__PSP__) && defined(BS_TRIG_COUNT)
extern unsigned int g_trigCalls;
static inline float bsTrigSin(float x) { g_trigCalls++; return sinf(x); }
static inline float bsTrigCos(float x) { g_trigCalls++; return cosf(x); }
#define GMLReal_sin bsTrigSin
#define GMLReal_cos bsTrigCos
#else
#define GMLReal_sin sinf
#define GMLReal_cos cosf
#endif
#define GMLReal_tan tanf
#define GMLReal_acos acosf
#define GMLReal_asin asinf
#define GMLReal_atan atanf
#define GMLReal_atan2 atan2f
#define GMLReal_sqrt sqrtf
#define GMLReal_fabs fabsf
#define GMLReal_fmod fmodf
#define GMLReal_floor floorf
#define GMLReal_ceil ceilf
#define GMLReal_round roundf
#define GMLReal_pow powf
#define GMLReal_log2 log2f
#define GMLReal_fmax fmaxf
#define GMLReal_fmin fminf
#define GMLReal_nextafter nextafterf
#define GMLReal_strtod(str, endptr) strtof(str, endptr)

#else

typedef double GMLReal;

#define GMLReal_sin sin
#define GMLReal_cos cos
#define GMLReal_tan tan
#define GMLReal_acos acos
#define GMLReal_asin asin
#define GMLReal_atan atan
#define GMLReal_atan2 atan2
#define GMLReal_sqrt sqrt
#define GMLReal_fabs fabs
#define GMLReal_fmod fmod
#define GMLReal_floor floor
#define GMLReal_ceil ceil
#define GMLReal_round round
#define GMLReal_pow pow
#define GMLReal_log2 log2
#define GMLReal_fmax fmax
#define GMLReal_fmin fmin
#define GMLReal_nextafter nextafter
#define GMLReal_strtod(str, endptr) strtod(str, endptr)

#endif

// Round-half-to-even (banker's rounding).
// While the original runner uses "llrint(double)", we use our own banker's rounding implementation to avoid quirks in specific platforms (like the PlayStation 2) having different llrint rounding implementations.
static inline GMLReal GMLReal_bankersRound(GMLReal v) {
    if (isnan(v) || isinf(v)) return v;
    GMLReal f = GMLReal_floor(v);
    GMLReal frac = v - f;
    if (0.5 > frac) return f;
    if (frac > 0.5) return f + 1.0;
    // Exactly halfway: round to the even neighbor.
    int64_t fi = (int64_t) f;
    return (fi & 1) == 0 ? f : f + 1.0;
}

#endif /* _BS_REAL_TYPE_H_ */
