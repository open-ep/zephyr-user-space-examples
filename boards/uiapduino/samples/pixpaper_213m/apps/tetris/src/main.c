/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * Self-playing Tetris on a PixPaper 2.13" mono e-paper (250x122), UIAPduino
 * Pro Micro CH32V003, Zephyr. PORTRAIT: the panel is held with its long
 * (gate) axis vertical, so the 10-wide x 20-deep well has gravity running
 * along the gate axis - exactly the axis partial updates can window cheaply.
 *
 * No input: a greedy one-piece-lookahead AI picks every drop (see plan()),
 * so the demo runs unattended. Game over restarts after a short pause.
 *
 * Rendering: partial-update mode (LUT from the Linux pet example). There is
 * no framebuffer (2KB SRAM): the well frame and every cell - locked or
 * falling - are generated procedurally per gate column from a 20 x uint16
 * bitmap, and only the well rows the piece touched are rewritten. 0x37
 * ping-pong is OFF and 0x26 is manually synced after every kick (the
 * ttt/pong-proven GxEPD2-style flow) so erases always run the strong
 * waveform. Line clears trigger a full refresh - the flash doubles as
 * feedback and pays off accumulated ghosting.
 *
 * Board must be reworked to 3.3V (Volt-Sel jumper).
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

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

/* set to 1 if the panel is rotated the other way (flips both axes) */
#define ROT_180 0

/* ---------------- well geometry (portrait) ---------------- */
#define CELL    12
#define WELL_W  10                   /* cells across the panel's short axis */
#define WELL_D  20                   /* cells along the gate axis (gravity) */
#define WX0     5                    /* well interior: panel x 5..244 */
#define WY0     1                    /* well interior: panel y 1..120 */

#define TICK_MS      20              /* pause on top of each refresh */
#define GAMEOVER_MS  3000            /* show the dead well this long */
#define GHOST_EVERY  120             /* fallback rebase period (partials) */

/* ---------------- game state ---------------- */
static uint16_t rows_occ[WELL_D];    /* bit c (0..9) = cell occupied */
static int8_t cur_t, cur_r, cur_px, cur_py;  /* piece type/rot/4x4 box pos */
static int8_t tgt_r, tgt_px;         /* AI's chosen landing pose */
static uint8_t cur_active;
static int8_t next_t;
static uint16_t kicks;
static uint32_t rng = 0x7E7515;

/* 7 tetrominoes x 4 rotations, 4x4 box, bit (i*4+j) = row i col j */
static const uint16_t PIECES[7][4] = {
	{0x00F0, 0x4444, 0x0F00, 0x2222},   /* I */
	{0x0660, 0x0660, 0x0660, 0x0660},   /* O */
	{0x0072, 0x0262, 0x0270, 0x0232},   /* T */
	{0x0036, 0x0462, 0x0360, 0x0231},   /* S */
	{0x0063, 0x0264, 0x0630, 0x0132},   /* Z */
	{0x0071, 0x0226, 0x0470, 0x0322},   /* J */
	{0x0074, 0x0622, 0x0170, 0x0223},   /* L */
};

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

#define PHASE0_TP 0x10
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

	/* 0x37 ALL ZERO - ping-pong OFF (the ttt-proven fix): with 0x40 the
	 * 0x24 address alternates physical planes per refresh, so partial-area
	 * writes resurrect stale content one kick later ("zombie" artifacts). */
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

/* Kick one partial refresh and wait for it to COMPLETE. BUSY takes a few
 * ms to assert after 0x20 - waiting only for "BUSY low" races that window
 * and returns while the refresh is still running, so the next frame's RAM
 * writes interrupt the waveform mid-flight (root cause of the ghosting
 * chaos across five test videos). Wait for the rise, then the fall. */
static void epd_partial_kick(void)
{
	epd_set_full_window();
	epd_command(0x22);
	epd_data(DISPLAY_PART_KEEP_ON);
	epd_command(0x20);

	int rose = 0;

	for (int i = 0; i < 300; i++) {
		if (gpio_pin_get_dt(&busy) == 1) {
			rose = 1;
			break;
		}
		k_busy_wait(1000);
	}
	if (!rose) {
		sleep_ms(400);           /* BUSY never seen: blind-wait a full kick */
		return;
	}
	for (int i = 0; i < 5000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_busy_wait(1000);
	}
}

/* ---------------- procedural scene (no framebuffer) ---------------- */

