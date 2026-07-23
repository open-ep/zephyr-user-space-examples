/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * Tic-tac-toe on a PixPaper 2.13" mono e-paper (250x122, landscape),
 * UIAPduino Pro Micro CH32V003, Zephyr. Controlled over the UART console
 * (115200 8N1, TX=PD5/RX=PD6):
 *
 *   w / a / s / d = move the selection cursor
 *   Enter         = place your O (the computer answers with X)
 *   any key restarts after the game ends
 *
 * Display strategy - built around this panel's verified strengths:
 * pieces are drawn once and NEVER erased, so the moving-object ghosting
 * problem does not exist here. The only transient element is the thin
 * cursor frame, updated with partial refreshes for instant feedback; every
 * placed move triggers one FULL refresh (a natural "move confirmed" flash)
 * which wipes any accumulated cursor ghosting. Idle = zero refreshes.
 *
 * BUSY is always waited rise-then-fall (a low-only wait races the few-ms
 * assertion delay after 0x20 and truncates the waveform).
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

/* ---------------- board geometry ---------------- */
#define CELL   34
#define LINE   2
#define GRID   (3 * CELL + 2 * LINE)          /* 106 px square */
#define GX0    ((DISP_W - GRID) / 2)          /* 72 */
#define GY0    ((COURT_H - GRID) / 2)         /* 8 */

#define cell_x(c) (GX0 + (c) * (CELL + LINE))
#define cell_y(r) (GY0 + (r) * (CELL + LINE))

/* ---------------- tiny UART I/O ---------------- */

