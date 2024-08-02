#include "emit_c.h"

#include "portability.h"
#include "log.h"
#include "dict.h"
#include "util.h"

#include "../shady/type.h"
#include "../shady/ir_private.h"

#include <assert.h>
#include <stdlib.h>

#pragma GCC diagnostic error "-Wswitch"

void emit_pack_code(Printer* p, Strings src, String dst) {
    for (size_t i = 0; i < src.count; i++) {
        print(p, "\n%s->_%d = %s", dst, src.strings[i], i);
    }
}

void emit_unpack_code(Printer* p, String src, Strings dst) {
    for (size_t i = 0; i < dst.count; i++) {
        print(p, "\n%s = %s->_%d", dst.strings[i], src, i);
    }
}

static Strings emit_variable_declarations(Emitter* emitter, Printer* p, String given_name, Strings* given_names, Nodes types, bool mut, const Nodes* init_values) {
    if (given_names)
        assert(given_names->count == types.count);
    if (init_values)
        assert(init_values->count == types.count);
    LARRAY(String, names, types.count);
    for (size_t i = 0; i < types.count; i++) {
        String name = given_names ? given_names->strings[i] : given_name;
        assert(name);
        names[i] = unique_name(emitter->arena, name);
        if (init_values) {
            CTerm initializer = emit_value(emitter, p, init_values->nodes[i]);
            emit_variable_declaration(emitter, p, types.nodes[i], names[i], mut, &initializer);
        } else
            emit_variable_declaration(emitter, p, types.nodes[i], names[i], mut, NULL);
    }
    return strings(emitter->arena, types.count, names);
}

static const Type* get_first_op_scalar_type(Nodes ops) {
    const Type* t = first(ops)->type;
    deconstruct_qualified_type(&t);
    deconstruct_maybe_packed_type(&t);
    return t;
}

typedef enum {
    OsInfix, OsPrefix, OsCall,
} OpStyle;

typedef enum {
    IsNone, // empty entry
    IsMono,
    IsPoly
} ISelMechanism;

typedef struct {
    ISelMechanism isel_mechanism;
    OpStyle style;
    String op;
    String u_ops[4];
    String s_ops[4];
    String f_ops[3];
} ISelTableEntry;

static const ISelTableEntry isel_dummy = { IsNone };

static const ISelTableEntry isel_table[PRIMOPS_COUNT] = {
    [add_op] = { IsMono, OsInfix,  "+" },
    [sub_op] = { IsMono, OsInfix,  "-" },
    [mul_op] = { IsMono, OsInfix,  "*" },
    [div_op] = { IsMono, OsInfix,  "/" },
    [mod_op] = { IsMono, OsInfix,  "%" },
    [neg_op] = { IsMono, OsPrefix, "-" },
    [gt_op] =  { IsMono, OsInfix,  ">" },
    [gte_op] = { IsMono, OsInfix,  ">=" },
    [lt_op] =  { IsMono, OsInfix,  "<"  },
    [lte_op] = { IsMono, OsInfix,  "<=" },
    [eq_op] =  { IsMono, OsInfix,  "==" },
    [neq_op] = { IsMono, OsInfix,  "!=" },
    [and_op] = { IsMono, OsInfix,  "&" },
    [or_op]  = { IsMono, OsInfix,  "|" },
    [xor_op] = { IsMono, OsInfix,  "^" },
    [not_op] = { IsMono, OsPrefix, "!" },
    /*[rshift_arithm_op] = { IsMono, OsInfix,  ">>" },
    [rshift_logical_op] = { IsMono, OsInfix,  ">>" }, // TODO achieve desired right shift semantics through unsigned/signed casts
    [lshift_op] = { IsMono, OsInfix,  "<<" },*/
};

static const ISelTableEntry isel_table_c[PRIMOPS_COUNT] = {
    [abs_op] = { IsPoly, OsCall, .s_ops = { "abs", "abs", "abs", "llabs" }, .f_ops = {"fabsf", "fabsf", "fabs"}},

    [sin_op] = { IsPoly, OsCall, .f_ops = {"sinf", "sinf", "sin"}},
    [cos_op] = { IsPoly, OsCall, .f_ops = {"cosf", "cosf", "cos"}},
    [floor_op] = { IsPoly, OsCall, .f_ops = {"floorf", "floorf", "floor"}},
    [ceil_op] = { IsPoly, OsCall, .f_ops = {"ceilf", "ceilf", "ceil"}},
    [round_op] = { IsPoly, OsCall, .f_ops = {"roundf", "roundf", "round"}},

    [sqrt_op] = { IsPoly, OsCall, .f_ops = {"sqrtf", "sqrtf", "sqrt"}},
    [exp_op] = { IsPoly, OsCall, .f_ops = {"expf", "expf", "exp"}},
    [pow_op] = { IsPoly, OsCall, .f_ops = {"powf", "powf", "pow"}},
};

