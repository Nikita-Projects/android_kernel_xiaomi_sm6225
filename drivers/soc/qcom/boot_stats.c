// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2019, 2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/uaccess.h>
#include <soc/qcom/boot_stats.h>
#include <soc/qcom/soc_sleep_stats.h>
#include <linux/hashtable.h>
#include <clocksource/arm_arch_timer.h>

#define MARKER_STRING_WIDTH 50
#define TS_WHOLE_NUM_WIDTH 8
#define TS_PRECISION_WIDTH 3
/* Field width to consider the spaces, 's' character and \n */
#define TIME_FIELD_MISC 4
#define TIME_FIELD_WIDTH \
	(TS_WHOLE_NUM_WIDTH + TS_PRECISION_WIDTH + TIME_FIELD_MISC)

#define MARKER_TOTAL_LEN (MARKER_STRING_WIDTH + TIME_FIELD_WIDTH)
#define MAX_NUM_MARKERS (PAGE_SIZE * 4 / MARKER_TOTAL_LEN)
#define BOOTKPI_BUF_SIZE (PAGE_SIZE * 4)
#define TIMER_KHZ 32768
#define MSM_ARCH_TIMER_FREQ     19200000

#if defined(CONFIG_MSM_BOOT_TIME_MARKER) && !defined(CONFIG_MSM_GVM_BOOT_TIME_MARKER)
struct boot_stats {
	uint32_t bootloader_start;
	uint32_t bootloader_end;
	uint32_t bootloader_load_boot_start;
	uint32_t bootloader_load_boot_end;
	uint32_t bootloader_load_vendor_boot_start;
	uint32_t bootloader_load_vendor_boot_end;
	uint32_t bootloader_load_init_boot_start;
	uint32_t bootloader_load_init_boot_end;
} __packed;

static struct boot_stats __iomem *boot_stats;
#endif

static void __iomem *mpm_counter_base;
static uint32_t mpm_counter_freq;
static void __iomem *mpm_hr_counter_base;

#ifdef CONFIG_MSM_BOOT_TIME_MARKER

struct boot_marker {
	char marker_name[MARKER_STRING_WIDTH];
	unsigned long long timer_value;
	struct list_head list;
	struct hlist_node hash;
	spinlock_t slock;
};

static struct boot_marker boot_marker_list;
static struct kobject *bootkpi_obj;
static int num_markers;
static DECLARE_HASHTABLE(marker_htable, 5);


#if IS_ENABLED(CONFIG_QCOM_SOC_SLEEP_STATS)
static u64 get_time_in_msec(u64 counter)
{
	counter *= MSEC_PER_SEC;
	do_div(counter, MSM_ARCH_TIMER_FREQ);
	return counter;
}

static void measure_wake_up_time(void)
{
	u64 wake_up_time, deep_sleep_exit_time, current_time;
	char wakeup_marker[50] = {0,};

	current_time = arch_timer_read_counter();
	deep_sleep_exit_time = get_aosd_sleep_exit_time();

	if (deep_sleep_exit_time) {
		wake_up_time = get_time_in_msec(current_time - deep_sleep_exit_time);
		pr_debug("Current= %llu, wakeup=%llu, kpi=%llu msec\n",
				current_time, deep_sleep_exit_time,
				wake_up_time);
		snprintf(wakeup_marker, sizeof(wakeup_marker),
				"M - STR Wakeup : %llu ms", wake_up_time);
		destroy_marker("M - STR Wakeup");
		place_marker(wakeup_marker);
	} else
		destroy_marker("M - STR Wakeup");
}

/**
 * boot_kpi_pm_notifier() - PM notifier callback function.
 * @nb:		Pointer to the notifier block.
 * @event:	Suspend state event from PM module.
 * @unused:	Null pointer from PM module.
 *
 * This function is register as callback function to get notifications
 * from the PM module on the system suspend state.
 */
static int boot_kpi_pm_notifier(struct notifier_block *nb,
				  unsigned long event, void *unused)
{
	if (event == PM_POST_SUSPEND)
		measure_wake_up_time();
	return NOTIFY_DONE;
}

static struct notifier_block boot_kpi_pm_nb = {
	.notifier_call = boot_kpi_pm_notifier,
};
#endif

