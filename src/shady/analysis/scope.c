#include "scope.h"
#include "log.h"

#include "list.h"
#include "dict.h"
#include "arena.h"

#include <stdlib.h>
#include <assert.h>

struct List* build_scopes(const Node* root) {
    struct List* scopes = new_list(Scope);

    for (size_t i = 0; i < root->payload.root.declarations.count; i++) {
        const Node* decl = root->payload.root.declarations.nodes[i];
        if (decl->tag != Lambda_TAG) continue;
        Scope scope = build_scope_from_basic_block(decl);
        append_list(Scope, scopes, scope);
    }

    return scopes;
}

KeyHash hash_node(const Node**);
bool compare_node(const Node**, const Node**);

KeyHash hash_location(const CFLocation** ploc) {
    const Node* head = (*ploc)->head;
    const Node* body = (*ploc)->body;
    return hash_node(&head) ^ hash_node(&body) ^ (*ploc)->offset;
}

bool compare_location(const CFLocation** a, const CFLocation** b) {
    const Node* ahead = (*a)->head;
    const Node* bhead = (*b)->head;
    const Node* abody = (*a)->body;
    const Node* bbody = (*b)->body;
    return compare_node(&ahead, &bhead) && compare_node(&abody, &bbody) && (*a)->offset == (*b)->offset;
}

typedef struct {
    Arena* arena;
    struct Dict* nodes;
    struct List* queue;
    struct List* contents;
} ScopeBuildContext;

static CFNode* get_or_enqueue(ScopeBuildContext* ctx, CFLocation location) {
    assert(location.head->tag == Lambda_TAG);
    CFNode** found = find_value_dict(const Node*, CFNode*, ctx->nodes, location);
    if (found) return *found;

    CFNode* new = arena_alloc(ctx->arena, sizeof(CFNode));
    *new = (CFNode) {
        .location = location,
        .succ_edges = new_list(CFEdge),
        .pred_edges = new_list(CFEdge),
        .rpo_index = SIZE_MAX,
        .idom = NULL,
        .dominates = NULL,
    };
    insert_dict(const Node*, CFNode*, ctx->nodes, location, new);
    append_list(Node*, ctx->queue, new);
    append_list(Node*, ctx->contents, new);
    return new;
}

/// Adds an edge to somewhere inside a basic block (see CFLocation)
static void add_edge(ScopeBuildContext* ctx, CFLocation src, CFLocation dst, CFEdgeType type) {
    CFNode* src_node = get_or_enqueue(ctx, src);
    CFNode* dst_node = get_or_enqueue(ctx, dst);
    CFEdge edge = {
        .type = type,
        .src = src_node,
        .dst = dst_node,
    };
    append_list(CFNode*, src_node->succ_edges, edge);
    append_list(CFNode*, dst_node->pred_edges, edge);
}

/// Adds an edge to the start of a basic block
static void add_edge_to_bb(ScopeBuildContext* ctx, CFLocation src, const Node* dest_bb, CFEdgeType type) {
    assert(dest_bb->tag == Lambda_TAG && dest_bb->payload.lam.tier != FnTier_Function);
    const Node* dest_body = dest_bb->payload.lam.body;
    assert(dest_body && dest_body->tag == Body_TAG);
    CFLocation dst = {
        .head = dest_bb,
        .body = dest_body,
    };
    add_edge(ctx, src, dst, type);
}

static void process_cf_node(ScopeBuildContext* ctx, CFNode* node) {
    CFLocation const location = node->location;
    const Body* body = &location.head->payload.lam.body->payload.body;
    assert(body);

    for (size_t i = 0; i < body->instructions.count; i++) {
        // TODO: walk the body and search for structured constructs...
    }

    const Node* terminator = body->terminator;
    switch (terminator->tag) {
        case Branch_TAG: {
            switch (terminator->payload.branch.branch_mode) {
                case BrJump: {
                    const Node* target = terminator->payload.branch.target;
                    add_edge_to_bb(ctx, location, target, ForwardEdge);
                    break;
                }
                case BrIfElse: {
                    const Node* true_target = terminator->payload.branch.true_target;
                    const Node* false_target = terminator->payload.branch.false_target;
                    add_edge_to_bb(ctx, location, true_target, ForwardEdge);
                    add_edge_to_bb(ctx, location, false_target, ForwardEdge);
                    break;
                }
                case BrSwitch: error("TODO")
            }
            break;
        }
        case Callc_TAG: {
            if (terminator->payload.callc.is_return_indirect) break;
            const Node* target = terminator->payload.callc.join_at;
            add_edge_to_bb(ctx, location, target, CallcReturnEdge);
            break;
        }
        case Join_TAG: {
            break;
        }
        case MergeConstruct_TAG: {
            error("TODO: only allow this if we have traversed structured constructs...")
            break;
        }
        case TailCall_TAG:
        case Return_TAG:
        case Unreachable_TAG: break;
        default: error("scope: unhandled terminator");
    }
}

Scope build_scope_from_basic_block(const Node* bb) {
    assert(bb->tag == Lambda_TAG);
    CFLocation entry_location = {
        .head = bb,
        .offset = 0
    };
    return build_scope(entry_location);
}

