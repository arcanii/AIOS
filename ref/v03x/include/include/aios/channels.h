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
#define CH_FS           4   /* orchestrator  <-> fs_server      */
#define CH_VFS          5   /* orchestrator  <-> vfs_server     */
#define CH_UART_IRQ     2   /* serial_driver <- hardware IRQ    */
#define CH_FS_BLK_PPC  10  /* fs_server -> blk_driver (PPC)    */

/* Sandbox channels — one per slot */
#define CH_SANDBOX      7   /* orchestrator <-> sandbox         */

#define NUM_SANDBOXES   1

/* Network */
/* Auth server */
#define CH_AUTH        11   /* orchestrator <-> auth_server    */

#define CH_NET          8   /* net_driver <-> net_server        */
#define CH_NET_SRV     12   /* orchestrator <-> net_server      */
#define CH_NET_IRQ      9   /* net_driver <- hardware IRQ       */

#endif /* AIOS_CHANNELS_H */
