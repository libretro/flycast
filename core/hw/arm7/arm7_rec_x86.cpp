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

/* The sound processor's recompiler for 32-bit x86, with xbyak.
 *
 * It is arm7_rec_x64.cpp made for a processor with eight registers, and
 * does what that one does for every instruction, in the same order and
 * with the same results, flags and memory accesses: the blocks, their
 * cycles and the register allocation are arm7_rec.cpp's, shared by every
 * backend. What differs is what the processor has:
 *
 * - four registers for the ARM's (ebx, ebp, esi, edi, which the C code
 *   called keeps), and eax, ecx and edx for everything else. The two
 *   operands of an instruction are taken into edx and eax, and the
 *   shifter's carry, where it is wanted, into ecx. Where x86-64 had a
 *   spare register for a value to use in place of a shift's, the value is
 *   in memory (cmov takes it from there), and a rotate through the carry
 *   (RRX) is the host's own rcr, with the ARM's carry put in the host's
 *   by bt;
 * - the emulator's data reached at its address, which 32 bits can always
 *   give, where x86-64 reaches it relative to the instruction;
 * - calls made fastcall, the first two arguments in ecx and edx, as every
 *   handler called from here is declared (DYNACALL), with the stack a
 *   multiple of 16 at each one, as Linux and Android want;
 * - four bytes to an entry point.
 *
 * Everything blocks jump to and call that is not C - the main loop, the
 * routine that has a block compiled - is written into the code buffer by
 * arm7backend_flush(), so nothing here is assembly linked into the
 * library, which keeps it free of relocations in its code (Android refuses
 * a library that has them). */

#include "build.h"

#if	HOST_CPU == CPU_X86 && FEAT_AREC != DYNAREC_NONE

#include "deps/xbyak/xbyak.h"
#include "deps/xbyak/xbyak_util.h"
using namespace Xbyak::util;

#include "arm7_rec.h"
#include "hw/aica/aica_if.h"

namespace aicaarm
{

static void (*arm_dispatch)();

// fastcall
static const Xbyak::Reg32 call_regs[] = { ecx, edx };
static void (**entry_points)();

/* What a shift by a register gives in place of its own result for a count
 * of 32 or more: 0 for the logical shifts, and the count an arithmetic
 * shift by 31 is the same as. */
static const u32 shift_zero = 0;
static const u32 shift_asr_max = 31;

class Arm7Compiler;

#define ALLOC_REGS 4
static const std::array<Xbyak::Reg32, 4> alloc_regs {
		ebx, ebp, esi, edi
};

class X86ArmRegAlloc : public ArmRegAlloc<ALLOC_REGS, X86ArmRegAlloc>
{
	using super = ArmRegAlloc<ALLOC_REGS, X86ArmRegAlloc>;
	Arm7Compiler& assembler;

	void LoadReg(int host_reg, Arm7Reg armreg);
	void StoreReg(int host_reg, Arm7Reg armreg);

	static const Xbyak::Reg32& getReg32(int i)
	{
		verify(i >= 0 && (u32)i < alloc_regs.size());
		return alloc_regs[i];
	}

public:
	X86ArmRegAlloc(Arm7Compiler& assembler, const std::vector<ArmOp>& block_ops)
		: super(block_ops), assembler(assembler) {}

	const Xbyak::Reg32& map(Arm7Reg r)
	{
		int i = super::map(r);
		return getReg32(i);
	}

	friend super;
};

class Arm7Compiler : public Xbyak::CodeGenerator
{
	bool logical_op_set_flags = false;
	bool set_carry_bit = false;
	bool set_flags = false;
	X86ArmRegAlloc *regalloc = nullptr;

	static const u32 N_FLAG = 1 << 31;
	static const u32 Z_FLAG = 1 << 30;
	static const u32 C_FLAG = 1 << 29;
	static const u32 V_FLAG = 1 << 28;

