/*
 * Copyright (c) 2017, I-SENSE group of ICCS
 * Copyright (c) 2017 Linaro Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C Driver for: STM32F1, STM32F2, STM32F4 and STM32L1
 *
 */

#include <clock_control/stm32_clock_control.h>
#include <drivers/clock_control.h>
#include <sys/util.h>
#include <kernel.h>
#include <soc.h>
#include <errno.h>
#include <drivers/i2c.h>
#include "i2c_ll_stm32.h"

#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(i2c_ll_stm32_v1);

#include "i2c-priv.h"

#define STM32_I2C_TIMEOUT_USEC  1000
#define I2C_REQUEST_WRITE       0x00
#define I2C_REQUEST_READ        0x01
#define HEADER                  0xF0

#ifdef CONFIG_I2C_STM32_INTERRUPT

static void stm32_i2c_disable_transfer_interrupts(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	LL_I2C_DisableIT_TX(i2c);
	LL_I2C_DisableIT_RX(i2c);
	LL_I2C_DisableIT_EVT(i2c);
	LL_I2C_DisableIT_BUF(i2c);
	LL_I2C_DisableIT_ERR(i2c);
}

static void stm32_i2c_enable_transfer_interrupts(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	LL_I2C_EnableIT_ERR(i2c);
	LL_I2C_EnableIT_EVT(i2c);
	LL_I2C_EnableIT_BUF(i2c);
}

#endif

static void stm32_i2c_master_finish(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	I2C_TypeDef *i2c = cfg->i2c;

#ifdef CONFIG_I2C_STM32_INTERRUPT
	stm32_i2c_disable_transfer_interrupts(dev);
#endif

#if defined(CONFIG_I2C_SLAVE)
	struct i2c_stm32_data *data = DEV_DATA(dev);
	data->master_active = false;
	if (!data->slave_attached) {
		LL_I2C_Disable(i2c);
	} else {
		stm32_i2c_enable_transfer_interrupts(dev);
		LL_I2C_AcknowledgeNextData(i2c, LL_I2C_ACK);
	}
#else
	LL_I2C_Disable(i2c);
#endif
}

static inline void msg_init(struct device *dev, struct i2c_msg *msg,
			    u8_t *next_msg_flags, u16_t slave,
			    u32_t transfer)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	ARG_UNUSED(next_msg_flags);

#ifdef CONFIG_I2C_STM32_INTERRUPT
	k_sem_reset(&data->device_sync_sem);
#endif

	data->current.len = msg->len;
	data->current.buf = msg->buf;
	data->current.flags = msg->flags;
	data->current.is_restart = 0U;
	data->current.is_write = (transfer == I2C_REQUEST_WRITE);
	data->current.is_arlo = 0U;
	data->current.is_err = 0U;
	data->current.is_nack = 0U;
	data->current.msg = msg;
#if defined(CONFIG_I2C_SLAVE)
	data->master_active = true;
#endif
	data->slave_address = slave;

	LL_I2C_Enable(i2c);

	LL_I2C_DisableBitPOS(i2c);
	LL_I2C_AcknowledgeNextData(i2c, LL_I2C_ACK);
	if (msg->flags & I2C_MSG_RESTART) {
		LL_I2C_GenerateStartCondition(i2c);
	}
}

static s32_t msg_end(struct device *dev, u8_t *next_msg_flags, const char *funcname)
{
	struct i2c_stm32_data *data = DEV_DATA(dev);

	if (data->current.is_nack || data->current.is_err ||
	    data->current.is_arlo) {
		goto error;
	}

	if (!next_msg_flags) {
		stm32_i2c_master_finish(dev);
	}

	return 0;

error:
	if (data->current.is_arlo) {
		LOG_DBG("%s: ARLO %d", funcname,
			data->current.is_arlo);
		data->current.is_arlo = 0U;
	}

	if (data->current.is_nack) {
		LOG_DBG("%s: NACK", funcname);
		data->current.is_nack = 0U;
	}

	if (data->current.is_err) {
		LOG_DBG("%s: ERR %d", funcname,
			data->current.is_err);
		data->current.is_err = 0U;
	}
	stm32_i2c_master_finish(dev);

	return -EIO;
}

#ifdef CONFIG_I2C_STM32_INTERRUPT

static void stm32_i2c_master_mode_end(struct device *dev)
{
	struct i2c_stm32_data *data = DEV_DATA(dev);

	k_sem_give(&data->device_sync_sem);
}

