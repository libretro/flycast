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
/* The SH4 recompiler for 32-bit x86, with xbyak.
 *
 * It does what the x86-64 one (rec-x64/rec_x64.cpp) does, the same way and
 * in the same places, so that the two run a game the same to the cycle:
 * a block's cycles come off the counter where it ends, the time slice is
 * looked at in the same places, wait loops are found the same way, and the
 * floating point gives the same bits. What differs is what the processor
 * has: seven registers and no room for the SH4's context or the cycle
 * counter in one of them, so both are reached at their addresses, which
 * 32 bits can always give; the calls blocks make are fastcall (ecx, edx),
 * as every handler they reach is on this processor (DYNACALL); and a load
 * or store goes straight to memory only within the 512 MB the host has
 * laid out, the address taken down to it first.
 *
 * Everything the blocks jump to and call that is not C - the main loop,
 * the routines that compile a block or link one to the next, the ones a
 * fast memory access is rewritten to call - is written into the code
 * cache by ngen_init(), ahead of the blocks. So nothing here is assembly
 * linked into the library, which then has no relocations in its code: a
 * library that has them is not loaded by Android from version 6 on. */
#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86

#include "deps/xbyak/xbyak.h"
#include "deps/xbyak/xbyak_util.h"

#include <vector>
#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/dyna/wait_site.h"
#include "hw/holly/sb_mem.h"
#include "x86_regalloc.h"
#include "x86_vector.h"

using namespace Xbyak::util;

/* One block goes on to the next with a jump straight to its code, as on
 * x86-64 (see DynaRBI there). A link site is six bytes here: a call to the
 * link stub, a jump to the next block, or a jump through the table of
 * blocks (jmp dword [address]), each with what is left over filled. */
#define LINK_SITE_SIZE 6

struct DynaRBI : RuntimeBlockInfo
{
	/* Where in the block's code its link sites are, 0 where there is none:
	 * the way out to BranchBlock, and the one to NextBlock. */
	u32 link_at[2] = { 0, 0 };
	// where a block linked to this one comes in: past its address test
	u32 linked_entry = 0;

	virtual u32 Relink();

	virtual void Relocate(void* dst) {
		verify(false);
	}
};

/* What is left of the time slice: x86-64 has it in r14, this processor has
 * no register to spare and keeps it here. The SH4's context has another
 * copy, for the timers, which is written where the x86-64 recompiler
 * writes it, and only there: the state has it. */
extern "C" {
	int cycle_counter;
}

// Made by ngen_init(), in the code cache
static u8 *mainloop_code;
static u8 *block_return;	// a block that does not go on to another jumps here
static u8 *compile_stub;	// what the table of blocks has for an address with no block
static u8 *unwind_stub;		// back to the main loop from an exception, wherever it is
static u8 *link_stub;
// the stack as blocks run on it, which unwind_stub puts back
static u32 jmp_esp;

/* Back to the main loop, from code a block called that has raised an
 * SH4 exception and set the context for it (x86-64 has longjmp() for this:
 * the stack as it was is all there is to put back here). */
[[noreturn]] static void rec_unwind()
{
	((void (*)())unwind_stub)();
	__builtin_unreachable();
}

void ngen_mainloop(void* v_cntx)
{
	((void (*)())mainloop_code)();
}

/* The kinds of fast memory access there are, as on x86-64: a load or a
 * store, of 1, 2, 4 or 8 bytes, the 4 from or to a general register or a
 * floating-point one. 8 bytes are always in xmm0. */
enum
{
	MEM_LOAD8, MEM_LOAD16, MEM_LOAD32, MEM_LOAD32F, MEM_LOAD64,
	MEM_STORE8, MEM_STORE16, MEM_STORE32, MEM_STORE32F, MEM_STORE64,
	MEM_KINDS
};
#define MEM_KIND_F(kind) ((kind) == MEM_LOAD32F || (kind) == MEM_STORE32F || (kind) == MEM_LOAD64 || (kind) == MEM_STORE64)

/* What a fast access that faulted is written over with a call to
 * (ngen_Rewrite()): a routine that does the access by asking. The access
 * has the address as the SH4 gave it in eax (ecx has it taken down to the
 * 512 MB), and its data in a general register (by number: eax for what is
 * loaded, edx or one of the block's for what is stored) or in an xmm
 * register (one of the block's, or xmm0); there is a routine for each kind
 * and each register. They keep the block's xmm registers, which the
 * function they call need not. */
static u8 *mem_stubs[MEM_KINDS][8];

static u32 DYNACALL mem_load8(u32 addr)  { return (s32)(s8)ReadMem8(addr); }
static u32 DYNACALL mem_load16(u32 addr) { return (s32)(s16)ReadMem16(addr); }
static u32 DYNACALL mem_load32(u32 addr) { return ReadMem32(addr); }
static u64 DYNACALL mem_load64(u32 addr) { return ReadMem64(addr); }
static void DYNACALL mem_store8(u32 addr, u32 data)  { WriteMem8(addr, (u8)data); }
static void DYNACALL mem_store16(u32 addr, u32 data) { WriteMem16(addr, (u16)data); }
static void DYNACALL mem_store32(u32 addr, u32 data) { WriteMem32(addr, data); }
// (64 bits from where @data points: they do not fit in the two registers)
static void DYNACALL mem_store64(u32 addr, const u32 *data)
{
	WriteMem64(addr, ((u64)data[1] << 32) | data[0]);
}

static void mem_stub_make(int kind, int data)
{
	static const void *const routines[MEM_KINDS] = {
		(const void *)&mem_load8, (const void *)&mem_load16, (const void *)&mem_load32, (const void *)&mem_load32,
		(const void *)&mem_load64,
		(const void *)&mem_store8, (const void *)&mem_store16, (const void *)&mem_store32, (const void *)&mem_store32,
		(const void *)&mem_store64,
	};
	const bool load = kind < MEM_STORE8;
	Xbyak::CodeGenerator stub(128, emit_GetCCPtr());

	mem_stubs[kind][data] = (u8 *)stub.getCode();
	/* Called from a block, whose stack is a multiple of 16 at a call: 12
	 * here, and 28 more make it one again, with room for the block's four
	 * xmm registers and 8 bytes of data. */
	stub.sub(esp, 28);
	for (int i = 0; i < 4; i++)
		stub.movd(stub.dword[esp + i * 4], Xbyak::Xmm(4 + i));
	// what is left of the time slice, for the timers (see cycle_counter in sh4_if.h)
	stub.mov(ecx, stub.dword[&cycle_counter]);
	stub.mov(stub.dword[&Sh4cntx.cycle_counter], ecx);
	stub.mov(ecx, eax);
	if (kind == MEM_STORE64)
	{
		stub.movq(stub.qword[esp + 16], xmm0);
		stub.lea(edx, stub.ptr[esp + 16]);
	}
	else if (kind == MEM_STORE32F)
		stub.movd(edx, Xbyak::Xmm(data));
	else if (!load && data != Xbyak::Operand::EDX)
		stub.mov(edx, Xbyak::Reg32(data));
	stub.call(routines[kind]);
	for (int i = 0; i < 4; i++)
		stub.movd(Xbyak::Xmm(4 + i), stub.dword[esp + i * 4]);
	if (kind == MEM_LOAD64)
	{
		stub.movd(xmm0, eax);
		stub.movd(xmm1, edx);
		stub.punpckldq(xmm0, xmm1);
	}
	else if (kind == MEM_LOAD32F)
		stub.movd(Xbyak::Xmm(data), eax);
	else if (load && data != Xbyak::Operand::EAX)
		stub.mov(Xbyak::Reg32(data), eax);
	stub.add(esp, 28);
	stub.ret();
	stub.ready();
	emit_Skip((stub.getSize() + 15) & ~15u);
}

static void link_site_call_stub(u8 *site)
{
	const s32 rel = (s32)(link_stub - (site + 5));

	site[0] = 0xE8;		// call rel32
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
}

static void link_site_jump(u8 *site, const u8 *code)
{
	const s32 rel = (s32)(code - (site + 5));

	site[0] = 0xE9;		// jmp rel32
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
}

u32 DynaRBI::Relink()
{
	for (int i = 0; i < 2; i++)
	{
		const RuntimeBlockInfo *to = i == 0 ? pBranchBlock : pNextBlock;

		if (link_at[i] == 0)
			continue;
		if (to != NULL)
			link_site_jump((u8 *)code + link_at[i], (const u8 *)to->code + ((const DynaRBI *)to)->linked_entry);
		else
			link_site_call_stub((u8 *)code + link_at[i]);
	}
	return 0;
}

/* From the link stub: the block whose link site returns to @after is
 * leaving for @pc for the first time. As on x86-64 (ngen_link() there). */
extern "C" __attribute__((used)) void * DYNACALL ngen_link(u8 *after, u32 pc)
{
	u8 *const site = after - 5;
	RuntimeBlockInfoPtr from = bm_GetBlock2(site);
	const bool mmu = mmu_enabled();
	u32 addr = pc;
	DynarecCodeEntryPtr code;

	if (mmu)
	{
		if (from == NULL || !rdv_MmuSamePlace(from.get(), pc, &addr))
		{
			next_pc = pc;
			return (void *)block_return;
		}
		code = (DynarecCodeEntryPtr)p_sh4rcb->fpcb[(addr >> 1) & FPCB_MASK];
	}
	else
		code = bm_GetCodeByVAddr(pc);
	if (code == ngen_FailedToFindBlock)
	{
		code = rdv_FailedToFindBlock(pc);
		if (mmu && next_pc != pc)
			return (void *)block_return;
	}
	if (code == ngen_FailedToFindBlock)
	{
		next_pc = pc;
		return (void *)block_return;
	}

	if (from == NULL || bm_GetBlock2(site) != from)
		return (void *)code;

	DynaRBI *const block = (DynaRBI *)from.get();
	const u32 at = (u32)(site - (u8 *)block->code);
	const int which = at == block->link_at[0] ? 0 : at == block->link_at[1] ? 1 : -1;
	RuntimeBlockInfoPtr to = bm_GetBlock(addr);

	if (which < 0 || pc != (which == 0 ? block->BranchBlock : block->NextBlock))
		return (void *)code;
	if (to == NULL || to->addr != addr || (DynarecCodeEntryPtr)to->code != code)
		return (void *)code;
	if (mmu && to->vaddr != pc)
		return (void *)code;

	if (to->temp_block)
	{
		// Not linked, ever: through the table from now on. jmp dword [address]
		const u32 entry = (u32)(uintptr_t)&p_sh4rcb->fpcb[(addr >> 1) & FPCB_MASK];

		site[0] = 0xFF;
		site[1] = 0x25;
		memcpy(site + 2, &entry, 4);
		block->link_at[which] = 0;
		return (void *)code;
	}

	if (which == 0)
		block->pBranchBlock = to.get();
	else
		block->pNextBlock = to.get();
	to->AddRef(from);
	block->Relink();
	return (void *)code;
}

