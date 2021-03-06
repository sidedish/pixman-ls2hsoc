/*
 * Copyright © 2004, 2005 Red Hat, Inc.
 * Copyright © 2004 Nicholas Miell
 * Copyright © 2005 Trolltech AS
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Red Hat not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  Red Hat makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 *
 * Author:  Søren Sandmann (sandmann@redhat.com)
 * Minor Improvements: Nicholas Miell (nmiell@gmail.com)
 * MMX code paths for fbcompose.c by Lars Knoll (lars@trolltech.com)
 *
 * Based on work by Owen Taylor
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if defined USE_X86_MMX || defined USE_ARM_IWMMXT || defined USE_LOONGSON_MMI

#ifdef USE_LOONGSON_MMI
#include <loongson-mmintrin.h>
#else
#include <mmintrin.h>
#endif
#include "pixman-private.h"
#include "pixman-combine32.h"
#include "pixman-inlines.h"

#ifdef VERBOSE
#define CHECKPOINT() error_f ("at %s %d\n", __FUNCTION__, __LINE__)
#else
#define CHECKPOINT()
#endif

#if defined USE_ARM_IWMMXT && __GNUC__ == 4 && __GNUC_MINOR__ < 8
/* Empty the multimedia state. For some reason, ARM's mmintrin.h doesn't provide this.  */
extern __inline void __attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_empty (void)
{

}
#endif

#define COMBINE_A_OUT 1
#define COMBINE_A_IN  2
#define COMBINE_B_OUT 4
#define COMBINE_B_IN  8

#define COMBINE_CLEAR   0
#define COMBINE_A       (COMBINE_A_OUT | COMBINE_A_IN)
#define COMBINE_B       (COMBINE_B_OUT | COMBINE_B_IN)
#define COMBINE_A_OVER  (COMBINE_A_OUT | COMBINE_B_OUT | COMBINE_A_IN)
#define COMBINE_B_OVER  (COMBINE_A_OUT | COMBINE_B_OUT | COMBINE_B_IN)
#define COMBINE_A_ATOP  (COMBINE_B_OUT | COMBINE_A_IN)
#define COMBINE_B_ATOP  (COMBINE_A_OUT | COMBINE_B_IN)
#define COMBINE_XOR     (COMBINE_A_OUT | COMBINE_B_OUT)

/* no SIMD instructions for div, so leave it alone */
/* portion covered by a but not b */
static uint8_t
combine_disjoint_out_part (uint8_t a, uint8_t b)
{
    /* min (1, (1-b) / a) */

    b = ~b;                 /* 1 - b */
    if (b >= a)             /* 1 - b >= a -> (1-b)/a >= 1 */
	return MASK;        /* 1 */
    return DIV_UN8 (b, a);     /* (1-b) / a */
}

/* portion covered by both a and b */
static uint8_t
combine_disjoint_in_part (uint8_t a, uint8_t b)
{
    /* max (1-(1-b)/a,0) */
    /*  = - min ((1-b)/a - 1, 0) */
    /*  = 1 - min (1, (1-b)/a) */

    b = ~b;                 /* 1 - b */
    if (b >= a)             /* 1 - b >= a -> (1-b)/a >= 1 */
	return 0;           /* 1 - 1 */
    return ~DIV_UN8(b, a);    /* 1 - (1-b) / a */
}

/* portion covered by a but not b */
static uint8_t
combine_conjoint_out_part (uint8_t a, uint8_t b)
{
    /* max (1-b/a,0) */
    /* = 1-min(b/a,1) */

    /* min (1, (1-b) / a) */

    if (b >= a)             /* b >= a -> b/a >= 1 */
	return 0x00;        /* 0 */
    return ~DIV_UN8(b, a);    /* 1 - b/a */
}

/* portion covered by both a and b */
static uint8_t
combine_conjoint_in_part (uint8_t a, uint8_t b)
{
    /* min (1,b/a) */

    if (b >= a)             /* b >= a -> b/a >= 1 */
	return MASK;        /* 1 */
    return DIV_UN8 (b, a);     /* b/a */
}


#ifdef USE_X86_MMX
# if (defined(__SUNPRO_C) || defined(_MSC_VER) || defined(_WIN64))
#  include <xmmintrin.h>
# else
/* We have to compile with -msse to use xmmintrin.h, but that causes SSE
 * instructions to be generated that we don't want. Just duplicate the
 * functions we want to use.  */

extern __inline int __attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_movemask_pi8 (__m64 __A)
{
    int ret;

    asm ("pmovmskb %1, %0\n\t"
	: "=r" (ret)
	: "y" (__A)
    );

    return ret;
}

extern __inline __m64 __attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_mulhi_pu16 (__m64 __A, __m64 __B)
{
    asm ("pmulhuw %1, %0\n\t"
	: "+y" (__A)
	: "y" (__B)
    );
    return __A;
}

#  ifdef __OPTIMIZE__
extern __inline __m64 __attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_shuffle_pi16 (__m64 __A, int8_t const __N)
{
    __m64 ret;

    asm ("pshufw %2, %1, %0\n\t"
	: "=y" (ret)
	: "y" (__A), "K" (__N)
    );

    return ret;
}
#  else
#   define _mm_shuffle_pi16(A, N)					\
    ({									\
	__m64 ret;							\
									\
	asm ("pshufw %2, %1, %0\n\t"					\
	     : "=y" (ret)						\
	     : "y" (A), "K" ((const int8_t)N)				\
	);								\
									\
	ret;								\
    })
#  endif
# endif
#endif

#ifndef _MSC_VER
#define _MM_SHUFFLE(fp3,fp2,fp1,fp0) \
 (((fp3) << 6) | ((fp2) << 4) | ((fp1) << 2) | (fp0))
#endif

/* Notes about writing mmx code
 *
 * give memory operands as the second operand. If you give it as the
 * first, gcc will first load it into a register, then use that
 * register
 *
 *   ie. use
 *
 *         _mm_mullo_pi16 (x, mmx_constant);
 *
 *   not
 *
 *         _mm_mullo_pi16 (mmx_constant, x);
 *
 * Also try to minimize dependencies. i.e. when you need a value, try
 * to calculate it from a value that was calculated as early as
 * possible.
 */

/* --------------- MMX primitives ------------------------------------- */

/* If __m64 is defined as a struct or union, then define M64_MEMBER to be
 * the name of the member used to access the data.
 * If __m64 requires using mm_cvt* intrinsics functions to convert between
 * uint64_t and __m64 values, then define USE_CVT_INTRINSICS.
 * If __m64 and uint64_t values can just be cast to each other directly,
 * then define USE_M64_CASTS.
 * If __m64 is a double datatype, then define USE_M64_DOUBLE.
 */
#ifdef _MSC_VER
# define M64_MEMBER m64_u64
#elif defined(__ICC)
# define USE_CVT_INTRINSICS
#elif defined(USE_LOONGSON_MMI)
# define USE_M64_DOUBLE
#elif defined(__GNUC__)
# define USE_M64_CASTS
#elif defined(__SUNPRO_C)
# if (__SUNPRO_C >= 0x5120) && !defined(__NOVECTORSIZE__)
/* Solaris Studio 12.3 (Sun C 5.12) introduces __attribute__(__vector_size__)
 * support, and defaults to using it to define __m64, unless __NOVECTORSIZE__
 * is defined.   If it is used, then the mm_cvt* intrinsics must be used.
 */
#  define USE_CVT_INTRINSICS
# else
/* For Studio 12.2 or older, or when __attribute__(__vector_size__) is
 * disabled, __m64 is defined as a struct containing "unsigned long long l_".
 */
#  define M64_MEMBER l_
# endif
#endif

#if defined(USE_M64_CASTS) || defined(USE_CVT_INTRINSICS) || defined(USE_M64_DOUBLE)
typedef uint64_t mmxdatafield;
#else
typedef __m64 mmxdatafield;
#endif

typedef struct
{
    mmxdatafield mmx_4xffff;
    mmxdatafield mmx_4xff00;
    mmxdatafield mmx_4x00ff;
    mmxdatafield mmx_4x0080;
    mmxdatafield mmx_565_rgb;
    mmxdatafield mmx_565_unpack_multiplier;
    mmxdatafield mmx_565_pack_multiplier;
    mmxdatafield mmx_565_r;
    mmxdatafield mmx_565_g;
    mmxdatafield mmx_565_b;
    mmxdatafield mmx_packed_565_rb;
    mmxdatafield mmx_packed_565_g;
    mmxdatafield mmx_expand_565_g;
    mmxdatafield mmx_expand_565_b;
    mmxdatafield mmx_expand_565_r;
#ifndef USE_LOONGSON_MMI
    mmxdatafield mmx_mask_0;
    mmxdatafield mmx_mask_1;
    mmxdatafield mmx_mask_2;
    mmxdatafield mmx_mask_3;
#endif
    mmxdatafield mmx_full_alpha;
    mmxdatafield mmx_4x0101;
    mmxdatafield mmx_ff000000;
} mmx_data_t;

#if defined(_MSC_VER)
# define MMXDATA_INIT(field, val) { val ## UI64 }
#elif defined(M64_MEMBER)       /* __m64 is a struct, not an integral type */
# define MMXDATA_INIT(field, val) field =   { val ## ULL }
#else                           /* mmxdatafield is an integral type */
# define MMXDATA_INIT(field, val) field =   val ## ULL
#endif

static const mmx_data_t c =
{
    MMXDATA_INIT (.mmx_4xffff,                   0xffffffffffffffff),
    MMXDATA_INIT (.mmx_4xff00,                   0xff00ff00ff00ff00),
    MMXDATA_INIT (.mmx_4x00ff,                   0x00ff00ff00ff00ff),
    MMXDATA_INIT (.mmx_4x0080,                   0x0080008000800080),
    MMXDATA_INIT (.mmx_565_rgb,                  0x000001f0003f001f),
    MMXDATA_INIT (.mmx_565_unpack_multiplier,    0x0000008404100840),
    MMXDATA_INIT (.mmx_565_pack_multiplier,      0x2000000420000004),
    MMXDATA_INIT (.mmx_565_r,                    0x000000f800000000),
    MMXDATA_INIT (.mmx_565_g,                    0x0000000000fc0000),
    MMXDATA_INIT (.mmx_565_b,                    0x00000000000000f8),
    MMXDATA_INIT (.mmx_packed_565_rb,            0x00f800f800f800f8),
    MMXDATA_INIT (.mmx_packed_565_g,             0x0000fc000000fc00),
    MMXDATA_INIT (.mmx_expand_565_g,             0x07e007e007e007e0),
    MMXDATA_INIT (.mmx_expand_565_b,             0x001f001f001f001f),
    MMXDATA_INIT (.mmx_expand_565_r,             0xf800f800f800f800),
#ifndef USE_LOONGSON_MMI
    MMXDATA_INIT (.mmx_mask_0,                   0xffffffffffff0000),
    MMXDATA_INIT (.mmx_mask_1,                   0xffffffff0000ffff),
    MMXDATA_INIT (.mmx_mask_2,                   0xffff0000ffffffff),
    MMXDATA_INIT (.mmx_mask_3,                   0x0000ffffffffffff),
#endif
    MMXDATA_INIT (.mmx_full_alpha,               0x00ff000000000000),
    MMXDATA_INIT (.mmx_4x0101,                   0x0101010101010101),
    MMXDATA_INIT (.mmx_ff000000,                 0xff000000ff000000),
};

#ifdef USE_CVT_INTRINSICS
#    define MC(x) to_m64 (c.mmx_ ## x)
#elif defined(USE_M64_CASTS)
#    define MC(x) ((__m64)c.mmx_ ## x)
#elif defined(USE_M64_DOUBLE)
#    define MC(x) (*(__m64 *)&c.mmx_ ## x)
#else
#    define MC(x) c.mmx_ ## x
#endif

static force_inline __m64
to_m64 (uint64_t x)
{
#ifdef USE_CVT_INTRINSICS
    return _mm_cvtsi64_m64 (x);
#elif defined M64_MEMBER        /* __m64 is a struct, not an integral type */
    __m64 res;

    res.M64_MEMBER = x;
    return res;
#elif defined USE_M64_DOUBLE
    return *(__m64 *)&x;
#else /* USE_M64_CASTS */
    return (__m64)x;
#endif
}

static force_inline uint64_t
to_uint64 (__m64 x)
{
#ifdef USE_CVT_INTRINSICS
    return _mm_cvtm64_si64 (x);
#elif defined M64_MEMBER        /* __m64 is a struct, not an integral type */
    uint64_t res = x.M64_MEMBER;
    return res;
#elif defined USE_M64_DOUBLE
    return *(uint64_t *)&x;
#else /* USE_M64_CASTS */
    return (uint64_t)x;
#endif
}

static force_inline __m64
shift (__m64 v,
       int   s)
{
    if (s > 0)
	return _mm_slli_si64 (v, s);
    else if (s < 0)
	return _mm_srli_si64 (v, -s);
    else
	return v;
}

static force_inline __m64
negate (__m64 mask)
{
    return _mm_xor_si64 (mask, MC (4x00ff));
}

static force_inline __m64
pix_multiply (__m64 a, __m64 b)
{
    __m64 res;

    res = _mm_mullo_pi16 (a, b);
    res = _mm_adds_pu16 (res, MC (4x0080));
    res = _mm_mulhi_pu16 (res, MC (4x0101));

    return res;
}

static force_inline __m64
pix_add (__m64 a, __m64 b)
{
    return _mm_adds_pu8 (a, b);
}

static force_inline __m64
expand_alpha (__m64 pixel)
{
    return _mm_shuffle_pi16 (pixel, _MM_SHUFFLE (3, 3, 3, 3));
}

static force_inline __m64
expand_alpha_rev (__m64 pixel)
{
    return _mm_shuffle_pi16 (pixel, _MM_SHUFFLE (0, 0, 0, 0));
}

static force_inline __m64
invert_colors (__m64 pixel)
{
    return _mm_shuffle_pi16 (pixel, _MM_SHUFFLE (3, 0, 1, 2));
}

static force_inline __m64
over (__m64 src,
      __m64 srca,
      __m64 dest)
{
    return _mm_adds_pu8 (src, pix_multiply (dest, negate (srca)));
}

static force_inline __m64
over_rev_non_pre (__m64 src, __m64 dest)
{
    __m64 srca = expand_alpha (src);
    __m64 srcfaaa = _mm_or_si64 (srca, MC (full_alpha));

    return over (pix_multiply (invert_colors (src), srcfaaa), srca, dest);
}

static force_inline __m64
in (__m64 src, __m64 mask)
{
    return pix_multiply (src, mask);
}

#ifndef _MSC_VER
static force_inline __m64
in_over (__m64 src, __m64 srca, __m64 mask, __m64 dest)
{
    return over (in (src, mask), pix_multiply (srca, mask), dest);
}

#else

#define in_over(src, srca, mask, dest)					\
    over (in (src, mask), pix_multiply (srca, mask), dest)

#endif

/* Elemental unaligned loads */

static force_inline __m64 ldq_u(__m64 *p)
{
#ifdef USE_X86_MMX
    /* x86's alignment restrictions are very relaxed. */
    return *(__m64 *)p;
#elif defined USE_ARM_IWMMXT
    int align = (uintptr_t)p & 7;
    __m64 *aligned_p;
    if (align == 0)
	return *p;
    aligned_p = (__m64 *)((uintptr_t)p & ~7);
    return (__m64) _mm_align_si64 (aligned_p[0], aligned_p[1], align);
#else
    struct __una_u64 { __m64 x __attribute__((packed)); };
    const struct __una_u64 *ptr = (const struct __una_u64 *) p;
    return (__m64) ptr->x;
#endif
}

static force_inline uint32_t ldl_u(const uint32_t *p)
{
#ifdef USE_X86_MMX
    /* x86's alignment restrictions are very relaxed. */
    return *p;
#else
    struct __una_u32 { uint32_t x __attribute__((packed)); };
    const struct __una_u32 *ptr = (const struct __una_u32 *) p;
    return ptr->x;
#endif
}

static force_inline __m64
load (const uint32_t *v)
{
#ifdef USE_LOONGSON_MMI
    __m64 ret;
    asm ("lwc1 %0, %1\n\t"
	: "=f" (ret)
	: "m" (*v)
    );
    return ret;
#else
    return _mm_cvtsi32_si64 (*v);
#endif
}

static force_inline __m64
load8888 (const uint32_t *v)
{
#ifdef USE_LOONGSON_MMI
    return _mm_unpacklo_pi8_f (*(__m32 *)v, _mm_setzero_si64 ());
#else
    return _mm_unpacklo_pi8 (load (v), _mm_setzero_si64 ());
#endif
}

static force_inline __m64
load8888u (const uint32_t *v)
{
    uint32_t l = ldl_u (v);
    return load8888 (&l);
}

static force_inline __m64
pack8888 (__m64 lo, __m64 hi)
{
    return _mm_packs_pu16 (lo, hi);
}

static force_inline void
store (uint32_t *dest, __m64 v)
{
#ifdef USE_LOONGSON_MMI
    asm ("swc1 %1, %0\n\t"
	: "=m" (*dest)
	: "f" (v)
	: "memory"
    );
#else
    *dest = _mm_cvtsi64_si32 (v);
#endif
}

static force_inline void
store8888 (uint32_t *dest, __m64 v)
{
    v = pack8888 (v, _mm_setzero_si64 ());
    store (dest, v);
}

static force_inline pixman_bool_t
is_equal (__m64 a, __m64 b)
{
#ifdef USE_LOONGSON_MMI
    /* __m64 is double, we can compare directly. */
    return a == b;
#else
    return _mm_movemask_pi8 (_mm_cmpeq_pi8 (a, b)) == 0xff;
#endif
}

static force_inline pixman_bool_t
is_opaque (__m64 v)
{
#ifdef USE_LOONGSON_MMI
    return is_equal (_mm_and_si64 (v, MC (full_alpha)), MC (full_alpha));
#else
    __m64 ffs = _mm_cmpeq_pi8 (v, v);
    return (_mm_movemask_pi8 (_mm_cmpeq_pi8 (v, ffs)) & 0x40);
#endif
}

static force_inline pixman_bool_t
is_zero (__m64 v)
{
    return is_equal (v, _mm_setzero_si64 ());
}

/* Expand 16 bits positioned at @pos (0-3) of a mmx register into
 *
 *    00RR00GG00BB
 *
 * --- Expanding 565 in the low word ---
 *
 * m = (m << (32 - 3)) | (m << (16 - 5)) | m;
 * m = m & (01f0003f001f);
 * m = m * (008404100840);
 * m = m >> 8;
 *
 * Note the trick here - the top word is shifted by another nibble to
 * avoid it bumping into the middle word
 */
static force_inline __m64
expand565 (__m64 pixel, int pos)
{
    __m64 p = pixel;
    __m64 t1, t2;

    /* move pixel to low 16 bit and zero the rest */
#ifdef USE_LOONGSON_MMI
    p = loongson_extract_pi16 (p, pos);
#else
    p = shift (shift (p, (3 - pos) * 16), -48);
#endif

    t1 = shift (p, 36 - 11);
    t2 = shift (p, 16 - 5);

    p = _mm_or_si64 (t1, p);
    p = _mm_or_si64 (t2, p);
    p = _mm_and_si64 (p, MC (565_rgb));

    pixel = _mm_mullo_pi16 (p, MC (565_unpack_multiplier));
    return _mm_srli_pi16 (pixel, 8);
}

/* Expand 4 16 bit pixels in an mmx register into two mmx registers of
 *
 *    AARRGGBBRRGGBB
 */
