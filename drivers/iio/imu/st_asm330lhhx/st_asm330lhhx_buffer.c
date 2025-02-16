// SPDX-License-Identifier: GPL-2.0-only
/*
 * STMicroelectronics st_asm330lhhx FIFO buffer library driver
 *
 * Copyright 2021 STMicroelectronics Inc.
 *
 * Tesi Mario <mario.tesi@st.com>
 */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/events.h>
#include <linux/iio/buffer.h>
#include <asm/unaligned.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger.h>
#include <linux/of.h>
#include <asm/arch_timer.h>

#include "st_asm330lhhx.h"

#define ST_ASM330LHHX_SAMPLE_DISCHARD		0x7ffd

/* Timestamp convergence filter parameters */
#define ST_ASM330LHHX_EWMA_LEVEL		120
#define ST_ASM330LHHX_EWMA_DIV			128

#define ST_ASM330LHHX_WAKEUP_MS			10000

/* FIFO tags */
enum {
	ST_ASM330LHHX_GYRO_TAG = 0x01,
	ST_ASM330LHHX_ACC_TAG = 0x02,
	ST_ASM330LHHX_TEMP_TAG = 0x03,
	ST_ASM330LHHX_TS_TAG = 0x04,
	ST_ASM330LHHX_EXT0_TAG = 0x0f,
	ST_ASM330LHHX_EXT1_TAG = 0x10,
};

