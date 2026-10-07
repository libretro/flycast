/* The SH4's two vector instructions as the x86-64 recompiler emits them:
 * FIPR, the inner product of two vectors of four floats, and FTRV, a 4x4
 * matrix times such a vector.
 *
 * Both work in doubles: a float times a float is exact in a double, so the
 * only rounding is in the sums and in the final conversion back, and the
 * sums are taken in the order the reference implementations in
 * hw/sh4/dyna/shil_canonical.h take them. The result is then theirs to the
 * last bit, which tools/sh4/run.sh checks by running this very code against
 * them. These are here, and not in rec_x64.cpp, so that it can.
 *
 * In: rax and rcx point at the operands. Uses xmm0 to xmm4, which the
 * recompiler does not hand out. */
#pragma once

// rax, rcx: the two vectors. The product goes to @rd, as a float.
template<typename Gen>
static inline void x64_emit_fipr(Gen& c, const Xbyak::Xmm& rd)
{
	using namespace Xbyak::util;

	c.cvtss2sd(xmm0, c.dword[rax]);
	c.cvtss2sd(xmm1, c.dword[rcx]);
	c.mulsd(xmm0, xmm1);
	for (int i = 1; i < 4; i++)
	{
		c.cvtss2sd(xmm1, c.dword[rax + i * 4]);
		c.cvtss2sd(xmm2, c.dword[rcx + i * 4]);
		c.mulsd(xmm1, xmm2);
		c.addsd(xmm0, xmm1);
	}
	c.cvtsd2ss(rd, xmm0);
}

// rax: the vector. rcx: the matrix, sixteen floats by columns. The four
// results are left in xmm0.
template<typename Gen>
static inline void x64_emit_ftrv(Gen& c)
{
	using namespace Xbyak::util;

	for (int col = 0; col < 4; col++)
	{
		// this column times its element of the vector, two rows to a register
		c.cvtss2sd(xmm4, c.dword[rax + col * 4]);
		c.unpcklpd(xmm4, xmm4);
		c.cvtps2pd(xmm2, c.qword[rcx + col * 16]);		// rows 0 and 1
		c.cvtps2pd(xmm3, c.qword[rcx + col * 16 + 8]);	// rows 2 and 3
		c.mulpd(xmm2, xmm4);
		c.mulpd(xmm3, xmm4);
		if (col == 0)
		{
			c.movapd(xmm0, xmm2);
			c.movapd(xmm1, xmm3);
		}
		else
		{
			c.addpd(xmm0, xmm2);
			c.addpd(xmm1, xmm3);
		}
	}
	c.cvtpd2ps(xmm0, xmm0);
	c.cvtpd2ps(xmm1, xmm1);
	c.movlhps(xmm0, xmm1);
}
