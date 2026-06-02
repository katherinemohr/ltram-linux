/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_LTRAM_H
#define _LINUX_LTRAM_H

#include <linux/jump_label.h>

#define LTRAM_NUMA_NODE 1

/* Per-frame state, packed into ltram_frame_origin[]: bit0 = origin of the
 * current occupant, bit1 = whether it has taken a write fault yet. */
#define LTRAM_ORIGIN_ALLOC	0x0	/* placed at allocation time      */
#define LTRAM_ORIGIN_MIGRATED	0x1	/* migrated in from DRAM          */
#define LTRAM_ORIGIN_MASK	0x1
#define LTRAM_FRAME_WRITTEN	0x2	/* occupant has been write-faulted */

struct folio;
struct page;

int ltram_migrate_to(struct folio *folio);
int ltram_migrate_from(struct folio *folio);

/* Endurance rate limiter for DRAM->LtRAM placement: spend one token per
 * migration; false => over budget this instant, skip. */
bool ltram_token_try_consume(void);

/* Called from do_wp_page() when a write fault lands on a ZONE_LTRAM folio;
 * counts each placed page once, split by placement origin. */
void ltram_note_write_fault(struct folio *folio);

/* Called from wp_page_copy() when an LtRAM folio is copied back to DRAM
 * (repatriation on write); counts it against migrated_back, by origin. */
void ltram_note_repatriated(struct folio *folio);

/*
 * Runtime instrumentation gate. Enabled by ltram_init() once a populated
 * ZONE_LTRAM exists, so post_alloc_hook() is a patched-out nop on kernels
 * (or boots) without LtRAM and one cold branch when LtRAM is present.
 */
DECLARE_STATIC_KEY_FALSE(ltram_accounting_enabled);

void __ltram_note_alloc(struct page *page, unsigned int order);
void __ltram_note_free(struct page *page, unsigned int order);

static inline void ltram_note_alloc(struct page *page, unsigned int order)
{
	if (static_branch_unlikely(&ltram_accounting_enabled))
		__ltram_note_alloc(page, order);
}

static inline void ltram_note_free(struct page *page, unsigned int order)
{
	if (static_branch_unlikely(&ltram_accounting_enabled))
		__ltram_note_free(page, order);
}

#endif /* _LINUX_LTRAM_H */