/* Default timeout before to re-enable gyro */
static int asm330lhhx_delay_gyro = 10;
module_param(asm330lhhx_delay_gyro, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(asm330lhhx_delay_gyro, "Delay for Gyro arming");
static bool delayed_enable_gyro = true;

static inline s64 st_asm330lhhx_ewma(s64 old, s64 new, int weight)
{
	s64 diff, incr;

	diff = new - old;
	incr = div_s64((ST_ASM330LHHX_EWMA_DIV - weight) * diff,
		       ST_ASM330LHHX_EWMA_DIV);

	return old + incr;
}

static inline int st_asm330lhhx_reset_hwts(struct st_asm330lhhx_hw *hw)
{
	u8 data = 0xaa;

	hw->ts = st_asm330lhhx_get_time_ns();
	hw->ts_offset = hw->ts;
	hw->val_ts_old = 0;
	hw->hw_ts_high = 0;
	hw->tsample = 0ull;

	if (hw->asm330_hrtimer)
		st_asm330lhhx_set_cpu_idle_state(true);

	return st_asm330lhhx_write_locked(hw,
				      ST_ASM330LHHX_REG_TIMESTAMP2_ADDR,
				      data);
}

int st_asm330lhhx_set_fifo_mode(struct st_asm330lhhx_hw *hw,
			        enum st_asm330lhhx_fifo_mode fifo_mode)
{
	int err;

	err = st_asm330lhhx_write_with_mask_locked(hw,
				ST_ASM330LHHX_REG_FIFO_CTRL4_ADDR,
				ST_ASM330LHHX_REG_FIFO_MODE_MASK,
				fifo_mode);
	if (err < 0)
		return err;

	hw->fifo_mode = fifo_mode;

	if (fifo_mode == ST_ASM330LHHX_FIFO_BYPASS)
		clear_bit(ST_ASM330LHHX_HW_OPERATIONAL, &hw->state);
	else
		set_bit(ST_ASM330LHHX_HW_OPERATIONAL, &hw->state);

	return 0;
}

static inline int
st_asm330lhhx_set_sensor_batching_odr(struct st_asm330lhhx_sensor *sensor,
				      bool enable)
{
	enum st_asm330lhhx_sensor_id id = sensor->id;
	struct st_asm330lhhx_hw *hw = sensor->hw;
	u8 data = 0;
	int err;

	if (enable) {
		err = st_asm330lhhx_get_batch_val(sensor, sensor->odr,
						  sensor->uodr, &data);
		if (err < 0)
			return err;
	}

	return st_asm330lhhx_update_bits_locked(hw,
				hw->odr_table_entry[id].batching_reg.addr,
				hw->odr_table_entry[id].batching_reg.mask,
				data);
}

int st_asm330lhhx_update_watermark(struct st_asm330lhhx_sensor *sensor,
				   u16 watermark)
{
	u16 fifo_watermark = ST_ASM330LHHX_MAX_FIFO_DEPTH;
	struct st_asm330lhhx_hw *hw = sensor->hw;
	struct st_asm330lhhx_sensor *cur_sensor;
	u16 cur_watermark = 0;
	__le16 wdata;
	int i, err;
	int data = 0;

	for (i = ST_ASM330LHHX_ID_GYRO; i <= ST_ASM330LHHX_ID_EXT1; i++) {
		if (!hw->iio_devs[i])
			continue;

		cur_sensor = iio_priv(hw->iio_devs[i]);

		if (!(hw->enable_mask & BIT(cur_sensor->id)))
			continue;

		cur_watermark = (cur_sensor == sensor) ? watermark :
				cur_sensor->watermark;

		fifo_watermark = min_t(u16, fifo_watermark,
				       cur_watermark);
	}

	if (hw->resuming)
		fifo_watermark = watermark;
	else
		fifo_watermark = max_t(u16, fifo_watermark,
				       hw->hw_timestamp_enabled ? 2 : 1);

	mutex_lock(&hw->page_lock);
	err = regmap_read(hw->regmap,
			  ST_ASM330LHHX_REG_FIFO_CTRL1_ADDR + 1,
			  &data);
	if (err < 0)
		goto out;

	fifo_watermark = ((data << 8) & ~ST_ASM330LHHX_REG_FIFO_WTM_MASK) |
			 (fifo_watermark & ST_ASM330LHHX_REG_FIFO_WTM_MASK);
	wdata = cpu_to_le16(fifo_watermark);

	err = regmap_bulk_write(hw->regmap,
				ST_ASM330LHHX_REG_FIFO_CTRL1_ADDR,
				&wdata, sizeof(wdata));

	/* save fifo watermark for suspend/resume */
	hw->fifo_watermark = fifo_watermark;
out:
	mutex_unlock(&hw->page_lock);

	return err;
}

static struct
iio_dev *st_asm330lhhx_get_iiodev_from_tag(struct st_asm330lhhx_hw *hw,
					   u8 tag)
{
	struct iio_dev *iio_dev;

	switch (tag) {
	case ST_ASM330LHHX_GYRO_TAG:
		iio_dev = hw->iio_devs[ST_ASM330LHHX_ID_GYRO];
		break;
	case ST_ASM330LHHX_ACC_TAG:
		iio_dev = hw->iio_devs[ST_ASM330LHHX_ID_ACC];
		break;
	case ST_ASM330LHHX_TEMP_TAG:
		iio_dev = hw->iio_devs[ST_ASM330LHHX_ID_TEMP];
		break;
	case ST_ASM330LHHX_EXT0_TAG:
		if (hw->enable_mask & BIT(ST_ASM330LHHX_ID_EXT0))
			iio_dev = hw->iio_devs[ST_ASM330LHHX_ID_EXT0];
		else
			iio_dev = hw->iio_devs[ST_ASM330LHHX_ID_EXT1];
		break;
	case ST_ASM330LHHX_EXT1_TAG:
		iio_dev = hw->iio_devs[ST_ASM330LHHX_ID_EXT1];
		break;
	default:
		iio_dev = NULL;
		break;
	}

	return iio_dev;
}
#ifdef CONFIG_ENABLE_ASMX_ACC_GYRO_BUFFERING
int asm330lhhx_check_acc_gyro_early_buff_enable_flag(
		struct st_asm330lhhx_sensor *sensor)
{
		return 0;
}
int asm330lhhx_check_sensor_enable_flag(
		struct st_asm330lhhx_sensor *sensor, bool enable)
{
	sensor->buffer_asm_samples = false;
	sensor->enable = enable;
	return 0;
}
#else
int asm330lhhx_check_acc_gyro_early_buff_enable_flag(
		struct st_asm330lhhx_sensor *sensor)
{
	return 0;
}
int asm330lhhx_check_sensor_enable_flag(
		struct st_asm330lhhx_sensor *sensor, bool enable)
{
	return 0;
}
#endif

#ifdef CONFIG_ENABLE_ASMX_ACC_GYRO_BUFFERING
static void store_acc_gyro_boot_sample(struct iio_dev *iio_dev,
					u8 *iio_buf, s64 tsample)
{
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);
	struct st_asm330lhhx_hw *hw = sensor->hw;
	int x, y, z;

	if (!sensor->buffer_asm_samples)
		return;

	mutex_lock(&sensor->sensor_buff);
	sensor->timestamp = (ktime_t)tsample;
	x = iio_buf[1]<<8|iio_buf[0];
	y = iio_buf[3]<<8|iio_buf[2];
	z = iio_buf[5]<<8|iio_buf[4];

	if (ktime_to_timespec64(sensor->timestamp).tv_sec
			<  sensor->max_buffer_time) {
		if (sensor->bufsample_cnt < ASM_MAXSAMPLE) {
			sensor->asm_samplist[sensor->bufsample_cnt]->xyz[0] = x;
			sensor->asm_samplist[sensor->bufsample_cnt]->xyz[1] = y;
			sensor->asm_samplist[sensor->bufsample_cnt]->xyz[2] = z;
			sensor->asm_samplist[sensor->bufsample_cnt]->tsec =
				ktime_to_timespec64(sensor->timestamp).tv_sec;
			sensor->asm_samplist[sensor->bufsample_cnt]->tnsec =
				ktime_to_timespec64(sensor->timestamp).tv_nsec;
			sensor->bufsample_cnt++;
		}
	} else {
		dev_info(sensor->hw->dev, "End of sensor %d buffering %d\n",
				sensor->id, sensor->bufsample_cnt);
		sensor->buffer_asm_samples = false;
		if (!sensor->enable)
			st_asm330lhhx_sensor_set_enable(sensor, false);
		if (!hw->enable_mask) {
			st_asm330lhhx_set_fifo_mode(hw,
					ST_ASM330LHHX_FIFO_BYPASS);
		}
	}
	mutex_unlock(&sensor->sensor_buff);
}
#else
static void store_acc_gyro_boot_sample(struct iio_dev *iio_dev,
					u8 *iio_buf, s64 tsample)
{
}
#endif

