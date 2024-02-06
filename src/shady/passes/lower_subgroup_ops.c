#include "passes.h"

#include "portability.h"
#include "log.h"
#include "dict.h"

#include "../rewrite.h"
#include "../type.h"
#include "../transform/ir_gen_helpers.h"
#include "../transform/memory_layout.h"

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
    struct Dict* fns;
} Context;

static bool is_extended_type(SHADY_UNUSED IrArena* a, const Type* t, bool allow_vectors) {
    switch (t->tag) {
        case Int_TAG: return true;
        // TODO allow 16-bit floats specifically !
        case Float_TAG: return true;
        case PackType_TAG:
            if (allow_vectors)
                return is_extended_type(a, t->payload.pack_type.element_type, false);
            return false;
        default: return false;
    }
}

static bool is_supported_natively(Context* ctx, const Type* element_type) {
    IrArena* a = ctx->rewriter.dst_arena;
    if (element_type->tag == Int_TAG && element_type->payload.int_type.width == IntTy32) {
        return true;
    } else if (!ctx->config->lower.emulate_subgroup_ops_extended_types && is_extended_type(a, element_type, true)) {
        return true;
    }

    return false;
}

static const Node* build_subgroup_first(Context* ctx, BodyBuilder* bb, const Node* src);

static void build_fn_body(Context* ctx, Node* fn, const Node* param, const Node* t) {
    IrArena* a = ctx->rewriter.dst_arena;
    BodyBuilder* bb = begin_body(a);
    t = get_maybe_nominal_type_body(t);
    switch (is_type(t)) {
        case Type_ArrType_TAG:
        case Type_RecordType_TAG: {
            assert(t->payload.record_type.special == 0);
            Nodes element_types = get_composite_type_element_types(t);
            LARRAY(const Node*, elements, element_types.count);
            for (size_t i = 0; i < element_types.count; i++) {
                const Node* e = gen_extract(bb, param, singleton(uint32_literal(a, i)));
                elements[i] = build_subgroup_first(ctx, bb, e);
            }
            fn->payload.fun.body = finish_body(bb, fn_ret(a, (Return) {
                    .fn = fn,
                    .args = singleton(composite_helper(a, t, nodes(a, element_types.count, elements)))
            }));
            return;
        }
        default:
            log_string(ERROR, "subgroup_first is not supported on ");
            log_node(ERROR, t);
            log_string(ERROR, ".\n");
            error_die();
    }
    SHADY_UNREACHABLE;
}

static const Node* build_subgroup_first(Context* ctx, BodyBuilder* bb, const Node* src) {
    IrArena* a = ctx->rewriter.dst_arena;
    Module* m = ctx->rewriter.dst_module;
    const Node* t =  get_unqualified_type(src->type);
    if (is_supported_natively(ctx, t))
        return gen_primop_e(bb, subgroup_broadcast_first_op, empty(a), singleton(src));

    Node* fn = NULL;
    Node** found = find_value_dict(const Node*, Node*, ctx->fns, t);
    if (found)
        fn = *found;
    else {
        const Node* param = var(a, qualified_type_helper(t, false), "src");
        fn = function(m, singleton(param), format_string_interned(a, "subgroup_first_%s", name_type_safe(a, t)),
                      singleton(annotation(a, (Annotation) { .name = "Generated"})), singleton(qualified_type_helper(t, true)));
        insert_dict(const Node*, Node*, ctx->fns, t, fn);
        build_fn_body(ctx, fn, param, t);
    }

    return first(gen_call(bb, fn_addr_helper(a, fn), singleton(src)));
}

static const Node* process(Context* ctx, const Node* node) {
    if (!node) return NULL;
    const Node* found = search_processed(&ctx->rewriter, node);
    if (found) return found;

    IrArena* a = ctx->rewriter.dst_arena;
    Rewriter* r = &ctx->rewriter;
    switch (node->tag) {
        case PrimOp_TAG: {
            PrimOp payload = node->payload.prim_op;
            switch (payload.op) {
                case subgroup_broadcast_first_op: {
                    BodyBuilder* bb = begin_body(a);
                    return yield_values_and_wrap_in_block(bb, singleton(
                            build_subgroup_first(ctx, bb, rewrite_node(r, first(payload.operands)))));
                }
                default: break;
            }
        }
        default: break;
    }
    return recreate_node_identity(&ctx->rewriter, node);
}

KeyHash hash_node(Node**);
bool compare_node(Node**, Node**);

Module* lower_subgroup_ops(const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = get_arena_config(get_module_arena(src));
    IrArena* a = new_ir_arena(aconfig);
    Module* dst = new_module(a, get_module_name(src));
    assert(!config->lower.emulate_subgroup_ops && "TODO");
    Context ctx = {
        .rewriter = create_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
        .fns =  new_dict(const Node*, Node*, (HashFn) hash_node, (CmpFn) compare_node)
    };
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    destroy_dict(ctx.fns);
    return dst;
}
