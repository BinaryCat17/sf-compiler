#include "sf_compiler_internal.h"
#include <sionflow/base/sf_json.h>
#include <string.h>

void sf_ir_parse_window_settings(const sf_json_value* root, sf_graph_ir* out_ir) {
    if (!root || root->type != SF_JSON_VAL_OBJECT) return;

    // Defaults
    strncpy(out_ir->app_title, "SionFlow Cartridge", SF_MAX_TITLE_NAME - 1);
    out_ir->window_width = 800;
    out_ir->window_height = 600;
    out_ir->vsync = 1;
    out_ir->resizable = 1;

    const sf_json_value* window = sf_json_get_field(root, "window");
    if (window && window->type == SF_JSON_VAL_OBJECT) {
        const sf_json_value* title = sf_json_get_field(window, "title");
        if (title && title->type == SF_JSON_VAL_STRING) strncpy(out_ir->app_title, title->as.s, SF_MAX_TITLE_NAME - 1);
        
        const sf_json_value* w = sf_json_get_field(window, "width");
        if (w && w->type == SF_JSON_VAL_NUMBER) out_ir->window_width = (u32)w->as.n;

        const sf_json_value* h = sf_json_get_field(window, "height");
        if (h && h->type == SF_JSON_VAL_NUMBER) out_ir->window_height = (u32)h->as.n;

        const sf_json_value* vsync = sf_json_get_field(window, "vsync");
        if (vsync && vsync->type == SF_JSON_VAL_BOOL) out_ir->vsync = vsync->as.b;

        const sf_json_value* fs = sf_json_get_field(window, "fullscreen");
        if (fs && fs->type == SF_JSON_VAL_BOOL) out_ir->fullscreen = fs->as.b;

        const sf_json_value* resizable = sf_json_get_field(window, "resizable");
        if (resizable && resizable->type == SF_JSON_VAL_BOOL) out_ir->resizable = resizable->as.b;
    }

    const sf_json_value* runtime = sf_json_get_field(root, "runtime");
    if (runtime && runtime->type == SF_JSON_VAL_OBJECT) {
        const sf_json_value* threads = sf_json_get_field(runtime, "threads");
        if (threads && threads->type == SF_JSON_VAL_NUMBER) out_ir->num_threads = (u32)threads->as.n;
    }
}

// --- Helper: Find Input Source ---
sf_ir_node* find_input_source(sf_graph_ir* ir, u32 dst_node_idx, u32 dst_port) {
    for (size_t i = 0; i < ir->link_count; ++i) {
        if (ir->links[i].dst_node_idx == dst_node_idx && ir->links[i].dst_port == dst_port) {
            return &ir->nodes[ir->links[i].src_node_idx];
        }
    }
    return NULL;
}

sf_ir_node* sf_ir_find_input_by_name(sf_graph_ir* ir, u32 dst_node_idx, const char* port_name) {
    if (!port_name) return NULL;
    for (size_t i = 0; i < ir->link_count; ++i) {
        if (ir->links[i].dst_node_idx == dst_node_idx) {
            if (ir->links[i].dst_port_name && strcmp(ir->links[i].dst_port_name, port_name) == 0) {
                return &ir->nodes[ir->links[i].src_node_idx];
            }
        }
    }
    return NULL;
}

// --- Topological Sort Helpers ---

typedef struct {
    sf_ir_node** sorted_nodes; 
    u8* visited;
    size_t count;
    sf_graph_ir* ir;
} sort_ctx;

static bool visit_node(sort_ctx* ctx, u32 node_idx) {
    if (ctx->visited[node_idx] == 2) return true;
    
    // Cycle detection
    if (ctx->visited[node_idx] == 1) {
        return false; 
    }
    
    ctx->visited[node_idx] = 1;

    // Visit dependencies
    for (size_t i = 0; i < ctx->ir->link_count; ++i) {
        if (ctx->ir->links[i].dst_node_idx == node_idx) {
            if (!visit_node(ctx, ctx->ir->links[i].src_node_idx)) return false;
        }
    }

    ctx->visited[node_idx] = 2;
    ctx->sorted_nodes[ctx->count++] = &ctx->ir->nodes[node_idx];
    return true;
}

sf_ir_node** sf_topo_sort(sf_graph_ir* ir, sf_arena* arena, size_t* out_count) {
    sf_ir_node** sorted = SF_ARENA_PUSH(arena, sf_ir_node*, ir->node_count);
    u8* visited = SF_ARENA_PUSH(arena, u8, ir->node_count);
    if (!sorted || !visited) return NULL;
    
    memset(visited, 0, ir->node_count);

    sort_ctx ctx = { .sorted_nodes = sorted, .visited = visited, .count = 0, .ir = ir };
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (visited[i] == 0) {
            if (!visit_node(&ctx, (u32)i)) return NULL;
        }
    }
    
    if (out_count) *out_count = ctx.count;
    return sorted;
}
