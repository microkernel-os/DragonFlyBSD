/*-
 * Copyright (C) 1994, David Greenman
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the University of Utah, and William Jolitz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)trap.c	7.4 (Berkeley) 5/13/91
 * $FreeBSD: src/sys/i386/i386/trap.c,v 1.147.2.11 2003/02/27 19:09:59 luoqi Exp $
 * $DragonFly: src/sys/platform/pc32/i386/trap.c,v 1.106 2007/07/01 01:11:38 dillon Exp $
 */

/*
 * 386 Trap and System call handling
 */

#include "use_isa.h"
#include "use_npx.h"

#include "opt_cpu.h"
#include "opt_ddb.h"
#include "opt_ktrace.h"
#include "opt_clock.h"
#include "opt_trap.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/pioctl.h>
#include <sys/kernel.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/signal2.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/uio.h>
#include <sys/vmmeter.h>
#include <sys/malloc.h>
#ifdef KTRACE
#include <sys/ktrace.h>
#endif
#include <sys/upcall.h>
#include <sys/vkernel.h>
#include <sys/sysproto.h>
#include <sys/sysunion.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>
#include <vm/vm_extern.h>

#include <machine/cpu.h>
#include <machine/md_var.h>
#include <machine/pcb.h>
#include <machine/smp.h>
#include <machine/tss.h>
#include <machine/specialreg.h>
#include <machine/globaldata.h>

#include <machine_base/isa/intr_machdep.h>

#ifdef POWERFAIL_NMI
#include <sys/syslog.h>
#include <machine/clock.h>
#endif

#include <machine/vm86.h>

#include <ddb/ddb.h>
#include <sys/msgport2.h>
#include <sys/thread2.h>

#ifdef SMP

#define MAKEMPSAFE(have_mplock)			\
	if (have_mplock == 0) {			\
		get_mplock();			\
		have_mplock = 1;		\
	}

#else

#define MAKEMPSAFE(have_mplock)

#endif

int (*pmath_emulate) (struct trapframe *);

extern void trap (struct trapframe *frame);
extern int trapwrite (unsigned addr);
extern void syscall2 (struct trapframe *frame);

static int trap_pfault (struct trapframe *, int, vm_offset_t);
static void trap_fatal (struct trapframe *, vm_offset_t);
void dblfault_handler (void);

extern inthand_t IDTVEC(syscall);

#define MAX_TRAP_MSG		28
static char *trap_msg[] = {
	"",					/*  0 unused */
	"privileged instruction fault",		/*  1 T_PRIVINFLT */
	"",					/*  2 unused */
	"breakpoint instruction fault",		/*  3 T_BPTFLT */
	"",					/*  4 unused */
	"",					/*  5 unused */
	"arithmetic trap",			/*  6 T_ARITHTRAP */
	"system forced exception",		/*  7 T_ASTFLT */
	"",					/*  8 unused */
	"general protection fault",		/*  9 T_PROTFLT */
	"trace trap",				/* 10 T_TRCTRAP */
	"",					/* 11 unused */
	"page fault",				/* 12 T_PAGEFLT */
	"",					/* 13 unused */
	"alignment fault",			/* 14 T_ALIGNFLT */
	"",					/* 15 unused */
	"",					/* 16 unused */
	"",					/* 17 unused */
	"integer divide fault",			/* 18 T_DIVIDE */
	"non-maskable interrupt trap",		/* 19 T_NMI */
	"overflow trap",			/* 20 T_OFLOW */
	"FPU bounds check fault",		/* 21 T_BOUND */
	"FPU device not available",		/* 22 T_DNA */
	"double fault",				/* 23 T_DOUBLEFLT */
	"FPU operand fetch fault",		/* 24 T_FPOPFLT */
	"invalid TSS fault",			/* 25 T_TSSFLT */
	"segment not present fault",		/* 26 T_SEGNPFLT */
	"stack fault",				/* 27 T_STKFLT */
	"machine check trap",			/* 28 T_MCHK */
};

#if defined(I586_CPU) && !defined(NO_F00F_HACK)
extern int has_f00f_bug;
#endif

#ifdef DDB
static int ddb_on_nmi = 1;
SYSCTL_INT(_machdep, OID_AUTO, ddb_on_nmi, CTLFLAG_RW,
	&ddb_on_nmi, 0, "Go to DDB on NMI");
#endif
static int panic_on_nmi = 1;
SYSCTL_INT(_machdep, OID_AUTO, panic_on_nmi, CTLFLAG_RW,
	&panic_on_nmi, 0, "Panic on NMI");
static int fast_release;
SYSCTL_INT(_machdep, OID_AUTO, fast_release, CTLFLAG_RW,
	&fast_release, 0, "Passive Release was optimal");
static int slow_release;
SYSCTL_INT(_machdep, OID_AUTO, slow_release, CTLFLAG_RW,
	&slow_release, 0, "Passive Release was nonoptimal");
#ifdef SMP
static int syscall_mpsafe = 0;
SYSCTL_INT(_kern, OID_AUTO, syscall_mpsafe, CTLFLAG_RW,
	&syscall_mpsafe, 0, "Allow MPSAFE marked syscalls to run without BGL");
TUNABLE_INT("kern.syscall_mpsafe", &syscall_mpsafe);
static int trap_mpsafe = 0;
SYSCTL_INT(_kern, OID_AUTO, trap_mpsafe, CTLFLAG_RW,
	&trap_mpsafe, 0, "Allow traps to mostly run without the BGL");
TUNABLE_INT("kern.trap_mpsafe", &trap_mpsafe);
#endif

MALLOC_DEFINE(M_SYSMSG, "sysmsg", "sysmsg structure");
extern int max_sysmsg;

/*
 * Passive USER->KERNEL transition.  This only occurs if we block in the
 * kernel while still holding our userland priority.  We have to fixup our
 * priority in order to avoid potential deadlocks before we allow the system
 * to switch us to another thread.
 */
