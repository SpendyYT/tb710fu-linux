// SPDX-License-Identifier: GPL-2.0-only
//
// aw88461.c  --  AW88461 ALSA SoC Audio driver
//
// Copyright (c) 2023 awinic Technology CO., LTD
//
// Author: Jimmy Zhang <zhangjianming@awinic.com>
// Author: Weidong Wang <wangweidong.a@awinic.com>
//

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/i2c.h>
#include <linux/firmware.h>
#include <linux/bitops.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <linux/gpio/consumer.h>
#include "aw88461.h"
#include "aw88395/aw88395_data_type.h"
#include "aw88395/aw88395_device.h"

static const struct regmap_config aw88461_remap_config = {
	.val_bits = 16,
	.reg_bits = 8,
	.max_register = AW88461_REG_MAX,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_format_endian = REGMAP_ENDIAN_BIG,
};

static void aw88461_dev_set_volume(struct aw_device *aw_dev, unsigned int value)
{
	unsigned int volume = min(value, (unsigned int)AW88461_MUTE_VOL);

	regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL2_REG,
		~AW88461_VOL_MASK, DB_TO_REG_VAL(volume));
}

static void aw88461_dev_i2s_tx_enable(struct aw_device *aw_dev, bool flag)
{
	if (flag)
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
			~AW88461_I2STXEN_MASK, AW88461_I2STXEN_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
			~AW88461_I2STXEN_MASK, AW88461_I2STXEN_DISABLE_VALUE);
}

static void aw88461_dev_pwd(struct aw_device *aw_dev, bool pwd)
{
	if (pwd)
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_PWDN_MASK, AW88461_PWDN_POWER_DOWN_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_PWDN_MASK, AW88461_PWDN_WORKING_VALUE);
}

static void aw88461_dev_amppd(struct aw_device *aw_dev, bool amppd)
{
	if (amppd)
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_AMPPD_MASK, AW88461_AMPPD_POWER_DOWN_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_AMPPD_MASK, AW88461_AMPPD_WORKING_VALUE);
}

static void aw88461_dev_mute(struct aw_device *aw_dev, bool is_mute)
{
	if (is_mute) {
		aw88461_dev_set_volume(aw_dev, AW88461_MUTE_VOL);
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_HMUTE_MASK, AW88461_HMUTE_ENABLE_VALUE);
	} else {
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_HMUTE_MASK, AW88461_HMUTE_DISABLE_VALUE);
		aw88461_dev_set_volume(aw_dev, aw_dev->volume_desc.ctl_volume);
	}
}

static void aw88461_set_psm(struct aw_device *aw_dev, bool psm)
{
	if (psm)
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL2_REG,
				~AW88461_PSM_EN_MASK, AW88461_PSM_EN_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL2_REG,
				~AW88461_PSM_EN_MASK, AW88461_PSM_EN_DISABLE_VALUE);
}

static void aw88461_set_mpd(struct aw_device *aw_dev, bool mpd)
{
	if (mpd)
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL2_REG,
				~AW88461_EN_MPD_MASK, AW88461_EN_MPD_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL2_REG,
				~AW88461_EN_MPD_MASK, AW88461_EN_MPD_DISABLE_VALUE);
}

static void aw88461_set_dsmzth(struct aw_device *aw_dev, bool dsmzth)
{
        if (dsmzth)
                regmap_update_bits(aw_dev->regmap, AW88461_NGCTRL3_REG,
                                ~AW88461_DSMZTH_MASK, AW88461_DSMZTH_21P33MS_VALUE);
        else
                regmap_update_bits(aw_dev->regmap, AW88461_NGCTRL3_REG,
                                ~AW88461_DSMZTH_MASK, AW88461_DSMZTH_NO_RESET_VALUE);
}

static void aw88461_forbidden_output(struct aw_device *aw_dev, bool power_waste)
{
	aw88461_set_psm(aw_dev, power_waste);
	aw88461_set_mpd(aw_dev, power_waste);
	aw88461_set_dsmzth(aw_dev, power_waste);
}

static void aw88461_dev_clear_int_status(struct aw_device *aw_dev)
{
	unsigned int int_status;

	/* read int status and clear */
	regmap_read(aw_dev->regmap, AW88461_SYSINT_REG, &int_status);
	/* make sure int status is clear */
	regmap_read(aw_dev->regmap, AW88461_SYSINT_REG, &int_status);

	dev_dbg(aw_dev->dev, "read interrupt reg = 0x%04x", int_status);
}

static int aw88461_dev_get_iis_status(struct aw_device *aw_dev)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(aw_dev->regmap, AW88461_SYSST_REG, &reg_val);
	if (ret)
		return ret;
	if ((reg_val & AW88461_BIT_PLL_CHECK) != AW88461_BIT_PLL_CHECK) {
		dev_dbg(aw_dev->dev, "check pll lock fail,reg_val:0x%04x", reg_val);
		return -EINVAL;
	}

	return ret;
}

static bool aw88461_active(struct aw88461 *aw88461)
{
	return READ_ONCE(aw88461->enabled) && READ_ONCE(aw88461->running);
}

/*
 * Dump the registers that matter for clock/PLL/I2S problems.
 * is_err = true prints at error level (always visible), otherwise dev_dbg:
 *   echo 'file aw88461.c +p' > /sys/kernel/debug/dynamic_debug/control
 * Note: SYSINT is not read here, reading it clears the interrupt status.
 */
static void aw88461_dump_regs(struct aw_device *aw_dev, const char *tag, bool is_err)
{
	static const u8 regs[] = {
		AW88461_SYSST_REG, AW88461_SYSCTRL_REG, AW88461_SYSCTRL2_REG,
		AW88461_I2SCTRL1_REG, AW88461_I2SCTRL2_REG,
		AW88461_PLLCTRL1_REG, AW88461_NGCTRL3_REG,
	};
	unsigned int v[ARRAY_SIZE(regs)];
	char buf[192];
	int i;

	for (i = 0; i < ARRAY_SIZE(regs); i++)
		if (regmap_read(aw_dev->regmap, regs[i], &v[i]))
			v[i] = 0xdead;

	scnprintf(buf, sizeof(buf),
		  "SYSST=0x%04x[NOCLKS=%u CLKS=%u PLLS=%u] SYSCTRL=0x%04x SYSCTRL2=0x%04x I2SCTRL1=0x%04x I2SCTRL2=0x%04x PLLCTRL1=0x%04x NGCTRL3=0x%04x",
		  v[0], (v[0] >> AW88461_NOCLKS_START_BIT) & 1,
		  (v[0] >> AW88461_CLKS_START_BIT) & 1,
		  (v[0] >> AW88461_PLLS_START_BIT) & 1,
		  v[1], v[2], v[3], v[4], v[5], v[6]);

	if (is_err)
		dev_err(aw_dev->dev, "[%s] %s\n", tag, buf);
	else
		dev_dbg(aw_dev->dev, "[%s] %s\n", tag, buf);
}

