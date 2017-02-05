#include <tbb/task.h>

#include <lua.hpp>
#include <lauxlib.h>
#include <lualib.h>

#include <set>
#include <map>
#include <vector>
#include <tuple>
#include <cassert>

struct node
{
    int var;
    const node* lo;
    const node* hi;
};

node terminal_nodes[2] = {
    node{ INT_MAX, &terminal_nodes[0], &terminal_nodes[0] },
    node{ INT_MAX - 1, &terminal_nodes[1], &terminal_nodes[1] }
};
const node* const false_node = &terminal_nodes[0];
const node* const true_node = &terminal_nodes[1];

namespace opcode
{
    enum {
        bdd_and,
        bdd_or
    };
}

bool is_terminal(const node* n)
{
    return n == false_node || n == true_node;
}

bool terminal_value(const node* n)
{
    return n == true_node;
}

const node* operation(const node* bdd1, const node* bdd2, int op)
{
    switch (op)
    {
    case opcode::bdd_and:
        return (terminal_value(bdd1) && terminal_value(bdd2)) ? true_node : false_node;
    case opcode::bdd_or:
        return (terminal_value(bdd1) || terminal_value(bdd2)) ? true_node : false_node;
    }
    return NULL;
}

class robdd
{
    struct node_cmp
    {
        bool operator()(const node& a, const node& b) const
        {
            return std::tie(a.var, a.lo, a.hi) < std::tie(b.var, b.lo, b.hi);
        }
    };

    std::set<node, node_cmp> hashtable;

    struct cache_key
    {
        const node* lo;
        const node* hi;
        int op;

        bool operator<(const cache_key& other) const
        {
            return std::tie(lo, hi, op) < std::tie(other.lo, other.hi, other.op);
        }
    };

    std::map<cache_key, const node*> cache;

public:
    const node* make_node(int var, const node* lo, const node* hi)
    {
        // enforce no-redundance constraint of ROBDD
        if (lo == hi) return lo;
        // enforce uniqueness constraint of ROBDD
        // hash table returns the node if it exists
        // and inserts the node if it doesn't
        return &*hashtable.insert(node{ var, lo, hi }).first;
    }

    const node* apply(const node* bdd1, const node* bdd2, int op)
    {
        auto found = cache.find(cache_key{ bdd1,bdd2,op }); 
        if (found != cache.end())
        {
            return found->second;
        }
        
        if (is_terminal(bdd1) && is_terminal(bdd2))
        {
            return operation(bdd1, bdd2, op);
        }
        
        const node* n;
        if (bdd1->var == bdd2->var)
        {
            const node* lo = apply(bdd1->lo, bdd2->lo, op);
            const node* hi = apply(bdd1->hi, bdd2->hi, op);
            n = make_node(bdd1->var, lo, hi);
        }
        else if (bdd1->var < bdd2->var)
        {
            const node* lo = apply(bdd1->lo, bdd2, op);
            const node* hi = apply(bdd1->hi, bdd2, op);
            n = make_node(bdd1->var, lo, hi);
        }
        else {
            const node* lo = apply(bdd1, bdd2->lo, op);
            const node* hi = apply(bdd1, bdd2->hi, op);
            n = make_node(bdd2->var, lo, hi);
        }
        cache.emplace(cache_key{ bdd1, bdd2, op }, n);
        return n;
    }
};

struct bdd_instr
{
    enum {
        opcode_newvar,
        opcode_and,
        opcode_or,
    };

    enum {
        operand_newvar_id
    };

    enum {
        operand_and_dst_id,
        operand_and_src1_id,
        operand_and_src2_id,
    };

    enum {
        operand_or_dst_id,
        operand_or_src1_id,
        operand_or_src2_id,
    };

    int opcode;
    int operands[3];
};

