#include "sf_passes.h"
#include "sf_compiler_internal.h"
#include <sionflow/isa/sf_opcodes.h>
#include <sionflow/base/sf_log.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

/**
 * Clean CodeGen
 * Now it only handles the actual emission of data and instructions.
 * Planning (Tasks, Strides, Barriers) is moved to sf_pass_task_plan.
 */

bool sf_codegen_emit(sf_program* prog, sf_pass_ctx* ctx, sf_arena* arena) {
    sf_graph_ir* ir = ctx->ir;
    sf_ir_node** sorted = ctx->sorted_nodes;
    size_t sorted_count = ctx->sorted_count;

    // 1. Calculate Program Header Info
    u16 max_reg = 0;
    u32 symbol_count = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        if (ir->nodes[i].type == SF_NODE_UNKNOWN) continue;
        if (ir->nodes[i].out_reg_idx > max_reg) max_reg = ir->nodes[i].out_reg_idx;
        if (ir->nodes[i].id && strcmp(ir->nodes[i].id, "unknown") != 0) symbol_count++;
    }

    prog->meta.tensor_count = (u32)max_reg + 1; 
    prog->meta.symbol_count = symbol_count;
    prog->symbols = (symbol_count > 0) ? SF_ARENA_PUSH(arena, sf_bin_symbol, symbol_count) : NULL;
    prog->tensor_infos = SF_ARENA_PUSH(arena, sf_type_info, prog->meta.tensor_count);
    prog->tensor_data = SF_ARENA_PUSH(arena, void*, prog->meta.tensor_count);
    prog->tensor_flags = SF_ARENA_PUSH(arena, uint8_t, prog->meta.tensor_count);
    memset(prog->tensor_flags, 0, prog->meta.tensor_count);

    // 2. Emit Symbols and Tensors
    u32 current_symbol = 0;
    for (size_t i = 0; i < ir->node_count; ++i) {
        sf_ir_node* node = &ir->nodes[i];
        if (node->type == SF_NODE_UNKNOWN) continue;

        u16 r_idx = node->out_reg_idx;
        prog->tensor_infos[r_idx] = node->out_info;
        
        if (node->id && strcmp(node->id, "unknown") != 0) {
            sf_bin_symbol* sym = &prog->symbols[current_symbol++];
            strncpy(sym->name, node->id, SF_MAX_SYMBOL_NAME - 1);
            sym->name_hash = sf_fnv1a_hash(sym->name);
            
            u16 target_reg = r_idx;
            if (node->type == SF_NODE_OUTPUT) {
                sf_ir_node* src = sf_ir_find_input_by_name(ir, (u32)i, "in");
                if (src) target_reg = src->out_reg_idx;
            }
            sym->register_idx = target_reg;
            
            // Symbol Flags (for Port Mapping)
            if (node->type == SF_NODE_INPUT)  sym->flags |= SF_SYMBOL_FLAG_INPUT;
            if (node->type == SF_NODE_OUTPUT) sym->flags |= SF_SYMBOL_FLAG_OUTPUT;
            
            // Transfer resource flags to symbol metadata
            sym->flags |= (node->resource_flags & 0x1F); // Forward resource flags (Persistent, Readonly, etc)

            if (node->type == SF_NODE_INPUT || node->type == SF_NODE_OUTPUT) {
                prog->tensor_flags[target_reg] |= SF_TENSOR_FLAG_ALIAS;
            }
        }

        if (node->type == SF_NODE_CONST) {
            prog->tensor_data[r_idx] = node->const_data;
            prog->tensor_flags[r_idx] |= SF_TENSOR_FLAG_CONSTANT;
        }
    }

    // 3. Emit Instructions (Straightforward mapping)
    sf_instruction* instrs = SF_ARENA_PUSH(arena, sf_instruction, sorted_count);
    u32 instr_count = 0;
    bool needs_sync = false;

    for (size_t i = 0; i < sorted_count; ++i) {
        sf_ir_node* node = sorted[i];
        if (node->type == SF_NODE_UNKNOWN || node->type == SF_NODE_INPUT || 
            node->type == SF_NODE_OUTPUT || node->type == SF_NODE_CONST) continue;

        const sf_op_metadata* meta = &SF_OP_METADATA[node->type];
        sf_instruction* inst = &instrs[instr_count++];
        memset(inst, 0, sizeof(sf_instruction));
        inst->opcode = meta->opcode;
        inst->dest_idx = node->out_reg_idx;
        inst->line = (u16)node->loc.line;
        inst->column = (u16)node->loc.column;

        if (meta->strategy == SF_STRATEGY_TWO_PASS_SYNC) needs_sync = true;

        for (u32 k = 0; k < 4; ++k) {
            if (meta->ports[k]) {
                sf_ir_node* src = find_input_source(ir, (u32)(node - ir->nodes), k);
                if (src) {
                    u16 r = src->out_reg_idx;
                    if (k == 0) inst->src1_idx = r;
                    else if (k == 1) inst->src2_idx = r;
                    else if (k == 2) inst->src3_idx = r;
                    else if (k == 3) inst->src4_idx = r;
                }
            }
        }
    }

    // 4. Transfer Task Results from Pipeline Context
    prog->code = instrs;
    prog->tasks = ctx->tasks;
    prog->bindings = ctx->bindings;
    prog->meta.instruction_count = instr_count;
    prog->meta.task_count = ctx->task_count;
    prog->meta.binding_count = ctx->binding_count;
    prog->meta.sync_scratch_size = needs_sync ? 1024 : 0;

    return true;
}