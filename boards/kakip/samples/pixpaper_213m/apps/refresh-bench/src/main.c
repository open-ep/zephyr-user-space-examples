/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pixpaper-213m refresh benchmark on the Kakip (RZ/V2H) Cortex-M33.
 *
 * Shows a moving bar in partial-refresh mode and measures, per frame, how long
 * the SPI transfer takes and how long the panel is busy, for several waveform
 * lengths (TP). Watch the panel while the numbers print: shorter TP = faster but
 * weaker drive. The last TP that still gives clean black and white is the speed
 * limit for this panel. The panel recipe is the one the W6300 video app uses:
 * full frame to 0x24 on every kick, 0x37 ping-pong on, 0x26 never written.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define EPD_NODE DT_NODELABEL(epd)

static const struct spi_dt_spec epd_spi =
	SPI_DT_SPEC_GET(EPD_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec dc_gpio = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec rst_gpio = GPIO_DT_SPEC_GET(EPD_NODE, reset_gpios);
static const struct gpio_dt_spec busy_gpio = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

#define EPD_GATES     250
#define EPD_SRC_BYTES 16
#define EPD_FB_SIZE   (EPD_GATES * EPD_SRC_BYTES)

/* waveform lengths to try, longest first; FR = frame rate code (0..7) */
static const uint8_t TP_LIST[] = { 0x10, 0x0C, 0x08, 0x06, 0x05, 0x04, 0x03 };
#define PARTIAL_FR     3
#define FRAMES_PER_TP  24
#define BAR_GATES      20

static uint8_t framebuf[EPD_FB_SIZE];

/* ---------------- panel low level (RSPI0 + GPIO) ------------------------ */

static void spi_tx(const uint8_t *data, size_t len)
{
	struct spi_buf buf = { .buf = (void *)data, .len = len };
	struct spi_buf_set set = { .buffers = &buf, .count = 1 };

	spi_write_dt(&epd_spi, &set);
}

static void epd_cmd(uint8_t c)
{
	gpio_pin_set_dt(&dc_gpio, 0);
	spi_tx(&c, 1);
}

static void epd_data(uint8_t d)
{
	gpio_pin_set_dt(&dc_gpio, 1);
	spi_tx(&d, 1);
}

static void epd_data_bulk(const uint8_t *p, size_t len)
{
	gpio_pin_set_dt(&dc_gpio, 1);
	for (size_t i = 0; i < len; i += EPD_SRC_BYTES) {
		spi_tx(p + i, EPD_SRC_BYTES);
	}
}

static void epd_wait_idle(void)
{
	k_msleep(2);
	for (int i = 0; i < 5000; i++) {
		if (gpio_pin_get_dt(&busy_gpio) == 0) {
			return;
		}
		k_msleep(1);
	}
}

/* BUSY rises a few ms after 0x20: wait for the rise, then the fall.
 * Returns the time the panel was busy, in microseconds.
 */
static int64_t epd_wait_refresh(uint32_t blind_ms)
{
	int64_t t0 = k_uptime_ticks();
	int rose = 0;

	for (int i = 0; i < 3000; i++) {
		if (gpio_pin_get_dt(&busy_gpio) == 1) {
			rose = 1;
			break;
		}
		k_usleep(100);
	}
	if (!rose) {
		k_msleep(blind_ms);
		return k_ticks_to_us_floor64(k_uptime_ticks() - t0);
	}
	while (gpio_pin_get_dt(&busy_gpio) == 1) {
		k_usleep(100);
	}
	return k_ticks_to_us_floor64(k_uptime_ticks() - t0);
}

static void epd_hw_reset(void)
{
	k_msleep(20);
	gpio_pin_set_dt(&rst_gpio, 0);
	k_msleep(20);
	gpio_pin_set_dt(&rst_gpio, 1);
	k_msleep(20);
}

static void epd_set_full_window(void)
{
	epd_cmd(0x44); epd_data(0x00); epd_data(EPD_SRC_BYTES - 1);
	epd_cmd(0x45); epd_data(EPD_GATES - 1); epd_data(0x00); epd_data(0x00); epd_data(0x00);
}

static void epd_set_full_cursor(void)
{
	epd_cmd(0x4E); epd_data(0x00);
	epd_cmd(0x4F); epd_data(EPD_GATES - 1); epd_data(0x00);
}

static void epd_reg_init(void)
{
	epd_wait_idle();
	epd_cmd(0x12);
	epd_wait_idle();
	epd_cmd(0x01); epd_data(EPD_GATES - 1); epd_data(0x00); epd_data(0x00);
	epd_cmd(0x11); epd_data(0x01);
	epd_set_full_window();
	epd_cmd(0x3C); epd_data(0x05);
	epd_cmd(0x21); epd_data(0x00); epd_data(0x80);
	epd_cmd(0x18); epd_data(0x80);
	epd_set_full_cursor();
	epd_wait_idle();
}

/* ---------------- partial mode ------------------------------------------ */

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

static void epd_load_partial_lut(uint8_t tp)
{
	uint8_t fr_byte = ((PARTIAL_FR & 7) << 4) | (PARTIAL_FR & 7);

	epd_cmd(0x32);
	for (int i = 0; i < 153; i++) {
		uint8_t b = WF_PARTIAL[i];

		if (i == 60) {
			b = tp;
		} else if (i >= 144 && i <= 149) {
			b = fr_byte;
		}
		epd_data(b);
	}
	epd_wait_idle();

	epd_cmd(0x3F); epd_data(WF_PARTIAL[153]);
	epd_cmd(0x03); epd_data(WF_PARTIAL[154]);
	epd_cmd(0x04); epd_data(WF_PARTIAL[155]); epd_data(WF_PARTIAL[156]); epd_data(WF_PARTIAL[157]);
	epd_cmd(0x2C); epd_data(WF_PARTIAL[158]);

	/* ping-pong on (byte 5 = 0x40): we write the full frame every kick and the
	 * controller's bank swap does the old/new diff. Never write 0x26 after this.
	 */
	epd_cmd(0x37);
	for (int i = 0; i < 10; i++) {
		epd_data(i == 5 ? 0x40 : 0x00);
	}
	epd_cmd(0x3C); epd_data(0x80);
}

static void epd_partial_begin(uint8_t tp)
{
	gpio_pin_set_dt(&rst_gpio, 0);
	k_msleep(2);
	gpio_pin_set_dt(&rst_gpio, 1);
	k_msleep(2);

	epd_cmd(0x01); epd_data(EPD_GATES - 1); epd_data(0x00); epd_data(0x00);
	epd_cmd(0x11); epd_data(0x01);
	epd_cmd(0x21); epd_data(0x00); epd_data(0x80);
	epd_cmd(0x18); epd_data(0x80);
	epd_load_partial_lut(tp);
	epd_cmd(0x22); epd_data(0xC0);
	epd_cmd(0x20);
	epd_wait_idle();
	epd_set_full_window();
}

/* first frame into both banks + one OTP full refresh: a clean base */
static void epd_set_base(const uint8_t *frame)
{
	epd_hw_reset();
	epd_reg_init();
	for (int plane = 0; plane < 2; plane++) {
		epd_set_full_window();
		epd_set_full_cursor();
		epd_cmd(plane ? 0x26 : 0x24);
		epd_data_bulk(frame, EPD_FB_SIZE);
	}
	epd_cmd(0x22); epd_data(0xF7);
	epd_cmd(0x20);
	epd_wait_refresh(2500);
}

/* one frame: full frame to 0x24 only, then a partial kick */
static void epd_partial_frame(const uint8_t *frame, int64_t *spi_us, int64_t *busy_us)
{
	int64_t t0 = k_uptime_ticks();

	epd_set_full_cursor();
	epd_cmd(0x24);
	epd_data_bulk(frame, EPD_FB_SIZE);
	*spi_us = k_ticks_to_us_floor64(k_uptime_ticks() - t0);

	epd_cmd(0x22); epd_data(0x0C);
	epd_cmd(0x20);
	*busy_us = epd_wait_refresh(400);
}

/* ---------------- test pattern: a black bar sweeping along the gates ---- */

static void fb_bar(int pos)
{
	memset(framebuf, 0xFF, sizeof(framebuf));           /* 1 = white */
	for (int g = pos; g < pos + BAR_GATES && g < EPD_GATES; g++) {
		memset(&framebuf[g * EPD_SRC_BYTES], 0x00, EPD_SRC_BYTES);
	}
}

int main(void)
{
	if (!spi_is_ready_dt(&epd_spi) || !gpio_is_ready_dt(&dc_gpio) ||
	    !gpio_is_ready_dt(&rst_gpio) || !gpio_is_ready_dt(&busy_gpio)) {
		printk("panel not ready\n");
		return -ENODEV;
	}
	gpio_pin_configure_dt(&dc_gpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst_gpio, GPIO_OUTPUT_ACTIVE);
	gpio_pin_configure_dt(&busy_gpio, GPIO_INPUT);

	printk("pixpaper-213m refresh benchmark: %d frames per TP, SPI %u Hz\n",
	       FRAMES_PER_TP, epd_spi.config.frequency);
	printk("  TP   spi_ms  busy_ms  frame_ms   fps\n");

	while (1) {
		for (size_t t = 0; t < ARRAY_SIZE(TP_LIST); t++) {
			uint8_t tp = TP_LIST[t];
			int64_t spi_sum = 0, busy_sum = 0, total_us;
			int64_t t0;

			fb_bar(0);
			epd_set_base(framebuf);
			epd_partial_begin(tp);

			t0 = k_uptime_ticks();
			for (int i = 1; i <= FRAMES_PER_TP; i++) {
				int64_t spi_us, busy_us;

				fb_bar((i * (EPD_GATES / 12)) % EPD_GATES);
				epd_partial_frame(framebuf, &spi_us, &busy_us);
				spi_sum += spi_us;
				busy_sum += busy_us;
			}
			total_us = k_ticks_to_us_floor64(k_uptime_ticks() - t0);

			printk("  0x%02X  %5lld.%01lld  %6lld.%01lld  %7lld.%01lld  %3lld.%01lld\n", tp,
			       spi_sum / FRAMES_PER_TP / 1000, (spi_sum / FRAMES_PER_TP / 100) % 10,
			       busy_sum / FRAMES_PER_TP / 1000, (busy_sum / FRAMES_PER_TP / 100) % 10,
			       total_us / FRAMES_PER_TP / 1000, (total_us / FRAMES_PER_TP / 100) % 10,
			       (10000000LL * FRAMES_PER_TP / total_us) / 10,
			       (10000000LL * FRAMES_PER_TP / total_us) % 10);
			k_msleep(1500);
		}
		printk("--- sweep done, repeating ---\n");
	}
	return 0;
}
