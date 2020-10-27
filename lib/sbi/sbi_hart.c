/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_barrier.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_fp.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_csr_detect.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_math.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_trap.h>

extern void __sbi_expected_trap(void);
extern void __sbi_expected_trap_hext(void);

void (*sbi_hart_expected_trap)(void) = &__sbi_expected_trap;

struct hart_features {
	unsigned long features;
	unsigned int pmp_count;
	unsigned int mhpm_count;
};
static unsigned long hart_features_offset;

static void mstatus_init(struct sbi_scratch *scratch)
{
	unsigned long mstatus_val = 0;

	/* Enable FPU */
	if (misa_extension('D') || misa_extension('F'))
		mstatus_val |=  MSTATUS_FS;

	/* Enable Vector context */
	if (misa_extension('V'))
		mstatus_val |=  MSTATUS_VS;

	csr_write(CSR_MSTATUS, mstatus_val);

	/* Enable user/supervisor use of perf counters */
	if (misa_extension('S') &&
	    sbi_hart_has_feature(scratch, SBI_HART_HAS_SCOUNTEREN))
		csr_write(CSR_SCOUNTEREN, -1);
	if (sbi_hart_has_feature(scratch, SBI_HART_HAS_MCOUNTEREN))
		csr_write(CSR_MCOUNTEREN, -1);

	/* Disable all interrupts */
	csr_write(CSR_MIE, 0);

	/* Disable S-mode paging */
	if (misa_extension('S'))
		csr_write(CSR_SATP, 0);
}

static int fp_init(struct sbi_scratch *scratch)
{
#ifdef __riscv_flen
	int i;
#endif

	if (!misa_extension('D') && !misa_extension('F'))
		return 0;

	if (!(csr_read(CSR_MSTATUS) & MSTATUS_FS))
		return SBI_EINVAL;

#ifdef __riscv_flen
	for (i = 0; i < 32; i++)
		init_fp_reg(i);
	csr_write(CSR_FCSR, 0);
#endif

	return 0;
}

static int delegate_traps(struct sbi_scratch *scratch)
{
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	unsigned long interrupts, exceptions;

	if (!misa_extension('S'))
		/* No delegation possible as mideleg does not exist */
		return 0;

	/* Send M-mode interrupts and most exceptions to S-mode */
	interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
	exceptions = (1U << CAUSE_MISALIGNED_FETCH) | (1U << CAUSE_BREAKPOINT) |
		     (1U << CAUSE_USER_ECALL);
	if (sbi_platform_has_mfaults_delegation(plat))
		exceptions |= (1U << CAUSE_FETCH_PAGE_FAULT) |
			      (1U << CAUSE_LOAD_PAGE_FAULT) |
			      (1U << CAUSE_STORE_PAGE_FAULT);

	/*
	 * If hypervisor extension available then we only handle hypervisor
	 * calls (i.e. ecalls from HS-mode) in M-mode.
	 *
	 * The HS-mode will additionally handle supervisor calls (i.e. ecalls
	 * from VS-mode), Guest page faults and Virtual interrupts.
	 */
	if (misa_extension('H')) {
		exceptions |= (1U << CAUSE_VIRTUAL_SUPERVISOR_ECALL);
		exceptions |= (1U << CAUSE_FETCH_GUEST_PAGE_FAULT);
		exceptions |= (1U << CAUSE_LOAD_GUEST_PAGE_FAULT);
		exceptions |= (1U << CAUSE_VIRTUAL_INST_FAULT);
		exceptions |= (1U << CAUSE_STORE_GUEST_PAGE_FAULT);
	}

	csr_write(CSR_MIDELEG, interrupts);
	csr_write(CSR_MEDELEG, exceptions);

	return 0;
}

void sbi_hart_delegation_dump(struct sbi_scratch *scratch)
{
	if (!misa_extension('S'))
		/* No delegation possible as mideleg does not exist*/
		return;

#if __riscv_xlen == 32
	sbi_printf("MIDELEG : 0x%08lx\n", csr_read(CSR_MIDELEG));
	sbi_printf("MEDELEG : 0x%08lx\n", csr_read(CSR_MEDELEG));
#else
	sbi_printf("MIDELEG : 0x%016lx\n", csr_read(CSR_MIDELEG));
	sbi_printf("MEDELEG : 0x%016lx\n", csr_read(CSR_MEDELEG));
#endif
}

unsigned int sbi_hart_mhpm_count(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->mhpm_count;
}

unsigned int sbi_hart_pmp_count(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->pmp_count;
}

