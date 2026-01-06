#include <sionflow/compiler/sf_compiler.h>

const sf_op_metadata SF_OP_METADATA[SF_NODE_COUNT] = {
    [SF_NODE_UNKNOWN] = { "Unknown", 0, SF_OP_CAT_SPECIAL, SF_STRATEGY_DEFAULT, 0, 0, 0, 0, SF_ACCESS_SPECIAL, {NULL, NULL, NULL, NULL}, 0 },

#define SF_OP(_s, _n, _op, _cat, _strat, _in, _out, _t_rule, _s_rule, _a_rule, _p1, _p2, _p3, _p4, _kt, _ke, _ar) \
    [SF_NODE_##_s] = { \
        _n, \
        SF_OP_##_op, \
        _cat, \
        _strat, \
        _in, \
        _out, \
        _t_rule, \
        _s_rule, \
        _a_rule, \
        { _p1, _p2, _p3, _p4 }, \
        _ar \
    },

    SF_OP_LIST
#undef SF_OP
};