/* does the active piece cover well cell (row, col)? */
static int piece_covers(int row, int col)
{
	if (!cur_active) {
		return 0;
	}
	int i = row - cur_py, j = col - cur_px;

	if (i < 0 || i > 3 || j < 0 || j > 3) {
		return 0;
	}
	return (PIECES[cur_t][cur_r] >> (i * 4 + j)) & 1;
}

/* one byte of the scene at gate column x, byte row b */
static uint8_t scene_byte(int x, int b)
{
	uint8_t out = 0xFF;

#if ROT_180
	x = DISP_W - 1 - x;
#endif
	for (int k = 0; k < 8; k++) {
		int y = b * 8 + k;

		if (y >= DISP_H) {
			continue;
		}
#if ROT_180
		y = DISP_H - 1 - y;
#endif
		int black = 0;

		/* well frame: side walls along y, end walls along x */
		if (x >= WX0 - 3 && x < WX0 + WELL_D * CELL + 3) {
			if (x < WX0 || x >= WX0 + WELL_D * CELL) {
				black = 1;            /* end walls (3px) */
			} else if (y < WY0 || y >= WY0 + WELL_W * CELL) {
				black = 1;            /* side walls (1px) */
			}
		}

		if (!black && x >= WX0 && x < WX0 + WELL_D * CELL &&
		    y >= WY0 && y < WY0 + WELL_W * CELL) {
			/* bottom row (r=WELL_D-1) sits at the WX0 end */
			int r = WELL_D - 1 - (x - WX0) / CELL;
			int c = (y - WY0) / CELL;
			int lx = (x - WX0) % CELL, ly = (y - WY0) % CELL;

			if (((rows_occ[r] >> c) & 1) || piece_covers(r, c)) {
				/* filled block, 1px gap between neighbours */
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

/* rewrite the full stride for screen columns [x0, x1] into a RAM plane */
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

/* full scene into both planes + one full refresh (pet's base-map pattern) */
static void epd_write_base(void)
{
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
	epd_wait_idle();
}

static void rebase(void)
{
	epd_hw_reset();
	epd_reg_init();
	epd_write_base();
	epd_partial_begin();
	kicks = 0;
}

/* well row range -> panel column range (handles the bottom-at-WX0 mapping) */
static void rows_to_cols(int r0, int r1, int *xa, int *xb)
{
	int a = WX0 + (WELL_D - 1 - r1) * CELL;
	int b = WX0 + (WELL_D - r0) * CELL - 1;

#if ROT_180
	int na = DISP_W - 1 - b;

	b = DISP_W - 1 - a;
	a = na;
#endif
	*xa = a;
	*xb = b;
}

/* update well rows r0..r1 on glass: 0x24 -> kick -> sync 0x26 */
static void flush_rows(int r0, int r1)
{
	int xa, xb;

	if (r0 < 0) {
		r0 = 0;
	}
	if (r1 > WELL_D - 1) {
		r1 = WELL_D - 1;
	}
	rows_to_cols(r0, r1, &xa, &xb);

	emit_cols(xa, xb, 0x24);
	epd_partial_kick();
	/* reference plane = exact copy of this frame (0x37 ping-pong is not
	 * effective on this panel), so next frame's every change gets the
	 * full-strength waveform */
	emit_cols(xa, xb, 0x26);
	epd_set_full_window();
	kicks++;
}

/* ---------------- game logic ---------------- */

static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 16;
}

/* piece (t, r) at box position (px, py) against board `occ` */
static int collides_on(const uint16_t *occ, int t, int r, int px, int py)
{
	uint16_t m = PIECES[t][r];

	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			if (!((m >> (i * 4 + j)) & 1)) {
				continue;
			}
			int row = py + i, col = px + j;

			if (col < 0 || col >= WELL_W || row >= WELL_D) {
				return 1;
			}
			if (row >= 0 && ((occ[row] >> col) & 1)) {
				return 1;
			}
		}
	}
	return 0;
}

#define collides(t, r, px, py) collides_on(rows_occ, (t), (r), (px), (py))

/* OR piece into `occ`, drop full rows; returns lines cleared */
static int lock_on(uint16_t *occ, int t, int r, int px, int py)
{
	uint16_t m = PIECES[t][r];

	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			if ((m >> (i * 4 + j)) & 1) {
				int row = py + i;

				if (row >= 0) {
					occ[row] |= 1 << (px + j);
				}
			}
		}
	}

	int cleared = 0;

	for (int row = WELL_D - 1; row >= 0; row--) {
		if (occ[row] == (1 << WELL_W) - 1) {
			for (int k = row; k > 0; k--) {
				occ[k] = occ[k - 1];
			}
			occ[0] = 0;
			cleared++;
			row++;                /* re-check the shifted-down row */
		}
	}
	return cleared;
}

