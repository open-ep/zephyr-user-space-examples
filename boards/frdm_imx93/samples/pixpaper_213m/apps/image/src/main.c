/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Open-EP pixpaper-213m (2.13", 250x122) e-paper driver for FRDM-IMX93 / M33.
 * Framebuffer is column-major: 250 gates x 16 source bytes = 4000 bytes, 1 = white.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>
#include "png_HEX.h"

LOG_MODULE_REGISTER(pixpaper_213m, LOG_LEVEL_INF);

#define EPD_NODE DT_NODELABEL(epd)

static const struct spi_dt_spec epd_spi =
	SPI_DT_SPEC_GET(EPD_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec dc_gpio = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec rst_gpio = GPIO_DT_SPEC_GET(EPD_NODE, reset_gpios);
static const struct gpio_dt_spec busy_gpio = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

/* Panel RAM: 250 gate lines x 128 source bits (16 bytes) each. */
#define EPD_GATES     250
#define EPD_SRC       128
#define EPD_SRC_BYTES (EPD_SRC / 8)
#define EPD_FB_SIZE   (EPD_GATES * EPD_SRC_BYTES)

/* This SoC's LPSPI corrupts single transfers longer than 10 bytes, so the RAM
 * data phase is sent in chunks (8 leaves margin and is ~8x faster than 1 byte).
 */
#define EPD_CHUNK     8

static uint8_t framebuf[EPD_FB_SIZE];

static int spi_tx(const uint8_t *data, size_t len)
{
	struct spi_buf buf = { .buf = (void *)data, .len = len };
	struct spi_buf_set set = { .buffers = &buf, .count = 1 };
	int ret = spi_write_dt(&epd_spi, &set);

	if (ret < 0) {
		LOG_ERR("SPI write failed: %d", ret);
	}
	return ret;
}

static void epd_cmd(uint8_t cmd)
{
	gpio_pin_set_dt(&dc_gpio, 0);
	k_busy_wait(1);
	spi_tx(&cmd, 1);
}

static void epd_data(uint8_t d)
{
	gpio_pin_set_dt(&dc_gpio, 1);
	k_busy_wait(1);
	spi_tx(&d, 1);
}

static void epd_wait_idle(void)
{
	k_msleep(2);
	/* BUSY high = busy; bail out after ~10s so a stuck panel can't hang us. */
	for (int i = 0; i < 1000; i++) {
		if (gpio_pin_get_dt(&busy_gpio) == 0) {
			return;
		}
		k_msleep(10);
	}
	LOG_WRN("BUSY stuck high - continuing anyway");
}

static void epd_hw_reset(void)
{
	k_msleep(50);
	gpio_pin_set_dt(&rst_gpio, 0);
	k_msleep(50);
	gpio_pin_set_dt(&rst_gpio, 1);
	k_msleep(50);
}

static void epd_init(void)
{
	epd_hw_reset();
	k_msleep(1000);
	epd_wait_idle();

	epd_cmd(0x12);			/* SW reset */
	epd_wait_idle();

	epd_cmd(0x01);			/* driver output: 0xF9+1 = 250 gates */
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);

	epd_cmd(0x11);			/* data entry mode: X+, Y- */
	epd_data(0x01);

	epd_cmd(0x44);			/* RAM X range: 0 .. 15 (16 bytes) */
	epd_data(0x00);
	epd_data(0x0F);

	epd_cmd(0x45);			/* RAM Y range: 249 .. 0 */
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);
	epd_data(0x00);

	epd_cmd(0x3C);			/* border waveform */
	epd_data(0x05);

	epd_cmd(0x21);			/* display update control 1 */
	epd_data(0x00);
	epd_data(0x80);

	epd_cmd(0x18);			/* internal temperature sensor */
	epd_data(0x80);

	epd_cmd(0x4E);			/* RAM X counter */
	epd_data(0x00);

	epd_cmd(0x4F);			/* RAM Y counter: 249 */
	epd_data(0xF9);
	epd_data(0x00);

	epd_wait_idle();
}

static void epd_refresh(void)
{
	epd_cmd(0x24);			/* write RAM (B/W) */
	k_msleep(10);
	gpio_pin_set_dt(&dc_gpio, 1);
	for (size_t i = 0; i < sizeof(framebuf); i += EPD_CHUNK) {
		spi_tx(&framebuf[i], MIN(EPD_CHUNK, sizeof(framebuf) - i));
	}

	epd_cmd(0x22);			/* display update control 2 */
	k_msleep(10);
	epd_data(0xF7);
	epd_cmd(0x20);			/* master activation */
	epd_wait_idle();
}

/* Set one pixel. gate: 0..249, src: 0..127. white = set bit. */
static void fb_set(int gate, int src, bool white)
{
	uint8_t *byte = &framebuf[gate * EPD_SRC_BYTES + (src / 8)];
	uint8_t mask = 1u << (7 - (src % 8));

	if (white) {
		*byte |= mask;
	} else {
		*byte &= ~mask;
	}
}

static void fb_uniform(bool white)
{
	memset(framebuf, white ? 0xFF : 0x00, sizeof(framebuf));
}

/* png_HEX.h: img0[] holds the image as IMG_H rows of IMG_CHUNKS uint32 words,
 * MSB-first across IMG_W columns; bit 1 = white. Image width maps to the gate
 * axis, height to the source axis; the rest of the panel is padded white.
 * IMG_W / IMG_H are defined by png_HEX.h.
 */
#define IMG_CHUNKS ((IMG_W + 31) / 32)

static bool img_white(int gate, int src)
{
	if (gate >= IMG_W || src >= IMG_H) {
		return true; /* pad white */
	}

	return (img0[src * IMG_CHUNKS + (gate / 32)] >> (31 - (gate % 32))) & 1u;
}

static void fb_image(void)
{
	for (int g = 0; g < EPD_GATES; g++) {
		for (int s = 0; s < EPD_SRC; s++) {
			fb_set(g, s, img_white(g, s));
		}
	}
}

int main(void)
{
	if (!spi_is_ready_dt(&epd_spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&dc_gpio) || !gpio_is_ready_dt(&rst_gpio) ||
	    !gpio_is_ready_dt(&busy_gpio)) {
		LOG_ERR("GPIO not ready");
		return -ENODEV;
	}

	if (gpio_pin_configure_dt(&dc_gpio, GPIO_OUTPUT_INACTIVE) < 0 ||
	    gpio_pin_configure_dt(&rst_gpio, GPIO_OUTPUT_ACTIVE) < 0 ||
	    gpio_pin_configure_dt(&busy_gpio, GPIO_INPUT) < 0) {
		LOG_ERR("GPIO configure failed");
		return -EIO;
	}

	LOG_INF("Initializing pixpaper-213m over LPSPI3...");
	epd_init();
	LOG_INF("Init done");

	/* Clear to white first to avoid ghosting, then show the image. */
	fb_uniform(true);
	epd_refresh();
	LOG_INF("Cleared to white");

	fb_image();
	epd_refresh();
	LOG_INF("Displayed image (%ux%u)", IMG_W, IMG_H);

	/* E-paper retains the image with no power; nothing more to do. */
	k_sleep(K_FOREVER);

	return 0;
}
