// SPDX-License-Identifier: GPL-2.0
/*
 * Omnivision OV9281 1280x800 global shutter image sensor driver
 *
 * This driver has been taken from
 * https://github.com/rockchip-linux/kernel/blob/develop-4.4/drivers/media/i2c/ov9281.c
 * cleaned up, made to compile against mainline kernels instead of the Rockchip
 * vendor kernel, and the relevant controls added to work with libcamera.
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 * V0.0X01.0X02 fix mclk issue when probe multiple camera.
 * V0.0X01.0X03 add enum_frame_interval function.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#define OV9281_LINK_FREQ_400MHZ		400000000
#define OV9281_LANES			2
#define OV9281_BITS_PER_SAMPLE		10

/* pixel rate = link frequency * 2 * lanes / BITS_PER_SAMPLE */
#define OV9281_PIXEL_RATE		(OV9281_LINK_FREQ_400MHZ * 2 * \
					 OV9281_LANES / OV9281_BITS_PER_SAMPLE)
#define OV9281_XVCLK_FREQ		24000000

#define CHIP_ID				0x9281
#define OV9281_REG_CHIP_ID		0x300a

#define OV9281_REG_CTRL_MODE		0x0100
#define OV9281_MODE_SW_STANDBY		0x0
#define OV9281_MODE_STREAMING		BIT(0)

#define OV9281_REG_EXPOSURE		0x3500
#define	OV9281_EXPOSURE_MIN		4
#define	OV9281_EXPOSURE_STEP		1
#define OV9281_VTS_MAX			0x7fff

#define OV9281_REG_GAIN_H		0x3508
#define OV9281_REG_GAIN_L		0x3509
#define OV9281_GAIN_H_MASK		0x07
#define OV9281_GAIN_H_SHIFT		8
#define OV9281_GAIN_L_MASK		0xff
#define OV9281_GAIN_MIN			0x10
#define OV9281_GAIN_MAX			0xf8
#define OV9281_GAIN_STEP		1
#define OV9281_GAIN_DEFAULT		0x10

#define OV9281_REG_TEST_PATTERN		0x5e00
#define OV9281_TEST_PATTERN_ENABLE	0x80
#define OV9281_TEST_PATTERN_DISABLE	0x0

#define OV9281_REG_VTS			0x380e

/*
 * OV9281 native and active pixel array size.
 * Datasheet not available to confirm these values, so assume there are no
 * border pixels.
 */
#define OV9281_NATIVE_WIDTH		1280U
#define OV9281_NATIVE_HEIGHT		800U
#define OV9281_PIXEL_ARRAY_LEFT		0U
#define OV9281_PIXEL_ARRAY_TOP		0U
#define OV9281_PIXEL_ARRAY_WIDTH	1280U
#define OV9281_PIXEL_ARRAY_HEIGHT	800U

#define REG_NULL			0xFFFF

#define OV9281_REG_VALUE_08BIT		1
#define OV9281_REG_VALUE_16BIT		2
#define OV9281_REG_VALUE_24BIT		3

#define OV9281_NAME			"ov9281"

static const char * const ov9281_supply_names[] = {
	"avdd",		/* Analog power */
	"dovdd",	/* Digital I/O power */
	"dvdd",		/* Digital core power */
};

#define OV9281_NUM_SUPPLIES ARRAY_SIZE(ov9281_supply_names)

struct regval {
	u16 addr;
	u8 val;
};

struct ov9281_mode {
	u32 width;
	u32 height;
	u32 hts_def;
	u32 vts_def;
	u32 exp_def;
	struct v4l2_rect crop;
	const struct regval *reg_list;
};

struct ov9281 {
	struct i2c_client	*client;
	struct clk		*xvclk;
	struct gpio_desc	*reset_gpio;
	struct gpio_desc	*pwdn_gpio;
	struct regulator_bulk_data supplies[OV9281_NUM_SUPPLIES];