	// the ARM's flags, in its register file
	Xbyak::Address armFlags() const {
		return dword[&arm_Reg[RN_PSR_FLAGS].I];
	}

	/* The shifter's carry goes in ecx (bit 0, the rest 0, or bit 0 of cl
	 * with the rest of ecx anything above bit 7: saveFlags() only keeps
	 * bit 0's place). */
	Xbyak::Operand getOperand(const ArmOp::Operand& arg, Xbyak::Reg32 scratch_reg)
	{
		Xbyak::Reg32 r;
		if (!arg.isReg())
		{
			/* A plain constant is left to the caller. One that still has
			 * a shift to go through is r15 read as a value, with a shift
			 * the decoder could not work out ahead. */
			if (arg.isNone() || !arg.isShifted())
				return Xbyak::Operand();
			mov(scratch_reg, arg.getImmediate());
			r = scratch_reg;
		}
		else
			r = regalloc->map(arg.getReg().armreg);
		if (arg.isShifted())
		{
			if (r != scratch_reg)
			{
				mov(scratch_reg, r);
				r = scratch_reg;
			}
			if (arg.shift_imm)
			{
				// shift by immediate
				if (arg.shift_type != ArmOp::ROR && arg.shift_value != 0 && !logical_op_set_flags)
				{
					switch (arg.shift_type)
					{
					case ArmOp::LSL:
						shl(r, arg.shift_value);
						break;
					case ArmOp::LSR:
						shr(r, arg.shift_value);
						break;
					case ArmOp::ASR:
						sar(r, arg.shift_value);
						break;
					default:
						die("invalid");
						break;
					}
				}
				else if (arg.shift_value == 0)
				{
					// Shift by 32
					if (logical_op_set_flags)
						set_carry_bit = true;
					if (arg.shift_type == ArmOp::LSR)
					{
						if (set_carry_bit)
						{
							mov(ecx, r);			// ecx = rm[31]
							shr(ecx, 31);
						}
						mov(r, 0);					// r = 0
					}
					else if (arg.shift_type == ArmOp::ASR)
					{
						if (set_carry_bit)
						{
							mov(ecx, r);			// ecx = rm[31]
							shr(ecx, 31);
						}
						sar(r, 31);					// r = rm < 0 ? -1 : 0
					}
					else if (arg.shift_type == ArmOp::ROR)
					{
						// RRX: the ARM's carry into the top, bit 0 out to the host's carry
						bt(armFlags(), 29);
						rcr(r, 1);
						if (set_carry_bit)
							setc(cl);				// cl = rm[0] (new C)
					}
					else
						die("Invalid shift");
				}
				else
				{
					// Carry must be preserved or Ror shift
					if (logical_op_set_flags)
						set_carry_bit = true;
					if (arg.shift_type == ArmOp::LSL)
					{
						if (set_carry_bit)
						{
							mov(ecx, r);
							shr(ecx, 32 - arg.shift_value);
						}
						shl(r, arg.shift_value);			// r <<= shift
						if (set_carry_bit)
							and_(ecx, 1);					// ecx = rm[lsb]
					}
					else
					{
						if (set_carry_bit)
						{
							mov(ecx, r);
							shr(ecx, arg.shift_value - 1);
							and_(ecx, 1);					// ecx = rm[msb]
						}

						if (arg.shift_type == ArmOp::LSR)
							shr(r, arg.shift_value);		// r >>= shift
						else if (arg.shift_type == ArmOp::ASR)
							sar(r, arg.shift_value);
						else if (arg.shift_type == ArmOp::ROR)
							ror(r, arg.shift_value);
						else
							die("Invalid shift");
					}
				}
			}
			else
			{
				// shift by register
				const Xbyak::Reg32 shift_reg = regalloc->map(arg.shift_reg.armreg);
				/* The count is the low byte of the register, whatever the
				 * rest of it holds. (esi, edi and ebp have no byte register
				 * of their own here: the whole register is taken first.) */
				switch (arg.shift_type)
				{
				case ArmOp::LSL:
				case ArmOp::LSR:
					mov(ecx, shift_reg);
					movzx(ecx, cl);
					if (arg.shift_type == ArmOp::LSL)
						shl(r, cl);
					else
						shr(r, cl);
					cmp(ecx, 32);
					cmovnb(r, dword[&shift_zero]);	// LSL and LSR by 32 or more gives 0
					break;
				case ArmOp::ASR:
					mov(ecx, shift_reg);
					movzx(ecx, cl);
					cmp(ecx, 32);
					cmovnb(ecx, dword[&shift_asr_max]);	// ASR by 32 or more gives 0 or -1 depending on operand sign, as by 31
					sar(r, cl);
					break;
				case ArmOp::ROR:
					mov(ecx, shift_reg);
					ror(r, cl);
					break;
				default:
					die("Invalid shift");
					break;
				}
			}
		}
		return r;
	}

