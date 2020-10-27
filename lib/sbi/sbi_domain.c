/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_hsm.h>
#include <sbi/sbi_math.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>

struct sbi_domain *hartid_to_domain_table[SBI_HARTMASK_MAX_BITS] = { 0 };
struct sbi_domain *domidx_to_domain_table[SBI_DOMAIN_MAX_INDEX] = { 0 };

static u32 domain_count = 0;

static struct sbi_hartmask root_hmask = { 0 };

#define ROOT_FW_REGION		0
#define ROOT_ALL_REGION	1
#define ROOT_END_REGION	2
static struct sbi_domain_memregion root_memregs[ROOT_END_REGION + 1] = { 0 };

static struct sbi_domain root = {
	.name = "root",
	.possible_harts = &root_hmask,
	.regions = root_memregs,
	.system_reset_allowed = TRUE,
};

bool sbi_domain_is_assigned_hart(const struct sbi_domain *dom, u32 hartid)
{
	if (dom)
		return sbi_hartmask_test_hart(hartid, &dom->assigned_harts);

	return FALSE;
}

ulong sbi_domain_get_assigned_hartmask(const struct sbi_domain *dom,
				       ulong hbase)
{
	ulong ret, bword, boff;

	if (!dom)
		return 0;

	bword = BIT_WORD(hbase);
	boff = BIT_WORD_OFFSET(hbase);

	ret = sbi_hartmask_bits(&dom->assigned_harts)[bword++] >> boff;
	if (boff && bword < BIT_WORD(SBI_HARTMASK_MAX_BITS)) {
		ret |= (sbi_hartmask_bits(&dom->assigned_harts)[bword] &
			(BIT(boff) - 1UL)) << (BITS_PER_LONG - boff);
	}

	return ret;
}

void sbi_domain_memregion_initfw(struct sbi_domain_memregion *reg)
{
	if (!reg)
		return;

	sbi_memcpy(reg, &root_memregs[ROOT_FW_REGION], sizeof(*reg));
}

bool sbi_domain_check_addr(const struct sbi_domain *dom,
			   unsigned long addr, unsigned long mode,
			   unsigned long access_flags)
{
	bool mmio = FALSE;
	struct sbi_domain_memregion *reg;
	unsigned long rstart, rend, rflags, rwx = 0;

	if (!dom)
		return FALSE;

	if (access_flags & SBI_DOMAIN_READ)
		rwx |= SBI_DOMAIN_MEMREGION_READABLE;
	if (access_flags & SBI_DOMAIN_WRITE)
		rwx |= SBI_DOMAIN_MEMREGION_WRITEABLE;
	if (access_flags & SBI_DOMAIN_EXECUTE)
		rwx |= SBI_DOMAIN_MEMREGION_EXECUTABLE;
	if (access_flags & SBI_DOMAIN_MMIO)
		mmio = TRUE;

	sbi_domain_for_each_memregion(dom, reg) {
		rflags = reg->flags;
		if (mode == PRV_M && !(rflags & SBI_DOMAIN_MEMREGION_MMODE))
			continue;

		rstart = reg->base;
		rend = (reg->order < __riscv_xlen) ?
			rstart + ((1UL << reg->order) - 1) : -1UL;
		if (rstart <= addr && addr <= rend) {
			if ((mmio && !(rflags & SBI_DOMAIN_MEMREGION_MMIO)) ||
			    (!mmio && (rflags & SBI_DOMAIN_MEMREGION_MMIO)))
				return FALSE;
			return ((rflags & rwx) == rwx) ? TRUE : FALSE;
		}
	}

	return (mode == PRV_M) ? TRUE : FALSE;
}

/* Check if region complies with constraints */
static bool is_region_valid(const struct sbi_domain_memregion *reg)
{
	if (reg->order < 3 || __riscv_xlen < reg->order)
		return FALSE;

	if (reg->base & (BIT(reg->order) - 1))
		return FALSE;

	return TRUE;
}

