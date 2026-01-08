#include "sf_compiler_internal.h"
#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static void _calculate_grid(sf_grid* grid, const sf_type_info* domain) {
    memset(grid, 0, sizeof(sf_grid));
    u8 ndim = domain->ndim;
    size_t domain_elements = sf_shape_calc_count(domain->shape, ndim);

    if (ndim <= 1) {
        grid->dims[0] = 1;
        grid->tile_shape[0] = (u32)domain_elements;
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

bool sf_codegen_emit(sf_program* prog, sf_graph_ir* ir, sf_ir_node** sorted, size_t sorted_count, sf_arena* arena) {
    u16 max_reg = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].out_reg_idx > max_reg) max_reg = ir->nodes[i].out_reg_idx;
    }

    prog->meta.tensor_count = (u32)max_reg + 1; 
    
    u32 symbol_count = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type != SF_NODE_UNKNOWN && ir->nodes[i].id && strcmp(ir->nodes[i].id, "unknown") != 0) symbol_count++;
    }
    prog->meta.symbol_count = symbol_count;
    prog->symbols = (symbol_count > 0) ? SF_ARENA_PUSH(arena, sf_bin_symbol, symbol_count) : NULL;

    prog->tensor_infos = SF_ARENA_PUSH(arena, sf_type_info, prog->meta.tensor_count);
    memset(prog->tensor_infos, 0, sizeof(sf_type_info) * prog->meta.tensor_count);
    prog->tensor_data = SF_ARENA_PUSH(arena, void*, prog->meta.tensor_count);
    memset(prog->tensor_data, 0, sizeof(void*) * prog->meta.tensor_count);
    prog->tensor_flags = SF_ARENA_PUSH(arena, uint8_t, prog->meta.tensor_count);
    memset(prog->tensor_flags, 0, prog->meta.tensor_count);

    // --- Phase 1: Declarations (Symbols & Tensor Metadata) ---
    u32 current_symbol = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        u16 r_idx = node->out_reg_idx;
        prog->tensor_infos[r_idx] = node->out_info;
        
        if (node->id && strcmp(node->id, "unknown") != 0) {
            sf_bin_symbol* sym = &prog->symbols[current_symbol++];
            strncpy(sym->name, node->id, SF_MAX_SYMBOL_NAME - 1);
            sym->name[SF_MAX_SYMBOL_NAME - 1] = '\0';
            sym->name_hash = sf_fnv1a_hash(sym->name);
            
            // Redirect output symbols to their source register (since OUTPUT is not an instruction)
            u16 target_reg = r_idx;
            if (node->type == SF_NODE_OUTPUT) {
                sf_ir_node* src = sf_ir_find_input_by_name(ir, (u32)i, "in");
                if (src) target_reg = src->out_reg_idx;
            }
            sym->register_idx = target_reg;
            sym->flags = (node->resource_flags & (SF_RESOURCE_FLAG_READONLY | SF_RESOURCE_FLAG_PERSISTENT | SF_RESOURCE_FLAG_TRANSIENT | SF_RESOURCE_FLAG_SCREEN_SIZE | SF_RESOURCE_FLAG_OUTPUT));
            if (node->type == SF_NODE_INPUT || node->type == SF_NODE_OUTPUT) {
                prog->tensor_flags[target_reg] |= SF_TENSOR_FLAG_ALIAS;
            }
        }
    }

    // --- Phase 2: Push Constants ---
    u32 push_constants_size = 0;
    for (u16 r = 0; r < prog->meta.tensor_count; ++r) {
        for (size_t i = 0; i < ir->node_count; ++i) {
            if (ir->nodes[i].out_reg_idx == r && ir->nodes[i].type == SF_NODE_CONST && ir->nodes[i].out_info.ndim == 0) {
                push_constants_size += (u32)sf_dtype_size(ir->nodes[i].out_info.dtype);
                break;
            }
        }
    }
    u8* pc_block = (push_constants_size > 0) ? SF_ARENA_PUSH(arena, u8, push_constants_size) : NULL;
    prog->meta.push_constants_size = push_constants_size;
    prog->push_constants_data = pc_block;

    if (pc_block) {
        u32 pc_offset = 0;
        for (u16 r = 0; r < prog->meta.tensor_count; ++r) {
            for (size_t i = 0; i < ir->node_count; ++i) {
                if (ir->nodes[i].out_reg_idx == r && ir->nodes[i].type == SF_NODE_CONST && ir->nodes[i].out_info.ndim == 0) {
                    size_t sz = sf_dtype_size(ir->nodes[i].out_info.dtype);
                    memcpy(pc_block + pc_offset, ir->nodes[i].const_data, sz);
                    prog->tensor_data[r] = pc_block + pc_offset;
                    prog->tensor_flags[r] |= SF_TENSOR_FLAG_CONSTANT;
                    pc_offset += (u32)sz;
                    break;
                }
            }
        }
    }

    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type == SF_NODE_CONST && ir->nodes[i].out_info.ndim > 0) {
            prog->tensor_data[ir->nodes[i].out_reg_idx] = ir->nodes[i].const_data;
            prog->tensor_flags[ir->nodes[i].out_reg_idx] |= SF_TENSOR_FLAG_CONSTANT;
        }
    }

    // --- Phase 3: Instructions & Tasks ---
    sf_instruction* instrs = SF_ARENA_PUSH(arena, sf_instruction, ir->node_count);
    sf_task* tasks = SF_ARENA_PUSH(arena, sf_task, ir->node_count);
    sf_bin_task_binding* bindings = SF_ARENA_PUSH(arena, sf_bin_task_binding, ir->node_count * 5);
    
    size_t instr_count = 0;
    u32 task_count = 0;
    u32 total_binding_count = 0;
    u32 current_domain_node_idx = UINT32_MAX;
    u8 current_strategy = SF_STRATEGY_DEFAULT;
    bool needs_sync_scratch = false;
    uint8_t modified_regs[SF_MAX_REGISTERS / 8] = {0};

    for (size_t i = 0; i < sorted_count; ++i) {
        sf_ir_node* node = sorted[i];
        if (node->type == SF_NODE_UNKNOWN || 
            node->type == SF_NODE_INPUT || 
            node->type == SF_NODE_OUTPUT || 
            node->type == SF_NODE_CONST) continue;

        u32 node_idx = (u32)(node - ir->nodes);
        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        
        sf_ir_node* inputs[4] = {0};
        u16 ops[5] = { node->out_reg_idx, 0, 0, 0, 0 };
        for (u32 k = 0; k < 4; ++k) {
            if (meta->ports[k]) {
                inputs[k] = find_input_source(ir, node_idx, k);
                if (inputs[k]) ops[k+1] = inputs[k]->out_reg_idx;
            }
        }

        uint32_t start_instr_idx = (uint32_t)instr_count;
        sf_instruction* inst = &instrs[instr_count++];
        memset(inst, 0, sizeof(sf_instruction));
        inst->opcode = meta->opcode;
        inst->dest_idx = ops[0];
        inst->src1_idx = ops[1]; inst->src2_idx = ops[2]; inst->src3_idx = ops[3]; inst->src4_idx = ops[4];
        inst->line = (u16)node->loc.line; inst->column = (u16)node->loc.column;

        bool is_sync = (meta->strategy == SF_STRATEGY_TWO_PASS_SYNC);
        bool is_reduction = (meta->strategy == SF_STRATEGY_REDUCTION);
        bool domain_changed = (current_domain_node_idx == UINT32_MAX || node->domain_node_idx != current_domain_node_idx);
        if (is_reduction) prog->tensor_flags[ops[0]] |= SF_TENSOR_FLAG_REDUCTION;
        if (is_sync) needs_sync_scratch = true;

        if (domain_changed || is_sync || current_strategy != meta->strategy || task_count == 0) {
            if (task_count > 0) tasks[task_count - 1].inst_count = start_instr_idx - tasks[task_count - 1].start_inst;
            sf_task* t = &tasks[task_count++];
            t->start_inst = start_instr_idx;
            t->strategy = meta->strategy;
            t->flags = 0;
            u32 dom_idx = (node->domain_node_idx == UINT32_MAX) ? node_idx : node->domain_node_idx;
            t->domain_reg = ir->nodes[dom_idx].out_reg_idx;
            t->binding_offset = total_binding_count;
            t->binding_count = 0;
            current_domain_node_idx = node->domain_node_idx;
            current_strategy = meta->strategy;

            _calculate_grid(&t->grid, &ir->nodes[dom_idx].out_info);
        }

        sf_task* curr_task = &tasks[task_count - 1];
        for (int k = 1; k < 5; ++k) {
            if (ops[k] > 0 && (modified_regs[ops[k] / 8] & (1 << (ops[k] % 8)))) {
                curr_task->flags |= SF_TASK_FLAG_BARRIER;
                memset(modified_regs, 0, sizeof(modified_regs)); break;
            }
        }
        for (int k = 0; k < 5; ++k) {
            if (k > 0 && ops[k] == 0) continue;
            u16 r = ops[k];
            if (k == 0) modified_regs[r / 8] |= (1 << (r % 8));
            bool found = false;
            u16 flags = (k == 0) ? SF_BINDING_FLAG_WRITE : SF_BINDING_FLAG_READ;
            if (is_reduction && k == 0) flags |= SF_BINDING_FLAG_REDUCTION;
            for (u32 b = 0; b < curr_task->binding_count; ++b) {
                if (bindings[curr_task->binding_offset + b].reg_idx == r) {
                    bindings[curr_task->binding_offset + b].flags |= flags;
                    found = true; break;
                }
            }
            if (!found) {
                sf_bin_task_binding* b = &bindings[total_binding_count++];
                b->reg_idx = r; b->flags = flags; curr_task->binding_count++;
            }
        }
    }

    if (task_count > 0) tasks[task_count - 1].inst_count = (u32)instr_count - tasks[task_count - 1].start_inst;

    // --- Phase 4: Stride Baking ---
    for (u32 t_idx = 0; t_idx < task_count; ++t_idx) {
        sf_task* t = &tasks[t_idx];
        sf_type_info* dom_info = &prog->tensor_infos[t->domain_reg];
        
        for (u32 b_idx = 0; b_idx < t->binding_count; ++b_idx) {
            sf_bin_task_binding* b = &bindings[t->binding_offset + b_idx];
            sf_type_info* reg_info = &prog->tensor_infos[b->reg_idx];
            
            // Calculate element strides for broadcasting
            sf_shape_get_broadcast_strides(reg_info, dom_info, b->strides);
            
            // Convert to byte strides
            i32 dtype_sz = (i32)sf_dtype_size(reg_info->dtype);
            if (dtype_sz == 0) dtype_sz = 4; // Fallback for safety
            
            for (int d = 0; d < SF_MAX_DIMS; ++d) b->strides[d] *= dtype_sz;
            
            b->offset = 0; // TODO: Support SLICE/View offsets
        }
    }

    prog->code = instrs; prog->tasks = tasks; prog->bindings = bindings;
    prog->meta.instruction_count = (u32)instr_count;
    prog->meta.task_count = task_count;
    prog->meta.binding_count = total_binding_count;
    prog->meta.reduction_scratch_size = (needs_sync_scratch) ? SF_MAX_REGISTERS : 0; 
    prog->meta.sync_scratch_size = needs_sync_scratch ? 1024 : 0; 

    return true;
}