static int aw88461_dev_check_pll(struct aw88461 *aw88461)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	int ret, i;

	for (i = 0; i < AW88461_DEV_PLL_CHECK_MAX; i++) {
		/* stream stopped while waiting, no point to continue */
		if (!aw88461_active(aw88461)) {
			dev_dbg(aw_dev->dev,
				"pll wait cancelled after %d polls (enabled=%d running=%d)\n",
				i, READ_ONCE(aw88461->enabled),
				READ_ONCE(aw88461->running));
			return -ECANCELED;
		}

		ret = aw88461_dev_get_iis_status(aw_dev);
		if (!ret) {
			dev_dbg(aw_dev->dev, "pll locked after %d polls (~%d ms)\n",
				i, i * 2);
			return 0;
		}

		usleep_range(AW88461_2000_US, AW88461_2000_US + 10);
	}

	dev_err(aw_dev->dev,
		"PLL not locked after %d polls (~%d ms), no BCK/WCK? enabled=%d running=%d\n",
		i, i * 2, READ_ONCE(aw88461->enabled), READ_ONCE(aw88461->running));
	aw88461_dump_regs(aw_dev, "pll fail", true);

	return -EPERM;
}

static int aw88461_dev_configure_syspll(struct aw88461 *aw88461)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	int ret;

	/* Configure TDM slots (I2S is represented as no slots) */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL2_REG,
			~AW88461_SLOT_NUM_MASK, aw88461->slot_num_value);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL2_REG,
			~AW88461_I2S_TX_SLOTVLD_MASK,
			aw88461->tx_slotvld_mask);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL2_REG,
			~AW88461_I2S_RXL_SLOTVLD_MASK,
			aw88461->rxl_slotvld_mask);
	if (ret)
		return ret;

	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL2_REG,
			~AW88461_I2S_RXR_SLOTVLD_MASK,
			aw88461->rxr_slotvld_mask);
	if (ret)
		return ret;

	/* PLL divider must be used for 8/16/32 kHz modes */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_PLLCTRL1_REG,
			~AW88461_CCO_MUX_MASK, aw88461->cco_mux_value);
	if (ret)
		return ret;

	/* The word clock (WCK) defines the beginning of a frame */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL1_REG,
			~AW88461_I2SSR_MASK, aw88461->sr_value);
	if (ret)
		return ret;

	/* The bit clock (BCK) defines the length of a frame */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL1_REG,
			~AW88461_I2SBCK_MASK,
			(aw88461->tdm_bck_value != AW88461_TDM_BCK_UNSET)
			? aw88461->tdm_bck_value : aw88461->bck_value);
	if (ret)
		return ret;

	/* The logical frame size is the width of data for 1 slot */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL1_REG,
			~AW88461_I2SFS_MASK, aw88461->fs_value);
	if (ret)
		return ret;

	/* The I2S interface mode (Philips standard, LSB/MSB justified) */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_I2SCTRL1_REG,
			~AW88461_I2SMD_MASK, aw88461->md_value);
	if (ret)
		return ret;

	/* The polarity of the bit clock (BCK) */
	ret = regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
			~AW88461_BCKINV_MASK, aw88461->bck_inv_value);
	if (ret)
		return ret;

	return aw88461_dev_check_pll(aw88461);
}

static void aw88461_set_noise_gate(struct aw_device *aw_dev, bool enable)
{
	if (enable)
		regmap_update_bits(aw_dev->regmap,
				AW88461_NGCTRL3_REG,
				~AW88461_NOISE_GATE_EN_MASK,
				AW88461_NOISE_GATE_EN_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap,
				AW88461_NGCTRL3_REG,
				~AW88461_NOISE_GATE_EN_MASK,
				AW88461_NOISE_GATE_EN_DISABLE_VALUE);
}

static int aw88461_dev_check_sysst(struct aw_device *aw_dev)
{
	unsigned int check_val;
	unsigned int reg_val;
	int ret, i;

	for (i = 0; i < AW88461_DEV_SYSST_CHECK_MAX; i++) {
		ret = regmap_read(aw_dev->regmap, AW88461_SYSST_REG, &reg_val);
		if (ret)
			return ret;

		check_val = reg_val & (~AW88461_BIT_SYSST_CHECK_MASK)
							& AW88461_BIT_PLL_CHECK;
		if (check_val != AW88461_BIT_PLL_CHECK) {
			dev_dbg(aw_dev->dev,
				"check sysst fail, reg_val=0x%04x, check:0x%x",
				reg_val, AW88461_BIT_PLL_CHECK);
			usleep_range(AW88461_2000_US, AW88461_2000_US + 10);
		} else {
			dev_dbg(aw_dev->dev, "sysst ok after %d polls\n", i);
			return 0;
		}
	}

	dev_err(aw_dev->dev, "SYSST check failed after %d polls\n", i);
	aw88461_dump_regs(aw_dev, "sysst fail", true);

	return -EPERM;
}

static void aw88461_dev_uls_hmute(struct aw_device *aw_dev, bool uls_hmute)
{
	if (uls_hmute)
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_ULS_HMUTE_MASK,
				AW88461_ULS_HMUTE_ENABLE_VALUE);
	else
		regmap_update_bits(aw_dev->regmap, AW88461_SYSCTRL_REG,
				~AW88461_ULS_HMUTE_MASK,
				AW88461_ULS_HMUTE_DISABLE_VALUE);
}

static int aw88461_dev_get_icalk(struct aw_device *aw_dev, int16_t *icalk)
{
	// Disabled
	*icalk = 0;

	return 0;
}

// Disabled
static int aw88461_dev_get_vcalk(struct aw_device *aw_dev, int16_t *vcalk)
{
	*vcalk = 0;

	return 0;
}

