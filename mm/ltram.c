// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/mmzone.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/nodemask.h>
#include <linux/printk.h>
#include <linux/ltram.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/moduleparam.h>
#include "internal.h"

/*
 * TODO(kmohr): This needs more exhaustive reviewing, but I'll leave it for now
 * so this branch can get merged in and not block other work from progressing.
 */

/* ==================== LtRAM runtime instrumentation ====================== */

DEFINE_STATIC_KEY_FALSE(ltram_accounting_enabled);

/* node 1 hosts only ZONE_LTRAM, so the zone's stats are the LtRAM stats. */
static struct zone *ltram_zone;

/* Per-4KB-frame state, indexed by (pfn - ltram_base_pfn); vzalloc'd at init. */
static u32 *ltram_erase_count;		/* program (erase) cycles per frame   */
static u8  *ltram_frame_origin;		/* LTRAM_ORIGIN_* of current occupant */
static unsigned long ltram_base_pfn;
static unsigned long ltram_nr_frames;

/* Erase-block size (in 4KB frames) for the block-level histogram. NOR erases
 * per block; default 16 frames = 64 KiB. Tunable via ltram.block_frames=. */
static unsigned int ltram_block_frames = 16;
module_param_named(block_frames, ltram_block_frames, uint, 0644);

/* Global tallies; per-cpu to avoid cache-line bouncing, summed on read. */
static DEFINE_PER_CPU(unsigned long, ltram_total_programs);
static DEFINE_PER_CPU(unsigned long, ltram_migrated_in);
static DEFINE_PER_CPU(unsigned long, ltram_migrated_back_of_alloc);
static DEFINE_PER_CPU(unsigned long, ltram_migrated_back_of_migrated);
/* Distinct placed pages that took a write fault, split by placement origin.
 * alloc should stay ~0 (read-only routing is write-cold by construction);
 * migrated is the placement-policy quality signal. */
static DEFINE_PER_CPU(unsigned long, ltram_write_faulted_of_alloc);
static DEFINE_PER_CPU(unsigned long, ltram_write_faulted_of_migrated);

/*
 * Record an allocation of @page (1<<order frames) into ZONE_LTRAM. Reached
 * from post_alloc_hook() only when the static key is on (LtRAM present), via
 * ltram_note_alloc(). Bumps the per-frame program/erase count and stamps
 * placement provenance from the current task's migration context.
 */
void __ltram_note_alloc(struct page *page, unsigned int order)
{
	unsigned int i, n = 1u << order;
	unsigned long pfn;
	u8 origin;

	if (page_zonenum(page) != ZONE_LTRAM)
		return;

	origin = current->ltram_migrate_active ? LTRAM_ORIGIN_MIGRATED
					       : LTRAM_ORIGIN_ALLOC;
	pfn = page_to_pfn(page);
	for (i = 0; i < n; i++) {
		unsigned long idx = (pfn + i) - ltram_base_pfn;

		if (idx >= ltram_nr_frames)	/* outside cached span */
			continue;
		/* One owner per frame at program time -> non-atomic is safe. */
		if (ltram_erase_count[idx] != U32_MAX)
			ltram_erase_count[idx]++;
		ltram_frame_origin[idx] = origin;
	}
	this_cpu_add(ltram_total_programs, n);
}

/* Provenance recorded for the frame backing @folio (must be in ZONE_LTRAM). */
static u8 ltram_folio_origin(struct folio *folio)
{
	unsigned long idx = folio_pfn(folio) - ltram_base_pfn;

	if (!ltram_frame_origin || idx >= ltram_nr_frames)
		return LTRAM_ORIGIN_ALLOC;
	return ltram_frame_origin[idx] & LTRAM_ORIGIN_MASK;
}

/*
 * A write fault landed on the ZONE_LTRAM folio @folio (called from
 * do_wp_page). Count each placed page at most once -- the WRITTEN bit is
 * cleared whenever the frame is reprogrammed (see __ltram_note_alloc) -- and
 * attribute it to the current occupant's placement origin so we can report the
 * read-only fraction separately for allocated vs migrated pages.
 *
 * Serialized per page by the pte lock held in do_wp_page, so the
 * read-modify-write of the frame byte needs no extra locking.
 */
void ltram_note_write_fault(struct folio *folio)
{
	unsigned long idx;
	u8 flags;

	if (!ltram_frame_origin)
		return;
	idx = folio_pfn(folio) - ltram_base_pfn;
	if (idx >= ltram_nr_frames)
		return;

	flags = ltram_frame_origin[idx];
	if (flags & LTRAM_FRAME_WRITTEN)	/* already counted this occupant */
		return;
	ltram_frame_origin[idx] = flags | LTRAM_FRAME_WRITTEN;

	if (flags & LTRAM_ORIGIN_MIGRATED)
		this_cpu_inc(ltram_write_faulted_of_migrated);
	else
		this_cpu_inc(ltram_write_faulted_of_alloc);
}

