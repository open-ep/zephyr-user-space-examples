/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * PixPaper 2.13" mono (250x122) on UIAPduino Pro Micro CH32V003 - Zephyr port
 * with an interactive UART console.
 *
 * Console: 115200 8N1 on USART1, TX=PD5 / RX=PD6 (external 3.3V USB-UART
 * dongle required - the board's USB-C is bootloader-only). Zephyr's shell
 * subsystem is far too big for 16KB flash, so this is a tiny hand-rolled
 * command loop over the polled UART API. Commands:
 *
 *   help   - list commands
 *   draw   - draw the image stored in flash
 *   inv    - draw the stored image inverted
 *   white  - clear panel to white
 *   black  - fill panel black
 *   load   - receive a 4000-byte packed frame over UART and display it
 *            (16 chunks x 250 bytes, each chunk ACKed with '.';
 *             see tools/epd_load.py) - swap pictures without reflashing!
 *   sleep  - put the panel into deep sleep
 *
 * The panel is re-initialized for every draw (it is deep-slept after each),
 * so every command is self-contained. CH32V003 has 2KB SRAM: frames are
 * always streamed byte-by-byte, never buffered.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include "img_packed.h"

#define APP_VERSION "R1.0.0"

#define ZUSER DT_PATH(zephyr_user)

static const struct gpio_dt_spec sck  = GPIO_DT_SPEC_GET(ZUSER, sck_gpios);
static const struct gpio_dt_spec mosi = GPIO_DT_SPEC_GET(ZUSER, mosi_gpios);
static const struct gpio_dt_spec cs   = GPIO_DT_SPEC_GET(ZUSER, cs_gpios);
static const struct gpio_dt_spec dc   = GPIO_DT_SPEC_GET(ZUSER, dc_gpios);
static const struct gpio_dt_spec rst  = GPIO_DT_SPEC_GET(ZUSER, rst_gpios);
static const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(ZUSER, busy_gpios);

static const struct device *uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

#define EPD_BYTES (250 * 16)
#define LOAD_CHUNK 250

/* ---------------- tiny UART I/O ---------------- */

static void uputs(const char *s)
{
	while (*s) {
		uart_poll_out(uart, *s++);
	}
}

/* poll one byte with timeout; returns -1 on timeout */
static int ugetc_timeout(uint32_t ms)
{
	unsigned char c;

	for (uint32_t i = 0; i < ms * 100; i++) {
		if (uart_poll_in(uart, &c) == 0) {
			return c;
		}
		k_busy_wait(10);
	}
	return -1;
}

/* ---------------- panel driver (same logic as the Arduino port) --------- */

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

static void epd_init(void)
{
	sleep_ms(50);
	gpio_pin_set_dt(&rst, 0);
	sleep_ms(50);
	gpio_pin_set_dt(&rst, 1);
	sleep_ms(50);
	sleep_ms(1000);           /* post-reset settle, as in the proven port */
	epd_wait_idle();

	epd_command(0x12);        /* soft reset */
	epd_wait_idle();

	epd_command(0x01);
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x11);
	epd_data(0x01);

	epd_command(0x44);
	epd_data(0x00);
	epd_data(0x0F);

	epd_command(0x45);
	epd_data(0xF9);
	epd_data(0x00);
	epd_data(0x00);
	epd_data(0x00);

	epd_command(0x3C);
	epd_data(0x05);

	epd_command(0x21);
	epd_data(0x00);
	epd_data(0x80);

	epd_command(0x18);
	epd_data(0x80);

	epd_command(0x4E);
	epd_data(0x00);

	epd_command(0x4F);
	epd_data(0xF9);
	epd_data(0x00);

	epd_wait_idle();
}

/* init panel and open the B/W RAM for streaming */
static void epd_frame_begin(void)
{
	epd_init();
	epd_command(0x24);
	sleep_ms(10);
}

/* Wait out the ~2s refresh. BUSY should rise once 0x20 starts; if it never
 * does (wire loose / stuck level), blind-wait instead - the deep sleep that
 * follows must never cut a running refresh short (that shows as "OK but the
 * panel never changes"). */
static void epd_refresh_wait(void)
{
	int rose = 0;

	for (int i = 0; i < 500; i++) {
		if (gpio_pin_get_dt(&busy) == 1) {
			rose = 1;
			break;
		}
		k_busy_wait(1000);
	}
	if (!rose) {
		sleep_ms(3000);
		return;
	}
	for (int i = 0; i < 10000; i++) {
		if (gpio_pin_get_dt(&busy) == 0) {
			return;
		}
		k_busy_wait(1000);
	}
}

