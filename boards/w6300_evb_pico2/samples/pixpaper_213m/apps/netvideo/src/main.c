/*
 * SPDX-License-Identifier: Apache-2.0
 * Author: Wig Cheng <onlywig@gmail.com>
 *
 * Network frame player for PIXPAPER-213-M on W6300-EVB-Pico2 (RP2350), Zephyr.
 * The PC does all the work (decode, scale, threshold, pack) and streams
 * ready-to-display 4000-byte frames over TCP; the board only pushes them to
 * the panel. See tools/epdstream.py for the sender.
 *
 * Protocol (little endian), port 5001:
 *   hello   "EPDS" + u8 version(1) + u8 mode(0=mono) + u16 reserved
 *   then    u32 cmd, repeatedly:
 *             4000 -> a 4000-byte packed frame follows (live mode)
 *                1 -> full refresh (repaint the last frame, clears ghosting)
 *                2 -> clear to white
 *                3 -> clip upload: u32 frame count, u32 us-per-frame, then
 *                     that many frames. The board acks every frame with one byte
 *                     and loops the clip on its own once the upload finishes,
 *                     so the sender can disconnect.
 *
 * Two modes, one socket. Live mode: newest frame wins - the receiver keeps
 * draining while the panel is busy so a live source stays current instead of
 * drifting behind. Clip mode: the frames land in RAM (96 x 4000 B) and play
 * back from there at full speed, which is the only way to beat the ~20 KB/s
 * ceiling of the W6300's bit-banged SPI.
 *
 * The per-frame ack is application-level flow control: it keeps in-flight data
 * to one frame, because a raw 384 KB burst overruns this small net stack.
 *
 * Discipline: full frame to 0x24 every kick, 0x37 ping-pong ON, 0x26 untouched
 * (same as the offline video player).
 *
 * Panel wiring comes from ../../common/w6300_evb_pico2.overlay; the ethernet
 * side (fixed MAC, unthrottled SPI) from this app's own ethernet.overlay.
 * The W6300 itself sits on GP15-GP22 in the upstream board devicetree.
 *
 * NOTE: needs the two eth_w6300 fixes in ../../../../patches/zephyr/ until
 * they land upstream - see this app's README.
 */


#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

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


/* ---------------- boot screen: show the IP on the panel ----------------- */

struct glyph {
	char ch;
	uint8_t col[5];                     /* bit0 = top row */
};