static void
passive_release(struct thread *td)
{
	struct lwp *lp = td->td_lwp;

	td->td_release = NULL;
	lwkt_setpri_self(TDPRI_KERN_USER);
	lp->lwp_proc->p_usched->release_curproc(lp);
}

/*
 * userenter() passively intercepts the thread switch function to increase
 * the thread priority from a user priority to a kernel priority, reducing
 * syscall and trap overhead for the case where no switch occurs.
 */

static __inline void
userenter(struct thread *curtd)
{
	curtd->td_release = passive_release;
}

/*
 * Handle signals, upcalls, profiling, and other AST's and/or tasks that
 * must be completed before we can return to or try to return to userland.
 *
 * Note that td_sticks is a 64 bit quantity, but there's no point doing 64
 * arithmatic on the delta calculation so the absolute tick values are
 * truncated to an integer.
 */
static void
userret(struct lwp *lp, struct trapframe *frame, int sticks)
{
	struct proc *p = lp->lwp_proc;
	int sig;

	/*
	 * Charge system time if profiling.  Note: times are in microseconds.
	 * This may do a copyout and block, so do it first even though it
	 * means some system time will be charged as user time.
	 */
	if (p->p_flag & P_PROFIL) {
		addupc_task(p, frame->tf_eip, 
			(u_int)((int)lp->lwp_thread->td_sticks - sticks));
	}

recheck:
	/*
	 * If the jungle wants us dead, so be it.
	 */
	if (lp->lwp_flag & LWP_WEXIT)
		lwp_exit(0);

	/*
	 * Block here if we are in a stopped state.
	 */
	if (p->p_stat == SSTOP) {
		get_mplock();
		tstop();
		rel_mplock();
		goto recheck;
	}

	/*
	 * Post any pending upcalls.  If running a virtual kernel be sure
	 * to restore the virtual kernel's vmspace before posting the upcall.
	 */
	if (p->p_flag & P_UPCALLPEND) {
		p->p_flag &= ~P_UPCALLPEND;
		get_mplock();
		postupcall(lp);
		rel_mplock();
		goto recheck;
	}

	/*
	 * Post any pending signals.  If running a virtual kernel be sure
	 * to restore the virtual kernel's vmspace before posting the signal.
	 */
	if ((sig = CURSIG(lp)) != 0) {
		get_mplock();
		postsig(sig);
		rel_mplock();
		goto recheck;
	}

	/*
	 * block here if we are swapped out, but still process signals
	 * (such as SIGKILL).  proc0 (the swapin scheduler) is already
	 * aware of our situation, we do not have to wake it up.
	 */
	if (p->p_flag & P_SWAPPEDOUT) {
		get_mplock();
		p->p_flag |= P_SWAPWAIT;
		swapin_request();
		if (p->p_flag & P_SWAPWAIT)
			tsleep(p, PCATCH, "SWOUT", 0);
		p->p_flag &= ~P_SWAPWAIT;
		rel_mplock();
		goto recheck;
	}
}

/*
 * Cleanup from userenter and any passive release that might have occured.
 * We must reclaim the current-process designation before we can return
 * to usermode.  We also handle both LWKT and USER reschedule requests.
 */
static __inline void
userexit(struct lwp *lp)
{
	struct thread *td = lp->lwp_thread;
	globaldata_t gd = td->td_gd;

#if 0
	/*
	 * If a user reschedule is requested force a new process to be
	 * chosen by releasing the current process.  Our process will only
	 * be chosen again if it has a considerably better priority.
	 */
	if (user_resched_wanted())
		lp->lwp_proc->p_usched->release_curproc(lp);
#endif

	/*
	 * Handle a LWKT reschedule request first.  Since our passive release
	 * is still in place we do not have to do anything special.
	 */
	if (lwkt_resched_wanted())
		lwkt_switch();

	/*
	 * Acquire the current process designation for this user scheduler
	 * on this cpu.  This will also handle any user-reschedule requests.
	 */
	lp->lwp_proc->p_usched->acquire_curproc(lp);
	/* We may have switched cpus on acquisition */
	gd = td->td_gd;

	/*
	 * Reduce our priority in preparation for a return to userland.  If
	 * our passive release function was still in place, our priority was
	 * never raised and does not need to be reduced.
	 */
	if (td->td_release == NULL)
		lwkt_setpri_self(TDPRI_USER_NORM);
	td->td_release = NULL;

	/*
	 * After reducing our priority there might be other kernel-level
	 * LWKTs that now have a greater priority.  Run them as necessary.
	 * We don't have to worry about losing cpu to userland because
	 * we still control the current-process designation and we no longer
	 * have a passive release function installed.
	 */
	if (lwkt_checkpri_self())
		lwkt_switch();
}

/*
 * Exception, fault, and trap interface to the kernel.
 * This common code is called from assembly language IDT gate entry
 * routines that prepare a suitable stack frame, and restore this
 * frame after the exception has been processed.
 *
 * This function is also called from doreti in an interlock to handle ASTs.
 * For example:  hardwareint->INTROUTINE->(set ast)->doreti->trap
 *
 * NOTE!  We have to retrieve the fault address prior to obtaining the
 * MP lock because get_mplock() may switch out.  YYY cr2 really ought
 * to be retrieved by the assembly code, not here.
 *
 * XXX gd_trap_nesting_level currently prevents lwkt_switch() from panicing
 * if an attempt is made to switch from a fast interrupt or IPI.  This is
 * necessary to properly take fatal kernel traps on SMP machines if 
 * get_mplock() has to block.
 */

