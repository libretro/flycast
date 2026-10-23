/*
	Copyright 2020 flyinghead

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

#include "build.h"

#if	HOST_CPU == CPU_ARM64 && FEAT_AREC != DYNAREC_NONE

#include "arm7_rec.h"
#include "hw/aica/aica_if.h"
#include "hw/mem/_vmem.h"
#include "deps/vixl/aarch64/macro-assembler-aarch64.h"
using namespace vixl::aarch64;

extern void vmem_platform_flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end);

/* The ARM7 recompiler's backend for ARM64, behind the same decoder as the
 * x86-64 one (arm7_rec.cpp): the blocks end at the same places and take the
 * same cycles, and each instruction does what the x86-64 code does for it,
 * flags and all, so that the two kinds of machine run a sound program the
 * same to the cycle.
 *
 * While a block runs: x28 points to the ARM's registers, x26 to the table
 * of entry points, and the ARM's registers the allocator gives a host one
 * are in w19-w25 and w27, all kept across calls. The ARM's flags are kept
 * in its register file, as on x86-64, and taken into the host's NZCV only
 * for the instruction that needs them: the two processors' flags are the
 * same four bits in the same places, with the same carry for subtraction.
 * The main loop's frame holds the sound memory's address and mask for the
 * loads and stores done in line. */

namespace aicaarm
{

static void (*arm_dispatch)();

class Arm7Compiler;

#define MAX_REGS 8

// where the main loop's frame keeps the sound memory's address and mask
static const int RAM_SLOT = 96;
static const int MASK_SLOT = 104;
static const int FRAME_SIZE = 112;

class AArch64ArmRegAlloc : public ArmRegAlloc<MAX_REGS, AArch64ArmRegAlloc>
{
	using super = ArmRegAlloc<MAX_REGS, AArch64ArmRegAlloc>;
	Arm7Compiler& assembler;

	void LoadReg(int host_reg, Arm7Reg armreg);
	void StoreReg(int host_reg, Arm7Reg armreg);

	static const Register& getReg(int i)
	{
		static const Register regs[MAX_REGS] = {
				w19, w20, w21, w22, w23, w24, w25, w27
		};
		verify(i >= 0 && i < MAX_REGS);
		return regs[i];
	}

public:
	AArch64ArmRegAlloc(Arm7Compiler& assembler, const std::vector<ArmOp>& block_ops)
		: super(block_ops), assembler(assembler) {}

	const Register& map(Arm7Reg r)
	{
		int i = super::map(r);
		return getReg(i);
	}

	friend super;
};

static MemOperand arm_reg_operand(Arm7Reg reg)
{
	return MemOperand(x28, (u8*)&arm_Reg[reg].I - (u8*)&arm_Reg[0].I);
}

class Arm7Compiler : public MacroAssembler
{
	bool logical_op_set_flags = false;
	bool set_carry_bit = false;
	bool set_flags = false;
	AArch64ArmRegAlloc *regalloc = nullptr;

	/* Whether @target, in the core's own code, is out of reach of a branch
	 * from here (128 MB). It never is where the code is in the core's
	 * .text; on macOS on Apple's processors it can be (see
	 * vmem_platform_alloc_jit()), and the call is then made through a
	 * register. */
	bool outOfReach(const void *target)
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(target)
				- reinterpret_cast<uintptr_t>(recompiler::writeToExec(GetCursorAddress<void *>()));
		return offset < -128 * 1024 * 1024 || offset >= 128 * 1024 * 1024;
	}

	void call(const void *loc)
	{
		if (outOfReach(loc))
		{
			UseScratchRegisterScope temps(this);
			Register target = temps.AcquireX();
			Mov(target, reinterpret_cast<uintptr_t>(loc));
			Blr(target);
			return;
		}
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(loc)
				- reinterpret_cast<uintptr_t>(recompiler::writeToExec(GetBuffer()->GetStartAddress<void *>()));
		Label function_label;
		BindToOffset(&function_label, offset);
		Bl(&function_label);
	}