static inline void st_asm330lhhx_sync_hw_ts(struct st_asm330lhhx_hw *hw,
					    s64 ts)
{
	s64 delta = ts - hw->hw_ts;

	hw->ts_offset = st_asm330lhhx_ewma(hw->ts_offset, delta,
					   ST_ASM330LHHX_EWMA_LEVEL);
}

/*
 * st_asm330lhhx_read_fifo - read sample data from HW FIFO
 *
 * @notify - Ask to notify events on iio_dev_mlc_fifo_acc device
 */
int st_asm330lhhx_read_fifo(struct st_asm330lhhx_hw *hw, int notify)
{
	u8 iio_buf[ALIGN(ST_ASM330LHHX_SAMPLE_SIZE, sizeof(s64)) + sizeof(s64)];
	u8 buf[60 * ST_ASM330LHHX_FIFO_SAMPLE_SIZE], tag, *ptr;
	struct iio_dev *iio_dev, *iio_dev_mlc_fifo_acc;
	int i, err, word_len, fifo_len, read_len;
	__le16 fifo_status;
	u16 fifo_depth;
	s64 hw_ts_old;
	s16 drdymask;
	s64 ts_irq;
	s64 delta;
	u32 val;
	u8 iio_last_buf[16] = { 0 };
	s64 last_ts = 0xFFFFFFFF;

	/* return if FIFO is already disabled */
	if (!test_bit(ST_ASM330LHHX_HW_OPERATIONAL, &hw->state)) {
		dev_warn(hw->dev, "%s: FIFO in bypass mode\n", __func__);

		return 0;
	}

	err = st_asm330lhhx_read_locked(hw, ST_ASM330LHHX_REG_FIFO_STATUS1_ADDR,
				        &fifo_status, sizeof(fifo_status));
	if (err < 0)
		return err;

	fifo_depth = le16_to_cpu(fifo_status) & ST_ASM330LHHX_REG_FIFO_STATUS_DIFF;
	if (!fifo_depth)
		return 0;

	fifo_len = fifo_depth * ST_ASM330LHHX_FIFO_SAMPLE_SIZE;
	read_len = 0;
	ts_irq = hw->ts - hw->delta_ts;
	delta = div_s64(hw->delta_ts, fifo_depth);

	if (hw->resuming && notify) {
		/* take approx wake-up timestamp event */
		hw->ts_offset_resume = st_asm330lhhx_get_time_ns();
		hw->ts_offset_resume -= ((fifo_depth >> hw->resume_sample_in_packet) *
				         hw->resume_sample_tick_ns);
	}

	while (read_len < fifo_len) {
		word_len = min_t(int, fifo_len - read_len, sizeof(buf));
		err = st_asm330lhhx_read_locked(hw,
			       ST_ASM330LHHX_REG_FIFO_DATA_OUT_TAG_ADDR,
			       buf, word_len);
		if (err < 0)
			return err;

		for (i = 0; i < word_len; i += ST_ASM330LHHX_FIFO_SAMPLE_SIZE) {
			ptr = &buf[i + ST_ASM330LHHX_TAG_SIZE];
			tag = buf[i] >> 3;
			ts_irq += delta;
			if (tag == ST_ASM330LHHX_TS_TAG) {
				val = get_unaligned_le32(ptr);

				if (hw->val_ts_old > val)
					hw->hw_ts_high++;

				hw_ts_old = hw->hw_ts;

				/* check hw rollover */
				hw->val_ts_old = val;
				hw->hw_ts = (val +
					     ((s64)hw->hw_ts_high << 32)) *
					     hw->ts_delta_ns;

				if (!test_bit(ST_ASM330LHHX_HW_FLUSH, &hw->state))
					/* sync ap timestamp and sensor one */
					st_asm330lhhx_sync_hw_ts(hw, ts_irq);
			} else {
				struct st_asm330lhhx_sensor *sensor;
				s64 ts;

				iio_dev = st_asm330lhhx_get_iiodev_from_tag(hw, tag);
				if (!iio_dev)
					continue;

				/* skip samples if not ready */
				drdymask = (s16)le16_to_cpu(get_unaligned_le16(ptr));
				if (unlikely(drdymask >= ST_ASM330LHHX_SAMPLE_DISCHARD)) {
					continue;
				}

				memcpy(iio_buf, ptr, ST_ASM330LHHX_SAMPLE_SIZE);

#ifdef CONFIG_IIO_ST_ASM330LHHX_MAY_WAKEUP
				if (hw->resuming && notify) {
					iio_dev_mlc_fifo_acc = hw->iio_devs[ST_ASM330LHHX_ID_FIFO_MLC];
					iio_push_to_buffers_with_timestamp(iio_dev_mlc_fifo_acc,
								iio_buf,
								hw->ts_offset_resume);
					hw->ts_offset_resume += hw->resume_sample_tick_ns;
				} else {
#endif /* CONFIG_IIO_ST_ASM330LHHX_MAY_WAKEUP */
					sensor = iio_priv(iio_dev);
					ts = hw->hw_ts + hw->ts_offset;

					/* decimation for ODR < 12.5 Hz on SHUB */
					if (sensor->dec_counter > 0) {
						sensor->dec_counter--;
					} else {
						sensor->dec_counter = sensor->decimator;
						iio_push_to_buffers_with_timestamp(iio_dev,
									iio_buf,
									ts);
						store_acc_gyro_boot_sample(iio_dev,
							iio_buf, ts);
						sensor->last_fifo_timestamp = ts;
					}
#ifdef CONFIG_IIO_ST_ASM330LHHX_MAY_WAKEUP
				}
#endif /* CONFIG_IIO_ST_ASM330LHHX_MAY_WAKEUP */
			}
		}
		read_len += word_len;
	}

	if (hw->resuming && notify) {
		iio_dev_mlc_fifo_acc = hw->iio_devs[
			ST_ASM330LHHX_ID_FIFO_MLC];
		//u8 iio_last_buf[16];
		//memset(iio_last_buf, 0, sizeof(iio_last_buf));
		//s64 last_ts = 0xFFFFFFFF;

		iio_push_to_buffers_with_timestamp(
				iio_dev_mlc_fifo_acc,
				iio_last_buf, last_ts);
		pm_wakeup_ws_event(hw->ws, ST_ASM330LHHX_WAKEUP_MS, true);
	}
	return read_len;
}