	struct v4l2_subdev	subdev;
	struct media_pad	pad;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl	*exposure;
	struct v4l2_ctrl	*anal_gain;
	struct v4l2_ctrl	*digi_gain;
	struct v4l2_ctrl	*hblank;
	struct v4l2_ctrl	*vblank;
	struct v4l2_ctrl	*test_pattern;
	struct mutex		mutex;
	bool			streaming;
	bool			power_on;
	const struct ov9281_mode *cur_mode;
};

#define to_ov9281(sd) container_of(sd, struct ov9281, subdev)

/*
 * Xclk 24Mhz
 * max_framerate 120fps
 * mipi_datarate per lane 800Mbps
 */
static const struct regval ov9281_1280x800_regs[] = {
	{0x0103, 0x01},
	{0x0302, 0x32},
	{0x030d, 0x50},
	{0x030e, 0x02},
	{0x3001, 0x00},
	{0x3004, 0x00},
	{0x3005, 0x00},
	{0x3006, 0x04},
	{0x3011, 0x0a},
	{0x3013, 0x18},
	{0x3022, 0x01},
	{0x3023, 0x00},
	{0x302c, 0x00},
	{0x302f, 0x00},
	{0x3030, 0x04},
	{0x3039, 0x32},
	{0x303a, 0x00},
	{0x303f, 0x01},
	{0x3500, 0x00},
	{0x3501, 0x2a},
	{0x3502, 0x90},
	{0x3503, 0x08},
	{0x3505, 0x8c},
	{0x3507, 0x03},
	{0x3508, 0x00},
	{0x3509, 0x10},
	{0x3610, 0x80},
	{0x3611, 0xa0},
	{0x3620, 0x6f},
	{0x3632, 0x56},
	{0x3633, 0x78},
	{0x3662, 0x05},
	{0x3666, 0x00},
	{0x366f, 0x5a},
	{0x3680, 0x84},
	{0x3712, 0x80},
	{0x372d, 0x22},
	{0x3731, 0x80},
	{0x3732, 0x30},
	{0x3778, 0x00},
	{0x377d, 0x22},
	{0x3788, 0x02},
	{0x3789, 0xa4},
	{0x378a, 0x00},
	{0x378b, 0x4a},
	{0x3799, 0x20},
	{0x3800, 0x00},
	{0x3801, 0x00},
	{0x3802, 0x00},
	{0x3803, 0x00},
	{0x3804, 0x05},
	{0x3805, 0x0f},
	{0x3806, 0x03},
	{0x3807, 0x2f},
	{0x3808, 0x05},
	{0x3809, 0x00},
	{0x380a, 0x03},
	{0x380b, 0x20},
	{0x380c, 0x02},
	{0x380d, 0xd8},
	{0x380e, 0x03},
	{0x380f, 0x8e},
	{0x3810, 0x00},
	{0x3811, 0x08},
	{0x3812, 0x00},
	{0x3813, 0x08},
	{0x3814, 0x11},
	{0x3815, 0x11},
	{0x3820, 0x40},
	{0x3821, 0x00},
	{0x3881, 0x42},
	{0x38b1, 0x00},
	{0x3920, 0xff},
	{0x4003, 0x40},
	{0x4008, 0x04},
	{0x4009, 0x0b},
	{0x400c, 0x00},
	{0x400d, 0x07},
	{0x4010, 0x40},
	{0x4043, 0x40},
	{0x4307, 0x30},
	{0x4317, 0x00},
	{0x4501, 0x00},
	{0x4507, 0x00},
	{0x4509, 0x00},
	{0x450a, 0x08},
	{0x4601, 0x04},
	{0x470f, 0x00},
	{0x4f07, 0x00},
	{0x4800, 0x00},
	{0x5000, 0x9f},
	{0x5001, 0x00},
	{0x5e00, 0x00},
	{0x5d00, 0x07},
	{0x5d01, 0x00},
	{REG_NULL, 0x00},
};