	// A branch to the main loop's dispatcher, which is in the same code buffer
	void jumpToDispatch()
	{
		ptrdiff_t offset = reinterpret_cast<uintptr_t>(arm_dispatch)
				- reinterpret_cast<uintptr_t>(recompiler::writeToExec(GetBuffer()->GetStartAddress<void *>()));
		Label dispatch_label;
		BindToOffset(&dispatch_label, offset);
		B(&dispatch_label);
	}

	/* The value of an operand: a plain constant as it is, a register as it
	 * is, anything shifted worked out into @scratch - with the carry the
	 * shift leaves into w14 when a logical instruction that sets the flags
	 * wants it, as the x86-64 backend does. */
	Operand getOperand(const ArmOp::Operand& arg, const Register& scratch)
	{
		Register rm;
		if (arg.isImmediate())
		{
			if (!arg.isShifted())
				return Operand(arg.getImmediate());
			// r15 read as a value, with a shift the decoder left to be done
			Mov(scratch, arg.getImmediate());
			rm = scratch;
		}
		else
			rm = regalloc->map(arg.getReg().armreg);
		if (!arg.isShifted())
			return Operand(rm);

		if (!arg.shift_imm)
		{
			// shift by register: the count is the register's low byte
			const Register& shift_reg = regalloc->map(arg.shift_reg.armreg);
			switch (arg.shift_type)
			{
			case ArmOp::LSL:
			case ArmOp::LSR:
				Uxtb(w3, shift_reg);
				if (arg.shift_type == ArmOp::LSL)
					Lsl(scratch, rm, w3);
				else
					Lsr(scratch, rm, w3);
				Cmp(w3, 32);
				Csel(scratch, wzr, scratch, hs);	// LSL and LSR by 32 or more gives 0
				break;
			case ArmOp::ASR:
				Uxtb(w3, shift_reg);
				Asr(w13, rm, 31);
				Asr(scratch, rm, w3);
				Cmp(w3, 32);
				Csel(scratch, w13, scratch, hs);	// ASR by 32 or more gives 0 or -1 depending on operand sign
				break;
			case ArmOp::ROR:
				Ror(scratch, rm, shift_reg);
				break;
			default:
				die("Invalid shift");
				break;
			}
			return Operand(scratch);
		}

		// shift by immediate
		if (arg.shift_type != ArmOp::ROR && arg.shift_value != 0 && !logical_op_set_flags)
			return Operand(rm, (Shift)arg.shift_type, arg.shift_value);

		if (arg.shift_value == 0)
		{
			// Shift by 32
			if (logical_op_set_flags)
				set_carry_bit = true;
			if (arg.shift_type == ArmOp::LSR)
			{
				if (set_carry_bit)
					Ubfx(w14, rm, 31, 1);			// w14 = rm[31]
				Mov(scratch, 0);					// scratch = 0
			}
			else if (arg.shift_type == ArmOp::ASR)
			{
				if (set_carry_bit)
					Ubfx(w14, rm, 31, 1);			// w14 = rm[31]
				Asr(scratch, rm, 31);				// scratch = rm < 0 ? -1 : 0
			}
			else if (arg.shift_type == ArmOp::ROR)
			{
				// RRX, with the ARM's carry as it is in its register file
				Ldr(w15, arm_reg_operand(RN_PSR_FLAGS));
				Ubfx(w15, w15, 29, 1);				// w15 = C
				if (set_carry_bit)
					Ubfx(w14, rm, 0, 1);			// w14 = rm[0] (new C)
				Lsr(w13, rm, 1);
				Orr(scratch, w13, Operand(w15, LSL, 31));
			}
			else
				die("Invalid shift");
		}
		else
		{
			// Carry must be preserved or Ror shift
			if (logical_op_set_flags)
				set_carry_bit = true;
			if (set_carry_bit)
			{
				if (arg.shift_type == ArmOp::LSL)
					Ubfx(w14, rm, 32 - arg.shift_value, 1);	// w14 = rm[lsb]
				else
					Ubfx(w14, rm, arg.shift_value - 1, 1);	// w14 = rm[msb]
			}
			switch (arg.shift_type)
			{
			case ArmOp::LSL:
				Lsl(scratch, rm, arg.shift_value);
				break;
			case ArmOp::LSR:
				Lsr(scratch, rm, arg.shift_value);
				break;
			case ArmOp::ASR:
				Asr(scratch, rm, arg.shift_value);
				break;
			case ArmOp::ROR:
				Ror(scratch, rm, arg.shift_value);
				break;
			default:
				die("Invalid shift");
				break;
			}
		}
		return Operand(scratch);
	}

