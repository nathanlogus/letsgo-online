#pragma once
#include "cpu.h"
typedef int (*RuntimeFnHook)(Cpu *,uint64_t);
// Install a wrapper while retaining the existing handler. A wrapper calls the
// previous handler, then the original only if that handler returns zero.
// Registering the same wrapper again preserves its predecessor.
void rt_register_fnhook_chain(uint64_t addr,RuntimeFnHook wrapper,RuntimeFnHook *previous);