static inline void handle_sb(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	u16_t saddr = data->slave_address;
	u8_t slave;

	if (I2C_ADDR_10_BITS & data->dev_config) {
		slave = (((saddr & 0x0300) >> 7) & 0xFF);
		u8_t header = slave | HEADER;

		if (data->current.is_restart == 0U) {
			data->current.is_restart = 1U;
		} else {
			header |= I2C_REQUEST_READ;
			data->current.is_restart = 0U;
		}
		LL_I2C_TransmitData8(i2c, header);

		return;
	}
	slave = (saddr << 1) & 0xFF;
	if (data->current.is_write) {
		LL_I2C_TransmitData8(i2c, slave | I2C_REQUEST_WRITE);
	} else {
		LL_I2C_TransmitData8(i2c, slave | I2C_REQUEST_READ);
		if (data->current.len == 2) {
			LL_I2C_EnableBitPOS(i2c);
		}
	}
}

static inline void handle_addr(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	if (I2C_ADDR_10_BITS & data->dev_config) {
		if (!data->current.is_write && data->current.is_restart) {
			data->current.is_restart = 0U;
			LL_I2C_ClearFlag_ADDR(i2c);
			LL_I2C_GenerateStartCondition(i2c);

			return;
		}
	}

	if (data->current.is_write || data->current.len > 2) {
		LL_I2C_ClearFlag_ADDR(i2c);
		return;
	}
	if (data->current.len == 0U) {
		LL_I2C_GenerateStopCondition(i2c);
		LL_I2C_ClearFlag_ADDR(i2c);
	} else if (data->current.len == 1U) {
		/* Single byte reception: enable NACK and clear POS */
		LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
		LL_I2C_ClearFlag_ADDR(i2c);
		LL_I2C_GenerateStopCondition(i2c);
	} else if (data->current.len == 2U) {
		/* 2-byte reception: enable NACK and set POS */
		LL_I2C_ClearFlag_ADDR(i2c);
		LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
		LL_I2C_EnableBitPOS(i2c);
	}
}

static inline void handle_txe(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	if (data->current.len) {
		data->current.len--;
		if (data->current.len == 0U) {
			/*
			 * This is the last byte to transmit disable Buffer
			 * interrupt and wait for a BTF interrupt
			 */
			LL_I2C_DisableIT_BUF(i2c);
		}
		LL_I2C_TransmitData8(i2c, *data->current.buf);
		data->current.buf++;
	} else {
		if (data->current.flags & I2C_MSG_STOP) {
			LL_I2C_GenerateStopCondition(i2c);
		}
		if (LL_I2C_IsActiveFlag_BTF(i2c)) {
			/* Read DR to clear BTF flag */
			LL_I2C_ReceiveData8(i2c);
		}

		k_sem_give(&data->device_sync_sem);
	}
}

static inline void handle_rxne(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	if (data->current.len > 0) {
		switch (data->current.len) {
		case 1:
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
			LL_I2C_DisableBitPOS(i2c);
			/* Single byte reception */
			if (data->current.flags & I2C_MSG_STOP) {
				LL_I2C_GenerateStopCondition(i2c);
			}
			LL_I2C_DisableIT_BUF(i2c);
			data->current.len--;
			*data->current.buf = LL_I2C_ReceiveData8(i2c);
			data->current.buf++;

			k_sem_give(&data->device_sync_sem);
			break;
		case 2:
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
			LL_I2C_EnableBitPOS(i2c);
		case 3:
			/*
			 * 2-byte, 3-byte reception and for N-2, N-1,
			 * N when N > 3
			 */
			LL_I2C_DisableIT_BUF(i2c);
			break;
		default:
			/* N byte reception when N > 3 */
			data->current.len--;
			*data->current.buf = LL_I2C_ReceiveData8(i2c);
			data->current.buf++;
		}
	} else {

		if (data->current.flags & I2C_MSG_STOP) {
			LL_I2C_GenerateStopCondition(i2c);
		}
		k_sem_give(&data->device_sync_sem);
	}
}

static inline void handle_btf(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	if (data->current.is_write) {
		handle_txe(dev);
	} else {
		u32_t counter = 0U;

		switch (data->current.len) {
		case 2:
			/*
			 * Stop condition must be generated before reading the
			 * last two bytes.
			 */
			if (data->current.flags & I2C_MSG_STOP) {
				LL_I2C_GenerateStopCondition(i2c);
			}

			for (counter = 2U; counter > 0; counter--) {
				data->current.len--;
				*data->current.buf = LL_I2C_ReceiveData8(i2c);
				data->current.buf++;
			}
			k_sem_give(&data->device_sync_sem);
			break;
		case 3:
			/* Set NACK before reading N-2 byte*/
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
			data->current.len--;
			*data->current.buf = LL_I2C_ReceiveData8(i2c);
			data->current.buf++;
			break;
		default:
			handle_rxne(dev);
		}
	}
}