static int aw88461_dev_reg_update(struct aw88461 *aw88461,
					unsigned char *data, unsigned int len)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	struct aw_volume_desc *vol_desc = &aw_dev->volume_desc;
	unsigned int read_val, read_vol;
	int data_len, i, ret;
	int16_t *reg_data;
	u16 reg_val;
	u8 reg_addr;

	if (!len || !data) {
		dev_err(aw_dev->dev, "reg data is null or len is 0");
		return -EINVAL;
	}

	reg_data = (int16_t *)data;
	data_len = len >> 1;

	if (data_len & 0x1) {
		dev_err(aw_dev->dev, "data len:%d unsupported",	data_len);
		return -EINVAL;
	}

	for (i = 0; i < data_len; i += 2) {
	    if ((u16)reg_data[i] == 0xff)
		   continue;

		reg_addr = reg_data[i];
		reg_val = reg_data[i + 1];

		if (reg_addr == AW88461_SYSCTRL_REG) {
			aw88461->amppd_st = reg_val & (~AW88461_AMPPD_MASK);
			ret = regmap_read(aw_dev->regmap, reg_addr, &read_val);
			if (ret) {
		            dev_err(aw_dev->dev,
	                	    "regmap_write failed: reg=0x%02x val=0x%04x ret=%d\n",
        	            	    reg_addr, reg_val, ret);
            		    break;
        		}

			/* keep all three bits from current hw status */
			read_val &= (~AW88461_AMPPD_MASK) | (~AW88461_PWDN_MASK) |
								(~AW88461_HMUTE_MASK);
			reg_val &= (AW88461_AMPPD_MASK & AW88461_PWDN_MASK & AW88461_HMUTE_MASK);
			reg_val |= read_val;

			/* enable uls hmute */
			reg_val &= AW88461_ULS_HMUTE_MASK;
			reg_val |= AW88461_ULS_HMUTE_ENABLE_VALUE;
		}

		/* i2stxen */
		if (reg_addr == AW88461_SYSCTRL_REG) {
			/* close tx */
			reg_val &= AW88461_I2STXEN_MASK;
			reg_val |= AW88461_I2STXEN_DISABLE_VALUE;
		}

		if (reg_addr == AW88461_SYSCTRL2_REG) {
			read_vol = (reg_val & (~AW88461_VOL_MASK)) >>
				AW88461_VOL_START_BIT;
			aw_dev->volume_desc.init_volume =
				REG_VAL_TO_DB(read_vol);
		}

		if (reg_addr == AW88461_VSNTM1_REG)
			continue;

		ret = regmap_write(aw_dev->regmap, reg_addr, reg_val);
		if (ret) {
		   dev_err(aw_dev->dev,
			       "write failed: raw_addr=0x%04x reg=0x%02x val=0x%04x ret=%d (pair %d of %d)\n",
			        (u16)reg_data[i], reg_addr, reg_val, ret, i / 2, data_len / 2);
		   break;
	    }
	}

	if (aw_dev->prof_cur != aw_dev->prof_index)
		vol_desc->ctl_volume = 0;

	/* keep min volume */
	aw88461_dev_set_volume(aw_dev, vol_desc->mute_volume);

	return ret;
}

static int aw88461_dev_get_prof_name(struct aw_device *aw_dev, int index, char **prof_name)
{
	struct aw_prof_info *prof_info = &aw_dev->prof_info;
	struct aw_prof_desc *prof_desc;

	if ((index >= aw_dev->prof_info.count) || (index < 0)) {
		dev_err(aw_dev->dev, "index[%d] overflow count[%d]",
			index, aw_dev->prof_info.count);
		return -EINVAL;
	}

	prof_desc = &aw_dev->prof_info.prof_desc[index];

	*prof_name = prof_info->prof_name_list[prof_desc->id];

	return 0;
}

static int aw88461_dev_get_prof_data(struct aw_device *aw_dev, int index,
			struct aw_prof_desc **prof_desc)
{
	if ((index >= aw_dev->prof_info.count) || (index < 0)) {
		dev_err(aw_dev->dev, "%s: index[%d] overflow count[%d]\n",
				__func__, index, aw_dev->prof_info.count);
		return -EINVAL;
	}

	*prof_desc = &aw_dev->prof_info.prof_desc[index];

	return 0;
}

static int aw88461_dev_fw_update(struct aw88461 *aw88461)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	struct aw_prof_desc *prof_index_desc;
	struct aw_sec_data_desc *sec_desc;
	char *prof_name;
	int ret;

	ret = aw88461_dev_get_prof_name(aw_dev, aw_dev->prof_index, &prof_name);
	if (ret) {
		dev_err(aw_dev->dev, "get prof name failed");
		return -EINVAL;
	}

	dev_dbg(aw_dev->dev, "start update %s", prof_name);

	ret = aw88461_dev_get_prof_data(aw_dev, aw_dev->prof_index, &prof_index_desc);
	if (ret)
		return ret;

	/* update reg */
	sec_desc = prof_index_desc->sec_desc;
	ret = aw88461_dev_reg_update(aw88461, sec_desc[AW88395_DATA_TYPE_REG].data,
					sec_desc[AW88395_DATA_TYPE_REG].len);
	if (ret) {
		dev_err(aw_dev->dev, "update reg failed");
		return ret;
	}

	aw_dev->prof_cur = aw_dev->prof_index;

	return ret;
}