static const struct ov9281_mode supported_modes[] = {
	{
		.width = 1280,
		.height = 800,
		.exp_def = 0x0320,
		.hts_def = 0x05b0,	/* 0x2d8*2 */
		.vts_def = 0x038e,
		.crop = {
			.left = 0,
			.top = 0,
			.width = 1280,
			.height = 800
		},
		.reg_list = ov9281_1280x800_regs,
	},
};

static const s64 link_freq_menu_items[] = {
	OV9281_LINK_FREQ_400MHZ
};

static const char * const ov9281_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bar Type 1",
	"Vertical Color Bar Type 2",
	"Vertical Color Bar Type 3",
	"Vertical Color Bar Type 4"
};

/* Write registers up to 4 at a time */
static int ov9281_write_reg(struct i2c_client *client, u16 reg,
			    u32 len, u32 val)
{
	u32 buf_i, val_i;
	u8 buf[6];
	u8 *val_p;
	__be32 val_be;

	if (len > 4)
		return -EINVAL;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	val_be = cpu_to_be32(val);
	val_p = (u8 *)&val_be;
	buf_i = 2;
	val_i = 4 - len;

	while (val_i < 4)
		buf[buf_i++] = val_p[val_i++];

	if (i2c_master_send(client, buf, len + 2) != len + 2)
		return -EIO;

	return 0;
}

static int ov9281_write_array(struct i2c_client *client,
			      const struct regval *regs)
{
	u32 i;
	int ret = 0;

	for (i = 0; ret == 0 && regs[i].addr != REG_NULL; i++)
		ret = ov9281_write_reg(client, regs[i].addr,
				       OV9281_REG_VALUE_08BIT, regs[i].val);

	return ret;
}

/* Read registers up to 4 at a time */
static int ov9281_read_reg(struct i2c_client *client, u16 reg, unsigned int len,
			   u32 *val)
{
	struct i2c_msg msgs[2];
	u8 *data_be_p;
	__be32 data_be = 0;
	__be16 reg_addr_be = cpu_to_be16(reg);
	int ret;

	if (len > 4 || !len)
		return -EINVAL;

	data_be_p = (u8 *)&data_be;
	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 2;
	msgs[0].buf = (u8 *)&reg_addr_be;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_be_p[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = be32_to_cpu(data_be);

	return 0;
}

static int ov9281_get_reso_dist(const struct ov9281_mode *mode,
				struct v4l2_mbus_framefmt *framefmt)
{
	return abs(mode->width - framefmt->width) +
	       abs(mode->height - framefmt->height);
}

static const struct ov9281_mode *
ov9281_find_best_fit(struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;
	int dist;
	int cur_best_fit = 0;
	int cur_best_fit_dist = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		dist = ov9281_get_reso_dist(&supported_modes[i], framefmt);
		if (cur_best_fit_dist == -1 || dist < cur_best_fit_dist) {
			cur_best_fit_dist = dist;
			cur_best_fit = i;
		}
	}

	return &supported_modes[cur_best_fit];
}

static int ov9281_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ov9281 *ov9281 = to_ov9281(sd);
	const struct ov9281_mode *mode;
	s64 h_blank, vblank_def;

	mutex_lock(&ov9281->mutex);

	mode = ov9281_find_best_fit(fmt);
	fmt->format.code = MEDIA_BUS_FMT_Y10_1X10;
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;
	fmt->format.ycbcr_enc =
			V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->format.colorspace);
	fmt->format.quantization =
		V4L2_MAP_QUANTIZATION_DEFAULT(true, fmt->format.colorspace,
					      fmt->format.ycbcr_enc);
	fmt->format.xfer_func =
		V4L2_MAP_XFER_FUNC_DEFAULT(fmt->format.colorspace);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		*v4l2_subdev_get_try_format(sd, cfg, fmt->pad) = fmt->format;
	} else {
		ov9281->cur_mode = mode;
		h_blank = mode->hts_def - mode->width;
		__v4l2_ctrl_modify_range(ov9281->hblank, h_blank,
					 h_blank, 1, h_blank);
		__v4l2_ctrl_s_ctrl(ov9281->hblank, h_blank);
		vblank_def = mode->vts_def - mode->height;
		__v4l2_ctrl_modify_range(ov9281->vblank, vblank_def,
					 OV9281_VTS_MAX - mode->height,
					 1, vblank_def);
		__v4l2_ctrl_s_ctrl(ov9281->vblank, vblank_def);
	}

	mutex_unlock(&ov9281->mutex);

	return 0;
}

