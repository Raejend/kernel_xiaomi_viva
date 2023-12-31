/*
 * SC8551 battery charging driver
*/

#define pr_fmt(fmt)	"[sc8551] %s: " fmt, __func__
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/err.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/debugfs.h>
#include <linux/bitops.h>
#include <linux/math64.h>
#include <mt-plat/charger_class.h>
#include <mt-plat/charger_type.h>
/* add notify vbusov, eoc, rechg */
#include <mtk_charger_intf.h>
#include "sc8551.h"
#include <linux/hardware_info.h>



static int sc8551_mode_data[] = {
	[SC8551_STDALONE] = SC8551_STDALONE,
	[SC8551_MASTER] = SC8551_ROLE_MASTER,
	[SC8551_SLAVE] = SC8551_ROLE_SLAVE,
};



/************************************************************************/
static int __sc8551_read_byte(struct sc8551 *sc, u8 reg, u8 *data)
{
	s32 ret;

	ret = i2c_smbus_read_byte_data(sc->client, reg);
	if (ret < 0) {
		sc_err("i2c read fail: can't read from reg 0x%02X\n", reg);
		return ret;
	}

	*data = (u8) ret;

	return 0;
}

static int __sc8551_write_byte(struct sc8551 *sc, int reg, u8 val)
{
	s32 ret;

	ret = i2c_smbus_write_byte_data(sc->client, reg, val);
	if (ret < 0) {
		sc_err("i2c write fail: can't write 0x%02X to reg 0x%02X: %d\n",
		       val, reg, ret);
		return ret;
	}
	return 0;
}

static int sc8551_read_byte(struct sc8551 *sc, u8 reg, u8 *data)
{
	int ret;

	if (sc->skip_reads) {
		*data = 0;
		return 0;
	}

	mutex_lock(&sc->i2c_rw_lock);
	ret = __sc8551_read_byte(sc, reg, data);
	mutex_unlock(&sc->i2c_rw_lock);

	return ret;
}

static int sc8551_write_byte(struct sc8551 *sc, u8 reg, u8 data)
{
	int ret;

	if (sc->skip_writes)
		return 0;

	mutex_lock(&sc->i2c_rw_lock);
	ret = __sc8551_write_byte(sc, reg, data);
	mutex_unlock(&sc->i2c_rw_lock);

	return ret;
}

static int sc8551_update_bits(struct sc8551*sc, u8 reg,
				    u8 mask, u8 data)
{
	int ret;
	u8 tmp;

	if (sc->skip_reads || sc->skip_writes)
		return 0;

	mutex_lock(&sc->i2c_rw_lock);
	ret = __sc8551_read_byte(sc, reg, &tmp);
	if (ret) {
		sc_err("Failed: reg=%02X, ret=%d\n", reg, ret);
		goto out;
	}

	tmp &= ~mask;
	tmp |= data & mask;

	ret = __sc8551_write_byte(sc, reg, tmp);
	if (ret)
		sc_err("Failed: reg=%02X, ret=%d\n", reg, ret);

out:
	mutex_unlock(&sc->i2c_rw_lock);
	return ret;
}

/*********************************************************************/

static int sc8551_enable_charge(struct sc8551 *sc, bool enable)
{
	int ret;	
	u8 val;

	if (enable)
		val = SC8551_CHG_ENABLE;
	else
		val = SC8551_CHG_DISABLE;

	val <<= SC8551_CHG_EN_SHIFT;

	sc_err("sc8551 charger %s\n", enable == false ? "disable" : "enable");
	ret = sc8551_update_bits(sc, SC8551_REG_0C,
				SC8551_CHG_EN_MASK, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_charge);

static int sc8551_check_charge_enabled(struct sc8551 *sc, bool *enabled)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_0C, &val);
	sc_info(">>>reg [0x0c] = 0x%02x\n", val);
	if (!ret)
		*enabled = !!(val & SC8551_CHG_EN_MASK);
	return ret;
}

static int sc8551_enable_wdt(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_WATCHDOG_ENABLE;
	else
		val = SC8551_WATCHDOG_DISABLE;

	val <<= SC8551_WATCHDOG_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_0B,
				SC8551_WATCHDOG_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_wdt);

static int sc8551_set_wdt(struct sc8551 *sc, int ms)
{
	int ret;
	u8 val;

	if (ms == 500)
		val = SC8551_WATCHDOG_0P5S;
	else if (ms == 1000)
		val = SC8551_WATCHDOG_1S;
	else if (ms == 5000)
		val = SC8551_WATCHDOG_5S;
	else if (ms == 30000)
		val = SC8551_WATCHDOG_30S;
	else
		val = SC8551_WATCHDOG_30S;

	val <<= SC8551_WATCHDOG_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_0B,
				SC8551_WATCHDOG_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_wdt);


//

static int sc8551_enable_batovp(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BAT_OVP_ENABLE;
	else
		val = SC8551_BAT_OVP_DISABLE;

	val <<= SC8551_BAT_OVP_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_00,
				SC8551_BAT_OVP_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_batovp);

static int sc8551_set_batovp_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BAT_OVP_BASE)
		threshold = SC8551_BAT_OVP_BASE;

	val = (threshold - SC8551_BAT_OVP_BASE) / SC8551_BAT_OVP_LSB;

	val <<= SC8551_BAT_OVP_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_00,
				SC8551_BAT_OVP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_batovp_th);

static int sc8551_enable_batovp_alarm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BAT_OVP_ALM_ENABLE;
	else
		val = SC8551_BAT_OVP_ALM_DISABLE;

	val <<= SC8551_BAT_OVP_ALM_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_01,
				SC8551_BAT_OVP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_batovp_alarm);

static int sc8551_set_batovp_alarm_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BAT_OVP_ALM_BASE)
		threshold = SC8551_BAT_OVP_ALM_BASE;

	val = (threshold - SC8551_BAT_OVP_ALM_BASE) / SC8551_BAT_OVP_ALM_LSB;

	val <<= SC8551_BAT_OVP_ALM_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_01,
				SC8551_BAT_OVP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_batovp_alarm_th);

static int sc8551_enable_batocp(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BAT_OCP_ENABLE;
	else
		val = SC8551_BAT_OCP_DISABLE;

	val <<= SC8551_BAT_OCP_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_02,
				SC8551_BAT_OCP_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_batocp);

static int sc8551_set_batocp_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BAT_OCP_BASE)
		threshold = SC8551_BAT_OCP_BASE;

	val = (threshold - SC8551_BAT_OCP_BASE) / SC8551_BAT_OCP_LSB;

	val <<= SC8551_BAT_OCP_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_02,
				SC8551_BAT_OCP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_batocp_th);

static int sc8551_enable_batocp_alarm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BAT_OCP_ALM_ENABLE;
	else
		val = SC8551_BAT_OCP_ALM_DISABLE;

	val <<= SC8551_BAT_OCP_ALM_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_03,
				SC8551_BAT_OCP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_batocp_alarm);

static int sc8551_set_batocp_alarm_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BAT_OCP_ALM_BASE)
		threshold = SC8551_BAT_OCP_ALM_BASE;

	val = (threshold - SC8551_BAT_OCP_ALM_BASE) / SC8551_BAT_OCP_ALM_LSB;

	val <<= SC8551_BAT_OCP_ALM_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_03,
				SC8551_BAT_OCP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_batocp_alarm_th);