static force_inline void
expand_4xpacked565 (__m64 vin, __m64 *vout0, __m64 *vout1, int full_alpha)
{
    __m64 t0, t1, alpha = _mm_setzero_si64 ();
    __m64 r = _mm_and_si64 (vin, MC (expand_565_r));
    __m64 g = _mm_and_si64 (vin, MC (expand_565_g));
    __m64 b = _mm_and_si64 (vin, MC (expand_565_b));
    if (full_alpha)
	alpha = _mm_cmpeq_pi32 (alpha, alpha);

    /* Replicate high bits into empty low bits. */
    r = _mm_or_si64 (_mm_srli_pi16 (r, 8), _mm_srli_pi16 (r, 13));
    g = _mm_or_si64 (_mm_srli_pi16 (g, 3), _mm_srli_pi16 (g, 9));
    b = _mm_or_si64 (_mm_slli_pi16 (b, 3), _mm_srli_pi16 (b, 2));

    r = _mm_packs_pu16 (r, _mm_setzero_si64 ());	/* 00 00 00 00 R3 R2 R1 R0 */
    g = _mm_packs_pu16 (g, _mm_setzero_si64 ());	/* 00 00 00 00 G3 G2 G1 G0 */
    b = _mm_packs_pu16 (b, _mm_setzero_si64 ());	/* 00 00 00 00 B3 B2 B1 B0 */

    t1 = _mm_unpacklo_pi8 (r, alpha);			/* A3 R3 A2 R2 A1 R1 A0 R0 */
    t0 = _mm_unpacklo_pi8 (b, g);			/* G3 B3 G2 B2 G1 B1 G0 B0 */

    *vout0 = _mm_unpacklo_pi16 (t0, t1);		/* A1 R1 G1 B1 A0 R0 G0 B0 */
    *vout1 = _mm_unpackhi_pi16 (t0, t1);		/* A3 R3 G3 B3 A2 R2 G2 B2 */
}

static force_inline __m64
expand8888 (__m64 in, int pos)
{
    if (pos == 0)
	return _mm_unpacklo_pi8 (in, _mm_setzero_si64 ());
    else
	return _mm_unpackhi_pi8 (in, _mm_setzero_si64 ());
}

static force_inline __m64
expandx888 (__m64 in, int pos)
{
    return _mm_or_si64 (expand8888 (in, pos), MC (full_alpha));
}

static force_inline void
expand_4x565 (__m64 vin, __m64 *vout0, __m64 *vout1, __m64 *vout2, __m64 *vout3, int full_alpha)
{
    __m64 v0, v1;
    expand_4xpacked565 (vin, &v0, &v1, full_alpha);
    *vout0 = expand8888 (v0, 0);
    *vout1 = expand8888 (v0, 1);
    *vout2 = expand8888 (v1, 0);
    *vout3 = expand8888 (v1, 1);
}

static force_inline __m64
pack_565 (__m64 pixel, __m64 target, int pos)
{
    __m64 p = pixel;
    __m64 t = target;
    __m64 r, g, b;

    r = _mm_and_si64 (p, MC (565_r));
    g = _mm_and_si64 (p, MC (565_g));
    b = _mm_and_si64 (p, MC (565_b));

#ifdef USE_LOONGSON_MMI
    r = shift (r, -(32 - 8));
    g = shift (g, -(16 - 3));
    b = shift (b, -(0  + 3));

    p = _mm_or_si64 (r, g);
    p = _mm_or_si64 (p, b);
    return loongson_insert_pi16 (t, p, pos);
#else
    r = shift (r, -(32 - 8) + pos * 16);
    g = shift (g, -(16 - 3) + pos * 16);
    b = shift (b, -(0  + 3) + pos * 16);

    if (pos == 0)
	t = _mm_and_si64 (t, MC (mask_0));
    else if (pos == 1)
	t = _mm_and_si64 (t, MC (mask_1));
    else if (pos == 2)
	t = _mm_and_si64 (t, MC (mask_2));
    else if (pos == 3)
	t = _mm_and_si64 (t, MC (mask_3));

    p = _mm_or_si64 (r, t);
    p = _mm_or_si64 (g, p);

    return _mm_or_si64 (b, p);
#endif
}

static force_inline __m64
pack_4xpacked565 (__m64 a, __m64 b)
{
    __m64 rb0 = _mm_and_si64 (a, MC (packed_565_rb));
    __m64 rb1 = _mm_and_si64 (b, MC (packed_565_rb));

    __m64 t0 = _mm_madd_pi16 (rb0, MC (565_pack_multiplier));
    __m64 t1 = _mm_madd_pi16 (rb1, MC (565_pack_multiplier));

    __m64 g0 = _mm_and_si64 (a, MC (packed_565_g));
    __m64 g1 = _mm_and_si64 (b, MC (packed_565_g));

    t0 = _mm_or_si64 (t0, g0);
    t1 = _mm_or_si64 (t1, g1);

    t0 = shift(t0, -5);
#ifdef USE_ARM_IWMMXT
    t1 = shift(t1, -5);
    return _mm_packs_pu32 (t0, t1);
#else
    t1 = shift(t1, -5 + 16);
    return _mm_shuffle_pi16 (_mm_or_si64 (t0, t1), _MM_SHUFFLE (3, 1, 2, 0));
#endif
}

#ifndef _MSC_VER

static force_inline __m64
pack_4x565 (__m64 v0, __m64 v1, __m64 v2, __m64 v3)
{
    return pack_4xpacked565 (pack8888 (v0, v1), pack8888 (v2, v3));
}

static force_inline __m64
pix_add_mul (__m64 x, __m64 a, __m64 y, __m64 b)
{
    x = pix_multiply (x, a);
    y = pix_multiply (y, b);

    return pix_add (x, y);
}

#else

/* MSVC only handles a "pass by register" of up to three SSE intrinsics */

#define pack_4x565(v0, v1, v2, v3) \
    pack_4xpacked565 (pack8888 (v0, v1), pack8888 (v2, v3))

#define pix_add_mul(x, a, y, b)	 \
    ( x = pix_multiply (x, a),	 \
      y = pix_multiply (y, b),	 \
      pix_add (x, y) )

#endif

/* --------------- MMX code patch for fbcompose.c --------------------- */

static force_inline __m64
combine (const uint32_t *src, const uint32_t *mask)
{
    __m64 vsrc = load8888 (src);

    if (mask)
    {
	__m64 m = load8888 (mask);

	m = expand_alpha (m);
	vsrc = pix_multiply (vsrc, m);
    }

    return vsrc;
}

static force_inline void
mmx_combine_mask_ca(const uint32_t *src, const uint32_t *mask, __m64 *s64, __m64 *m64)
{
    __m64 res, tmp;
    
    if(!(*mask))
	{
	    *s64 = 0;
	    *m64 = 0;
	    return;
	}
    
    *s64 = load8888(src);
    
    if (*mask == ~0)
	{
	    *m64 = expand_alpha(*s64);
	    return;
	}
    
    *m64 = load8888(mask);
    
    res = pix_multiply(*s64, *m64);
    tmp = expand_alpha(*s64);
    *s64 = res;
    *m64 = pix_multiply(*m64, tmp);
}

static force_inline __m64
core_combine_over_u_pixel_mmx (__m64 vsrc, __m64 vdst)
{
    vsrc = _mm_unpacklo_pi8 (vsrc, _mm_setzero_si64 ());

    if (is_opaque (vsrc))
    {
	return vsrc;
    }
    else if (!is_zero (vsrc))
    {
	return over (vsrc, expand_alpha (vsrc),
		     _mm_unpacklo_pi8 (vdst, _mm_setzero_si64 ()));
    }

    return _mm_unpacklo_pi8 (vdst, _mm_setzero_si64 ());
}

static void
mmx_combine_over_u (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 vsrc = combine (src, mask);

	if (is_opaque (vsrc))
	{
	    store8888 (dest, vsrc);
	}
	else if (!is_zero (vsrc))
	{
	    __m64 sa = expand_alpha (vsrc);
	    store8888 (dest, over (vsrc, sa, load8888 (dest)));
	}

	++dest;
	++src;
	if (mask)
	    ++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_over_reverse_u (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               dest,
                            const uint32_t *         src,
                            const uint32_t *         mask,
                            int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 d, da;
	__m64 s = combine (src, mask);

	d = load8888 (dest);
	da = expand_alpha (d);
	store8888 (dest, over (d, da, s));

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_in_u (pixman_implementation_t *imp,
                  pixman_op_t              op,
                  uint32_t *               dest,
                  const uint32_t *         src,
                  const uint32_t *         mask,
                  int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 a;
	__m64 x = combine (src, mask);

	a = load8888 (dest);
	a = expand_alpha (a);
	x = pix_multiply (x, a);

	store8888 (dest, x);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_in_reverse_u (pixman_implementation_t *imp,
                          pixman_op_t              op,
                          uint32_t *               dest,
                          const uint32_t *         src,
                          const uint32_t *         mask,
                          int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 a = combine (src, mask);
	__m64 x;

	x = load8888 (dest);
	a = expand_alpha (a);
	x = pix_multiply (x, a);
	store8888 (dest, x);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_out_u (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 a;
	__m64 x = combine (src, mask);

	a = load8888 (dest);
	a = expand_alpha (a);
	a = negate (a);
	x = pix_multiply (x, a);
	store8888 (dest, x);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_out_reverse_u (pixman_implementation_t *imp,
                           pixman_op_t              op,
                           uint32_t *               dest,
                           const uint32_t *         src,
                           const uint32_t *         mask,
                           int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 a = combine (src, mask);
	__m64 x;

	x = load8888 (dest);
	a = expand_alpha (a);
	a = negate (a);
	x = pix_multiply (x, a);

	store8888 (dest, x);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_atop_u (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 da, d, sia;
	__m64 s = combine (src, mask);

	d = load8888 (dest);
	sia = expand_alpha (s);
	sia = negate (sia);
	da = expand_alpha (d);
	s = pix_add_mul (s, da, d, sia);
	store8888 (dest, s);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_atop_reverse_u (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               dest,
                            const uint32_t *         src,
                            const uint32_t *         mask,
                            int                      width)
{
    const uint32_t *end;

    end = dest + width;

    while (dest < end)
    {
	__m64 dia, d, sa;
	__m64 s = combine (src, mask);

	d = load8888 (dest);
	sa = expand_alpha (s);
	dia = expand_alpha (d);
	dia = negate (dia);
	s = pix_add_mul (s, dia, d, sa);
	store8888 (dest, s);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_xor_u (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 dia, d, sia;
	__m64 s = combine (src, mask);

	d = load8888 (dest);
	sia = expand_alpha (s);
	dia = expand_alpha (d);
	sia = negate (sia);
	dia = negate (dia);
	s = pix_add_mul (s, dia, d, sia);
	store8888 (dest, s);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_add_u (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 d;
	__m64 s = combine (src, mask);

	d = load8888 (dest);
	s = pix_add (s, d);
	store8888 (dest, s);

	++dest;
	++src;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

static void
mmx_combine_disjoint_over_u (pixman_implementation_t *imp,
			 pixman_op_t              op,
                         uint32_t *                dest,
                         const uint32_t *          src,
                         const uint32_t *          mask,
                         int                      width)
{
    uint32_t *end = dest + width;
    uint32_t s32;
    uint64_t sa64;
    __m64 s64, d64;

    while (dest < end)
	{
	    s64 = combine (src, mask);
	    
	    if (s64)
		{
		    store8888(&s32, s64);
		    sa64 = combine_disjoint_out_part (*dest >> A_SHIFT, s32 >> A_SHIFT);
		    d64 = pix_add (pix_multiply (load8888 (dest),expand_alpha_rev ((*(__m64*)&sa64))), s64);
		    store8888 (dest, d64);
		}
	    
	    ++dest;
	    ++src;
	    if (mask)
		++mask;
	    
	}
}

static void
mmx_combine_saturate_u (pixman_implementation_t *imp,
                        pixman_op_t              op,
                        uint32_t *               dest,
                        const uint32_t *         src,
                        const uint32_t *         mask,
                        int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	uint32_t s, sa, da;
	uint32_t d = *dest;
	__m64 ms = combine (src, mask);
	__m64 md = load8888 (dest);

	store8888(&s, ms);
	da = ~d >> 24;
	sa = s >> 24;

	if (sa > da)
	{
	    uint32_t quot = DIV_UN8 (da, sa) << 24;
	    __m64 msa = load8888 (&quot);
	    msa = expand_alpha (msa);
	    ms = pix_multiply (ms, msa);
	}

	md = pix_add (md, ms);
	store8888 (dest, md);

	++src;
	++dest;
	if (mask)
	    mask++;
    }
    _mm_empty ();
}

/* 在combine_conjoint_general_u等函数中,有多个分支,由参数combine决定,*/
/*此值在函数运行期间并不会改变,所以原始代码中每个值都去判断是没有必要的.*/
/*可以在函数入口处判断,并设置好相应的函数指针,以后直接调用即可.*/
/*有两个分支是直接返回常量,为了做到统一,自定义两个内联函数,其作用为直接返回相应的常量.*/
/*_u和_ca后綴的两套代均码可用*/
#define DEF_FUNC_ZERO_MASK(type, zm, suffix, res)			\
    static type inline combine_joint_ ##zm## _ ##suffix( type sa, type da, type io_flag) \
    {									\
	return res;							\
    }									

/* 四个分支中的另外两个分支函数,conjoint与disjoint的代码结构一样,函数名不一样,设置此宏来生成相应的函数*/
/* 参数顺序不一样,由io_flag标记决定,0表示in_part,1表示out_part*/
/*由于是底层函数,并不向外提供此接口,因此这里没有判断其值是否合法. */
#define DEF_FUNC_COMBINE_JOINT_U(cd, io)				\
    static uint8_t inline combine_ ##cd## joint_ ##io## _part_u(uint8_t sa, uint8_t da, uint8_t io_flag) \
    {									\
	uint8_t parm[2];						\
	parm[0] = sa * (io_flag ^ 0x1) + da * (io_flag ^ 0x0);		\
	parm[1] = sa * (io_flag ^ 0x0) + da * (io_flag ^ 0x1);		\
	return combine_ ##cd## joint_ ##io## _part (parm[0], parm[1]);	\
    }

/* 设置函数指针数组的宏,在函数入口处存放正确的处理函数. */
/*_u和_ca后綴的两套代均码可用*/
#define DEF_COMB_FUNC_ARR(cd,SUFFIX,suffix)				\
    COMBINE_JOINT_FUNC_##SUFFIX combine_ ##cd## joint_ ##suffix[4] ={	\
	combine_joint_zero_ ##suffix,					\
	combine_ ##cd## joint_out_part_ ##suffix,			\
	combine_ ##cd## joint_in_part_ ##suffix,			\
	combine_joint_mask_ ##suffix					\
    };

typedef  uint8_t (*COMBINE_JOINT_FUNC_U)(uint8_t a, uint8_t b, uint8_t io_flag);

DEF_FUNC_ZERO_MASK(uint8_t,zero,u, 0x0)
DEF_FUNC_ZERO_MASK(uint8_t,mask,u, ~0x0)

DEF_FUNC_COMBINE_JOINT_U(dis, in);
DEF_FUNC_COMBINE_JOINT_U(dis, out);
DEF_COMB_FUNC_ARR(dis,U,u)

DEF_FUNC_COMBINE_JOINT_U(con, in);
DEF_FUNC_COMBINE_JOINT_U(con, out);
DEF_COMB_FUNC_ARR(con, U, u)

/* conjoint与disjoint相关函数主体结构一样的,只是四个分支中调用的函数不同,因此设置一个底层函数*/
static void
mmx_combine_joint_general_u (uint32_t * dest,
			 const uint32_t *src,
			 const uint32_t *mask,
			 int            width,
			 uint8_t        comb,
			 COMBINE_JOINT_FUNC_U *cjf)
{
    COMBINE_JOINT_FUNC_U combine_joint_u[2];
    combine_joint_u[0] = cjf[comb & COMBINE_A]; /* in_part */
    combine_joint_u[1] = cjf[(comb & COMBINE_B)>>2]; /* out_par */
    
    uint32_t *end = dest + width;
    while (dest < end)
	{
	    __m64 s64 = combine (src, mask);
	    __m64 d64,sa64,da64;
	    uint8_t sa, da;
	    uint32_t tmp;
	    uint64_t Fa, Fb;
	        
	    /* 由于这些函数中有除法指令,因此并未用多媒体指令来优化, */
	    /* 从而需要再转存到uint32_t类型变量中,再取出运算.*/
	    store8888(&tmp, s64);
	    sa = tmp >> A_SHIFT;
	    da = *dest >> A_SHIFT;
	        
	    Fa = combine_joint_u[0](sa, da, 0);
	    Fb = combine_joint_u[1](sa, da, 1);
	        
	    d64 = load8888(dest);
	    sa64 = expand_alpha_rev (*(__m64*)&Fa);
	    da64 = expand_alpha_rev (*(__m64*)&Fb);
	        
	    d64 = pix_add_mul (s64, sa64, d64, da64);
	        
	    store8888 (dest, d64);
	        
	    ++dest;
	    ++src;
	    if (mask)
		++mask;
	}
}


static void
mmx_combine_disjoint_general_u (uint32_t * dest,
				const uint32_t *src,
				const uint32_t *mask,
				int            width,
				uint8_t        comb)
{
    mmx_combine_joint_general_u (dest, src, mask, width, comb, combine_disjoint_u);
}

static void
mmx_combine_disjoint_in_u (pixman_implementation_t *imp,
			   pixman_op_t              op,
			   uint32_t *                dest,
			   const uint32_t *          src,
			   const uint32_t *          mask,
			   int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_A_IN);
}

static void
mmx_combine_disjoint_in_reverse_u (pixman_implementation_t *imp,
				   pixman_op_t              op,
				   uint32_t *                dest,
				   const uint32_t *          src,
				   const uint32_t *          mask,
				   int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_B_IN);
}

static void
mmx_combine_disjoint_out_u (pixman_implementation_t *imp,
			    pixman_op_t              op,
			    uint32_t *                dest,
			    const uint32_t *          src,
			    const uint32_t *          mask,
			    int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_A_OUT);
}

static void
mmx_combine_disjoint_out_reverse_u (pixman_implementation_t *imp,
				    pixman_op_t              op,
				    uint32_t *                dest,
				    const uint32_t *          src,
				    const uint32_t *          mask,
				    int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_B_OUT);
}

static void
mmx_combine_disjoint_atop_u (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_A_ATOP);
}

static void
mmx_combine_disjoint_atop_reverse_u (pixman_implementation_t *imp,
				     pixman_op_t              op,
				     uint32_t *                dest,
				     const uint32_t *          src,
				     const uint32_t *          mask,
				     int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_B_ATOP);
}

static void
mmx_combine_disjoint_xor_u (pixman_implementation_t *imp,
			    pixman_op_t              op,
			    uint32_t *                dest,
			    const uint32_t *          src,
			    const uint32_t *          mask,
			    int                      width)
{
    mmx_combine_disjoint_general_u (dest, src, mask, width, COMBINE_XOR);
}

/* Conjoint */
static void
mmx_combine_conjoint_general_u(uint32_t * dest,
			       const uint32_t *src,
			       const uint32_t *mask,
			       int            width,
			       uint8_t        comb)
{
    mmx_combine_joint_general_u (dest, src, mask, width, comb, combine_conjoint_u);
}

static void
mmx_combine_conjoint_over_u (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_A_OVER);
}

static void
mmx_combine_conjoint_over_reverse_u (pixman_implementation_t *imp,
				     pixman_op_t              op,
				     uint32_t *                dest,
				     const uint32_t *          src,
				     const uint32_t *          mask,
				     int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_B_OVER);
}

static void
mmx_combine_conjoint_in_u (pixman_implementation_t *imp,
			   pixman_op_t              op,
			   uint32_t *                dest,
			   const uint32_t *          src,
			   const uint32_t *          mask,
			   int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_A_IN);
}

static void
mmx_combine_conjoint_in_reverse_u (pixman_implementation_t *imp,
				   pixman_op_t              op,
				   uint32_t *                dest,
				   const uint32_t *          src,
				   const uint32_t *          mask,
				   int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_B_IN);
}