static int aw88461_dev_start(struct aw88461 *aw88461)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	int ret;

	if (aw_dev->status == AW88461_DEV_PW_ON) {
		dev_dbg(aw_dev->dev, "already power on");
		return 0;
	}

	dev_dbg(aw_dev->dev,
		"dev_start: slot_num=0x%04x sr=0x%04x cco=0x%04x fs=0x%04x bck=0x%04x tdm_bck=0x%x md=0x%04x bck_inv=0x%04x tx_vld=0x%04x rxl_vld=0x%04x rxr_vld=0x%04x\n",
		aw88461->slot_num_value, aw88461->sr_value, aw88461->cco_mux_value,
		aw88461->fs_value, aw88461->bck_value, aw88461->tdm_bck_value,
		aw88461->md_value, aw88461->bck_inv_value, aw88461->tx_slotvld_mask,
		aw88461->rxl_slotvld_mask, aw88461->rxr_slotvld_mask);
	aw88461_dump_regs(aw_dev, "before power on", false);

	/* power on */
	aw88461_dev_pwd(aw_dev, false);
	usleep_range(AW88461_2000_US, AW88461_2000_US + 10);

	ret = aw88461_dev_configure_syspll(aw88461);
	if (ret) {
		dev_dbg(aw_dev->dev, "pll configure/check failed, ret=%d\n", ret);
		goto pll_check_fail;
	}
	aw88461_dump_regs(aw_dev, "pll ok", false);

	/* amppd on */
	aw88461_dev_amppd(aw_dev, false);
	usleep_range(AW88461_1000_US, AW88461_1000_US + 50);

	/* check i2s status */
	ret = aw88461_dev_check_sysst(aw_dev);
	if (ret) {
		dev_dbg(aw_dev->dev, "sysst check failed, ret=%d\n", ret);
		goto sysst_check_fail;
	}

	/* enable tx feedback */
	aw88461_dev_i2s_tx_enable(aw_dev, true);

	if (aw88461->amppd_st)
		aw88461_dev_amppd(aw_dev, true);

	/* close uls mute */
	aw88461_dev_uls_hmute(aw_dev, false);

	/* close mute */
	if (!aw88461->mute_st)
		aw88461_dev_mute(aw_dev, false);

	/* clear inturrupt */
	aw88461_dev_clear_int_status(aw_dev);
	aw_dev->status = AW88461_DEV_PW_ON;
	dev_dbg(aw_dev->dev, "dev_start: powered on (mute_st=%u amppd_st=%u)\n",
		aw88461->mute_st, aw88461->amppd_st);
	aw88461_dump_regs(aw_dev, "started", false);

	return 0;

sysst_check_fail:
	aw88461_dev_i2s_tx_enable(aw_dev, false);
	aw88461_dev_clear_int_status(aw_dev);
	aw88461_dev_amppd(aw_dev, true);
pll_check_fail:
	aw88461_dev_pwd(aw_dev, true);
	aw_dev->status = AW88461_DEV_PW_OFF;

	return ret;
}

static int aw88461_dev_stop(struct aw_device *aw_dev)
{
	if (aw_dev->status == AW88461_DEV_PW_OFF) {
		dev_dbg(aw_dev->dev, "already power off");
		return 0;
	}

	dev_dbg(aw_dev->dev, "dev_stop\n");
	aw88461_dump_regs(aw_dev, "before stop", false);
	aw_dev->status = AW88461_DEV_PW_OFF;

	/* clear inturrupt */
	aw88461_dev_clear_int_status(aw_dev);

	aw88461_dev_uls_hmute(aw_dev, true);
	/* set mute */
	aw88461_dev_mute(aw_dev, true);

	/* close tx feedback */
	aw88461_dev_i2s_tx_enable(aw_dev, false);
	usleep_range(AW88461_1000_US, AW88461_1000_US + 100);

	/* enable amppd */
	aw88461_dev_amppd(aw_dev, true);

	/* set power down */
	aw88461_dev_pwd(aw_dev, true);

	return 0;
}

static int aw88461_reg_update(struct aw88461 *aw88461, bool force)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	int ret;

	if (force) {
		ret = regmap_write(aw_dev->regmap,
					AW88461_ID_REG, AW88461_SOFT_RESET_VALUE);
		if (ret)
			return ret;

		ret = aw88461_dev_fw_update(aw88461);
		if (ret)
			return ret;
	} else {
		if (aw_dev->prof_cur != aw_dev->prof_index) {
			ret = aw88461_dev_fw_update(aw88461);
			if (ret)
				return ret;
		} else {
			ret = 0;
		}
	}

	aw_dev->prof_cur = aw_dev->prof_index;

	return ret;
}

static void aw88461_start_pa(struct aw88461 *aw88461)
{
	int ret, i;

	for (i = 0; i < AW88461_START_RETRIES; i++) {
		ret = aw88461_reg_update(aw88461, aw88461->phase_sync);
		if (ret) {
			dev_err(aw88461->aw_pa->dev,
				"aw88461_reg_update failed, cnt:%d, ret:%d\n", i, ret);
			continue;
		}
		ret = aw88461_dev_start(aw88461);
		if (ret == -ECANCELED) {
			/* stream was stopped meanwhile, not an error */
			dev_dbg(aw88461->aw_pa->dev, "start cancelled\n");
			return;
		}
		if (ret) {
			dev_dbg(aw88461->aw_pa->dev,
				"aw88461_dev_start failed, cnt:%d, ret:%d\n", i, ret);
			msleep(10);
			continue;
		} else {
			dev_dbg(aw88461->aw_pa->dev, "start success\n");
			break;
		}
	}
	if (ret != 0)
		dev_err(aw88461->aw_pa->dev, "start failure (%d) after %d attempts\n",
			ret, AW88461_START_RETRIES);
}

static void aw88461_start(struct aw88461 *aw88461)
{
	struct aw_device *aw_dev = aw88461->aw_pa;

	if (!aw88461_active(aw88461)) {
		dev_dbg(aw_dev->dev, "start skipped: not active (enabled=%d running=%d)\n",
			READ_ONCE(aw88461->enabled), READ_ONCE(aw88461->running));
		return;
	}

	if (aw_dev->fw_status != AW88461_DEV_FW_OK) {
		dev_err(aw_dev->dev, "start skipped: firmware not loaded (fw_status=%d)\n",
			aw_dev->fw_status);
		return;
	}

	if (aw_dev->status == AW88461_DEV_PW_ON) {
		dev_dbg(aw_dev->dev, "start skipped: already on\n");
		return;
	}

	dev_dbg(aw_dev->dev, "start: profile cur=%d index=%d phase_sync=%d\n",
		aw_dev->prof_cur, aw_dev->prof_index, aw88461->phase_sync);

	aw88461_start_pa(aw88461);
}

