/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Open-EP PIXPAPER-426-M (4.26", 800x480, mono) as a Zephyr display device, so
 * LVGL (or any display API user) can draw on it.
 *
 * Refresh strategy, ported from the Linux DRM driver that is proven on the
 * same panel (imx93 and Kakip):
 *  - first update:  full OTP refresh that really flashes (frame to 0x24, the
 *                   same frame to 0x26 as the ping-pong base, 0x21 = 0x40 so
 *                   the OTP waveform drives every pixel), then enter partial
 *                   mode once: RAM ping-pong on, short custom LUT, analog on.
 *  - every update after that: whole frame to 0x24, kick 0x22 = 0x0C. The
 *                   controller compares it with the shown frame and drives
 *                   only the pixels that changed.
 *
 * The panel RAM is mirrored in X compared with the glass, so the driver keeps
 * its framebuffer in RAM order and flips X when it copies pixels in.
 */

#define DT_DRV_COMPAT open_ep_pixpaper_426m

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <string.h>

LOG_MODULE_REGISTER(pixpaper_426m, LOG_LEVEL_INF);

#define EPD_W       DT_INST_PROP(0, width)
#define EPD_H       DT_INST_PROP(0, height)
#define EPD_STRIDE  (EPD_W / 8)
#define EPD_FB_SIZE (EPD_STRIDE * EPD_H)

/* Tuning knobs. */
#define EPD_CHUNK      EPD_FB_SIZE /* bytes per SPI transfer: the whole frame (each transfer re-configures the bus) */
#define EPD_TP         4          /* partial drive length in LUT frames */
#define EPD_FR         4          /* partial LUT frame-rate code */
#define EPD_FULL_EVERY 0          /* full refresh every N partials, 0 = first update only */

/* Partial LUT voltages, as in the Linux driver. */
#define EPD_VGH  0x17 /* 0x03: 20 V */
#define EPD_VSH1 0x41 /* 0x04: 15 V */
#define EPD_VSH2 0xA8 /* 0x04: power-on default, unused */
#define EPD_VSL  0x32 /* 0x04: -15 V */
#define EPD_VCOM 0x50 /* 0x2C: -2.0 V */
#define EPD_WF_TIMING_AT    50
#define EPD_WF_FRAMERATE_AT 100
#define EPD_WF_LEN          (EPD_WF_FRAMERATE_AT + 5)

/* ponytail: one panel per board, so plain statics instead of per-instance config/data. */
static const struct spi_dt_spec epd_spi =
	SPI_DT_SPEC_INST_GET(0, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec dc_gpio = GPIO_DT_SPEC_INST_GET(0, dc_gpios);
static const struct gpio_dt_spec rst_gpio = GPIO_DT_SPEC_INST_GET(0, reset_gpios);
static const struct gpio_dt_spec busy_gpio = GPIO_DT_SPEC_INST_GET(0, busy_gpios);

static uint8_t fb[EPD_FB_SIZE]; /* RAM order, 1 = white */
static bool blanking;           /* LVGL batches a multi-area frame inside blanking on/off */
static bool dirty;
static bool partial_ready;
static uint32_t partials;
static uint8_t rev8[256];      /* bit-reversed byte, for the X mirror */

static void spi_tx(const uint8_t *data, size_t len)
{
	while (len) {
		size_t n = MIN(len, EPD_CHUNK);
		struct spi_buf buf = { .buf = (void *)data, .len = n };
		struct spi_buf_set set = { .buffers = &buf, .count = 1 };

		if (spi_write_dt(&epd_spi, &set) < 0) {
			LOG_ERR("SPI write failed");
			return;
		}
		data += n;
		len -= n;
	}
}

static void epd_cmd(uint8_t cmd)
{
	gpio_pin_set_dt(&dc_gpio, 0);
	spi_tx(&cmd, 1);
}

static void epd_data(uint8_t d)
{
	gpio_pin_set_dt(&dc_gpio, 1);
	spi_tx(&d, 1);
}

static void epd_bulk(const uint8_t *buf, size_t len)
{
	gpio_pin_set_dt(&dc_gpio, 1);
	spi_tx(buf, len);
}

static void epd_wait_idle(void)
{
	/* BUSY high = busy. A full refresh takes ~4 s; give up after 10 s. */
	for (int i = 0; i < 10000; i++) {
		if (gpio_pin_get_dt(&busy_gpio) == 0) {
			return;
		}
		k_msleep(1);
	}
	LOG_WRN("BUSY stuck high - continuing anyway");
}

static void epd_reset(int ms)
{
	gpio_pin_set_dt(&rst_gpio, 0);
	k_msleep(ms);
	gpio_pin_set_dt(&rst_gpio, 1);
	k_msleep(ms);
	epd_wait_idle();
}

static void epd_ram_window(void)
{
	epd_cmd(0x44); /* X: 0 .. 799 */
	epd_data(0x00);
	epd_data(0x00);
	epd_data((EPD_W - 1) & 0xFF);
	epd_data((EPD_W - 1) >> 8);

	epd_cmd(0x45); /* Y: 0 .. 479 */
	epd_data(0x00);
	epd_data(0x00);
	epd_data((EPD_H - 1) & 0xFF);
	epd_data((EPD_H - 1) >> 8);
}

static void epd_ram_write(uint8_t ram)
{
	epd_cmd(0x4E);
	epd_data(0x00);
	epd_data(0x00);
	epd_cmd(0x4F);
	epd_data(0x00);
	epd_data(0x00);

	epd_cmd(ram);
	epd_bulk(fb, sizeof(fb));
}

static void epd_kick(uint8_t mode)
{
	epd_cmd(0x22);
	epd_data(mode);
	epd_cmd(0x20);
	epd_wait_idle();
}

/* Same init as the openep user-space reference, from reset. */
static void epd_hw_init(void)
{
	epd_reset(50);

	epd_cmd(0x12); /* SW reset: registers back to OTP defaults */
	epd_wait_idle();

	epd_cmd(0x18); /* internal temperature sensor */
	epd_data(0x80);

	epd_cmd(0x0C); /* booster soft start */
	epd_data(0xAE);
	epd_data(0xC7);
	epd_data(0xC3);
	epd_data(0xC0);
	epd_data(0x80);

	epd_cmd(0x01); /* driver output: 480 gates */
	epd_data((EPD_H - 1) & 0xFF);
	epd_data((EPD_H - 1) >> 8);
	epd_data(0x02);

	epd_cmd(0x3C); /* border (full update) */
	epd_data(0x01);

	epd_ram_window();
}

/*
 * Partial waveform. Bytes 0..49 are LUT0..LUT4 (10 bytes each, phase A in the
 * top two bits: 00 VSS, 01 VSH1, 10 VSL). With ping-pong the LUT is picked by
 * (shown, new), so unchanged pixels stay at VSS and only changes are driven.
 */
static void epd_partial_begin(void)
{
	static const uint8_t pingpong[10] = { 0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0 };
	uint8_t wf[EPD_WF_LEN] = { 0 };

	wf[10] = 0x80; /* black -> white: VSL  */
	wf[20] = 0x40; /* white -> black: VSH1 */
	wf[EPD_WF_TIMING_AT] = EPD_TP;
	memset(&wf[EPD_WF_FRAMERATE_AT], (EPD_FR << 4) | EPD_FR, 5);

	epd_reset(10);

	epd_cmd(0x18);
	epd_data(0x80);

	epd_cmd(0x3C); /* border (partial) */
	epd_data(0x80);

	epd_cmd(0x37); /* byte 5 bit 6: RAM ping-pong for display mode 2 */
	epd_bulk(pingpong, sizeof(pingpong));

	epd_cmd(0x03);
	epd_data(EPD_VGH);
	epd_cmd(0x04);
	epd_data(EPD_VSH1);
	epd_data(EPD_VSH2);
	epd_data(EPD_VSL);
	epd_cmd(0x2C);
	epd_data(EPD_VCOM);

	epd_cmd(0x32);
	epd_bulk(wf, sizeof(wf));
	epd_wait_idle();

	epd_ram_window();

	/* Power the analog up once, so each partial kick (0x0C) can skip it. */
	epd_kick(0xC0);
}

static void epd_full_refresh(void)
{
	epd_hw_init();
	epd_ram_write(0x24);
	epd_ram_write(0x26); /* ping-pong base for the partials that follow */

	epd_cmd(0x21); /* RED RAM read as 0: the OTP waveform drives every pixel */
	epd_data(0x40);
	epd_data(0x00);
	epd_kick(0xF7);

	epd_cmd(0x21); /* partial mode needs the RED/old RAM back */
	epd_data(0x00);
	epd_data(0x00);

	epd_partial_begin();
	partial_ready = true;
	partials = 0;
}

static void epd_update(void)
{
	int64_t t0 = k_uptime_get();

	if (!dirty) {
		return;
	}
	dirty = false;

	if (!partial_ready || (EPD_FULL_EVERY && partials >= EPD_FULL_EVERY)) {
		epd_full_refresh();
		LOG_INF("full refresh: %lld ms", k_uptime_get() - t0);
		return;
	}

	static int64_t spi_ms, panel_ms;
	int64_t t1;

	epd_ram_write(0x24);
	t1 = k_uptime_get();
	epd_kick(0x0C);
	spi_ms += t1 - t0;
	panel_ms += k_uptime_get() - t1;
	if (++partials % 10 == 0) {
		LOG_INF("partial refresh: %lld ms avg (SPI %lld + panel %lld) over 10",
			(spi_ms + panel_ms) / 10, spi_ms / 10, panel_ms / 10);
		spi_ms = panel_ms = 0;
	}
}

static int pp_write(const struct device *dev, const uint16_t x, const uint16_t y,
		    const struct display_buffer_descriptor *desc, const void *buf)
{
	const uint8_t *src = buf;

	if (x + desc->width > EPD_W || y + desc->height > EPD_H) {
		return -EINVAL;
	}

	/* MONO01, MSB first, rows of desc->pitch pixels. Flip X into RAM order. */
	if (x % 8 == 0 && desc->width % 8 == 0 && desc->pitch % 8 == 0) {
		/* byte-aligned (LVGL's mono rounder guarantees it): whole bytes, bit-reversed */
		for (uint16_t r = 0; r < desc->height; r++) {
			const uint8_t *row = src + r * (desc->pitch / 8);
			uint8_t *dst = fb + (y + r) * EPD_STRIDE + EPD_STRIDE - 1 - x / 8;

			for (uint16_t j = 0; j < desc->width / 8; j++) {
				*dst-- = rev8[row[j]];
			}
		}
		goto done;
	}
	for (uint16_t r = 0; r < desc->height; r++) {
		const uint8_t *row = src + r * (desc->pitch / 8);
		uint8_t *dst = fb + (y + r) * EPD_STRIDE;

		for (uint16_t c = 0; c < desc->width; c++) {
			uint16_t fx = EPD_W - 1 - (x + c);
			uint8_t bit = BIT(7 - (fx % 8));

			if (row[c / 8] & BIT(7 - (c % 8))) {
				dst[fx / 8] |= bit;
			} else {
				dst[fx / 8] &= ~bit;
			}
		}
	}
done:
	dirty = true;

	if (!blanking && !desc->frame_incomplete) {
		epd_update();
	}
	return 0;
}

static int pp_blanking_on(const struct device *dev)
{
	blanking = true;
	return 0;
}

static int pp_blanking_off(const struct device *dev)
{
	blanking = false;
	epd_update();
	return 0;
}

static void pp_get_capabilities(const struct device *dev, struct display_capabilities *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->x_resolution = EPD_W;
	caps->y_resolution = EPD_H;
	caps->supported_pixel_formats = PIXEL_FORMAT_MONO01;
	caps->current_pixel_format = PIXEL_FORMAT_MONO01;
	caps->screen_info = SCREEN_INFO_MONO_MSB_FIRST | SCREEN_INFO_EPD;
}

static int pp_set_pixel_format(const struct device *dev, const enum display_pixel_format pf)
{
	return pf == PIXEL_FORMAT_MONO01 ? 0 : -ENOTSUP;
}

static int pp_init(const struct device *dev)
{
	if (!spi_is_ready_dt(&epd_spi) || !gpio_is_ready_dt(&dc_gpio) ||
	    !gpio_is_ready_dt(&rst_gpio) || !gpio_is_ready_dt(&busy_gpio)) {
		LOG_ERR("SPI or GPIO not ready");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&dc_gpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst_gpio, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&busy_gpio, GPIO_INPUT);

	for (int i = 0; i < 256; i++) {
		uint8_t r = 0;

		for (int b = 0; b < 8; b++) {
			r |= ((i >> b) & 1) << (7 - b);
		}
		rev8[i] = r;
	}

	/* The panel itself is set up on the first update (a full refresh). */
	memset(fb, 0xFF, sizeof(fb));
	return 0;
}

static DEVICE_API(display, pp_api) = {
	.blanking_on = pp_blanking_on,
	.blanking_off = pp_blanking_off,
	.write = pp_write,
	.get_capabilities = pp_get_capabilities,
	.set_pixel_format = pp_set_pixel_format,
};

DEVICE_DT_INST_DEFINE(0, pp_init, NULL, NULL, NULL, POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,
		      &pp_api);
