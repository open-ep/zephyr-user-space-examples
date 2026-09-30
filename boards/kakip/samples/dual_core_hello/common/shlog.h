/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Tiny one-writer / one-reader log ring shared by the two Zephyr cores on Kakip.
 * CR8 writes its printk output here, CM33 reads it and forwards it to its UART.
 * Lives in the last 4 KB of R8SRAM (the CR8 build shrinks sram3 to keep it free).
 */
#ifndef SHLOG_H
#define SHLOG_H
#include <stdint.h>

#define SHLOG_PHYS   0x081FF000u   /* R8SRAM 0x08180000 + 508 KB */
#define SHLOG_MAGIC  0x43523853u   /* "S8RC" */
#define SHLOG_SIZE   (4096u - 16u)

struct shlog {
	volatile uint32_t magic;
	volatile uint32_t head;      /* total bytes ever written; buf index = head % SHLOG_SIZE */
	volatile uint32_t boots;     /* bumped on every CR8 boot */
	volatile uint32_t rsvd;
	volatile char buf[SHLOG_SIZE];
};
#endif
