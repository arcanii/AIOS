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
    VKA_SUB_GPU,
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

/* Live frame counter: incremented on alloc, decremented on free */
extern int vka_live_frames;
extern int vka_peak_frames;
void vka_audit_frame_alloc(void);
void vka_audit_frame_free(void);

/* v0.4.103: pool pressure helpers
 *   vka_audit_frame_release(N): decrement live count by N (used in reap)
 *   vka_audit_check_headroom(N): returns -1 if < N free pages, else 0
 */
void vka_audit_frame_release(int pages);
int  vka_audit_check_headroom(int needed_pages);

/* v0.4.109: per-process page release helper. Pass an active_proc_t*
 * (forward-declared as void here to avoid including root_shared.h).
 * Decrements vka_live_frames by the per-process audit count and zeros
 * it. Called immediately before sel4utils_destroy_process. */
struct active_proc;
void vka_audit_release_proc_pages(int *audit_pages_allocated);

#endif