static void
mmx_combine_conjoint_out_u (pixman_implementation_t *imp,
			    pixman_op_t              op,
			    uint32_t *                dest,
			    const uint32_t *          src,
			    const uint32_t *          mask,
			    int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_A_OUT);
}

static void
mmx_combine_conjoint_out_reverse_u (pixman_implementation_t *imp,
				    pixman_op_t              op,
				    uint32_t *                dest,
				    const uint32_t *          src,
				    const uint32_t *          mask,
				    int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_B_OUT);
}

static void
mmx_combine_conjoint_atop_u (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_A_ATOP);
}

static void
mmx_combine_conjoint_atop_reverse_u (pixman_implementation_t *imp,
				     pixman_op_t              op,
				     uint32_t *                dest,
				     const uint32_t *          src,
				     const uint32_t *          mask,
				     int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_B_ATOP);
}

static void
mmx_combine_conjoint_xor_u (pixman_implementation_t *imp,
			    pixman_op_t              op,
			    uint32_t *                dest,
			    const uint32_t *          src,
			    const uint32_t *          mask,
			    int                      width)
{
    mmx_combine_conjoint_general_u (dest, src, mask, width, COMBINE_XOR);
}

/* 要注意优化前计算中A的计算方式与RGB不同 */
#define MMX_PDF_SEPARABLE_BLEND_MODE(name)				\
    static void								\
    mmx_combine_ ## name ## _u (pixman_implementation_t *imp,		\
				pixman_op_t              op,		\
				uint32_t *                dest,		\
				const uint32_t *          src,		\
				const uint32_t *          mask,		\
				int                      width)		\
    {									\
        uint32_t *end = dest + width;					\
									\
	while (dest < end)						\
	    {								\
		__m64 s64 = combine (src, mask);			\
		__m64 d64 = load8888 (dest);				\
		__m64 sa64 = expand_alpha (s64);			\
		__m64 isa64 = _mm_xor_si64 (sa64, MC (4x00ff));		\
		__m64 da64 = expand_alpha (d64);			\
		__m64 ida64 = _mm_xor_si64 (da64, MC (4x00ff));		\
		__m64 res;						\
		uint32_t result;					\
		store8888 (&result, pix_add_mul (d64,isa64,s64,ida64));	\
									\
		res = mmx_blend_ ## name (d64, da64, s64, sa64);	\
		*dest = result;						\
		*dest += (uint32_t)(to_uint64 (loongson_extract_pi16 (res, 0))); \
		*dest += ((uint32_t)(to_uint64 (loongson_extract_pi16 (res, 1)))) << G_SHIFT; \
		*dest += ((uint32_t)(to_uint64 (loongson_extract_pi16 (res, 2)))) << R_SHIFT; \
		*dest += ((uint32_t)(to_uint64 (loongson_extract_pi16 (res, 3)))) << A_SHIFT; \
									\
		++dest;							\
		++src;							\
		if (mask)						\
		    ++mask;						\
	    }								\
    }									\
									\
    static void								\
    mmx_combine_ ## name ## _ca (pixman_implementation_t *imp,		\
				 pixman_op_t              op,		\
				 uint32_t *                dest,	\
				 const uint32_t *          src,		\
				 const uint32_t *          mask,	\
				 int                     width)		\
    {									\
        uint32_t *end = dest + width;					\
									\
	while (dest < end)						\
	    {								\
		__m64 d64 = load8888 (dest);				\
		__m64 da64 = expand_alpha (d64);			\
		__m64 ida64 = _mm_xor_si64 (da64, MC (4x00ff));		\
		__m64 s64, m64, im64, res;				\
		uint32_t result;					\
		mmx_combine_mask_ca (src, mask, &s64, &m64);		\
		im64 = _mm_xor_si64 (m64, MC (4x00ff));			\
									\
		store8888 (&result, pix_add_mul (d64,im64,s64,ida64));	\
									\
		res = mmx_blend_ ## name (d64, da64, s64, m64);		\
		*dest = result;						\
		*dest += (uint32_t)(to_uint64 (loongson_extract_pi16 (res, 0))); \
		*dest += ((uint32_t)(to_uint64 (loongson_extract_pi16 (res, 1)))) << G_SHIFT; \
		*dest += ((uint32_t)(to_uint64 (loongson_extract_pi16 (res, 2)))) << R_SHIFT; \
		*dest += ((uint32_t)(to_uint64 (loongson_extract_pi16 (res, 3)))) << A_SHIFT; \
									\
		++dest;							\
		++src;							\
		if (mask)						\
		    ++mask;						\
	    }								\
    }		

/* DIV_ONE_UN8 */
static inline __m64
mmx_div_one_un8 (__m64 x)
{
    __m64 res;			     
    res = _mm_adds_pu16 (x, MC(4x0080));
    res = _mm_mulhi_pu16 (res, MC(4x0101));

    return res;
}

/* DIV_ONE_UN8 (x + y + z); */
/* 等价于 */
/* uint32_t hi = (x>>8) + (y>>8) + (z>>8); */
/* uint32_t lo = (x&0xff) + (y&0xff) + (z&0xff) + ONE_HALF; */
/* uint32_t res = hi + (lo>>8) + ((hi + (lo>>8) + (lo&0xff))>>8); */
/* 减法相当于加上其补码，需要注意移位时负数补1，其它补0 */
static inline __m64
mmx_div_one_un8_3p (__m64 x, __m64 y, __m64 z, __m64 cmpfy, __m64 cmpfz)
{
    __m64 hi = _mm_srli_pi16 (y, G_SHIFT);
    cmpfy = _mm_and_si64 (cmpfy, MC (4xff00));
    hi = _mm_add_pi16 (_mm_srli_pi16 (x, G_SHIFT), _mm_xor_si64 (hi, cmpfy));
    
    __m64 tmp = _mm_srli_pi16 (z, G_SHIFT);
    cmpfz =  _mm_and_si64 (cmpfz, MC (4xff00));
    hi = _mm_add_pi16 (hi, _mm_xor_si64(tmp, cmpfz));

    __m64 lo = _mm_add_pi16 (_mm_and_si64 (x, MC(4x00ff)),
			     _mm_and_si64 (y, MC(4x00ff)));
    tmp = _mm_add_pi16 (_mm_and_si64(z, MC (4x00ff)), MC (4x0080));
    lo = _mm_add_pi16 (lo, tmp);

    __m64 res = _mm_add_pi16 (hi, _mm_srli_pi16 (lo, G_SHIFT));
    
    tmp = _mm_add_pi16 (res, _mm_and_si64 (lo, MC (4x00ff)));
    __m64 cmpf = _mm_cmpgt_pi16 (0, tmp);
    tmp = _mm_srli_pi16 (tmp, G_SHIFT);
    cmpf = _mm_and_si64 (cmpf, MC (4xff00));
    tmp = _mm_xor_si64 (tmp, cmpf);
    res = _mm_add_pi16 (res, tmp);

    return res;
}

/* 根据cmpf标记从x中抽取cmpf中为1的位与从y中抽取为0的位组合成新的参数 */
static inline __m64
mmx_construct_with_cmpf (__m64 x, __m64 y, __m64 cmpf)
{
    __m64 neg_cmpf = _mm_xor_si64 (cmpf, MC (4xffff));
    __m64 res = _mm_or_si64 (_mm_and_si64 (x, cmpf),
			     _mm_and_si64 (y, neg_cmpf));
    return res;

}
    
/* DIV_ONE_UN8 (sca * da + dca * sa - sca * dca); */
static inline __m64
mmx_blend_screen (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    __m64 sca_da = _mm_mullo_pi16 (sca, da);
    
    /* A的计算方式与RGB的方式不同，所以这里需要保证mmx_div_one_un8_3p */
    /*第一个参数的高16位等于da*sa, 第二、三个参数的高16位为0 */
    dca = loongson_insert_pi16 (dca, 0x0, 3); /* 置0后dca_sa和sca_dca的最高16位将为0 */
    __m64 dca_sa = _mm_mullo_pi16 (dca, sa);
    __m64 sca_dca = _mm_mullo_pi16 (sca, dca);

    /* 控制mmx_div_one_un8_3p中移位后高位补位, 0被0，负数补1 */
    __m64 cmpf = _mm_cmpeq_pi16 (sca_dca, 0x0);
    cmpf = _mm_xor_si64 (cmpf, MC (4xffff));
    
    sca_dca = _mm_sub_pi16 (0x0, sca_dca); /* 此项肯定为负，因此取其补码 */

    return mmx_div_one_un8_3p (sca_da, dca_sa, sca_dca, 0x0, cmpf);
}

MMX_PDF_SEPARABLE_BLEND_MODE (screen)

/* if (2 * dca < da) */
/*     rca = 2 * sca * dca; */
/* 等价于： sca * dca + sca * dca + 0 */
/*  else */
/*      rca = sa * da - 2 * (da - dca) * (sa - sca); */
/* 等价于：  rca = sa * da +  (dca - da) * (sa - sca) +  (dca - da) * (sa - sca); */
static inline __m64
mmx_blend_overlay (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{

    /* 分支标记 (2*dca < da) 1->if 0->else*/
    __m64 cmpf_2dca_da = _mm_cmpgt_pi16 (da, _mm_slli_si64 (dca, 1)); 

    /* if 分支 */
    /* sa*da 的计算方式与RGB不同，需要将mmx_div_one_un8_3p的第一个参数的最高16位 */
    /* 设置为sa*da的结果，第二和第三个参数的最高16位设置为0。*/
    /* 由于if分支中的第一个参数是sca*dca，else分支中的第一个参数是sa * da，正好是所 */
    /* 需要的sa*da。因此无论走哪个分支，A都不用额外处理 */
    __m64 sca_dca = _mm_mullo_pi16 (sca, dca);

    /* else 分支 */
    __m64 sa_da = _mm_mullo_pi16 (sa, da); 

    /* 计算(dca - da)由于减法可能导致负数，负数则是用补码表示，因此要记录符号 */
    /* 注意：这里dca和da实际上只有8位，如果其实际有16位，则下面的做法可能导致错误 */
    /* 因为比较是带符号的比较 */
    __m64 dca_da = _mm_sub_pi16 (dca, da);
    __m64 cmpf_dca_da = _mm_cmpgt_pi16 (0x0, dca_da); /*1->negative 0->positive and 0*/

    /* 计算sa - sca */
    __m64 sa_sca = _mm_sub_pi16 (sa, sca);
    __m64 cmpf_sa_sca = _mm_cmpgt_pi16 (0x0, sa_sca);
    
    __m64 ddss = _mm_mullo_pi16 (dca_da, sa_sca);	     /* （dca - da) * (sa - sca) */
    __m64 cmpf_ddss = _mm_xor_si64 (_mm_cmpeq_pi16 (0x0, ddss), MC (4xffff)); /* 是否为0 */
    cmpf_ddss = _mm_and_si64 (cmpf_ddss, _mm_xor_si64 (cmpf_dca_da, cmpf_sa_sca));/* （dca - da) * (sa - sca)的符号，1=>负 0=>0或正*/

    /* 构造mmx_div_one_un8_3p的三个参数 */
    __m64 fir = mmx_construct_with_cmpf (sca_dca, sa_da, cmpf_2dca_da);
    __m64 sec = mmx_construct_with_cmpf (sca_dca, ddss, cmpf_2dca_da);
    __m64 trd = mmx_construct_with_cmpf (0x0, ddss, cmpf_2dca_da);
    /* 保证第二、三个参数的最高16位为0 */
    sec = loongson_insert_pi16 (sec, 0x0, 3);
    trd = loongson_insert_pi16 (trd, 0x0, 3);

    __m64 cmpf_sec = _mm_and_si64 (_mm_xor_si64 (cmpf_2dca_da, MC (4xffff)),
				   cmpf_ddss);
    __m64 cmpf_trd = cmpf_sec;
    
    return mmx_div_one_un8_3p (fir, sec, trd, cmpf_sec, cmpf_trd);
}

MMX_PDF_SEPARABLE_BLEND_MODE (overlay)

/* DIV_ONE_UN8 (s > d ? d : s); loongson多媒体比较指令是比较有符号数，所以这里不能用*/
/* 等价于s与d做无符号数减法，其结果为0时说明s<=d,否则s>d */
/* 根据此符号构造出传递给DIV_ONE_UN8的参数 */
/* 由于sca*da或者dca*sa的最高16位于da*sa的最高16位肯定一样，所以最高16位不用单独处理 */
static inline __m64
mmx_blend_darken (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    __m64 sca_da = _mm_mullo_pi16 (sca, da);
    __m64 dca_sa = _mm_mullo_pi16 (dca, sa);

    __m64 cmpf = _mm_cmpeq_pi16 (0x0, _mm_sub_pu16 (sca_da, dca_sa));

    sca_da = mmx_construct_with_cmpf (sca_da, dca_sa, cmpf);
    
    return mmx_div_one_un8 (sca_da);
}

MMX_PDF_SEPARABLE_BLEND_MODE (darken)

/* DIV_ONE_UN8 (s > d ? s : d); loongson多媒体比较指令是比较有符号数，所以这里不能用*/
/* 等价于s与d做无符号数减法，其结果为0时说明s<=d,否则s>d */
/* 根据此符号构造出传递给DIV_ONE_UN8的参数 */
/* 由于sca*da或者dca*sa的最高16位于da*sa的最高16位肯定一样，所以最高16位不用单独处理 */
static inline __m64
mmx_blend_lighten (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    __m64 sca_da = _mm_mullo_pi16 (sca, da);
    __m64 dca_sa = _mm_mullo_pi16 (dca, sa);

    __m64 cmpf = _mm_cmpeq_pi16 (0x0, _mm_sub_pu16 (sca_da, dca_sa));

    sca_da = mmx_construct_with_cmpf (dca_sa, sca_da, cmpf);
    
    return mmx_div_one_un8 (sca_da);
}

MMX_PDF_SEPARABLE_BLEND_MODE (lighten)

/* 原式中两个分支最终的结果都是sa * x的形式，所以只要组合好x就可以了。 */
/* 最后再来做乘法 */
static inline __m64
mmx_blend_color_dodge (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    uint8_t da8 = to_uint64 (da) & MASK;
    if (da8 == 0)
        return 0;

    /* cmpare flag */
    __m64 sa_sub_sca = _mm_sub_pu16 (sa, sca);
    __m64 cmpf_sca_sa = _mm_cmpeq_pi16 (0x0, sa_sub_sca); /* 1=>if 0=>else */

    /* if分支不用管 */
    __m64 cmpf_dca_0 = _mm_cmpeq_pi16 (0x0, dca);
    da = _mm_and_si64 (da, _mm_xor_si64 (cmpf_dca_0, MC (4xffff)));
    cmpf_sca_sa = _mm_or_si64 (cmpf_sca_sa, cmpf_dca_0);

    /* else */
    __m64 dca_sa = _mm_mullo_pi16 (dca, sa);
    __m64 sa_sca = _mm_sub_pi16 (sa, sca);
    __m64 divident, divisor;
    uint32_t i;

    for (i=0; i<3; i++)
        {
            /* dca 为0，sa * MIN(rca, da)必为0，因为rca=dca*sa/(sa-sca) */
            if (!_mm_cmpeq_pi16 (loongson_extract_pi16 (dca, i), 0x0))
                {
                    dca_sa = loongson_insert_pi16 (dca_sa, 0x0, i);
                    continue;
                }

            divident = loongson_extract_pi16 (sa_sca, i);
            if (_mm_cmpgt_pi16 (divident, 0x0))
                {
                    divisor = loongson_extract_pi16 (dca_sa, i);
                    uint32_t end = (uint32_t)(to_uint64 (divisor)) / (uint32_t)(to_uint64 (divident));
                    if (end > da8)
                        {
			    dca_sa = loongson_insert_pi16 (dca_sa,
							   loongson_extract_pi16 (da,i),
							   i);
                        }
                    else
                        {
                            uint64_t end64 = end;
			    dca_sa = loongson_insert_pi16 (dca_sa,
							   to_m64 (end64),
							   i);
                        }
                }
        }
    /* 组合sa * x中的x */
    __m64 sa_da = mmx_construct_with_cmpf (da, dca_sa, cmpf_sca_sa);
    sa_da = _mm_mullo_pi16 (sa, sa_da);

    return mmx_div_one_un8 (sa_da);
}

MMX_PDF_SEPARABLE_BLEND_MODE (color_dodge)

/* 按照优化前函数注释分析发现else分支返回值可修改为：*/
/* return  DIV_ONE_UN8 (sa * (da - MIN (da, rca))); */
/* 原式中有MAX，而当da小于dca时，其差值将为一个非常大的数 */
/* 从而无法用16位表示，也就无法使用多媒体指令来优化了 */
static inline __m64
mmx_blend_color_burn (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    /* 确定分支 1=>if 0=>else*/
    __m64 cmpf_sca_0 = _mm_cmpeq_pi16 (sca , 0);
    /* sa为0时，两个分支的计算结果都将为0，因此让其走第一个分支，避免复杂的计算 */
    /*  */
    __m64 cmpf_sa_0 = _mm_cmpeq_pi16 (sa, 0);
    cmpf_sca_0 = _mm_or_si64 (cmpf_sca_0, cmpf_sa_0);
    /* 处理最高16位,让其走第一个分支，保证计算结果为sa*da*/
    /*由于dca的最高位与da相等，因此if分支中的小于不成立，肯定走sa*da */
    cmpf_sca_0 = loongson_insert_pi16 (cmpf_sca_0, MC (4xffff), 3);
    
    /* if分支 */
    __m64 cmpf_dca_da = _mm_cmpgt_pi16 (da, dca);
    __m64 if_da = _mm_and_si64 (da, _mm_xor_si64 (cmpf_dca_da, MC (4xffff)));

    /* else分支 */
    __m64 da_dca = _mm_sub_pi16 (da, dca);
    __m64 rca = _mm_mullo_pi16 (da_dca, sa);
    __m64 divident, divisor;
    uint32_t i;

    for (i=0; i<3; i++)
        {
            if (!loongson_extract_pi16 (cmpf_sca_0, i))
                {
		    divisor = loongson_extract_pi16 (rca, i);
		    divident = loongson_extract_pi16 (sca, i);
	    
		    uint32_t end = (uint32_t)(to_uint64 (divisor)) / (uint32_t)(to_uint64 (divident));
		    uint64_t end64 = end;
		    rca = loongson_insert_pi16 (rca, to_m64 (end64), i);
		}
        }
    /* 求rca与da的最小值 */
    __m64 cmpf_else = _mm_cmpgt_pi16 (_mm_sub_pi16 (rca, da), 0);
    __m64 else_da = mmx_construct_with_cmpf (da, rca, cmpf_else);

    else_da = _mm_sub_pu16 (da, else_da);

    __m64 sa_da = mmx_construct_with_cmpf (if_da, else_da, cmpf_sca_0);

    sa_da = _mm_mullo_pi16 (sa, sa_da);

    return mmx_div_one_un8 (sa_da);
}

MMX_PDF_SEPARABLE_BLEND_MODE (color_burn)

static inline __m64
mmx_blend_hard_light (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{

    /* 分支标记 (2*dca < da) 1->if 0->else*/
    __m64 cmpf_2dca_da = _mm_cmpgt_pi16 (sa, _mm_slli_si64 (sca,1)); 

    /* if 分支 */
    /* sa*da 的计算方式与RGB不同，需要将mmx_div_one_un8_3p的第一个参数的最高16位 */
    /* 设置为sa*da的结果，第二和第三个参数的最高16位设置为0。*/
    /* 由于if分支中的第一个参数是sca*dca，else分支中的第一个参数是sa * da，正好是所 */
    /* 需要的sa*da。因此无论走哪个分支，A都不用额外处理 */
    __m64 sca_dca = _mm_mullo_pi16 (sca, dca);

    /* else 分支 */
    __m64 sa_da = _mm_mullo_pi16 (sa, da); 

    /* 计算(dca - da)由于减法可能导致负数，负数则是用补码表示，因此要记录符号 */
    /* 注意：这里dca和da实际上只有8位，如果其实际有16位，则下面的做法可能导致错误 */
    /* 因为比较是带符号的比较 */
    __m64 dca_da = _mm_sub_pi16 (dca, da);
    __m64 cmpf_dca_da = _mm_cmpgt_pi16 (0x0, dca_da); /*1->negative 0->positive and 0*/

    /* 计算sa - sca */
    __m64 sa_sca = _mm_sub_pi16 (sa, sca);
    __m64 cmpf_sa_sca = _mm_cmpgt_pi16 (0x0, sa_sca);
    
    __m64 ddss = _mm_mullo_pi16 (dca_da, sa_sca);	     /* （dca - da) * (sa - sca)的值 */
    __m64 cmpf_ddss = _mm_xor_si64 (_mm_cmpeq_pi16 (0x0, ddss), MC (4xffff));
    cmpf_ddss = _mm_and_si64 (cmpf_ddss, _mm_xor_si64 (cmpf_dca_da, cmpf_sa_sca));/* （dca - da) * (sa - sca)的符号，1=>负 0=>0或正 */

    __m64 fir = mmx_construct_with_cmpf (sca_dca, sa_da, cmpf_2dca_da);
    __m64 sec = mmx_construct_with_cmpf (sca_dca, ddss, cmpf_2dca_da);
    __m64 trd = mmx_construct_with_cmpf (0x0, ddss, cmpf_2dca_da);
    /* 保证第二、三个参数的最高16位为0 */
    sec = loongson_insert_pi16 (sec, 0x0, 3);
    trd = loongson_insert_pi16 (trd, 0x0, 3);
    __m64 cmpf_sec = _mm_and_si64 (_mm_xor_si64 (cmpf_2dca_da, MC  (4xffff)), cmpf_ddss);
    __m64 cmpf_trd = cmpf_sec;
    
    return mmx_div_one_un8_3p (fir, sec, trd, cmpf_sec, cmpf_trd);
}

MMX_PDF_SEPARABLE_BLEND_MODE (hard_light)

static inline __m64
mmx_blend_difference (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    __m64 dca_sa = _mm_mullo_pi16 (dca, sa);
    __m64 sca_da = _mm_mullo_pi16 (sca, da);

    __m64 cmpf = _mm_sub_pu16 (dca_sa, sca_da); 
    cmpf = _mm_cmpeq_pi16 (0x0, cmpf); /* 0=>if 1=>else */

    __m64 fir = mmx_construct_with_cmpf (sca_da, dca_sa, cmpf);
    __m64 sec = mmx_construct_with_cmpf (dca_sa, sca_da, cmpf);

    /* 处理最高16位 */
    sec = loongson_insert_pi16 (sec, 0x0, 3);

    return mmx_div_one_un8 (_mm_sub_pi16 (fir, sec));
}

MMX_PDF_SEPARABLE_BLEND_MODE (difference)

/* DIV_ONE_UN8 (sca * da + dca * sa - 2 * dca * sca); */
/* 等价于DIV_ONE_UN8 (sca * da + dca * (sa - sca)  - dca * sca); */
/* 从而可以使用mmx_div_one_un8_3p函数 */
static inline __m64
mmx_blend_exclusion (__m64 dca, __m64 da, __m64 sca, __m64 sa)
{
    __m64 fir = _mm_mullo_pi16 (sca, da);

    /* 用于保证第二、三个参数的高16位为0 */
    dca = loongson_insert_pi16 (dca, 0x0, 3);
    
    __m64 sec = _mm_sub_pi16 (sa, sca);
    /* 0移位与正数移位都应该补0，负数补1 */
    __m64 cmpf_sec = _mm_cmpgt_pi16 (0x0, sec); /* 1=>负 0=>正数或0 */
    sec = _mm_mullo_pi16 (dca, sec);
    cmpf_sec = _mm_and_si64 (cmpf_sec,
			     _mm_xor_si64 (_mm_cmpeq_pi16(0x0, sec), MC (4xffff)));

    __m64 trd = _mm_mullo_pi16 (dca, sca);
    __m64 cmpf_trd = _mm_xor_si64 (_mm_cmpeq_pi16(0x0, trd), MC (4xffff));
    /* 第三个参数做减法，相当于加上其补码 */
    trd = _mm_sub_pi16 (0x0, trd); 
    return mmx_div_one_un8_3p (fir, sec, trd, cmpf_sec, cmpf_trd);
}

MMX_PDF_SEPARABLE_BLEND_MODE (exclusion)

/* Component alpha combiners */
static void
mmx_combine_src_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);

	s = pix_multiply (s, a);
	store8888 (dest, s);

	++src;
	++mask;
	++dest;
    }
    _mm_empty ();
}