#if defined(CONFIG_I2C_SLAVE)
static void stm32_i2c_slave_event(struct device *dev)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;
	const struct i2c_slave_callbacks *slave_cb =
		data->slave_cfg->callbacks;

	if (LL_I2C_IsActiveFlag_TXE(i2c) && LL_I2C_IsActiveFlag_BTF(i2c)) {
		u8_t val;
		slave_cb->read_processed(data->slave_cfg, &val);
		LL_I2C_TransmitData8(i2c, val);
		return;
	}

	if (LL_I2C_IsActiveFlag_RXNE(i2c)) {
		u8_t val = LL_I2C_ReceiveData8(i2c);
		if (slave_cb->write_received(data->slave_cfg, val)) {
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
		}
		return;
	}

	if (LL_I2C_IsActiveFlag_AF(i2c)) {
		LL_I2C_ClearFlag_AF(i2c);
	}

	if (LL_I2C_IsActiveFlag_STOP(i2c)) {
		LL_I2C_ClearFlag_STOP(i2c);
		slave_cb->stop(data->slave_cfg);
		/* Prepare to ACK next transmissions address byte */
		LL_I2C_AcknowledgeNextData(i2c, LL_I2C_ACK);
	}

	if (LL_I2C_IsActiveFlag_ADDR(i2c)) {
		u32_t dir = LL_I2C_GetTransferDirection(i2c);
		if (dir == LL_I2C_DIRECTION_READ) {
			slave_cb->write_requested(data->slave_cfg);
			LL_I2C_EnableIT_RX(i2c);
		} else {
			u8_t val;
			slave_cb->read_requested(data->slave_cfg, &val);
			LL_I2C_TransmitData8(i2c, val);
			LL_I2C_EnableIT_TX(i2c);
		}

		stm32_i2c_enable_transfer_interrupts(dev);
	}
}

/* Attach and start I2C as slave */
int i2c_stm32_slave_register(struct device *dev,
			     struct i2c_slave_config *config)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;
	u32_t bitrate_cfg;
	int ret;

	if (!config) {
		return -EINVAL;
	}

	if (data->slave_attached) {
		return -EBUSY;
	}

	if (data->master_active) {
		return -EBUSY;
	}

	bitrate_cfg = i2c_map_dt_bitrate(cfg->bitrate);

	ret = i2c_stm32_runtime_configure(dev, bitrate_cfg);
	if (ret < 0) {
		LOG_ERR("i2c: failure initializing");
		return ret;
	}

	data->slave_cfg = config;

	LL_I2C_Enable(i2c);

	LL_I2C_SetOwnAddress1(i2c, config->address << 1,
			      LL_I2C_OWNADDRESS1_7BIT);

	data->slave_attached = true;

	LOG_DBG("i2c: slave registered");

	stm32_i2c_enable_transfer_interrupts(dev);
	LL_I2C_AcknowledgeNextData(i2c, LL_I2C_ACK);

	return 0;
}

int i2c_stm32_slave_unregister(struct device *dev,
			       struct i2c_slave_config *config)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	if (!data->slave_attached) {
		return -EINVAL;
	}

	if (data->master_active) {
		return -EBUSY;
	}

	stm32_i2c_disable_transfer_interrupts(dev);

	LL_I2C_ClearFlag_AF(i2c);
	LL_I2C_ClearFlag_STOP(i2c);
	LL_I2C_ClearFlag_ADDR(i2c);

	LL_I2C_Disable(i2c);

	data->slave_attached = false;

	LOG_DBG("i2c: slave unregistered");

	return 0;
}
#endif /* defined(CONFIG_I2C_SLAVE) */


