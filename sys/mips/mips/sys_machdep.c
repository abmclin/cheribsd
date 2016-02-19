/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
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
 *	from: @(#)sys_machdep.c	5.5 (Berkeley) 1/19/91
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/syscall.h>
#include <sys/sysent.h>

#include <machine/cpufunc.h>
#include <machine/cpuinfo.h>
#include <machine/sysarch.h>
#include <machine/cpuregs.h>
#include <machine/tls.h>

#ifndef _SYS_SYSPROTO_H_
struct sysarch_args {
	int op;
	char *parms;
};
#endif

int
sysarch(struct thread *td, struct sysarch_args *uap)
{
	int error;
	void *tlsbase;

	switch (uap->op) {
	case MIPS_SET_TLS:
		td->td_md.md_tls = uap->parms;

		/*
		 * If there is an user local register implementation (ULRI)
		 * update it as well.  Add the TLS and TCB offsets so the
		 * value in this register is adjusted like in the case of the
		 * rdhwr trap() instruction handler.
		 *
		 * XXXSS For more information why this offset is required see:
		 * 	'git show c6be4f4d2d1b71c04de5d3bbb6933ce2dbcdb317'
		 */
		if (cpuinfo.userlocal_reg == true) {
#if defined(__mips_n64) && defined(COMPAT_FREEBSD32)
			if (SV_PROC_FLAG(td->td_proc, SV_ILP32))
				mips_wr_userlocal((unsigned long)(uap->parms +
				    TLS_TP_OFFSET + TLS_TCB_SIZE32));
			else
#endif
			mips_wr_userlocal((unsigned long)(uap->parms +
			    TLS_TP_OFFSET + TLS_TCB_SIZE));
		}
		return (0);

	case MIPS_GET_TLS:
		tlsbase = td->td_md.md_tls;
		error = copyout(&tlsbase, uap->parms, sizeof(tlsbase));
		return (error);

	case MIPS_GET_COUNT:
		td->td_retval[0] = mips_rd_count();
		return (0);

#ifdef CPU_CHERI
	case CHERI_GET_STACK:
		return (cheri_sysarch_getstack(td, uap));

	case CHERI_SET_STACK:
		return (cheri_sysarch_setstack(td, uap));

	case CHERI_GET_TYPECAP:
		return (cheri_sysarch_gettypecap(td, uap));
#endif

	default:
		break;
	}
	return (EINVAL);
}
