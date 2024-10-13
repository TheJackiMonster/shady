#include "check.h"
#include "shady/ir/memory_layout.h"

#include "log.h"
#include "ir_private.h"
#include "portability.h"
#include "dict.h"
#include "util.h"

#include "shady/ir/builtin.h"

#include <string.h>
#include <assert.h>

static bool are_types_identical(size_t num_types, const Type* types[]) {
    for (size_t i = 0; i < num_types; i++) {
        assert(types[i]);
        if (types[0] != types[i])
            return false;
    }
    return true;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

const Type* check_type_join_point_type(IrArena* arena, JoinPointType type) {
    for (size_t i = 0; i < type.yield_types.count; i++) {
        assert(shd_is_data_type(type.yield_types.nodes[i]));
    }
    return NULL;
}

const Type* check_type_record_type(IrArena* arena, RecordType type) {
    assert(type.names.count == 0 || type.names.count == type.members.count);
    for (size_t i = 0; i < type.members.count; i++) {
        // member types are value types iff this is a return tuple
        if (type.special == MultipleReturn)
            assert(shd_is_value_type(type.members.nodes[i]));
        else
            assert(shd_is_data_type(type.members.nodes[i]));
    }
    return NULL;
}

const Type* check_type_qualified_type(IrArena* arena, QualifiedType qualified_type) {
    assert(shd_is_data_type(qualified_type.type));
    assert(arena->config.is_simt || qualified_type.is_uniform);
    return NULL;
}

const Type* check_type_arr_type(IrArena* arena, ArrType type) {
    assert(shd_is_data_type(type.element_type));
    return NULL;
}

const Type* check_type_pack_type(IrArena* arena, PackType pack_type) {
    assert(shd_is_data_type(pack_type.element_type));
    return NULL;
}

const Type* check_type_ptr_type(IrArena* arena, PtrType ptr_type) {
    if (!arena->config.address_spaces[ptr_type.address_space].allowed) {
        shd_error_print("Address space %s is not allowed in this arena\n", shd_get_address_space_name(ptr_type.address_space));
        shd_error_die();
    }
    assert(ptr_type.pointed_type && "Shady does not support untyped pointers, but can infer them, see infer.c");
    if (ptr_type.pointed_type) {
        if (ptr_type.pointed_type->tag == ArrType_TAG) {
            assert(shd_is_data_type(ptr_type.pointed_type->payload.arr_type.element_type));
            return NULL;
        }
        if (ptr_type.pointed_type->tag == FnType_TAG || ptr_type.pointed_type == unit_type(arena)) {
            // no diagnostic required, we just allow these
            return NULL;
        }
        const Node* maybe_record_type = ptr_type.pointed_type;
        if (maybe_record_type->tag == TypeDeclRef_TAG)
            maybe_record_type = get_nominal_type_body(maybe_record_type);
        if (maybe_record_type && maybe_record_type->tag == RecordType_TAG && maybe_record_type->payload.record_type.special == DecorateBlock) {
            return NULL;
        }
        assert(shd_is_data_type(ptr_type.pointed_type));
    }
    return NULL;
}

const Type* check_type_param(IrArena* arena, Param variable) {
    assert(shd_is_value_type(variable.type));
    return variable.type;
}

const Type* check_type_untyped_number(IrArena* arena, UntypedNumber untyped) {
    shd_error("should never happen");
}

const Type* check_type_int_literal(IrArena* arena, IntLiteral lit) {
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = true,
        .type = int_type(arena, (Int) { .width = lit.width, .is_signed = lit.is_signed })
    });
}

const Type* check_type_float_literal(IrArena* arena, FloatLiteral lit) {
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = true,
        .type = float_type(arena, (Float) { .width = lit.width })
    });
}

const Type* check_type_true_lit(IrArena* arena) { return qualified_type(arena, (QualifiedType) { .type = bool_type(arena), .is_uniform = true }); }
const Type* check_type_false_lit(IrArena* arena) { return qualified_type(arena, (QualifiedType) { .type = bool_type(arena), .is_uniform = true }); }

const Type* check_type_string_lit(IrArena* arena, StringLiteral str_lit) {
    const Type* t = arr_type(arena, (ArrType) {
        .element_type = shd_int8_type(arena),
        .size = shd_int32_literal(arena, strlen(str_lit.string))
    });
    return qualified_type(arena, (QualifiedType) {
        .type = t,
        .is_uniform = true,
    });
}

const Type* check_type_null_ptr(IrArena* a, NullPtr payload) {
    assert(shd_is_data_type(payload.ptr_type) && payload.ptr_type->tag == PtrType_TAG);
    return shd_as_qualified_type(payload.ptr_type, true);
}

