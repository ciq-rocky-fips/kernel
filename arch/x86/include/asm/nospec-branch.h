/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_X86_NOSPEC_BRANCH_H_
#define _ASM_X86_NOSPEC_BRANCH_H_

#include <linux/static_key.h>
#include <linux/objtool.h>

#include <asm/alternative.h>
#include <asm/cpufeatures.h>
#include <asm/msr-index.h>
#include <asm/unwind_hints.h>
#include <asm/percpu.h>

/*
 * This should be used immediately before a retpoline alternative. It tells
 * objtool where the retpolines are so that it can make sense of the control
 * flow by just reading the original instruction(s) and ignoring the
 * alternatives.
 */
#define ANNOTATE_NOSPEC_ALTERNATIVE \
	ANNOTATE_IGNORE_ALTERNATIVE

/*
 * Fill the CPU return stack buffer.
 *
 * Each entry in the RSB, if used for a speculative 'ret', contains an
 * infinite 'pause; lfence; jmp' loop to capture speculative execution.
 *
 * This is required in various cases for retpoline and IBRS-based
 * mitigations for the Spectre variant 2 vulnerability. Sometimes to
 * eliminate potentially bogus entries from the RSB, and sometimes
 * purely to ensure that it doesn't get empty, which on some CPUs would
 * allow predictions from other (unwanted!) sources to be used.
 *
 * We define a CPP macro such that it can be used from both .S files and
 * inline assembly. It's possible to do a .macro and then include that
 * from C via asm(".include <asm/nospec-branch.h>") but let's not go there.
 */

#define RSB_CLEAR_LOOPS		32	/* To forcibly overwrite all entries */
#define RSB_FILL_LOOPS		16	/* To avoid underflow */

/*
 * Google experimented with loop-unrolling and this turned out to be
 * the optimal version - two calls, each with their own speculation
 * trap should their return address end up getting used, in a loop.
 */
#define __FILL_RETURN_BUFFER(reg, nr, sp)	\
	mov	$(nr/2), reg;			\
771:						\
	ANNOTATE_INTRA_FUNCTION_CALL;		\
	call	772f;				\
773:	/* speculation trap */			\
	UNWIND_HINT_EMPTY;			\
	pause;					\
	lfence;					\
	jmp	773b;				\
772:						\
	ANNOTATE_INTRA_FUNCTION_CALL;		\
	call	774f;				\
775:	/* speculation trap */			\
	UNWIND_HINT_EMPTY;			\
	pause;					\
	lfence;					\
	jmp	775b;				\
774:						\
	add	$(BITS_PER_LONG/8) * 2, sp;	\
	dec	reg;				\
	jnz	771b;				\
	/* barrier for jnz misprediction */	\
	lfence;

#ifdef __ASSEMBLY__

/*
 * This should be used immediately before an indirect jump/call. It tells
 * objtool the subsequent indirect jump/call is vouched safe for retpoline
 * builds.
 */
.macro ANNOTATE_RETPOLINE_SAFE
	.Lannotate_\@:
	.pushsection .discard.retpoline_safe
	_ASM_PTR .Lannotate_\@
	.popsection
.endm

/*
 * These are the bare retpoline primitives for indirect jmp and call.
 * Do not use these directly; they only exist to make the ALTERNATIVE
 * invocation below less ugly.
 */
.macro RETPOLINE_JMP reg:req
	call	.Ldo_rop_\@
.Lspec_trap_\@:
	pause
	lfence
	jmp	.Lspec_trap_\@
.Ldo_rop_\@:
	mov	\reg, (%_ASM_SP)
	ret
.endm

/*
 * This is a wrapper around RETPOLINE_JMP so the called function in reg
 * returns to the instruction after the macro.
 */
.macro RETPOLINE_CALL reg:req
	jmp	.Ldo_call_\@
.Ldo_retpoline_jmp_\@:
	RETPOLINE_JMP \reg
.Ldo_call_\@:
	call	.Ldo_retpoline_jmp_\@
.endm

