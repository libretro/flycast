#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X64
#include <setjmp.h>
//#define EXPLODE_SPANS

#include "deps/xbyak/xbyak.h"
#include "deps/xbyak/xbyak_util.h"

#include <vector>
#include <memory>
#include <algorithm>
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
#include "x64_regalloc.h"
#include "x64_vector.h"

/* One block goes on to the next with a jump straight to its code.
 *
 * Where a block ends in a jump whose target is known as it is compiled -
 * an unconditional one, or either way out of a conditional one - there is
 * a link site: seven bytes that are one of three things.
 *
 *   call the link stub   as compiled. The first time the block leaves
 *                        that way, ngen_link() has the target found or
 *                        compiled and makes the site one of the other two.
 *   jmp target           linked: the next block's code, directly.
 *   jmp [table]          through the table of blocks, as every block went
 *                        on before: for a way out that is not to be
 *                        linked - into or out of a block in the temporary
 *                        cache, which is thrown away without its
 *                        neighbours being told.
 *
 * A jump through the table is a load and an indirect jump, at the end of
 * every block; in a loop that is one block - half a dozen instructions
 * that jump back to their own start - they were a good part of what a turn
 * cost.
 *
 * When a block is thrown away the block manager goes through the blocks
 * that are linked to it (pre_refs), takes it out of them and has them
 * linked again (Relink()): their sites go back to calling the stub, which
 * finds the new block that is compiled in its place. A block that is
 * thrown away is unlinked the same way, since it may still be running -
 * it may be what wrote over the code - and has yet to leave. */
struct DynaRBI : RuntimeBlockInfo
{
   /* Where in the block's code its link sites are, 0 where there is none:
    * the way out to BranchBlock, and the one to NextBlock. */
   u32 link_at[2] = { 0, 0 };

   virtual u32 Relink();

   virtual void Relocate(void* dst) {
      verify(false);
   }
};

extern "C" {
   int cycle_counter;
}

#ifdef __MACH__
#define _U "_"
#else
#define _U
#endif

#ifdef _WIN32
#define WIN32_ONLY(x) x
#else
#define WIN32_ONLY(x)
#endif

#define STRINGIFY(x) #x
#define _S(x) STRINGIFY(x)
#define CPU_RUNNING 135266148
#define PC 135266120
/* Recompiled code keeps r15 pointing into the SH4's context, so that a
 * register in it is reached with one short instruction where it took a
 * 64-bit address in rax and then the access. It points 160 bytes in: a
 * signed byte then reaches the general registers, the floating-point ones
 * but the first eight of the second bank, and pc, pr, T and FPSCR.
 * CTX_BASE is that place from the start of the block holding the context. */
#define CTX_BIAS 160
#define CTX_BASE 135266016

jmp_buf jmp_env;

#ifdef _WIN32
        // Fully naked function in win32 for proper SEH prologue
        __asm__ (
                        ".text                                                  \n\t"
                        ".p2align 4,,15                                 \n\t"
                        ".globl ngen_mainloop                   \n\t"
                        ".def   ngen_mainloop;  .scl    2;      .type   32;     .endef  \n\t"
                        ".seh_proc      ngen_mainloop           \n\t"
                "ngen_mainloop:                                         \n\t"
#else
void ngen_mainloop(void* v_cntx)
{
        __asm__ (
#endif
                        "pushq %rbx                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %rbx                              \n\t")
                        "pushq %rbp                                             \n\t"
#ifdef _WIN32
                        ".seh_pushreg %rbp                              \n\t"
                        "pushq %rdi                                             \n\t"
                        ".seh_pushreg %rdi                              \n\t"
                        "pushq %rsi                                             \n\t"
                        ".seh_pushreg %rsi                              \n\t"
#endif
                        "pushq %r12                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %r12                              \n\t")
                        "pushq %r13                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %r13                              \n\t")
                        "pushq %r14                                             \n\t"
WIN32_ONLY(     ".seh_pushreg %r14                              \n\t")
                        "pushq %r15                                             \n\t"
#ifdef _WIN32
                        ".seh_pushreg %r15                              \n\t"
                        "subq $40, %rsp                                 \n\t"   // 32-byte shadow space + 8 for stack 16-byte alignment
                        ".seh_stackalloc 40                             \n\t"
                        ".seh_endprologue                               \n\t"
#else
                        "subq $8, %rsp                                  \n\t"   // 8 for stack 16-byte alignment
#endif
                        "movl $" _S(SH4_TIMESLICE) "," _U "cycle_counter(%rip)  \n\t"
                        "movq " _U "p_sh4rcb(%rip), %r15                 \n\t"   // the context, for all recompiled code
                        "addq $" _S(CTX_BASE) ", %r15                    \n"

#ifdef _WIN32
               			"leaq " _U "jmp_env(%rip), %rcx			\n\t"	// SETJMP
               			"xor %rdx, %rdx								\n\t"	// no frame pointer
#else
                        "leaq " _U "jmp_env(%rip), %rdi			\n\t"
#endif
#if defined(__MACH__) || defined(_WIN32)
                        "call " _U "setjmp							\n\t"
#else
                        "call " _U "setjmp@PLT						\n\t"
#endif

                "1:															\n\t"   // run_loop
                        "movq " _U "p_sh4rcb(%rip), %rax       \n\t"
                        "movl " _S(CPU_RUNNING) "(%rax), %edx  \n\t"
                        "testl %edx, %edx                      \n\t"
                        "je 3f                                                          \n"             // end_run_loop

                "2:                                                                             \n\t"   // slice_loop
                        "movq " _U "p_sh4rcb(%rip), %rax        \n\t"
#ifdef _WIN32
                        "movl " _S(PC)"(%rax), %ecx     \n\t"
#else
                        "movl " _S(PC)"(%rax), %edi     \n\t"
#endif
                        "call " _U "bm_GetCodeByVAddr				\n\t"
                        /* A block is jumped to, not called, and comes back by
                         * jumping to the label below: recompiled code runs on
                         * this function's stack as it stands - a multiple of
                         * 16 for the calls it makes, with the 32 bytes Windows
                         * wants above them already there - so no block has a
                         * stack to set up or put back, and one block goes on
                         * to the next without either. The address it is for
                         * goes with it in edx: see ngen_compile_stub. */
                        "movq " _U "p_sh4rcb(%rip), %rcx        \n\t"
                        "movl " _S(PC)"(%rcx), %edx     \n\t"
                        "jmp *%rax                                              \n\t"
                        ".globl " _U "ngen_block_return         \n"
                _U "ngen_block_return:                                  \n\t"
                        "movl " _U "cycle_counter(%rip), %ecx \n\t"
                        "testl %ecx, %ecx                                       \n\t"
                        "jg 2b                                                          \n\t"   // slice_loop

                        "addl $" _S(SH4_TIMESLICE) ", %ecx              \n\t"
                        "movl %ecx, " _U "cycle_counter(%rip)   \n\t"
                        "call " _U "UpdateSystem_INTC           \n\t"
                        "jmp 1b                                                         \n"             // run_loop

                "3:                                                                             \n\t"   // end_run_loop

#ifdef _WIN32
                        "addq $40, %rsp                                         \n\t"
#else
                        "addq $8, %rsp                                          \n\t"
#endif
                        "popq %r15                                                      \n\t"
                        "popq %r14                                                      \n\t"
                        "popq %r13                                                      \n\t"
                        "popq %r12                                                      \n\t"
#ifdef _WIN32
                        "popq %rsi                                                      \n\t"
                        "popq %rdi                                                      \n\t"
#endif
                        "popq %rbp                                                      \n\t"
                        "popq %rbx                                                      \n\t"
#ifdef _WIN32
                        "ret                                                            \n\t"
						".seh_endproc                   \n"
        );
#else
        );
}
#endif

/* What the table of blocks holds for an address with no block yet. It is
 * reached as a block is - jumped to, from the main loop or from the end of
 * another block, with the address in edx - has the block compiled, and
 * goes back to the main loop, which finds it there. (The place used to
 * hold a C function, which blocks got to by putting the stack back as the
 * main loop's call had left it and jumping; and the function read the
 * address from the context, where every block had stored it for that.) */
extern "C" void ngen_block_return();
extern "C" void ngen_compile_stub();
extern "C" __attribute__((used)) void ngen_compile_missing(u32 pc)
{
	rdv_FailedToFindBlock(pc);
}
__asm__ (
		".text                                  \n\t"
		".p2align 4                             \n\t"
		".globl " _U "ngen_compile_stub         \n"
	_U "ngen_compile_stub:                      \n\t"
#ifdef _WIN32
		"movl %edx, %ecx                        \n\t"
#else
		"movl %edx, %edi                        \n\t"
#endif
		"call " _U "ngen_compile_missing        \n\t"
		"jmp " _U "ngen_block_return            \n"
);

#ifndef _WIN32
/* Calls the function whose address is in rax, with the arguments as they
 * are, and keeps xmm8 to xmm15 across it: the floating-point registers
 * recompiled code is handed, which no function has to keep on these hosts.
 *
 * For a fast memory access that faulted and was written over with a call
 * (ngen_Rewrite): the code around it was compiled for a move, which leaves
 * every register alone, so whatever of those four is in use there has to
 * survive, and there is no room at the place itself to save them. The low
 * 32 bits are what recompiled code keeps in them. */
