#ifndef _KNOC_MATH_H
#define _KNOC_MATH_H

#define M_PI 3.14159265358979323846
#define M_E 2.71828182845904523536
#if defined __GNUC__ && !defined __TINYC__
#define HUGE_VAL __builtin_huge_val()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#else
#define HUGE_VAL (1.0 / 0.0)
#define INFINITY (1.0f / 0.0f)
#define NAN (0.0f / 0.0f)
#define isnan(x) ((x) != (x))
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#endif

double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
double ldexp(double x, int exponent);
long double ldexpl(long double x, int exponent);
double frexp(double x, int *exponent);
double modf(double x, double *whole);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
long lround(double x);
long lroundf(float x);
float expf(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float logf(float x);
float fmodf(float x, float y);

#endif
