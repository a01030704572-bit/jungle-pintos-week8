#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#include <stdint.h>

/* 17.14 고정소수점 연산 (mlfqs용)
 * 커널에서는 float를 쓸 수 없으므로 정수에 FP_F(2^14)를 곱해 소수를 표현한다.
 * 예) 1.0 = 16384, 0.5 = 8192
 *
 * 표기: x, y = 고정소수점 값 / n = 정수 */

#define FP_F (1 << 14)                       // 1.0에 해당하는 값 (16384)

/* 변환 */
static inline int int_to_fp (int n)          { return n * FP_F; }   // 정수 → 고정소수점
static inline int fp_to_int (int x)          { return x / FP_F; }   // 고정소수점 → 정수 (0 쪽으로 버림)
static inline int fp_to_int_round (int x) {                         // 고정소수점 → 정수 (반올림)
	return x >= 0 ? (x + FP_F / 2) / FP_F                           // 양수: 0.5 더하고 버림
	              : (x - FP_F / 2) / FP_F;                          // 음수: 0.5 빼고 버림
}

/* 고정소수점 끼리 */
static inline int fp_add (int x, int y)      { return x + y; }
static inline int fp_sub (int x, int y)      { return x - y; }
static inline int fp_mul (int x, int y)      { return ((int64_t) x) * y / FP_F; }   // 64비트로 계산해 오버플로 방지
static inline int fp_div (int x, int y)      { return ((int64_t) x) * FP_F / y; }   // 64비트로 계산해 오버플로 방지

/* 고정소수점 와 정수 */
static inline int fp_add_int (int x, int n)  { return x + n * FP_F; }
static inline int fp_sub_int (int x, int n)  { return x - n * FP_F; }
static inline int fp_mul_int (int x, int n)  { return x * n; }
static inline int fp_div_int (int x, int n)  { return x / n; }

#endif /* threads/fixed_point.h */