void sbi_hart_pmp_dump(struct sbi_scratch *scratch)
{
	unsigned long prot, addr, size, log2size;
	unsigned int i, pmp_count;

	pmp_count = sbi_hart_pmp_count(scratch);
	for (i = 0; i < pmp_count; i++) {
		pmp_get(i, &prot, &addr, &log2size);
		if (!(prot & PMP_A))
			continue;
		size = (log2size < __riscv_xlen) ? 1UL << log2size : 0;
#if __riscv_xlen == 32
		sbi_printf("PMP%d    : 0x%08lx-0x%08lx (A",
#else
		sbi_printf("PMP%d    : 0x%016lx-0x%016lx (A",
#endif
			   i, addr, addr + size - 1);
		if (prot & PMP_L)
			sbi_printf(",L");
		if (prot & PMP_R)
			sbi_printf(",R");
		if (prot & PMP_W)
			sbi_printf(",W");
		if (prot & PMP_X)
			sbi_printf(",X");
		sbi_printf(")\n");
	}
}

int sbi_hart_pmp_configure(struct sbi_scratch *scratch)
{
	u32 pmp_idx = 0;
	unsigned long fw_start, fw_size_log2;

	if (!sbi_hart_pmp_count(scratch))
		return 0;

	/* Firmware PMP region to protect OpenSBI firmware */
	fw_size_log2 = log2roundup(scratch->fw_size);
	fw_start = scratch->fw_start & ~((1UL << fw_size_log2) - 1UL);
	pmp_set(pmp_idx++, 0, fw_start, fw_size_log2);

	/*
	 * Default PMP region for allowing S-mode and U-mode access to
	 * memory not covered by:
	 * 1) Firmware PMP region
	 * 2) Platform specific PMP regions
	 */
	pmp_set(pmp_idx++, PMP_R | PMP_W | PMP_X, 0, __riscv_xlen);

	return 0;
}

/**
 * Check whether a particular hart feature is available
 *
 * @param scratch pointer to the HART scratch space
 * @param feature the feature to check
 * @returns true (feature available) or false (feature not available)
 */
bool sbi_hart_has_feature(struct sbi_scratch *scratch, unsigned long feature)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	if (hfeatures->features & feature)
		return true;
	else
		return false;
}

static unsigned long hart_get_features(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->features;
}

static inline char *sbi_hart_feature_id2string(unsigned long feature)
{
	char *fstr = NULL;

	if (!feature)
		return NULL;

	switch (feature) {
	case SBI_HART_HAS_SCOUNTEREN:
		fstr = "scounteren";
		break;
	case SBI_HART_HAS_MCOUNTEREN:
		fstr = "mcounteren";
		break;
	case SBI_HART_HAS_TIME:
		fstr = "time";
		break;
	default:
		break;
	}

	return fstr;
}

/**
 * Get the hart features in string format
 *
 * @param scratch pointer to the HART scratch space
 * @param features_str pointer to a char array where the features string will be
 *		       updated
 * @param nfstr length of the features_str. The feature string will be truncated
 *		if nfstr is not long enough.
 */
void sbi_hart_get_features_str(struct sbi_scratch *scratch,
			       char *features_str, int nfstr)
{
	unsigned long features, feat = 1UL;
	char *temp;
	int offset = 0;

	if (!features_str || nfstr <= 0)
		return;
	sbi_memset(features_str, 0, nfstr);

	features = hart_get_features(scratch);
	if (!features)
		goto done;

	do {
		if (features & feat) {
			temp = sbi_hart_feature_id2string(feat);
			if (temp) {
				sbi_snprintf(features_str + offset, nfstr,
					     "%s,", temp);
				offset = offset + sbi_strlen(temp) + 1;
			}
		}
		feat = feat << 1;
	} while (feat <= SBI_HART_HAS_LAST_FEATURE);

done:
	if (offset)
		features_str[offset - 1] = '\0';
	else
		sbi_strncpy(features_str, "none", nfstr);
}

