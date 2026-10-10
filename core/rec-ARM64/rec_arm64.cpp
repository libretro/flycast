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

#include "types.h"

#if FEAT_SHREC == DYNAREC_JIT

#include <unistd.h>
#include <map>
#include <setjmp.h>

#include "deps/vixl/aarch64/macro-assembler-aarch64.h"
using namespace vixl::aarch64;

//#define EXPLODE_SPANS

#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/wait_site.h"

#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_rom.h"
#include "arm64_regalloc.h"

#undef do_sqw_nommu

extern "C" void ngen_blockcheckfail(u32 pc);
extern "C" void ngen_LinkBlock_Generic_stub();
extern "C" void ngen_LinkBlock_cond_Branch_stub();
extern "C" void ngen_LinkBlock_cond_Next_stub();
extern "C" void ngen_FailedToFindBlock_mmu();
extern "C" void ngen_FailedToFindBlock_nommu();
extern void vmem_platform_flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end);
static void generate_mainloop();

struct DynaRBI : RuntimeBlockInfo
{
	virtual u32 Relink() override;

	virtual void Relocate(void* dst) override {
		verify(false);
	}
};

static jmp_buf jmp_env;

/* The kinds of fast memory access there are (Arm64Assembler::GenFastAccess()):
 * a load or a store, of 1, 2, 4 or 8 bytes, the 4 from or to a general
 * register or a floating-point one. */
enum
{
	MEM_LOAD8, MEM_LOAD16, MEM_LOAD32, MEM_LOAD32F, MEM_LOAD64,
	MEM_STORE8, MEM_STORE16, MEM_STORE32, MEM_STORE32F, MEM_STORE64,
	MEM_KINDS
};
// (whether a kind's data is in a floating-point register)
#define MEM_KIND_F(kind) ((kind) == MEM_LOAD32F || (kind) == MEM_STORE32F)

/* What a fast access that faulted is written over with a call to
 * (ngen_Rewrite()): a routine that does the access by asking. A fast
 * access has its address and its data in whatever registers the block has
 * them in, so there is a routine for each kind of access, each register
 * the address can be in and each the data can be in - 528 of them, a few
 * instructions each, written after the main loop each time that is.
 *
 * The registers, by number: for the address and for data in a general
 * register, 0 is the one a call has it in (w0; w1 for what is stored) and
 * 1 on are the block's own, alloc_regs; for data in a floating-point
 * register, 0 on are the block's, alloc_fregs. 64 bits are always in x0
 * or x1. */
#define MEM_REGS 8
static u8 *mem_stubs[MEM_KINDS][MEM_REGS][MEM_REGS];

// (signed char and short by name: s8 and s16 are registers as well here)
static s32 mem_load8(u32 addr)  { return (signed char)ReadMem8(addr); }
static s32 mem_load16(u32 addr) { return (short)ReadMem16(addr); }
static u32 mem_load32(u32 addr) { return ReadMem32(addr); }
static u64 mem_load64(u32 addr) { return ReadMem64(addr); }
static void mem_store8(u32 addr, u32 data)  { WriteMem8(addr, (u8)data); }
static void mem_store16(u32 addr, u32 data) { WriteMem16(addr, (u16)data); }
static void mem_store32(u32 addr, u32 data) { WriteMem32(addr, data); }
static void mem_store64(u32 addr, u64 data) { WriteMem64(addr, data); }

static int mem_block_regs()
{
	int count = 0;

	while (alloc_regs[count] != (eReg)-1)
		count++;
	return count;
}

static int mem_block_fregs()
{
	int count = 0;

	while (alloc_fregs[count] != (eFReg)-1)
		count++;
	return count;
}

/* The number a general register goes by here: 0 if it is @first, 1 on for
 * the block's own, -1 for any other. */
static int mem_reg_number(int reg, int first)
{
	if (reg == first)
		return 0;
	for (int i = 0; alloc_regs[i] != (eReg)-1; i++)
		if ((int)alloc_regs[i] == reg)
			return i + 1;
	return -1;
}

// The same for a floating-point register, which is one of the block's or nothing
static int mem_freg_number(int reg)
{
	for (int i = 0; alloc_fregs[i] != (eFReg)-1; i++)
		if ((int)alloc_fregs[i] == reg)
			return i;
	return -1;
}

// The register of the SH4's memory as the host has it mapped: see arm64_regalloc.h
#define MEM_BASE x26
#define MEM_BASE_CODE 26

static void (*mainloop)(void *context);
static int (*arm64_intc_sched)();
static void (*arm64_no_update)();

static bool restarting;

void ngen_mainloop(void* v_cntx)
{
	do {
		restarting = false;
		generate_mainloop();

		mainloop(v_cntx);
		if (restarting)
			p_sh4rcb->cntx.CpuRunning = 1;
	} while (restarting);
}

void ngen_init()
{
	INFO_LOG(DYNAREC, "Initializing the ARM64 dynarec");
	ngen_FailedToFindBlock = &ngen_FailedToFindBlock_nommu;
}

void ngen_ResetBlocks()
{
	mainloop = NULL;
	if (mmu_enabled())
		ngen_FailedToFindBlock = &ngen_FailedToFindBlock_mmu;
	else
		ngen_FailedToFindBlock = &ngen_FailedToFindBlock_nommu;
	if (p_sh4rcb->cntx.CpuRunning)
	{
		// Force the dynarec out of mainloop() to regenerate it
		p_sh4rcb->cntx.CpuRunning = 0;
		restarting = true;
	}
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds     = false;
}

template<typename T>
static T ReadMemNoEx(u32 addr, u32, u32 pc)
{
#ifndef NO_MMU
	u32 ex;
	T rv = mmu_ReadMemNoEx<T>(addr, &ex);
	if (ex)
	{
		spc = pc;
		longjmp(jmp_env, 1);
	}
	return rv;
#else
	return (T)0;	// not used
#endif
}

template<typename T>
static void WriteMemNoEx(u32 addr, T data, u32 pc)
{
#ifndef NO_MMU
	u32 ex = mmu_WriteMemNoEx<T>(addr, data);
	if (ex)
	{
		spc = pc;
		longjmp(jmp_env, 1);
	}
#endif
}

static void interpreter_fallback(u16 op, OpCallFP *oph, u32 pc)
{
	try {
		oph(op);
	} catch (SH4ThrownException& ex) {
		if (pc & 1)
		{
			// Delay slot
			AdjustDelaySlotException(ex);
			pc--;
		}
		Do_Exception(pc, ex.expEvn, ex.callVect);
		longjmp(jmp_env, 1);
	}
}

static void do_sqw_mmu_no_ex(u32 addr, u32 pc)
{
	try {
		do_sqw_mmu(addr);
	} catch (SH4ThrownException& ex) {
		if (pc & 1)
		{
			// Delay slot
			AdjustDelaySlotException(ex);
			pc--;
		}
		Do_Exception(pc, ex.expEvn, ex.callVect);
		longjmp(jmp_env, 1);
	}
}

class Arm64Assembler : public MacroAssembler
{
	typedef void (MacroAssembler::*Arm64Op_RRO)(const Register&, const Register&, const Operand&);
	typedef void (MacroAssembler::*Arm64Op_RROF)(const Register&, const Register&, const Operand&, enum FlagsUpdate);
	typedef void (MacroAssembler::*Arm64Fop_RRR)(const VRegister&, const VRegister&, const VRegister&);

public:
	Arm64Assembler() : Arm64Assembler(emit_GetCCPtr())
	{
	}
	Arm64Assembler(void *buffer) : MacroAssembler((u8 *)buffer, emit_FreeSpace()), regalloc(this)
	{
		call_regs.push_back(&w0);
		call_regs.push_back(&w1);
		call_regs.push_back(&w2);
		call_regs.push_back(&w3);
		call_regs.push_back(&w4);
		call_regs.push_back(&w5);
		call_regs.push_back(&w6);
		call_regs.push_back(&w7);

		call_regs64.push_back(&x0);
		call_regs64.push_back(&x1);
		call_regs64.push_back(&x2);
		call_regs64.push_back(&x3);
		call_regs64.push_back(&x4);
		call_regs64.push_back(&x5);
		call_regs64.push_back(&x6);
		call_regs64.push_back(&x7);

		call_fregs.push_back(&s0);
		call_fregs.push_back(&s1);
		call_fregs.push_back(&s2);
		call_fregs.push_back(&s3);
		call_fregs.push_back(&s4);
		call_fregs.push_back(&s5);
		call_fregs.push_back(&s6);
		call_fregs.push_back(&s7);
	}