unsigned long long msm_timer_get_sclk_ticks(void)
{
	unsigned long long t1, t2;
	int loop_count = 10;
	int loop_zero_count = 3;
	u64 tmp = USEC_PER_SEC;
	void __iomem *sclk_tick;

	do_div(tmp, TIMER_KHZ);
	tmp /= (loop_zero_count-1);
	sclk_tick = mpm_counter_base;
	if (!sclk_tick)
		return -EINVAL;

	while (loop_zero_count--) {
		t1 = readl_relaxed(sclk_tick);
		do {
			udelay(1);
			t2 = t1;
			t1 = readl_relaxed(sclk_tick);
		} while ((t2 != t1) && --loop_count);
		if (!loop_count) {
			pr_err("boot_stats: SCLK  did not stabilize\n");
			return 0;
		}
		if (t1)
			break;

		udelay(tmp);
	}
	if (!loop_zero_count) {
		pr_err("boot_stats: SCLK reads zero\n");
		return 0;
	}
	return t1;
}
EXPORT_SYMBOL(msm_timer_get_sclk_ticks);

unsigned long long msm_hr_timer_get_sclk_ticks(void)
{
	unsigned long long tl, th;
	void __iomem *sclk_tick_high;
	void __iomem *sclk_tick_low;

	if (!mpm_hr_counter_base)
		return -EINVAL;

	sclk_tick_high = mpm_hr_counter_base + 0xc;
	sclk_tick_low = mpm_hr_counter_base + 0x8;

	tl = readl_relaxed(sclk_tick_low);
	th = readl_relaxed(sclk_tick_high);

	return (th << 32) | tl;
}
EXPORT_SYMBOL(msm_hr_timer_get_sclk_ticks);

static void _destroy_boot_marker(const char *name)
{
	struct boot_marker *marker;
	struct boot_marker *temp_addr;
	unsigned long flags;

	spin_lock_irqsave(&boot_marker_list.slock, flags);
	list_for_each_entry_safe(marker, temp_addr, &boot_marker_list.list,
			list) {
		if (strnstr(marker->marker_name, name,
			 strlen(marker->marker_name))) {
			num_markers--;
			hash_del(&marker->hash);
			list_del(&marker->list);
			kfree(marker);
		}
	}
	spin_unlock_irqrestore(&boot_marker_list.slock, flags);
}

/*
 * Function to calculate the cumulative sum of all
 * the characters in the string
 */
static unsigned int calculate_marker_charsum(const char *name)
{
	unsigned int sum = 0;
	int len = strlen(name);

	do {
		sum += (unsigned int)name[--len];
	} while (len);

	return sum;
}

static struct boot_marker *find_entry(const char *name)
{
	struct boot_marker *marker;
	unsigned int sum = calculate_marker_charsum(name);

	hash_for_each_possible(marker_htable, marker, hash, sum) {
		if (!strcmp(marker->marker_name, name))
			return marker;
	}

	return NULL;
}

static void _create_boot_marker(const char *name,
		unsigned long long timer_value)
{
	struct boot_marker *new_boot_marker;
	struct boot_marker *marker;
	unsigned long flags;
	unsigned int sum;

	if (num_markers >= MAX_NUM_MARKERS) {
		pr_err("boot_stats: Cannot create marker %s. Limit exceeded!\n",
			name);
		return;
	}

	marker = find_entry(name);
	if (marker) {
		marker->timer_value = timer_value;
		return;
	}

	pr_debug("%-*s%*llu.%0*llu seconds\n",
			MARKER_STRING_WIDTH, name,
			TS_WHOLE_NUM_WIDTH, timer_value/TIMER_KHZ,
			TS_PRECISION_WIDTH, ((timer_value % TIMER_KHZ)
			 * 1000) / TIMER_KHZ);

	new_boot_marker = kmalloc(sizeof(*new_boot_marker), GFP_ATOMIC);
	if (!new_boot_marker)
		return;

	strscpy(new_boot_marker->marker_name, name,
			sizeof(new_boot_marker->marker_name));
	new_boot_marker->timer_value = timer_value;
	sum = calculate_marker_charsum(new_boot_marker->marker_name);

	spin_lock_irqsave(&boot_marker_list.slock, flags);
	list_add_tail(&(new_boot_marker->list), &(boot_marker_list.list));
	hash_add(marker_htable, &new_boot_marker->hash, sum);
	spin_unlock_irqrestore(&boot_marker_list.slock, flags);
	num_markers++;
}