/*
 * (ab)use RETPOLINE_SAFE on RET to annotate away 'bare' RET instructions
 * vs RETBleed validation.
 */
#define ANNOTATE_UNRET_SAFE ANNOTATE_RETPOLINE_SAFE

/*
 * JMP_NOSPEC and CALL_NOSPEC macros can be used instead of a simple
 * indirect jmp/call which may be susceptible to the Spectre variant 2
 * attack.
 */
.macro JMP_NOSPEC reg:req
#ifdef CONFIG_RETPOLINE
	ANNOTATE_NOSPEC_ALTERNATIVE
	ALTERNATIVE_2 __stringify(RETPOLINE_JMP \reg), \
		__stringify(lfence; ANNOTATE_RETPOLINE_SAFE; jmp *\reg; int3), X86_FEATURE_RETPOLINE_LFENCE, \
		__stringify(ANNOTATE_RETPOLINE_SAFE; jmp *\reg), ALT_NOT(X86_FEATURE_RETPOLINE)
#else
	jmp	*\reg
#endif
.endm

.macro CALL_NOSPEC reg:req
#ifdef CONFIG_RETPOLINE
	ANNOTATE_NOSPEC_ALTERNATIVE
	ALTERNATIVE_2 __stringify(ANNOTATE_RETPOLINE_SAFE; call *\reg),	\
		__stringify(RETPOLINE_CALL \reg), X86_FEATURE_RETPOLINE,\
		__stringify(lfence; ANNOTATE_RETPOLINE_SAFE; call *\reg), X86_FEATURE_RETPOLINE_LFENCE
#else
	call	*\reg
#endif
.endm

.macro ISSUE_UNBALANCED_RET_GUARD
	ANNOTATE_INTRA_FUNCTION_CALL
	call .Lunbalanced_ret_guard_\@
	int3
.Lunbalanced_ret_guard_\@:
	add $(BITS_PER_LONG/8), %_ASM_SP
	lfence
.endm

 /*
  * A simpler FILL_RETURN_BUFFER macro. Don't make people use the CPP
  * monstrosity above, manually.
  */
.macro FILL_RETURN_BUFFER reg:req nr:req ftr:req ftr2
.ifb \ftr2
	ALTERNATIVE "jmp .Lskip_rsb_\@", "", \ftr
.else
	ALTERNATIVE_2 "jmp .Lskip_rsb_\@", "", \ftr, "jmp .Lunbalanced_\@", \ftr2
.endif
	__FILL_RETURN_BUFFER(\reg,\nr,%_ASM_SP)
.Lunbalanced_\@:
	ISSUE_UNBALANCED_RET_GUARD
.Lskip_rsb_\@:
.endm

#ifdef CONFIG_CPU_UNRET_ENTRY
#define CALL_UNTRAIN_RET	"call entry_untrain_ret"
#else
#define CALL_UNTRAIN_RET	""
#endif

/*
 * Mitigate RETBleed for AMD/Hygon Zen uarch. Requires KERNEL CR3 because the
 * return thunk isn't mapped into the userspace tables (then again, AMD
 * typically has NO_MELTDOWN).
 *
 * While retbleed_untrain_ret() doesn't clobber anything but requires stack,
 * entry_ibpb() will clobber AX, CX, DX.
 *
 * As such, this must be placed after every *SWITCH_TO_KERNEL_CR3 at a point
 * where we have a stack but before any RET instruction.
 */
.macro UNTRAIN_RET
#if defined(CONFIG_RETHUNK) || defined(CONFIG_CPU_IBPB_ENTRY)
	ALTERNATIVE_2 "",						\
	              CALL_UNTRAIN_RET, X86_FEATURE_UNRET,		\
		      "call entry_ibpb", X86_FEATURE_ENTRY_IBPB
#endif
.endm

.macro UNTRAIN_RET_VM
#if defined(CONFIG_RETHUNK) || defined(CONFIG_CPU_IBPB_ENTRY)
	ALTERNATIVE_2 "",						\
		      CALL_UNTRAIN_RET, X86_FEATURE_UNRET,		\
		      "call entry_ibpb", X86_FEATURE_IBPB_ON_VMEXIT
