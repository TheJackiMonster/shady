#include "passes.h"

#include "list.h"
#include "log.h"
#include "portability.h"

#include "../ir_private.h"
#include "../rewrite.h"

#include <assert.h>
#include <string.h>

typedef struct NamedBindEntry_ NamedBindEntry;
struct NamedBindEntry_ {
    const char* name;
    bool is_var;
    Node* node;
    NamedBindEntry* next;
};

typedef struct {
    Rewriter rewriter;

    const Node* current_function;
    NamedBindEntry* local_variables;
} Context;

typedef struct {
    bool is_var;
    const Node* node;
} Resolved;

static Resolved resolve_using_name(Context* ctx, const char* name) {
    for (NamedBindEntry* entry = ctx->local_variables; entry != NULL; entry = entry->next) {
        if (strcmp(entry->name, name) == 0) {
            return (Resolved) {
                .is_var = entry->is_var,
                .node = entry->node
            };
        }
    }

    Nodes new_decls = get_module_declarations(ctx->rewriter.dst_module);
    for (size_t i = 0; i < new_decls.count; i++) {
        const Node* decl = new_decls.nodes[i];
        if (strcmp(get_decl_name(decl), name) == 0) {
            return (Resolved) {
                .is_var = decl->tag == GlobalVariable_TAG,
                .node = decl
            };
        }
    }

    Nodes old_decls = get_module_declarations(ctx->rewriter.src_module);
    for (size_t i = 0; i < old_decls.count; i++) {
        const Node* old_decl = old_decls.nodes[i];
        if (strcmp(get_decl_name(old_decl), name) == 0) {
            Context top_ctx = *ctx;
            top_ctx.current_function = NULL;
            top_ctx.local_variables = NULL;
            const Node* decl = rewrite_node(&top_ctx.rewriter, old_decl);
            return (Resolved) {
                .is_var = decl->tag == GlobalVariable_TAG,
                .node = decl
            };
        }
    }

    error("could not resolve node %s", name)
}

static void add_binding(Context* ctx, bool is_var, String name, const Node* node) {
    NamedBindEntry* entry = arena_alloc(ctx->rewriter.dst_arena->arena, sizeof(NamedBindEntry));
    *entry = (NamedBindEntry) {
        .name = string(ctx->rewriter.dst_arena, name),
        .is_var = is_var,
        .node = (Node*) node,
        .next = NULL
    };
    entry->next = ctx->local_variables;
    ctx->local_variables = entry;
}

static const Node* get_node_address(Context* ctx, const Node* node) {
    IrArena* dst_arena = ctx->rewriter.dst_arena;
    switch (node->tag) {
        case Unbound_TAG: {
            Resolved entry = resolve_using_name(ctx, node->payload.unbound.name);
            assert(entry.is_var && "Cannot take the address");
            return entry.node;
        }
        case PrimOp_TAG: {
            if (node->tag == PrimOp_TAG && node->payload.prim_op.op == subscript_op) {
                const Node* src_ptr = get_node_address(ctx, node->payload.prim_op.operands.nodes[0]);
                const Node* index = rewrite_node(&ctx->rewriter, node->payload.prim_op.operands.nodes[1]);
                return prim_op(dst_arena, (PrimOp) {
                    .op = lea_op,
                    .operands = nodes(dst_arena, 3, (const Node* []) { src_ptr, NULL, index })
                });
            }
        }
        default: break;
    }
    error("This doesn't really look like a place expression...")
}

/*static Node* rewrite_lambda_head(Context* ctx, const Node* node) {
    assert(node != NULL && node->tag == Lambda_TAG);
    IrArena* dst_arena = ctx->rewriter.dst_arena;

    // rebuild the parameters and shove them in the list
    size_t params_count = node->payload.lam.params.count;
    LARRAY(const Node*, nparams, params_count);
    for (size_t i = 0; i < params_count; i++) {
        const Variable* old_param = &node->payload.lam.params.nodes[i]->payload.var;
        const Node* new_param = var(dst_arena, rewrite_node(&ctx->rewriter, old_param->type), string(dst_arena, old_param->name));

        nparams[i] = new_param;
    }

    switch (node->payload.lam.tier) {
        case FnTier_Lambda:
            return lambda(dst_arena, nodes(dst_arena, params_count, nparams));
        case FnTier_BasicBlock:
            return basic_block(dst_arena, nodes(dst_arena, params_count, nparams), string(dst_arena, node->payload.lam.name));
        case FnTier_Function:
            return function(ctx->rewriter.dst_module, nodes(dst_arena, params_count, nparams), string(dst_arena, node->payload.lam.name), rewrite_nodes(&ctx->rewriter, node->payload.lam.annotations), rewrite_nodes(&ctx->rewriter, node->payload.lam.return_types));
    }
    SHADY_UNREACHABLE;
}

static void rewrite_lambda_body(Context* ctx, const Node* node, Node* target) {
    assert(node != NULL && node->tag == Lambda_TAG);
    IrArena* dst_arena = ctx->rewriter.dst_arena;

    Context body_infer_ctx = *ctx;
    // bind the rebuilt parameters for rewriting the body
    for (size_t i = 0; i < node->payload.lam.params.count; i++) {
        const Node* param = target->payload.lam.params.nodes[i];

        NamedBindEntry* entry = arena_alloc(ctx->rewriter.dst_arena->arena, sizeof(NamedBindEntry));
        *entry = (NamedBindEntry) {
            .name = string(dst_arena, param->payload.var.name),
            .is_var = false,
            .node = (Node*) param,
            .next = NULL
        };

        entry->node = (Node*) param;

        add_binding(&body_infer_ctx, entry);
        debug_print("Bound param %s\n", entry->name);
    }


}
*/

