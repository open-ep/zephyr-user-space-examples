/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PIXPAPER-426-M full + partial refresh demo on the Kakip Cortex-M33, through
 * the Zephyr display API:
 *  1. the bundled 800x480 image (full refresh, the panel flashes, ~4 s)
 *  2. a progress bar along the bottom, one partial refresh per step,
 *     timed, forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "image_output.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define W      800
#define H      480
#define STRIDE (W / 8)

/* Progress bar: STEPS blocks of BLK_W x BAR_H pixels at the bottom. */
#define BAR_Y  448
#define BAR_H  24
#define BLK_W  32
#define STEPS  (W / BLK_W)

static uint8_t frame[W * H / 8];
static uint8_t black[BLK_W / 8 * BAR_H];
static uint8_t white_bar[STRIDE * BAR_H];

static int64_t draw(const struct device *dev, int x, int y, int w, int h, const void *buf)
{
	struct display_buffer_descriptor desc = {
		.buf_size = w * h / 8,
		.width = w,
		.height = h,
		.pitch = w,
	};
	int64_t t0 = k_uptime_get();

	display_write(dev, x, y, &desc, buf); /* returns when the panel is done */
	return k_uptime_get() - t0;
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(dev)) {
		LOG_ERR("display not ready");
		return 0;
	}

	/*
	 * image_output.h comes from the openep Linux demo (png2bit_426.py), which
	 * stores it mirrored, in panel RAM order. The display API wants it as seen,
	 * so flip each row back: reverse the bytes and the bits in each byte.
	 */
	for (int y = 0; y < H; y++) {
		for (int b = 0; b < STRIDE; b++) {
			uint8_t v = epd_image[y * STRIDE + b], r = 0;

			for (int i = 0; i < 8; i++) {
				r = (r << 1) | ((v >> i) & 1);
			}
			frame[y * STRIDE + (STRIDE - 1 - b)] = r;
		}
	}

	LOG_INF("Full refresh of the image...");
	LOG_INF("Image shown in %lld ms", draw(dev, 0, 0, W, H, frame));

	memset(black, 0x00, sizeof(black));     /* MONO01: 0 = black */
	memset(white_bar, 0xFF, sizeof(white_bar));

	for (int round = 0;; round++) {
		int64_t total = 0;

		for (int i = 0; i < STEPS; i++) {
			total += draw(dev, i * BLK_W, BAR_Y, BLK_W, BAR_H, black);
		}
		LOG_INF("round %d: %d partial refreshes, %lld ms each on average", round, STEPS,
			total / STEPS);
		draw(dev, 0, BAR_Y, W, BAR_H, white_bar);
		k_msleep(1000);
	}
	return 0;
}