void decode(
    bdd_instr* instrs, int num_instrs,
    int num_ast_nodes,
    robdd* r)
{
    std::vector<const node*> ast2bdd_v(num_ast_nodes);
    const node** ast2bdd = ast2bdd_v.data() - 1;

    for (int i = 0; i < num_instrs; i++)
    {
        const bdd_instr& inst = instrs[i];

        switch (inst.opcode)
        {
        case bdd_instr::opcode_newvar:
        {
            int ast_id = inst.operands[bdd_instr::operand_newvar_id];
            printf("new %d\n", ast_id);

            int var_id = ast_id; // straightforward mapping.
            ast2bdd[ast_id] = r->make_node(ast_id, false_node, true_node);

            break;
        }
        case bdd_instr::opcode_and:
        {
            int dst_ast_id = inst.operands[bdd_instr::operand_and_dst_id];
            int src1_ast_id = inst.operands[bdd_instr::operand_and_src1_id];
            int src2_ast_id = inst.operands[bdd_instr::operand_and_src2_id];
            printf("%d = %d AND %d\n", dst_ast_id, src1_ast_id, src2_ast_id);

            const node* src1_bdd = ast2bdd[src1_ast_id];
            const node* src2_bdd = ast2bdd[src2_ast_id];
            ast2bdd[dst_ast_id] = r->apply(src1_bdd, src2_bdd, opcode::bdd_and);

            break;
        }
        case bdd_instr::opcode_or:
        {
            int dst_ast_id = inst.operands[bdd_instr::operand_or_dst_id];
            int src1_ast_id = inst.operands[bdd_instr::operand_or_src1_id];
            int src2_ast_id = inst.operands[bdd_instr::operand_or_src2_id];
            printf("%d = %d OR %d\n", dst_ast_id, src1_ast_id, src2_ast_id);

            const node* src1_bdd = ast2bdd[src1_ast_id];
            const node* src2_bdd = ast2bdd[src2_ast_id];
            ast2bdd[dst_ast_id] = r->apply(src1_bdd, src2_bdd, opcode::bdd_or);

            break;
        }
        default:
            assert(false);
        }
    }
}

std::vector<bdd_instr> g_bdd_instructions;
int g_next_ast_id = 1;

int l_var_newindex(lua_State* L)
{
    luaL_error(L, "Cannot write to variables table");
    return 0;
}

int l_var_index(lua_State* L)
{
    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr new_instr;
    new_instr.opcode = bdd_instr::opcode_newvar;
    new_instr.operands[bdd_instr::operand_newvar_id] = *ast_id;
    g_bdd_instructions.push_back(new_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    // var[bdd_userdata] = var_name;
    lua_pushvalue(L, -1);
    lua_pushvalue(L, 2);
    lua_rawset(L, 1);

    return 1;
}

int l_and(lua_State* L)
{
    int* ast1_id = (int*)luaL_checkudata(L, 1, "ast");
    int* ast2_id = (int*)luaL_checkudata(L, 2, "ast");

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr and_instr;
    and_instr.opcode = bdd_instr::opcode_and;
    and_instr.operands[bdd_instr::operand_and_dst_id] = *ast_id;
    and_instr.operands[bdd_instr::operand_and_src1_id] = *ast1_id;
    and_instr.operands[bdd_instr::operand_and_src2_id] = *ast2_id;
    g_bdd_instructions.push_back(and_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int l_or(lua_State* L)
{
    int* ast1_id = (int*)luaL_checkudata(L, 1, "ast");
    int* ast2_id = (int*)luaL_checkudata(L, 2, "ast");

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr or_instr;
    or_instr.opcode = bdd_instr::opcode_or;
    or_instr.operands[bdd_instr::operand_or_dst_id] = *ast_id;
    or_instr.operands[bdd_instr::operand_or_src1_id] = *ast1_id;
    or_instr.operands[bdd_instr::operand_or_src2_id] = *ast2_id;
    g_bdd_instructions.push_back(or_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <filename>\n", argc >= 1 ? argv[0] : "robdd");
        return 0;
    }

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    luaL_newmetatable(L, "ast");
    {
        lua_pushcfunction(L, l_and);
        lua_setfield(L, -2, "__mul");
        
        lua_pushcfunction(L, l_or);
        lua_setfield(L, -2, "__add");
    }
    lua_pop(L, 1);

    luaL_newmetatable(L, "var_mt");
    {
        lua_pushcfunction(L, l_var_newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, l_var_index);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_newmetatable(L, "var_mt");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "var");
    
    if (luaL_dofile(L, argv[1]))
    {
        printf("%s\n", lua_tostring(L, -1));
        return 1;
    }

    int num_ast_nodes = g_next_ast_id - 1;
    robdd r;
    decode(g_bdd_instructions.data(), (int)g_bdd_instructions.size(), num_ast_nodes, &r);
}