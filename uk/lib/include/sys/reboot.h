/* sys/reboot.h -- AIOS shadow header. reboot() asks the AIOS KERNEL to bring the AIOS system down (a
 * root-only operation); on the appliance the host is powered off in turn. The RB_* command names match
 * Linux's so `poweroff`/`halt`/`reboot` read like ordinary code; the values are AIOS-owned. */
#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H
#include "aios_abi.h"

#define RB_POWER_OFF   AIOS_RB_POWEROFF
#define RB_HALT_SYSTEM AIOS_RB_HALT
#define RB_AUTOBOOT    AIOS_RB_REBOOT

int reboot(int cmd);

#endif /* _SYS_REBOOT_H */
