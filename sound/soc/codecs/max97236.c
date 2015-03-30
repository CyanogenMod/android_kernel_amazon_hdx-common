/*
 * max97236.c -- MAX97236 ALSA SoC Audio driver
 *
 * Copyright 2012-2013 Maxim Integrated Products
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/debugfs.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/jack.h>
#include <sound/max97236.h>
#include "max97236.h"
#include <linux/version.h>

#define DEBUG
#define DAPM_ENABLE
#define MAX_STRING 16

static int extclk_freq = EXTCLK_FREQUENCY;

/*headset jack belongs  here*/
#include <linux/input.h>
static struct snd_soc_jack max97236_headset;

/* Allows for sparsely populated register maps */
static struct reg_default max97236_reg[] = {
	{ 0x00, 0x00 }, /* 00 Status1 */
	{ 0x01, 0x00 }, /* 01 Status2 */
	{ 0x02, 0x00 }, /* 02 Status3 */
	{ 0x04, 0x00 }, /* 04 IRQ Mask1 */
	{ 0x05, 0x00 }, /* 05 IRQ Mask2 */
	{ 0x07, 0xC0 }, /* 07 Left Volume */
	{ 0x08, 0x40 }, /* 08 Right Volume */
	{ 0x09, 0x00 }, /* 09 Microphone */
	{ 0x0B, 0xC0 }, /* 0B Revision ID */
	{ 0x12, 0x00 }, /* 12 Keyscan Clock Divider 1 */
	{ 0x13, 0x00 }, /* 13 Keyscan Clock Divider 2 */
	{ 0x14, 0x00 }, /* 14 Keyscan Clock Divider ADC */
	{ 0x15, 0x00 }, /* 15 Keyscan Debounce */
	{ 0x16, 0x00 }, /* 16 Keyscan Delay */
	{ 0x17, 0x00 }, /* 17 Passive MBH Keyscan Data */
	{ 0x18, 0x00 }, /* 18 DC Test Slew Control */
	{ 0x19, 0x20 }, /* 19 State Forcing */
	{ 0x1A, 0x05 }, /* 1A AC Test Control */
	{ 0x1D, 0x00 }, /* 1D Enable 1 */
	{ 0x1E, 0x00 }, /* 1E Enable 2 */

	{ 0x1F, 0x00 }, /* 1F Test Enable 1 */
	{ 0x20, 0x00 }, /* 20 Test Enable 2 */
	{ 0x21, 0x00 }, /* 21 Test Data 1 */
	{ 0x23, 0x00 }, /* 23 Test Data 3 */
};

static bool max97236_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case M97236_REG_00_STATUS1:
	case M97236_REG_01_STATUS2:
	case M97236_REG_02_STATUS3:
	case M97236_REG_0B_REV_ID:
	case M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA:
	case M97236_REG_1D_ENABLE_1:
	case M97236_REG_1E_ENABLE_2:
	case M97236_REG_21_TEST_DATA_1:
	case M97236_REG_23_TEST_DATA_3:
		return true;
	default:
		return false;
	}
}

static bool max97236_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case M97236_REG_00_STATUS1:
	case M97236_REG_01_STATUS2:
	case M97236_REG_02_STATUS3:
	case M97236_REG_04_IRQ_MASK1:
	case M97236_REG_05_IRQ_MASK2:
	case M97236_REG_07_LEFT_VOLUME:
	case M97236_REG_08_RIGHT_VOLUME:
	case M97236_REG_09_MICROPHONE:
	case M97236_REG_0B_REV_ID:
	case M97236_REG_12_KEYSCAN_CLK_DIV_HI:
	case M97236_REG_13_KEYSCAN_CLK_DIV_LO:
	case M97236_REG_14_KEYSCAN_CLK_DIV_ADC:
	case M97236_REG_15_KEYSCAN_DEBOUNCE:
	case M97236_REG_16_KEYSCAN_DELAY:
	case M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA:
	case M97236_REG_18_DC_TEST_SLEW_CONTROL:
	case M97236_REG_19_STATE_FORCING:
	case M97236_REG_1A_AC_TEST_CONTROL:
	case M97236_REG_1D_ENABLE_1:
	case M97236_REG_1E_ENABLE_2:
	case M97236_REG_1F_TEST_ENABLE_1:
	case M97236_REG_20_TEST_ENABLE_2:
	case M97236_REG_21_TEST_DATA_1:
	case M97236_REG_23_TEST_DATA_3:
		return true;
	default:
		return false;
	}
}

static const unsigned int max97236_vol_tlv[] = {
	TLV_DB_RANGE_HEAD(2),
	0, 3, TLV_DB_SCALE_ITEM(-6000, 200, 0),
	4, 63, TLV_DB_SCALE_ITEM(-5300, 100, 0),
};

static const DECLARE_TLV_DB_SCALE(max97236_gain_tlv, 1200, 1200, 0);

static const char * const max97236_gnd_text[] = { "NONE", "RING2", "SLEEVE",
		"RING2 and SLEEVE" };

static const struct soc_enum max97236_gnd_enum =
	SOC_ENUM_SINGLE(M97236_REG_02_STATUS3, M97236_GND_SHIFT,
		ARRAY_SIZE(max97236_gnd_text),
		max97236_gnd_text);

static const char * const max97236_micr_text[] = { "2.2k", "2.6k", "3.0k",
		"Bypassed", "High Z" };

static const struct soc_enum max97236_micr_enum =
	SOC_ENUM_SINGLE(M97236_REG_09_MICROPHONE, M97236_MICR_SHIFT,
		ARRAY_SIZE(max97236_micr_text),
		max97236_micr_text);

static const char * const max97236_bias_text[] = { "2.0V", "2.6V" };

static const struct soc_enum max97236_bias_enum =
	SOC_ENUM_SINGLE(M97236_REG_09_MICROPHONE, M97236_BIAS_SHIFT,
		ARRAY_SIZE(max97236_bias_text),
		max97236_bias_text);

static const char * const max97236_ac_repeat_text[] = { "1", "3", "5", "7" };

static const struct soc_enum max97236_ac_repeat_enum =
	SOC_ENUM_SINGLE(M97236_REG_1A_AC_TEST_CONTROL,
		M97236_AC_REPEAT_SHIFT,
		ARRAY_SIZE(max97236_ac_repeat_text),
		max97236_ac_repeat_text);

static const char * const max97236_pulse_width_text[] = { "50us", "100us",
		"150us", "300us" };

static const struct soc_enum max97236_pulse_width_enum =
	SOC_ENUM_SINGLE(M97236_REG_1A_AC_TEST_CONTROL,
		M97236_PULSE_WIDTH_SHIFT,
		ARRAY_SIZE(max97236_pulse_width_text),
		max97236_pulse_width_text);

static const char * const max97236_pulse_amplitude_text[] = { "25mV", "50mV",
		"100mV", "200mV" };