static int sc8551_set_busovp_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BUS_OVP_BASE)
		threshold = SC8551_BUS_OVP_BASE;

	val = (threshold - SC8551_BUS_OVP_BASE) / SC8551_BUS_OVP_LSB;

	val <<= SC8551_BUS_OVP_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_06,
				SC8551_BUS_OVP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_busovp_th);

static int sc8551_enable_busovp_alarm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BUS_OVP_ALM_ENABLE;
	else
		val = SC8551_BUS_OVP_ALM_DISABLE;

	val <<= SC8551_BUS_OVP_ALM_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_07,
				SC8551_BUS_OVP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_busovp_alarm);

static int sc8551_set_busovp_alarm_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BUS_OVP_ALM_BASE)
		threshold = SC8551_BUS_OVP_ALM_BASE;

	val = (threshold - SC8551_BUS_OVP_ALM_BASE) / SC8551_BUS_OVP_ALM_LSB;

	val <<= SC8551_BUS_OVP_ALM_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_07,
				SC8551_BUS_OVP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_busovp_alarm_th);

static int sc8551_enable_busocp(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BUS_OCP_ENABLE;
	else
		val = SC8551_BUS_OCP_DISABLE;

	val <<= SC8551_BUS_OCP_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_08,
				SC8551_BUS_OCP_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_busocp);


static int sc8551_set_busocp_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BUS_OCP_BASE)
		threshold = SC8551_BUS_OCP_BASE;

	val = (threshold - SC8551_BUS_OCP_BASE) / SC8551_BUS_OCP_LSB;

	val <<= SC8551_BUS_OCP_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_08,
				SC8551_BUS_OCP_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_busocp_th);

static int sc8551_enable_busocp_alarm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BUS_OCP_ALM_ENABLE;
	else
		val = SC8551_BUS_OCP_ALM_DISABLE;

	val <<= SC8551_BUS_OCP_ALM_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_09,
				SC8551_BUS_OCP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_busocp_alarm);

static int sc8551_set_busocp_alarm_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BUS_OCP_ALM_BASE)
		threshold = SC8551_BUS_OCP_ALM_BASE;

	val = (threshold - SC8551_BUS_OCP_ALM_BASE) / SC8551_BUS_OCP_ALM_LSB;

	val <<= SC8551_BUS_OCP_ALM_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_09,
				SC8551_BUS_OCP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_busocp_alarm_th);

static int sc8551_enable_batucp_alarm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_BAT_UCP_ALM_ENABLE;
	else
		val = SC8551_BAT_UCP_ALM_DISABLE;

	val <<= SC8551_BAT_UCP_ALM_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_04,
				SC8551_BAT_UCP_ALM_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_batucp_alarm);

static int sc8551_set_batucp_alarm_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_BAT_UCP_ALM_BASE)
		threshold = SC8551_BAT_UCP_ALM_BASE;

	val = (threshold - SC8551_BAT_UCP_ALM_BASE) / SC8551_BAT_UCP_ALM_LSB;

	val <<= SC8551_BAT_UCP_ALM_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_04,
				SC8551_BAT_UCP_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_batucp_alarm_th);

static int sc8551_set_acovp_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold < SC8551_AC_OVP_BASE)
		threshold = SC8551_AC_OVP_BASE;

	if (threshold == SC8551_AC_OVP_6P5V)
		val = 0x07;
	else
		val = (threshold - SC8551_AC_OVP_BASE) /  SC8551_AC_OVP_LSB;

	val <<= SC8551_AC_OVP_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_05,
				SC8551_AC_OVP_MASK, val);

	return ret;

}
EXPORT_SYMBOL_GPL(sc8551_set_acovp_th);

static int sc8551_set_vdrop_th(struct sc8551 *sc, int threshold)
{
	int ret;
	u8 val;

	if (threshold == 300)
		val = SC8551_VDROP_THRESHOLD_300MV;
	else
		val = SC8551_VDROP_THRESHOLD_400MV;

	val <<= SC8551_VDROP_THRESHOLD_SET_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_05,
				SC8551_VDROP_THRESHOLD_SET_MASK,
				val);

	return ret;
}

static int sc8551_set_vdrop_deglitch(struct sc8551 *sc, int us)
{
	int ret;
	u8 val;

	if (us == 8)
		val = SC8551_VDROP_DEGLITCH_8US;
	else
		val = SC8551_VDROP_DEGLITCH_5MS;

	val <<= SC8551_VDROP_DEGLITCH_SET_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_05,
				SC8551_VDROP_DEGLITCH_SET_MASK,
				val);
	return ret;
}

static int sc8551_enable_bat_therm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_TSBAT_ENABLE;
	else
		val = SC8551_TSBAT_DISABLE;

	val <<= SC8551_TSBAT_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_0C,
				SC8551_TSBAT_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_bat_therm);

/*
 * the input threshold is the raw value that would write to register directly.
 */
static int sc8551_set_bat_therm_th(struct sc8551 *sc, u8 threshold)
{
	int ret;

	ret = sc8551_write_byte(sc, SC8551_REG_29, threshold);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_bat_therm_th);

static int sc8551_enable_bus_therm(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_TSBUS_ENABLE;
	else
		val = SC8551_TSBUS_DISABLE;

	val <<= SC8551_TSBUS_DIS_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_0C,
				SC8551_TSBUS_DIS_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_bus_therm);

/*
 * the input threshold is the raw value that would write to register directly.
 */
static int sc8551_set_bus_therm_th(struct sc8551 *sc, u8 threshold)
{
	int ret;

	ret = sc8551_write_byte(sc, SC8551_REG_28, threshold);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_bus_therm_th);

/*
 * please be noted that the unit here is degC
 */
static int sc8551_set_die_therm_th(struct sc8551 *sc, u8 threshold)
{
	int ret;
	u8 val;

	/*BE careful, LSB is here is 1/LSB, so we use multiply here*/
	val = (threshold - SC8551_TDIE_ALM_BASE) * 10/SC8551_TDIE_ALM_LSB;
	val <<= SC8551_TDIE_ALM_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_2A,
				SC8551_TDIE_ALM_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_die_therm_th);

static int sc8551_enable_adc(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_ADC_ENABLE;
	else
		val = SC8551_ADC_DISABLE;

	val <<= SC8551_ADC_EN_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_14,
				SC8551_ADC_EN_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_enable_adc);


static int sc8551_set_adc_scanrate(struct sc8551 *sc, bool oneshot)
{
	int ret;
	u8 val;

	if (oneshot)
		val = SC8551_ADC_RATE_ONESHOT;
	else
		val = SC8551_ADC_RATE_CONTINOUS;

	val <<= SC8551_ADC_RATE_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_14,
				SC8551_ADC_EN_MASK, val);
	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_adc_scanrate);


