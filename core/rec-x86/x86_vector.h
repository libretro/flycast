/* The SH4's two vector instructions as the 32-bit x86 recompiler emits
 * them: FIPR and FTRV, worked out in doubles exactly as the x86-64
 * recompiler does (rec-x64/x64_vector.h says why that gives the reference
 * implementations' results to the last bit), with what this processor has:
 * eax and ecx point at the operands, and only xmm0 to xmm3 are used - the
 * others are the block's. */
#pragma once

// eax, ecx: the two vectors. The product goes to @rd, as a float.
template<typename Gen>
static inline void x86_emit_fipr(Gen& c, const Xbyak::Xmm& rd)
{
	using namespace Xbyak::util;

	c.cvtss2sd(xmm0, c.dword[eax]);
	c.cvtss2sd(xmm1, c.dword[ecx]);
	c.mulsd(xmm0, xmm1);
	for (int i = 1; i < 4; i++)
	{
		c.cvtss2sd(xmm1, c.dword[eax + i * 4]);
		c.cvtss2sd(xmm2, c.dword[ecx + i * 4]);
		c.mulsd(xmm1, xmm2);
		c.addsd(xmm0, xmm1);
	}
	c.cvtsd2ss(rd, xmm0);
}

// eax: the vector. ecx: the matrix, sixteen floats by columns. The four
// results are left in xmm0.
template<typename Gen>
static inline void x86_emit_ftrv(Gen& c)
{
	using namespace Xbyak::util;

	for (int col = 0; col < 4; col++)
	{
		// this column's element of the vector, twice
		c.cvtss2sd(xmm2, c.dword[eax + col * 4]);
		c.unpcklpd(xmm2, xmm2);
		// times rows 0 and 1, then rows 2 and 3, into the two sums
		c.cvtps2pd(xmm3, c.qword[ecx + col * 16]);
		c.mulpd(xmm3, xmm2);
		if (col == 0)
			c.movapd(xmm0, xmm3);
		else
			c.addpd(xmm0, xmm3);
		c.cvtps2pd(xmm3, c.qword[ecx + col * 16 + 8]);
		c.mulpd(xmm3, xmm2);
		if (col == 0)
			c.movapd(xmm1, xmm3);
		else
			c.addpd(xmm1, xmm3);
	}
	c.cvtpd2ps(xmm0, xmm0);
	c.cvtpd2ps(xmm1, xmm1);
	c.movlhps(xmm0, xmm1);
}
