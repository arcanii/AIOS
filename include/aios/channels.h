/*
 * AIOS – Channel IDs
 *
 * Every PD-to-PD channel in hello.system must use these IDs.
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

#endif /* AIOS_CHANNELS_H */

#define CH_SANDBOX      7   /* orchestrator  <-> sandbox        */
