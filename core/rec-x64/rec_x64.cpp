#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X64
#include <setjmp.h>
//#define EXPLODE_SPANS

#include "deps/xbyak/xbyak.h"
#include "deps/xbyak/xbyak_util.h"

#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_interrupts.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/mem/vmem32.h"
#include "x64_regalloc.h"
#include "x64_vector.h"

struct DynaRBI : RuntimeBlockInfo
{
   virtual u32 Relink() {
      return 0;
   }

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
                        "call *%rax                                             \n\t"
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

void ngen_init()
{
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
	u32 exception_raised;
	T rv = mmu_ReadMemNoEx<T>(addr, &exception_raised);
	handle_mem_exception(exception_raised, pc);

	return rv;
#else
	// not used
	return (T)0;
#endif
}

template<typename T>
static void WriteMemNoEx(u32 addr, T data, u32 pc)
{
#ifndef NO_MMU
	u32 exception_raised = mmu_WriteMemNoEx<T>(addr, data);
	handle_mem_exception(exception_raised, pc);
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

#ifdef _WIN32
		sub(rsp, 0x28);		// 32-byte shadow space + 8 byte alignment
#else
		sub(rsp, 0x8);		// align stack
#endif

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
#ifdef FEAT_NO_RWX_PAGES
		// Use absolute addressing for this one
		// TODO(davidgfnet) remove the ifsef using CC_RX2RW/CC_RW2RX
		mov(rax, (uintptr_t)&cycle_counter);
		sub(dword[rax], block->guest_cycles);
#else
		sub(dword[rip + &cycle_counter], block->guest_cycles);
#endif
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
				  lea(rax, Ctx(op.rs1.reg_ptr()));
				  mov(rax, qword[rax]);
				  lea(rcx, Ctx(op.rd.reg_ptr()));
				  mov(qword[rcx], rax);
#endif
               }
               break;

            case shop_readm:
            	if (!GenReadMemImmediate(op, block))
               {
						// Not an immediate address
            		shil_param_to_host_reg(op.rs1, call_regs[0]);
						if (!op.rs3.is_null())
						{
							if (op.rs3.is_imm())
								add(call_regs[0], op.rs3._imm);
							else if (regalloc.IsAllocg(op.rs3))
								add(call_regs[0], regalloc.MapRegister(op.rs3));
							else
							{
								lea(rax, Ctx(op.rs3.reg_ptr()));
								add(call_regs[0], dword[rax]);
							}
						}
						if (!optimise || !GenReadMemoryFast(op, block))
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
								lea(rcx, Ctx(op.rd.reg_ptr()));
								mov(qword[rcx], rax);
							}
						}
               }
               break;

            case shop_writem:
               {
						if (!GenWriteMemImmediate(op, block))
						{
							shil_param_to_host_reg(op.rs1, call_regs[0]);
							if (!op.rs3.is_null())
							{
								if (op.rs3.is_imm())
									add(call_regs[0], op.rs3._imm);
								else if (regalloc.IsAllocg(op.rs3))
									add(call_regs[0], regalloc.MapRegister(op.rs3));
								else
								{
									lea(rax, Ctx(op.rs3.reg_ptr()));
									add(call_regs[0], dword[rax]);
								}
							}

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
									lea(rax, Ctx(op.rs2.reg_ptr()));
									mov(call_regs64[1], qword[rax]);
								}
							}
							if (!optimise || !GenWriteMemoryFast(op, block))
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
					jne(no_sqw);

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
		 * Not with the MMU on: an address then has to be translated before
		 * it means a place in the table, and the lookup function does that. */
		const bool go_on = !mmu_enabled();

	  switch (block->BlockType) {

		case BET_StaticJump:
		case BET_StaticCall:
			//next_pc = block->BranchBlock;
			mov(Ctx(&next_pc), block->BranchBlock);
			if (go_on)
				GenGoOn(block->BranchBlock);
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
				mov(Ctx(&next_pc), block->BranchBlock);
				if (go_on)
					GenGoOn(block->BranchBlock);
				else
					jmp(exit_block, T_NEAR);
				L(branch_not_taken);
				mov(Ctx(&next_pc), block->NextBlock);
				if (go_on)
					GenGoOn(block->NextBlock);
			}
			break;

		case BET_DynamicJump:
		case BET_DynamicCall:
		case BET_DynamicRet:
			//next_pc = *jdyn;
			mov(edx, Ctx(&Sh4cntx.jdyn));
			mov(Ctx(&next_pc), edx);
			if (go_on)
			{
				// the address is only known now: its place in the table is worked out here
				GenSliceCheck();
				shr(edx, 1);
				and_(edx, FPCB_MASK);
				mov(rcx, (uintptr_t)&p_sh4rcb->fpcb[0]);
				jmp(qword[rcx + rdx * 8]);
			}
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

			GenCall(UpdateINTC);
			break;

		default:
			die("Invalid block end type");
		}

		L(exit_block);
#ifdef _WIN32
		add(rsp, 0x28);
#else
		add(rsp, 0x8);