ssize_t st_asm330lhhx_get_max_watermark(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct iio_dev *iio_dev = dev_get_drvdata(dev);
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);

	return sprintf(buf, "%d\n", sensor->max_watermark);
}

ssize_t st_asm330lhhx_get_watermark(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct iio_dev *iio_dev = dev_get_drvdata(dev);
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);

	return sprintf(buf, "%d\n", sensor->watermark);
}

ssize_t st_asm330lhhx_set_watermark(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct iio_dev *iio_dev = dev_get_drvdata(dev);
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);
	int err, val;

	if (asm330lhhx_check_acc_gyro_early_buff_enable_flag(sensor))
		return -EBUSY;

	err = iio_device_claim_direct_mode(iio_dev);
	if (err)
		return err;

	err = kstrtoint(buf, 10, &val);
	if (err < 0)
		goto out;

	err = st_asm330lhhx_update_watermark(sensor, val);
	if (err < 0)
		goto out;

	sensor->watermark = val;

out:
	iio_device_release_direct_mode(iio_dev);

	return err < 0 ? err : size;
}

ssize_t st_asm330lhhx_flush_fifo(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct iio_dev *iio_dev = dev_get_drvdata(dev);
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);
	struct st_asm330lhhx_hw *hw = sensor->hw;
	s64 event;
	int count;
	s64 type;
	s64 fts;
	s64 ts;

	mutex_lock(&hw->fifo_lock);
	ts = st_asm330lhhx_get_time_ns();
	hw->delta_ts = ts - hw->ts;
	hw->ts = ts;
	set_bit(ST_ASM330LHHX_HW_FLUSH, &hw->state);
	count = st_asm330lhhx_read_fifo(hw, 0);
	sensor->dec_counter = 0;
	if (count > 0)
		fts = sensor->last_fifo_timestamp;
	else
		fts = ts;
	mutex_unlock(&hw->fifo_lock);

	type = count > 0 ? CUSTOM_IIO_EV_DIR_FIFO_DATA : CUSTOM_IIO_EV_DIR_FIFO_EMPTY;
	event = IIO_UNMOD_EVENT_CODE(iio_dev->channels[0].type, -1,
				     CUSTOM_IIO_EV_TYPE_FIFO_FLUSH, type);
	iio_push_event(iio_dev, event, fts);

	return size;
}