static const ISelTableEntry isel_table_glsl[PRIMOPS_COUNT] = {
    [abs_op] = { IsMono, OsCall, "abs" },

    [sin_op] = { IsMono, OsCall, "sin" },
    [cos_op] = { IsMono, OsCall, "cos" },
    [floor_op] = { IsMono, OsCall, "floor" },
    [ceil_op] = { IsMono, OsCall, "ceil" },
    [round_op] = { IsMono, OsCall, "round" },

    [sqrt_op] = { IsMono, OsCall, "sqrt" },
    [exp_op] = { IsMono, OsCall, "exp" },
    [pow_op] = { IsMono, OsCall, "pow" },
};

static const ISelTableEntry isel_table_glsl_120[PRIMOPS_COUNT] = {
    [mod_op] = { IsMono, OsCall,  "mod" },

    [and_op] = { IsMono, OsCall,  "and" },
    [ or_op] = { IsMono, OsCall,   "or" },
    [xor_op] = { IsMono, OsCall,  "xor" },
    [not_op] = { IsMono, OsCall,  "not" },
};

static const ISelTableEntry isel_table_ispc[PRIMOPS_COUNT] = {
    [abs_op] = { IsMono, OsCall, "abs" },

    [sin_op] = { IsMono, OsCall, "sin" },
    [cos_op] = { IsMono, OsCall, "cos" },
    [floor_op] = { IsMono, OsCall, "floor" },
    [ceil_op] = { IsMono, OsCall, "ceil" },
    [round_op] = { IsMono, OsCall, "round" },

    [sqrt_op] = { IsMono, OsCall, "sqrt" },
    [exp_op] = { IsMono, OsCall, "exp" },
    [pow_op] = { IsMono, OsCall, "pow" },

    [subgroup_active_mask_op] = { IsMono, OsCall, "lanemask" },
    [subgroup_ballot_op] = { IsMono, OsCall, "packmask" },
    [subgroup_reduce_sum_op] = { IsMono, OsCall, "reduce_add" },
};

static bool emit_using_entry(CTerm* out, Emitter* emitter, Printer* p, const ISelTableEntry* entry, Nodes operands) {
    String operator_str = NULL;
    switch (entry->isel_mechanism) {
        case IsNone: return false;
        case IsMono: operator_str = entry->op; break;
        case IsPoly: {
            const Type* t = get_first_op_scalar_type(operands);
            if (t->tag == Float_TAG)
                operator_str = entry->f_ops[t->payload.float_type.width];
            else if (t->tag == Int_TAG && t->payload.int_type.is_signed)
                operator_str = entry->s_ops[t->payload.int_type.width];
            else if (t->tag == Int_TAG)
                operator_str = entry->u_ops[t->payload.int_type.width];
            break;
        }
    }

    if (!operator_str)
        return false;

    switch (entry->style) {
        case OsInfix: {
            CTerm a = emit_value(emitter, p, operands.nodes[0]);
            CTerm b = emit_value(emitter, p, operands.nodes[1]);
            *out = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s %s %s", to_cvalue(emitter, a), operator_str, to_cvalue(emitter, b)));
            break;
        }
        case OsPrefix: {
            CTerm operand = emit_value(emitter, p, operands.nodes[0]);
            *out = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s%s", operator_str, to_cvalue(emitter, operand)));
            break;
        }
        case OsCall: {
            LARRAY(CTerm, cops, operands.count);
            for (size_t i = 0; i < operands.count; i++)
                cops[i] = emit_value(emitter, p, operands.nodes[i]);
            if (operands.count == 1)
                *out = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s(%s)", operator_str, to_cvalue(emitter, cops[0])));
            else {
                Growy* g = new_growy();
                growy_append_string(g, operator_str);
                growy_append_string_literal(g, "(");
                for (size_t i = 0; i < operands.count; i++) {
                    growy_append_string(g, to_cvalue(emitter, cops[i]));
                    if (i + 1 < operands.count)
                        growy_append_string_literal(g, ", ");
                }
                growy_append_string_literal(g, ")");
                *out = term_from_cvalue(growy_deconstruct(g));
            }
            break;
        }
    }
    return true;
}