const Type* check_type_composite(IrArena* arena, Composite composite) {
    if (composite.type) {
        assert(shd_is_data_type(composite.type));
        Nodes expected_member_types = get_composite_type_element_types(composite.type);
        bool is_uniform = true;
        assert(composite.contents.count == expected_member_types.count);
        for (size_t i = 0; i < composite.contents.count; i++) {
            const Type* element_type = composite.contents.nodes[i]->type;
            is_uniform &= shd_deconstruct_qualified_type(&element_type);
            assert(shd_is_subtype(expected_member_types.nodes[i], element_type));
        }
        return qualified_type(arena, (QualifiedType) {
            .is_uniform = is_uniform,
            .type = composite.type
        });
    }
    bool is_uniform = true;
    LARRAY(const Type*, member_ts, composite.contents.count);
    for (size_t i = 0; i < composite.contents.count; i++) {
        const Type* element_type = composite.contents.nodes[i]->type;
        is_uniform &= shd_deconstruct_qualified_type(&element_type);
        member_ts[i] = element_type;
    }
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = is_uniform,
        .type = record_type(arena, (RecordType) {
            .members = shd_nodes(arena, composite.contents.count, member_ts)
        })
    });
}

const Type* check_type_fill(IrArena* arena, Fill payload) {
    assert(shd_is_data_type(payload.type));
    const Node* element_t = get_fill_type_element_type(payload.type);
    const Node* value_t = payload.value->type;
    bool u = shd_deconstruct_qualified_type(&value_t);
    assert(shd_is_subtype(element_t, value_t));
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = u,
        .type = payload.type
    });
}

const Type* check_type_undef(IrArena* arena, Undef payload) {
    assert(shd_is_data_type(payload.type));
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = true,
        .type = payload.type
    });
}

const Type* check_type_mem_and_value(IrArena* arena, MemAndValue mav) {
    return mav.value->type;
}

const Type* check_type_fn_addr(IrArena* arena, FnAddr fn_addr) {
    assert(fn_addr.fn->type->tag == FnType_TAG);
    assert(fn_addr.fn->tag == Function_TAG);
    return qualified_type(arena, (QualifiedType) {
        .is_uniform = true,
        .type = ptr_type(arena, (PtrType) {
            .pointed_type = fn_addr.fn->type,
            .address_space = AsGeneric /* the actual AS does not matter because these are opaque anyways */,
        })
    });
}

const Type* check_type_ref_decl(IrArena* arena, RefDecl ref_decl) {
    const Type* t = ref_decl.decl->type;
    assert(t && "RefDecl needs to be applied on a decl with a non-null type. Did you forget to set 'type' on a constant ?");
    switch (ref_decl.decl->tag) {
        case GlobalVariable_TAG:
        case Constant_TAG: break;
        default: shd_error("You can only use RefDecl on a global or a constant. See FnAddr for taking addresses of functions.")
    }
    assert(t->tag != QualifiedType_TAG && "decl types may not be qualified");
    return qualified_type(arena, (QualifiedType) {
        .type = t,
        .is_uniform = true,
    });
}

