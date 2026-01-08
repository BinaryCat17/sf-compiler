#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_utils.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// --- IR Cache ---
typedef struct { const char* path; sf_graph_ir ir; } sf_ir_cache_entry;
typedef struct {
    sf_ir_cache_entry* entries; size_t count, capacity;
    sf_arena* arena; sf_compiler_diag* diag; const char* base_path;
} sf_inline_ctx;

bool sf_compile_load_json_ir(const char* json_path, sf_graph_ir* out_ir, sf_arena* arena, sf_compiler_diag* diag, const char* base_path);

static const sf_graph_ir* get_cached_ir(sf_inline_ctx* ctx, const char* path) {
    for (size_t i = 0; i < ctx->count; ++i) if (strcmp(ctx->entries[i].path, path) == 0) return &ctx->entries[i].ir;
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity ? ctx->capacity * 2 : 16;
        sf_ir_cache_entry* new_entries = SF_ARENA_PUSH(ctx->arena, sf_ir_cache_entry, new_cap);
        if (ctx->entries) memcpy(new_entries, ctx->entries, sizeof(sf_ir_cache_entry) * ctx->count);
        ctx->entries = new_entries; ctx->capacity = new_cap;
    }
    sf_ir_cache_entry* e = &ctx->entries[ctx->count++]; e->path = sf_arena_strdup(ctx->arena, path);
    if (!sf_compile_load_json_ir(path, &e->ir, ctx->arena, ctx->diag, ctx->base_path)) return NULL;
    return &e->ir;
}

// --- Grafting ---
typedef struct {
    sf_ir_node* nodes; size_t node_count, node_capacity;
    sf_ir_link* links; size_t link_count, link_capacity;
    sf_arena* arena;
} sf_graft_buffer;

static u32 push_node(sf_graft_buffer* buf, const sf_ir_node* n) {
    if (buf->node_count >= buf->node_capacity) {
        size_t new_cap = buf->node_capacity ? buf->node_capacity * 2 : 128;
        sf_ir_node* new_nodes = SF_ARENA_PUSH(buf->arena, sf_ir_node, new_cap);
        if (buf->nodes) memcpy(new_nodes, buf->nodes, sizeof(sf_ir_node) * buf->node_count);
        buf->nodes = new_nodes; buf->node_capacity = new_cap;
    }
    u32 idx = (u32)buf->node_count++; buf->nodes[idx] = *n; return idx;
}

static void push_link(sf_graft_buffer* buf, const sf_ir_link* l) {
    if (buf->link_count >= buf->link_capacity) {
        size_t new_cap = buf->link_capacity ? buf->link_capacity * 2 : 256;
        sf_ir_link* new_links = SF_ARENA_PUSH(buf->arena, sf_ir_link, new_cap);
        if (buf->links) memcpy(new_links, buf->links, sizeof(sf_ir_link) * buf->link_count);
        buf->links = new_links; buf->link_capacity = new_cap;
    }
    buf->links[buf->link_count++] = *l;
}

typedef struct { u32* map; const sf_graph_ir* parent_ir; u32 call_node_idx; const char* prefix; } sf_graft_scope;

static bool graft_recursive(sf_inline_ctx* ictx, sf_graft_buffer* buf, const sf_graph_ir* ir, sf_graft_scope* scope) {
    u32* local_map = SF_ARENA_PUSH(ictx->arena, u32, ir->node_count);
    for (u32 i = 0; i < ir->node_count; ++i) {
        const sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_CALL) {
            const sf_graph_ir* child_ir = get_cached_ir(ictx, node->sub_graph_path);
            if (!child_ir) return false;
            sf_graft_scope child_scope = { .map = local_map, .parent_ir = ir, .call_node_idx = i, 
                .prefix = scope->prefix ? sf_arena_sprintf(ictx->arena, "%s::%s", scope->prefix, node->id) : sf_arena_strdup(ictx->arena, node->id) };
            if (!graft_recursive(ictx, buf, child_ir, &child_scope)) return false;
            local_map[i] = UINT32_MAX;
        } else {
            sf_ir_node copied = *node;
            copied.id = scope->prefix ? sf_arena_sprintf(ictx->arena, "%s::%s", scope->prefix, node->id) : sf_arena_strdup(ictx->arena, node->id);
            local_map[i] = push_node(buf, &copied);
        }
    }
    for (u32 i = 0; i < ir->link_count; ++i) {
        sf_ir_link l = ir->links[i];
        l.src_node_idx = local_map[l.src_node_idx]; l.dst_node_idx = local_map[l.dst_node_idx];
        if (l.src_node_idx != UINT32_MAX && l.dst_node_idx != UINT32_MAX) {
            push_link(buf, &l);
        } else if (scope->parent_ir) {
            // Bridge cross-boundary links
            push_link(buf, &l);
        }
    }
    return true;
}

bool sf_pass_inline(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_inline_ctx ictx = { .arena = ctx->arena, .diag = diag, .base_path = ctx->base_path, .capacity = 16, .entries = SF_ARENA_PUSH(ctx->arena, sf_ir_cache_entry, 16) };
    sf_graft_buffer buf = { .arena = ctx->arena };
    u32* top_map = SF_ARENA_PUSH(ctx->arena, u32, ctx->ir->node_count);
    for(u32 i=0; i<ctx->ir->node_count; ++i) top_map[i] = i;
    sf_graft_scope top_scope = { .map = top_map, .parent_ir = NULL, .call_node_idx = UINT32_MAX, .prefix = NULL };
    if (!graft_recursive(&ictx, &buf, ctx->ir, &top_scope)) return false;
    ctx->ir->node_count = (u32)buf.node_count; ctx->ir->nodes = buf.nodes;
    ctx->ir->link_count = (u32)buf.link_count; ctx->ir->links = buf.links;
    return true;
}

bool sf_pass_inline_wrapper(sf_pass_ctx* ctx, sf_compiler_diag* diag) { return sf_pass_inline(ctx, diag); }
