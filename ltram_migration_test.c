// SPDX-License-Identifier: GPL-2.0
/*
 * ltram_migration_test.c - Test automatic LTRAM page migration
 *
 * Verifies two fault hooks in mm/memory.c:
 *
 *   1. Read phase: mmap a file PROT_READ, read-fault all pages.
 *      do_fault() should migrate each page to ZONE_LTRAM (node 1).
 *      move_pages() query must report node 1 for migrated pages.
 *
 *   2. Write phase: mprotect to PROT_READ|PROT_WRITE, write each page.
 *      do_wp_page() detects LTRAM folio, migrates back to DRAM (node 0),
 *      returns VM_FAULT_RETRY, and the write completes via COW.
 *      move_pages() query must report node 0 for all pages.
 *
 * Uses the move_pages(2) syscall in query mode (nodes=NULL) directly to
 * avoid a libnuma dependency.
 *
 * Build: gcc -O2 -o ltram_migration_test ltram_migration_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <errno.h>

#define LTRAM_NODE  1
#define DRAM_NODE   0
#define PAGE_SIZE   4096
#define TEST_PAGES  32

static int query_nodes(void *base, int npages, int *out)
{
	void **pages = malloc(npages * sizeof(void *));
	int ret;

	if (!pages)
		return -ENOMEM;
	for (int i = 0; i < npages; i++)
		pages[i] = (char *)base + (size_t)i * PAGE_SIZE;

	ret = (int)syscall(SYS_move_pages, 0, (unsigned long)npages,
			   pages, NULL, out, 0);
	if (ret)
		ret = -errno;
	free(pages);
	return ret;
}

static void count_nodes(const int *nodes, int n, int *on_ltram, int *on_dram)
{
	*on_ltram = *on_dram = 0;
	for (int i = 0; i < n; i++) {
		if (nodes[i] == LTRAM_NODE)
			(*on_ltram)++;
		else if (nodes[i] == DRAM_NODE)
			(*on_dram)++;
	}
}

int main(void)
{
	char tmpfile[] = "/tmp/ltram_test_XXXXXX";
	char buf[PAGE_SIZE];
	const size_t len = (size_t)TEST_PAGES * PAGE_SIZE;
	int fd, ret, pass = 1;
	void *ptr;
	int *nodes;
	int on_ltram, on_dram;

	if (access("/sys/devices/system/node/node1", F_OK) != 0) {
		printf("SKIP: LTRAM node 1 not available\n");
		return 0;
	}

	nodes = malloc(TEST_PAGES * sizeof(int));
	if (!nodes) {
		perror("malloc");
		return 1;
	}

	fd = mkstemp(tmpfile);
	if (fd < 0) {
		perror("mkstemp");
		free(nodes);
		return 1;
	}
	unlink(tmpfile);

	/* Write distinct non-zero content so pages are never the zero-page. */
	memset(buf, 0xab, sizeof(buf));
	for (int i = 0; i < TEST_PAGES; i++) {
		if (write(fd, buf, PAGE_SIZE) != PAGE_SIZE) {
			perror("write");
			pass = 0;
			goto out_fd;
		}
	}

	/*
	 * Map file read-only with MAP_PRIVATE.  VMA has no VM_WRITE, so
	 * do_fault() will attempt ltram_migrate_to() on each read fault.
	 */
	ptr = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	if (ptr == MAP_FAILED) {
		perror("mmap");
		pass = 0;
		goto out_fd;
	}

	/* Phase 1: read-fault every page. */
	for (int i = 0; i < TEST_PAGES; i++)
		(void)((volatile char *)ptr)[i * PAGE_SIZE];

	ret = query_nodes(ptr, TEST_PAGES, nodes);
	if (ret) {
		fprintf(stderr, "move_pages query failed: %s\n", strerror(-ret));
		pass = 0;
		goto out_mmap;
	}
	count_nodes(nodes, TEST_PAGES, &on_ltram, &on_dram);
	printf("[read phase]  node 1 (LTRAM): %d/%d   node 0 (DRAM): %d/%d\n",
	       on_ltram, TEST_PAGES, on_dram, TEST_PAGES);
	if (on_ltram == 0) {
		fprintf(stderr, "FAIL: no pages migrated to LTRAM\n");
		pass = 0;
		goto out_mmap;
	}

	/*
	 * Phase 2: make the mapping writable, then write each page.
	 * Each write fault hits do_wp_page(), which sees folio_nid == 1,
	 * calls ltram_migrate_from(), and returns VM_FAULT_RETRY.
	 * The retry completes the COW write in DRAM.
	 */
	if (mprotect(ptr, len, PROT_READ | PROT_WRITE)) {
		perror("mprotect");
		pass = 0;
		goto out_mmap;
	}
	for (int i = 0; i < TEST_PAGES; i++)
		((volatile char *)ptr)[i * PAGE_SIZE] = 0xcd;

	ret = query_nodes(ptr, TEST_PAGES, nodes);
	if (ret) {
		fprintf(stderr, "move_pages query failed: %s\n", strerror(-ret));
		pass = 0;
		goto out_mmap;
	}
	count_nodes(nodes, TEST_PAGES, &on_ltram, &on_dram);
	printf("[write phase] node 1 (LTRAM): %d/%d   node 0 (DRAM): %d/%d\n",
	       on_ltram, TEST_PAGES, on_dram, TEST_PAGES);
	if (on_ltram > 0) {
		fprintf(stderr, "FAIL: %d pages still on LTRAM after write faults\n",
			on_ltram);
		pass = 0;
	}

out_mmap:
	munmap(ptr, len);
out_fd:
	close(fd);
	free(nodes);
	printf("%s\n", pass ? "PASS" : "FAIL");
	return pass ? 0 : 1;
}
