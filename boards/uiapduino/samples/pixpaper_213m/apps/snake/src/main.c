/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * Snake on a PixPaper 2.13" mono e-paper (250x122, landscape),
 * UIAPduino Pro Micro CH32V003, Zephyr. Controlled over the UART console
 * (115200 8N1, TX=PD5/RX=PD6):
 *
 *   w / a / s / d = steer (reversal ignored)
 *   any key restarts after game over
 *
 * Rendering: the hardware-verified anti-ghosting recipe from the ttt game -
 * 0x37 all zeros (ping-pong OFF), partial flow = write changed cells to 0x24
 * -> kick (BUSY rise-then-fall) -> sync the same cells to 0x26 (GxEPD2
 * canonical); a full refresh runs only at game start/restart. Only the cells that changed each
 * tick (new head, vacated tail, new food) are rewritten.
 *
 * No framebuffer (2KB SRAM): the scene is procedural per gate column; the
 * snake body is a 2-bit-per-cell direction map (tail follows head by walking
 * it), 105B + a 53B occupancy bitmap for the 30x14 grid.
 *
 * Board must be reworked to 3.3V (Volt-Sel jumper).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>

#define APP_VERSION "R1.0.0"

#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec sck  = GPIO_DT_SPEC_GET(ZUSER, sck_gpios);
static const struct gpio_dt_spec mosi = GPIO_DT_SPEC_GET(ZUSER, mosi_gpios);
static const struct gpio_dt_spec cs   = GPIO_DT_SPEC_GET(ZUSER, cs_gpios);
static const struct gpio_dt_spec dc   = GPIO_DT_SPEC_GET(ZUSER, dc_gpios);
static const struct gpio_dt_spec rst  = GPIO_DT_SPEC_GET(ZUSER, rst_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(ZUSER, busy_gpios);

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

#define DISP_W        250
#define DISP_STRIDE   16
#define DISP_GATE_MAX (DISP_W - 1)
#define COURT_H       122

/* ---------------- playfield geometry ---------------- */
#define CELLPX  8
#define GCOLS   30
#define GROWS   14
#define NCELLS  (GCOLS * GROWS)              /* 420 */
#define PX0     5                            /* playfield 5..244 */
#define PY0     5                            /* playfield 5..116 */
#define PXW     (GCOLS * CELLPX)
#define PYH     (GROWS * CELLPX)

#define GROW_PER_FOOD 2

/* ---------------- tiny UART I/O ---------------- */

static void uputs(const char *s)
{
	while (*s) {
		uart_poll_out(uart, *s++);
	}
}

static void uput_u16(uint16_t v)
{
	char b[6];
	int i = 0;

	do {
		b[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i) {
		uart_poll_out(uart, b[--i]);
	}
}

/* ---------------- panel low level (bit-banged SPI mode 0) ---------------- */

static void sleep_ms(uint32_t ms)
{
	k_busy_wait(ms * 1000u);
}

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

/* wait for a refresh to COMPLETE: BUSY rises a few ms after 0x20, so wait
 * for the rise first, then the fall; blind-wait if it never rises */
static void epd_wait_refresh(uint32_t blind_ms)
{
	int rose = 0;

	for (int i = 0; i < 300; i++) {
		if (gpio_pin_get_dt(&busy) == 1) {
			rose = 1;
			break;
		}
		k_busy_wait(1000);
	}
	if (!rose) {
		sleep_ms(blind_ms);
		return;
	}
	for (int i = 0; i < 8000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_busy_wait(1000);
	}
}

/* short waits inside init sequences (BUSY may already be low and stay low) */
static void epd_wait_idle(void)
{
	sleep_ms(2);
	for (int i = 0; i < 5000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_busy_wait(1000);
	}
}

static void epd_hw_reset(void)
{
	sleep_ms(50);
	gpio_pin_set_dt(&rst, 0);
	sleep_ms(50);
	gpio_pin_set_dt(&rst, 1);
	sleep_ms(50);
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

/* ---------------- partial update mode (LUT from the pet example) -------- */

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

/* SPEED KNOB: phase-0 length in frames - dominates partial refresh time
 * (the tick rate). ttt uses 0x10; 0x08 halves the drive time for a faster
 * game. If erases turn weak (tail residue creeping in), raise toward 0x10. */
#define PHASE0_TP 0x08
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

	/* 0x37 ALL ZERO - ping-pong OFF. With 0x40 the controller alternates
	 * which physical plane the 0x24 address targets after every mode-2
	 * refresh; partial-area writes then leave stale content in the other
	 * plane, which resurfaces one kick later as solid "zombie" artifacts
	 * (this was the original snake's unfixable trail). */
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
	sleep_ms(2);
	gpio_pin_set_dt(&rst, 1);
	sleep_ms(2);

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

static void epd_partial_kick(void)
{
	epd_set_full_window();
	epd_command(0x22);
	epd_data(DISPLAY_PART_KEEP_ON);
	epd_command(0x20);
	epd_wait_refresh(400);
}

/* ---------------- snake state ---------------- */

/* body = occupancy bitmap + 2-bit direction map (each occupied cell stores
 * the direction toward the next segment / head, so the tail can follow) */
static uint8_t occ[(NCELLS + 7) / 8];
static uint8_t dmap[(NCELLS + 3) / 4];
static uint16_t head, tail;
static uint16_t snake_len;
static uint8_t dir;                   /* 0=up 1=right 2=down 3=left */
static uint8_t grow;
static uint16_t food;

static const int8_t DC[4] = {0, 1, 0, -1};
static const int8_t DR[4] = {-1, 0, 1, 0};

static int occ_get(int i)
{
	return occ[i >> 3] & (1 << (i & 7));
}

static void occ_set(int i, int v)
{
	if (v) {
		occ[i >> 3] |= 1 << (i & 7);
	} else {
		occ[i >> 3] &= ~(1 << (i & 7));
	}
}

static int dmap_get(int i)
{
	return (dmap[i >> 2] >> ((i & 3) * 2)) & 3;
}

static void dmap_set(int i, int d)
{
	int sh = (i & 3) * 2;

	dmap[i >> 2] = (dmap[i >> 2] & ~(3 << sh)) | (d << sh);
}

static uint32_t rng = 0x2C3A9E51;

static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 16;
}

static void place_food(void)
{
	do {
		food = rnd() % NCELLS;
	} while (occ_get(food));
}

/* ---------------- procedural scene (no framebuffer) ---------------- */

static uint8_t span_black(uint8_t out, int b, int y0, int y1)
{
	for (int i = 0; i < 8; i++) {
		int y = b * 8 + i;

		if (y >= y0 && y <= y1) {
			out &= ~(0x80 >> i);
		}
	}
	return out;
}

static uint8_t scene_byte(int x, int b)
{
	uint8_t out = 0xFF;

	if (b * 8 >= COURT_H) {
		return out;
	}

	/* 2px border frame around the playfield */
	if (x >= PX0 - 2 && x < PX0 + PXW + 2) {
		if (x < PX0 || x >= PX0 + PXW) {
			out = span_black(out, b, PY0 - 2, PY0 + PYH + 1);
		} else {
			out = span_black(out, b, PY0 - 2, PY0 - 1);
			out = span_black(out, b, PY0 + PYH, PY0 + PYH + 1);
		}
	}
	if (x < PX0 || x >= PX0 + PXW) {
		return out;
	}

	int c = (x - PX0) / CELLPX;
	int lx = (x - PX0) % CELLPX;

	for (int r = 0; r < GROWS; r++) {
		int idx = r * GCOLS + c;
		int cy = PY0 + r * CELLPX;

		if (occ_get(idx)) {
			/* body segment: 6x6 square, 1px gap between segments */
			if (lx >= 1 && lx <= 6) {
				out = span_black(out, b, cy + 1, cy + 6);
			}
		} else if (idx == food) {
			/* food: small 4x4 square */
			if (lx >= 2 && lx <= 5) {
				out = span_black(out, b, cy + 2, cy + 5);
			}
		}
	}
	return out;
}

static void emit_cols(int x0, int x1, uint8_t ram_cmd)
{
	if (x0 < 0) {
		x0 = 0;
	}
	if (x1 > DISP_W - 1) {
		x1 = DISP_W - 1;
	}

	epd_set_window(0x00, DISP_STRIDE - 1,
		       DISP_GATE_MAX - x0, DISP_GATE_MAX - x1);
	epd_set_cursor(0x00, DISP_GATE_MAX - x0);
	epd_command(ram_cmd);

	for (int x = x0; x <= x1; x++) {
		for (int b = 0; b < DISP_STRIDE; b++) {
			epd_data(scene_byte(x, b));
		}
	}
}

static void emit_cell(int idx, uint8_t ram_cmd)
{
	int x0 = PX0 + (idx % GCOLS) * CELLPX;

	emit_cols(x0, x0 + CELLPX - 1, ram_cmd);
}

/* Full scene into both planes + full refresh, then back to partial mode.
 * Keeps 0x24 and 0x26 identical at the moment we enter partial mode and
 * pays off any accumulated partial-update ghosting. */
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
				epd_data(scene_byte(x, b));
			}
		}
	}

	epd_command(0x22);
	epd_data(0xF7);
	epd_command(0x20);
	epd_wait_refresh(2500);

	epd_partial_begin();
}

