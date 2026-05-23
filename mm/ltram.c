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

/*
 * TODO(kmohr): This needs more exhaustive reviewing, but I'll leave it for now
 * so this branch can get merged in and not block other work from progressing.
 */

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
	 * subsystems never visit it.
	 * This is intended to protect the LtRAM from threads like kswapd
	 * and kcompactd.
	 * TODO(kmohr): ensure this doesn't break anything.
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

static int __init ltram_selftest(void)
{
	struct zone *zone = &NODE_DATA(LTRAM_NUMA_NODE)->node_zones[ZONE_LTRAM];
	struct address_space *mapping;
	struct file *file;
	struct folio *folio;
	int ret = 0;

	if (!node_online(LTRAM_NUMA_NODE) || !populated_zone(zone)) {
		pr_info("LTRAM selftest: ZONE_LTRAM not available, skipping\n");
		return 0;
	}

	/* Test 1: GFP_LTRAM allocation lands on the LTRAM node. */
	folio = folio_alloc(GFP_LTRAM, 0);
	if (WARN(!folio, "LTRAM selftest: GFP_LTRAM allocation failed\n"))
		return -ENOMEM;
	if (WARN(folio_nid(folio) != LTRAM_NUMA_NODE,
		 "LTRAM selftest: GFP_LTRAM allocated on node %d, expected %d\n",
		 folio_nid(folio), LTRAM_NUMA_NODE)) {
		folio_put(folio);
		return -EINVAL;
	}
	folio_put(folio);
	pr_info("LTRAM selftest: alloc           OK\n");

	/*
	 * Tests 2 and 3 use a shmem file to obtain a folio with a real
	 * address_space.  folio_migrate_mapping() checks:
	 *   folio_ref_freeze(folio, folio_expected_refs(mapping, folio))
	 * For a single-page shmem folio: expected = 1 + nr_pages = 2.
	 * The freeze must see exactly pagecache-ref(1) + isolation-ref(1).
	 * Any extra caller reference makes it 3 and the freeze fails silently.
	 * We therefore drop our reference before calling ltram_migrate_to/from,
	 * relying on the page cache to keep the folio alive.
	 */
	file = shmem_file_setup("ltram_selftest", PAGE_SIZE, 0);
	if (WARN(IS_ERR(file), "LTRAM selftest: shmem_file_setup: %ld\n",
		 PTR_ERR(file)))
		return PTR_ERR(file);
	mapping = file->f_mapping;

	/* Test 2: migrate DRAM → LTRAM. */
	folio = read_mapping_folio(mapping, 0, file);
	if (WARN(IS_ERR(folio), "LTRAM selftest: read_mapping_folio: %ld\n",
		 PTR_ERR(folio))) {
		ret = PTR_ERR(folio);
		goto out;
	}
	folio_unlock(folio);
	folio_add_lru(folio);
	lru_add_drain();
	folio_put(folio); /* drop caller ref; pagecache keeps folio alive */

	ret = ltram_migrate_to(folio); /* folio ptr invalid after this */
	if (WARN(ret, "LTRAM selftest: migrate_to failed: %d\n", ret))
		goto out;

	folio = filemap_get_folio(mapping, 0); /* look up new LTRAM folio */
	if (WARN(IS_ERR(folio),
		 "LTRAM selftest: folio missing after migrate_to\n")) {
		ret = PTR_ERR(folio);
		goto out;
	}
	if (WARN(folio_nid(folio) != LTRAM_NUMA_NODE,
		 "LTRAM selftest: folio on node %d after migrate_to, expected %d\n",
		 folio_nid(folio), LTRAM_NUMA_NODE)) {
		folio_put(folio);
		ret = -EINVAL;
		goto out;
	}
	pr_info("LTRAM selftest: migrate_to      OK\n");

	/* Test 3: migrate LTRAM → DRAM. */
	folio_put(folio); /* drop filemap_get ref; migration put folio on LRU */
	lru_add_drain();  /* flush per-CPU LRU batch so isolation sees it */

	ret = ltram_migrate_from(folio); /* folio ptr invalid after this */
	if (WARN(ret, "LTRAM selftest: migrate_from failed: %d\n", ret))
		goto out;

	folio = filemap_get_folio(mapping, 0); /* look up new DRAM folio */
	if (WARN(IS_ERR(folio),
		 "LTRAM selftest: folio missing after migrate_from\n")) {
		ret = PTR_ERR(folio);
		goto out;
	}
	if (WARN(folio_nid(folio) == LTRAM_NUMA_NODE,
		 "LTRAM selftest: folio still on LTRAM node after migrate_from\n")) {
		folio_put(folio);
		ret = -EINVAL;
		goto out;
	}
	folio_put(folio);
	pr_info("LTRAM selftest: migrate_from    OK\n");
	pr_info("LTRAM selftest: PASS\n");
out:
	fput(file);
	return ret;
}
late_initcall(ltram_selftest);