const Type* check_type_prim_op(IrArena* arena, PrimOp prim_op) {
    for (size_t i = 0; i < prim_op.type_arguments.count; i++) {
        const Node* ta = prim_op.type_arguments.nodes[i];
        assert(ta && is_type(ta));
    }
    for (size_t i = 0; i < prim_op.operands.count; i++) {
        const Node* operand = prim_op.operands.nodes[i];
        assert(operand && is_value(operand));
    }

    bool extended = false;
    bool ordered = false;
    AddressSpace as;
    switch (prim_op.op) {
        case neg_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);

            const Type* type = shd_first(prim_op.operands)->type;
            assert(shd_is_arithm_type(get_maybe_packed_type_element(shd_get_unqualified_type(type))));
            return type;
        }
        case rshift_arithm_op:
        case rshift_logical_op:
        case lshift_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = shd_first(prim_op.operands)->type;
            const Type* second_operand_type = prim_op.operands.nodes[1]->type;

            bool uniform_result = shd_deconstruct_qualified_type(&first_operand_type);
            uniform_result &= shd_deconstruct_qualified_type(&second_operand_type);

            size_t value_simd_width = deconstruct_maybe_packed_type(&first_operand_type);
            size_t shift_simd_width = deconstruct_maybe_packed_type(&second_operand_type);
            assert(value_simd_width == shift_simd_width);

            assert(first_operand_type->tag == Int_TAG);
            assert(second_operand_type->tag == Int_TAG);

            return shd_as_qualified_type(maybe_packed_type_helper(first_operand_type, value_simd_width), uniform_result);
        }
        case add_carry_op:
        case sub_borrow_op:
        case mul_extended_op: extended = true; SHADY_FALLTHROUGH;
        case min_op:
        case max_op:
        case add_op:
        case sub_op:
        case mul_op:
        case div_op:
        case mod_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = shd_get_unqualified_type(shd_first(prim_op.operands)->type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = shd_deconstruct_qualified_type(&operand_type);

                assert(shd_is_arithm_type(get_maybe_packed_type_element(operand_type)));
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            const Type* result_t = first_operand_type;
            if (extended) {
                // TODO: assert unsigned
                result_t = record_type(arena, (RecordType) {.members = mk_nodes(arena, result_t, result_t)});
            }
            return shd_as_qualified_type(result_t, result_uniform);
        }

        case not_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);

            const Type* type = shd_first(prim_op.operands)->type;
            assert(shd_has_boolean_ops(get_maybe_packed_type_element(shd_get_unqualified_type(type))));
            return type;
        }
        case or_op:
        case xor_op:
        case and_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = shd_get_unqualified_type(shd_first(prim_op.operands)->type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = shd_deconstruct_qualified_type(&operand_type);

                assert(shd_has_boolean_ops(get_maybe_packed_type_element(operand_type)));
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return shd_as_qualified_type(first_operand_type, result_uniform);
        }
        case lt_op:
        case lte_op:
        case gt_op:
        case gte_op: ordered = true; SHADY_FALLTHROUGH
        case eq_op:
        case neq_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = shd_get_unqualified_type(shd_first(prim_op.operands)->type);
            size_t first_operand_width = get_maybe_packed_type_width(first_operand_type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = shd_deconstruct_qualified_type(&operand_type);

                assert((ordered ? shd_is_ordered_type : shd_is_comparable_type)(get_maybe_packed_type_element(operand_type)));
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return shd_as_qualified_type(maybe_packed_type_helper(bool_type(arena), first_operand_width),
                                         result_uniform);
        }
        case sqrt_op:
        case inv_sqrt_op:
        case floor_op:
        case ceil_op:
        case round_op:
        case fract_op:
        case sin_op:
        case cos_op:
        case exp_op:
        {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);
            const Node* src_type = shd_first(prim_op.operands)->type;
            bool uniform = shd_deconstruct_qualified_type(&src_type);
            size_t width = deconstruct_maybe_packed_type(&src_type);
            assert(src_type->tag == Float_TAG);
            return shd_as_qualified_type(maybe_packed_type_helper(src_type, width), uniform);
        }
        case pow_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* first_operand_type = shd_get_unqualified_type(shd_first(prim_op.operands)->type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = shd_deconstruct_qualified_type(&operand_type);

                assert(get_maybe_packed_type_element(operand_type)->tag == Float_TAG);
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return shd_as_qualified_type(first_operand_type, result_uniform);
        }
        case fma_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 3);
            const Type* first_operand_type = shd_get_unqualified_type(shd_first(prim_op.operands)->type);

            bool result_uniform = true;
            for (size_t i = 0; i < prim_op.operands.count; i++) {
                const Node* arg = prim_op.operands.nodes[i];
                const Type* operand_type = arg->type;
                bool operand_uniform = shd_deconstruct_qualified_type(&operand_type);

                assert(get_maybe_packed_type_element(operand_type)->tag == Float_TAG);
                assert(first_operand_type == operand_type &&  "operand type mismatch");

                result_uniform &= operand_uniform;
            }

            return shd_as_qualified_type(first_operand_type, result_uniform);
        }
        case abs_op:
        case sign_op:
        {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);
            const Node* src_type = shd_first(prim_op.operands)->type;
            bool uniform = shd_deconstruct_qualified_type(&src_type);
            size_t width = deconstruct_maybe_packed_type(&src_type);
            assert(src_type->tag == Float_TAG || src_type->tag == Int_TAG && src_type->payload.int_type.is_signed);
            return shd_as_qualified_type(maybe_packed_type_helper(src_type, width), uniform);
        }
        case align_of_op:
        case size_of_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 0);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = int_type(arena, (Int) { .width = arena->config.memory.ptr_size, .is_signed = false })
            });
        }
        case offset_of_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 1);
            const Type* optype = shd_first(prim_op.operands)->type;
            bool uniform = shd_deconstruct_qualified_type(&optype);
            assert(uniform && optype->tag == Int_TAG);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = int_type(arena, (Int) { .width = arena->config.memory.ptr_size, .is_signed = false })
            });
        }
        case select_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 3);
            const Type* condition_type = prim_op.operands.nodes[0]->type;
            bool condition_uniform = shd_deconstruct_qualified_type(&condition_type);
            size_t width = deconstruct_maybe_packed_type(&condition_type);

            const Type* alternatives_types[2];
            bool alternatives_all_uniform = true;
            for (size_t i = 0; i < 2; i++) {
                alternatives_types[i] = prim_op.operands.nodes[1 + i]->type;
                alternatives_all_uniform &= shd_deconstruct_qualified_type(&alternatives_types[i]);
                size_t alternative_width = deconstruct_maybe_packed_type(&alternatives_types[i]);
                assert(alternative_width == width);
            }

            assert(shd_is_subtype(bool_type(arena), condition_type));
            // todo find true supertype
            assert(are_types_identical(2, alternatives_types));

            return shd_as_qualified_type(maybe_packed_type_helper(alternatives_types[0], width),
                                         alternatives_all_uniform && condition_uniform);
        }
        case insert_op:
        case extract_dynamic_op:
        case extract_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count >= 2);
            const Node* source = shd_first(prim_op.operands);

            size_t indices_start = prim_op.op == insert_op ? 2 : 1;
            Nodes indices = shd_nodes(arena, prim_op.operands.count - indices_start, &prim_op.operands.nodes[indices_start]);

            const Type* t = source->type;
            bool uniform = shd_deconstruct_qualified_type(&t);
            enter_composite_indices(&t, &uniform, indices, true);

            if (prim_op.op == insert_op) {
                const Node* inserted_data = prim_op.operands.nodes[1];
                const Type* inserted_data_type = inserted_data->type;
                bool is_uniform = uniform & shd_deconstruct_qualified_type(&inserted_data_type);
                assert(shd_is_subtype(t, inserted_data_type) && "inserting data into a composite, but it doesn't match the target and indices");
                return qualified_type(arena, (QualifiedType) {
                    .is_uniform = is_uniform,
                    .type = shd_get_unqualified_type(source->type),
                });
            }

            return shd_as_qualified_type(t, uniform);
        }
        case shuffle_op: {
            assert(prim_op.operands.count >= 2);
            assert(prim_op.type_arguments.count == 0);
            const Node* lhs = prim_op.operands.nodes[0];
            const Node* rhs = prim_op.operands.nodes[1];
            const Type* lhs_t = lhs->type;
            const Type* rhs_t = rhs->type;
            bool lhs_u = shd_deconstruct_qualified_type(&lhs_t);
            bool rhs_u = shd_deconstruct_qualified_type(&rhs_t);
            assert(lhs_t->tag == PackType_TAG && rhs_t->tag == PackType_TAG);
            size_t total_size = lhs_t->payload.pack_type.width + rhs_t->payload.pack_type.width;
            const Type* element_t = lhs_t->payload.pack_type.element_type;
            assert(element_t == rhs_t->payload.pack_type.element_type);

            size_t indices_count = prim_op.operands.count - 2;
            const Node** indices = &prim_op.operands.nodes[2];
            bool u = lhs_u & rhs_u;
            for (size_t i = 0; i < indices_count; i++) {
                u &= shd_is_qualified_type_uniform(indices[i]->type);
                int64_t index = shd_get_int_literal_value(*shd_resolve_to_int_literal(indices[i]), true);
                assert(index < 0 /* poison */ || (index >= 0 && index < total_size && "shuffle element out of range"));
            }
            return shd_as_qualified_type(
                    pack_type(arena, (PackType) {.element_type = element_t, .width = indices_count}), u);
        }
        case reinterpret_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 1);
            const Node* source = shd_first(prim_op.operands);
            const Type* src_type = source->type;
            bool src_uniform = shd_deconstruct_qualified_type(&src_type);

            const Type* dst_type = shd_first(prim_op.type_arguments);
            assert(shd_is_data_type(dst_type));
            assert(shd_is_reinterpret_cast_legal(src_type, dst_type));

            return qualified_type(arena, (QualifiedType) {
                .is_uniform = src_uniform,
                .type = dst_type
            });
        }
        case convert_op: {
            assert(prim_op.type_arguments.count == 1);
            assert(prim_op.operands.count == 1);
            const Node* source = shd_first(prim_op.operands);
            const Type* src_type = source->type;
            bool src_uniform = shd_deconstruct_qualified_type(&src_type);

            const Type* dst_type = shd_first(prim_op.type_arguments);
            assert(shd_is_data_type(dst_type));
            assert(shd_is_conversion_legal(src_type, dst_type));

            // TODO check the conversion is legal
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = src_uniform,
                .type = dst_type
            });
        }
        // Mask management
        case empty_mask_op: {
            assert(prim_op.type_arguments.count == 0 && prim_op.operands.count == 0);
            return shd_as_qualified_type(shd_get_actual_mask_type(arena), true);
        }
        case mask_is_thread_active_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = shd_is_qualified_type_uniform(prim_op.operands.nodes[0]->type) && shd_is_qualified_type_uniform(prim_op.operands.nodes[1]->type),
                .type = bool_type(arena)
            });
        }
        // Subgroup ops
        case subgroup_assume_uniform_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 1);
            const Type* operand_type = shd_get_unqualified_type(prim_op.operands.nodes[0]->type);
            return qualified_type(arena, (QualifiedType) {
                .is_uniform = true,
                .type = operand_type
            });
        }
        // Intermediary ops
        case sample_texture_op: {
            assert(prim_op.type_arguments.count == 0);
            assert(prim_op.operands.count == 2);
            const Type* sampled_image_t = shd_first(prim_op.operands)->type;
            bool uniform_src = shd_deconstruct_qualified_type(&sampled_image_t);
            const Type* coords_t = prim_op.operands.nodes[1]->type;
            shd_deconstruct_qualified_type(&coords_t);
            assert(sampled_image_t->tag == SampledImageType_TAG);
            const Type* image_t = sampled_image_t->payload.sampled_image_type.image_type;
            assert(image_t->tag == ImageType_TAG);
            size_t coords_dim = deconstruct_packed_type(&coords_t);
            return qualified_type(arena, (QualifiedType) { .is_uniform = false, .type = maybe_packed_type_helper(image_t->payload.image_type.sampled_type, 4) });
        }
        case PRIMOPS_COUNT: assert(false);
    }
}

