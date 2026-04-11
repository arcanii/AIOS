#ifndef AIOS_X86_64_PAGE_H
#define AIOS_X86_64_PAGE_H

#include <sel4/sel4.h>

/* seL4 x86 page object type for 4K pages */
#define ARCH_PAGE_OBJECT  seL4_X86_4K

/* Get physical address of a mapped page capability */
static inline int arch_page_get_address(seL4_CPtr cap, uint64_t *paddr) {
    seL4_X86_Page_GetAddress_t ga = seL4_X86_Page_GetAddress(cap);
    if (ga.error) return -1;
    *paddr = ga.paddr;
    return 0;
}

#endif /* AIOS_X86_64_PAGE_H */
