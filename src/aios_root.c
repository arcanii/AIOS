/*
 * AIOS 0.4.x Root Task
 *
 * Phase 1: Boot on bare seL4, enumerate resources, print diagnostics.
 * This replaces Microkit's capDL loader + monitor.
 */
#include <stdio.h>
#include <sel4/sel4.h>
#include <sel4platsupport/bootinfo.h>
#include <allocman/bootstrap.h>
#include <allocman/vka.h>
#include <sel4utils/vspace.h>
#include <simple/simple.h>
#include <simple-default/simple-default.h>
#include <utils/util.h>

/* Static memory pools for bootstrap */
#define ALLOCATOR_STATIC_POOL_SIZE (BIT(seL4_PageBits) * 20)
static char allocator_mem_pool[ALLOCATOR_STATIC_POOL_SIZE];
static sel4utils_alloc_data_t vspace_data;

/* Global state */
static simple_t simple;
static vka_t vka;
static vspace_t vspace;
static allocman_t *allocman;

int main(int argc, char *argv[]) {
    printf("\n");
    printf("============================================\n");
    printf("  AIOS 0.4.0 Root Task\n");
    printf("  seL4 direct mode (no Microkit)\n");
    printf("============================================\n\n");

    /* Step 1: Get boot info from kernel */
    seL4_BootInfo *info = platsupport_get_bootinfo();
    if (!info) {
        printf("FATAL: Failed to get bootinfo\n");
        return -1;
    }
    printf("[boot] BootInfo received\n");
    printf("[boot] Initial CNode size: %zu slots\n",
           (size_t)BIT(info->initThreadCNodeSizeBits));

    /* Step 2: Initialize simple interface */
    simple_default_init_bootinfo(&simple, info);
    printf("[boot] Simple interface initialized\n");

    /* Step 3: Bootstrap allocator */
    allocman = bootstrap_use_current_simple(
        &simple, ALLOCATOR_STATIC_POOL_SIZE, allocator_mem_pool);
    if (!allocman) {
        printf("FATAL: Failed to init allocman\n");
        return -1;
    }
    printf("[boot] Allocman bootstrapped\n");

    /* Step 4: Create VKA interface */
    allocman_make_vka(&vka, allocman);
    printf("[boot] VKA interface ready\n");

    /* Step 5: Bootstrap VSpace */
    int error = sel4utils_bootstrap_vspace_with_bootinfo_leaky(
        &vspace, &vspace_data,
        simple_get_pd(&simple), &vka, info);
    if (error) {
        printf("FATAL: Failed to bootstrap vspace: %d\n", error);
        return -1;
    }
    printf("[boot] VSpace bootstrapped\n");

    /* Step 6: Enumerate resources */
    printf("\n[info] Platform resources:\n");
    printf("  Untypeds: %d\n",
           (int)(info->untyped.end - info->untyped.start));

    int total_mem = 0;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (!ut->isDevice) {
            total_mem += BIT(ut->sizeBits);
        }
    }
    printf("  Available RAM: %d MB\n", total_mem / (1024 * 1024));
    printf("  Cores: %d\n", (int)info->numNodes);
    printf("  Device untypeds: ");
    int dev_count = 0;
    for (seL4_Word i = info->untyped.start; i < info->untyped.end; i++) {
        seL4_UntypedDesc *ut = &info->untypedList[i - info->untyped.start];
        if (ut->isDevice) dev_count++;
    }
    printf("%d\n", dev_count);

    printf("\n============================================\n");
    printf("  AIOS root task running on bare seL4\n");
    printf("  Phase 1 complete — all resources enumerated\n");
    printf("============================================\n\n");

    /* Phase 2 will: create endpoints, spawn servers, load shell */
    printf("[root] Entering idle loop (Phase 2: TODO)\n");
    while (1) {
        seL4_Yield();
    }

    return 0;
}
