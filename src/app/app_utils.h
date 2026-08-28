#ifndef APP_UTILS_H
#define APP_UTILS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif



//===========================================================
// Definition
//===========================================================

// ---------- small utils ----------
//#define COMPILEASSERT(exp)        extern int _CompileAssert[(exp)?1:-1]
#define COMPILEASSERT(exp)        extern int _CompileAssert[(exp)?1:-1] __attribute__((unused))
#define ARRAY_SIZE(arr)           (sizeof(arr) / sizeof((arr)[0]))

#if !defined(isnanf)
  #define isnanf(x)               isnan((float)(x))
#endif //!defined(isnanf)

#if !defined(M_PI)
#define M_PI 3.14159265358979323846
#endif //!defined(M_PI)


static inline float clampf(float x, float lo, float hi)
{
    return (x<lo)?lo:((x>hi)?hi:x);
}
static inline float lerp(float cur, float tgt, float g)
{
    return cur + g * (tgt - cur);
}
//static inline float db_to_lin(float db)
//{
//    return powf(10.0f, db / 20.0f);
//}
/* 10^(dB/20) as e^(dB * ln10/20). Same value -- and one base is not the general
 * case of two: powf() is x^y for any x and y and costs 1,876 bytes of libm on
 * its own, while expf() (712 bytes, and it needs scalbnf which powf needs too) is
 * already linked because every one-pole smoother in the DSP calls it. This macro
 * was the only reason powf() was in a Classic image at all: it is expanded in the
 * gain setup of several modules, so one of them always pulled it in.
 * M_LN10 is folded at compile time -- there is no runtime multiply added. */
#if !defined(M_LN10)
#define M_LN10 2.30258509299404568402
#endif //!defined(M_LN10)
#define db_to_lin(val_db)         (expf((val_db) * (float)(M_LN10 / 20.0)))






//===========================================================
// Enum & Struct typedef
//===========================================================

typedef struct
{
    float b0, b1, b2, a1, a2;

} biquad_t;

typedef struct
{
    float z1;
    float z2;

} biquad_stat_t;


/* Added: common structure for biquad (DF2T) coefficients and states */
typedef struct
{
    biquad_t      bq;
    biquad_stat_t bqs;  // single channel state only (mono)

} biquad_mono_t;


//===========================================================
// Function Prototype
//===========================================================

void *app_memset(void *dst, int value, size_t size);
void *app_memcpy(void *dst, const void *src, size_t size);
void *app_memmove(void *dst, const void *src, size_t size);
int   app_memcmp(const void *s1, const void *s2, size_t size);
void *app_memchr(const void *s, int value, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* APP_UTILS_H */
