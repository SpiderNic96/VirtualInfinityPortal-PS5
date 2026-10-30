#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
int mdbg_copyout(pid_t pid, intptr_t addr, void *buf, size_t len);
