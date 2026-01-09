#include "../sf_passes.h"
#include "../sf_graph_utils.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

/**
 * Task Planning Pass
 * Groups instructions into execution tasks, plans barriers, and bakes strides for broadcasting.
 * This logic was extracted from codegen to keep the compiler modular and elegant.
 */

static void calculate_grid(sf_grid* grid, const sf_type_info* domain) {
    memset(grid, 0, sizeof(sf_grid));
    u8 ndim = domain->ndim;
    size_t total_elements = sf_shape_calc_count(domain->shape, ndim);

    if (ndim <= 1) {
        grid->dims[0] = 1;
        grid->tile_shape[0] = (u32)total_elements;
        grid->total_tiles = 1;
    } else {
        grid->total_tiles = 1;
        for (int d = 0; d < ndim - 1; ++d) {
            grid->dims[d] = domain->shape[d];
            grid->tile_shape[d] = 1;
            grid->total_tiles *= grid->dims[d];
        }
        grid->dims[ndim - 1] = 1;
        grid->tile_shape[ndim - 1] = domain->shape[ndim - 1];
    }
}

bool sf_pass_task_plan(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    sf_graph_ir* ir = ctx->ir;
    sf_arena* arena = ctx->arena;
    sf_ir_node** sorted = ctx->sorted_nodes;
    size_t count = ctx->sorted_count;

    // Allocate Task and Binding buffers
    sf_task* tasks = SF_ARENA_PUSH(arena, sf_task, count);
    sf_bin_task_binding* bindings = SF_ARENA_PUSH(arena, sf_bin_task_binding, count * 5);
    u32 task_count = 0;
    u32 binding_count = 0;
    u32 instr_idx = 0;

    u32 current_domain_idx = UINT32_MAX;
    u8 current_strategy = SF_STRATEGY_DEFAULT;
    uint8_t modified_regs[SF_MAX_REGISTERS / 8] = {0};

    for (size_t i = 0; i < count; ++i) {
        sf_ir_node* node = sorted[i];
        if (node->type == SF_NODE_UNKNOWN || node->type == SF_NODE_INPUT || 
            node->type == SF_NODE_OUTPUT || node->type == SF_NODE_CONST) continue;

        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        u32 node_idx = (u32)(node - ir->nodes);
        u16 out_reg = node->out_reg_idx;

        // Check for Task Break conditions
        bool domain_changed = (current_domain_idx == UINT32_MAX || node->domain_node_idx != current_domain_idx);
        bool strategy_changed = (current_strategy != meta->strategy);
        bool is_sync = (meta->strategy == SF_STRATEGY_TWO_PASS_SYNC);

        if (domain_changed || strategy_changed || is_sync || task_count == 0) {
            // Finalize previous task
            if (task_count > 0) {
                tasks[task_count - 1].inst_count = instr_idx - tasks[task_count - 1].start_inst;
            }

            // Start new task
            sf_task* t = &tasks[task_count++];
            memset(t, 0, sizeof(sf_task));
            t->start_inst = instr_idx;
            t->strategy = meta->strategy;
            
            u32 dom_idx = (node->domain_node_idx == UINT32_MAX) ? node_idx : node->domain_node_idx;
            t->domain_reg = ir->nodes[dom_idx].out_reg_idx;
            t->binding_offset = binding_count;
            
            current_domain_idx = node->domain_node_idx;
            current_strategy = meta->strategy;
            calculate_grid(&t->grid, &ir->nodes[dom_idx].out_info);
            memset(modified_regs, 0, sizeof(modified_regs));
        }

        sf_task* curr_task = &tasks[task_count - 1];

        // Resource Binding & Barrier Planning
        u16 regs[5] = { out_reg, 0, 0, 0, 0 };
        for (u32 k = 0; k < 4; ++k) {
            if (meta->ports[k]) {
                sf_ir_node* src = find_input_source(ir, node_idx, k);
                if (src) regs[k+1] = src->out_reg_idx;
            }
        }

        // Add Barrier if we read something that was modified in the SAME task
        for (int k = 1; k < 5; ++k) {
            u16 r = regs[k];
            if (r > 0 && (modified_regs[r / 8] & (1 << (r % 8)))) {
                curr_task->flags |= SF_TASK_FLAG_BARRIER;
                memset(modified_regs, 0, sizeof(modified_regs));
                break;
            }
        }

        // Update bindings
        for (int k = 0; k < 5; ++k) {
            u16 r = regs[k];
            if (k > 0 && r == 0) continue;
            
            if (k == 0) modified_regs[r / 8] |= (1 << (r % 8));
            
            bool found = false;
            u16 b_flags = (k == 0) ? SF_BINDING_FLAG_WRITE : SF_BINDING_FLAG_READ;
            if (meta->strategy == SF_STRATEGY_REDUCTION && k == 0) b_flags |= SF_BINDING_FLAG_REDUCTION;

            for (u32 b = 0; b < curr_task->binding_count; ++b) {
                if (bindings[curr_task->binding_offset + b].reg_idx == r) {
                    bindings[curr_task->binding_offset + b].flags |= b_flags;
                    found = true; break;
                }
            }
            if (!found) {
                sf_bin_task_binding* b = &bindings[curr_task->binding_offset + curr_task->binding_count++];
                b->reg_idx = r;
                b->flags = b_flags;
                binding_count++;
            }
        }

        instr_idx++;
    }

    if (task_count > 0) {
        tasks[task_count - 1].inst_count = instr_idx - tasks[task_count - 1].start_inst;
    }

    // Phase 2: Stride Baking (Broadcasting logic)
    for (u32 t_idx = 0; t_idx < task_count; ++t_idx) {
        sf_task* t = &tasks[t_idx];
        sf_type_info* dom_info = &ir->nodes[find_node_by_reg(ir, t->domain_reg)].out_info;
        
        for (u32 b_idx = 0; b_idx < t->binding_count; ++b_idx) {
            sf_bin_task_binding* b = &bindings[t->binding_offset + b_idx];
            sf_type_info* reg_info = &ir->nodes[find_node_by_reg(ir, b->reg_idx)].out_info;
            
            sf_shape_get_broadcast_strides(reg_info, dom_info, b->strides);
            i32 dtype_sz = (i32)sf_dtype_size(reg_info->dtype);
            for (int d = 0; d < SF_MAX_DIMS; ++d) b->strides[d] *= (dtype_sz ? dtype_sz : 4);
        }
    }

    ctx->tasks = tasks;
    ctx->task_count = task_count;
    ctx->bindings = bindings;
    ctx->binding_count = binding_count;

    return true;
}