/** Check if regionA is sub-region of regionB */
static bool is_region_subset(const struct sbi_domain_memregion *regA,
			     const struct sbi_domain_memregion *regB)
{
	ulong regA_start = regA->base;
	ulong regA_end = regA->base + (BIT(regA->order) - 1);
	ulong regB_start = regB->base;
	ulong regB_end = regB->base + (BIT(regA->order) - 1);

	if ((regB_start <= regA_start) &&
	    (regA_start < regB_end) &&
	    (regB_start < regA_end) &&
	    (regA_end <= regB_end))
		return TRUE;

	return FALSE;
}

/** Check if regionA conflicts regionB */
static bool is_region_conflict(const struct sbi_domain_memregion *regA,
				const struct sbi_domain_memregion *regB)
{
	if ((is_region_subset(regA, regB) || is_region_subset(regB, regA)) &&
	    regA->flags == regB->flags)
		return TRUE;

	return FALSE;
}

/** Check if regionA should be placed before regionB */
static bool is_region_before(const struct sbi_domain_memregion *regA,
			     const struct sbi_domain_memregion *regB)
{
	if (regA->order < regB->order)
		return TRUE;

	if ((regA->order == regB->order) &&
	    (regA->base < regB->base))
		return TRUE;

	return FALSE;
}

static int sanitize_domain(const struct sbi_platform *plat,
			   struct sbi_domain *dom)
{
	u32 i, j, count;
	bool have_fw_reg;
	struct sbi_domain_memregion treg, *reg, *reg1;

	/* Check possible HARTs */
	if (!dom->possible_harts)
		return SBI_EINVAL;
	sbi_hartmask_for_each_hart(i, dom->possible_harts) {
		if (sbi_platform_hart_invalid(plat, i))
			return SBI_EINVAL;
	};

	/* Check memory regions */
	if (!dom->regions)
		return SBI_EINVAL;
	sbi_domain_for_each_memregion(dom, reg) {
		if (!is_region_valid(reg))
			return SBI_EINVAL;
	}

	/* Count memory regions and check presence of firmware region */
	count = 0;
	have_fw_reg = FALSE;
	sbi_domain_for_each_memregion(dom, reg) {
		if (reg->order == root_memregs[ROOT_FW_REGION].order &&
		    reg->base == root_memregs[ROOT_FW_REGION].base &&
		    reg->flags == root_memregs[ROOT_FW_REGION].flags)
			have_fw_reg = TRUE;
		count++;
	}
	if (!have_fw_reg)
		return SBI_EINVAL;

	/* Sort the memory regions */
	for (i = 0; i < (count - 1); i++) {
		reg = &dom->regions[i];
		for (j = i + 1; j < count; j++) {
			reg1 = &dom->regions[j];

			if (is_region_conflict(reg1, reg))
				return SBI_EINVAL;

			if (!is_region_before(reg1, reg))
				continue;

			sbi_memcpy(&treg, reg1, sizeof(treg));
			sbi_memcpy(reg1, reg, sizeof(treg));
			sbi_memcpy(reg, &treg, sizeof(treg));
		}
	}

	/*
	 * We don't need to check boot HART id of domain because if boot
	 * HART id is not possible/assigned to this domain then it won't
	 * be started at boot-time by sbi_domain_finalize().
	 */

	/*
	 * Check next mode
	 *
	 * We only allow next mode to be S-mode or U-mode.so that we can
	 * protect M-mode context and enforce checks on memory accesses.
	 */
	if (dom->next_mode != PRV_S &&
	    dom->next_mode != PRV_U)
		return SBI_EINVAL;

	/* Check next address and next mode*/
	if (!sbi_domain_check_addr(dom, dom->next_addr, dom->next_mode,
				   SBI_DOMAIN_EXECUTE))
		return SBI_EINVAL;

	return 0;
}