static const struct soc_enum max97236_pulse_amplitude_enum =
	SOC_ENUM_SINGLE(M97236_REG_1A_AC_TEST_CONTROL,
		M97236_PULSE_AMPLITUDE_SHIFT,
		ARRAY_SIZE(max97236_pulse_amplitude_text),
		max97236_pulse_amplitude_text);

static const char * const max97236_auto_text[] = { "Host", "Auto",
		"Pre Config" };

static const struct soc_enum max97236_auto_enum =
	SOC_ENUM_SINGLE(M97236_REG_1E_ENABLE_2, M97236_AUTO_SHIFT,
		ARRAY_SIZE(max97236_auto_text), max97236_auto_text);


static const struct snd_kcontrol_new max97236_snd_controls[] = {

	SOC_SINGLE_TLV("L Volume",
		M97236_REG_07_LEFT_VOLUME, M97236_LVOL_SHIFT,
		M97236_LVOL_NUM - 1, 0, max97236_vol_tlv),

	SOC_SINGLE_TLV("R Volume",
		M97236_REG_08_RIGHT_VOLUME, M97236_RVOL_SHIFT,
		M97236_RVOL_NUM - 1, 0, max97236_vol_tlv),

	SOC_SINGLE_TLV("GAIN Volume",
		M97236_REG_09_MICROPHONE, M97236_GAIN_SHIFT,
		M97236_GAIN_NUM - 1, 0, max97236_gain_tlv),

	SOC_SINGLE("L_R", M97236_REG_07_LEFT_VOLUME,
		M97236_L_R_SHIFT, M97236_L_R_NUM - 1, 0),

	SOC_SINGLE("MUTEL", M97236_REG_07_LEFT_VOLUME,
		M97236_MUTEL_SHIFT, M97236_MUTEL_NUM - 1, 0),

	SOC_SINGLE("MUTER", M97236_REG_08_RIGHT_VOLUME,
		M97236_MUTER_SHIFT, M97236_MUTER_NUM - 1, 0),

	SOC_ENUM("GND Enum", max97236_gnd_enum),
	SOC_ENUM("MICR Enum", max97236_micr_enum),
	SOC_ENUM("BIAS Enum", max97236_bias_enum),
	SOC_ENUM("AC_REPEAT Enum", max97236_ac_repeat_enum),
	SOC_ENUM("PULSE_WIDTH Enum", max97236_pulse_width_enum),
	SOC_ENUM("PULSE_AMPLITUDE Enum", max97236_pulse_amplitude_enum),
	SOC_ENUM("AUTO Enum", max97236_auto_enum),

	SOC_SINGLE("JKIN", M97236_REG_00_STATUS1,
		M97236_JKIN_SHIFT, M97236_JKIN_NUM - 1, 0),

	SOC_SINGLE("DDONE", M97236_REG_00_STATUS1,
		M97236_DDONE_SHIFT, M97236_DDONE_NUM - 1, 0),

	SOC_SINGLE("VOL", M97236_REG_00_STATUS1,
		M97236_VOL_SHIFT, M97236_VOL_NUM - 1, 0),

	SOC_SINGLE("VID_IN", M97236_REG_00_STATUS1,
		M97236_VID_IN_SHIFT, M97236_VID_IN_NUM - 1, 0),

	SOC_SINGLE("MIC_IN", M97236_REG_00_STATUS1,
		M97236_MIC_IN_SHIFT, M97236_MIC_IN_NUM - 1, 0),

	SOC_SINGLE("JACKSW", M97236_REG_00_STATUS1,
		M97236_JACKSW_SHIFT, M97236_JACKSW_NUM - 1, 0),

	SOC_SINGLE("MCSW", M97236_REG_00_STATUS1,
		M97236_MCSW_SHIFT, M97236_MCSW_NUM - 1, 0),

	SOC_SINGLE("MBH", M97236_REG_00_STATUS1,
		M97236_MBH_SHIFT, M97236_MBH_NUM - 1, 0),

	SOC_SINGLE("LINE_L", M97236_REG_01_STATUS2,
		M97236_LINE_L_SHIFT, M97236_LINE_L_NUM - 1, 0),

	SOC_SINGLE("LINE_R", M97236_REG_01_STATUS2,
		M97236_LINE_R_SHIFT, M97236_LINE_R_NUM - 1, 0),

	SOC_SINGLE("HP_L", M97236_REG_01_STATUS2,
		M97236_HP_L_SHIFT, M97236_HP_L_NUM - 1, 0),

	SOC_SINGLE("HP_R", M97236_REG_01_STATUS2,
		M97236_HP_R_SHIFT, M97236_HP_R_NUM - 1, 0),

	SOC_SINGLE("JACKSWINC", M97236_REG_01_STATUS2,
		M97236_JACKSWINC_SHIFT, M97236_JACKSWINC_NUM - 1, 0),

	SOC_SINGLE("KEY", M97236_REG_01_STATUS2,
		M97236_KEY_SHIFT, M97236_KEY_NUM - 1, 0),

	SOC_SINGLE("IJKIN", M97236_REG_04_IRQ_MASK1,
		M97236_IJKIN_SHIFT, M97236_IJKIN_NUM - 1, 0),

	SOC_SINGLE("IDDONE", M97236_REG_04_IRQ_MASK1,
		M97236_IDDONE_SHIFT, M97236_IDDONE_NUM - 1, 0),

	SOC_SINGLE("IVOL", M97236_REG_04_IRQ_MASK1,
		M97236_IVOL_SHIFT, M97236_IVOL_NUM - 1, 0),

	SOC_SINGLE("IVID", M97236_REG_04_IRQ_MASK1,
		M97236_IVID_SHIFT, M97236_IVID_NUM - 1, 0),

	SOC_SINGLE("IMIC", M97236_REG_04_IRQ_MASK1,
		M97236_IMIC_SHIFT, M97236_IMIC_NUM - 1, 0),

	SOC_SINGLE("IJACKSW", M97236_REG_04_IRQ_MASK1,
		M97236_IJACKSW_SHIFT, M97236_IJACKSW_NUM - 1, 0),

	SOC_SINGLE("IMCSW", M97236_REG_04_IRQ_MASK1,
		M97236_IMCSW_SHIFT, M97236_IMCSW_NUM - 1, 0),
	SOC_SINGLE("IMBH", M97236_REG_04_IRQ_MASK1,
		M97236_IMBH_SHIFT, M97236_IMBH_NUM - 1, 0),

	SOC_SINGLE("ILINE_L", M97236_REG_05_IRQ_MASK2,
		M97236_ILINE_L_SHIFT, M97236_ILINE_L_NUM - 1, 0),

	SOC_SINGLE("ILINE_R", M97236_REG_05_IRQ_MASK2,
		M97236_ILINE_R_SHIFT, M97236_ILINE_R_NUM - 1, 0),

	SOC_SINGLE("IHP_L", M97236_REG_05_IRQ_MASK2,
		M97236_IHP_L_SHIFT, M97236_IHP_L_NUM - 1, 0),

	SOC_SINGLE("IHP_R", M97236_REG_05_IRQ_MASK2,
		M97236_IHP_R_SHIFT, M97236_IHP_R_NUM - 1, 0),

	SOC_SINGLE("IJACKSWINC", M97236_REG_05_IRQ_MASK2,
		M97236_IJACKSWINC_SHIFT, M97236_IJACKSWINC_NUM - 1, 0),

	SOC_SINGLE("IKEY", M97236_REG_05_IRQ_MASK2,
		M97236_IKEY_SHIFT, M97236_IKEY_NUM - 1, 0),

	SOC_SINGLE("ID", M97236_REG_0B_REV_ID,
		M97236_ID_SHIFT, M97236_ID_NUM - 1, 0),

	SOC_SINGLE("KEY_DIV_HIGH", M97236_REG_12_KEYSCAN_CLK_DIV_HI,
		M97236_KEY_DIV_HIGH_SHIFT, M97236_KEY_DIV_HIGH_NUM - 1, 0),

	SOC_SINGLE("KEY_DIV_LOW", M97236_REG_13_KEYSCAN_CLK_DIV_LO,
		M97236_KEY_DIV_LOW_SHIFT, M97236_KEY_DIV_LOW_NUM - 1, 0),

	SOC_SINGLE("KEY_DIV_ADC", M97236_REG_14_KEYSCAN_CLK_DIV_ADC,
		M97236_KEY_DIV_ADC_SHIFT, M97236_KEY_DIV_ADC_NUM - 1, 0),

	SOC_SINGLE("KEY_DEB", M97236_REG_15_KEYSCAN_DEBOUNCE,
		M97236_KEY_DEB_SHIFT, M97236_KEY_DEB_NUM - 1, 0),

	SOC_SINGLE("KEY_DEL", M97236_REG_16_KEYSCAN_DELAY,
		M97236_KEY_DEL_SHIFT, M97236_KEY_DEL_NUM - 1, 0),

	SOC_SINGLE("PRESS", M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA,
		M97236_PRESS_SHIFT, M97236_PRESS_NUM - 1, 0),

	SOC_SINGLE("RANGE", M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA,
		M97236_RANGE_SHIFT, M97236_RANGE_NUM - 1, 0),

	SOC_SINGLE("KEYDATA", M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA,
		M97236_KEYDATA_SHIFT, M97236_KEYDATA_NUM - 1, 0),

	SOC_SINGLE("DC_SLEW", M97236_REG_18_DC_TEST_SLEW_CONTROL,
		M97236_DC_SLEW_SHIFT, M97236_DC_SLEW_NUM - 1, 0),

	SOC_SINGLE("FORCEN", M97236_REG_19_STATE_FORCING,
		M97236_FORCEN_SHIFT, M97236_FORCEN_NUM - 1, 0),

	SOC_SINGLE("STATE", M97236_REG_19_STATE_FORCING,
		M97236_STATE_SHIFT, M97236_STATE_NUM - 1, 0),

	SOC_SINGLE("SHDNN", M97236_REG_1D_ENABLE_1,
		M97236_SHDNN_SHIFT, M97236_SHDNN_NUM - 1, 0),

	SOC_SINGLE("RESET", M97236_REG_1D_ENABLE_1,
		M97236_RESET_SHIFT, M97236_RESET_NUM - 1, 0),

	SOC_SINGLE("VSENN", M97236_REG_1E_ENABLE_2,
		M97236_VSENN_SHIFT, M97236_VSENN_NUM - 1, 0),

	SOC_SINGLE("ZDENN", M97236_REG_1E_ENABLE_2,
		M97236_ZDENN_SHIFT, M97236_ZDENN_NUM - 1, 0),

	SOC_SINGLE("FAST", M97236_REG_1E_ENABLE_2,
		M97236_FAST_SHIFT, M97236_FAST_NUM - 1, 0),

	SOC_SINGLE("THRH", M97236_REG_1E_ENABLE_2,
		M97236_THRH_SHIFT, M97236_THRH_NUM - 1, 0),

	SOC_SINGLE("AUTO", M97236_REG_1E_ENABLE_2,
		0, 1, 0),
};

