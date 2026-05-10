void __init ltram_memblock_init(void)
{
    struct memblock_region *reg;
    phys_addr_t base = 0, size = 0;

#ifdef CONFIG_LTRAM_EMULATE
    /*
     * QEMU has already set up node 1 via -numa params.
     * Find it in memblock by node ID rather than
     * hardcoding a physical address — QEMU doesn't
     * guarantee which physical address node 1 lands at.
     *
     * TODO: replace with ltram_get_range_from_dt() once
     * the FPGA PCIe driver is ready.
     */
    for_each_mem_region(reg) {
        if (memblock_get_region_node(reg) == 1) {
            base = reg->base;
            size = reg->size;
            break;
        }
    }

    if (!size) {
        pr_warn("ltram: node 1 not found, is NUMA enabled?\n");
        return;
    }

    WARN_ON(size != SZ_256M); /* sanity check vs QEMU params */

#else
    /* TODO: real path — read BAR address from FPGA DT node */
    ltram_get_range_from_dt(&base, &size);
#endif

    /*
     * Reserve the region so memblock doesn't hand it to
     * early boot allocations before ZONE_LTRAM is ready.
     */
    memblock_reserve(base, size);

    ltram_base_pfn = PFN_DOWN(base);
    ltram_end_pfn  = PFN_UP(base + size);

    pr_info("ltram: %lu MiB at PFN [%lu, %lu)\n",
            size >> 20, ltram_base_pfn, ltram_end_pfn);
}