#endif
		ret();

		ready();

		block->code = (DynarecCodeEntryPtr)getCode();
		block->host_code_size = getSize();

		emit_Skip(getSize());
	}

	// Returns to the main loop if the time slice is used up; otherwise puts
	// the stack back as it was when the block was entered, for a jump on
	void GenSliceCheck()
	{
#ifdef FEAT_NO_RWX_PAGES
		mov(rcx, (uintptr_t)&cycle_counter);
		cmp(dword[rcx], 0);
#else
		cmp(dword[rip + &cycle_counter], 0);
#endif
		jle(exit_block, T_NEAR);
#ifdef _WIN32
		add(rsp, 0x28);
#else
		add(rsp, 0x8);
#endif
	}

	// On to the block at @target, through its place in the table
	void GenGoOn(u32 target)
	{
		GenSliceCheck();
		mov(rcx, (uintptr_t)&p_sh4rcb->fpcb[(target >> 1) & FPCB_MASK]);
		jmp(qword[rcx]);
	}

	void GenReadMemorySlow(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		const u8 *start_addr = getCurr();
		if (mmu_enabled())
			mov(call_regs[1], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

		u32 size = op.flags & 0x7f;
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

		// as long as the fast access it may be written over: see ngen_Rewrite()
		if (FastMemory())
		{
			Xbyak::Label quick_exit;
			if (getCurr() - start_addr <= read_mem_op_size - 6)
				jmp(quick_exit, T_NEAR);
			while (getCurr() - start_addr < read_mem_op_size)
				nop();
			L(quick_exit);
			verify(getCurr() - start_addr == read_mem_op_size);
		}
	}

	void GenWriteMemorySlow(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		const u8 *start_addr = getCurr();
		if (mmu_enabled())
			mov(call_regs[2], block->vaddr + op.guest_offs - (op.delay_slot ? 1 : 0));	// pc

		u32 size = op.flags & 0x7f;
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
		// as long as the fast access it may be written over: see ngen_Rewrite()
		if (FastMemory())
		{
			Xbyak::Label quick_exit;
			if (getCurr() - start_addr <= write_mem_op_size - 6)
				jmp(quick_exit, T_NEAR);
			while (getCurr() - start_addr < write_mem_op_size)
				nop();
			L(quick_exit);
			verify(getCurr() - start_addr == write_mem_op_size);
		}
	}

	void InitializeRewrite(RuntimeBlockInfo *block, size_t opid)
	{
		rewriting = true;
	}

	void FinalizeRewrite()
	{
		ready();
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

	// Something in the SH4's context, from r15
	Xbyak::Address Ctx(const void *p)
	{
		const ptrdiff_t at = (const u8*)p - (const u8*)&p_sh4rcb->cntx;
		verify(at >= 0 && at < (ptrdiff_t)sizeof(Sh4Context));
		return dword[r15 + ((int)at - CTX_BIAS)];
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
	 * guest's memory as the host has it mapped, in place of a call. With
	 * the MMU on that is the mapping of the guest's virtual addresses,
	 * where there is one. With it off - every game but the Windows CE ones -
	 * it is the mapping of the whole 4 GB the SH4 can address, where the
	 * host has room for that; this used to be for the MMU case only, and
	 * everything else called a function for every load and store.
	 *
	 * What is not memory - registers, the BIOS, the store queues - is not
	 * mapped. A move there faults, and ngen_Rewrite() turns it into the
	 * call it would have been, once; that is what the padding after it is
	 * for. A store to a page that is watched, because code was compiled
	 * from it or a texture read from it, faults too, and is let through by
	 * the handlers that watch. */
	static bool FastMemory()
	{
		return mmu_enabled() ? vmem32_enabled() : _nvmem_4gb_space();
	}

	// How far into its code a fast access's move is: with the MMU on there
	// is more in front of it
	static u32& MemAccessOffset()
	{
		return mmu_enabled() ? mem_access_offset : mem_access_offset_direct;
	}

	bool GenReadMemoryFast(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (!FastMemory())
			return false;
		const u8 *start_addr = getCurr();

		if (mmu_enabled())
		{
			mov(rax, (uintptr_t)&p_sh4rcb->cntx.exception_pc);
			mov(dword[rax], block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));
		}

		mov(rax, (uintptr_t)virt_ram_base);

		u32 size = op.flags & 0x7f;
		//verify(getCurr() - start_addr == 26);
		u32& access_offset = MemAccessOffset();
		if (access_offset == 0)
			access_offset = getCurr() - start_addr;
		else
			verify(getCurr() - start_addr == access_offset);

		block->memory_accesses[(void*)getCurr()] = (u32)current_opid;
		switch (size)
		{
		case 1:
			movsx(eax, byte[rax + call_regs64[0]]);
			break;

		case 2:
			movsx(eax, word[rax + call_regs64[0]]);
			break;

		case 4:
			mov(eax, dword[rax + call_regs64[0]]);
			break;

		case 8:
			mov(rax, qword[rax + call_regs64[0]]);
			break;

		default:
			die("1..8 bytes");
		}

		// room for the call this becomes if the access faults
		nop(read_mem_op_size - (getCurr() - start_addr));
		verify(getCurr() - start_addr == read_mem_op_size);

		return true;
	}

	bool GenWriteMemoryFast(const shil_opcode& op, RuntimeBlockInfo* block)
	{
		if (!FastMemory())
			return false;
		const u8 *start_addr = getCurr();

		if (mmu_enabled())
		{
			mov(rax, (uintptr_t)&p_sh4rcb->cntx.exception_pc);
			mov(dword[rax], block->vaddr + op.guest_offs - (op.delay_slot ? 2 : 0));
		}

		mov(rax, (uintptr_t)virt_ram_base);

		u32 size = op.flags & 0x7f;
		//verify(getCurr() - start_addr == 26);
		u32& access_offset = MemAccessOffset();
		if (access_offset == 0)
			access_offset = getCurr() - start_addr;
		else
			verify(getCurr() - start_addr == access_offset);

		block->memory_accesses[(void*)getCurr()] = (u32)current_opid;
		switch (size)
		{
		case 1:
			mov(byte[rax + call_regs64[0] + 0], call_regs[1].cvt8());
			break;

		case 2:
			mov(word[rax + call_regs64[0]], call_regs[1].cvt16());
			break;

		case 4:
			mov(dword[rax + call_regs64[0]], call_regs[1]);
			break;

		case 8:
			mov(qword[rax + call_regs64[0]], call_regs64[1]);
			break;

		default:
			die("1..8 bytes");
		}

		nop(write_mem_op_size - (getCurr() - start_addr));
		verify(getCurr() - start_addr == write_mem_op_size);

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
			jne(reinterpret_cast<const void*>(&ngen_blockcheckfail));
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
			 jne(reinterpret_cast<const void*>(&ngen_blockcheckfail));
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
	   	mov(ecx, op.rs2._imm);
			(this->*natop)(rd, ecx);
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
		if (rewriting && !mmu_enabled())
		{
			/* Written over a fast memory access that faulted. Which of the
			 * floating-point registers are in use here is not known any
			 * more, and with the MMU off they are not written back before
			 * a memory access as they are with it on: all eight are kept. */
			mov(rax, (uintptr_t)function);
			call((const void*)ngen_call_keep_xmm);
			return;
		}
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

	// uses eax/rax
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
	   			lea(rax, Ctx(param.reg_ptr()));
					verify(!reg.isXMM());
					mov((const Xbyak::Reg32 &)reg, dword[rax]);
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
					lea(rax, Ctx(param.reg_ptr()));
					if (!reg.isXMM())
						mov((const Xbyak::Reg32 &)reg, dword[rax]);
					else
						movss((const Xbyak::Xmm &)reg, dword[rax]);
				}
			}
		}
	   else
	   {
	   	verify(param.is_null());
	   }
	}

	// uses rax
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
			lea(rax, Ctx(param.reg_ptr()));
			if (!reg.isXMM())
				mov(dword[rax], (const Xbyak::Reg32 &)reg);
			else
				movss(dword[rax], (const Xbyak::Xmm &)reg);
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
	bool rewriting = false;	// writing a call over a fast memory access: see ngen_Rewrite()
	static const u32 read_mem_op_size;
	static const u32 write_mem_op_size;
