#ifndef AIOS_NETD_SKEL_H
#define AIOS_NETD_SKEL_H
/*
 * netd_skel.h -- Stage-2 netd SKELETON IPC protocol (netd <-> root spawn path).
 *
 * DESIGN_NETD Stage 2 proves the MECHANISM an isolated leaf driver-process
 * needs before any net code is trusted to it: spawn, argv-cap parse, ntfn
 * self-bind, a ctrl/fault endpoint, deferred replies via child-cnode
 * SaveCaller, and -- the one kernel-semantics bet -- the root-side reply-sweep
 * (CNode_Move a saved reply cap out of netd's cnode and Send it). This is a
 * THROWAWAY selftest surface; Stage 3 replaces it with the real NET_* path.
 *
 * Kept in ONE header so src/apps/netd.c (the CPIO app) and src/boot/spawn_netd.c
 * (root) can never skew on the wire.
 */

/* ---- test/main EP: client/root -> netd ----
 * NETD_PING=5 matches serverstats' SVC_PING; the rest sit ABOVE the NET_*
 * request range (90-103, net_proto.h) and the display range (110+,
 * root_shared.h) so a skeleton op can never be confused with a real one. */
#define NETD_PING          5     /* liveness probe; reply MR0=0 */
#define NETD_BLOCK_KICK    200   /* park caller; netd self-wakes it on a badge-2 kick */
#define NETD_BLOCK_SWEEP   201   /* park caller; root reply-sweep wakes it (the bet) */
#define NETD_CRASH         202   /* null deref -> root fault listener (containment) */

/* ---- ctrl/fault EP: netd -> root listener ----
 * All strictly above the aarch64 seL4 fault-label range (<=6) so the listener
 * discriminates a netd status message from a kernel-delivered fault by label. */
#define DEVD_READY         100   /* device/skeleton init done, ready to serve */
#define DEVD_FAIL          101   /* fatal init failure; errcode in MR0 */
#define NETD_PARKED        110   /* a caller was parked; MR0=mode, MR1=netd-cnode slot */

/* NETD_PARKED modes (MR0) */
#define NETD_PARK_KICK     1     /* netd self-wakes via its bound ntfn (badge 2) */
#define NETD_PARK_SWEEP    2     /* root must CNode_Move the saved reply out + Send it */

/* bound-ntfn badge, root -> netd (drives the in-netd self-wake) */
#define NETD_KICK_BADGE    2

/* selftest reply tokens, so the parked caller proves WHICH path woke it */
#define NETD_KICK_TOKEN    0x600dUL   /* delivered by netd's own Send (in-netd wake) */
#define NETD_SWEEP_TOKEN   0xd00dUL   /* delivered by root's reply-sweep (the bet) */

/* SaveCaller slots netd carves out of its own cnode AFTER the cap donations
 * (DESIGN_NETD s3 row 6): [base+0]=KICK park, [base+1]=SWEEP park. The base is
 * passed to netd as argv[3]; root reserves it by bumping cspace_next_free. */
#define NETD_RESERVED_SLOTS 4

#endif /* AIOS_NETD_SKEL_H */
