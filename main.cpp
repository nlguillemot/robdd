// TODO:
// * Make the root node logic more robust (handle multiple roots?)
// * Extend the algorithm to multi-core using fork/join parallelism
// * Improve the hashtable data-structure (used to have unique nodes)
// * Improve the cache data-structure (used to avoid recomputing the ITE)
// * Try solving a more interesting problem (full adder? N-queens?)

#include <tbb/task.h>

#include <lua.hpp>
#include <lauxlib.h>
#include <lualib.h>

#include <set>
#include <map>
#include <unordered_set>
#include <vector>
#include <tuple>
#include <string>
#include <cassert>
#include <cstdlib>

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

enum {
    ast_id_false,
    ast_id_true,
    ast_id_user
};

void decode(
    bdd_instr* instrs, int num_instrs,
    int num_user_ast_nodes,
    robdd* r,
    const node** root)
{
    std::vector<const node*> astnode2bddnode(ast_id_user + num_user_ast_nodes);
    astnode2bddnode[ast_id_false] = false_node;
    astnode2bddnode[ast_id_true] = true_node;

    const node** ast2bdd = astnode2bddnode.data();

    int main_ast_id = -1;

    for (int i = 0; i < num_instrs; i++)
    {
        const bdd_instr& inst = instrs[i];

        switch (inst.opcode)
        {
        case bdd_instr::opcode_newvar:
        {
            int ast_id = inst.operands[bdd_instr::operand_newvar_id];
            printf("new %d\n", ast_id);

            ast2bdd[ast_id] = r->make_node(ast_id, false_node, true_node);

            if (main_ast_id == -1 || ast_id < main_ast_id)
            {
                main_ast_id = ast_id;
                *root = ast2bdd[ast_id];
            }

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

            if (src1_bdd->var == main_ast_id || src2_bdd->var == main_ast_id)
                *root = ast2bdd[dst_ast_id];

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

            if (src1_bdd->var == main_ast_id || src2_bdd->var == main_ast_id)
                *root = ast2bdd[dst_ast_id];

            break;
        }
        default:
            assert(false);
        }
    }
}

std::map<int, std::string> g_astid2name;

void write_dot(const robdd* bdd, const node* root, const char* fn)
{
    FILE* f = fopen(fn, "w");

    fprintf(f, "digraph {\n");

    std::vector<const node*> nodes2add;
    if (root)
        nodes2add.push_back(root);

    std::unordered_set<const node*> added;
    added.insert(false_node);
    added.insert(true_node);

    std::unordered_set<const node*> declared;
    if (root != true_node)
    {
        fprintf(f, "  n%p [label=\"0\",shape=box];\n", false_node);
        declared.insert(false_node);
    }
    if (root != false_node)
    {
        fprintf(f, "  n%p [label=\"1\",shape=box];\n", true_node);
        declared.insert(true_node);
    }
    if (root && root != false_node && root != true_node)
    {
        fprintf(f, "  n%p [label=\"%s\"];\n", root, g_astid2name.at(root->var).c_str());
        declared.insert(root);
    }

    while (!nodes2add.empty())
    {
        const node* n = nodes2add.back();
        nodes2add.pop_back();

        if (added.find(n) != added.end())
            continue;

        const node* children[] = { n->lo, n->hi };
        for (const node* child : children)
        {
            if (declared.insert(child).second)
                fprintf(f, "  n%p [label=\"%s\"];\n", child, g_astid2name.at(child->var).c_str());

            fprintf(f, "  n%p -> n%p [style=%s];\n", n, child, child == n->lo ? "dotted" : "solid");

            if (added.find(child) == added.end())
                nodes2add.push_back(child);
        }

        added.insert(n);
    }

    fprintf(f, "}\n");

    fclose(f);

    std::string dotcmd = std::string("packages\\Graphviz.2.38.0.2\\dot.exe") + " -Tpng " + fn + " -o " + fn + ".png";
    if (system(dotcmd.c_str()) == 0)
    {
        std::string pngcmd = std::string(fn) + ".png";
        system(pngcmd.c_str());
    }
}

std::vector<bdd_instr> g_bdd_instructions;
int g_next_ast_id = ast_id_user;

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

    g_astid2name[*ast_id] = luaL_checkstring(L, 2);

    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_rawset(L, -5);

    return 1;
}

int l_and(lua_State* L)
{
    int ast1_id = *(int*)luaL_checkudata(L, 1, "ast");
    int ast2_id = *(int*)luaL_checkudata(L, 2, "ast");

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr and_instr;
    and_instr.opcode = bdd_instr::opcode_and;
    and_instr.operands[bdd_instr::operand_and_dst_id] = *ast_id;
    and_instr.operands[bdd_instr::operand_and_src1_id] = ast1_id;
    and_instr.operands[bdd_instr::operand_and_src2_id] = ast2_id;
    g_bdd_instructions.push_back(and_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int l_or(lua_State* L)
{
    int ast1_id = *(int*)luaL_checkudata(L, 1, "ast");
    int ast2_id = *(int*)luaL_checkudata(L, 2, "ast");

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr or_instr;
    or_instr.opcode = bdd_instr::opcode_or;
    or_instr.operands[bdd_instr::operand_or_dst_id] = *ast_id;
    or_instr.operands[bdd_instr::operand_or_src1_id] = ast1_id;
    or_instr.operands[bdd_instr::operand_or_src2_id] = ast2_id;
    g_bdd_instructions.push_back(or_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <input file> [output file]\n", argc >= 1 ? argv[0] : "robdd");
        return 0;
    }

    const char* infile = argv[1];
    const char* outfile = argc >= 3 ? argv[2] : NULL;

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

    lua_getglobal(L, "var");
    lua_pushboolean(L, 0);
    int* false_ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *false_ast_id = ast_id_false;
    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    lua_getglobal(L, "var");
    lua_pushboolean(L, 1);
    int* true_ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *true_ast_id = ast_id_true;
    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    
    if (luaL_dofile(L, infile))
    {
        printf("%s\n", lua_tostring(L, -1));
        return 1;
    }

    robdd bdd;
    const node* root = NULL;

    decode(
        g_bdd_instructions.data(), (int)g_bdd_instructions.size(), 
        g_next_ast_id - ast_id_user, // num user ast nodes
        &bdd, &root);

    write_dot(&bdd, root, outfile);
}
