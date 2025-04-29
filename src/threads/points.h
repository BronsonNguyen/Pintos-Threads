#ifndef FIXED_POINT_H
#define FIXED_POINT_H

/* Fixed-point arithmetic for the 4.4BSD scheduler.
   Uses 17.14 format: 17 bits for the integer part, 14 bits for the fractional part. */

typedef int fixed_point_t;

/* Conversion */
#define FP_SHIFT 14
#define FP_FACTOR (1 << FP_SHIFT)

/* Convert n to fixed point */
#define FP_FROM_INT(n) ((n) * FP_FACTOR)

/* Convert x to integer (rounding toward zero) */
#define FP_TO_INT_TRUNC(x) ((x) / FP_FACTOR)

/* Convert x to integer (rounding to nearest) */
#define FP_TO_INT_ROUND(x) ((x) >= 0 ? ((x) + (FP_FACTOR / 2)) / FP_FACTOR : \
                                      ((x) - (FP_FACTOR / 2)) / FP_FACTOR)

/* Add x and y */
#define FP_ADD(x, y) ((x) + (y))

/* Subtract y from x */
#define FP_SUB(x, y) ((x) - (y))

/* Add x and integer n */
#define FP_ADD_INT(x, n) ((x) + FP_FROM_INT(n))

/* Subtract integer n from x */
#define FP_SUB_INT(x, n) ((x) - FP_FROM_INT(n))

/* Multiply x by y */
#define FP_MUL(x, y) (((int64_t) (x)) * (y) / FP_FACTOR)

/* Multiply x by integer n */
#define FP_MUL_INT(x, n) ((x) * (n))

/* Divide x by y */
#define FP_DIV(x, y) (((int64_t) (x)) * FP_FACTOR / (y))

/* Divide x by integer n */
#define FP_DIV_INT(x, n) ((x) / (n))

#endif /* fixed-point.h */