static int aw88461_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(component);

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		aw88461->bck_inv_value = AW88461_BCKINV_NOT_INVERT_VALUE;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		aw88461->bck_inv_value = AW88461_BCKINV_INVERTED_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported invert mode 0x%x\n",
			fmt & SND_SOC_DAIFMT_INV_MASK);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_DSP_A:
		aw88461->md_value = AW88461_I2SMD_PHILIPS_STANDARD_VALUE;
		break;
	case SND_SOC_DAIFMT_MSB:
	case SND_SOC_DAIFMT_DSP_B:
		aw88461->md_value = AW88461_I2SMD_MSB_JUSTIFIED_VALUE;
		break;
	case SND_SOC_DAIFMT_LSB:
		aw88461->md_value = AW88461_I2SMD_LSB_JUSTIFIED_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported DAI format 0x%x\n",
			fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	dev_dbg(aw88461->aw_pa->dev,
		"set_fmt: fmt=0x%x md=0x%04x bck_inv=0x%04x (provider mask 0x%x not checked)\n",
		fmt, aw88461->md_value, aw88461->bck_inv_value,
		fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK);

	return 0;
}

static int aw88461_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params,
	struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return 0;

	aw88461->cco_mux_value = AW88461_CCO_MUX_BYPASS_VALUE;
	switch (params_rate(params)) {
	case 8000:
		aw88461->sr_value = AW88461_I2SSR_8KHZ_VALUE;
		aw88461->cco_mux_value = AW88461_CCO_MUX_DIVIDED_VALUE;
		break;
	case 11025:
		aw88461->sr_value = AW88461_I2SSR_11P025KHZ_VALUE;
		break;
	case 12000:
		aw88461->sr_value = AW88461_I2SSR_12KHZ_VALUE;
		break;
	case 16000:
		aw88461->sr_value = AW88461_I2SSR_16KHZ_VALUE;
		aw88461->cco_mux_value = AW88461_CCO_MUX_DIVIDED_VALUE;
		break;
	case 22050:
		aw88461->sr_value = AW88461_I2SSR_22P05KHZ_VALUE;
		break;
	case 24000:
		aw88461->sr_value = AW88461_I2SSR_24KHZ_VALUE;
		break;
	case 32000:
		aw88461->sr_value = AW88461_I2SSR_32KHZ_VALUE;
		aw88461->cco_mux_value = AW88461_CCO_MUX_DIVIDED_VALUE;
		break;
	case 44100:
		aw88461->sr_value = AW88461_I2SSR_44P1KHZ_VALUE;
		break;
	case 48000:
		aw88461->sr_value = AW88461_I2SSR_48KHZ_VALUE;
		break;
	case 96000:
		aw88461->sr_value = AW88461_I2SSR_96KHZ_VALUE;
		break;
	case 192000:
		aw88461->sr_value = AW88461_I2SSR_192KHZ_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported sample rate %d\n",
			params_rate(params));
		return -EINVAL;
	}

	switch (params_width(params)) {
	case 16:
		aw88461->fs_value = AW88461_I2SFS_16_BITS_VALUE;
		break;
	case 20:
		aw88461->fs_value = AW88461_I2SFS_20_BITS_VALUE;
		break;
	case 24:
		aw88461->fs_value = AW88461_I2SFS_24_BITS_VALUE;
		break;
	case 32:
		aw88461->fs_value = AW88461_I2SFS_32_BITS_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported bit width %d\n",
			params_width(params));
		return -EINVAL;
	}

	switch (params_physical_width(params)) {
	case 16:
		aw88461->bck_value = AW88461_I2SBCK_32FS_VALUE;
		break;
	case 24:
		aw88461->bck_value = AW88461_I2SBCK_48FS_VALUE;
		break;
	case 32:
		aw88461->bck_value = AW88461_I2SBCK_64FS_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported physical bit width %d\n",
			params_physical_width(params));
		return -EINVAL;
	}

	dev_dbg(aw88461->aw_pa->dev,
		"hw_params: rate=%u ch=%u width=%d phys_width=%d -> sr=0x%04x cco=0x%04x fs=0x%04x bck=0x%04x (tdm_bck=0x%x overrides if set)\n",
		params_rate(params), params_channels(params),
		params_width(params), params_physical_width(params),
		aw88461->sr_value, aw88461->cco_mux_value,
		aw88461->fs_value, aw88461->bck_value, aw88461->tdm_bck_value);

	return 0;
}

static int aw88461_set_tdm_slot(struct snd_soc_dai *dai,
	unsigned int tx_mask, unsigned int rx_mask, int slots, int slot_width)
{
	struct snd_soc_component *component = dai->component;
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(component);
	int chan;

	dev_dbg(aw88461->aw_pa->dev,
		"set_tdm_slot: tx_mask=0x%x rx_mask=0x%x slots=%d slot_width=%d\n",
		tx_mask, rx_mask, slots, slot_width);

	switch (slots) {
	case 0:
		/* Just reset everything TDM related to I2S values */
		aw88461->slot_num_value = AW88461_SLOT_NUM_I2S_MODE_VALUE;
		aw88461->tdm_bck_value = AW88461_TDM_BCK_UNSET;
		aw88461->tx_slotvld_mask = 0 << AW88461_I2S_TX_SLOTVLD_START_BIT;
		aw88461->rxl_slotvld_mask = 0 << AW88461_I2S_RXL_SLOTVLD_START_BIT;
		aw88461->rxr_slotvld_mask = 1 << AW88461_I2S_RXR_SLOTVLD_START_BIT;
		return 0;
	case 1:
		aw88461->slot_num_value = AW88461_SLOT_NUM_TDM1S_VALUE;
		break;
	case 2:
		aw88461->slot_num_value = AW88461_SLOT_NUM_TDM2S_VALUE;
		break;
	case 4:
		aw88461->slot_num_value = AW88461_SLOT_NUM_TDM4S_VALUE;
		break;
	case 6:
		aw88461->slot_num_value = AW88461_SLOT_NUM_TDM6S_VALUE;
		break;
	case 8:
		aw88461->slot_num_value = AW88461_SLOT_NUM_TDM8S_VALUE;
		break;
	case 16:
		aw88461->slot_num_value = AW88461_SLOT_NUM_TDM16S_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported slot count %d\n", slots);
		return -EINVAL;
	}

	switch (slot_width) {
	case 16:
		aw88461->tdm_bck_value = AW88461_I2SBCK_32FS_VALUE;
		break;
	case 20:
	case 24:
		aw88461->tdm_bck_value = AW88461_I2SBCK_48FS_VALUE;
		break;
	case 32:
		aw88461->tdm_bck_value = AW88461_I2SBCK_64FS_VALUE;
		break;
	default:
		dev_err(aw88461->aw_pa->dev, "unsupported slot width %d\n",
			slot_width);
		return -EINVAL;
	}

	/* slot valid fields are 4 bits wide: 0..15 */
	if (tx_mask != 0) {
		chan = __ffs(tx_mask);
		if (chan > 15)
			return -EINVAL;

		aw88461->tx_slotvld_mask = chan << AW88461_I2S_TX_SLOTVLD_START_BIT;
	}

	if (rx_mask != 0) {
		chan = __ffs(rx_mask);
		if (chan > 15)
			return -EINVAL;

		aw88461->rxl_slotvld_mask = chan << AW88461_I2S_RXL_SLOTVLD_START_BIT;

		rx_mask &= ~BIT(chan);
		if (rx_mask != 0) {
			chan = __ffs(rx_mask);
			if (chan > 15)
				return -EINVAL;

			aw88461->rxr_slotvld_mask = chan << AW88461_I2S_RXR_SLOTVLD_START_BIT;
		}
	}

	dev_dbg(aw88461->aw_pa->dev,
		"set_tdm_slot: slot_num=0x%04x tdm_bck=0x%x tx_vld=0x%04x rxl_vld=0x%04x rxr_vld=0x%04x\n",
		aw88461->slot_num_value, aw88461->tdm_bck_value, aw88461->tx_slotvld_mask,
		aw88461->rxl_slotvld_mask, aw88461->rxr_slotvld_mask);

	return 0;
}

