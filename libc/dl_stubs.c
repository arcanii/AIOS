/* dl_stubs.c
 * Dynamic loading stubs for AIOS libc.
 * Dynamic loading is not applicable in a seL4 microkernel model.
 * These stubs allow programs referencing dl symbols to link.
 */

#include <stddef.h>
#include <errno.h>

static char dl_err[] = "dynamic loading not supported";

void *dlopen(const char *filename, int flags)
{ (void)filename; (void)flags; return NULL; }

void *dlsym(void *handle, const char *symbol)
{ (void)handle; (void)symbol; return NULL; }

int dlclose(void *handle)
{ (void)handle; return 0; }

char *dlerror(void)
{ return dl_err; }
