#include "verify.h"

#include "shady/visit.h"

#include "free_frontier.h"
#include "cfg.h"
#include "../ir_private.h"
#include "../check.h"

#include "log.h"
#include "dict.h"
#include "list.h"

#include <assert.h>

typedef struct {
    Visitor visitor;
    const IrArena* arena;
    struct Dict* once;
} ArenaVerifyVisitor;

static void visit_verify_same_arena(ArenaVerifyVisitor* visitor, const Node* node) {
    assert(visitor->arena == node->arena);
    if (shd_dict_find_key(const Node*, visitor->once, node))
        return;
    shd_set_insert_get_result(const Node*, visitor->once, node);
    shd_visit_node_operands(&visitor->visitor, 0, node);
}

KeyHash shd_hash_node(const Node**);
bool shd_compare_node(const Node**, const Node**);

static void verify_same_arena(Module* mod) {
    const IrArena* arena = shd_module_get_arena(mod);
    ArenaVerifyVisitor visitor = {
        .visitor = {
            .visit_node_fn = (VisitNodeFn) visit_verify_same_arena,
        },
        .arena = arena,
        .once = shd_new_set(const Node*, (HashFn) shd_hash_node, (CmpFn) shd_compare_node)
    };
    shd_visit_module(&visitor.visitor, mod);
    shd_destroy_dict(visitor.once);
}

static void verify_scoping(const CompilerConfig* config, Module* mod) {
    struct List* cfgs = shd_build_cfgs(mod, structured_scope_cfg_build());
    for (size_t i = 0; i < shd_list_count(cfgs); i++) {
        CFG* cfg = shd_read_list(CFG*, cfgs)[i];
        Scheduler* scheduler = shd_new_scheduler(cfg);
        struct Dict* set = shd_free_frontier(scheduler, cfg, cfg->entry->node);
        if (shd_dict_count(set) > 0) {
            shd_log_fmt(ERROR, "Leaking variables in ");
            shd_log_node(ERROR, cfg->entry->node);
            shd_log_fmt(ERROR, ":\n");

            size_t j = 0;
            const Node* leaking;
            while (shd_dict_iter(set, &j, &leaking, NULL)) {
                shd_log_node(ERROR, leaking);
                shd_error_print("\n");
            }

            shd_log_fmt(ERROR, "Problematic module:\n");
            shd_log_module(ERROR, config, mod);
            shd_error_die();
        }
        shd_destroy_dict(set);
        shd_destroy_scheduler(scheduler);
        shd_destroy_cfg(cfg);
    }
    shd_destroy_list(cfgs);
}

static void verify_nominal_node(const Node* fn, const Node* n) {
    switch (n->tag) {
        case Function_TAG: {
            assert(!fn && "functions cannot be part of a CFG, except as the entry");
            break;
        }
        case BasicBlock_TAG: {
            assert(shd_is_subtype(noret_type(n->arena), n->payload.basic_block.body->type));
            break;
        }
        case NominalType_TAG: {
            assert(is_type(n->payload.nom_type.body));
            break;
        }
        case Constant_TAG: {
            if (n->payload.constant.value) {
                const Type* t = n->payload.constant.value->type;
                bool u = shd_deconstruct_qualified_type(&t);
                assert(u);
                assert(shd_is_subtype(n->payload.constant.type_hint, t));
            }
            break;
        }
        case GlobalVariable_TAG: {
            if (n->payload.global_variable.init) {
                const Type* t = n->payload.global_variable.init->type;
                bool u = shd_deconstruct_qualified_type(&t);
                assert(u);
                assert(shd_is_subtype(n->payload.global_variable.type, t));
            }
            break;
        }
        default: break;
    }
}

typedef struct ScheduleContext_ {
    Visitor visitor;
    struct Dict* bound;
    struct ScheduleContext_* parent;
    CompilerConfig* config;
    Module* mod;
} ScheduleContext;

static void verify_schedule_visitor(ScheduleContext* ctx, const Node* node) {
    if (is_instruction(node)) {
        ScheduleContext* search = ctx;
        while (search) {
            if (shd_dict_find_key(const Node*, search->bound, node))
                break;
            search = search->parent;
        }
        if (!search) {
            shd_log_fmt(ERROR, "Scheduling problem: ");
            shd_log_node(ERROR, node);
            shd_log_fmt(ERROR, "was encountered before we saw it be bound by a let!\n");
            shd_log_fmt(ERROR, "Problematic module:\n");
            shd_log_module(ERROR, ctx->config, ctx->mod);
            shd_error_die();
        }
    }
    shd_visit_node_operands(&ctx->visitor, NcTerminator | NcFunction, node);
}

static void verify_bodies(const CompilerConfig* config, Module* mod) {
    struct List* cfgs = shd_build_cfgs(mod, structured_scope_cfg_build());
    for (size_t i = 0; i < shd_list_count(cfgs); i++) {
        CFG* cfg = shd_read_list(CFG*, cfgs)[i];

        for (size_t j = 0; j < cfg->size; j++) {
            CFNode* n = cfg->rpo[j];
            if (n->node->tag == BasicBlock_TAG) {
                verify_nominal_node(cfg->entry->node, n->node);
            }
        }

        shd_destroy_cfg(cfg);
    }
    shd_destroy_list(cfgs);

    Nodes decls = shd_module_get_declarations(mod);
    for (size_t i = 0; i < decls.count; i++) {
        const Node* decl = decls.nodes[i];
        verify_nominal_node(NULL, decl);
    }
}

void shd_verify_module(const CompilerConfig* config, Module* mod) {
    verify_same_arena(mod);
    // before we normalize the IR, scopes are broken because decls appear where they should not
    // TODO add a normalized flag to the IR and check grammar is adhered to strictly
    if (shd_module_get_arena(mod)->config.check_types) {
        verify_scoping(config, mod);
        verify_bodies(config, mod);
    }
}