static int aw88461_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(component);

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		WRITE_ONCE(aw88461->running, true);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(aw88461->running, false);
		break;
	default:
		return -EINVAL;
	}

	/* trigger may run in atomic context: only atomic-safe logging here */
	dev_dbg(aw88461->aw_pa->dev, "trigger: cmd=%d -> enabled=%d running=%d\n",
		cmd, READ_ONCE(aw88461->enabled), READ_ONCE(aw88461->running));

	/* trigger may run in atomic context, I2C access goes to the work */
	schedule_work(&aw88461->start_work);

	return 0;
}

static void aw88461_start_work(struct work_struct *work)
{
	struct aw88461 *aw88461 = container_of(work, struct aw88461, start_work);

	guard(mutex)(&aw88461->lock);
	dev_dbg(aw88461->aw_pa->dev, "work: enabled=%d running=%d status=%d -> %s\n",
		READ_ONCE(aw88461->enabled), READ_ONCE(aw88461->running),
		aw88461->aw_pa->status,
		aw88461_active(aw88461) ? "start" : "stop");

	if (aw88461_active(aw88461))
		aw88461_start(aw88461);
	else
		aw88461_dev_stop(aw88461->aw_pa);
}

static void aw88461_cancel_work(void *data)
{
	struct aw88461 *aw88461 = data;

	WRITE_ONCE(aw88461->enabled, false);
	disable_work_sync(&aw88461->start_work);
}

static const struct snd_soc_dai_ops aw88461_dai_ops = {
	.set_fmt = aw88461_set_fmt,
	.hw_params = aw88461_hw_params,
	.set_tdm_slot = aw88461_set_tdm_slot,
	.trigger = aw88461_trigger,
};

static struct snd_soc_dai_driver aw88461_dai[] = {
	{
		.name = "aw88461-aif",
		.id = 1,
		.playback = {
			.stream_name = "Speaker_Playback",
			.channels_min = 1,
			.channels_max = 4,
			.rates = AW88461_RATES,
			.formats = AW88461_FORMATS,
		},
		.capture = {
			.stream_name = "Speaker_Capture",
			.channels_min = 1,
			.channels_max = 4,
			.rates = AW88461_RATES,
			.formats = AW88461_FORMATS,
		},
		.ops = &aw88461_dai_ops,
	},
};

static int aw88461_dev_set_profile_index(struct aw_device *aw_dev, int index)
{
	/* check the index whether is valid */
	if ((index >= aw_dev->prof_info.count) || (index < 0))
		return -EINVAL;
	/* check the index whether change */
	if (aw_dev->prof_index == index)
		return -EPERM;

	aw_dev->prof_index = index;

	return 0;
}

static int aw88461_profile_info(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_info *uinfo)
{
	struct snd_soc_component *codec = snd_kcontrol_chip(kcontrol);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(codec);
	char *prof_name;
	int count, ret;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;

	count = aw88461->aw_pa->prof_info.count;
	if (count <= 0) {
		uinfo->value.enumerated.items = 0;
		return 0;
	}

	uinfo->value.enumerated.items = count;

	if (uinfo->value.enumerated.item >= count)
		uinfo->value.enumerated.item = count - 1;

	count = uinfo->value.enumerated.item;

	ret = aw88461_dev_get_prof_name(aw88461->aw_pa, count, &prof_name);
	if (ret) {
		strscpy(uinfo->value.enumerated.name, "null");
		return 0;
	}

	strscpy(uinfo->value.enumerated.name, prof_name);

	return 0;
}

static int aw88461_profile_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_kcontrol_chip(kcontrol);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(codec);

	ucontrol->value.integer.value[0] = aw88461->aw_pa->prof_index;

	return 0;
}

static int aw88461_profile_set(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_kcontrol_chip(kcontrol);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(codec);
	int ret;

	/* pa stop or stopping just set profile */
	guard(mutex)(&aw88461->lock);
	ret = aw88461_dev_set_profile_index(aw88461->aw_pa, ucontrol->value.integer.value[0]);
	if (ret) {
		dev_dbg(codec->dev, "profile index does not change");
		return 0;
	}

	dev_dbg(codec->dev, "profile set: index=%d status=%d\n",
		aw88461->aw_pa->prof_index, aw88461->aw_pa->status);

	if (aw88461->aw_pa->status) {
		aw88461_dev_stop(aw88461->aw_pa);
		aw88461_start(aw88461);
	}

	return 1;
}

static int aw88461_volume_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_kcontrol_chip(kcontrol);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(codec);
	struct aw_volume_desc *vol_desc = &aw88461->aw_pa->volume_desc;

	ucontrol->value.integer.value[0] =
		(AW88461_MUTE_VOL - vol_desc->ctl_volume) / 2;

	return 0;
}