int max97236_hp_enable(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);

	/* DEBUG */
	pr_info("%s: enter\n", __func__);

	regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
		M97236_SHDNN_MASK, M97236_SHDNN_MASK);

	return 0;
}

int max97236_hp_disable(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);

	/* DEBUG */
	pr_info("%s: enter\n", __func__);

	regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
		M97236_SHDNN_MASK, 0);

	return 0;
}

static int max97236_hp_event(struct snd_soc_dapm_widget *w,
	     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		max97236_hp_enable(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		max97236_hp_disable(codec);
		break;

	case SND_SOC_DAPM_POST_PMD:
		pr_info("%s: event SND_SOC_DAPM_POST_PMD\n", __func__);
		break;

	case SND_SOC_DAPM_PRE_PMU:
		pr_info("%s: event SND_SOC_DAPM_PRE_PMU\n", __func__);
		break;

	default:
		pr_info("%s: event UNKNOWN\n", __func__);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int max97236_headset_mic_event(struct snd_soc_dapm_widget *w,
	     struct snd_kcontrol *kcontrol, int event)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(w->codec);
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
			M97236_SHDNN_MASK, M97236_SHDNN_MASK);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
			M97236_SHDNN_MASK, 0);
		break;

	case SND_SOC_DAPM_POST_PMD:
		pr_info("%s: event SND_SOC_DAPM_POST_PMD\n", __func__);
		break;

	case SND_SOC_DAPM_PRE_PMU:
		pr_info("%s: event SND_SOC_DAPM_PRE_PMU\n", __func__);
		break;

	default:
		pr_info("%s: event UNKNOWN\n", __func__);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct snd_soc_dapm_widget max97236_dapm_widgets[] = {

	SND_SOC_DAPM_INPUT("MAX97236_HPL"),
	SND_SOC_DAPM_INPUT("MAX97236_HPR"),
	SND_SOC_DAPM_INPUT("MAX97236_JACK_MICROPHONE"),

	SND_SOC_DAPM_HP("Headphone", max97236_hp_event),
	SND_SOC_DAPM_MIC("Headset Mic", max97236_headset_mic_event),

	SND_SOC_DAPM_DAC("SHDN", "HiFi Playback", M97236_REG_1D_ENABLE_1,
		M97236_SHDNN_SHIFT, 0),

	SND_SOC_DAPM_SUPPLY("MAX97236_SHDN", M97236_REG_1D_ENABLE_1,
		M97236_SHDNN_SHIFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MAX97236_MIC_BIAS", M97236_REG_1D_ENABLE_1,
		M97236_MIC_BIAS_SHIFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MAX97236_MIC_AMP", M97236_REG_1D_ENABLE_1,
		M97236_MIC_AMP_SHIFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MAX97236_KS", M97236_REG_1D_ENABLE_1,
		M97236_KS_SHIFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MAX97236_LFTEN", M97236_REG_1E_ENABLE_2,
		M97236_LFTEN_SHIFT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("MAX97236_RGHEN", M97236_REG_1E_ENABLE_2,
		M97236_RGHEN_SHIFT, 0, NULL, 0),

	SND_SOC_DAPM_OUTPUT("MAX97236_MOUT"),
	SND_SOC_DAPM_OUTPUT("MAX97236_JACK_LEFT_AUDIO"),
	SND_SOC_DAPM_OUTPUT("MAX97236_JACK_RIGHT_AUDIO"),

};


#ifdef DAPM_ENABLE
static const struct snd_soc_dapm_route max97236_dapm_routes[] = {
	{"MAX97236_JACK_LEFT_AUDIO", NULL, "MAX97236_HPL"},
	{"MAX97236_JACK_RIGHT_AUDIO", NULL, "MAX97236_HPR"},

	{"MAX97236_JACK_LEFT_AUDIO", NULL, "MAX97236_SHDN"},
	{"MAX97236_JACK_RIGHT_AUDIO", NULL, "MAX97236_SHDN"},
	{"MAX97236_MOUT", NULL, "MAX97236_SHDN"},

};
#endif

static int max97236_add_widgets(struct snd_soc_codec *codec)
{
#ifdef DAPM_ENABLE
	struct snd_soc_dapm_context *dapm = &codec->dapm;
#endif

	snd_soc_add_codec_controls(codec, max97236_snd_controls,
		ARRAY_SIZE(max97236_snd_controls));

#ifdef DAPM_ENABLE
	snd_soc_dapm_new_controls(dapm, max97236_dapm_widgets,
		ARRAY_SIZE(max97236_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, max97236_dapm_routes,
		ARRAY_SIZE(max97236_dapm_routes));
#endif

	return 0;
}

static int max97236_set_bias_level(struct snd_soc_codec *codec,
				   enum snd_soc_bias_level level)
{
	codec->dapm.bias_level = level;

	return 0;
}

static void string_copy(char *dest, char *src, int length)
{
	strncpy(dest, src, length);
	dest[length-1] = '\0';
}

static unsigned int map_adc(unsigned int reg)
{
	unsigned int adc = reg & 0x3F;

	if ((reg & M97236_RANGE_MASK) == 0)
		adc += 64;

	return adc;
}

static void max97236_keypress(struct max97236_priv *max97236,
		unsigned int *status_reg)
{
	unsigned char keystr[MAX_STRING] = "";
	unsigned int key = 0;
	unsigned int reg;
	unsigned int adc;
	int press;

	regmap_read(max97236->regmap, M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA, &reg);
	press = (reg & M97236_PRESS_MASK) == M97236_PRESS_MASK;

	adc = map_adc(reg);
	if (press) {
		if ((status_reg[0] & M97236_MCSW_MASK) ||
				(adc < M97236_KEY_THRESH_0)) {
			string_copy(keystr, "SND_JACK_BTN_0", MAX_STRING);
			key = SND_JACK_BTN_0;
		} else if (status_reg[1] & M97236_KEY_MASK) {
			if (adc < M97236_KEY_THRESH_1) {
				string_copy(keystr, "SND_JACK_BTN_2",
						MAX_STRING);
				key = SND_JACK_BTN_2;
			} else {
				string_copy(keystr, "SND_JACK_BTN_4",
						MAX_STRING);
				key = SND_JACK_BTN_4;
			}
		} else {
			dev_err(max97236->codec->dev,
			        "Unknown key interrupt s1 %02X, s2 %02X\n",
			        status_reg[0], status_reg[1]);
			}
		}
		dev_info(max97236->codec->dev, "%s %s\n",
					press ? (char *) keystr : "BUTTON",
					press ? "PRESS" : "RELEASE");

		snd_soc_jack_report(max97236->jack, key, 0x7E00000);
}

static void max97236_report_jack_state(struct max97236_priv *max97236,
		unsigned int *status_reg)
{
	char string[MAX_STRING];
	int state;

	if ((status_reg[0] & 0x88) == 0x88) {
		state = SND_JACK_HEADSET;
		if (status_reg[3])
			string_copy(string, "HEADSET", MAX_STRING);
		else
			string_copy(string, "HEADSET*", MAX_STRING);
	} else if ((status_reg[0] & 0x80) == 0x80) {
		state = SND_JACK_HEADPHONE;
		if (status_reg[3])
			string_copy(string, "HEADPHONES", MAX_STRING);
		else
			string_copy(string, "HEADPHONES*", MAX_STRING);
	} else if ((status_reg[1] & 0xCC) == 0xCC) {
		state = SND_JACK_LINEOUT;
		string_copy(string, "LINEOUT", MAX_STRING);
	} else {
		state = M97236_JACK_STATE_NONE;
		string_copy(string, "NOTHING", MAX_STRING);
	}

		dev_info(max97236->codec->dev, "0x%02X, 0x%02X, 0x%02X, 0x%02X - %s\n",
		status_reg[0],
		status_reg[1],
		status_reg[2],
		status_reg[3],
		string);

	if (max97236->jack_state != state) {
		snd_soc_jack_report(max97236->jack, state,
			SND_JACK_HEADSET | SND_JACK_LINEOUT);
		max97236->jack_state = state;
	}
}

static void max97236_set_clk_dividers(struct max97236_priv *max97236,
		unsigned int freq)
{
	unsigned int clk_div;
	unsigned int adc_div;

	clk_div = freq / 2000;
	adc_div = freq / 200000;

	regmap_write(max97236->regmap,
			M97236_REG_12_KEYSCAN_CLK_DIV_HI, clk_div >> 8);
	regmap_write(max97236->regmap,
			M97236_REG_13_KEYSCAN_CLK_DIV_LO, clk_div & 0xFF);
	regmap_write(max97236->regmap,
			M97236_REG_14_KEYSCAN_CLK_DIV_ADC, adc_div);
}

static void max97236_configure_for_detection(struct max97236_priv *max97236,
					unsigned int mode)
{
	unsigned int reg;

	regmap_read(max97236->regmap, M97236_REG_00_STATUS1, &reg);
	regmap_read(max97236->regmap, M97236_REG_01_STATUS2, &reg);

	regmap_write(max97236->regmap, M97236_REG_09_MICROPHONE,
		     M97236_BIAS_MASK);
	regmap_write(max97236->regmap, M97236_REG_18_DC_TEST_SLEW_CONTROL,
			DEFAULT_TEST_SLEW_RATE);
	regmap_update_bits(max97236->regmap, M97236_REG_1E_ENABLE_2,
			M97236_VSENN_MASK | M97236_AUTO_MASK,
			M97236_VSENN_MASK | mode);

	if (max97236->jack_state == SND_JACK_HEADSET) {
		regmap_write(max97236->regmap,
				M97236_REG_15_KEYSCAN_DEBOUNCE, 0x09);
		regmap_write(max97236->regmap,
				M97236_REG_16_KEYSCAN_DELAY, 0x18);
		regmap_write(max97236->regmap,
				M97236_REG_04_IRQ_MASK1,
				M97236_IJACKSW_MASK | M97236_IMCSW_MASK |
					M97236_IMBH_MASK);
		regmap_write(max97236->regmap, M97236_REG_05_IRQ_MASK2,
				M97236_IKEY_MASK);
	} else {
		regmap_write(max97236->regmap, M97236_REG_04_IRQ_MASK1,
				M97236_IJACKSW_MASK);
		regmap_write(max97236->regmap, M97236_REG_05_IRQ_MASK2, 0x00);
	}
}

static int max97236_jacksw_active(struct max97236_priv *max97236)
{
	unsigned int reg;
#ifdef M97236_JACK_SWITCH_NORMALLY_CLOSED
	int test_value = M97236_JACKSW_MASK;
#else
	int test_value = 0;
#endif
	int ret;

	regmap_read(max97236->regmap, M97236_REG_00_STATUS1, &reg);
	ret = (reg & M97236_JACKSW_MASK) == test_value;

	return ret;
}

#ifdef MAX97236_AUTOMODE1_JACK_DETECTION

static int max97236_reset(struct max97236_priv *max97236)
{
	int ret;

	ret  = regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
		M97236_RESET_MASK, M97236_RESET_MASK);
	msleep(20);
	ret |= regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
		M97236_RESET_MASK, 0);

	return ret;
}