static const Node* desugar_let_mut(Context* ctx, const Node* node) {
    assert(node->tag == LetMut_TAG);
    IrArena* dst_arena = ctx->rewriter.dst_arena;
    Context body_infer_ctx = *ctx;
    const Node* ninstruction = rewrite_node(&ctx->rewriter, node->payload.let.instruction);

    const Node* old_lam = node->payload.let.tail;
    assert(old_lam && is_anonymous_lambda(old_lam));

    BodyBuilder* bb = begin_body(dst_arena);

    Nodes initial_values = bind_instruction_extra(bb, ninstruction, old_lam->payload.anon_lam.params.count, NULL, NULL);
    Nodes old_params = old_lam->payload.anon_lam.params;
    for (size_t i = 0; i < old_params.count; i++) {
        const Node* oparam = old_params.nodes[i];
        const Type* type_annotation = oparam->payload.var.type;
        assert(type_annotation);
        const Node* alloca = prim_op(dst_arena, (PrimOp) {
            .op = alloca_op,
            .type_arguments = nodes(dst_arena, 1, (const Node* []){ rewrite_node(&ctx->rewriter, type_annotation) }),
            .operands = nodes(dst_arena, 0, NULL)
        });
        const Node* ptr = bind_instruction_extra(bb, alloca, 1, NULL, &oparam->payload.var.name).nodes[0];
        const Node* store = prim_op(dst_arena, (PrimOp) {
            .op = store_op,
            .operands = nodes(dst_arena, 2, (const Node* []) { ptr, initial_values.nodes[0] })
        });
        bind_instruction_extra(bb, store, 0, NULL, NULL);

        add_binding(&body_infer_ctx, true, oparam->payload.var.name, ptr);
        debug_print("Lowered mutable variable %s\n", oparam->payload.var.name);
    }

    const Node* terminator = rewrite_node(&body_infer_ctx.rewriter, old_lam->payload.anon_lam.body);
    return finish_body(bb, terminator);
}

static const Node* rewrite_decl(Context* ctx, const Node* decl) {
    assert(is_declaration(decl));
    Node* bound = NULL;

    switch (decl->tag) {
        case GlobalVariable_TAG: {
            const GlobalVariable* ogvar = &decl->payload.global_variable;
            bound = global_var(ctx->rewriter.dst_module, rewrite_nodes(&ctx->rewriter, ogvar->annotations), rewrite_node(&ctx->rewriter, ogvar->type), ogvar->name, ogvar->address_space);
            bound->payload.global_variable.init = rewrite_node(&ctx->rewriter, decl->payload.global_variable.init);
            break;
        }
        case Constant_TAG: {
            const Constant* cnst = &decl->payload.constant;
            Node* new_constant = constant(ctx->rewriter.dst_module, rewrite_nodes(&ctx->rewriter, cnst->annotations), cnst->name);
            new_constant->payload.constant.type_hint = rewrite_node(&ctx->rewriter, decl->payload.constant.type_hint);
            bound = new_constant;
            bound->payload.constant.value = rewrite_node(&ctx->rewriter, decl->payload.constant.value);
            break;
        }
        case Function_TAG: {
            Nodes new_fn_params = recreate_variables(&ctx->rewriter, decl->payload.fun.params);
            bound = function(ctx->rewriter.dst_module, new_fn_params, decl->payload.fun.name, rewrite_nodes(&ctx->rewriter, decl->payload.fun.annotations), rewrite_nodes(&ctx->rewriter, decl->payload.fun.return_types));
            Context fn_ctx = *ctx;
            for (size_t i = 0; i < new_fn_params.count; i++)
                add_binding(&fn_ctx, false, decl->payload.fun.params.nodes[i]->payload.var.name, new_fn_params.nodes[i]);

            fn_ctx.current_function = bound;

            // handle basic blocks if we are a function
            Nodes unbound_blocks = decl->payload.fun.children_blocks;
            LARRAY(Node*, new_bbs, unbound_blocks.count);

            // First create stubs and inline that crap
            for (size_t i = 0; i < unbound_blocks.count; i++) {
                const Node* old_bb = unbound_blocks.nodes[i];
                assert(is_basic_block(old_bb));
                Nodes new_bb_params = recreate_variables(&ctx->rewriter, old_bb->payload.basic_block.params);
                Node* new_bb = basic_block(ctx->rewriter.dst_arena, bound, new_bb_params, old_bb->payload.basic_block.name);
                new_bbs[i] = new_bb;
                add_binding(&fn_ctx, false, old_bb->payload.basic_block.name, new_bb);
                debug_print("Bound (stub) basic block %s\n", old_bb->payload.basic_block.name);

                for (size_t j = 0; j < new_bb_params.count; j++)
                    add_binding(&fn_ctx, false, decl->payload.fun.params.nodes[j]->payload.var.name, new_fn_params.nodes[j]);
            }

            bound->payload.fun.body = rewrite_node(&fn_ctx.rewriter, decl->payload.fun.body);

            // Rebuild the basic blocks now
            for (size_t i = 0; i < unbound_blocks.count; i++) {
                const Node* old_bb = unbound_blocks.nodes[i];
                Node* new_bb = new_bbs[i];
                new_bb->payload.basic_block.body = rewrite_node(&fn_ctx.rewriter, old_bb->payload.basic_block.body);
                debug_print("Bound basic block %s\n", new_bb->payload.basic_block.name);
            }
            break;
        }
        default: error("unknown declaration kind");
    }

    return bound;
}

