/*
 * SPDX-License-Identifier: Apache-2.0
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * PIXPAPER-213-M showcase loop on W6300-EVB-Pico2 / Pico 2 (RP2350), Zephyr.
 * No input needed, 30 s per scene with a title card between:
 * TETRIS -> SNAKE -> PONG (all self-playing) -> ESL -> GRAY-4 -> NAGOYA.
 *
 * Games:  partial update, 0x37 all zeros, BUSY rise-then-fall, 0x26 synced
 *         after every kick, full-refresh rebase on scene entry.
 * GRAY-4: two RAM planes as a 2bpp code with the reference waveform from
 *         pixpaper-213-m-test-frdm-imx93.c - black=(1,1) dark=(0,1)
 *         light=(1,0) white=(0,0) on (0x24, 0x26).
 *
 * Wiring in common/w6300_evb_pico2.overlay: CLK=GP2 DIN=GP3 CS=GP4 DC=GP5
 * RST=GP6 BUSY=GP7, VCC=3V3.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "img_esl.h"
#include "img_gray4_test.h"
#include "img_nagoya_gray4.h"

#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec sck  = GPIO_DT_SPEC_GET(ZUSER, sck_gpios);
static const struct gpio_dt_spec mosi = GPIO_DT_SPEC_GET(ZUSER, mosi_gpios);
static const struct gpio_dt_spec cs   = GPIO_DT_SPEC_GET(ZUSER, cs_gpios);
static const struct gpio_dt_spec dc   = GPIO_DT_SPEC_GET(ZUSER, dc_gpios);
static const struct gpio_dt_spec rst  = GPIO_DT_SPEC_GET(ZUSER, rst_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(ZUSER, busy_gpios);

#define DISP_W        250
#define DISP_H        122
#define DISP_STRIDE   16
#define DISP_GATE_MAX (DISP_W - 1)

#define SCENE_MS      30000
#define TRANSITION_MS 2200

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

	epd_set_cursor(0x00, DISP_GATE_MAX);
	epd_wait_idle();
}

/* ---------------- scene rendering dispatch ---------------- */

/* current scene's per-byte renderer */
static uint8_t (*g_scene)(int x, int b);

static void emit_cols(int xa, int xb, uint8_t ram_cmd)
{
	if (xa < 0) {
		xa = 0;
	}
	if (xb > DISP_W - 1) {
		xb = DISP_W - 1;
	}

	epd_set_window(0x00, DISP_STRIDE - 1,
		       DISP_GATE_MAX - xa, DISP_GATE_MAX - xb);
	epd_set_cursor(0x00, DISP_GATE_MAX - xa);
	epd_command(ram_cmd);

	for (int x = xa; x <= xb; x++) {
		for (int b = 0; b < DISP_STRIDE; b++) {
			epd_data(g_scene(x, b));
		}
	}
}

/* ---------------- partial update mode (proven games recipe) ------------- */

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