#endif
.endm

/*
 * Macro to execute VERW instruction that mitigate transient data sampling
 * attacks such as MDS. On affected systems a microcode update overloaded VERW
 * instruction to also clear the CPU buffers. VERW clobbers CFLAGS.ZF.
 *
 * Note: Only the memory operand variant of VERW clears the CPU buffers.
 */
.macro CLEAR_CPU_BUFFERS
	ALTERNATIVE __stringify(verw _ASM_RIP(mds_verw_sel)), "", ALT_NOT(X86_FEATURE_CLEAR_CPU_BUF)
.endm

#ifdef CONFIG_X86_64
.macro CLEAR_BRANCH_HISTORY
	ALTERNATIVE "", "call clear_bhb_loop", X86_FEATURE_CLEAR_BHB_LOOP
.endm

.macro CLEAR_BRANCH_HISTORY_VMEXIT
	ALTERNATIVE "", "call clear_bhb_loop", X86_FEATURE_CLEAR_BHB_LOOP_ON_VMEXIT
.endm
#else
#define CLEAR_BRANCH_HISTORY
#define CLEAR_BRANCH_HISTORY_VMEXIT
#endif

#else /* __ASSEMBLY__ */

#define ANNOTATE_RETPOLINE_SAFE					\
	"999:\n\t"						\
	".pushsection .discard.retpoline_safe\n\t"		\
	_ASM_PTR " 999b\n\t"					\
	".popsection\n\t"

#ifdef CONFIG_RETHUNK
extern void __x86_return_thunk(void);
#else
static inline void __x86_return_thunk(void) {}
#endif

#ifdef CONFIG_CPU_UNRET_ENTRY
extern void retbleed_return_thunk(void);
#else
static inline void retbleed_return_thunk(void) {}
#endif

#ifdef CONFIG_CPU_SRSO
extern void srso_return_thunk(void);
extern void srso_alias_return_thunk(void);
#else
static inline void srso_return_thunk(void) {}
static inline void srso_alias_return_thunk(void) {}
#endif

extern void retbleed_return_thunk(void);
extern void srso_return_thunk(void);
extern void srso_alias_return_thunk(void);

extern void entry_untrain_ret(void);
extern void entry_ibpb(void);

#ifdef CONFIG_X86_64
extern void clear_bhb_loop(void);
#endif

extern void (*x86_return_thunk)(void);

#ifdef CONFIG_RETPOLINE
#ifdef CONFIG_X86_64

/*
 * Inline asm uses the %V modifier which is only in newer GCC
 * which is ensured when CONFIG_RETPOLINE is defined.
 */
# define CALL_NOSPEC						\
	ANNOTATE_NOSPEC_ALTERNATIVE				\
	ALTERNATIVE_2(						\
	ANNOTATE_RETPOLINE_SAFE					\
	"call *%[thunk_target]\n",				\
	"call __x86_indirect_thunk_%V[thunk_target]\n",		\
	X86_FEATURE_RETPOLINE,					\
	"lfence;\n"						\
	ANNOTATE_RETPOLINE_SAFE					\
	"call *%[thunk_target]\n",				\
	X86_FEATURE_RETPOLINE_LFENCE)
# define THUNK_TARGET(addr) [thunk_target] "r" (addr)

#else /* CONFIG_X86_32 */
/*
 * For i386 we use the original ret-equivalent retpoline, because
 * otherwise we'll run out of registers. We don't care about CET
 * here, anyway.
 */