int st_asm330lhhx_suspend_fifo(struct st_asm330lhhx_hw *hw)
{
	st_asm330lhhx_read_fifo(hw, 0);

	return st_asm330lhhx_set_fifo_mode(hw,
					   ST_ASM330LHHX_FIFO_BYPASS);
}

int st_asm330lhhx_update_batching(struct iio_dev *iio_dev, bool enable)
{
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);
	struct st_asm330lhhx_hw *hw = sensor->hw;
	int err;

	disable_irq(hw->irq);
	err = st_asm330lhhx_set_sensor_batching_odr(sensor, enable);
	enable_irq(hw->irq);

	return err;
}

int st_asm330lhhx_update_fifo(struct iio_dev *iio_dev,
				     bool enable)
{
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);
	struct st_asm330lhhx_hw *hw = sensor->hw;
	int err;

	if (sensor->id == ST_ASM330LHHX_ID_GYRO && !enable)
		delayed_enable_gyro = true;

	if (sensor->id == ST_ASM330LHHX_ID_GYRO &&
	    enable && delayed_enable_gyro) {
		delayed_enable_gyro = false;
		msleep(asm330lhhx_delay_gyro);
	}

	disable_irq(hw->irq);

	if (sensor->id == ST_ASM330LHHX_ID_EXT0 ||
	    sensor->id == ST_ASM330LHHX_ID_EXT1) {
		err = st_asm330lhhx_shub_set_enable(sensor, enable);
		if (err < 0)
			goto out;
	} else {
		err = st_asm330lhhx_sensor_set_enable(sensor, enable);
		if (err < 0)
			goto out;

		/* power up, wait 100 ms for stable output */
		msleep(100);

		err = st_asm330lhhx_set_sensor_batching_odr(sensor,
							    enable);
		if (err < 0)
			goto out;
	}

	/*
	 * This is an auxiliary sensor, it need to get batched
	 * toghether at least with a primary sensor (Acc/Gyro).
	 */
	if (sensor->id == ST_ASM330LHHX_ID_TEMP) {
		if (!(hw->enable_mask & (BIT(ST_ASM330LHHX_ID_ACC) |
					 BIT(ST_ASM330LHHX_ID_GYRO)))) {
			struct st_asm330lhhx_sensor *acc_sensor;
			u8 data = 0;

			acc_sensor = iio_priv(hw->iio_devs[ST_ASM330LHHX_ID_ACC]);
			if (enable) {
				err = st_asm330lhhx_get_batch_val(acc_sensor,
						sensor->odr, sensor->uodr,
						&data);
				if (err < 0)
					goto out;
			}

			err = st_asm330lhhx_update_bits_locked(hw,
				hw->odr_table_entry[ST_ASM330LHHX_ID_ACC].batching_reg.addr,
				hw->odr_table_entry[ST_ASM330LHHX_ID_ACC].batching_reg.mask,
				data);
			if (err < 0)
				goto out;
		}
	}

	err = st_asm330lhhx_update_watermark(sensor, sensor->watermark);
	if (err < 0)
		goto out;

	if (enable && hw->fifo_mode == ST_ASM330LHHX_FIFO_BYPASS) {
		st_asm330lhhx_reset_hwts(hw);
		err = st_asm330lhhx_set_fifo_mode(hw, ST_ASM330LHHX_FIFO_CONT);
	} else if (!hw->enable_mask) {
		err = st_asm330lhhx_set_fifo_mode(hw, ST_ASM330LHHX_FIFO_BYPASS);
	}

