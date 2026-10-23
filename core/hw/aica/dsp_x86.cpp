/*
	Copyright 2021 flyinghead

	This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
/* The sound chip's DSP recompiler for 32-bit x86. It is the x86-64 one
 * (dsp_x64.cpp) step for step, so that the two leave the same state and
 * the same sound to the bit - a game played over netplay between the two
 * kinds of machine has to - with what this processor has: four registers
 * for the DSP's (INPUTS, ACC, B and Y), and the rest of what x86-64 keeps
 * in registers in words of this file's (Y_REG, ADRS_REG, MDEC_CT and
 * SHIFTED). Those are worked out again at the top of every sample, as
 * they are there, and are not in the state. */
#include "build.h"

#if HOST_CPU == CPU_X86 && FEAT_DSPREC != DYNAREC_NONE

#include "deps/xbyak/xbyak.h"
#include "dsp.h"
#include "aica_if.h"
#include "hw/mem/_vmem.h"

DECL_ALIGN(4096) static u8 CodeBuffer[32 * 1024]
#if defined(_WIN32) || defined(__HAIKU__)
	;
#elif defined(__linux__)
	__attribute__((section(".text")));
#elif defined(__MACH__)
	__attribute__((section("__TEXT,.text")));
#else
	#error CodeBuffer code section unknown
#endif
static u8 *pCodeBuffer;

// what x86-64 has in r13, r14 and r15, and in edx between the shifter and its uses
static s32 y_reg;
static s32 adrs_reg;
static u32 mdec_ct;
static s32 shifted;

class DSPAssembler : public Xbyak::CodeGenerator
{
public:
	DSPAssembler(u8 *code_buffer, size_t size) : Xbyak::CodeGenerator(size, code_buffer) {}