static const ISelTableEntry* lookup_entry(Emitter* emitter, Op op) {
    const ISelTableEntry* isel_entry = &isel_dummy;

    switch (emitter->config.dialect) {
        case CDialect_CUDA: /* TODO: do better than that */
        case CDialect_C11: isel_entry = &isel_table_c[op]; break;
        case CDialect_GLSL: isel_entry = &isel_table_glsl[op]; break;
        case CDialect_ISPC: isel_entry = &isel_table_ispc[op]; break;
    }

    if (emitter->config.dialect == CDialect_GLSL && emitter->config.glsl_version <= 120)
        isel_entry = &isel_table_glsl_120[op];

    if (isel_entry->isel_mechanism == IsNone)
        isel_entry = &isel_table[op];
    return isel_entry;
}

static String index_into_array(Emitter* emitter, const Type* arr_type, CTerm expr, CTerm index) {
    IrArena* arena = emitter->arena;

    String index2 = emitter->config.dialect == CDialect_GLSL ? format_string_arena(arena->arena, "int(%s)", to_cvalue(emitter, index)) : to_cvalue(emitter, index);
    if (emitter->config.decay_unsized_arrays && !arr_type->payload.arr_type.size)
        return format_string_arena(arena->arena, "((&%s)[%s])", deref_term(emitter, expr), index2);
    else
        return format_string_arena(arena->arena, "(%s.arr[%s])", deref_term(emitter, expr), index2);
}