void stm32_i2c_event_isr(void *arg)
{
	struct device *dev = (struct device *)arg;
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

#if defined(CONFIG_I2C_SLAVE)
	if (data->slave_attached && !data->master_active) {
		stm32_i2c_slave_event(dev);
		return;
	}
#endif

	if (LL_I2C_IsActiveFlag_SB(i2c)) {
		handle_sb(dev);
	} else if (LL_I2C_IsActiveFlag_ADD10(i2c)) {
		LL_I2C_TransmitData8(i2c, data->slave_address);
	} else if (LL_I2C_IsActiveFlag_ADDR(i2c)) {
		handle_addr(dev);
	} else if (LL_I2C_IsActiveFlag_BTF(i2c)) {
		handle_btf(dev);
	} else if (LL_I2C_IsActiveFlag_TXE(i2c) && data->current.is_write) {
		handle_txe(dev);
	} else if (LL_I2C_IsActiveFlag_RXNE(i2c) && !data->current.is_write) {
		handle_rxne(dev);
	}
}

void stm32_i2c_error_isr(void *arg)
{
	struct device *dev = (struct device *)arg;
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

#if defined(CONFIG_I2C_SLAVE)
	if (data->slave_attached && !data->master_active) {
		/* No need for a slave error function right now. */
		return;
	}
#endif

	if (LL_I2C_IsActiveFlag_AF(i2c)) {
		LL_I2C_ClearFlag_AF(i2c);
		LL_I2C_GenerateStopCondition(i2c);
		data->current.is_nack = 1U;
		goto end;
	}
	if (LL_I2C_IsActiveFlag_ARLO(i2c)) {
		LL_I2C_ClearFlag_ARLO(i2c);
		data->current.is_arlo = 1U;
		goto end;
	}

	if (LL_I2C_IsActiveFlag_BERR(i2c)) {
		LL_I2C_ClearFlag_BERR(i2c);
		data->current.is_err = 1U;
		goto end;
	}
	return;
end:
	stm32_i2c_master_mode_end(dev);
}

s32_t stm32_i2c_msg_write(struct device *dev, struct i2c_msg *msg,
			  u8_t *next_msg_flags, u16_t saddr)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	msg_init(dev, msg, next_msg_flags, saddr, I2C_REQUEST_WRITE);

	stm32_i2c_enable_transfer_interrupts(dev);
	LL_I2C_EnableIT_TX(i2c);

	k_sem_take(&data->device_sync_sem, K_FOREVER);

	return msg_end(dev, next_msg_flags, __func__);
}

s32_t stm32_i2c_msg_read(struct device *dev, struct i2c_msg *msg,
			 u8_t *next_msg_flags, u16_t saddr)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	msg_init(dev, msg, next_msg_flags, saddr, I2C_REQUEST_READ);

	stm32_i2c_enable_transfer_interrupts(dev);
	LL_I2C_EnableIT_RX(i2c);

	k_sem_take(&data->device_sync_sem, K_FOREVER);

	return msg_end(dev, next_msg_flags, __func__);
}

#else

static inline int check_errors(struct device *dev, const char *funcname)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	if (LL_I2C_IsActiveFlag_AF(i2c)) {
		LL_I2C_ClearFlag_AF(i2c);
		LOG_DBG("%s: NACK", funcname);
		data->current.is_nack = 1U;
		goto error;
	}

	if (LL_I2C_IsActiveFlag_ARLO(i2c)) {
		LL_I2C_ClearFlag_ARLO(i2c);
		LOG_DBG("%s: ARLO", funcname);
		data->current.is_arlo = 1U;
		goto error;
	}

	if (LL_I2C_IsActiveFlag_OVR(i2c)) {
		LL_I2C_ClearFlag_OVR(i2c);
		LOG_DBG("%s: OVR", funcname);
		data->current.is_err = 1U;
		goto error;
	}

	if (LL_I2C_IsActiveFlag_BERR(i2c)) {
		LL_I2C_ClearFlag_BERR(i2c);
		LOG_DBG("%s: BERR", funcname);
		data->current.is_err = 1U;
		goto error;
	}

	return 0;
error:
	return -EIO;
}

static int stm32_i2c_wait_timeout(u16_t *timeout)
{
	if (*timeout == 0) {
		return 1;
	} else {
		k_busy_wait(1);
		(*timeout)--;
		return 0;
	}
}

