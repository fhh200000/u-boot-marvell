// SPDX-License-Identifier:    GPL-2.0+
/*
 * Copyright (C) 2016 Stefan Roese <sr@denx.de>
 * Copyright (C) 2020 Marvell International Ltd.
 *
 * https://spdx.org/licenses
 */

#include <common.h>
#include <console.h>
#include <dm.h>
#include <i2c.h>
#include <asm/io.h>
#include <asm/arch/cpu.h>
#include <asm/arch/soc.h>
#include <power/regulator.h>
#ifdef CONFIG_BOARD_CONFIG_EEPROM
#include <mvebu/cfg_eeprom.h>
#endif
#ifdef CONFIG_ARMV8_SEC_FIRMWARE_SUPPORT
#include <asm/armv8/sec_firmware.h>
#endif

#define CP_USB20_BASE_REG(cp, p)	(MVEBU_REGS_BASE_CP(0, cp) + \
						0x00580000 + 0x1000 * (p))
#define CP_USB20_TX_CTRL_REG(cp, p)	(CP_USB20_BASE_REG(cp, p) + 0xC)
#define CP_USB20_TX_OUT_AMPL_MASK	(0x7 << 20)
#define CP_USB20_TX_OUT_AMPL_VALUE	(0x3 << 20)

DECLARE_GLOBAL_DATA_PTR;

#define BOOTCMD_NAME	"pci-bootcmd"

__weak int soc_early_init_f(void)
{
	return 0;
}

int board_early_init_f(void)
{
	soc_early_init_f();

	return 0;
}

int board_early_init_r(void)
{
	if (CONFIG_IS_ENABLED(DM_REGULATOR)) {
		/* Check if any existing regulator should be turned down */
		regulators_enable_boot_off(false);
	}

	return 0;
}

int board_init(void)
{
	/* address of boot parameters */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;

#if (CONFIG_IS_ENABLED(BOARD_CONFIG_EEPROM))
		cfg_eeprom_init();
#endif

	return 0;
}

#if (CONFIG_IS_ENABLED(OCTEONTX_SERIAL_BOOTCMD) ||	\
	CONFIG_IS_ENABLED(OCTEONTX_SERIAL_PCIE_CONSOLE)) &&	\
	!CONFIG_IS_ENABLED(CONSOLE_MUX)
# error CONFIG_CONSOLE_MUX must be enabled!
#endif

#if CONFIG_IS_ENABLED(OCTEONTX_SERIAL_BOOTCMD)
static int init_bootcmd_console(void)
{
	int ret = 0;
	char *stdinname = env_get("stdin");
	struct udevice *bootcmd_dev = NULL;
	bool stdin_set;
	char iomux_name[128];

	debug("%s: stdin before: %s\n", __func__,
	      stdinname ? stdinname : "NONE");
	if (!stdinname) {
		env_set("stdin", "serial");
		stdinname = env_get("stdin");
	}
	stdin_set = !!strstr(stdinname, BOOTCMD_NAME);
	ret = uclass_get_device_by_driver(UCLASS_SERIAL,
					  DM_GET_DRIVER(octeontx_bootcmd),
					  &bootcmd_dev);
	if (ret) {
		pr_err("%s: Error getting %s serial class\n", __func__,
		       BOOTCMD_NAME);
	} else if (bootcmd_dev) {
		if (stdin_set)
			strncpy(iomux_name, stdinname, sizeof(iomux_name));
		else
			snprintf(iomux_name, sizeof(iomux_name), "%s,%s",
				 stdinname, bootcmd_dev->name);
		ret = iomux_doenv(stdin, iomux_name);
		if (ret)
			pr_err("%s: Error %d enabling the PCI bootcmd input console \"%s\"\n",
			       __func__, ret, iomux_name);
		if (!stdin_set)
			env_set("stdin", iomux_name);
	}
	debug("%s: Set iomux and stdin to %s (ret: %d)\n",
	      __func__, iomux_name, ret);
	return ret;
}
#endif
u64 fdt_get_board_info(void);
int board_late_init(void)
{
#if CONFIG_IS_ENABLED(OCTEONTX_SERIAL_BOOTCMD)
	if (init_bootcmd_console())
		printf("Failed to init bootcmd input\n");
#endif
	fdt_get_board_info();
	return 0;
}

void ft_cpu_setup(void *blob, struct bd_info *bd)
{
#ifdef CONFIG_ARMV8_SEC_FIRMWARE_SUPPORT
	fdt_fixup_kaslr(blob);
#endif
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	ft_cpu_setup(blob, bd);
	return 0;
}
