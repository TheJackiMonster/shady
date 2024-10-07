#include "ir_private.h"

#include "log.h"
#include "portability.h"
#include "dict.h"

#include <string.h>
#include <assert.h>

String shd_get_value_name_unsafe(const Node* v) {
    assert(v && is_value(v));
    if (v->tag == Param_TAG)
        return v->payload.param.name;
    return NULL;
}

String shd_get_value_name_safe(const Node* v) {
    String name = shd_get_value_name_unsafe(v);
    if (name && strlen(name) > 0)
        return name;
    //if (v->tag == Variable_TAG)
    return shd_fmt_string_irarena(v->arena, "%%%d", v->id);
    //return node_tags[v->tag];
}

void shd_set_value_name(const Node* var, String name) {
    // TODO: annotations
    // if (var->tag == Variablez_TAG)
    //     var->payload.varz.name = string(var->arena, name);
}

static bool is_zero(const Node* node) {
    const IntLiteral* lit = shd_resolve_to_int_literal(node);
    if (lit && shd_get_int_literal_value(*lit, false) == 0)
        return true;
    return false;
}

const Node* shd_chase_ptr_to_source(const Node* ptr, NodeResolveConfig config) {
    while (true) {
        ptr = shd_resolve_node_to_definition(ptr, config);
        switch (ptr->tag) {
            case PtrArrayElementOffset_TAG: break;
            case PtrCompositeElement_TAG: {
                PtrCompositeElement payload = ptr->payload.ptr_composite_element;
                if (!is_zero(payload.index))
                    break;
                ptr = payload.ptr;
                continue;
            }
            case PrimOp_TAG: {
                switch (ptr->payload.prim_op.op) {
                    case convert_op: {
                        // chase generic pointers to their source
                        if (shd_first(ptr->payload.prim_op.type_arguments)->tag == PtrType_TAG) {
                            ptr = shd_first(ptr->payload.prim_op.operands);
                            continue;
                        }
                        break;
                    }
                    case reinterpret_op: {
                        // chase ptr casts to their source
                        // TODO: figure out round-trips through integer casts?
                        if (shd_first(ptr->payload.prim_op.type_arguments)->tag == PtrType_TAG) {
                            ptr = shd_first(ptr->payload.prim_op.operands);
                            continue;
                        }
                        break;
                    }
                    default: break;
                }
                break;
            }
            default: break;
        }
        break;
    }
    return ptr;
}

const Node* shd_resolve_ptr_to_value(const Node* ptr, NodeResolveConfig config) {
    while (ptr) {
        ptr = shd_resolve_node_to_definition(ptr, config);
        switch (ptr->tag) {
            case PrimOp_TAG: {
                switch (ptr->payload.prim_op.op) {
                    case convert_op: { // allow address space conversions
                        ptr = shd_first(ptr->payload.prim_op.operands);
                        continue;
                    }
                    default: break;
                }
            }
            case GlobalVariable_TAG:
                if (config.assume_globals_immutability)
                    return ptr->payload.global_variable.init;
                break;
            default: break;
        }
        ptr = NULL;
    }
    return NULL;
}

NodeResolveConfig shd_default_node_resolve_config(void) {
    return (NodeResolveConfig) {
        .enter_loads = true,
        .allow_incompatible_types = false,
        .assume_globals_immutability = false,
    };
}

const Node* shd_resolve_node_to_definition(const Node* node, NodeResolveConfig config) {
    while (node) {
        switch (node->tag) {
            case Constant_TAG:
                node = node->payload.constant.value;
                continue;
            case RefDecl_TAG:
                node = node->payload.ref_decl.decl;
                continue;
            case Load_TAG: {
                if (config.enter_loads) {
                    const Node* source = node->payload.load.ptr;
                    const Node* result = shd_resolve_ptr_to_value(source, config);
                    if (!result)
                        break;
                    node = result;
                    continue;
                }
            }
            case PrimOp_TAG: {
                switch (node->payload.prim_op.op) {
                    case convert_op:
                    case reinterpret_op: {
                        if (config.allow_incompatible_types) {
                            node = shd_first(node->payload.prim_op.operands);
                            continue;
                        }
                    }
                    default: break;
                }
                break;
            }
            default: break;
        }
        break;
    }
    return node;
}

