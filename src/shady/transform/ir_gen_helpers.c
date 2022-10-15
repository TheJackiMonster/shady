#include "ir_gen_helpers.h"

#include "list.h"
#include "portability.h"

#include "../ir_private.h"
#include "../type.h"
#include "../body_builder.h"
#include "../rewrite.h"

#include <string.h>
#include <assert.h>

Nodes gen_primop(BodyBuilder* instructions, Op op, Nodes operands) {
    const Node* instruction = prim_op(instructions->arena, (PrimOp) { .op = op, .operands = operands });
    Nodes output_types = unwrap_multiple_yield_types(instructions->arena, instruction->type);

    LARRAY(const char*, names, output_types.count);
    for (size_t i = 0; i < output_types.count; i++)
        names[i] = format_string(instructions->arena, "%s_out", primop_names[op]);

    if (output_types.count > 0)
        instruction = let(instructions->arena,  instruction, output_types.count, names);
    append_body(instructions, instruction);

    return output_types.count > 0 ? instruction->payload.let.variables : nodes(instructions->arena, 0, NULL);
}

Nodes gen_primop_c(BodyBuilder* bb, Op op, size_t operands_count, const Node* operands[]) {
    return gen_primop(bb, op, nodes(bb->arena, operands_count, operands));
}

const Node* gen_primop_ce(BodyBuilder* bb, Op op, size_t operands_count, const Node* operands[]) {
    Nodes result = gen_primop_c(bb, op, operands_count, operands);
    assert(result.count == 1);
    return result.nodes[0];
}

const Node* gen_primop_e(BodyBuilder* bb, Op op, Nodes nodes) {
    Nodes result = gen_primop(bb, op, nodes);
    assert(result.count == 1);
    return result.nodes[0];
}

void gen_push_value_stack(BodyBuilder* instructions, const Node* value) {
    append_body(instructions, prim_op(instructions->arena, (PrimOp) {
        .op = push_stack_op,
        .operands = nodes(instructions->arena, 2, (const Node*[]) { extract_operand_type(value->type), value })
    }));
}

void gen_push_values_stack(BodyBuilder* instructions, Nodes values) {
    for (size_t i = values.count - 1; i < values.count; i--) {
        const Node* value = values.nodes[i];
        gen_push_value_stack(instructions, value);
    }
}

const Node* gen_pop_value_stack(BodyBuilder* instructions, String var_name, const Type* type) {
    const char* names[] = { var_name };
    const Node* let_i = let(instructions->arena, prim_op(instructions->arena, (PrimOp) {
            .op = pop_stack_op,
            .operands = nodes(instructions->arena, 1, (const Node*[]) { type })
    }), 1, names);
    append_body(instructions, let_i);
    return let_i->payload.let.variables.nodes[0];
}

Nodes gen_pop_values_stack(BodyBuilder* instructions, String var_name, const Nodes types) {
    LARRAY(const Node*, tmp, types.count);
    for (size_t i = 0; i < types.count; i++) {
        tmp[i] = gen_pop_value_stack(instructions, format_string(instructions->arena, "%s_%d", var_name, (int) i), types.nodes[i]);
    }
    return nodes(instructions->arena, types.count, tmp);
}

const Node* gen_merge_i32s_i64(BodyBuilder* bb, const Node* lo, const Node* hi) {
    // widen them
    lo = gen_primop_ce(bb, reinterpret_op, 2, (const Node* []) {int64_type(bb->arena), lo});
    hi = gen_primop_ce(bb, reinterpret_op, 2, (const Node* []) {int64_type(bb->arena), hi});
    // shift hi by 32
    hi = gen_primop_ce(bb, lshift_op, 2, (const Node* []) { hi, int64_literal(bb->arena, 32) });
    // Merge the two
    return gen_primop_ce(bb, or_op, 2, (const Node* []) { lo, hi });
}

const Node* gen_load(BodyBuilder* instructions, const Node* ptr) {
    return gen_primop_ce(instructions, load_op, 1, (const Node* []) { ptr });
}

void gen_store(BodyBuilder* instructions, const Node* ptr, const Node* value) {
    gen_primop_c(instructions, store_op, 2, (const Node* []) { ptr, value });
}

const Node* gen_lea(BodyBuilder* instructions, const Node* base, const Node* offset, Nodes selectors) {
    LARRAY(const Node*, ops, 2 + selectors.count);
    ops[0] = base;
    ops[1] = offset;
    for (size_t i = 0; i < selectors.count; i++)
        ops[2 + i] = selectors.nodes[i];
    return gen_primop_ce(instructions, lea_op, 2 + selectors.count, ops);
}

const Node* find_or_process_decl(Rewriter* rewriter, const Node* root, const char* name) {
    for (size_t i = 0; i < root->payload.root.declarations.count; i++) {
        const Node* decl = root->payload.root.declarations.nodes[i];
        if (strcmp(get_decl_name(decl), name) == 0) {
            return rewrite_node(rewriter, decl);
        }
    }
    assert(false);
}

const Node* access_decl(Rewriter* rewriter, const Node* root, const char* name) {
    const Node* decl = find_or_process_decl(rewriter, root, name);
    if (decl->tag == Lambda_TAG)
        return fn_addr(rewriter->dst_arena, (FnAddr) { .fn = decl });
    else
        return ref_decl(rewriter->dst_arena, (RefDecl) { .decl = decl });
}