static void uputs(const char *s)
{
	while (*s) {
		uart_poll_out(uart, *s++);
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

	/* 0x37 ALL ZERO - ping-pong OFF. With 0x40 the controller alternates
	 * which physical plane the 0x24 address targets after every mode-2
	 * refresh; partial-area writes then leave stale content in the other
	 * plane, which resurfaces one kick later ("zombie" cursor frames at
	 * cells written two kicks ago - the user's exact repro: first move
	 * always clean, artifacts from the second move on). The pet example
	 * survives 0x40 only because it rewrites the FULL frame every kick. */
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

/* ---------------- procedural scene (no framebuffer) ---------------- */

static char board[9];                 /* 0 empty, 'O' player, 'X' computer */
static int cur;                       /* cursor cell 0..8 */
static int cursor_visible;

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

/* is local pixel (lx, ly) inside the glyph for piece p? */
static int glyph_pixel(char p, int lx, int ly)
{
	if (p == 'O') {
		int dx = lx - CELL / 2, dy = ly - CELL / 2;
		int d2 = dx * dx + dy * dy;

		return d2 <= 13 * 13 && d2 >= 9 * 9;
	}
	if (p == 'X') {
		if (lx < 6 || lx >= CELL - 6 || ly < 6 || ly >= CELL - 6) {
			return 0;
		}
		int a = lx - ly;
		int c = lx + ly - (CELL - 1);

		return (a >= -2 && a <= 2) || (c >= -2 && c <= 2);
	}
	return 0;
}

static uint8_t scene_byte(int x, int b)
{
	uint8_t out = 0xFF;

	if (b * 8 >= COURT_H) {
		return out;
	}

	/* grid lines */
	if (x >= GX0 && x < GX0 + GRID) {
		for (int k = 1; k <= 2; k++) {
			int ly0 = GY0 + k * CELL + (k - 1) * LINE;

			out = span_black(out, b, ly0, ly0 + LINE - 1);
		}
	}
	for (int k = 1; k <= 2; k++) {
		int lx0 = GX0 + k * CELL + (k - 1) * LINE;

		if (x >= lx0 && x < lx0 + LINE) {
			out = span_black(out, b, GY0, GY0 + GRID - 1);
		}
	}

	/* pieces + cursor, per column of the cell x falls into */
	for (int c = 0; c < 3; c++) {
		int cx = cell_x(c);

		if (x < cx || x >= cx + CELL) {
			continue;
		}
		int lx = x - cx;

		for (int r = 0; r < 3; r++) {
			int cy = cell_y(r);
			char p = board[r * 3 + c];

			if (p) {
				for (int i = 0; i < 8; i++) {
					int ly = b * 8 + i - cy;

					if (ly >= 0 && ly < CELL &&
					    glyph_pixel(p, lx, ly)) {
						out &= ~(0x80 >> i);
					}
				}
			}
			if (cursor_visible && cur == r * 3 + c) {
				/* 2px frame just inside the cell */
				if (lx < 2 || lx >= CELL - 2) {
					out = span_black(out, b, cy,
							 cy + CELL - 1);
				} else {
					out = span_black(out, b, cy, cy + 1);
					out = span_black(out, b, cy + CELL - 2,
							 cy + CELL - 1);
				}
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

/* Full scene into both planes + full refresh, then back to partial mode.
 * The base is emitted WITHOUT the cursor; the cursor is then drawn as the
 * first partial (0x24 -> kick -> sync 0x26), same flow as cursor_partial().
 * Writing both planes here keeps 0x24 and 0x26 identical at the moment we
 * enter partial mode, so the first partial diff is exactly "the cursor". */
static void rebase(void)
{
	int saved = cursor_visible;

	cursor_visible = 0;
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

	cursor_visible = saved;
	if (cursor_visible) {
		int cx = cell_x(cur % 3);

		emit_cols(cx, cx + CELL - 1, 0x24);   /* cursor as 1st partial */
		epd_partial_kick();
		emit_cols(cx, cx + CELL - 1, 0x26);
		epd_set_full_window();
	}
}

/* partial-update the cursor moving from cell a to cell b */
static void cursor_partial(int a, int bcell)
{
	int ca = a % 3, cb = bcell % 3;
	int x0 = cell_x(ca < cb ? ca : cb);
	int x1 = cell_x(ca > cb ? ca : cb) + CELL - 1;

	emit_cols(x0, x1, 0x24);
	epd_partial_kick();
	/* sync the reference plane (safe with ping-pong OFF; this exact combo
	 * is the canonical GxEPD2 partial flow). Next move's erase then runs
	 * the strong black-to-white waveform instead of the weak white-to-white
	 * touch-up that only fades the old frame slowly. */
	emit_cols(x0, x1, 0x26);
	epd_set_full_window();
}

/* ---------------- game logic ---------------- */

static int line_win(char p, int a, int b, int c)
{
	return board[a] == p && board[b] == p && board[c] == p;
}

static int has_won(char p)
{
	static const int L[8][3] = {
		{0, 1, 2}, {3, 4, 5}, {6, 7, 8},
		{0, 3, 6}, {1, 4, 7}, {2, 5, 8},
		{0, 4, 8}, {2, 4, 6},
	};

	for (int i = 0; i < 8; i++) {
		if (line_win(p, L[i][0], L[i][1], L[i][2])) {
			return 1;
		}
	}
	return 0;
}

static int board_full(void)
{
	for (int i = 0; i < 9; i++) {
		if (!board[i]) {
			return 0;
		}
	}
	return 1;
}

/* winning/blocking cell for p, or -1 */
static int find_line_move(char p)
{
	for (int i = 0; i < 9; i++) {
		if (board[i]) {
			continue;
		}
		board[i] = p;
		int w = has_won(p);

		board[i] = 0;
		if (w) {
			return i;
		}
	}
	return -1;
}

static int computer_move(void)
{
	static const int pref[] = {4, 0, 2, 6, 8, 1, 3, 5, 7};
	int m = find_line_move('X');          /* win if possible */

	if (m < 0) {
		m = find_line_move('O');      /* otherwise block */
	}
	if (m < 0) {
		for (unsigned int i = 0; i < 9; i++) {
			if (!board[pref[i]]) {
				m = pref[i];
				break;
			}
		}
	}
	board[m] = 'X';
	return m;
}

static void new_game(void)
{
	for (int i = 0; i < 9; i++) {
		board[i] = 0;
	}
	cur = 4;
	cursor_visible = 1;
	rebase();
	uputs("your move: wasd + Enter\r\n");
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
}

int main(void)
{
	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	uputs("\r\npixpaper tic-tac-toe " APP_VERSION
	      "\r\nwasd = move cursor, Enter = place O\r\n"
	      "preparing panel...\r\n");

	epd_hw_reset();
	sleep_ms(1000);
	new_game();

	for (;;) {
		unsigned char c;

		if (uart_poll_in(uart, &c) != 0) {
			k_busy_wait(2000);
			continue;
		}

		int old = cur, r = cur / 3, col = cur % 3;

		if (c == 'w' || c == 'W') {
			r = r > 0 ? r - 1 : r;
		} else if (c == 's' || c == 'S') {
			r = r < 2 ? r + 1 : r;
		} else if (c == 'a' || c == 'A') {
			col = col > 0 ? col - 1 : col;
		} else if (c == 'd' || c == 'D') {
			col = col < 2 ? col + 1 : col;
		} else if (c == '\r' || c == '\n') {
			if (board[cur]) {
				uputs("cell taken\r\n");
				continue;
			}
			board[cur] = 'O';

			const char *msg = NULL;

			if (has_won('O')) {
				msg = "YOU WIN!";
			} else if (board_full()) {
				msg = "draw.";
			} else {
				computer_move();
				if (has_won('X')) {
					msg = "computer wins.";
				} else if (board_full()) {
					msg = "draw.";
				}
			}

			if (msg) {
				cursor_visible = 0;
			}
			rebase();             /* move-confirm full refresh */

			if (msg) {
				uputs(msg);
				uputs("  press any key for a new game\r\n");
				wait_fresh_key();
				new_game();
			} else {
				uputs("your move\r\n");
			}
			continue;
		} else {
			continue;
		}

		cur = r * 3 + col;
		if (cur != old) {
			cursor_partial(old, cur);
		}
	}

	return 0;
}