extern "C" void ngen_call_keep_xmm();
__asm__ (
		".text                                  \n\t"
		".p2align 4                             \n\t"
		".globl " _U "ngen_call_keep_xmm        \n"
	_U "ngen_call_keep_xmm:                     \n\t"
		"subq $40, %rsp                         \n\t"   // 32 for the eight, and the stack back to a multiple of 16
		"movd %xmm8, 0(%rsp)                    \n\t"
		"movd %xmm9, 4(%rsp)                    \n\t"
		"movd %xmm10, 8(%rsp)                   \n\t"
		"movd %xmm11, 12(%rsp)                  \n\t"
		"movd %xmm12, 16(%rsp)                  \n\t"
		"movd %xmm13, 20(%rsp)                  \n\t"
		"movd %xmm14, 24(%rsp)                  \n\t"
		"movd %xmm15, 28(%rsp)                  \n\t"
		"call *%rax                             \n\t"
		"movd 0(%rsp), %xmm8                    \n\t"
		"movd 4(%rsp), %xmm9                    \n\t"
		"movd 8(%rsp), %xmm10                   \n\t"
		"movd 12(%rsp), %xmm11                  \n\t"
		"movd 16(%rsp), %xmm12                  \n\t"
		"movd 20(%rsp), %xmm13                  \n\t"
		"movd 24(%rsp), %xmm14                  \n\t"
		"movd 28(%rsp), %xmm15                  \n\t"
		"addq $40, %rsp                         \n\t"
		"ret                                    \n"
);
#endif

#undef _U
#undef _S

/* The table of blocks is first filled before ngen_init() is called, and
 * a place in it filled with the C function this replaces would be jumped
 * to as a block is - with nowhere to return to. So: from the start. */
static struct CompileStubInit
{
	CompileStubInit() { ngen_FailedToFindBlock = &ngen_compile_stub; }
} compile_stub_init;

/* The kinds of fast memory access there are (BlockCompiler::FastMemory()):
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

/* What a fast memory access that faulted is written over with a call to
 * (ngen_Rewrite()): a routine that does the access by asking.
 *
 * A fast access has its address and its data in whatever registers the
 * block has them in, so there is a routine for each kind of access, each
 * register the address can be in and each the data can be in: 324 of
 * them on Linux and macOS and 560 on Windows, a few instructions each,
 * written once. They are in the code cache, like the link stub, so that a
 * near call reaches them. The access they stand in for changes nothing
 * but its data, so they keep the block's floating-point registers: by
 * going through ngen_call_keep_xmm, or, on Windows, by those being ones
 * every function keeps.
 *
 * The registers, by number: for the address and for data in a general
 * register, 0 is the one a call has it in (the first argument; rax for
 * what is loaded, the second argument for what is stored) and 1 on are
 * the block's own, alloc_regs; for data in a floating-point register, 0
 * on are the block's, alloc_fregs. 64 bits are always in rax or the
 * second argument. */
#define MEM_REGS 8
#define MEM_FREGS 10
static u8 *mem_stubs[MEM_KINDS][MEM_REGS][MEM_FREGS];

#ifdef _WIN32
#define MEM_REG_ADDR  Xbyak::Operand::RCX
#define MEM_REG_STORE Xbyak::Operand::RDX
#else
#define MEM_REG_ADDR  Xbyak::Operand::RDI
#define MEM_REG_STORE Xbyak::Operand::RSI
#endif

static s32 mem_load8(u32 addr)  { return (s8)ReadMem8(addr); }
static s32 mem_load16(u32 addr) { return (s16)ReadMem16(addr); }
static u32 mem_load32(u32 addr) { return ReadMem32(addr); }
static u64 mem_load64(u32 addr) { return ReadMem64(addr); }
static void mem_store8(u32 addr, u32 data)  { WriteMem8(addr, (u8)data); }
static void mem_store16(u32 addr, u32 data) { WriteMem16(addr, (u16)data); }
static void mem_store32(u32 addr, u32 data) { WriteMem32(addr, data); }
static void mem_store64(u32 addr, u64 data) { WriteMem64(addr, data); }

// How many general registers a block is given of its own, and how many floating-point ones
static int mem_block_regs()
{
	int count = 0;

	while (alloc_regs[count] != (Xbyak::Operand::Code)-1)
		count++;
	return count;
}

static int mem_block_fregs()
{
	int count = 0;

	while (alloc_fregs[count] != -1)
		count++;
	return count;
}

/* The number a general register goes by here: 0 if it is @first, 1 on for
 * the block's own, -1 for any other. */
static int mem_reg_number(int reg, int first)
{
	if (reg == first)
		return 0;
	for (int i = 0; alloc_regs[i] != (Xbyak::Operand::Code)-1; i++)
		if ((int)alloc_regs[i] == reg)
			return i + 1;
	return -1;
}

// The same for a floating-point register, which is one of the block's or nothing
static int mem_freg_number(int reg)
{
	for (int i = 0; alloc_fregs[i] != -1; i++)
		if (alloc_fregs[i] == reg)
			return i;
	return -1;
}

static void mem_stub_make(int kind, int addr, int data)
{
	static const void *const routines[MEM_KINDS] = {
		(const void *)&mem_load8, (const void *)&mem_load16, (const void *)&mem_load32, (const void *)&mem_load32,
		(const void *)&mem_load64,
		(const void *)&mem_store8, (const void *)&mem_store16, (const void *)&mem_store32, (const void *)&mem_store32,
		(const void *)&mem_store64,
	};
	const bool load = kind < MEM_STORE8;
	// (what is loaded has to be put somewhere after the call, unless rax is where it is wanted)
	const bool after = load && (MEM_KIND_F(kind) || data != 0);
	const Xbyak::Reg32 first(MEM_REG_ADDR), second(MEM_REG_STORE);
	Xbyak::CodeGenerator stub(64, emit_GetCCPtr());

	mem_stubs[kind][addr][data] = (u8 *)stub.getCode();
	// the address and what is stored to where a function takes them (neither is ever in the other's place)
	if (addr != 0)
		stub.mov(first, Xbyak::Reg32(alloc_regs[addr - 1]));
	if (!load && MEM_KIND_F(kind))
		stub.movd(second, Xbyak::Xmm(alloc_fregs[data]));
	else if (!load && data != 0)
		stub.mov(second, Xbyak::Reg32(alloc_regs[data - 1]));
	stub.mov(stub.rax, (uintptr_t)routines[kind]);
#ifdef _WIN32
	// room for the function to keep its arguments in, and the stack a multiple of 16
	stub.sub(stub.rsp, 40);
	stub.call(stub.rax);
	stub.add(stub.rsp, 40);
#else
	stub.mov(stub.r11, (uintptr_t)&ngen_call_keep_xmm);
	if (after)
	{
		stub.sub(stub.rsp, 8);
		stub.call(stub.r11);
		stub.add(stub.rsp, 8);
	}
	else
		stub.jmp(stub.r11);
#endif
	if (after)
	{
		if (MEM_KIND_F(kind))
			stub.movd(Xbyak::Xmm(alloc_fregs[data]), stub.eax);
		else
			stub.mov(Xbyak::Reg32(alloc_regs[data - 1]), stub.eax);
	}
#ifdef _WIN32
	stub.ret();
#else
	if (after)
		stub.ret();
#endif
	stub.ready();
	emit_Skip((stub.getSize() + 15) & ~15u);
}

/* The link stub, which is in the code cache so that every block reaches
 * it with a near call: see DynaRBI. In the cache's writable mapping; a
 * block is the same distance from it in the executable one. */
static u8 *link_stub;

// Where entry @index of the table of blocks is from r15: see BlockCompiler::FpcbAt()
static int fpcb_at(u32 index)
{
	return (int)((ptrdiff_t)(offsetof(Sh4RCB, fpcb) + (size_t)index * sizeof(void *)) - (ptrdiff_t)CTX_BASE);
}

static void link_site_call_stub(u8 *site)
{
	const s32 rel = (s32)(link_stub - (site + 5));

	site[0] = 0xE8;		// call rel32
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
	site[6] = 0xCC;
}

static void link_site_jump(u8 *site, const u8 *code)
{
	const s32 rel = (s32)(code - (site + 5));

	site[0] = 0xE9;		// jmp rel32
	memcpy(site + 1, &rel, 4);
	site[5] = 0xCC;
	site[6] = 0xCC;
}

u32 DynaRBI::Relink()
{
	for (int i = 0; i < 2; i++)
	{
		const RuntimeBlockInfo *to = i == 0 ? pBranchBlock : pNextBlock;

		if (link_at[i] == 0)
			continue;
		if (to != NULL)
			link_site_jump((u8 *)code + link_at[i], (const u8 *)to->code);
		else
			link_site_call_stub((u8 *)code + link_at[i]);
	}
	return 0;
}

/* From the link stub: the block whose link site returns to @after is
 * leaving for @pc for the first time. The block there is found or
 * compiled, the site is made a jump to it if the two may be linked, and
 * its code is where the stub goes. */
