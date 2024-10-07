#include "shady/pass.h"

#include "../ir_private.h"
#include "../type.h"
#include "../transform/ir_gen_helpers.h"

#include "log.h"
#include "portability.h"

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
} Context;

static bool should_convert(Context* ctx, const Type* t) {
    t = get_unqualified_type(t);
    return t->tag == Int_TAG && t->payload.int_type.width == IntTy64 && ctx->config->lower.int64;
}

static void extract_low_hi_halves(BodyBuilder* bb, const Node* src, const Node** lo, const Node** hi) {
    *lo = shd_first(bind_instruction(bb, prim_op(bb->arena,
                                                 (PrimOp) { .op = extract_op, .operands = mk_nodes(bb->arena, src, shd_int32_literal(bb->arena, 0)) })));
    *hi = shd_first(bind_instruction(bb, prim_op(bb->arena,
                                                 (PrimOp) { .op = extract_op, .operands = mk_nodes(bb->arena, src, shd_int32_literal(bb->arena, 1)) })));
}

static void extract_low_hi_halves_list(BodyBuilder* bb, Nodes src, const Node** lows, const Node** his) {
    for (size_t i = 0; i < src.count; i++) {
        extract_low_hi_halves(bb, src.nodes[i], lows, his);
        lows++;
        his++;
    }
}

static const Node* process(Context* ctx, const Node* node) {
    IrArena* a = ctx->rewriter.dst_arena;

    switch (node->tag) {
        case Int_TAG:
            if (node->payload.int_type.width == IntTy64 && ctx->config->lower.int64)
                return record_type(a, (RecordType) {
                    .members = mk_nodes(a, shd_int32_type(a), shd_int32_type(a))
                });
            break;
        case IntLiteral_TAG:
            if (node->payload.int_literal.width == IntTy64 && ctx->config->lower.int64) {
                uint64_t raw = node->payload.int_literal.value;
                const Node* lower = shd_uint32_literal(a, (uint32_t) raw);
                const Node* upper = shd_uint32_literal(a, (uint32_t) (raw >> 32));
                return tuple_helper(a, mk_nodes(a, lower, upper));
            }
            break;
        case PrimOp_TAG: {
            Op op = node->payload.prim_op.op;
            Nodes old_nodes = node->payload.prim_op.operands;
            LARRAY(const Node*, lows, old_nodes.count);
            LARRAY(const Node*, his, old_nodes.count);
            switch(op) {
                case add_op: if (should_convert(ctx, shd_first(old_nodes)->type)) {
                    Nodes new_nodes = rewrite_nodes(&ctx->rewriter, old_nodes);
                    // TODO: convert into and then out of unsigned
                    BodyBuilder* bb = begin_block_pure(a);
                    extract_low_hi_halves_list(bb, new_nodes, lows, his);
                    Nodes low_and_carry = bind_instruction(bb, prim_op(a, (PrimOp) { .op = add_carry_op, .operands = shd_nodes(a, 2, lows)}));
                    const Node* lo = shd_first(low_and_carry);
                    // compute the high side, without forgetting the carry bit
                    const Node* hi = shd_first(bind_instruction(bb, prim_op(a, (PrimOp) { .op = add_op, .operands = shd_nodes(a, 2, his) })));
                                hi = shd_first(bind_instruction(bb, prim_op(a, (PrimOp) { .op = add_op, .operands = mk_nodes(a, hi, low_and_carry.nodes[1]) })));
                    return yield_values_and_wrap_in_block(bb, shd_singleton(tuple_helper(a, mk_nodes(a, lo, hi))));
                } break;
                default: break;
            }
            break;
        }
        default: break;
    }

    rebuild:
    return recreate_node_identity(&ctx->rewriter, node);
}

Module* lower_int(const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *shd_get_arena_config(get_module_arena(src));
    IrArena* a = shd_new_ir_arena(&aconfig);
    Module* dst = new_module(a, get_module_name(src));
    Context ctx = {
        .rewriter = create_node_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
    };
    rewrite_module(&ctx.rewriter);
    destroy_rewriter(&ctx.rewriter);
    return dst;
}