static void max97236_jack_event(struct max97236_priv *max97236)
{
	unsigned int status_reg[] = {0, 0, 0, 1};
	int count;

	status_reg[0] = max97236->status0;
	regmap_read(max97236->regmap, M97236_REG_01_STATUS2,
			&status_reg[1]);

	/* First check for a key press */
	if (((status_reg[0] & M97236_IMBH_MASK)     ||
			(status_reg[0] & M97236_IMCSW_MASK) ||
			(status_reg[1] & M97236_IKEY_MASK)) &&
			(status_reg[0] & 0x80)) {
		max97236_keypress(max97236, status_reg);
	} else {
		if (max97236_jacksw_active(max97236))
			goto max97236_jack_event_10;

		count = 30;
		do {
			msleep(20);
			regmap_read(max97236->regmap,
					M97236_REG_00_STATUS1,
					&status_reg[0]);
		} while (((status_reg[0] & 0x80) == 0x80) && --count);

		regmap_read(max97236->regmap, M97236_REG_00_STATUS1,
				&status_reg[0]);
		regmap_read(max97236->regmap, M97236_REG_01_STATUS2,
				&status_reg[1]);
		regmap_read(max97236->regmap, M97236_REG_02_STATUS3,
				&status_reg[2]);

		/* test for jack switch malfunction indication */
		if ((status_reg[1] & M97236_IJACKSWINC_MASK) &&
				(status_reg[0] & 0x80))
			pr_err("JACKSWINC set\n");

		max97236->ignore_int = 0;

		max97236_report_jack_state(max97236, status_reg);
		if (max97236->jack_state == M97236_JACK_STATE_NONE) {
			regmap_update_bits(max97236->regmap,
					M97236_REG_07_LEFT_VOLUME,
					M97236_MUTEL_MASK, M97236_MUTEL_MASK);
			regmap_update_bits(max97236->regmap,
					M97236_REG_08_RIGHT_VOLUME,
					M97236_MUTER_MASK, M97236_MUTER_MASK);
			regmap_update_bits(max97236->regmap,
					M97236_REG_1D_ENABLE_1,
					M97236_SHDNN_MASK, 0);
		}
	}

max97236_jack_event_10:
	max97236_configure_for_detection(max97236, M97236_AUTO_MODE_1);

	return;
}