/* The main loop, as on x86-64 (ngen_mainloop there), and the routines that
 * go with it. Blocks are jumped to with the stack a multiple of 16, which
 * is what a call wants on Linux (and does no harm on Windows, which only
 * asks for 4), and with the address they are for in edx. */
static void gen_mainloop()
{
	Xbyak::CodeGenerator c(1024, emit_GetCCPtr());
	Xbyak::Label run_loop, slice_loop, ret, update, end;

	mainloop_code = (u8 *)c.getCode();
	c.push(ebp);
	c.push(ebx);
	c.push(esi);
	c.push(edi);
	// (the caller's stack pointer, under the place it is aligned to)
	c.mov(eax, esp);
	c.and_(esp, ~15);
	c.sub(esp, 12);
	c.push(eax);
	c.mov(c.dword[&cycle_counter], SH4_TIMESLICE);
	c.mov(c.dword[&jmp_esp], esp);

	c.L(run_loop);
	c.cmp(c.dword[&Sh4cntx.CpuRunning], 0);
	c.je(end, Xbyak::CodeGenerator::T_NEAR);

	c.L(slice_loop);
	c.mov(ecx, c.dword[&next_pc]);
	c.call((const void *)&bm_GetCodeByVAddr);
	c.mov(edx, c.dword[&next_pc]);
	c.jmp(eax);

	c.L(ret);
	c.cmp(c.dword[&cycle_counter], 0);
	c.jg(slice_loop);

	c.add(c.dword[&cycle_counter], SH4_TIMESLICE);
	// UpdateSystem_INTC(), here, when no event is due and no interrupt waits
	c.mov(edx, c.dword[&Sh4cntx.sh4_sched_next]);
	c.sub(edx, SH4_TIMESLICE);
	c.js(update);
	c.cmp(c.dword[&Sh4cntx.interrupt_pend], 0);
	c.jne(update);
	c.mov(c.dword[&Sh4cntx.sh4_sched_next], edx);
	c.jmp(run_loop);
	c.L(update);
	// (the timers' time is the end of the slice here: see cycle_counter in sh4_if.h)
	c.mov(c.dword[&Sh4cntx.cycle_counter], SH4_TIMESLICE);
	c.call((const void *)&UpdateSystem_INTC);
	c.jmp(run_loop);

	c.L(end);
	c.pop(esp);
	c.pop(edi);
	c.pop(esi);
	c.pop(ebx);
	c.pop(ebp);
	c.ret();

	// back from an exception: what the stack was as blocks run, and on
	c.align(16);
	unwind_stub = (u8 *)c.getCurr();
	c.mov(esp, c.dword[&jmp_esp]);
	c.jmp(run_loop);

	/* What the table of blocks holds for an address with no block yet: it
	 * is jumped to as a block is, with the address in edx, has the block
	 * compiled and goes back to the main loop, which finds it there. */
	c.align(16);
	compile_stub = (u8 *)c.getCurr();
	c.mov(ecx, edx);
	c.call((const void *)&rdv_FailedToFindBlock);
	c.jmp(ret);

	/* The link stub, called from a link site with the address the block is
	 * leaving for in edx: the site's return address and that address are
	 * ngen_link()'s arguments, and what it returns is jumped to. */
	c.align(16);
	link_stub = (u8 *)c.getCurr();
	c.pop(ecx);
	c.call((const void *)&ngen_link);
	c.jmp(eax);

	c.ready();
	block_return = (u8 *)ret.getAddress();
	emit_Skip((c.getSize() + 15) & ~15u);
}

void ngen_init()
{
	gen_mainloop();
	ngen_FailedToFindBlock = (void (*)())compile_stub;
	// the routines for the general registers there can be, and the xmm ones
	static const int gregs[] = { Xbyak::Operand::EAX, Xbyak::Operand::EDX, Xbyak::Operand::EBX, Xbyak::Operand::EBP,
			Xbyak::Operand::ESI, Xbyak::Operand::EDI };
	static const int fregs[] = { 0, 4, 5, 6, 7 };
	for (int kind = 0; kind < MEM_KINDS; kind++)
	{
		if (kind == MEM_LOAD64 || kind == MEM_STORE64)
			mem_stub_make(kind, 0);
		else if (MEM_KIND_F(kind))
			for (int data : fregs)
				mem_stub_make(kind, data);
		else
			for (int data : gregs)
			{
				// (what is loaded is in eax or one of the block's, a byte stored in edx or ebx)
				if (kind < MEM_STORE8 && data == Xbyak::Operand::EDX)
					continue;
				if (kind >= MEM_STORE8 && data == Xbyak::Operand::EAX)
					continue;
				if (kind == MEM_STORE8 && data != Xbyak::Operand::EDX && data != Xbyak::Operand::EBX)
					continue;
				mem_stub_make(kind, data);
			}
	}
	emit_SetBaseAddr();
}

void ngen_ResetBlocks()
{
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback = false;
	dst->OnlyDynamicEnds = false;
}

RuntimeBlockInfo* ngen_AllocateBlock(void)
{
	return new DynaRBI();
}

static void DYNACALL ngen_blockcheckfail(u32 pc)
{
	rdv_BlockCheckFail(pc);
}

static void handle_mem_exception(u32 pc)
{
	if (pc & 1)
		// Delay slot
		spc = pc - 1;
	else
		spc = pc;
	cycle_counter += CPU_RATIO * 2;	// probably more is needed but no easy way to find out
	rec_unwind();
}

/* The MMU's loads and stores: the address in ecx and what is stored in edx,
 * as for the others, and the pc of the instruction here, which a block
 * stores before the call when the MMU is on. */
static u32 noex_pc;

template<typename T>
static T DYNACALL ReadMemNoEx(u32 addr, u32 pc)
{
#ifndef NO_MMU
	// (two accesses, each translated: mmu.h)
	if (sizeof(T) == 8 && mmu_in_two_pages(addr))
	{
		const u32 low = ReadMemNoEx<u32>(addr, pc);
		return (T)(((u64)ReadMemNoEx<u32>(addr + 4, pc) << 32) | low);
	}

	u32 paddr;
	u32 rv = mmu_data_translation<MMU_TT_DREAD, T>(addr, paddr);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DREAD);
		handle_mem_exception(pc);
	}
	mmu_lut_fill(addr, paddr, false);

	return _vmem_readt<T, T>(paddr);
#else
	return (T)0;
#endif
}

template<typename T>
static void WriteMemNoExT(u32 addr, T data, u32 pc)
{
#ifndef NO_MMU
	if (sizeof(T) == 8 && mmu_in_two_pages(addr))
	{
		WriteMemNoExT<u32>(addr, (u32)data, pc);
		WriteMemNoExT<u32>(addr + 4, (u32)((u64)data >> 32), pc);
		return;
	}

	u32 paddr;
	u32 rv = mmu_data_translation<MMU_TT_DWRITE, T>(addr, paddr);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DWRITE);
		handle_mem_exception(pc);
	}
	mmu_lut_fill(addr, paddr, true);
	_vmem_writet<T>(paddr, data);
#endif
}

template<typename T>
static void DYNACALL WriteMemNoEx(u32 addr, u32 data)
{
	WriteMemNoExT<T>(addr, (T)data, noex_pc);
}

static void DYNACALL WriteMemNoEx64(u32 addr, const u32 *data)
{
	WriteMemNoExT<u64>(addr, ((u64)data[1] << 32) | data[0], noex_pc);
}

[[noreturn]] static void handle_sh4_exception(SH4ThrownException ex, u32 pc)
{
	if (pc & 1)
	{
		// Delay slot
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex.expEvn, ex.callVect);
	cycle_counter += CPU_RATIO * 4;	// probably more is needed
	rec_unwind();
}

/* An instruction the interpreter runs, with the MMU on: what raises an
 * exception is caught, and the catch over, before the main loop is gone
 * back to. The handler is set here before the call. */
static OpCallFP *fallback_oph;

static void DYNACALL interpreter_fallback(u32 op, u32 pc)
{
	SH4ThrownException caught;
	try {
		fallback_oph(op);
		return;
	} catch (SH4ThrownException& ex) {
		caught = ex;
	}
	handle_sh4_exception(caught, pc);
}

static void DYNACALL do_sqw_mmu_no_ex(u32 addr, u32 pc)
{
	SH4ThrownException caught;
	try {
		do_sqw_mmu(addr);
		return;
	} catch (SH4ThrownException& ex) {
		caught = ex;
	}
	handle_sh4_exception(caught, pc);
}

static void DYNACALL do_sqw_nommu_local(u32 addr, u8* sqb)
{
	do_sqw_nommu(addr, sqb);
}

// Whether every argument of a fastcall function goes in a register
template<typename... P> struct InRegisters;
template<> struct InRegisters<> { static constexpr bool value = true; };
template<typename A, typename... P> struct InRegisters<A, P...>
{
	static constexpr bool value = sizeof(A) <= 4 && sizeof...(P) <= 1 && InRegisters<P...>::value;
};

class BlockCompiler : public Xbyak::CodeGenerator
{
public:
	BlockCompiler() : BlockCompiler((u8 *)emit_GetCCPtr()) {}

