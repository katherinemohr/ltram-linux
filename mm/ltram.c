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
#include <linux/slab.h>
#include <linux/vmstat.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#include <linux/log2.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/pid.h>
#include <linux/swap.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/pagewalk.h>
#include <linux/uaccess.h>
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

/* Global tallies; per-cpu to avoid cache-line bouncing, summed on read. */

/* Total frame program/erase operations on LtRAM (one per page programmed in;
 * += 1<<order in __ltram_note_alloc). Device-wide wear count. */
static DEFINE_PER_CPU(unsigned long, ltram_total_programs);

/* Pages moved DRAM->LtRAM by the placement policy (scan/migrate_va/range);
 * += nr on a successful ltram_migrate_to(). */
static DEFINE_PER_CPU(unsigned long, ltram_migrated_in);

/* Pages repatriated LtRAM->DRAM whose origin was LTRAM_ORIGIN_ALLOC (placed at
 * allocation time by read-only fault routing). Should stay ~0: a read-only
 * placement that got written and pulled back. */
static DEFINE_PER_CPU(unsigned long, ltram_migrated_back_of_alloc);

/* Pages repatriated LtRAM->DRAM whose origin was LTRAM_ORIGIN_MIGRATED (moved in
 * by the scanner). The migration-policy quality signal: scanner-placed pages
 * that turned out to be written and had to come back. */
static DEFINE_PER_CPU(unsigned long, ltram_migrated_back_of_migrated);

/* Distinct placed pages that took a write fault, split by placement origin.
 * alloc should stay ~0 (read-only routing is write-cold by construction);
 * migrated is the placement-policy quality signal. */
static DEFINE_PER_CPU(unsigned long, ltram_write_faulted_of_alloc);
static DEFINE_PER_CPU(unsigned long, ltram_write_faulted_of_migrated);
/* Pages leaving ZONE_LTRAM (frees). placed - freed = net change in residency,
 * which exposes churn (how transient LtRAM placements are). */
static DEFINE_PER_CPU(unsigned long, ltram_freed);

/* ---- scanning hand (autonomous DRAM->LtRAM placement policy) state -------- */
static int ltram_scan_pid;			/* 0/-1 = off                  */
static unsigned int ltram_scan_interval_ms = 100;
static unsigned int ltram_scan_batch = 4096;	/* PTEs visited per wake       */
static unsigned int ltram_scan_max_migrate = 64;/* candidates queued per wake  */
module_param_named(scan_interval_ms, ltram_scan_interval_ms, uint, 0644);
module_param_named(scan_batch, ltram_scan_batch, uint, 0644);
module_param_named(scan_max_migrate, ltram_scan_max_migrate, uint, 0644);
static struct task_struct *ltram_scan_task;
static unsigned long ltram_scan_cursor;		/* resume address across wakes */
static unsigned long ltram_scan_passes;		/* sweeps performed (display)  */
static unsigned long ltram_scan_total_migrated;	/* migrated by the scan        */
static unsigned long ltram_scan_last_cand;	/* candidates found last wake  */

/* ---- endurance token bucket (DRAM->LtRAM placement rate limiter) ---------
 * The sustainable device-wide program rate is the total program budget spread
 * over the target lifetime:
 *   rate = (frames * endurance_cycles) / lifetime_seconds
 * Computed at compile time from the constants below (round-to-nearest) so it
 * tracks the modeled NOR endurance and LtRAM size instead of being a magic
 * number. With the defaults (256 MiB = 65536 frames, 1e5 cycles, 5
 * years) this works out to ~42/s.
 * Each placement migration spends one token. Time-based refill; tokens kept in
 * milli-units so sub-second refill works at low rates. token_rate=0 disables
 * the limit (bring-up). Valid as a device-wide budget only WITH wear-leveling
 * (else the hottest frame's endurance limit binds first -- see
 * docs/wear_leveling_allocator.md).
 *
 * LTRAM_ASSUMED_SIZE_MB is a compile-time stand-in: the real zone size is a
 * boot-time property (ltram_nr_frames). If you boot a different LtRAM size,
 * scale token_rate= to match (or recompute from ltram_nr_frames in
 * ltram_init()). ULL keeps frames*cycles (6.5e9) from overflowing 32-bit.
 */
