/*
 * SPDX-License-Identifier: Apache-2.0
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * .epdv video player for PIXPAPER-213-M on W6300-EVB-Pico2 / Pico 2 (RP2350),
 * Zephyr. Port of the Linux reference player, but the clip is embedded in
 * flash (no filesystem here). Loops forever, one full refresh per loop.
 *
 * New clip: python3 tools/video2epd.py in.mp4 -o clip.epdv --fps 6
 *           python3 tools/epdv2h.py clip.epdv src/clip_epdv.h
 *           (2 MB flash ~= 470 frames ~= 78 s @ 6 fps)
 *
 * Discipline: full frame to 0x24 every kick, 0x37 ping-pong ON, 0x26 never
 * touched - the controller diffs against the previous bank itself. The games
 * use the opposite discipline; do not mix.
 *
 * Wiring in app.overlay: CLK=GP2 DIN=GP3 CS=GP4 DC=GP5 RST=GP6 BUSY=GP7, 3V3.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "clip_epdv.h"

#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec sck  = GPIO_DT_SPEC_GET(ZUSER, sck_gpios);
static const struct gpio_dt_spec mosi = GPIO_DT_SPEC_GET(ZUSER, mosi_gpios);
static const struct gpio_dt_spec cs   = GPIO_DT_SPEC_GET(ZUSER, cs_gpios);
static const struct gpio_dt_spec dc   = GPIO_DT_SPEC_GET(ZUSER, dc_gpios);
static const struct gpio_dt_spec rst  = GPIO_DT_SPEC_GET(ZUSER, rst_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(ZUSER, busy_gpios);

#define DISP_W        250
#define DISP_STRIDE   16
#define DISP_BUF_SIZE (DISP_W * DISP_STRIDE)
#define DISP_GATE_MAX (DISP_W - 1)

/* partial waveform length: lower = faster kicks, weaker drive (0x05..0x10) */
#define PHASE0_TP 0x06
#define PARTIAL_FR 3
#define DISPLAY_PART_KEEP_ON 0x0C

/* ---------------- panel low level (bit-banged SPI mode 0) --------------- */

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

/* one CS assertion per frame: ~3x faster than per-byte writes */
static void epd_data_bulk(const uint8_t *data, int len)
{
	gpio_pin_set_dt(&dc, 1);
	k_busy_wait(1);
	gpio_pin_set_dt(&cs, 0);
	for (int i = 0; i < len; i++) {
		spi_out(data[i]);
	}
	gpio_pin_set_dt(&cs, 1);
}

static void epd_wait_idle(void)
{
	k_msleep(2);
	for (int i = 0; i < 5000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_msleep(1);
	}
}

/* BUSY rises a few ms after 0x20: wait rise, then fall */
static void epd_wait_refresh(uint32_t blind_ms)
{
	int rose = 0;

	for (int i = 0; i < 300; i++) {
		if (gpio_pin_get_dt(&busy) == 1) {
			rose = 1;
			break;
		}
		k_msleep(1);
	}
	if (!rose) {
		k_msleep(blind_ms);
		return;
	}
	for (int i = 0; i < 8000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_msleep(1);
	}
}

static void epd_hw_reset(void)
{
	k_msleep(50);
	gpio_pin_set_dt(&rst, 0);
	k_msleep(50);
	gpio_pin_set_dt(&rst, 1);
	k_msleep(50);
}

static void epd_set_window(int xb_start, int xb_end, int g_start, int g_end)
{
	epd_command(0x44);
	epd_data(xb_start & 0xFF);
	epd_data(xb_end & 0xFF);

	epd_command(0x45);
	epd_data(g_start & 0xFF);
	epd_data((g_start >> 8) & 0xFF);
	epd_data(g_end & 0xFF);
	epd_data((g_end >> 8) & 0xFF);
}

static void epd_set_cursor(int xb, int g)
{
	epd_command(0x4E);
	epd_data(xb & 0xFF);

	epd_command(0x4F);
	epd_data(g & 0xFF);
	epd_data((g >> 8) & 0xFF);
}

static void epd_set_full_window(void)
{
	epd_set_window(0x00, DISP_STRIDE - 1, DISP_GATE_MAX, 0x00);
}

static void epd_set_full_cursor(void)
{
	epd_set_cursor(0x00, DISP_GATE_MAX);
}

static void epd_reg_init(void)
{
	epd_wait_idle();
	epd_command(0x12);
	epd_wait_idle();

	epd_command(0x01);
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x11);
	epd_data(0x01);

	epd_set_full_window();

	epd_command(0x3C);
	epd_data(0x05);

	epd_command(0x21);
	epd_data(0x00);
	epd_data(0x80);

	epd_command(0x18);
	epd_data(0x80);

	epd_set_full_cursor();
	epd_wait_idle();
}