static void max97236_jack_plugged(struct max97236_priv *max97236)
{
	unsigned int status_reg[] = {0, 0, 0, 1};
	int count;

	if (!max97236_jacksw_active(max97236))
		goto max97236_jack_plugged_20;

	msleep(250);

	regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
			M97236_SHDNN_MASK,
			M97236_SHDNN_MASK);

max97236_jack_plugged_10:
	max97236_reset(max97236);
	count = M97236_DEFAULT_JACK_DETECT_DELAY;
	do {
		msleep(20);
		regmap_read(max97236->regmap, M97236_REG_00_STATUS1,
				&status_reg[0]);
	} while (((status_reg[0] & M97236_DDONE_MASK) != M97236_DDONE_MASK) &&
			--count);

	regmap_read(max97236->regmap, M97236_REG_00_STATUS1, &status_reg[0]);
	regmap_read(max97236->regmap, M97236_REG_01_STATUS2, &status_reg[1]);
	regmap_read(max97236->regmap, M97236_REG_02_STATUS3, &status_reg[2]);

	pr_info("%s: status 0x%02X, 0x%02X, count %d\n", __func__, status_reg[0], status_reg[1], count);

	max97236_report_jack_state(max97236, status_reg);

	if (!max97236_jacksw_active(max97236)) {
		goto max97236_jack_plugged_10;
	}

	if (max97236->jack_state == SND_JACK_HEADSET)
		max97236->ignore_int = 1;

max97236_jack_plugged_20:
	max97236_configure_for_detection(max97236, M97236_AUTO_MODE_1);
}

#else		/* MAX97236_AUTOMODE1_JACK_DETECTION */

#ifdef IGNORE_KEY_INT_FUNCTION_ENABLED
static void max97236_ignore_key_ints(struct max97236_priv *max97236)
{
	unsigned int status1;
	unsigned int status2;
	int triggered;
	int count;

	count = 50;
	do {
		msleep(20);
		regmap_read(max97236->regmap, M97236_REG_00_STATUS1, &status1);
		regmap_read(max97236->regmap, M97236_REG_01_STATUS2, &status2);
		triggered = (status1 & 0x02) || (status2 & 0x04);
	} while (!triggered && --count);

/*	regmap_read(max97236->regmap, M97236_REG_17_PASSIVE_MBH_KEYSCAN_DATA,
	 &status1); */

	return;
}
#endif

static void max97236_translate_detected(unsigned int *status_reg, unsigned int *force)
{
	status_reg[3] = 0x01;

	switch (status_reg[0]) {
	case 0x01:
		*force = 0x12;
		status_reg[0] = 0x84;
		status_reg[1] = 0x30;
		status_reg[2] = 0x03;
		break;
	case 0x09:
		*force = 0x03;
		status_reg[0] = 0x8C;
		status_reg[1] = 0x30;
		status_reg[2] = 0x02;
		break;
	case 0x19:
		*force = 0x02;
		status_reg[0] = 0x8C;
		status_reg[1] = 0x30;
		status_reg[2] = 0x01;
		break;
	default:
		status_reg[3] = 0x00;		/* Detection failed/timed out */

		if (status_reg[0] & 0x08) {
			*force = 0x02;			/* need MIC bias */
			status_reg[0] = 0x8C;
			status_reg[1] = 0x30;
			status_reg[2] = 0x01;
		} else {
			*force = 0x12;			/* no MIC bias needed */
			status_reg[0] = 0x84;
			status_reg[1] = 0x30;
			status_reg[2] = 0x03;
		}
		break;
	}

	return;
}