/* update the changed cells on glass: 0x24 -> kick -> sync 0x26 (the
 * hardware-verified ttt recipe; sync is only safe with ping-pong OFF) */
static void flush_cells(const uint16_t *cells, int n)
{
	for (int i = 0; i < n; i++) {
		emit_cell(cells[i], 0x24);
	}
	epd_partial_kick();
	for (int i = 0; i < n; i++) {
		emit_cell(cells[i], 0x26);
	}
	epd_set_full_window();
}

/* ---------------- game ---------------- */

static void new_game(void)
{
	for (unsigned int i = 0; i < sizeof(occ); i++) {
		occ[i] = 0;
	}

	/* 3 segments, heading right, middle of the field */
	tail = 7 * GCOLS + 12;
	head = 7 * GCOLS + 14;
	snake_len = 3;
	dir = 1;
	grow = 0;
	for (int i = 0; i < 3; i++) {
		occ_set(tail + i, 1);
		dmap_set(tail + i, 1);
	}

	place_food();
	rebase();
	uputs("wasd to steer - press a key to start\r\n");
}

static void wait_fresh_key(void)
{
	unsigned char c;

	for (;;) {                            /* drain, require quiet */
		while (uart_poll_in(uart, &c) == 0) {
		}
		sleep_ms(300);
		if (uart_poll_in(uart, &c) != 0) {
			break;
		}
	}
	while (uart_poll_in(uart, &c) != 0) {
		k_busy_wait(1000);
	}
	rng ^= k_cycle_get_32();
}