public:
	static u32 mem_access_offset;
	static u32 mem_access_offset_direct;
};

const u32 BlockCompiler::read_mem_op_size = 30;
const u32 BlockCompiler::write_mem_op_size = 30;
u32 BlockCompiler::mem_access_offset = 0;
u32 BlockCompiler::mem_access_offset_direct = 0;

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

bool ngen_Rewrite(unat& host_pc, unat, unat)
{
	if (!BlockCompiler::FastMemory())
		return false;

	//printf("ngen_Rewrite pc %p\n", host_pc);
	RuntimeBlockInfoPtr block = bm_GetBlock2((void *)host_pc);
	if (block == NULL)
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: Block at %p not found", (void *)host_pc);
		return false;
	}
	u8 *code_ptr = (u8*)host_pc;
	auto it = block->memory_accesses.find(code_ptr);
	if (it == block->memory_accesses.end())
	{
		WARN_LOG(DYNAREC, "ngen_Rewrite: memory access at %p not found (%lu entries)", code_ptr, block->memory_accesses.size());
		return false;
	}
	u32 opid = it->second;
	verify(opid < block->oplist.size());
	const shil_opcode& op = block->oplist[opid];

	BlockCompiler *assembler = new BlockCompiler(code_ptr - BlockCompiler::MemAccessOffset());
	assembler->InitializeRewrite(block.get(), opid);
	if (op.op == shop_readm)
		assembler->GenReadMemorySlow(op, block.get());
	else
		assembler->GenWriteMemorySlow(op, block.get());
	assembler->FinalizeRewrite();
	verify(block->host_code_size >= assembler->getSize());
	delete assembler;
	block->memory_accesses.erase(it);
	host_pc = (unat)(code_ptr - BlockCompiler::MemAccessOffset());

	return true;
}

void ngen_HandleException()
{
	longjmp(jmp_env, 1);
}
#endif