#define ADC_REG_BASE SC8551_REG_16
static int sc8551_get_adc_data(struct sc8551 *sc, int channel,  int *result)
{
	int ret;
	u8 val_l, val_h;
	u16 val;

	if(channel >= ADC_MAX_NUM) return 0;

	ret = sc8551_read_byte(sc, ADC_REG_BASE + (channel << 1), &val_h);
	ret = sc8551_read_byte(sc, ADC_REG_BASE + (channel << 1) + 1, &val_l);

	if (ret < 0)
		return ret;
	val = (val_h << 8) | val_l;

	if(sc->is_sc8551)
	{
		/*if(channel == ADC_IBUS)				val *= SC8551_IBUS_ADC_LSB;
		else if(channel == ADC_VBUS)		val *= SC8551_VBUS_ADC_LSB;
		else if(channel == ADC_VAC)			val *= SC8551_VAC_ADC_LSB;
		else if(channel == ADC_VOUT)		val *= SC8551_VOUT_ADC_LSB;
		else if(channel == ADC_VBAT)		val *= SC8551_VBAT_ADC_LSB;
		else if(channel == ADC_TBUS)		val *= SC8551_TSBUS_ADC_LSB;
		else if(channel == ADC_TBAT)		val *= SC8551_TSBAT_ADC_LSB;
		else if(channel == ADC_TDIE)		val *= SC8551_TDIE_ADC_LSB;*/
		if(channel == ADC_IBUS)	{			val = val * 15625/10000;}
		else if(channel == ADC_VBUS)		val = val * 375/100;
		else if(channel == ADC_VAC)			val = val * 5;
		else if(channel == ADC_VOUT)		val = val * 125 / 100;
		else if(channel == ADC_VBAT)		val = val * 125/100;
		else if(channel == ADC_IBAT)		val = val * 3125/1000 ;
		else if(channel == ADC_TBUS)		val = val * 9766/100000;
		else if(channel == ADC_TBAT)		val = val * 9766/100000;
		//Bug674901,wangbin wt.MODIFY	.20210612,modify for return 3 digit degree.
		else if(channel == ADC_TDIE)		val = val * 5;
	}

	*result = val;

	return 0;
}
EXPORT_SYMBOL_GPL(sc8551_get_adc_data);

static int sc8551_set_adc_scan(struct sc8551 *sc, int channel, bool enable)
{
	int ret;
	u8 reg;
	u8 mask;
	u8 shift;
	u8 val;

	if (channel > ADC_MAX_NUM)
		return -EINVAL;

	if (channel == ADC_IBUS) {
		reg = SC8551_REG_14;
		shift = SC8551_IBUS_ADC_DIS_SHIFT;
		mask = SC8551_IBUS_ADC_DIS_MASK;
	} else {
		reg = SC8551_REG_15;
		shift = 8 - channel;
		mask = 1 << shift;
	}

	if (enable)
		val = 0 << shift;
	else
		val = 1 << shift;

	ret = sc8551_update_bits(sc, reg, mask, val);

	return ret;
}

static int sc8551_set_alarm_int_mask(struct sc8551 *sc, u8 mask)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_0F, &val);
	if (ret)
		return ret;

	val |= mask;

	ret = sc8551_write_byte(sc, SC8551_REG_0F, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_alarm_int_mask);

static int sc8551_clear_alarm_int_mask(struct sc8551 *sc, u8 mask)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_0F, &val);
	if (ret)
		return ret;

	val &= ~mask;

	ret = sc8551_write_byte(sc, SC8551_REG_0F, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_clear_alarm_int_mask);

static int sc8551_set_fault_int_mask(struct sc8551 *sc, u8 mask)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_12, &val);
	if (ret)
		return ret;

	val |= mask;

	ret = sc8551_write_byte(sc, SC8551_REG_12, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_set_fault_int_mask);

static int sc8551_clear_fault_int_mask(struct sc8551 *sc, u8 mask)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_12, &val);
	if (ret)
		return ret;

	val &= ~mask;

	ret = sc8551_write_byte(sc, SC8551_REG_12, val);

	return ret;
}
EXPORT_SYMBOL_GPL(sc8551_clear_fault_int_mask);

static int sc8551_set_sense_resistor(struct sc8551 *sc, int r_mohm)
{
	int ret;
	u8 val;

	if (r_mohm == 2)
		val = SC8551_SET_IBAT_SNS_RES_2MHM;
	else if (r_mohm == 5)
		val = SC8551_SET_IBAT_SNS_RES_5MHM;
	else
		return -EINVAL;

	val <<= SC8551_SET_IBAT_SNS_RES_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_2B,
				SC8551_SET_IBAT_SNS_RES_MASK,
				val);
	return ret;
}

static int sc8551_enable_regulation(struct sc8551 *sc, bool enable)
{
	int ret;
	u8 val;

	if (enable)
		val = SC8551_EN_REGULATION_ENABLE;
	else
		val = SC8551_EN_REGULATION_DISABLE;

	val <<= SC8551_EN_REGULATION_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_2B,
				SC8551_EN_REGULATION_MASK,
				val);

	return ret;

}

static int sc8551_set_ss_timeout(struct sc8551 *sc, int timeout)
{
	int ret;
	u8 val;

	switch (timeout) {
	case 0:
		val = SC8551_SS_TIMEOUT_DISABLE;
		break;
	case 12:
		val = SC8551_SS_TIMEOUT_12P5MS;
		break;
	case 25:
		val = SC8551_SS_TIMEOUT_25MS;
		break;
	case 50:
		val = SC8551_SS_TIMEOUT_50MS;
		break;
	case 100:
		val = SC8551_SS_TIMEOUT_100MS;
		break;
	case 400:
		val = SC8551_SS_TIMEOUT_400MS;
		break;
	case 1500:
		val = SC8551_SS_TIMEOUT_1500MS;
		break;
	case 100000:
		val = SC8551_SS_TIMEOUT_100000MS;
		break;
	default:
		val = SC8551_SS_TIMEOUT_DISABLE;
		break;
	}

	val <<= SC8551_SS_TIMEOUT_SET_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_2B,
				SC8551_SS_TIMEOUT_SET_MASK,
				val);

	return ret;
}

static int sc8551_set_ibat_reg_th(struct sc8551 *sc, int th_ma)
{
	int ret;
	u8 val;

	if (th_ma == 200)
		val = SC8551_IBAT_REG_200MA;
	else if (th_ma == 300)
		val = SC8551_IBAT_REG_300MA;
	else if (th_ma == 400)
		val = SC8551_IBAT_REG_400MA;
	else if (th_ma == 500)
		val = SC8551_IBAT_REG_500MA;
	else
		val = SC8551_IBAT_REG_500MA;

	val <<= SC8551_IBAT_REG_SHIFT;
	ret = sc8551_update_bits(sc, SC8551_REG_2C,
				SC8551_IBAT_REG_MASK,
				val);

	return ret;

}

static int sc8551_set_vbat_reg_th(struct sc8551 *sc, int th_mv)
{
	int ret;
	u8 val;

	if (th_mv == 50)
		val = SC8551_VBAT_REG_50MV;
	else if (th_mv == 100)
		val = SC8551_VBAT_REG_100MV;
	else if (th_mv == 150)
		val = SC8551_VBAT_REG_150MV;
	else
		val = SC8551_VBAT_REG_200MV;

	val <<= SC8551_VBAT_REG_SHIFT;

	ret = sc8551_update_bits(sc, SC8551_REG_2C,
				SC8551_VBAT_REG_MASK,
				val);

	return ret;
}


