// TODO:
// * Try solving a more interesting problem (full adder? N-queens?)
// * Extend the algorithm to multi-core using fork/join parallelism
// * Improve the hashtable data-structure (used to have unique nodes)
// * Improve the cache data-structure (used to avoid recomputing the ITE)

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
    node{ INT_MAX,     &terminal_nodes[0], &terminal_nodes[0] },
    node{ INT_MAX - 1, &terminal_nodes[1], &terminal_nodes[1] }
};
const node* const false_node = &terminal_nodes[0];
const node* const true_node = &terminal_nodes[1];

namespace opcode
{
    enum {
        bdd_and,
        bdd_or,
        bdd_xor
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
    case opcode::bdd_xor:
        return (terminal_value(bdd1) != terminal_value(bdd2)) ? true_node : false_node;
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
        opcode_newinput,
        opcode_and,
        opcode_or,
        opcode_xor,
        opcode_not
    };

    int opcode;

    union
    {
        struct {
            int operand_newinput_id;
            const std::string* operand_newinput_name;
        };

        struct {
            int operand_and_dst_id;
            int operand_and_src1_id;
            int operand_and_src2_id;
        };

        struct {
            int operand_or_dst_id;
            int operand_or_src1_id;
            int operand_or_src2_id;
        };

        struct {
            int operand_xor_dst_id;
            int operand_xor_src1_id;
            int operand_xor_src2_id;
        };

        struct {
            int operand_not_dst_id;
            int operand_not_src_id;
        };
    };
};

enum {
    ast_id_false,
    ast_id_true,
    ast_id_user
};

void decode(
    int num_instrs, bdd_instr* instrs,
    int num_user_ast_nodes,
    int num_root_ast_ids, int* root_ast_ids,
    robdd* r,
    const node** roots)
{
    std::vector<const node*> astnode2bddnode(ast_id_user + num_user_ast_nodes);
    astnode2bddnode[ast_id_false] = false_node;
    astnode2bddnode[ast_id_true] = true_node;

    for (int root_ast_idx = 0; root_ast_idx < num_root_ast_ids; root_ast_idx++)
    {
        if (root_ast_ids[root_ast_idx] == ast_id_true)
            roots[root_ast_idx] = true_node;
        else if (root_ast_ids[root_ast_idx] == ast_id_false)
            roots[root_ast_idx] = false_node;
    }

    const node** ast2bdd = astnode2bddnode.data();

    for (int i = 0; i < num_instrs; i++)
    {
        const bdd_instr& inst = instrs[i];

        int inst_dst_ast_id = -1;
        const node* inst_dst_node = NULL;

        switch (inst.opcode)
        {
        case bdd_instr::opcode_newinput:
        {
            int ast_id = inst.operand_newinput_id;
            const char* ast_name = inst.operand_newinput_name->c_str();

            printf("new %d (%s)\n", ast_id, ast_name);

            const node* new_bdd = r->make_node(ast_id, false_node, true_node);
            
            ast2bdd[ast_id] = new_bdd;

            inst_dst_ast_id = ast_id;
            inst_dst_node = new_bdd;

            break;
        }
        case bdd_instr::opcode_and:
        {
            int dst_ast_id  = inst.operand_and_dst_id;
            int src1_ast_id = inst.operand_and_src1_id;
            int src2_ast_id = inst.operand_and_src2_id;
            printf("%d = %d AND %d\n", dst_ast_id, src1_ast_id, src2_ast_id);

            const node* src1_bdd = ast2bdd[src1_ast_id];
            const node* src2_bdd = ast2bdd[src2_ast_id];
            const node* new_bdd = r->apply(src1_bdd, src2_bdd, opcode::bdd_and);

            ast2bdd[dst_ast_id] = new_bdd;

            inst_dst_ast_id = dst_ast_id;
            inst_dst_node = new_bdd;

            break;
        }
        case bdd_instr::opcode_or:
        {
            int dst_ast_id  = inst.operand_or_dst_id;
            int src1_ast_id = inst.operand_or_src1_id;
            int src2_ast_id = inst.operand_or_src2_id;
            printf("%d = %d OR %d\n", dst_ast_id, src1_ast_id, src2_ast_id);

            const node* src1_bdd = ast2bdd[src1_ast_id];
            const node* src2_bdd = ast2bdd[src2_ast_id];
            const node* new_bdd = r->apply(src1_bdd, src2_bdd, opcode::bdd_or);

            ast2bdd[dst_ast_id] = new_bdd;

            inst_dst_ast_id = dst_ast_id;
            inst_dst_node = new_bdd;

            break;
        }
        case bdd_instr::opcode_xor:
        {
            int dst_ast_id = inst.operand_xor_dst_id;
            int src1_ast_id = inst.operand_xor_src1_id;
            int src2_ast_id = inst.operand_xor_src2_id;
            printf("%d = %d XOR %d\n", dst_ast_id, src1_ast_id, src2_ast_id);

            const node* src1_bdd = ast2bdd[src1_ast_id];
            const node* src2_bdd = ast2bdd[src2_ast_id];
            const node* new_bdd = r->apply(src1_bdd, src2_bdd, opcode::bdd_xor);

            ast2bdd[dst_ast_id] = new_bdd;

            inst_dst_ast_id = dst_ast_id;
            inst_dst_node = new_bdd;

            break;
        }
        case bdd_instr::opcode_not:
        {
            int dst_ast_id = inst.operand_not_dst_id;
            int src_ast_id = inst.operand_not_src_id;
            printf("%d = NOT %d\n", dst_ast_id, src_ast_id);

            const node* src_bdd = ast2bdd[src_ast_id];
            const node* new_bdd = r->apply(src_bdd, true_node, opcode::bdd_xor);

            ast2bdd[dst_ast_id] = new_bdd;

            inst_dst_ast_id = dst_ast_id;
            inst_dst_node = new_bdd;

            break;
        }
        default:
            assert(false);
        }

        for (int root_ast_idx = 0; root_ast_idx < num_root_ast_ids; root_ast_idx++)
        {
            if (inst_dst_ast_id == root_ast_ids[root_ast_idx])
            {
                roots[root_ast_idx] = inst_dst_node;
            }
        }
    }
}