static int ov9281_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct ov9281 *ov9281 = to_ov9281(sd);
	const struct ov9281_mode *mode = ov9281->cur_mode;

	mutex_lock(&ov9281->mutex);
	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		fmt->format = *v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
	} else {
		fmt->format.width = mode->width;
		fmt->format.height = mode->height;
		fmt->format.code = MEDIA_BUS_FMT_Y10_1X10;
		fmt->format.field = V4L2_FIELD_NONE;
		fmt->format.colorspace = V4L2_COLORSPACE_SRGB;
		fmt->format.ycbcr_enc =
			V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->format.colorspace);
		fmt->format.quantization =
			V4L2_MAP_QUANTIZATION_DEFAULT(true,
						      fmt->format.colorspace,
						      fmt->format.ycbcr_enc);
		fmt->format.xfer_func =
			V4L2_MAP_XFER_FUNC_DEFAULT(fmt->format.colorspace);
	}
	mutex_unlock(&ov9281->mutex);

	return 0;
}

static int ov9281_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_pad_config *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_Y10_1X10;

	return 0;
}

static int ov9281_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_pad_config *cfg,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != MEDIA_BUS_FMT_Y10_1X10)
		return -EINVAL;

	fse->min_width  = supported_modes[fse->index].width;
	fse->max_width  = supported_modes[fse->index].width;
	fse->max_height = supported_modes[fse->index].height;
	fse->min_height = supported_modes[fse->index].height;

	return 0;
}

static int ov9281_enable_test_pattern(struct ov9281 *ov9281, u32 pattern)
{
	u32 val;

	if (pattern)
		val = (pattern - 1) | OV9281_TEST_PATTERN_ENABLE;
	else
		val = OV9281_TEST_PATTERN_DISABLE;

	return ov9281_write_reg(ov9281->client, OV9281_REG_TEST_PATTERN,
				OV9281_REG_VALUE_08BIT, val);
}

static const struct v4l2_rect *
__ov9281_get_pad_crop(struct ov9281 *ov9281, struct v4l2_subdev_pad_config *cfg,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&ov9281->subdev, cfg, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ov9281->cur_mode->crop;
	}

	return NULL;
}

static int ov9281_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_pad_config *cfg,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP: {
		struct ov9281 *ov9281 = to_ov9281(sd);

		mutex_lock(&ov9281->mutex);
		sel->r = *__ov9281_get_pad_crop(ov9281, cfg, sel->pad,
						sel->which);
		mutex_unlock(&ov9281->mutex);

		return 0;
	}

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = OV9281_NATIVE_WIDTH;
		sel->r.height = OV9281_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = OV9281_PIXEL_ARRAY_TOP;
		sel->r.left = OV9281_PIXEL_ARRAY_LEFT;
		sel->r.width = OV9281_PIXEL_ARRAY_WIDTH;
		sel->r.height = OV9281_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

static int __ov9281_start_stream(struct ov9281 *ov9281)
{
	int ret;

	ret = ov9281_write_array(ov9281->client, ov9281->cur_mode->reg_list);
	if (ret)
		return ret;

	/* In case these controls are set before streaming */
	mutex_unlock(&ov9281->mutex);
	ret = v4l2_ctrl_handler_setup(&ov9281->ctrl_handler);
	mutex_lock(&ov9281->mutex);
	if (ret)
		return ret;

	return ov9281_write_reg(ov9281->client, OV9281_REG_CTRL_MODE,
				OV9281_REG_VALUE_08BIT, OV9281_MODE_STREAMING);
}

