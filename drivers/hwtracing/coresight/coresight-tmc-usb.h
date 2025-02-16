/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _CORESIGHT_TMC_USB_H
#define _CORESIGHT_TMC_USB_H

#include <linux/usb_bam.h>
#include <linux/amba/bus.h>
#include <linux/msm-sps.h>
#include <linux/usb/usb_qdss.h>
#include <linux/iommu.h>

#define TMC_USB_BAM_PIPE_INDEX	0
#define TMC_USB_BAM_NR_PIPES	2

enum tmc_etr_usb_mode {
	TMC_ETR_USB_NONE,
	TMC_ETR_USB_BAM_TO_BAM,
	TMC_ETR_USB_SW,
};

struct tmc_usb_data {
	enum tmc_etr_usb_mode	usb_mode;
	struct usb_qdss_ch	*usbch;
	struct tmc_usb_bam_data	*bamdata;
	struct tmc_drvdata	*tmcdrvdata;
	bool			data_overwritten;
	bool			enable_to_bam;
	u64			drop_data_size;
	u32			buf_size;
};

extern int tmc_usb_enable(struct tmc_usb_data *usb_data);
extern void tmc_usb_disable(struct tmc_usb_data *usb_data);

static inline int tmc_usb_qdss_alloc_req(struct usb_qdss_ch *ch, int n_write)
{

#if IS_ENABLED(CONFIG_USB_F_QDSS)
	return usb_qdss_alloc_req(ch, n_write);
#else
	return usb_qdss_alloc_req(ch, n_write, 0);
#endif

}

#endif