void
trap(struct trapframe *frame)
{
	struct globaldata *gd = mycpu;
	struct thread *td = gd->gd_curthread;
	struct lwp *lp = td->td_lwp;
	struct proc *p;
	int sticks = 0;
	int i = 0, ucode = 0, type, code;
#ifdef SMP
	int have_mplock = 0;
#endif
#ifdef INVARIANTS
	int crit_count = td->td_pri & ~TDPRI_MASK;
#endif
	vm_offset_t eva;

	p = td->td_proc;
#ifdef DDB
	if (db_active) {
		eva = (frame->tf_trapno == T_PAGEFLT ? rcr2() : 0);
		++gd->gd_trap_nesting_level;
		MAKEMPSAFE(have_mplock);
		trap_fatal(frame, eva);
		--gd->gd_trap_nesting_level;
		goto out2;
	}
#endif

	eva = 0;
	++gd->gd_trap_nesting_level;
	if (frame->tf_trapno == T_PAGEFLT) {
		/*
		 * For some Cyrix CPUs, %cr2 is clobbered by interrupts.
		 * This problem is worked around by using an interrupt
		 * gate for the pagefault handler.  We are finally ready
		 * to read %cr2 and then must reenable interrupts.
		 *
		 * XXX this should be in the switch statement, but the
		 * NO_FOOF_HACK and VM86 goto and ifdefs obfuscate the
		 * flow of control too much for this to be obviously
		 * correct.
		 */
		eva = rcr2();
		cpu_enable_intr();
	}
#ifdef SMP
	if (trap_mpsafe == 0)
		MAKEMPSAFE(have_mplock);
#endif

	--gd->gd_trap_nesting_level;

	if (!(frame->tf_eflags & PSL_I)) {
		/*
		 * Buggy application or kernel code has disabled interrupts
		 * and then trapped.  Enabling interrupts now is wrong, but
		 * it is better than running with interrupts disabled until
		 * they are accidentally enabled later.
		 */
		type = frame->tf_trapno;
		if (ISPL(frame->tf_cs)==SEL_UPL || (frame->tf_eflags & PSL_VM)) {
			MAKEMPSAFE(have_mplock);
			kprintf(
			    "pid %ld (%s): trap %d with interrupts disabled\n",
			    (long)curproc->p_pid, curproc->p_comm, type);
		} else if (type != T_BPTFLT && type != T_TRCTRAP) {
			/*
			 * XXX not quite right, since this may be for a
			 * multiple fault in user mode.
			 */
			MAKEMPSAFE(have_mplock);
			kprintf("kernel trap %d with interrupts disabled\n",
			    type);
		}
		cpu_enable_intr();
	}

#if defined(I586_CPU) && !defined(NO_F00F_HACK)
restart:
#endif
	type = frame->tf_trapno;
	code = frame->tf_err;

	if (in_vm86call) {
		ASSERT_MP_LOCK_HELD(curthread);
		if (frame->tf_eflags & PSL_VM &&
		    (type == T_PROTFLT || type == T_STKFLT)) {
#ifdef SMP
			KKASSERT(td->td_mpcount > 0);
#endif
			i = vm86_emulate((struct vm86frame *)frame);
#ifdef SMP
			KKASSERT(td->td_mpcount > 0);
#endif
			if (i != 0) {
				/*
				 * returns to original process
				 */
#ifdef SMP
				vm86_trap((struct vm86frame *)frame,
					  have_mplock);
#else
				vm86_trap((struct vm86frame *)frame, 0);
#endif
				KKASSERT(0); /* NOT REACHED */
			}
			goto out2;
		}
		switch (type) {
			/*
			 * these traps want either a process context, or
			 * assume a normal userspace trap.
			 */
		case T_PROTFLT:
		case T_SEGNPFLT:
			trap_fatal(frame, eva);
			goto out2;
		case T_TRCTRAP:
			type = T_BPTFLT;	/* kernel breakpoint */
			/* FALL THROUGH */
		}
		goto kernel_trap;	/* normal kernel trap handling */
	}

        if ((ISPL(frame->tf_cs) == SEL_UPL) || (frame->tf_eflags & PSL_VM)) {
		/* user trap */

		userenter(td);

		sticks = (int)td->td_sticks;
		lp->lwp_md.md_regs = frame;

		switch (type) {
		case T_PRIVINFLT:	/* privileged instruction fault */
			ucode = type;
			i = SIGILL;
			break;

		case T_BPTFLT:		/* bpt instruction fault */
		case T_TRCTRAP:		/* trace trap */
			frame->tf_eflags &= ~PSL_T;
			i = SIGTRAP;
			break;

		case T_ARITHTRAP:	/* arithmetic trap */
			ucode = code;
			i = SIGFPE;
			break;

		case T_ASTFLT:		/* Allow process switch */
			mycpu->gd_cnt.v_soft++;
			if (mycpu->gd_reqflags & RQF_AST_OWEUPC) {
				atomic_clear_int_nonlocked(&mycpu->gd_reqflags,
					    RQF_AST_OWEUPC);
				addupc_task(p, p->p_prof.pr_addr,
					    p->p_prof.pr_ticks);
			}
			goto out;

			/*
			 * The following two traps can happen in
			 * vm86 mode, and, if so, we want to handle
			 * them specially.
			 */
		case T_PROTFLT:		/* general protection fault */
		case T_STKFLT:		/* stack fault */
			if (frame->tf_eflags & PSL_VM) {
				i = vm86_emulate((struct vm86frame *)frame);
				if (i == 0)
					goto out;
				break;
			}
			/* FALL THROUGH */

		case T_SEGNPFLT:	/* segment not present fault */
		case T_TSSFLT:		/* invalid TSS fault */
		case T_DOUBLEFLT:	/* double fault */
		default:
			ucode = code + BUS_SEGM_FAULT ;
			i = SIGBUS;
			break;

		case T_PAGEFLT:		/* page fault */
			MAKEMPSAFE(have_mplock);
			i = trap_pfault(frame, TRUE, eva);
			if (i == -1)
				goto out;
#if defined(I586_CPU) && !defined(NO_F00F_HACK)
			if (i == -2)
				goto restart;
#endif
			if (i == 0)
				goto out;

			ucode = T_PAGEFLT;
			break;

		case T_DIVIDE:		/* integer divide fault */
			ucode = FPE_INTDIV;
			i = SIGFPE;
			break;

#if NISA > 0
		case T_NMI:
			MAKEMPSAFE(have_mplock);
#ifdef POWERFAIL_NMI
			goto handle_powerfail;
#else /* !POWERFAIL_NMI */
			/* machine/parity/power fail/"kitchen sink" faults */
			if (isa_nmi(code) == 0) {
#ifdef DDB
				/*
				 * NMI can be hooked up to a pushbutton
				 * for debugging.
				 */
				if (ddb_on_nmi) {
					kprintf ("NMI ... going to debugger\n");
					kdb_trap (type, 0, frame);
				}
#endif /* DDB */
				goto out2;
			} else if (panic_on_nmi)
				panic("NMI indicates hardware failure");
			break;
#endif /* POWERFAIL_NMI */
#endif /* NISA > 0 */

		case T_OFLOW:		/* integer overflow fault */
			ucode = FPE_INTOVF;
			i = SIGFPE;
			break;

		case T_BOUND:		/* bounds check fault */
			ucode = FPE_FLTSUB;
			i = SIGFPE;
			break;

		case T_DNA:
			/*
			 * Virtual kernel intercept - pass the DNA exception
			 * to the virtual kernel if it asked to handle it.
			 * This occurs when the virtual kernel is holding
			 * onto the FP context for a different emulated
			 * process then the one currently running.
			 *
			 * We must still call npxdna() since we may have
			 * saved FP state that the virtual kernel needs
			 * to hand over to a different emulated process.
			 */
			if (lp->lwp_vkernel && lp->lwp_vkernel->ve &&
			    (td->td_pcb->pcb_flags & FP_VIRTFP)
			) {
				npxdna();
				break;
			}

#if NNPX > 0
			/* 
			 * The kernel may have switched out the FP unit's
			 * state, causing the user process to take a fault
			 * when it tries to use the FP unit.  Restore the
			 * state here
			 */
			if (npxdna())
				goto out;
#endif
			if (!pmath_emulate) {
				i = SIGFPE;
				ucode = FPE_FPU_NP_TRAP;
				break;
			}
			i = (*pmath_emulate)(frame);
			if (i == 0) {
				if (!(frame->tf_eflags & PSL_T))
					goto out2;
				frame->tf_eflags &= ~PSL_T;
				i = SIGTRAP;
			}
			/* else ucode = emulator_only_knows() XXX */
			break;

		case T_FPOPFLT:		/* FPU operand fetch fault */
			ucode = T_FPOPFLT;
			i = SIGILL;
			break;

		case T_XMMFLT:		/* SIMD floating-point exception */
			ucode = 0; /* XXX */
			i = SIGFPE;
			break;
		}
	} else {
kernel_trap:
		/* kernel trap */

		switch (type) {
		case T_PAGEFLT:			/* page fault */
			MAKEMPSAFE(have_mplock);
			trap_pfault(frame, FALSE, eva);
			goto out2;

		case T_DNA:
#if NNPX > 0
			/*
			 * The kernel may be using npx for copying or other
			 * purposes.
			 */
			if (npxdna())
				goto out2;
#endif
			break;

		case T_PROTFLT:		/* general protection fault */
		case T_SEGNPFLT:	/* segment not present fault */
			/*
			 * Invalid segment selectors and out of bounds
			 * %eip's and %esp's can be set up in user mode.
			 * This causes a fault in kernel mode when the
			 * kernel tries to return to user mode.  We want
			 * to get this fault so that we can fix the
			 * problem here and not have to check all the
			 * selectors and pointers when the user changes
			 * them.
			 */
#define	MAYBE_DORETI_FAULT(where, whereto)				\
	do {								\
		if (frame->tf_eip == (int)where) {			\
			frame->tf_eip = (int)whereto;			\
			goto out2;					\
		}							\
	} while (0)
			if (mycpu->gd_intr_nesting_level == 0) {
				/*
				 * Invalid %fs's and %gs's can be created using
				 * procfs or PT_SETREGS or by invalidating the
				 * underlying LDT entry.  This causes a fault
				 * in kernel mode when the kernel attempts to
				 * switch contexts.  Lose the bad context
				 * (XXX) so that we can continue, and generate
				 * a signal.
				 */
				MAYBE_DORETI_FAULT(doreti_iret,
						   doreti_iret_fault);
				MAYBE_DORETI_FAULT(doreti_popl_ds,
						   doreti_popl_ds_fault);
				MAYBE_DORETI_FAULT(doreti_popl_es,
						   doreti_popl_es_fault);
				MAYBE_DORETI_FAULT(doreti_popl_fs,
						   doreti_popl_fs_fault);
				MAYBE_DORETI_FAULT(doreti_popl_gs,
						   doreti_popl_gs_fault);
				if (td->td_pcb->pcb_onfault) {
					frame->tf_eip = 
					    (register_t)td->td_pcb->pcb_onfault;
					goto out2;
				}
			}
			break;

		case T_TSSFLT:
			/*
			 * PSL_NT can be set in user mode and isn't cleared
			 * automatically when the kernel is entered.  This
			 * causes a TSS fault when the kernel attempts to
			 * `iret' because the TSS link is uninitialized.  We
			 * want to get this fault so that we can fix the
			 * problem here and not every time the kernel is
			 * entered.
			 */
			if (frame->tf_eflags & PSL_NT) {
				frame->tf_eflags &= ~PSL_NT;
				goto out2;
			}
			break;

		case T_TRCTRAP:	 /* trace trap */
			if (frame->tf_eip == (int)IDTVEC(syscall)) {
				/*
				 * We've just entered system mode via the
				 * syscall lcall.  Continue single stepping
				 * silently until the syscall handler has
				 * saved the flags.
				 */
				goto out2;
			}
			if (frame->tf_eip == (int)IDTVEC(syscall) + 1) {
				/*
				 * The syscall handler has now saved the
				 * flags.  Stop single stepping it.
				 */
				frame->tf_eflags &= ~PSL_T;
				goto out2;
			}
                        /*
                         * Ignore debug register trace traps due to
                         * accesses in the user's address space, which
                         * can happen under several conditions such as
                         * if a user sets a watchpoint on a buffer and
                         * then passes that buffer to a system call.
                         * We still want to get TRCTRAPS for addresses
                         * in kernel space because that is useful when
                         * debugging the kernel.
                         */
                        if (user_dbreg_trap()) {
                                /*
                                 * Reset breakpoint bits because the
                                 * processor doesn't
                                 */
                                load_dr6(rdr6() & 0xfffffff0);
                                goto out2;
                        }
			/*
			 * Fall through (TRCTRAP kernel mode, kernel address)
			 */
		case T_BPTFLT:
			/*
			 * If DDB is enabled, let it handle the debugger trap.
			 * Otherwise, debugger traps "can't happen".
			 */
#ifdef DDB
			MAKEMPSAFE(have_mplock);
			if (kdb_trap (type, 0, frame))
				goto out2;
#endif
			break;

#if NISA > 0
		case T_NMI:
			MAKEMPSAFE(have_mplock);
#ifdef POWERFAIL_NMI
#ifndef TIMER_FREQ
#  define TIMER_FREQ 1193182
#endif
	handle_powerfail:
		{
		  static unsigned lastalert = 0;

		  if(time_second - lastalert > 10)
		    {
		      log(LOG_WARNING, "NMI: power fail\n");
		      sysbeep(TIMER_FREQ/880, hz);
		      lastalert = time_second;
		    }
		    /* YYY mp count */
		  goto out2;
		}
#else /* !POWERFAIL_NMI */
			/* machine/parity/power fail/"kitchen sink" faults */
			if (isa_nmi(code) == 0) {
#ifdef DDB
				/*
				 * NMI can be hooked up to a pushbutton
				 * for debugging.
				 */
				if (ddb_on_nmi) {
					kprintf ("NMI ... going to debugger\n");
					kdb_trap (type, 0, frame);
				}
#endif /* DDB */
				goto out2;
			} else if (panic_on_nmi == 0)
				goto out2;
			/* FALL THROUGH */
#endif /* POWERFAIL_NMI */
#endif /* NISA > 0 */
		}

		MAKEMPSAFE(have_mplock);
		trap_fatal(frame, eva);
		goto out2;
	}

	/*
	 * Virtual kernel intercept - if the fault is directly related to a
	 * VM context managed by a virtual kernel then let the virtual kernel
	 * handle it.
	 */
	if (lp->lwp_vkernel && lp->lwp_vkernel->ve) {
		vkernel_trap(lp, frame);
		goto out;
	}

	/*
	 * Translate fault for emulators (e.g. Linux) 
	 */
	if (*p->p_sysent->sv_transtrap)
		i = (*p->p_sysent->sv_transtrap)(i, type);

	MAKEMPSAFE(have_mplock);
	trapsignal(lp, i, ucode);

#ifdef DEBUG
	if (type <= MAX_TRAP_MSG) {
		uprintf("fatal process exception: %s",
			trap_msg[type]);
		if ((type == T_PAGEFLT) || (type == T_PROTFLT))
			uprintf(", fault VA = 0x%lx", (u_long)eva);
		uprintf("\n");
	}
#endif

out:
#ifdef SMP
        if (ISPL(frame->tf_cs) == SEL_UPL)
		KASSERT(td->td_mpcount == have_mplock, ("badmpcount trap/end from %p", (void *)frame->tf_eip));
#endif
	userret(lp, frame, sticks);
	userexit(lp);
out2:	;
#ifdef SMP
	if (have_mplock)
		rel_mplock();
#endif
#ifdef INVARIANTS
	KASSERT(crit_count == (td->td_pri & ~TDPRI_MASK),
		("syscall: critical section count mismatch! %d/%d",
		crit_count / TDPRI_CRIT, td->td_pri / TDPRI_CRIT));
#endif
}