static void
mmx_combine_over_ca (pixman_implementation_t *imp,
                     pixman_op_t              op,
                     uint32_t *               dest,
                     const uint32_t *         src,
                     const uint32_t *         mask,
                     int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 sa = expand_alpha (s);

	store8888 (dest, in_over (s, sa, a, d));

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_over_reverse_ca (pixman_implementation_t *imp,
                             pixman_op_t              op,
                             uint32_t *               dest,
                             const uint32_t *         src,
                             const uint32_t *         mask,
                             int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 da = expand_alpha (d);

	store8888 (dest, over (d, da, in (s, a)));

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_in_ca (pixman_implementation_t *imp,
                   pixman_op_t              op,
                   uint32_t *               dest,
                   const uint32_t *         src,
                   const uint32_t *         mask,
                   int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 da = expand_alpha (d);

	s = pix_multiply (s, a);
	s = pix_multiply (s, da);
	store8888 (dest, s);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_in_reverse_ca (pixman_implementation_t *imp,
                           pixman_op_t              op,
                           uint32_t *               dest,
                           const uint32_t *         src,
                           const uint32_t *         mask,
                           int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 sa = expand_alpha (s);

	a = pix_multiply (a, sa);
	d = pix_multiply (d, a);
	store8888 (dest, d);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_out_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 da = expand_alpha (d);

	da = negate (da);
	s = pix_multiply (s, a);
	s = pix_multiply (s, da);
	store8888 (dest, s);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_out_reverse_ca (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               dest,
                            const uint32_t *         src,
                            const uint32_t *         mask,
                            int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 sa = expand_alpha (s);

	a = pix_multiply (a, sa);
	a = negate (a);
	d = pix_multiply (d, a);
	store8888 (dest, d);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_atop_ca (pixman_implementation_t *imp,
                     pixman_op_t              op,
                     uint32_t *               dest,
                     const uint32_t *         src,
                     const uint32_t *         mask,
                     int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 da = expand_alpha (d);
	__m64 sa = expand_alpha (s);

	s = pix_multiply (s, a);
	a = pix_multiply (a, sa);
	a = negate (a);
	d = pix_add_mul (d, a, s, da);
	store8888 (dest, d);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_atop_reverse_ca (pixman_implementation_t *imp,
                             pixman_op_t              op,
                             uint32_t *               dest,
                             const uint32_t *         src,
                             const uint32_t *         mask,
                             int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 da = expand_alpha (d);
	__m64 sa = expand_alpha (s);

	s = pix_multiply (s, a);
	a = pix_multiply (a, sa);
	da = negate (da);
	d = pix_add_mul (d, a, s, da);
	store8888 (dest, d);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_xor_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 da = expand_alpha (d);
	__m64 sa = expand_alpha (s);

	s = pix_multiply (s, a);
	a = pix_multiply (a, sa);
	da = negate (da);
	a = negate (a);
	d = pix_add_mul (d, a, s, da);
	store8888 (dest, d);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_add_ca (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dest,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    const uint32_t *end = src + width;

    while (src < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);

	s = pix_multiply (s, a);
	d = pix_add (s, d);
	store8888 (dest, d);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

static void
mmx_combine_saturate_ca (pixman_implementation_t *imp,
			 pixman_op_t              op,
			 uint32_t *                dest,
			 const uint32_t *          src,
			 const uint32_t *          mask,
			 int                      width)
{
    uint32_t *end = dest + width;
    while (dest < end)
	{
	    uint16_t sa, sr, sg, sb;
	    uint32_t sa32, m32;
	    __m64 m64, s64, d64, sa64, da64, cmpf, res;
	    
	    mmx_combine_mask_ca (src, mask, &s64, &m64);
	    
	    d64 = load8888 (dest);
	    da64 = expand_alpha (negate(d64));
	    /*这里涉及到分支,分支中的运算GENERIC实际上是先做乘法,再做ADD,*/
	    /*而另一分支是ADD,因此可以无条件地先判断,记下标记位,做乘法,*/
	    /*再将不需要做乘法的相应关字恢复,最后做加法. */
	    cmpf = _mm_cmpgt_pi16 (m64, da64);
	    if (cmpf)
		{
		    store8888 (&m32, m64);
		    sa = (m32 >> (A_SHIFT));
		    sr = (m32 >> (R_SHIFT)) & MASK;
		    sg = (m32 >> (G_SHIFT)) & MASK;
		    sb =  m32               & MASK;
		    sa32 = (~(*dest) >> A_SHIFT) & MASK;
		    
		    /* 可能为０，当为０时，其结果肯定将被恢复，因此这里为其随便赋一值 */
		    sa = (sa) ? sa : 0x1;
		    sr = (sr) ? sr : 0x1;
		    sg = (sg) ? sg : 0x1;
		    sb = (sb) ? sb : 0x1;
		    
		    sa32 = ((sa32 << G_SHIFT) / sb & MASK) |
			((((sa32 << G_SHIFT) / sg) & MASK) << G_SHIFT) |
			((((sa32 << G_SHIFT) / sr) & MASK) << R_SHIFT) |
			((((sa32 << G_SHIFT) / sa) & MASK) << A_SHIFT);
		    sa64 = load8888 (&sa32);
		    da64 = MC (4x00ff);
		    res = pix_multiply (s64, sa64);
		    s64 = _mm_or_si64 (_mm_and_si64 (res, cmpf), _mm_and_si64 (s64, negate (cmpf)));
		    res = pix_multiply (d64, da64);
		    d64 = _mm_or_si64 (_mm_and_si64 (res, cmpf), _mm_and_si64 (d64, negate (cmpf)));
		}
	    res = _mm_adds_pu8 (s64, d64);
	    store8888 (dest, res);
	    
	    ++dest;
	    ++src;
	    if (mask)
		++mask;
	}
}

#define DEF_FUNC_COMBINE_JOINT_CA(cd, io)				\
    static uint32_t inline combine_ ##cd## joint_ ##io## _part_ca(uint32_t sa, uint32_t da, uint32_t io_flag) \
    {									\
	uint8_t da8 = da >> A_SHIFT;					\
	uint32_t m, n, o, p, res;					\
	uint8_t i, parm[2][4], shift=0;					\
	for (i=0; i<4; i++)						\
	    {								\
		parm[0][i] = (uint8_t)(sa>>shift) * (io_flag ^ 0x1) + da8 * (io_flag ^ 0x0); \
		parm[1][i] = (uint8_t)(sa>>shift) * (io_flag ^ 0x0) + da8 * (io_flag ^ 0x1); \
		shift += G_SHIFT;					\
	    }								\
	m = (uint32_t)combine_ ##cd## joint_ ##io## _part (parm[0][0], parm[1][0]); \
	n = (uint32_t)combine_ ##cd## joint_ ##io## _part (parm[0][1], parm[1][1]) << G_SHIFT; \
	o = (uint32_t)combine_ ##cd## joint_ ##io## _part (parm[0][2], parm[1][2]) << R_SHIFT; \
	p = (uint32_t)combine_ ##cd## joint_ ##io## _part (parm[0][3], parm[1][3]) << A_SHIFT; \
	res = m | n | o | p;						\
	return res;							\
    }

typedef  uint32_t (*COMBINE_JOINT_FUNC_CA)(uint32_t sa, uint32_t da, uint32_t io_flag);

DEF_FUNC_ZERO_MASK(uint32_t, zero, ca, 0x0)
DEF_FUNC_ZERO_MASK(uint32_t, mask, ca, ~0x0)

DEF_FUNC_COMBINE_JOINT_CA(dis, in);
DEF_FUNC_COMBINE_JOINT_CA(dis, out);
DEF_COMB_FUNC_ARR(dis, CA, ca)

DEF_FUNC_COMBINE_JOINT_CA(con, in);
DEF_FUNC_COMBINE_JOINT_CA(con, out);
DEF_COMB_FUNC_ARR(con, CA, ca)

static void
mmx_combine_joint_general_ca (uint32_t * dest,
			      const uint32_t *src,
			      const uint32_t *mask,
			      int            width,
			      uint8_t        comb,
			      COMBINE_JOINT_FUNC_CA *cjf)
{
    COMBINE_JOINT_FUNC_CA combine_joint_ca[2];
    combine_joint_ca[0] = cjf[comb & COMBINE_A];
    combine_joint_ca[1] = cjf[(comb & COMBINE_B)>>2];
    
    uint32_t *end = dest + width;
    while (dest < end)
	{
	    __m64 m64, s64, sa64, da64, d64;
	    uint32_t m32, Fa, Fb;
	    
	    mmx_combine_mask_ca (src, mask, &s64, &m64);
	    store8888(&m32, m64);
	    
	    Fa = combine_joint_ca[0](m32, *dest, 0);
	    Fb = combine_joint_ca[1](m32, *dest, 1);
	    
	    sa64 = load8888 (&Fa);
	    da64 = load8888 (&Fb);
	    
	    d64 = load8888 (dest);
	    d64 = pix_add_mul(s64, sa64, d64, da64);
	    
	    store8888 (dest, d64);
	    
	    ++dest;
	    ++src;
	    if (mask)
		++mask;
	}
    
}

static void
mmx_combine_disjoint_general_ca (uint32_t * dest,
				 const uint32_t *src,
				 const uint32_t *mask,
				 int            width,
				 uint8_t        comb)
{
    mmx_combine_joint_general_ca (dest, src, mask, width, comb, combine_disjoint_ca);
}

static void
mmx_combine_disjoint_over_ca (pixman_implementation_t *imp,
			      pixman_op_t              op,
			      uint32_t *                dest,
			      const uint32_t *          src,
			      const uint32_t *          mask,
			      int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_A_OVER);
}

static void
mmx_combine_disjoint_in_ca (pixman_implementation_t *imp,
			    pixman_op_t              op,
			    uint32_t *                dest,
			    const uint32_t *          src,
			    const uint32_t *          mask,
			    int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_A_IN);
}

static void
mmx_combine_disjoint_in_reverse_ca (pixman_implementation_t *imp,
				    pixman_op_t              op,
				    uint32_t *                dest,
				    const uint32_t *          src,
				    const uint32_t *          mask,
				    int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_B_IN);
}

static void
mmx_combine_disjoint_out_ca (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_A_OUT);
}

static void
mmx_combine_disjoint_out_reverse_ca (pixman_implementation_t *imp,
				     pixman_op_t              op,
				     uint32_t *                dest,
				     const uint32_t *          src,
				     const uint32_t *          mask,
				     int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_B_OUT);
}

static void
mmx_combine_disjoint_atop_ca (pixman_implementation_t *imp,
			      pixman_op_t              op,
			      uint32_t *                dest,
			      const uint32_t *          src,
			      const uint32_t *          mask,
			      int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_A_ATOP);
}

static void
mmx_combine_disjoint_atop_reverse_ca (pixman_implementation_t *imp,
				      pixman_op_t              op,
				      uint32_t *                dest,
				      const uint32_t *          src,
				      const uint32_t *          mask,
				      int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_B_ATOP);
}

static void
mmx_combine_disjoint_xor_ca (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_disjoint_general_ca (dest, src, mask, width, COMBINE_XOR);
}

static void
mmx_combine_conjoint_general_ca(uint32_t * dest,
				const uint32_t *src,
				const uint32_t *mask,
				int            width,
				uint8_t        comb)
{
    mmx_combine_joint_general_ca(dest,src,mask,width,comb,combine_conjoint_ca);
}

static void
mmx_combine_conjoint_over_ca (pixman_implementation_t *imp,
			      pixman_op_t              op,
			      uint32_t *                dest,
			      const uint32_t *          src,
			      const uint32_t *          mask,
			      int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_A_OVER);
}

static void
mmx_combine_conjoint_over_reverse_ca (pixman_implementation_t *imp,
				      pixman_op_t              op,
				      uint32_t *                dest,
				      const uint32_t *          src,
				      const uint32_t *          mask,
				      int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_B_OVER);
}

static void
mmx_combine_conjoint_in_ca (pixman_implementation_t *imp,
			    pixman_op_t              op,
			    uint32_t *                dest,
			    const uint32_t *          src,
			    const uint32_t *          mask,
			    int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_A_IN);
}

static void
mmx_combine_conjoint_in_reverse_ca (pixman_implementation_t *imp,
				    pixman_op_t              op,
				    uint32_t *                dest,
				    const uint32_t *          src,
				    const uint32_t *          mask,
				    int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_B_IN);
}

static void
mmx_combine_conjoint_out_ca (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_A_OUT);
}

static void
mmx_combine_conjoint_out_reverse_ca (pixman_implementation_t *imp,
				     pixman_op_t              op,
				     uint32_t *                dest,
				     const uint32_t *          src,
				     const uint32_t *          mask,
				     int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_B_OUT);
}

static void
mmx_combine_conjoint_atop_ca (pixman_implementation_t *imp,
			      pixman_op_t              op,
			      uint32_t *                dest,
			      const uint32_t *          src,
			      const uint32_t *          mask,
			      int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_A_ATOP);
}

static void
mmx_combine_conjoint_atop_reverse_ca (pixman_implementation_t *imp,
				      pixman_op_t              op,
				      uint32_t *                dest,
				      const uint32_t *          src,
				      const uint32_t *          mask,
				      int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_B_ATOP);
}

static void
mmx_combine_conjoint_xor_ca (pixman_implementation_t *imp,
			     pixman_op_t              op,
			     uint32_t *                dest,
			     const uint32_t *          src,
			     const uint32_t *          mask,
			     int                      width)
{
    mmx_combine_conjoint_general_ca (dest, src, mask, width, COMBINE_XOR);
}

/*
 * Multiply
 * B(Dca, ad, Sca, as) = Dca.Sca
 */
 
static void
mmx_combine_multiply_u (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *                dest,
                    const uint32_t *          src,
                    const uint32_t *          mask,
                    int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 dia, d, sia;
	__m64 s = combine (src, mask); 
	__m64 ss = s;
	d = load8888 (dest);   
	sia = negate (expand_alpha (s));     
	dia = negate (expand_alpha (d));
	ss = pix_add_mul (ss, dia, d, sia);
	d = pix_multiply (d, s);
	d = pix_add (d, ss);	
	store8888 (dest, d);

	++dest;
	++src;
	if (mask)
		mask++;
    }
    _mm_empty ();
}
 
static void
mmx_combine_multiply_ca (pixman_implementation_t *imp,
                     pixman_op_t              op,
                     uint32_t *                dest,
                     const uint32_t *          src,
                     const uint32_t *          mask,
                     int                      width)
{
    const uint32_t *end = dest + width;

    while (dest < end)
    {
	__m64 a = load8888 (mask);
	__m64 s = load8888 (src);
	__m64 d = load8888 (dest);
	__m64 r = d;	
	__m64 da = negate (expand_alpha (d));
	__m64 sa = expand_alpha (s);
	s = pix_multiply (s, a);
	a = pix_multiply (a, sa);
	a = negate (a);
	r = pix_add_mul (r, a, s, da);
	d = pix_multiply (d, s);
	r = pix_add (r, d);
	store8888 (dest, r);

	++src;
	++dest;
	++mask;
    }
    _mm_empty ();
}

/* ------------- MMX code paths called from fbpict.c -------------------- */