const Type* check_type_ext_instr(IrArena* arena, ExtInstr payload) {
    return payload.result_t;
}

static void check_arguments_types_against_parameters_helper(Nodes param_types, Nodes arg_types) {
    if (param_types.count != arg_types.count)
        shd_error("Mismatched number of arguments/parameters");
    for (size_t i = 0; i < param_types.count; i++)
        shd_check_subtype(param_types.nodes[i], arg_types.nodes[i]);
}

/// Shared logic between indirect calls and tailcalls
static Nodes check_value_call(const Node* callee, Nodes argument_types) {
    assert(is_value(callee));

    const Type* callee_type = callee->type;
    SHADY_UNUSED bool callee_uniform = shd_deconstruct_qualified_type(&callee_type);
    AddressSpace as = deconstruct_pointer_type(&callee_type);
    assert(as == AsGeneric);

    assert(callee_type->tag == FnType_TAG);

    const FnType* fn_type = &callee_type->payload.fn_type;
    check_arguments_types_against_parameters_helper(fn_type->param_types, argument_types);
    // TODO force the return types to be varying if the callee is not uniform
    return fn_type->return_types;
}

const Type* check_type_call(IrArena* arena, Call call) {
    Nodes args = call.args;
    for (size_t i = 0; i < args.count; i++) {
        const Node* argument = args.nodes[i];
        assert(is_value(argument));
    }
    Nodes argument_types = shd_get_values_types(arena, args);
    return maybe_multiple_return(arena, check_value_call(call.callee, argument_types));
}