	// An operand in a register: a constant is put in @scratch
	Register toRegister(const Operand& op, const Register& scratch)
	{
		if (op.IsImmediate())
		{
			Mov(scratch, op.GetImmediate());
			return scratch;
		}
		if (op.IsPlainRegister())
			return op.GetRegister();
		Mov(scratch, op);
		return scratch;
	}

	Label *startConditional(ArmOp::Condition cc)
	{
		if (cc == ArmOp::AL)
			return nullptr;
		verify(cc <= ArmOp::LE);
		Label *label = new Label();
		// The ARM's conditions are encoded as the host's
		Ldr(w0, arm_reg_operand(RN_PSR_FLAGS));
		Msr(NZCV, x0);
		B(label, (Condition)((u32)cc ^ 1));

		return label;
	}

	void endConditional(Label *label)
	{
		if (label != nullptr)
		{
			Bind(label);
			delete label;
		}
	}

	/* The flags an instruction sets go back to the ARM's register file at
	 * once. A logical instruction sets N and Z from its result @result, C
	 * from the shift or the constant when there is one (w14), and leaves V;
	 * an arithmetic one sets all four as the host's NZCV has them. The bits
	 * below the flags are kept. */
	void storeLogicalFlags(const Register& result)
	{
		Ldr(w3, arm_reg_operand(RN_PSR_FLAGS));
		Tst(result, result);
		Mrs(x0, NZCV);
		Lsr(w0, w0, 30);
		Bfi(w3, w0, 30, 2);			// N, Z
		if (set_carry_bit)
			Bfi(w3, w14, 29, 1);	// C
		Str(w3, arm_reg_operand(RN_PSR_FLAGS));
	}

	void storeArithFlags()
	{
		Mrs(x0, NZCV);
		Ldr(w3, arm_reg_operand(RN_PSR_FLAGS));
		Lsr(w0, w0, 28);
		Bfi(w3, w0, 28, 4);			// N, Z, C, V
		Str(w3, arm_reg_operand(RN_PSR_FLAGS));
	}

