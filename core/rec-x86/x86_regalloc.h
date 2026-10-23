/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once

#include "deps/xbyak/xbyak.h"
#include "hw/sh4/dyna/ssa_regalloc.h"

/* The four registers a function keeps on 32-bit x86 are handed out; eax,
 * ecx and edx are scratch, for the calls blocks make (fastcall: ecx, edx)
 * and for whatever an operation needs on the side. xmm4 to xmm7 are handed
 * out and xmm0 to xmm3 are scratch: no function keeps any of them, so the
 * ones in use are saved round every call (BlockCompiler::GenCall()). */
static Xbyak::Operand::Code alloc_regs[] = { Xbyak::Operand::EBX, Xbyak::Operand::EBP, Xbyak::Operand::ESI,
		Xbyak::Operand::EDI, (Xbyak::Operand::Code)-1 };
static s8 alloc_fregs[] = { 7, 6, 5, 4, -1 };
/* Pairs of the SH4's floating-point registers (DRn, XDn) stay in memory:
 * four xmm registers are not enough to give them two each. */
#define ALLOC_F64 false

class BlockCompiler;

struct X86RegAlloc : RegAlloc<Xbyak::Operand::Code, s8, ALLOC_F64>
{
	X86RegAlloc(BlockCompiler *compiler) : compiler(compiler) {}

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
		s8 ereg = mapf(param, index);
		if (ereg == -1)
			die("VRegister not allocated");
		return Xbyak::Xmm(ereg);
	}

	bool IsMapped(const Xbyak::Xmm &xmm, size_t opid)
	{
		return regf_used((s8)xmm.getIdx());
	}

	BlockCompiler *compiler;
};