static void
mmx_composite_over_n_8888 (pixman_implementation_t *imp,
                           pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src;
    uint32_t    *dst_line, *dst;
    int32_t w;
    int dst_stride;
    __m64 vsrc, vsrca;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    if (src == 0)
	return;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    store8888 (dst, over (vsrc, vsrca, load8888 (dst)));

	    w--;
	    dst++;
	}

	while (w >= 2)
	{
	    __m64 vdest;
	    __m64 dest0, dest1;

	    vdest = *(__m64 *)dst;

	    dest0 = over (vsrc, vsrca, expand8888 (vdest, 0));
	    dest1 = over (vsrc, vsrca, expand8888 (vdest, 1));

	    *(__m64 *)dst = pack8888 (dest0, dest1);

	    dst += 2;
	    w -= 2;
	}

	CHECKPOINT ();

	if (w)
	{
	    store8888 (dst, over (vsrc, vsrca, load8888 (dst)));
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_n_0565 (pixman_implementation_t *imp,
                           pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src;
    uint16_t    *dst_line, *dst;
    int32_t w;
    int dst_stride;
    __m64 vsrc, vsrca;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    if (src == 0)
	return;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    uint64_t d = *dst;
	    __m64 vdest = expand565 (to_m64 (d), 0);

	    vdest = pack_565 (over (vsrc, vsrca, vdest), vdest, 0);
	    *dst = to_uint64 (vdest);

	    w--;
	    dst++;
	}

	while (w >= 4)
	{
	    __m64 vdest = *(__m64 *)dst;
	    __m64 v0, v1, v2, v3;

	    expand_4x565 (vdest, &v0, &v1, &v2, &v3, 0);

	    v0 = over (vsrc, vsrca, v0);
	    v1 = over (vsrc, vsrca, v1);
	    v2 = over (vsrc, vsrca, v2);
	    v3 = over (vsrc, vsrca, v3);

	    *(__m64 *)dst = pack_4x565 (v0, v1, v2, v3);

	    dst += 4;
	    w -= 4;
	}

	CHECKPOINT ();

	while (w)
	{
	    uint64_t d = *dst;
	    __m64 vdest = expand565 (to_m64 (d), 0);

	    vdest = pack_565 (over (vsrc, vsrca, vdest), vdest, 0);
	    *dst = to_uint64 (vdest);

	    w--;
	    dst++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_n_8888_8888_ca (pixman_implementation_t *imp,
                                   pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src;
    uint32_t    *dst_line;
    uint32_t    *mask_line;
    int dst_stride, mask_stride;
    __m64 vsrc, vsrca;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    if (src == 0)
	return;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint32_t, mask_stride, mask_line, 1);

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	int twidth = width;
	uint32_t *p = (uint32_t *)mask_line;
	uint32_t *q = (uint32_t *)dst_line;

	while (twidth && (uintptr_t)q & 7)
	{
	    uint32_t m = *(uint32_t *)p;

	    if (m)
	    {
		__m64 vdest = load8888 (q);
		vdest = in_over (vsrc, vsrca, load8888 (&m), vdest);
		store8888 (q, vdest);
	    }

	    twidth--;
	    p++;
	    q++;
	}

	while (twidth >= 2)
	{
	    uint32_t m0, m1;
	    m0 = *p;
	    m1 = *(p + 1);

	    if (m0 | m1)
	    {
		__m64 dest0, dest1;
		__m64 vdest = *(__m64 *)q;

		dest0 = in_over (vsrc, vsrca, load8888 (&m0),
		                 expand8888 (vdest, 0));
		dest1 = in_over (vsrc, vsrca, load8888 (&m1),
		                 expand8888 (vdest, 1));

		*(__m64 *)q = pack8888 (dest0, dest1);
	    }

	    p += 2;
	    q += 2;
	    twidth -= 2;
	}

	if (twidth)
	{
	    uint32_t m = *(uint32_t *)p;

	    if (m)
	    {
		__m64 vdest = load8888 (q);
		vdest = in_over (vsrc, vsrca, load8888 (&m), vdest);
		store8888 (q, vdest);
	    }

	    twidth--;
	    p++;
	    q++;
	}

	dst_line += dst_stride;
	mask_line += mask_stride;
    }

    _mm_empty ();
}

static void
mmx_composite_over_8888_n_8888 (pixman_implementation_t *imp,
                                pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t    *dst_line, *dst;
    uint32_t    *src_line, *src;
    uint32_t mask;
    __m64 vmask;
    int dst_stride, src_stride;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

    mask = _pixman_image_get_solid (imp, mask_image, dest_image->bits.format);
    vmask = expand_alpha (load8888 (&mask));

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    __m64 s = load8888 (src);
	    __m64 d = load8888 (dst);

	    store8888 (dst, in_over (s, expand_alpha (s), vmask, d));

	    w--;
	    dst++;
	    src++;
	}

	while (w >= 2)
	{
	    __m64 vs = ldq_u ((__m64 *)src);
	    __m64 vd = *(__m64 *)dst;
	    __m64 vsrc0 = expand8888 (vs, 0);
	    __m64 vsrc1 = expand8888 (vs, 1);

	    *(__m64 *)dst = pack8888 (
	        in_over (vsrc0, expand_alpha (vsrc0), vmask, expand8888 (vd, 0)),
	        in_over (vsrc1, expand_alpha (vsrc1), vmask, expand8888 (vd, 1)));

	    w -= 2;
	    dst += 2;
	    src += 2;
	}

	if (w)
	{
	    __m64 s = load8888 (src);
	    __m64 d = load8888 (dst);

	    store8888 (dst, in_over (s, expand_alpha (s), vmask, d));
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_x888_n_8888 (pixman_implementation_t *imp,
                                pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t *dst_line, *dst;
    uint32_t *src_line, *src;
    uint32_t mask;
    __m64 vmask;
    int dst_stride, src_stride;
    int32_t w;
    __m64 srca;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);
    mask = _pixman_image_get_solid (imp, mask_image, dest_image->bits.format);

    vmask = expand_alpha (load8888 (&mask));
    srca = MC (4x00ff);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    uint32_t ssrc = *src | 0xff000000;
	    __m64 s = load8888 (&ssrc);
	    __m64 d = load8888 (dst);

	    store8888 (dst, in_over (s, srca, vmask, d));

	    w--;
	    dst++;
	    src++;
	}

	while (w >= 16)
	{
	    __m64 vd0 = *(__m64 *)(dst + 0);
	    __m64 vd1 = *(__m64 *)(dst + 2);
	    __m64 vd2 = *(__m64 *)(dst + 4);
	    __m64 vd3 = *(__m64 *)(dst + 6);
	    __m64 vd4 = *(__m64 *)(dst + 8);
	    __m64 vd5 = *(__m64 *)(dst + 10);
	    __m64 vd6 = *(__m64 *)(dst + 12);
	    __m64 vd7 = *(__m64 *)(dst + 14);

	    __m64 vs0 = ldq_u ((__m64 *)(src + 0));
	    __m64 vs1 = ldq_u ((__m64 *)(src + 2));
	    __m64 vs2 = ldq_u ((__m64 *)(src + 4));
	    __m64 vs3 = ldq_u ((__m64 *)(src + 6));
	    __m64 vs4 = ldq_u ((__m64 *)(src + 8));
	    __m64 vs5 = ldq_u ((__m64 *)(src + 10));
	    __m64 vs6 = ldq_u ((__m64 *)(src + 12));
	    __m64 vs7 = ldq_u ((__m64 *)(src + 14));

	    vd0 = pack8888 (
	        in_over (expandx888 (vs0, 0), srca, vmask, expand8888 (vd0, 0)),
	        in_over (expandx888 (vs0, 1), srca, vmask, expand8888 (vd0, 1)));

	    vd1 = pack8888 (
	        in_over (expandx888 (vs1, 0), srca, vmask, expand8888 (vd1, 0)),
	        in_over (expandx888 (vs1, 1), srca, vmask, expand8888 (vd1, 1)));

	    vd2 = pack8888 (
	        in_over (expandx888 (vs2, 0), srca, vmask, expand8888 (vd2, 0)),
	        in_over (expandx888 (vs2, 1), srca, vmask, expand8888 (vd2, 1)));

	    vd3 = pack8888 (
	        in_over (expandx888 (vs3, 0), srca, vmask, expand8888 (vd3, 0)),
	        in_over (expandx888 (vs3, 1), srca, vmask, expand8888 (vd3, 1)));

	    vd4 = pack8888 (
	        in_over (expandx888 (vs4, 0), srca, vmask, expand8888 (vd4, 0)),
	        in_over (expandx888 (vs4, 1), srca, vmask, expand8888 (vd4, 1)));

	    vd5 = pack8888 (
	        in_over (expandx888 (vs5, 0), srca, vmask, expand8888 (vd5, 0)),
	        in_over (expandx888 (vs5, 1), srca, vmask, expand8888 (vd5, 1)));

	    vd6 = pack8888 (
	        in_over (expandx888 (vs6, 0), srca, vmask, expand8888 (vd6, 0)),
	        in_over (expandx888 (vs6, 1), srca, vmask, expand8888 (vd6, 1)));

	    vd7 = pack8888 (
	        in_over (expandx888 (vs7, 0), srca, vmask, expand8888 (vd7, 0)),
	        in_over (expandx888 (vs7, 1), srca, vmask, expand8888 (vd7, 1)));

	    *(__m64 *)(dst + 0) = vd0;
	    *(__m64 *)(dst + 2) = vd1;
	    *(__m64 *)(dst + 4) = vd2;
	    *(__m64 *)(dst + 6) = vd3;
	    *(__m64 *)(dst + 8) = vd4;
	    *(__m64 *)(dst + 10) = vd5;
	    *(__m64 *)(dst + 12) = vd6;
	    *(__m64 *)(dst + 14) = vd7;

	    w -= 16;
	    dst += 16;
	    src += 16;
	}

	while (w)
	{
	    uint32_t ssrc = *src | 0xff000000;
	    __m64 s = load8888 (&ssrc);
	    __m64 d = load8888 (dst);

	    store8888 (dst, in_over (s, srca, vmask, d));

	    w--;
	    dst++;
	    src++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_8888_8888 (pixman_implementation_t *imp,
                              pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t *dst_line, *dst;
    uint32_t *src_line, *src;
    uint32_t s;
    int dst_stride, src_stride;
    uint8_t a;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w--)
	{
	    s = *src++;
	    a = s >> 24;

	    if (a == 0xff)
	    {
		*dst = s;
	    }
	    else if (s)
	    {
		__m64 ms, sa;
		ms = load8888 (&s);
		sa = expand_alpha (ms);
		store8888 (dst, over (ms, sa, load8888 (dst)));
	    }

	    dst++;
	}
    }
    _mm_empty ();
}

static void
mmx_composite_over_8888_0565 (pixman_implementation_t *imp,
                              pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint16_t    *dst_line, *dst;
    uint32_t    *src_line, *src;
    int dst_stride, src_stride;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

#if 0
    /* FIXME */
    assert (src_image->drawable == mask_image->drawable);
#endif

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    __m64 vsrc = load8888 (src);
	    uint64_t d = *dst;
	    __m64 vdest = expand565 (to_m64 (d), 0);

	    vdest = pack_565 (
		over (vsrc, expand_alpha (vsrc), vdest), vdest, 0);

	    *dst = to_uint64 (vdest);

	    w--;
	    dst++;
	    src++;
	}

	CHECKPOINT ();

	while (w >= 4)
	{
	    __m64 vdest = *(__m64 *)dst;
	    __m64 v0, v1, v2, v3;
	    __m64 vsrc0, vsrc1, vsrc2, vsrc3;

	    expand_4x565 (vdest, &v0, &v1, &v2, &v3, 0);

	    vsrc0 = load8888 ((src + 0));
	    vsrc1 = load8888 ((src + 1));
	    vsrc2 = load8888 ((src + 2));
	    vsrc3 = load8888 ((src + 3));

	    v0 = over (vsrc0, expand_alpha (vsrc0), v0);
	    v1 = over (vsrc1, expand_alpha (vsrc1), v1);
	    v2 = over (vsrc2, expand_alpha (vsrc2), v2);
	    v3 = over (vsrc3, expand_alpha (vsrc3), v3);

	    *(__m64 *)dst = pack_4x565 (v0, v1, v2, v3);

	    w -= 4;
	    dst += 4;
	    src += 4;
	}

	CHECKPOINT ();

	while (w)
	{
	    __m64 vsrc = load8888 (src);
	    uint64_t d = *dst;
	    __m64 vdest = expand565 (to_m64 (d), 0);

	    vdest = pack_565 (over (vsrc, expand_alpha (vsrc), vdest), vdest, 0);

	    *dst = to_uint64 (vdest);

	    w--;
	    dst++;
	    src++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_n_8_8888 (pixman_implementation_t *imp,
                             pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src, srca;
    uint32_t *dst_line, *dst;
    uint8_t *mask_line, *mask;
    int dst_stride, mask_stride;
    int32_t w;
    __m64 vsrc, vsrca;
    uint64_t srcsrc;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    srca = src >> 24;
    if (src == 0)
	return;

    srcsrc = (uint64_t)src << 32 | src;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint8_t, mask_stride, mask_line, 1);

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	mask = mask_line;
	mask_line += mask_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		__m64 vdest = in_over (vsrc, vsrca,
				       expand_alpha_rev (to_m64 (m)),
				       load8888 (dst));

		store8888 (dst, vdest);
	    }

	    w--;
	    mask++;
	    dst++;
	}

	CHECKPOINT ();

	while (w >= 2)
	{
	    uint64_t m0, m1;

	    m0 = *mask;
	    m1 = *(mask + 1);

	    if (srca == 0xff && (m0 & m1) == 0xff)
	    {
		*(uint64_t *)dst = srcsrc;
	    }
	    else if (m0 | m1)
	    {
		__m64 vdest;
		__m64 dest0, dest1;

		vdest = *(__m64 *)dst;

		dest0 = in_over (vsrc, vsrca, expand_alpha_rev (to_m64 (m0)),
				 expand8888 (vdest, 0));
		dest1 = in_over (vsrc, vsrca, expand_alpha_rev (to_m64 (m1)),
				 expand8888 (vdest, 1));

		*(__m64 *)dst = pack8888 (dest0, dest1);
	    }

	    mask += 2;
	    dst += 2;
	    w -= 2;
	}

	CHECKPOINT ();

	if (w)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		__m64 vdest = load8888 (dst);

		vdest = in_over (
		    vsrc, vsrca, expand_alpha_rev (to_m64 (m)), vdest);
		store8888 (dst, vdest);
	    }
	}
    }

    _mm_empty ();
}

static pixman_bool_t
mmx_fill (pixman_implementation_t *imp,
          uint32_t *               bits,
          int                      stride,
          int                      bpp,
          int                      x,
          int                      y,
          int                      width,
          int                      height,
          uint32_t		   filler)
{
    uint64_t fill;
    __m64 vfill;
    uint32_t byte_width;
    uint8_t     *byte_line;

#if defined __GNUC__ && defined USE_X86_MMX
    __m64 v1, v2, v3, v4, v5, v6, v7;
#endif

    if (bpp != 16 && bpp != 32 && bpp != 8)
	return FALSE;

    if (bpp == 8)
    {
	stride = stride * (int) sizeof (uint32_t) / 1;
	byte_line = (uint8_t *)(((uint8_t *)bits) + stride * y + x);
	byte_width = width;
	stride *= 1;
        filler = (filler & 0xff) * 0x01010101;
    }
    else if (bpp == 16)
    {
	stride = stride * (int) sizeof (uint32_t) / 2;
	byte_line = (uint8_t *)(((uint16_t *)bits) + stride * y + x);
	byte_width = 2 * width;
	stride *= 2;
        filler = (filler & 0xffff) * 0x00010001;
    }
    else
    {
	stride = stride * (int) sizeof (uint32_t) / 4;
	byte_line = (uint8_t *)(((uint32_t *)bits) + stride * y + x);
	byte_width = 4 * width;
	stride *= 4;
    }

    fill = ((uint64_t)filler << 32) | filler;
    vfill = to_m64 (fill);

#if defined __GNUC__ && defined USE_X86_MMX
    __asm__ (
        "movq		%7,	%0\n"
        "movq		%7,	%1\n"
        "movq		%7,	%2\n"
        "movq		%7,	%3\n"
        "movq		%7,	%4\n"
        "movq		%7,	%5\n"
        "movq		%7,	%6\n"
	: "=&y" (v1), "=&y" (v2), "=&y" (v3),
	  "=&y" (v4), "=&y" (v5), "=&y" (v6), "=y" (v7)
	: "y" (vfill));
#endif

    while (height--)
    {
	int w;
	uint8_t *d = byte_line;

	byte_line += stride;
	w = byte_width;

	if (w >= 1 && ((uintptr_t)d & 1))
	{
	    *(uint8_t *)d = (filler & 0xff);
	    w--;
	    d++;
	}

	if (w >= 2 && ((uintptr_t)d & 3))
	{
	    *(uint16_t *)d = filler;
	    w -= 2;
	    d += 2;
	}

	while (w >= 4 && ((uintptr_t)d & 7))
	{
	    *(uint32_t *)d = filler;

	    w -= 4;
	    d += 4;
	}

	while (w >= 64)
	{
#if defined __GNUC__ && defined USE_X86_MMX
	    __asm__ (
	        "movq	%1,	  (%0)\n"
	        "movq	%2,	 8(%0)\n"
	        "movq	%3,	16(%0)\n"
	        "movq	%4,	24(%0)\n"
	        "movq	%5,	32(%0)\n"
	        "movq	%6,	40(%0)\n"
	        "movq	%7,	48(%0)\n"
	        "movq	%8,	56(%0)\n"
		:
		: "r" (d),
		  "y" (vfill), "y" (v1), "y" (v2), "y" (v3),
		  "y" (v4), "y" (v5), "y" (v6), "y" (v7)
		: "memory");
#else
	    *(__m64*) (d +  0) = vfill;
	    *(__m64*) (d +  8) = vfill;
	    *(__m64*) (d + 16) = vfill;
	    *(__m64*) (d + 24) = vfill;
	    *(__m64*) (d + 32) = vfill;
	    *(__m64*) (d + 40) = vfill;
	    *(__m64*) (d + 48) = vfill;
	    *(__m64*) (d + 56) = vfill;
#endif
	    w -= 64;
	    d += 64;
	}

	while (w >= 4)
	{
	    *(uint32_t *)d = filler;

	    w -= 4;
	    d += 4;
	}
	if (w >= 2)
	{
	    *(uint16_t *)d = filler;
	    w -= 2;
	    d += 2;
	}
	if (w >= 1)
	{
	    *(uint8_t *)d = (filler & 0xff);
	    w--;
	    d++;
	}

    }

    _mm_empty ();
    return TRUE;
}