#define PHASE0_TP 0x0C               /* slightly fast partials for the demo */
#define PARTIAL_FR 3
#define DISPLAY_PART_KEEP_ON 0x0C

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

	/* ping-pong OFF: the games write moving regions (see UIAP ports) */
	epd_command(0x37);
	for (int i = 0; i < 10; i++) {
		epd_data(0x00);
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

/* update columns xa..xb: 0x24 -> kick -> sync 0x26 */
static void flush_cols(int xa, int xb)
{
	emit_cols(xa, xb, 0x24);
	epd_set_full_window();
	epd_command(0x22);
	epd_data(DISPLAY_PART_KEEP_ON);
	epd_command(0x20);
	epd_wait_refresh(400);
	emit_cols(xa, xb, 0x26);
	epd_set_full_window();
}

/* whole scene to both planes + OTP full refresh, then back to partial */
static void rebase(void)
{
	epd_hw_reset();
	epd_reg_init();

	for (int plane = 0; plane < 2; plane++) {
		epd_set_full_window();
		epd_set_cursor(0x00, DISP_GATE_MAX);
		epd_command(plane ? 0x26 : 0x24);
		for (int x = 0; x < DISP_W; x++) {
			for (int b = 0; b < DISP_STRIDE; b++) {
				epd_data(g_scene(x, b));
			}
		}
	}

	epd_command(0x22);
	epd_data(0xF7);
	epd_command(0x20);
	epd_wait_refresh(2500);

	epd_partial_begin();
}

/* ---------------- 4-gray mode (reference waveform) ---------------------- */

/* Proven 4-gray recipe ported from the Linux reference
 * (pixpaper-213-m-test-frdm-imx93.c): dedicated analog/voltage setup, the
 * tuned LUT below, and pixel coding black=(1,1) dark=(0,1) light=(1,0)
 * white=(0,0) across the (0x24, 0x26) planes. */
static const uint8_t LUT_4G[153] = {
	0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x48, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x48, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x20, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0A, 0x19, 0x00, 0x03, 0x08, 0x00, 0x00, 0x14, 0x01, 0x00, 0x14, 0x01,
	0x00, 0x03, 0x0A, 0x03, 0x00, 0x08, 0x19, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

static void epd_show_gray4(const uint8_t *p24, const uint8_t *p26)
{
	epd_hw_reset();
	k_msleep(100);
	epd_wait_idle();

	epd_command(0x12);
	epd_wait_idle();

	epd_command(0x74);
	epd_data(0x54);
	epd_command(0x7E);
	epd_data(0x3B);

	epd_command(0x01);
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x11);
	epd_data(0x01);

	epd_set_full_window();

	epd_command(0x3C);
	epd_data(0x00);

	epd_command(0x2C);
	epd_data(0x1C);
	epd_command(0x3F);
	epd_data(0x22);
	epd_command(0x03);
	epd_data(0x17);
	epd_command(0x04);
	epd_data(0x41);
	epd_data(0x00);
	epd_data(0x32);

	epd_command(0x21);
	epd_data(0x00);
	epd_data(0x80);

	epd_command(0x32);
	for (int i = 0; i < 153; i++) {
		epd_data(LUT_4G[i]);
	}

	epd_set_cursor(0x00, DISP_GATE_MAX);
	epd_wait_idle();

	epd_command(0x24);
	k_msleep(10);
	for (int i = 0; i < DISP_W * DISP_STRIDE; i++) {
		epd_data(p24[i]);
	}

	epd_set_cursor(0x00, DISP_GATE_MAX);
	epd_command(0x26);
	k_msleep(10);
	for (int i = 0; i < DISP_W * DISP_STRIDE; i++) {
		epd_data(p26[i]);
	}

	epd_command(0x22);
	k_msleep(10);
	epd_data(0xC7);
	epd_command(0x20);
	epd_wait_refresh(4000);
}

/* ---------------- tiny 5x7 font (transition cards) ---------------------- */

struct glyph {
	char ch;
	uint8_t col[5];                  /* bit0 = top row */
};

static const struct glyph FONT[] = {
	{'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
	{'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
	{'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
	{'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
	{'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
	{'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
	{'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
	{'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
	{'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
};

static const uint8_t *glyph_cols(char c)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(FONT); i++) {
		if (FONT[i].ch == c) {
			return FONT[i].col;
		}
	}
	return NULL;
}

#define CARD_SCALE 5                 /* glyph cell: 30x35 px */
static const char *g_card_text;

/* landscape text card: letters upright, string runs along the 250 axis */
static uint8_t scene_card(int x, int b)
{
	uint8_t out = 0xFF;
	int len = 0;

	while (g_card_text[len]) {
		len++;
	}

	int cell_w = 6 * CARD_SCALE;
	int total_w = len * cell_w - CARD_SCALE;
	int x0 = (DISP_W - total_w) / 2;
	int y0 = (DISP_H - 7 * CARD_SCALE) / 2;

	if (x < x0 || x >= x0 + total_w) {
		return out;
	}
	int li = (x - x0) / cell_w;
	int lx = (x - x0) % cell_w;

	if (lx >= 5 * CARD_SCALE) {
		return out;              /* letter spacing */
	}
	const uint8_t *g = glyph_cols(g_card_text[li]);

	if (!g) {
		return out;
	}
	uint8_t colbits = g[lx / CARD_SCALE];

	for (int k = 0; k < 8; k++) {
		int y = b * 8 + k;

		if (y < y0 || y >= y0 + 7 * CARD_SCALE) {
			continue;
		}
		if ((colbits >> ((y - y0) / CARD_SCALE)) & 1) {
			out &= ~(0x80 >> k);
		}
	}
	return out;
}

static void show_transition(const char *text)
{
	g_card_text = text;
	g_scene = scene_card;
	rebase();
	k_msleep(TRANSITION_MS);
}

/* ---------------- static image scenes ---------------- */

static const uint8_t *g_img;

static uint8_t scene_img(int x, int b)
{
	return g_img[x * DISP_STRIDE + b];
}

/* ---------------- shared RNG ---------------- */

static uint32_t rng = 0x51C3A7;

static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 16;
}

/* ================= TETRIS (self-playing) ================= */

#define T_CELL   12
#define T_WELLW  10
#define T_WELLD  20
#define T_WX0    5
#define T_WY0    1

static uint16_t t_rows[T_WELLD];
static int8_t t_type, t_rot, t_px, t_py;
static uint8_t t_active;
static int8_t t_trot, t_tpx;         /* AI target */

static const uint16_t PIECES[7][4] = {
	{0x00F0, 0x4444, 0x0F00, 0x2222},
	{0x0660, 0x0660, 0x0660, 0x0660},
	{0x0072, 0x0262, 0x0270, 0x0232},
	{0x0036, 0x0462, 0x0360, 0x0231},
	{0x0063, 0x0264, 0x0630, 0x0132},
	{0x0071, 0x0226, 0x0470, 0x0322},
	{0x0074, 0x0622, 0x0170, 0x0223},
};

static int t_collides(const uint16_t *rows, int t, int r, int px, int py)
{
	uint16_t m = PIECES[t][r];

	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			if (!((m >> (i * 4 + j)) & 1)) {
				continue;
			}
			int row = py + i, col = px + j;

			if (col < 0 || col >= T_WELLW || row >= T_WELLD) {
				return 1;
			}
			if (row >= 0 && ((rows[row] >> col) & 1)) {
				return 1;
			}
		}
	}
	return 0;
}

static int t_piece_covers(int row, int col)
{
	if (!t_active) {
		return 0;
	}
	int i = row - t_py, j = col - t_px;

	if (i < 0 || i > 3 || j < 0 || j > 3) {
		return 0;
	}
	return (PIECES[t_type][t_rot] >> (i * 4 + j)) & 1;
}

static uint8_t scene_tetris(int x, int b)
{
	uint8_t out = 0xFF;

	for (int k = 0; k < 8; k++) {
		int y = b * 8 + k;

		if (y >= DISP_H) {
			continue;
		}
		int black = 0;

		if (x >= T_WX0 - 3 && x < T_WX0 + T_WELLD * T_CELL + 3) {
			if (x < T_WX0 || x >= T_WX0 + T_WELLD * T_CELL) {
				black = 1;
			} else if (y < T_WY0 || y >= T_WY0 + T_WELLW * T_CELL) {
				black = 1;
			}
		}
		if (!black && x >= T_WX0 && x < T_WX0 + T_WELLD * T_CELL &&
		    y >= T_WY0 && y < T_WY0 + T_WELLW * T_CELL) {
			int r = T_WELLD - 1 - (x - T_WX0) / T_CELL;
			int c = (y - T_WY0) / T_CELL;
			int lx = (x - T_WX0) % T_CELL, ly = (y - T_WY0) % T_CELL;

			if (((t_rows[r] >> c) & 1) || t_piece_covers(r, c)) {
				if (lx >= 1 && lx <= 10 && ly >= 1 && ly <= 10) {
					black = 1;
				}
			}
		}
		if (black) {
			out &= ~(0x80 >> k);
		}
	}
	return out;
}

static void t_flush_rows(int r0, int r1)
{
	if (r0 < 0) {
		r0 = 0;
	}
	if (r1 > T_WELLD - 1) {
		r1 = T_WELLD - 1;
	}
	flush_cols(T_WX0 + (T_WELLD - 1 - r1) * T_CELL,
		   T_WX0 + (T_WELLD - r0) * T_CELL - 1);
}

/* classic heuristic: lines good, height/holes/bumpiness bad */
static int t_eval(const uint16_t *rows)
{
	int heights[T_WELLW], agg = 0, holes = 0, bump = 0, lines = 0;

	for (int c = 0; c < T_WELLW; c++) {
		heights[c] = 0;
		int seen = 0;

		for (int r = 0; r < T_WELLD; r++) {
			if ((rows[r] >> c) & 1) {
				if (!seen) {
					heights[c] = T_WELLD - r;
					seen = 1;
				}
			} else if (seen) {
				holes++;
			}
		}
		agg += heights[c];
	}
	for (int c = 0; c + 1 < T_WELLW; c++) {
		int d = heights[c] - heights[c + 1];

		bump += d < 0 ? -d : d;
	}
	for (int r = 0; r < T_WELLD; r++) {
		if (rows[r] == (1 << T_WELLW) - 1) {
			lines++;
		}
	}
	return lines * 76 - agg * 51 - holes * 136 - bump * 18;
}

static void t_ai_plan(void)
{
	int best = -1000000;

	t_trot = t_rot;
	t_tpx = t_px;
	for (int r = 0; r < 4; r++) {
		for (int px = -2; px < T_WELLW; px++) {
			if (t_collides(t_rows, t_type, r, px, t_py < 0 ? 0 : t_py)) {
				continue;
			}
			int py = t_py < 0 ? 0 : t_py;

			while (!t_collides(t_rows, t_type, r, px, py + 1)) {
				py++;
			}

			uint16_t sim[T_WELLD];

			for (int i = 0; i < T_WELLD; i++) {
				sim[i] = t_rows[i];
			}
			uint16_t m = PIECES[t_type][r];

			for (int i = 0; i < 4; i++) {
				for (int j = 0; j < 4; j++) {
					if ((m >> (i * 4 + j)) & 1 && py + i >= 0) {
						sim[py + i] |= 1 << (px + j);
					}
				}
			}
			int s = t_eval(sim);

			if (s > best) {
				best = s;
				t_trot = r;
				t_tpx = px;
			}
		}
	}
}

static void t_spawn(void)
{
	t_type = rnd() % 7;
	t_rot = 0;
	t_px = 3;
	t_py = -1;
	t_active = !t_collides(t_rows, t_type, 0, 3, -1);
	if (t_active) {
		t_ai_plan();
	}
}

static int t_lock_and_clear(void)
{
	uint16_t m = PIECES[t_type][t_rot];

	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			if ((m >> (i * 4 + j)) & 1 && t_py + i >= 0) {
				t_rows[t_py + i] |= 1 << (t_px + j);
			}
		}
	}
	t_active = 0;

	int cleared = 0;

	for (int r = T_WELLD - 1; r >= 0; r--) {
		if (t_rows[r] == (1 << T_WELLW) - 1) {
			for (int k = r; k > 0; k--) {
				t_rows[k] = t_rows[k - 1];
			}
			t_rows[0] = 0;
			cleared++;
			r++;
		}
	}
	return cleared;
}

static void scene_run_tetris(void)
{
	int64_t deadline = k_uptime_get() + SCENE_MS;

	for (int i = 0; i < T_WELLD; i++) {
		t_rows[i] = 0;
	}
	g_scene = scene_tetris;
	t_spawn();
	rebase();

	while (k_uptime_get() < deadline) {
		if (!t_active) {
			/* topped out: clear the well and continue the show */
			for (int i = 0; i < T_WELLD; i++) {
				t_rows[i] = 0;
			}
			t_spawn();
			rebase();
			continue;
		}

		int r0 = t_py, r1 = t_py + 3, moved = 0;

		/* one AI action per frame: rotate, then shift, then drop x2 */
		if (t_rot != t_trot &&
		    !t_collides(t_rows, t_type, t_trot, t_px, t_py)) {
			t_rot = t_trot;
			moved = 1;
		} else if (t_px != t_tpx) {
			int step = t_tpx > t_px ? 1 : -1;

			if (!t_collides(t_rows, t_type, t_rot, t_px + step, t_py)) {
				t_px += step;
				moved = 1;
			}
		}
		int fell = 0;

		for (int d = 0; d < 2; d++) {
			if (!t_collides(t_rows, t_type, t_rot, t_px, t_py + 1)) {
				t_py++;
				fell++;
			}
		}
		if (moved || fell) {
			if (t_py + 3 > r1) {
				r1 = t_py + 3;
			}
			if (t_py < r0) {
				r0 = t_py;
			}
			t_flush_rows(r0, r1);
		}
		if (!fell && t_px == t_tpx && t_rot == t_trot) {
			int cleared = t_lock_and_clear();

			t_spawn();
			if (cleared) {
				t_flush_rows(0, T_WELLD - 1);
			} else {
				t_flush_rows(-1, 3);
			}
		}
	}
}

/* ================= SNAKE (self-playing) ================= */

#define S_CELL  8
#define S_COLS  30
#define S_ROWS  14
#define S_N     (S_COLS * S_ROWS)
#define S_PX0   5
#define S_PY0   5

static uint8_t s_occ[(S_N + 7) / 8];
static uint8_t s_dmap[(S_N + 3) / 4];
static uint16_t s_head, s_tail, s_food;
static uint16_t s_len;
static uint8_t s_dir, s_grow;

static const int8_t DC[4] = {0, 1, 0, -1};
static const int8_t DR[4] = {-1, 0, 1, 0};

static int s_get(int i)
{
	return s_occ[i >> 3] & (1 << (i & 7));
}

static void s_set(int i, int v)
{
	if (v) {
		s_occ[i >> 3] |= 1 << (i & 7);
	} else {
		s_occ[i >> 3] &= ~(1 << (i & 7));
	}
}

static int s_dget(int i)
{
	return (s_dmap[i >> 2] >> ((i & 3) * 2)) & 3;
}

static void s_dset(int i, int d)
{
	int sh = (i & 3) * 2;

	s_dmap[i >> 2] = (s_dmap[i >> 2] & ~(3 << sh)) | (d << sh);
}

static uint8_t scene_snake(int x, int b)
{
	uint8_t out = 0xFF;

	for (int k = 0; k < 8; k++) {
		int y = b * 8 + k;

		if (y >= DISP_H) {
			continue;
		}
		int black = 0;

		if (x >= S_PX0 - 2 && x < S_PX0 + S_COLS * S_CELL + 2) {
			if (x < S_PX0 || x >= S_PX0 + S_COLS * S_CELL) {
				if (y >= S_PY0 - 2 && y < S_PY0 + S_ROWS * S_CELL + 2) {
					black = 1;
				}
			} else if ((y >= S_PY0 - 2 && y < S_PY0) ||
				   (y >= S_PY0 + S_ROWS * S_CELL &&
				    y < S_PY0 + S_ROWS * S_CELL + 2)) {
				black = 1;
			}
		}
		if (!black && x >= S_PX0 && x < S_PX0 + S_COLS * S_CELL &&
		    y >= S_PY0 && y < S_PY0 + S_ROWS * S_CELL) {
			int c = (x - S_PX0) / S_CELL;
			int r = (y - S_PY0) / S_CELL;
			int idx = r * S_COLS + c;
			int lx = (x - S_PX0) % S_CELL, ly = (y - S_PY0) % S_CELL;

			if (s_get(idx)) {
				if (lx >= 1 && lx <= 6 && ly >= 1 && ly <= 6) {
					black = 1;
				}
			} else if (idx == s_food) {
				if (lx >= 2 && lx <= 5 && ly >= 2 && ly <= 5) {
					black = 1;
				}
			}
		}
		if (black) {
			out &= ~(0x80 >> k);
		}
	}
	return out;
}

static void s_flush_cell(int idx)
{
	int x0 = S_PX0 + (idx % S_COLS) * S_CELL;

	emit_cols(x0, x0 + S_CELL - 1, 0x24);
}

static void s_flush_cells(const uint16_t *cells, int n)
{
	for (int i = 0; i < n; i++) {
		s_flush_cell(cells[i]);
	}
	epd_set_full_window();
	epd_command(0x22);
	epd_data(DISPLAY_PART_KEEP_ON);
	epd_command(0x20);
	epd_wait_refresh(400);
	for (int i = 0; i < n; i++) {
		int x0 = S_PX0 + (cells[i] % S_COLS) * S_CELL;

		emit_cols(x0, x0 + S_CELL - 1, 0x26);
	}
	epd_set_full_window();
}

static void s_place_food(void)
{
	do {
		s_food = rnd() % S_N;
	} while (s_get(s_food));
}

/* flood-fill reachable count from cell (body = walls) */
static uint16_t s_bfs_q[S_N];

static int s_flood(int start)
{
	uint8_t seen[(S_N + 7) / 8] = {0};
	int qh = 0, qt = 0, count = 0;

	if (s_get(start)) {
		return 0;
	}
	s_bfs_q[qt++] = start;
	seen[start >> 3] |= 1 << (start & 7);
	while (qh < qt) {
		int cur = s_bfs_q[qh++];

		count++;
		int cc = cur % S_COLS, cr = cur / S_COLS;

		for (int d = 0; d < 4; d++) {
			int nc = cc + DC[d], nr = cr + DR[d];

			if (nc < 0 || nc >= S_COLS || nr < 0 || nr >= S_ROWS) {
				continue;
			}
			int ni = nr * S_COLS + nc;

			if (s_get(ni) || (seen[ni >> 3] & (1 << (ni & 7)))) {
				continue;
			}
			seen[ni >> 3] |= 1 << (ni & 7);
			s_bfs_q[qt++] = ni;
		}
	}
	return count;
}

/* greedy toward food, only into regions big enough to survive */
static int s_ai_dir(void)
{
	int hc = s_head % S_COLS, hr = s_head / S_COLS;
	int fc = s_food % S_COLS, fr = s_food / S_COLS;
	int order[4], no = 0;

	/* preferred axes first */
	if (fc != hc) {
		order[no++] = fc > hc ? 1 : 3;
	}
	if (fr != hr) {
		order[no++] = fr > hr ? 2 : 0;
	}
	for (int d = 0; d < 4; d++) {
		int dup = 0;

		for (int i = 0; i < no; i++) {
			if (order[i] == d) {
				dup = 1;
			}
		}
		if (!dup) {
			order[no++] = d;
		}
	}

	int best = -1, best_space = -1;

	for (int i = 0; i < no; i++) {
		int d = order[i];

		if (d == (s_dir ^ 2) && s_len > 1) {
			continue;
		}
		int nc = hc + DC[d], nr = hr + DR[d];

		if (nc < 0 || nc >= S_COLS || nr < 0 || nr >= S_ROWS) {
			continue;
		}
		int ni = nr * S_COLS + nc;

		if (s_get(ni)) {
			continue;
		}
		int space = s_flood(ni);

		if (space >= (int)s_len + 2) {
			return d;        /* first preferred safe direction */
		}
		if (space > best_space) {
			best_space = space;
			best = d;
		}
	}
	return best;                     /* least-bad, or -1 = trapped */
}

static void s_new_game(void)
{
	for (unsigned int i = 0; i < sizeof(s_occ); i++) {
		s_occ[i] = 0;
	}
	s_tail = 7 * S_COLS + 12;
	s_head = 7 * S_COLS + 14;
	s_len = 3;
	s_dir = 1;
	s_grow = 0;
	for (int i = 0; i < 3; i++) {
		s_set(s_tail + i, 1);
		s_dset(s_tail + i, 1);
	}
	s_place_food();
}

static void scene_run_snake(void)
{
	int64_t deadline = k_uptime_get() + SCENE_MS;

	g_scene = scene_snake;
	s_new_game();
	rebase();

	while (k_uptime_get() < deadline) {
		int d = s_ai_dir();

		if (d < 0) {
			s_new_game();
			rebase();
			continue;
		}
		s_dir = d;

		int nh = (s_head / S_COLS + DR[d]) * S_COLS +
			 (s_head % S_COLS + DC[d]);
		uint16_t chg[3];
		int n = 0;

		s_dset(s_head, d);
		s_set(nh, 1);
		s_head = nh;
		chg[n++] = nh;

		if (nh == s_food) {
			s_grow += 2;
			s_len += 2;
			if (s_len >= S_N - 4) {
				s_new_game();
				rebase();
				continue;
			}
			s_place_food();
			chg[n++] = s_food;
		}
		if (s_grow) {
			s_grow--;
		} else {
			uint16_t t = s_tail;
			int td = s_dget(t);

			s_tail = t + DC[td] + DR[td] * S_COLS;
			s_set(t, 0);
			chg[n++] = t;
		}
		s_flush_cells(chg, n);
	}
}


/* ================= PONG (self-playing, inverted court) ================= */

#define P_BALL   8
#define P_PADW   4
#define P_PADH   28
#define P_LX     6
#define P_RX     (DISP_W - 6 - P_PADW)
#define P_LSTEP  5                   /* left paddle a bit slower: drama */
#define P_RSTEP  6
#define P_BDX    8

static int p_lpad, p_rpad, p_bx, p_by, p_bdx, p_bdy;

static uint8_t p_span_black(uint8_t out, int b, int y0, int y1)
{
	for (int i = 0; i < 8; i++) {
		int y = b * 8 + i;

		if (y >= y0 && y <= y1) {
			out &= ~(0x80 >> i);
		}
	}
	return out;
}

static uint8_t scene_pong(int x, int b)
{
	uint8_t out = 0xFF;

	if (b * 8 >= DISP_H) {
		return (uint8_t)~out;
	}
	if (x == DISP_W / 2 && (b & 1) == 0) {
		out = 0x00;                  /* dashed centre line */
	}
	if (x >= P_LX && x < P_LX + P_PADW) {
		out = p_span_black(out, b, p_lpad, p_lpad + P_PADH - 1);
	}
	if (x >= P_RX && x < P_RX + P_PADW) {
		out = p_span_black(out, b, p_rpad, p_rpad + P_PADH - 1);
	}
	if (x >= p_bx && x < p_bx + P_BALL) {
		out = p_span_black(out, b, p_by, p_by + P_BALL - 1);
	}
	return (uint8_t)~out;            /* inverted court */
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void p_reset(void)
{
	p_lpad = p_rpad = (DISP_H - P_PADH) / 2;
	p_bx = (DISP_W - P_BALL) / 2;
	p_by = (DISP_H - P_BALL) / 2;
	p_bdx = (rnd() & 1) ? P_BDX : -P_BDX;
	p_bdy = (int)(rnd() % 9) - 4;
	if (p_bdy == 0) {
		p_bdy = 3;
	}
}

/* paddles drift toward the ball, speed-capped (imperfection = drama) */
static void p_track(int *pad, int step)
{
	int target = p_by + P_BALL / 2 - P_PADH / 2;
	int d = clampi(target - *pad, -step, step);

	*pad = clampi(*pad + d, 0, DISP_H - P_PADH);
}

static int p_deflect(int pad_y)
{
	int off = (p_by + P_BALL / 2) - (pad_y + P_PADH / 2);

	return clampi(off / 3, -7, 7);
}

/* returns 0 when a paddle misses */
static int p_ball_step(void)
{
	p_bx += p_bdx;
	p_by += p_bdy;

	if (p_by < 0) {
		p_by = 0;
		p_bdy = -p_bdy;
	} else if (p_by + P_BALL > DISP_H) {
		p_by = DISP_H - P_BALL;
		p_bdy = -p_bdy;
	}

	if (p_bdx < 0 && p_bx <= P_LX + P_PADW) {
		if (p_by + P_BALL > p_lpad && p_by < p_lpad + P_PADH) {
			p_bx = P_LX + P_PADW;
			p_bdx = -p_bdx;
			p_bdy = p_deflect(p_lpad);
		} else if (p_bx + P_BALL < P_LX) {
			return 0;
		}
	}
	if (p_bdx > 0 && p_bx + P_BALL >= P_RX) {
		if (p_by + P_BALL > p_rpad && p_by < p_rpad + P_PADH) {
			p_bx = P_RX - P_BALL;
			p_bdx = -p_bdx;
			p_bdy = p_deflect(p_rpad);
		} else if (p_bx > P_RX + P_PADW) {
			return 0;
		}
	}
	return 1;
}

static void scene_run_pong(void)
{
	int64_t deadline = k_uptime_get() + SCENE_MS;

	g_scene = scene_pong;
	p_reset();
	rebase();

	while (k_uptime_get() < deadline) {
		int obx = p_bx, olp = p_lpad, orp = p_rpad;
		int alive = p_ball_step();

		p_track(&p_lpad, P_LSTEP);
		p_track(&p_rpad, P_RSTEP);

		if (!alive) {
			p_reset();
			rebase();
			continue;
		}

		/* ball sweep span */
		int xa = obx < p_bx ? obx : p_bx;
		int xb = (obx > p_bx ? obx : p_bx) + P_BALL - 1;

		emit_cols(xa, xb, 0x24);
		if (olp != p_lpad) {
			emit_cols(P_LX, P_LX + P_PADW - 1, 0x24);
		}
		if (orp != p_rpad) {
			emit_cols(P_RX, P_RX + P_PADW - 1, 0x24);
		}
		epd_set_full_window();
		epd_command(0x22);
		epd_data(DISPLAY_PART_KEEP_ON);
		epd_command(0x20);
		epd_wait_refresh(400);
		emit_cols(xa, xb, 0x26);
		if (olp != p_lpad) {
			emit_cols(P_LX, P_LX + P_PADW - 1, 0x26);
		}
		if (orp != p_rpad) {
			emit_cols(P_RX, P_RX + P_PADW - 1, 0x26);
		}
		epd_set_full_window();
	}
}

/* ================= static scenes ================= */

static void scene_show_esl(void)
{
	g_img = img_esl;
	g_scene = scene_img;
	rebase();
	k_msleep(SCENE_MS);
}

static void scene_show_gray4(const uint8_t *p24, const uint8_t *p26)
{
	epd_show_gray4(p24, p26);
	k_msleep(SCENE_MS);
}

/* ================= main loop ================= */

int main(void)
{
	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	printk("\npixpaper mega demo on rp2350, R1.0.0\n");

	epd_hw_reset();
	k_msleep(500);
	epd_reg_init();

	while (1) {
		rng ^= (uint32_t)k_cycle_get_32();

		printk("scene: tetris\n");
		show_transition("TETRIS");
		scene_run_tetris();

		printk("scene: snake\n");
		show_transition("SNAKE");
		scene_run_snake();

		printk("scene: pong\n");
		show_transition("PONG");
		scene_run_pong();

		printk("scene: esl\n");
		show_transition("ESL");
		scene_show_esl();

		printk("scene: gray4 test\n");
		show_transition("GRAY-4");
		scene_show_gray4(img_gray4_test_p24, img_gray4_test_p26);

		printk("scene: gray4 nagoya\n");
		show_transition("NAGOYA");
		scene_show_gray4(img_nagoya_gray4_p24, img_nagoya_gray4_p26);
	}

	return 0;
}