int
trap_pfault(struct trapframe *frame, int usermode, vm_offset_t eva)
{
	vm_offset_t va;
	struct vmspace *vm = NULL;
	vm_map_t map = 0;
	int rv = 0;
	vm_prot_t ftype;
	thread_t td = curthread;
	struct lwp *lp = td->td_lwp;

	va = trunc_page(eva);
	if (va >= KERNBASE) {
		/*
		 * Don't allow user-mode faults in kernel address space.
		 * An exception:  if the faulting address is the invalid
		 * instruction entry in the IDT, then the Intel Pentium
		 * F00F bug workaround was triggered, and we need to
		 * treat it is as an illegal instruction, and not a page
		 * fault.
		 */
#if defined(I586_CPU) && !defined(NO_F00F_HACK)
		if ((eva == (unsigned int)&idt[6]) && has_f00f_bug) {
			frame->tf_trapno = T_PRIVINFLT;
			return -2;
		}
#endif
		if (usermode)
			goto nogo;

		map = &kernel_map;
	} else {
		/*
		 * This is a fault on non-kernel virtual memory.
		 * vm is initialized above to NULL. If curproc is NULL
		 * or curproc->p_vmspace is NULL the fault is fatal.
		 */
		if (lp != NULL)
			vm = lp->lwp_vmspace;

		if (vm == NULL)
			goto nogo;

		map = &vm->vm_map;
	}

	if (frame->tf_err & PGEX_W)
		ftype = VM_PROT_WRITE;
	else
		ftype = VM_PROT_READ;

	if (map != &kernel_map) {
		/*
		 * Keep swapout from messing with us during this
		 *	critical time.
		 */
		PHOLD(lp->lwp_proc);

		/*
		 * Grow the stack if necessary
		 */
		/* grow_stack returns false only if va falls into
		 * a growable stack region and the stack growth
		 * fails.  It returns true if va was not within
		 * a growable stack region, or if the stack 
		 * growth succeeded.
		 */
		if (!grow_stack(lp->lwp_proc, va)) {
			rv = KERN_FAILURE;
			PRELE(lp->lwp_proc);
			goto nogo;
		}

		/* Fault in the user page: */
		rv = vm_fault(map, va, ftype,
			      (ftype & VM_PROT_WRITE) ? VM_FAULT_DIRTY
						      : VM_FAULT_NORMAL);

		PRELE(lp->lwp_proc);
	} else {
		/*
		 * Don't have to worry about process locking or stacks in the kernel.
		 */
		rv = vm_fault(map, va, ftype, VM_FAULT_NORMAL);
	}

	if (rv == KERN_SUCCESS)
		return (0);
nogo:
	if (!usermode) {
		if (td->td_gd->gd_intr_nesting_level == 0 &&
		    td->td_pcb->pcb_onfault) {
			frame->tf_eip = (register_t)td->td_pcb->pcb_onfault;
			return (0);
		}
		trap_fatal(frame, eva);
		return (-1);
	}

	/* kludge to pass faulting virtual address to sendsig */
	frame->tf_xflags = frame->tf_err;
	frame->tf_err = eva;

	return((rv == KERN_PROTECTION_FAILURE) ? SIGBUS : SIGSEGV);
}