static void ensure_types_are_data_types(const Nodes* yield_types) {
    for (size_t i = 0; i < yield_types->count; i++) {
        assert(shd_is_data_type(yield_types->nodes[i]));
    }
}

static void ensure_types_are_value_types(const Nodes* yield_types) {
    for (size_t i = 0; i < yield_types->count; i++) {
        assert(shd_is_value_type(yield_types->nodes[i]));
    }
}

const Type* check_type_if_instr(IrArena* arena, If if_instr) {
    assert(if_instr.tail && is_abstraction(if_instr.tail));
    ensure_types_are_data_types(&if_instr.yield_types);
    if (shd_get_unqualified_type(if_instr.condition->type) != bool_type(arena))
        shd_error("condition of an if should be bool");
    // TODO check the contained Merge instrs
    if (if_instr.yield_types.count > 0)
        assert(if_instr.if_false);

    check_arguments_types_against_parameters_helper(shd_get_param_types(arena, get_abstraction_params(if_instr.tail)), shd_add_qualifiers(arena, if_instr.yield_types, false));
    return noret_type(arena);
}

const Type* check_type_match_instr(IrArena* arena, Match match_instr) {
    ensure_types_are_data_types(&match_instr.yield_types);
    // TODO check param against initial_args
    // TODO check the contained Merge instrs
    return noret_type(arena);
}

