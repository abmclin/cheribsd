/*-
 * Copyright (c) 2014-2015 SRI International
 * Copyright (c) 2015 Robert N. M. Watson
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract (FA8750-10-C-0237)
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/queue.h>

#include <assert.h>
#include <elf.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sandbox_elf.h"

STAILQ_HEAD(sandbox_map_head, sandbox_map_entry);

struct sandbox_map_entry {
	STAILQ_ENTRY(sandbox_map_entry)	sme_entries;

	size_t	sme_map_offset;		/* Offset to sandbox start */
	size_t	sme_len;		/* Length of mapping */
	int	sme_prot;		/* Page protections */
	int	sme_flags;		/* Mmap flags */
	int	sme_fd;			/* File */
	off_t	sme_file_offset;	/* Offset in file */
	size_t	sme_tailbytes;		/* Bytes to zero on last page */
};

struct sandbox_map {
	struct sandbox_map_head	sm_head;
	size_t			sm_maxoffset;
};

static struct sandbox_map_entry *
sandbox_map_entry_new(size_t map_offset, size_t len, int prot, int flags,
    int fd, off_t file_offset, size_t tailbytes)
{
	struct sandbox_map_entry *sme;

	if ((sme = calloc(1, sizeof(*sme))) == NULL) {
		warn("%s: calloc", __func__);
		return (NULL);
	}
	sme->sme_map_offset = map_offset;
	sme->sme_len = len;
	sme->sme_prot = prot;
	sme->sme_flags = flags;
	sme->sme_fd = fd;
	sme->sme_file_offset = file_offset;
	sme->sme_tailbytes = tailbytes;

	return (sme);
}

static void *
sandbox_map_entry_mmap(void *base, struct sandbox_map_entry *sme)
{
	caddr_t taddr;
	void *addr;

	taddr = (caddr_t)base + sme->sme_map_offset;
#ifdef DEBUG
	if (sme->sme_fd > -1)
		printf("mapping 0x%zx bytes at %p, file offset 0x%zx\n",
		    sme->sme_len, taddr, sme->sme_file_offset);
	else
		printf("mapping 0x%zx bytes at 0x%p\n", sme->sme_len, taddr);
#endif
	if ((addr = mmap(taddr, sme->sme_len, sme->sme_prot, sme->sme_flags,
	    sme->sme_fd, sme->sme_file_offset)) == sandbox_map_entry_mmap) {
		warn("%s: mmap", __func__);
		return (addr);
	}
	assert(addr == taddr);

	/*
	 * XXXBD: Optimization opportunity.  Check contents and clear
	 * sme->sme_tailbytes if memset isn't useful.  Requires a
	 * first-pass flag.
	 */
	memset(taddr + sme->sme_len, 0, sme->sme_tailbytes);

	return (addr);
}

int
sandbox_map_load(void *base, struct sandbox_map *sm)
{
	struct sandbox_map_entry *sme;

	STAILQ_FOREACH(sme, &sm->sm_head, sme_entries) {
		if (sandbox_map_entry_mmap(base, sme) == MAP_FAILED)
			return (-1);
	}
	return (0);
}

void
sandbox_map_free(struct sandbox_map *sm)
{
	struct sandbox_map_entry *sme, *sme_temp;

	if (sm == NULL)
		return;

	STAILQ_FOREACH_SAFE(sme, &sm->sm_head, sme_entries, sme_temp) {
		STAILQ_REMOVE(&sm->sm_head, sme, sandbox_map_entry,
		    sme_entries);
		free(sme);
	}
	free(sm);
}

size_t
sandbox_map_maxoffset(struct sandbox_map *sm)
{

	return(sm->sm_maxoffset);
}

