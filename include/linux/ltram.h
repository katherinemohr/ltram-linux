/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_LTRAM_H
#define _LINUX_LTRAM_H

#define LTRAM_NUMA_NODE 1

struct folio;

int ltram_migrate_to(struct folio *folio);
int ltram_migrate_from(struct folio *folio);

#endif /* _LINUX_LTRAM_H */