static int __ov9281_stop_stream(struct ov9281 *ov9281)
{
	return ov9281_write_reg(ov9281->client, OV9281_REG_CTRL_MODE,
				OV9281_REG_VALUE_08BIT, OV9281_MODE_SW_STANDBY);
}

static int ov9281_s_stream(struct v4l2_subdev *sd, int on)
{
	struct ov9281 *ov9281 = to_ov9281(sd);
	struct i2c_client *client = ov9281->client;
	int ret = 0;

	mutex_lock(&ov9281->mutex);
	on = !!on;
	if (on == ov9281->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = __ov9281_start_stream(ov9281);
		if (ret) {
			v4l2_err(sd, "start stream failed while write regs\n");
			pm_runtime_put(&client->dev);
			goto unlock_and_return;
		}
	} else {
		__ov9281_stop_stream(ov9281);
		pm_runtime_put(&client->dev);
	}

	ov9281->streaming = on;

unlock_and_return:
	mutex_unlock(&ov9281->mutex);

	return ret;
}

static int ov9281_s_power(struct v4l2_subdev *sd, int on)
{
	struct ov9281 *ov9281 = to_ov9281(sd);
	struct i2c_client *client = ov9281->client;
	int ret = 0;

	mutex_lock(&ov9281->mutex);

	/* If the power state is not modified - no work to do. */
	if (ov9281->power_on == !!on)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}
		ov9281->power_on = true;
	} else {
		pm_runtime_put(&client->dev);
		ov9281->power_on = false;
	}

unlock_and_return:
	mutex_unlock(&ov9281->mutex);

	return ret;
}

/* Calculate the delay in us by clock rate and clock cycles */
static inline u32 ov9281_cal_delay(u32 cycles)
{
	return DIV_ROUND_UP(cycles, OV9281_XVCLK_FREQ / 1000 / 1000);
}

static int __ov9281_power_on(struct ov9281 *ov9281)
{
	int ret;
	u32 delay_us;
	struct device *dev = &ov9281->client->dev;

	ret = clk_set_rate(ov9281->xvclk, OV9281_XVCLK_FREQ);
	if (ret < 0)
		dev_warn(dev, "Failed to set xvclk rate (24MHz)\n");
	if (clk_get_rate(ov9281->xvclk) != OV9281_XVCLK_FREQ)
		dev_warn(dev, "xvclk mismatched, modes are based on 24MHz - rate is %lu\n",
			 clk_get_rate(ov9281->xvclk));

	ret = clk_prepare_enable(ov9281->xvclk);
	if (ret < 0) {
		dev_err(dev, "Failed to enable xvclk\n");
		return ret;
	}

	if (!IS_ERR(ov9281->reset_gpio))
		gpiod_set_value_cansleep(ov9281->reset_gpio, 0);

	ret = regulator_bulk_enable(OV9281_NUM_SUPPLIES, ov9281->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators\n");
		goto disable_clk;
	}

	if (!IS_ERR(ov9281->reset_gpio))
		gpiod_set_value_cansleep(ov9281->reset_gpio, 1);

	usleep_range(500, 1000);
	if (!IS_ERR(ov9281->pwdn_gpio))
		gpiod_set_value_cansleep(ov9281->pwdn_gpio, 1);

	/* 8192 cycles prior to first SCCB transaction */
	delay_us = ov9281_cal_delay(8192);
	usleep_range(delay_us, delay_us * 2);

	return 0;

disable_clk:
	clk_disable_unprepare(ov9281->xvclk);

	return ret;
}

static void __ov9281_power_off(struct ov9281 *ov9281)
{
	if (!IS_ERR(ov9281->pwdn_gpio))
		gpiod_set_value_cansleep(ov9281->pwdn_gpio, 0);
	clk_disable_unprepare(ov9281->xvclk);
	if (!IS_ERR(ov9281->reset_gpio))
		gpiod_set_value_cansleep(ov9281->reset_gpio, 0);
	regulator_bulk_disable(OV9281_NUM_SUPPLIES, ov9281->supplies);
}