	BlockCompiler(u8 *code_ptr) : Xbyak::CodeGenerator(emit_FreeSpace(), code_ptr), regalloc(this)
	{
	}

	void compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
	{
		current_opid = -1;
		if (force_checks)
			CheckBlock(block);
		else if (mmu_enabled())
		{
			// Is this the address the block was compiled for? See x86-64.
			cmp(Ctx(&next_pc), block->vaddr);
			jne(vaddr_failed, T_NEAR);
			vaddr_checked = true;
			((DynaRBI *)block)->linked_entry = (u32)getSize();
		}

		if (mmu_enabled() && block->has_fpu_op)
		{
			Xbyak::Label fpu_enabled;
			test(dword[&sr], 0x8000);			// test SR.FD bit
			jz(fpu_enabled, T_NEAR);
			GenCallC(Do_Exception, { Arg::I(block->vaddr), Arg::I(0x800), Arg::I(0x100) });
			jmp(exit_block, T_NEAR);
			L(fpu_enabled);
		}
		block_cycles = block->guest_cycles;
		charge_at_tail = !mmu_enabled();
		if (!charge_at_tail)
			GenCharge();
		if (settings.dynarec.AccurateTiming && !mmu_enabled())
		{
			// a block that can change something other than a register says so: see wait_site.h
			if (sh4_block_writes(block))
				inc(dword[&sh4_write_gen]);
		}
		regalloc.DoAlloc(block);

		for (current_opid = 0; current_opid < block->oplist.size(); current_opid++)
		{
			shil_opcode& op  = block->oplist[current_opid];

			regalloc.OpBegin(&op, current_opid);

			switch (op.op)
			{
			case shop_ifb:
				GenSliceLeft();
				if (op.rs1._imm)
					mov(Ctx(&next_pc), op.rs2._imm);
				mov(ecx, op.rs3._imm);
				if (!mmu_enabled())
					GenCall(OpDesc[op.rs3._imm]->oph);
				else
				{
					mov(dword[&fallback_oph], (u32)(uintptr_t)OpDesc[op.rs3._imm]->oph);
					mov(edx, block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
					GenCall(interpreter_fallback);
				}
				break;

			case shop_jcond:
			case shop_jdyn:
				{
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					if (op.rs1.is_imm())
					{
						if (op.rs2.is_imm())
							mov(rd, op.rs1._imm + op.rs2._imm);
						else
						{
							mov(rd, op.rs1._imm);
							verify(op.rs2.is_null());
						}
					}
					else
					{
						Xbyak::Reg32 rs1 = regalloc.MapRegister(op.rs1);
						if (rd != rs1)
							mov(rd, rs1);
						if (op.rs2.is_imm())
							add(rd, op.rs2._imm);
					}
				}
				break;

			case shop_mov32:
				verify(op.rd.is_reg());
				verify(op.rs1.is_reg() || op.rs1.is_imm());

				if (regalloc.IsAllocf(op.rd))
					shil_param_to_host_reg(op.rs1, regalloc.MapXRegister(op.rd));
				else
					shil_param_to_host_reg(op.rs1, regalloc.MapRegister(op.rd));
				break;

			case shop_mov64:
				verify(op.rd.is_r64());
				verify(op.rs1.is_r64());
				// (pairs are never in registers here: x86_regalloc.h)
				movq(xmm0, qword[op.rs1.reg_ptr()]);
				movq(qword[op.rd.reg_ptr()], xmm0);
				break;

			case shop_readm:
				if (!GenReadMemImmediate(op, block) && (!optimise || !GenReadMemoryFast(op)))
				{
					GenMemAddr(op, ecx);
					GenReadMemorySlow(op, block);

					const u32 size = op.flags & 0x7f;
					if (size != 8)
						host_reg_to_shil_param(op.rd, eax);
					else
					{
						mov(Ctx(op.rd.reg_ptr()), eax);
						mov(Ctx(op.rd.reg_ptr() + 1), edx);
					}
				}
				break;

			case shop_writem:
				if (!GenWriteMemImmediate(op, block) && (!optimise || !GenWriteMemoryFast(op)))
				{
					GenMemAddr(op, ecx);
					const u32 size = op.flags & 0x7f;
					if (size != 8)
						shil_param_to_host_reg(op.rs2, edx);
					else
						mov(edx, (u32)(uintptr_t)op.rs2.reg_ptr());
					GenWriteMemorySlow(op, block);
				}
				break;

#ifndef CANONICAL_TEST
			case shop_sync_sr:
				GenCallC(UpdateSR);
				break;
			case shop_sync_fpscr:
				GenCallC(UpdateFPSCR);
				break;

			case shop_swaplb:
				if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
					mov(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				ror(regalloc.MapRegister(op.rd).cvt16(), 8);
				break;

			case shop_neg:
				if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
					mov(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				neg(regalloc.MapRegister(op.rd));
				break;
			case shop_not:
				if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
					mov(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				not_(regalloc.MapRegister(op.rd));
				break;

			case shop_and:
				GenBinaryOp(op, &BlockCompiler::and_);
				break;
			case shop_or:
				GenBinaryOp(op, &BlockCompiler::or_);
				break;
			case shop_xor:
				GenBinaryOp(op, &BlockCompiler::xor_);
				break;
			case shop_add:
				GenBinaryOp(op, &BlockCompiler::add);
				break;
			case shop_sub:
				GenBinaryOp(op, &BlockCompiler::sub);
				break;

#define SHIFT_OP(natop) \
		if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))	\
			mov(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));	\
		if (op.rs2.is_imm())	\
			natop(regalloc.MapRegister(op.rd), op.rs2._imm);	\
		else  \
			die("Unsupported operand");
			case shop_shl:
				SHIFT_OP(shl)
				break;
			case shop_shr:
				SHIFT_OP(shr)
				break;
			case shop_sar:
				SHIFT_OP(sar)
				break;
			case shop_ror:
				SHIFT_OP(ror)
				break;

			case shop_adc:
				{
					cmp(regalloc.MapRegister(op.rs3), 1);	// C = ~rs3
					Xbyak::Reg32 rs2;
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					if (op.rs2.is_reg())
					{
						rs2 = regalloc.MapRegister(op.rs2);
						if (regalloc.mapg(op.rd) == regalloc.mapg(op.rs2))
						{
							mov(ecx, rs2);
							rs2 = ecx;
						}
					}
					if (op.rs1.is_imm())
						mov(rd, op.rs1.imm_value());
					else if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
						mov(rd, regalloc.MapRegister(op.rs1));
					cmc();		// C = rs3
					if (op.rs2.is_reg())
						adc(rd, rs2); 							// (C,rd)=rs1+rs2+rs3(C)
					else
						adc(rd, op.rs2.imm_value());
					setc(al);
					movzx(regalloc.MapRegister(op.rd2), al);	// rd2 = C
				}
				break;

			case shop_negc:
				{
					/* rd = -(rs1 + rs2), and rd2 the borrow out of the 64-bit
					 * subtraction x86-64 does: 1 unless both are 0. */
					if (op.rs1.is_imm())
						mov(ecx, op.rs1.imm_value());
					else
						mov(ecx, regalloc.MapRegister(op.rs1));
					mov(eax, ecx);
					if (op.rs2.is_imm())
						add(ecx, op.rs2.imm_value());
					else
						add(ecx, regalloc.MapRegister(op.rs2));
					neg(ecx);
					if (op.rs2.is_imm())
						or_(eax, op.rs2.imm_value());
					else
						or_(eax, regalloc.MapRegister(op.rs2));
					setnz(al);
					movzx(eax, al);
					mov(regalloc.MapRegister(op.rd), ecx);
					mov(regalloc.MapRegister(op.rd2), eax);
				}
				break;

			case shop_rocr:
			case shop_rocl:
				{
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					cmp(regalloc.MapRegister(op.rs2), 1);	// C = ~rs2
					if (op.rs1.is_imm())
						mov(rd, op.rs1.imm_value());
					else if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
						mov(rd, regalloc.MapRegister(op.rs1));
					cmc();		// C = rs2
					if (op.op == shop_rocr)
						rcr(rd, 1);
					else
						rcl(rd, 1);
					setc(al);
					movzx(regalloc.MapRegister(op.rd2), al);	// rd2 = C
				}
				break;

			case shop_shld:
			case shop_shad:
				{
					if (op.rs2.is_reg())
						mov(ecx, regalloc.MapRegister(op.rs2));
					else
						// This shouldn't happen. If arg is imm -> shop_shl/shr/sar
						mov(ecx, op.rs2.imm_value());
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					if (op.rs1.is_imm())
						mov(rd, op.rs1.imm_value());
					else if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
						mov(rd, regalloc.MapRegister(op.rs1));
					Xbyak::Label negative_shift;
					Xbyak::Label non_zero;
					Xbyak::Label exit;

					cmp(ecx, 0);
					js(negative_shift);
					shl(rd, cl);
					jmp(exit);

					L(negative_shift);
					test(ecx, 0x1f);
					jnz(non_zero);
					if (op.op == shop_shld)
						xor_(rd, rd);
					else
						sar(rd, 31);
					jmp(exit);

					L(non_zero);
					neg(ecx);
					if (op.op == shop_shld)
						shr(rd, cl);
					else
						sar(rd, cl);
					L(exit);
				}
				break;

			case shop_test:
			case shop_seteq:
			case shop_setge:
			case shop_setgt:
			case shop_setae:
			case shop_setab:
				{
					if (op.op == shop_test)
					{
						if (op.rs2.is_imm())
							test(regalloc.MapRegister(op.rs1), op.rs2._imm);
						else
							test(regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
					}
					else
					{
						if (op.rs2.is_imm())
							cmp(regalloc.MapRegister(op.rs1), op.rs2._imm);
						else
							cmp(regalloc.MapRegister(op.rs1), regalloc.MapRegister(op.rs2));
					}
					switch (op.op)
					{
					case shop_test:
					case shop_seteq:
						sete(al);
						break;
					case shop_setge:
						setge(al);
						break;
					case shop_setgt:
						setg(al);
						break;
					case shop_setae:
						setae(al);
						break;
					case shop_setab:
						seta(al);
						break;
					default:
						die("invalid case");
						break;
					}
					movzx(regalloc.MapRegister(op.rd), al);
				}
				break;

			case shop_setpeq:
				{
					Xbyak::Label end;
					mov(ecx, regalloc.MapRegister(op.rs1));
					if (op.rs2.is_r32i())
						xor_(ecx, regalloc.MapRegister(op.rs2));
					else
						xor_(ecx, op.rs2._imm);

					mov(eax, 1);
					test(ecx, 0xFF000000);
					je(end);
					test(ecx, 0x00FF0000);
					je(end);
					test(ecx, 0x0000FF00);
					je(end);
					xor_(eax, eax);
					test(cl, cl);
					sete(al);
					L(end);
					mov(regalloc.MapRegister(op.rd), eax);
				}
				break;

			case shop_mul_u16:
				movzx(eax, regalloc.MapRegister(op.rs1).cvt16());
				if (op.rs2.is_reg())
					movzx(ecx, regalloc.MapRegister(op.rs2).cvt16());
				else
					mov(ecx, op.rs2._imm & 0xFFFF);
				mul(ecx);
				mov(regalloc.MapRegister(op.rd), eax);
				break;
			case shop_mul_s16:
				movsx(eax, regalloc.MapRegister(op.rs1).cvt16());
				if (op.rs2.is_reg())
					movsx(ecx, regalloc.MapRegister(op.rs2).cvt16());
				else
					mov(ecx, (s32)(s16)op.rs2._imm);
				mul(ecx);
				mov(regalloc.MapRegister(op.rd), eax);
				break;
			case shop_mul_i32:
				mov(eax, regalloc.MapRegister(op.rs1));
				if (op.rs2.is_reg())
					mul(regalloc.MapRegister(op.rs2));
				else
				{
					mov(ecx, op.rs2._imm);
					mul(ecx);
				}
				mov(regalloc.MapRegister(op.rd), eax);
				break;
			case shop_mul_u64:
			case shop_mul_s64:
				mov(eax, regalloc.MapRegister(op.rs1));
				if (op.rs2.is_reg())
					mov(ecx, regalloc.MapRegister(op.rs2));
				else
					mov(ecx, op.rs2._imm);
				if (op.op == shop_mul_u64)
					mul(ecx);
				else
					imul(ecx);
				mov(regalloc.MapRegister(op.rd), eax);
				mov(regalloc.MapRegister(op.rd2), edx);
				break;

			case shop_pref:
				if (op.rs1.is_imm())
				{
					// this test shouldn't be necessary
					if ((op.rs1._imm & 0xFC000000) == 0xE0000000)
					{
						mov(ecx, op.rs1._imm);
						GenStoreQueue(op, block);
					}
				}
				else
				{
					Xbyak::Reg32 rn;
					if (regalloc.IsAllocg(op.rs1))
						rn = regalloc.MapRegister(op.rs1);
					else
					{
						mov(eax, Ctx(op.rs1.reg_ptr()));
						rn = eax;
					}
					mov(ecx, rn);
					shr(ecx, 26);
					cmp(ecx, 0x38);
					Xbyak::Label no_sqw;
					jne(no_sqw, T_NEAR);

					mov(ecx, rn);
					GenStoreQueue(op, block);
					L(no_sqw);
				}
				break;

			case shop_ext_s8:
				mov(eax, regalloc.MapRegister(op.rs1));
				movsx(regalloc.MapRegister(op.rd), al);
				break;
			case shop_ext_s16:
				movsx(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1).cvt16());
				break;

			case shop_xtrct:
				{
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					Xbyak::Reg32 rs1 = ecx;
					if (op.rs1.is_reg())
						rs1 = regalloc.MapRegister(op.rs1);
					else
						mov(rs1, op.rs1.imm_value());
					Xbyak::Reg32 rs2 = eax;
					if (op.rs2.is_reg())
						rs2 = regalloc.MapRegister(op.rs2);
					else
						mov(rs2, op.rs2.imm_value());
					if (rd == rs2)
					{
						shl(rd, 16);
						mov(eax, rs1);
						shr(eax, 16);
						or_(rd, eax);
						break;
					}
					else if (rd != rs1)
					{
						mov(rd, rs1);
					}
					shr(rd, 16);
					mov(eax, rs2);
					shl(eax, 16);
					or_(rd, eax);
				}
				break;

			//
			// FPU
			//

			case shop_fadd:
				GenBinaryFOp(op, &BlockCompiler::addss);
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;
			case shop_fsub:
				GenBinaryFOp(op, &BlockCompiler::subss);
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;
			case shop_fmul:
				GenBinaryFOp(op, &BlockCompiler::mulss);
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;
			case shop_fdiv:
				GenBinaryFOp(op, &BlockCompiler::divss);
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;

			case shop_fabs:
				movd(eax, regalloc.MapXRegister(op.rs1));
				and_(eax, 0x7FFFFFFF);
				movd(regalloc.MapXRegister(op.rd), eax);
				break;
			case shop_fneg:
				movd(eax, regalloc.MapXRegister(op.rs1));
				xor_(eax, 0x80000000);
				movd(regalloc.MapXRegister(op.rd), eax);
				break;

			case shop_fsqrt:
				sqrtss(regalloc.MapXRegister(op.rd), regalloc.MapXRegister(op.rs1));
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;

			case shop_fmac:
				// One rounding, of the sum, as everywhere else: see x86-64
				if (!cpu.has(Xbyak::util::Cpu::tFMA))
				{
					shil_chf[op.op](&op);
					break;
				}
				{
					Xbyak::Xmm rs1 = regalloc.MapXRegister(op.rs1);
					Xbyak::Xmm rs2 = regalloc.MapXRegister(op.rs2);
					Xbyak::Xmm rs3 = regalloc.MapXRegister(op.rs3);
					Xbyak::Xmm rd = regalloc.MapXRegister(op.rd);
					if (rd == rs2)
					{
						movss(xmm1, rs2);
						rs2 = xmm1;
					}
					if (rd == rs3)
					{
						movss(xmm2, rs3);
						rs3 = xmm2;
					}
					if (op.rs1.is_imm())
					{
						mov(eax, op.rs1._imm);
						movd(rd, eax);
					}
					else if (rd != rs1)
					{
						movss(rd, rs1);
					}
					vfmadd231ss(rd, rs2, rs3);
					GenNaNFix(rd);
				}
				break;

			case shop_fsrra:
				// RSQRTSS has an |error| <= 1.5*2^-12 where the SH4 FSRRA needs |error| <= 2^-21
				sqrtss(xmm0, regalloc.MapXRegister(op.rs1));
				mov(eax, 0x3f800000);	// 1.0
				movd(regalloc.MapXRegister(op.rd), eax);
				divss(regalloc.MapXRegister(op.rd), xmm0);
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;

			case shop_fsetgt:
			case shop_fseteq:
				ucomiss(regalloc.MapXRegister(op.rs1), regalloc.MapXRegister(op.rs2));
				if (op.op == shop_fsetgt)
				{
					seta(al);
				}
				else
				{
					//special case
					//We want to take in account the 'unordered' case on the fpu
					lahf();
					test(ah, 0x44);
					setnp(al);
				}
				movzx(regalloc.MapRegister(op.rd), al);
				break;

			case shop_fsca:
				if (op.rs1.is_imm())
					mov(eax, op.rs1._imm & 0xFFFF);
				else
					movzx(eax, regalloc.MapRegister(op.rs1).cvt16());
				// (a pair, which is never in registers here)
				movq(xmm0, qword[eax * 8 + (u32)(uintptr_t)&sin_table]);
				movq(qword[op.rd.reg_ptr()], xmm0);
				break;

			case shop_fipr:
				mov(eax, (u32)(uintptr_t)op.rs1.reg_ptr());
				mov(ecx, (u32)(uintptr_t)op.rs2.reg_ptr());
				x86_emit_fipr(*this, regalloc.MapXRegister(op.rd));
				GenNaNFix(regalloc.MapXRegister(op.rd));
				break;

			case shop_ftrv:
				mov(eax, (u32)(uintptr_t)op.rs1.reg_ptr());		// the vector
				mov(ecx, (u32)(uintptr_t)op.rs2.reg_ptr());		// the matrix
				x86_emit_ftrv(*this);
				GenNaNFix4();
				movups(xword[op.rd.reg_ptr()], xmm0);
				break;

			case shop_frswap:
				mov(eax, (u32)(uintptr_t)op.rs1.reg_ptr());
				mov(ecx, (u32)(uintptr_t)op.rd.reg_ptr());
				for (int i = 0; i < 4; i++)
				{
					movaps(xmm0, xword[eax + (i * 16)]);
					movaps(xmm1, xword[ecx + (i * 16)]);
					movaps(xword[eax + (i * 16)], xmm1);
					movaps(xword[ecx + (i * 16)], xmm0);
				}
				break;

			case shop_cvt_f2i_t:
				{
					// As on x86-64: see there
					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					Xbyak::Label done;

					cvttss2si(edx, regalloc.MapXRegister(op.rs1));
					mov(rd, 0x7fffffff);
					cmp(edx, 0x7fffff80);	// 2147483520.0f
					jg(done, T_SHORT);
					mov(rd, edx);
					cmp(rd, 0x80000000);	// indefinite integer
					jne(done, T_SHORT);
					xor_(eax, eax);
					pxor(xmm0, xmm0);
					ucomiss(regalloc.MapXRegister(op.rs1), xmm0);
					setb(al);
					add(eax, 0x7fffffff);
					mov(rd, eax);
					L(done);
				}
				break;
			case shop_cvt_i2f_n:
			case shop_cvt_i2f_z:
				cvtsi2ss(regalloc.MapXRegister(op.rd), regalloc.MapRegister(op.rs1));
				break;
#endif

			default:
				shil_chf[op.op](&op);
				break;
			}
			regalloc.OpEnd(&op);
		}
		regalloc.Cleanup();
		current_opid = -1;

		// How a block ends, as on x86-64: see there
		const bool go_on = !mmu_enabled();
		u32 place;
		const bool mmu_go_on = !go_on && block->mmu_go_on;
		const bool go_on_branch = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->BranchBlock, &place));
		const bool go_on_next = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->NextBlock, &place));
		// see wait_site.h
		const bool watch = go_on && settings.dynarec.AccurateTiming && block->BranchBlock <= block->vaddr;

		switch (block->BlockType) {

		case BET_StaticJump:
		case BET_StaticCall:
			if (go_on_branch)
			{
				if (watch)
					GenWaitCheck(block);
				GenGoOn(block, 0);
			}
			else
				mov(Ctx(&next_pc), block->BranchBlock);
			break;

		case BET_Cond_0:
		case BET_Cond_1:
			{
				cmp(block->has_jcond ? Ctx(&Sh4cntx.jdyn) : Ctx(&sr.T), block->BlockType & 1);
				Xbyak::Label branch_not_taken;

				jne(branch_not_taken, T_NEAR);
				if (go_on_branch)
				{
					if (watch)
						GenWaitCheck(block);
					GenGoOn(block, 0);
				}
				else
				{
					mov(Ctx(&next_pc), block->BranchBlock);
					jmp(exit_block, T_NEAR);
				}
				L(branch_not_taken);
				if (go_on_next)
					GenGoOn(block, 1);
				else
					mov(Ctx(&next_pc), block->NextBlock);
			}
			break;

		case BET_DynamicJump:
		case BET_DynamicCall:
		case BET_DynamicRet:
			mov(edx, Ctx(&Sh4cntx.jdyn));
			if (go_on)
			{
				// the address is only known now: its place in the table is worked out here
				GenSliceCheck();
				mov(eax, edx);
				shr(eax, 1);
				and_(eax, FPCB_MASK);
				jmp(dword[eax * 4 + (u32)(uintptr_t)&p_sh4rcb->fpcb[0]]);
			}
			else
				mov(Ctx(&next_pc), edx);
			break;

		case BET_DynamicIntr:
		case BET_StaticIntr:
			if (block->BlockType == BET_DynamicIntr)
			{
				mov(edx, Ctx(&Sh4cntx.jdyn));
				mov(Ctx(&next_pc), edx);
			}
			else
				mov(Ctx(&next_pc), block->NextBlock);

			if (charge_at_tail)
				GenCharge();
			GenCallC(UpdateINTC);
			break;

		default:
			die("Invalid block end type");
		}

		// Back to the main loop
		L(exit_block);
		jmp((const void *)block_return, T_NEAR);

		/* The time slice is used up where the block would have gone on to
		 * another: the address it was going to is in edx. */
		if (slice_out_used)
		{
			L(slice_out);
			mov(Ctx(&next_pc), edx);
			jmp(exit_block, T_NEAR);
		}
		/* The block's code is no longer what it was compiled from (ecx has
		 * its address from CheckBlock()) */
		if (force_checks)
		{
			L(check_failed);
			call((const void *)&ngen_blockcheckfail);
			jmp(exit_block, T_NEAR);
		}
		// Or it is not the block for this address
		if (vaddr_checked)
		{
			L(vaddr_failed);
			mov(ecx, block->addr);
			call((const void *)&ngen_blockcheckfail);
			jmp(exit_block, T_NEAR);
		}

		ready();

		block->code = (DynarecCodeEntryPtr)getCode();
		block->host_code_size = getSize();

		emit_Skip(getSize());
	}

	// To the main loop if the time slice is used up, by way of slice_out; otherwise on
	void GenSliceCheck()
	{
		if (charge_at_tail)
			GenCharge();		// leaves the flags of the counter against 0
		else
			cmp(dword[&cycle_counter], 0);
		jle(slice_out, T_NEAR);
		slice_out_used = true;
	}

	// Takes what the block costs off the cycle counter. (See x86-64.)
	void GenCharge()
	{
		sub(dword[&cycle_counter], block_cycles);
	}

	/* What is left of the time slice, where the timers find it, before a
	 * call that can reach an on-chip register: see cycle_counter in
	 * sh4_if.h. Uses eax. */
	void GenSliceLeft()
	{
		mov(eax, dword[&cycle_counter]);
		mov(Ctx(&Sh4cntx.cycle_counter), eax);
	}

	// A block going back: see wait_site.h
	void GenWaitCheck(const RuntimeBlockInfo *block)
	{
		WaitSite *site = sh4_wait_site(block);
		if (site == nullptr)
			return;
		Xbyak::Label over;

		dec(dword[&site->skip]);
		jns(over, T_NEAR);
		GenCallC(sh4_wait_check, { Arg::I((u32)(uintptr_t)site), Arg::M(&cycle_counter) });
		mov(dword[&cycle_counter], eax);
		L(over);
	}

	/* On to the block at @target: by a link site (see DynaRBI), which is
	 * the block's way out to its BranchBlock (@which 0) or its NextBlock
	 * (1). As on x86-64. */
	void GenGoOn(RuntimeBlockInfo *block, int which)
	{
		const u32 target = which == 0 ? block->BranchBlock : block->NextBlock;
		u32 place = target;

		mov(edx, target);
		if (mmu_enabled())
		{
			mov(Ctx(&next_pc), edx);
			rdv_MmuSamePlace(block, target, &place);
		}
		GenSliceCheck();
		if (block->temp_block || target == block->vaddr || link_stub == NULL)
		{
			jmp(dword[(u32)(uintptr_t)&p_sh4rcb->fpcb[(place >> 1) & FPCB_MASK]]);
			return;
		}
		((DynaRBI *)block)->link_at[which] = (u32)getSize();
		call((const void *)link_stub);
		db(0xCC);
	}

	/* With the MMU on an access is a call. First, though, the table of
	 * translations (mmu.h): on a hit, eax is what to add to the address to
	 * be at the page on the host. The address is in ecx. As on x86-64. */
	bool GenMmuLookup(const uintptr_t *table, u32 size, Xbyak::Label& miss)
	{
#ifdef MMU_HOST_PAGE_LUT
		if (!mmu_enabled())
			return false;
		if (size == 2 || size == 4 || size == 8)
		{
			test(ecx, size == 2 ? 1 : 3);
			jnz(miss, T_NEAR);
		}
		if (size == 8)
		{
			lea(eax, ptr[ecx + 4]);
			test(eax, 0xFFF);
			jz(miss, T_NEAR);
		}
		mov(eax, ecx);
		shr(eax, 12);
		mov(eax, dword[eax * 4 + (u32)(uintptr_t)table]);
		test(eax, eax);
		jz(miss, T_NEAR);
		return true;
#else
		return false;
#endif
	}

	/* A load by asking, the address in ecx: what is loaded comes back in
	 * eax, and 64 bits in edx and eax. */
	void GenReadMemorySlow(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		const u32 size = op.flags & 0x7f;
		Xbyak::Label lut_miss, lut_done;
#ifdef MMU_HOST_PAGE_LUT
		const bool lut = GenMmuLookup(mmu_read_lut, size, lut_miss);
#else
		const bool lut = false;
#endif
		if (lut)
		{
			switch (size) {
			case 1:
				movsx(eax, byte[eax + ecx]);
				break;
			case 2:
				movsx(eax, word[eax + ecx]);
				break;
			case 8:
				mov(edx, dword[eax + ecx + 4]);
				mov(eax, dword[eax + ecx]);
				break;
			default:
				mov(eax, dword[eax + ecx]);
				break;
			}
			jmp(lut_done, T_NEAR);
			L(lut_miss);
		}
		GenSliceLeft();
		if (mmu_enabled())
			mov(edx, block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

		switch (size) {
		case 1:
			if (!mmu_enabled())
				GenCall(ReadMem8);
			else
				GenCall(ReadMemNoEx<u8>);
			movsx(eax, al);
			break;
		case 2:
			if (!mmu_enabled())
				GenCall(ReadMem16);
			else
				GenCall(ReadMemNoEx<u16>);
			movsx(eax, ax);
			break;
		case 4:
			if (!mmu_enabled())
				GenCall(ReadMem32);
			else
				GenCall(ReadMemNoEx<u32>);
			break;
		case 8:
			if (!mmu_enabled())
				GenCall(ReadMem64);
			else
				GenCall(ReadMemNoEx<u64>);
			break;
		default:
			die("1..8 bytes");
		}
		if (lut)
			L(lut_done);
	}

	/* A store by asking, the address in ecx and the data in edx - or for 64
	 * bits, where they are. */
	void GenWriteMemorySlow(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		const u32 size = op.flags & 0x7f;
		Xbyak::Label lut_miss, lut_done;
#ifdef MMU_HOST_PAGE_LUT
		const bool lut = GenMmuLookup(mmu_write_lut, size, lut_miss);
#else
		const bool lut = false;
#endif
		if (lut)
		{
			switch (size) {
			case 1:
				mov(byte[eax + ecx], dl);
				break;
			case 2:
				mov(word[eax + ecx], dx);
				break;
			case 8:
				movq(xmm0, qword[edx]);
				movq(qword[eax + ecx], xmm0);
				break;
			default:
				mov(dword[eax + ecx], edx);
				break;
			}
			jmp(lut_done, T_NEAR);
			L(lut_miss);
		}
		GenSliceLeft();
		if (mmu_enabled())
			mov(dword[&noex_pc], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

		switch (size) {
		case 1:
			if (!mmu_enabled())
				GenCall(WriteMem8);
			else
				GenCall(WriteMemNoEx<u8>);
			break;
		case 2:
			if (!mmu_enabled())
				GenCall(WriteMem16);
			else
				GenCall(WriteMemNoEx<u16>);
			break;
		case 4:
			if (!mmu_enabled())
				GenCall(WriteMem32);
			else
				GenCall(WriteMemNoEx<u32>);
			break;
		case 8:
			if (!mmu_enabled())
				GenCall(mem_store64);
			else
				GenCall(WriteMemNoEx64);
			break;
		default:
			die("1..8 bytes");
		}
		if (lut)
			L(lut_done);
	}

	// A store queue written out, its address in ecx
	void GenStoreQueue(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (mmu_enabled())
		{
			mov(edx, block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
			GenCall(do_sqw_mmu_no_ex);
		}
		else if (CCN_MMUCR.AT == 1)
			GenCall(do_sqw_mmu);
		else
		{
			mov(edx, (u32)(uintptr_t)sq_both);
			GenCall(do_sqw_nommu_local);
		}
	}

	void ngen_CC_Start(const shil_opcode& op)
	{
		CC_pars.clear();
	}

	void ngen_CC_param(const shil_opcode& op, const shil_param& prm, CanonicalParamType tp)
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

		// from eax
		case CPT_u64rvL:
		case CPT_u32rv:
			host_reg_to_shil_param(prm, eax);
			break;

		// from edx, after the low half
		case CPT_u64rvH:
			host_reg_to_shil_param(prm, edx);
			break;

		// from the x87's top of stack, where a float is returned
		case CPT_f32rv:
			fstp(dword[&f32_rv]);
			movss(xmm0, dword[&f32_rv]);
			host_reg_to_shil_param(prm, xmm0);
			break;
		}
	}

	// A canonical implementation, which is a plain C function: everything on the stack
	void ngen_CC_Call(const shil_opcode& op, void* function)
	{
		Arg args[8];
		const int count = (int)CC_pars.size();

		verify(count <= 8);
		// (they came last argument first)
		for (int i = 0; i < count; i++)
		{
			const shil_param& prm = *CC_pars[count - 1 - i].prm;
			switch (CC_pars[count - 1 - i].type)
			{
			case CPT_ptr:
				verify(prm.is_reg());
				args[i] = Arg::I((u32)(uintptr_t)prm.reg_ptr());
				break;
			default:
				if (prm.is_imm())
					args[i] = Arg::I(prm._imm);
				else if (regalloc.IsAllocg(prm))
					args[i] = Arg::R(regalloc.MapRegister(prm));
				else if (regalloc.IsAllocf(prm))
					args[i] = Arg::X(regalloc.MapXRegister(prm));
				else
				{
					verify(prm.is_reg());
					args[i] = Arg::M(prm.reg_ptr());
				}
				break;
			}
		}
		GenCallRaw(function, args, count, false);
	}

	// Something in the SH4's context, at its address
	Xbyak::Address Ctx(const void *p)
	{
		const ptrdiff_t at = (const u8*)p - (const u8*)&p_sh4rcb->cntx;
		verify(at >= 0 && at < (ptrdiff_t)sizeof(Sh4Context));
		return dword[(u32)(uintptr_t)p];
	}

	void RegPreload(u32 reg, Xbyak::Operand::Code nreg)
	{
		mov(Xbyak::Reg32(nreg), Ctx(GetRegPtr(reg)));
	}
	void RegWriteback(u32 reg, Xbyak::Operand::Code nreg)
	{
		mov(Ctx(GetRegPtr(reg)), Xbyak::Reg32(nreg));
	}
	void RegPreload_FPU(u32 reg, s8 nreg)
	{
		movss(Xbyak::Xmm(nreg), Ctx(GetRegPtr(reg)));
	}
	void RegWriteback_FPU(u32 reg, s8 nreg)
	{
		movss(Ctx(GetRegPtr(reg)), Xbyak::Xmm(nreg));
	}

	/* Whether a load or store can be one move from or to the guest's memory
	 * as the host has it mapped: with the MMU off, and the 512 MB layout
	 * there, the address taken down to it (in P0 to P3 every area is where
	 * it is in P0: what is not memory there faults, and is rewritten). */
	static bool FastMemory()
	{
		return !mmu_enabled() && _nvmem_enabled() && !_nvmem_4gb_space();
	}

	/* Whether what is at @code is a fast access as GenFastAccess() writes
	 * them, and if so which kind, with which register, and how long it is:
	 * a move to or from [ecx + where the memory is], which nothing else a
	 * block has. */
	static bool FastAccessAt(const u8 *code, int *kind, int *data, int *length)
	{
		const u8 *p = code;
		u8 prefix = 0;

		if (*p == 0x66 || *p == 0xF3)
			prefix = *p++;
		if (p[0] == 0x0F)
		{
			if (p[1] == 0xBE && prefix == 0)
				*kind = MEM_LOAD8;
			else if (p[1] == 0xBF && prefix == 0)
				*kind = MEM_LOAD16;
			else if (p[1] == 0x10 && prefix == 0xF3)
				*kind = MEM_LOAD32F;
			else if (p[1] == 0x11 && prefix == 0xF3)
				*kind = MEM_STORE32F;
			else if (p[1] == 0x7E && prefix == 0xF3)
				*kind = MEM_LOAD64;
			else if (p[1] == 0xD6 && prefix == 0x66)
				*kind = MEM_STORE64;
			else
				return false;
			p += 2;
		}
		else if (*p == 0x8B && prefix == 0)
		{
			*kind = MEM_LOAD32;
			p++;
		}
		else if (*p == 0x88 && prefix == 0)
		{
			*kind = MEM_STORE8;
			p++;
		}
		else if (*p == 0x89 && prefix != 0xF3)
		{
			*kind = prefix != 0 ? MEM_STORE16 : MEM_STORE32;
			p++;
		}
		else
			return false;

		// [ecx + a distance]
		const u8 modrm = *p++;
		if ((modrm & 0xC7) != 0x81)
			return false;
		u32 distance;
		memcpy(&distance, p, 4);
		if (distance != (u32)(uintptr_t)virt_ram_base)
			return false;
		p += 4;

		*data = (modrm >> 3) & 7;
		if ((*kind == MEM_LOAD64 || *kind == MEM_STORE64) && *data != 0)
			return false;
		*length = (int)(p - code);
		return true;
	}

	/* A fast access of @kind, the SH4's address in eax: the data in, or
	 * wanted in, register @data - a general one, or an xmm one for the
	 * kinds that are. */
	void GenFastAccess(int kind, int data)
	{
		mov(ecx, eax);
		and_(ecx, 0x1FFFFFFF);
		const u8 *start_addr = getCurr();
		const Xbyak::RegExp at = ecx + (u32)(uintptr_t)virt_ram_base;

		switch (kind)
		{
		case MEM_LOAD8:    movsx(Xbyak::Reg32(data), byte[at]); break;
		case MEM_LOAD16:   movsx(Xbyak::Reg32(data), word[at]); break;
		case MEM_LOAD32:   mov(Xbyak::Reg32(data), dword[at]); break;
		case MEM_LOAD32F:  movss(Xbyak::Xmm(data), dword[at]); break;
		case MEM_LOAD64:   movq(Xbyak::Xmm(data), qword[at]); break;
		case MEM_STORE8:   mov(byte[at], Xbyak::Reg8(data)); break;
		case MEM_STORE16:  mov(word[at], Xbyak::Reg16(data)); break;
		case MEM_STORE32:  mov(dword[at], Xbyak::Reg32(data)); break;
		case MEM_STORE32F: movss(dword[at], Xbyak::Xmm(data)); break;
		case MEM_STORE64:  movq(qword[at], Xbyak::Xmm(data)); break;
		default:           die("no such access");
		}
		// ngen_Rewrite() has to know it again, and needs five bytes for its call
		int k, d, length;
		verify(FastAccessAt(start_addr, &k, &d, &length) && k == kind && d == data
				&& length == (int)(getCurr() - start_addr) && length >= 5 && mem_stubs[kind][data] != NULL);
	}

	// The address of a load or store, into @reg
	void GenMemAddr(const shil_opcode& op, const Xbyak::Reg32& reg)
	{
		if (!op.rs3.is_null() && op.rs1.is_reg() && regalloc.IsAllocg(op.rs1))
		{
			// one instruction for the two of them, where both are to hand
			const Xbyak::Reg32 base = regalloc.MapRegister(op.rs1);

			if (op.rs3.is_imm())
			{
				lea(reg, ptr[base + (int)(s32)op.rs3._imm]);
				return;
			}
			if (regalloc.IsAllocg(op.rs3))
			{
				lea(reg, ptr[base + regalloc.MapRegister(op.rs3)]);
				return;
			}
		}
		shil_param_to_host_reg(op.rs1, reg);
		if (!op.rs3.is_null())
		{
			if (op.rs3.is_imm())
				add(reg, op.rs3._imm);
			else if (regalloc.IsAllocg(op.rs3))
				add(reg, regalloc.MapRegister(op.rs3));
			else
				add(reg, Ctx(op.rs3.reg_ptr()));
		}
	}

	/* A load as one instruction, from the SH4's memory as the host has it
	 * mapped straight into the register the block wants it in. False if
	 * that is not to be had (FastMemory()), and nothing written. */
	bool GenReadMemoryFast(const shil_opcode& op)
	{
		const u32 size = op.flags & 0x7f;

		if (!FastMemory())
			return false;
		GenMemAddr(op, eax);

		if (size == 8)
		{
			GenFastAccess(MEM_LOAD64, 0);
			movq(qword[op.rd.reg_ptr()], xmm0);
			return true;
		}
		const int kind = size == 1 ? MEM_LOAD8 : size == 2 ? MEM_LOAD16 : MEM_LOAD32;
		if (regalloc.IsAllocg(op.rd))
			GenFastAccess(kind, regalloc.MapRegister(op.rd).getIdx());
		else if (size == 4 && regalloc.IsAllocf(op.rd))
			GenFastAccess(MEM_LOAD32F, regalloc.MapXRegister(op.rd).getIdx());
		else
		{
			GenFastAccess(kind, Xbyak::Operand::EAX);
			host_reg_to_shil_param(op.rd, eax);
		}
		return true;
	}

	// A store, likewise
	bool GenWriteMemoryFast(const shil_opcode& op)
	{
		const u32 size = op.flags & 0x7f;

		if (!FastMemory())
			return false;
		GenMemAddr(op, eax);

		if (size == 8)
		{
			movq(xmm0, qword[op.rs2.reg_ptr()]);
			GenFastAccess(MEM_STORE64, 0);
			return true;
		}
		const int kind = size == 1 ? MEM_STORE8 : size == 2 ? MEM_STORE16 : MEM_STORE32;
		if (op.rs2.is_reg() && regalloc.IsAllocg(op.rs2)
				&& (kind != MEM_STORE8 || regalloc.MapRegister(op.rs2).getIdx() == Xbyak::Operand::EBX))
			GenFastAccess(kind, regalloc.MapRegister(op.rs2).getIdx());
		else if (size == 4 && op.rs2.is_reg() && regalloc.IsAllocf(op.rs2))
			GenFastAccess(MEM_STORE32F, regalloc.MapXRegister(op.rs2).getIdx());
		else
		{
			shil_param_to_host_reg(op.rs2, edx);
			GenFastAccess(kind, Xbyak::Operand::EDX);
		}
		return true;
	}

	void CheckBlock(RuntimeBlockInfo* block)
	{
		// (ecx is the block's address at check_failed)
		mov(ecx, block->addr);

		// FIXME This test shouldn't be necessary: see x86-64
		if (mmu_enabled())
		{
			cmp(dword[&next_pc], block->vaddr);
			jne(check_failed, T_NEAR);
		}

		s32 sz = block->sh4_code_size;
		u32 sa = block->addr;

		while (sz > 0)
		{
			void* ptr = (void*)GetMemPtr(sa, sz > 4 ? 4 : sz);
			if (ptr)
			{
				const u32 at = (u32)(uintptr_t)ptr;

				if (sz >= 4 && !(at & 3))
				{
					cmp(dword[at], *(u32*)ptr);
					sz -= 4;
					sa += 4;
				}
				else
				{
					cmp(word[at], (u32)(s32)*(s16*)ptr);	// (xbyak takes a 16-bit constant signed)
					sz -= 2;
					sa += 2;
				}
				jne(check_failed, T_NEAR);
			}
			else
				break;
		}
	}

private:
	typedef void (BlockCompiler::*X86BinaryOp)(const Xbyak::Operand&, const Xbyak::Operand&);
	typedef void (BlockCompiler::*X86BinaryFOp)(const Xbyak::Xmm&, const Xbyak::Operand&);

	bool GenReadMemImmediate(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (!op.rs1.is_imm())
			return false;
		u32 size = op.flags & 0x7f;
		u32 addr = op.rs1._imm;
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
				return false;
			}
			if (rv != MMU_ERROR_NONE)
				return false;

			addr = paddr;
		}
		bool isram = false;
		void* ptr = _vmem_read_const(addr, isram, size > 4 ? 4 : size);
		if (!isram && size <= 4)
		{
			// a system bus register that is only storage is read as memory
			const void *reg = area0_read_const(ptr, addr, size);
			if (reg != nullptr)
			{
				ptr = (void *)reg;
				isram = true;
			}
		}

		if (isram)
		{
			const u32 at = (u32)(uintptr_t)ptr;
			switch (size)
			{
			case 1:
				if (regalloc.IsAllocg(op.rd))
					movsx(regalloc.MapRegister(op.rd), byte[at]);
				else
				{
					movsx(eax, byte[at]);
					mov(Ctx(op.rd.reg_ptr()), eax);
				}
				break;

			case 2:
				if (regalloc.IsAllocg(op.rd))
					movsx(regalloc.MapRegister(op.rd), word[at]);
				else
				{
					movsx(eax, word[at]);
					mov(Ctx(op.rd.reg_ptr()), eax);
				}
				break;

			case 4:
				if (regalloc.IsAllocg(op.rd))
					mov(regalloc.MapRegister(op.rd), dword[at]);
				else if (regalloc.IsAllocf(op.rd))
					movd(regalloc.MapXRegister(op.rd), dword[at]);
				else
				{
					mov(eax, dword[at]);
					mov(Ctx(op.rd.reg_ptr()), eax);
				}
				break;

			case 8:
				movq(xmm0, qword[at]);
				movq(qword[op.rd.reg_ptr()], xmm0);
				break;

			default:
				die("Invalid immediate size");
				break;
			}
		}
		else
		{
			// Not RAM: the returned pointer is a memory handler
			if (sh4_area7(addr))
				GenSliceLeft();
			if (size == 8)
			{
				// Need to call the handler twice
				mov(ecx, addr);
				GenCallRaw(ptr, nullptr, 0, true);
				mov(Ctx(op.rd.reg_ptr()), eax);

				mov(ecx, addr + 4);
				GenCallRaw(ptr, nullptr, 0, true);
				mov(Ctx(op.rd.reg_ptr() + 1), eax);
			}
			else
			{
				mov(ecx, addr);
				GenCallRaw(ptr, nullptr, 0, true);
				switch(size)
				{
				case 1:
					movsx(eax, al);
					break;
				case 2:
					movsx(eax, ax);
					break;
				case 4:
					break;
				default:
					die("Invalid immediate size");
					break;
				}
				host_reg_to_shil_param(op.rd, eax);
			}
		}

		return true;
	}

	bool GenWriteMemImmediate(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (!op.rs1.is_imm())
			return false;
		u32 size = op.flags & 0x7f;
		u32 addr = op.rs1._imm;
		if (mmu_enabled() && mmu_is_translated<MMU_TT_DWRITE>(addr, size))
		{
			if ((addr >> 12) != (block->vaddr >> 12))
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
				return false;
			}
			if (rv != MMU_ERROR_NONE)
				return false;

			addr = paddr;
		}
		bool isram = false;
		void* ptr = _vmem_write_const(addr, isram, size > 4 ? 4 : size);

		if (isram)
		{
			const u32 at = (u32)(uintptr_t)ptr;
			switch (size)
			{
			case 1:
				if (regalloc.IsAllocg(op.rs2) && regalloc.MapRegister(op.rs2).getIdx() == Xbyak::Operand::EBX)
					mov(byte[at], bl);
				else if (op.rs2.is_imm())
					mov(byte[at], (u8)op.rs2._imm);
				else
				{
					shil_param_to_host_reg(op.rs2, ecx);
					mov(byte[at], cl);
				}
				break;

			case 2:
				if (regalloc.IsAllocg(op.rs2))
					mov(word[at], regalloc.MapRegister(op.rs2).cvt16());
				else if (op.rs2.is_imm())
					mov(word[at], (u16)op.rs2._imm);
				else
				{
					mov(ecx, Ctx(op.rs2.reg_ptr()));
					mov(word[at], cx);
				}
				break;

			case 4:
				if (regalloc.IsAllocg(op.rs2))
					mov(dword[at], regalloc.MapRegister(op.rs2));
				else if (regalloc.IsAllocf(op.rs2))
					movd(dword[at], regalloc.MapXRegister(op.rs2));
				else if (op.rs2.is_imm())
					mov(dword[at], op.rs2._imm);
				else
				{
					mov(ecx, Ctx(op.rs2.reg_ptr()));
					mov(dword[at], ecx);
				}
				break;

			case 8:
				movq(xmm0, qword[op.rs2.reg_ptr()]);
				movq(qword[at], xmm0);
				break;

			default:
				die("Invalid immediate size");
				break;
			}
		}
		else
		{
			// Not RAM: the returned pointer is a memory handler
			if (sh4_area7(addr))
				GenSliceLeft();
			if (size == 8)
			{
				// a 32-bit handler: called for each half, as for a read
				mov(ecx, addr);
				mov(edx, Ctx(op.rs2.reg_ptr()));
				GenCallRaw(ptr, nullptr, 0, true);

				mov(ecx, addr + 4);
				mov(edx, Ctx(op.rs2.reg_ptr() + 1));
				GenCallRaw(ptr, nullptr, 0, true);
			}
			else
			{
				mov(ecx, addr);
				shil_param_to_host_reg(op.rs2, edx);
				GenCallRaw(ptr, nullptr, 0, true);
			}
		}

		return true;
	}

	void GenBinaryOp(const shil_opcode &op, X86BinaryOp natop)
	{
		Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
		const shil_param *rs2 = &op.rs2;
		if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
		{
			if (op.rs2.is_reg() && regalloc.mapg(op.rd) == regalloc.mapg(op.rs2))
			{
				if (op.op == shop_sub)
				{
					// This op isn't commutative
					neg(rd);
					add(rd, regalloc.MapRegister(op.rs1));

					return;
				}
				// otherwise just swap the operands
				rs2 = &op.rs1;
			}
			else
				mov(rd, regalloc.MapRegister(op.rs1));
		}
		if (op.rs2.is_imm())
		{
			// The constant goes in the instruction, not through a register
			switch (op.op)
			{
			case shop_and: and_(rd, op.rs2._imm); break;
			case shop_or:  or_(rd, op.rs2._imm);  break;
			case shop_xor: xor_(rd, op.rs2._imm); break;
			case shop_add: add(rd, op.rs2._imm);  break;
			case shop_sub: sub(rd, op.rs2._imm);  break;
			default:
				mov(ecx, op.rs2._imm);
				(this->*natop)(rd, ecx);
				break;
			}
		}
		else
			(this->*natop)(rd, regalloc.MapRegister(*rs2));
	}

	/* A NaN comes out as the one NaN an ARM host gives, 0x7fc00000: see
	 * x86-64. */
	void GenNaNFix(const Xbyak::Xmm& rd)
	{
		Xbyak::Label ok;
		ucomiss(rd, rd);
		jnp(ok);
		mov(eax, 0x7fc00000);
		movd(rd, eax);
		L(ok);
	}

	// The same for FTRV's four results, in xmm0. Uses xmm1 and xmm2.
	void GenNaNFix4()
	{
		Xbyak::Label ok;
		movaps(xmm1, xmm0);
		cmpunordps(xmm1, xmm1);		// all ones where a NaN is
		movmskps(eax, xmm1);
		test(eax, eax);
		jz(ok);
		mov(eax, 0x7fc00000);
		movd(xmm2, eax);
		shufps(xmm2, xmm2, 0);
		andps(xmm2, xmm1);
		andnps(xmm1, xmm0);
		orps(xmm1, xmm2);
		movaps(xmm0, xmm1);
		L(ok);
	}

	void GenBinaryFOp(const shil_opcode &op, X86BinaryFOp natop)
	{
		Xbyak::Xmm rd = regalloc.MapXRegister(op.rd);
		const shil_param *rs2 = &op.rs2;
		if (regalloc.mapf(op.rd) != regalloc.mapf(op.rs1))
		{
			if (op.rs2.is_reg() && regalloc.mapf(op.rd) == regalloc.mapf(op.rs2))
			{
				if (op.op == shop_fsub || op.op == shop_fdiv)
				{
					// these ops aren't commutative so we need a scratch reg
					movss(xmm0, regalloc.MapXRegister(op.rs2));
					movss(rd, regalloc.MapXRegister(op.rs1));
					(this->*natop)(rd, xmm0);

					return;
				}
				// otherwise just swap the operands
				rs2 = &op.rs1;
			}
			else
				movss(rd, regalloc.MapXRegister(op.rs1));
		}
		if (op.rs2.is_imm())
		{
			mov(eax, op.rs2._imm);
			movd(xmm0, eax);
			(this->*natop)(rd, xmm0);
		}
		else
			(this->*natop)(rd, regalloc.MapXRegister(*rs2));
	}

	// An argument on the stack
	struct Arg
	{
		enum Kind { Imm, Reg, Mem, Xmm } kind;
		u32 value;		// the constant, the register's number or the address

		static Arg I(u32 v) { Arg a; a.kind = Imm; a.value = v; return a; }
		static Arg R(const Xbyak::Reg32& reg) { Arg a; a.kind = Reg; a.value = reg.getIdx(); return a; }
		static Arg M(const void *p) { Arg a; a.kind = Mem; a.value = (u32)(uintptr_t)p; return a; }
		static Arg X(const Xbyak::Xmm& reg) { Arg a; a.kind = Xmm; a.value = reg.getIdx(); return a; }
	};

	/* A call, with the stack a multiple of 16 at it, @count arguments
	 * pushed (@args the first one first) and taken off again after unless
	 * the function does that (@callee_pops). Whatever goes in registers is
	 * there already. The block's xmm registers in use are kept: no function
	 * has to keep any of them on this processor. */
	void GenCallRaw(const void *function, const Arg *args, int count, bool callee_pops)
	{
		int saved[4];
		int saved_count = 0;

		if (current_opid != (size_t)-1)
			for (int i = 4; i < 8; i++)
				if (regalloc.IsMapped(Xbyak::Xmm(i), current_opid))
					saved[saved_count++] = i;
		if (saved_count != 0)
		{
			sub(esp, 16);
			for (int i = 0; i < saved_count; i++)
				movd(dword[esp + i * 4], Xbyak::Xmm(saved[i]));
		}
		const int pad = (16 - (count * 4) % 16) % 16;
		if (pad != 0)
			sub(esp, pad);
		for (int i = count; i-- > 0;)
		{
			switch (args[i].kind)
			{
			case Arg::Imm:
				push(args[i].value);
				break;
			case Arg::Reg:
				push(Xbyak::Reg32(args[i].value));
				break;
			case Arg::Mem:
				push(dword[args[i].value]);
				break;
			case Arg::Xmm:
				sub(esp, 4);
				movss(dword[esp], Xbyak::Xmm(args[i].value));
				break;
			}
		}
		call(function);
		const int cleanup = pad + (callee_pops ? 0 : count * 4);
		if (cleanup != 0)
			add(esp, cleanup);
		if (saved_count != 0)
		{
			for (int i = 0; i < saved_count; i++)
				movd(Xbyak::Xmm(saved[i]), dword[esp + i * 4]);
			add(esp, 16);
		}
	}

	// A fastcall function, whose arguments are all in ecx and edx already
	template<class Ret, class... Params>
	void GenCall(Ret (DYNACALL *function)(Params...))
	{
		static_assert(InRegisters<Params...>::value, "an argument that goes on the stack");
		GenCallRaw((const void *)function, nullptr, 0, true);
	}

	// A plain C function, its arguments given
	template<class Ret, class... Params>
	void GenCallC(Ret (*function)(Params...), std::initializer_list<Arg> args = {})
	{
		verify(args.size() == sizeof...(Params));
		GenCallRaw((const void *)function, args.begin(), (int)args.size(), false);
	}

	// uses eax for an immediate into a floating-point register
	void shil_param_to_host_reg(const shil_param& param, const Xbyak::Reg& reg)
	{
		if (param.is_imm())
		{
			if (!reg.isXMM())
				mov(reg, param._imm);
			else
			{
				mov(eax, param._imm);
				movd((const Xbyak::Xmm &)reg, eax);
			}
		}
		else if (param.is_reg())
		{
			if (param.is_r32f())
			{
				if (regalloc.IsAllocf(param))
				{
					Xbyak::Xmm sreg = regalloc.MapXRegister(param);
					if (!reg.isXMM())
						movd((const Xbyak::Reg32 &)reg, sreg);
					else if (reg != sreg)
						movss((const Xbyak::Xmm &)reg, sreg);
				}
				else
				{
					if (!reg.isXMM())
						mov((const Xbyak::Reg32 &)reg, Ctx(param.reg_ptr()));
					else
						movss((const Xbyak::Xmm &)reg, Ctx(param.reg_ptr()));
				}
			}
			else
			{
				if (regalloc.IsAllocg(param))
				{
					Xbyak::Reg32 sreg = regalloc.MapRegister(param);
					if (reg.isXMM())
						movd((const Xbyak::Xmm &)reg, sreg);
					else if (reg != sreg)
						mov((const Xbyak::Reg32 &)reg, sreg);
				}
				else
				{
					if (!reg.isXMM())
						mov((const Xbyak::Reg32 &)reg, Ctx(param.reg_ptr()));
					else
						movss((const Xbyak::Xmm &)reg, Ctx(param.reg_ptr()));
				}
			}
		}
		else
		{
			verify(param.is_null());
		}
	}

	void host_reg_to_shil_param(const shil_param& param, const Xbyak::Reg& reg)
	{
		if (regalloc.IsAllocg(param))
		{
			Xbyak::Reg32 sreg = regalloc.MapRegister(param);
			if (!reg.isXMM())
				mov(sreg, (const Xbyak::Reg32 &)reg);
			else if (reg != sreg)
				movd(sreg, (const Xbyak::Xmm &)reg);
		}
		else if (regalloc.IsAllocf(param))
		{
			Xbyak::Xmm sreg = regalloc.MapXRegister(param);
			if (!reg.isXMM())
				movd(sreg, (const Xbyak::Reg32 &)reg);
			else if (reg != sreg)
				movss(sreg, (const Xbyak::Xmm &)reg);
		}
		else
		{
			if (!reg.isXMM())
				mov(Ctx(param.reg_ptr()), (const Xbyak::Reg32 &)reg);
			else
				movss(Ctx(param.reg_ptr()), (const Xbyak::Xmm &)reg);
		}
	}

	struct CC_PS
	{
		CanonicalParamType type;
		const shil_param* prm;
	};
	std::vector<CC_PS> CC_pars;

	X86RegAlloc regalloc;
	static Xbyak::util::Cpu cpu;
	// where a float a canonical implementation returns is put on its way to an xmm register
	static f32 f32_rv;
	size_t current_opid;
	Xbyak::Label exit_block;
	Xbyak::Label slice_out;
	Xbyak::Label check_failed;
	Xbyak::Label vaddr_failed;
	bool vaddr_checked = false;
	bool slice_out_used = false;
	u32 block_cycles = 0;
	bool charge_at_tail = false;
};