static const struct glyph FONT[] = {
	{'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
	{'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
	{'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
	{'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
	{'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}}, {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
	{'P', {0x7F, 0x09, 0x09, 0x09, 0x06}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
	{'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
};

static const uint8_t *glyph_cols(char c)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(FONT); i++) {
		if (FONT[i].ch == c) {
			return FONT[i].col;
		}
	}
	return NULL;                        /* space and unknown: blank */
}

#define DISP_H 122

static uint8_t g_bits[DISP_H][DISP_W];  /* 1 = white; RP2350 has RAM to spare */

static void draw_text(const char *s, int y0, int scale)
{
	int len = strlen(s);
	int x0 = (DISP_W - len * 6 * scale) / 2;

	for (int i = 0; i < len; i++) {
		const uint8_t *g = glyph_cols(s[i]);

		if (!g) {
			continue;
		}
		for (int cx = 0; cx < 5; cx++) {
			for (int cy = 0; cy < 7; cy++) {
				if (!((g[cx] >> cy) & 1)) {
					continue;
				}
				for (int dx = 0; dx < scale; dx++) {
					for (int dy = 0; dy < scale; dy++) {
						int x = x0 + (i * 6 + cx) * scale + dx;
						int y = y0 + cy * scale + dy;

						if (x >= 0 && x < DISP_W && y >= 0 && y < DISP_H) {
							g_bits[y][x] = 0;
						}
					}
				}
			}
		}
	}
}

/* pack g_bits into panel RAM order (same layout the PC sender uses) */
static void pack_bits(uint8_t *frame)
{
	for (int x = 0; x < DISP_W; x++) {
		for (int b = 0; b < DISP_STRIDE; b++) {
			uint8_t out = 0;

			for (int k = 0; k < 8; k++) {
				int y = b * 8 + k;

				if (y >= DISP_H || g_bits[y][x]) {
					out |= 0x80 >> k;
				}
			}
			frame[x * DISP_STRIDE + b] = out;
		}
	}
}

/* ---------------- network frame server ---------------- */

#include <string.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>

#define FRAME_BYTES  DISP_BUF_SIZE          /* 4000 */
#define LISTEN_PORT  5001
#define CMD_FRAME    FRAME_BYTES
#define CMD_REFRESH  1u
#define CMD_CLEAR    2u
#define CMD_CLIP     3u                     /* upload-then-loop playback */

#define CLIP_MAX_FRAMES 96                  /* 96 x 4000 B = 384 KB of RAM */
static uint8_t g_clip[CLIP_MAX_FRAMES][FRAME_BYTES];
static uint32_t g_clip_frames;
static uint32_t g_clip_frame_us = 200000;
static bool g_clip_play;
static uint32_t g_clip_gen;
#define STATIC_IP    "192.168.7.2"          /* fallback for a direct cable */
#define STATIC_MASK  "255.255.255.0"
#define DHCP_WAIT_MS 15000

static uint8_t g_shared[FRAME_BYTES];
static bool g_have;
static bool g_force_base = true;
static uint32_t g_rx, g_shown, g_dropped;
static char g_ip[NET_IPV4_ADDR_LEN] = "NO IP";
static int64_t g_disp_ms;

K_MUTEX_DEFINE(g_lock);
K_SEM_DEFINE(g_sem, 0, 1);                  /* limit 1 = newest frame wins */

static void display_thread(void *a, void *b, void *c)
{
	static uint8_t show[FRAME_BYTES];

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	uint32_t idx = 0, gen = 0;
	int64_t next_ms = 0;

	while (1) {
		/* bounded wait: a clip upload finishing must not depend on a
		 * semaphore give to wake us (that was a lost-wakeup bug) */
		bool live = (k_sem_take(&g_sem, K_MSEC(g_clip_play ? 10 : 100)) == 0);
		const uint8_t *frame;

		if (live) {
			k_mutex_lock(&g_lock, K_FOREVER);
			memcpy(show, g_shared, FRAME_BYTES);
			g_have = false;
			k_mutex_unlock(&g_lock);
			frame = show;
		} else if (g_clip_play && g_clip_frames > 0) {
			/* clip playback tick */
			if (gen != g_clip_gen) {
				gen = g_clip_gen;      /* new clip: start at 0 */
				idx = 0;
				next_ms = 0;
			}
			if (k_uptime_get() < next_ms) {
				continue;
			}
			if (idx >= g_clip_frames) {
				idx = 0;
				g_force_base = true;   /* per-loop ghost payoff */
			}
			frame = g_clip[idx++];
			next_ms = k_uptime_get() + g_clip_frame_us / 1000;
		} else {
			continue;                      /* idle, nothing to show */
		}

		int64_t t0 = k_uptime_get();

		if (g_force_base) {
			epd_set_base(frame);        /* both planes + OTP refresh */
			g_force_base = false;
		} else {
			epd_partial_frame(frame);
		}
		g_disp_ms = k_uptime_get() - t0;
		g_shown++;
	}
}

K_THREAD_DEFINE(disp_tid, 2048, display_thread, NULL, NULL, NULL, 5, 0, 0);

/* hand a freshly received frame to the display thread */
static void submit(const uint8_t *frame)
{
	k_mutex_lock(&g_lock, K_FOREVER);
	if (g_have) {
		g_dropped++;                        /* panel still busy: newest wins */
	}
	memcpy(g_shared, frame, FRAME_BYTES);
	g_have = true;
	k_mutex_unlock(&g_lock);
	k_sem_give(&g_sem);
}

/* full-refresh a status card: big IP + port, via the display thread */
static void show_ip_screen(const char *ip)
{
	static uint8_t frame[FRAME_BYTES];

	memset(g_bits, 1, sizeof(g_bits));
	draw_text("EPD STREAM READY", 12, 2);
	draw_text(ip, 44, strlen(ip) <= 13 ? 3 : 2);
	draw_text("PORT 5001", 88, 2);
	pack_bits(frame);
	g_force_base = true;
	submit(frame);
}

static int recv_all(int s, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = zsock_recv(s, p, len, 0);

		if (n <= 0) {
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static void net_up(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		printk("no network interface\n");
		return;
	}
	net_if_up(iface);
	net_dhcpv4_start(iface);

	for (int i = 0; i < DHCP_WAIT_MS / 250; i++) {
		if (net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED)) {
			return;
		}
		k_msleep(250);
	}

	struct in_addr ip, nm;

	printk("no DHCP lease, falling back to %s\n", STATIC_IP);
	net_dhcpv4_stop(iface);
	net_addr_pton(NET_AF_INET, STATIC_IP, &ip);
	net_addr_pton(NET_AF_INET, STATIC_MASK, &nm);
	net_if_ipv4_addr_add(iface, &ip, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &ip, &nm);
}

static void serve(int srv)
{
	static uint8_t scratch[FRAME_BYTES];

	while (1) {
		struct net_sockaddr_in peer;
		net_socklen_t plen = sizeof(peer);
		int c = zsock_accept(srv, (struct net_sockaddr *)&peer, &plen);

		if (c < 0) {
			k_msleep(100);
			continue;
		}

		char ipbuf[NET_IPV4_ADDR_LEN];

		net_addr_ntop(NET_AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
		printk("client %s connected\n", ipbuf);

		uint8_t hello[8];

		if (recv_all(c, hello, sizeof(hello)) < 0 ||
		    memcmp(hello, "EPDS", 4) != 0) {
			printk("bad hello, dropping client\n");
			zsock_close(c);
			continue;
		}
		printk("hello ok\n");
		g_force_base = true;               /* first frame repaints the base */
		g_rx = g_shown = g_dropped = 0;

		int64_t t_report = k_uptime_get();

		while (1) {
			uint32_t cmd;

			if (recv_all(c, &cmd, sizeof(cmd)) < 0) {
				break;
			}
			if (cmd == CMD_FRAME) {
				if (recv_all(c, scratch, FRAME_BYTES) < 0) {
					break;
				}
				g_clip_play = false;
				g_rx++;
				if (g_rx <= 5) {
					printk("frame %u received\n", g_rx);
				}
				submit(scratch);
			} else if (cmd == CMD_REFRESH) {
				g_force_base = true;
				k_sem_give(&g_sem);
			} else if (cmd == CMD_CLIP) {
				uint32_t hdr[2];

				if (recv_all(c, hdr, sizeof(hdr)) < 0) {
					break;
				}
				uint32_t n = hdr[0];

				if (n == 0 || n > CLIP_MAX_FRAMES) {
					printk("clip too big: %u frames "
					       "(max %u)\n", n, CLIP_MAX_FRAMES);
					break;
				}
				g_clip_play = false;
				printk("receiving clip: %u frames...\n", n);

				int64_t t_up = k_uptime_get();
				uint32_t i;

				for (i = 0; i < n; i++) {
					if (recv_all(c, g_clip[i],
						     FRAME_BYTES) < 0) {
						break;
					}
					/* per-frame ack: app-level flow control
					 * keeps in-flight data <= one frame -
					 * the raw 384 KB burst intermittently
					 * RSTed/wedged the small-pool stack */
					uint8_t ack = 0x06;

					if (zsock_send(c, &ack, 1, 0) < 0) {
						break;
					}
					if ((i + 1) % 10 == 0) {
						printk("  %u/%u\n", i + 1, n);
					}
				}
				if (i < n) {
					printk("clip upload aborted\n");
					break;
				}
				g_clip_frames = n;
				g_clip_frame_us = hdr[1] ? hdr[1] : 200000;
				g_force_base = true;
				g_clip_gen++;
				g_clip_play = true;
				printk("clip loaded in %lld ms, looping at "
				       "%u ms/frame\n",
				       k_uptime_get() - t_up,
				       g_clip_frame_us / 1000);
			} else if (cmd == CMD_CLEAR) {
				memset(scratch, 0xFF, FRAME_BYTES);
				g_force_base = true;
				submit(scratch);
			} else {
				printk("bad cmd %u, resyncing\n", cmd);
				break;
			}

			if (k_uptime_get() - t_report >= 5000) {
				uint32_t secs = (k_uptime_get() - t_report) / 1000;

				printk("rx %u shown %u dropped %u | %u fps in, "
				       "%u fps out | %lld ms/frame\n",
				       g_rx, g_shown, g_dropped, g_rx / secs,
				       g_shown / secs, g_disp_ms);
				g_rx = g_shown = g_dropped = 0;
				t_report = k_uptime_get();
			}
		}
		printk("client gone\n");
		zsock_close(c);
		if (!g_clip_play) {
			show_ip_screen(g_ip);   /* clip keeps looping otherwise */
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

	printk("\npixpaper netvideo on rp2350, R1.0.0\n");
	epd_hw_reset();
	k_msleep(500);

	net_up();

	struct net_if *iface = net_if_get_default();
	struct net_in_addr *ifaddr =
		net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	if (ifaddr) {
		net_addr_ntop(NET_AF_INET, ifaddr, g_ip, sizeof(g_ip));
	}
	printk("listening on %s:%d\n", g_ip, LISTEN_PORT);
	show_ip_screen(g_ip);               /* panel tells you where to stream */

	int srv = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	struct net_sockaddr_in bind_addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(LISTEN_PORT),
		.sin_addr.s_addr = net_htonl(NET_INADDR_ANY),
	};

	if (srv < 0 || zsock_bind(srv, (struct net_sockaddr *)&bind_addr,
			    sizeof(bind_addr)) < 0 || zsock_listen(srv, 1) < 0) {
		printk("socket setup failed\n");
		return -1;
	}

	serve(srv);
	return 0;
}