static void emit_primop(Emitter* emitter, Printer* p, const Node* node, InstructionOutputs outputs) {
    assert(node->tag == PrimOp_TAG);
    IrArena* arena = emitter->arena;
    const PrimOp* prim_op = &node->payload.prim_op;
    CTerm term = term_from_cvalue(format_string_interned(emitter->arena, "/* todo %s */", get_primop_name(prim_op->op)));
    const ISelTableEntry* isel_entry = lookup_entry(emitter, prim_op->op);
    switch (prim_op->op) {
        case deref_op:
        case assign_op:
        case subscript_op: assert(false);
        case add_carry_op:
        case sub_borrow_op:
        case mul_extended_op:
            error("TODO: implement extended arithm ops in C");
            break;
        // MATH OPS
        case fract_op: {
            CTerm floored;
            emit_using_entry(&floored, emitter, p, lookup_entry(emitter, floor_op), prim_op->operands);
            term = term_from_cvalue(format_string_arena(arena->arena, "1 - %s", to_cvalue(emitter, floored)));
            break;
        }
        case inv_sqrt_op: {
            CTerm floored;
            emit_using_entry(&floored, emitter, p, lookup_entry(emitter, sqrt_op), prim_op->operands);
            term = term_from_cvalue(format_string_arena(arena->arena, "1.0f / %s", to_cvalue(emitter, floored)));
            break;
        }
        case min_op: {
            CValue a = to_cvalue(emitter, emit_value(emitter, p, first(prim_op->operands)));
            CValue b = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[1]));
            term = term_from_cvalue(format_string_arena(arena->arena, "(%s > %s ? %s : %s)", a, b, b, a));
            break;
        }
        case max_op: {
            CValue a = to_cvalue(emitter, emit_value(emitter, p, first(prim_op->operands)));
            CValue b = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[1]));
            term = term_from_cvalue(format_string_arena(arena->arena, "(%s > %s ? %s : %s)", a, b, a, b));
            break;
        }
        case sign_op: {
            CValue src = to_cvalue(emitter, emit_value(emitter, p, first(prim_op->operands)));
            term = term_from_cvalue(format_string_arena(arena->arena, "(%s > 0 ? 1 : -1)", src));
            break;
        }
        case fma_op: {
            CValue a = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[0]));
            CValue b = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[1]));
            CValue c = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[2]));
            switch (emitter->config.dialect) {
                case CDialect_C11:
                case CDialect_CUDA: {
                    term = term_from_cvalue(format_string_arena(arena->arena, "fmaf(%s, %s, %s)", a, b, c));
                    break;
                }
                default: {
                    term = term_from_cvalue(format_string_arena(arena->arena, "(%s * %s) + %s", a, b, c));
                    break;
                }
            }
            break;
        }
        case lshift_op:
        case rshift_arithm_op:
        case rshift_logical_op: {
            CValue src = to_cvalue(emitter, emit_value(emitter, p, first(prim_op->operands)));
            const Node* offset = prim_op->operands.nodes[1];
            CValue c_offset = to_cvalue(emitter, emit_value(emitter, p, offset));
            if (emitter->config.dialect == CDialect_GLSL) {
                if (get_unqualified_type(offset->type)->payload.int_type.width == IntTy64)
                    c_offset = format_string_arena(arena->arena, "int(%s)", c_offset);
            }
            term = term_from_cvalue(format_string_arena(arena->arena, "(%s %s %s)", src, prim_op->op == lshift_op ? "<<" : ">>", c_offset));
            break;
        }
        case size_of_op:
            term = term_from_cvalue(format_string_arena(emitter->arena->arena, "sizeof(%s)", c_emit_type(emitter, first(prim_op->type_arguments), NULL)));
            break;
        case align_of_op:
            term = term_from_cvalue(format_string_arena(emitter->arena->arena, "alignof(%s)", c_emit_type(emitter, first(prim_op->type_arguments), NULL)));
            break;
        case offset_of_op: {
            const Type* t = first(prim_op->type_arguments);
            while (t->tag == TypeDeclRef_TAG) {
                t = get_nominal_type_body(t);
            }
            const Node* index = first(prim_op->operands);
            uint64_t index_literal = get_int_literal_value(*resolve_to_int_literal(index), false);
            String member_name = get_record_field_name(t, index_literal);
            term = term_from_cvalue(format_string_arena(emitter->arena->arena, "offsetof(%s, %s)", c_emit_type(emitter, t, NULL), member_name));
            break;
        } case select_op: {
            assert(prim_op->operands.count == 3);
            CValue condition = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[0]));
            CValue l = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[1]));
            CValue r = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[2]));
            term = term_from_cvalue(format_string_arena(emitter->arena->arena, "(%s) ? (%s) : (%s)", condition, l, r));
            break;
        }
        case convert_op: {
            assert(outputs.count == 1);
            CTerm src = emit_value(emitter, p, first(prim_op->operands));
            const Type* src_type = get_unqualified_type(first(prim_op->operands)->type);
            const Type* dst_type = first(prim_op->type_arguments);
            if (emitter->config.dialect == CDialect_GLSL) {
                if (is_glsl_scalar_type(src_type) && is_glsl_scalar_type(dst_type)) {
                    CType t = emit_type(emitter, dst_type, NULL);
                    term = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s(%s)", t, to_cvalue(emitter, src)));
                } else
                    assert(false);
            } else {
                CType t = emit_type(emitter, dst_type, NULL);
                term = term_from_cvalue(format_string_arena(emitter->arena->arena, "((%s) %s)", t, to_cvalue(emitter, src)));
            }
            break;
        }
        case reinterpret_op: {
            assert(outputs.count == 1);
            CTerm src_value = emit_value(emitter, p, first(prim_op->operands));
            const Type* src_type = get_unqualified_type(first(prim_op->operands)->type);
            const Type* dst_type = first(prim_op->type_arguments);
            switch (emitter->config.dialect) {
                case CDialect_CUDA:
                case CDialect_C11: {
                    String src = unique_name(arena, "bitcast_src");
                    String dst = unique_name(arena, "bitcast_result");
                    print(p, "\n%s = %s;", emit_type(emitter, src_type, src), to_cvalue(emitter, src_value));
                    print(p, "\n%s;", emit_type(emitter, dst_type, dst));
                    print(p, "\nmemcpy(&%s, &%s, sizeof(%s));", dst, src, src);
                    outputs.results[0] = term_from_cvalue(dst);
                    outputs.binding[0] = NoBinding;
                    break;
                }
                case CDialect_GLSL: {
                    String n = NULL;
                    if (dst_type->tag == Float_TAG) {
                        assert(src_type->tag == Int_TAG);
                        switch (dst_type->payload.float_type.width) {
                            case FloatTy16: break;
                            case FloatTy32: n = src_type->payload.int_type.is_signed ? "intBitsToFloat" : "uintBitsToFloat";
                                break;
                            case FloatTy64: break;
                        }
                    } else if (dst_type->tag == Int_TAG) {
                        if (src_type->tag == Int_TAG) {
                            outputs.results[0] = src_value;
                            outputs.binding[0] = NoBinding;
                            break;
                        }
                        assert(src_type->tag == Float_TAG);
                        switch (src_type->payload.float_type.width) {
                            case FloatTy16: break;
                            case FloatTy32: n = dst_type->payload.int_type.is_signed ? "floatBitsToInt" : "floatBitsToUint";
                                break;
                            case FloatTy64: break;
                        }
                    }
                    if (n) {
                        outputs.results[0] = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s(%s)", n, to_cvalue(emitter, src_value)));
                        outputs.binding[0] = LetBinding;
                        break;
                    }
                    error_print("glsl: unsupported bit cast from ");
                    log_node(ERROR, src_type);
                    error_print(" to ");
                    log_node(ERROR, dst_type);
                    error_print(".\n");
                    error_die();
                }
                case CDialect_ISPC: {
                    if (dst_type->tag == Float_TAG) {
                        assert(src_type->tag == Int_TAG);
                        String n;
                        switch (dst_type->payload.float_type.width) {
                            case FloatTy16: n = "float16bits";
                                break;
                            case FloatTy32: n = "floatbits";
                                break;
                            case FloatTy64: n = "doublebits";
                                break;
                        }
                        outputs.results[0] = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s(%s)", n, to_cvalue(emitter, src_value)));
                        outputs.binding[0] = LetBinding;
                        break;
                    } else if (src_type->tag == Float_TAG) {
                        assert(dst_type->tag == Int_TAG);
                        outputs.results[0] = term_from_cvalue(format_string_arena(emitter->arena->arena, "intbits(%s)", to_cvalue(emitter, src_value)));
                        outputs.binding[0] = LetBinding;
                        break;
                    }

                    CType t = emit_type(emitter, dst_type, NULL);
                    outputs.results[0] = term_from_cvalue(format_string_arena(emitter->arena->arena, "((%s) %s)", t, to_cvalue(emitter, src_value)));
                    outputs.binding[0] = NoBinding;
                    break;
                }
            }
            return;
        }
        case insert_op:
        case extract_dynamic_op:
        case extract_op: {
            CValue acc = to_cvalue(emitter, emit_value(emitter, p, first(prim_op->operands)));
            bool insert = prim_op->op == insert_op;

            if (insert) {
                String dst = unique_name(arena, "modified");
                print(p, "\n%s = %s;", c_emit_type(emitter, node->type, dst), acc);
                acc = dst;
                term = term_from_cvalue(dst);
            }

            const Type* t = get_unqualified_type(first(prim_op->operands)->type);
            for (size_t i = (insert ? 2 : 1); i < prim_op->operands.count; i++) {
                const Node* index = prim_op->operands.nodes[i];
                const IntLiteral* static_index = resolve_to_int_literal(index);

                switch (is_type(t)) {
                    case Type_TypeDeclRef_TAG: {
                        const Node* decl = t->payload.type_decl_ref.decl;
                        assert(decl && decl->tag == NominalType_TAG);
                        t = decl->payload.nom_type.body;
                        SHADY_FALLTHROUGH
                    }
                    case Type_RecordType_TAG: {
                        assert(static_index);
                        Strings names = t->payload.record_type.names;
                        if (names.count == 0)
                            acc = format_string_arena(emitter->arena->arena, "(%s._%d)", acc, static_index->value);
                        else
                            acc = format_string_arena(emitter->arena->arena, "(%s.%s)", acc, names.strings[static_index->value]);
                        break;
                    }
                    case Type_PackType_TAG: {
                        assert(static_index);
                        assert(static_index->value < 4 && static_index->value < t->payload.pack_type.width);
                        String suffixes = "xyzw";
                        acc = format_string_arena(emitter->arena->arena, "(%s.%c)", acc, suffixes[static_index->value]);
                        break;
                    }
                    case Type_ArrType_TAG: {
                        acc = index_into_array(emitter, t, term_from_cvar(acc), emit_value(emitter, p, index));
                        break;
                    }
                    default:
                    case NotAType: error("Must be a type");
                }
            }

            if (insert) {
                print(p, "\n%s = %s;", acc, to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[1])));
                break;
            }

            term = term_from_cvalue(acc);
            break;
        }
        case shuffle_op: {
            String dst = unique_name(arena, "shuffled");
            const Node* lhs = prim_op->operands.nodes[0];
            const Node* rhs = prim_op->operands.nodes[1];
            String lhs_e = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[0]));
            String rhs_e = to_cvalue(emitter, emit_value(emitter, p, prim_op->operands.nodes[1]));
            const Type* lhs_t = lhs->type;
            const Type* rhs_t = rhs->type;
            bool lhs_u = deconstruct_qualified_type(&lhs_t);
            bool rhs_u = deconstruct_qualified_type(&rhs_t);
            size_t left_size = lhs_t->payload.pack_type.width;
            // size_t total_size = lhs_t->payload.pack_type.width + rhs_t->payload.pack_type.width;
            String suffixes = "xyzw";
            print(p, "\n%s = vec%d(", c_emit_type(emitter, node->type, dst), prim_op->operands.count - 2);
            for (size_t i = 2; i < prim_op->operands.count; i++) {
                const IntLiteral* selector = resolve_to_int_literal(prim_op->operands.nodes[i]);
                if (selector->value < left_size)
                    print(p, "%s.%c\n", lhs_e, suffixes[selector->value]);
                else
                    print(p, "%s.%c\n", rhs_e, suffixes[selector->value - left_size]);
                if (i + 1 < prim_op->operands.count)
                    print(p, ", ");
            }
            print(p, ");\n");
            term = term_from_cvalue(dst);
            break;
        }
        case default_join_point_op:
        case create_joint_point_op: error("lowered in lower_tailcalls.c");
        case subgroup_elect_first_op: {
            switch (emitter->config.dialect) {
                case CDialect_CUDA: term = term_from_cvalue(format_string_arena(emitter->arena->arena, "__shady_elect_first()")); break;
                case CDialect_ISPC: term = term_from_cvalue(format_string_arena(emitter->arena->arena, "(programIndex == count_trailing_zeros(lanemask()))")); break;
                case CDialect_C11:
                case CDialect_GLSL: error("TODO")
            }
            break;
        }
        case subgroup_assume_uniform_op: {
            if (emitter->config.dialect != CDialect_ISPC) {
                outputs.results[0] = emit_value(emitter, p, prim_op->operands.nodes[0]);
                outputs.binding[0] = NoBinding;
                return;
            }
        }
        case subgroup_broadcast_first_op: {
            CValue value = to_cvalue(emitter, emit_value(emitter, p, first(prim_op->operands)));
            switch (emitter->config.dialect) {
                case CDialect_CUDA: term = term_from_cvalue(format_string_arena(emitter->arena->arena, "__shady_broadcast_first(%s)", value)); break;
                case CDialect_ISPC: term = term_from_cvalue(format_string_arena(emitter->arena->arena, "extract(%s, count_trailing_zeros(lanemask()))", value)); break;
                case CDialect_C11:
                case CDialect_GLSL: error("TODO")
            }
            break;
        }
        case empty_mask_op:
        case mask_is_thread_active_op: error("lower_me");
        default: break;
        case PRIMOPS_COUNT: assert(false); break;
    }

    if (isel_entry->isel_mechanism != IsNone)
        emit_using_entry(&term, emitter, p, isel_entry, prim_op->operands);

    assert(outputs.count == 1);
    outputs.binding[0] = LetBinding;
    outputs.results[0] = term;
    return;
}

