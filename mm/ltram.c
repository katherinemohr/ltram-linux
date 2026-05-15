// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/mmzone.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/nodemask.h>
#include <linux/printk.h>
#include <linux/ltram.h>
#include "internal.h"

static int __init ltram_init(void)
{
	struct zone *zone;

	if (!node_online(LTRAM_NUMA_NODE)) {
		pr_warn("LTRAM: node %d is not online, no NOR flash memory available\n",
			LTRAM_NUMA_NODE);
		return 0;
	}

	zone = &NODE_DATA(LTRAM_NUMA_NODE)->node_zones[ZONE_LTRAM];

	if (!populated_zone(zone)) {
		pr_warn("LTRAM: ZONE_LTRAM is not populated\n");
		return 0;
	}

	/*
	 * Remove the LTRAM node from N_MEMORY and N_NORMAL_MEMORY so that
	 * kswapd, kcompactd, shrink_node(), and other per-N_MEMORY-node
	 * subsystems never visit it. This runs at subsys_initcall (level 4),
	 * before kswapd_init() and kcompactd_init() (module_init, level 6),
	 * so no threads are created for this node.
	 *
	 * GFP_LTRAM allocations are unaffected: they bypass node_states
	 * entirely and hardcode node_zonelist(LTRAM_NUMA_NODE, ...) in
	 * prepare_alloc_pages().
	 */
	node_clear_state(LTRAM_NUMA_NODE, N_MEMORY);
	node_clear_state(LTRAM_NUMA_NODE, N_NORMAL_MEMORY);

	pr_info("LTRAM: %lu pages (%lu MiB) available in ZONE_LTRAM on node 1\n",
		zone_managed_pages(zone),
		zone_managed_pages(zone) >> (20 - PAGE_SHIFT));

	return 0;
}
subsys_initcall(ltram_init);

/**
 * ltram_migrate_to - migrate a folio from DRAM to ZONE_LTRAM (NOR flash)
 * @folio: folio to migrate; must be on an LRU list
 *
 * Isolates @folio from its LRU, migrates it to a page in ZONE_LTRAM on
 * node 1, and returns it to the LRU on failure.
 *
 * Returns 0 on success, -EBUSY if the folio could not be isolated,
 * or -EFAULT if migration itself failed.
 */
int ltram_migrate_to(struct folio *folio)
{
	LIST_HEAD(list);
	struct migration_target_control mtc = {
		.nid      = LTRAM_NUMA_NODE,
		.gfp_mask = GFP_LTRAM,
	};
	int err;

	if (!folio_isolate_lru(folio))
		return -EBUSY;

	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    folio_nr_pages(folio));
	list_add_tail(&folio->lru, &list);

	err = migrate_pages(&list, alloc_migration_target, NULL,
			    (unsigned long)&mtc, MIGRATE_SYNC, MR_SYSCALL, NULL);
	if (err)
		putback_movable_pages(&list);
	return err ? -EFAULT : 0;
}

/**
 * ltram_migrate_from - migrate a folio from ZONE_LTRAM back to DRAM
 * @folio: folio to migrate; must be on an LRU list and in ZONE_LTRAM
 *
 * Isolates @folio from its LRU, migrates it to a page in ZONE_NORMAL on
 * node 0, and returns it to the LRU on failure.
 *
 * Returns 0 on success, -EBUSY if the folio could not be isolated,
 * or -EFAULT if migration itself failed.
 */
int ltram_migrate_from(struct folio *folio)
{
	LIST_HEAD(list);
	struct migration_target_control mtc = {
		.nid      = 0,  /* DRAM node */
		.gfp_mask = GFP_HIGHUSER_MOVABLE | __GFP_THISNODE,
	};
	int err;

	if (!folio_isolate_lru(folio))
		return -EBUSY;

	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    folio_nr_pages(folio));
	list_add_tail(&folio->lru, &list);

	err = migrate_pages(&list, alloc_migration_target, NULL,
			    (unsigned long)&mtc, MIGRATE_SYNC, MR_SYSCALL, NULL);
	if (err)
		putback_movable_pages(&list);
	return err ? -EFAULT : 0;
}