static void
mmx_composite_src_x888_0565 (pixman_implementation_t *imp,
                             pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint16_t    *dst_line, *dst;
    uint32_t    *src_line, *src, s;
    int dst_stride, src_stride;
    int32_t w;

    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);
    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    s = *src++;
	    *dst = convert_8888_to_0565 (s);
	    dst++;
	    w--;
	}

	while (w >= 4)
	{
	    __m64 vdest;
	    __m64 vsrc0 = ldq_u ((__m64 *)(src + 0));
	    __m64 vsrc1 = ldq_u ((__m64 *)(src + 2));

	    vdest = pack_4xpacked565 (vsrc0, vsrc1);

	    *(__m64 *)dst = vdest;

	    w -= 4;
	    src += 4;
	    dst += 4;
	}

	while (w)
	{
	    s = *src++;
	    *dst = convert_8888_to_0565 (s);
	    dst++;
	    w--;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_src_n_8_8888 (pixman_implementation_t *imp,
                            pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src, srca;
    uint32_t    *dst_line, *dst;
    uint8_t     *mask_line, *mask;
    int dst_stride, mask_stride;
    int32_t w;
    __m64 vsrc;
    uint64_t srcsrc;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    srca = src >> 24;
    if (src == 0)
    {
	mmx_fill (imp, dest_image->bits.bits, dest_image->bits.rowstride,
		  PIXMAN_FORMAT_BPP (dest_image->bits.format),
		  dest_x, dest_y, width, height, 0);
	return;
    }

    srcsrc = (uint64_t)src << 32 | src;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint8_t, mask_stride, mask_line, 1);

    vsrc = load8888 (&src);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	mask = mask_line;
	mask_line += mask_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		__m64 vdest = in (vsrc, expand_alpha_rev (to_m64 (m)));

		store8888 (dst, vdest);
	    }
	    else
	    {
		*dst = 0;
	    }

	    w--;
	    mask++;
	    dst++;
	}

	CHECKPOINT ();

	while (w >= 2)
	{
	    uint64_t m0, m1;
	    m0 = *mask;
	    m1 = *(mask + 1);

	    if (srca == 0xff && (m0 & m1) == 0xff)
	    {
		*(uint64_t *)dst = srcsrc;
	    }
	    else if (m0 | m1)
	    {
		__m64 dest0, dest1;

		dest0 = in (vsrc, expand_alpha_rev (to_m64 (m0)));
		dest1 = in (vsrc, expand_alpha_rev (to_m64 (m1)));

		*(__m64 *)dst = pack8888 (dest0, dest1);
	    }
	    else
	    {
		*(uint64_t *)dst = 0;
	    }

	    mask += 2;
	    dst += 2;
	    w -= 2;
	}

	CHECKPOINT ();

	if (w)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		__m64 vdest = load8888 (dst);

		vdest = in (vsrc, expand_alpha_rev (to_m64 (m)));
		store8888 (dst, vdest);
	    }
	    else
	    {
		*dst = 0;
	    }
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_n_8_0565 (pixman_implementation_t *imp,
                             pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src, srca;
    uint16_t *dst_line, *dst;
    uint8_t *mask_line, *mask;
    int dst_stride, mask_stride;
    int32_t w;
    __m64 vsrc, vsrca, tmp;
    __m64 srcsrcsrcsrc;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    srca = src >> 24;
    if (src == 0)
	return;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint8_t, mask_stride, mask_line, 1);

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    tmp = pack_565 (vsrc, _mm_setzero_si64 (), 0);
    srcsrcsrcsrc = expand_alpha_rev (tmp);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	mask = mask_line;
	mask_line += mask_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		uint64_t d = *dst;
		__m64 vd = to_m64 (d);
		__m64 vdest = in_over (
		    vsrc, vsrca, expand_alpha_rev (to_m64 (m)), expand565 (vd, 0));

		vd = pack_565 (vdest, _mm_setzero_si64 (), 0);
		*dst = to_uint64 (vd);
	    }

	    w--;
	    mask++;
	    dst++;
	}

	CHECKPOINT ();

	while (w >= 4)
	{
	    uint64_t m0, m1, m2, m3;
	    m0 = *mask;
	    m1 = *(mask + 1);
	    m2 = *(mask + 2);
	    m3 = *(mask + 3);

	    if (srca == 0xff && (m0 & m1 & m2 & m3) == 0xff)
	    {
		*(__m64 *)dst = srcsrcsrcsrc;
	    }
	    else if (m0 | m1 | m2 | m3)
	    {
		__m64 vdest = *(__m64 *)dst;
		__m64 v0, v1, v2, v3;
		__m64 vm0, vm1, vm2, vm3;

		expand_4x565 (vdest, &v0, &v1, &v2, &v3, 0);

		vm0 = to_m64 (m0);
		v0 = in_over (vsrc, vsrca, expand_alpha_rev (vm0), v0);

		vm1 = to_m64 (m1);
		v1 = in_over (vsrc, vsrca, expand_alpha_rev (vm1), v1);

		vm2 = to_m64 (m2);
		v2 = in_over (vsrc, vsrca, expand_alpha_rev (vm2), v2);

		vm3 = to_m64 (m3);
		v3 = in_over (vsrc, vsrca, expand_alpha_rev (vm3), v3);

		*(__m64 *)dst = pack_4x565 (v0, v1, v2, v3);;
	    }

	    w -= 4;
	    mask += 4;
	    dst += 4;
	}

	CHECKPOINT ();

	while (w)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		uint64_t d = *dst;
		__m64 vd = to_m64 (d);
		__m64 vdest = in_over (vsrc, vsrca, expand_alpha_rev (to_m64 (m)),
				       expand565 (vd, 0));
		vd = pack_565 (vdest, _mm_setzero_si64 (), 0);
		*dst = to_uint64 (vd);
	    }

	    w--;
	    mask++;
	    dst++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_pixbuf_0565 (pixman_implementation_t *imp,
                                pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint16_t    *dst_line, *dst;
    uint32_t    *src_line, *src;
    int dst_stride, src_stride;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

#if 0
    /* FIXME */
    assert (src_image->drawable == mask_image->drawable);
#endif

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    __m64 vsrc = load8888 (src);
	    uint64_t d = *dst;
	    __m64 vdest = expand565 (to_m64 (d), 0);

	    vdest = pack_565 (over_rev_non_pre (vsrc, vdest), vdest, 0);

	    *dst = to_uint64 (vdest);

	    w--;
	    dst++;
	    src++;
	}

	CHECKPOINT ();

	while (w >= 4)
	{
	    uint32_t s0, s1, s2, s3;
	    unsigned char a0, a1, a2, a3;

	    s0 = *src;
	    s1 = *(src + 1);
	    s2 = *(src + 2);
	    s3 = *(src + 3);

	    a0 = (s0 >> 24);
	    a1 = (s1 >> 24);
	    a2 = (s2 >> 24);
	    a3 = (s3 >> 24);

	    if ((a0 & a1 & a2 & a3) == 0xFF)
	    {
		__m64 v0 = invert_colors (load8888 (&s0));
		__m64 v1 = invert_colors (load8888 (&s1));
		__m64 v2 = invert_colors (load8888 (&s2));
		__m64 v3 = invert_colors (load8888 (&s3));

		*(__m64 *)dst = pack_4x565 (v0, v1, v2, v3);
	    }
	    else if (s0 | s1 | s2 | s3)
	    {
		__m64 vdest = *(__m64 *)dst;
		__m64 v0, v1, v2, v3;

		__m64 vsrc0 = load8888 (&s0);
		__m64 vsrc1 = load8888 (&s1);
		__m64 vsrc2 = load8888 (&s2);
		__m64 vsrc3 = load8888 (&s3);

		expand_4x565 (vdest, &v0, &v1, &v2, &v3, 0);

		v0 = over_rev_non_pre (vsrc0, v0);
		v1 = over_rev_non_pre (vsrc1, v1);
		v2 = over_rev_non_pre (vsrc2, v2);
		v3 = over_rev_non_pre (vsrc3, v3);

		*(__m64 *)dst = pack_4x565 (v0, v1, v2, v3);
	    }

	    w -= 4;
	    dst += 4;
	    src += 4;
	}

	CHECKPOINT ();

	while (w)
	{
	    __m64 vsrc = load8888 (src);
	    uint64_t d = *dst;
	    __m64 vdest = expand565 (to_m64 (d), 0);

	    vdest = pack_565 (over_rev_non_pre (vsrc, vdest), vdest, 0);

	    *dst = to_uint64 (vdest);

	    w--;
	    dst++;
	    src++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_pixbuf_8888 (pixman_implementation_t *imp,
                                pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t    *dst_line, *dst;
    uint32_t    *src_line, *src;
    int dst_stride, src_stride;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

#if 0
    /* FIXME */
    assert (src_image->drawable == mask_image->drawable);
#endif

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    __m64 s = load8888 (src);
	    __m64 d = load8888 (dst);

	    store8888 (dst, over_rev_non_pre (s, d));

	    w--;
	    dst++;
	    src++;
	}

	while (w >= 2)
	{
	    uint32_t s0, s1;
	    unsigned char a0, a1;
	    __m64 d0, d1;

	    s0 = *src;
	    s1 = *(src + 1);

	    a0 = (s0 >> 24);
	    a1 = (s1 >> 24);

	    if ((a0 & a1) == 0xFF)
	    {
		d0 = invert_colors (load8888 (&s0));
		d1 = invert_colors (load8888 (&s1));

		*(__m64 *)dst = pack8888 (d0, d1);
	    }
	    else if (s0 | s1)
	    {
		__m64 vdest = *(__m64 *)dst;

		d0 = over_rev_non_pre (load8888 (&s0), expand8888 (vdest, 0));
		d1 = over_rev_non_pre (load8888 (&s1), expand8888 (vdest, 1));

		*(__m64 *)dst = pack8888 (d0, d1);
	    }

	    w -= 2;
	    dst += 2;
	    src += 2;
	}

	if (w)
	{
	    __m64 s = load8888 (src);
	    __m64 d = load8888 (dst);

	    store8888 (dst, over_rev_non_pre (s, d));
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_n_8888_0565_ca (pixman_implementation_t *imp,
                                   pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src;
    uint16_t    *dst_line;
    uint32_t    *mask_line;
    int dst_stride, mask_stride;
    __m64 vsrc, vsrca;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    if (src == 0)
	return;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint32_t, mask_stride, mask_line, 1);

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	int twidth = width;
	uint32_t *p = (uint32_t *)mask_line;
	uint16_t *q = (uint16_t *)dst_line;

	while (twidth && ((uintptr_t)q & 7))
	{
	    uint32_t m = *(uint32_t *)p;

	    if (m)
	    {
		uint64_t d = *q;
		__m64 vdest = expand565 (to_m64 (d), 0);
		vdest = pack_565 (in_over (vsrc, vsrca, load8888 (&m), vdest), vdest, 0);
		*q = to_uint64 (vdest);
	    }

	    twidth--;
	    p++;
	    q++;
	}

	while (twidth >= 4)
	{
	    uint32_t m0, m1, m2, m3;

	    m0 = *p;
	    m1 = *(p + 1);
	    m2 = *(p + 2);
	    m3 = *(p + 3);

	    if ((m0 | m1 | m2 | m3))
	    {
		__m64 vdest = *(__m64 *)q;
		__m64 v0, v1, v2, v3;

		expand_4x565 (vdest, &v0, &v1, &v2, &v3, 0);

		v0 = in_over (vsrc, vsrca, load8888 (&m0), v0);
		v1 = in_over (vsrc, vsrca, load8888 (&m1), v1);
		v2 = in_over (vsrc, vsrca, load8888 (&m2), v2);
		v3 = in_over (vsrc, vsrca, load8888 (&m3), v3);

		*(__m64 *)q = pack_4x565 (v0, v1, v2, v3);
	    }
	    twidth -= 4;
	    p += 4;
	    q += 4;
	}

	while (twidth)
	{
	    uint32_t m;

	    m = *(uint32_t *)p;
	    if (m)
	    {
		uint64_t d = *q;
		__m64 vdest = expand565 (to_m64 (d), 0);
		vdest = pack_565 (in_over (vsrc, vsrca, load8888 (&m), vdest), vdest, 0);
		*q = to_uint64 (vdest);
	    }

	    twidth--;
	    p++;
	    q++;
	}

	mask_line += mask_stride;
	dst_line += dst_stride;
    }

    _mm_empty ();
}

static void
mmx_composite_in_n_8_8 (pixman_implementation_t *imp,
                        pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint8_t *dst_line, *dst;
    uint8_t *mask_line, *mask;
    int dst_stride, mask_stride;
    int32_t w;
    uint32_t src;
    uint8_t sa;
    __m64 vsrc, vsrca;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint8_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint8_t, mask_stride, mask_line, 1);

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    sa = src >> 24;

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	mask = mask_line;
	mask_line += mask_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    uint16_t tmp;
	    uint8_t a;
	    uint32_t m, d;

	    a = *mask++;
	    d = *dst;

	    m = MUL_UN8 (sa, a, tmp);
	    d = MUL_UN8 (m, d, tmp);

	    *dst++ = d;
	    w--;
	}

	while (w >= 4)
	{
	    __m64 vmask;
	    __m64 vdest;

	    vmask = load8888u ((uint32_t *)mask);
	    vdest = load8888 ((uint32_t *)dst);

	    store8888 ((uint32_t *)dst, in (in (vsrca, vmask), vdest));

	    dst += 4;
	    mask += 4;
	    w -= 4;
	}

	while (w--)
	{
	    uint16_t tmp;
	    uint8_t a;
	    uint32_t m, d;

	    a = *mask++;
	    d = *dst;

	    m = MUL_UN8 (sa, a, tmp);
	    d = MUL_UN8 (m, d, tmp);

	    *dst++ = d;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_in_8_8 (pixman_implementation_t *imp,
                      pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint8_t     *dst_line, *dst;
    uint8_t     *src_line, *src;
    int src_stride, dst_stride;
    int32_t w;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint8_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint8_t, src_stride, src_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 3)
	{
	    uint8_t s, d;
	    uint16_t tmp;

	    s = *src;
	    d = *dst;

	    *dst = MUL_UN8 (s, d, tmp);

	    src++;
	    dst++;
	    w--;
	}

	while (w >= 4)
	{
	    uint32_t *s = (uint32_t *)src;
	    uint32_t *d = (uint32_t *)dst;

	    store8888 (d, in (load8888u (s), load8888 (d)));

	    w -= 4;
	    dst += 4;
	    src += 4;
	}

	while (w--)
	{
	    uint8_t s, d;
	    uint16_t tmp;

	    s = *src;
	    d = *dst;

	    *dst = MUL_UN8 (s, d, tmp);

	    src++;
	    dst++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_add_n_8_8 (pixman_implementation_t *imp,
			 pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint8_t     *dst_line, *dst;
    uint8_t     *mask_line, *mask;
    int dst_stride, mask_stride;
    int32_t w;
    uint32_t src;
    uint8_t sa;
    __m64 vsrc, vsrca;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint8_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint8_t, mask_stride, mask_line, 1);

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    sa = src >> 24;

    if (src == 0)
	return;

    vsrc = load8888 (&src);
    vsrca = expand_alpha (vsrc);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	mask = mask_line;
	mask_line += mask_stride;
	w = width;

	while (w && (uintptr_t)dst & 3)
	{
	    uint16_t tmp;
	    uint16_t a;
	    uint32_t m, d;
	    uint32_t r;

	    a = *mask++;
	    d = *dst;

	    m = MUL_UN8 (sa, a, tmp);
	    r = ADD_UN8 (m, d, tmp);

	    *dst++ = r;
	    w--;
	}

	while (w >= 4)
	{
	    __m64 vmask;
	    __m64 vdest;

	    vmask = load8888u ((uint32_t *)mask);
	    vdest = load8888 ((uint32_t *)dst);

	    store8888 ((uint32_t *)dst, _mm_adds_pu8 (in (vsrca, vmask), vdest));

	    dst += 4;
	    mask += 4;
	    w -= 4;
	}

	while (w--)
	{
	    uint16_t tmp;
	    uint16_t a;
	    uint32_t m, d;
	    uint32_t r;

	    a = *mask++;
	    d = *dst;

	    m = MUL_UN8 (sa, a, tmp);
	    r = ADD_UN8 (m, d, tmp);

	    *dst++ = r;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_add_8_8 (pixman_implementation_t *imp,
		       pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint8_t *dst_line, *dst;
    uint8_t *src_line, *src;
    int dst_stride, src_stride;
    int32_t w;
    uint8_t s, d;
    uint16_t t;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint8_t, src_stride, src_line, 1);
    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint8_t, dst_stride, dst_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    s = *src;
	    d = *dst;
	    t = d + s;
	    s = t | (0 - (t >> 8));
	    *dst = s;

	    dst++;
	    src++;
	    w--;
	}

	while (w >= 8)
	{
	    *(__m64*)dst = _mm_adds_pu8 (ldq_u ((__m64 *)src), *(__m64*)dst);
	    dst += 8;
	    src += 8;
	    w -= 8;
	}

	while (w)
	{
	    s = *src;
	    d = *dst;
	    t = d + s;
	    s = t | (0 - (t >> 8));
	    *dst = s;

	    dst++;
	    src++;
	    w--;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_add_0565_0565 (pixman_implementation_t *imp,
                             pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint16_t    *dst_line, *dst;
    uint32_t	d;
    uint16_t    *src_line, *src;
    uint32_t	s;
    int dst_stride, src_stride;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint16_t, src_stride, src_line, 1);
    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint16_t, dst_stride, dst_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    s = *src++;
	    if (s)
	    {
		d = *dst;
		s = convert_0565_to_8888 (s);
		if (d)
		{
		    d = convert_0565_to_8888 (d);
		    UN8x4_ADD_UN8x4 (s, d);
		}
		*dst = convert_8888_to_0565 (s);
	    }
	    dst++;
	    w--;
	}

	while (w >= 4)
	{
	    __m64 vdest = *(__m64 *)dst;
	    __m64 vsrc = ldq_u ((__m64 *)src);
	    __m64 vd0, vd1;
	    __m64 vs0, vs1;

	    expand_4xpacked565 (vdest, &vd0, &vd1, 0);
	    expand_4xpacked565 (vsrc, &vs0, &vs1, 0);

	    vd0 = _mm_adds_pu8 (vd0, vs0);
	    vd1 = _mm_adds_pu8 (vd1, vs1);

	    *(__m64 *)dst = pack_4xpacked565 (vd0, vd1);

	    dst += 4;
	    src += 4;
	    w -= 4;
	}

	while (w--)
	{
	    s = *src++;
	    if (s)
	    {
		d = *dst;
		s = convert_0565_to_8888 (s);
		if (d)
		{
		    d = convert_0565_to_8888 (d);
		    UN8x4_ADD_UN8x4 (s, d);
		}
		*dst = convert_8888_to_0565 (s);
	    }
	    dst++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_add_8888_8888 (pixman_implementation_t *imp,
                             pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t    *dst_line, *dst;
    uint32_t    *src_line, *src;
    int dst_stride, src_stride;
    int32_t w;

    CHECKPOINT ();

    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);
    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;
	w = width;

	while (w && (uintptr_t)dst & 7)
	{
	    store (dst, _mm_adds_pu8 (load ((const uint32_t *)src),
	                              load ((const uint32_t *)dst)));
	    dst++;
	    src++;
	    w--;
	}

	while (w >= 2)
	{
	    *(__m64 *)dst = _mm_adds_pu8 (ldq_u ((__m64 *)src), *(__m64*)dst);
	    dst += 2;
	    src += 2;
	    w -= 2;
	}

	if (w)
	{
	    store (dst, _mm_adds_pu8 (load ((const uint32_t *)src),
	                              load ((const uint32_t *)dst)));

	}
    }

    _mm_empty ();
}

static pixman_bool_t
mmx_blt (pixman_implementation_t *imp,
         uint32_t *               src_bits,
         uint32_t *               dst_bits,
         int                      src_stride,
         int                      dst_stride,
         int                      src_bpp,
         int                      dst_bpp,
         int                      src_x,
         int                      src_y,
         int                      dest_x,
         int                      dest_y,
         int                      width,
         int                      height)
{
    uint8_t *   src_bytes;
    uint8_t *   dst_bytes;
    int byte_width;

    if (src_bpp != dst_bpp)
	return FALSE;

    if (src_bpp == 16)
    {
	src_stride = src_stride * (int) sizeof (uint32_t) / 2;
	dst_stride = dst_stride * (int) sizeof (uint32_t) / 2;
	src_bytes = (uint8_t *)(((uint16_t *)src_bits) + src_stride * (src_y) + (src_x));
	dst_bytes = (uint8_t *)(((uint16_t *)dst_bits) + dst_stride * (dest_y) + (dest_x));
	byte_width = 2 * width;
	src_stride *= 2;
	dst_stride *= 2;
    }
    else if (src_bpp == 32)
    {
	src_stride = src_stride * (int) sizeof (uint32_t) / 4;
	dst_stride = dst_stride * (int) sizeof (uint32_t) / 4;
	src_bytes = (uint8_t *)(((uint32_t *)src_bits) + src_stride * (src_y) + (src_x));
	dst_bytes = (uint8_t *)(((uint32_t *)dst_bits) + dst_stride * (dest_y) + (dest_x));
	byte_width = 4 * width;
	src_stride *= 4;
	dst_stride *= 4;
    }
    else
    {
	return FALSE;
    }

    while (height--)
    {
	int w;
	uint8_t *s = src_bytes;
	uint8_t *d = dst_bytes;
	src_bytes += src_stride;
	dst_bytes += dst_stride;
	w = byte_width;

	if (w >= 1 && ((uintptr_t)d & 1))
	{
	    *(uint8_t *)d = *(uint8_t *)s;
	    w -= 1;
	    s += 1;
	    d += 1;
	}

	if (w >= 2 && ((uintptr_t)d & 3))
	{
	    *(uint16_t *)d = *(uint16_t *)s;
	    w -= 2;
	    s += 2;
	    d += 2;
	}

	while (w >= 4 && ((uintptr_t)d & 7))
	{
	    *(uint32_t *)d = ldl_u ((uint32_t *)s);

	    w -= 4;
	    s += 4;
	    d += 4;
	}

	while (w >= 64)
	{
#if (defined (__GNUC__) || (defined(__SUNPRO_C) && (__SUNPRO_C >= 0x590))) && defined USE_X86_MMX
	    __asm__ (
	        "movq	  (%1),	  %%mm0\n"
	        "movq	 8(%1),	  %%mm1\n"
	        "movq	16(%1),	  %%mm2\n"
	        "movq	24(%1),	  %%mm3\n"
	        "movq	32(%1),	  %%mm4\n"
	        "movq	40(%1),	  %%mm5\n"
	        "movq	48(%1),	  %%mm6\n"
	        "movq	56(%1),	  %%mm7\n"

	        "movq	%%mm0,	  (%0)\n"
	        "movq	%%mm1,	 8(%0)\n"
	        "movq	%%mm2,	16(%0)\n"
	        "movq	%%mm3,	24(%0)\n"
	        "movq	%%mm4,	32(%0)\n"
	        "movq	%%mm5,	40(%0)\n"
	        "movq	%%mm6,	48(%0)\n"
	        "movq	%%mm7,	56(%0)\n"
		:
		: "r" (d), "r" (s)
		: "memory",
		  "%mm0", "%mm1", "%mm2", "%mm3",
		  "%mm4", "%mm5", "%mm6", "%mm7");
#else
	    __m64 v0 = ldq_u ((__m64 *)(s + 0));
	    __m64 v1 = ldq_u ((__m64 *)(s + 8));
	    __m64 v2 = ldq_u ((__m64 *)(s + 16));
	    __m64 v3 = ldq_u ((__m64 *)(s + 24));
	    __m64 v4 = ldq_u ((__m64 *)(s + 32));
	    __m64 v5 = ldq_u ((__m64 *)(s + 40));
	    __m64 v6 = ldq_u ((__m64 *)(s + 48));
	    __m64 v7 = ldq_u ((__m64 *)(s + 56));
	    *(__m64 *)(d + 0)  = v0;
	    *(__m64 *)(d + 8)  = v1;
	    *(__m64 *)(d + 16) = v2;
	    *(__m64 *)(d + 24) = v3;
	    *(__m64 *)(d + 32) = v4;
	    *(__m64 *)(d + 40) = v5;
	    *(__m64 *)(d + 48) = v6;
	    *(__m64 *)(d + 56) = v7;
#endif

	    w -= 64;
	    s += 64;
	    d += 64;
	}
	while (w >= 4)
	{
	    *(uint32_t *)d = ldl_u ((uint32_t *)s);

	    w -= 4;
	    s += 4;
	    d += 4;
	}
	if (w >= 2)
	{
	    *(uint16_t *)d = *(uint16_t *)s;
	    w -= 2;
	    s += 2;
	    d += 2;
	}
    }

    _mm_empty ();

    return TRUE;
}

static void
mmx_composite_copy_area (pixman_implementation_t *imp,
                         pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);

    mmx_blt (imp, src_image->bits.bits,
	     dest_image->bits.bits,
	     src_image->bits.rowstride,
	     dest_image->bits.rowstride,
	     PIXMAN_FORMAT_BPP (src_image->bits.format),
	     PIXMAN_FORMAT_BPP (dest_image->bits.format),
	     src_x, src_y, dest_x, dest_y, width, height);
}

