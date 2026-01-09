#include "../sf_passes.h"
#include "../sf_compiler_internal.h"
#include <sionflow/base/sf_log.h>
#include <sionflow/isa/sf_op_defs.h>
#include <sionflow/base/sf_shape.h>
#include <string.h>

bool sf_pass_validate(sf_pass_ctx* ctx, sf_compiler_diag* diag) {
    // We delegate everything to the generated validator.
    // If we need manual overrides, they can go here, but the goal is 100% automation.
    return sf_pass_validate_gen(ctx, diag);
}
