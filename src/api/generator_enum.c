#include "generator.h"

static void generate_address_spaces(Growy* g, json_object* address_spaces) {
    shd_growy_append_formatted(g, "typedef enum AddressSpace_ {\n");
    for (size_t i = 0; i < json_object_array_length(address_spaces); i++) {
        json_object* as = json_object_array_get_idx(address_spaces, i);
        String name = json_object_get_string(json_object_object_get(as, "name"));
        add_comments(g, "\t", json_object_object_get(as, "description"));
        shd_growy_append_formatted(g, "\tAs%s,\n", name);
    }
    shd_growy_append_formatted(g, "\tNumAddressSpaces,\n");
    shd_growy_append_formatted(g, "} AddressSpace;\n\n");
}

static void generate_node_tags(Growy* g, json_object* nodes) {
    assert(json_object_get_type(nodes) == json_type_array);
    shd_growy_append_formatted(g, "typedef enum {\n");
    shd_growy_append_formatted(g, "\tInvalidNode_TAG,\n");

    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);
        json_object* ops = json_object_object_get(node, "ops");
        if (!ops)
            add_comments(g, "\t", json_object_object_get(node, "description"));

        shd_growy_append_formatted(g, "\t%s_TAG,\n", name);
    }
    shd_growy_append_formatted(g, "} NodeTag;\n\n");
}

static void generate_primops(Growy* g, json_object* nodes) {
    shd_growy_append_formatted(g, "typedef enum Op_ {\n");

    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);

        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);

        shd_growy_append_formatted(g, "\t%s_op,\n", name);
    }

    shd_growy_append_formatted(g, "\tPRIMOPS_COUNT,\n");
    shd_growy_append_formatted(g, "} Op;\n");
}

static void generate_node_tags_for_class(Growy* g, json_object* nodes, String class, String capitalized_class) {
    assert(json_object_get_type(nodes) == json_type_array);
    shd_growy_append_formatted(g, "typedef enum {\n");
    if (starts_with_vowel(class))
        shd_growy_append_formatted(g, "\tNotAn%s = 0,\n", capitalized_class);
    else
        shd_growy_append_formatted(g, "\tNotA%s = 0,\n", capitalized_class);

    for (size_t i = 0; i < json_object_array_length(nodes); i++) {
        json_object* node = json_object_array_get_idx(nodes, i);
        String name = json_object_get_string(json_object_object_get(node, "name"));
        assert(name);
        json_object* nclass = json_object_object_get(node, "class");
        switch (json_object_get_type(nclass)) {
            case json_type_null:
                break;
            case json_type_string:
                if (nclass && strcmp(json_object_get_string(nclass), class) == 0)
                    shd_growy_append_formatted(g, "\t%s_%s_TAG = %s_TAG,\n", capitalized_class, name, name);
                break;
            case json_type_array: {
                for (size_t j = 0; j < json_object_array_length(nclass); j++) {
                    if (nclass && strcmp(json_object_get_string(json_object_array_get_idx(nclass, j)), class) == 0) {
                        shd_growy_append_formatted(g, "\t%s_%s_TAG = %s_TAG,\n", capitalized_class, name, name);
                        break;
                    }
                }
                break;
            }
            case json_type_boolean:
            case json_type_double:
            case json_type_int:
            case json_type_object:
                shd_error_print("Invalid datatype for a node's 'class' attribute");
        }

    }
    shd_growy_append_formatted(g, "} %sTag;\n\n", capitalized_class);
}

void generate(Growy* g, json_object* src) {
    generate_header(g, src);

    generate_primops(g, json_object_object_get(src, "prim-ops"));
    json_object* op_classes = json_object_object_get(src, "prim-ops-classes");
    generate_bit_enum(g, "OpClass", "Oc", op_classes, false);

    generate_address_spaces(g, json_object_object_get(src, "address-spaces"));
    json_object* nodes = json_object_object_get(src, "nodes");
    generate_node_tags(g, nodes);

    json_object* node_classes = json_object_object_get(src, "node-classes");
    for (size_t i = 0; i < json_object_array_length(node_classes); i++) {
        json_object* node_class = json_object_array_get_idx(node_classes, i);
        String name = json_object_get_string(json_object_object_get(node_class, "name"));
        assert(name);
        json_object* generate_enum = json_object_object_get(node_class, "generate-enum");
        String capitalized = capitalize(name);

        if (!generate_enum || json_object_get_boolean(generate_enum)) {
            generate_node_tags_for_class(g, nodes, name, capitalized);
        }

        free((void*) capitalized);
    }
}