static void
trap_fatal(struct trapframe *frame, vm_offset_t eva)
{
	int code, type, ss, esp;
	struct soft_segment_descriptor softseg;

	code = frame->tf_err;
	type = frame->tf_trapno;
	sdtossd(&gdt[mycpu->gd_cpuid * NGDT + IDXSEL(frame->tf_cs & 0xffff)].sd, &softseg);

	if (type <= MAX_TRAP_MSG)
		kprintf("\n\nFatal trap %d: %s while in %s mode\n",
			type, trap_msg[type],
        		frame->tf_eflags & PSL_VM ? "vm86" :
			ISPL(frame->tf_cs) == SEL_UPL ? "user" : "kernel");
#ifdef SMP
	/* three separate prints in case of a trap on an unmapped page */
	kprintf("mp_lock = %08x; ", mp_lock);
	kprintf("cpuid = %d; ", mycpu->gd_cpuid);
	kprintf("lapic.id = %08x\n", lapic.id);
#endif
	if (type == T_PAGEFLT) {
		kprintf("fault virtual address	= 0x%x\n", eva);
		kprintf("fault code		= %s %s, %s\n",
			code & PGEX_U ? "user" : "supervisor",
			code & PGEX_W ? "write" : "read",
			code & PGEX_P ? "protection violation" : "page not present");
	}
	kprintf("instruction pointer	= 0x%x:0x%x\n",
	       frame->tf_cs & 0xffff, frame->tf_eip);
        if ((ISPL(frame->tf_cs) == SEL_UPL) || (frame->tf_eflags & PSL_VM)) {
		ss = frame->tf_ss & 0xffff;
		esp = frame->tf_esp;
	} else {
		ss = GSEL(GDATA_SEL, SEL_KPL);
		esp = (int)&frame->tf_esp;
	}
	kprintf("stack pointer	        = 0x%x:0x%x\n", ss, esp);
	kprintf("frame pointer	        = 0x%x:0x%x\n", ss, frame->tf_ebp);
	kprintf("code segment		= base 0x%x, limit 0x%x, type 0x%x\n",
	       softseg.ssd_base, softseg.ssd_limit, softseg.ssd_type);
	kprintf("			= DPL %d, pres %d, def32 %d, gran %d\n",
	       softseg.ssd_dpl, softseg.ssd_p, softseg.ssd_def32,
	       softseg.ssd_gran);
	kprintf("processor eflags	= ");
	if (frame->tf_eflags & PSL_T)
		kprintf("trace trap, ");
	if (frame->tf_eflags & PSL_I)
		kprintf("interrupt enabled, ");
	if (frame->tf_eflags & PSL_NT)
		kprintf("nested task, ");
	if (frame->tf_eflags & PSL_RF)
		kprintf("resume, ");
	if (frame->tf_eflags & PSL_VM)
		kprintf("vm86, ");
	kprintf("IOPL = %d\n", (frame->tf_eflags & PSL_IOPL) >> 12);
	kprintf("current process		= ");
	if (curproc) {
		kprintf("%lu (%s)\n",
		    (u_long)curproc->p_pid, curproc->p_comm ?
		    curproc->p_comm : "");
	} else {
		kprintf("Idle\n");
	}
	kprintf("current thread          = pri %d ", curthread->td_pri);
	if (curthread->td_pri >= TDPRI_CRIT)
		kprintf("(CRIT)");
	kprintf("\n");
#ifdef SMP
/**
 *  XXX FIXME:
 *	we probably SHOULD have stopped the other CPUs before now!
 *	another CPU COULD have been touching cpl at this moment...
 */
	kprintf(" <- SMP: XXX");
#endif
	kprintf("\n");

#ifdef KDB
	if (kdb_trap(&psl))
		return;
#endif
#ifdef DDB
	if ((debugger_on_panic || db_active) && kdb_trap(type, code, frame))
		return;
#endif
	kprintf("trap number		= %d\n", type);
	if (type <= MAX_TRAP_MSG)
		panic("%s", trap_msg[type]);
	else
		panic("unknown/reserved trap");
}