	void ngen_BinaryOp_RRO(shil_opcode* op, Arm64Op_RRO arm_op, Arm64Op_RROF arm_op2)
	{
		Operand op3 = Operand(0);
		if (op->rs2.is_imm())
		{
			op3 = Operand(op->rs2._imm);
		}
		else if (op->rs2.is_r32i())
		{
			op3 = Operand(regalloc.MapRegister(op->rs2));
		}
		if (arm_op != NULL)
			((*this).*arm_op)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), op3);
		else
			((*this).*arm_op2)(regalloc.MapRegister(op->rd), regalloc.MapRegister(op->rs1), op3, LeaveFlags);
	}

	void ngen_BinaryFop(shil_opcode* op, Arm64Fop_RRR arm_op)
	{
		VRegister reg1;
		VRegister reg2;
		if (op->rs1.is_imm())
		{
			Fmov(s0, reinterpret_cast<f32&>(op->rs1._imm));
			reg1 = s0;
		}
		else
		{
			reg1 = regalloc.MapVRegister(op->rs1);
		}
		if (op->rs2.is_imm())
		{
			Fmov(s1, reinterpret_cast<f32&>(op->rs2._imm));
			reg2 = s1;
		}
		else
		{
			reg2 = regalloc.MapVRegister(op->rs2);
		}
		((*this).*arm_op)(regalloc.MapVRegister(op->rd), reg1, reg2);
	}

	const Register& GenMemAddr(const shil_opcode& op, const Register* raddr = NULL)
	{
		const Register* ret_reg = raddr == NULL ? &w0 : raddr;

		if (op.rs3.is_imm())
		{
			if (regalloc.IsAllocg(op.rs1))
				Add(*ret_reg, regalloc.MapRegister(op.rs1), op.rs3._imm);
			else
			{
				Ldr(*ret_reg, sh4_context_mem_operand(op.rs1.reg_ptr()));
				Add(*ret_reg, *ret_reg, op.rs3._imm);
			}
		}
		else if (op.rs3.is_r32i())
		{
			if (regalloc.IsAllocg(op.rs1) && regalloc.IsAllocg(op.rs3))
				Add(*ret_reg, regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs3));
			else
			{
				Ldr(*ret_reg, sh4_context_mem_operand(op.rs1.reg_ptr()));
				Ldr(w8, sh4_context_mem_operand(op.rs3.reg_ptr()));
				Add(*ret_reg, *ret_reg, w8);
			}
		}
		else if (!op.rs3.is_null())
		{
			die("invalid rs3");
		}
		else if (op.rs1.is_reg())
		{
			if (regalloc.IsAllocg(op.rs1))
			{
				if (raddr == NULL)
					ret_reg = &regalloc.MapRegister(op.rs1);
				else
					Mov(*ret_reg, regalloc.MapRegister(op.rs1));
			}
			else
			{
				Ldr(*ret_reg, sh4_context_mem_operand(op.rs1.reg_ptr()));
			}
		}
		else
		{
			verify(op.rs1.is_imm());
			Mov(*ret_reg, op.rs1._imm);
		}

		return *ret_reg;
	}

	void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
	{
		//printf("REC-ARM64 compiling %08x\n", block->addr);
		this->block = block;
		CheckBlock(force_checks, block);

		// run register allocator
		regalloc.DoAlloc(block);

		/* scheduler: what is left of the time slice is w27. With the MMU
		 * on a block can be left by an exception, which comes back to the
		 * main loop by longjmp() with w27 as it was at the setjmp(): so
		 * there is a copy in the main loop's stack frame, written here,
		 * for it to start again from. (With the MMU on it used to be a
		 * word in memory only: seven instructions here, for these two.) */
		Subs(w27, w27, block->guest_cycles);
		if (mmu_enabled())
			Str(w27, MemOperand(sp, 8));
		Label cycles_remaining;
		B(&cycles_remaining, pl);
		GenCall(*arm64_intc_sched);
		Label cpu_running;
		Cbnz(w0, &cpu_running);
		Mov(w29, block->vaddr);
		Str(w29, sh4_context_mem_operand(&next_pc));
		GenBranch(*arm64_no_update);
		Bind(&cpu_running);
		Bind(&cycles_remaining);

		const bool accurate = settings.dynarec.AccurateTiming && !mmu_enabled();
		if (accurate && sh4_block_writes(block))
		{
			// a block that can change something other than a register says so: see wait_site.h
			Mov(x1, reinterpret_cast<uintptr_t>(&sh4_write_gen));
			Ldr(w0, MemOperand(x1));
			Add(w0, w0, 1);
			Str(w0, MemOperand(x1));
		}

		for (size_t i = 0; i < block->oplist.size(); i++)
		{
			shil_opcode& op  = block->oplist[i];
			regalloc.OpBegin(&op, i);

			switch (op.op)
			{
			case shop_ifb:	// Interpreter fallback
				if (op.rs1._imm)	// if NeedPC()
				{
					Mov(w10, op.rs2._imm);
					Str(w10, sh4_context_mem_operand(&next_pc));
				}
				Mov(*call_regs[0], op.rs3._imm);

				if (!mmu_enabled())
				{
					GenCallRuntime(OpDesc[op.rs3._imm]->oph);
				}
				else
				{
					Mov(*call_regs64[1], reinterpret_cast<uintptr_t>(*OpDesc[op.rs3._imm]->oph));	// op handler
					Mov(*call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

					GenCallRuntime(interpreter_fallback);
				}

				break;

			case shop_jcond:
			case shop_jdyn:
				{
					const Register rd = regalloc.MapRegister(op.rd);
					if (op.rs2.is_imm())
						Add(rd, regalloc.MapRegister(op.rs1), op.rs2._imm);
					else
						Mov(rd, regalloc.MapRegister(op.rs1));
					// Save it for the branching at the end of the block
					Mov(w29, rd);
				}
				break;

			case shop_mov32:
				verify(op.rd.is_reg());
				verify(op.rs1.is_reg() || op.rs1.is_imm());

				if (regalloc.IsAllocf(op.rd))
				{
					const VRegister rd = regalloc.MapVRegister(op.rd);
					if (op.rs1.is_imm())
						Fmov(rd, reinterpret_cast<f32&>(op.rs1._imm));
					else if (regalloc.IsAllocf(op.rs1))
						Fmov(rd, regalloc.MapVRegister(op.rs1));
					else
						Fmov(rd, regalloc.MapRegister(op.rs1));
				}
				else
				{
					const Register rd = regalloc.MapRegister(op.rd);
					if (op.rs1.is_imm())
						Mov(rd, op.rs1._imm);
					else if (regalloc.IsAllocg(op.rs1))
						Mov(rd, regalloc.MapRegister(op.rs1));
					else
						Fmov(rd, regalloc.MapVRegister(op.rs1));
				}
				break;

			case shop_mov64:
				verify(op.rd.is_reg());
				verify(op.rs1.is_reg() || op.rs1.is_imm());

#ifdef EXPLODE_SPANS
				Fmov(regalloc.MapVRegister(op.rd, 0), regalloc.MapVRegister(op.rs1, 0));
				Fmov(regalloc.MapVRegister(op.rd, 1), regalloc.MapVRegister(op.rs1, 1));
#else
				shil_param_to_host_reg(op.rs1, x15);
				host_reg_to_shil_param(op.rd, x15);
#endif
				break;

			case shop_readm:
				GenReadMemory(op, i, optimise);
				break;

			case shop_writem:
				GenWriteMemory(op, i, optimise);
				break;

			case shop_sync_sr:
				GenCallRuntime(UpdateSR);
				break;
			case shop_sync_fpscr:
				GenCallRuntime(UpdateFPSCR);
				break;

			case shop_swaplb:
				{
					const Register rs1 = regalloc.MapRegister(op.rs1);
					const Register rd = regalloc.MapRegister(op.rd);
					Mov(w9, Operand(rs1, LSR, 16));
					Rev16(rd, rs1);
					Bfi(rd, w9, 16, 16);
				}
				break;

			case shop_neg:
				Neg(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
			case shop_not:
				Mvn(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			case shop_and:
				ngen_BinaryOp_RRO(&op, &MacroAssembler::And, NULL);
				break;
			case shop_or:
				ngen_BinaryOp_RRO(&op, &MacroAssembler::Orr, NULL);
				break;
			case shop_xor:
				ngen_BinaryOp_RRO(&op, &MacroAssembler::Eor, NULL);
				break;
			case shop_add:
				ngen_BinaryOp_RRO(&op, NULL, &MacroAssembler::Add);
				break;
			case shop_sub:
				ngen_BinaryOp_RRO(&op, NULL, &MacroAssembler::Sub);
				break;
			case shop_shl:
				if (op.rs2.is_imm())
					Lsl(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Lsl(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_shr:
				if (op.rs2.is_imm())
					Lsr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Lsr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_sar:
				if (op.rs2.is_imm())
					Asr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Asr(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;
			case shop_ror:
				if (op.rs2.is_imm())
					Ror(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), op.rs2._imm);
				else if (op.rs2.is_reg())
					Ror(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
				break;

			case shop_adc:
				{
					Register reg1;
					Operand op2;
					Register reg3;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
						op2 = Operand(op.rs2.imm_value());
					else
						op2 = regalloc.MapRegister(op.rs2);
					if (op.rs3.is_imm())
					{
						Mov(w1, op.rs3.imm_value());
						reg3 = w1;
					}
					else
					{
						reg3 = regalloc.MapRegister(op.rs3);
					}
					Cmp(reg3, 1);	// C = rs3
					Adcs(regalloc.MapRegister(op.rd), reg1, op2); // (C,rd)=rs1+rs2+rs3(C)
					Cset(regalloc.MapRegister(op.rd2), cs);	// rd2 = C
				}
				break;
			case shop_sbc:
				{
					Register reg1;
					Operand op2;
					Operand op3;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
						op2 = Operand(op.rs2.imm_value());
					else
						op2 = regalloc.MapRegister(op.rs2);
					if (op.rs3.is_imm())
						op3 = Operand(op.rs3.imm_value());
					else
						op3 = regalloc.MapRegister(op.rs3);
					Cmp(wzr, op3);	// C = ~rs3
					Sbcs(regalloc.MapRegister(op.rd), reg1, op2); // (C,rd) = rs1 - rs2 - ~rs3(C)
					Cset(regalloc.MapRegister(op.rd2), cc);	// rd2 = ~C
				}
				break;
			case shop_negc:
				{
					Operand op1;
					Operand op2;
					if (op.rs1.is_imm())
						op1 = Operand(op.rs1.imm_value());
					else
						op1 = regalloc.MapRegister(op.rs1);
					if (op.rs2.is_imm())
						op2 = Operand(op.rs2.imm_value());
					else
						op2 = regalloc.MapRegister(op.rs2);
					Cmp(wzr, op2);	// C = ~rs2
					Sbcs(regalloc.MapRegister(op.rd), wzr, op1);	// (C,rd) = 0 - rs1 - ~rs2(C)
					Cset(regalloc.MapRegister(op.rd2), cc);			// rd2 = ~C
				}
				break;

			case shop_rocr:
				{
					Register reg1;
					Register reg2;
					if (op.rs1.is_imm())
					{
						Mov(w1, op.rs1.imm_value());
						reg1 = w1;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
					{
						Mov(w2, op.rs2.imm_value());
						reg2 = w2;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Ubfx(w0, reg1, 0, 1);										// w0 = rs1[0] (new C)
					const Register rd = regalloc.MapRegister(op.rd);
					Mov(rd, Operand(reg1, LSR, 1));	// rd = rs1 >> 1
					Bfi(rd, reg2, 31, 1);				// rd |= C << 31
					Mov(regalloc.MapRegister(op.rd2), w0);						// rd2 = w0 (new C)
				}
				break;
			case shop_rocl:
				{
					Register reg1;
					Register reg2;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
					{
						Mov(w1, op.rs2.imm_value());
						reg2 = w1;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Tst(reg1, 0x80000000);						// Z = ~rs1[31]
					Orr(regalloc.MapRegister(op.rd), reg2, Operand(reg1, LSL, 1)); // rd = rs1 << 1 | rs2(C)
					Cset(regalloc.MapRegister(op.rd2), ne);		// rd2 = ~Z(C)
				}
				break;

			case shop_shld:
			case shop_shad:
				{
					Register reg1;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					Label positive_shift, negative_shift, end;
					const Register rs2 = regalloc.MapRegister(op.rs2);
					Tbz(rs2, 31, &positive_shift);
					Cmn(rs2, 32);
					B(&negative_shift, ne);
					const Register rd = regalloc.MapRegister(op.rd);
					// rs2 == -32 => rd = 0 (logical) or 0/-1 (arith)
					if (op.op == shop_shld)
						// Logical shift
						//Lsr(rd, reg1, 31);
						Mov(rd, wzr);
					else
						// Arithmetic shift
						Asr(rd, reg1, 31);
					B(&end);

					Bind(&positive_shift);
					// rs2 >= 0 => left shift
					Lsl(rd, reg1, rs2);
					B(&end);

					Bind(&negative_shift);
					// rs2 < 0 => right shift
					Neg(w1, rs2);
					if (op.op == shop_shld)
						// Logical shift
						Lsr(rd, reg1, w1);
					else
						// Arithmetic shift
						Asr(rd, reg1, w1);
					Bind(&end);
				}
				break;

			case shop_test:
			case shop_seteq:
			case shop_setge:
			case shop_setgt:
			case shop_setae:
			case shop_setab:
				{
					const Register rs1 = regalloc.MapRegister(op.rs1);
					if (op.op == shop_test)
					{
						if (op.rs2.is_imm())
							Tst(rs1, op.rs2._imm);
						else
							Tst(rs1, regalloc.MapRegister(op.rs2));
					}
					else
					{
						if (op.rs2.is_imm())
							Cmp(rs1, op.rs2._imm);
						else
							Cmp(rs1, regalloc.MapRegister(op.rs2));
					}

					static const Condition shop_conditions[] = { eq, eq, ge, gt, hs, hi };

					Cset(regalloc.MapRegister(op.rd), shop_conditions[op.op - shop_test]);
				}
				break;
			case shop_setpeq:
				{
					Register reg1;
					Register reg2;
					if (op.rs1.is_imm())
					{
						Mov(w0, op.rs1.imm_value());
						reg1 = w0;
					}
					else
					{
						reg1 = regalloc.MapRegister(op.rs1);
					}
					if (op.rs2.is_imm())
					{
						Mov(w1, op.rs2.imm_value());
						reg2 = w1;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Eor(w1, reg1, reg2);
					const Register rd = regalloc.MapRegister(op.rd);
					Mov(rd, wzr);
					Mov(w2, wzr);	// wzr not supported by csinc (?!)
					Tst(w1, 0xFF000000);
					Csinc(rd, rd, w2, ne);
					Tst(w1, 0x00FF0000);
					Csinc(rd, rd, w2, ne);
					Tst(w1, 0x0000FF00);
					Csinc(rd, rd, w2, ne);
					Tst(w1, 0x000000FF);
					Csinc(rd, rd, w2, ne);
				}
				break;

			case shop_mul_u16:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Uxth(w10, regalloc.MapRegister(op.rs1));
					Uxth(w11, reg2);
					Mul(regalloc.MapRegister(op.rd), w10, w11);
				}
				break;
			case shop_mul_s16:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Sxth(w10, regalloc.MapRegister(op.rs1));
					Sxth(w11, reg2);
					Mul(regalloc.MapRegister(op.rd), w10, w11);
				}
				break;
			case shop_mul_i32:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					Mul(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1), reg2);
				}
				break;
			case shop_mul_u64:
			case shop_mul_s64:
				{
					Register reg2;
					if (op.rs2.is_imm())
					{
						Mov(w0, op.rs2.imm_value());
						reg2 = w0;
					}
					else
					{
						reg2 = regalloc.MapRegister(op.rs2);
					}
					const Register& rd_xreg = Register::GetXRegFromCode(regalloc.MapRegister(op.rd).GetCode());
					if (op.op == shop_mul_u64)
						Umull(rd_xreg, regalloc.MapRegister(op.rs1), reg2);
					else
						Smull(rd_xreg, regalloc.MapRegister(op.rs1), reg2);
					const Register& rd2_xreg = Register::GetXRegFromCode(regalloc.MapRegister(op.rd2).GetCode());
					Lsr(rd2_xreg, rd_xreg, 32);
				}
				break;

			case shop_pref:
				{
					Label not_sqw;
					if (op.rs1.is_imm())
						Mov(*call_regs[0], op.rs1._imm);
					else
					{
						if (regalloc.IsAllocg(op.rs1))
							Lsr(w1, regalloc.MapRegister(op.rs1), 26);
						else
						{
							Ldr(w0, sh4_context_mem_operand(op.rs1.reg_ptr()));
							Lsr(w1, w0, 26);
						}
						Cmp(w1, 0x38);
						B(&not_sqw, ne);
						if (regalloc.IsAllocg(op.rs1))
							Mov(w0, regalloc.MapRegister(op.rs1));
					}

					if (mmu_enabled())
					{
						Mov(*call_regs[1], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

						GenCallRuntime(do_sqw_mmu_no_ex);
					}
					else
					{
						if (CCN_MMUCR.AT)
						{
							Ldr(x9, reinterpret_cast<uintptr_t>(&do_sqw_mmu));
						}
						else
						{
							Sub(x9, x28, offsetof(Sh4RCB, cntx) - offsetof(Sh4RCB, do_sqw_nommu));
							Ldr(x9, MemOperand(x9));
							Sub(x1, x28, offsetof(Sh4RCB, cntx) - offsetof(Sh4RCB, sq_buffer));
						}
						Blr(x9);
					}
					Bind(&not_sqw);
				}
				break;

			case shop_ext_s8:
				Sxtb(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
			case shop_ext_s16:
				Sxth(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			case shop_xtrct:
				{
					const Register rd = regalloc.MapRegister(op.rd);
					const Register rs1 = regalloc.MapRegister(op.rs1);
					const Register rs2 = regalloc.MapRegister(op.rs2);
					if (op.rs1._reg == op.rd._reg)
					{
						verify(op.rs2._reg != op.rd._reg);
						Lsr(rd, rs1, 16);
						Lsl(w0, rs2, 16);
					}
					else
					{
						Lsl(rd, rs2, 16);
						Lsr(w0, rs1, 16);
					}
					Orr(rd, rd, w0);
				}
				break;

			//
			// FPU
			//

			case shop_fadd:
				ngen_BinaryFop(&op, &MacroAssembler::Fadd);
				break;
			case shop_fsub:
				ngen_BinaryFop(&op, &MacroAssembler::Fsub);
				break;
			case shop_fmul:
				ngen_BinaryFop(&op, &MacroAssembler::Fmul);
				break;
			case shop_fdiv:
				ngen_BinaryFop(&op, &MacroAssembler::Fdiv);
				break;

			case shop_fabs:
				Fabs(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_fneg:
				Fneg(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;
			case shop_fsqrt:
				Fsqrt(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs1));
				break;

			case shop_fmac:
				Fmadd(regalloc.MapVRegister(op.rd), regalloc.MapVRegister(op.rs3), regalloc.MapVRegister(op.rs2), regalloc.MapVRegister(op.rs1));
				break;

			case shop_fsrra:
				Fsqrt(s0, regalloc.MapVRegister(op.rs1));
				Fmov(s1, 1.f);
				Fdiv(regalloc.MapVRegister(op.rd), s1, s0);
				break;

			case shop_fsetgt:
			case shop_fseteq:
				Fcmp(regalloc.MapVRegister(op.rs1), regalloc.MapVRegister(op.rs2));
				Cset(regalloc.MapRegister(op.rd), op.op == shop_fsetgt ? gt : eq);
				break;

			case shop_fsca:
				Mov(x1, reinterpret_cast<uintptr_t>(&sin_table));
				if (op.rs1.is_reg())
					Add(x1, x1, Operand(regalloc.MapRegister(op.rs1), UXTH, 3));
				else
					Add(x1, x1, Operand(op.rs1.imm_value() << 3));
#ifdef EXPLODE_SPANS
				Ldr(regalloc.MapVRegister(op.rd, 0), MemOperand(x1, 4, PostIndex));
				Ldr(regalloc.MapVRegister(op.rd, 1), MemOperand(x1));
#else
				Ldr(x2, MemOperand(x1));
				Str(x2, sh4_context_mem_operand(op.rd.reg_ptr()));
#endif
				break;

			case shop_fipr:
				Add(x9, x28, sh4_context_mem_operand(op.rs1.reg_ptr()).GetOffset());
				Ld1(v0.V4S(), MemOperand(x9));
				if (op.rs1._reg != op.rs2._reg)
				{
					Add(x9, x28, sh4_context_mem_operand(op.rs2.reg_ptr()).GetOffset());
					Ld1(v1.V4S(), MemOperand(x9));
					Fmul(v0.V4S(), v0.V4S(), v1.V4S());
				}
				else
					Fmul(v0.V4S(), v0.V4S(), v0.V4S());
				Faddp(v1.V4S(), v0.V4S(), v0.V4S());
				Faddp(regalloc.MapVRegister(op.rd), v1.V2S());
				break;

			case shop_ftrv:
				Add(x9, x28, sh4_context_mem_operand(op.rs1.reg_ptr()).GetOffset());
				Ld1(v0.V4S(), MemOperand(x9));
				Add(x9, x28, sh4_context_mem_operand(op.rs2.reg_ptr()).GetOffset());
				Ld1(v1.V4S(), MemOperand(x9, 16, PostIndex));
				Ld1(v2.V4S(), MemOperand(x9, 16, PostIndex));
				Ld1(v3.V4S(), MemOperand(x9, 16, PostIndex));
				Ld1(v4.V4S(), MemOperand(x9, 16, PostIndex));
				Fmul(v5.V4S(), v1.V4S(), s0, 0);
				Fmla(v5.V4S(), v2.V4S(), s0, 1);
				Fmla(v5.V4S(), v3.V4S(), s0, 2);
				Fmla(v5.V4S(), v4.V4S(), s0, 3);
				Add(x9, x28, sh4_context_mem_operand(op.rd.reg_ptr()).GetOffset());
				St1(v5.V4S(), MemOperand(x9));
				break;

			case shop_frswap:
				Add(x9, x28, sh4_context_mem_operand(op.rs1.reg_ptr()).GetOffset());
				Add(x10, x28, sh4_context_mem_operand(op.rd.reg_ptr()).GetOffset());
				Ld4(v0.V2D(), v1.V2D(), v2.V2D(), v3.V2D(), MemOperand(x9));
				Ld4(v4.V2D(), v5.V2D(), v6.V2D(), v7.V2D(), MemOperand(x10));
				St4(v4.V2D(), v5.V2D(), v6.V2D(), v7.V2D(), MemOperand(x9));
				St4(v0.V2D(), v1.V2D(), v2.V2D(), v3.V2D(), MemOperand(x10));
				break;

			case shop_cvt_f2i_t:
				{
					/* The host's conversion gives the largest or smallest
					 * integer for a value too large, as the SH4 does, but 0
					 * for a NaN, where the SH4 gives 0x80000000. A NaN is
					 * the one value that does not compare with itself. */
					const VRegister& from = regalloc.MapVRegister(op.rs1);
					const Register& to = regalloc.MapRegister(op.rd);
					Fcvtzs(to, from);
					Fcmp(from, from);
					Mov(w0, 0x80000000);
					Csel(to, to, w0, vc);
				}
				break;
			case shop_cvt_i2f_n:
			case shop_cvt_i2f_z:
				Scvtf(regalloc.MapVRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;

			default:
				shil_chf[op.op](&op);
				break;
			}
			regalloc.OpEnd(&op);
		}
		regalloc.Cleanup();

		if (accurate && block->BranchBlock <= block->vaddr
				&& (block->BlockType == BET_StaticJump || block->BlockType == BET_StaticCall
					|| block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1))
		{
			/* A block that can go back: every so often, see whether the
			 * game is only waiting, and give up the rest of the time slice
			 * if it is. See wait_site.h. This is ahead of the branch, so it
			 * is also passed on the way out of a loop; that changes
			 * nothing, since a pass that finds the registers as they were
			 * is one that goes round again. The cycle counter is w27: at 0,
			 * the next block's own subtraction ends the slice. */
			WaitSite *site = sh4_wait_site(block);
			if (site != nullptr)
			{
				Label over;

				Mov(x1, reinterpret_cast<uintptr_t>(&site->skip));
				Ldr(w0, MemOperand(x1));
				Subs(w0, w0, 1);
				Str(w0, MemOperand(x1));
				B(&over, pl);
				Mov(x0, reinterpret_cast<uintptr_t>(site));
				GenCallRuntime(sh4_wait_check);
				Cbz(w0, &over);
				Mov(w27, 0);
				Bind(&over);
			}
		}

		block->relink_offset = (u32)GetBuffer()->GetCursorOffset();
		block->relink_data = 0;

		RelinkBlock(block);

		Finalize();
	}

	void ngen_CC_Start(shil_opcode* op)
	{
		CC_pars.clear();
	}

	void ngen_CC_Param(shil_opcode& op, shil_param& prm, CanonicalParamType tp)
	{
		switch (tp)
		{

		case CPT_u32:
		case CPT_ptr:
		case CPT_f32:
		{
			CC_PS t = { tp, &prm };
			CC_pars.push_back(t);
		}
		break;

		case CPT_u64rvL:
		case CPT_u32rv:
			host_reg_to_shil_param(prm, w0);
			break;

		case CPT_u64rvH:
			Lsr(x10, x0, 32);
			host_reg_to_shil_param(prm, w10);
			break;

		case CPT_f32rv:
			host_reg_to_shil_param(prm, s0);
			break;
		}
	}

	void ngen_CC_Call(shil_opcode*op, void* function)
	{
		int regused = 0;
		int fregused = 0;

		// Args are pushed in reverse order by shil_canonical
		for (int i = CC_pars.size(); i-- > 0;)
		{
			verify(fregused < call_fregs.size() && regused < call_regs.size());
			shil_param& prm = *CC_pars[i].prm;
			switch (CC_pars[i].type)
			{
			// push the params

			case CPT_u32:
				shil_param_to_host_reg(prm, *call_regs[regused++]);

				break;

			case CPT_f32:
				if (prm.is_reg())
					Fmov(*call_fregs[fregused], regalloc.MapVRegister(prm));
				else if (prm.is_imm())
					Fmov(*call_fregs[fregused], reinterpret_cast<f32&>(prm._imm));
				else
					verify(prm.is_null());
				fregused++;
				break;

			case CPT_ptr:
				verify(prm.is_reg());
				// push the ptr itself
				Mov(*call_regs64[regused++], reinterpret_cast<uintptr_t>(prm.reg_ptr()));

				break;
			case CPT_u32rv:
			case CPT_u64rvL:
			case CPT_u64rvH:
			case CPT_f32rv:
				// return values are handled in ngen_CC_param()
				break;
			}
		}
		GenCallRuntime((void (*)())function);
	}

	MemOperand sh4_context_mem_operand(void *p)
	{
		u32 offset = (u8*)p - (u8*)&p_sh4rcb->cntx;
		verify((offset & 3) == 0 && offset <= 16380);	// FIXME 64-bit regs need multiple of 8 up to 32760
		return MemOperand(x28, offset);
	}

	/* A place in memory that is known when the block is compiled, as a
	 * load or store is to have it: so far from x26, which has the address
	 * of the SH4's memory as the host has it mapped, wherever it is within
	 * 4 GB above that - main memory and everything else of the machine's
	 * that is laid out there. The distance is put into @scratch with one
	 * or two instructions.
	 *
	 * It used to be the place's 64-bit address every time, read out of a
	 * word left among the code: eight bytes and a load ahead of each load
	 * or store. It is still that for a place that is not there (a host
	 * with nothing laid out, where x26 points at nothing in particular -
	 * the sum is the same place all the same, if it is in reach). */
	MemOperand ConstMem(const void *ptr, const Register& scratch)
	{
		const u8 *const base = (const u8 *)&p_sh4rcb->cntx + sizeof(Sh4Context);
		const u64 distance = (u64)((const u8 *)ptr - base);

		if (distance <= 0xFFFFFFFFu)
		{
			Mov(Register::GetWRegFromCode(scratch.GetCode()), (u32)distance);
			return MemOperand(MEM_BASE, Register::GetXRegFromCode(scratch.GetCode()));
		}
		Ldr(Register::GetXRegFromCode(scratch.GetCode()), reinterpret_cast<uintptr_t>(ptr));
		return MemOperand(Register::GetXRegFromCode(scratch.GetCode()));
	}

	void GenReadMemorySlow(u32 size)
	{
		switch (size)
		{
		case 1:
			if (!mmu_enabled())
				GenCallRuntime(ReadMem8);
			else
				GenCallRuntime(ReadMemNoEx<u8>);
			Sxtb(w0, w0);
			break;

		case 2:
			if (!mmu_enabled())
				GenCallRuntime(ReadMem16);
			else
				GenCallRuntime(ReadMemNoEx<u16>);
			Sxth(w0, w0);
			break;

		case 4:
			if (!mmu_enabled())
				GenCallRuntime(ReadMem32);
			else
				GenCallRuntime(ReadMemNoEx<u32>);
			break;

		case 8:
			if (!mmu_enabled())
				GenCallRuntime(ReadMem64);
			else
				GenCallRuntime(ReadMemNoEx<u64>);
			break;

		default:
			die("1..8 bytes");
			break;
		}
	}

	void GenWriteMemorySlow(u32 size)
	{
		switch (size)
		{
		case 1:
			if (!mmu_enabled())
				GenCallRuntime(WriteMem8);
			else
				GenCallRuntime(WriteMemNoEx<u8>);
			break;

		case 2:
			if (!mmu_enabled())
				GenCallRuntime(WriteMem16);
			else
				GenCallRuntime(WriteMemNoEx<u16>);
			break;

		case 4:
			if (!mmu_enabled())
				GenCallRuntime(WriteMem32);
			else
				GenCallRuntime(WriteMemNoEx<u32>);
			break;

		case 8:
			if (!mmu_enabled())
				GenCallRuntime(WriteMem64);
			else
				GenCallRuntime(WriteMemNoEx<u64>);
			break;

		default:
			die("1..8 bytes");
			break;
		}
	}

	/* The end of a block that goes to @target and nowhere else: to the
	 * block that is there, @linked, once it is known - until then to
	 * @stub, which finds it or has it compiled, and has this written again
	 * (rdv_LinkBlock()).
	 *
	 * With the MMU on that is for a target that is where it is whatever
	 * the TLB says (ngen.h); for any other, the main loop, which looks the
	 * address up. Either way the context has the address by then: a block
	 * begins by seeing that it is the one for it. */
	void GenStaticTail(RuntimeBlockInfo *block, u32 target, RuntimeBlockInfo *linked, void (*stub)())
	{
		if (mmu_enabled())
		{
			u32 place;

			Mov(w29, target);
			Str(w29, sh4_context_mem_operand(&next_pc));
			if (!block->mmu_go_on || !rdv_MmuSamePlace(block, target, &place))
			{
				GenBranch(*arm64_no_update);
				return;
			}
		}
		if (linked != NULL)
			GenBranch(linked->code);
		else
			GenCallRuntime(stub);
	}

	u32 RelinkBlock(RuntimeBlockInfo *block)
	{
		ptrdiff_t start_offset = GetBuffer()->GetCursorOffset();

		switch (block->BlockType)
		{

		case BET_StaticJump:
		case BET_StaticCall:
			// next_pc = block->BranchBlock;
			GenStaticTail(block, block->BranchBlock, block->pBranchBlock, ngen_LinkBlock_Generic_stub);
			break;

		case BET_Cond_0:
		case BET_Cond_1:
			{
				// next_pc = next_pc_value;
				// if (*jdyn == 0)
				//   next_pc = branch_pc_value;

				if (block->has_jcond)
					Ldr(w11, sh4_context_mem_operand(&Sh4cntx.jdyn));
				else
					Ldr(w11, sh4_context_mem_operand(&sr.T));

				Cmp(w11, block->BlockType & 1);

				Label branch_not_taken;

				B(ne, &branch_not_taken);
				GenStaticTail(block, block->BranchBlock, block->pBranchBlock, ngen_LinkBlock_cond_Branch_stub);

				Bind(&branch_not_taken);

				GenStaticTail(block, block->NextBlock, block->pNextBlock, ngen_LinkBlock_cond_Next_stub);
			}
			break;

		case BET_DynamicJump:
		case BET_DynamicCall:
		case BET_DynamicRet:
			// next_pc = *jdyn;

			Str(w29, sh4_context_mem_operand(&next_pc));
			if (!mmu_enabled())
			{
				// TODO Call no_update instead (and check CpuRunning less frequently?)
            Sub(x2, x28, offsetof(Sh4RCB, cntx));
#if RAM_SIZE_MAX == 33554432
				Ubfx(w1, w29, 1, 24);
#else
				Ubfx(w1, w29, 1, 23);
#endif
				Ldr(x15, MemOperand(x2, x1, LSL, 3));	// Get block entry point
				Br(x15);
			}
			else
			{
				GenBranch(*arm64_no_update);
			}

			break;

		case BET_DynamicIntr:
		case BET_StaticIntr:
			if (block->BlockType == BET_StaticIntr)
				// next_pc = next_pc_value;
				Mov(w29, block->NextBlock);
			// else next_pc = *jdyn (already in w29)

			Str(w29, sh4_context_mem_operand(&next_pc));

			GenCallRuntime(UpdateINTC);

			Ldr(w29, sh4_context_mem_operand(&next_pc));
			GenBranch(*arm64_no_update);

			break;

		default:
			die("Invalid block end type");
		}

		return GetBuffer()->GetCursorOffset() - start_offset;
	}

	void Finalize(bool rewrite = false)
	{
		Label code_end;
		Bind(&code_end);

		FinalizeCode();

		if (!rewrite)
		{
			block->code = GetBuffer()->GetStartAddress<DynarecCodeEntryPtr>();
			block->host_code_size = GetBuffer()->GetSizeInBytes();
			block->host_opcodes = GetLabelAddress<u32*>(&code_end) - GetBuffer()->GetStartAddress<u32*>();

			emit_Skip(block->host_code_size);
		}

		// Flush and invalidate caches
		vmem_platform_flush_cache(
			CC_RW2RX(GetBuffer()->GetStartAddress<void*>()), CC_RW2RX(GetBuffer()->GetEndAddress<void*>()),
			GetBuffer()->GetStartAddress<void*>(), GetBuffer()->GetEndAddress<void*>());
#if 0
		if (rewrite && block != NULL)
		{
			INFO_LOG(DYNAREC, "BLOCK %08x", block->vaddr);
			Instruction* instr_start = (Instruction*)block->code;
//			Instruction* instr_end = GetLabelAddress<Instruction*>(&code_end);
			Instruction* instr_end = (Instruction*)((u8 *)block->code + block->host_code_size);
			Decoder decoder;
			Disassembler disasm;
			decoder.AppendVisitor(&disasm);
			Instruction* instr;
			for (instr = instr_start; instr < instr_end; instr += kInstructionSize) {
				decoder.Decode(instr);
				INFO_LOG(DYNAREC, "VIXL  %p:  %s",
						   reinterpret_cast<void*>(instr),
						   disasm.GetOutput());
			}
		}
#endif
	}

	void GenMainloop()
	{
		Label no_update;
		Label intc_sched;
		Label end_mainloop;

		// int intc_sched()
		arm64_intc_sched = GetCursorAddress<int (*)()>();
      verify((void *)arm64_intc_sched == (void *)CodeCache);
		B(&intc_sched);

		// void no_update()
		Bind(&no_update);				// next_pc _MUST_ be on w29

		Ldr(w0, MemOperand(x28, offsetof(Sh4Context, CpuRunning)));
		Cbz(w0, &end_mainloop);
		if (!mmu_enabled())
		{
			Sub(x2, x28, offsetof(Sh4RCB, cntx));
			if (RAM_SIZE == 32 * 1024 * 1024)
				Ubfx(w1, w29, 1, 24);	// 24+1 bits: 32 MB
			else if (RAM_SIZE == 16 * 1024 * 1024)
				Ubfx(w1, w29, 1, 23);	// 23+1 bits: 16 MB
			else
				die("Unsupported RAM_SIZE");
			Ldr(x0, MemOperand(x2, x1, LSL, 3));
		}
		else
		{
			Mov(w0, w29);
			GenCallRuntime(bm_GetCodeByVAddr);
		}
		Br(x0);

		// void mainloop(void *context)
		mainloop = (void (*)(void *)) CC_RW2RX(GetCursorAddress<uintptr_t>());

		// Save registers
		Stp(x19, x20, MemOperand(sp, -160, PreIndex));
		Stp(x21, x22, MemOperand(sp, 16));
		Stp(x23, x24, MemOperand(sp, 32));
		Stp(x25, x26, MemOperand(sp, 48));
		Stp(x27, x28, MemOperand(sp, 64));
		Stp(s14, s15, MemOperand(sp, 80));
		Stp(vixl::aarch64::s8, s9, MemOperand(sp, 96));
		Stp(s10, s11, MemOperand(sp, 112));
		Stp(s12, s13, MemOperand(sp, 128));
		Stp(x29, x30, MemOperand(sp, 144));

		Sub(x0, x0, sizeof(Sh4Context));
		if (mmu_enabled())
		{
			// Push context, and what is left of the time slice (see ngen_Compile())
			Mov(x1, SH4_TIMESLICE);
			Stp(x0, x1, MemOperand(sp, -16, PreIndex));

			Ldr(x0, reinterpret_cast<uintptr_t>(jmp_env));
			Ldr(x1, reinterpret_cast<uintptr_t>(&setjmp));
			Blr(x1);

			// (here at the start, and again after every longjmp)
			Ldr(x28, MemOperand(sp));	// Set context
			Ldr(w27, MemOperand(sp, 8));
		}
		else
		{
			// Use x28 as sh4 context pointer
			Mov(x28, x0);
			// Use x27 as cycle_counter
			Mov(w27, SH4_TIMESLICE);
		}
		// the SH4's memory, where the host has it mapped, comes straight after its context
		Add(MEM_BASE, x28, sizeof(Sh4Context));
		Label do_interrupts;

		// w29 is next_pc
		Ldr(w29, MemOperand(x28, offsetof(Sh4Context, pc)));
		B(&no_update);

		Bind(&intc_sched);

		// Add timeslice to cycle counter, and to the copy of it if there is one
		Add(w27, w27, SH4_TIMESLICE);
		if (mmu_enabled())
			Str(w27, MemOperand(sp, 8));
		Mov(x29, lr);				// Trashing pc here but it will be reset at the end of the block or in DoInterrupts
		GenCallRuntime(UpdateSystem);
		Mov(lr, x29);
		Cbnz(w0, &do_interrupts);
		Ldr(w0, MemOperand(x28, offsetof(Sh4Context, CpuRunning)));
		Ret();

		Bind(&do_interrupts);
		Mov(x0, x29);
		GenCallRuntime(rdv_DoInterrupts);	// Updates next_pc based on host pc
		Mov(w29, w0);

		B(&no_update);

		Bind(&end_mainloop);
		if (mmu_enabled())
			// Pop context
			Add(sp, sp, 16);
		// Restore registers
		Ldp(x29, x30, MemOperand(sp, 144));
		Ldp(s12, s13, MemOperand(sp, 128));
		Ldp(s10, s11, MemOperand(sp, 112));
		Ldp(vixl::aarch64::s8, s9, MemOperand(sp, 96));
		Ldp(s14, s15, MemOperand(sp, 80));
		Ldp(x27, x28, MemOperand(sp, 64));
		Ldp(x25, x26, MemOperand(sp, 48));
		Ldp(x23, x24, MemOperand(sp, 32));
		Ldp(x21, x22, MemOperand(sp, 16));
		Ldp(x19, x20, MemOperand(sp, 160, PostIndex));
		Ret();

		GenMemStubs();

		FinalizeCode();
		emit_Skip(GetBuffer()->GetSizeInBytes());

		arm64_no_update = GetLabelAddress<void (*)()>(&no_update);

		// Flush and invalidate caches
		vmem_platform_flush_cache(
			CC_RW2RX(GetBuffer()->GetStartAddress<void*>()), CC_RW2RX(GetBuffer()->GetEndAddress<void*>()),
			GetBuffer()->GetStartAddress<void*>(), GetBuffer()->GetEndAddress<void*>());
	}


private:
	// Runtime branches/calls need to be adjusted if rx space is different to rw space.
	// Therefore can't mix GenBranch with GenBranchRuntime!

	template <typename R, typename... P>
	void GenCallRuntime(R (*function)(P...))
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(function) - reinterpret_cast<uintptr_t>(CC_RW2RX(GetBuffer()->GetStartAddress<void*>()));
		verify(offset >= -128 * 1024 * 1024 && offset <= 128 * 1024 * 1024);
		verify((offset & 3) == 0);
		Label function_label;
		BindToOffset(&function_label, offset);
		Bl(&function_label);
	}

	template <typename R, typename... P>
	void GenCall(R (*function)(P...))
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(function) - GetBuffer()->GetStartAddress<uintptr_t>();
		verify(offset >= -128 * 1024 * 1024 && offset <= 128 * 1024 * 1024);
		verify((offset & 3) == 0);
		Label function_label;
		BindToOffset(&function_label, offset);
		Bl(&function_label);
	}

   template <typename R, typename... P>
	void GenBranchRuntime(R (*target)(P...))
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(CC_RW2RX(GetBuffer()->GetStartAddress<void*>()));
		verify(offset >= -128 * 1024 * 1024 && offset <= 128 * 1024 * 1024);
		verify((offset & 3) == 0);
		Label target_label;
		BindToOffset(&target_label, offset);
		B(&target_label);
	}

	template <typename R, typename... P>
	void GenBranch(R (*code)(P...), Condition cond = al)
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(code) - GetBuffer()->GetStartAddress<uintptr_t>();
		verify(offset >= -128 * 1024 * 1024 && offset < 128 * 1024 * 1024);
		verify((offset & 3) == 0);
		Label code_label;
		BindToOffset(&code_label, offset);
		if (cond == al)
			B(&code_label);
		else
			B(&code_label, cond);
	}

	void GenReadMemory(const shil_opcode& op, size_t opid, bool optimise)
	{
		if (GenReadMemoryImmediate(op))
			return;
		// one instruction, with whatever registers the block has things in; or what follows
		if (optimise && GenReadMemoryFast(op))
			return;

		GenMemAddr(op, call_regs[0]);
		if (mmu_enabled())
			Mov(*call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));	// pc

		u32 size = op.flags & 0x7f;
		{
			Label lut_miss, lut_done;

			if (GenMmuLookup(mmu_read_lut, size, lut_miss))
			{
				switch (size)
				{
				case 1:
					Ldrsb(w0, MemOperand(x4, w0, UXTW));
					break;
				case 2:
					Ldrsh(w0, MemOperand(x4, w0, UXTW));
					break;
				case 8:
					Ldr(x0, MemOperand(x4, w0, UXTW));
					break;
				default:
					Ldr(w0, MemOperand(x4, w0, UXTW));
					break;
				}
				B(&lut_done);
				Bind(&lut_miss);
				GenReadMemorySlow(size);
				Bind(&lut_done);
			}
			else
				GenReadMemorySlow(size);
		}

		if (size < 8)
			host_reg_to_shil_param(op.rd, w0);
		else
		{
#ifdef EXPLODE_SPANS
			verify(op.rd.count() == 2 && regalloc.IsAllocf(op.rd, 0) && regalloc.IsAllocf(op.rd, 1));
			Fmov(regalloc.MapVRegister(op.rd, 0), w0);
			Lsr(x0, x0, 32);
			Fmov(regalloc.MapVRegister(op.rd, 1), w0);
#else
			Str(x0, sh4_context_mem_operand(op.rd.reg_ptr()));
#endif
		}
	}

	bool GenReadMemoryImmediate(const shil_opcode& op)
	{
		if (!op.rs1.is_imm())
			return false;

		u32 size = op.flags & 0x7f;
		u32 addr = op.rs1._imm;
#ifndef NO_MMU
      if (mmu_enabled() && mmu_is_translated<MMU_TT_DREAD>(addr, size))
		{
			if ((addr >> 12) != (block->vaddr >> 12))
				// When full mmu is on, only consider addresses in the same 4k page
				return false;
			u32 paddr;
			u32 rv;
			switch (size)
			{
			case 1:
				rv = mmu_data_translation<MMU_TT_DREAD, u8>(addr, paddr);
				break;
			case 2:
				rv = mmu_data_translation<MMU_TT_DREAD, u16>(addr, paddr);
				break;
			case 4:
			case 8:
				rv = mmu_data_translation<MMU_TT_DREAD, u32>(addr, paddr);
				break;
			default:
				die("Invalid immediate size");
				break;
			}
			if (rv != MMU_ERROR_NONE)
				return false;
			addr = paddr;
		}
#endif // NO_MMU
		bool isram = false;
		void* ptr = _vmem_read_const(addr, isram, size > 4 ? 4 : size);

		if (isram)
		{
			const MemOperand at = ConstMem(ptr, x1);

			if (regalloc.IsAllocAny(op.rd))
			{
				switch (size)
				{
				case 1:
					Ldrsb(regalloc.MapRegister(op.rd), at);
					break;

				case 2:
					Ldrsh(regalloc.MapRegister(op.rd), at);
					break;

				case 4:
					if (op.rd.is_r32f())
						Ldr(regalloc.MapVRegister(op.rd), at);
					else
						Ldr(regalloc.MapRegister(op.rd), at);
					break;

				default:
					die("Invalid size");
					break;
				}
			}
			else
			{
				switch (size)
				{
				case 1:
					Ldrsb(w1, at);
					break;

				case 2:
					Ldrsh(w1, at);
					break;

				case 4:
					Ldr(w1, at);
					break;

				case 8:
					Ldr(x1, at);
					break;

				default:
					die("Invalid size");
					break;
				}
				if (size == 8)
					Str(x1, sh4_context_mem_operand(op.rd.reg_ptr()));
				else
					Str(w1, sh4_context_mem_operand(op.rd.reg_ptr()));
			}
		}
		else
		{
			// Not RAM
			if (size == 8)
			{
				verify(!regalloc.IsAllocAny(op.rd));
				// Need to call the handler twice
				Mov(w0, addr);
				GenCallRuntime((void (*)())ptr);
				Str(w0, sh4_context_mem_operand(op.rd.reg_ptr()));

				Mov(w0, addr + 4);
				GenCallRuntime((void (*)())ptr);
				Str(w0, sh4_context_mem_operand((u8*)op.rd.reg_ptr() + 4));
			}
			else
			{
				Mov(w0, addr);

				switch(size)
				{
				case 1:
					GenCallRuntime((void (*)())ptr);
					Sxtb(w0, w0);
					break;

				case 2:
					GenCallRuntime((void (*)())ptr);
					Sxth(w0, w0);
					break;

				case 4:
					GenCallRuntime((void (*)())ptr);
					break;

				default:
					die("Invalid size");
					break;
				}

				if (regalloc.IsAllocg(op.rd))
					Mov(regalloc.MapRegister(op.rd), w0);
				else
				{
					verify(regalloc.IsAllocf(op.rd));
					Fmov(regalloc.MapVRegister(op.rd), w0);
				}
			}
		}

		return true;
	}

	/* A fast access of @kind: one instruction on the SH4's memory as the
	 * host has it mapped, which x26 has the address of. The SH4's address
	 * is in the general register @addr, as 32 bits, and the data in, or
	 * wanted in, @data - a general register, or a floating-point one for
	 * the two kinds that are. (Where the host has only 512 MB of the SH4's
	 * addresses mapped, the address is cut to that first: two instructions.)
	 *
	 * It used to take the address from w0 and the data from w0 or w1, with
	 * moves around it, and add the context's size to the address first.
	 *
	 * One that faults - an address that is not memory - is written over
	 * with a call: ngen_Rewrite(), which knows it by what it is. */
	void GenFastAccess(int kind, const Register& addr, const CPURegister& data)
	{
		MemOperand at(MEM_BASE, Register::GetWRegFromCode(addr.GetCode()), UXTW);

		if (!_nvmem_4gb_space())
		{
			Ubfx(x9, Register::GetXRegFromCode(addr.GetCode()), 0, 29);
			at = MemOperand(MEM_BASE, x9);
		}
		switch (kind)
		{
		case MEM_LOAD8:    Ldrsb(Register::GetWRegFromCode(data.GetCode()), at); break;
		case MEM_LOAD16:   Ldrsh(Register::GetWRegFromCode(data.GetCode()), at); break;
		case MEM_LOAD32:   Ldr(Register::GetWRegFromCode(data.GetCode()), at); break;
		case MEM_LOAD32F:  Ldr(VRegister::GetSRegFromCode(data.GetCode()), at); break;
		case MEM_LOAD64:   Ldr(Register::GetXRegFromCode(data.GetCode()), at); break;
		case MEM_STORE8:   Strb(Register::GetWRegFromCode(data.GetCode()), at); break;
		case MEM_STORE16:  Strh(Register::GetWRegFromCode(data.GetCode()), at); break;
		case MEM_STORE32:  Str(Register::GetWRegFromCode(data.GetCode()), at); break;
		case MEM_STORE32F: Str(VRegister::GetSRegFromCode(data.GetCode()), at); break;
		case MEM_STORE64:  Str(Register::GetXRegFromCode(data.GetCode()), at); break;
		default:           die("no such access");
		}
	}

	/* A load as a fast access, straight into the register the block wants
	 * it in. False if that is not to be had - no mapping, or the MMU on -
	 * and nothing written. */
	bool GenReadMemoryFast(const shil_opcode& op)
	{
		const u32 size = op.flags & 0x7f;

		if (!_nvmem_enabled() || mmu_enabled())
			return false;
#ifdef EXPLODE_SPANS
		if (size == 8)
			return false;
#endif
		// the register the block has the address in, or w0 with it worked out
		const Register& addr = GenMemAddr(op);

		if (size == 8)
		{
			GenFastAccess(MEM_LOAD64, addr, x0);
			Str(x0, sh4_context_mem_operand(op.rd.reg_ptr()));
			return true;
		}
		const int kind = size == 1 ? MEM_LOAD8 : size == 2 ? MEM_LOAD16 : MEM_LOAD32;
		if (regalloc.IsAllocg(op.rd))
			GenFastAccess(kind, addr, regalloc.MapRegister(op.rd));
		else if (size == 4 && regalloc.IsAllocf(op.rd))
			GenFastAccess(MEM_LOAD32F, addr, regalloc.MapVRegister(op.rd));
		else
		{
			GenFastAccess(kind, addr, w0);
			host_reg_to_shil_param(op.rd, w0);
		}
		return true;
	}

	void GenWriteMemory(const shil_opcode& op, size_t opid, bool optimise)
	{
		if (GenWriteMemoryImmediate(op))
			return;
		if (optimise && GenWriteMemoryFast(op))
			return;

		GenMemAddr(op, call_regs[0]);
		if (mmu_enabled())
			Mov(*call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));	// pc

		u32 size = op.flags & 0x7f;
		if (size != 8)
			shil_param_to_host_reg(op.rs2, *call_regs[1]);
		else
		{
#ifdef EXPLODE_SPANS
			verify(op.rs2.count() == 2 && regalloc.IsAllocf(op.rs2, 0) && regalloc.IsAllocf(op.rs2, 1));
			Fmov(*call_regs[1], regalloc.MapVRegister(op.rs2, 1));
			Lsl(*call_regs64[1], *call_regs64[1], 32);
			Fmov(w2, regalloc.MapVRegister(op.rs2, 0));
			Orr(*call_regs64[1], *call_regs64[1], x2);
#else
			shil_param_to_host_reg(op.rs2, *call_regs64[1]);
#endif
		}
		Label lut_miss, lut_done;

		if (GenMmuLookup(mmu_write_lut, size, lut_miss))
		{
			switch (size)
			{
			case 1:
				Strb(w1, MemOperand(x4, w0, UXTW));
				break;
			case 2:
				Strh(w1, MemOperand(x4, w0, UXTW));
				break;
			case 8:
				Str(x1, MemOperand(x4, w0, UXTW));
				break;
			default:
				Str(w1, MemOperand(x4, w0, UXTW));
				break;
			}
			B(&lut_done);
			Bind(&lut_miss);
			GenWriteMemorySlow(size);
			Bind(&lut_done);
		}
		else
			GenWriteMemorySlow(size);
	}

	/* With the MMU on an access is a call, which asks the MMU. First,
	 * though, the table of translations kept by (mmu.h), as the x86-64
	 * and the 32-bit ARM recompiler do: on a hit, straight to the page on
	 * the host - x4 is then what to add to the address, which is in x0, to
	 * be there. The call is for a miss, and fills the table in. Not for an
	 * address that is not a multiple of the access's size, which is an
	 * address error and the call's to raise; 64 bits are two accesses of 32
	 * to the SH4, so a multiple of 4 will do for them, but then the second
	 * half must not be in the next page. x3 and x4 belong to nobody here. */
	bool GenMmuLookup(const uintptr_t *table, u32 size, Label& miss)
	{
#ifdef MMU_HOST_PAGE_LUT
		if (!mmu_enabled())
			return false;
		if (size == 2 || size == 4 || size == 8)
		{
			Tst(w0, size == 2 ? 1 : 3);
			B(&miss, ne);
		}
		if (size == 8)
		{
			Add(w3, w0, 4);
			Tst(w3, 0xFFF);
			B(&miss, eq);
		}
		Lsr(w3, w0, 12);
		Ldr(x4, reinterpret_cast<uintptr_t>(table));
		Ldr(x4, MemOperand(x4, x3, LSL, 3));
		Cbz(x4, &miss);
		return true;
#else
		return false;
#endif
	}

	bool GenWriteMemoryImmediate(const shil_opcode& op)
	{
		if (!op.rs1.is_imm())
			return false;

		u32 size = op.flags & 0x7f;
		u32 addr = op.rs1._imm;
#ifndef NO_MMU
      if (mmu_enabled() && mmu_is_translated<MMU_TT_DWRITE>(addr, size))
		{
			if ((addr >> 12) != (block->vaddr >> 12) && ((addr >> 12) != ((block->vaddr + block->guest_opcodes * 2 - 1) >> 12)))
				// When full mmu is on, only consider addresses in the same 4k page
				return false;
			u32 paddr;
			u32 rv;
			switch (size)
			{
			case 1:
				rv = mmu_data_translation<MMU_TT_DWRITE, u8>(addr, paddr);
				break;
			case 2:
				rv = mmu_data_translation<MMU_TT_DWRITE, u16>(addr, paddr);
				break;
			case 4:
			case 8:
				rv = mmu_data_translation<MMU_TT_DWRITE, u32>(addr, paddr);
				break;
			default:
				die("Invalid immediate size");
				break;
			}
			if (rv != MMU_ERROR_NONE)
				return false;
			addr = paddr;
		}
#endif // NO_MMU
		bool isram = false;
		void* ptr = _vmem_write_const(addr, isram, size > 4 ? 4 : size);

		Register reg2;
		if (size != 8)
		{
			if (op.rs2.is_imm())
			{
				Mov(w1, op.rs2._imm);
				reg2 = w1;
			}
			else if (regalloc.IsAllocg(op.rs2))
			{
				reg2 = regalloc.MapRegister(op.rs2);
			}
			else if (regalloc.IsAllocf(op.rs2))
			{
				Fmov(w1, regalloc.MapVRegister(op.rs2));
				reg2 = w1;
			}
			else
				die("Invalid rs2 param");
		}
		if (isram)
		{
			const MemOperand at = ConstMem(ptr, x0);

			switch (size)
			{
			case 1:
				Strb(reg2, at);
				break;

			case 2:
				Strh(reg2, at);
				break;

			case 4:
				Str(reg2, at);
				break;

			case 8:
#ifdef EXPLODE_SPANS
				verify(op.rs2.count() == 2 && regalloc.IsAllocf(op.rs2, 0) && regalloc.IsAllocf(op.rs2, 1));
				Str(regalloc.MapVRegister(op.rs2, 0),  MemOperand(x1));
				Str(regalloc.MapVRegister(op.rs2, 1),  MemOperand(x1, 4));
#else
				shil_param_to_host_reg(op.rs2, x1);
				Str(x1, at);
#endif
				break;

			default:
				die("Invalid size");
				break;
			}
		}
		else
		{
			// Not RAM
			Mov(w0, addr);
			if (size == 8)
			{
				// Need to call the handler twice
				shil_param_to_host_reg(op.rs2, x1);
				GenCallRuntime((void (*)())ptr);

				Mov(w0, addr + 4);
				shil_param_to_host_reg(op.rs2, x1);
				Lsr(x1, x1, 32);
				GenCallRuntime((void (*)())ptr);
			}
			else
			{
				Mov(w1, reg2);

				switch(size)
				{
				case 1:
					GenCallRuntime((void (*)())ptr);
					break;

				case 2:
					GenCallRuntime((void (*)())ptr);
					break;

				case 4:
					GenCallRuntime((void (*)())ptr);
					break;

				default:
					die("Invalid size");
					break;
				}
			}
		}

		return true;
	}

	// A store, likewise
	bool GenWriteMemoryFast(const shil_opcode& op)
	{
		const u32 size = op.flags & 0x7f;

		if (!_nvmem_enabled() || mmu_enabled())
			return false;
#ifdef EXPLODE_SPANS
		if (size == 8)
			return false;
#endif
		const Register& addr = GenMemAddr(op);

		if (size == 8)
		{
			shil_param_to_host_reg(op.rs2, x1);
			GenFastAccess(MEM_STORE64, addr, x1);
			return true;
		}
		const int kind = size == 1 ? MEM_STORE8 : size == 2 ? MEM_STORE16 : MEM_STORE32;
		if (op.rs2.is_reg() && !op.rs2.is_r32f() && regalloc.IsAllocg(op.rs2))
			GenFastAccess(kind, addr, regalloc.MapRegister(op.rs2));
		else if (size == 4 && op.rs2.is_r32f() && regalloc.IsAllocf(op.rs2))
			GenFastAccess(MEM_STORE32F, addr, regalloc.MapVRegister(op.rs2));
		else
		{
			shil_param_to_host_reg(op.rs2, w1);
			GenFastAccess(kind, addr, w1);
		}
		return true;
	}

	/* The routines a fast access that faulted is written over with a call
	 * to: see mem_stubs. Each puts the address and what is stored where a
	 * function takes them, calls the one that does the access by asking,
	 * and puts what was loaded where the access had it - or, where there
	 * is nothing to do afterwards, goes to the function and lets it return
	 * for it. */
	void GenMemStubs()
	{
		static void (*const routines[MEM_KINDS])() = {
			(void (*)())&mem_load8, (void (*)())&mem_load16, (void (*)())&mem_load32, (void (*)())&mem_load32,
			(void (*)())&mem_load64,
			(void (*)())&mem_store8, (void (*)())&mem_store16, (void (*)())&mem_store32, (void (*)())&mem_store32,
			(void (*)())&mem_store64,
		};
		const int regs = 1 + mem_block_regs(), fregs = mem_block_fregs();

		verify(regs <= MEM_REGS && fregs <= MEM_REGS);
		memset(mem_stubs, 0, sizeof(mem_stubs));
		for (int kind = 0; kind < MEM_KINDS; kind++)
			for (int addr = 0; addr < regs; addr++)
			{
				const bool load = kind < MEM_STORE8;
				const int datas = MEM_KIND_F(kind) ? fregs : kind == MEM_LOAD64 || kind == MEM_STORE64 ? 1 : regs;

				for (int data = 0; data < datas; data++)
				{
					const bool after = load && (MEM_KIND_F(kind) || data != 0);

					mem_stubs[kind][addr][data] = GetCursorAddress<u8 *>();
					if (after)
						Str(x30, MemOperand(sp, -16, PreIndex));
					// (neither the address nor what is stored is ever in the other's place)
					if (addr != 0)
						Mov(w0, Register::GetWRegFromCode(alloc_regs[addr - 1]));
					if (!load && MEM_KIND_F(kind))
						Fmov(w1, VRegister::GetSRegFromCode(alloc_fregs[data]));
					else if (!load && data != 0)
						Mov(w1, Register::GetWRegFromCode(alloc_regs[data - 1]));
					if (!after)
					{
						GenBranchRuntime(routines[kind]);
						continue;
					}
					GenCallRuntime(routines[kind]);
					if (MEM_KIND_F(kind))
						Fmov(VRegister::GetSRegFromCode(alloc_fregs[data]), w0);
					else
						Mov(Register::GetWRegFromCode(alloc_regs[data - 1]), w0);
					Ldr(x30, MemOperand(sp, 16, PostIndex));
					Ret();
				}
			}
	}

	void CheckBlock(bool force_checks, RuntimeBlockInfo* block)
	{
		if (!mmu_enabled() && !force_checks)
			return;

		Label blockcheck_fail;

		if (mmu_enabled())
		{
			Ldr(w10, sh4_context_mem_operand(&next_pc));
			Ldr(w11, block->vaddr);
			Cmp(w10, w11);
			B(ne, &blockcheck_fail);
		}
		if (force_checks)
		{
			s32 sz = block->sh4_code_size;
			u8* ptr = GetMemPtr(block->addr, sz);
			if (ptr != NULL)
			{
				Ldr(x9, reinterpret_cast<uintptr_t>(ptr));

				while (sz > 0)
				{
					if (sz >= 8)
					{
						Ldr(x10, MemOperand(x9, 8, PostIndex));
						Ldr(x11, *(u64*)ptr);
						Cmp(x10, x11);
						sz -= 8;
						ptr += 8;
					}
					else if (sz >= 4)
					{
						Ldr(w10, MemOperand(x9, 4, PostIndex));
						Ldr(w11, *(u32*)ptr);
						Cmp(w10, w11);
						sz -= 4;
						ptr += 4;
					}
					else
					{
						Ldrh(w10, MemOperand(x9, 2, PostIndex));
						Mov(w11, *(u16*)ptr);
						Cmp(w10, w11);
						sz -= 2;
						ptr += 2;
					}
					B(ne, &blockcheck_fail);
				}
			}
		}
		Label blockcheck_success;
		B(&blockcheck_success);
		Bind(&blockcheck_fail);
		Ldr(w0, block->addr);
		TailCallRuntime(ngen_blockcheckfail);

		Bind(&blockcheck_success);

		if (mmu_enabled() && block->has_fpu_op)
		{
			Label fpu_enabled;
			Ldr(w10, sh4_context_mem_operand(&sr));
			Tbz(w10, 15, &fpu_enabled);			// test SR.FD bit

			Mov(*call_regs[0], block->vaddr);	// pc
			Mov(*call_regs[1], 0x800);			// event
			Mov(*call_regs[2], 0x100);			// vector
			CallRuntime(Do_Exception);
			Ldr(w29, sh4_context_mem_operand(&next_pc));
			GenBranch(*arm64_no_update);

			Bind(&fpu_enabled);
		}
	}

	void shil_param_to_host_reg(const shil_param& param, const Register& reg)
	{
		if (param.is_imm())
		{
			Mov(reg, param._imm);
		}
		else if (param.is_reg())
		{
			if (param.is_r64f())
				Ldr(reg, sh4_context_mem_operand(param.reg_ptr()));
			else if (param.is_r32f())
			{
				if (regalloc.IsAllocf(param))
					Fmov(reg, regalloc.MapVRegister(param));
				else
					Ldr(reg, sh4_context_mem_operand(param.reg_ptr()));
			}
			else
			{
				if (regalloc.IsAllocg(param))
					Mov(reg, regalloc.MapRegister(param));
				else
					Ldr(reg, sh4_context_mem_operand(param.reg_ptr()));
			}
		}
		else
		{
			verify(param.is_null());
		}
	}

	void host_reg_to_shil_param(const shil_param& param, const CPURegister& reg)
	{
		if (reg.Is64Bits())
		{
			Str((const Register&)reg, sh4_context_mem_operand(param.reg_ptr()));
		}
		else if (regalloc.IsAllocg(param))
		{
			if (reg.IsRegister())
				Mov(regalloc.MapRegister(param), (const Register&)reg);
			else
				Fmov(regalloc.MapRegister(param), (const VRegister&)reg);
		}
		else if (regalloc.IsAllocf(param))
		{
			if (reg.IsVRegister())
				Fmov(regalloc.MapVRegister(param), (const VRegister&)reg);
			else
				Fmov(regalloc.MapVRegister(param), (const Register&)reg);
		}
		else
		{
			Str(reg, sh4_context_mem_operand(param.reg_ptr()));
		}
	}

	struct CC_PS
	{
		CanonicalParamType type;
		shil_param* prm;
	};
	std::vector<CC_PS> CC_pars;
	std::vector<const WRegister*> call_regs;
	std::vector<const XRegister*> call_regs64;
	std::vector<const VRegister*> call_fregs;
	Arm64RegAlloc regalloc;
	RuntimeBlockInfo* block = NULL;
};

static Arm64Assembler* compiler;

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	verify(emit_FreeSpace() >= 16 * 1024);

	compiler = new Arm64Assembler();

	compiler->ngen_Compile(block, force_checks, reset, staging, optimise);

	delete compiler;
	compiler = NULL;
}

void ngen_CC_Start(shil_opcode* op)
{
	compiler->ngen_CC_Start(op);
}

void ngen_CC_Param(shil_opcode* op, shil_param* par, CanonicalParamType tp)
{
	compiler->ngen_CC_Param(*op, *par, tp);
}

void ngen_CC_Call(shil_opcode*op, void* function)
{
	compiler->ngen_CC_Call(op, function);
}

void ngen_CC_Finish(shil_opcode* op)
{

}

/* Whether what is at @site is a fast access as GenFastAccess() writes
 * them, and if so which kind and with which registers (as mem_stubs
 * numbers them). A fast access is told by what it is: a load or store
 * with a register for its offset and x26 for its base, which nothing else
 * a block has. Where the host has 512 MB mapped the offset is x9, which
 * the instruction before made from the register the address is in. */
static bool mem_access_at(const u32 *site, int *kind, int *addr, int *data)
{
	const u32 insn = *site;

	// size 111 V 00 opc 1 Rm option S 10 Rn Rt, with x26 the base and nothing shifted
	if ((insn & 0x3B200C00) != 0x38200800 || ((insn >> 5) & 31) != MEM_BASE_CODE || (insn & 0x1000) != 0)
		return false;
	const u32 size = insn >> 30, opc = (insn >> 22) & 3, option = (insn >> 13) & 7;
	const bool fp = (insn & 0x04000000) != 0;
	int reg = (insn >> 16) & 31;

	if (_nvmem_4gb_space())
	{
		// the address as it is, 32 bits of it
		if (option != 2)
			return false;
	}
	else
	{
		// ubfx x9, <the address>, #0, #29 before it
		if (option != 3 || reg != 9 || (site[-1] & 0xFFFFFC1F) != 0xD3407009)
			return false;
		reg = (site[-1] >> 5) & 31;
	}
	if (fp)
	{
		if (size != 2 || opc > 1)
			return false;
		*kind = opc == 0 ? MEM_STORE32F : MEM_LOAD32F;
	}
	else if (opc == 0)
		*kind = size == 0 ? MEM_STORE8 : size == 1 ? MEM_STORE16 : size == 2 ? MEM_STORE32 : MEM_STORE64;
	else if (opc == 1 && size >= 2)
		*kind = size == 2 ? MEM_LOAD32 : MEM_LOAD64;
	else if (opc == 3 && size <= 1)
		*kind = size == 0 ? MEM_LOAD8 : MEM_LOAD16;
	else
		return false;

	*addr = mem_reg_number(reg, 0);
	*data = fp ? mem_freg_number(insn & 31) : mem_reg_number(insn & 31, *kind < MEM_STORE8 ? 0 : 1);
	if ((*kind == MEM_LOAD64 || *kind == MEM_STORE64) && *data != 0)
		return false;
	return *addr >= 0 && *data >= 0;
}

/* A fast memory access faulted: it is written over with a call to the
 * routine that does such an access by asking, with the registers this one
 * has (mem_stubs), and run again. */
bool ngen_Rewrite(unat& host_pc, unat, unat)
{
	u32 *const site = (u32 *)CC_RX2RW(host_pc);
	int kind, addr, data;

	if (!_nvmem_enabled() || mmu_enabled())
		return false;
	if (!mem_access_at(site, &kind, &addr, &data) || mem_stubs[kind][addr][data] == NULL)
		return false;

	// bl, to where the routine is from where this is run
	const ptrdiff_t offset = mem_stubs[kind][addr][data] - (u8 *)site;
	verify(offset >= -128 * 1024 * 1024 && offset < 128 * 1024 * 1024 && (offset & 3) == 0);
	*site = 0x94000000 | (u32)((offset >> 2) & 0x03FFFFFF);
	vmem_platform_flush_cache((void *)host_pc, (void *)(host_pc + 4), site, site + 1);
	return true;
}

static void generate_mainloop()
{
	if (mainloop != nullptr)
		return;
	compiler = new Arm64Assembler();

	compiler->GenMainloop();

	delete compiler;
	compiler = nullptr;
}

RuntimeBlockInfo* ngen_AllocateBlock()
{
	generate_mainloop();
	return new DynaRBI();
}

void ngen_HandleException()
{
	longjmp(jmp_env, 1);
}

u32 DynaRBI::Relink()
{
	//printf("DynaRBI::Relink %08x\n", this->addr);
	Arm64Assembler *compiler = new Arm64Assembler((u8 *)this->code + this->relink_offset);

	u32 code_size = compiler->RelinkBlock(this);
	compiler->Finalize(true);
	delete compiler;

	return code_size;
}

void Arm64RegAlloc::Preload(u32 reg, eReg nreg)
{
	assembler->Ldr(Register(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}
void Arm64RegAlloc::Writeback(u32 reg, eReg nreg)
{
	assembler->Str(Register(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}
void Arm64RegAlloc::Preload_FPU(u32 reg, eFReg nreg)
{
	assembler->Ldr(VRegister(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}
void Arm64RegAlloc::Writeback_FPU(u32 reg, eFReg nreg)
{
	assembler->Str(VRegister(nreg, 32), assembler->sh4_context_mem_operand(GetRegPtr(reg)));
}


extern "C" naked void do_sqw_nommu_area_3(u32 dst, u8* sqb)
{
	__asm__
	(
		"and x12, x0, #0x20			\n\t"	// SQ# selection, isolate
		"add x12, x12, x1			\n\t"	// SQ# selection, add to SQ ptr
		"ld2 { v0.2D, v1.2D }, [x12]\n\t"
		"movz x11, #0x0C00, lsl #16 \n\t"
		"add x11, x1, x11			\n\t"	// get ram ptr from x1, part 1
		"ubfx x0, x0, #5, #20		\n\t"	// get ram offset
		"add x11, x11, #512			\n\t"	// get ram ptr from x1, part 2
		"add x11, x11, x0, lsl #5	\n\t"	// ram + offset
		"st2 { v0.2D, v1.2D }, [x11] \n\t"
		"ret						\n"

		: : : "memory"
	);
}
#endif	// FEAT_SHREC == DYNAREC_JIT