s32_t stm32_i2c_msg_write(struct device *dev, struct i2c_msg *msg,
			  u8_t *next_msg_flags, u16_t saddr)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;
	u32_t len = msg->len;
	u16_t timeout;
	u8_t *buf = msg->buf;

	msg_init(dev, msg, next_msg_flags, saddr, I2C_REQUEST_WRITE);

	if (msg->flags & I2C_MSG_RESTART) {
		timeout = STM32_I2C_TIMEOUT_USEC;
		while (!LL_I2C_IsActiveFlag_SB(i2c)) {
			if (stm32_i2c_wait_timeout(&timeout)) {
				LL_I2C_GenerateStopCondition(i2c);
				data->current.is_err = 1U;
				goto end;
			}
		}

		if (I2C_ADDR_10_BITS & data->dev_config) {
			u8_t slave = (((saddr & 0x0300) >> 7) & 0xFF);
			u8_t header = slave | HEADER;

			LL_I2C_TransmitData8(i2c, header);
			timeout = STM32_I2C_TIMEOUT_USEC;
			while (!LL_I2C_IsActiveFlag_ADD10(i2c)) {
				if (stm32_i2c_wait_timeout(&timeout)) {
					LL_I2C_GenerateStopCondition(i2c);
					data->current.is_err = 1U;
					goto end;
				}
			}

			slave = data->slave_address & 0xFF;
			LL_I2C_TransmitData8(i2c, slave);
		} else {
			u8_t slave = (saddr << 1) & 0xFF;

			LL_I2C_TransmitData8(i2c, slave | I2C_REQUEST_WRITE);
		}

		timeout = STM32_I2C_TIMEOUT_USEC;
		while (!LL_I2C_IsActiveFlag_ADDR(i2c)) {
			if (LL_I2C_IsActiveFlag_AF(i2c) || stm32_i2c_wait_timeout(&timeout)) {
				LL_I2C_ClearFlag_AF(i2c);
				LL_I2C_GenerateStopCondition(i2c);
				data->current.is_nack = 1U;
				goto end;
			}
		}
		LL_I2C_ClearFlag_ADDR(i2c);
	}

	while (len) {
		timeout = STM32_I2C_TIMEOUT_USEC;
		while (1) {
			if (LL_I2C_IsActiveFlag_TXE(i2c)) {
				break;
			}
			if (LL_I2C_IsActiveFlag_AF(i2c) || stm32_i2c_wait_timeout(&timeout)) {
				LL_I2C_ClearFlag_AF(i2c);
				LL_I2C_GenerateStopCondition(i2c);
				data->current.is_nack = 1U;
				goto end;
			}
		}
		LL_I2C_TransmitData8(i2c, *buf);
		buf++;
		len--;
	}

	timeout = STM32_I2C_TIMEOUT_USEC;
	while (!LL_I2C_IsActiveFlag_BTF(i2c)) {
		if (stm32_i2c_wait_timeout(&timeout)) {
			LL_I2C_GenerateStopCondition(i2c);
			data->current.is_err = 1U;
			goto end;
		}
	}

	if (msg->flags & I2C_MSG_STOP) {
		LL_I2C_GenerateStopCondition(i2c);
	}

end:
	check_errors(dev, __func__);
	return msg_end(dev, next_msg_flags, __func__);
}

