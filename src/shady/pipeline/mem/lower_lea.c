#include "shady/pass.h"
#include "shady/ir/cast.h"

#include "ir_private.h"

#include "log.h"
#include "portability.h"

#include <assert.h>

typedef struct {
    Rewriter rewriter;
    const CompilerConfig* config;
} Context;

// TODO: make this configuration-dependant
static bool is_as_emulated(SHADY_UNUSED Context* ctx, AddressSpace as) {
    switch (as) {
        case AsPrivate:  return true; // TODO have a config option to do this with swizzled global memory
        case AsSubgroup: return true;
        case AsShared:   return true;
        case AsGlobal:  return true; // TODO have a config option to do this with SSBOs
        default: return false;
    }
}

static const Node* lower_ptr_index(Context* ctx, BodyBuilder* bb, const Type* pointer_type, const Node* base, const Node* index) {
    IrArena* a = ctx->rewriter.dst_arena;
    const Type* emulated_ptr_t = int_type(a, (Int) { .width = a->config.target.memory.ptr_size, .is_signed = false });
    assert(pointer_type->tag == PtrType_TAG);

    const Node* ptr = base;

    assert(pointer_type->tag == PtrType_TAG);
    const Type* pointed_type = pointer_type->payload.ptr_type.pointed_type;
    switch (pointed_type->tag) {
        case PackType_TAG:
        case ArrType_TAG: {
            const Type* element_type = shd_get_fill_type_element_type(pointed_type);

            const Node* element_t_size = prim_op_helper(a, size_of_op, shd_singleton(element_type), shd_empty(a));

            const Node* new_index = shd_bld_convert_int_extend_according_to_src_t(bb, emulated_ptr_t, index);
            const Node* physical_offset = prim_op_helper(a, mul_op, shd_empty(a), mk_nodes(a, new_index, element_t_size));

            ptr = prim_op_helper(a, add_op, shd_empty(a), mk_nodes(a, ptr, physical_offset));

            pointer_type = ptr_type(a, (PtrType) {
                .pointed_type = element_type,
                .address_space = pointer_type->payload.ptr_type.address_space
            });
            break;
        }
        case TypeDeclRef_TAG: {
            const Node* nom_decl = pointed_type->payload.type_decl_ref.decl;
            assert(nom_decl && nom_decl->tag == NominalType_TAG);
            pointed_type = nom_decl->payload.nom_type.body;
            SHADY_FALLTHROUGH
        }
        case RecordType_TAG: {
            Nodes member_types = pointed_type->payload.record_type.members;

            const IntLiteral* selector_value = shd_resolve_to_int_literal(index);
            assert(selector_value && "selector value must be known for LEA into a record");
            size_t n = selector_value->value;
            assert(n < member_types.count);

            const Node* offset_of = prim_op_helper(a, offset_of_op, shd_singleton(pointed_type), shd_singleton(shd_uint64_literal(a, n)));
            ptr = prim_op_helper(a, add_op, shd_empty(a), mk_nodes(a, ptr, offset_of));

            pointer_type = ptr_type(a, (PtrType) {
                .pointed_type = member_types.nodes[n],
                .address_space = pointer_type->payload.ptr_type.address_space
            });
            break;
        }
        default: shd_error("cannot index into this")
    }

    return ptr;
}

static const Node* lower_ptr_offset(Context* ctx, BodyBuilder* bb, const Type* pointer_type, const Node* base, const Node* offset) {
    IrArena* a = ctx->rewriter.dst_arena;
    const Type* emulated_ptr_t = int_type(a, (Int) { .width = a->config.target.memory.ptr_size, .is_signed = false });
    assert(pointer_type->tag == PtrType_TAG);

    const Node* ptr = base;

    const IntLiteral* offset_value = shd_resolve_to_int_literal(offset);
    bool offset_is_zero = offset_value && offset_value->value == 0;
    if (!offset_is_zero) {
        const Type* element_type = pointer_type->payload.ptr_type.pointed_type;
        // assert(arr_type->tag == ArrType_TAG);
        // const Type* element_type = arr_type->payload.arr_type.element_type;

        const Node* element_t_size = prim_op_helper(a, size_of_op, shd_singleton(element_type), shd_empty(a));

        const Node* new_offset = shd_bld_convert_int_extend_according_to_src_t(bb, emulated_ptr_t, offset);
        const Node* physical_offset = prim_op_helper(a, mul_op, shd_empty(a), mk_nodes(a, new_offset, element_t_size));

        ptr = prim_op_helper(a, add_op, shd_empty(a), mk_nodes(a, ptr, physical_offset));
    }

    return ptr;
}