struct sandbox_map *
sandbox_parse_elf64(int fd, u_int flags)
{
	int i, prot;
	size_t taddr;
	ssize_t rlen;
	size_t maplen, mappedbytes, offset, tailbytes;
	Elf64_Ehdr ehdr;
	Elf64_Phdr phdr;
	struct sandbox_map *sm;
	struct sandbox_map_entry *sme;

	if ((sm = calloc(1, sizeof(*sm))) == NULL) {
		warn("%s: malloc sandbox_map", __func__);
		return (NULL);
	}
	STAILQ_INIT(&sm->sm_head);

	if ((rlen = pread(fd, &ehdr, sizeof(ehdr), 0)) != sizeof(ehdr)) {
		warn("%s: read ELF header", __func__);
		return (NULL);
	}

	/* XXX: check for magic number */

#ifdef DEBUG
	printf("type %d\n", ehdr.e_type);
	printf("version %d\n", ehdr.e_version);
	printf("entry %p\n", (void *)ehdr.e_entry);
	printf("elf header size %jd (read %jd)\n", (intmax_t)ehdr.e_ehsize,
	    rlen);
	printf("program header offset %jd\n", (intmax_t)ehdr.e_phoff);
	printf("program header size %jd\n", (intmax_t)ehdr.e_phentsize);
	printf("program header number %jd\n", (intmax_t)ehdr.e_phnum);
	printf("section header offset %jd\n", (intmax_t)ehdr.e_shoff);
	printf("section header size %jd\n", (intmax_t)ehdr.e_shentsize);
	printf("section header number %jd\n", (intmax_t)ehdr.e_shnum);
	printf("section name strings section %jd\n", (intmax_t)ehdr.e_shstrndx);
#endif

	for (i = 0; i < ehdr.e_phnum; i++) {
		if ((rlen = pread(fd, &phdr, sizeof(phdr), ehdr.e_phoff +
		    ehdr.e_phentsize * i)) != sizeof(phdr)) {
			warn("%s: reading %d program header", __func__, i+1);
			return (NULL);
		}
#ifdef DEBUG
		printf("phdr[%d] type        %jx\n", i, (intmax_t)phdr.p_type);
		printf("phdr[%d] flags       %jx (%c%c%c)\n", i,
		   (intmax_t)phdr.p_flags,
		   phdr.p_flags & PF_R ? 'r' : '-',
		   phdr.p_flags & PF_W ? 'w' : '-',
		   phdr.p_flags & PF_X ? 'x' : '-');
		printf("phdr[%d] offset      0x%0.16jx\n", i,
		    (intmax_t)phdr.p_offset);
		printf("phdr[%d] vaddr       0x%0.16jx\n", i,
		    (intmax_t)phdr.p_vaddr);
		printf("phdr[%d] file size   0x%0.16jx\n", i,
		    (intmax_t)phdr.p_filesz);
		printf("phdr[%d] memory size 0x%0.16jx\n", i,
		    (intmax_t)phdr.p_memsz);
#endif

		if (phdr.p_type != PT_LOAD) {
#ifdef DEBUG
			/* XXXBD: should we handled GNU_STACK? */
			printf("skipping program segment %d\n", i+1);
#endif
			continue;
		}

		/*
		 * Consider something 'data' if PF_X is unset; otherwise,
		 * consider it code.  Either way, load it only if requested by
		 * a suitable flag.
		 */
		if (phdr.p_flags & PF_X) {
#ifdef NOTYET
			/*
			 * XXXRW: Our current linker script will sometimes
			 * place data and code in the same page.  For now,
			 * map code into object instances.
			 */
			if (!(flags & SANDBOX_LOADELF_CODE))
				continue;
#endif
		} else {
			if (!(flags & SANDBOX_LOADELF_DATA))
				continue;
		}

		prot = (
		    (phdr.p_flags & PF_R ? PROT_READ : 0) |
		    (phdr.p_flags & PF_W ? PROT_WRITE : 0) |
		    (phdr.p_flags & PF_X ? PROT_EXEC : 0));
		/* XXXBD: write should not be required for code! */
		/* XXXBD: ideally read would not be required for code. */
		if (flags & SANDBOX_LOADELF_CODE)
			prot &= PROT_READ | PROT_WRITE | PROT_EXEC;
		if (flags & SANDBOX_LOADELF_DATA)
			prot &= PROT_READ | PROT_WRITE;

		taddr = rounddown2((phdr.p_vaddr), PAGE_SIZE);
		offset = rounddown2(phdr.p_offset, PAGE_SIZE);
		maplen = phdr.p_offset - rounddown2(phdr.p_offset, PAGE_SIZE)
		    + phdr.p_filesz;
		/* XXX-BD: rtld handles this, but I don't see why you would. */
		if (phdr.p_filesz != phdr.p_memsz && !(phdr.p_flags & PF_W)) {
			warnx("%s: segment %d expects 0 fill, but is not "
			    "writable, skipping", __func__, i+1);
			continue;
		}

		/* Calculate bytes to be zeroed in last page */
		mappedbytes = roundup2(maplen, PAGE_SIZE);
		tailbytes = mappedbytes - maplen;

		if ((sme = sandbox_map_entry_new(taddr, maplen, prot,
		    MAP_FIXED | MAP_PRIVATE | MAP_PREFAULT_READ,
		    fd, offset, tailbytes)) == NULL)
			goto error;
		STAILQ_INSERT_TAIL(&sm->sm_head, sme, sme_entries);

		sm->sm_maxoffset = MAX(sm->sm_maxoffset, phdr.p_vaddr +
		    phdr.p_memsz);

		/*
		 * If we would map everything directly or everything fit
		 * in the mapped range we're done.
		 */
		if (phdr.p_filesz == phdr.p_memsz || phdr.p_memsz <= mappedbytes)
			continue;

		taddr = taddr + mappedbytes;
		maplen = (phdr.p_offset - rounddown2(phdr.p_offset, PAGE_SIZE)) +
		    phdr.p_memsz - mappedbytes;

		if ((sme = sandbox_map_entry_new(taddr, maplen, prot,
		    MAP_FIXED | MAP_ANON, -1, 0, 0)) == NULL)
			goto error;
		STAILQ_INSERT_TAIL(&sm->sm_head, sme, sme_entries);
	}

	return (sm);
error:
	sandbox_map_free(sm);
	return (NULL);
}

