#include <tbb/task.h>

#include <lua.hpp>
#include <lauxlib.h>
#include <lualib.h>

#include <set>
#include <map>
#include <vector>
#include <tuple>

struct node
{
    enum {
        terminal_value_bit = 0b0001,
        is_terminal_bit    = 0b0010,
    };

    int var;
    const node* lo;
    const node* hi;
};

const node* const false_node = (node*)(0);
const node* const true_node = (node*)(node::terminal_value_bit);

namespace opcode
{
    enum {
        disj = 0,
        conj = 1
    };
}

bool is_terminal(const node* n)
{
    return ((uintptr_t)n & node::is_terminal_bit) != 0;
}

bool terminal_value(const node* n)
{
    return ((uintptr_t)n & node::terminal_value_bit) != 0;
}

const node* operation(const node* bdd1, const node* bdd2, int op)
{
    switch (op)
    {
    case opcode::disj:
        return (terminal_value(bdd1) || terminal_value(bdd2)) ? true_node : false_node;
    case opcode::conj:
        return (terminal_value(bdd1) && terminal_value(bdd2)) ? true_node : false_node;
    }
    return NULL;
}

class bdd_builder
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

void decode(bdd_instr* instrs, int num_instrs)
{
    for (int i = 0; i < num_instrs; i++)
    {
        const bdd_instr& inst = instrs[i];

        switch (inst.opcode)
        {
        case bdd_instr::opcode_newvar:
            printf("new %d\n", 
                inst.operands[bdd_instr::operand_newvar_id]);
            break;
        case bdd_instr::opcode_and:
            printf("%d = %d AND %d\n", 
                inst.operands[bdd_instr::operand_and_dst_id],
                inst.operands[bdd_instr::operand_and_src1_id],
                inst.operands[bdd_instr::operand_and_src2_id]);
            break;
        case bdd_instr::opcode_or:
            printf("%d = %d OR %d\n",
                inst.operands[bdd_instr::operand_or_dst_id],
                inst.operands[bdd_instr::operand_or_src1_id],
                inst.operands[bdd_instr::operand_or_src2_id]);
            break;
        }
    }
}

std::vector<bdd_instr> g_bdd_instructions;
int g_next_bdd_id = 1;

int l_var_newindex(lua_State* L)
{
    luaL_error(L, "Cannot write to variables table");
    return 0;
}

int l_var_index(lua_State* L)
{
    int* bdd_id = (int*)lua_newuserdata(L, sizeof(int));
    *bdd_id = g_next_bdd_id;
    g_next_bdd_id += 1;

    bdd_instr new_instr;
    new_instr.opcode = bdd_instr::opcode_newvar;
    new_instr.operands[bdd_instr::operand_newvar_id] = *bdd_id;
    g_bdd_instructions.push_back(new_instr);

    luaL_newmetatable(L, "bdd");
    lua_setmetatable(L, -2);

    // var[bdd_userdata] = var_name;
    lua_pushvalue(L, -1);
    lua_pushvalue(L, 2);
    lua_rawset(L, 1);

    return 1;
}

int l_and(lua_State* L)
{
    int* bdd1_id = (int*)luaL_checkudata(L, 1, "bdd");
    int* bdd2_id = (int*)luaL_checkudata(L, 2, "bdd");

    int* bdd_id = (int*)lua_newuserdata(L, sizeof(int));
    *bdd_id = g_next_bdd_id;
    g_next_bdd_id += 1;

    bdd_instr and_instr;
    and_instr.opcode = bdd_instr::opcode_and;
    and_instr.operands[bdd_instr::operand_and_dst_id] = *bdd_id;
    and_instr.operands[bdd_instr::operand_and_src1_id] = *bdd1_id;
    and_instr.operands[bdd_instr::operand_and_src2_id] = *bdd2_id;
    g_bdd_instructions.push_back(and_instr);

    luaL_newmetatable(L, "bdd");
    lua_setmetatable(L, -2);

    return 1;
}

int l_or(lua_State* L)
{
    int* bdd1_id = (int*)luaL_checkudata(L, 1, "bdd");
    int* bdd2_id = (int*)luaL_checkudata(L, 2, "bdd");

    int* bdd_id = (int*)lua_newuserdata(L, sizeof(int));
    *bdd_id = g_next_bdd_id;
    g_next_bdd_id += 1;

    bdd_instr or_instr;
    or_instr.opcode = bdd_instr::opcode_or;
    or_instr.operands[bdd_instr::operand_or_dst_id] = *bdd_id;
    or_instr.operands[bdd_instr::operand_or_src1_id] = *bdd1_id;
    or_instr.operands[bdd_instr::operand_or_src2_id] = *bdd2_id;
    g_bdd_instructions.push_back(or_instr);

    luaL_newmetatable(L, "bdd");
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

    luaL_newmetatable(L, "bdd");
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

    decode(g_bdd_instructions.data(), (int)g_bdd_instructions.size());
}