std::map<int, std::string> g_astid2name;

void write_dot(
    const char* title,
    const robdd* bdd,
    int num_roots, const node** roots, const std::string* root_names,
    const char* fn)
{
    FILE* f = fopen(fn, "w");

    fprintf(f, "digraph {\n");

    fprintf(f, "  labelloc=\"t\";\n");
    fprintf(f, "  label=\"%s\";\n", title);

    std::vector<const node*> nodes2add;
    nodes2add.insert(nodes2add.end(), roots, roots + num_roots);

    std::unordered_set<const node*> added;
    added.insert(false_node);
    added.insert(true_node);

    std::unordered_set<const node*> declared;

    for (int root_idx = 0; root_idx < num_roots; root_idx++)
    {
        if (declared.find(roots[root_idx]) != end(declared))
        {
            continue;
        }

        if (roots[root_idx] == false_node)
        {
            fprintf(f, "  n%p [label=\"0\",shape=box];\n", false_node);
        }
        else if (roots[root_idx] == true_node)
        {
            fprintf(f, "  n%p [label=\"1\",shape=box];\n", true_node);
        }
        else
        {
            fprintf(f, "  n%p [label=\"%s\"];\n", roots[root_idx], g_astid2name.at(roots[root_idx]->var).c_str());
        }

        declared.insert(roots[root_idx]);
    }

    for (int root_idx = 0; root_idx < num_roots; root_idx++)
    {
        fprintf(f, "  r%d [label=\"%s\",style=filled];\n", root_idx, root_names[root_idx].c_str());
        fprintf(f, "  r%d -> n%p [style=solid];\n", root_idx, roots[root_idx]);
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
            {
                if (child == false_node)
                {
                    fprintf(f, "  n%p [label=\"0\",shape=box];\n", false_node);
                }
                else if (child == true_node)
                {
                    fprintf(f, "  n%p [label=\"1\",shape=box];\n", true_node);
                }
                else
                {
                    fprintf(f, "  n%p [label=\"%s\"];\n", child, g_astid2name.at(child->var).c_str());
                }
            }

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

int l_input_newindex(lua_State* L)
{
    luaL_error(L, "Cannot write to inputs table");
    return 0;
}

int l_input_index(lua_State* L)
{
    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    auto astid2name = g_astid2name.emplace(*ast_id, luaL_checkstring(L, 2)).first;

    bdd_instr new_instr;
    new_instr.opcode = bdd_instr::opcode_newinput;
    new_instr.operand_newinput_id = *ast_id;
    new_instr.operand_newinput_name = &astid2name->second;
    g_bdd_instructions.push_back(new_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_rawset(L, -5);

    return 1;
}

int arg_to_ast(lua_State* L, int argidx)
{
    if (lua_isboolean(L, argidx))
        return lua_toboolean(L, argidx) ? ast_id_true : ast_id_false;
    else
        return *(int*)luaL_checkudata(L, argidx, "ast");
}

int l_and(lua_State* L)
{
    int ast1_id = arg_to_ast(L, 1);
    int ast2_id = arg_to_ast(L, 2);

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr and_instr;
    and_instr.opcode = bdd_instr::opcode_and;
    and_instr.operand_and_dst_id = *ast_id;
    and_instr.operand_and_src1_id = ast1_id;
    and_instr.operand_and_src2_id = ast2_id;
    g_bdd_instructions.push_back(and_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int l_or(lua_State* L)
{
    int ast1_id = arg_to_ast(L, 1);
    int ast2_id = arg_to_ast(L, 2);

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr or_instr;
    or_instr.opcode = bdd_instr::opcode_or;
    or_instr.operand_or_dst_id = *ast_id;
    or_instr.operand_or_src1_id = ast1_id;
    or_instr.operand_or_src2_id = ast2_id;
    g_bdd_instructions.push_back(or_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int l_xor(lua_State* L)
{
    int ast1_id = arg_to_ast(L, 1);
    int ast2_id = arg_to_ast(L, 2);

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr xor_instr;
    xor_instr.opcode = bdd_instr::opcode_xor;
    xor_instr.operand_xor_dst_id = *ast_id;
    xor_instr.operand_xor_src1_id = ast1_id;
    xor_instr.operand_xor_src2_id = ast2_id;
    g_bdd_instructions.push_back(xor_instr);

    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);

    return 1;
}

int l_not(lua_State* L)
{
    int ast_id_in = arg_to_ast(L, 1);

    int* ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *ast_id = g_next_ast_id;
    g_next_ast_id += 1;

    bdd_instr not_instr;
    not_instr.opcode = bdd_instr::opcode_not;
    not_instr.operand_not_dst_id = *ast_id;
    not_instr.operand_not_src_id = ast_id_in;
    g_bdd_instructions.push_back(not_instr);

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

    std::string default_outfile = std::string(argv[1]) + ".dot";
    const char* outfile = argc >= 3 ? argv[2] : default_outfile.c_str();

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    luaL_newmetatable(L, "ast");
    {
        lua_pushcfunction(L, l_and);
        lua_setfield(L, -2, "__mul");
        
        lua_pushcfunction(L, l_or);
        lua_setfield(L, -2, "__add");

        lua_pushcfunction(L, l_xor);
        lua_setfield(L, -2, "__pow");

        lua_pushcfunction(L, l_not);
        lua_setfield(L, -2, "__unm");
    }
    lua_pop(L, 1);

    luaL_newmetatable(L, "input_mt");
    {
        lua_pushcfunction(L, l_input_newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, l_input_index);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_newmetatable(L, "input_mt");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "input");

    lua_getglobal(L, "input");
    lua_pushboolean(L, 0);
    int* false_ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *false_ast_id = ast_id_false;
    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    lua_getglobal(L, "input");
    lua_pushboolean(L, 1);
    int* true_ast_id = (int*)lua_newuserdata(L, sizeof(int));
    *true_ast_id = ast_id_true;
    luaL_newmetatable(L, "ast");
    lua_setmetatable(L, -2);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    lua_newtable(L);
    lua_setglobal(L, "output");
    
    if (luaL_dofile(L, infile))
    {
        printf("%s\n", lua_tostring(L, -1));
        return 1;
    }

    std::vector<int> root_ast_ids;
    std::vector<std::string> root_ast_names;
    {
        auto read_results = [](lua_State* L)
        {
            auto p_root_ast_ids = (std::vector<int>*)lua_touserdata(L, 1);
            auto p_root_ast_names = (std::vector<std::string>*)lua_touserdata(L, 2);

            lua_getglobal(L, "output");
            lua_pushnil(L);
            while (lua_next(L, -2))
            {
                lua_pushvalue(L, -2);

                const char* ast_name = lua_tostring(L, -1);
                int ast_id;
                if (lua_isboolean(L, -2))
                {
                    if (lua_toboolean(L, -2))
                        ast_id = ast_id_true;
                    else
                        ast_id = ast_id_false;
                }
                else
                {
                    ast_id = *(int*)luaL_checkudata(L, -2, "ast");
                }

                p_root_ast_ids->push_back(ast_id);
                p_root_ast_names->push_back(ast_name);

                lua_pop(L, 2);
            }
            lua_pop(L, 1);

            return 0;
        };

        lua_pushcfunction(L, read_results);
        lua_pushlightuserdata(L, &root_ast_ids);
        lua_pushlightuserdata(L, &root_ast_names);
        if (lua_pcall(L, 2, 0, 0))
        {
            printf("%s\n", lua_tostring(L, -1));
            return 1;
        }
    }

    lua_getglobal(L, "title");
    const char* title = lua_isstring(L, -1) ? lua_tostring(L, -1) : infile;
    lua_pop(L, 1);

    robdd bdd;
    std::vector<const node*> roots(root_ast_ids.size());

    decode(
        (int)g_bdd_instructions.size(), g_bdd_instructions.data(),
        g_next_ast_id - ast_id_user, // num user ast nodes
        (int)root_ast_ids.size(), root_ast_ids.data(),
        &bdd,
        roots.data());

    write_dot(
        title,
        &bdd,
        (int)roots.size(), roots.data(), root_ast_names.data(),
        outfile);
}
