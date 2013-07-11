/*
 * Copyright (c) 2012, Shantanu Gupta <shans95g@gmail.com>
 * Based on the open source driver from HTC, Interrupts are not supported yet
 */

#include <stdlib.h>
#include <string.h>
#include <msm_i2c.h>
#include <dev/gpio.h>
#include <kernel/mutex.h>
#include <target/microp.h>
#include <platform/timer.h>

static struct microp_platform_data *pdata = NULL;
int msm_microp_i2c_status;

int microp_i2c_read(uint8_t addr, uint8_t *data, int length)
{
	if (!pdata)
		return -1;
		
	struct i2c_msg msgs[] = {
		{.addr = pdata->chip,	.flags = 0,			.len = 1,		.buf = &addr,},
		{.addr = pdata->chip,	.flags = I2C_M_RD,	.len = length,	.buf = data, },
	};
	
	int retry;
	for (retry = 0; retry <= MSM_I2C_READ_RETRY_TIMES; retry++) {
		if (msm_i2c_xfer(msgs, 2) == 2)
			break;
		mdelay(5);
	}
	if (retry > MSM_I2C_WRITE_RETRY_TIMES)
		return -1;
	
	return 0;
}

#define MICROP_I2C_WRITE_BLOCK_SIZE 21
int microp_i2c_write(uint8_t addr, uint8_t *cmd, int length)
{
	if (!pdata)
		return -1;
	if (length >= MICROP_I2C_WRITE_BLOCK_SIZE)
		return -1;
	
	uint8_t cmd_buffer[MICROP_I2C_WRITE_BLOCK_SIZE];
	
	struct i2c_msg msg[] = {
		{.addr = pdata->chip,	.flags = 0,		.len = length + 1,	.buf = cmd_buffer,}
	};
	
	cmd_buffer[0] = addr;
	memcpy((void *)&cmd_buffer[1], (void *)cmd, length);
	
	int retry;
	for (retry = 0; retry <= MSM_I2C_WRITE_RETRY_TIMES; retry++) {
		if (msm_i2c_xfer(msg, 1) == 1)
			break;
		mdelay(5);
	}
	if (retry > MSM_I2C_WRITE_RETRY_TIMES)
		return -1;
	
	return 0;
}

int microp_read_adc(uint8_t channel, uint16_t *value)
{
	uint8_t cmd[2], data[2];

	cmd[0] = 0;
	cmd[1] = 1;

	if (microp_i2c_write(MICROP_I2C_WCMD_READ_ADC_VALUE_REQ, cmd, 2) < 0) {
		dprintf(SPEW, "%s: request adc fail!\n", __func__);
		return -1;
	}

	if (microp_i2c_read(MICROP_I2C_RCMD_ADC_VALUE, data, 2) < 0) {
		dprintf(SPEW, "%s: read adc fail!\n", __func__);
		return -1;
	}
	
	*value = data[0] << 8 | data[1];

	return 0;
}

int microp_read_gpi_status(uint16_t *status)
{
	uint8_t data[2];

	if (microp_i2c_read(MICROP_I2C_RCMD_GPIO_STATUS, data, 2) < 0) {
		dprintf(SPEW, "%s: read fail!\n", __func__);
		return -1;
	}
	
	*status = (data[0] << 8) | data[1];
	
	return 0;
}

int microp_interrupt_get_status(uint16_t *interrupt_mask)
{
	uint8_t data[2];
	int ret = -1;

	ret = microp_i2c_read(MICROP_I2C_RCMD_GPI_INT_STATUS, data, 2);
	if (ret < 0) {
		dprintf(SPEW, "%s: read interrupt status fail\n",  __func__);
		return ret;
	}

	*interrupt_mask = data[0]<<8 | data[1];

	return 0;
}

int microp_interrupt_enable( uint16_t interrupt_mask)
{
	uint8_t data[2];
	int ret = -1;

	data[0] = interrupt_mask >> 8;
	data[1] = interrupt_mask & 0xFF;
	ret = microp_i2c_write(MICROP_I2C_WCMD_GPI_INT_CTL_EN, data, 2);

	if (ret < 0)
		dprintf(INFO, "%s: enable 0x%x interrupt failed\n", __func__, interrupt_mask);
	return ret;
}

int microp_interrupt_disable(uint16_t interrupt_mask)
{
	uint8_t data[2];
	int ret = -1;

	data[0] = interrupt_mask >> 8;
	data[1] = interrupt_mask & 0xFF;
	ret = microp_i2c_write(MICROP_I2C_WCMD_GPI_INT_CTL_DIS, data, 2);

	if (ret < 0)
		dprintf(INFO, "%s: disable 0x%x interrupt failed\n", __func__, interrupt_mask);
	return ret;
}