/* ---------------- autoplay AI ---------------- */

/* Board quality after a hypothetical drop: the classic four-feature linear
 * heuristic (Lee's "near-perfect player" weights x100, integer maths only):
 *   + lines cleared          (reward)
 *   - aggregate column height (stay low)
 *   - holes                  (empty cell with something above it)
 *   - bumpiness              (sum of neighbour height differences)
 * ponytail: one-piece lookahead, no next-piece search - plays for hours on
 * this board; add next_t to the search if it ever tops out too soon. */
static int32_t evaluate(const uint16_t *occ, int cleared)
{
	int agg = 0, holes = 0, bump = 0, prev_h = -1;

	for (int c = 0; c < WELL_W; c++) {
		int h = 0, seen = 0;

		for (int row = 0; row < WELL_D; row++) {
			if ((occ[row] >> c) & 1) {
				if (!seen) {
					h = WELL_D - row;
					seen = 1;
				}
			} else if (seen) {
				holes++;
			}
		}
		agg += h;
		if (prev_h >= 0) {
			bump += h > prev_h ? h - prev_h : prev_h - h;
		}
		prev_h = h;
	}
	return 76 * cleared - 51 * agg - 36 * holes - 18 * bump;
}

/* choose (tgt_r, tgt_px) for the current piece by exhaustive drop search */
static void plan(void)
{
	uint16_t tmp[WELL_D];
	int32_t best = INT32_MIN;

	tgt_r = cur_r;
	tgt_px = cur_px;

	for (int r = 0; r < 4; r++) {
		for (int px = -2; px < WELL_W; px++) {
			if (collides(cur_t, r, px, cur_py)) {
				continue;     /* not reachable at spawn height */
			}
			int py = cur_py;

			while (!collides(cur_t, r, px, py + 1)) {
				py++;
			}
			for (int k = 0; k < WELL_D; k++) {
				tmp[k] = rows_occ[k];
			}
			int32_t s = evaluate(tmp, lock_on(tmp, cur_t, r, px, py));

			if (s > best) {
				best = s;
				tgt_r = r;
				tgt_px = px;
			}
		}
	}
}

static void spawn(void)
{
	cur_t = next_t;
	rng ^= k_cycle_get_32();          /* refresh timing jitter as entropy */
	next_t = rnd() % 7;
	cur_r = 0;
	cur_px = 3;
	cur_py = -1;
	cur_active = !collides(cur_t, cur_r, cur_px, cur_py);
	if (cur_active) {
		plan();
	}
}

static void new_game(void)
{
	for (int r = 0; r < WELL_D; r++) {
		rows_occ[r] = 0;
	}
	next_t = rnd() % 7;
	spawn();
	rebase();
}

/* apply a legal move and redraw the rows spanned by old and new box */
static void move_to(int nr, int npx, int npy)
{
	int r0 = cur_py < npy ? cur_py : npy;
	int r1 = (cur_py > npy ? cur_py : npy) + 3;

	cur_r = nr;
	cur_px = npx;
	cur_py = npy;
	flush_rows(r0, r1);
}

/* one autoplay tick: rotate, else shift, else fall/lock */
static void step(void)
{
	if (cur_r != tgt_r) {
		int nr = (cur_r + 1) & 3;

		if (!collides(cur_t, nr, cur_px, cur_py)) {
			move_to(nr, cur_px, cur_py);
			return;
		}
	}
	if (cur_px != tgt_px) {
		int npx = cur_px + (tgt_px > cur_px ? 1 : -1);

		if (!collides(cur_t, cur_r, npx, cur_py)) {
			move_to(cur_r, npx, cur_py);
			return;
		}
	}
	if (!collides(cur_t, cur_r, cur_px, cur_py + 1)) {
		move_to(cur_r, cur_px, cur_py + 1);
		return;
	}

	int cleared = lock_on(rows_occ, cur_t, cur_r, cur_px, cur_py);

	cur_active = 0;
	spawn();
	if (cleared) {
		rebase();             /* clear flash = feedback + ghost payoff */
	} else if (cur_active) {
		flush_rows(-1, 3);    /* locked cells look identical; spawn only */
	}
	if (!cur_active) {
		sleep_ms(GAMEOVER_MS);
		new_game();
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

	epd_hw_reset();
	sleep_ms(1000);
	new_game();

	for (;;) {
		sleep_ms(TICK_MS);
		step();
		if (kicks >= GHOST_EVERY) {
			rebase();
		}
	}

	return 0;
}