# define CALL_NOSPEC						\
	ANNOTATE_NOSPEC_ALTERNATIVE				\
	ALTERNATIVE_2(						\
	ANNOTATE_RETPOLINE_SAFE					\
	"call *%[thunk_target]\n",				\
	"       jmp    904f;\n"					\
	"       .align 16\n"					\
	"901:	call   903f;\n"					\
	"902:	pause;\n"					\
	"    	lfence;\n"					\
	"       jmp    902b;\n"					\
	"       .align 16\n"					\
	"903:	addl   $4, %%esp;\n"				\
	"       pushl  %[thunk_target];\n"			\
	"       ret;\n"						\
	"       .align 16\n"					\
	"904:	call   901b;\n",				\
	X86_FEATURE_RETPOLINE,					\
	"lfence;\n"						\
	ANNOTATE_RETPOLINE_SAFE					\
	"call *%[thunk_target]\n",				\
	X86_FEATURE_RETPOLINE_LFENCE)

# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#endif
#else /* No retpoline for C / inline asm */
# define CALL_NOSPEC "call *%[thunk_target]\n"
# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#endif

/* The Spectre V2 mitigation variants */
enum spectre_v2_mitigation {
	SPECTRE_V2_NONE,
	SPECTRE_V2_RETPOLINE,
	SPECTRE_V2_LFENCE,
	SPECTRE_V2_EIBRS,
	SPECTRE_V2_EIBRS_RETPOLINE,
	SPECTRE_V2_EIBRS_LFENCE,
	SPECTRE_V2_IBRS,
	SPECTRE_V2_IBRS_ALWAYS,
	SPECTRE_V2_RETPOLINE_IBRS_USER,
};

/* The indirect branch speculation control variants */
enum spectre_v2_user_mitigation {
	SPECTRE_V2_USER_NONE,
	SPECTRE_V2_USER_STRICT,
	SPECTRE_V2_USER_STRICT_PREFERRED,
	SPECTRE_V2_USER_PRCTL,
	SPECTRE_V2_USER_SECCOMP,
};

/* The Speculative Store Bypass disable variants */
enum ssb_mitigation {
	SPEC_STORE_BYPASS_NONE,
	SPEC_STORE_BYPASS_DISABLE,
	SPEC_STORE_BYPASS_PRCTL,
	SPEC_STORE_BYPASS_SECCOMP,
};

extern char __indirect_thunk_start[];
extern char __indirect_thunk_end[];

static __always_inline
void alternative_msr_write(unsigned int msr, u64 val, unsigned int feature)
{
	asm volatile(ALTERNATIVE("", "wrmsr", %c[feature])
		: : "c" (msr),
		    "a" ((u32)val),
		    "d" ((u32)(val >> 32)),
		    [feature] "i" (feature)
		: "memory");
}

extern u64 x86_pred_cmd;

static inline void indirect_branch_prediction_barrier(void)
{
	alternative_msr_write(MSR_IA32_PRED_CMD, x86_pred_cmd, X86_FEATURE_USE_IBPB);
}

/* The Intel SPEC CTRL MSR base value cache */
extern u64 x86_spec_ctrl_base;
DECLARE_PER_CPU(u64, x86_spec_ctrl_current);
extern void update_spec_ctrl_cond(u64 val);
extern u64 spec_ctrl_current(void);

/*
 * With retpoline, we must use IBRS to restrict branch prediction
 * before calling into firmware.
 *
 * (Implemented as CPP macros due to header hell.)
 */
#define firmware_restrict_branch_speculation_start()			\
do {									\
	preempt_disable();						\
	alternative_msr_write(MSR_IA32_SPEC_CTRL,			\
			      spec_ctrl_current() | SPEC_CTRL_IBRS,	\
			      X86_FEATURE_USE_IBRS_FW);			\
	alternative_msr_write(MSR_IA32_PRED_CMD, PRED_CMD_IBPB,		\
			      X86_FEATURE_USE_IBPB_FW);			\
} while (0)

#define firmware_restrict_branch_speculation_end()			\
do {									\
	alternative_msr_write(MSR_IA32_SPEC_CTRL,			\
			      spec_ctrl_current(),			\
			      X86_FEATURE_USE_IBRS_FW);			\
	preempt_enable();						\
} while (0)

DECLARE_STATIC_KEY_FALSE(switch_to_cond_stibp);
DECLARE_STATIC_KEY_FALSE(switch_mm_cond_ibpb);
DECLARE_STATIC_KEY_FALSE(switch_mm_always_ibpb);