const Type* check_type_loop_instr(IrArena* arena, Loop loop_instr) {
    ensure_types_are_data_types(&loop_instr.yield_types);
    // TODO check param against initial_args
    // TODO check the contained Merge instrs
    return noret_type(arena);
}

const Type* check_type_control(IrArena* arena, Control control) {
    ensure_types_are_data_types(&control.yield_types);
    // TODO check it then !
    const Node* join_point = shd_first(get_abstraction_params(control.inside));

    const Type* join_point_type = join_point->type;
    shd_deconstruct_qualified_type(&join_point_type);
    assert(join_point_type->tag == JoinPointType_TAG);

    Nodes join_point_yield_types = join_point_type->payload.join_point_type.yield_types;
    assert(join_point_yield_types.count == control.yield_types.count);
    for (size_t i = 0; i < control.yield_types.count; i++) {
        assert(shd_is_subtype(control.yield_types.nodes[i], join_point_yield_types.nodes[i]));
    }

    assert(get_abstraction_params(control.tail).count == control.yield_types.count);

    return noret_type(arena);
}

const Type* check_type_comment(IrArena* arena, SHADY_UNUSED Comment payload) {
    return empty_multiple_return_type(arena);
}

const Type* check_type_stack_alloc(IrArena* a, StackAlloc alloc) {
    assert(is_type(alloc.type));
    return qualified_type(a, (QualifiedType) {
        .is_uniform = shd_is_addr_space_uniform(a, AsPrivate),
        .type = ptr_type(a, (PtrType) {
            .pointed_type = alloc.type,
            .address_space = AsPrivate,
            .is_reference = false
        })
    });
}

const Type* check_type_local_alloc(IrArena* a, LocalAlloc alloc) {
    assert(is_type(alloc.type));
    return qualified_type(a, (QualifiedType) {
        .is_uniform = shd_is_addr_space_uniform(a, AsFunction),
        .type = ptr_type(a, (PtrType) {
            .pointed_type = alloc.type,
            .address_space = AsFunction,
            .is_reference = true
        })
    });
}

const Type* check_type_load(IrArena* a, Load load) {
    const Node* ptr_type = load.ptr->type;
    bool ptr_uniform = shd_deconstruct_qualified_type(&ptr_type);
    size_t width = deconstruct_maybe_packed_type(&ptr_type);

    assert(ptr_type->tag == PtrType_TAG);
    const PtrType* node_ptr_type_ = &ptr_type->payload.ptr_type;
    const Type* elem_type = node_ptr_type_->pointed_type;
    elem_type = maybe_packed_type_helper(elem_type, width);
    return shd_as_qualified_type(elem_type,
                                 ptr_uniform && shd_is_addr_space_uniform(a, ptr_type->payload.ptr_type.address_space));
}

const Type* check_type_store(IrArena* a, Store store) {
    const Node* ptr_type = store.ptr->type;
    bool ptr_uniform = shd_deconstruct_qualified_type(&ptr_type);
    size_t width = deconstruct_maybe_packed_type(&ptr_type);
    assert(ptr_type->tag == PtrType_TAG);
    const PtrType* ptr_type_payload = &ptr_type->payload.ptr_type;
    const Type* elem_type = ptr_type_payload->pointed_type;
    assert(elem_type);
    elem_type = maybe_packed_type_helper(elem_type, width);
    // we don't enforce uniform stores - but we care about storing the right thing :)
    const Type* val_expected_type = qualified_type(a, (QualifiedType) {
        .is_uniform = !a->config.is_simt,
        .type = elem_type
    });

    assert(shd_is_subtype(val_expected_type, store.value->type));
    return empty_multiple_return_type(a);
}