static void emit_call(Emitter* emitter, Printer* p, const Node* call, InstructionOutputs outputs) {
    Nodes args;
    if (call->tag == Call_TAG)
        args = call->payload.call.args;
    else
        assert(false);

    Growy* g = new_growy();
    Printer* paramsp = open_growy_as_printer(g);
    if (emitter->use_private_globals) {
        print(paramsp, "__shady_private_globals");
        if (args.count > 0)
            print(paramsp, ", ");
    }
    for (size_t i = 0; i < args.count; i++) {
        print(paramsp, to_cvalue(emitter, emit_value(emitter, p, args.nodes[i])));
        if (i + 1 < args.count)
            print(paramsp, ", ");
    }

    CValue e_callee;
    const Node* callee = call->payload.call.callee;
    if (callee->tag == FnAddr_TAG)
        e_callee = get_declaration_name(callee->payload.fn_addr.fn);
    else
        e_callee = to_cvalue(emitter, emit_value(emitter, p, callee));

    String params = printer_growy_unwrap(paramsp);

    Nodes yield_types = unwrap_multiple_yield_types(emitter->arena, call->type);
    assert(yield_types.count == outputs.count);
    if (yield_types.count > 1) {
        String named = unique_name(emitter->arena, "result");
        print(p, "\n%s = %s(%s);", emit_type(emitter, call->type, named), e_callee, params);
        for (size_t i = 0; i < yield_types.count; i++) {
            outputs.results[i] = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s->_%d", named, i));
            // we have let-bound the actual result already, and extracting their components can be done inline
            outputs.binding[i] = NoBinding;
        }
    } else if (yield_types.count == 1) {
        outputs.results[0] = term_from_cvalue(format_string_arena(emitter->arena->arena, "%s(%s)", e_callee, params));
        outputs.binding[0] = LetBinding;
    } else {
        print(p, "\n%s(%s);", e_callee, params);
    }
    free_tmp_str(params);
}