/*
 * Double fault handler. Called when a fault occurs while writing
 * a frame for a trap/exception onto the stack. This usually occurs
 * when the stack overflows (such is the case with infinite recursion,
 * for example).
 *
 * XXX Note that the current PTD gets replaced by IdlePTD when the
 * task switch occurs. This means that the stack that was active at
 * the time of the double fault is not available at <kstack> unless
 * the machine was idle when the double fault occurred. The downside
 * of this is that "trace <ebp>" in ddb won't work.
 */
void
dblfault_handler(void)
{
	struct mdglobaldata *gd = mdcpu;

	kprintf("\nFatal double fault:\n");
	kprintf("eip = 0x%x\n", gd->gd_common_tss.tss_eip);
	kprintf("esp = 0x%x\n", gd->gd_common_tss.tss_esp);
	kprintf("ebp = 0x%x\n", gd->gd_common_tss.tss_ebp);
#ifdef SMP
	/* three separate prints in case of a trap on an unmapped page */
	kprintf("mp_lock = %08x; ", mp_lock);
	kprintf("cpuid = %d; ", mycpu->gd_cpuid);
	kprintf("lapic.id = %08x\n", lapic.id);
#endif
	panic("double fault");
}

/*
 * Compensate for 386 brain damage (missing URKR).
 * This is a little simpler than the pagefault handler in trap() because
 * it the page tables have already been faulted in and high addresses
 * are thrown out early for other reasons.
 */