static const Node* process(Context* ctx, const Node* old) {
    Rewriter* r = &ctx->rewriter;
    IrArena* a = r->dst_arena;

    const Type* emulated_ptr_t = int_type(a, (Int) { .width = a->config.target.memory.ptr_size, .is_signed = false });

    switch (old->tag) {
        case PtrArrayElementOffset_TAG: {
            PtrArrayElementOffset lea = old->payload.ptr_array_element_offset;
            const Node* old_base = lea.ptr;
            const Type* old_base_ptr_t = old_base->type;
            shd_deconstruct_qualified_type(&old_base_ptr_t);
            assert(old_base_ptr_t->tag == PtrType_TAG);
            const Node* old_result_t = old->type;
            shd_deconstruct_qualified_type(&old_result_t);
            bool must_lower = false;
            // we have to lower generic pointers if we emulate them using ints
            must_lower |= ctx->config->lower.emulate_generic_ptrs && old_base_ptr_t->payload.ptr_type.address_space == AsGeneric;
            must_lower |= ctx->config->lower.emulate_physical_memory && !old_base_ptr_t->payload.ptr_type.is_reference && is_as_emulated(ctx, old_base_ptr_t->payload.ptr_type.address_space);
            if (!must_lower)
                break;
            BodyBuilder* bb = shd_bld_begin_pure(a);
            // Nodes new_ops = rewrite_nodes(&ctx->rewriter, old_ops);
            const Node* cast_base = shd_bld_reinterpret_cast(bb, emulated_ptr_t, shd_rewrite_node(r, lea.ptr));
            const Type* new_base_t = shd_rewrite_node(&ctx->rewriter, old_base_ptr_t);
            const Node* result = lower_ptr_offset(ctx, bb, new_base_t, cast_base, shd_rewrite_node(r, lea.offset));
            const Type* new_ptr_t = shd_rewrite_node(&ctx->rewriter, old_result_t);
            const Node* cast_result = shd_bld_reinterpret_cast(bb, new_ptr_t, result);
            return shd_bld_to_instr_yield_values(bb, shd_singleton(cast_result));
        }
        case PtrCompositeElement_TAG: {
            PtrCompositeElement lea = old->payload.ptr_composite_element;
            const Node* old_base = lea.ptr;
            const Type* old_base_ptr_t = old_base->type;
            shd_deconstruct_qualified_type(&old_base_ptr_t);
            assert(old_base_ptr_t->tag == PtrType_TAG);
            const Node* old_result_t = old->type;
            shd_deconstruct_qualified_type(&old_result_t);
            bool must_lower = false;
            // we have to lower generic pointers if we emulate them using ints
            must_lower |= ctx->config->lower.emulate_generic_ptrs && old_base_ptr_t->payload.ptr_type.address_space == AsGeneric;
            must_lower |= ctx->config->lower.emulate_physical_memory && !old_base_ptr_t->payload.ptr_type.is_reference && is_as_emulated(ctx, old_base_ptr_t->payload.ptr_type.address_space);
            if (!must_lower)
                break;
            BodyBuilder* bb = shd_bld_begin_pure(a);
            // Nodes new_ops = rewrite_nodes(&ctx->rewriter, old_ops);
            const Node* cast_base = shd_bld_reinterpret_cast(bb, emulated_ptr_t, shd_rewrite_node(r, lea.ptr));
            const Type* new_base_t = shd_rewrite_node(&ctx->rewriter, old_base_ptr_t);
            const Node* result = lower_ptr_index(ctx, bb, new_base_t, cast_base, shd_rewrite_node(r, lea.index));
            const Type* new_ptr_t = shd_rewrite_node(&ctx->rewriter, old_result_t);
            const Node* cast_result = shd_bld_reinterpret_cast(bb, new_ptr_t, result);
            return shd_bld_to_instr_yield_values(bb, shd_singleton(cast_result));
        }
        default: break;
    }

    return shd_recreate_node(&ctx->rewriter, old);
}

Module* shd_pass_lower_lea(const CompilerConfig* config, Module* src) {
    ArenaConfig aconfig = *shd_get_arena_config(shd_module_get_arena(src));
    IrArena* a = shd_new_ir_arena(&aconfig);
    Module* dst = shd_new_module(a, shd_module_get_name(src));
    Context ctx = {
        .rewriter = shd_create_node_rewriter(src, dst, (RewriteNodeFn) process),
        .config = config,
    };
    shd_rewrite_module(&ctx.rewriter);
    shd_destroy_rewriter(&ctx.rewriter);
    return dst;
}