static const Node* bind_node(Context* ctx, const Node* node) {
    if (node == NULL)
        return NULL;

    const Node* found = search_processed(&ctx->rewriter, node);
    if (found) return found;

    IrArena* dst_arena = ctx->rewriter.dst_arena;
    switch (node->tag) {
        case Function_TAG:
        case Constant_TAG:
        case GlobalVariable_TAG: {
            assert(is_declaration(node));
            return rewrite_decl(ctx, node);
        }
        case Variable_TAG: error("the binders should be handled such that this node is never reached");
        case Unbound_TAG: {
            Resolved entry = resolve_using_name(ctx, node->payload.unbound.name);
            if (entry.is_var) {
                return prim_op(dst_arena, (PrimOp) {
                    .op = load_op,
                    .operands = nodes(dst_arena, 1, (const Node* []) { get_node_address(ctx, node) })
                });
            } else {
                return entry.node;
            }
        }
        case BasicBlock_TAG: error("rewrite_decl should handle this")
        case AnonLambda_TAG: {
            Nodes old_params = node->payload.anon_lam.params;
            Nodes new_params = recreate_variables(&ctx->rewriter, old_params);
            Node* new_lam = lambda(dst_arena, new_params);
            Context lambda_ctx = *ctx;
            for (size_t i = 0; i < new_params.count; i++)
                add_binding(&lambda_ctx, false, old_params.nodes[i]->payload.var.name, new_params.nodes[i]);
            new_lam->payload.anon_lam.body = rewrite_node(&lambda_ctx.rewriter, node->payload.anon_lam.body);
            return new_lam;
        }
        case LetMut_TAG: return desugar_let_mut(ctx, node);
        case Return_TAG: {
            assert(ctx->current_function);
            return fn_ret(dst_arena, (Return) {
                .fn = ctx->current_function,
                .args = rewrite_nodes(&ctx->rewriter, node->payload.fn_ret.args)
            });
        }
        default: {
            if (node->tag == PrimOp_TAG && node->payload.prim_op.op == assign_op) {
                const Node* target_ptr = get_node_address(ctx, node->payload.prim_op.operands.nodes[0]);
                const Node* value = rewrite_node(&ctx->rewriter, node->payload.prim_op.operands.nodes[1]);
                return prim_op(dst_arena, (PrimOp) {
                    .op = store_op,
                    .operands = nodes(dst_arena, 2, (const Node* []) { target_ptr, value })
                });
            } else if (node->tag == PrimOp_TAG && node->payload.prim_op.op == subscript_op) {
                return prim_op(dst_arena, (PrimOp) {
                    .op = load_op,
                    .operands = nodes(dst_arena, 1, (const Node* []) { get_node_address(ctx, node) })
                });
            }
            return recreate_node_identity(&ctx->rewriter, node);
        }
    }
}

void bind_program(SHADY_UNUSED CompilerConfig* config, Module* src, Module* dst) {
    Context ctx = {
        .rewriter = create_rewriter(src, dst, (RewriteFn) bind_node),
        .local_variables = NULL,
        .current_function = NULL,
    };

    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
}
