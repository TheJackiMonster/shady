#include "generator.h"

inline static bool should_include_instruction(json_object* instruction) {
    String class = json_object_get_string(json_object_object_get(instruction, "class"));
    if (strcmp(class, "@exclude") == 0)
        return false;
    return true;
}

void add_comments(Growy* g, String indent, json_object* comments) {
    if (!indent)
        indent = "";
    if (json_object_get_type(comments) == json_type_string) {
        shd_growy_append_formatted(g, "%s/// %s\n", indent, json_object_get_string(comments));
    } else if (json_object_get_type(comments) == json_type_array) {
        size_t size = json_object_array_length(comments);
        for (size_t i = 0; i < size; i++)
            add_comments(g, indent, json_object_array_get_idx(comments, i));
    }
}

String to_snake_case(String camel) {
    size_t camel_len = strlen(camel);
    size_t buffer_size = camel_len + 16;
    char* dst = malloc(buffer_size);
    while (true) {
        size_t j = 0;
        for (size_t i = 0; i < camel_len; i++) {
            if (j >= buffer_size)
                goto start_over;
            if (isupper(camel[i])) {
                if (i > 0 && !isupper(camel[i - 1]))
                    dst[j++] = '_';
                dst[j++] = tolower(camel[i]);
            } else {
                dst[j++] = camel[i];
            }
        }

        // null terminate if we have space
        if (j + 1 < buffer_size) {
            dst[j] = '\0';
            return dst;
        }

        start_over:
        buffer_size += 16;
        dst = realloc(dst, buffer_size);
    }
}

String capitalize(String str) {
    size_t len = strlen(str);
    assert(len > 0);
    size_t buffer_size = len + 1;
    char* dst = malloc(buffer_size);
    dst[0] = toupper(str[0]);
    for (size_t i = 1; i < len; i++) {
        dst[i] = str[i];
    }
    dst[len] = '\0';
    return dst;
}

void generate_header(Growy* g, json_object* root) {
    json_object* spv = json_object_object_get(root, "spv");
    int32_t major = json_object_get_int(json_object_object_get(spv, "major_version"));
    int32_t minor = json_object_get_int(json_object_object_get(spv, "minor_version"));
    int32_t revision = json_object_get_int(json_object_object_get(spv, "revision"));
    shd_growy_append_formatted(g, "/* Generated from SPIR-V %d.%d revision %d */\n", major, minor, revision);
    shd_growy_append_formatted(g, "/* Do not edit this file manually ! */\n");
    shd_growy_append_formatted(g, "/* It is generated by the 'generator' target using Json grammar files. */\n\n");
}

bool starts_with_vowel(String str) {
    char vowels[] = { 'a', 'e', 'i', 'o', 'u' };
    for (size_t i = 0; i < (sizeof(vowels) / sizeof(char)); i++) {
        if (str[0] == vowels[i]) {
            return true;
        }
    }
    return false;
}
