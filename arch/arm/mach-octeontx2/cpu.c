// SPDX-License-Identifier:    GPL-2.0
/*
 * Copyright (C) 2018 Marvell International Ltd.
 *
 * https://spdx.org/licenses
 */

#include <common.h>
#include <asm/armv8/mmu.h>
#include <asm/io.h>
#include <asm/arch/board.h>

DECLARE_GLOBAL_DATA_PTR;

#define OTX2_MEM_MAP_USED 4

/* +1 is end of list which needs to be empty */
#define OTX2_MEM_MAP_MAX (OTX2_MEM_MAP_USED + CONFIG_NR_DRAM_BANKS + 2)

static struct mm_region otx2_mem_map[OTX2_MEM_MAP_MAX] = {
	{
		.virt = 0x800000000000UL,
		.phys = 0x800000000000UL,
		.size = 0x40000000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE
	}, {
		.virt = 0x840000000000UL,
		.phys = 0x840000000000UL,
		.size = 0x40000000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE
	}, {
		.virt = 0x880000000000UL,
		.phys = 0x880000000000UL,
		.size = 0x40000000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE
	}, {
		.virt = 0x8c0000000000UL,
		.phys = 0x8c0000000000UL,
		.size = 0x40000000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE
	}
};

struct mm_region *mem_map = otx2_mem_map;

#define SHFW_REGION	0x3000000UL
void mem_map_fill(void)
{
	int banks = OTX2_MEM_MAP_USED;
	u32 dram_start = CONFIG_SYS_TEXT_BASE;

	/* Add 4K pci bootcmd buffer range for 96xx boards */
	if (otx_is_soc(CN96XX))
		dram_start -= 0x1000;
	for (int i = 0; i < CONFIG_NR_DRAM_BANKS; i++) {
		otx2_mem_map[banks].virt = dram_start;
		otx2_mem_map[banks].phys = dram_start;
		otx2_mem_map[banks].size = gd->ram_size;
		otx2_mem_map[banks].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
					    PTE_BLOCK_NON_SHARE;
		banks = banks + 1;
	}

	otx2_mem_map[banks].virt = dram_start - SHFW_REGION;
	otx2_mem_map[banks].phys = dram_start - SHFW_REGION;
	otx2_mem_map[banks].size = SHFW_REGION;
	otx2_mem_map[banks].attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
				    PTE_BLOCK_NON_SHARE;
}

u64 get_page_table_size(void)
{
	return 0x80000;
}

void reset_cpu(ulong addr)
{
}