	void emitDataProcOp(const ArmOp& op)
	{
		const bool mov_op = op.op_type == ArmOp::MOV || op.op_type == ArmOp::MVN;
		Operand arg0 = getOperand(op.arg[0], w1);
		Operand arg1 = Operand(0);
		if (!mov_op)
			arg1 = getOperand(op.arg[1], w2);
		Register rd;
		if (op.rd.isReg())
			rd = regalloc->map(op.rd.getReg().armreg);
		if (logical_op_set_flags)
		{
			// When an Operand2 constant is used with the instructions MOVS, MVNS, ANDS, ORRS, ORNS, EORS, BICS, TEQ or TST,
			// the carry flag is updated to bit[31] of the constant,
			// if the constant is greater than 255 and can be produced by shifting an 8-bit value.
			// The constant is the second operand, or the only one of MOV and
			// MVN. The first operand of the others can be a constant too,
			// r15 read as a value, and has nothing to do with the carry.
			const ArmOp::Operand& constant = mov_op ? op.arg[0] : op.arg[1];
			if (constant.isImmediate() && !constant.isShifted() && constant.getImmediate() > 255)
			{
				set_carry_bit = true;
				Mov(w14, (constant.getImmediate() & 0x80000000) >> 31);
			}
		}
		// the first operand of the others, in a register
		Register rn;
		if (!mov_op)
			rn = toRegister(arg0, w1);
		if (op.op_type == ArmOp::ADC || op.op_type == ArmOp::SBC || op.op_type == ArmOp::RSC)
		{
			// the ARM's carry into the host's: the same bit, the same sense
			Ldr(w0, arm_reg_operand(RN_PSR_FLAGS));
			Msr(NZCV, x0);
		}

		switch (op.op_type)
		{
		case ArmOp::AND:
			And(rd, rn, arg1);
			break;
		case ArmOp::ORR:
			Orr(rd, rn, arg1);
			break;
		case ArmOp::EOR:
			Eor(rd, rn, arg1);
			break;
		case ArmOp::BIC:
			Bic(rd, rn, arg1);
			break;
		case ArmOp::TST:
			And(w0, rn, arg1);
			break;
		case ArmOp::TEQ:
			Eor(w0, rn, arg1);
			break;
		case ArmOp::MOV:
			Mov(rd, arg0);
			break;
		case ArmOp::MVN:
			if (arg0.IsImmediate())
				Mov(rd, (u32)~arg0.GetImmediate());
			else
				Mvn(rd, arg0);
			break;

		case ArmOp::CMP:
			Cmp(rn, arg1);
			break;
		case ArmOp::CMN:
			Cmn(rn, arg1);
			break;
		case ArmOp::SUB:
			Sub(rd, rn, arg1, set_flags ? SetFlags : LeaveFlags);
			break;
		case ArmOp::SBC:
			if (set_flags)
				Sbcs(rd, rn, arg1);
			else
				Sbc(rd, rn, arg1);
			break;
		case ArmOp::RSB:
			// rd = op2 - rn, with the flags of that subtraction
			{
				Register op2 = toRegister(arg1, w2);
				Sub(rd, op2, rn, set_flags ? SetFlags : LeaveFlags);
			}
			break;
		case ArmOp::RSC:
			{
				Register op2 = toRegister(arg1, w2);
				if (set_flags)
					Sbcs(rd, op2, rn);
				else
					Sbc(rd, op2, rn);
			}
			break;
		case ArmOp::ADD:
			Add(rd, rn, arg1, set_flags ? SetFlags : LeaveFlags);
			break;
		case ArmOp::ADC:
			if (set_flags)
				Adcs(rd, rn, arg1);
			else
				Adc(rd, rn, arg1);
			break;
		default:
			die("invalid");
			break;
		}
		if (set_flags)
		{
			if (op.isLogicalOp())
				storeLogicalFlags(op.op_type == ArmOp::TST || op.op_type == ArmOp::TEQ ? Register(w0) : rd);
			else
				storeArithFlags();
		}
		/* A result that goes to the program counter loses its low two bits:
		 * the ARM takes instructions from whole words only. (A constant put
		 * there is the address of an instruction, and has none.) */
		if (op.rd.isReg() && op.rd.getReg().armreg == R15_ARM_NEXT
				&& !(op.op_type == ArmOp::MOV && op.arg[0].isImmediate() && !op.arg[0].isShifted()))
			And(rd, rd, 0xfffffffc);
	}