#define LTRAM_ENDURANCE_CYCLES	100000ULL	/* NOR program/erase cycles/frame */
#define LTRAM_TARGET_LIFE_YEARS	5ULL
#define LTRAM_SECS_PER_YEAR	31557600ULL	/* 365.25 d * 86400 s */
#define LTRAM_FRAME_BYTES	4096ULL
#define LTRAM_ASSUMED_SIZE_MB	256ULL		/* default node-1 LtRAM size      */
#define LTRAM_ASSUMED_FRAMES \
	(LTRAM_ASSUMED_SIZE_MB * 1024ULL * 1024ULL / LTRAM_FRAME_BYTES)
#define LTRAM_TARGET_LIFE_SECS	(LTRAM_TARGET_LIFE_YEARS * LTRAM_SECS_PER_YEAR)
#define LTRAM_TOKEN_RATE_DEFAULT \
	((LTRAM_ASSUMED_FRAMES * LTRAM_ENDURANCE_CYCLES + LTRAM_TARGET_LIFE_SECS / 2) \
	 / LTRAM_TARGET_LIFE_SECS)

static unsigned int ltram_token_rate = LTRAM_TOKEN_RATE_DEFAULT;  /* tokens/sec */
static unsigned int ltram_token_cap  = 512;	/* burst cap  */
module_param_named(token_rate, ltram_token_rate, uint, 0644);
module_param_named(token_cap,  ltram_token_cap,  uint, 0644);

static DEFINE_SPINLOCK(ltram_token_lock);
static u64 ltram_tokens_milli;			/* available tokens x1000 */
static u64 ltram_token_last_ns;

/* Refill by elapsed time; caller holds ltram_token_lock. */
static void ltram_token_refill_locked(void)
{
	u64 now = ktime_get_ns();
	u64 cap = (u64)ltram_token_cap * 1000ULL;

	ltram_tokens_milli += ((now - ltram_token_last_ns) * ltram_token_rate)
			      / 1000000ULL;
	ltram_token_last_ns = now;
	if (ltram_tokens_milli > cap)
		ltram_tokens_milli = cap;
}

/* Spend one token if available; true => a placement migration may proceed. */
bool ltram_token_try_consume(void)
{
	bool ok = false;

	if (!ltram_token_rate)			/* 0 == unlimited */
		return true;
	spin_lock(&ltram_token_lock);
	ltram_token_refill_locked();
	if (ltram_tokens_milli >= 1000ULL) {
		ltram_tokens_milli -= 1000ULL;
		ok = true;
	}
	spin_unlock(&ltram_token_lock);
	return ok;
}

/* Current token count (for stats). */
static unsigned long ltram_tokens_now(void)
{
	unsigned long t;

	spin_lock(&ltram_token_lock);
	ltram_token_refill_locked();
	t = (unsigned long)(ltram_tokens_milli / 1000ULL);
	spin_unlock(&ltram_token_lock);
	return t;
}

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

/* Record a page (1<<order frames) leaving ZONE_LTRAM. Mirror of note_alloc;
 * reached from free_pages_prepare() only when accounting is enabled. */
