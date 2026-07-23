/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * Paddle-ball (pong-style) game on a PixPaper 2.13" mono e-paper (250x122,
 * landscape), UIAPduino Pro Micro CH32V003, Zephyr. Controlled over the UART
 * console (115200 8N1, TX=PD5/RX=PD6):
 *
 *   w / s = left paddle up / down (player)
 *   i / k = right paddle up / down (optional - it auto-tracks the ball)
 *   any key restarts after game over
 *
 * Rendering: partial-update mode (LUT from the Linux pet example). There is
 * no framebuffer (2KB SRAM): every element - paddles, dashed centre line,
 * ball - is generated procedurally per gate column, and only the columns
 * that changed each tick are rewritten. 0x37 ping-pong is OFF and 0x26 is
 * manually synced after every kick (the ttt-proven GxEPD2-style flow) so
 * erases always run the strong waveform.
 *
 * Hardware notes baked in: RAM windows always span the full 16-byte stride;
 * the window is opened to full screen before every 0x20 kick; a full-refresh
 * rebase runs every GHOST_EVERY frames to clear ghosting.
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
#define COURT_H       122            /* visible rows */

/* ---------------- gameplay tuning ---------------- */
/* Inverse court: black background, white ball/paddles. Erase residue on this
 * panel physically means "black particles left where white should be" - on a
 * black background the equivalent leftover is dark gray on black, which the
 * eye cannot see. Set to 0 for the classic white court. */
#define INVERT_COURT 1

#define BALL_SZ     8
#define PADDLE_W    4
#define PADDLE_H    28
#define PADDLE_LX   6                 /* left paddle columns 6..9 */
#define PADDLE_RX   (DISP_W - 6 - PADDLE_W)
#define PADDLE_STEP 10                /* per keypress */
#define AI_STEP     6                 /* right paddle tracking speed */
#define BALL_DX     8                 /* horizontal speed, px per frame */
#define TICK_MS     20                /* pause on top of each refresh */
#define GHOST_EVERY 150               /* full-refresh rebase period */

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

static int lpad_y, rpad_y;            /* paddle tops */
static int ball_x, ball_y;            /* ball top-left */
static int ball_dx, ball_dy;
static uint16_t rally;

/* clear (blacken) the bits of byte row b that fall inside rows [y0, y1] */
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

/* one byte of the scene at gate column x, byte row b */
static uint8_t scene_byte(int x, int b)
{
	uint8_t out = 0xFF;

	if (b * 8 >= COURT_H) {
		return INVERT_COURT ? (uint8_t)~out : out;   /* below visible */
	}
	if (x == DISP_W / 2 && (b & 1) == 0) {
		out = 0x00;                       /* dashed centre line */
	}
	if (x >= PADDLE_LX && x < PADDLE_LX + PADDLE_W) {
		out = span_black(out, b, lpad_y, lpad_y + PADDLE_H - 1);
	}
	if (x >= PADDLE_RX && x < PADDLE_RX + PADDLE_W) {
		out = span_black(out, b, rpad_y, rpad_y + PADDLE_H - 1);
	}
	if (x >= ball_x && x < ball_x + BALL_SZ) {
		out = span_black(out, b, ball_y, ball_y + BALL_SZ - 1);
	}

	/* uniform inversion: only the colors swap, mechanisms unchanged */
	return INVERT_COURT ? (uint8_t)~out : out;
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
}

/* ---------------- game ---------------- */

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void game_reset(void)
{
	lpad_y = rpad_y = (COURT_H - PADDLE_H) / 2;
	ball_x = (DISP_W - BALL_SZ) / 2;
	ball_y = (COURT_H - BALL_SZ) / 2;
	ball_dx = BALL_DX;
	ball_dy = 4;
	rally = 0;
}

static void poll_input(void)
{
	unsigned char c;

	while (uart_poll_in(uart, &c) == 0) {
		if (c == 'w' || c == 'W') {
			lpad_y -= PADDLE_STEP;
		} else if (c == 's' || c == 'S') {
			lpad_y += PADDLE_STEP;
		} else if (c == 'i' || c == 'I') {
			rpad_y -= PADDLE_STEP;
		} else if (c == 'k' || c == 'K') {
			rpad_y += PADDLE_STEP;
		}
	}
	lpad_y = clampi(lpad_y, 0, COURT_H - PADDLE_H);
	rpad_y = clampi(rpad_y, 0, COURT_H - PADDLE_H);
}

