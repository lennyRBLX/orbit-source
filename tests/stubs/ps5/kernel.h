#ifndef ORBIT_TEST_PS5_KERNEL_H
#define ORBIT_TEST_PS5_KERNEL_H
#include <stdint.h>
#include <sys/types.h>
int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
#endif
