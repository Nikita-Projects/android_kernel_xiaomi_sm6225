/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _DT_BINDINGS_CLK_QCOM_GCC_SA410M_H
#define _DT_BINDINGS_CLK_QCOM_GCC_SA410M_H

/* GCC clocks */
#define GPLL0							0
#define GPLL0_OUT_AUX2						1
#define GPLL4							2
#define GPLL6							3
#define GPLL6_OUT_MAIN						4
#define GPLL7							5
#define GPLL8							6
#define GCC_AHB2PHY_CSI_CLK					7
#define GCC_AHB2PHY_USB_CLK					8
#define GCC_BOOT_ROM_AHB_CLK					9
#define GCC_CAM_THROTTLE_RT_CLK					10
#define GCC_CFG_NOC_USB3_PRIM_AXI_CLK				11
#define GCC_CPUSS_AHB_CLK_SRC					12
#define GCC_CPUSS_AHB_POSTDIV_CLK_SRC				13
#define GCC_CPUSS_GNOC_CLK					14
#define GCC_DISP_THROTTLE_CORE_CLK				15
#define GCC_EMAC0_AXI_CLK					16
#define GCC_EMAC0_PHY_AUX_CLK					17
#define GCC_EMAC0_PHY_AUX_CLK_SRC				18
#define GCC_EMAC0_PTP_CLK					19
#define GCC_EMAC0_PTP_CLK_SRC					20
#define GCC_EMAC0_RGMII_CLK					21
#define GCC_EMAC0_RGMII_CLK_SRC					22
#define GCC_EMAC0_SLV_AHB_CLK					23
#define GCC_GP1_CLK						24
#define GCC_GP1_CLK_SRC						25
#define GCC_GP2_CLK						26
#define GCC_GP2_CLK_SRC						27
#define GCC_GP3_CLK						28
#define GCC_GP3_CLK_SRC						29
#define GCC_GPU_IREF_EN						30
#define GCC_PCIE_0_AUX_CLK					31
#define GCC_PCIE_0_AUX_CLK_SRC					32
#define GCC_PCIE_0_CFG_AHB_CLK					33
#define GCC_PCIE_0_MSTR_AXI_CLK					34
#define GCC_PCIE_0_PIPE_CLK					35
#define GCC_PCIE_0_PIPE_CLK_SRC					36
#define GCC_PCIE_0_SLV_AXI_CLK					37
#define GCC_PCIE_0_SLV_Q2A_AXI_CLK				38
#define GCC_PCIE_THROTTLE_NRT_CLK				39
#define GCC_PCIE_THROTTLE_XO_CLK				40
#define GCC_PDM2_CLK						41
#define GCC_PDM2_CLK_SRC					42
#define GCC_PDM_AHB_CLK						43
#define GCC_PDM_XO4_CLK						44
#define GCC_PWM0_XO512_CLK					45
#define GCC_QMIP_PCIE_AHB_CLK					46
#define GCC_QUPV3_WRAP0_CORE_2X_CLK				47
#define GCC_QUPV3_WRAP0_CORE_CLK				48
#define GCC_QUPV3_WRAP0_S0_CLK					49
#define GCC_QUPV3_WRAP0_S0_CLK_SRC				50
#define GCC_QUPV3_WRAP0_S1_CLK					51
#define GCC_QUPV3_WRAP0_S1_CLK_SRC				52
#define GCC_QUPV3_WRAP0_S2_CLK					53
#define GCC_QUPV3_WRAP0_S2_CLK_SRC				54
#define GCC_QUPV3_WRAP0_S3_CLK					55
#define GCC_QUPV3_WRAP0_S3_CLK_SRC				56
#define GCC_QUPV3_WRAP0_S4_CLK					57
#define GCC_QUPV3_WRAP0_S4_CLK_SRC				58
#define GCC_QUPV3_WRAP0_S5_CLK					59
#define GCC_QUPV3_WRAP0_S5_CLK_SRC				60
#define GCC_QUPV3_WRAP_0_M_AHB_CLK				61
#define GCC_QUPV3_WRAP_0_S_AHB_CLK				62
#define GCC_SDCC1_AHB_CLK					63
#define GCC_SDCC1_APPS_CLK					64
#define GCC_SDCC1_APPS_CLK_SRC					65
#define GCC_SDCC1_ICE_CORE_CLK					66
#define GCC_SDCC1_ICE_CORE_CLK_SRC				67
#define GCC_SDCC2_AHB_CLK					68
#define GCC_SDCC2_APPS_CLK					69
#define GCC_SDCC2_APPS_CLK_SRC					70
#define GCC_SYS_NOC_CPUSS_AHB_CLK				71
#define GCC_SYS_NOC_USB3_PRIM_AXI_CLK				72
#define GCC_UFS_CLKREF_EN					73
#define GCC_USB30_PRIM_MASTER_CLK				74
#define GCC_USB30_PRIM_MASTER_CLK_SRC				75
#define GCC_USB30_PRIM_MOCK_UTMI_CLK				76
#define GCC_USB30_PRIM_MOCK_UTMI_CLK_SRC			77
#define GCC_USB30_PRIM_MOCK_UTMI_POSTDIV_CLK_SRC		78
#define GCC_USB30_PRIM_SLEEP_CLK				79
#define GCC_USB3_PRIM_CLKREF_EN					80
#define GCC_USB3_PRIM_PHY_AUX_CLK_SRC				81
#define GCC_USB3_PRIM_PHY_COM_AUX_CLK				82
#define GCC_USB3_PRIM_PHY_PIPE_CLK				83
#define GCC_USB3_PRIM_PHY_PIPE_CLK_SRC				84

/* GCC resets */
#define GCC_EMAC0_BCR						0
#define GCC_MMSS_BCR						1
#define GCC_PCIE_0_BCR						2
#define GCC_PCIE_0_LINK_DOWN_BCR				3
#define GCC_PCIE_0_NOCSR_COM_PHY_BCR				4
#define GCC_PCIE_0_PHY_BCR					5
#define GCC_PDM_BCR						6
#define GCC_QUPV3_WRAPPER_0_BCR					7
#define GCC_SDCC1_BCR						8
#define GCC_SDCC2_BCR						9
#define GCC_USB2_PHY_SEC_BCR					10
#define GCC_USB30_PRIM_BCR					11
#define GCC_USB3_DP_PHY_PRIM_BCR				12
#define GCC_USB3_DP_PHY_PRIM_SP0_BCR				13
#define GCC_USB3_DP_PHY_PRIM_SP1_BCR				14
#define GCC_USB3_PHY_PRIM_SP0_BCR				15
#define GCC_USB3_PHY_PRIM_SP1_BCR				16
#define GCC_USB3_UNIPHY_MP0_BCR					17
#define GCC_USB3_UNIPHY_MP1_BCR					18
#define GCC_USB3PHY_PHY_PRIM_SP0_BCR				19
#define GCC_USB3PHY_PHY_PRIM_SP1_BCR				20
#define GCC_USB3PHY_PHY_SEC_BCR					21
#define GCC_USB3UNIPHY_PHY_MP0_BCR				22
#define GCC_USB3UNIPHY_PHY_MP1_BCR				23
#define GCC_USB_PHY_CFG_AHB2PHY_BCR				24
#define GCC_QUSB2PHY_PRIM_BCR					25

#endif