	void emitMemOp(const ArmOp& op)
	{
		// the address in w0, what is stored in w1: DoMemOp()'s arguments
		Operand base = getOperand(op.arg[0], w0);
		if (base.IsImmediate())
			Mov(w0, base.GetImmediate());
		else if (!base.GetRegister().Is(w0))
			Mov(w0, base.GetRegister());
		if (op.pre_index)
		{
			const ArmOp::Operand& offset = op.arg[1];
			if (offset.isReg())
			{
				Operand offset_op = getOperand(offset, w2);
				if (op.add_offset)
					Add(w0, w0, offset_op);
				else
					Sub(w0, w0, offset_op);
			}
			else if (offset.isImmediate() && offset.getImmediate() != 0)
			{
				if (op.add_offset)
					Add(w0, w0, offset.getImmediate());
				else
					Sub(w0, w0, offset.getImmediate());
			}
		}
		if (op.aligned)
			And(w0, w0, 0xfffffffc);
		if (op.op_type == ArmOp::STR)
		{
			if (op.arg[2].isImmediate())
				Mov(w1, op.arg[2].getImmediate());
			else
				Mov(w1, regalloc->map(op.arg[2].getReg().armreg));
		}

		/* Nearly every access is to the ARM's memory, and that is one load
		 * or store of the host's: it is done here. What is left for the call
		 * is the sound chip's registers, above 0x800000, and a word read
		 * from an address that is not a word's, which comes back rotated. */
		Label slow, done;
		And(w2, w0, 0x00ffffff);
		Cmp(w2, 0x00800000);
		B(&slow, hs);
		if (!op.byte_xfer)
		{
			if (op.op_type == ArmOp::LDR)
			{
				Tst(w0, 3);
				B(&slow, ne);
			}
			else
				And(w2, w2, 0xfffffffc);		// a word is stored at a word's address
		}
		Ldr(w4, MemOperand(sp, MASK_SLOT));
		Ldr(x3, MemOperand(sp, RAM_SLOT));
		And(w2, w2, w4);
		if (op.op_type == ArmOp::LDR)
		{
			if (op.byte_xfer)
				Ldrb(w0, MemOperand(x3, x2));
			else
				Ldr(w0, MemOperand(x3, x2));
		}
		else
		{
			if (op.byte_xfer)
				Strb(w1, MemOperand(x3, x2));
			else
				Str(w1, MemOperand(x3, x2));
		}
		B(&done);
		Bind(&slow);
		call(recompiler::getMemOp(op.op_type == ArmOp::LDR, op.byte_xfer));
		Bind(&done);

		if (op.op_type == ArmOp::LDR)
		{
			// the program counter has no low two bits
			if (op.rd.getReg().armreg == R15_ARM_NEXT)
				And(w0, w0, 0xfffffffc);
			Mov(regalloc->map(op.rd.getReg().armreg), w0);
		}
	}

	void emitBranch(const ArmOp& op)
	{
		if (op.arg[0].isImmediate() && !op.arg[0].isShifted())
			Mov(w0, op.arg[0].getImmediate());
		else
		{
			Register target = toRegister(getOperand(op.arg[0], w0), w0);
			And(w0, target, 0xfffffffc);
		}
		Str(w0, arm_reg_operand(R15_ARM_NEXT));
	}

	void emitMSR(const ArmOp& op)
	{
		if (op.arg[0].isImmediate())
			Mov(w0, op.arg[0].getImmediate());
		else
			Mov(w0, regalloc->map(op.arg[0].getReg().armreg));
		Mov(w1, op.psrMask);
		if (op.spsr)
			call((void *)recompiler::MSR_do<1>);
		else
			call((void *)recompiler::MSR_do<0>);
	}

	void emitMRS(const ArmOp& op)
	{
		call((void *)CPUUpdateCPSR);

		if (op.spsr)
			Ldr(regalloc->map(op.rd.getReg().armreg), arm_reg_operand(RN_SPSR));
		else
			Ldr(regalloc->map(op.rd.getReg().armreg), arm_reg_operand(RN_CPSR));
	}

	void emitFallback(const ArmOp& op)
	{
		set_flags = false;
		Mov(w0, op.arg[0].getImmediate());
		call((void *)recompiler::interpret);
	}

