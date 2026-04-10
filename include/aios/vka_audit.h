#ifndef AIOS_VKA_AUDIT_H
#define AIOS_VKA_AUDIT_H
/*
 * vka_audit.h -- Lightweight VKA allocation counter
 * v0.4.65: instrument untyped consumption across subsystems
 */
#include <stdint.h>

typedef enum {
    VKA_SUB_BOOT = 0,
    VKA_SUB_FORK,
    VKA_SUB_EXEC,
    VKA_SUB_THREAD,
    VKA_SUB_PIPE,
    VKA_SUB_NET,
    VKA_SUB_OTHER,
    VKA_SUB_COUNT
} vka_subsystem_t;

typedef struct {
    uint32_t frames;
    uint32_t endpoints;
    uint32_t tcbs;
    uint32_t cslots;
    uint32_t untypeds;
    uint32_t total_pages;
} vka_audit_entry_t;

extern vka_audit_entry_t vka_audit[VKA_SUB_COUNT];
extern const char *vka_sub_names[VKA_SUB_COUNT];

void vka_audit_frame(vka_subsystem_t sub, int pages);
void vka_audit_endpoint(vka_subsystem_t sub);
void vka_audit_tcb(vka_subsystem_t sub);
void vka_audit_cslot(vka_subsystem_t sub);
void vka_audit_untyped(vka_subsystem_t sub, int size_bits);
void vka_audit_dump(void);

#endif