int
trapwrite(unsigned addr)
{
	struct lwp *lp;
	vm_offset_t va;
	struct vmspace *vm;
	int rv;

	va = trunc_page((vm_offset_t)addr);
	/*
	 * XXX - MAX is END.  Changed > to >= for temp. fix.
	 */
	if (va >= VM_MAX_USER_ADDRESS)
		return (1);

	lp = curthread->td_lwp;
	vm = lp->lwp_vmspace;

	PHOLD(lp->lwp_proc);

	if (!grow_stack(lp->lwp_proc, va)) {
		PRELE(lp->lwp_proc);
		return (1);
	}

	/*
	 * fault the data page
	 */
	rv = vm_fault(&vm->vm_map, va, VM_PROT_WRITE, VM_FAULT_DIRTY);

	PRELE(lp->lwp_proc);

	if (rv != KERN_SUCCESS)
		return 1;

	return (0);
}

/*
 *	syscall2 -	MP aware system call request C handler
 *
 *	A system call is essentially treated as a trap except that the
 *	MP lock is not held on entry or return.  We are responsible for
 *	obtaining the MP lock if necessary and for handling ASTs
 *	(e.g. a task switch) prior to return.
 *
 *	In general, only simple access and manipulation of curproc and
 *	the current stack is allowed without having to hold MP lock.
 *
 *	MPSAFE - note that large sections of this routine are run without
 *		 the MP lock.
 */