	Xbyak::Label *startConditional(ArmOp::Condition cc)
	{
		if (cc == ArmOp::AL)
			return nullptr;
		Xbyak::Label *label = new Xbyak::Label();
		cc = (ArmOp::Condition)((u32)cc ^ 1);	// invert the condition
		mov(eax, armFlags());
		switch (cc)
		{
		case ArmOp::EQ:	// Z==1
			and_(eax, Z_FLAG);
			jnz(*label, T_NEAR);
			break;
		case ArmOp::NE:	// Z==0
			and_(eax, Z_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::CS:	// C==1
			and_(eax, C_FLAG);
			jnz(*label, T_NEAR);
			break;
		case ArmOp::CC:	// C==0
			and_(eax, C_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::MI:	// N==1
			and_(eax, N_FLAG);
			jnz(*label, T_NEAR);
			break;
		case ArmOp::PL:	// N==0
			and_(eax, N_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::VS:	// V==1
			and_(eax, V_FLAG);
			jnz(*label, T_NEAR);
			break;
		case ArmOp::VC:	// V==0
			and_(eax, V_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::HI:	// (C==1) && (Z==0)
			and_(eax, C_FLAG | Z_FLAG);
			cmp(eax, C_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::LS:	// (C==0) || (Z==1)
			and_(eax, C_FLAG | Z_FLAG);
			cmp(eax, C_FLAG);
			jnz(*label, T_NEAR);
			break;
		case ArmOp::GE:	// N==V
			mov(ecx, eax);
			shl(ecx, 3);
			xor_(eax, ecx);
			and_(eax, N_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::LT:	// N!=V
			mov(ecx, eax);
			shl(ecx, 3);
			xor_(eax, ecx);
			and_(eax, N_FLAG);
			jnz(*label, T_NEAR);
			break;
		case ArmOp::GT:	// (Z==0) && (N==V)
			mov(ecx, eax);
			mov(edx, eax);
			shl(ecx, 3);
			shl(edx, 1);
			xor_(eax, ecx);
			or_(eax, edx);
			and_(eax, N_FLAG);
			jz(*label, T_NEAR);
			break;
		case ArmOp::LE:	// (Z==1) || (N!=V)
			mov(ecx, eax);
			mov(edx, eax);
			shl(ecx, 3);
			shl(edx, 1);
			xor_(eax, ecx);
			or_(eax, edx);
			and_(eax, N_FLAG);
			jnz(*label, T_NEAR);
			break;
		default:
			die("Invalid condition code");
			break;
		}

		return label;
	}

	void endConditional(Xbyak::Label *label)
	{
		if (label != nullptr)
		{
			L(*label);
			delete label;
		}
	}

	bool emitDataProcOp(const ArmOp& op)
	{
		bool save_v_flag = true;

		Xbyak::Operand arg0 = getOperand(op.arg[0], edx);
		Xbyak::Operand arg1 = getOperand(op.arg[1], eax);
		Xbyak::Reg32 rd;
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
			const ArmOp::Operand& constant = (op.op_type == ArmOp::MOV || op.op_type == ArmOp::MVN) ? op.arg[0] : op.arg[1];
			if (constant.isImmediate() && !constant.isShifted() && constant.getImmediate() > 255)
			{
				set_carry_bit = true;
				mov(ecx, (constant.getImmediate() & 0x80000000) >> 31);
			}
		}

		/* Every instruction here but MOV and MVN has two operands, and they
		 * are taken by value, into edx and eax, before anything is written:
		 * the result may be going to the register one of them came from. */
		if (op.op_type != ArmOp::MOV && op.op_type != ArmOp::MVN)
		{
			if (arg0.isNone())
				mov(edx, op.arg[0].getImmediate());
			else if (arg0 != edx)
				mov(edx, arg0);
			if (arg1.isNone())
				mov(eax, op.arg[1].getImmediate());
			else if (arg1 != eax)
				mov(eax, arg1);
		}
		if (op.op_type == ArmOp::ADC || op.op_type == ArmOp::SBC || op.op_type == ArmOp::RSC)
		{
			// the ARM's carry into the host's
			bt(armFlags(), 29);
			if (op.op_type != ArmOp::ADC)
				cmc();		// subtracting, the ARM's carry is "no borrow"
		}

		switch (op.op_type)
		{
		case ArmOp::AND:
			and_(edx, eax);
			mov(rd, edx);
			save_v_flag = false;
			break;
		case ArmOp::ORR:
			or_(edx, eax);
			mov(rd, edx);
			save_v_flag = false;
			break;
		case ArmOp::EOR:
			xor_(edx, eax);
			mov(rd, edx);
			save_v_flag = false;
			break;
		case ArmOp::BIC:
			not_(eax);
			and_(edx, eax);
			mov(rd, edx);
			save_v_flag = false;
			break;

		case ArmOp::TST:
			test(edx, eax);
			save_v_flag = false;
			break;
		case ArmOp::TEQ:
			xor_(edx, eax);
			save_v_flag = false;
			break;
		case ArmOp::CMP:
			cmp(edx, eax);
			if (set_flags)
			{
				setnb(cl);
				set_carry_bit = true;
			}
			break;
		case ArmOp::CMN:
			add(edx, eax);
			if (set_flags)
			{
				setb(cl);
				set_carry_bit = true;
			}
			break;

		case ArmOp::MOV:
			if (arg0.isNone())
				mov(rd, op.arg[0].getImmediate());
			else if (arg0 != rd)
				mov(rd, arg0);
			if (set_flags)
			{
				test(rd, rd);
				save_v_flag = false;
			}
			break;
		case ArmOp::MVN:
			if (arg0.isNone())
				mov(rd, ~op.arg[0].getImmediate());
			else
			{
				if (arg0 != rd)
					mov(rd, arg0);
				not_(rd);
			}
			if (set_flags)
			{
				test(rd, rd);
				save_v_flag = false;
			}
			break;

		case ArmOp::SUB:
		case ArmOp::SBC:
			// rd = rn - op2 (- !C)
			if (op.op_type == ArmOp::SUB)
				sub(edx, eax);
			else
				sbb(edx, eax);
			if (set_flags)
			{
				setnb(cl);
				set_carry_bit = true;
			}
			mov(rd, edx);
			break;
		case ArmOp::RSB:
		case ArmOp::RSC:
			// rd = op2 - rn (- !C)
			if (op.op_type == ArmOp::RSB)
				sub(eax, edx);
			else
				sbb(eax, edx);
			if (set_flags)
			{
				setnb(cl);
				set_carry_bit = true;
			}
			mov(rd, eax);
			break;
		case ArmOp::ADD:
		case ArmOp::ADC:
			if (op.op_type == ArmOp::ADD)
				add(edx, eax);
			else
				adc(edx, eax);
			if (set_flags)
			{
				setb(cl);
				set_carry_bit = true;
			}
			mov(rd, edx);
			break;
		default:
			die("invalid");
			break;
		}
		/* A result that goes to the program counter loses its low two bits:
		 * the ARM takes instructions from whole words only. (A constant put
		 * there is the address of an instruction, and has none. None of
		 * these sets the flags, so the host's are free.) */
		if (op.rd.isReg() && op.rd.getReg().armreg == R15_ARM_NEXT
				&& !(op.op_type == ArmOp::MOV && arg0.isNone()))
			and_(rd, 0xfffffffc);

		return save_v_flag;
	}

	void emitMemOp(const ArmOp& op)
	{
		Xbyak::Operand addr_reg = getOperand(op.arg[0], call_regs[0]);
		if (addr_reg != call_regs[0])
		{
			if (addr_reg.isNone())
				mov(call_regs[0], op.arg[0].getImmediate());
			else
				mov(call_regs[0], addr_reg);
			addr_reg = call_regs[0];
		}
		if (op.pre_index)
		{
			const ArmOp::Operand& offset = op.arg[1];
			Xbyak::Operand offset_reg = getOperand(offset, eax);
			if (!offset_reg.isNone())
			{
				if (op.add_offset)
					add(addr_reg, offset_reg);
				else
					sub(addr_reg, offset_reg);
			}
			else if (offset.isImmediate() && offset.getImmediate() != 0)
			{
				if (op.add_offset)
					add(addr_reg, offset.getImmediate());
				else
					sub(addr_reg, offset.getImmediate());
			}
		}
		if (op.aligned)
			and_(addr_reg, 0xfffffffc);
		if (op.op_type == ArmOp::STR)
		{
			if (op.arg[2].isImmediate())
				mov(call_regs[1], op.arg[2].getImmediate());
			else
				mov(call_regs[1], regalloc->map(op.arg[2].getReg().armreg));
		}

		/* Nearly every access is to the ARM's memory, and that is one move
		 * to or from the host's: it is done here. What is left for the call
		 * is the sound chip's registers, above 0x800000, and a word read
		 * from an address that is not a word's, which comes back rotated. */
		Xbyak::Label slow, done;
		mov(eax, addr_reg);
		and_(eax, 0x00ffffff);
		cmp(eax, 0x00800000);
		jae(slow);
		if (!op.byte_xfer)
		{
			if (op.op_type == ArmOp::LDR)
			{
				test(eax, 3);
				jnz(slow);
			}
			else
				and_(eax, 0xfffffffc);		// a word is stored at a word's address
		}
		and_(eax, dword[&ARAM_MASK]);
		add(eax, dword[&aica_ram.data]);
		if (op.op_type == ArmOp::LDR)
		{
			if (op.byte_xfer)
				movzx(eax, byte[eax]);
			else
				mov(eax, dword[eax]);
		}
		else
		{
			if (op.byte_xfer)
				mov(byte[eax], dl);
			else
				mov(dword[eax], call_regs[1]);
		}
		jmp(done);
		L(slow);
		call(recompiler::getMemOp(op.op_type == ArmOp::LDR, op.byte_xfer));
		L(done);

		if (op.op_type == ArmOp::LDR)
		{
			// the program counter has no low two bits
			if (op.rd.getReg().armreg == R15_ARM_NEXT)
				and_(eax, 0xfffffffc);
			mov(regalloc->map(op.rd.getReg().armreg), eax);
		}
	}

	void saveFlags(bool save_v_flag)
	{
		if (!set_flags)
			return;

		pushf();
		pop(eax);

		if (save_v_flag)
		{
			mov(edx, eax);
			shl(edx, 28 - 11);		// V
		}
		shl(eax, 30 - 6);			// Z,N
		if (save_v_flag)
			and_(edx, V_FLAG);		// V
		and_(eax, Z_FLAG | N_FLAG);	// Z,N
		if (save_v_flag)
			or_(eax, edx);

		mov(edx, armFlags());
		if (set_carry_bit)
		{
			if (save_v_flag)
				and_(edx, ~(Z_FLAG | N_FLAG | C_FLAG | V_FLAG));
			else
				and_(edx, ~(Z_FLAG | N_FLAG | C_FLAG));
			shl(ecx, 29);
			or_(edx, ecx);
		}
		else
		{
			if (save_v_flag)
				and_(edx, ~(Z_FLAG | N_FLAG | V_FLAG));
			else
				and_(edx, ~(Z_FLAG | N_FLAG));
		}
		or_(edx, eax);
		mov(armFlags(), edx);
	}

	void emitBranch(const ArmOp& op)
	{
		Xbyak::Operand addr_reg = getOperand(op.arg[0], eax);
		if (addr_reg.isNone())
			mov(eax, op.arg[0].getImmediate());
		else
		{
			if (eax != addr_reg)
				mov(eax, addr_reg);
			and_(eax, 0xfffffffc);
		}
		mov(dword[&arm_Reg[R15_ARM_NEXT].I], eax);
	}

	void emitMSR(const ArmOp& op)
	{
		if (op.arg[0].isImmediate())
			mov(call_regs[0], op.arg[0].getImmediate());
		else
			mov(call_regs[0], regalloc->map(op.arg[0].getReg().armreg));
		mov(call_regs[1], op.psrMask);
		if (op.spsr)
			call((const void *)&recompiler::MSR_do<1>);
		else
			call((const void *)&recompiler::MSR_do<0>);
	}

	void emitMRS(const ArmOp& op)
	{
		call((const void *)&CPUUpdateCPSR);

		if (op.spsr)
			mov(regalloc->map(op.rd.getReg().armreg), dword[&arm_Reg[RN_SPSR].I]);
		else
			mov(regalloc->map(op.rd.getReg().armreg), dword[&arm_Reg[RN_CPSR].I]);
	}

	/* On to the next block, by a jump of the block's own (see
	 * arm7_rec_x64.cpp): through the address's entry point when the block
	 * ends by going to a fixed address, else through the table. The
	 * shared dispatcher takes over when the time slice is used up or an
	 * interrupt is waiting. */
	void emitDispatch(const std::vector<ArmOp>& block_ops, u32 cycles)
	{
		/* The block's cycles come off here, at its end, and the slice is
		 * over at none left or less: the subtraction leaves the flags the
		 * jump wants. */
		sub(dword[&arm_Reg[CYCL_CNT].I], cycles);
		jle((const void *)arm_dispatch);
		cmp(dword[&arm_Reg[INTR_PEND].I], 0);
		jne((const void *)arm_dispatch);

		const ArmOp *last = block_ops.empty() ? nullptr : &block_ops.back();
		if (last != nullptr && last->condition == ArmOp::AL && last->arg[0].isImmediate() && !last->arg[0].isShifted()
				&& ((last->op_type == ArmOp::B || last->op_type == ArmOp::BL)
					|| (last->op_type == ArmOp::MOV && last->rd.isReg() && last->rd.getReg().armreg == R15_ARM_NEXT)))
		{
			const u32 target = last->arg[0].getImmediate();
			jmp(dword[&recompiler::EntryPoints[(target & (ARAM_SIZE_MAX - 1)) / 4]]);
		}
		else
		{
			mov(ecx, dword[&arm_Reg[R15_ARM_NEXT].I]);
			mov(edx, dword[&entry_points]);
			and_(ecx, 0x7ffffc);
			jmp(dword[edx + ecx]);		// four bytes to an entry point, one for each word
		}
	}

	void emitFallback(const ArmOp& op)
	{
		set_flags = false;
		mov(call_regs[0], op.arg[0].getImmediate());
		call((const void *)&recompiler::interpret);
	}

public:
	Arm7Compiler() : Xbyak::CodeGenerator(recompiler::spaceLeft(), recompiler::currentCode()) { }

	void compile(const std::vector<ArmOp>& block_ops, u32 cycles)
	{
		X86ArmRegAlloc block_regalloc(*this, block_ops);
		regalloc = &block_regalloc;

		ArmOp::Condition currentCondition = ArmOp::AL;
		Xbyak::Label *condLabel = nullptr;

		for (u32 i = 0; i < block_ops.size(); i++)
		{
			const ArmOp& op = block_ops[i];
			DEBUG_LOG(AICA_ARM, "-> %s", op.toString().c_str());

			set_flags = op.flags & ArmOp::OP_SETS_FLAGS;
			logical_op_set_flags = op.isLogicalOp() && set_flags;
			set_carry_bit = false;
			bool save_v_flag = true;

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
				save_v_flag = emitDataProcOp(op);
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

			saveFlags(save_v_flag);

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

		ready();
		recompiler::advance(getSize());

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
		Xbyak::Label arm_dispatch_label;
		Xbyak::Label arm_mainloop_label;

		//arm_compilecode:
		call((const void *)&recompiler::compile);
		jmp(arm_dispatch_label, T_NEAR);

		// arm_mainloop: called from C, with its arguments on the stack
		L(arm_mainloop_label);
		mov(eax, dword[esp + 8]);		// entrypoints
		mov(dword[&entry_points], eax);
		push(ebp);
		push(ebx);
		push(esi);
		push(edi);
		// (the caller's stack pointer, under the place it is aligned to)
		mov(eax, esp);
		and_(esp, ~15);
		sub(esp, 12);
		push(eax);

		// arm_dispatch:
		L(arm_dispatch_label);
		mov(edx, dword[&entry_points]);
		mov(ecx, dword[&arm_Reg[R15_ARM_NEXT].I]);
		mov(eax, dword[&arm_Reg[INTR_PEND].I]);
		cmp(dword[&arm_Reg[CYCL_CNT].I], 0);
		Xbyak::Label arm_exit;
		jle(arm_exit, T_NEAR);	// timeslice is over
		test(eax, eax);
		Xbyak::Label arm_dofiq;
		jne(arm_dofiq, T_NEAR);	// if interrupt pending, handle it

		and_(ecx, 0x7ffffc);
		jmp(dword[edx + ecx]);

		// arm_dofiq:
		L(arm_dofiq);
		call((const void *)&CPUFiq);
		jmp(arm_dispatch_label, T_NEAR);

		// arm_exit:
		L(arm_exit);
		pop(esp);
		pop(edi);
		pop(esi);
		pop(ebx);
		pop(ebp);
		ret();

		ready();
		arm_compilecode = (void (*)())getCode();
		arm_mainloop = (arm_mainloop_t)arm_mainloop_label.getAddress();
		arm_dispatch = (void (*)())arm_dispatch_label.getAddress();

		recompiler::advance(getSize());
	}

};

void X86ArmRegAlloc::LoadReg(int host_reg, Arm7Reg armreg)
{
	assembler.mov(getReg32(host_reg), dword[&arm_Reg[(u32)armreg].I]);
}

void X86ArmRegAlloc::StoreReg(int host_reg, Arm7Reg armreg)
{
	assembler.mov(dword[&arm_Reg[(u32)armreg].I], getReg32(host_reg));
}

void arm7backend_compile(const std::vector<ArmOp>& block_ops, u32 cycles)
{
	Arm7Compiler assembler;
	assembler.compile(block_ops, cycles);
}

void arm7backend_flush()
{
	Arm7Compiler assembler;
	assembler.generateMainLoop();
}

} // namespace aicaarm
#endif // X86 && DYNAREC_JIT