#ifdef TEST_LOADELF64
static ssize_t
sandbox_loadelf64(int fd, void *base, size_t maxsize __unused, u_int flags)
{
	struct sandbox_map *sm;
	ssize_t maxoffset;

	assert((intptr_t)base % PAGE_SIZE == 0);

	if ((sm = sandbox_parse_elf64(fd, flags)) == NULL) {
		warnx("%s: sandbox_parse_elf64", __func__);
		return (-1);
	}
	if (sandbox_map_load(base, sm) == -1) {
		warnx("%s: sandbox_map_load", __func__);
		return (-1);
	}
	maxoffset = sm->sm_maxoffset;
	sandbox_map_free(sm);

	return (maxoffset);
}

int
main(int argc, char **argv)
{
	void *base;
	ssize_t len;
	size_t maxlen;
	int fd;

	if (argc != 2)
		errx(1, "usage: elf_loader <file>");

	maxlen = 10 * 1024 * 1024;
	base = mmap(NULL, maxlen, 0, MAP_ANON, -1, 0);
	if (base == MAP_FAILED)
		err(1, "%s: mmap region", __func__);

	if ((fd = open(argv[1], O_RDONLY)) == -1)
		err(1, "%s: open(%s)", __func__, argv[1]);

	if ((len = sandbox_loadelf64(fd, base, maxlen, SANDBOX_LOADELF_CODE))
	    == -1)
		err(1, "%s: sandbox_loadelf64", __func__);
	printf("mapped %jd bytes from %s\n", len, argv[1]);

	return (0);
}
#endif