void
syscall2(struct trapframe *frame)
{
	struct thread *td = curthread;
	struct proc *p = td->td_proc;
	struct lwp *lp = td->td_lwp;
	caddr_t params;
	struct sysent *callp;
	register_t orig_tf_eflags;
	int sticks;
	int error;
	int narg;
#ifdef INVARIANTS
	int crit_count = td->td_pri & ~TDPRI_MASK;
#endif
#ifdef SMP
	int have_mplock = 0;
#endif
	u_int code;
	union sysunion args;

#ifdef DIAGNOSTIC
	if (ISPL(frame->tf_cs) != SEL_UPL) {
		get_mplock();
		panic("syscall");
		/* NOT REACHED */
	}
#endif

#ifdef SMP
	KASSERT(td->td_mpcount == 0, ("badmpcount syscall2 from %p", (void *)frame->tf_eip));
	if (syscall_mpsafe == 0)
		MAKEMPSAFE(have_mplock);
#endif
	userenter(td);		/* lazy raise our priority */

	/*
	 * Misc
	 */
	sticks = (int)td->td_sticks;
	orig_tf_eflags = frame->tf_eflags;

	/*
	 * Virtual kernel intercept - if a VM context managed by a virtual
	 * kernel issues a system call the virtual kernel handles it, not us.
	 * Restore the virtual kernel context and return from its system
	 * call.  The current frame is copied out to the virtual kernel.
	 */
	if (lp->lwp_vkernel && lp->lwp_vkernel->ve) {
		error = vkernel_trap(lp, frame);
		frame->tf_eax = error;
		if (error)
			frame->tf_eflags |= PSL_C;
		error = EJUSTRETURN;
		goto out;
	}

	/*
	 * Get the system call parameters and account for time
	 */
	lp->lwp_md.md_regs = frame;
	params = (caddr_t)frame->tf_esp + sizeof(int);
	code = frame->tf_eax;

	if (p->p_sysent->sv_prepsyscall) {
		(*p->p_sysent->sv_prepsyscall)(
			frame, (int *)(&args.nosys.sysmsg + 1),
			&code, &params);
	} else {
		/*
		 * Need to check if this is a 32 bit or 64 bit syscall.
		 * fuword is MP aware.
		 */
		if (code == SYS_syscall) {
			/*
			 * Code is first argument, followed by actual args.
			 */
			code = fuword(params);
			params += sizeof(int);
		} else if (code == SYS___syscall) {
			/*
			 * Like syscall, but code is a quad, so as to maintain
			 * quad alignment for the rest of the arguments.
			 */
			code = fuword(params);
			params += sizeof(quad_t);
		}
	}

	code &= p->p_sysent->sv_mask;
	if (code >= p->p_sysent->sv_size)
		callp = &p->p_sysent->sv_table[0];
	else
		callp = &p->p_sysent->sv_table[code];

	narg = callp->sy_narg & SYF_ARGMASK;

	/*
	 * copyin is MP aware, but the tracing code is not
	 */
	if (narg && params) {
		error = copyin(params, (caddr_t)(&args.nosys.sysmsg + 1),
				narg * sizeof(register_t));
		if (error) {
#ifdef KTRACE
			if (KTRPOINT(td, KTR_SYSCALL)) {
				MAKEMPSAFE(have_mplock);
				
				ktrsyscall(p, code, narg,
					(void *)(&args.nosys.sysmsg + 1));
			}
#endif
			goto bad;
		}
	}

#ifdef KTRACE
	if (KTRPOINT(td, KTR_SYSCALL)) {
		MAKEMPSAFE(have_mplock);
		ktrsyscall(p, code, narg, (void *)(&args.nosys.sysmsg + 1));
	}
#endif

	/*
	 * For traditional syscall code edx is left untouched when 32 bit
	 * results are returned.  Since edx is loaded from fds[1] when the 
	 * system call returns we pre-set it here.
	 */
	args.sysmsg_fds[0] = 0;
	args.sysmsg_fds[1] = frame->tf_edx;

	/*
	 * The syscall might manipulate the trap frame. If it does it
	 * will probably return EJUSTRETURN.
	 */
	args.sysmsg_frame = frame;

	STOPEVENT(p, S_SCE, narg);	/* MP aware */

#ifdef SMP
	/*
	 * Try to run the syscall without the MP lock if the syscall
	 * is MP safe.  We have to obtain the MP lock no matter what if 
	 * we are ktracing
	 */
	if ((callp->sy_narg & SYF_MPSAFE) == 0)
		MAKEMPSAFE(have_mplock);
#endif

	error = (*callp->sy_call)(&args);

out:
	/*
	 * MP SAFE (we may or may not have the MP lock at this point)
	 */
	switch (error) {
	case 0:
		/*
		 * Reinitialize proc pointer `p' as it may be different
		 * if this is a child returning from fork syscall.
		 */
		p = curproc;
		lp = curthread->td_lwp;
		frame->tf_eax = args.sysmsg_fds[0];
		frame->tf_edx = args.sysmsg_fds[1];
		frame->tf_eflags &= ~PSL_C;
		break;
	case ERESTART:
		/*
		 * Reconstruct pc, assuming lcall $X,y is 7 bytes,
		 * int 0x80 is 2 bytes. We saved this in tf_err.
		 */
		frame->tf_eip -= frame->tf_err;
		break;
	case EJUSTRETURN:
		break;
	case EASYNC:
		panic("Unexpected EASYNC return value (for now)");
	default:
bad:
		if (p->p_sysent->sv_errsize) {
			if (error >= p->p_sysent->sv_errsize)
				error = -1;	/* XXX */
			else
				error = p->p_sysent->sv_errtbl[error];
		}
		frame->tf_eax = error;
		frame->tf_eflags |= PSL_C;
		break;
	}

	/*
	 * Traced syscall.  trapsignal() is not MP aware.
	 */
	if ((orig_tf_eflags & PSL_T) && !(orig_tf_eflags & PSL_VM)) {
		MAKEMPSAFE(have_mplock);
		frame->tf_eflags &= ~PSL_T;
		trapsignal(lp, SIGTRAP, 0);
	}

	/*
	 * Handle reschedule and other end-of-syscall issues
	 */
	userret(lp, frame, sticks);

#ifdef KTRACE
	if (KTRPOINT(td, KTR_SYSRET)) {
		MAKEMPSAFE(have_mplock);
		ktrsysret(p, code, error, args.sysmsg_result);
	}
#endif

	/*
	 * This works because errno is findable through the
	 * register set.  If we ever support an emulation where this
	 * is not the case, this code will need to be revisited.
	 */
	STOPEVENT(p, S_SCX, code);

	userexit(lp);
#ifdef SMP
	/*
	 * Release the MP lock if we had to get it
	 */
	KASSERT(td->td_mpcount == have_mplock, 
		("badmpcount syscall2/end from %p", (void *)frame->tf_eip));
	if (have_mplock)
		rel_mplock();
#endif
#ifdef INVARIANTS
	KASSERT(crit_count == (td->td_pri & ~TDPRI_MASK), 
		("syscall: critical section count mismatch! %d/%d",
		crit_count / TDPRI_CRIT, td->td_pri / TDPRI_CRIT));
#endif
}

void
fork_return(struct lwp *lp, struct trapframe *frame)
{
	frame->tf_eax = 0;		/* Child returns zero */
	frame->tf_eflags &= ~PSL_C;	/* success */
	frame->tf_edx = 1;

	generic_lwp_return(lp, frame);
}

/*
 * Simplified back end of syscall(), used when returning from fork()
 * directly into user mode.  MP lock is held on entry and should be
 * released on return.  This code will return back into the fork
 * trampoline code which then runs doreti.
 */
void
generic_lwp_return(struct lwp *lp, struct trapframe *frame)
{
	struct proc *p = lp->lwp_proc;

	/*
	 * Newly forked processes are given a kernel priority.  We have to
	 * adjust the priority to a normal user priority and fake entry
	 * into the kernel (call userenter()) to install a passive release
	 * function just in case userret() decides to stop the process.  This
	 * can occur when ^Z races a fork.  If we do not install the passive
	 * release function the current process designation will not be
	 * released when the thread goes to sleep.
	 */
	lwkt_setpri_self(TDPRI_USER_NORM);
	userenter(lp->lwp_thread);
	userret(lp, frame, 0);
#ifdef KTRACE
	if (KTRPOINT(lp->lwp_thread, KTR_SYSRET))
		ktrsysret(p, SYS_fork, 0, 0);
#endif
	p->p_flag |= P_PASSIVE_ACQ;
	userexit(lp);
	p->p_flag &= ~P_PASSIVE_ACQ;
#ifdef SMP
	KKASSERT(lp->lwp_thread->td_mpcount == 1);
	rel_mplock();
#endif
}

/*
 * If PGEX_FPFAULT is set then set FP_VIRTFP in the PCB to force a T_DNA
 * fault (which is then passed back to the virtual kernel) if an attempt is
 * made to use the FP unit.
 *
 * XXX this is a fairly big hack.
 */
void
set_vkernel_fp(struct trapframe *frame)
{
	struct thread *td = curthread;

	if (frame->tf_xflags & PGEX_FPFAULT) {
		td->td_pcb->pcb_flags |= FP_VIRTFP;
		if (mdcpu->gd_npxthread == td)
			npxexit();
	} else {
		td->td_pcb->pcb_flags &= ~FP_VIRTFP;
	}
}