const char* shd_get_string_literal(IrArena* arena, const Node* node) {
    if (!node)
        return NULL;
    if (node->type && get_unqualified_type(node->type)->tag == PtrType_TAG) {
        NodeResolveConfig nrc = shd_default_node_resolve_config();
        const Node* ptr = shd_chase_ptr_to_source(node, nrc);
        const Node* value = shd_resolve_ptr_to_value(ptr, nrc);
        if (value)
            return shd_get_string_literal(arena, value);
    }
    switch (node->tag) {
        case Declaration_GlobalVariable_TAG: {
            const Node* init = node->payload.global_variable.init;
            if (init) {
                return shd_get_string_literal(arena, init);
            }
            break;
        }
        case Declaration_Constant_TAG: {
            return shd_get_string_literal(arena, node->payload.constant.value);
        }
        case RefDecl_TAG: {
            const Node* decl = node->payload.ref_decl.decl;
            return shd_get_string_literal(arena, decl);
        }
        /*case Lea_TAG: {
            Lea lea = node->payload.lea;
            if (lea.indices.count == 3 && is_zero(lea.offset) && is_zero(first(lea.indices))) {
                const Node* ref = lea.ptr;
                if (ref->tag != RefDecl_TAG)
                    return NULL;
                const Node* decl = ref->payload.ref_decl.decl;
                if (decl->tag != GlobalVariable_TAG || !decl->payload.global_variable.init)
                    return NULL;
                return get_string_literal(arena, decl->payload.global_variable.init);
            }
            break;
        }*/
        case StringLiteral_TAG: return node->payload.string_lit.string;
        case Composite_TAG: {
            Nodes contents = node->payload.composite.contents;
            LARRAY(char, chars, contents.count);
            for (size_t i = 0; i < contents.count; i++) {
                const Node* value = contents.nodes[i];
                assert(value->tag == IntLiteral_TAG && value->payload.int_literal.width == IntTy8);
                chars[i] = (unsigned char) shd_get_int_literal_value(*shd_resolve_to_int_literal(value), false);
            }
            assert(chars[contents.count - 1] == 0);
            return string(arena, chars);
        }
        default: break;
    }
    return NULL;
}

const char* node_tags[];

const char* shd_get_node_tag_string(NodeTag tag) {
    return node_tags[tag];
}

const bool node_type_has_payload[];

KeyHash _shd_hash_node_payload(const Node* node);

KeyHash shd_hash_node(Node** pnode) {
    const Node* node = *pnode;
    KeyHash combined;

    if (shd_is_node_nominal(node)) {
        size_t ptr = (size_t) node;
        uint32_t upper = ptr >> 32;
        uint32_t lower = ptr;
        combined = upper ^ lower;
        goto end;
    }

    KeyHash tag_hash = shd_hash_murmur(&node->tag, sizeof(NodeTag));
    KeyHash payload_hash = 0;

    if (node_type_has_payload[node->tag]) {
        payload_hash = _shd_hash_node_payload(node);
    }
    combined = tag_hash ^ payload_hash;

    end:
    return combined;
}

bool _shd_compare_node_payload(const Node*, const Node*);

bool shd_compare_node(Node** pa, Node** pb) {
    if ((*pa)->tag != (*pb)->tag) return false;
    if (shd_is_node_nominal((*pa)))
        return *pa == *pb;

    const Node* a = *pa;
    const Node* b = *pb;

    #undef field
    #define field(w) eq &= memcmp(&a->payload.w, &b->payload.w, sizeof(a->payload.w)) == 0;

    if (node_type_has_payload[a->tag]) {
        return _shd_compare_node_payload(a, b);
    } else return true;
}

#include "node_generated.c"
