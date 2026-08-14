#ifndef NGCD_TARGET_DLFCN_H
#define NGCD_TARGET_DLFCN_H

#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100

extern void *dlopen(const char *path, int mode);
extern int dlclose(void *handle);
extern void *dlsym(void *handle, const char *name);
extern char *dlerror(void);

#endif
