#include "parser.h"

#include "shady/pass.h"

#include "../shady/transform/internal_constants.h"
#include "../shady/passes/passes.h"

#include "log.h"

/// Removes all Unresolved nodes and replaces them with the appropriate decl/value
RewritePass slim_pass_bind;
/// Enforces the grammar, notably by let-binding any intermediary result
RewritePass slim_pass_normalize;
/// Makes sure every node is well-typed
RewritePass slim_pass_infer;

void slim_parse_string(const SlimParserConfig* config, const char* contents, Module* mod);

Module* shd_parse_slim_module(const CompilerConfig* config, const SlimParserConfig* pconfig, const char* contents, String name) {
    ArenaConfig aconfig = shd_default_arena_config(&config->target);
    aconfig.name_bound = false;
    aconfig.check_op_classes = false;
    aconfig.check_types = false;
    aconfig.validate_builtin_types = false;
    aconfig.allow_fold = false;
    IrArena* initial_arena = shd_new_ir_arena(&aconfig);
    Module* m = shd_new_module(initial_arena, name);
    slim_parse_string(pconfig, contents, m);
    Module** pmod = &m;

    shd_debugv_print("Parsed slim module:\n");
    shd_log_module(DEBUGV, config, *pmod);

    shd_generate_dummy_constants(config, *pmod);

    RUN_PASS(slim_pass_bind, config)
    RUN_PASS(slim_pass_normalize, config)

    RUN_PASS(shd_pass_normalize_builtins, config)
    RUN_PASS(slim_pass_infer, config)
    RUN_PASS(shd_pass_lower_cf_instrs, config)

    return *pmod;
}
