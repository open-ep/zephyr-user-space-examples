/*
 * SPDX-License-Identifier: Apache-2.0
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * PIXPAPER-213-M (2.13" mono) boot-image demo on W6300-EVB-Pico2 / Pico 2
 * (RP2350), Zephyr. Draws the bundled image once, then panel deep sleep.
 * SSD1680-class: BUSY idles LOW, full refresh ~2 s.
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

#define DISP_W        250
#define DISP_STRIDE   16
#define DISP_GATE_MAX (DISP_W - 1)

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

/* this controller idles LOW (busy = HIGH) */
static void epd_wait_idle(void)
{
	k_msleep(2);
	for (int i = 0; i < 5000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_msleep(1);
	}
	printk("warn: BUSY timeout\n");
}

/* refresh completion: BUSY rises a few ms after 0x20 - wait rise then fall */
static void epd_wait_refresh(void)
{
	for (int i = 0; i < 300; i++) {
		if (gpio_pin_get_dt(&busy) == 1) {
			break;
		}
		k_msleep(1);
	}
	for (int i = 0; i < 8000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_msleep(1);
	}
	printk("warn: refresh timeout\n");
}

static void epd_hw_reset(void)
{
	k_msleep(50);
	gpio_pin_set_dt(&rst, 0);
	k_msleep(50);
	gpio_pin_set_dt(&rst, 1);
	k_msleep(50);
}

static void epd_reg_init(void)
{
	epd_wait_idle();
	epd_command(0x12);            /* SW reset */
	epd_wait_idle();

	epd_command(0x01);            /* driver output: 250 gate lines */
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x11);            /* data entry mode */
	epd_data(0x01);

	epd_command(0x44);            /* full-screen RAM window */
	epd_data(0x00);
	epd_data(DISP_STRIDE - 1);
	epd_command(0x45);
	epd_data(DISP_GATE_MAX & 0xFF);
	epd_data(0x00);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x3C);            /* border */
	epd_data(0x05);

	epd_command(0x21);            /* display update control */
	epd_data(0x00);
	epd_data(0x80);

	epd_command(0x18);            /* internal temperature sensor */
	epd_data(0x80);

	epd_command(0x4E);            /* RAM cursor home */
	epd_data(0x00);
	epd_command(0x4F);
	epd_data(DISP_GATE_MAX & 0xFF);
	epd_data(0x00);
	epd_wait_idle();
}

static void epd_write_plane(uint8_t ram_cmd, const uint8_t *img)
{
	epd_command(0x4E);
	epd_data(0x00);
	epd_command(0x4F);
	epd_data(DISP_GATE_MAX & 0xFF);
	epd_data(0x00);

	epd_command(ram_cmd);
	for (int i = 0; i < DISP_W * DISP_STRIDE; i++) {
		epd_data(img[i]);
	}
}

int main(void)
{
	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	printk("\npixpaper-213-m on rp2350, R1.0.0\n");

	epd_hw_reset();
	k_msleep(100);
	epd_reg_init();

	printk("drawing image\n");
	/* both planes get the image so partial mode starts from a clean reference */
	epd_write_plane(0x24, img_packed);
	epd_write_plane(0x26, img_packed);

	epd_command(0x22);            /* full refresh via OTP waveform */
	epd_data(0xF7);
	epd_command(0x20);
	epd_wait_refresh();
	printk("done\n");

	epd_command(0x10);            /* panel deep sleep */
	epd_data(0x01);
	printk("panel in deep sleep - power-cycle to redraw\n");

	return 0;
}
