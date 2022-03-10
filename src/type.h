#ifndef SHADY_TYPE_H
#define SHADY_TYPE_H

#include "ir.h"

struct TypeTable;
struct TypeTable* new_type_table();
void destroy_type_table(struct TypeTable*);

bool is_subtype(const struct Type* supertype, const struct Type* type);
void check_subtype(const struct Type* supertype, const struct Type* type);
enum DivergenceQualifier resolve_divergence(const struct Type* type);

#define DEFINE_NODE_CHECK_FN_true(struct_name, short_name) const struct Type* check_type_##short_name(struct IrArena*, struct struct_name);
#define DEFINE_NODE_CHECK_FN_false(struct_name, short_name)
#define NODEDEF(has_typing_fn, _, struct_name, short_name) DEFINE_NODE_CHECK_FN_##has_typing_fn(struct_name, short_name)
NODES()
#undef NODEDEF

const struct Type* noret_type(struct IrArena* arena);

const struct Type* derive_fn_type(struct IrArena* arena, const struct Function* fn);

const struct Type* strip_qualifier(const struct Type* type, enum DivergenceQualifier* qual_out);

#endif