	/* On to the next block, as the x86-64 backend does it: the block's
	 * cycles come off here, and the shared dispatcher takes over when the
	 * time slice is used up (none left or less) or an interrupt is waiting;
	 * otherwise the block jumps to the next itself, through the slot of the
	 * address it goes to when that is fixed. */
	void emitDispatch(const std::vector<ArmOp>& block_ops, u32 cycles)
	{
		Label to_dispatch;
		Ldr(w0, arm_reg_operand(CYCL_CNT));
		Subs(w0, w0, cycles);
		Str(w0, arm_reg_operand(CYCL_CNT));
		B(&to_dispatch, le);
		Ldr(w1, arm_reg_operand(INTR_PEND));
		Cbnz(w1, &to_dispatch);

		const ArmOp *last = block_ops.empty() ? nullptr : &block_ops.back();
		if (last != nullptr && last->condition == ArmOp::AL && last->arg[0].isImmediate() && !last->arg[0].isShifted()
				&& ((last->op_type == ArmOp::B || last->op_type == ArmOp::BL)
					|| (last->op_type == ArmOp::MOV && last->rd.isReg() && last->rd.getReg().armreg == R15_ARM_NEXT)))
		{
			const u32 target = last->arg[0].getImmediate();
			Ldr(x3, MemOperand(x26, (int64_t)((target & (ARAM_SIZE_MAX - 1)) / 4) * 8));
		}
		else
		{
			Ldr(w0, arm_reg_operand(R15_ARM_NEXT));
			Ubfx(w2, w0, 2, 21);					// pc & 0x7ffffc, in entries
			Ldr(x3, MemOperand(x26, x2, LSL, 3));
		}
		Br(x3);

		Bind(&to_dispatch);
		jumpToDispatch();
	}

	void finish()
	{
		FinalizeCode();
		verify((size_t)GetBuffer()->GetCursorOffset() <= GetBuffer()->GetCapacity());
		vmem_platform_flush_cache(
				recompiler::writeToExec(GetBuffer()->GetStartAddress<void*>()), recompiler::writeToExec(GetBuffer()->GetEndAddress<void*>()),
				GetBuffer()->GetStartAddress<void*>(), GetBuffer()->GetEndAddress<void*>());
		recompiler::advance(GetBuffer()->GetSizeInBytes());
	}

public:
	Arm7Compiler() : MacroAssembler((vixl::byte *)recompiler::currentCode(), recompiler::spaceLeft()) {}

	void compile(const std::vector<ArmOp>& block_ops, u32 cycles)
	{
		regalloc = new AArch64ArmRegAlloc(*this, block_ops);

		ArmOp::Condition currentCondition = ArmOp::AL;
		Label *condLabel = nullptr;

		for (u32 i = 0; i < block_ops.size(); i++)
		{
			const ArmOp& op = block_ops[i];
			DEBUG_LOG(AICA_ARM, "-> %s", op.toString().c_str());

			set_flags = op.flags & ArmOp::OP_SETS_FLAGS;
			logical_op_set_flags = op.isLogicalOp() && set_flags;
			set_carry_bit = false;

			if (op.op_type == ArmOp::FALLBACK)
			{
				endConditional(condLabel);
				condLabel = nullptr;
				currentCondition = ArmOp::AL;
			}
			else if (op.condition != currentCondition)
			{
				endConditional(condLabel);
				currentCondition = op.condition;
				if (op.condition != ArmOp::AL)
					regalloc->storeDirty();
				condLabel = startConditional(op.condition);
			}

			regalloc->load(i);

			if (op.op_type <= ArmOp::MVN)
				// data processing op
				emitDataProcOp(op);
			else if (op.op_type <= ArmOp::STR)
				// memory load/store
				emitMemOp(op);
			else if (op.op_type <= ArmOp::BL)
				// branch
				emitBranch(op);
			else if (op.op_type == ArmOp::MRS)
				emitMRS(op);
			else if (op.op_type == ArmOp::MSR)
				emitMSR(op);
			else if (op.op_type == ArmOp::FALLBACK)
				emitFallback(op);
			else
				die("invalid");

			regalloc->store(i);

			/* The next instruction's condition is for the flags as this one
			 * leaves them, and writing the status register changes them as
			 * surely as an instruction that sets them. */
			if (set_flags || op.op_type == ArmOp::MSR)
			{
				currentCondition = ArmOp::AL;
				endConditional(condLabel);
				condLabel = nullptr;
			}
		}
		endConditional(condLabel);

		emitDispatch(block_ops, cycles);

		finish();

		delete regalloc;
		regalloc = nullptr;
	}