void __ltram_note_free(struct page *page, unsigned int order)
{
	if (page_zonenum(page) != ZONE_LTRAM)
		return;
	this_cpu_add(ltram_freed, 1u << order);
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
 * Account a folio leaving LtRAM for DRAM, split by placement origin. Single
 * chokepoint for both repatriation paths -- write-fault COW (wp_page_copy, via
 * the export) and the explicit ltram_migrate_from() -- so the origin split and
 * the warning can't drift between call sites. An alloc-origin page coming back
 * means a page we placed in LtRAM as read-only got written and pulled back;
 * read-only routing is meant to be write-cold, so migrated_back_of_alloc should
 * stay ~0 -- warn (ratelimited) so a run surfaces it.
 *
 * @folio is the *source* (LtRAM) folio. Its struct page and PFN are address-
 * stable and the page stays in ZONE_LTRAM regardless of refcount, so callers may
 * invoke this after a successful migrate_pages() has moved the contents to DRAM
 * (ltram_migrate_from does); the origin byte read here is the LtRAM frame we
 * came from.
 */
void ltram_note_repatriated(struct folio *folio)
{
	unsigned long nr = folio_nr_pages(folio);

	if (!ltram_frame_origin)
		return;
	if (ltram_folio_origin(folio) == LTRAM_ORIGIN_MIGRATED) {
		this_cpu_add(ltram_migrated_back_of_migrated, nr);
	} else {
		// pr_warn_ratelimited("ltram: read-only-placed page repatriated pfn=0x%lx nr=%lu\n",
		// 		    folio_pfn(folio), nr);
		this_cpu_add(ltram_migrated_back_of_alloc, nr);
	}
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
	unsigned long freed = 0;
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
		freed  += per_cpu(ltram_freed, cpu);
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
	seq_printf(m, "freed                      %lu\n", freed);
	seq_printf(m, "net_programmed             %ld\n", (long)total - (long)freed);
	seq_printf(m, "currently_resident_pages   %lu\n", resident);
	seq_printf(m, "frames_total               %lu\n", ltram_nr_frames);
	seq_printf(m, "frames_ever_programmed     %lu\n", nz);
	seq_printf(m, "erase_count_min            %lu\n", mn);
	seq_printf(m, "erase_count_max            %lu\n", mx);
	seq_printf(m, "erase_count_mean_x1000     %lu\n", nz ? (sum * 1000) / nz : 0);
	/* skew = max / mean, x1000; 1000 == perfectly even */
	seq_printf(m, "skew_max_over_mean_x1000   %lu\n",
		   (nz && sum) ? (mx * 1000UL * nz) / sum : 0);

	/* Exact median and mode of the per-frame erase counts, over programmed
	 * frames only (values are small integers, so a counting array is cheap;
	 * cap it so a long campaign can't ask for an absurd allocation). */
	{
		unsigned long cap = min(mx, 65536UL);
		unsigned long *hist = nz ? kcalloc(cap + 1, sizeof(*hist), GFP_KERNEL)
					 : NULL;
		unsigned long med = 0, mode = 0, mode_cnt = 0, acc = 0;

		if (hist) {
			for (i = 0; i < ltram_nr_frames; i++) {
				u32 c = ltram_erase_count[i];

				if (c)
					hist[c > cap ? cap : c]++;
			}
			for (i = 1; i <= cap; i++) {
				if (hist[i] > mode_cnt) {
					mode_cnt = hist[i];
					mode = i;
				}
				acc += hist[i];
				if (!med && acc >= (nz + 1) / 2)
					med = i;
			}
			kfree(hist);
		}
		seq_printf(m, "erase_count_median         %lu\n", med);
		seq_printf(m, "erase_count_mode           %lu\n", mode);
	}

	seq_printf(m, "token_rate_per_s           %u\n", ltram_token_rate);
	seq_printf(m, "tokens_available           %lu\n", ltram_tokens_now());
	seq_printf(m, "scan_pid                   %d\n", READ_ONCE(ltram_scan_pid));
	seq_printf(m, "scan_passes                %lu\n", ltram_scan_passes);
	seq_printf(m, "scan_migrated              %lu\n", ltram_scan_total_migrated);
	seq_printf(m, "scan_last_candidates       %lu\n", ltram_scan_last_cand);
	seq_puts(m, "# erase_count models NOR program cycles per 4KB frame\n");
	seq_puts(m, "# tokens: endurance budget for DRAM->LtRAM placement (see token_rate_per_s)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ltram_stats);

/*
 * Log2-bucketed distribution of per-4KB-frame program/erase counts. The modeled
 * NOR erases per 4KB frame (see the LtRAM specs in README.md), so the frame IS
 * the erase block -- no separate per-block aggregation is needed.
 */
static int ltram_erase_histogram_show(struct seq_file *m, void *v)
{
	unsigned long buckets[33] = {0};
	unsigned long i;
	int b;

	for (i = 0; i < ltram_nr_frames; i++) {
		u32 c = ltram_erase_count[i];

		b = c ? min_t(int, 1 + ilog2(c), 32) : 0;
		buckets[b]++;
	}

	seq_puts(m, "# per-4KB-frame program/erase-count distribution\n");
	seq_printf(m, "  %-9s %-18s %s\n", "bucket", "erase-count range", "frames");
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
		per_cpu(ltram_freed, cpu) = 0;
	}
	return len;
}

static const struct file_operations ltram_reset_fops = {
	.open	= simple_open,
	.write	= ltram_reset_write,
	.llseek	= noop_llseek,
};

/*
 * Write-protect the PTE mapping @va in @mm so the next write faults into
 * do_wp_page() -> COW repatriation. Single-PTE: correct for private,
 * single-mapped pages (the placement target), which ltram_migrate_one()
 * enforces by skipping folios with folio_mapcount() != 1. Covering folios
 * mapped in several processes (COW after fork) would need an rmap walk; until
 * then those folios are simply not migrated in.
 */
static void ltram_wrprotect_va(struct mm_struct *mm, unsigned long va)
{
	struct vm_area_struct *vma;
	pte_t *ptep, pte;
	spinlock_t *ptl;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, va);
	if (vma && follow_pte(mm, va, &ptep, &ptl) == 0) {
		pte = ptep_get(ptep);
		if (pte_present(pte) && pte_write(pte)) {
			flush_cache_page(vma, va, pte_pfn(pte));
			pte = ptep_clear_flush(vma, va, ptep);
			set_pte_at(mm, va, ptep, pte_wrprotect(pte));
		}
		pte_unmap_unlock(ptep, ptl);
	}
	mmap_read_unlock(mm);
}

/*
 * Migrate the single page at @va in @mm into LtRAM and write-protect it so a
 * later write faults -> do_wp_page COW repatriation. Caller must have drained
 * the per-CPU LRU (lru_add_drain_all) so the page is isolable. Skips ineligible
 * VMAs. Returns 0 (migrated, or already in LtRAM) or a negative errno.
 */
static int ltram_migrate_one(struct mm_struct *mm, unsigned long va)
{
	struct vm_area_struct *vma;
	struct page *page;
	struct folio *folio;
	int ret;

	mmap_read_lock(mm);
	vma = vma_lookup(mm, va);
	if (!vma ||
	    !(vma->vm_flags & VM_WRITE) ||			/* RO already LtRAM-eligible */
	    (vma->vm_flags & (VM_SHARED | VM_MAYSHARE)) ||	/* shared excluded */
	    (vma->vm_flags & VM_GROWSDOWN) ||			/* stack */
	    (vma->vm_flags & (VM_HUGETLB | VM_IO | VM_PFNMAP | VM_MIXEDMAP))) {
		mmap_read_unlock(mm);
		return -EINVAL;
	}
	page = follow_page(vma, va, FOLL_GET);			/* refs the page */
	mmap_read_unlock(mm);
	if (IS_ERR_OR_NULL(page))
		return -EFAULT;
	folio = page_folio(page);
	if (folio_zonenum(folio) == ZONE_LTRAM) {		/* already there */
		folio_put(folio);
		return 0;
	}
	/*
	 * Only migrate single-mapped folios. Repatriation relies on
	 * ltram_wrprotect_va() write-protecting the mapping so a later write
	 * faults into do_wp_page(); it fixes only the one (mm, va) we hold here.
	 * A folio mapped at more than one PTE -- COW-shared after fork, or a
	 * multiply-mapped large folio -- would keep a writable alias that could
	 * write the LtRAM page in place, bypassing repatriation. Skip those
	 * until an rmap-wide write-protect exists.
	 */
	if (folio_mapcount(folio) != 1) {
		folio_put(folio);
		return -EBUSY;
	}
	ret = ltram_migrate_to(folio);				/* consumes the ref */
	if (!ret)
		ltram_wrprotect_va(mm, va);
	return ret;
}

/* Resolve "<pid>" to its mm with a reference held, or NULL. */
static struct mm_struct *ltram_get_mm(int pid)
{
	struct task_struct *task = find_get_task_by_vpid(pid);
	struct mm_struct *mm;

	if (!task)
		return NULL;
	mm = get_task_mm(task);
	put_task_struct(task);
	return mm;
}

/*
 * debugfs: write "<pid> <hex-va>" to migrate that one DRAM page into LtRAM
 * (token-gated). The placement *mechanism*; the scanning-hand policy replaces
 * this manual trigger later.
 */
static ssize_t ltram_migrate_va_write(struct file *f, const char __user *buf,
				      size_t len, loff_t *ppos)
{
	char kbuf[80];
	struct mm_struct *mm;
	unsigned long va;
	int pid, ret;

	if (len == 0 || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (sscanf(kbuf, "%d %lx", &pid, &va) != 2)
		return -EINVAL;
	va &= PAGE_MASK;

	mm = ltram_get_mm(pid);
	if (!mm)
		return -ESRCH;
	if (!ltram_token_try_consume()) {	/* over endurance budget */
		mmput(mm);
		return -EBUSY;
	}
	lru_add_drain_all();
	ret = ltram_migrate_one(mm, va);
	if (!ret)
		pr_info_ratelimited("ltram: migrate DRAM->LtRAM va=0x%lx pid=%d (WP)\n",
				    va, pid);
	mmput(mm);
	return ret ? ret : len;
}

/*
 * debugfs: write "<pid> <hex-start> <npages>" to migrate a contiguous range
 * into LtRAM with a SINGLE LRU drain for the whole batch (the per-page drain in
 * migrate_va is far too slow for the 1M migrate-stress harness). Token-gated
 * per page; stops early when the bucket is empty. Always returns len.
 */
static ssize_t ltram_migrate_range_write(struct file *f, const char __user *buf,
					 size_t len, loff_t *ppos)
{
	char kbuf[96];
	struct mm_struct *mm;
	unsigned long start, npages, i, done = 0;
	int pid;

	if (len == 0 || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (sscanf(kbuf, "%d %lx %lu", &pid, &start, &npages) != 3)
		return -EINVAL;
	start &= PAGE_MASK;
	if (npages > (1UL << 20))		/* sanity cap */
		npages = 1UL << 20;

	mm = ltram_get_mm(pid);
	if (!mm)
		return -ESRCH;
	lru_add_drain_all();			/* once for the whole batch */
	for (i = 0; i < npages; i++) {
		if (!ltram_token_try_consume())	/* over budget: stop here */
			break;
		if (ltram_migrate_one(mm, start + i * PAGE_SIZE) == 0)
			done++;
	}
	mmput(mm);
	pr_info_ratelimited("ltram: migrate range pid=%d start=0x%lx n=%lu done=%lu\n",
			    pid, start, npages, done);
	return len;
}

static const struct file_operations ltram_migrate_va_fops = {
	.open	= simple_open,
	.write	= ltram_migrate_va_write,
	.llseek	= noop_llseek,
};
static const struct file_operations ltram_migrate_range_fops = {
	.open	= simple_open,
	.write	= ltram_migrate_range_write,
	.llseek	= noop_llseek,
};

/* ===========================================================================
 * The scanning hand: autonomous DRAM->LtRAM placement policy.
 *
 * A single kthread sweeps a target process's private writable ANON pages as a
 * CLOCK. On each visit it ages the page by clearing the hardware dirty bit; a
 * page found still clean on a later visit went a full lap without a write =
 * write-cold, and is migrated to LtRAM (endurance-token-gated, then write-
 * protected so a future write repatriates it). Anon-only keeps dirty-bit
 * clearing safe (no writeback; no swap in this model). THP is skipped (v1).
 * Inert until a pid is written to /sys/kernel/debug/ltram/scan_pid.
 * ===========================================================================
 */
struct ltram_scan_ctx {
	unsigned long cand[64];			/* write-cold VAs to migrate   */
	int ncand, maxc;
	unsigned long scanned, batch, aged, stop_addr;
};

/* Age a present pte: clear the dirty bit so a future write re-sets it. The
 * caller holds the pte lock; the TLB is flushed once per sweep (see below). */
static void ltram_pte_age(struct vm_area_struct *vma, unsigned long addr,
			  pte_t *ptep)
{
	pte_t old = ptep_modify_prot_start(vma, addr, ptep);

	ptep_modify_prot_commit(vma, addr, ptep, old, pte_mkclean(old));
}

static bool ltram_scan_vma_ok(struct vm_area_struct *vma)
{
	if (!vma || !(vma->vm_flags & VM_WRITE))		/* RO already routed */
		return false;
	if (vma->vm_flags & (VM_SHARED | VM_MAYSHARE))		/* shared excluded   */
		return false;
	if (vma->vm_flags & VM_GROWSDOWN)			/* stack             */
		return false;
	if (vma->vm_flags & (VM_HUGETLB | VM_IO | VM_PFNMAP | VM_MIXEDMAP))
		return false;
	return true;
}

static int ltram_scan_test(unsigned long start, unsigned long end,
			   struct mm_walk *walk)
{
	return ltram_scan_vma_ok(walk->vma) ? 0 : 1;	/* 1 => skip this VMA */
}

static int ltram_scan_pmd(pmd_t *pmd, unsigned long addr, unsigned long next,
			  struct mm_walk *walk)
{
	if (pmd_trans_huge(*pmd))			/* v1: base pages only */
		walk->action = ACTION_CONTINUE;
	return 0;
}

static int ltram_scan_pte(pte_t *pte, unsigned long addr, unsigned long next,
			  struct mm_walk *walk)
{
	struct ltram_scan_ctx *c = walk->private;
	pte_t p = ptep_get(pte);
	struct page *pg;

	if (pte_present(p)) {
		pg = vm_normal_page(walk->vma, addr, p);
		if (pg && PageAnon(pg)) {
			if (pte_dirty(p)) {
				ltram_pte_age(walk->vma, addr, pte);	/* written */
				c->aged++;
			} else if (page_zonenum(pg) != ZONE_LTRAM &&
				   c->ncand < c->maxc) {
				c->cand[c->ncand++] = addr;		/* cold    */
			}
		}
	}
	if (++c->scanned >= c->batch || c->ncand >= c->maxc) {
		c->stop_addr = addr + PAGE_SIZE;
		return 1;					/* end this sweep */
	}
	return 0;
}

static const struct mm_walk_ops ltram_scan_ops = {
	.test_walk = ltram_scan_test,
	.pmd_entry = ltram_scan_pmd,
	.pte_entry = ltram_scan_pte,
};

static int ltram_scan_thread(void *unused)
{
	while (!kthread_should_stop()) {
		int pid = READ_ONCE(ltram_scan_pid);
		struct mm_struct *mm;
		struct ltram_scan_ctx c;
		int i, ret;

		if (pid <= 0) {
			msleep_interruptible(ltram_scan_interval_ms);
			continue;
		}
		mm = ltram_get_mm(pid);
		if (!mm) {				/* target exited: stop */
			WRITE_ONCE(ltram_scan_pid, 0);
			continue;
		}

		memset(&c, 0, sizeof(c));
		c.batch = ltram_scan_batch ? ltram_scan_batch : 4096;
		c.maxc  = min_t(int, ltram_scan_max_migrate,
				(int)ARRAY_SIZE(c.cand));

		/* Pass 1: age written pages, collect write-cold candidates. */
		lru_add_drain_all();
		mmap_read_lock(mm);
		ret = walk_page_range(mm, ltram_scan_cursor, TASK_SIZE,
				      &ltram_scan_ops, &c);
		if (c.aged)
			flush_tlb_mm(mm);		/* one flush per sweep */
		mmap_read_unlock(mm);
		/* ret==1 => stopped at the batch limit; else swept to the end. */
		ltram_scan_cursor = (ret == 1) ? c.stop_addr : 0;

		/* Pass 2: migrate candidates with locks dropped, endurance-gated.
		 * Each ltram_migrate_one re-checks eligibility and write-protects. */
		for (i = 0; i < c.ncand; i++) {
			if (!ltram_token_try_consume())
				break;
			ltram_migrate_one(mm, c.cand[i]);
		}
		mmput(mm);

		ltram_scan_passes++;
		ltram_scan_last_cand = c.ncand;
		ltram_scan_total_migrated += i;

		msleep_interruptible(ltram_scan_interval_ms);
	}
	return 0;
}

/* debugfs: write a pid to start scanning it; 0 or -1 to stop. */
static ssize_t ltram_scan_pid_write(struct file *f, const char __user *buf,
				    size_t len, loff_t *ppos)
{
	char kbuf[32];
	int pid;

	if (len == 0 || len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';
	if (kstrtoint(strim(kbuf), 10, &pid))
		return -EINVAL;
	ltram_scan_cursor = 0;
	WRITE_ONCE(ltram_scan_pid, pid);
	pr_info("ltram: scan target pid=%d (interval=%ums batch=%u maxmig=%u)\n",
		pid, ltram_scan_interval_ms, ltram_scan_batch,
		ltram_scan_max_migrate);
	return len;
}
static int ltram_scan_pid_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", READ_ONCE(ltram_scan_pid));
	return 0;
}
static int ltram_scan_pid_open(struct inode *i, struct file *f)
{
	return single_open(f, ltram_scan_pid_show, NULL);
}
static const struct file_operations ltram_scan_pid_fops = {
	.open	 = ltram_scan_pid_open,
	.read	 = seq_read,
	.write	 = ltram_scan_pid_write,
	.llseek	 = seq_lseek,
	.release = single_release,
};

/* Allocate wear-tracking state, expose debugfs, and turn accounting on. */
static void ltram_accounting_init(struct zone *zone)
{
	struct dentry *dir;

	ltram_zone	= zone;
	ltram_base_pfn	= zone->zone_start_pfn;
	ltram_nr_frames	= zone->spanned_pages;

	/* Start the endurance token bucket EMPTY; tokens accrue at the endurance rate. */
	ltram_token_last_ns = ktime_get_ns();
	ltram_tokens_milli  = 0;  /* start empty: tokens are earned at the endurance rate */
	pr_info("LTRAM: endurance token bucket = %u programs/s (cap %u)%s\n",
		ltram_token_rate, ltram_token_cap,
		ltram_token_rate ? "" : "  [DISABLED: unlimited]");

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
	debugfs_create_file("migrate_va", 0200, dir, NULL, &ltram_migrate_va_fops);
	debugfs_create_file("migrate_range", 0200, dir, NULL, &ltram_migrate_range_fops);
	debugfs_create_file("scan_pid", 0644, dir, NULL, &ltram_scan_pid_fops);

	/* Start the scanning hand (idle until a pid is written to scan_pid). */
	ltram_scan_task = kthread_run(ltram_scan_thread, NULL, "ltram_scan");
	if (IS_ERR(ltram_scan_task)) {
		pr_warn("LTRAM: scan kthread failed to start (%ld); policy disabled\n",
			PTR_ERR(ltram_scan_task));
		ltram_scan_task = NULL;
	}

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

	/*
	 * Contract (mirrors migrate_misplaced_folio): the caller holds an
	 * elevated reference on @folio; this function consumes it. On success
	 * the isolation reference keeps the folio alive through migration; on
	 * any failure we drop the caller's reference before returning.
	 */
	if (!folio_isolate_lru(folio)) {
		folio_put(folio);
		return -EBUSY;
	}

	node_stat_mod_folio(folio, NR_ISOLATED_ANON + folio_is_file_lru(folio),
			    nr);
	folio_put(folio);		/* isolation took its own ref */
	list_add_tail(&folio->lru, &list);

	/* Mark the migration context so the destination allocation is attributed
	 * as migrated-in (see __ltram_note_alloc via post_alloc_hook).
	 *
	 * TODO(kmohr): this stamp is load-bearing on migrate_pages() allocating
	 * the destination synchronously in THIS task, inside the active=1 window
	 * (alloc_migration_target runs as the get_new_folio callback, before the
	 * copy). If that ever changes -- deferred/async destination allocation, or
	 * a new path that migrates into LtRAM without setting this flag -- pages
	 * would silently mislabel as LTRAM_ORIGIN_ALLOC. It is also a single bit,
	 * not save/restore, so re-entrant ltram_migrate_to() would clear it early.
	 * Affects placement-origin stats only, not correctness. Harden by
	 * save/restoring the flag, or thread origin through migration_target_control. */
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
	else
		ltram_note_repatriated(folio);
	return err ? -EFAULT : 0;
}