static void emit_lea(Emitter* emitter, Printer* p, Lea lea, InstructionOutputs outputs) {
    IrArena* arena = emitter->arena;
    CTerm acc = emit_value(emitter, p, lea.ptr);

    const Type* src_qtype = lea.ptr->type;
    bool uniform = is_qualified_type_uniform(src_qtype);
    const Type* curr_ptr_type = get_unqualified_type(src_qtype);
    assert(curr_ptr_type->tag == PtrType_TAG);

    const IntLiteral* offset_static_value = resolve_to_int_literal(lea.offset);
    if (!offset_static_value || offset_static_value->value != 0) {
        CTerm offset = emit_value(emitter, p, lea.offset);
        // we sadly need to drop to the value level (aka explicit pointer arithmetic) to do this
        // this means such code is never going to be legal in GLSL
        // also the cast is to account for our arrays-in-structs hack
        const Type* pointee_type = get_pointee_type(arena, curr_ptr_type);
        acc = term_from_cvalue(format_string_arena(arena->arena, "((%s) &(%s)[%s])", emit_type(emitter, curr_ptr_type, NULL), to_cvalue(emitter, acc), to_cvalue(emitter, offset)));
        uniform &= is_qualified_type_uniform(lea.offset->type);
    }

    //t = t->payload.ptr_type.pointed_type;
    for (size_t i = 0; i < lea.indices.count; i++) {
        const Type* pointee_type = get_pointee_type(arena, curr_ptr_type);
        const Node* selector = lea.indices.nodes[i];
        uniform &= is_qualified_type_uniform(selector->type);
        switch (is_type(pointee_type)) {
            case ArrType_TAG: {
                CTerm index = emit_value(emitter, p, selector);
                acc = term_from_cvar(index_into_array(emitter, pointee_type, acc, index));
                curr_ptr_type = ptr_type(arena, (PtrType) {
                        .pointed_type = pointee_type->payload.arr_type.element_type,
                        .address_space = curr_ptr_type->payload.ptr_type.address_space
                });
                break;
            }
            case TypeDeclRef_TAG: {
                pointee_type = get_nominal_type_body(pointee_type);
                SHADY_FALLTHROUGH
            }
            case RecordType_TAG: {
                // yet another ISPC bug and workaround
                // ISPC cannot deal with subscripting if you've done pointer arithmetic (!) inside the expression
                // so hum we just need to introduce a temporary variable to hold the pointer expression so far, and go again from there
                // See https://github.com/ispc/ispc/issues/2496
                if (emitter->config.dialect == CDialect_ISPC) {
                    String interm = unique_name(arena, "lea_intermediary_ptr_value");
                    print(p, "\n%s = %s;", emit_type(emitter, qualified_type_helper(curr_ptr_type, uniform), interm), to_cvalue(emitter, acc));
                    acc = term_from_cvalue(interm);
                }

                assert(selector->tag == IntLiteral_TAG && "selectors when indexing into a record need to be constant");
                size_t static_index = get_int_literal_value(*resolve_to_int_literal(selector), false);
                String field_name = get_record_field_name(pointee_type, static_index);
                acc = term_from_cvar(format_string_arena(arena->arena, "(%s.%s)", deref_term(emitter, acc), field_name));
                curr_ptr_type = ptr_type(arena, (PtrType) {
                        .pointed_type = pointee_type->payload.record_type.members.nodes[static_index],
                        .address_space = curr_ptr_type->payload.ptr_type.address_space
                });
                break;
            }
            case Type_PackType_TAG: {
                size_t static_index = get_int_literal_value(*resolve_to_int_literal(selector), false);
                String suffixes = "xyzw";
                acc = term_from_cvar(format_string_arena(emitter->arena->arena, "(%s.%c)", deref_term(emitter, acc), suffixes[static_index]));
                curr_ptr_type = ptr_type(arena, (PtrType) {
                        .pointed_type = pointee_type->payload.pack_type.element_type,
                        .address_space = curr_ptr_type->payload.ptr_type.address_space
                });
                break;
            }
            default: error("lea can't work on this");
        }
    }
    assert(outputs.count == 1);
    outputs.results[0] = acc;
    outputs.binding[0] = emitter->config.dialect == CDialect_ISPC ? LetBinding : NoBinding;
    outputs.binding[0] = NoBinding;
    return;
}