out:
	enable_irq(hw->irq);

	return err;
}

static irqreturn_t st_asm330lhhx_handler_irq(int irq, void *private)
{
	struct st_asm330lhhx_hw *hw = (struct st_asm330lhhx_hw *)private;
	s64 ts = st_asm330lhhx_get_time_ns();

	hw->delta_ts = ts - hw->ts;
	hw->ts = ts;

	if (hw->asm330_hrtimer)
		st_asm330lhhx_hrtimer_reset(hw, hw->delta_ts);

	return IRQ_WAKE_THREAD;
}

static irqreturn_t st_asm330lhhx_handler_thread(int irq, void *private)
{
	struct st_asm330lhhx_hw *hw = (struct st_asm330lhhx_hw *)private;
	int notify = 0;

	mutex_lock(&hw->handler_lock);
	if (hw->asm330_hrtimer)
		st_asm330lhhx_set_cpu_idle_state(false);

#ifdef CONFIG_IIO_ST_ASM330LHHX_MLC
	notify = st_asm330lhhx_mlc_check_status(hw);
#endif /* CONFIG_IIO_ST_ASM330LHHX_MLC */

	mutex_lock(&hw->fifo_lock);
	st_asm330lhhx_read_fifo(hw, notify);
	clear_bit(ST_ASM330LHHX_HW_FLUSH, &hw->state);
	mutex_unlock(&hw->fifo_lock);
	mutex_unlock(&hw->handler_lock);

	return IRQ_HANDLED;
}