static int ltram_stats_show(struct seq_file *m, void *v)
{
	unsigned long total = 0, mig_in = 0, mb_a = 0, mb_m = 0, wf_a = 0, wf_m = 0;
	unsigned long resident, mn = U32_MAX, mx = 0, sum = 0, nz = 0, i;
	unsigned long placed_a, placed_m;
	int cpu;

	for_each_possible_cpu(cpu) {
		total  += per_cpu(ltram_total_programs, cpu);
		mig_in += per_cpu(ltram_migrated_in, cpu);
		mb_a   += per_cpu(ltram_migrated_back_of_alloc, cpu);
		mb_m   += per_cpu(ltram_migrated_back_of_migrated, cpu);
		wf_a   += per_cpu(ltram_write_faulted_of_alloc, cpu);
		wf_m   += per_cpu(ltram_write_faulted_of_migrated, cpu);
	}
	placed_a = total - mig_in;	/* allocation-time placements */
	placed_m = mig_in;		/* migrated-in placements     */

	resident = ltram_zone ? (zone_managed_pages(ltram_zone) -
				 zone_page_state(ltram_zone, NR_FREE_PAGES)) : 0;

	for (i = 0; i < ltram_nr_frames; i++) {
		u32 c = ltram_erase_count[i];

		sum += c;
		if (c) {
			nz++;
			if (c < mn)
				mn = c;
			if (c > mx)
				mx = c;
		}
	}
	if (!nz)
		mn = 0;

	seq_printf(m, "placed_at_alloc            %lu\n", placed_a);
	seq_printf(m, "placed_migrated_in         %lu\n", placed_m);
	seq_printf(m, "migrated_back_of_alloc     %lu\n", mb_a);
	seq_printf(m, "migrated_back_of_migrated  %lu\n", mb_m);
	seq_printf(m, "write_faulted_of_alloc     %lu\n", wf_a);
	seq_printf(m, "write_faulted_of_migrated  %lu\n", wf_m);
	/* read-only fraction in permille (1000 = 100%% never written). alloc
	 * should be ~1000 by construction; migrated reflects placement quality. */
	seq_printf(m, "read_only_permille_alloc    %lu\n",
		   placed_a ? ((placed_a - wf_a) * 1000) / placed_a : 1000);
	seq_printf(m, "read_only_permille_migrated %lu\n",
		   placed_m ? ((placed_m - wf_m) * 1000) / placed_m : 1000);
	seq_printf(m, "total_programs             %lu\n", total);
	seq_printf(m, "currently_resident_pages   %lu\n", resident);
	seq_printf(m, "frames_total               %lu\n", ltram_nr_frames);
	seq_printf(m, "frames_ever_programmed     %lu\n", nz);
	seq_printf(m, "erase_count_min            %lu\n", mn);
	seq_printf(m, "erase_count_max            %lu\n", mx);
	seq_printf(m, "erase_count_mean_x1000     %lu\n", nz ? (sum * 1000) / nz : 0);
	/* skew = max / mean, x1000; 1000 == perfectly even */
	seq_printf(m, "skew_max_over_mean_x1000   %lu\n",
		   (nz && sum) ? (mx * 1000UL * nz) / sum : 0);
	seq_puts(m, "# erase_count models NOR program cycles per 4KB frame\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ltram_stats);

/* Log2-bucketed distribution of per-frame counts, then per erase-block. */
static void ltram_emit_hist(struct seq_file *m, unsigned long (*get)(unsigned long),
			    unsigned long count, const char *unit)
{
	unsigned long buckets[33] = {0};
	unsigned long i;
	int b;

	for (i = 0; i < count; i++) {
		u32 c = get(i);

		b = c ? min_t(int, 1 + ilog2(c), 32) : 0;
		buckets[b]++;
	}
	seq_printf(m, "  %-9s %-18s %s\n", "bucket", "erase-count range", unit);
	seq_printf(m, "  %-9d %-18s %lu\n", 0, "0", buckets[0]);
	for (b = 1; b <= 32; b++) {
		unsigned long lo, hi;

		if (!buckets[b])
			continue;
		lo = 1UL << (b - 1);
		hi = (1UL << b) - 1;
		if (lo == hi)
			seq_printf(m, "  %-9d %-18lu %lu\n", b, lo, buckets[b]);
		else {
			char r[40];

			snprintf(r, sizeof(r), "%lu-%lu", lo, hi);
			seq_printf(m, "  %-9d %-18s %lu\n", b, r, buckets[b]);
		}
	}
}

static unsigned long ltram_frame_get(unsigned long i)
{
	return ltram_erase_count[i];
}

static unsigned long ltram_block_get(unsigned long blk)
{
	unsigned long start = blk * ltram_block_frames;
	unsigned long end = min(start + ltram_block_frames, ltram_nr_frames);
	unsigned long i, sum = 0;

	for (i = start; i < end; i++)
		sum += ltram_erase_count[i];
	return sum;
}

static int ltram_erase_histogram_show(struct seq_file *m, void *v)
{
	unsigned long nblocks;

	seq_puts(m, "# per-4KB-frame program/erase-count distribution\n");
	ltram_emit_hist(m, ltram_frame_get, ltram_nr_frames, "frames");

	nblocks = ltram_block_frames ?
		DIV_ROUND_UP(ltram_nr_frames, ltram_block_frames) : 0;
	seq_printf(m, "\n# per-erase-block (%u frames = %u KiB) summed distribution\n",
		   ltram_block_frames, ltram_block_frames * 4);
	ltram_emit_hist(m, ltram_block_get, nblocks, "blocks");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ltram_erase_histogram);

static ssize_t ltram_reset_write(struct file *f, const char __user *buf,
				 size_t len, loff_t *off)
{
	int cpu;

	if (ltram_erase_count)
		memset(ltram_erase_count, 0, ltram_nr_frames * sizeof(u32));
	if (ltram_frame_origin)
		memset(ltram_frame_origin, 0, ltram_nr_frames);
	for_each_possible_cpu(cpu) {
		per_cpu(ltram_total_programs, cpu) = 0;
		per_cpu(ltram_migrated_in, cpu) = 0;
		per_cpu(ltram_migrated_back_of_alloc, cpu) = 0;
		per_cpu(ltram_migrated_back_of_migrated, cpu) = 0;
		per_cpu(ltram_write_faulted_of_alloc, cpu) = 0;
		per_cpu(ltram_write_faulted_of_migrated, cpu) = 0;
	}
	return len;
}

static const struct file_operations ltram_reset_fops = {
	.open	= simple_open,
	.write	= ltram_reset_write,
	.llseek	= noop_llseek,
};

/* Allocate wear-tracking state, expose debugfs, and turn accounting on. */
static void ltram_accounting_init(struct zone *zone)
{
	struct dentry *dir;

	ltram_zone	= zone;
	ltram_base_pfn	= zone->zone_start_pfn;
	ltram_nr_frames	= zone->spanned_pages;

	ltram_erase_count  = vzalloc(array_size(ltram_nr_frames, sizeof(u32)));
	ltram_frame_origin = vzalloc(ltram_nr_frames);
	if (!ltram_erase_count || !ltram_frame_origin) {
		pr_warn("LTRAM: wear-tracking alloc failed for %lu frames; accounting off\n",
			ltram_nr_frames);
		vfree(ltram_erase_count);
		vfree(ltram_frame_origin);
		ltram_erase_count = NULL;
		ltram_frame_origin = NULL;
		return;
	}

	dir = debugfs_create_dir("ltram", NULL);
	debugfs_create_file("stats", 0444, dir, NULL, &ltram_stats_fops);
	debugfs_create_file("erase_histogram", 0444, dir, NULL,
			    &ltram_erase_histogram_fops);
	debugfs_create_file("reset", 0200, dir, NULL, &ltram_reset_fops);

	static_branch_enable(&ltram_accounting_enabled);

	pr_info("LTRAM: wear/placement accounting on; %lu frames, %lu KiB tracking state\n",
		ltram_nr_frames,
		(ltram_nr_frames * (sizeof(u32) + sizeof(u8))) >> 10);
}

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

	ltram_accounting_init(zone);

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
	unsigned long nr = folio_nr_pages(folio);
	int err;

	if (!folio_isolate_lru(folio))
		return -EBUSY;

	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    folio_nr_pages(folio));
	list_add_tail(&folio->lru, &list);

	/* Mark the migration context so the destination allocation is attributed
	 * as migrated-in (see __ltram_note_alloc via post_alloc_hook). */
	current->ltram_migrate_active = 1;
	err = migrate_pages(&list, alloc_migration_target, NULL,
			    (unsigned long)&mtc, MIGRATE_SYNC, MR_SYSCALL, NULL);
	current->ltram_migrate_active = 0;

	if (err)
		putback_movable_pages(&list);
	else
		this_cpu_add(ltram_migrated_in, nr);
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
	unsigned long nr = folio_nr_pages(folio);
	u8 origin = ltram_folio_origin(folio);	/* read before the page moves */
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
	else if (origin == LTRAM_ORIGIN_MIGRATED)
		this_cpu_add(ltram_migrated_back_of_migrated, nr);
	else
		this_cpu_add(ltram_migrated_back_of_alloc, nr);
	return err ? -EFAULT : 0;
}