void X86RegAlloc::Preload(u32 reg, Xbyak::Operand::Code nreg)
{
	compiler->RegPreload(reg, nreg);
}
void X86RegAlloc::Writeback(u32 reg, Xbyak::Operand::Code nreg)
{
	compiler->RegWriteback(reg, nreg);
}
void X86RegAlloc::Preload_FPU(u32 reg, s8 nreg)
{
	compiler->RegPreload_FPU(reg, nreg);
}
void X86RegAlloc::Writeback_FPU(u32 reg, s8 nreg)
{
	compiler->RegWriteback_FPU(reg, nreg);
}

static BlockCompiler* compiler;

// (process-invariant, and costly to find out: once)
Xbyak::util::Cpu BlockCompiler::cpu;
f32 BlockCompiler::f32_rv;

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	verify(emit_FreeSpace() >= CODE_MARGIN);

	compiler = new BlockCompiler();

	compiler->compile(block, force_checks, reset, staging, optimise);

	delete compiler;
}

void ngen_CC_Call(shil_opcode*op, void* function)
{
	compiler->ngen_CC_Call(*op, function);
}

void ngen_CC_Param(shil_opcode* op,shil_param* par,CanonicalParamType tp)
{
	compiler->ngen_CC_param(*op, *par, tp);
}