const Type* check_type_ptr_array_element_offset(IrArena* a, PtrArrayElementOffset lea) {
    const Type* base_ptr_type = lea.ptr->type;
    bool uniform = shd_deconstruct_qualified_type(&base_ptr_type);
    assert(base_ptr_type->tag == PtrType_TAG && "lea expects a ptr or ref as a base");
    const Type* pointee_type = base_ptr_type->payload.ptr_type.pointed_type;

    assert(lea.offset);
    const Type* offset_type = lea.offset->type;
    bool offset_uniform = shd_deconstruct_qualified_type(&offset_type);
    assert(offset_type->tag == Int_TAG && "lea expects an integer offset");

    const IntLiteral* lit = shd_resolve_to_int_literal(lea.offset);
    bool offset_is_zero = lit && lit->value == 0;
    assert(offset_is_zero || !base_ptr_type->payload.ptr_type.is_reference && "if an offset is used, the base cannot be a reference");
    assert(offset_is_zero || shd_is_data_type(pointee_type) && "if an offset is used, the base must point to a data type");
    uniform &= offset_uniform;

    return qualified_type(a, (QualifiedType) {
        .is_uniform = uniform,
        .type = ptr_type(a, (PtrType) {
            .pointed_type = pointee_type,
            .address_space = base_ptr_type->payload.ptr_type.address_space,
            .is_reference = base_ptr_type->payload.ptr_type.is_reference
        })
    });
}

const Type* check_type_ptr_composite_element(IrArena* a, PtrCompositeElement lea) {
    const Type* base_ptr_type = lea.ptr->type;
    bool uniform = shd_deconstruct_qualified_type(&base_ptr_type);
    assert(base_ptr_type->tag == PtrType_TAG && "lea expects a ptr or ref as a base");
    const Type* pointee_type = base_ptr_type->payload.ptr_type.pointed_type;

    enter_composite(&pointee_type, &uniform, lea.index, true);

    return qualified_type(a, (QualifiedType) {
        .is_uniform = uniform,
        .type = ptr_type(a, (PtrType) {
            .pointed_type = pointee_type,
            .address_space = base_ptr_type->payload.ptr_type.address_space,
            .is_reference = base_ptr_type->payload.ptr_type.is_reference
        })
    });
}

const Type* check_type_copy_bytes(IrArena* a, CopyBytes copy_bytes) {
    const Type* dst_t = copy_bytes.dst->type;
    shd_deconstruct_qualified_type(&dst_t);
    assert(dst_t->tag == PtrType_TAG);
    const Type* src_t = copy_bytes.src->type;
    shd_deconstruct_qualified_type(&src_t);
    assert(src_t);
    const Type* cnt_t = copy_bytes.count->type;
    shd_deconstruct_qualified_type(&cnt_t);
    assert(cnt_t->tag == Int_TAG);
    return empty_multiple_return_type(a);
}

const Type* check_type_fill_bytes(IrArena* a, FillBytes fill_bytes) {
    const Type* dst_t = fill_bytes.dst->type;
    shd_deconstruct_qualified_type(&dst_t);
    assert(dst_t->tag == PtrType_TAG);
    const Type* src_t = fill_bytes.src->type;
    shd_deconstruct_qualified_type(&src_t);
    assert(src_t);
    const Type* cnt_t = fill_bytes.count->type;
    shd_deconstruct_qualified_type(&cnt_t);
    assert(cnt_t->tag == Int_TAG);
    return empty_multiple_return_type(a);
}

const Type* check_type_push_stack(IrArena* a, PushStack payload) {
    assert(payload.value);
    return empty_multiple_return_type(a);
}

const Type* check_type_pop_stack(IrArena* a, PopStack payload) {
    return shd_as_qualified_type(payload.type, false);
}

const Type* check_type_set_stack_size(IrArena* a, SetStackSize payload) {
    assert(shd_get_unqualified_type(payload.value->type) == shd_uint32_type(a));
    return shd_as_qualified_type(unit_type(a), true);
}

const Type* check_type_get_stack_size(IrArena* a, SHADY_UNUSED GetStackSize ss) {
    return qualified_type(a, (QualifiedType) { .is_uniform = false, .type = shd_uint32_type(a) });
}

const Type* check_type_get_stack_base_addr(IrArena* a, SHADY_UNUSED GetStackBaseAddr gsba) {
    const Node* ptr = ptr_type(a, (PtrType) { .pointed_type = shd_uint8_type(a), .address_space = AsPrivate});
    return qualified_type(a, (QualifiedType) { .is_uniform = false, .type = ptr });
}

const Type* check_type_debug_printf(IrArena* a, DebugPrintf payload) {
    return empty_multiple_return_type(a);
}

const Type* check_type_tail_call(IrArena* arena, TailCall tail_call) {
    Nodes args = tail_call.args;
    for (size_t i = 0; i < args.count; i++) {
        const Node* argument = args.nodes[i];
        assert(is_value(argument));
    }
    assert(check_value_call(tail_call.callee, shd_get_values_types(arena, tail_call.args)).count == 0);
    return noret_type(arena);
}