static void hart_detect_features(struct sbi_scratch *scratch)
{
	struct sbi_trap_info trap = {0};
	struct hart_features *hfeatures;
	unsigned long val;

	/* Reset hart features */
	hfeatures = sbi_scratch_offset_ptr(scratch, hart_features_offset);
	hfeatures->features = 0;
	hfeatures->pmp_count = 0;
	hfeatures->mhpm_count = 0;

#define __check_csr(__csr, __rdonly, __wrval, __field, __skip)	\
	val = csr_read_allowed(__csr, (ulong)&trap);			\
	if (!trap.cause) {						\
		if (__rdonly) {						\
			(hfeatures->__field)++;				\
		} else {						\
			csr_write_allowed(__csr, (ulong)&trap, __wrval);\
			if (!trap.cause) {				\
				if (csr_swap(__csr, val) == __wrval)	\
					(hfeatures->__field)++;		\
				else					\
					goto __skip;			\
			} else {					\
				goto __skip;				\
			}						\
		}							\
	} else {							\
		goto __skip;						\
	}
#define __check_csr_2(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr(__csr + 1, __rdonly, __wrval, __field, __skip)
#define __check_csr_4(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_2(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_2(__csr + 2, __rdonly, __wrval, __field, __skip)
#define __check_csr_8(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_4(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_4(__csr + 4, __rdonly, __wrval, __field, __skip)
#define __check_csr_16(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_8(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_8(__csr + 8, __rdonly, __wrval, __field, __skip)
#define __check_csr_32(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_16(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_16(__csr + 16, __rdonly, __wrval, __field, __skip)
#define __check_csr_64(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_32(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_32(__csr + 32, __rdonly, __wrval, __field, __skip)

	/* Detect number of PMP regions */
	__check_csr_64(CSR_PMPADDR0, 0, 1UL, pmp_count, __pmp_skip);
__pmp_skip:

	/* Detect number of MHPM counters */
	__check_csr(CSR_MHPMCOUNTER3, 0, 1UL, mhpm_count, __mhpm_skip);
	__check_csr_4(CSR_MHPMCOUNTER4, 0, 1UL, mhpm_count, __mhpm_skip);
	__check_csr_8(CSR_MHPMCOUNTER8, 0, 1UL, mhpm_count, __mhpm_skip);
	__check_csr_16(CSR_MHPMCOUNTER16, 0, 1UL, mhpm_count, __mhpm_skip);
__mhpm_skip:

#undef __check_csr_64
#undef __check_csr_32
#undef __check_csr_16
#undef __check_csr_8
#undef __check_csr_4
#undef __check_csr_2
#undef __check_csr

	/* Detect if hart supports SCOUNTEREN feature */
	trap.cause = 0;
	val = csr_read_allowed(CSR_SCOUNTEREN, (unsigned long)&trap);
	if (!trap.cause) {
		csr_write_allowed(CSR_SCOUNTEREN, (unsigned long)&trap, val);
		if (!trap.cause)
			hfeatures->features |= SBI_HART_HAS_SCOUNTEREN;
	}

	/* Detect if hart supports MCOUNTEREN feature */
	trap.cause = 0;
	val = csr_read_allowed(CSR_MCOUNTEREN, (unsigned long)&trap);
	if (!trap.cause) {
		csr_write_allowed(CSR_MCOUNTEREN, (unsigned long)&trap, val);
		if (!trap.cause)
			hfeatures->features |= SBI_HART_HAS_MCOUNTEREN;
	}

	/* Detect if hart supports time CSR */
	trap.cause = 0;
	csr_read_allowed(CSR_TIME, (unsigned long)&trap);
	if (!trap.cause)
		hfeatures->features |= SBI_HART_HAS_TIME;
}

int sbi_hart_init(struct sbi_scratch *scratch, bool cold_boot)
{
	int rc;

	if (cold_boot) {
		if (misa_extension('H'))
			sbi_hart_expected_trap = &__sbi_expected_trap_hext;

		hart_features_offset = sbi_scratch_alloc_offset(
						sizeof(struct hart_features),
						"HART_FEATURES");
		if (!hart_features_offset)
			return SBI_ENOMEM;
	}

	hart_detect_features(scratch);

	mstatus_init(scratch);

	rc = fp_init(scratch);
	if (rc)
		return rc;

	rc = delegate_traps(scratch);
	if (rc)
		return rc;

	return 0;
}

void __attribute__((noreturn)) sbi_hart_hang(void)
{
	while (1)
		wfi();
	__builtin_unreachable();
}

void __attribute__((noreturn))
sbi_hart_switch_mode(unsigned long arg0, unsigned long arg1,
		     unsigned long next_addr, unsigned long next_mode,
		     bool next_virt)
{
#if __riscv_xlen == 32
	unsigned long val, valH;
#else
	unsigned long val;
#endif

	switch (next_mode) {
	case PRV_M:
		break;
	case PRV_S:
		if (!misa_extension('S'))
			sbi_hart_hang();
		break;
	case PRV_U:
		if (!misa_extension('U'))
			sbi_hart_hang();
		break;
	default:
		sbi_hart_hang();
	}

	val = csr_read(CSR_MSTATUS);
	val = INSERT_FIELD(val, MSTATUS_MPP, next_mode);
	val = INSERT_FIELD(val, MSTATUS_MPIE, 0);
#if __riscv_xlen == 32
	if (misa_extension('H')) {
		valH = csr_read(CSR_MSTATUSH);
		if (next_virt)
			valH = INSERT_FIELD(valH, MSTATUSH_MPV, 1);
		else
			valH = INSERT_FIELD(valH, MSTATUSH_MPV, 0);
		csr_write(CSR_MSTATUSH, valH);
	}
#else
	if (misa_extension('H')) {
		if (next_virt)
			val = INSERT_FIELD(val, MSTATUS_MPV, 1);
		else
			val = INSERT_FIELD(val, MSTATUS_MPV, 0);
	}
#endif
	csr_write(CSR_MSTATUS, val);
	csr_write(CSR_MEPC, next_addr);

	if (next_mode == PRV_S) {
		csr_write(CSR_STVEC, next_addr);
		csr_write(CSR_SSCRATCH, 0);
		csr_write(CSR_SIE, 0);
		csr_write(CSR_SATP, 0);
	} else if (next_mode == PRV_U) {
		csr_write(CSR_UTVEC, next_addr);
		csr_write(CSR_USCRATCH, 0);
		csr_write(CSR_UIE, 0);
	}

	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	__asm__ __volatile__("mret" : : "r"(a0), "r"(a1));
	__builtin_unreachable();
}
