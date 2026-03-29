/*
 * Open Aries – Channel IDs
 *
 * Every PD-to-PD channel in aios.system must use these IDs.
 * Both endpoints of a channel share the same numeric ID.
 */
#ifndef AIOS_CHANNELS_H
#define AIOS_CHANNELS_H

#define CH_SERIAL       1   /* orchestrator  <-> serial_driver  */
#define CH_BLK          2   /* orchestrator  <-> blk_driver     */
#define CH_ECHO         3   /* orchestrator  <-> echo_server    */
#define CH_FS           4   /* orchestrator  <-> fs_server      */
#define CH_FS_BLK       5   /* fs_server     <-> blk_driver     */
#define CH_LLM          6   /* orchestrator  <-> llm_server     */
#define CH_UART_IRQ     2   /* serial_driver <- hardware IRQ    */
#define CH_FS_BLK_PPC  10  /* fs_server -> blk_driver (PPC)    */

/* Sandbox channels — one per slot */
#define CH_SBX0         7   /* orchestrator <-> sbx0            */
#define CH_SBX1         8   /* orchestrator <-> sbx1            */
#define CH_SBX2         9   /* orchestrator <-> sbx2            */
#define CH_SBX3        10   /* orchestrator <-> sbx3            */

#define NUM_SANDBOXES   8
#define CH_SBX4        14   /* orchestrator <-> sbx4            */
#define CH_SBX5        15   /* orchestrator <-> sbx5            */
#define CH_SBX6        16   /* orchestrator <-> sbx6            */
#define CH_SBX7        17   /* orchestrator <-> sbx7            */
#define CH_SBX_BASE     CH_SBX0  /* first sandbox channel ID   */

/* Network */
/* Auth server */
#define CH_AUTH        11   /* orchestrator <-> auth_server    */

#define CH_NET          8   /* net_driver <-> net_server        */
#define CH_NET_IRQ      9   /* net_driver <- hardware IRQ       */

#endif /* AIOS_CHANNELS_H */
