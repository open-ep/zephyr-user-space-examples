/*
 * SPDX-License-Identifier: Apache-2.0
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * PIXPAPER-213-C (2.13" 4-colour) on W6300-EVB-Pico2 / Pico 2 (RP2350), Zephyr.
 * Alternates the bundled dithered image and a colour-bar pattern every 30 s.
 * Panel: BUSY idles HIGH, full refresh ~15-25 s, parked after each refresh
 * (hence the re-init every cycle). No framebuffer - 2bpp image streamed from
 * flash over bit-banged SPI.
 *
 * Wiring in app.overlay: CLK=GP2 DIN=GP3 CS=GP4 DC=GP5 RST=GP6 BUSY=GP7, 3V3.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "img_packed.h"

#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec sck  = GPIO_DT_SPEC_GET(ZUSER, sck_gpios);
static const struct gpio_dt_spec mosi = GPIO_DT_SPEC_GET(ZUSER, mosi_gpios);
static const struct gpio_dt_spec cs   = GPIO_DT_SPEC_GET(ZUSER, cs_gpios);
static const struct gpio_dt_spec dc   = GPIO_DT_SPEC_GET(ZUSER, dc_gpios);
static const struct gpio_dt_spec rst  = GPIO_DT_SPEC_GET(ZUSER, rst_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(ZUSER, busy_gpios);

#define EPD_W         250
#define EPD_COL_BYTES 31   /* rows 0-123; rows 124-127 are runtime 0xFF */

/* ---------------- bit-banged SPI mode 0 ---------------- */

static void spi_out(uint8_t b)
{
	for (int i = 0; i < 8; i++) {
		gpio_pin_set_dt(&mosi, b & 0x80);
		gpio_pin_set_dt(&sck, 1);
		b <<= 1;
		gpio_pin_set_dt(&sck, 0);
	}
}

static void epd_write(int is_data, uint8_t b)
{
	gpio_pin_set_dt(&dc, is_data);
	k_busy_wait(1);
	gpio_pin_set_dt(&cs, 0);
	spi_out(b);
	gpio_pin_set_dt(&cs, 1);
}

#define epd_command(c) epd_write(0, (c))
#define epd_data(d)    epd_write(1, (d))

/* this controller idles HIGH; 30 s timeout covers the long refresh */
static void epd_wait_idle(void)
{
	k_msleep(2);
	for (int i = 0; i < 30000; i++) {
		if (gpio_pin_get_dt(&busy) == 1) {
			return;
		}
		k_msleep(1);
	}
	printk("warn: BUSY timeout\n");
}

static void epd_hw_reset(void)
{
	k_msleep(50);
	gpio_pin_set_dt(&rst, 0);
	k_msleep(50);
	gpio_pin_set_dt(&rst, 1);
	k_msleep(50);
}

/* init sequence as {cmd, ndata, data...} records - same values as the
 * Linux/Arduino reference */
static const uint8_t INIT_SEQ[] = {
	0x4D, 1, 0x78,
	0x00, 2, 0x0F, 0x09,                         /* PSR */
	0x01, 6, 0x07, 0x00, 0x22, 0x78, 0x0A, 0x22, /* PWRR */
	0x03, 3, 0x10, 0x54, 0x44,                   /* POFS */
	0x06, 7, 0x0F, 0x0A, 0x2F, 0x25, 0x22, 0x2E, 0x21, /* BTST_P */
	0x30, 1, 0x02,                               /* CDI */
	0x41, 1, 0x00,
	0x50, 1, 0x37,
	0x60, 2, 0x02, 0x02,
	0x61, 4, 0x00, 0x80, 0x00, 0xFA,             /* resolution 128x250 */
	0x65, 4, 0x00, 0x00, 0x00, 0x00,
	0xE7, 1, 0x1C,
	0xE3, 1, 0x22,
	0xE0, 1, 0x00,
	0xB4, 1, 0xD0,
	0xB5, 1, 0x03,
	0xE9, 1, 0x01,
};

static void epd_init(void)
{
	epd_hw_reset();
	k_msleep(1000);
	epd_wait_idle();

	for (unsigned int i = 0; i < sizeof(INIT_SEQ);) {
		uint8_t n = INIT_SEQ[i + 1];

		epd_command(INIT_SEQ[i]);
		for (uint8_t k = 0; k < n; k++) {
			epd_data(INIT_SEQ[i + 2 + k]);
		}
		epd_wait_idle();
		i += 2 + n;
	}
}

/* power on + refresh (~15-25 s) + park, shared by both frame writers */
static void epd_refresh(void)
{
	epd_wait_idle();

	epd_command(0x04);            /* power on */
	k_msleep(10);
	epd_data(0x00);
	epd_wait_idle();

	printk("refreshing (15-25 s)...\n");
	epd_command(0x12);            /* refresh */
	k_msleep(10);
	epd_data(0x00);
	epd_wait_idle();

	/* as in the reference: park the panel after update */
	gpio_pin_set_dt(&rst, 0);
	gpio_pin_set_dt(&dc, 0);
	printk("done\n");
}

static void epd_write_img(const uint8_t *packed)
{
	epd_command(0x10);
	k_msleep(10);

	for (int x = 0; x < EPD_W; x++) {
		for (int j = 0; j < EPD_COL_BYTES; j++) {
			epd_data(packed[x * EPD_COL_BYTES + j]);
		}
		epd_data(0xFF);       /* rows 124-127 (outside the panel) */
	}

	epd_refresh();
}

/* 4 vertical bars, generated on the fly (1 byte = 4 same-colour pixels) */
static void epd_write_bars(void)
{
	static const uint8_t BAR_BYTES[4] = {0x00, 0x55, 0xFF, 0xAA};

	epd_command(0x10);
	k_msleep(10);

	for (int x = 0; x < EPD_W; x++) {
		int col = EPD_W - 1 - x;  /* same mirror as the image stream */
		uint8_t b = BAR_BYTES[(col * 4) / EPD_W];

		for (int j = 0; j < 32; j++) {
			epd_data(b);
		}
	}

	epd_refresh();
}

int main(void)
{
	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	printk("\npixpaper-213-c on w6300_evb_pico2, R1.0.0\n");

	/* panel is parked after each refresh, so re-init every cycle */
	while (1) {
		printk("drawing image\n");
		epd_init();
		epd_write_img(img_packed);
		k_msleep(30000);

		printk("drawing color bars\n");
		epd_init();
		epd_write_bars();
		k_msleep(30000);
	}

	return 0;
}