static int sc8551_get_work_mode(struct sc8551 *sc, int *mode)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_0C, &val);

	if (ret) {
		sc_err("Failed to read operation mode register\n");
		return ret;
	}

	val = (val & SC8551_MS_MASK) >> SC8551_MS_SHIFT;
	if (val == SC8551_MS_MASTER)
		*mode = SC8551_ROLE_MASTER;
	else if (val == SC8551_MS_SLAVE)
		*mode = SC8551_ROLE_SLAVE;
	else 
		*mode = SC8551_ROLE_STDALONE;

	sc_info("work mode:%s\n", *mode == SC8551_ROLE_STDALONE ? "Standalone" :
			(*mode == SC8551_ROLE_SLAVE ? "Slave" : "Master"));
	return ret;
}

static int sc8551_check_vbus_error_status(struct sc8551 *sc)
{
	int ret;
	u8 data;
	
	ret = sc8551_read_byte(sc, SC8551_REG_0A, &data);
	if(ret == 0){
		sc_err("vbus error >>>>%02x\n", data);
		sc->vbus_error = data;
	}

	return ret;
}

static int sc8551_detect_device(struct sc8551 *sc)
{
	int ret;
	u8 data;

	ret = sc8551_read_byte(sc, SC8551_REG_13, &data);
	if (ret == 0) {
		sc->part_no = (data & SC8551_DEV_ID_MASK);
		sc->part_no >>= SC8551_DEV_ID_SHIFT;
	}

	return ret;
}