static void max97236_jack_event(struct max97236_priv *max97236)
{
	unsigned int status_reg[] = {0, 0, 0, 1};

	status_reg[0] = max97236->status0;
	regmap_read(max97236->regmap, M97236_REG_01_STATUS2, &status_reg[1]);

	/* Key press or jack removal? */
	if (((status_reg[0] & M97236_IMBH_MASK)     ||
			(status_reg[0] & M97236_IMCSW_MASK) ||
			(status_reg[1] & M97236_IKEY_MASK))
			&& (max97236_jacksw_active(max97236))) {
		max97236_keypress(max97236, status_reg);
	} else {
		if (max97236_jacksw_active(max97236))
			goto max97236_jack_event_10;
		/*
		regmap_update_bits(max97236->regmap, M97236_REG_07_LEFT_VOLUME,
				M97236_MUTEL_MASK, M97236_MUTEL_MASK);
		regmap_update_bits(max97236->regmap, M97236_REG_08_RIGHT_VOLUME,
				M97236_MUTER_MASK, M97236_MUTER_MASK);
		*/
		regmap_write(max97236->regmap, M97236_REG_19_STATE_FORCING,
				M97236_STATE_FLOAT);
			pr_info("%s: M97236_STATE_FLOAT set\n", __func__);
		regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
				M97236_SHDNN_MASK, 0);
		max97236->ignore_int = 0;
		status_reg[0] = 0;
		status_reg[1] = 0;
		status_reg[2] = 0;
		max97236_report_jack_state(max97236, status_reg);
	}

max97236_jack_event_10:
	max97236_configure_for_detection(max97236, M97236_AUTO_MODE_0);

	return;
}

static void max97236_begin_detect(struct max97236_priv *max97236, int test)
{
	regmap_write(max97236->regmap, M97236_REG_23_TEST_DATA_3, 0x00);
	regmap_write(max97236->regmap, M97236_REG_19_STATE_FORCING,
				M97236_FORCEN_MASK);
	regmap_write(max97236->regmap, M97236_REG_20_TEST_ENABLE_2, 0x80);
	switch (test) {
	case 1:
		regmap_write(max97236->regmap, M97236_REG_1F_TEST_ENABLE_1, 0x0D);
		break;
	case 2:
		regmap_write(max97236->regmap, M97236_REG_1F_TEST_ENABLE_1, 0x03);
		break;
	default:
		dev_err(max97236->codec->dev, "unknown detection test %d\n", test);
		break;
	}

	regmap_write(max97236->regmap, M97236_REG_23_TEST_DATA_3, 0x80);
}

static void max97236_end_detect(struct max97236_priv *max97236)
{
	regmap_write(max97236->regmap, M97236_REG_20_TEST_ENABLE_2, 0x00);
	regmap_write(max97236->regmap, M97236_REG_23_TEST_DATA_3, 0x00);
}

static unsigned int max97236_get_detect_result(struct max97236_priv *max97236)
{
	unsigned int reg;
	regmap_read(max97236->regmap, M97236_REG_21_TEST_DATA_1, &reg);
	return reg;
}

static void max97236_jack_plugged(struct max97236_priv *max97236)
{
	unsigned int status_reg[] = {0, 0, 0, 0};
	int retries = M97236_DEFAULT_RETRIES;
	int test_number = 1;
	int force_value = 0;
	int count;

	/* Check for spurious interrupt */
	if (!max97236_jacksw_active(max97236))
		goto max97236_jack_plugged_30;

	/* Start debounce while periodically verifying jack presence */
	/* Change debounce time to 760ms */
	for (count = 0; count < 37; count++) {
		msleep(20);
		if (!max97236_jacksw_active(max97236))
			goto max97236_jack_plugged_30;
	}

max97236_jack_plugged_10:
	if (!max97236_jacksw_active(max97236))
		goto max97236_jack_plugged_30;

	max97236_begin_detect(max97236, test_number);

	count = 10;
	do {
		msleep(20);
		regmap_read(max97236->regmap, M97236_REG_00_STATUS1,
				&status_reg[0]);
	} while (((status_reg[0] & 0x40) != 0x40) && --count);

	status_reg[0] = max97236_get_detect_result(max97236);
	max97236_translate_detected(status_reg, &force_value);
	max97236_end_detect(max97236);

	if ((status_reg[3] == 0) && --retries)
		goto max97236_jack_plugged_10;

	regmap_write(max97236->regmap, M97236_REG_19_STATE_FORCING,
		force_value);
	regmap_update_bits(max97236->regmap,
		M97236_REG_19_STATE_FORCING, 0x20, 0x00);
	regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
		M97236_SHDNN_MASK, M97236_SHDNN_MASK);
	msleep(10);

max97236_jack_plugged_20:
	max97236_report_jack_state(max97236, status_reg);
	max97236_configure_for_detection(max97236, M97236_AUTO_MODE_0);

	if (max97236->jack_state == SND_JACK_HEADSET)
#ifdef IGNORE_KEY_INT_FUNCTION_ENABLED
		max97236_ignore_key_ints(max97236);
#else
		max97236->ignore_int = 1;
#endif

	if ((max97236->jack_state == M97236_JACK_STATE_NONE) ||
		(max97236->jack_state == M97236_JACK_STATE_UNKNOWN)) {
		regmap_write(max97236->regmap,
			M97236_REG_19_STATE_FORCING, M97236_STATE_FLOAT);
	} else {
	   	if (!max97236_jacksw_active(max97236)) {
			status_reg[0] = 0;
			status_reg[1] = 0;
			goto max97236_jack_plugged_20;
		}
	}

	return;

max97236_jack_plugged_30:
	max97236_configure_for_detection(max97236, M97236_AUTO_MODE_0);
}

#endif		/* MAX97236_AUTOMODE1_JACK_DETECTION */

static void max97236_jack_work(struct work_struct *work)
{
	struct max97236_priv *max97236 =
		container_of(work, struct max97236_priv, jack_work.work);

	if ((max97236->jack_state == M97236_JACK_STATE_NONE) ||
			(max97236->jack_state == M97236_JACK_STATE_UNKNOWN))
		max97236_jack_plugged(max97236);
	else
		max97236_jack_event(max97236);
}

static irqreturn_t max97236_interrupt(int irq, void *data)
{
	struct snd_soc_codec *codec = data;
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);

 	dev_info(codec->dev, "***** max97236_interrupt *****\n");

	regmap_read(max97236->regmap, M97236_REG_00_STATUS1, &max97236->status0);

	if ((max97236->status0 & M97236_MCSW_MASK) && (max97236->ignore_int)) {
		max97236->ignore_int = 0;
	} else {
		regmap_write(max97236->regmap, M97236_REG_04_IRQ_MASK1, 0x00);
		regmap_write(max97236->regmap, M97236_REG_05_IRQ_MASK2, 0x00);
		schedule_delayed_work(&max97236->jack_work,
				msecs_to_jiffies(10));
	}

	return IRQ_HANDLED;
}