s32_t stm32_i2c_msg_read(struct device *dev, struct i2c_msg *msg,
			 u8_t *next_msg_flags, u16_t saddr)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;
	u32_t len = msg->len;
	u16_t timeout;
	u8_t *buf = msg->buf;

	msg_init(dev, msg, next_msg_flags, saddr, I2C_REQUEST_READ);

	if (msg->flags & I2C_MSG_RESTART) {
		timeout = STM32_I2C_TIMEOUT_USEC;
		while (!LL_I2C_IsActiveFlag_SB(i2c)) {
			if (stm32_i2c_wait_timeout(&timeout)) {
				LL_I2C_GenerateStopCondition(i2c);
				data->current.is_err = 1U;
				goto end;
			}
		}

		if (I2C_ADDR_10_BITS & data->dev_config) {
			u8_t slave = (((saddr & 0x0300) >> 7) & 0xFF);
			u8_t header = slave | HEADER;

			LL_I2C_TransmitData8(i2c, header);
			timeout = STM32_I2C_TIMEOUT_USEC;
			while (!LL_I2C_IsActiveFlag_ADD10(i2c)) {
				if (stm32_i2c_wait_timeout(&timeout)) {
					LL_I2C_GenerateStopCondition(i2c);
					data->current.is_err = 1U;
					goto end;
				}
			}

			slave = saddr & 0xFF;
			LL_I2C_TransmitData8(i2c, slave);
			timeout = STM32_I2C_TIMEOUT_USEC;
			while (!LL_I2C_IsActiveFlag_ADDR(i2c)) {
				if (stm32_i2c_wait_timeout(&timeout)) {
					LL_I2C_GenerateStopCondition(i2c);
					data->current.is_err = 1U;
					goto end;
				}
			}

			LL_I2C_ClearFlag_ADDR(i2c);
			LL_I2C_GenerateStartCondition(i2c);
			timeout = STM32_I2C_TIMEOUT_USEC;
			while (!LL_I2C_IsActiveFlag_SB(i2c)) {
				if (stm32_i2c_wait_timeout(&timeout)) {
					LL_I2C_GenerateStopCondition(i2c);
					data->current.is_err = 1U;
					goto end;
				}
			}

			header |= I2C_REQUEST_READ;
			LL_I2C_TransmitData8(i2c, header);
		} else {
			u8_t slave = ((saddr) << 1) & 0xFF;

			LL_I2C_TransmitData8(i2c, slave | I2C_REQUEST_READ);
		}

		timeout = STM32_I2C_TIMEOUT_USEC;
		while (!LL_I2C_IsActiveFlag_ADDR(i2c)) {
			if (LL_I2C_IsActiveFlag_AF(i2c) || stm32_i2c_wait_timeout(&timeout)) {
				LL_I2C_ClearFlag_AF(i2c);
				LL_I2C_GenerateStopCondition(i2c);
				data->current.is_nack = 1U;
				goto end;
			}
		}
		/* ADDR must be cleared before NACK generation. Either in 2 byte reception
		 * byte 1 will be NACK'ed and slave wont sent the last byte
		 */
		LL_I2C_ClearFlag_ADDR(i2c);
		if (len == 1U) {
			/* Single byte reception: enable NACK and set STOP */
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
		} else if (len == 2U) {
			/* 2-byte reception: enable NACK and set POS */
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
			LL_I2C_EnableBitPOS(i2c);
		}
	}

	while (len) {
		timeout = STM32_I2C_TIMEOUT_USEC;
		while (!LL_I2C_IsActiveFlag_RXNE(i2c)) {
			if (stm32_i2c_wait_timeout(&timeout)) {
				LL_I2C_GenerateStopCondition(i2c);
				data->current.is_err = 1U;
				goto end;
			}
		}

		timeout = STM32_I2C_TIMEOUT_USEC;
		switch (len) {
		case 1:
			if (msg->flags & I2C_MSG_STOP) {
				LL_I2C_GenerateStopCondition(i2c);
			}
			len--;
			*buf = LL_I2C_ReceiveData8(i2c);
			buf++;
			break;
		case 2:
			while (!LL_I2C_IsActiveFlag_BTF(i2c)) {
				if (stm32_i2c_wait_timeout(&timeout)) {
					LL_I2C_GenerateStopCondition(i2c);
					data->current.is_err = 1U;
					goto end;
				}
			}

			/*
			 * Stop condition must be generated before reading the
			 * last two bytes.
			 */
			if (msg->flags & I2C_MSG_STOP) {
				LL_I2C_GenerateStopCondition(i2c);
			}

			for (u32_t counter = 2; counter > 0; counter--) {
				len--;
				*buf = LL_I2C_ReceiveData8(i2c);
				buf++;
			}

			break;
		case 3:
			while (!LL_I2C_IsActiveFlag_BTF(i2c)) {
				if (stm32_i2c_wait_timeout(&timeout)) {
					LL_I2C_GenerateStopCondition(i2c);
					data->current.is_err = 1U;
					goto end;
				}
			}

			/* Set NACK before reading N-2 byte*/
			LL_I2C_AcknowledgeNextData(i2c, LL_I2C_NACK);
		/* Fall through */
		default:
			len--;
			*buf = LL_I2C_ReceiveData8(i2c);
			buf++;
		}
	}
end:
	check_errors(dev, __func__);
	return msg_end(dev, next_msg_flags, __func__);
}
#endif

s32_t stm32_i2c_configure_timing(struct device *dev, u32_t clock)
{
	const struct i2c_stm32_config *cfg = DEV_CFG(dev);
	struct i2c_stm32_data *data = DEV_DATA(dev);
	I2C_TypeDef *i2c = cfg->i2c;

	switch (I2C_SPEED_GET(data->dev_config)) {
	case I2C_SPEED_STANDARD:
		LL_I2C_ConfigSpeed(i2c, clock, 100000, LL_I2C_DUTYCYCLE_2);
		break;
	case I2C_SPEED_FAST:
		LL_I2C_ConfigSpeed(i2c, clock, 400000, LL_I2C_DUTYCYCLE_2);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