static void boot_marker_cleanup(void)
{
	struct boot_marker *marker;
	struct boot_marker *temp_addr;
	unsigned long flags;

	spin_lock_irqsave(&boot_marker_list.slock, flags);
	list_for_each_entry_safe(marker, temp_addr, &boot_marker_list.list,
			list) {
		num_markers--;
		hash_del(&marker->hash);
		list_del(&marker->list);
		kfree(marker);
	}
	spin_unlock_irqrestore(&boot_marker_list.slock, flags);
}

void place_marker(const char *name)
{
	_create_boot_marker((char *)name, msm_timer_get_sclk_ticks());
}
EXPORT_SYMBOL(place_marker);

void destroy_marker(const char *name)
{
	_destroy_boot_marker((char *) name);
}
EXPORT_SYMBOL(destroy_marker);

#ifndef CONFIG_MSM_GVM_BOOT_TIME_MARKER
static void set_bootloader_stats(void)
{
	unsigned long long ts1, ts2;

	if (IS_ERR_OR_NULL(boot_stats)) {
		pr_err("boot_marker: imem not initialized!\n");
		return;
	}

	_create_boot_marker("M - APPSBL Start - ",
			readl_relaxed(&boot_stats->bootloader_start));

	ts1 = readl_relaxed(&boot_stats->bootloader_load_boot_start);
	if (ts1) {
		_create_boot_marker("M - APPSBL Boot Load Start - ", ts1);
		ts2 = readl_relaxed(&boot_stats->bootloader_load_boot_end);
		_create_boot_marker("M - APPSBL Boot Load End - ", ts2);
		_create_boot_marker("D - APPSBL Boot Load Time - ", ts2 - ts1);
	}

	ts1 = readl_relaxed(&boot_stats->bootloader_load_vendor_boot_start);
	if (ts1) {
		_create_boot_marker("M - APPSBL Vendor Boot Load Start - ", ts1);
		ts2 = readl_relaxed(&boot_stats->bootloader_load_vendor_boot_end);
		_create_boot_marker("M - APPSBL Vendor Boot Load End - ", ts2);
		_create_boot_marker("D - APPSBL Vendor Boot Load Time - ", ts2 - ts1);
	}

	ts1 = readl_relaxed(&boot_stats->bootloader_load_init_boot_start);
	if (ts1) {
		_create_boot_marker("M - APPSBL Init Boot Load Start - ", ts1);
		ts2 = readl_relaxed(&boot_stats->bootloader_load_init_boot_end);
		_create_boot_marker("M - APPSBL Init Boot Load End - ", ts2);
		_create_boot_marker("D - APPSBL Init Load Time - ", ts2 - ts1);
	}

	_create_boot_marker("M - APPSBL End - ",
			readl_relaxed(&boot_stats->bootloader_end));
}

static int imem_boots_stat_parse_dt(void)
{
	struct device_node *np_imem;

	np_imem = of_find_compatible_node(NULL, NULL,
				"qcom,msm-imem-boot_stats");
	if (!np_imem) {
		pr_err("can't find qcom,msm-imem node\n");
		return -ENODEV;
	}
	boot_stats = of_iomap(np_imem, 0);
	if (!boot_stats) {
		pr_err("boot_stats: Can't map imem\n");
		of_node_put(np_imem);
		return -ENODEV;
	}

	return 0;
}
static void print_boot_stats(void)
{
	pr_info("KPI: Bootloader start count = %u\n",
		readl_relaxed(&boot_stats->bootloader_start));
	pr_info("KPI: Bootloader end count = %u\n",
		readl_relaxed(&boot_stats->bootloader_end));
	pr_info("KPI: Bootloader load kernel count = %u\n",
		readl_relaxed(&boot_stats->bootloader_load_boot_end) -
		readl_relaxed(&boot_stats->bootloader_load_boot_start));
	pr_info("KPI: Kernel MPM timestamp = %u\n",
		readl_relaxed(mpm_counter_base));
	pr_info("KPI: Kernel MPM Clock frequency = %u\n",
		mpm_counter_freq);
}