static void
mmx_composite_over_x888_8_8888 (pixman_implementation_t *imp,
                                pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t  *src, *src_line;
    uint32_t  *dst, *dst_line;
    uint8_t  *mask, *mask_line;
    int src_stride, mask_stride, dst_stride;
    int32_t w;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (mask_image, mask_x, mask_y, uint8_t, mask_stride, mask_line, 1);
    PIXMAN_IMAGE_GET_LINE (src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

    while (height--)
    {
	src = src_line;
	src_line += src_stride;
	dst = dst_line;
	dst_line += dst_stride;
	mask = mask_line;
	mask_line += mask_stride;

	w = width;

	while (w--)
	{
	    uint64_t m = *mask;

	    if (m)
	    {
		uint32_t ssrc = *src | 0xff000000;
		__m64 s = load8888 (&ssrc);

		if (m == 0xff)
		{
		    store8888 (dst, s);
		}
		else
		{
		    __m64 sa = expand_alpha (s);
		    __m64 vm = expand_alpha_rev (to_m64 (m));
		    __m64 vdest = in_over (s, sa, vm, load8888 (dst));

		    store8888 (dst, vdest);
		}
	    }

	    mask++;
	    dst++;
	    src++;
	}
    }

    _mm_empty ();
}

static void
mmx_composite_over_reverse_n_8888 (pixman_implementation_t *imp,
                                   pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src;
    uint32_t    *dst_line, *dst;
    int32_t w;
    int dst_stride;
    __m64 vsrc;

    CHECKPOINT ();

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    if (src == 0)
	return;

    PIXMAN_IMAGE_GET_LINE (dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);

    vsrc = load8888 (&src);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	w = width;

	CHECKPOINT ();

	while (w && (uintptr_t)dst & 7)
	{
	    __m64 vdest = load8888 (dst);

	    store8888 (dst, over (vdest, expand_alpha (vdest), vsrc));

	    w--;
	    dst++;
	}

	while (w >= 2)
	{
	    __m64 vdest = *(__m64 *)dst;
	    __m64 dest0 = expand8888 (vdest, 0);
	    __m64 dest1 = expand8888 (vdest, 1);


	    dest0 = over (dest0, expand_alpha (dest0), vsrc);
	    dest1 = over (dest1, expand_alpha (dest1), vsrc);

	    *(__m64 *)dst = pack8888 (dest0, dest1);

	    dst += 2;
	    w -= 2;
	}

	CHECKPOINT ();

	if (w)
	{
	    __m64 vdest = load8888 (dst);

	    store8888 (dst, over (vdest, expand_alpha (vdest), vsrc));
	}
    }

    _mm_empty ();
}

#define BSHIFT ((1 << BILINEAR_INTERPOLATION_BITS))
#define BMSK (BSHIFT - 1)

#define BILINEAR_DECLARE_VARIABLES						\
    const __m64 mm_wt = _mm_set_pi16 (wt, wt, wt, wt);				\
    const __m64 mm_wb = _mm_set_pi16 (wb, wb, wb, wb);				\
    const __m64 mm_BSHIFT = _mm_set_pi16 (BSHIFT, BSHIFT, BSHIFT, BSHIFT);	\
    const __m64 mm_addc7 = _mm_set_pi16 (0, 1, 0, 1);				\
    const __m64 mm_xorc7 = _mm_set_pi16 (0, BMSK, 0, BMSK);			\
    const __m64 mm_ux = _mm_set_pi16 (unit_x, unit_x, unit_x, unit_x);		\
    const __m64 mm_zero = _mm_setzero_si64 ();					\
    __m64 mm_x = _mm_set_pi16 (vx, vx, vx, vx)

#define BILINEAR_INTERPOLATE_ONE_PIXEL(pix)					\
do {										\
    /* fetch 2x2 pixel block into 2 mmx registers */				\
    __m64 t = ldq_u ((__m64 *)&src_top [pixman_fixed_to_int (vx)]);		\
    __m64 b = ldq_u ((__m64 *)&src_bottom [pixman_fixed_to_int (vx)]);		\
    /* vertical interpolation */						\
    __m64 t_hi = _mm_mullo_pi16 (_mm_unpackhi_pi8 (t, mm_zero), mm_wt);		\
    __m64 t_lo = _mm_mullo_pi16 (_mm_unpacklo_pi8 (t, mm_zero), mm_wt);		\
    __m64 b_hi = _mm_mullo_pi16 (_mm_unpackhi_pi8 (b, mm_zero), mm_wb);		\
    __m64 b_lo = _mm_mullo_pi16 (_mm_unpacklo_pi8 (b, mm_zero), mm_wb);		\
    __m64 hi = _mm_add_pi16 (t_hi, b_hi);					\
    __m64 lo = _mm_add_pi16 (t_lo, b_lo);					\
    vx += unit_x;								\
    if (BILINEAR_INTERPOLATION_BITS < 8)					\
    {										\
	/* calculate horizontal weights */					\
	__m64 mm_wh = _mm_add_pi16 (mm_addc7, _mm_xor_si64 (mm_xorc7,		\
			  _mm_srli_pi16 (mm_x,					\
					 16 - BILINEAR_INTERPOLATION_BITS)));	\
	/* horizontal interpolation */						\
	__m64 p = _mm_unpacklo_pi16 (lo, hi);					\
	__m64 q = _mm_unpackhi_pi16 (lo, hi);					\
	lo = _mm_madd_pi16 (p, mm_wh);						\
	hi = _mm_madd_pi16 (q, mm_wh);						\
    }										\
    else									\
    {										\
	/* calculate horizontal weights */					\
	__m64 mm_wh_lo = _mm_sub_pi16 (mm_BSHIFT, _mm_srli_pi16 (mm_x,		\
					16 - BILINEAR_INTERPOLATION_BITS));	\
	__m64 mm_wh_hi = _mm_srli_pi16 (mm_x,					\
					16 - BILINEAR_INTERPOLATION_BITS);	\
	/* horizontal interpolation */						\
	__m64 mm_lo_lo = _mm_mullo_pi16 (lo, mm_wh_lo);				\
	__m64 mm_lo_hi = _mm_mullo_pi16 (hi, mm_wh_hi);				\
	__m64 mm_hi_lo = _mm_mulhi_pu16 (lo, mm_wh_lo);				\
	__m64 mm_hi_hi = _mm_mulhi_pu16 (hi, mm_wh_hi);				\
	lo = _mm_add_pi32 (_mm_unpacklo_pi16 (mm_lo_lo, mm_hi_lo),		\
			   _mm_unpacklo_pi16 (mm_lo_hi, mm_hi_hi));		\
	hi = _mm_add_pi32 (_mm_unpackhi_pi16 (mm_lo_lo, mm_hi_lo),		\
			   _mm_unpackhi_pi16 (mm_lo_hi, mm_hi_hi));		\
    }										\
    mm_x = _mm_add_pi16 (mm_x, mm_ux);						\
    /* shift and pack the result */						\
    hi = _mm_srli_pi32 (hi, BILINEAR_INTERPOLATION_BITS * 2);			\
    lo = _mm_srli_pi32 (lo, BILINEAR_INTERPOLATION_BITS * 2);			\
    lo = _mm_packs_pi32 (lo, hi);						\
    lo = _mm_packs_pu16 (lo, lo);						\
    pix = lo;									\
} while (0)

#define BILINEAR_SKIP_ONE_PIXEL()						\
do {										\
    vx += unit_x;								\
    mm_x = _mm_add_pi16 (mm_x, mm_ux);						\
} while(0)

static force_inline void
scaled_bilinear_scanline_mmx_8888_8888_SRC (uint32_t *       dst,
					    const uint32_t * mask,
					    const uint32_t * src_top,
					    const uint32_t * src_bottom,
					    int32_t          w,
					    int              wt,
					    int              wb,
					    pixman_fixed_t   vx,
					    pixman_fixed_t   unit_x,
					    pixman_fixed_t   max_vx,
					    pixman_bool_t    zero_src)
{
    BILINEAR_DECLARE_VARIABLES;
    __m64 pix;

    while (w--)
    {
	BILINEAR_INTERPOLATE_ONE_PIXEL (pix);
	store (dst, pix);
	dst++;
    }

    _mm_empty ();
}

FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_cover_SRC,
			       scaled_bilinear_scanline_mmx_8888_8888_SRC,
			       uint32_t, uint32_t, uint32_t,
			       COVER, FLAG_NONE)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_pad_SRC,
			       scaled_bilinear_scanline_mmx_8888_8888_SRC,
			       uint32_t, uint32_t, uint32_t,
			       PAD, FLAG_NONE)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_none_SRC,
			       scaled_bilinear_scanline_mmx_8888_8888_SRC,
			       uint32_t, uint32_t, uint32_t,
			       NONE, FLAG_NONE)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_normal_SRC,
			       scaled_bilinear_scanline_mmx_8888_8888_SRC,
			       uint32_t, uint32_t, uint32_t,
			       NORMAL, FLAG_NONE)

static force_inline void
scaled_bilinear_scanline_mmx_8888_8888_OVER (uint32_t *       dst,
					     const uint32_t * mask,
					     const uint32_t * src_top,
					     const uint32_t * src_bottom,
					     int32_t          w,
					     int              wt,
					     int              wb,
					     pixman_fixed_t   vx,
					     pixman_fixed_t   unit_x,
					     pixman_fixed_t   max_vx,
					     pixman_bool_t    zero_src)
{
    BILINEAR_DECLARE_VARIABLES;
    __m64 pix1, pix2;

    while (w)
    {
	BILINEAR_INTERPOLATE_ONE_PIXEL (pix1);

	if (!is_zero (pix1))
	{
	    pix2 = load (dst);
	    store8888 (dst, core_combine_over_u_pixel_mmx (pix1, pix2));
	}

	w--;
	dst++;
    }

    _mm_empty ();
}

FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_cover_OVER,
			       scaled_bilinear_scanline_mmx_8888_8888_OVER,
			       uint32_t, uint32_t, uint32_t,
			       COVER, FLAG_NONE)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_pad_OVER,
			       scaled_bilinear_scanline_mmx_8888_8888_OVER,
			       uint32_t, uint32_t, uint32_t,
			       PAD, FLAG_NONE)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_none_OVER,
			       scaled_bilinear_scanline_mmx_8888_8888_OVER,
			       uint32_t, uint32_t, uint32_t,
			       NONE, FLAG_NONE)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8888_normal_OVER,
			       scaled_bilinear_scanline_mmx_8888_8888_OVER,
			       uint32_t, uint32_t, uint32_t,
			       NORMAL, FLAG_NONE)

static force_inline void
scaled_bilinear_scanline_mmx_8888_8_8888_OVER (uint32_t *       dst,
					       const uint8_t  * mask,
					       const uint32_t * src_top,
					       const uint32_t * src_bottom,
					       int32_t          w,
					       int              wt,
					       int              wb,
					       pixman_fixed_t   vx,
					       pixman_fixed_t   unit_x,
					       pixman_fixed_t   max_vx,
					       pixman_bool_t    zero_src)
{
    BILINEAR_DECLARE_VARIABLES;
    __m64 pix1, pix2;
    uint32_t m;

    while (w)
    {
	m = (uint32_t) *mask++;

	if (m)
	{
	    BILINEAR_INTERPOLATE_ONE_PIXEL (pix1);

	    if (m == 0xff && is_opaque (pix1))
	    {
		store (dst, pix1);
	    }
	    else
	    {
		__m64 ms, md, ma, msa;

		pix2 = load (dst);
		ma = expand_alpha_rev (to_m64 (m));
		ms = _mm_unpacklo_pi8 (pix1, _mm_setzero_si64 ());
		md = _mm_unpacklo_pi8 (pix2, _mm_setzero_si64 ());

		msa = expand_alpha (ms);

		store8888 (dst, (in_over (ms, msa, ma, md)));
	    }
	}
	else
	{
	    BILINEAR_SKIP_ONE_PIXEL ();
	}

	w--;
	dst++;
    }

    _mm_empty ();
}

FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8_8888_cover_OVER,
			       scaled_bilinear_scanline_mmx_8888_8_8888_OVER,
			       uint32_t, uint8_t, uint32_t,
			       COVER, FLAG_HAVE_NON_SOLID_MASK)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8_8888_pad_OVER,
			       scaled_bilinear_scanline_mmx_8888_8_8888_OVER,
			       uint32_t, uint8_t, uint32_t,
			       PAD, FLAG_HAVE_NON_SOLID_MASK)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8_8888_none_OVER,
			       scaled_bilinear_scanline_mmx_8888_8_8888_OVER,
			       uint32_t, uint8_t, uint32_t,
			       NONE, FLAG_HAVE_NON_SOLID_MASK)
FAST_BILINEAR_MAINLOOP_COMMON (mmx_8888_8_8888_normal_OVER,
			       scaled_bilinear_scanline_mmx_8888_8_8888_OVER,
			       uint32_t, uint8_t, uint32_t,
			       NORMAL, FLAG_HAVE_NON_SOLID_MASK)

static uint32_t *
mmx_fetch_x8r8g8b8 (pixman_iter_t *iter, const uint32_t *mask)
{
    int w = iter->width;
    uint32_t *dst = iter->buffer;
    uint32_t *src = (uint32_t *)iter->bits;

    iter->bits += iter->stride;

    while (w && ((uintptr_t)dst) & 7)
    {
	*dst++ = (*src++) | 0xff000000;
	w--;
    }

    while (w >= 8)
    {
	__m64 vsrc1 = ldq_u ((__m64 *)(src + 0));
	__m64 vsrc2 = ldq_u ((__m64 *)(src + 2));
	__m64 vsrc3 = ldq_u ((__m64 *)(src + 4));
	__m64 vsrc4 = ldq_u ((__m64 *)(src + 6));

	*(__m64 *)(dst + 0) = _mm_or_si64 (vsrc1, MC (ff000000));
	*(__m64 *)(dst + 2) = _mm_or_si64 (vsrc2, MC (ff000000));
	*(__m64 *)(dst + 4) = _mm_or_si64 (vsrc3, MC (ff000000));
	*(__m64 *)(dst + 6) = _mm_or_si64 (vsrc4, MC (ff000000));

	dst += 8;
	src += 8;
	w -= 8;
    }

    while (w)
    {
	*dst++ = (*src++) | 0xff000000;
	w--;
    }

    _mm_empty ();
    return iter->buffer;
}

static uint32_t *
mmx_fetch_r5g6b5 (pixman_iter_t *iter, const uint32_t *mask)
{
    int w = iter->width;
    uint32_t *dst = iter->buffer;
    uint16_t *src = (uint16_t *)iter->bits;

    iter->bits += iter->stride;

    while (w && ((uintptr_t)dst) & 0x0f)
    {
	uint16_t s = *src++;

	*dst++ = convert_0565_to_8888 (s);
	w--;
    }

    while (w >= 4)
    {
	__m64 vsrc = ldq_u ((__m64 *)src);
	__m64 mm0, mm1;

	expand_4xpacked565 (vsrc, &mm0, &mm1, 1);

	*(__m64 *)(dst + 0) = mm0;
	*(__m64 *)(dst + 2) = mm1;

	dst += 4;
	src += 4;
	w -= 4;
    }

    while (w)
    {
	uint16_t s = *src++;

	*dst++ = convert_0565_to_8888 (s);
	w--;
    }

    _mm_empty ();
    return iter->buffer;
}