/*
 * The MAX97236 has no DAC and therefore only digital mute.  This should be
 * handled by the CODEC before the signal reaches the MAX97236, but,
 * in case it isn't, mute the 97236.
 */
static int max97236_dai_digital_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int reg = mute ? M97236_L_R_MASK | M97236_MUTEL_MASK : M97236_L_R_MASK;

	dev_info(codec->dev, "digital mute %d\n", mute);

	snd_soc_update_bits(codec, M97236_REG_07_LEFT_VOLUME,
			M97236_L_R_MASK | M97236_MUTEL_MASK, reg);

	return 0;
}

static struct snd_soc_dai_ops max97236_dai_ops = {
	.digital_mute = max97236_dai_digital_mute,
};

static struct snd_soc_dai_driver max97236_dai[] = {
{
	.name = "HiFi",
	.playback = {
		.stream_name = "HiFiPlayback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = 0,
		.formats = 0,
	},
	.capture = {
		.stream_name = "HiFiCapture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = 0,
		.formats = 0,
	},
	.ops = &max97236_dai_ops,
}
};

static void max97236_handle_pdata(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);
	struct max97236_pdata *pdata = max97236->pdata;

	if (!pdata) {
		dev_dbg(codec->dev, "No platform data\n");
		return;
	}
}

static int test_jack_presence(struct max97236_priv *max97236, int delay)
{
	unsigned int reg;
	int test_value;
	int ret = 0;

	regmap_read(max97236->regmap, M97236_REG_00_STATUS1, &reg);
#ifdef M97236_JACK_SWITCH_NORMALLY_CLOSED
	test_value = 4;
#else
	test_value = 0;
#endif

	if ((reg & M97236_JACKSW_MASK) == test_value) {
		schedule_delayed_work(&max97236->jack_work,
			msecs_to_jiffies(delay));
	} else {
		ret = 1;
		/* Clear any interrupts then enable jack detection */
		regmap_read(max97236->regmap, M97236_REG_00_STATUS1,
			&reg);
		regmap_read(max97236->regmap, M97236_REG_01_STATUS2,
			&reg);
#ifdef MAX97236_AUTOMODE1_JACK_DETECTION
		max97236_configure_for_detection(max97236,
			M97236_AUTO_MODE_1);
#else
		/* clear /FORCE bit to ensure hi-Z configures correctly */
		regmap_write(max97236->regmap, M97236_REG_19_STATE_FORCING,
				M97236_FORCEN_MASK);
		/* now set hi-Z */
		regmap_write(max97236->regmap, M97236_REG_19_STATE_FORCING,
				M97236_STATE_FLOAT);
			pr_info("%s: M97236_STATE_FLOAT set\n", __func__);
		max97236_configure_for_detection(max97236,
			M97236_AUTO_MODE_0);
#endif
	}

	return ret;
}

int max97236_mic_detect(struct snd_soc_codec *codec,
	struct snd_soc_jack *jack)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);
	int ret = -1;

	/* dev_info(codec->dev, "%s enter\n", __func__); */

	if (jack) {
		max97236->jack = jack;
		max97236->jack_state = M97236_JACK_STATE_NONE;
		test_jack_presence(max97236, 250);
		ret = 0;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(max97236_mic_detect);

static int max97236_probe(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);
	struct max97236_pdata *pdata = max97236->pdata;
	unsigned int reg;
	int ret;

#ifdef MAX97236_AUTOMODE1_JACK_DETECTION
	dev_info(codec->dev, "built on %s at %s, mode is DETECT1\n",
		__DATE__,
		__TIME__);
#else
	dev_info(codec->dev, "built on %s at %s, mode is DETECT0\n",
		__DATE__,
		__TIME__);
#endif

	dev_info(codec->dev, "build number %s\n", MAX97236_REVISION);

	codec->cache_bypass = 1;
	max97236->codec = codec;
	codec->control_data = max97236->regmap;

	ret = snd_soc_codec_set_cache_io(codec, 8, 8, SND_SOC_REGMAP);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
		goto err_access;
	}

	/* Disable all interrupts until we're ready to handle them */
	regmap_write(max97236->regmap, M97236_REG_04_IRQ_MASK1, 0x00);
	regmap_write(max97236->regmap, M97236_REG_05_IRQ_MASK2, 0x00);
	regmap_write(max97236->regmap, M97236_REG_09_MICROPHONE,
		M97236_BIAS_MASK);
	regmap_write(max97236->regmap, M97236_REG_16_KEYSCAN_DELAY, 0x18);

	ret = regmap_read(max97236->regmap, M97236_REG_0B_REV_ID, &reg);
	if (ret < 0) {
		dev_err(codec->dev, "Cannot read device version: %d\n", ret);
		goto err_access;
	}

	reg >>= M97236_ID_SHIFT;
	dev_info(codec->dev, "MAX97236 ID = 0x%02x\n", reg);

	if (reg == M97236_REVA) {
		max97236->devtype = MAX97236;
	} else {
		dev_err(codec->dev, "Unrecognized device 0x%02X\n", reg);
		ret = -1;
		goto err_access;
	}

	/* configure clock divider registers */
	max97236_set_clk_dividers(max97236, extclk_freq);

        if (gpio_is_valid(pdata->irq_gpio)) {
                ret = gpio_request(pdata->irq_gpio, "max97236_interrupt");
                if (ret) {
                        dev_err(codec->dev,
                                "%s: Failed to request max97236_interrupt GPIO, rc=%d\n",
                                __func__, ret);
                }

                ret = gpio_direction_input(pdata->irq_gpio);
                if (ret) {
                        dev_err(codec->dev,
                                "%s: Failed to configure max97236_interrupt GPIO, rc=%d\n",
                                __func__, ret);
				gpio_free(pdata->irq_gpio);
                }

                max97236->irq = gpio_to_irq(pdata->irq_gpio);
                dev_info(codec->dev,"Setting irq: GPIO %d -> irq %d\n",
                                 pdata->irq_gpio, max97236->irq);

	        if ((request_threaded_irq(max97236->irq, NULL, max97236_interrupt,
	        	IRQF_TRIGGER_FALLING|IRQF_ONESHOT, "max97236_interrupt", codec)) < 0) {
	        	dev_err(codec->dev, "request_irq failed\n");
	        	ret = -1;
	        	goto err_access;
	        }
        }

	max97236->ignore_int = 0;

	INIT_DELAYED_WORK(&max97236->jack_work, max97236_jack_work);

        if (pdata->vreg) {
                ret = regulator_set_voltage(pdata->vreg, 2850000, 2850000);
                if (ret) {
                         dev_err(codec->dev, "Setting regulator voltage failed for "
                                 "regulator err = %d\n", ret);
                 }

                 ret = regulator_set_optimum_mode(pdata->vreg, 150);
                 if (ret < 0) {
                         dev_err(codec->dev, "Setting regulator optimum mode failed for "
                                 "regulator err = %d\n", ret);
                 }

                 ret = regulator_enable(pdata->vreg);
                 if (ret) {
                           dev_err(codec->dev, "failed to enable supply err = %d\n", ret);
                 }
        }

	ret = snd_soc_jack_new(codec, "JACK",
			SND_JACK_HEADSET | SND_JACK_LINEOUT,
			&max97236_headset);
	if (ret)
		dev_err(codec->dev,"install jack error return = %d\n", ret);
	else {
		dev_info(codec->dev,"install jack successful\n");
		snd_jack_set_key(max97236_headset.jack, SND_JACK_BTN_0 | SND_JACK_BTN_2 | SND_JACK_BTN_4, KEY_MEDIA);
		max97236_mic_detect(codec, &max97236_headset);
	}

	max97236_handle_pdata(codec);
	max97236_add_widgets(codec);

