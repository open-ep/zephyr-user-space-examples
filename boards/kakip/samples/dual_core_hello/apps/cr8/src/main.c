/* SPDX-License-Identifier: Apache-2.0 */

/* Kakip CR8_0 hello: printk -> shared ring in R8SRAM, forwarded to a UART by the CM33. */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/cache.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>
#include "shlog.h"

static struct shlog *const L = (struct shlog *)SHLOG_PHYS;

static void flush(volatile void *p, size_t n)
{
	/* CR8 has a data cache; the CM33 reads SRAM directly */
	sys_cache_data_flush_range((void *)p, n);
}

static int shlog_out(int c)
{
	uint32_t h = L->head;

	L->buf[h % SHLOG_SIZE] = (char)c;
	flush(&L->buf[h % SHLOG_SIZE], 1);
	barrier_dmem_fence_full();     /* byte lands before the reader sees the new head */
	L->head = h + 1;
	flush(&L->head, sizeof(L->head));
	return c;
}

static int shlog_init(void)
{
	L->magic = 0;
	flush(&L->magic, sizeof(L->magic));
	L->head = 0;
	L->boots = L->boots + 1;
	flush(L, 16);
	barrier_dmem_fence_full();
	L->magic = SHLOG_MAGIC;
	flush(&L->magic, sizeof(L->magic));
	__printk_hook_install(shlog_out);
	return 0;
}
SYS_INIT(shlog_init, PRE_KERNEL_1, 0);   /* before the boot banner */

int main(void)
{
	printk("Hello World from Zephyr on Kakip CR8_0 (%s)\n", CONFIG_BOARD_TARGET);
	for (unsigned int n = 0;; n++) {
		printk("tick %u\n", n);
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
