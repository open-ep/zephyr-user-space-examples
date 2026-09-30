/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Kakip CM33: own 1 s heartbeat on SCI5, plus a relay that copies whatever the
 * CR8 printk()s into the shared ring (common/shlog.h) to the same UART, tagged [cr8].
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "shlog.h"

/* CM33 sees R8SRAM at the same physical address as the A55 and the CR8 */
static const struct shlog *const L = (const struct shlog *)SHLOG_PHYS;

static void relay(void)
{
	static uint32_t seen, boots;
	static bool line_start = true;

	if (L->magic != SHLOG_MAGIC) {
		return;                          /* CR8 not running (yet) */
	}
	uint32_t head = L->head;

	if (L->boots != boots || head < seen) {  /* CR8 restarted */
		boots = L->boots;
		seen = 0;
		line_start = true;
	}
	if (head - seen > SHLOG_SIZE) {          /* fell behind: skip what was overwritten */
		seen = head - SHLOG_SIZE;
	}
	for (; seen != head; seen++) {
		char c = L->buf[seen % SHLOG_SIZE];

		if (line_start) {
			printk("[cr8]  ");
		}
		printk("%c", c);
		line_start = (c == '\n');
	}
}

int main(void)
{
	printk("[cm33] Hello World from Zephyr on Kakip CM33 (%s)\n", CONFIG_BOARD_TARGET);
	printk("[cm33] relaying CR8 log from 0x%08x\n", SHLOG_PHYS);   /* last line before the first shared read */
	for (unsigned int n = 0;; n++) {
		if (n % 20 == 0) {
			printk("[cm33] tick %u\n", n / 20);
		}
		relay();
		k_sleep(K_MSEC(50));
	}
	return 0;
}
