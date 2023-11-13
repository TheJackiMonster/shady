#ifndef SHADY_COMPILE_H
#define SHADY_COMPILE_H

#include "shady/ir.h"
#include "passes/passes.h"
#include "log.h"
#include "analysis/verify.h"

#ifdef NDEBUG
#define SHADY_RUN_VERIFY 0
#else
#define SHADY_RUN_VERIFY 1
#endif

#define RUN_PASS(pass_name) {                           \
old_mod = *pmod;                                        \
*pmod = pass_name(config, *pmod);                       \
debugvv_print("After "#pass_name" pass: \n");           \
log_module(DEBUGVV, config, *pmod);                     \
if (SHADY_RUN_VERIFY)                                   \
  verify_module(*pmod);                                 \
if (config->hooks.after_pass.fn)                                 \
  config->hooks.after_pass.fn(config->hooks.after_pass.uptr, #pass_name, *pmod); \
(*pmod)->sealed = true;                                 \
if (get_module_arena(old_mod) != get_module_arena(*pmod) && get_module_arena(old_mod) != initial_arena) \
  destroy_ir_arena(get_module_arena(old_mod));          \
}

#endif
