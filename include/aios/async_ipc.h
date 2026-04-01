#ifndef AIOS_ASYNC_IPC_H
#define AIOS_ASYNC_IPC_H

/* AIOS Async IPC Framework
 *
 * Allows the sandbox kernel to issue non-blocking syscalls so other
 * threads can run while one thread waits for I/O.
 *
 * Protocol:
 *   1. Sandbox writes request to async_slot in sandbox_io
 *   2. Sandbox sends microkit_notify(CH_ORCH) instead of ppcall
 *   3. Sandbox marks calling thread TH_BLOCKED_IO, yields
 *   4. Orchestrator notified() reads request, processes, writes result
 *   5. Orchestrator sends microkit_notify(CH_SANDBOX)
 *   6. Sandbox notified() reads result, unblocks thread
 *
 * Layout in sandbox_io (4KB page):
 *   0x000 - 0x0FF: control area (SBX_CMD, SBX_STATUS, etc. - existing)
 *   0x100 - 0x1FF: async request/response header
 *   0x200 - 0xFFF: data area (existing, shared with sync path)
 *
 * Request header (offset 0x100):
 *   +0x00: req_id        (uint32_t) unique request ID
 *   +0x04: req_syscall    (uint32_t) SYS_* number
 *   +0x08: req_thread_id  (uint32_t) sandbox thread that issued it
 *   +0x0C: req_arg1       (uint64_t) syscall arg 1
 *   +0x14: req_arg2       (uint64_t) syscall arg 2
 *   +0x1C: req_arg3       (uint64_t) syscall arg 3
 *   +0x24: req_state      (uint32_t) 0=empty, 1=pending, 2=complete
 *   +0x28: resp_result    (int64_t)  result from orchestrator
 *   +0x30: resp_arg1      (uint64_t) extra return value 1
 *   +0x38: resp_arg2      (uint64_t) extra return value 2
 *
 * Limitation: one in-flight async request at a time (single slot).
 * Future: ring buffer for multiple concurrent requests.
 */

#include <stdint.h>

/* Async request area offsets within sandbox_io */
#define ASYNC_BASE      0x100
#define ASYNC_REQ_ID    (ASYNC_BASE + 0x00)
#define ASYNC_SYSCALL   (ASYNC_BASE + 0x04)
#define ASYNC_THREAD_ID (ASYNC_BASE + 0x08)
#define ASYNC_ARG1      (ASYNC_BASE + 0x0C)
#define ASYNC_ARG2      (ASYNC_BASE + 0x14)
#define ASYNC_ARG3      (ASYNC_BASE + 0x1C)
#define ASYNC_STATE     (ASYNC_BASE + 0x24)
#define ASYNC_RESULT    (ASYNC_BASE + 0x28)
#define ASYNC_RESP1     (ASYNC_BASE + 0x30)
#define ASYNC_RESP2     (ASYNC_BASE + 0x38)

/* Async state values */
#define ASYNC_EMPTY     0
#define ASYNC_PENDING   1
#define ASYNC_COMPLETE  2

/* Thread state for async I/O blocked */
#define TH_BLOCKED_IO   7

#endif /* AIOS_ASYNC_IPC_H */