static void check_basic_block_call(const Node* block, Nodes argument_types) {
    assert(is_basic_block(block));
    assert(block->type->tag == BBType_TAG);
    BBType bb_type = block->type->payload.bb_type;
    check_arguments_types_against_parameters_helper(bb_type.param_types, argument_types);
}

const Type* check_type_jump(IrArena* arena, Jump jump) {
    for (size_t i = 0; i < jump.args.count; i++) {
        const Node* argument = jump.args.nodes[i];
        assert(is_value(argument));
    }

    check_basic_block_call(jump.target, shd_get_values_types(arena, jump.args));
    return noret_type(arena);
}

const Type* check_type_branch(IrArena* arena, Branch payload) {
    assert(payload.true_jump->tag == Jump_TAG);
    assert(payload.false_jump->tag == Jump_TAG);
    return noret_type(arena);
}

const Type* check_type_br_switch(IrArena* arena, Switch payload) {
    for (size_t i = 0; i < payload.case_jumps.count; i++)
        assert(payload.case_jumps.nodes[i]->tag == Jump_TAG);
    assert(payload.case_values.count == payload.case_jumps.count);
    assert(payload.default_jump->tag == Jump_TAG);
    return noret_type(arena);
}

const Type* check_type_join(IrArena* arena, Join join) {
    for (size_t i = 0; i < join.args.count; i++) {
        const Node* argument = join.args.nodes[i];
        assert(is_value(argument));
    }

    const Type* join_target_type = join.join_point->type;

    shd_deconstruct_qualified_type(&join_target_type);
    assert(join_target_type->tag == JoinPointType_TAG);

    Nodes join_point_param_types = join_target_type->payload.join_point_type.yield_types;
    join_point_param_types = shd_add_qualifiers(arena, join_point_param_types, !arena->config.is_simt);

    check_arguments_types_against_parameters_helper(join_point_param_types, shd_get_values_types(arena, join.args));

    return noret_type(arena);
}

const Type* check_type_unreachable(IrArena* arena, SHADY_UNUSED Unreachable u) {
    return noret_type(arena);
}

const Type* check_type_merge_continue(IrArena* arena, MergeContinue mc) {
    // TODO check it
    return noret_type(arena);
}

const Type* check_type_merge_break(IrArena* arena, MergeBreak mc) {
    // TODO check it
    return noret_type(arena);
}

const Type* check_type_merge_selection(IrArena* arena, SHADY_UNUSED MergeSelection payload) {
    // TODO check it
    return noret_type(arena);
}

const Type* check_type_fn_ret(IrArena* arena, Return ret) {
    // assert(ret.fn);
    // TODO check it then !
    return noret_type(arena);
}

const Type* check_type_fun(IrArena* arena, Function fn) {
    for (size_t i = 0; i < fn.return_types.count; i++) {
        assert(shd_is_value_type(fn.return_types.nodes[i]));
    }
    return fn_type(arena, (FnType) { .param_types = shd_get_param_types(arena, (&fn)->params), .return_types = (&fn)->return_types });
}

const Type* check_type_basic_block(IrArena* arena, BasicBlock bb) {
    return bb_type(arena, (BBType) { .param_types = shd_get_param_types(arena, (&bb)->params) });
}

const Type* check_type_global_variable(IrArena* arena, GlobalVariable global_variable) {
    assert(is_type(global_variable.type));

    const Node* ba = shd_lookup_annotation_list(global_variable.annotations, "Builtin");
    if (ba && arena->config.validate_builtin_types) {
        Builtin b = shd_get_builtin_by_name(shd_get_annotation_string_payload(ba));
        assert(b != BuiltinsCount);
        const Type* t = shd_get_builtin_type(arena, b);
        if (t != global_variable.type) {
            shd_error_print("Creating a @Builtin global variable '%s' with the incorrect type: ", global_variable.name);
            shd_log_node(ERROR, global_variable.type);
            shd_error_print(" instead of the expected ");
            shd_log_node(ERROR, t);
            shd_error_print(".\n");
            shd_error_die();
        }
    }

    assert(global_variable.address_space < NumAddressSpaces);

    return ptr_type(arena, (PtrType) {
        .pointed_type = global_variable.type,
        .address_space = global_variable.address_space,
        .is_reference = shd_lookup_annotation_list(global_variable.annotations, "Logical"),
    });
}

const Type* check_type_constant(IrArena* arena, Constant cnst) {
    assert(shd_is_data_type(cnst.type_hint));
    return cnst.type_hint;
}

#include "type_generated.c"

#pragma GCC diagnostic pop