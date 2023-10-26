#include "l2s_private.h"

#include "portability.h"
#include "log.h"

#include "llvm-c/DebugInfo.h"

static Nodes convert_mdnode_operands(Parser* p, LLVMValueRef mdnode) {
    IrArena* a = get_module_arena(p->dst);
    assert(LLVMIsAMDNode(mdnode));

    unsigned count = LLVMGetMDNodeNumOperands(mdnode);
    LARRAY(LLVMValueRef, ops, count);
    LLVMGetMDNodeOperands(mdnode, ops);

    LARRAY(const Node*, cops, count);
    for (size_t i = 0; i < count; i++)
        cops[i] = ops[i] ? convert_value(p, ops[i]) : string_lit_helper(a, "null");
    Nodes args = nodes(a, count, cops);
    return args;
}

static const Node* convert_named_tuple_metadata(Parser* p, LLVMValueRef v, String name) {
    // printf("%s\n", name);
    IrArena* a = get_module_arena(p->dst);
    Nodes args = convert_mdnode_operands(p, v);
    args = prepend_nodes(a, args, string_lit_helper(a, name));
    return tuple_helper(a, args);
}

#define LLVM_DI_METADATA_NODES(N) \
N(DILocation)\
N(DIExpression)\
N(DIGlobalVariableExpression)\
N(GenericDINode)\
N(DISubrange)\
N(DIEnumerator)\
N(DIBasicType)\
N(DIDerivedType)\
N(DICompositeType)\
N(DISubroutineType)\
N(DIFile)\
N(DICompileUnit)\
N(DISubprogram)\
N(DILexicalBlock)\
N(DILexicalBlockFile)\
N(DINamespace)\
N(DIModule)\
N(DITemplateTypeParameter)\
N(DITemplateValueParameter)\
N(DIGlobalVariable)\
N(DILocalVariable)\
N(DILabel)\
N(DIObjCProperty)\
N(DIImportedEntity)\
N(DIMacro)\
N(DIMacroFile)\
N(DICommonBlock)\
N(DIStringType)\
N(DIGenericSubrange)\
N(DIArgList)\

// braindead solution to a braindead problem
#define LLVM_DI_WITH_PARENT_SCOPES(N) \
N(DIBasicType)\
N(DIDerivedType)\
N(DICompositeType)\
N(DISubroutineType)\
N(DISubprogram)\
N(DILexicalBlock)\
N(DILexicalBlockFile)\
N(DINamespace)\
N(DIModule)\
N(DICommonBlock)                      \

static LLVMValueRef shady_LLVMGetParentScope(Parser* p, LLVMMetadataRef meta) {
    LLVMMetadataKind kind = LLVMGetMetadataKind(meta);
    LLVMValueRef v = LLVMMetadataAsValue(p->ctx, meta);

    switch (kind) {
#define N(e) case LLVM##e##MetadataKind: break;
LLVM_DI_WITH_PARENT_SCOPES(N)
#undef N
        default: return NULL;
    }

    unsigned count = LLVMGetMDNodeNumOperands(v);
    LARRAY(LLVMValueRef, ops, count);
    LLVMGetMDNodeOperands(v, ops);

    assert(count >= 2);
    return ops[1];
}

Nodes scope_to_string(Parser* p, LLVMMetadataRef dbgloc) {
    IrArena* a = get_module_arena(p->dst);
    Nodes str = empty(a);

    LLVMMetadataRef scope = LLVMDILocationGetScope(dbgloc);
    while (true) {
        if (!scope) break;

        str = prepend_nodes(a, str, convert_metadata(p, scope));

        // LLVMDumpValue(LLVMMetadataAsValue(p->ctx, scope));
        // printf("\n");

        LLVMValueRef v = shady_LLVMGetParentScope(p, scope);
        if (!v) break;
        scope = LLVMValueAsMetadata(v);
    }
    //dump_node(convert_metadata(p, dbgloc));
    return str;
}

const Node* convert_metadata(Parser* p, LLVMMetadataRef meta) {
    IrArena* a = get_module_arena(p->dst);
    LLVMMetadataKind kind = LLVMGetMetadataKind(meta);
    LLVMValueRef v = LLVMMetadataAsValue(p->ctx, meta);

    switch (kind) {
        case LLVMMDTupleMetadataKind: return tuple_helper(a, convert_mdnode_operands(p, v));
        case LLVMDICompileUnitMetadataKind: return string_lit_helper(a, "CompileUnit");
    }

    switch (kind) {
        case LLVMMDStringMetadataKind: {
            unsigned l;
            String name = LLVMGetMDString(v, &l);
            return string_lit_helper(a, name);
        }
        case LLVMConstantAsMetadataMetadataKind:
        case LLVMLocalAsMetadataMetadataKind: {
            Nodes ops = convert_mdnode_operands(p, v);
            assert(ops.count == 1);
            return first(ops);
        }
        case LLVMDistinctMDOperandPlaceholderMetadataKind: goto default_;

#define N(e) case LLVM##e##MetadataKind: return convert_named_tuple_metadata(p, v, #e);
LLVM_DI_METADATA_NODES(N)
#undef N
        default: default_:
            error_print("Unknown metadata kind %d for ", kind);
            LLVMDumpValue(v);
            error_print(".\n");
            error_die();
    }
}
