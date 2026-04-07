/*
 * vka_audit.c -- VKA allocation counter implementation
 * v0.4.65: tracks per-subsystem resource consumption
 */
#include <stdio.h>
#include "aios/vka_audit.h"

vka_audit_entry_t vka_audit[VKA_SUB_COUNT];

const char *vka_sub_names[VKA_SUB_COUNT] = {
    "boot", "fork", "exec", "thread", "pipe", "other"
};

void vka_audit_frame(vka_subsystem_t sub, int pages) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].frames += (uint32_t)pages;
    vka_audit[sub].total_pages += (uint32_t)pages;
}

void vka_audit_endpoint(vka_subsystem_t sub) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].endpoints++;
}

void vka_audit_tcb(vka_subsystem_t sub) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].tcbs++;
}

void vka_audit_cslot(vka_subsystem_t sub) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].cslots++;
}

void vka_audit_untyped(vka_subsystem_t sub, int size_bits) {
    if (sub >= VKA_SUB_COUNT) sub = VKA_SUB_OTHER;
    vka_audit[sub].untypeds++;
    vka_audit[sub].total_pages += (uint32_t)(1 << (size_bits - 12));
}

void vka_audit_dump(void) {
    printf("[VKA-AUDIT] Per-subsystem allocation summary:\n");
    printf("  %-8s %6s %5s %4s %6s %6s %8s\n",
           "subsys", "frames", "eps", "tcbs", "cslots", "untypd", "tot_pg");
    uint32_t grand = 0;
    for (int i = 0; i < VKA_SUB_COUNT; i++) {
        vka_audit_entry_t *e = &vka_audit[i];
        if (e->frames || e->endpoints || e->tcbs ||
            e->cslots || e->untypeds || e->total_pages) {
            printf("  %-8s %6u %5u %4u %6u %6u %8u\n",
                   vka_sub_names[i], e->frames, e->endpoints,
                   e->tcbs, e->cslots, e->untypeds, e->total_pages);
            grand += e->total_pages;
        }
    }
    printf("  %-8s %6s %5s %4s %6s %6s %8u\n",
           "TOTAL", "", "", "", "", "", grand);
    printf("  pool = 4000 pages, remaining ~ %u pages\n", 4000 - grand);
}