static int ov9281_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9281 *ov9281 = to_ov9281(sd);

	return __ov9281_power_on(ov9281);
}

static int ov9281_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9281 *ov9281 = to_ov9281(sd);

	__ov9281_power_off(ov9281);

	return 0;
}

static int ov9281_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct ov9281 *ov9281 = to_ov9281(sd);
	struct v4l2_mbus_framefmt *try_fmt =
				v4l2_subdev_get_try_format(sd, fh->pad, 0);
	const struct ov9281_mode *def_mode = &supported_modes[0];

	mutex_lock(&ov9281->mutex);
	/* Initialize try_fmt */
	try_fmt->width = def_mode->width;
	try_fmt->height = def_mode->height;
	try_fmt->code = MEDIA_BUS_FMT_Y10_1X10;
	try_fmt->field = V4L2_FIELD_NONE;
	try_fmt->colorspace = V4L2_COLORSPACE_SRGB;
	try_fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(try_fmt->colorspace);
	try_fmt->quantization =
		V4L2_MAP_QUANTIZATION_DEFAULT(true, try_fmt->colorspace,
					      try_fmt->ycbcr_enc);
	try_fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(try_fmt->colorspace);

	mutex_unlock(&ov9281->mutex);
	/* No crop or compose */

	return 0;
}

static const struct dev_pm_ops ov9281_pm_ops = {
	SET_RUNTIME_PM_OPS(ov9281_runtime_suspend,
			   ov9281_runtime_resume, NULL)
};

static const struct v4l2_subdev_internal_ops ov9281_internal_ops = {
	.open = ov9281_open,
};

static const struct v4l2_subdev_core_ops ov9281_core_ops = {
	.s_power = ov9281_s_power,
};

static const struct v4l2_subdev_video_ops ov9281_video_ops = {
	.s_stream = ov9281_s_stream,
};

static const struct v4l2_subdev_pad_ops ov9281_pad_ops = {
	.enum_mbus_code = ov9281_enum_mbus_code,
	.enum_frame_size = ov9281_enum_frame_sizes,
	.get_fmt = ov9281_get_fmt,
	.set_fmt = ov9281_set_fmt,
	.get_selection = ov9281_get_selection,
};

static const struct v4l2_subdev_ops ov9281_subdev_ops = {
	.core	= &ov9281_core_ops,
	.video	= &ov9281_video_ops,
	.pad	= &ov9281_pad_ops,
};

