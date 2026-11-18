// musl's sinf.c, cosf.c and sincosf.c each define these small multiples of pi/2 for themselves.
// They are one set of constants, so they live here and the three files use them from this include.
static const double
s1pio2 = 1.57079632679489661923, /* 0x3FF921FB, 0x54442D18 */
s2pio2 = 3.14159265358979323846, /* 0x400921FB, 0x54442D18 */
s3pio2 = 4.71238898038468985769, /* 0x4012D97C, 0x7F3321D2 */
s4pio2 = 6.28318530717958647693; /* 0x401921FB, 0x54442D18 */
static const double
c1pio2 = 1.57079632679489661923,
c2pio2 = 3.14159265358979323846,
c3pio2 = 4.71238898038468985769,
c4pio2 = 6.28318530717958647693;
