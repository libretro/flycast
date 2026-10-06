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

#if	HOST_CPU == CPU_X64 && FEAT_AREC != DYNAREC_NONE

#include "deps/xbyak/xbyak.h"
#include "deps/xbyak/xbyak_util.h"
using namespace Xbyak::util;

#include "arm7_rec.h"

namespace aicaarm
{

static void (*arm_dispatch)();

#ifdef _WIN32
static const Xbyak::Reg32 call_regs[] = { ecx, edx, r8d, r9d };
#else
static const Xbyak::Reg32 call_regs[] = { edi, esi, edx, ecx  };
#endif
static void (**entry_points)();

class Arm7Compiler;

#ifdef _WIN32
#define ALLOC_REGS 8
static const std::array<Xbyak::Reg32, 8> alloc_regs {
		ebx, ebp, edi, esi, r12d, r13d, r14d, r15d
};
#else
#define ALLOC_REGS 6
static const std::array<Xbyak::Reg32, 6> alloc_regs {
		ebx, ebp, r12d, r13d, r14d, r15d
};
#endif

class X64ArmRegAlloc : public ArmRegAlloc<ALLOC_REGS, X64ArmRegAlloc>
{
	using super = ArmRegAlloc<ALLOC_REGS, X64ArmRegAlloc>;
	Arm7Compiler& assembler;

	void LoadReg(int host_reg, Arm7Reg armreg);
	void StoreReg(int host_reg, Arm7Reg armreg);

	static const Xbyak::Reg32& getReg32(int i)
	{
		verify(i >= 0 && (u32)i < alloc_regs.size());
		return alloc_regs[i];
	}

public:
	X64ArmRegAlloc(Arm7Compiler& assembler, const std::vector<ArmOp>& block_ops)
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
	X64ArmRegAlloc *regalloc = nullptr;

	static const u32 N_FLAG = 1 << 31;
	static const u32 Z_FLAG = 1 << 30;
	static const u32 C_FLAG = 1 << 29;
	static const u32 V_FLAG = 1 << 28;

	Xbyak::util::Cpu cpu;