	void Compile(struct dsp_t *DSP)
	{
		this->DSP = DSP;
		DEBUG_LOG(AICA_ARM, "DSPAssembler::DSPCompile recompiling for x86 at %p", this->getCode());

		if (DSP->Stopped)
		{
			// (nothing to do, and EFREG is left as it is: see dsp_interp.cpp)
			ret();
			ready();

			return;
		}

		push(ebx);
		push(ebp);
		push(esi);
		push(edi);
		sub(esp, 12);	// 16-byte stack alignment
		const Xbyak::Reg32 INPUTS = esi;	// 24 bits
		const Xbyak::Reg32 ACC = edi;		// 26 bits
		const Xbyak::Reg32 B = ebp;			// 26 bits
		const Xbyak::Reg32 Y = ebx;			// 13 bits
		xor_(ACC, ACC);
		mov(dword[&DSP->FRC_REG], 0);
		mov(dword[&y_reg], 0);
		mov(dword[&adrs_reg], 0);
		mov(eax, dword[&DSP->regs.MDEC_CT]);
		mov(dword[&mdec_ct], eax);

		// The empty steps after the last one in use are left out: see dsp_x64.cpp
		int steps = 128;
		while (steps > 0 && DSPData->MPRO[steps * 4 - 4] == 0 && DSPData->MPRO[steps * 4 - 3] == 0
				&& DSPData->MPRO[steps * 4 - 2] == 0 && DSPData->MPRO[steps * 4 - 1] == 0)
			steps--;

		for (int step = 0; step < steps; ++step)
		{
			u32 *mpro = &DSPData->MPRO[step * 4];
			_INST op;
			DecodeInst(mpro, &op);
			const u32 COEF = step;

			if (op.XSEL || op.YRL || (op.ADRL && op.SHIFT != 3))
			{
				// (IRA is six bits of the program's: any value gets here,
				// and the ones that select nothing read as 0)
				if (op.IRA <= 0x1f)
					//INPUTS = DSP->MEMS[op.IRA];
					mov(INPUTS, dword[&DSP->MEMS[op.IRA]]);
				else if (op.IRA <= 0x2F)
				{
					//INPUTS = DSP->MIXS[op.IRA - 0x20] << 4;		// MIXS is 20 bit
					mov(INPUTS, dword[&DSP->MIXS[op.IRA - 0x20]]);
					shl(INPUTS, 4);
				}
				else if (op.IRA <= 0x31)
				{
					//INPUTS = DSPData->EXTS[op.IRA - 0x30] << 8;	// EXTS is 16 bits
					mov(INPUTS, dword[&DSPData->EXTS[op.IRA - 0x30]]);
					shl(INPUTS, 8);
				}
				else
				{
					xor_(INPUTS, INPUTS);
				}
			}

			if (op.IWT)
			{
				//DSP->MEMS[op.IWA] = MEMVAL[step & 3];	// MEMVAL was selected in previous MRD
				mov(eax, dword[&DSP->MEMVAL[step & 3]]);
				mov(dword[&DSP->MEMS[op.IWA]], eax);
			}

			// Operand sel
			// B
			if (!op.ZERO)
			{
				if (op.BSEL)
					//B = ACC;
					mov(B, ACC);
				else
				{
					//B = DSP->TEMP[(TRA + DSP->regs.MDEC_CT) & 0x7F];
					mov(eax, dword[&mdec_ct]);
					if (op.TRA)
						add(eax, op.TRA);
					and_(eax, 0x7f);
					mov(B, dword[eax * 4 + (u32)(uintptr_t)&DSP->TEMP[0]]);
				}
				if (op.NEGB)
					//B = 0 - B;
					neg(B);
			}

			if (op.TWT || op.FRCL || op.MWT || (op.ADRL && op.SHIFT == 3) || op.EWT)
			{
				// Shifter
				// There's a 1-step delay at the output of the X*Y + B adder. So we use the ACC value from the previous step.
				// (worked out here, before X: it only takes ACC, and X may want ecx)
				if (op.SHIFT == 0)
				{
					// SHIFTED = clamp(ACC, -0x80000, 0x7FFFF)
					cmp(ACC, 0xFF800000);
					mov(edx, 0xFF800000);
					mov(eax, 0x007FFFFF);
					cmovge(edx, ACC);
					cmp(edx, 0x007FFFFF);
					cmovg(edx, eax);
				}
				else if (op.SHIFT == 1)
				{
					//SHIFTED = ACC << 1;	// x2 scale
					mov(ecx, ACC);
					shl(ecx, 1);
					// SHIFTED = clamp(SHIFTED, -0x80000, 0x7FFFF)
					cmp(ecx, 0xFF800000);
					mov(edx, 0xFF800000);
					mov(eax, 0x007FFFFF);
					cmovge(edx, ecx);
					cmp(edx, 0x007FFFFF);
					cmovg(edx, eax);
				}
				else if (op.SHIFT == 2)
				{
					//SHIFTED = ACC << 1;	// x2 scale
					mov(edx, ACC);
					shl(edx, 1);
				}
				else if (op.SHIFT == 3)
				{
					//SHIFTED = ACC;
					mov(edx, ACC);
				}
				mov(dword[&shifted], edx);
			}

			// X
			Xbyak::Reg32 X = ecx;
			if (op.XSEL)
				//X = INPUTS;
				X = INPUTS;
			else
			{
				//X = DSP->TEMP[(TRA + DSP->regs.MDEC_CT) & 0x7F];
				// (B was read from there, and will do unless it is negated: see dsp_x64.cpp)
				if (!op.ZERO && !op.BSEL && !op.NEGB)
					X = B;
				else
				{
					mov(eax, dword[&mdec_ct]);
					if (op.TRA)
						add(eax, op.TRA);
					and_(eax, 0x7f);
					mov(ecx, dword[eax * 4 + (u32)(uintptr_t)&DSP->TEMP[0]]);
				}
			}

			// Y
			if (op.YSEL == 0)
			{
				//Y = FRC_REG;
				mov(Y, dword[&DSP->FRC_REG]);
			}
			else if (op.YSEL == 1)
			{
				//Y = DSPData->COEF[COEF] >> 3;	//COEF is 16 bits
				movsx(Y, word[&DSPData->COEF[COEF]]);
				sar(Y, 3);
			}
			else if (op.YSEL == 2)
			{
				//Y = Y_REG >> 11;
				mov(Y, dword[&y_reg]);
				sar(Y, 11);
			}
			else if (op.YSEL == 3)
			{
				//Y = (Y_REG >> 4) & 0x0FFF;
				mov(Y, dword[&y_reg]);
				sar(Y, 4);
				and_(Y, 0x0fff);
			}

			if (op.YRL)
				//Y_REG = INPUTS;
				mov(dword[&y_reg], INPUTS);

			// ACCUM
			//ACC = (((s64)X * (s64)Y) >> 12) + B;
			mov(eax, X);
			imul(Y);
			shrd(eax, edx, 12);
			mov(ACC, eax);
			if (!op.ZERO)
				add(ACC, B);

			if (op.TWT)
			{
				//DSP->TEMP[(op.TWA + DSP->regs.MDEC_CT) & 0x7F] = SHIFTED;
				mov(ecx, dword[&mdec_ct]);
				if (op.TWA)
					add(ecx, op.TWA);
				and_(ecx, 0x7f);
				mov(edx, dword[&shifted]);
				mov(dword[ecx * 4 + (u32)(uintptr_t)&DSP->TEMP[0]], edx);
			}

			if (op.FRCL)
			{
				mov(ecx, dword[&shifted]);
				if (op.SHIFT == 3)
					//FRC_REG = SHIFTED & 0x0FFF;
					and_(ecx, 0xFFF);
				else
					//FRC_REG = SHIFTED >> 11;
					sar(ecx, 11);
				mov(dword[&DSP->FRC_REG], ecx);
			}

			if (step & 1)
			{
				// (what x86-64 keeps round the calls here is in memory, or in a register a call keeps)
				const Xbyak::Reg32 ADDR = Y;
				if (op.MRD)			// memory only allowed on odd. DoA inserts NOPs on even
				{
					//MEMVAL[(step + 2) & 3] = UNPACK(*(u16 *)&aica_ram[ADDR & ARAM_MASK]);
					CalculateADDR(ADDR, op);
					movzx(ecx, word[ADDR + (u32)(uintptr_t)&aica_ram[0]]);
					call((const void *)&UNPACK);
					mov(dword[&DSP->MEMVAL[(step + 2) & 3]], eax);
				}
				if (op.MWT)
				{
					// *(u16 *)&aica_ram[ADDR & ARAM_MASK] = PACK(SHIFTED);
					mov(ecx, dword[&shifted]);
					call((const void *)&PACK);

					CalculateADDR(ADDR, op);
					mov(word[ADDR + (u32)(uintptr_t)&aica_ram[0]], ax);
				}
			}

			if (op.ADRL)
			{
				if (op.SHIFT == 3)
				{
					//ADRS_REG = SHIFTED >> 12;
					mov(eax, dword[&shifted]);
					sar(eax, 12);
				}
				else
				{
					//ADRS_REG = INPUTS >> 16;
					mov(eax, INPUTS);
					sar(eax, 16);
				}
				mov(dword[&adrs_reg], eax);
			}

			if (op.EWT)
			{
				//DSPData->EFREG[op.EWA] = SHIFTED >> 8;
				mov(edx, dword[&shifted]);
				sar(edx, 8);
				mov(dword[&DSPData->EFREG[op.EWA]], edx);
			}
		}
		// DSP->regs.MDEC_CT--
		mov(eax, dsp.RBL + 1);
		mov(ecx, dword[&mdec_ct]);
		sub(ecx, 1);
		//if (dsp.regs.MDEC_CT == 0)
		//	dsp.regs.MDEC_CT = dsp.RBL + 1;			// RBL is ring buffer length - 1
		cmove(ecx, eax);
		mov(dword[&DSP->regs.MDEC_CT], ecx);

		add(esp, 12);
		pop(edi);
		pop(esi);
		pop(ebp);
		pop(ebx);

		ret();
		ready();
	}

private:
	// Uses ecx
	void CalculateADDR(const Xbyak::Reg32 ADDR, const _INST& op)
	{
		//u32 ADDR = DSPData->MADRS[op.MASA];
		mov(ADDR, dword[&DSPData->MADRS[op.MASA]]);
		if (op.ADREB)
		{
			//ADDR += ADRS_REG & 0x0FFF;
			mov(ecx, dword[&adrs_reg]);
			and_(ecx, 0x0FFF);
			add(ADDR, ecx);
		}
		if (op.NXADR)
			//ADDR++;
			add(ADDR, 1);
		if (!op.TABLE)
		{
			//ADDR += DSP->regs.MDEC_CT;
			add(ADDR, dword[&mdec_ct]);
			//ADDR &= DSP->RBL;
			// RBL is constant for this program
			and_(ADDR, DSP->RBL);
		}
		else
			//ADDR &= 0xFFFF;
			and_(ADDR, 0xFFFF);

		//ADDR <<= 1;				// Word -> byte address
		shl(ADDR, 1);
		//ADDR += DSP->RBP;			// RBP is already a byte address
		// RBP is constant for this program
		add(ADDR, DSP->RBP);
		// ADDR & ARAM_MASK, and even: see dsp_x64.cpp
		and_(ADDR, ARAM_MASK & ~1);
	}