/* drain pending keys, apply the last valid steer */
static void poll_keys(void)
{
	unsigned char c;

	while (uart_poll_in(uart, &c) == 0) {
		int d = -1;

		rng ^= k_cycle_get_32();
		if (c == 'w' || c == 'W') {
			d = 0;
		} else if (c == 'd' || c == 'D') {
			d = 1;
		} else if (c == 's' || c == 'S') {
			d = 2;
		} else if (c == 'a' || c == 'A') {
			d = 3;
		}
		if (d >= 0 && d != (dir ^ 2)) {   /* ignore reversal */
			dir = d;
		}
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

	uputs("\r\npixpaper snake " APP_VERSION
	      "\r\nwasd = steer\r\npreparing panel...\r\n");

	epd_hw_reset();
	sleep_ms(1000);
	new_game();
	wait_fresh_key();


	for (;;) {
		poll_keys();

		/* advance the head */
		int hc = head % GCOLS + DC[dir];
		int hr = head / GCOLS + DR[dir];
		int hit = hc < 0 || hc >= GCOLS || hr < 0 || hr >= GROWS;
		int nh = hr * GCOLS + hc;

		if (!hit && occ_get(nh)) {
			hit = 1;
		}
		if (hit) {
			uputs("GAME OVER  len=");
			uput_u16(snake_len);
			uputs("  press any key for a new game\r\n");
			wait_fresh_key();
			new_game();
			wait_fresh_key();
			continue;
		}

		uint16_t chg[3];
		int n = 0;

		dmap_set(head, dir);
		occ_set(nh, 1);
		head = nh;
		chg[n++] = nh;

		if (nh == food) {
			grow += GROW_PER_FOOD;
			snake_len += GROW_PER_FOOD;
			if (snake_len >= NCELLS - GROW_PER_FOOD) {
				uputs("YOU WIN!  press any key\r\n");
				wait_fresh_key();
				new_game();
				wait_fresh_key();
				continue;
			}
			place_food();
			chg[n++] = food;
			uputs("len=");
			uput_u16(snake_len);
			uputs("\r\n");
		}
		if (grow) {
			grow--;
		} else {
			uint16_t t = tail;

			tail = t + DC[dmap_get(t)] + DR[dmap_get(t)] * GCOLS;
			occ_set(t, 0);
			chg[n++] = t;
		}

		flush_cells(chg, n);
	}

	return 0;
}