/* right paddle drifts toward the ball, capped at AI_STEP px per frame */
static void ai_step(void)
{
	int target = ball_y + BALL_SZ / 2 - PADDLE_H / 2;
	int d = clampi(target - rpad_y, -AI_STEP, AI_STEP);

	rpad_y = clampi(rpad_y + d, 0, COURT_H - PADDLE_H);
}

/* deflection: hitting near a paddle edge steepens the bounce */
static int deflect(int pad_y)
{
	int off = (ball_y + BALL_SZ / 2) - (pad_y + PADDLE_H / 2);

	return clampi(off / 3, -7, 7);
}

/* returns 0 on game over */
static int ball_step(void)
{
	ball_x += ball_dx;
	ball_y += ball_dy;

	if (ball_y < 0) {                     /* top/bottom walls */
		ball_y = 0;
		ball_dy = -ball_dy;
	} else if (ball_y + BALL_SZ > COURT_H) {
		ball_y = COURT_H - BALL_SZ;
		ball_dy = -ball_dy;
	}

	if (ball_dx < 0 && ball_x <= PADDLE_LX + PADDLE_W) {
		if (ball_y + BALL_SZ > lpad_y && ball_y < lpad_y + PADDLE_H) {
			ball_x = PADDLE_LX + PADDLE_W;
			ball_dx = -ball_dx;
			ball_dy = deflect(lpad_y);
			rally++;
			uputs("rally: ");
			uput_u16(rally);
			uputs("\r\n");
		} else if (ball_x + BALL_SZ < PADDLE_LX) {
			return 0;                    /* missed on the left */
		}
	}
	if (ball_dx > 0 && ball_x + BALL_SZ >= PADDLE_RX) {
		if (ball_y + BALL_SZ > rpad_y && ball_y < rpad_y + PADDLE_H) {
			ball_x = PADDLE_RX - BALL_SZ;
			ball_dx = -ball_dx;
			ball_dy = deflect(rpad_y);
		} else if (ball_x > PADDLE_RX + PADDLE_W) {
			return 0;                    /* missed on the right */
		}
	}
	return 1;
}

int main(void)
{
	unsigned int partials = 0;

	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	uputs("\r\npixpaper pong " APP_VERSION
	      "\r\ncontrols: w/s left paddle, i/k right paddle (auto-tracks)"
	      "\r\npreparing panel...\r\n");

	game_reset();
	epd_hw_reset();
	sleep_ms(1000);
	rebase();
	uputs("go!\r\n");

	for (;;) {
		sleep_ms(TICK_MS);
		poll_input();
		ai_step();

		int obx = ball_x;
		int alive = ball_step();

		/* redraw range: the ball sweep */
		int rx0 = obx < ball_x ? obx : ball_x;
		int rx1 = (obx > ball_x ? obx : ball_x) + BALL_SZ - 1;

		emit_cols(rx0, rx1, 0x24);
		emit_cols(PADDLE_LX, PADDLE_LX + PADDLE_W - 1, 0x24);
		emit_cols(PADDLE_RX, PADDLE_RX + PADDLE_W - 1, 0x24);
		epd_partial_kick();

		/* reference plane = exact copy of this frame (0x37 ping-pong is
		 * not effective on this panel), so next frame's every change
		 * gets the full-strength waveform */
		emit_cols(rx0, rx1, 0x26);
		emit_cols(PADDLE_LX, PADDLE_LX + PADDLE_W - 1, 0x26);
		emit_cols(PADDLE_RX, PADDLE_RX + PADDLE_W - 1, 0x26);
		epd_set_full_window();

		if (!alive) {
			uputs("GAME OVER - rally: ");
			uput_u16(rally);
			uputs("\r\npress any key to restart\r\n");

			unsigned char c;

			for (;;) {               /* drain held keys, wait quiet */
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
			game_reset();
			rebase();
			partials = 0;
			uputs("go!\r\n");
			continue;
		}

		if (++partials >= GHOST_EVERY) {
			rebase();
			partials = 0;
		}
	}

	return 0;
}