extern "C" __attribute__((used)) void *ngen_link(u8 *after, u32 pc)
{
	u8 *const site_rx = after - 5;
	RuntimeBlockInfoPtr from = bm_GetBlock2(site_rx);
	const bool mmu = mmu_enabled();
	// where @pc is, for the block manager
	u32 addr = pc;
	DynarecCodeEntryPtr code;

	if (mmu)
	{
		/* With the MMU on a block has a link site only for an address
		 * that is where it is whatever the TLB says (rdv_MmuSamePlace()),
		 * and the context has the address as well: GenGoOn(). So nothing
		 * is translated here - and no exception raised in passing. */
		if (from == NULL || !rdv_MmuSamePlace(from.get(), pc, &addr))
			return (void *)&ngen_block_return;
		code = (DynarecCodeEntryPtr)p_sh4rcb->fpcb[(addr >> 1) & FPCB_MASK];
	}
	else
		code = bm_GetCodeByVAddr(pc);
	if (code == ngen_FailedToFindBlock)
	{
		code = rdv_FailedToFindBlock(pc);
		// (compiling it was an exception instead: the context says where to)
		if (mmu && next_pc != pc)
			return (void *)&ngen_block_return;
	}
	/* No block to be had there: to the main loop, which goes by the pc in
	 * the context. (The compile stub, which is what there is in a block's
	 * place then, wants the address in edx, and this has used edx.) */
	if (code == ngen_FailedToFindBlock)
	{
		next_pc = pc;
		return (void *)&ngen_block_return;
	}

	/* Compiling may have emptied the cache, and the block that asked with
	 * it; or the block was thrown away before it got here (it wrote over
	 * its own code), and is not found at all. Nothing is linked then. */
	if (from == NULL || bm_GetBlock2(site_rx) != from)
		return (void *)code;

	DynaRBI *const block = (DynaRBI *)from.get();
	u8 *const site = (u8 *)CC_RX2RW(site_rx);
	const u32 at = (u32)(site - (u8 *)block->code);
	const int which = at == block->link_at[0] ? 0 : at == block->link_at[1] ? 1 : -1;
	RuntimeBlockInfoPtr to = bm_GetBlock(addr);

	if (which < 0 || pc != (which == 0 ? block->BranchBlock : block->NextBlock))
		return (void *)code;		// not a site of this block's: left as it is
	if (to == NULL || to->addr != addr || (DynarecCodeEntryPtr)CC_RW2RX(to->code) != code)
		return (void *)code;		// nothing to link to (yet)
	// (with the MMU on a block is compiled for the address it was first run at)
	if (mmu && to->vaddr != pc)
		return (void *)code;

	if (to->temp_block)
	{
		/* Not linked, ever: through the table from now on.
		 * jmp qword [r15 + disp32] */
		const s32 disp = fpcb_at((addr >> 1) & FPCB_MASK);

		site[0] = 0x41;
		site[1] = 0xFF;
		site[2] = 0xA7;
		memcpy(site + 3, &disp, 4);
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

void ngen_init()
{
	ngen_FailedToFindBlock = &ngen_compile_stub;

	/* The link stub, ahead of the blocks, where emptying the cache leaves
	 * it. It is called from a link site with the address the block is
	 * leaving for in edx, as every block is left: the site's return
	 * address and that address are ngen_link()'s arguments, and what it
	 * returns is jumped to. (The stack is the main loop's, as it is for
	 * any call a block makes, once the return address is off it.) */
	{
		Xbyak::CodeGenerator stub(64, emit_GetCCPtr());

		link_stub = (u8 *)stub.getCode();
#ifdef _WIN32
		stub.pop(stub.rcx);
		// (edx is the second argument already)
#else
		stub.pop(stub.rdi);
		stub.mov(stub.esi, stub.edx);
#endif
		stub.mov(stub.rax, (uintptr_t)&ngen_link);
		stub.call(stub.rax);
		stub.jmp(stub.rax);
		stub.ready();
		emit_Skip((stub.getSize() + 15) & ~15u);
	}
	{
		const int regs = 1 + mem_block_regs();
		const int fregs = mem_block_fregs();

		verify(regs <= MEM_REGS && fregs <= MEM_FREGS);
		for (int kind = 0; kind < MEM_KINDS; kind++)
			for (int addr = 0; addr < regs; addr++)
			{
				const int datas = MEM_KIND_F(kind) ? fregs : kind == MEM_LOAD64 || kind == MEM_STORE64 ? 1 : regs;

				for (int data = 0; data < datas; data++)
					mem_stub_make(kind, addr, data);
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

void ngen_blockcheckfail(u32 pc) {
	//printf("X64 JIT: SMC invalidation at %08X\n", pc);
	rdv_BlockCheckFail(pc);
}
static void handle_mem_exception(u32 exception_raised, u32 pc)
{
	if (exception_raised)
	{
		if (pc & 1)
			// Delay slot
			spc = pc - 1;
		else
			spc = pc;
		cycle_counter += CPU_RATIO * 2;	// probably more is needed but no easy way to find out
		longjmp(jmp_env, 1);
	}
}

template<typename T>
static T ReadMemNoEx(u32 addr, u32 pc)
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
		handle_mem_exception(1, pc);
	}
	// where the host's mapping does not do the translating, the next read
	// of the page need not come here: see mmu.h
	mmu_lut_fill(addr, paddr, false);

	return _vmem_readt<T, T>(paddr);
#else
	// not used
	return (T)0;
#endif
}

template<typename T>
static void WriteMemNoEx(u32 addr, T data, u32 pc)
{
#ifndef NO_MMU
	if (sizeof(T) == 8 && mmu_in_two_pages(addr))
	{
		WriteMemNoEx<u32>(addr, (u32)data, pc);
		WriteMemNoEx<u32>(addr + 4, (u32)((u64)data >> 32), pc);
		return;
	}

	u32 paddr;
	u32 rv = mmu_data_translation<MMU_TT_DWRITE, T>(addr, paddr);
	if (rv != MMU_ERROR_NONE)
	{
		DoMMUException(addr, rv, MMU_TT_DWRITE);
		handle_mem_exception(1, pc);
	}
	mmu_lut_fill(addr, paddr, true);
	_vmem_writet<T>(paddr, data);
#endif
}

static void handle_sh4_exception(SH4ThrownException& ex, u32 pc)
{
	if (pc & 1)
	{
		// Delay slot
		AdjustDelaySlotException(ex);
		pc--;
	}
	Do_Exception(pc, ex.expEvn, ex.callVect);
	cycle_counter += CPU_RATIO * 4;	// probably more is needed
	longjmp(jmp_env, 1);
}

static void interpreter_fallback(u16 op, OpCallFP *oph, u32 pc)
{
	try {
		oph(op);
	} catch (SH4ThrownException& ex) {
		handle_sh4_exception(ex, pc);
	}
}

static void do_sqw_mmu_no_ex(u32 addr, u32 pc)
{
	try {
		do_sqw_mmu(addr);
	} catch (SH4ThrownException& ex) {
		handle_sh4_exception(ex, pc);
	}
}

static void do_sqw_nommu_local(u32 addr, u8* sqb)
{
	do_sqw_nommu(addr, sqb);
}

class BlockCompiler : public Xbyak::CodeGenerator
{
public:
	BlockCompiler() : BlockCompiler((u8 *)emit_GetCCPtr()) {}

	BlockCompiler(u8 *code_ptr) : Xbyak::CodeGenerator(emit_FreeSpace(), code_ptr), regalloc(this)
	{
#ifdef _WIN32
      call_regs.push_back(ecx);
      call_regs.push_back(edx);
      call_regs.push_back(r8d);
      call_regs.push_back(r9d);

      call_regs64.push_back(rcx);
      call_regs64.push_back(rdx);
      call_regs64.push_back(r8);
      call_regs64.push_back(r9);
#else
      call_regs.push_back(edi);
      call_regs.push_back(esi);
      call_regs.push_back(edx);
      call_regs.push_back(ecx);

      call_regs64.push_back(rdi);
      call_regs64.push_back(rsi);
      call_regs64.push_back(rdx);
      call_regs64.push_back(rcx);
#endif

		call_regsxmm.push_back(xmm0);
		call_regsxmm.push_back(xmm1);
		call_regsxmm.push_back(xmm2);
		call_regsxmm.push_back(xmm3);
	}

	void compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
   {
		current_opid = -1;
      if (force_checks) {
			CheckBlock(block);
		}
		/* (No stack to set up: the block runs on the main loop's, which is
		 * as calls want it. See ngen_mainloop.) */

		if (mmu_enabled() && block->has_fpu_op)
		{
			Xbyak::Label fpu_enabled;
			mov(rax, (uintptr_t)&sr);
			test(dword[rax], 0x8000);			// test SR.FD bit
			jz(fpu_enabled);
			mov(call_regs[0], block->vaddr);	// pc
			mov(call_regs[1], 0x800);			// event
			mov(call_regs[2], 0x100);			// vector
			GenCall(Do_Exception);
			jmp(exit_block, T_NEAR);
			L(fpu_enabled);
		}
		/* What the block costs is taken off the cycle counter where the
		 * block ends, when it can be: the subtraction then also says
		 * whether the time slice is used up, and the counter is touched
		 * once a block instead of twice. With the MMU on a block can be
		 * left from the middle, so it pays as it starts. */
		block_cycles = block->guest_cycles;
		charge_at_tail = !mmu_enabled();
		if (!charge_at_tail)
			GenCharge();
		if (settings.dynarec.AccurateTiming && !mmu_enabled())
		{
			// a block that can change something other than a register says so: see wait_site.h
			if (sh4_block_writes(block))
			{
#ifdef FEAT_NO_RWX_PAGES
				mov(rax, (uintptr_t)&sh4_write_gen);
				inc(dword[rax]);
#else
				inc(dword[rip + &sh4_write_gen]);
#endif
			}
		}
		regalloc.DoAlloc(block);

		for (current_opid = 0; current_opid < block->oplist.size(); current_opid++)
		{
			shil_opcode& op  = block->oplist[current_opid];

			regalloc.OpBegin(&op, current_opid);

         switch (op.op)
         {
            case shop_ifb:
					if (mmu_enabled())
					{
						mov(call_regs64[1], reinterpret_cast<uintptr_t>(*OpDesc[op.rs3._imm]->oph));	// op handler
						mov(call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc
					}
               if (op.rs1._imm)
               {
                  mov(rax, (size_t)&next_pc);
                  mov(dword[rax], op.rs2._imm);
               }

               mov(call_regs[0], op.rs3._imm);

					if (!mmu_enabled())
						GenCall(OpDesc[op.rs3._imm]->oph);
					else
						GenCall(interpreter_fallback);

               break;

            case shop_jcond:
            case shop_jdyn:
					{
						Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					// This shouldn't happen since the block type would have been changed to static.
					// But it doesn't hurt and is handy when partially disabling ssa for testing
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
            	{
            		verify(op.rd.is_reg());

            		verify(op.rs1.is_reg() || op.rs1.is_imm());

            		if (regalloc.IsAllocf(op.rd))
            			shil_param_to_host_reg(op.rs1, regalloc.MapXRegister(op.rd));
            		else
            			shil_param_to_host_reg(op.rs1, regalloc.MapRegister(op.rd));
            	}
               break;

            case shop_mov64:
               {
				  verify(op.rd.is_r64());
				  verify(op.rs1.is_r64());

#ifdef EXPLODE_SPANS
				  movss(regalloc.MapXRegister(op.rd, 0), regalloc.MapXRegister(op.rs1, 0));
				  movss(regalloc.MapXRegister(op.rd, 1), regalloc.MapXRegister(op.rs1, 1));
#else
				  mov(rax, qword[CtxAt(op.rs1.reg_ptr())]);
				  mov(qword[CtxAt(op.rd.reg_ptr())], rax);
#endif
               }
               break;

            case shop_readm:
				// Not an immediate address: one instruction, or the call
				if (!GenReadMemImmediate(op, block) && (!optimise || !GenReadMemoryFast(op)))
               {
						GenMemAddr(op);
						GenReadMemorySlow(op, block);

						u32 size = op.flags & 0x7f;
						if (size != 8)
						host_reg_to_shil_param(op.rd, eax);
						else {
#ifdef EXPLODE_SPANS
							if (op.rd.count() == 2 && regalloc.IsAllocf(op.rd, 0) && regalloc.IsAllocf(op.rd, 1))
							{
							movd(regalloc.MapXRegister(op.rd, 0), eax);
							shr(rax, 32);
							movd(regalloc.MapXRegister(op.rd, 1), eax);
							}
							else
#endif
							{
								mov(qword[CtxAt(op.rd.reg_ptr())], rax);
							}
						}
               }
               break;

            case shop_writem:
               {
						if (!GenWriteMemImmediate(op, block) && (!optimise || !GenWriteMemoryFast(op)))
						{
							GenMemAddr(op);

							u32 size = op.flags & 0x7f;
							if (size != 8)
								shil_param_to_host_reg(op.rs2, call_regs[1]);
							else {
#ifdef EXPLODE_SPANS
								if (op.rs2.count() == 2 && regalloc.IsAllocf(op.rs2, 0) && regalloc.IsAllocf(op.rs2, 1))
								{
									movd(call_regs[1], regalloc.MapXRegister(op.rs2, 1));
									shl(call_regs64[1], 32);
									movd(eax, regalloc.MapXRegister(op.rs2, 0));
									or_(call_regs64[1], rax);
								}
								else
#endif
								{
									mov(call_regs64[1], qword[CtxAt(op.rs2.reg_ptr())]);
								}
							}
							GenWriteMemorySlow(op, block);
						}
               }
               break;

#ifndef CANONICAL_TEST
            case shop_sync_sr:
               GenCall(UpdateSR);
               break;
            case shop_sync_fpscr:
               GenCall(UpdateFPSCR);
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
					setc(regalloc.MapRegister(op.rd2).cvt8());	// rd2 = C
				}
				break;
            /* FIXME buggy
			case shop_sbc:
				if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
					mov(regalloc.MapRegister(op.rd), regalloc.MapRegister(op.rs1));
				cmp(regalloc.MapRegister(op.rs3), 1);	// C = ~rs3
				cmc();		// C = rs3
				mov(ecx, 1);
				mov(regalloc.MapRegister(op.rd2), 0);
				mov(eax, regalloc.MapRegister(op.rs2));
				neg(eax);
				adc(regalloc.MapRegister(op.rd), eax); // (C,rd)=rs1-rs2+rs3(C)
				cmovc(regalloc.MapRegister(op.rd2), ecx);	// rd2 = C
				break;
                */
   			case shop_negc:
   				{
   					Xbyak::Reg32 rs2;
						if (op.rs2.is_reg())
						{
							rs2 = regalloc.MapRegister(op.rs2);
							if (regalloc.mapg(op.rd) == regalloc.mapg(op.rs2))
							{
								mov(ecx, rs2);
								rs2 = ecx;
							}
						}
						Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
						if (op.rs1.is_imm())
							mov(rd, op.rs1.imm_value());
						else if (regalloc.mapg(op.rd) != regalloc.mapg(op.rs1))
							mov(rd, regalloc.MapRegister(op.rs1));
						Xbyak::Reg64 rd64 = rd.cvt64();
							neg(rd64);
						if (op.rs2.is_imm())
							sub(rd64, op.rs2.imm_value());
						else
							sub(rd64, rs2.cvt64());
						Xbyak::Reg64 rd2_64 = regalloc.MapRegister(op.rd2).cvt64();
						mov(rd2_64, rd64);
						shr(rd2_64, 63);
						/* Nothing is left above the low 32 bits of the result:
						 * this very instruction, done again on it, would take
						 * the borrow from what was there, and a register of
						 * the block's is an address as it is (GenFastAddr()). */
						mov(rd, rd);
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

					Xbyak::Reg32 rd = regalloc.MapRegister(op.rd);
					mov(rd, 1);
					test(ecx, 0xFF000000);
					je(end);
					test(ecx, 0x00FF0000);
					je(end);
					test(ecx, 0x0000FF00);
					je(end);
					xor_(rd, rd);
					test(cl, cl);
					sete(rd.cvt8());
					L(end);
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
               mov(eax, regalloc.MapRegister(op.rs1));
					if (op.rs2.is_reg())
						mov(ecx, regalloc.MapRegister(op.rs2));
					else
						mov(ecx, op.rs2._imm);
               mul(rcx);
               mov(regalloc.MapRegister(op.rd), eax);
               shr(rax, 32);
               mov(regalloc.MapRegister(op.rd2), eax);
               break;
            case shop_mul_s64:
               movsxd(rax, regalloc.MapRegister(op.rs1));
					if (op.rs2.is_reg())
						movsxd(rcx, regalloc.MapRegister(op.rs2));
					else
						mov(rcx, (s64)(s32)op.rs2._imm);
               mul(rcx);
               mov(regalloc.MapRegister(op.rd), eax);
               shr(rax, 32);
               mov(regalloc.MapRegister(op.rd2), eax);
               break;

			case shop_pref:
				if (op.rs1.is_imm())
				{
					// this test shouldn't be necessary
					if ((op.rs1._imm & 0xFC000000) == 0xE0000000)
					{
						mov(call_regs[0], op.rs1._imm);
						if (mmu_enabled())
						{
							mov(call_regs[1], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));      // pc

							GenCall(do_sqw_mmu_no_ex);
						}
						else
						{
							if (CCN_MMUCR.AT == 1)
							{
								GenCall(do_sqw_mmu);
							}
							else
							{
								mov(call_regs64[1], (uintptr_t)sq_both);
								GenCall(&do_sqw_nommu_local);
							}
						}
					}
				}
				else
				{
					Xbyak::Reg32 rn;
					if (regalloc.IsAllocg(op.rs1))
					{
						rn = regalloc.MapRegister(op.rs1);
					}
					else
					{
						lea(rax, Ctx(op.rs1.reg_ptr()));
						mov(eax, dword[rax]);
						rn = eax;
					}
					mov(ecx, rn);
					shr(ecx, 26);
					cmp(ecx, 0x38);
					Xbyak::Label no_sqw;
					jne(no_sqw, T_NEAR);	// (the call can be long: it keeps the floating-point registers in use)

					mov(call_regs[0], rn);
					if (mmu_enabled())
					{
						mov(call_regs[1], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

						GenCall(do_sqw_mmu_no_ex);
					}
					else
					{
						if (CCN_MMUCR.AT == 1)
						{
							GenCall(do_sqw_mmu);
						}
						else
						{
							mov(call_regs64[1], (uintptr_t)sq_both);
							GenCall(&do_sqw_nommu_local);
						}
					}
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
               break;
            case shop_fsub:
               GenBinaryFOp(op, &BlockCompiler::subss);
               break;
            case shop_fmul:
               GenBinaryFOp(op, &BlockCompiler::mulss);
               break;
            case shop_fdiv:
               GenBinaryFOp(op, &BlockCompiler::divss);
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
               break;

            case shop_fmac:
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
						if (cpu.has(Xbyak::util::Cpu::tFMA))
							vfmadd231ss(rd, rs2, rs3);
						else
						{
							movss(xmm0, rs2);
							mulss(xmm0, rs3);
							addss(rd, xmm0);
						}
					}
               break;

            case shop_fsrra:
   				// RSQRTSS has an |error| <= 1.5*2^-12 where the SH4 FSRRA needs |error| <= 2^-21
   				sqrtss(xmm0, regalloc.MapXRegister(op.rs1));
   				mov(eax, 0x3f800000);	// 1.0
   				movd(regalloc.MapXRegister(op.rd), eax);
   				divss(regalloc.MapXRegister(op.rd), xmm0);
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
					mov(rax, op.rs1._imm & 0xFFFF);
				else
            		movzx(rax, regalloc.MapRegister(op.rs1).cvt16());
               mov(rcx, (uintptr_t)&sin_table);
#ifdef EXPLODE_SPANS
               movss(regalloc.MapXRegister(op.rd, 0), dword[rcx + rax * 8]);
               movss(regalloc.MapXRegister(op.rd, 1), dword[rcx + (rax * 8) + 4]);
#else
               mov(rcx, qword[rcx + rax * 8]);
               lea(rdx, Ctx(op.rd.reg_ptr()));
               mov(qword[rdx], rcx);
#endif
               break;

            case shop_fipr:
					/* In line, in doubles: see x64_vector.h. The code for this was
					 * commented out, and every inner product called the reference
					 * function. */
					lea(rax, Ctx(op.rs1.reg_ptr()));
					lea(rcx, Ctx(op.rs2.reg_ptr()));
					x64_emit_fipr(*this, regalloc.MapXRegister(op.rd));
					break;

            case shop_ftrv:
					/* As above. This called the reference function for every
					 * vertex a game transforms. */
					lea(rax, Ctx(op.rs1.reg_ptr()));		// the vector
					lea(rcx, Ctx(op.rs2.reg_ptr()));		// the matrix
					x64_emit_ftrv(*this);
					lea(rax, Ctx(op.rd.reg_ptr()));
					movups(xword[rax], xmm0);
					break;

            case shop_frswap:
               lea(rax, Ctx(op.rs1.reg_ptr()));
               lea(rcx, Ctx(op.rd.reg_ptr()));
					if (cpu.has(Xbyak::util::Cpu::tAVX512F))
					{
						vmovaps(zmm0, zword[rax]);
						vmovaps(zmm1, zword[rcx]);
						vmovaps(zword[rax], zmm1);
						vmovaps(zword[rcx], zmm0);
					}
					else if (cpu.has(Xbyak::util::Cpu::tAVX))
					{
						vmovaps(ymm0, yword[rax]);
						vmovaps(ymm1, yword[rcx]);
						vmovaps(yword[rax], ymm1);
						vmovaps(yword[rcx], ymm0);

						vmovaps(ymm0, yword[rax + 32]);
						vmovaps(ymm1, yword[rcx + 32]);
						vmovaps(yword[rax + 32], ymm1);
						vmovaps(yword[rcx + 32], ymm0);
					}
					else
					{
						for (int i = 0; i < 4; i++)
						{
							movaps(xmm0, xword[rax + (i * 16)]);
							movaps(xmm1, xword[rcx + (i * 16)]);
							movaps(xword[rax + (i * 16)], xmm1);
							movaps(xword[rcx + (i * 16)], xmm0);
						}
					}
               break;

            case shop_cvt_f2i_t:
					{
						/* Too large for 32 bits, the host answers
						 * 0x80000000 whatever the sign, and so it does for
						 * a NaN. That is right for a negative value and
						 * for a NaN; a positive one is 0x7fffffff. So the
						 * value is compared with zero: below it or not
						 * comparable (a NaN) keeps 0x80000000.
						 *
						 * This used to go by the sign bit, which made a NaN
						 * with it clear 0x7fffffff, and it also turned
						 * 2147483520, the largest float that does fit,
						 * into 0x7fffffff. */
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

		/* Where the block ends by going on to another, it goes there itself:
		 * through the next block's place in the table of blocks, which
		 * always holds that block's code, or the stub that compiles it if
		 * there is none (or no longer). It used to return to the main loop
		 * after every block, which then called a function to look the next
		 * one up and called that.
		 *
		 * What the main loop did in between still happens here: the next
		 * block is only gone on to while there is time left in the slice,
		 * and otherwise this returns as before. The stack is put back
		 * first, so that whatever is jumped to starts as if the main loop
		 * had called it.
		 *
		 * With the MMU on an address has to be translated before it means a
		 * place in the table, and the lookup function does that - but for
		 * the addresses that are where they are whatever the TLB says: see
		 * rdv_MmuMayGoOn(). */
		const bool go_on = !mmu_enabled();
		u32 place;
		const bool mmu_go_on = !go_on && block->mmu_go_on;
		const bool go_on_branch = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->BranchBlock, &place));
		const bool go_on_next = go_on || (mmu_go_on && rdv_MmuSamePlace(block, block->NextBlock, &place));
		// see wait_site.h
		const bool watch = go_on && settings.dynarec.AccurateTiming && block->BranchBlock <= block->vaddr;

	  switch (block->BlockType) {

		/* A block that goes on to another does not store the address in the
		 * context first, as every block did: it takes it along in edx, and
		 * it is stored only if the time slice turns out to be used up
		 * (slice_out) or there is no block there yet (ngen_compile_stub).
		 * The context's pc is what the main loop goes by, and is right
		 * whenever it is the main loop's turn. */
		case BET_StaticJump:
		case BET_StaticCall:
			//next_pc = block->BranchBlock;
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
				//next_pc = next_pc_value;
				//if (*jdyn == 0)
				//next_pc = branch_pc_value;

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
			//next_pc = *jdyn;
			mov(edx, Ctx(&Sh4cntx.jdyn));
			if (go_on)
			{
				// the address is only known now: its place in the table is worked out here
				GenSliceCheck();
				mov(eax, edx);
				shr(eax, 1);
				and_(eax, FPCB_MASK);
				jmp(qword[r15 + rax * 8 + FpcbAt(0)]);
			}
			else
				mov(Ctx(&next_pc), edx);
			break;

		case BET_DynamicIntr:
		case BET_StaticIntr:
			if (block->BlockType == BET_DynamicIntr) {
				//next_pc = *jdyn;
				mov(edx, Ctx(&Sh4cntx.jdyn));
				mov(Ctx(&next_pc), edx);
			}
			else {
				//next_pc = next_pc_value;
				mov(Ctx(&next_pc), block->NextBlock);
			}

			if (charge_at_tail)
				GenCharge();
			GenCall(UpdateINTC);
			break;

		default:
			die("Invalid block end type");
		}

		/* Back to the main loop. (Far from here, as a rule: by its address.) */
		L(exit_block);
		mov(rax, (uintptr_t)&ngen_block_return);
		jmp(rax);

		/* The time slice is used up where the block would have gone on to
		 * another: the address it was going to is in edx, and is where the
		 * main loop picks up from. */
		if (slice_out_used)
		{
			L(slice_out);
			mov(Ctx(&next_pc), edx);
			jmp(exit_block, T_NEAR);
		}
		/* The block's code is no longer what it was compiled from. */
		if (force_checks)
		{
			L(check_failed);
			mov(rax, (uintptr_t)&ngen_blockcheckfail);
			call(rax);
			jmp(exit_block, T_NEAR);
		}

		ready();

		block->code = (DynarecCodeEntryPtr)getCode();
		block->host_code_size = getSize();

		emit_Skip(getSize());
	}

	// To the main loop if the time slice is used up, by way of slice_out,
	// which stores the address in edx; otherwise on
	void GenSliceCheck()
	{
		if (charge_at_tail)
			GenCharge();		// leaves the flags of the counter against 0
		else
		{
#ifdef FEAT_NO_RWX_PAGES
			mov(rcx, (uintptr_t)&cycle_counter);
			cmp(dword[rcx], 0);
#else
			cmp(dword[rip + &cycle_counter], 0);
#endif
		}
		jle(slice_out, T_NEAR);
		slice_out_used = true;
	}

	// A block going back: every so often, see whether it is only waiting, and
	// give up the rest of the time slice if it is. See wait_site.h.
	// Takes what the block costs off the cycle counter
	void GenCharge()
	{
#ifdef FEAT_NO_RWX_PAGES
		// Use absolute addressing for this one
		// TODO(davidgfnet) remove the ifsef using CC_RX2RW/CC_RW2RX
		mov(rcx, (uintptr_t)&cycle_counter);
		sub(dword[rcx], block_cycles);
#else
		sub(dword[rip + &cycle_counter], block_cycles);
#endif
	}

	void GenWaitCheck(const RuntimeBlockInfo *block)
	{
		WaitSite *site = sh4_wait_site(block);
		if (site == nullptr)
			return;
		Xbyak::Label over;

		mov(rax, (uintptr_t)&site->skip);
		dec(dword[rax]);
		jns(over, T_NEAR);
		mov(call_regs64[0], (uintptr_t)site);
		GenCall(sh4_wait_check);
		test(eax, eax);
		jz(over, T_NEAR);
		// the block has yet to pay: this leaves the counter at 0 when it has
#ifdef FEAT_NO_RWX_PAGES
		mov(rax, (uintptr_t)&cycle_counter);
		mov(dword[rax], charge_at_tail ? block_cycles : 0);
#else
		mov(dword[rip + &cycle_counter], charge_at_tail ? block_cycles : 0);
#endif
		L(over);
	}

	/* On to the block at @target: by a link site (see DynaRBI), which is
	 * the block's way out to its BranchBlock (@which 0) or its NextBlock
	 * (1). A block in the temporary cache is not linked: it goes through
	 * the target's place in the table. So does a block that goes round to
	 * its own start. Linked to itself, such a loop came out faster or
	 * slower by the loop - one of shifts and adds a twelfth faster, one of
	 * stores to memory a twelfth slower, on the one processor it was tried
	 * on - and two games slower rather than faster: left as it was. */
	void GenGoOn(RuntimeBlockInfo *block, int which)
	{
		const u32 target = which == 0 ? block->BranchBlock : block->NextBlock;
		// where the target is, for the table of blocks
		u32 place = target;

		mov(edx, target);
		if (mmu_enabled())
		{
			/* With the MMU on the context has the address as well: a
			 * block that checks its code goes by it, and so does whatever
			 * finds that there is an exception to raise instead. */
			mov(Ctx(&next_pc), edx);
			rdv_MmuSamePlace(block, target, &place);
		}
		GenSliceCheck();
		if (block->temp_block || target == block->vaddr || link_stub == NULL)
		{
			jmp(qword[r15 + FpcbAt((place >> 1) & FPCB_MASK)]);
			return;
		}
		((DynaRBI *)block)->link_at[which] = (u32)getSize();
		call((const void *)link_stub);
		db(0xCC);
		db(0xCC);
	}

	/* Where entry @index of the table of blocks is from r15. The table is
	 * the first thing in the block of memory the context is at the end
	 * of, so r15, which every block has, reaches it: the table's address
	 * does not have to be loaded - ten bytes and an instruction less at
	 * every way out of a block. */
	static int FpcbAt(u32 index)
	{
		return fpcb_at(index);
	}

	/* With the MMU on an access is a call. First, though, the table of
	 * translations kept by (mmu.h): on a hit, rax is what to add to the
	 * address to be at the page on the host; the call is for a miss, and
	 * fills the table in. The address is in call_regs[0]; rax, r10 and r11
	 * belong to nobody here. */
	bool GenMmuLookup(const uintptr_t *table, u32 size, Xbyak::Label& miss)
	{
#ifdef MMU_HOST_PAGE_LUT
		if (!mmu_enabled())
			return false;
		/* Not for an address that is not a multiple of the access's size:
		 * that is an address error, which the call raises - and which a
		 * page already in the table used to get past, reading on into the
		 * next page of the host's memory at the end of one. 64 bits are
		 * two accesses of 32 to the SH4, so a multiple of 4 will do for
		 * them - but then the second half must not be in the next page,
		 * which is another page altogether: the call's again. (They used
		 * all to be the call's. A game moves its vertices 64 bits at a
		 * time.) */
		if (size == 2 || size == 4 || size == 8)
		{
			test(call_regs[0], size == 2 ? 1 : 3);
			jnz(miss);
		}
		if (size == 8)
		{
			lea(eax, ptr[call_regs64[0] + 4]);
			test(eax, 0xFFF);
			jz(miss);
		}
		mov(eax, call_regs[0]);
		shr(eax, 12);
		mov(r10, (uintptr_t)table);
		mov(rax, qword[r10 + rax * 8]);
		test(rax, rax);
		jz(miss);
		return true;
#else
		return false;
#endif
	}

	void GenReadMemorySlow(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		u32 size = op.flags & 0x7f;
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
				movsx(eax, byte[rax + call_regs64[0]]);
				break;
			case 2:
				movsx(eax, word[rax + call_regs64[0]]);
				break;
			case 8:
				mov(rax, qword[rax + call_regs64[0]]);
				break;
			default:
				mov(eax, dword[rax + call_regs64[0]]);
				break;
			}
			jmp(lut_done);
			L(lut_miss);
		}
		if (mmu_enabled())
			mov(call_regs[1], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

		switch (size) {
		case 1:
			if (!mmu_enabled())
				GenCall(ReadMem8);
			else
				GenCall(ReadMemNoEx<u8>, true);
			movsx(eax, al);
			break;
		case 2:
			if (!mmu_enabled())
				GenCall(ReadMem16);
			else
				GenCall(ReadMemNoEx<u16>, true);
			movsx(eax, ax);
			break;

		case 4:
			if (!mmu_enabled())
				GenCall(ReadMem32);
			else
				GenCall(ReadMemNoEx<u32>, true);
			break;
		case 8:
			if (!mmu_enabled())
				GenCall(ReadMem64);
			else
				GenCall(ReadMemNoEx<u64>, true);
			break;
		default:
			die("1..8 bytes");
		}
		if (lut)
			L(lut_done);
	}

	void GenWriteMemorySlow(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		u32 size = op.flags & 0x7f;
		Xbyak::Label lut_miss, lut_done;
#ifdef MMU_HOST_PAGE_LUT
		const bool lut = GenMmuLookup(mmu_write_lut, size, lut_miss);
#else
		const bool lut = false;
#endif
		if (lut)
		{
			// the data is in call_regs[1]
			switch (size) {
			case 1:
				mov(byte[rax + call_regs64[0]], call_regs[1].cvt8());
				break;
			case 2:
				mov(word[rax + call_regs64[0]], call_regs[1].cvt16());
				break;
			case 8:
				mov(qword[rax + call_regs64[0]], call_regs64[1]);
				break;
			default:
				mov(dword[rax + call_regs64[0]], call_regs[1]);
				break;
			}
			jmp(lut_done);
			L(lut_miss);
		}
		if (mmu_enabled())
			mov(call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

		switch (size) {
		case 1:
			if (!mmu_enabled())
				GenCall(WriteMem8);
			else
				GenCall(WriteMemNoEx<u8>, true);
			break;
		case 2:
			if (!mmu_enabled())
				GenCall(WriteMem16);
			else
				GenCall(WriteMemNoEx<u16>, true);
			break;
		case 4:
			if (!mmu_enabled())
				GenCall(WriteMem32);
			else
				GenCall(WriteMemNoEx<u32>, true);
			break;
		case 8:
			if (!mmu_enabled())
				GenCall(WriteMem64);
			else
				GenCall(WriteMemNoEx<u64>, true);
			break;
		default:
			die("1..8 bytes");
		}
		if (lut)
			L(lut_done);
	}

	void ngen_CC_Start(const shil_opcode& op)
	{
		CC_pars.clear();
	}

	void ngen_CC_param(const shil_opcode& op, const shil_param& prm, CanonicalParamType tp) {
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


		//store from EAX
		case CPT_u64rvL:
		case CPT_u32rv:
			mov(rcx, rax);
			host_reg_to_shil_param(prm, ecx);
			break;

		case CPT_u64rvH:
			// assuming CPT_u64rvL has just been called
			shr(rcx, 32);
			host_reg_to_shil_param(prm, ecx);
			break;

		// store from xmm0
		case CPT_f32rv:
			host_reg_to_shil_param(prm, xmm0);
#ifdef EXPLODE_SPANS
			// The x86 dynarec saves to mem as well
			//lea(rax, Ctx(prm.reg_ptr()));
			//movd(dword[rax], xmm0);
#endif
			break;
		}
	}

	void ngen_CC_Call(const shil_opcode& op, void* function)
	{
		int regused = 0;
		int xmmused = 0;

		for (int i = CC_pars.size(); i-- > 0;)
		{
         verify(xmmused < 4 && regused < 4);
			const shil_param& prm = *CC_pars[i].prm;
			switch (CC_pars[i].type) {
            //push the contents

            case CPT_u32:
               shil_param_to_host_reg(prm, call_regs[regused++]);
               break;

            case CPT_f32:
               shil_param_to_host_reg(prm, call_regsxmm[xmmused++]);
               break;

               //push the ptr itself
            case CPT_ptr:
               verify(prm.is_reg());

               mov(call_regs64[regused++], (size_t)prm.reg_ptr());

               break;
            default:
               // Other cases handled in ngen_CC_param
               break;
			}
		}
		GenCall((void (*)())function);
	}

	// Something in the SH4's context, from r15: where it is, and 32 bits of it
	Xbyak::RegExp CtxAt(const void *p)
	{
		const ptrdiff_t at = (const u8*)p - (const u8*)&p_sh4rcb->cntx;
		verify(at >= 0 && at < (ptrdiff_t)sizeof(Sh4Context));
		return r15 + ((int)at - CTX_BIAS);
	}

	Xbyak::Address Ctx(const void *p)
	{
		return dword[CtxAt(p)];
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

private:
	typedef void (BlockCompiler::*X64BinaryOp)(const Xbyak::Operand&, const Xbyak::Operand&);
	typedef void (BlockCompiler::*X64BinaryFOp)(const Xbyak::Xmm&, const Xbyak::Operand&);

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

		if (isram)
		{
			// Immediate pointer to RAM: super-duper fast access
			mov(rax, reinterpret_cast<uintptr_t>(ptr));
			switch (size)
			{
			case 1:
				if (regalloc.IsAllocg(op.rd))
					movsx(regalloc.MapRegister(op.rd), byte[rax]);
				else
				{
					movsx(eax, byte[rax]);
					lea(rcx, Ctx(op.rd.reg_ptr()));
					mov(dword[rcx], eax);
				}
				break;

			case 2:
				if (regalloc.IsAllocg(op.rd))
					movsx(regalloc.MapRegister(op.rd), word[rax]);
				else
				{
					movsx(eax, word[rax]);
					lea(rcx, Ctx(op.rd.reg_ptr()));
					mov(dword[rcx], eax);
				}
				break;

			case 4:
				if (regalloc.IsAllocg(op.rd))
					mov(regalloc.MapRegister(op.rd), dword[rax]);
				else if (regalloc.IsAllocf(op.rd))
					movd(regalloc.MapXRegister(op.rd), dword[rax]);
				else
				{
					mov(eax, dword[rax]);
					lea(rcx, Ctx(op.rd.reg_ptr()));
					mov(dword[rcx], eax);
				}
				break;

			case 8:
				mov(rcx, qword[rax]);
#ifdef EXPLODE_SPANS
				if (op.rd.count() == 2 && regalloc.IsAllocf(op.rd, 0) && regalloc.IsAllocf(op.rd, 1))
				{
					movd(regalloc.MapXRegister(op.rd, 0), ecx);
					shr(rcx, 32);
					movd(regalloc.MapXRegister(op.rd, 1), ecx);
				}
				else
#endif
				{
					lea(rax, Ctx(op.rd.reg_ptr()));
					mov(qword[rax], rcx);
				}
				break;

			default:
				die("Invalid immediate size");
            break;
			}
		}
		else
		{
			// Not RAM: the returned pointer is a memory handler
			if (size == 8)
			{
				verify(!regalloc.IsAllocAny(op.rd));

				// Need to call the handler twice
			mov(call_regs[0], addr);
				GenCall((void (*)())ptr);
				lea(rcx, Ctx(op.rd.reg_ptr()));
				mov(dword[rcx], eax);

				mov(call_regs[0], addr + 4);
				GenCall((void (*)())ptr);
				mov(rcx, (size_t)op.rd.reg_ptr() + 4);
				mov(dword[rcx], eax);
			}
			else
			{
				mov(call_regs[0], addr);

			switch(size)
			{
			case 1:
				GenCall((void (*)())ptr);
				movsx(eax, al);
				break;

			case 2:
				GenCall((void (*)())ptr);
				movsx(eax, ax);
				break;

			case 4:
				GenCall((void (*)())ptr);
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
			// Immediate pointer to RAM: super-duper fast access
			mov(rax, reinterpret_cast<uintptr_t>(ptr));
			switch (size)
			{
			case 1:
				if (regalloc.IsAllocg(op.rs2))
					mov(byte[rax], regalloc.MapRegister(op.rs2).cvt8());
				else if (op.rs2.is_imm())
					mov(byte[rax], (u8)op.rs2._imm);
				else
				{
					lea(rcx, Ctx(op.rs2.reg_ptr()));
					mov(cl, byte[rcx]);
					mov(byte[rax], cl);
				}
				break;

			case 2:
				if (regalloc.IsAllocg(op.rs2))
					mov(word[rax], regalloc.MapRegister(op.rs2).cvt16());
				else if (op.rs2.is_imm())
					mov(word[rax], (u16)op.rs2._imm);
				else
				{
					lea(rcx, Ctx(op.rs2.reg_ptr()));
					mov(cx, word[rcx]);
					mov(word[rax], cx);
				}
				break;

			case 4:
				if (regalloc.IsAllocg(op.rs2))
					mov(dword[rax], regalloc.MapRegister(op.rs2));
				else if (regalloc.IsAllocf(op.rs2))
					movd(dword[rax], regalloc.MapXRegister(op.rs2));
				else if (op.rs2.is_imm())
					mov(dword[rax], op.rs2._imm);
				else
				{
					lea(rcx, Ctx(op.rs2.reg_ptr()));
					mov(ecx, dword[rcx]);
					mov(dword[rax], ecx);
				}
				break;

			case 8:
#ifdef EXPLODE_SPANS
				if (op.rs2.count() == 2 && regalloc.IsAllocf(op.rs2, 0) && regalloc.IsAllocf(op.rs2, 1))
				{
					movd(call_regs[1], regalloc.MapXRegister(op.rs2, 1));
					shl(call_regs64[1], 32);
					movd(eax, regalloc.MapXRegister(op.rs2, 0));
					or_(call_regs64[1], rax);
				}
				else
#endif
				{
					lea(rcx, Ctx(op.rs2.reg_ptr()));
					mov(rcx, qword[rcx]);
					mov(qword[rax], rcx);
				}
				break;

			default:
				die("Invalid immediate size");
				break;
			}
		}
		else
		{
			// Not RAM: the returned pointer is a memory handler
			mov(call_regs[0], addr);
			shil_param_to_host_reg(op.rs2, call_regs[1]);

			GenCall((void (*)())ptr);
		}

		return true;
	}

public:
	/* Whether a load or store can be done as one move from or to the
	 * guest's memory as the host has it mapped, in place of a call: with
	 * the MMU off - every game but the Windows CE ones - and where the
	 * host has room to map the whole 4 GB the SH4 can address. (With the
	 * MMU on it is the table of pages: GenMmuLookup().)
	 *
	 * What is not memory - registers, the BIOS, the store queues - is not
	 * mapped. A move there faults, and ngen_Rewrite() turns it into the
	 * call it would have been, once. A store to a page that is watched,
	 * because code was compiled from it or a texture read from it, faults
	 * too, and is let through by the handlers that watch. */
	static bool FastMemory()
	{
		return !mmu_enabled() && _nvmem_4gb_space();
	}

	/* Where the SH4's memory as the host has it mapped is from r15. The
	 * mapping comes straight after the block that holds the context, which
	 * is what r15 points into, so a fast access is one instruction:
	 * [r15 + address + this]. (It was two: the mapping's 64-bit address
	 * into rax, and then the move.) */
	static int MemFromCtx()
	{
		return (int)((ptrdiff_t)sizeof(Sh4RCB) - (ptrdiff_t)CTX_BASE);
	}

	/* Whether what is at @code is a fast access as GenFastAccess() writes
	 * them, and if so which kind, with which registers (as mem_stubs
	 * numbers them) and how long it is. A fast access is told by what it
	 * is, so nothing has to be kept about where in a block they are: one
	 * of ten moves, to or from [r15 + a register + MemFromCtx()]. Nothing
	 * else a block has looks like that. */
	static bool FastAccessAt(const u8 *code, int *kind, int *addr, int *data, int *length)
	{
		const u8 *p = code;
		u8 prefix = 0;

		if (*p == 0x66 || *p == 0xF3)
			prefix = *p++;
		// REX, with B for r15
		const u8 rex = *p++;
		if ((rex & 0xF1) != 0x41)
			return false;
		const bool wide = (rex & 8) != 0;
		if (p[0] == 0x0F && !wide)
		{
			if (p[1] == 0xBE && prefix == 0)
				*kind = MEM_LOAD8;
			else if (p[1] == 0xBF && prefix == 0)
				*kind = MEM_LOAD16;
			else if (p[1] == 0x10 && prefix == 0xF3)
				*kind = MEM_LOAD32F;
			else if (p[1] == 0x11 && prefix == 0xF3)
				*kind = MEM_STORE32F;
			else
				return false;
			p += 2;
		}
		else if (*p == 0x8B && prefix == 0)
		{
			*kind = wide ? MEM_LOAD64 : MEM_LOAD32;
			p++;
		}
		else if (*p == 0x88 && prefix == 0 && !wide)
		{
			*kind = MEM_STORE8;
			p++;
		}
		else if (*p == 0x89 && prefix != 0xF3 && !(prefix != 0 && wide))
		{
			*kind = prefix != 0 ? MEM_STORE16 : wide ? MEM_STORE64 : MEM_STORE32;
			p++;
		}
		else
			return false;

		// [base + index + a distance]: r15, a register times one, and that distance
		const u8 modrm = *p++;
		const u8 sib = *p++;
		if ((modrm & 7) != 4 || (sib & 0xC7) != 0x07)
			return false;
		if ((modrm >> 6) == 2)
		{
			s32 distance;
			memcpy(&distance, p, 4);
			if (distance != MemFromCtx())
				return false;
			p += 4;
		}
		else if ((modrm >> 6) == 1)
		{
			if ((s8)*p++ != MemFromCtx())
				return false;
		}
		else
			return false;

		const int index = ((sib >> 3) & 7) | ((rex & 2) != 0 ? 8 : 0);
		const int reg = ((modrm >> 3) & 7) | ((rex & 4) != 0 ? 8 : 0);
		const bool load = *kind < MEM_STORE8;

		*addr = mem_reg_number(index, MEM_REG_ADDR);
		if (MEM_KIND_F(*kind))
			*data = mem_freg_number(reg);
		else
		{
			*data = mem_reg_number(reg, load ? Xbyak::Operand::RAX : MEM_REG_STORE);
			if ((*kind == MEM_LOAD64 || *kind == MEM_STORE64) && *data != 0)
				return false;
		}
		if (*addr < 0 || *data < 0)
			return false;
		*length = (int)(p - code);
		return true;
	}

	/* A fast access of @kind: the SH4's address is in the general register
	 * @addr, and the data in, or wanted in, register @data - a general
	 * one, or a floating-point one for the two kinds that are. */
	void GenFastAccess(int kind, int addr, int data)
	{
		const u8 *start_addr = getCurr();
		const Xbyak::RegExp at = r15 + Xbyak::Reg64(addr) * 1 + MemFromCtx();

		switch (kind)
		{
		case MEM_LOAD8:    movsx(Xbyak::Reg32(data), byte[at]); break;
		case MEM_LOAD16:   movsx(Xbyak::Reg32(data), word[at]); break;
		case MEM_LOAD32:   mov(Xbyak::Reg32(data), dword[at]); break;
		case MEM_LOAD32F:  movss(Xbyak::Xmm(data), dword[at]); break;
		case MEM_LOAD64:   mov(Xbyak::Reg64(data), qword[at]); break;
		case MEM_STORE8:   mov(byte[at], Xbyak::Reg32(data).cvt8()); break;
		case MEM_STORE16:  mov(word[at], Xbyak::Reg32(data).cvt16()); break;
		case MEM_STORE32:  mov(dword[at], Xbyak::Reg32(data)); break;
		case MEM_STORE32F: movss(dword[at], Xbyak::Xmm(data)); break;
		case MEM_STORE64:  mov(qword[at], Xbyak::Reg64(data)); break;
		default:           die("no such access");
		}
		// ngen_Rewrite() has to know it again, and needs five bytes for its call
		int k, a, d, length;
		verify(FastAccessAt(start_addr, &k, &a, &d, &length) && k == kind
				&& length == (int)(getCurr() - start_addr) && length >= 5);
	}

	// The address of a load or store, into call_regs[0]
	void GenMemAddr(const shil_opcode& op)
	{
		if (!op.rs3.is_null() && op.rs1.is_reg() && regalloc.IsAllocg(op.rs1))
		{
			// one instruction for the two of them, where both are to hand
			const Xbyak::Reg64 base = regalloc.MapRegister(op.rs1).cvt64();

			if (op.rs3.is_imm())
			{
				lea(call_regs[0], ptr[base + (int)(s32)op.rs3._imm]);
				return;
			}
			if (regalloc.IsAllocg(op.rs3))
			{
				lea(call_regs[0], ptr[base + regalloc.MapRegister(op.rs3).cvt64()]);
				return;
			}
		}
		shil_param_to_host_reg(op.rs1, call_regs[0]);
		if (!op.rs3.is_null())
		{
			if (op.rs3.is_imm())
				add(call_regs[0], op.rs3._imm);
			else if (regalloc.IsAllocg(op.rs3))
				add(call_regs[0], regalloc.MapRegister(op.rs3));
			else
				add(call_regs[0], Ctx(op.rs3.reg_ptr()));
		}
	}

	/* The same for a fast access, which takes it from any register: the
	 * one the block has it in, where there is nothing to add to it. (A
	 * register of the block's has nothing above its low 32 bits: every
	 * instruction that writes one writes 32.) */
	int GenFastAddr(const shil_opcode& op)
	{
		if (op.rs3.is_null() && op.rs1.is_reg() && regalloc.IsAllocg(op.rs1))
			return regalloc.MapRegister(op.rs1).getIdx();
		GenMemAddr(op);
		return call_regs[0].getIdx();
	}

	/* A load as one instruction, from the SH4's memory as the host has it
	 * mapped straight into the register the block wants it in. False if
	 * that is not to be had (FastMemory()), and nothing written. */
	bool GenReadMemoryFast(const shil_opcode& op)
	{
		const u32 size = op.flags & 0x7f;

		if (!FastMemory())
			return false;
#ifdef EXPLODE_SPANS
		if (size == 8)
			return false;
#endif
		const int addr = GenFastAddr(op);

		if (size == 8)
		{
			GenFastAccess(MEM_LOAD64, addr, Xbyak::Operand::RAX);
			mov(qword[CtxAt(op.rd.reg_ptr())], rax);
			return true;
		}
		const int kind = size == 1 ? MEM_LOAD8 : size == 2 ? MEM_LOAD16 : MEM_LOAD32;
		if (regalloc.IsAllocg(op.rd))
			GenFastAccess(kind, addr, regalloc.MapRegister(op.rd).getIdx());
		else if (size == 4 && regalloc.IsAllocf(op.rd))
			GenFastAccess(MEM_LOAD32F, addr, regalloc.MapXRegister(op.rd).getIdx());
		else
		{
			GenFastAccess(kind, addr, Xbyak::Operand::RAX);
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
#ifdef EXPLODE_SPANS
		if (size == 8)
			return false;
#endif
		const int addr = GenFastAddr(op);

		if (size == 8)
		{
			mov(call_regs64[1], qword[CtxAt(op.rs2.reg_ptr())]);
			GenFastAccess(MEM_STORE64, addr, call_regs64[1].getIdx());
			return true;
		}
		const int kind = size == 1 ? MEM_STORE8 : size == 2 ? MEM_STORE16 : MEM_STORE32;
		if (op.rs2.is_reg() && regalloc.IsAllocg(op.rs2))
			GenFastAccess(kind, addr, regalloc.MapRegister(op.rs2).getIdx());
		else if (size == 4 && op.rs2.is_reg() && regalloc.IsAllocf(op.rs2))
			GenFastAccess(MEM_STORE32F, addr, regalloc.MapXRegister(op.rs2).getIdx());
		else
		{
			shil_param_to_host_reg(op.rs2, call_regs[1]);
			GenFastAccess(kind, addr, call_regs[1].getIdx());
		}
		return true;
	}

	void CheckBlock(RuntimeBlockInfo* block) {
	   mov(call_regs[0], block->addr);

		// FIXME This test shouldn't be necessary
		// However the decoder makes various assumptions about the current PC value, which are simply not
		// true in a virtualized memory model. So this can only work if virtual and phy addresses are the
		// same at compile and run times.
		if (mmu_enabled())
		{
			mov(rax, (uintptr_t)&next_pc);
			cmp(dword[rax], block->vaddr);
			jne(check_failed, T_NEAR);
		}

	   s32 sz=block->sh4_code_size;
	   u32 sa=block->addr;

	   while (sz > 0)
	   {
		  void* ptr = (void*)GetMemPtr(sa, sz > 8 ? 8 : sz);
		  if (ptr)
		  {
			 uintptr_t uintptr = reinterpret_cast<uintptr_t>(ptr);
			 mov(rax, uintptr);

			 if (sz >= 8 && !(uintptr & 7)) {
				mov(rdx, *(u64*)ptr);
				cmp(qword[rax], rdx);
				sz -= 8;
				sa += 8;
			 }
			 else if (sz >= 4 && !(uintptr & 3)) {
				mov(edx, *(u32*)ptr);
				cmp(dword[rax], edx);
				sz -= 4;
				sa += 4;
			 }
			 else {
				mov(edx, *(u16*)ptr);
				cmp(word[rax],dx);
				sz -= 2;
				sa += 2;
			 }
			 jne(check_failed, T_NEAR);
		  }
	   }
	}

	void GenBinaryOp(const shil_opcode &op, X64BinaryOp natop)
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
			/* The constant goes in the instruction, not through a register */
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

	void GenBinaryFOp(const shil_opcode &op, X64BinaryFOp natop)
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

	template<class Ret, class... Params>
	void GenCall(Ret(*function)(Params...), bool skip_floats = false)
	{
#ifndef _WIN32
		/* The floating-point registers handed out are xmm8 to xmm15, and
		 * none of them is kept by a function on these hosts: the ones in
		 * use at this point are saved round the call. */
		int saved[8];
		int saved_count = 0;
		u32 stack_size = 0;
		if (!skip_floats && current_opid != (size_t)-1)
			for (int i = 8; i < 16; i++)
				if (regalloc.IsMapped(Xbyak::Xmm(i), current_opid))
					saved[saved_count++] = i;
		if (saved_count != 0)
		{
			stack_size = (((4 * saved_count + 15) >> 4) << 4); // Stack needs to be 16-byte aligned before the call
			sub(rsp, stack_size);
			for (int i = 0; i < saved_count; i++)
				movd(ptr[rsp + i * 4], Xbyak::Xmm(saved[i]));
		}
#endif

		call(CC_RX2RW(function));

#ifndef _WIN32
		if (saved_count != 0)
		{
			for (int i = 0; i < saved_count; i++)
				movd(Xbyak::Xmm(saved[i]), ptr[rsp + i * 4]);
			add(rsp, stack_size);
		}
#endif
	}

	// uses eax/rax for an immediate into a floating-point register
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
					verify(!reg.isXMM());
					mov((const Xbyak::Reg32 &)reg, Ctx(param.reg_ptr()));
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
			// (straight there: by way of rax, it was lost if it was in rax)
			if (!reg.isXMM())
				mov(Ctx(param.reg_ptr()), (const Xbyak::Reg32 &)reg);
			else
				movss(Ctx(param.reg_ptr()), (const Xbyak::Xmm &)reg);
		}
	}

   std::vector<Xbyak::Reg32> call_regs;
   std::vector<Xbyak::Reg64> call_regs64;
   std::vector<Xbyak::Xmm> call_regsxmm;

	struct CC_PS
	{
	   CanonicalParamType type;
	   const shil_param* prm;
	};
   std::vector<CC_PS> CC_pars;

	X64RegAlloc regalloc;
	static Xbyak::util::Cpu cpu;
	size_t current_opid;
	Xbyak::Label exit_block;
	Xbyak::Label slice_out;
	Xbyak::Label check_failed;
	bool slice_out_used = false;
	u32 block_cycles = 0;
	bool charge_at_tail = false;
};

void X64RegAlloc::Preload(u32 reg, Xbyak::Operand::Code nreg)
{
   compiler->RegPreload(reg, nreg);
}
void X64RegAlloc::Writeback(u32 reg, Xbyak::Operand::Code nreg)
{
   compiler->RegWriteback(reg, nreg);
}
void X64RegAlloc::Preload_FPU(u32 reg, s8 nreg)
{
   compiler->RegPreload_FPU(reg, nreg);
}
void X64RegAlloc::Writeback_FPU(u32 reg, s8 nreg)
{
   compiler->RegWriteback_FPU(reg, nreg);
}

static BlockCompiler* compiler;

/* CPU feature detection is process-invariant; run it once instead of on every
   block compile. A per-instance Cpu member re-ran the full cpuid battery
   (~100us) for every BlockCompiler, dominating compile time. */
Xbyak::util::Cpu BlockCompiler::cpu;

void ngen_Compile(RuntimeBlockInfo* block, bool force_checks, bool reset, bool staging, bool optimise)
{
	verify(CPU_RUNNING == offsetof(Sh4RCB, cntx.CpuRunning));
	verify(PC == offsetof(Sh4RCB, cntx.pc));
	verify(CTX_BASE == offsetof(Sh4RCB, cntx) + CTX_BIAS);
	verify(emit_FreeSpace() >= 16 * 1024);

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
 * asking, with the registers this one has (mem_stubs), and run again from
 * where it was. It is five bytes or more, which is what a call takes, so
 * there is no room kept after it for this; what is left of it is filled
 * in. */
bool ngen_Rewrite(unat& host_pc, unat, unat)
{
	int kind, addr, data, length;

	if (!BlockCompiler::FastMemory())
		return false;
	if (bm_GetBlock2((void *)host_pc) == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: Block at %p not found", (void *)host_pc);
		return false;
	}
	if (!BlockCompiler::FastAccessAt((const u8 *)host_pc, &kind, &addr, &data, &length)
			|| mem_stubs[kind][addr][data] == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: no memory access at %p", (void *)host_pc);
		return false;
	}

	// (a block is as far from the stubs where it is run as where it is written)
	u8 *const site = (u8 *)CC_RX2RW((u8 *)host_pc);
	const s32 rel = (s32)(mem_stubs[kind][addr][data] - (site + 5));
	static const u8 fill[6][5] = { { 0 }, { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 }, { 0x0F, 0x1F, 0x40, 0x00 },
		{ 0x0F, 0x1F, 0x44, 0x00, 0x00 } };

	verify(length >= 5 && length <= 10);
	site[0] = 0xE8;
	memcpy(site + 1, &rel, 4);
	memcpy(site + 5, fill[length - 5], length - 5);
	return true;
}

void ngen_HandleException()
{
	longjmp(jmp_env, 1);
}
#endif