static int aw88461_volume_set(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *codec = snd_kcontrol_chip(kcontrol);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(codec);
	struct aw_volume_desc *vol_desc = &aw88461->aw_pa->volume_desc;
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	int value = ucontrol->value.integer.value[0];

	if (value < mc->min || value > mc->max)
		return -EINVAL;

	value = AW88461_MUTE_VOL - (value * 2);

	if (vol_desc->ctl_volume != value) {
		vol_desc->ctl_volume = value;
		aw88461_dev_set_volume(aw88461->aw_pa, vol_desc->ctl_volume);

		return 1;
	}

	return 0;
}

/*
 * The field contains 4 bits in units of 6dB + 6 bits in units of 0.125dB
 * which is too precise for TLV (!) so we have to multiply the scale by 2.
 *
 * The range is clamped at -90dB to prevent overflowing the 4-bit part.
 */
static const DECLARE_TLV_DB_SCALE(volume_tlv, -9000, 25, 0);

static const struct snd_kcontrol_new aw88461_controls[] = {
	SOC_SINGLE_EXT_TLV("PCM Playback Volume", AW88461_SYSCTRL2_REG,
		6, AW88461_CTL_MAX_VOL, 1,
		aw88461_volume_get, aw88461_volume_set, volume_tlv),
	AW88461_PROFILE_EXT("Profile Set", aw88461_profile_info,
		aw88461_profile_get, aw88461_profile_set),
};

static int aw88461_playback_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *k, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* clocks may appear only after PCM trigger, start from the work */
		WRITE_ONCE(aw88461->enabled, true);
		dev_dbg(aw88461->aw_pa->dev, "dapm POST_PMU: enabled=1 running=%d\n",
			READ_ONCE(aw88461->running));
		schedule_work(&aw88461->start_work);
		return 0;
	case SND_SOC_DAPM_PRE_PMD: {
		WRITE_ONCE(aw88461->enabled, false);
		dev_dbg(aw88461->aw_pa->dev, "dapm PRE_PMD: enabled=0 running=%d\n",
			READ_ONCE(aw88461->running));
		/* must be outside of the lock, the work takes it */
		cancel_work_sync(&aw88461->start_work);
		guard(mutex)(&aw88461->lock);
		aw88461_dev_stop(aw88461->aw_pa);
		return 0;
	}
	default:
		return 0;
	}
}