static uint32_t *
mmx_fetch_a8 (pixman_iter_t *iter, const uint32_t *mask)
{
    int w = iter->width;
    uint32_t *dst = iter->buffer;
    uint8_t *src = iter->bits;

    iter->bits += iter->stride;

    while (w && (((uintptr_t)dst) & 15))
    {
        *dst++ = *(src++) << 24;
        w--;
    }

    while (w >= 8)
    {
	__m64 mm0 = ldq_u ((__m64 *)src);

	__m64 mm1 = _mm_unpacklo_pi8  (_mm_setzero_si64(), mm0);
	__m64 mm2 = _mm_unpackhi_pi8  (_mm_setzero_si64(), mm0);
	__m64 mm3 = _mm_unpacklo_pi16 (_mm_setzero_si64(), mm1);
	__m64 mm4 = _mm_unpackhi_pi16 (_mm_setzero_si64(), mm1);
	__m64 mm5 = _mm_unpacklo_pi16 (_mm_setzero_si64(), mm2);
	__m64 mm6 = _mm_unpackhi_pi16 (_mm_setzero_si64(), mm2);

	*(__m64 *)(dst + 0) = mm3;
	*(__m64 *)(dst + 2) = mm4;
	*(__m64 *)(dst + 4) = mm5;
	*(__m64 *)(dst + 6) = mm6;

	dst += 8;
	src += 8;
	w -= 8;
    }

    while (w)
    {
	*dst++ = *(src++) << 24;
	w--;
    }

    _mm_empty ();
    return iter->buffer;
}

typedef struct
{
    pixman_format_code_t	format;
    pixman_iter_get_scanline_t	get_scanline;
} fetcher_info_t;

static const fetcher_info_t fetchers[] =
{
    { PIXMAN_x8r8g8b8,		mmx_fetch_x8r8g8b8 },
    { PIXMAN_r5g6b5,		mmx_fetch_r5g6b5 },
    { PIXMAN_a8,		mmx_fetch_a8 },
    { PIXMAN_null }
};

static pixman_bool_t
mmx_src_iter_init (pixman_implementation_t *imp, pixman_iter_t *iter)
{
    pixman_image_t *image = iter->image;

#define FLAGS								\
    (FAST_PATH_STANDARD_FLAGS | FAST_PATH_ID_TRANSFORM |		\
     FAST_PATH_BITS_IMAGE | FAST_PATH_SAMPLES_COVER_CLIP_NEAREST)

    if ((iter->iter_flags & ITER_NARROW)			&&
	(iter->image_flags & FLAGS) == FLAGS)
    {
	const fetcher_info_t *f;

	for (f = &fetchers[0]; f->format != PIXMAN_null; f++)
	{
	    if (image->common.extended_format_code == f->format)
	    {
		uint8_t *b = (uint8_t *)image->bits.bits;
		int s = image->bits.rowstride * 4;

		iter->bits = b + s * iter->y + iter->x * PIXMAN_FORMAT_BPP (f->format) / 8;
		iter->stride = s;

		iter->get_scanline = f->get_scanline;
		return TRUE;
	    }
	}
    }

    return FALSE;
}

static const pixman_fast_path_t mmx_fast_paths[] =
{
    PIXMAN_STD_FAST_PATH    (OVER, solid,    a8,       r5g6b5,   mmx_composite_over_n_8_0565       ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    a8,       b5g6r5,   mmx_composite_over_n_8_0565       ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    a8,       a8r8g8b8, mmx_composite_over_n_8_8888       ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    a8,       x8r8g8b8, mmx_composite_over_n_8_8888       ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    a8,       a8b8g8r8, mmx_composite_over_n_8_8888       ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    a8,       x8b8g8r8, mmx_composite_over_n_8_8888       ),
    PIXMAN_STD_FAST_PATH_CA (OVER, solid,    a8r8g8b8, a8r8g8b8, mmx_composite_over_n_8888_8888_ca ),
    PIXMAN_STD_FAST_PATH_CA (OVER, solid,    a8r8g8b8, x8r8g8b8, mmx_composite_over_n_8888_8888_ca ),
    PIXMAN_STD_FAST_PATH_CA (OVER, solid,    a8r8g8b8, r5g6b5,   mmx_composite_over_n_8888_0565_ca ),
    PIXMAN_STD_FAST_PATH_CA (OVER, solid,    a8b8g8r8, a8b8g8r8, mmx_composite_over_n_8888_8888_ca ),
    PIXMAN_STD_FAST_PATH_CA (OVER, solid,    a8b8g8r8, x8b8g8r8, mmx_composite_over_n_8888_8888_ca ),
    PIXMAN_STD_FAST_PATH_CA (OVER, solid,    a8b8g8r8, b5g6r5,   mmx_composite_over_n_8888_0565_ca ),
    PIXMAN_STD_FAST_PATH    (OVER, pixbuf,   pixbuf,   a8r8g8b8, mmx_composite_over_pixbuf_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, pixbuf,   pixbuf,   x8r8g8b8, mmx_composite_over_pixbuf_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, pixbuf,   pixbuf,   r5g6b5,   mmx_composite_over_pixbuf_0565    ),
    PIXMAN_STD_FAST_PATH    (OVER, rpixbuf,  rpixbuf,  a8b8g8r8, mmx_composite_over_pixbuf_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, rpixbuf,  rpixbuf,  x8b8g8r8, mmx_composite_over_pixbuf_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, rpixbuf,  rpixbuf,  b5g6r5,   mmx_composite_over_pixbuf_0565    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8r8g8b8, solid,    a8r8g8b8, mmx_composite_over_x888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8r8g8b8, solid,    x8r8g8b8, mmx_composite_over_x888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8b8g8r8, solid,    a8b8g8r8, mmx_composite_over_x888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8b8g8r8, solid,    x8b8g8r8, mmx_composite_over_x888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, a8r8g8b8, solid,    a8r8g8b8, mmx_composite_over_8888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, a8r8g8b8, solid,    x8r8g8b8, mmx_composite_over_8888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, a8b8g8r8, solid,    a8b8g8r8, mmx_composite_over_8888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, a8b8g8r8, solid,    x8b8g8r8, mmx_composite_over_8888_n_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8r8g8b8, a8,       x8r8g8b8, mmx_composite_over_x888_8_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8r8g8b8, a8,       a8r8g8b8, mmx_composite_over_x888_8_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8b8g8r8, a8,       x8b8g8r8, mmx_composite_over_x888_8_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, x8b8g8r8, a8,       a8b8g8r8, mmx_composite_over_x888_8_8888    ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    null,     a8r8g8b8, mmx_composite_over_n_8888         ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    null,     x8r8g8b8, mmx_composite_over_n_8888         ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    null,     r5g6b5,   mmx_composite_over_n_0565         ),
    PIXMAN_STD_FAST_PATH    (OVER, solid,    null,     b5g6r5,   mmx_composite_over_n_0565         ),
    PIXMAN_STD_FAST_PATH    (OVER, x8r8g8b8, null,     x8r8g8b8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (OVER, x8b8g8r8, null,     x8b8g8r8, mmx_composite_copy_area           ),

    PIXMAN_STD_FAST_PATH    (OVER, a8r8g8b8, null,     a8r8g8b8, mmx_composite_over_8888_8888      ),
    PIXMAN_STD_FAST_PATH    (OVER, a8r8g8b8, null,     x8r8g8b8, mmx_composite_over_8888_8888      ),
    PIXMAN_STD_FAST_PATH    (OVER, a8r8g8b8, null,     r5g6b5,   mmx_composite_over_8888_0565      ),
    PIXMAN_STD_FAST_PATH    (OVER, a8b8g8r8, null,     a8b8g8r8, mmx_composite_over_8888_8888      ),
    PIXMAN_STD_FAST_PATH    (OVER, a8b8g8r8, null,     x8b8g8r8, mmx_composite_over_8888_8888      ),
    PIXMAN_STD_FAST_PATH    (OVER, a8b8g8r8, null,     b5g6r5,   mmx_composite_over_8888_0565      ),

    PIXMAN_STD_FAST_PATH    (OVER_REVERSE, solid, null, a8r8g8b8, mmx_composite_over_reverse_n_8888),
    PIXMAN_STD_FAST_PATH    (OVER_REVERSE, solid, null, a8b8g8r8, mmx_composite_over_reverse_n_8888),

    PIXMAN_STD_FAST_PATH    (ADD,  r5g6b5,   null,     r5g6b5,   mmx_composite_add_0565_0565       ),
    PIXMAN_STD_FAST_PATH    (ADD,  b5g6r5,   null,     b5g6r5,   mmx_composite_add_0565_0565       ),
    PIXMAN_STD_FAST_PATH    (ADD,  a8r8g8b8, null,     a8r8g8b8, mmx_composite_add_8888_8888       ),
    PIXMAN_STD_FAST_PATH    (ADD,  a8b8g8r8, null,     a8b8g8r8, mmx_composite_add_8888_8888       ),
    PIXMAN_STD_FAST_PATH    (ADD,  a8,       null,     a8,       mmx_composite_add_8_8		   ),
    PIXMAN_STD_FAST_PATH    (ADD,  solid,    a8,       a8,       mmx_composite_add_n_8_8           ),

    PIXMAN_STD_FAST_PATH    (SRC,  a8r8g8b8, null,     r5g6b5,   mmx_composite_src_x888_0565       ),
    PIXMAN_STD_FAST_PATH    (SRC,  a8b8g8r8, null,     b5g6r5,   mmx_composite_src_x888_0565       ),
    PIXMAN_STD_FAST_PATH    (SRC,  x8r8g8b8, null,     r5g6b5,   mmx_composite_src_x888_0565       ),
    PIXMAN_STD_FAST_PATH    (SRC,  x8b8g8r8, null,     b5g6r5,   mmx_composite_src_x888_0565       ),
    PIXMAN_STD_FAST_PATH    (SRC,  solid,    a8,       a8r8g8b8, mmx_composite_src_n_8_8888        ),
    PIXMAN_STD_FAST_PATH    (SRC,  solid,    a8,       x8r8g8b8, mmx_composite_src_n_8_8888        ),
    PIXMAN_STD_FAST_PATH    (SRC,  solid,    a8,       a8b8g8r8, mmx_composite_src_n_8_8888        ),
    PIXMAN_STD_FAST_PATH    (SRC,  solid,    a8,       x8b8g8r8, mmx_composite_src_n_8_8888        ),
    PIXMAN_STD_FAST_PATH    (SRC,  a8r8g8b8, null,     a8r8g8b8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  a8b8g8r8, null,     a8b8g8r8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  a8r8g8b8, null,     x8r8g8b8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  a8b8g8r8, null,     x8b8g8r8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  x8r8g8b8, null,     x8r8g8b8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  x8b8g8r8, null,     x8b8g8r8, mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  r5g6b5,   null,     r5g6b5,   mmx_composite_copy_area           ),
    PIXMAN_STD_FAST_PATH    (SRC,  b5g6r5,   null,     b5g6r5,   mmx_composite_copy_area           ),

    PIXMAN_STD_FAST_PATH    (IN,   a8,       null,     a8,       mmx_composite_in_8_8              ),
    PIXMAN_STD_FAST_PATH    (IN,   solid,    a8,       a8,       mmx_composite_in_n_8_8            ),

    SIMPLE_BILINEAR_FAST_PATH (SRC, a8r8g8b8,          a8r8g8b8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (SRC, a8r8g8b8,          x8r8g8b8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (SRC, x8r8g8b8,          x8r8g8b8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (SRC, a8b8g8r8,          a8b8g8r8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (SRC, a8b8g8r8,          x8b8g8r8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (SRC, x8b8g8r8,          x8b8g8r8, mmx_8888_8888                     ),

    SIMPLE_BILINEAR_FAST_PATH (OVER, a8r8g8b8,         x8r8g8b8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (OVER, a8b8g8r8,         x8b8g8r8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (OVER, a8r8g8b8,         a8r8g8b8, mmx_8888_8888                     ),
    SIMPLE_BILINEAR_FAST_PATH (OVER, a8b8g8r8,         a8b8g8r8, mmx_8888_8888                     ),

    SIMPLE_BILINEAR_A8_MASK_FAST_PATH (OVER, a8r8g8b8, x8r8g8b8, mmx_8888_8_8888                   ),
    SIMPLE_BILINEAR_A8_MASK_FAST_PATH (OVER, a8b8g8r8, x8b8g8r8, mmx_8888_8_8888                   ),
    SIMPLE_BILINEAR_A8_MASK_FAST_PATH (OVER, a8r8g8b8, a8r8g8b8, mmx_8888_8_8888                   ),
    SIMPLE_BILINEAR_A8_MASK_FAST_PATH (OVER, a8b8g8r8, a8b8g8r8, mmx_8888_8_8888                   ),

    { PIXMAN_OP_NONE },
};

pixman_implementation_t *
_pixman_implementation_create_mmx (pixman_implementation_t *fallback)
{
    pixman_implementation_t *imp = _pixman_implementation_create (fallback, mmx_fast_paths);
    /* Unified alpha */
    imp->combine_32[PIXMAN_OP_OVER] = mmx_combine_over_u;
    imp->combine_32[PIXMAN_OP_OVER_REVERSE] = mmx_combine_over_reverse_u;
    imp->combine_32[PIXMAN_OP_IN] = mmx_combine_in_u;
    imp->combine_32[PIXMAN_OP_IN_REVERSE] = mmx_combine_in_reverse_u;
    imp->combine_32[PIXMAN_OP_OUT] = mmx_combine_out_u;
    imp->combine_32[PIXMAN_OP_OUT_REVERSE] = mmx_combine_out_reverse_u;
    imp->combine_32[PIXMAN_OP_ATOP] = mmx_combine_atop_u;
    imp->combine_32[PIXMAN_OP_ATOP_REVERSE] = mmx_combine_atop_reverse_u;
    imp->combine_32[PIXMAN_OP_XOR] = mmx_combine_xor_u;
    imp->combine_32[PIXMAN_OP_ADD] = mmx_combine_add_u;
    imp->combine_32[PIXMAN_OP_SATURATE] = mmx_combine_saturate_u;

    /* Disjoint, unified */
    imp->combine_32[PIXMAN_OP_DISJOINT_OVER] = mmx_combine_disjoint_over_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_OVER_REVERSE] = mmx_combine_saturate_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_IN] = mmx_combine_disjoint_in_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_IN_REVERSE] = mmx_combine_disjoint_in_reverse_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_OUT] = mmx_combine_disjoint_out_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_OUT_REVERSE] = mmx_combine_disjoint_out_reverse_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_ATOP] = mmx_combine_disjoint_atop_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_ATOP_REVERSE] = mmx_combine_disjoint_atop_reverse_u;
    imp->combine_32[PIXMAN_OP_DISJOINT_XOR] = mmx_combine_disjoint_xor_u;

    /* Conjoint, unified */
    imp->combine_32[PIXMAN_OP_CONJOINT_OVER] = mmx_combine_conjoint_over_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_OVER_REVERSE] = mmx_combine_conjoint_over_reverse_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_IN] = mmx_combine_conjoint_in_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_IN_REVERSE] = mmx_combine_conjoint_in_reverse_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_OUT] = mmx_combine_conjoint_out_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_OUT_REVERSE] = mmx_combine_conjoint_out_reverse_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_ATOP] = mmx_combine_conjoint_atop_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_ATOP_REVERSE] = mmx_combine_conjoint_atop_reverse_u;
    imp->combine_32[PIXMAN_OP_CONJOINT_XOR] = mmx_combine_conjoint_xor_u;
    
    /* Multiply, Unified */
    imp->combine_32[PIXMAN_OP_MULTIPLY] = mmx_combine_multiply_u;
    imp->combine_32[PIXMAN_OP_SCREEN] = mmx_combine_screen_u;
    imp->combine_32[PIXMAN_OP_OVERLAY] = mmx_combine_overlay_u;
    imp->combine_32[PIXMAN_OP_DARKEN] = mmx_combine_darken_u;
    imp->combine_32[PIXMAN_OP_LIGHTEN] = mmx_combine_lighten_u;
    imp->combine_32[PIXMAN_OP_COLOR_DODGE] = mmx_combine_color_dodge_u;
    imp->combine_32[PIXMAN_OP_COLOR_BURN] = mmx_combine_color_burn_u;
    imp->combine_32[PIXMAN_OP_HARD_LIGHT] = mmx_combine_hard_light_u;
    imp->combine_32[PIXMAN_OP_DIFFERENCE] = mmx_combine_difference_u;
    imp->combine_32[PIXMAN_OP_EXCLUSION] = mmx_combine_exclusion_u;
	
    /* Component alpha combiners */
    imp->combine_32_ca[PIXMAN_OP_SRC] = mmx_combine_src_ca;
    imp->combine_32_ca[PIXMAN_OP_OVER] = mmx_combine_over_ca;
    imp->combine_32_ca[PIXMAN_OP_OVER_REVERSE] = mmx_combine_over_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_IN] = mmx_combine_in_ca;
    imp->combine_32_ca[PIXMAN_OP_IN_REVERSE] = mmx_combine_in_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_OUT] = mmx_combine_out_ca;
    imp->combine_32_ca[PIXMAN_OP_OUT_REVERSE] = mmx_combine_out_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_ATOP] = mmx_combine_atop_ca;
    imp->combine_32_ca[PIXMAN_OP_ATOP_REVERSE] = mmx_combine_atop_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_XOR] = mmx_combine_xor_ca;
    imp->combine_32_ca[PIXMAN_OP_ADD] = mmx_combine_add_ca;
    imp->combine_32_ca[PIXMAN_OP_SATURATE] = mmx_combine_saturate_ca;

    /* Disjoint CA */
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_OVER] = mmx_combine_disjoint_over_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_OVER_REVERSE] = mmx_combine_saturate_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_IN] = mmx_combine_disjoint_in_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_IN_REVERSE] = mmx_combine_disjoint_in_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_OUT] = mmx_combine_disjoint_out_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_OUT_REVERSE] = mmx_combine_disjoint_out_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_ATOP] = mmx_combine_disjoint_atop_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_ATOP_REVERSE] = mmx_combine_disjoint_atop_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_DISJOINT_XOR] = mmx_combine_disjoint_xor_ca;

    /* Conjoint CA */
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_OVER] = mmx_combine_conjoint_over_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_OVER_REVERSE] = mmx_combine_conjoint_over_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_IN] = mmx_combine_conjoint_in_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_IN_REVERSE] = mmx_combine_conjoint_in_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_OUT] = mmx_combine_conjoint_out_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_OUT_REVERSE] = mmx_combine_conjoint_out_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_ATOP] = mmx_combine_conjoint_atop_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_ATOP_REVERSE] = mmx_combine_conjoint_atop_reverse_ca;
    imp->combine_32_ca[PIXMAN_OP_CONJOINT_XOR] = mmx_combine_conjoint_xor_ca;

    /* Multiply CA */
    imp->combine_32_ca[PIXMAN_OP_MULTIPLY] = mmx_combine_multiply_ca;
    imp->combine_32_ca[PIXMAN_OP_SCREEN] = mmx_combine_screen_ca;
    imp->combine_32_ca[PIXMAN_OP_OVERLAY] = mmx_combine_overlay_ca;
    imp->combine_32_ca[PIXMAN_OP_DARKEN] = mmx_combine_darken_ca;
    imp->combine_32_ca[PIXMAN_OP_LIGHTEN] = mmx_combine_lighten_ca;
    imp->combine_32_ca[PIXMAN_OP_COLOR_DODGE] = mmx_combine_color_dodge_ca;
    imp->combine_32_ca[PIXMAN_OP_COLOR_BURN] = mmx_combine_color_burn_ca;
    imp->combine_32_ca[PIXMAN_OP_HARD_LIGHT] = mmx_combine_hard_light_ca;
    imp->combine_32_ca[PIXMAN_OP_DIFFERENCE] = mmx_combine_difference_ca;
    imp->combine_32_ca[PIXMAN_OP_EXCLUSION] = mmx_combine_exclusion_ca;

    imp->blt = mmx_blt;
    imp->fill = mmx_fill;

    imp->src_iter_init = mmx_src_iter_init;

    return imp;
}

#endif /* USE_X86_MMX || USE_ARM_IWMMXT || USE_LOONGSON_MMI */