/* refresh and put the panel back to deep sleep */
static void epd_frame_end(void)
{
	epd_command(0x22);
	sleep_ms(10);
	epd_data(0xF7);
	epd_command(0x20);
	epd_refresh_wait();

	epd_command(0x10);
	epd_data(0x01);
}

static void epd_show_flash(int invert)
{
	epd_frame_begin();
	for (int i = 0; i < EPD_BYTES; i++) {
		epd_data(invert ? (uint8_t)~img_packed[i] : img_packed[i]);
	}
	epd_frame_end();
}

static void epd_show_fill(uint8_t v)
{
	epd_frame_begin();
	for (int i = 0; i < EPD_BYTES; i++) {
		epd_data(v);
	}
	epd_frame_end();
}

/* receive EPD_BYTES over UART in ACKed chunks and stream them to the panel */
static void epd_load_uart(void)
{
	epd_frame_begin();
	uputs("READY\r\n");

	for (int i = 0; i < EPD_BYTES; i++) {
		/* first byte: allow 60s so a human can navigate a terminal's
		 * send-file dialog; after that the stream must keep flowing */
		int c = ugetc_timeout(i == 0 ? 60000 : 3000);

		if (c < 0) {
			epd_frame_end();
			uputs("TIMEOUT\r\n");
			return;
		}
		epd_data((uint8_t)c);
		if ((i + 1) % LOAD_CHUNK == 0) {
			uart_poll_out(uart, '.');   /* chunk ACK */
		}
	}

	epd_frame_end();
	uputs("OK\r\n");
}

/* ---------------- command console ---------------- */

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void run_cmd(const char *line)
{
	if (streq(line, "help")) {
		uputs("draw|inv|white|black|load|sleep|busy|ver|help\r\n");
	} else if (streq(line, "ver")) {
		uputs("pixpaper_213_mono " APP_VERSION "\r\n");
	} else if (streq(line, "draw")) {
		epd_show_flash(0);
		uputs("OK\r\n");
	} else if (streq(line, "inv")) {
		epd_show_flash(1);
		uputs("OK\r\n");
	} else if (streq(line, "white")) {
		epd_show_fill(0xFF);
		uputs("OK\r\n");
	} else if (streq(line, "black")) {
		epd_show_fill(0x00);
		uputs("OK\r\n");
	} else if (streq(line, "load")) {
		epd_load_uart();
	} else if (streq(line, "sleep")) {
		epd_command(0x10);
		epd_data(0x01);
		uputs("OK\r\n");
	} else if (streq(line, "busy")) {
		/* wire diagnosis: panel idle = 0, refreshing or deep sleep = 1 */
		uputs(gpio_pin_get_dt(&busy) ? "BUSY=1\r\n" : "BUSY=0\r\n");
	} else if (line[0] != '\0') {
		uputs("? (help)\r\n");
	}
}

int main(void)
{
	char line[16];
	int len = 0;

	gpio_pin_configure_dt(&sck, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&mosi, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&cs, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&dc, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy, GPIO_INPUT);

	/* boot: draw the stored image once, narrating each phase over UART so
	 * a stuck step is visible from the terminal */
	uputs("\r\npixpaper_213_mono " APP_VERSION "\r\n");
	uputs("boot: reset+init panel...\r\n");
	epd_frame_begin();
	uputs("boot: streaming image...\r\n");
	for (int i = 0; i < EPD_BYTES; i++) {
		epd_data(img_packed[i]);
	}
	uputs("boot: refreshing (takes ~2s)...\r\n");
	epd_frame_end();
	uputs("boot: done, console ready\r\n");

	uputs("pixpaper> ");

	for (;;) {
		int c = ugetc_timeout(1000);

		if (c < 0) {
			continue;
		}
		if (c == '\r' || c == '\n') {
			uputs("\r\n");
			line[len] = '\0';
			run_cmd(line);
			len = 0;
			uputs("pixpaper> ");
		} else if (c == 0x7F || c == 0x08) {   /* backspace */
			if (len > 0) {
				len--;
				uputs("\b \b");
			}
		} else if (len < (int)sizeof(line) - 1 && c >= ' ') {
			line[len++] = (char)c;
			uart_poll_out(uart, (unsigned char)c);   /* echo */
		}
	}

	return 0;
}