	Xbyak::Operand getOperand(const ArmOp::Operand& arg, Xbyak::Reg32 scratch_reg)
	{
		Xbyak::Reg32 r;
		if (!arg.isReg())
		{
			/* A plain constant is left to the caller. One that still has
			 * a shift to go through is not plain: it is r15 read as a
			 * value, with a shift the decoder could not work out ahead
			 * (a rotate, or one whose carry the flags want). This used to
			 * hand those back as plain too, and the shift was lost. */
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
							mov(r10d, r);			// r10d = rm[31]
							shr(r10d, 31);
						}
						mov(r, 0);					// r = 0
					}
					else if (arg.shift_type == ArmOp::ASR)
					{
						if (set_carry_bit)
						{
							mov(r10d, r);			// r10d = rm[31]
							shr(r10d, 31);
						}
						sar(r, 31);					// r = rm < 0 ? -1 : 0
					}
					else if (arg.shift_type == ArmOp::ROR)
					{
						// RRX
						mov(r10d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
						shl(r10d, 2);
						verify(r != eax);
						mov(eax, r);				// eax = rm
						and_(r10d, 0x80000000);		// r10[31] = C
						shr(eax, 1);				// eax = eax >> 1
						or_(eax, r10d);				// eax[31] = C
						if (set_carry_bit)
						{
							mov(r10d, r);
							and_(r10d, 1);			// r10 = rm[0] (new C)
						}
						mov(r, eax);				// r = eax
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
							mov(r10d, r);
						if (set_carry_bit)
							shr(r10d, 32 - arg.shift_value);
						shl(r, arg.shift_value);			// r <<= shift
						if (set_carry_bit)
							and_(r10d, 1);					// r10d = rm[lsb]
					}
					else
					{
						if (set_carry_bit)
						{
							mov(r10d, r);
							shr(r10d, arg.shift_value - 1);
							and_(r10d, 1);					// r10d = rm[msb]
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
				switch (arg.shift_type)
				{
				/* The count is the low byte of the register, whatever the
				 * rest of it holds. This used to compare the whole register
				 * with 32, and took a count like 0x102 for "32 or more". */
				case ArmOp::LSL:
				case ArmOp::LSR:
					movzx(ecx, shift_reg.cvt8());
					mov(eax, 0);
					if (arg.shift_type == ArmOp::LSL)
						shl(r, cl);
					else
						shr(r, cl);
					cmp(ecx, 32);
					cmovnb(r, eax);		// LSL and LSR by 32 or more gives 0
					break;
				case ArmOp::ASR:
					movzx(ecx, shift_reg.cvt8());
					mov(eax, r);
					sar(eax, 31);
					sar(r, cl);
					cmp(ecx, 32);
					cmovnb(r, eax);		// ASR by 32 or more gives 0 or -1 depending on operand sign
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
		mov(eax, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
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

		Xbyak::Operand arg0 = getOperand(op.arg[0], r8d);
		Xbyak::Operand arg1 = getOperand(op.arg[1], r9d);
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
				mov(r10d, (constant.getImmediate() & 0x80000000) >> 31);
			}
		}

		/* Every instruction here but MOV and MVN has two operands, and they
		 * are taken by value, into r8d and r9d, before anything is written:
		 * the result may be going to the register one of them came from, or
		 * to both, and the first may be a constant (r15 read as a value).
		 * Working on the operands' own registers, as this used to, got some
		 * of those cases wrong and could not be assembled for others. */
		if (op.op_type != ArmOp::MOV && op.op_type != ArmOp::MVN)
		{
			if (arg0.isNone())
				mov(r8d, op.arg[0].getImmediate());
			else if (arg0 != r8d)
				mov(r8d, arg0);
			if (arg1.isNone())
				mov(r9d, op.arg[1].getImmediate());
			else if (arg1 != r9d)
				mov(r9d, arg1);
		}
		if (op.op_type == ArmOp::ADC || op.op_type == ArmOp::SBC || op.op_type == ArmOp::RSC)
		{
			// the ARM's carry into the host's
			mov(r11d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
			and_(r11d, C_FLAG);
			neg(r11d);
			if (op.op_type != ArmOp::ADC)
				cmc();		// subtracting, the ARM's carry is "no borrow"
		}

		switch (op.op_type)
		{
		case ArmOp::AND:
			and_(r8d, r9d);
			mov(rd, r8d);
			save_v_flag = false;
			break;
		case ArmOp::ORR:
			or_(r8d, r9d);
			mov(rd, r8d);
			save_v_flag = false;
			break;
		case ArmOp::EOR:
			xor_(r8d, r9d);
			mov(rd, r8d);
			save_v_flag = false;
			break;
		case ArmOp::BIC:
			not_(r9d);
			and_(r8d, r9d);
			mov(rd, r8d);
			save_v_flag = false;
			break;

		case ArmOp::TST:
			test(r8d, r9d);
			save_v_flag = false;
			break;
		case ArmOp::TEQ:
			xor_(r8d, r9d);
			save_v_flag = false;
			break;
		case ArmOp::CMP:
			cmp(r8d, r9d);
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			break;
		case ArmOp::CMN:
			add(r8d, r9d);
			if (set_flags)
			{
				setb(r10b);
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
				sub(r8d, r9d);
			else
				sbb(r8d, r9d);
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			mov(rd, r8d);
			break;
		case ArmOp::RSB:
		case ArmOp::RSC:
			// rd = op2 - rn (- !C)
			if (op.op_type == ArmOp::RSB)
				sub(r9d, r8d);
			else
				sbb(r9d, r8d);
			if (set_flags)
			{
				setnb(r10b);
				set_carry_bit = true;
			}
			mov(rd, r9d);
			break;
		case ArmOp::ADD:
		case ArmOp::ADC:
			if (op.op_type == ArmOp::ADD)
				add(r8d, r9d);
			else
				adc(r8d, r9d);
			if (set_flags)
			{
				setb(r10b);
				set_carry_bit = true;
			}
			mov(rd, r8d);
			break;
		default:
			die("invalid");
			break;
		}

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
			Xbyak::Operand offset_reg = getOperand(offset, r9d);
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

		call(recompiler::getMemOp(op.op_type == ArmOp::LDR, op.byte_xfer));

		if (op.op_type == ArmOp::LDR)
			mov(regalloc->map(op.rd.getReg().armreg), eax);
	}

	void saveFlags(bool save_v_flag)
	{
		if (!set_flags)
			return;

		pushf();
		pop(rax);

		if (save_v_flag)
		{
			mov(r11d, eax);
			shl(r11d, 28 - 11);		// V
		}
		shl(eax, 30 - 6);			// Z,N
		if (save_v_flag)
			and_(r11d, V_FLAG);		// V
		and_(eax, Z_FLAG | N_FLAG);	// Z,N
		if (save_v_flag)
			or_(eax, r11d);

		mov(r11d, dword[rip + &arm_Reg[RN_PSR_FLAGS].I]);
		if (set_carry_bit)
		{
			if (save_v_flag)
				and_(r11d, ~(Z_FLAG | N_FLAG | C_FLAG | V_FLAG));
			else
				and_(r11d, ~(Z_FLAG | N_FLAG | C_FLAG));
			shl(r10d, 29);
			or_(r11d, r10d);
		}
		else
		{
			if (save_v_flag)
				and_(r11d, ~(Z_FLAG | N_FLAG | V_FLAG));
			else
				and_(r11d, ~(Z_FLAG | N_FLAG));
		}
		or_(r11d, eax);
		mov(dword[rip + &arm_Reg[RN_PSR_FLAGS].I], r11d);
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
		mov(dword[rip + &arm_Reg[R15_ARM_NEXT].I], eax);
	}

	void emitMSR(const ArmOp& op)
	{
		if (op.arg[0].isImmediate())
			mov(call_regs[0], op.arg[0].getImmediate());
		else
			mov(call_regs[0], regalloc->map(op.arg[0].getReg().armreg));
		mov(call_regs[1], op.psrMask);
		if (op.spsr)
			call(recompiler::MSR_do<1>);
		else
			call(recompiler::MSR_do<0>);
	}

	void emitMRS(const ArmOp& op)
	{
		call(CPUUpdateCPSR);

		if (op.spsr)
			mov(regalloc->map(op.rd.getReg().armreg), dword[rip + &arm_Reg[RN_SPSR]]);
		else
			mov(regalloc->map(op.rd.getReg().armreg), dword[rip + &arm_Reg[RN_CPSR]]);
	}

	/* On to the next block. Every block used to jump to one dispatcher,
	 * whose one indirect jump then had to serve every block in the program,
	 * and the host has no way to guess where a jump like that goes next.
	 * Each block now ends in its own: a block mostly goes on to the same
	 * one or two, and its own jump learns them. Where the block ends by
	 * going to a fixed address there is nothing to look up at all.
	 *
	 * The shared dispatcher still takes over when the time slice is used
	 * up or an interrupt is waiting, which it checks for again itself. */
	void emitDispatch(const std::vector<ArmOp>& block_ops)
	{
		cmp(dword[rip + &arm_Reg[CYCL_CNT]], 0);
		jle((void*)arm_dispatch);
		cmp(dword[rip + &arm_Reg[INTR_PEND]], 0);
		jne((void*)arm_dispatch);

		const ArmOp *last = block_ops.empty() ? nullptr : &block_ops.back();
		if (last != nullptr && last->condition == ArmOp::AL && last->arg[0].isImmediate() && !last->arg[0].isShifted()
				&& ((last->op_type == ArmOp::B || last->op_type == ArmOp::BL)
					|| (last->op_type == ArmOp::MOV && last->rd.isReg() && last->rd.getReg().armreg == R15_ARM_NEXT)))
		{
			const u32 target = last->arg[0].getImmediate();
			jmp(qword[rip + &recompiler::EntryPoints[(target & (ARAM_SIZE_MAX - 1)) / 4]]);
		}
		else
		{
			mov(ecx, dword[rip + &arm_Reg[R15_ARM_NEXT]]);
			mov(rdx, qword[rip + &entry_points]);
			and_(ecx, 0x7ffffc);
			jmp(qword[rdx + rcx * 2]);
		}
	}

	void emitFallback(const ArmOp& op)
	{
		set_flags = false;
		mov(call_regs[0], op.arg[0].getImmediate());
		call(recompiler::interpret);
	}

public:
	Arm7Compiler() : Xbyak::CodeGenerator(recompiler::spaceLeft(), recompiler::currentCode()) { }

	void compile(const std::vector<ArmOp>& block_ops, u32 cycles)
	{
		regalloc = new X64ArmRegAlloc(*this, block_ops);

		sub(dword[rip + &arm_Reg[CYCL_CNT]], cycles);

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
			 * leaves them - and writing the status register changes them
			 * as surely as an instruction that sets them, which this did
			 * not count: an instruction after it with the same condition
			 * was run or skipped on the flags from before. */
			if (set_flags || op.op_type == ArmOp::MSR)
			{
				currentCondition = ArmOp::AL;
				endConditional(condLabel);
				condLabel = nullptr;
			}
		}
		endConditional(condLabel);

		emitDispatch(block_ops);

		ready();
		recompiler::advance(getSize());

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
		Xbyak::Label arm_dispatch_label;
		Xbyak::Label arm_mainloop_label;

		//arm_compilecode:
		call(recompiler::compile);
		jmp(arm_dispatch_label);

		// arm_mainloop:
		L(arm_mainloop_label);
#ifdef _WIN32
		push(rdi);
		push(rsi);
#endif
		push(r12);
		push(r13);
		push(r14);
		push(r15);
		push(rbx);
		push(rbp);
#ifdef _WIN32
		sub(rsp, 40);	// 32-byte shadow space + 16-byte stack alignment
#else
		sub(rsp, 8);		// 16-byte stack alignment
#endif

		mov(qword[rip + &entry_points], call_regs[1].cvt64());

		// arm_dispatch:
		L(arm_dispatch_label);
		mov(rdx, qword[rip + &entry_points]);
		mov(ecx, dword[rip + &arm_Reg[R15_ARM_NEXT]]);
		mov(eax, dword[rip + &arm_Reg[INTR_PEND]]);
		cmp(dword[rip + &arm_Reg[CYCL_CNT]], 0);
		Xbyak::Label arm_exit;
		jle(arm_exit);			// timeslice is over
		test(eax, eax);
		Xbyak::Label arm_dofiq;
		jne(arm_dofiq);			// if interrupt pending, handle it

		and_(ecx, 0x7ffffc);
		jmp(qword[rdx + rcx * 2]);

		// arm_dofiq:
		L(arm_dofiq);
		call(CPUFiq);
		jmp(arm_dispatch_label);

		// arm_exit:
		L(arm_exit);
#ifdef _WIN32
		add(rsp, 40);
#else
		add(rsp, 8);
#endif
		pop(rbp);
		pop(rbx);
		pop(r15);
		pop(r14);
		pop(r13);
		pop(r12);
#ifdef _WIN32
		pop(rsi);
		pop(rdi);
#endif
		ret();

		ready();
		arm_compilecode = (void (*)())getCode();
		arm_mainloop = (arm_mainloop_t)arm_mainloop_label.getAddress();
		arm_dispatch = (void (*)())arm_dispatch_label.getAddress();

		recompiler::advance(getSize());
	}

};

void X64ArmRegAlloc::LoadReg(int host_reg, Arm7Reg armreg)
{
	// printf("LoadReg X%d <- r%d\n", host_reg, armreg);
	assembler.mov(getReg32(host_reg), dword[rip + &arm_Reg[(u32)armreg].I]);
}

void X64ArmRegAlloc::StoreReg(int host_reg, Arm7Reg armreg)
{
	// printf("StoreReg X%d -> r%d\n", host_reg, armreg);
	assembler.mov(dword[rip + &arm_Reg[(u32)armreg].I], getReg32(host_reg));
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
#endif // X64 && DYNAREC_JIT
