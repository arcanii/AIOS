/* Socket forwarding to net_server
 * Orchestrator acts as proxy between sandbox and net_server
 */

static int sock_cmd_sync(uint32_t cmd) {
    volatile uint32_t *base = (volatile uint32_t *)sock_data;
    base[0] = cmd;  /* SOCK_CMD */
    microkit_ppcall(CH_NET_SRV, microkit_msginfo_new(0, 0));
    return (int)base[1];  /* SOCK_STATUS */
}