static int print_bootloader_stats(void)
{
	int ret;

	ret = imem_boots_stat_parse_dt();
	if (ret < 0)
		return -ENODEV;
	print_boot_stats();

#ifdef CONFIG_MSM_BOOT_TIME_MARKER
	set_bootloader_stats();
#endif
	return 0;

}
#endif
void update_marker(const char *name)
{
	_destroy_boot_marker(name);
	_create_boot_marker(name, msm_timer_get_sclk_ticks());
}
EXPORT_SYMBOL(update_marker);

static ssize_t bootkpi_reader(struct file *fp, struct kobject *obj,
		struct bin_attribute *bin_attr, char *user_buffer, loff_t off,
		size_t count)
{
	struct boot_marker *marker;
	unsigned long long ts_whole_num, ts_precision;
	unsigned long flags;
	static char *kpi_buf;
	static int temp;
	int ret = 0;

	if (!kpi_buf) {
		kpi_buf = kmalloc(BOOTKPI_BUF_SIZE, GFP_KERNEL);
		if (!kpi_buf)
			return -ENOMEM;
	}

	if (!temp) {
		spin_lock_irqsave(&boot_marker_list.slock, flags);
		list_for_each_entry(marker, &boot_marker_list.list, list) {
			WARN_ON((BOOTKPI_BUF_SIZE - temp) <= 0);

			ts_whole_num = marker->timer_value/TIMER_KHZ;
			ts_precision = ((marker->timer_value % TIMER_KHZ)
					* 1000)	/ TIMER_KHZ;

			/*
			 * Field width of
			 * Marker name		- MARKER_STRING_WIDTH
			 * Timestamp		- TS_WHOLE_NUM_WIDTH
			 * Timestamp precision	- TS_PRECISION_WIDTH
			 */
			temp += scnprintf(kpi_buf + temp,
					BOOTKPI_BUF_SIZE - temp,
					"%-*s%*llu.%0*llu s\n",
					MARKER_STRING_WIDTH,
					marker->marker_name,
					TS_WHOLE_NUM_WIDTH, ts_whole_num,
					TS_PRECISION_WIDTH, ts_precision);


		}

		spin_unlock_irqrestore(&boot_marker_list.slock, flags);
	}

	if (temp - off > count)
		ret = scnprintf(user_buffer, count, "%s", kpi_buf + off);
	else
		ret = scnprintf(user_buffer, temp + 1 - off, "%s", kpi_buf + off);

	if (ret == 0) {
		kfree(kpi_buf);
		kpi_buf = NULL;
		temp = 0;
	}
	return ret;
}

static ssize_t bootkpi_writer(struct file *fp, struct kobject *obj,
		struct bin_attribute *bin_attr, char *user_buffer, loff_t off,
		size_t count)
{
	int rc = 0;
	char buf[MARKER_STRING_WIDTH];

	if (count >= MARKER_STRING_WIDTH)
		return -EINVAL;

	rc = scnprintf(buf, sizeof(buf) - 1, "%s", user_buffer);
	if (rc < 0)
		return rc;

	buf[rc] = '\0';
	update_marker(buf);
	return rc;
}

static ssize_t mpm_timer_read(struct kobject *obj, struct kobj_attribute *attr,
		char *user_buffer)
{
	unsigned long long timer_value;
	char buf[100];
	int temp = 0;

	timer_value = msm_timer_get_sclk_ticks();

	temp = scnprintf(buf, sizeof(buf), "%llu.%03llu seconds\n",
			timer_value/TIMER_KHZ,
			(((timer_value % TIMER_KHZ) * 1000) / TIMER_KHZ));

	return scnprintf(user_buffer, temp + 1, "%s\n", buf);
}

static struct bin_attribute kpi_values_attribute =
	__BIN_ATTR(kpi_values, 0664, bootkpi_reader, bootkpi_writer, 0);

static struct kobj_attribute mpm_timer_attribute =
	__ATTR(mpm_timer, 0444, mpm_timer_read, NULL);

