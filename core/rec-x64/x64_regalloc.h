/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef CORE_REC_X64_X64_REGALLOC_H_
#define CORE_REC_X64_X64_REGALLOC_H_

//#define OLD_REGALLOC

#include "deps/xbyak/xbyak.h"
#ifdef OLD_REGALLOC
#include "hw/sh4/dyna/regalloc.h"
#else
#include "hw/sh4/dyna/ssa_regalloc.h"
#endif

/* r15 is not handed out: recompiled code keeps the address of the SH4's
 * context in it (see CTX_BIAS in rec_x64.cpp). Nor is r14, which has what
 * is left of the time slice (cycle_counter there) - it was the fifth
 * register a block could have on Linux, and blocks came out no smaller
 * and no quicker for having it. */
#ifdef _WIN32
static Xbyak::Operand::Code alloc_regs[] = { Xbyak::Operand::RBX, Xbyak::Operand::RBP, Xbyak::Operand::RDI, Xbyak::Operand::RSI,
		Xbyak::Operand::R12, Xbyak::Operand::R13, (Xbyak::Operand::Code)-1 };
static s8 alloc_fregs[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -1 };          // XMM6 to XMM15 are callee-saved in Windows
#else
static Xbyak::Operand::Code alloc_regs[] = { Xbyak::Operand::RBX, Xbyak::Operand::RBP, Xbyak::Operand::R12, Xbyak::Operand::R13,
		(Xbyak::Operand::Code)-1 };
// XMM8 to XMM15. No function has to keep any of them on these hosts: the
// ones in use are saved round every call recompiled code makes (GenCall).
static s8 alloc_fregs[] = { 8, 9, 10, 11, 12, 13, 14, 15, -1 };
#endif
/* A pair of the SH4's floating-point registers (DRn, XDn) is kept in two
 * xmm registers, as single ones are. On hosts where no function keeps an
 * xmm register, the ones in use are saved round every call (GenCall) and
 * round a rewritten memory access (ngen_call_keep_xmm), pairs as well. */
#define ALLOC_F64 true

class BlockCompiler;

struct X64RegAlloc : RegAlloc<Xbyak::Operand::Code, s8, ALLOC_F64>
{
	X64RegAlloc(BlockCompiler *compiler) : compiler(compiler) {}

	void DoAlloc(RuntimeBlockInfo* block)
	{
		RegAlloc::DoAlloc(block, alloc_regs, alloc_fregs);
	}

	virtual void Preload(u32 reg, Xbyak::Operand::Code nreg) override;
	virtual void Writeback(u32 reg, Xbyak::Operand::Code nreg) override;
	virtual void Preload_FPU(u32 reg, s8 nreg) override;
	virtual void Writeback_FPU(u32 reg, s8 nreg) override;

	Xbyak::Reg32 MapRegister(const shil_param& param)
	{
		Xbyak::Operand::Code ereg = mapg(param);
		if (ereg == (Xbyak::Operand::Code)-1)
			die("Register not allocated");
		return Xbyak::Reg32(ereg);
	}

	Xbyak::Xmm MapXRegister(const shil_param& param, u32 index = 0)
	{
#ifdef OLD_REGALLOC
		s8 ereg = mapfv(param, index);
#else
		s8 ereg = mapf(param, index);
#endif
		if (ereg == -1)
			die("VRegister not allocated");
		return Xbyak::Xmm(ereg);
	}

	bool IsMapped(const Xbyak::Xmm &xmm, size_t opid)
	{
#ifndef OLD_REGALLOC
		return regf_used((s8)xmm.getIdx());
#else
		for (size_t sid = 0; sid < all_spans.size(); sid++)
		{
			if (all_spans[sid]->nregf == xmm.getIdx() && all_spans[sid]->contains(opid))
				return true;
		}
		return false;
#endif
	}

	BlockCompiler *compiler;
};

#endif /* CORE_REC_X64_X64_REGALLOC_H_ */
