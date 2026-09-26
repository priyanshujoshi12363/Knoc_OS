#include <math.h>

#define LN2 0.69314718055994530942
#define PI_2 1.57079632679489661923

double sqrt(double x)
{
    double r;

    asm("fsqrt.d %0, %1" : "=f"(r) : "f"(x));
    return r;
}

float sqrtf(float x)
{
    float r;

    asm("fsqrt.s %0, %1" : "=f"(r) : "f"(x));
    return r;
}

double fabs(double x)
{
    return x < 0 ? -x : x;
}

float fabsf(float x)
{
    return x < 0 ? -x : x;
}

double trunc(double x)
{
    if (x >= 9007199254740992.0 || x <= -9007199254740992.0 || x != x)
    {
        return x;
    }

    return (double)(long long)x;
}

double floor(double x)
{
    double t = trunc(x);

    return t > x ? t - 1 : t;
}

double ceil(double x)
{
    double t = trunc(x);

    return t < x ? t + 1 : t;
}

double round(double x)
{
    return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}

double fmod(double x, double y)
{
    if (y == 0)
    {
        return NAN;
    }

    return x - trunc(x / y) * y;
}

double exp(double x)
{
    if (x > 709)
    {
        return HUGE_VAL;
    }

    if (x < -745)
    {
        return 0;
    }

    double k = floor(x / LN2 + 0.5);
    double r = x - k * LN2;
    double term = 1;
    double sum = 1;

    for (int i = 1; i < 20; i++)
    {
        term *= r / i;
        sum += term;
    }

    long n = (long)k;

    while (n > 0)
    {
        sum *= 2;
        n--;
    }

    while (n < 0)
    {
        sum *= 0.5;
        n++;
    }

    return sum;
}

double log(double x)
{
    if (x < 0 || x != x)
    {
        return NAN;
    }

    if (x == 0)
    {
        return -HUGE_VAL;
    }

    int exponent = 0;

    while (x >= 2)
    {
        x /= 2;
        exponent++;
    }

    while (x < 1)
    {
        x *= 2;
        exponent--;
    }

    double t = (x - 1) / (x + 1);
    double t2 = t * t;
    double sum = 0;
    double power = t;

    for (int k = 1; k < 60; k += 2)
    {
        sum += power / k;
        power *= t2;
    }

    return exponent * LN2 + 2 * sum;
}

double log2(double x)
{
    return log(x) / LN2;
}

double log10(double x)
{
    return log(x) / 2.30258509299404568402;
}

double pow(double x, double y)
{
    if (y == 0)
    {
        return 1;
    }

    if (x == 0)
    {
        return y > 0 ? 0 : HUGE_VAL;
    }

    if (y == trunc(y) && fabs(y) < 1e9)
    {
        long long n = (long long)(y < 0 ? -y : y);
        double result = 1;
        double base = x;

        while (n)
        {
            if (n & 1)
            {
                result *= base;
            }

            base *= base;
            n >>= 1;
        }

        return y < 0 ? 1 / result : result;
    }

    if (x < 0)
    {
        return NAN;
    }

    return exp(y * log(x));
}

static double reduce(double x, int *quadrant)
{
    double k = floor(x / PI_2 + 0.5);

    *quadrant = (int)((long long)k & 3);
    return x - k * PI_2;
}

static double sin_series(double x)
{
    double x2 = x * x;
    double term = x;
    double sum = x;

    for (int i = 1; i < 12; i++)
    {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }

    return sum;
}

static double cos_series(double x)
{
    double x2 = x * x;
    double term = 1;
    double sum = 1;

    for (int i = 1; i < 12; i++)
    {
        term *= -x2 / ((2 * i - 1) * (2 * i));
        sum += term;
    }

    return sum;
}

double sin(double x)
{
    int quadrant;
    double r = reduce(x, &quadrant);

    switch (quadrant)
    {
    case 0:
        return sin_series(r);
    case 1:
        return cos_series(r);
    case 2:
        return -sin_series(r);
    default:
        return -cos_series(r);
    }
}

double cos(double x)
{
    int quadrant;
    double r = reduce(x, &quadrant);

    switch (quadrant)
    {
    case 0:
        return cos_series(r);
    case 1:
        return -sin_series(r);
    case 2:
        return -cos_series(r);
    default:
        return sin_series(r);
    }
}

double tan(double x)
{
    return sin(x) / cos(x);
}

double atan(double x)
{
    int invert = 0;
    int negative = x < 0;

    if (negative)
    {
        x = -x;
    }

    if (x > 1)
    {
        x = 1 / x;
        invert = 1;
    }

    double reduced = x / (1 + sqrt(1 + x * x));
    double r2 = reduced * reduced;
    double term = reduced;
    double sum = reduced;

    for (int k = 1; k < 40; k++)
    {
        term *= -r2;
        sum += term / (2 * k + 1);
    }

    sum *= 2;

    if (invert)
    {
        sum = PI_2 - sum;
    }

    return negative ? -sum : sum;
}

double atan2(double y, double x)
{
    if (x > 0)
    {
        return atan(y / x);
    }

    if (x < 0)
    {
        return y >= 0 ? atan(y / x) + M_PI : atan(y / x) - M_PI;
    }

    return y > 0 ? PI_2 : y < 0 ? -PI_2 : 0;
}

double asin(double x)
{
    if (x > 1 || x < -1)
    {
        return NAN;
    }

    return atan2(x, sqrt(1 - x * x));
}

double acos(double x)
{
    if (x > 1 || x < -1)
    {
        return NAN;
    }

    return atan2(sqrt(1 - x * x), x);
}

double sinh(double x)
{
    return (exp(x) - exp(-x)) / 2;
}

double cosh(double x)
{
    return (exp(x) + exp(-x)) / 2;
}

double tanh(double x)
{
    if (x > 20)
    {
        return 1;
    }

    if (x < -20)
    {
        return -1;
    }

    double e = exp(2 * x);

    return (e - 1) / (e + 1);
}

double ldexp(double x, int exponent)
{
    while (exponent > 0)
    {
        x *= 2;
        exponent--;
    }

    while (exponent < 0)
    {
        x *= 0.5;
        exponent++;
    }

    return x;
}

long double ldexpl(long double x, int exponent)
{
    while (exponent > 0)
    {
        x *= 2;
        exponent--;
    }

    while (exponent < 0)
    {
        x *= 0.5;
        exponent++;
    }

    return x;
}

double frexp(double x, int *exponent)
{
    int e = 0;

    if (x == 0 || x != x || isinf(x))
    {
        *exponent = 0;
        return x;
    }

    double magnitude = x < 0 ? -x : x;

    while (magnitude >= 1)
    {
        magnitude *= 0.5;
        e++;
    }

    while (magnitude < 0.5)
    {
        magnitude *= 2;
        e--;
    }

    *exponent = e;
    return x < 0 ? -magnitude : magnitude;
}

double modf(double x, double *whole)
{
    *whole = trunc(x);
    return x - *whole;
}

double hypot(double x, double y)
{
    return sqrt(x * x + y * y);
}