static void emit_alloca(Emitter* emitter, Printer* p, const Type* type, InstructionOutputs outputs) {
    assert(outputs.count == 1);
    String variable_name = unique_name(emitter->arena, "alloca");
    CTerm variable = (CTerm) { .value = NULL, .var = variable_name };
    emit_variable_declaration(emitter, p, type, variable_name, true, NULL);
    outputs.results[0] = variable;
    if (emitter->config.dialect == CDialect_ISPC) {
        outputs.results[0] = ispc_varying_ptr_helper(emitter, p, type, variable);
    }
    outputs.binding[0] = NoBinding;
}

void emit_instruction(Emitter* emitter, Printer* p, const Node* instruction, InstructionOutputs outputs) {
    assert(is_instruction(instruction));
    IrArena* a = emitter->arena;

    switch (is_instruction(instruction)) {
        case NotAnInstruction: assert(false);
        case Instruction_PushStack_TAG:
        case Instruction_PopStack_TAG:
        case Instruction_GetStackSize_TAG:
        case Instruction_SetStackSize_TAG:
        case Instruction_GetStackBaseAddr_TAG: error("Stack operations need to be lowered.");
        case Instruction_BindIdentifiers_TAG:       error("front-end only!");
        case Instruction_PrimOp_TAG:       emit_primop(emitter, p, instruction, outputs); break;
        case Instruction_Call_TAG:         emit_call  (emitter, p, instruction, outputs); break;
        case Instruction_CompoundInstruction_TAG: {
            Nodes instructions = instruction->payload.compound_instruction.instructions;
            for (size_t i = 0; i < instructions.count; i++) {
                const Node* instruction2 = instructions.nodes[i];

                // we declare N local variables in order to store the result of the instruction
                Nodes yield_types = unwrap_multiple_yield_types(emitter->arena, instruction2->type);

                LARRAY(CTerm, results, yield_types.count);
                LARRAY(InstrResultBinding, bindings, yield_types.count);
                InstructionOutputs ioutputs = {
                    .count = yield_types.count,
                    .results = results,
                    .binding = bindings,
                };

                emit_instruction(emitter, p, instruction2, ioutputs);
            }
            Nodes results2 = instruction->payload.compound_instruction.results;
            for (size_t i = 0; i < results2.count; i++) {
                outputs.results[0] = emit_value(emitter, p, results2.nodes[i]);
                outputs.binding[0] = NoBinding;
            }
            return;
        }
        case Instruction_Block_TAG:        error("Should be eliminated by the compiler")
        case Instruction_Comment_TAG:      print(p, "/* %s */", instruction->payload.comment.string); break;
        case Instruction_StackAlloc_TAG: return emit_alloca(emitter, p, instruction->payload.stack_alloc.type, outputs);
        case Instruction_LocalAlloc_TAG: return emit_alloca(emitter, p, instruction->payload.local_alloc.type, outputs);
        case Instruction_Load_TAG: {
            Load payload = instruction->payload.load;
            CAddr dereferenced = deref_term(emitter, emit_value(emitter, p, payload.ptr));
            outputs.results[0] = term_from_cvalue(dereferenced);
            outputs.binding[0] = LetBinding;
            return;
        }
        case Instruction_Store_TAG: {
            Store payload = instruction->payload.store;
            const Type* addr_type = payload.ptr->type;
            bool addr_uniform = deconstruct_qualified_type(&addr_type);
            bool value_uniform = is_qualified_type_uniform(payload.value->type);
            assert(addr_type->tag == PtrType_TAG);
            CAddr dereferenced = deref_term(emitter, emit_value(emitter, p, payload.ptr));
            CValue cvalue = to_cvalue(emitter, emit_value(emitter, p, payload.value));
            // ISPC lets you broadcast to a uniform address space iff the address is non-uniform, otherwise we need to do this
            if (emitter->config.dialect == CDialect_ISPC && addr_uniform && is_addr_space_uniform(a, addr_type->payload.ptr_type.address_space) && !value_uniform)
                cvalue = format_string_arena(emitter->arena->arena, "extract(%s, count_trailing_zeros(lanemask()))", cvalue);

            print(p, "\n%s = %s;", dereferenced, cvalue);
            return;
        }
        case Instruction_Lea_TAG:
            emit_lea(emitter, p, instruction->payload.lea, outputs);
            return;
        case Instruction_CopyBytes_TAG: {
            CopyBytes payload = instruction->payload.copy_bytes;
            print(p, "\nmemcpy(%s, %s, %s);", to_cvalue(emitter, c_emit_value(emitter, p, payload.dst)), to_cvalue(emitter, c_emit_value(emitter, p, payload.src)), to_cvalue(emitter, c_emit_value(emitter, p, payload.count)));
            return;
        }
        case Instruction_FillBytes_TAG:{
            FillBytes payload = instruction->payload.fill_bytes;
            print(p, "\nmemset(%s, %s, %s);", to_cvalue(emitter, c_emit_value(emitter, p, payload.dst)), to_cvalue(emitter, c_emit_value(emitter, p, payload.src)), to_cvalue(emitter, c_emit_value(emitter, p, payload.count)));
            return;
        }
        case Instruction_DebugPrintf_TAG: {
            String args_list = format_string_interned(emitter->arena, "\"%s\"", instruction->payload.debug_printf.string);
            for (size_t i = 0; i < instruction->payload.debug_printf.args.count; i++) {
                CValue str = to_cvalue(emitter, emit_value(emitter, p, instruction->payload.debug_printf.args.nodes[i]));

                if (emitter->config.dialect == CDialect_ISPC && i > 0)
                    str = format_string_arena(emitter->arena->arena, "extract(%s, printf_thread_index)", str);

                args_list = format_string_arena(emitter->arena->arena, "%s, %s", args_list, str);
            }
            switch (emitter->config.dialect) {
                case CDialect_ISPC:
                    print(p, "\nforeach_active(printf_thread_index) { print(%s); }", args_list);
                    break;
                case CDialect_CUDA:
                case CDialect_C11:
                    print(p, "\nprintf(%s);", args_list);
                    break;
                case CDialect_GLSL: warn_print("printf is not supported in GLSL");
                    break;
            }

            return;
        }
    }
}