DECLARE_STATIC_KEY_FALSE(mds_idle_clear);

DECLARE_STATIC_KEY_FALSE(mmio_stale_data_clear);

extern u16 mds_verw_sel;

#include <asm/segment.h>

/**
 * mds_clear_cpu_buffers - Mitigation for MDS and TAA vulnerability
 *
 * This uses the otherwise unused and obsolete VERW instruction in
 * combination with microcode which triggers a CPU buffer flush when the
 * instruction is executed.
 */
static __always_inline void mds_clear_cpu_buffers(void)
{
	static const u16 ds = __KERNEL_DS;

	/*
	 * Has to be the memory-operand variant because only that
	 * guarantees the CPU buffer flush functionality according to
	 * documentation. The register-operand variant does not.
	 * Works with any segment selector, but a valid writable
	 * data segment is the fastest variant.
	 *
	 * "cc" clobber is required because VERW modifies ZF.
	 */
	asm volatile("verw %[ds]" : : [ds] "m" (ds) : "cc");
}

/**
 * mds_idle_clear_cpu_buffers - Mitigation for MDS vulnerability
 *
 * Clear CPU buffers if the corresponding static key is enabled
 */
static inline void mds_idle_clear_cpu_buffers(void)
{
	if (static_branch_likely(&mds_idle_clear))
		mds_clear_cpu_buffers();
}

#endif /* __ASSEMBLY__ */

/*
 * Below is used in the eBPF JIT compiler and emits the byte sequence
 * for the following assembly:
 *
 * With retpolines configured:
 *
 *    callq do_rop
 *  spec_trap:
 *    pause
 *    lfence
 *    jmp spec_trap
 *  do_rop:
 *    mov %rcx,(%rsp) for x86_64
 *    mov %edx,(%esp) for x86_32
 *    retq
 *
 * Without retpolines configured:
 *
 *    jmp *%rcx for x86_64
 *    jmp *%edx for x86_32
 */
#ifdef CONFIG_RETPOLINE
# ifdef CONFIG_X86_64
#  define RETPOLINE_RCX_BPF_JIT_SIZE	17
#  define RETPOLINE_RCX_BPF_JIT()				\
do {								\
	EMIT1_off32(0xE8, 7);	 /* callq do_rop */		\
	/* spec_trap: */					\
	EMIT2(0xF3, 0x90);       /* pause */			\
	EMIT3(0x0F, 0xAE, 0xE8); /* lfence */			\
	EMIT2(0xEB, 0xF9);       /* jmp spec_trap */		\
	/* do_rop: */						\
	EMIT4(0x48, 0x89, 0x0C, 0x24); /* mov %rcx,(%rsp) */	\
	EMIT1(0xC3);             /* retq */			\
} while (0)
# else /* !CONFIG_X86_64 */
#  define RETPOLINE_EDX_BPF_JIT()				\
do {								\
	EMIT1_off32(0xE8, 7);	 /* call do_rop */		\
	/* spec_trap: */					\
	EMIT2(0xF3, 0x90);       /* pause */			\
	EMIT3(0x0F, 0xAE, 0xE8); /* lfence */			\
	EMIT2(0xEB, 0xF9);       /* jmp spec_trap */		\
	/* do_rop: */						\
	EMIT3(0x89, 0x14, 0x24); /* mov %edx,(%esp) */		\
	EMIT1(0xC3);             /* ret */			\
} while (0)
# endif
#else /* !CONFIG_RETPOLINE */
# ifdef CONFIG_X86_64
#  define RETPOLINE_RCX_BPF_JIT_SIZE	2
#  define RETPOLINE_RCX_BPF_JIT()				\
	EMIT2(0xFF, 0xE1);       /* jmp *%rcx */
# else /* !CONFIG_X86_64 */
#  define RETPOLINE_EDX_BPF_JIT()				\
	EMIT2(0xFF, 0xE2)        /* jmp *%edx */
# endif
#endif

#endif /* _ASM_X86_NOSPEC_BRANCH_H_ */