int microp_read_gpo_status(uint16_t *status)
{
	uint8_t data[2];

	if (microp_i2c_read(MICROP_I2C_RCMD_GPIO_STATUS, data, 2) < 0) 
	{
		dprintf(CRITICAL, "%s: read failed!\n", __func__);
		return -1;
	}

	*status = (data[0] << 8) | data[1];

	return 0;
}

int microp_gpo_enable(uint16_t gpo_mask)
{
	uint8_t data[2];
	int ret = -1;

	data[0] = gpo_mask >> 8;
	data[1] = gpo_mask & 0xFF;
	ret = microp_i2c_write(MICROP_I2C_WCMD_GPO_LED_STATUS_EN, data, 2);

	if (ret < 0)
		dprintf(CRITICAL, "%s: enable 0x%x interrupt failed\n", __func__, gpo_mask);

	return ret;
}

int microp_gpo_disable(uint16_t gpo_mask)
{
	uint8_t data[2];
	int ret = -1;

	data[0] = gpo_mask >> 8;
	data[1] = gpo_mask & 0xFF;
	ret = microp_i2c_write(MICROP_I2C_WCMD_GPO_LED_STATUS_DIS, data, 2);

	if (ret < 0)
		dprintf(CRITICAL, "%s: disable 0x%x interrupt failed\n", __func__, gpo_mask);

	return ret;
}

static int als_power_control=0;
struct mutex capella_cm3602_lock;
int capella_cm3602_power(int pwr_device, uint8_t enable)
{
	unsigned int old_status = 0;
	uint16_t interrupts = 0;
	int ret = 0, on = 0;
	
	mutex_acquire(&capella_cm3602_lock);
	if(pwr_device==PS_PWR_ON) { // Switch the Proximity IRQ
		if(enable) {
			microp_gpo_enable(PS_PWR_ON);
			ret = microp_interrupt_get_status(&interrupts);
			if (ret < 0) {
				printf("read interrupt status fail\n");
				return ret;
			}
			interrupts |= IRQ_PROXIMITY;
			ret = microp_interrupt_enable(interrupts);
		}
		else {
			interrupts |= IRQ_PROXIMITY;
			ret = microp_interrupt_disable(interrupts);
			microp_gpo_disable(PS_PWR_ON);
		}
		if (ret < 0) {
			printf("failed to enable gpi irqs\n");
			return ret;
		}
	}
	
	old_status = als_power_control;
	if (enable)
		als_power_control |= pwr_device;
	else
		als_power_control &= ~pwr_device;

	on = als_power_control ? 1 : 0;
	if (old_status == 0 && on)
		microp_gpo_enable(LS_PWR_ON);
	else if (!on)
		microp_gpo_disable(LS_PWR_ON);
		
	mutex_release(&capella_cm3602_lock);
	
	return ret;
}

/*
static irqreturn_t microp_i2c_intr_irq_handler(int irq, void *dev_id)
{
	disable_irq_nosync(client->irq);
	schedule_work(&cdata->work.work);
	return IRQ_HANDLED;
}

static void microp_i2c_intr_work_func(struct work_struct *work)
{
	struct microp_i2c_work *up_work;
	struct i2c_client *client;
	struct microp_i2c_client_data *cdata;
	uint8_t data[3];
	uint16_t intr_status = 0;
	int ret = 0;

	up_work = container_of(work, struct microp_i2c_work, work);
	client = up_work->client;
	cdata = i2c_get_clientdata(client);

	ret = microp_interrupt_get_status(&intr_status);
	if (ret < 0) {
		dprintf(CRITICAL, "%s: read interrupt status fail\n", __func__);
	}

	ret = msm_i2c_read(MICROP_ADDR, MICROP_I2C_WCMD_GPI_INT_STATUS_CLR, data, 2);
	if (ret < 0) {
		dprintf(CRITICAL, "%s: clear interrupt status fail\n", __func__);
	}

	if (intr_status & IRQ_PROXIMITY) {
		p_sensor_irq_handler();
	}

	enable_irq(client->irq);
}
*/

static int microp_function_initialize(void)
{    
	uint16_t stat, interrupts = 0;
	int ret;

	ret = microp_interrupt_enable(interrupts);
	if (ret < 0) {
		dprintf(CRITICAL, "%s: failed to enable gpi irqs\n", __func__);
		goto err_irq_en;
	}

	microp_read_gpi_status(&stat);
	return 0;

err_irq_en:
	return ret;
}

void microp_i2c_probe(struct microp_platform_data *kpdata)
{
	if(!kpdata || pdata) return;

	pdata = kpdata;
	
	uint8_t data[6];
	if (microp_i2c_read(MICROP_I2C_RCMD_VERSION, data, 2) < 0) {
		msm_microp_i2c_status = 0;
		printf("microp get version failed!\n");
		return;
	}
	printf("HTC MicroP 0x%02X\n", data[0]);
	msm_microp_i2c_status = 1;
	
	//gpio_set(pdata->gpio_reset, 1);
	
	//microp_function_initialize();
}