static int st_asm330lhhx_fifo_preenable(struct iio_dev *iio_dev)
{
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);

	asm330lhhx_check_sensor_enable_flag(sensor, true);

	if (asm330lhhx_check_acc_gyro_early_buff_enable_flag(sensor))
		return 0;
	else
		return st_asm330lhhx_update_fifo(iio_dev, true);
}

static int st_asm330lhhx_fifo_postdisable(struct iio_dev *iio_dev)
{
	struct st_asm330lhhx_sensor *sensor = iio_priv(iio_dev);

	asm330lhhx_check_sensor_enable_flag(sensor, false);

	if (asm330lhhx_check_acc_gyro_early_buff_enable_flag(sensor))
		return 0;
	else
		return st_asm330lhhx_update_fifo(iio_dev, false);
}

static const struct iio_buffer_setup_ops st_asm330lhhx_fifo_ops = {
	.preenable = st_asm330lhhx_fifo_preenable,
	.postdisable = st_asm330lhhx_fifo_postdisable,
};

int st_asm330lhhx_buffers_setup(struct st_asm330lhhx_hw *hw)
{
	struct device_node *np = hw->dev->of_node;
	struct iio_buffer *buffer;
	unsigned long irq_type;
	bool irq_active_low;
	int i, err;

	irq_type = irqd_get_trigger_type(irq_get_irq_data(hw->irq));
	if (irq_type == IRQF_TRIGGER_NONE || irq_type == IRQF_TRIGGER_RISING)
		irq_type = IRQF_TRIGGER_HIGH;

	switch (irq_type) {
	case IRQF_TRIGGER_HIGH:
	case IRQF_TRIGGER_RISING:
		irq_active_low = false;
		break;
	case IRQF_TRIGGER_LOW:
	case IRQF_TRIGGER_FALLING:
		irq_active_low = true;
		break;
	default:
		dev_info(hw->dev, "mode %lx unsupported\n", irq_type);
		return -EINVAL;
	}

	err = st_asm330lhhx_write_with_mask_locked(hw,
				 ST_ASM330LHHX_REG_CTRL3_C_ADDR,
				 ST_ASM330LHHX_REG_H_LACTIVE_MASK,
				 irq_active_low);
	if (err < 0)
		return err;

	if (np && of_property_read_bool(np, "drive-open-drain")) {
		err = st_asm330lhhx_write_with_mask_locked(hw,
					 ST_ASM330LHHX_REG_CTRL3_C_ADDR,
					 ST_ASM330LHHX_REG_PP_OD_MASK,
					 1);
		if (err < 0)
			return err;

		irq_type |= IRQF_SHARED;
	}

	err = devm_request_threaded_irq(hw->dev, hw->irq,
					st_asm330lhhx_handler_irq,
					st_asm330lhhx_handler_thread,
					irq_type | IRQF_ONESHOT,
					"asm330lhhx", hw);
	if (err) {
		dev_err(hw->dev, "failed to request trigger irq %d\n",
			hw->irq);
		return err;
	}

	for (i = ST_ASM330LHHX_ID_GYRO;
	     i <= ST_ASM330LHHX_ID_EXT1;
	     i++) {
		if (!hw->iio_devs[i])
			continue;

		buffer = iio_kfifo_allocate();
		if (!buffer)
			return -ENOMEM;

		iio_device_attach_buffer(hw->iio_devs[i], buffer);
		hw->iio_devs[i]->modes |= INDIO_BUFFER_SOFTWARE;
		hw->iio_devs[i]->setup_ops = &st_asm330lhhx_fifo_ops;
	}

	return st_asm330lhhx_write_with_mask_locked(hw,
				  ST_ASM330LHHX_REG_FIFO_CTRL4_ADDR,
				  ST_ASM330LHHX_REG_DEC_TS_MASK, 1);
}