static int ov9281_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov9281 *ov9281 = container_of(ctrl->handler,
					     struct ov9281, ctrl_handler);
	struct i2c_client *client = ov9281->client;
	s64 max;
	int ret = 0;

	/* Propagate change of current control to all related controls */
	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		/* Update max exposure while meeting expected vblanking */
		max = ov9281->cur_mode->height + ctrl->val - 4;
		__v4l2_ctrl_modify_range(ov9281->exposure,
					 ov9281->exposure->minimum, max,
					 ov9281->exposure->step,
					 ov9281->exposure->default_value);
		break;
	}

	if (pm_runtime_get(&client->dev) <= 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		/* 4 least significant bits of expsoure are fractional part */
		ret = ov9281_write_reg(ov9281->client, OV9281_REG_EXPOSURE,
				       OV9281_REG_VALUE_24BIT, ctrl->val << 4);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = ov9281_write_reg(ov9281->client, OV9281_REG_GAIN_H,
				       OV9281_REG_VALUE_08BIT,
				       (ctrl->val >> OV9281_GAIN_H_SHIFT) &
							OV9281_GAIN_H_MASK);
		ret |= ov9281_write_reg(ov9281->client, OV9281_REG_GAIN_L,
				       OV9281_REG_VALUE_08BIT,
				       ctrl->val & OV9281_GAIN_L_MASK);
		break;
	case V4L2_CID_VBLANK:
		ret = ov9281_write_reg(ov9281->client, OV9281_REG_VTS,
				       OV9281_REG_VALUE_16BIT,
				       ctrl->val + ov9281->cur_mode->height);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov9281_enable_test_pattern(ov9281, ctrl->val);
		break;
	default:
		dev_warn(&client->dev, "%s Unhandled id:0x%x, val:0x%x\n",
			 __func__, ctrl->id, ctrl->val);
		break;
	}

	pm_runtime_put(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops ov9281_ctrl_ops = {
	.s_ctrl = ov9281_set_ctrl,
};

static int ov9281_initialize_controls(struct ov9281 *ov9281)
{
	const struct ov9281_mode *mode;
	struct v4l2_ctrl_handler *handler;
	struct v4l2_ctrl *ctrl;
	s64 exposure_max, vblank_def;
	u32 h_blank;
	int ret;

	handler = &ov9281->ctrl_handler;
	mode = ov9281->cur_mode;
	ret = v4l2_ctrl_handler_init(handler, 8);
	if (ret)
		return ret;
	handler->lock = &ov9281->mutex;

	ctrl = v4l2_ctrl_new_int_menu(handler, NULL, V4L2_CID_LINK_FREQ,
				      0, 0, link_freq_menu_items);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_new_std(handler, NULL, V4L2_CID_PIXEL_RATE,
			  0, OV9281_PIXEL_RATE, 1, OV9281_PIXEL_RATE);

	h_blank = mode->hts_def - mode->width;
	ov9281->hblank = v4l2_ctrl_new_std(handler, NULL, V4L2_CID_HBLANK,
					   h_blank, h_blank, 1, h_blank);
	if (ov9281->hblank)
		ov9281->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank_def = mode->vts_def - mode->height;
	ov9281->vblank = v4l2_ctrl_new_std(handler, &ov9281_ctrl_ops,
					   V4L2_CID_VBLANK, vblank_def,
					   OV9281_VTS_MAX - mode->height, 1,
					   vblank_def);

	exposure_max = mode->vts_def - 4;
	ov9281->exposure = v4l2_ctrl_new_std(handler, &ov9281_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     OV9281_EXPOSURE_MIN, exposure_max,
					     OV9281_EXPOSURE_STEP,
					     mode->exp_def);

	ov9281->anal_gain = v4l2_ctrl_new_std(handler, &ov9281_ctrl_ops,
					      V4L2_CID_ANALOGUE_GAIN,
					      OV9281_GAIN_MIN, OV9281_GAIN_MAX,
					      OV9281_GAIN_STEP,
					      OV9281_GAIN_DEFAULT);

	ov9281->test_pattern =
		v4l2_ctrl_new_std_menu_items(handler, &ov9281_ctrl_ops,
					     V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(ov9281_test_pattern_menu) - 1,
					     0, 0, ov9281_test_pattern_menu);

	if (handler->error) {
		ret = handler->error;
		dev_err(&ov9281->client->dev,
			"Failed to init controls(%d)\n", ret);
		goto err_free_handler;
	}

	ov9281->subdev.ctrl_handler = handler;

	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(handler);

	return ret;
}

static int ov9281_check_sensor_id(struct ov9281 *ov9281,
				  struct i2c_client *client)
{
	struct device *dev = &ov9281->client->dev;
	u32 id = 0, id_msb;
	int ret;

	ret = ov9281_read_reg(client, OV9281_REG_CHIP_ID + 1,
			      OV9281_REG_VALUE_08BIT, &id);
	if (!ret)
		ret = ov9281_read_reg(client, OV9281_REG_CHIP_ID,
				      OV9281_REG_VALUE_08BIT, &id_msb);
	id |= (id_msb << 8);
	if (ret || id != CHIP_ID) {
		dev_err(dev, "Unexpected sensor id(%04x), ret(%d)\n", id, ret);
		return -ENODEV;
	}

	dev_info(dev, "Detected OV%06x sensor\n", CHIP_ID);

	return 0;
}

static int ov9281_configure_regulators(struct ov9281 *ov9281)
{
	unsigned int i;

	for (i = 0; i < OV9281_NUM_SUPPLIES; i++)
		ov9281->supplies[i].supply = ov9281_supply_names[i];

	return devm_regulator_bulk_get(&ov9281->client->dev,
				       OV9281_NUM_SUPPLIES,
				       ov9281->supplies);
}

static int ov9281_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ov9281 *ov9281;
	struct v4l2_subdev *sd;
	int ret;

	ov9281 = devm_kzalloc(dev, sizeof(*ov9281), GFP_KERNEL);
	if (!ov9281)
		return -ENOMEM;

	ov9281->client = client;
	ov9281->cur_mode = &supported_modes[0];

	ov9281->xvclk = devm_clk_get(dev, "xvclk");
	if (IS_ERR(ov9281->xvclk)) {
		dev_err(dev, "Failed to get xvclk\n");
		return -EINVAL;
	}

	ov9281->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_LOW);
	if (IS_ERR(ov9281->reset_gpio))
		dev_warn(dev, "Failed to get reset-gpios\n");

	ov9281->pwdn_gpio = devm_gpiod_get_optional(dev, "pwdn", GPIOD_OUT_LOW);
	if (IS_ERR(ov9281->pwdn_gpio))
		dev_warn(dev, "Failed to get pwdn-gpios\n");

	ret = ov9281_configure_regulators(ov9281);
	if (ret) {
		dev_err(dev, "Failed to get power regulators\n");
		return ret;
	}

	mutex_init(&ov9281->mutex);

	sd = &ov9281->subdev;
	v4l2_i2c_subdev_init(sd, client, &ov9281_subdev_ops);
	ret = ov9281_initialize_controls(ov9281);
	if (ret)
		goto err_destroy_mutex;

	ret = __ov9281_power_on(ov9281);
	if (ret)
		goto err_free_handler;

	ret = ov9281_check_sensor_id(ov9281, client);
	if (ret)
		goto err_power_off;

	sd->internal_ops = &ov9281_internal_ops;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	ov9281->pad.flags = MEDIA_PAD_FL_SOURCE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sd->entity, 1, &ov9281->pad);
	if (ret < 0)
		goto err_power_off;

	snprintf(sd->name, sizeof(sd->name), "m%s %s",
		 OV9281_NAME, dev_name(sd->dev));
	ret = v4l2_async_register_subdev_sensor_common(sd);
	if (ret) {
		dev_err(dev, "v4l2 async register subdev failed\n");
		goto err_clean_entity;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	return 0;

err_clean_entity:
	media_entity_cleanup(&sd->entity);
err_power_off:
	__ov9281_power_off(ov9281);
err_free_handler:
	v4l2_ctrl_handler_free(&ov9281->ctrl_handler);
err_destroy_mutex:
	mutex_destroy(&ov9281->mutex);

	return ret;
}

static int ov9281_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov9281 *ov9281 = to_ov9281(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ov9281->ctrl_handler);
	mutex_destroy(&ov9281->mutex);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		__ov9281_power_off(ov9281);
	pm_runtime_set_suspended(&client->dev);

	return 0;
}

static const struct of_device_id ov9281_of_match[] = {
	{ .compatible = "ovti,ov9281" },
	{},
};
MODULE_DEVICE_TABLE(of, ov9281_of_match);

static const struct i2c_device_id ov9281_match_id[] = {
	{ "ovti,ov9281", 0 },
	{ },
};

static struct i2c_driver ov9281_i2c_driver = {
	.driver = {
		.name = OV9281_NAME,
		.pm = &ov9281_pm_ops,
		.of_match_table = of_match_ptr(ov9281_of_match),
	},
	.probe		= &ov9281_probe,
	.remove		= &ov9281_remove,
	.id_table	= ov9281_match_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&ov9281_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&ov9281_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_DESCRIPTION("OmniVision ov9281 sensor driver");
MODULE_LICENSE("GPL v2");
