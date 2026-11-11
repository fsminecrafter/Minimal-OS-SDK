/* Doom for Minimal-OS: the little floating point the engine uses
 * (gamma, mouse acceleration, map setup). */
#ifndef DOOM_LIBC_MATH_H
#define DOOM_LIBC_MATH_H
double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double fmod(double x, double y);
#define M_PI 3.14159265358979323846
#endif