int sbi_domain_finalize(struct sbi_scratch *scratch, u32 cold_hartid)
{
	int rc;
	u32 i, j, dhart;
	bool dom_exists;
	struct sbi_domain *dom, *tdom;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	/* Discover domains */
	for (i = 0; i < SBI_HARTMASK_MAX_BITS; i++) {
		/* Ignore invalid HART */
		if (sbi_platform_hart_invalid(plat, i))
			continue;

		/* Get domain assigned to HART */
		dom = sbi_platform_domain_get(plat, i);
		if (!dom)
			continue;

		/* Check if domain already discovered */
		dom_exists = FALSE;
		sbi_domain_for_each(j, tdom) {
			if (tdom == dom) {
				dom_exists = TRUE;
				break;
			}
		}

		/* Newly discovered domain */
		if (!dom_exists) {
			/*
			 * Ensure that we have room for Domain Index to
			 * HART ID mapping
			 */
			if (domain_count <= SBI_DOMAIN_MAX_INDEX)
				return SBI_ENOSPC;

			/* Sanitize discovered domain */
			rc = sanitize_domain(plat, dom);
			if (rc)
				return rc;

			/* Assign index to domain */
			dom->index = domain_count++;
			domidx_to_domain_table[dom->index] = dom;

			/* Clear assigned HARTs of domain */
			sbi_hartmask_clear_all(&dom->assigned_harts);
		}

		/* Assign domain to HART if HART is a possible HART */
		if (sbi_hartmask_test_hart(i, dom->possible_harts)) {
			tdom = hartid_to_domain_table[i];
			if (tdom)
				sbi_hartmask_clear_hart(i,
						&tdom->assigned_harts);
			hartid_to_domain_table[i] = dom;
			sbi_hartmask_set_hart(i, &dom->assigned_harts);
		}
	}

	/* Startup boot HART of domains */
	sbi_domain_for_each(i, dom) {
		/* Domain boot HART */
		dhart = dom->boot_hartid;

		/* Ignore if boot HART not possible for this domain */
		if (!sbi_hartmask_test_hart(i, dom->possible_harts))
			continue;

		/* Ignore if boot HART assigned different domain */
		if (sbi_hartid_to_domain(dhart) != dom ||
		    !sbi_hartmask_test_hart(i, &dom->assigned_harts))
			continue;

		/* Startup boot HART of domain */
		if (dhart == cold_hartid) {
			scratch->next_addr = dom->next_addr;
			scratch->next_mode = dom->next_mode;
			scratch->next_arg1 = dom->next_arg1;
		} else {
			rc = sbi_hsm_hart_start(scratch, dhart, dom->next_addr,
					dom->next_mode, dom->next_arg1);
			if (rc)
				return rc;
		}
	}

	return 0;
}

int sbi_domain_init(struct sbi_scratch *scratch, u32 cold_hartid)
{
	u32 i;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	/* Root domain firmware memory region */
	root_memregs[ROOT_FW_REGION].order = log2roundup(scratch->fw_size);
	root_memregs[ROOT_FW_REGION].base = scratch->fw_start &
				~((1UL << root_memregs[0].order) - 1UL);
	root_memregs[ROOT_FW_REGION].flags = 0;

	/* Root domain allow everything memory region */
	root_memregs[ROOT_ALL_REGION].order = __riscv_xlen;
	root_memregs[ROOT_ALL_REGION].base = 0;
	root_memregs[ROOT_ALL_REGION].flags = (SBI_DOMAIN_MEMREGION_READABLE |
						SBI_DOMAIN_MEMREGION_WRITEABLE |
						SBI_DOMAIN_MEMREGION_EXECUTABLE);

	/* Root domain memory region end */
	root_memregs[ROOT_END_REGION].order = 0;

	/* Root domain boot HART id is same as coldboot HART id */
	root.boot_hartid = cold_hartid;

	/* Root domain next booting stage details */
	root.next_arg1 = scratch->next_arg1;
	root.next_addr = scratch->next_addr;
	root.next_mode = scratch->next_mode;

	/* Select root domain for all valid HARTs */
	for (i = 0; i < SBI_HARTMASK_MAX_BITS; i++) {
		if (sbi_platform_hart_invalid(plat, i))
			continue;
		sbi_hartmask_set_hart(i, &root_hmask);
		hartid_to_domain_table[i] = &root;
		sbi_hartmask_set_hart(i, &root.assigned_harts);
	}

	/* Set root domain index */
	root.index = domain_count++;
	domidx_to_domain_table[root.index] = &root;

	return 0;
}