static const struct snd_soc_dapm_widget aw88461_dapm_widgets[] = {
	 /* playback */
	SND_SOC_DAPM_AIF_IN("AIF_RX", "Speaker_Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_PGA_E("Amplifier", SND_SOC_NOPM, 0, 0, NULL, 0,
					aw88461_playback_event,
					SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("DAC Output"),

	/* capture */
	SND_SOC_DAPM_AIF_OUT("AIF_TX", "Speaker_Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_INPUT("ADC Input"),
};

static const struct snd_soc_dapm_route aw88461_audio_map[] = {
	{"Amplifier", NULL, "AIF_RX"},
	{"DAC Output", NULL, "Amplifier"},
	{"AIF_TX", NULL, "ADC Input"},
};

static int aw88461_dev_init(struct aw88461 *aw88461, struct aw_container *aw_cfg)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	int ret;

	ret = aw88395_dev_cfg_load(aw_dev, aw_cfg);
	if (ret) {
		dev_err(aw_dev->dev, "aw_dev acf parse failed");
		return -EINVAL;
	}

	ret = regmap_write(aw_dev->regmap, AW88461_ID_REG, AW88461_SOFT_RESET_VALUE);
	if (ret)
		return ret;

	aw_dev->prof_cur = AW88461_INIT_PROFILE;
	aw_dev->prof_index = AW88461_INIT_PROFILE;

	ret = aw88461_dev_fw_update(aw88461);
	if (ret) {
		dev_err(aw_dev->dev, "fw update failed ret = %d\n", ret);
		return ret;
	}

	aw88461_dev_clear_int_status(aw_dev);

	aw88461_dev_uls_hmute(aw_dev, true);

	aw88461_dev_mute(aw_dev, true);

	aw88461_dev_i2s_tx_enable(aw_dev, false);

	usleep_range(AW88461_1000_US, AW88461_1000_US + 100);

	aw88461_dev_amppd(aw_dev, true);

	aw88461_dev_pwd(aw_dev, true);

	aw88461_set_noise_gate(aw_dev, true);

	aw88461_forbidden_output(aw_dev, true);

	return 0;
}

static int aw88461_request_firmware_file(struct aw88461 *aw88461)
{
	const struct firmware *cont __free(firmware) = NULL;
	struct aw_container *aw_cfg;
	const char *fw_name;
	int ret;

	aw88461->aw_pa->fw_status = AW88461_DEV_FW_FAILED;

	ret = device_property_read_string(aw88461->aw_pa->dev, "firmware-name", &fw_name);
	if (ret)
		fw_name = AW88461_ACF_FILE;

	ret = request_firmware(&cont, fw_name, aw88461->aw_pa->dev);
	if (ret)
		return dev_err_probe(aw88461->aw_pa->dev, ret,
					"load [%s] failed!", fw_name);

	dev_info(aw88461->aw_pa->dev, "loaded %s - size: %zu\n",
			fw_name, cont ? cont->size : 0);

	aw_cfg = devm_kzalloc(aw88461->aw_pa->dev, struct_size(aw_cfg, data, cont->size), GFP_KERNEL);
	if (!aw_cfg)
		return -ENOMEM;

	aw_cfg->len = (int)cont->size;
	memcpy(aw_cfg->data, cont->data, cont->size);

	aw88461->aw_cfg = aw_cfg;

	ret = aw88395_dev_load_acf_check(aw88461->aw_pa, aw88461->aw_cfg);
	if (ret) {
		dev_err(aw88461->aw_pa->dev, "load [%s] failed !", fw_name);
		return ret;
	}

	scoped_guard(mutex, &aw88461->lock) {	
		/* aw device init */
		ret = aw88461_dev_init(aw88461, aw88461->aw_cfg);
		if (ret)
			dev_err(aw88461->aw_pa->dev, "dev init failed");
	};

	return ret;
}

static int aw88461_codec_probe(struct snd_soc_component *component)
{
	struct snd_soc_dapm_context *dapm = snd_soc_component_to_dapm(component);
	struct aw88461 *aw88461 = snd_soc_component_get_drvdata(component);
	int ret;

	ret = aw88461_request_firmware_file(aw88461);
	if (ret)
		return dev_err_probe(aw88461->aw_pa->dev, ret,
				"aw88461_request_firmware_file failed\n");

	/* add widgets */
	ret = snd_soc_dapm_new_controls(dapm, aw88461_dapm_widgets,
							ARRAY_SIZE(aw88461_dapm_widgets));
	if (ret)
		return ret;

	/* add route */
	ret = snd_soc_dapm_add_routes(dapm, aw88461_audio_map,
							ARRAY_SIZE(aw88461_audio_map));
	if (ret)
		return ret;

	ret = snd_soc_add_component_controls(component, aw88461_controls,
							ARRAY_SIZE(aw88461_controls));

	return ret;
}

static const struct snd_soc_component_driver soc_codec_dev_aw88461 = {
	.probe = aw88461_codec_probe,
};

static void aw88461_hw_reset(struct aw88461 *aw88461)
{
	gpiod_set_value_cansleep(aw88461->reset_gpio, 0);
	usleep_range(AW88461_1000_US, AW88461_1000_US + 10);
	gpiod_set_value_cansleep(aw88461->reset_gpio, 1);
	usleep_range(AW88461_1000_US, AW88461_1000_US + 10);
}

static void aw88461_parse_channel_dt(struct aw88461 *aw88461)
{
	struct aw_device *aw_dev = aw88461->aw_pa;
	struct device_node *np = aw_dev->dev->of_node;
	u32 channel_value = AW88461_DEV_DEFAULT_CH;

	of_property_read_u32(np, "awinic,audio-channel", &channel_value);
	aw88461->phase_sync = of_property_read_bool(np, "awinic,sync-flag");

	aw_dev->channel = channel_value;
}

static int aw88461_init(struct aw88461 *aw88461, struct i2c_client *i2c, struct regmap *regmap)
{
	struct aw_device *aw_dev;
	unsigned int chip_id;
	int ret;

	/* read chip id */
	ret = regmap_read(regmap, AW88461_ID_REG, &chip_id);
	if (ret) {
		dev_err(&i2c->dev, "%s read chipid error. ret = %d", __func__, ret);
		return ret;
	}
	if (chip_id != AW88461_CHIP_ID) {
		dev_err(&i2c->dev, "unsupported device id = %x", chip_id);
		return -ENXIO;
	}

	dev_info(&i2c->dev, "chip id = %x\n", chip_id);

	aw_dev = devm_kzalloc(&i2c->dev, sizeof(*aw_dev), GFP_KERNEL);
	if (!aw_dev)
		return -ENOMEM;

	aw88461->aw_pa = aw_dev;
	aw_dev->i2c = i2c;
	aw_dev->regmap = regmap;
	aw_dev->dev = &i2c->dev;
	aw_dev->chip_id = AW88461_CHIP_ID;
	aw_dev->acf = NULL;
	aw_dev->prof_info.prof_desc = NULL;
	aw_dev->prof_info.count = 0;
	aw_dev->prof_info.prof_type = AW88395_DEV_NONE_TYPE_ID;
	aw_dev->channel = 0;
	aw_dev->fw_status = AW88461_DEV_FW_FAILED;
	aw_dev->volume_desc.ctl_volume = AW88461_CTL_DEFAULT_VOL;
	aw_dev->volume_desc.mute_volume = AW88461_MUTE_VOL;
	aw88461_parse_channel_dt(aw88461);

	return ret;
}

static int aw88461_i2c_probe(struct i2c_client *i2c)
{
	struct aw88461 *aw88461;
	int ret;

	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&i2c->dev, -ENXIO, "check_functionality failed");

	aw88461 = devm_kzalloc(&i2c->dev, sizeof(*aw88461), GFP_KERNEL);
	if (!aw88461)
		return -ENOMEM;

	/* set defaults */
	aw88461->slot_num_value = AW88461_SLOT_NUM_I2S_MODE_VALUE;
	aw88461->sr_value = AW88461_I2SSR_48KHZ_VALUE;
	aw88461->cco_mux_value = AW88461_CCO_MUX_BYPASS_VALUE;
	aw88461->fs_value = AW88461_I2SFS_24_BITS_VALUE;
	aw88461->bck_value = AW88461_I2SBCK_64FS_VALUE;
	aw88461->bck_inv_value = AW88461_BCKINV_NOT_INVERT_VALUE;
	aw88461->tdm_bck_value = AW88461_TDM_BCK_UNSET;
	aw88461->md_value = AW88461_I2SMD_PHILIPS_STANDARD_VALUE;
	aw88461->rxr_slotvld_mask = 1 << AW88461_I2S_RXR_SLOTVLD_START_BIT;

	mutex_init(&aw88461->lock);
	INIT_WORK(&aw88461->start_work, aw88461_start_work);

	ret = devm_add_action_or_reset(&i2c->dev, aw88461_cancel_work, aw88461);
	if (ret)
		return ret;

	i2c_set_clientdata(i2c, aw88461);
	
	aw88461->reset_gpio =
		devm_gpiod_get_optional(&i2c->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(aw88461->reset_gpio))
		dev_info(&i2c->dev, "reset gpio not defined\n");
	else
		aw88461_hw_reset(aw88461);

	aw88461->regmap = devm_regmap_init_i2c(i2c, &aw88461_remap_config);
	if (IS_ERR(aw88461->regmap)) {
		ret = PTR_ERR(aw88461->regmap);
		return dev_err_probe(&i2c->dev, ret, "failed to init regmap: %d\n", ret);
	}

	/* aw pa init */
	ret = aw88461_init(aw88461, i2c, aw88461->regmap);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_component(&i2c->dev,
			&soc_codec_dev_aw88461,
			aw88461_dai, ARRAY_SIZE(aw88461_dai));
	if (ret)
		dev_err(&i2c->dev, "failed to register aw88461: %d", ret);

	return ret;
}

static const struct i2c_device_id aw88461_i2c_id[] = {
	{ .name = "aw88461" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw88461_i2c_id);

static const struct of_device_id aw88461_of_table[] = {
	{ .compatible = "awinic,aw88461" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw88461_of_table);

static struct i2c_driver aw88461_i2c_driver = {
	.driver = {
		.name = "aw88461",
		.of_match_table = aw88461_of_table,
	},
	.probe = aw88461_i2c_probe,
	.id_table = aw88461_i2c_id,
};
module_i2c_driver(aw88461_i2c_driver);

MODULE_DESCRIPTION("ASoC AW88461 Smart PA Driver");
MODULE_LICENSE("GPL v2");