	struct dsp_t *DSP = nullptr;
};

// What the code was made from: see dsp_x64.cpp
static u32 compiled_mpro[128 * 4];
static bool compiled;
static u32 compiled_rbl, compiled_rbp, compiled_mask;
static u8 *compiled_ram;

void dsp_recompile()
{
	dsp.Stopped = true;
	for (int i = 127; i >= 0; --i)
	{
		u32 *IPtr = DSPData->MPRO + i * 4;

		if (IPtr[0] != 0 || IPtr[1] != 0 || IPtr[2 ]!= 0 || IPtr[3] != 0)
		{
			dsp.Stopped = false;
			break;
		}
	}
	if (compiled && memcmp(compiled_mpro, DSPData->MPRO, sizeof(compiled_mpro)) == 0
			&& compiled_rbl == dsp.RBL && compiled_rbp == dsp.RBP
			&& compiled_mask == ARAM_MASK && compiled_ram == &aica_ram[0])
		return;
	memcpy(compiled_mpro, DSPData->MPRO, sizeof(compiled_mpro));
	compiled_rbl = dsp.RBL;
	compiled_rbp = dsp.RBP;
	compiled_mask = ARAM_MASK;
	compiled_ram = &aica_ram[0];
	compiled = true;
	DSPAssembler assembler(pCodeBuffer, sizeof(CodeBuffer));
	assembler.Compile(&dsp);
}

void dsp_init()
{
	memset(&dsp, 0, sizeof(dsp));
	dsp.RBL = 0x8000 - 1;
	dsp.RBP = 0;
	dsp.regs.MDEC_CT = 1;
	dsp.dyndirty = true;

	// Once: see dsp_x64.cpp
	if (pCodeBuffer == nullptr
			&& !vmem_platform_prepare_jit_block(CodeBuffer, sizeof(CodeBuffer), (void**)&pCodeBuffer))
		die("mprotect failed in x86 dsp");
}

void dsp_step()
{
	if (dsp.dyndirty)
	{
		dsp.dyndirty = false;
		dsp_recompile();
	}

	((void (*)())pCodeBuffer)();
}

void dsp_writenmem(u32 addr)
{
	if (addr >= 0x3400 && addr < 0x3C00)
	{
		dsp.dyndirty = true;
	}
	// (TEMP and MEMS are written in aica_mem.cpp: they are the DSP's own)
}

void dsp_term()
{
}
#endif