Scope build_scope(CFLocation entry_location) {
    assert(entry_location.head->tag == Lambda_TAG);
    Arena* arena = new_arena();

    ScopeBuildContext context = {
        .arena = arena,
        .nodes = new_dict(CFLocation, CFNode*, (HashFn) hash_location, (CmpFn) compare_location),
        .queue = new_list(CFNode*),
        .contents = new_list(CFNode*),
    };

    CFNode* entry_node = get_or_enqueue(&context, entry_location);

    while (entries_count_list(context.queue) > 0) {
        CFNode* this = pop_last_list(CFNode*, context.queue);
        process_cf_node(&context, this);
    }

    destroy_dict(context.nodes);
    destroy_list(context.queue);

    Scope scope = {
        .arena = arena,
        .entry = entry_node,
        .size = entries_count_list(context.contents),
        .contents = context.contents,
        .rpo = NULL
    };

    compute_rpo(&scope);
    compute_domtree(&scope);

    return scope;
}

static size_t post_order_visit(Scope* scope, CFNode* n, size_t i) {
    n->rpo_index = -2;

    for (size_t j = 0; j < entries_count_list(n->succ_edges); j++) {
        CFEdge edge = read_list(CFEdge, n->succ_edges)[j];
        if (edge.dst->rpo_index == SIZE_MAX)
            i = post_order_visit(scope, edge.dst, i);
    }

    n->rpo_index = i - 1;
    scope->rpo[n->rpo_index] = n;
    return n->rpo_index;
}

void compute_rpo(Scope* scope) {
    scope->rpo = malloc(sizeof(const CFNode*) * scope->size);
    size_t index = post_order_visit(scope,  scope->entry, scope->size);
    assert(index == 0);

    debug_print("RPO: ");
    for (size_t i = 0; i < scope->size; i++) {
        debug_print("%s %d, ", scope->rpo[i]->location.head->payload.lam.name, scope->rpo[i]->location.offset);
    }
    debug_print("\n");
}

CFNode* least_common_ancestor(CFNode* i, CFNode* j) {
    assert(i && j);
    while (i->rpo_index != j->rpo_index) {
        while (i->rpo_index < j->rpo_index) j = j->idom;
        while (i->rpo_index > j->rpo_index) i = i->idom;
    }
    return i;
}

void compute_domtree(Scope* scope) {
    for (size_t i = 1; i < scope->size; i++) {
        CFNode* n = read_list(CFNode*, scope->contents)[i];
        for (size_t j = 0; j < entries_count_list(n->pred_edges); j++) {
            CFEdge e = read_list(CFEdge, n->pred_edges)[j];
            CFNode* p = e.src;
            if (p->rpo_index < n->rpo_index) {
                n->idom = p;
                goto outer_loop;
            }
        }
        error("no idom found for %s", n->location.head->payload.lam.name);
        outer_loop:;
    }

    bool todo = true;
    while (todo) {
        todo = false;
        for (size_t i = 1; i < scope->size; i++) {
            CFNode* n = read_list(CFNode*, scope->contents)[i];
            CFNode* new_idom = NULL;
            for (size_t j = 0; j < entries_count_list(n->pred_edges); j++) {
                CFEdge e = read_list(CFEdge, n->pred_edges)[j];
                CFNode* p = e.src;
                new_idom = new_idom ? least_common_ancestor(new_idom, p) : p;
            }
            assert(new_idom);
            if (n->idom != new_idom) {
                n->idom = new_idom;
                todo = true;
            }
        }
    }

    for (size_t i = 0; i < scope->size; i++) {
        CFNode* n = read_list(CFNode*, scope->contents)[i];
        n->dominates = new_list(CFNode*);
    }
    for (size_t i = 1; i < scope->size; i++) {
        CFNode* n = read_list(CFNode*, scope->contents)[i];
        append_list(CFNode*, n->idom->dominates, n);
    }
}

void dispose_scope(Scope* scope) {
    for (size_t i = 0; i < scope->size; i++) {
        CFNode* node = read_list(CFNode*, scope->contents)[i];
        destroy_list(node->pred_edges);
        destroy_list(node->succ_edges);
        if (node->dominates)
            destroy_list(node->dominates);
    }
    destroy_arena(scope->arena);
    free(scope->rpo);
    destroy_list(scope->contents);
}

static int extra_uniqueness = 0;

static void dump_cfg_scope(FILE* output, Scope* scope) {
    extra_uniqueness++;

    const Lambda* entry = &scope->entry->location.head->payload.lam;
    fprintf(output, "subgraph cluster_%s {\n", entry->name);
    fprintf(output, "label = \"%s\";\n", entry->name);
    for (size_t i = 0; i < entries_count_list(scope->contents); i++) {
        const Lambda* bb = &read_list(const CFNode*, scope->contents)[i]->location.head->payload.lam;
        fprintf(output, "%s_%d;\n", bb->name, extra_uniqueness);
    }
    for (size_t i = 0; i < entries_count_list(scope->contents); i++) {
        const CFNode* bb_node = read_list(const CFNode*, scope->contents)[i];
        const Lambda* bb = &bb_node->location.head->payload.lam;

        for (size_t j = 0; j < entries_count_list(bb_node->succ_edges); j++) {
            CFEdge edge = read_list(CFEdge, bb_node->succ_edges)[j];
            const CFNode* target_node = edge.dst;
            const Lambda* target_bb = &target_node->location.head->payload.lam;
            fprintf(output, "%s_%d -> %s_%d;\n", bb->name, extra_uniqueness, target_bb->name, extra_uniqueness);
        }
    }
    fprintf(output, "}\n");
}

void dump_cfg(FILE* output, const Node* root) {
    if (output == NULL)
        output = stderr;

    fprintf(output, "digraph G {\n");
    struct List* scopes = build_scopes(root);
    for (size_t i = 0; i < entries_count_list(scopes); i++) {
        Scope* scope = &read_list(Scope, scopes)[i];
        dump_cfg_scope(output, scope);
        dispose_scope(scope);
    }
    destroy_list(scopes);
    fprintf(output, "}\n");
}
