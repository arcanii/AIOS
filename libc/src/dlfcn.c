/* dlfcn.c - AIOS Dynamic Loading stubs
 *
 * No shared library support yet. All functions return errors.
 * dlerror() returns a static message.
 * TODO: Could be implemented via sandbox ELF loader.
 */

#include <dlfcn.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

static const char *_dl_error_msg = "dynamic loading not supported";
static int _dl_error_set = 0;

void *dlopen(const char *filename, int flags)
{
    (void)filename; (void)flags;
    _dl_error_set = 1;
    return NULL;
}

void *dlsym(void *handle, const char *symbol)
{
    (void)handle; (void)symbol;
    _dl_error_set = 1;
    return NULL;
}

int dlclose(void *handle)
{
    (void)handle;
    _dl_error_set = 1;
    return -1;
}

char *dlerror(void)
{
    if (_dl_error_set) {
        _dl_error_set = 0;
        return (char *)_dl_error_msg;
    }
    return NULL;
}
