# Copyright (C) 2017 Texas Instruments Incorporated - http://www.ti.com/
#
# SPDX-License-Identifier:	GPL-2.0+
#

ifdef CONFIG_ARCH_OCTEONTX2
obj-$(CONFIG_CMD_BOOTIMGUP) += bootimgup.o
obj-$(CONFIG_CMD_FAILSAFE) += octeontx2_failsafe_cmds.o
obj-$(CONFIG_CMD_ETH) += eth_cmds.o
obj-$(CONFIG_CMD_MDIO_DBG) += mdio_dbg.o
obj-$(CONFIG_OCTEONTX2_CGX_INTF) += octeontx2_qlm_cmds.o
obj-$(CONFIG_CMD_ATTEST) += octeontx2_attest_cmds.o
obj-$(CONFIG_CMD_AVSSTATUS) += set_avsstatus.o
endif
ifdef CONFIG_ARCH_OCTEONTX
obj-$(CONFIG_CMD_BOOTIMGUP) += bootimgup.o
endif
ifdef CONFIG_ARCH_CN10K
obj-$(CONFIG_CMD_ETH) += eth_cmds-cn10k.o
obj-$(CONFIG_CMD_SERDES_DIAGNOSTICS) += serdes_diag-cn10k.o
obj-$(CONFIG_CMD_PHY_DIAGNOSTICS) += phy_diag.o
obj-$(CONFIG_CMD_EFI_CMDS) += efi_cmds.o
obj-$(CONFIG_CMD_BOOTIMGUP_CN10K) += bootimgup-cn10k.o
obj-$(CONFIG_CMD_FAILSAFE) += octeontx2_failsafe_cmds.o
obj-$(CONFIG_CMD_SWITCH_CMDS) += switch_cmds.o
endif
