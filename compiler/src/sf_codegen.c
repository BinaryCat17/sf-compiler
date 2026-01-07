#include "sf_compiler_internal.h"
#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

bool sf_codegen_emit(sf_program* prog, sf_graph_ir* ir, sf_ir_node** sorted, size_t sorted_count, sf_arena* arena) {
    u16 max_reg = 0;
    for (size_t i = 0; i < sorted_count; ++i) {
        if (sorted[i]->out_reg_idx > max_reg) max_reg = sorted[i]->out_reg_idx;
    }

    prog->meta.tensor_count = (u32)max_reg + 1; 
    
    u32 symbol_count = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].id && strcmp(ir->nodes[i].id, "unknown") != 0) symbol_count++;
    }
    prog->meta.symbol_count = symbol_count;
    prog->symbols = (symbol_count > 0) ? SF_ARENA_PUSH(arena, sf_bin_symbol, symbol_count) : NULL;

    prog->tensor_infos = SF_ARENA_PUSH(arena, sf_type_info, prog->meta.tensor_count);
    memset(prog->tensor_infos, 0, sizeof(sf_type_info) * prog->meta.tensor_count);
    prog->tensor_data = SF_ARENA_PUSH(arena, void*, prog->meta.tensor_count);
    memset(prog->tensor_data, 0, sizeof(void*) * prog->meta.tensor_count);
    prog->tensor_flags = SF_ARENA_PUSH(arena, uint8_t, prog->meta.tensor_count);
    memset(prog->tensor_flags, 0, prog->meta.tensor_count);

    sf_instruction* instrs = SF_ARENA_PUSH(arena, sf_instruction, ir->node_count * 3);
    sf_task* tasks = SF_ARENA_PUSH(arena, sf_task, ir->node_count * 2);
    
    sf_bin_task_binding* bindings = SF_ARENA_PUSH(arena, sf_bin_task_binding, ir->node_count * 10);
    u32 total_binding_count = 0;

    size_t instr_count = 0;
    u32 task_count = 0;
    u32 current_symbol = 0;
    u32 current_domain_node_idx = UINT32_MAX;
    u8 current_strategy = SF_STRATEGY_DEFAULT;
    bool needs_sync_scratch = false;

    // Static Barrier Planning
    uint8_t modified_regs[SF_MAX_REGISTERS / 8] = {0};

    // --- Push Constant Optimization (Phase 1: Count & Size) ---
    u32 push_constants_size = 0;
    for (u16 r = 0; r < prog->meta.tensor_count; ++r) {
        for (size_t i = 0; i < sorted_count; ++i) {
            if (sorted[i]->out_reg_idx == r && sorted[i]->type == SF_NODE_CONST && sorted[i]->out_info.ndim == 0) {
                push_constants_size += (u32)sf_dtype_size(sorted[i]->out_info.dtype);
                break;
            }
        }
    }
    
    u8* pc_block = (push_constants_size > 0) ? SF_ARENA_PUSH(arena, u8, push_constants_size) : NULL;
    u32 pc_offset = 0;
    prog->meta.push_constants_size = push_constants_size;
    prog->push_constants_data = pc_block;

    // Phase 2: Fill PC Block in register order
    if (pc_block) {
        for (u16 r = 0; r < prog->meta.tensor_count; ++r) {
            for (size_t i = 0; i < sorted_count; ++i) {
                if (sorted[i]->out_reg_idx == r && sorted[i]->type == SF_NODE_CONST && sorted[i]->out_info.ndim == 0) {
                    size_t sz = sf_dtype_size(sorted[i]->out_info.dtype);
                    memcpy(pc_block + pc_offset, sorted[i]->const_data, sz);
                    pc_offset += (u32)sz;
                    break;
                }
            }
        }
    }

    // Reset offset for pointers assignment in the main loop
    pc_offset = 0;

    for (size_t i = 0; i < sorted_count; ++i) {
        sf_ir_node* node = sorted[i];
        u32 node_idx = (u32)(node - ir->nodes); 
        u16 r_idx = node->out_reg_idx;
        
        // --- 1. Symbol Table Entry ---
        if (node->id && strcmp(node->id, "unknown") != 0) {
            sf_bin_symbol* sym = &prog->symbols[current_symbol++];
            strncpy(sym->name, node->id, SF_MAX_SYMBOL_NAME - 1);
            sym->name[SF_MAX_SYMBOL_NAME - 1] = '\0';
            sym->name_hash = sf_fnv1a_hash(sym->name);
            sym->register_idx = r_idx;
            sym->related_name_hash = 0;
            sym->provider[0] = '\0';
            
            sym->flags = (node->type == SF_NODE_INPUT) ? SF_SYMBOL_FLAG_INPUT : 
                         (node->type == SF_NODE_OUTPUT) ? SF_SYMBOL_FLAG_OUTPUT : 0;
            sym->flags |= (node->resource_flags & (SF_RESOURCE_FLAG_READONLY | SF_RESOURCE_FLAG_PERSISTENT | SF_RESOURCE_FLAG_TRANSIENT | SF_RESOURCE_FLAG_SCREEN_SIZE | SF_RESOURCE_FLAG_OUTPUT));

            if (node->type == SF_NODE_INPUT || node->type == SF_NODE_OUTPUT) {
                prog->tensor_flags[r_idx] |= SF_TENSOR_FLAG_ALIAS;
            }
        }

        // --- 2. Register Metadata ---
        sf_type_info* t_info = &prog->tensor_infos[r_idx];
        if (t_info->ndim == 0) {
            *t_info = node->out_info;
        }

        // --- 3. Instruction Generation ---
        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        sf_ir_node* inputs[4] = {0};
        for (u8 k = 0; k < 4; ++k) if (meta->ports[k]) inputs[k] = sf_ir_find_input_by_name(ir, node_idx, meta->ports[k]);

        if (node->type == SF_NODE_CONST) {
            if (node->out_info.ndim == 0 && pc_block) {
                // Scalar Constant: Use pointer into grouped block
                // We need to find the correct offset by summing sizes of previous scalars
                u32 current_pc_offset = 0;
                for (u16 r = 0; r < r_idx; ++r) {
                    // Check if register 'r' was a scalar constant
                    for (size_t j = 0; j < sorted_count; ++j) {
                        if (sorted[j]->out_reg_idx == r && sorted[j]->type == SF_NODE_CONST && sorted[j]->out_info.ndim == 0) {
                            current_pc_offset += (u32)sf_dtype_size(sorted[j]->out_info.dtype);
                            break;
                        }
                    }
                }
                prog->tensor_data[r_idx] = pc_block + current_pc_offset;
            } else {
                prog->tensor_data[r_idx] = node->const_data;
            }
            prog->tensor_flags[r_idx] |= SF_TENSOR_FLAG_CONSTANT;
        }

        uint32_t start_instr_idx = (uint32_t)instr_count;
        bool emitted = false;

        if (meta->category != SF_OP_CAT_SPECIAL || node->type == SF_NODE_COPY || node->type == SF_NODE_OUTPUT) {
            sf_instruction* inst = &instrs[instr_count++];
            memset(inst, 0, sizeof(sf_instruction));
            inst->dest_idx = r_idx;
            inst->src1_idx = inputs[0] ? inputs[0]->out_reg_idx : 0;
            inst->src2_idx = inputs[1] ? inputs[1]->out_reg_idx : 0;
            inst->src3_idx = inputs[2] ? inputs[2]->out_reg_idx : 0;
            inst->src4_idx = inputs[3] ? inputs[3]->out_reg_idx : 0;
            inst->line = (u16)node->loc.line;
            inst->column = (u16)node->loc.column;
            inst->opcode = (meta->category == SF_OP_CAT_SPECIAL) ? SF_OP_COPY : meta->opcode;
            emitted = true;
        }

        // --- 4. Task Management ---
        if (emitted) {
            bool is_sync = (meta->strategy == SF_STRATEGY_TWO_PASS_SYNC);
            bool is_reduction = (meta->strategy == SF_STRATEGY_REDUCTION);
            bool domain_changed = (current_domain_node_idx == UINT32_MAX || node->domain_node_idx != current_domain_node_idx);
            
            if (is_reduction && r_idx < SF_MAX_REGISTERS) prog->tensor_flags[r_idx] |= SF_TENSOR_FLAG_REDUCTION;
            if (is_sync) needs_sync_scratch = true;

            bool needs_split = domain_changed || is_sync || (current_strategy != meta->strategy);

            if (needs_split && task_count > 0) {
                sf_task* prev_task = &tasks[task_count - 1];
                prev_task->inst_count = start_instr_idx - prev_task->start_inst;
            }

            if (needs_split || task_count == 0) {
                tasks[task_count].start_inst = start_instr_idx;
                tasks[task_count].strategy = meta->strategy;
                tasks[task_count].flags = 0;
                u32 dom_node_idx = (node->domain_node_idx == UINT32_MAX) ? node_idx : node->domain_node_idx;
                tasks[task_count].domain_reg = ir->nodes[dom_node_idx].out_reg_idx;
                tasks[task_count].binding_offset = total_binding_count;
                tasks[task_count].binding_count = 0;
                current_domain_node_idx = node->domain_node_idx;
                current_strategy = meta->strategy;
                task_count++;
            }

            u16 ops[5] = { r_idx, 
                           inputs[0] ? inputs[0]->out_reg_idx : 0,
                           inputs[1] ? inputs[1]->out_reg_idx : 0,
                           inputs[2] ? inputs[2]->out_reg_idx : 0,
                           inputs[3] ? inputs[3]->out_reg_idx : 0 };
            
            sf_task* curr_task = &tasks[task_count - 1];

            // --- Barrier Planning (Static RAW Analysis) ---
            for (int k = 1; k < 5; ++k) {
                if (!inputs[k-1]) continue;
                u16 r = ops[k];
                if (r < SF_MAX_REGISTERS && (modified_regs[r / 8] & (1 << (r % 8)))) {
                    curr_task->flags |= SF_TASK_FLAG_BARRIER;
                    memset(modified_regs, 0, sizeof(modified_regs));
                    break;
                }
            }

            for (int k = 0; k < 5; ++k) {
                if (k > 0 && !inputs[k-1]) continue;
                u16 r = ops[k];
                bool found = false;
                u16 current_flags = (k == 0) ? SF_BINDING_FLAG_WRITE : SF_BINDING_FLAG_READ;
                if (is_reduction && k == 0) current_flags |= SF_BINDING_FLAG_REDUCTION;

                if (k == 0 && r < SF_MAX_REGISTERS) {
                    modified_regs[r / 8] |= (1 << (r % 8));
                }

                for (u32 b = 0; b < curr_task->binding_count; ++b) {
                    if (bindings[curr_task->binding_offset + b].reg_idx == r) {
                        bindings[curr_task->binding_offset + b].flags |= current_flags;
                        found = true; break;
                    }
                }
                if (!found) {
                    sf_bin_task_binding* b = &bindings[total_binding_count++];
                    b->reg_idx = r;
                    b->flags = current_flags;
                    curr_task->binding_count++;
                }
            }
        }
    }

    if (task_count > 0) {
        tasks[task_count - 1].inst_count = (u32)instr_count - tasks[task_count - 1].start_inst;
    }
        
    prog->code = instrs;
    prog->tasks = tasks;
    prog->bindings = bindings;
    prog->meta.instruction_count = (u32)instr_count;
    prog->meta.task_count = task_count;
    prog->meta.binding_count = total_binding_count;

    u32 reduction_reg_count = 0;
    for (int r = 0; r < (int)prog->meta.tensor_count; ++r) {
        if (prog->tensor_flags[r] & SF_TENSOR_FLAG_REDUCTION) reduction_reg_count = SF_MAX_REGISTERS; 
    }

    prog->meta.reduction_scratch_size = reduction_reg_count;
    prog->meta.sync_scratch_size = needs_sync_scratch ? 1024 : 0; 

    return true;
}