err_access:
	return ret;
}

static int max97236_remove(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);

	cancel_delayed_work_sync(&max97236->jack_work);

	return 0;
}

#ifdef CONFIG_PM
static int max97236_soc_suspend(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);


	dev_info(max97236->codec->dev, "soc suspend\n");
	msleep(3000);

	regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
			M97236_SHDNN_MASK, 0);

	return 0;
}

static int max97236_soc_resume(struct snd_soc_codec *codec)
{
	struct max97236_priv *max97236 = snd_soc_codec_get_drvdata(codec);


	dev_info(max97236->codec->dev, "soc resume\n");
	msleep(3000);

	test_jack_presence(max97236, 10);

	return 0;
}
#else
#define max97236_soc_suspend NULL
#define max97236_soc_resume NULL
#endif

static struct snd_soc_codec_driver soc_codec_dev_max97236 = {
	.probe = max97236_probe,
	.remove = max97236_remove,

	.reg_cache_size = M97236_REG_CNT,
	.reg_word_size = 1,

	.set_bias_level = max97236_set_bias_level,
	.suspend = max97236_soc_suspend,
	.resume = max97236_soc_resume,
};

static const struct regmap_config max97236_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = MAX97236_MAX_REGISTER,
	.reg_defaults = max97236_reg,
	.num_reg_defaults = ARRAY_SIZE(max97236_reg),
	.volatile_reg = max97236_volatile_register,
	.readable_reg = max97236_readable_register,
	.cache_type = REGCACHE_RBTREE,
};

static struct max97236_pdata *max97236_of_pdata(struct i2c_client *i2c)
{
	struct max97236_pdata *pdata;
	struct device_node *dn = i2c->dev.of_node;

	pdata = devm_kzalloc(&i2c->dev, sizeof(struct max97236_pdata),
			     GFP_KERNEL);
	if (!pdata)
		return NULL;

	pdata->irq_gpio = of_get_named_gpio(dn, "maxim,irq-gpio", 0);

	if (pdata->irq_gpio) {
		dev_info(&i2c->dev, "max97236: GPIO %d\n", pdata->irq_gpio);
	} else {
		dev_err(&i2c->dev, "max97236: No GPIO value found in dn\n");
	}

        pdata->vreg = devm_regulator_get(&i2c->dev, "vdd_hp_mux");
        if (IS_ERR(pdata->vreg)) {
                dev_err(&i2c->dev, "max97236: failed to get vdd hp supply");
                pdata->vreg = NULL;
        }

	return pdata;
}

static int max97236_i2c_probe(struct i2c_client *i2c,
				 const struct i2c_device_id *id)
{
	struct max97236_priv *max97236;
	int ret;

	max97236 = kzalloc(sizeof(struct max97236_priv), GFP_KERNEL);
	if (max97236 == NULL)
		return -ENOMEM;

	max97236->devtype = id->driver_data;
	i2c_set_clientdata(i2c, max97236);
	max97236->control_data = i2c;
	max97236->pdata = i2c->dev.platform_data;

	if (!max97236->pdata && i2c->dev.of_node)
		max97236->pdata = max97236_of_pdata(i2c);

	max97236->regmap = regmap_init_i2c(i2c, &max97236_regmap);
	if (IS_ERR(max97236->regmap)) {
		ret = PTR_ERR(max97236->regmap);
		dev_err(&i2c->dev, "Failed to allocate regmap: %d\n", ret);
		goto err_enable;
	}

	regcache_cache_bypass(max97236->regmap, false);

	ret = snd_soc_register_codec(&i2c->dev,
			&soc_codec_dev_max97236, max97236_dai,
			ARRAY_SIZE(max97236_dai));

	if (ret < 0)
		regmap_exit(max97236->regmap);

err_enable:
	return ret;
}

static int max97236_i2c_remove(struct i2c_client *client)
{
	struct max97236_priv *max97236 = dev_get_drvdata(&client->dev);
	snd_soc_unregister_codec(&client->dev);
	regmap_exit(max97236->regmap);
	kfree(i2c_get_clientdata(client));
	return 0;
}

static int max97236_pm_suspend(struct device *dev)
{
	struct max97236_priv *max97236 = dev_get_drvdata(dev);

	dev_info(max97236->codec->dev, "suspend enter\n");


	/* set IRQ enables */
	regmap_write(max97236->regmap, M97236_REG_04_IRQ_MASK1,
			M97236_IJACKSW_MASK);
	regmap_write(max97236->regmap, M97236_REG_05_IRQ_MASK2, 0);
	regmap_update_bits(max97236->regmap, M97236_REG_1D_ENABLE_1,
			M97236_SHDNN_MASK, 0);

	/* force hi-Z */
	regmap_write(max97236->regmap,
		M97236_REG_19_STATE_FORCING, M97236_STATE_FLOAT);

	regcache_cache_only(max97236->regmap, true);

	return 0;
}

static int max97236_pm_resume(struct device *dev)
{
	struct max97236_priv *max97236 = dev_get_drvdata(dev);

	cancel_delayed_work(&max97236->jack_work);

	regcache_cache_only(max97236->regmap, false);
	/* max97236_reset(max97236); */
	regcache_sync(max97236->regmap);

	max97236_set_clk_dividers(max97236, extclk_freq);

	max97236->jack_state = M97236_JACK_STATE_NONE;
	if (test_jack_presence(max97236, 10))
		snd_soc_jack_report(max97236->jack, max97236->jack_state, SND_JACK_HEADSET | SND_JACK_LINEOUT);

	dev_info(max97236->codec->dev, "resume exit\n");

	return 0;
}

static struct dev_pm_ops max97236_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(max97236_pm_suspend, max97236_pm_resume)
};

static const struct i2c_device_id max97236_i2c_id[] = {
	{ "max97236", MAX97236 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max97236_i2c_id);

static struct i2c_driver max97236_i2c_driver = {
	.driver = {
		.name = "max97236",
		.owner = THIS_MODULE,
		.pm = &max97236_pm,
	},
	.probe  = max97236_i2c_probe,
	.remove = max97236_i2c_remove,
	.id_table = max97236_i2c_id,
};

module_i2c_driver(max97236_i2c_driver);

MODULE_DESCRIPTION("ALSA SoC MAX97236 driver");
MODULE_AUTHOR("Ralph Birt <rdbirt@gmail.com>");
MODULE_LICENSE("GPL");