static int bootkpi_sysfs_init(void)
{
	int ret;

	bootkpi_obj = kobject_create_and_add("boot_kpi", kernel_kobj);
	if (!bootkpi_obj) {
		pr_err("boot_marker: Could not create kobject\n");
		ret = -ENOMEM;
		goto kobj_err;
	}

	ret = sysfs_create_file(bootkpi_obj, &mpm_timer_attribute.attr);
	if (ret) {
		pr_err("boot_marker: Could not create sysfs file\n");
		goto err;
	}

	ret = sysfs_create_bin_file(bootkpi_obj, &kpi_values_attribute);
	if (ret) {
		pr_err("boot_marker: Could not create sysfs bin file\n");
		sysfs_remove_file(bootkpi_obj, &mpm_timer_attribute.attr);
	}

	return 0;
err:
	kobject_del(bootkpi_obj);
kobj_err:
	return ret;
}

static int init_bootkpi(void)
{
	int ret = 0;

	ret = bootkpi_sysfs_init();
	if (ret)
		return ret;

	INIT_LIST_HEAD(&boot_marker_list.list);
	spin_lock_init(&boot_marker_list.slock);

#if IS_ENABLED(CONFIG_QCOM_SOC_SLEEP_STATS)
	ret = register_pm_notifier(&boot_kpi_pm_nb);
	if (ret)
		pr_err("boot_marker: power state notif error\n");
#endif

	return 0;
}

static void exit_bootkpi(void)
{
	boot_marker_cleanup();
	sysfs_remove_file(bootkpi_obj, &mpm_timer_attribute.attr);
	sysfs_remove_bin_file(bootkpi_obj, &kpi_values_attribute);
	kobject_del(bootkpi_obj);
}
#endif

static int mpm_parse_dt(void)
{
	struct device_node *np_mpm2, *np_mpm_hr;

	np_mpm2 = of_find_compatible_node(NULL, NULL,
				"qcom,mpm2-sleep-counter");
	if (!np_mpm2) {
		pr_err("mpm_counter: can't find DT node\n");
		return -ENODEV;
	}

	if (of_property_read_u32(np_mpm2, "clock-frequency", &mpm_counter_freq))
		goto err;

	if (of_get_address(np_mpm2, 0, NULL, NULL)) {
		mpm_counter_base = of_iomap(np_mpm2, 0);
		if (!mpm_counter_base) {
			pr_err("mpm_counter: cant map counter base\n");
			goto err;
		}
	} else
		goto err;

	/* qcom,mpm-hr-counter is not mandatory */
	np_mpm_hr = of_find_compatible_node(NULL, NULL,
				"qcom,mpm-hr-counter");
	if (!np_mpm_hr) {
		pr_info("mpm_hr_counter: can't find DT node\n");
		return 0;
	}

	if (of_get_address(np_mpm_hr, 0, NULL, NULL)) {
		mpm_hr_counter_base = of_iomap(np_mpm_hr, 0);
		if (!mpm_hr_counter_base) {
			pr_err("mpm_hr_counter: cant map counter base\n");
			of_node_put(np_mpm_hr);
		}
	} else
		of_node_put(np_mpm_hr);

	return 0;
err:
	of_node_put(np_mpm2);
	return -ENODEV;
}

static int __init boot_stats_init(void)
{
	int ret;

	ret = mpm_parse_dt();
	if (ret < 0)
		return -ENODEV;

	if (boot_marker_enabled()) {
		ret = init_bootkpi();
		if (ret) {
			pr_err("boot_stats: BootKPI init failed %d\n");
			return ret;
		}
#if defined(CONFIG_MSM_BOOT_TIME_MARKER) && !defined(CONFIG_MSM_GVM_BOOT_TIME_MARKER)
		ret = print_bootloader_stats();
		if (ret < 0)
			return -ENODEV;
#endif
	} else {
		iounmap(mpm_counter_base);
		if (mpm_hr_counter_base)
			iounmap(mpm_hr_counter_base);
	}

	return 0;
}
module_init(boot_stats_init);

static void __exit boot_stats_exit(void)
{
	if (boot_marker_enabled()) {
		exit_bootkpi();
#if defined(CONFIG_MSM_BOOT_TIME_MARKER) && !defined(CONFIG_MSM_GVM_BOOT_TIME_MARKER)
		iounmap(boot_stats);
#endif
		iounmap(mpm_counter_base);
		if (mpm_hr_counter_base)
			iounmap(mpm_hr_counter_base);
	}
}
module_exit(boot_stats_exit)

MODULE_DESCRIPTION("MSM boot stats info driver");
MODULE_LICENSE("GPL v2");