static int sc8551_parse_dt(struct sc8551 *sc, struct device *dev)
{
	int ret;
	struct device_node *np = dev->of_node;

	sc->cfg = devm_kzalloc(dev, sizeof(struct sc8551_cfg),
					GFP_KERNEL);

	if (!sc->cfg)
		return -ENOMEM;

	sc->cfg->bat_ovp_disable = of_property_read_bool(np,
			"sc,sc8551,bat-ovp-disable");
	sc->cfg->bat_ocp_disable = of_property_read_bool(np,
			"sc,sc8551,bat-ocp-disable");
	sc->cfg->bat_ovp_alm_disable = of_property_read_bool(np,
			"sc,sc8551,bat-ovp-alarm-disable");
	sc->cfg->bat_ocp_alm_disable = of_property_read_bool(np,
			"sc,sc8551,bat-ocp-alarm-disable");
	sc->cfg->bus_ocp_disable = of_property_read_bool(np,
			"sc,sc8551,bus-ocp-disable");
	sc->cfg->bus_ovp_alm_disable = of_property_read_bool(np,
			"sc,sc8551,bus-ovp-alarm-disable");
	sc->cfg->bus_ocp_alm_disable = of_property_read_bool(np,
			"sc,sc8551,bus-ocp-alarm-disable");
	sc->cfg->bat_ucp_alm_disable = of_property_read_bool(np,
			"sc,sc8551,bat-ucp-alarm-disable");
	sc->cfg->bat_therm_disable = of_property_read_bool(np,
			"sc,sc8551,bat-therm-disable");
	sc->cfg->bus_therm_disable = of_property_read_bool(np,
			"sc,sc8551,bus-therm-disable");

	ret = of_property_read_u32(np, "sc,sc8551,bat-ovp-threshold",
			&sc->cfg->bat_ovp_th);
	if (ret) {
		sc_err("failed to read bat-ovp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bat-ovp-alarm-threshold",
			&sc->cfg->bat_ovp_alm_th);
	if (ret) {
		sc_err("failed to read bat-ovp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bat-ocp-threshold",
			&sc->cfg->bat_ocp_th);
	if (ret) {
		sc_err("failed to read bat-ocp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bat-ocp-alarm-threshold",
			&sc->cfg->bat_ocp_alm_th);
	if (ret) {
		sc_err("failed to read bat-ocp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bus-ovp-threshold",
			&sc->cfg->bus_ovp_th);
	if (ret) {
		sc_err("failed to read bus-ovp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bus-ovp-alarm-threshold",
			&sc->cfg->bus_ovp_alm_th);
	if (ret) {
		sc_err("failed to read bus-ovp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bus-ocp-threshold",
			&sc->cfg->bus_ocp_th);
	if (ret) {
		sc_err("failed to read bus-ocp-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bus-ocp-alarm-threshold",
			&sc->cfg->bus_ocp_alm_th);
	if (ret) {
		sc_err("failed to read bus-ocp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bat-ucp-alarm-threshold",
			&sc->cfg->bat_ucp_alm_th);
	if (ret) {
		sc_err("failed to read bat-ucp-alarm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bat-therm-threshold",
			&sc->cfg->bat_therm_th);
	if (ret) {
		sc_err("failed to read bat-therm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,bus-therm-threshold",
			&sc->cfg->bus_therm_th);
	if (ret) {
		sc_err("failed to read bus-therm-threshold\n");
		return ret;
	}
	ret = of_property_read_u32(np, "sc,sc8551,die-therm-threshold",
			&sc->cfg->die_therm_th);
	if (ret) {
		sc_err("failed to read die-therm-threshold\n");
		return ret;
	}

	ret = of_property_read_u32(np, "sc,sc8551,ac-ovp-threshold",
			&sc->cfg->ac_ovp_th);
	if (ret) {
		sc_err("failed to read ac-ovp-threshold\n");
		return ret;
	}

	ret = of_property_read_u32(np, "sc,sc8551,sense-resistor-mohm",
			&sc->cfg->sense_r_mohm);
	if (ret) {
		sc_err("failed to read sense-resistor-mohm\n");
		return ret;
	}

	/*sc->irq_gpio = devm_gpiod_get(sc->dev, "sc,sc8551,interrupt_gpios", GPIOD_IN);
	if (IS_ERR(sc->irq_gpio)) {
		return PTR_ERR(sc->irq_gpio);
	}*/
	ret = of_get_named_gpio(np, "sc,sc8551,interrupt_gpios", 0);
	if (ret < 0) {
		sc_err("no intr_gpio info\n");
		return ret;
	}
	sc->irq_gpio = ret;

	return 0;
}


static int sc8551_init_protection(struct sc8551 *sc)
{
	int ret;

	ret = sc8551_enable_batovp(sc, !sc->cfg->bat_ovp_disable);
	sc_info("%s bat ovp %s\n",
		sc->cfg->bat_ovp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_batocp(sc, !sc->cfg->bat_ocp_disable);
	sc_info("%s bat ocp %s\n",
		sc->cfg->bat_ocp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_batovp_alarm(sc, !sc->cfg->bat_ovp_alm_disable);
	sc_info("%s bat ovp alarm %s\n",
		sc->cfg->bat_ovp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_batocp_alarm(sc, !sc->cfg->bat_ocp_alm_disable);
	sc_info("%s bat ocp alarm %s\n",
		sc->cfg->bat_ocp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_batucp_alarm(sc, !sc->cfg->bat_ucp_alm_disable);
	sc_info("%s bat ocp alarm %s\n",
		sc->cfg->bat_ucp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_busovp_alarm(sc, !sc->cfg->bus_ovp_alm_disable);
	sc_info("%s bus ovp alarm %s\n",
		sc->cfg->bus_ovp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_busocp(sc, !sc->cfg->bus_ocp_disable);
	sc_info("%s bus ocp %s\n",
		sc->cfg->bus_ocp_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_busocp_alarm(sc, !sc->cfg->bus_ocp_alm_disable);
	sc_info("%s bus ocp alarm %s\n",
		sc->cfg->bus_ocp_alm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_bat_therm(sc, !sc->cfg->bat_therm_disable);
	sc_info("%s bat therm %s\n",
		sc->cfg->bat_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_enable_bus_therm(sc, !sc->cfg->bus_therm_disable);
	sc_info("%s bus therm %s\n",
		sc->cfg->bus_therm_disable ? "disable" : "enable",
		!ret ? "successfullly" : "failed");

	ret = sc8551_set_batovp_th(sc, sc->cfg->bat_ovp_th);
	sc_info("set bat ovp th %d %s\n", sc->cfg->bat_ovp_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_batovp_alarm_th(sc, sc->cfg->bat_ovp_alm_th);
	sc_info("set bat ovp alarm threshold %d %s\n", sc->cfg->bat_ovp_alm_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_batocp_th(sc, sc->cfg->bat_ocp_th);
	sc_info("set bat ocp threshold %d %s\n", sc->cfg->bat_ocp_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_batocp_alarm_th(sc, sc->cfg->bat_ocp_alm_th);
	sc_info("set bat ocp alarm threshold %d %s\n", sc->cfg->bat_ocp_alm_th,
		!ret ? "successfully" : "failed");
//sc->cfg->bus_ovp_th = 12350;
	ret = sc8551_set_busovp_th(sc, sc->cfg->bus_ovp_th);
	sc_info("set bus ovp threshold %d %s\n", sc->cfg->bus_ovp_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_busovp_alarm_th(sc, sc->cfg->bus_ovp_alm_th);
	sc_info("set bus ovp alarm threshold %d %s\n", sc->cfg->bus_ovp_alm_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_busocp_th(sc, sc->cfg->bus_ocp_th);
	sc_info("set bus ocp threshold %d %s\n", sc->cfg->bus_ocp_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_busocp_alarm_th(sc, sc->cfg->bus_ocp_alm_th);
	sc_info("set bus ocp alarm th %d %s\n", sc->cfg->bus_ocp_alm_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_batucp_alarm_th(sc, sc->cfg->bat_ucp_alm_th);
	sc_info("set bat ucp threshold %d %s\n", sc->cfg->bat_ucp_alm_th,
		!ret ? "successfully" : "failed");

	ret = sc8551_set_bat_therm_th(sc, sc->cfg->bat_therm_th);
	sc_info("set die therm threshold %d %s\n", sc->cfg->bat_therm_th,
		!ret ? "successfully" : "failed");
	ret = sc8551_set_bus_therm_th(sc, sc->cfg->bus_therm_th);
	sc_info("set bus therm threshold %d %s\n", sc->cfg->bus_therm_th,
		!ret ? "successfully" : "failed");
	ret = sc8551_set_die_therm_th(sc, sc->cfg->die_therm_th);
	sc_info("set die therm threshold %d %s\n", sc->cfg->die_therm_th,
		!ret ? "successfully" : "failed");
//sc->cfg->ac_ovp_th = 12;
	ret = sc8551_set_acovp_th(sc, sc->cfg->ac_ovp_th);
	sc_info("set ac ovp threshold %d %s\n", sc->cfg->ac_ovp_th,
		!ret ? "successfully" : "failed");

	return 0;
}

static int sc8551_init_adc(struct sc8551 *sc)
{

	sc8551_set_adc_scanrate(sc, false);
	sc8551_set_adc_scan(sc, ADC_IBUS, true);
	sc8551_set_adc_scan(sc, ADC_VBUS, true);
	sc8551_set_adc_scan(sc, ADC_VOUT, true);
	sc8551_set_adc_scan(sc, ADC_VBAT, true);
	sc8551_set_adc_scan(sc, ADC_IBAT, true);
	sc8551_set_adc_scan(sc, ADC_TBUS, true);
	sc8551_set_adc_scan(sc, ADC_TBAT, true);
	sc8551_set_adc_scan(sc, ADC_TDIE, true);
	sc8551_set_adc_scan(sc, ADC_VAC, true);

	sc8551_enable_adc(sc, true);

	return 0;
}

static int sc8551_init_int_src(struct sc8551 *sc)
{
	int ret;
	/*TODO:be careful ts bus and ts bat alarm bit mask is in
	 *	fault mask register, so you need call
	 *	sc8551_set_fault_int_mask for tsbus and tsbat alarm
	 */
	ret = sc8551_set_alarm_int_mask(sc, ADC_DONE
		/*			| BAT_UCP_ALARM */
					| BAT_OVP_ALARM);
	if (ret) {
		sc_err("failed to set alarm mask:%d\n", ret);
		return ret;
	}
#if 0
	ret = sc8551_set_fault_int_mask(sc, TS_BUS_FAULT);
	if (ret) {
		sc_err("failed to set fault mask:%d\n", ret);
		return ret;
	}
#endif
	return ret;
}

static int sc8551_init_regulation(struct sc8551 *sc)
{
	sc8551_set_ibat_reg_th(sc, 300);
	sc8551_set_vbat_reg_th(sc, 100);

	sc8551_set_vdrop_deglitch(sc, 5000);
	sc8551_set_vdrop_th(sc, 400);

	sc8551_enable_regulation(sc, false);
	
	sc8551_write_byte(sc, SC8551_REG_2E, 0x08);
	sc8551_update_bits(sc, SC8551_REG_34, 0x01, 0x01);
	//add 20210825
	//sc8551_update_bits(sc, SC8551_REG_30, 0x80, 0x80);
	
	return 0;
}

 int sc8551_dump_register(struct sc8551 *sc)
{
	u8 addr = 0;
	u8 val = 0 ;
	int ret = 0;

	for (addr = 0x0; addr <= 0x31; addr++) {
		ret = sc8551_read_byte(sc, addr, &val);
		if (!ret)
			pr_err("SC8551_REG[%.2x] = 0x%.2x\n", addr, val);
	}
	return 0;
}
 
static int sc8551_init_device(struct sc8551 *sc)
{
	sc8551_update_bits(sc, SC8551_REG_0B, 0x80, 0x80);
	sc8551_enable_wdt(sc, false);

	sc8551_set_ss_timeout(sc, 100000);
	sc8551_set_sense_resistor(sc, sc->cfg->sense_r_mohm);

	sc8551_init_protection(sc);
	sc8551_init_adc(sc);
	sc8551_init_int_src(sc);

	sc8551_init_regulation(sc);
	sc8551_dump_register(sc);
	return 0;
}


static int sc8551_set_present(struct sc8551 *sc, bool present)
{
	sc->usb_present = present;

	if (present)
		sc8551_init_device(sc);
	return 0;
}

static ssize_t sc8551_show_registers(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sc8551 *sc = dev_get_drvdata(dev);
	u8 addr;
	u8 val;
	u8 tmpbuf[300];
	int len;
	int idx = 0;
	int ret;

	idx = snprintf(buf, PAGE_SIZE, "%s:\n", "sc8551");
	for (addr = 0x0; addr <= 0x31; addr++) {
		ret = sc8551_read_byte(sc, addr, &val);
		if (ret == 0) {
			len = snprintf(tmpbuf, PAGE_SIZE - idx,
					"Reg[%.2X] = 0x%.2x\n", addr, val);
			memcpy(&buf[idx], tmpbuf, len);
			idx += len;
		}
	}

	return idx;
}

static ssize_t sc8551_store_register(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct sc8551 *sc = dev_get_drvdata(dev);
	int ret;
	unsigned int reg;
	unsigned int val;

	ret = sscanf(buf, "%x %x", &reg, &val);
	if (ret == 2 && reg <= 0x31)
		sc8551_write_byte(sc, (unsigned char)reg, (unsigned char)val);

	return count;
}


static DEVICE_ATTR(registers, 0660, sc8551_show_registers, sc8551_store_register);

static void sc8551_create_device_node(struct device *dev)
{
	device_create_file(dev, &dev_attr_registers);
}


static enum power_supply_property sc8551_charger_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
	POWER_SUPPLY_PROP_STATUS,

	POWER_SUPPLY_PROP_SC_BATTERY_PRESENT,
	POWER_SUPPLY_PROP_SC_VBUS_PRESENT,
	POWER_SUPPLY_PROP_SC_BATTERY_VOLTAGE,
	POWER_SUPPLY_PROP_SC_BATTERY_CURRENT,
	POWER_SUPPLY_PROP_SC_BATTERY_TEMPERATURE,
	POWER_SUPPLY_PROP_SC_BUS_VOLTAGE,
	POWER_SUPPLY_PROP_SC_BUS_CURRENT,
	POWER_SUPPLY_PROP_SC_BUS_TEMPERATURE,
	POWER_SUPPLY_PROP_SC_DIE_TEMPERATURE,
	POWER_SUPPLY_PROP_SC_ALARM_STATUS,
	POWER_SUPPLY_PROP_SC_FAULT_STATUS,
	POWER_SUPPLY_PROP_SC_VBUS_ERROR_STATUS,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,

};



static void sc8551_check_alarm_status(struct sc8551 *sc);
static void sc8551_check_fault_status(struct sc8551 *sc);
extern int get_constant_current_max(void);

static int sc8551_charger_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct sc8551 *sc = power_supply_get_drvdata(psy);
	int result;
	int ret;
	u8 reg_val;

	sc_dbg(">>>>>psp = %d\n", psp);

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		sc8551_check_charge_enabled(sc, &sc->charge_enabled);
		val->intval = sc->charge_enabled;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = sc->usb_present;
		break;
	case POWER_SUPPLY_PROP_SC_BATTERY_PRESENT:
		ret = sc8551_read_byte(sc, SC8551_REG_0D, &reg_val);
		if (!ret)
			sc->batt_present  = !!(reg_val & VBAT_INSERT);
		val->intval = sc->batt_present;
		break;
	case POWER_SUPPLY_PROP_SC_VBUS_PRESENT:
		ret = sc8551_read_byte(sc, SC8551_REG_0D, &reg_val);
		if (!ret)
			sc->vbus_present  = !!(reg_val & VBUS_INSERT);
		val->intval = sc->vbus_present;
		break;
	case POWER_SUPPLY_PROP_SC_BATTERY_VOLTAGE:
		ret = sc8551_get_adc_data(sc, ADC_VBAT, &result);
		if (!ret)
			sc->vbat_volt = result;

		val->intval = sc->vbat_volt;
		break;
	case POWER_SUPPLY_PROP_SC_BATTERY_CURRENT:
		ret = sc8551_get_adc_data(sc, ADC_IBUS, &result);
		if (!ret)
			sc->ibat_curr = result;

		val->intval = sc->ibat_curr*2;
		break;
	case POWER_SUPPLY_PROP_SC_BATTERY_TEMPERATURE:
		ret = sc8551_get_adc_data(sc, ADC_TBAT, &result);
		if (!ret)
			sc->bat_temp = result;

		val->intval = sc->bat_temp;
		break;
	case POWER_SUPPLY_PROP_SC_BUS_VOLTAGE:
		ret = sc8551_get_adc_data(sc, ADC_VBUS, &result);
		if (!ret)
			sc->vbus_volt = result;

		val->intval = sc->vbus_volt;
		break;
	case POWER_SUPPLY_PROP_SC_BUS_CURRENT:
		ret = sc8551_get_adc_data(sc, ADC_IBUS, &result);
		if (!ret)
			sc->ibus_curr = result;

		val->intval = sc->ibus_curr;
		break;
	case POWER_SUPPLY_PROP_SC_BUS_TEMPERATURE:
		ret = sc8551_get_adc_data(sc, ADC_TBUS, &result);
		if (!ret)
			sc->bus_temp = result;

		val->intval = sc->bus_temp;

		break;
	case POWER_SUPPLY_PROP_SC_DIE_TEMPERATURE:
		ret = sc8551_get_adc_data(sc, ADC_TDIE, &result);
		if (!ret)
			sc->die_temp = result;

		val->intval = sc->die_temp;
		break;
	case POWER_SUPPLY_PROP_SC_ALARM_STATUS:

		sc8551_check_alarm_status(sc);

		val->intval = ((sc->bat_ovp_alarm << BAT_OVP_ALARM_SHIFT)
			| (sc->bat_ocp_alarm << BAT_OCP_ALARM_SHIFT)
			| (sc->bat_ucp_alarm << BAT_UCP_ALARM_SHIFT)
			| (sc->bus_ovp_alarm << BUS_OVP_ALARM_SHIFT)
			| (sc->bus_ocp_alarm << BUS_OCP_ALARM_SHIFT)
			| (sc->bat_therm_alarm << BAT_THERM_ALARM_SHIFT)
			| (sc->bus_therm_alarm << BUS_THERM_ALARM_SHIFT)
			| (sc->die_therm_alarm << DIE_THERM_ALARM_SHIFT));
		break;

	case POWER_SUPPLY_PROP_SC_FAULT_STATUS:
		sc8551_check_fault_status(sc);

		val->intval = ((sc->bat_ovp_fault << BAT_OVP_FAULT_SHIFT)
			| (sc->bat_ocp_fault << BAT_OCP_FAULT_SHIFT)
			| (sc->bus_ovp_fault << BUS_OVP_FAULT_SHIFT)
			| (sc->bus_ocp_fault << BUS_OCP_FAULT_SHIFT)
			| (sc->bat_therm_fault << BAT_THERM_FAULT_SHIFT)
			| (sc->bus_therm_fault << BUS_THERM_FAULT_SHIFT)
			| (sc->die_therm_fault << DIE_THERM_FAULT_SHIFT));
		break;

	case POWER_SUPPLY_PROP_SC_VBUS_ERROR_STATUS:
		sc8551_check_vbus_error_status(sc);
		val->intval = sc->vbus_error;

		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = 12000000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = get_constant_current_max();
		break;
	default:
		return -EINVAL;

	}

	return 0;
}


static int sc8551_charger_set_property(struct power_supply *psy,
				       enum power_supply_property prop,
				       const union power_supply_propval *val)
{
	struct sc8551 *sc = power_supply_get_drvdata(psy);
	
	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		sc8551_enable_charge(sc, val->intval);
		sc8551_check_charge_enabled(sc, &sc->charge_enabled);
		sc_info("POWER_SUPPLY_PROP_CHARGING_ENABLED: %s\n",
				val->intval ? "enable" : "disable");
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		sc8551_set_present(sc, !!val->intval);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}


static int sc8551_charger_is_writeable(struct power_supply *psy,
				       enum power_supply_property prop)
{
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_CHARGING_ENABLED:
		ret = 1;
		break;
	default:
		ret = 0;
		break;
	}
	return ret;
}

static int sc8551_psy_register(struct sc8551 *sc)
{
	int ret;

	sc->psy_cfg.drv_data = sc;
	sc->psy_cfg.of_node = sc->dev->of_node;
	sc->psy_desc.name = "sc8551-standalone";
	sc->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	sc->psy_desc.properties = sc8551_charger_props;
	sc->psy_desc.num_properties = ARRAY_SIZE(sc8551_charger_props);
	sc->psy_desc.get_property = sc8551_charger_get_property;
	sc->psy_desc.set_property = sc8551_charger_set_property;
	sc->psy_desc.property_is_writeable = sc8551_charger_is_writeable;
	
	if(sc->fc2_psy==NULL){
		sc->fc2_psy = devm_power_supply_register(sc->dev, 
				&sc->psy_desc, &sc->psy_cfg);
		if (IS_ERR(sc->fc2_psy)) {
			sc_err("Failed to Register fc2_psy:%d\n", ret);
			return PTR_ERR(sc->fc2_psy);
		}
	}
	sc_info("%s power supply register successfully\n", sc->psy_desc.name);

	return 0;
}

static irqreturn_t sc8551_charger_interrupt(int irq, void *dev_id);
static int sc8551_init_irq(struct sc8551 *sc)
{
	int ret;
	
	gpio_free(sc->irq_gpio);
	
	sc_err(">>>>>>>>>>>>%d\n", sc->irq_gpio);
	ret = gpio_request(sc->irq_gpio, "sc8551");
	if (ret < 0) {
		sc_err("fail to request GPIO(%d)   %d\n", sc->irq_gpio, ret);
		return ret;
	}
	
	ret = gpio_direction_input(sc->irq_gpio);
	if (ret < 0) {
		sc_err("fail to set GPIO%d as input pin(%d)\n", sc->irq_gpio, ret);
		return ret;
	}
	
	sc->irq = gpio_to_irq(sc->irq_gpio);
	if (sc->irq <= 0) {
		sc_err("irq mapping fail\n");
		return 0;
	}
	
	sc_info("irq : %d\n",  sc->irq);
	
	if (sc->mode == SC8551_ROLE_MASTER) {
		ret = devm_request_threaded_irq(sc->dev, sc->irq,
			NULL, sc8551_charger_interrupt,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"sc8551 master irq", sc);
		if (ret < 0) {
			sc_err("request irq for irq=%d failed, ret =%d\n",
							sc->irq, ret);
		}
	}
	else if (sc->mode == SC8551_ROLE_SLAVE) {
		ret = devm_request_threaded_irq(sc->dev, sc->irq,
			NULL, sc8551_charger_interrupt,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"sc8551 slave irq", sc);
		if (ret < 0) {
			sc_err("request irq for isrq=%d failed, ret =%d\n",
							sc->irq, ret);
		}
	}
	else {
		ret = devm_request_threaded_irq(sc->dev, sc->irq,
			NULL, sc8551_charger_interrupt,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"sc8551 standalone irq", sc);
		if (ret < 0) {
			sc_err("request irq for irq=%d failed, ret =%d\n",
							sc->irq, ret);
		}
	}
	
	enable_irq_wake(sc->irq);
	
	device_init_wakeup(sc->dev, 1);

	return 0;
}

static void sc8551_check_alarm_status(struct sc8551 *sc)
{
	int ret;
	u8 flag = 0;
	u8 stat = 0;

	mutex_lock(&sc->data_lock);

	ret = sc8551_read_byte(sc, SC8551_REG_08, &flag);
	if (!ret && (flag & SC8551_IBUS_UCP_FALL_FLAG_MASK))
		sc_dbg("UCP_FLAG =0x%02X\n",
			!!(flag & SC8551_IBUS_UCP_FALL_FLAG_MASK));

	ret = sc8551_read_byte(sc, SC8551_REG_2D, &flag);
	if (!ret && (flag & SC8551_VDROP_OVP_FLAG_MASK))
		sc_dbg("VDROP_OVP_FLAG =0x%02X\n",
			!!(flag & SC8551_VDROP_OVP_FLAG_MASK));

	/*read to clear alarm flag*/
	ret = sc8551_read_byte(sc, SC8551_REG_0E, &flag);
	if (!ret && flag)
		sc_dbg("INT_FLAG =0x%02X\n", flag);

	ret = sc8551_read_byte(sc, SC8551_REG_0D, &stat);
	if (!ret && stat != sc->prev_alarm) {
		sc_dbg("INT_STAT = 0X%02x\n", stat);
		sc->prev_alarm = stat;
		sc->bat_ovp_alarm = !!(stat & BAT_OVP_ALARM);
		sc->bat_ocp_alarm = !!(stat & BAT_OCP_ALARM);
		sc->bus_ovp_alarm = !!(stat & BUS_OVP_ALARM);
		sc->bus_ocp_alarm = !!(stat & BUS_OCP_ALARM);
		sc->batt_present  = !!(stat & VBAT_INSERT);
		sc->vbus_present  = !!(stat & VBUS_INSERT);
		sc->bat_ucp_alarm = !!(stat & BAT_UCP_ALARM);
	}


	ret = sc8551_read_byte(sc, SC8551_REG_08, &stat);
	if (!ret && (stat & 0x50))
		sc_err("Reg[05]BUS_UCPOVP = 0x%02X\n", stat);

	ret = sc8551_read_byte(sc, SC8551_REG_0A, &stat);
	if (!ret && (stat & 0x02))
		sc_err("Reg[0A]CONV_OCP = 0x%02X\n", stat);

	mutex_unlock(&sc->data_lock);
}

static void sc8551_check_fault_status(struct sc8551 *sc)
{
	int ret;
	u8 flag = 0;
	u8 stat = 0;
	bool changed = false;

	mutex_lock(&sc->data_lock);
	
	ret = sc8551_read_byte(sc, SC8551_REG_10, &stat);
	if (!ret && stat)
		sc_err("FAULT_STAT = 0x%02X\n", stat);

	ret = sc8551_read_byte(sc, SC8551_REG_11, &flag);
	if (!ret && flag)
		sc_err("FAULT_FLAG = 0x%02X\n", flag);

	if (!ret && flag != sc->prev_fault) {
		changed = true;
		sc->prev_fault = flag;
		sc->bat_ovp_fault = !!(flag & BAT_OVP_FAULT);
		sc->bat_ocp_fault = !!(flag & BAT_OCP_FAULT);
		sc->bus_ovp_fault = !!(flag & BUS_OVP_FAULT);
		sc->bus_ocp_fault = !!(flag & BUS_OCP_FAULT);
		sc->bat_therm_fault = !!(flag & TS_BAT_FAULT);
		sc->bus_therm_fault = !!(flag & TS_BUS_FAULT);

		sc->bat_therm_alarm = !!(flag & TBUS_TBAT_ALARM);
		sc->bus_therm_alarm = !!(flag & TBUS_TBAT_ALARM);
	}

	mutex_unlock(&sc->data_lock);
}

/*static int sc8551_check_reg_status(struct sc8551 *sc)
{
	int ret;
	u8 val;

	ret = sc8551_read_byte(sc, SC8551_REG_2C, &val);
	if (!ret) {
		sc->vbat_reg = !!(val & SC8551_VBAT_REG_ACTIVE_STAT_MASK);
		sc->ibat_reg = !!(val & SC8551_IBAT_REG_ACTIVE_STAT_MASK);
	}


	return ret;
}*/

/*
 * interrupt does nothing, just info event chagne, other module could get info
 * through power supply interface
 */
static irqreturn_t sc8551_charger_interrupt(int irq, void *dev_id)
{
	struct sc8551 *sc = dev_id;

	sc_dbg("INT OCCURED\n");
#if 0
	mutex_lock(&sc->irq_complete);
	sc->irq_waiting = true;
	if (!sc->resume_completed) {
		dev_dbg(sc->dev, "IRQ triggered before device-resume\n");
		if (!sc->irq_disabled) {
			disable_irq_nosync(irq);
			sc->irq_disabled = true;
		}
		mutex_unlock(&sc->irq_complete);
		return IRQ_HANDLED;
	}
	sc->irq_waiting = false;
#if 0
	/* TODO */
	sc8551_check_alarm_status(sc);
	sc8551_check_fault_status(sc);
#endif

#if 0
	sc8551_dump_reg(sc);
#endif
	mutex_unlock(&sc->irq_complete);
#endif
	power_supply_changed(sc->fc2_psy);

	return IRQ_HANDLED;
}

static void determine_initial_status(struct sc8551 *sc)
{
	if (sc->client->irq)
		sc8551_charger_interrupt(sc->client->irq, sc);
}


static struct of_device_id sc8551_charger_match_table[] = {
	{
		.compatible = "sc,sc8551-standalone",
		.data = &sc8551_mode_data[SC8551_STDALONE],
	},
	{},
};

static int sc8551_charger_probe(struct i2c_client *client,
					const struct i2c_device_id *id)
{
	struct sc8551 *sc;
	const struct of_device_id *match;
	struct device_node *node = client->dev.of_node;
	int ret;

	sc = devm_kzalloc(&client->dev, sizeof(struct sc8551), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;

	sc->dev = &client->dev;

	sc->client = client;
	

	mutex_init(&sc->i2c_rw_lock);
	mutex_init(&sc->data_lock);
	mutex_init(&sc->charging_disable_lock);
	mutex_init(&sc->irq_complete);

	sc->resume_completed = true;
	sc->irq_waiting = false;
	sc->is_sc8551 = true;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)){
		sc_err("i2c_check_functionality fail!\n");
		return -EINVAL;
	}
	ret = sc8551_detect_device(sc);
	if (ret) {
		sc_err("No sc8551 device found!\n");
		return -ENODEV;
	}
	
	if (sc->part_no != SC8551_DEVICE_ID) {
		sc_err("detect sc8551_2 device fail, sc->part_no = %d!\n", sc->part_no);
		return -ENODEV;
	}

	i2c_set_clientdata(client, sc);
	sc8551_create_device_node(&(client->dev));

	match = of_match_node(sc8551_charger_match_table, node);
	if (match == NULL) {
		sc_err("device tree match not found!\n");
		return -ENODEV;
	}

	sc8551_get_work_mode(sc, &sc->mode);

	if (sc->mode !=  *(int *)match->data) {
		sc_err("device operation mode mismatch with dts configuration\n");
		return -EINVAL;
	}

	ret = sc8551_parse_dt(sc, &client->dev);
	if (ret)
		return -EIO;

	ret = sc8551_init_device(sc);
	if (ret) {
		sc_err("Failed to init device\n");
		return ret;
	}

	ret = sc8551_psy_register(sc);
	if (ret)
		goto err_1;

	ret = sc8551_init_irq(sc);
	if (ret)
		return ret;

	determine_initial_status(sc);

	sc_info("sc8551 probe successfully, Part Num:%d\n!",
				sc->part_no);

	hardwareinfo_set_prop(HARDWARE_SUB_CHARGER_MASTER_IC_INFO, "SC8551_CHARGER_MASTER");
	return 0;

err_1:
	power_supply_unregister(sc->fc2_psy);
	return ret;
}

static inline bool is_device_suspended(struct sc8551 *sc)
{
	return !sc->resume_completed;
}

static int sc8551_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sc8551 *sc = i2c_get_clientdata(client);
	enum charger_type chgType = CHARGER_UNKNOWN;
	u8 regVal = (SC8551_ADC_DISABLE << SC8551_ADC_EN_SHIFT);
	sc->resume_completed = false;
	
	sc_err("%s %d: regVal = %#x\n", __func__, __LINE__,regVal);
	chgType = mt_get_charger_type();
	if (chgType == CHARGER_UNKNOWN)
		sc8551_update_bits(sc, SC8551_REG_14, SC8551_ADC_EN_MASK, regVal);

	sc_err("%s %d: \n", __func__, __LINE__);
	return 0;
}

static int sc8551_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sc8551 *sc = i2c_get_clientdata(client);
	enum charger_type chgType = CHARGER_UNKNOWN;
	u8 regVal = (SC8551_ADC_ENABLE << SC8551_ADC_EN_SHIFT);

	sc->resume_completed = true;

	sc_err("%s %d: regVal = %#x\n", __func__, __LINE__,regVal);
	chgType = mt_get_charger_type();
	if (chgType == CHARGER_UNKNOWN)
		sc8551_update_bits(sc, SC8551_REG_14, SC8551_ADC_EN_MASK, regVal);

	sc_err("%s %d: \n", __func__, __LINE__);
	return 0;
}
static int sc8551_charger_remove(struct i2c_client *client)
{
	struct sc8551 *sc = i2c_get_clientdata(client);


	sc8551_enable_adc(sc, false);

	power_supply_unregister(sc->fc2_psy);

	mutex_destroy(&sc->charging_disable_lock);
	mutex_destroy(&sc->data_lock);
	mutex_destroy(&sc->i2c_rw_lock);
	mutex_destroy(&sc->irq_complete);

	return 0;
}


static void sc8551_charger_shutdown(struct i2c_client *client)
{
	struct sc8551 *sc = i2c_get_clientdata(client);

	sc8551_enable_adc(sc, false);
}

static const struct dev_pm_ops sc8551_pm_ops = {
	.resume		= sc8551_resume,
//	.suspend_noirq = sc8551_suspend_noirq,
	.suspend	= sc8551_suspend,
};

static const struct i2c_device_id sc8551_charger_id[] = {
	{"sc8551-standalone", SC8551_ROLE_STDALONE},
	{},
};
	
static struct i2c_driver sc8551_charger_driver = {
	.driver		= {
		.name	= "sc8551-charger",
		.owner	= THIS_MODULE,
		.of_match_table = sc8551_charger_match_table,
		.pm	= &sc8551_pm_ops,
	},
	.id_table	= sc8551_charger_id,

	.probe		= sc8551_charger_probe,
	.remove		= sc8551_charger_remove,
	.shutdown	= sc8551_charger_shutdown,
};

module_i2c_driver(sc8551_charger_driver);

MODULE_DESCRIPTION("SC SC8551 Charge Pump Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aiden-yu@southchip.com");

