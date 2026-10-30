/* host stub: "game memory" is this process's memory */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
int kernel_proc_copyout(pid_t pid, intptr_t addr, void *buf, size_t len);
int kernel_proc_copyin(pid_t pid, const void *buf, intptr_t addr, size_t len);
int kernel_copyout(intptr_t kaddr, void *udaddr, size_t len);
int kernel_dynlib_handle(pid_t pid, const char *basename, uint32_t *handle);
intptr_t kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *nid);
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, uint32_t handle);
