#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

/* Fixed-point arithmetic for the 4.4BSD scheduler.
   Uses 17.14 format: 17 bits for the integer part, 14 bits for the fractional part. */

typedef int fixed_point_t;

/* 17.14 fixed-point format - 14 fractional bits */
#define FP_F 14

/* Convert integer n to fixed-point value */
#define FP_FROM_INT(n) ((n) * (1 << FP_F))

/* Convert fixed-point value x to integer (rounding toward zero) */
#define FP_TO_INT_TRUNC(x) ((x) / (1 << FP_F))

/* Convert fixed-point value x to integer (rounding to nearest) */
#define FP_TO_INT_ROUND(x) ((x) >= 0 ? \
                           ((x) + (1 << (FP_F - 1))) / (1 << FP_F) : \
                           ((x) - (1 << (FP_F - 1))) / (1 << FP_F))

/* Add two fixed-point values */
#define FP_ADD(x, y) ((x) + (y))

/* Subtract fixed-point value y from x */
#define FP_SUB(x, y) ((x) - (y))

/* Add integer n to fixed-point value x */
#define FP_ADD_INT(x, n) ((x) + FP_FROM_INT(n))

/* Subtract integer n from fixed-point value x */
#define FP_SUB_INT(x, n) ((x) - FP_FROM_INT(n))

/* Multiply two fixed-point values */
#define FP_MUL(x, y) (((int64_t) (x)) * (y) / (1 << FP_F))

/* Multiply fixed-point value x by integer n */
#define FP_MUL_INT(x, n) ((x) * (n))

/* Divide fixed-point value x by fixed-point value y */
#define FP_DIV(x, y) (((int64_t) (x)) * (1 << FP_F) / (y))

/* Divide fixed-point value x by integer n */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* threads/fixed-point.h */