void ngen_CC_Start(shil_opcode* op)
{
	compiler->ngen_CC_Start(*op);
}

void ngen_CC_Finish(shil_opcode* op)
{
}

/* A fast memory access (BlockCompiler::FastMemory()) faulted: it is
 * written over with a call to the routine that does such an access by
 * asking (mem_stubs), and run again from where it was. It is six bytes or
 * more, and a call takes five; what is left of it is filled in. */
bool ngen_Rewrite(unat& host_pc, unat, unat)
{
	int kind, data, length;

	if (!BlockCompiler::FastMemory())
		return false;
	if (bm_GetBlock2((void *)host_pc) == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: Block at %p not found", (void *)host_pc);
		return false;
	}
	if (!BlockCompiler::FastAccessAt((const u8 *)host_pc, &kind, &data, &length)
			|| mem_stubs[kind][data] == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: no memory access at %p", (void *)host_pc);
		return false;
	}

	u8 *const site = (u8 *)host_pc;
	const s32 rel = (s32)(mem_stubs[kind][data] - (site + 5));
	static const u8 fill[4][3] = { { 0 }, { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 } };

	verify(length >= 5 && length <= 8);
	site[0] = 0xE8;
	memcpy(site + 1, &rel, 4);
	memcpy(site + 5, fill[length - 5], length - 5);
	return true;
}

void ngen_HandleException()
{
	rec_unwind();
}
#endif