	void generateMainLoop()
	{
		if (!recompiler::empty())
		{
			verify(arm_mainloop != nullptr);
			verify(arm_compilecode != nullptr);
			return;
		}
		Label arm_dispatch_label;
		Label arm_dofiq;
		Label arm_exit;

		// arm_compilecode:
		arm_compilecode = (void (*)())recompiler::writeToExec(GetCursorAddress<void *>());
		call((void *)recompiler::compile);
		B(&arm_dispatch_label);

		// arm_mainloop(regs, entry points)
		arm_mainloop = (arm_mainloop_t)recompiler::writeToExec(GetCursorAddress<void *>());
		Stp(x19, x20, MemOperand(sp, -FRAME_SIZE, PreIndex));
		Stp(x21, x22, MemOperand(sp, 16));
		Stp(x23, x24, MemOperand(sp, 32));
		Stp(x25, x26, MemOperand(sp, 48));
		Stp(x27, x28, MemOperand(sp, 64));
		Stp(x29, x30, MemOperand(sp, 80));

		Mov(x28, x0);		// arm7 registers
		Mov(x26, x1);		// lookup base
		// the sound memory, for the loads and stores done in line
		Mov(x9, reinterpret_cast<uintptr_t>(&aica_ram.data));
		Ldr(x9, MemOperand(x9));
		Str(x9, MemOperand(sp, RAM_SLOT));
		Mov(x9, reinterpret_cast<uintptr_t>(&ARAM_MASK));
		Ldr(w9, MemOperand(x9));
		Str(w9, MemOperand(sp, MASK_SLOT));

		// arm_dispatch:
		Bind(&arm_dispatch_label);
		arm_dispatch = (void (*)())recompiler::writeToExec(GetCursorAddress<void *>());
		Ldr(w3, arm_reg_operand(CYCL_CNT));
		Ldp(w0, w1, arm_reg_operand(R15_ARM_NEXT));	// load Next PC, interrupt
		Cmp(w3, 0);
		B(&arm_exit, le);							// timeslice is over
		Cbnz(w1, &arm_dofiq);						// if interrupt pending, handle it

		Ubfx(w2, w0, 2, 21);						// pc & 0x7ffffc, in entries
		Ldr(x3, MemOperand(x26, x2, LSL, 3));
		Br(x3);

		// arm_dofiq:
		Bind(&arm_dofiq);
		call((void *)CPUFiq);
		B(&arm_dispatch_label);

		// arm_exit:
		Bind(&arm_exit);
		Ldp(x29, x30, MemOperand(sp, 80));
		Ldp(x27, x28, MemOperand(sp, 64));
		Ldp(x25, x26, MemOperand(sp, 48));
		Ldp(x23, x24, MemOperand(sp, 32));
		Ldp(x21, x22, MemOperand(sp, 16));
		Ldp(x19, x20, MemOperand(sp, FRAME_SIZE, PostIndex));
		Ret();

		finish();
	}
};

static_assert(INTR_PEND == R15_ARM_NEXT + 1, "the dispatcher loads the next pc and the interrupt as a pair");

void AArch64ArmRegAlloc::LoadReg(int host_reg, Arm7Reg armreg)
{
	assembler.Ldr(getReg(host_reg), arm_reg_operand(armreg));
}

void AArch64ArmRegAlloc::StoreReg(int host_reg, Arm7Reg armreg)
{
	assembler.Str(getReg(host_reg), arm_reg_operand(armreg));
}

void arm7backend_compile(const std::vector<ArmOp>& block_ops, u32 cycles)
{
	vmem_jit_write_begin();
	{
		Arm7Compiler assembler;
		assembler.compile(block_ops, cycles);
	}
	vmem_jit_write_end();
}

void arm7backend_flush()
{
	vmem_jit_write_begin();
	{
		Arm7Compiler assembler;
		assembler.generateMainLoop();
	}
	vmem_jit_write_end();
}

} // namespace aicaarm
#endif // ARM64 && DYNAREC_JIT