/* ---------------- partial mode, pet/video discipline -------------------- */

static const uint8_t WF_PARTIAL[159] = {
	0x0, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x80, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x40, 0x40, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x80, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x14, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x0, 0x0, 0x0,
	0x22, 0x17, 0x41, 0x0, 0x32, 0x36,
};

static void epd_load_partial_lut(void)
{
	uint8_t fr_byte = ((PARTIAL_FR & 7) << 4) | (PARTIAL_FR & 7);

	epd_command(0x32);
	for (int i = 0; i < 153; i++) {
		uint8_t b = WF_PARTIAL[i];

		if (i == 60) {
			b = PHASE0_TP;
		} else if (i >= 144 && i <= 149) {
			b = fr_byte;
		}
		epd_data(b);
	}
	epd_wait_idle();

	epd_command(0x3F);
	epd_data(WF_PARTIAL[153]);
	epd_command(0x03);
	epd_data(WF_PARTIAL[154]);
	epd_command(0x04);
	epd_data(WF_PARTIAL[155]);
	epd_data(WF_PARTIAL[156]);
	epd_data(WF_PARTIAL[157]);
	epd_command(0x2C);
	epd_data(WF_PARTIAL[158]);

	/* ping-pong ON (byte5=0x40): the player writes the FULL frame every
	 * kick, so the hardware bank alternation does the old/new diff for
	 * us - the pet/video discipline. Never sync 0x26 in this mode. */
	epd_command(0x37);
	for (int i = 0; i < 10; i++) {
		epd_data(i == 5 ? 0x40 : 0x00);
	}

	epd_command(0x3C);
	epd_data(0x80);
}

static void epd_partial_begin(void)
{
	gpio_pin_set_dt(&rst, 0);
	k_msleep(2);
	gpio_pin_set_dt(&rst, 1);
	k_msleep(2);

	epd_command(0x01);
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x11);
	epd_data(0x01);

	epd_command(0x21);
	epd_data(0x00);
	epd_data(0x80);

	epd_command(0x18);
	epd_data(0x80);

	epd_load_partial_lut();

	epd_command(0x22);
	epd_data(0xC0);
	epd_command(0x20);
	epd_wait_idle();

	epd_set_full_window();
}

/* first frame into both banks + one OTP full refresh (clean base) */
static void epd_set_base(const uint8_t *frame)
{
	epd_hw_reset();
	epd_reg_init();

	for (int plane = 0; plane < 2; plane++) {
		epd_set_full_window();
		epd_set_full_cursor();
		epd_command(plane ? 0x26 : 0x24);
		epd_data_bulk(frame, DISP_BUF_SIZE);
	}

	epd_command(0x22);
	epd_data(0xF7);
	epd_command(0x20);
	epd_wait_refresh(2500);

	epd_partial_begin();
}

/* one video frame: full frame to 0x24 only, then a mode-2 kick */
static void epd_partial_frame(const uint8_t *frame)
{
	epd_set_full_cursor();
	epd_command(0x24);
	epd_data_bulk(frame, DISP_BUF_SIZE);

	epd_command(0x22);
	epd_data(DISPLAY_PART_KEEP_ON);
	epd_command(0x20);
	epd_wait_refresh(400);
}

int main(void)
{
	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	printk("\npixpaper .epdv player on rp2350, R1.0.0\n");
	printk("clip: %d frames, %d us/frame\n",
	       CLIP_FRAME_COUNT, CLIP_FRAME_US);

	epd_hw_reset();
	k_msleep(500);

	while (1) {
		/* loop start: base map = frame 0, pays off ghosting debt */
		epd_set_base(&clip_frames[0]);

		int64_t loop_t0 = k_uptime_get();
		int64_t raw_ms = 0;

		for (uint32_t i = 1; i < CLIP_FRAME_COUNT; i++) {
			int64_t t0 = k_uptime_get();

			epd_partial_frame(&clip_frames[(size_t)i * CLIP_FRAME_BYTES]);

			int64_t spent = (k_uptime_get() - t0) * 1000;

			raw_ms += spent / 1000;
			if ((int64_t)CLIP_FRAME_US > spent) {
				k_usleep(CLIP_FRAME_US - spent);
			}
		}

		/* effective fps report, printed once per clip loop on UART0 */
		int64_t total_ms = k_uptime_get() - loop_t0;
		int n = CLIP_FRAME_COUNT - 1;

		printk("loop: %d frames, raw %lld ms/frame (max %lld fps x10), "
		       "paced %lld ms/frame\n",
		       n, raw_ms / n, 10000 / (raw_ms / n),
		       total_ms / n);
	}

	return 0;
}
