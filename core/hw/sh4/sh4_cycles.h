/* How many cycles an SH4 instruction takes, as upstream flycast counts them
 * (its sh4_cycles.cpp, 2023, by flyinghead).
 *
 * The SH4 issues two instructions in a cycle when they do not need the same
 * part of the processor: each instruction belongs to an execution unit, and
 * one that follows another from a different unit goes with it, at no cost.
 * Otherwise it costs its issue cycles, which is one for most. On top of
 * that the first three instructions of a block that go to memory are
 * charged for the access.
 *
 * This is the "accurate" setting of the SH4 Timing core option. The other,
 * the rule of thumb this core has always had, charges every integer
 * instruction 8 cycles and every floating-point one nothing: about three
 * times what this comes to, so that the processor it describes is a third
 * as fast. */
#pragma once
#include "types.h"
#include "sh4_opcode_list.h"

struct Sh4Cycles
{
	sh4_eu lastUnit = CO;
	int memOps = 0;

	// at the start of a block
	void reset()
	{
		lastUnit = CO;
		memOps = 0;
	}

	// @memCycles: what an access to memory costs, more with the MMU on
	int count(u16 op, int memCycles)
	{
		// by group of the manual's exception table: the ones that access memory
		static const bool isMemOp[45] = {
			false, false,
			true,	// all mem moves, ldtlb, sts.l FPUL/FPSCR, @-Rn, lds.l @Rn+,FPUL
			true,	// gbr-based load/store
			false,
			true,	// tst.b #<imm8>, @(R0,GBR)
			true,	// and/or/xor.b #<imm8>, @(R0,GBR)
			true,	// tas.b @Rn
			false, false, false, false,
			true,	// movca.l R0, @Rn
			false, false, false, false,
			true,	// ldc.l @Rn+, VBR/SPC/SSR/Rn_Bank/DBR
			true,	// ldc.l @Rn+, GBR/SGR
			true,	// ldc.l @Rn+, SR
			false, false,
			true,	// stc.l DBR/SR/GBR/VBR/SSR/SPC/Rn_Bank, @-Rn
			true,	// stc.l SGR, @-Rn
			false,
			true,	// lds.l @Rn+, PR
			false,
			true,	// sts.l PR, @-Rn
			false,
			true,	// lds.l @Rn+, MACH/MACL
			false,
			true,	// sts.l MACH/MACL, @-Rn
			false,
			true,	// lds.l @Rn+,FPSCR
			false,
			true,	// mac.wl @Rm+,@Rn+
		};
		const sh4_opcodelistentry *opcode = OpDesc[op];
		int cycles = 0;

		if (opcode->ex_type < 45 && isMemOp[opcode->ex_type] && ++memOps < 4)
			cycles = memCycles;

		if (lastUnit == CO || opcode->unit == CO
				|| (lastUnit == opcode->unit && lastUnit != MT))
		{
			// cannot go with the one before
			lastUnit = opcode->unit;
			cycles += opcode->IssueCycles;
		}
		else
			// goes with the one before; the next starts afresh
			lastUnit = CO;
		return cycles;
	}
};
