// TODO:
// * Count the number of solutions and output a sample solution
// * Consider implementing garbage collection
// * Extend the algorithm to multi-core using fork/join parallelism
// * Improve the hashtable data-structure (used to have unique nodes)
// * Improve the cache data-structure (used to avoid recomputing the ITE)

#include <tbb/task.h>

#include <lua.hpp>
#include <lauxlib.h>
#include <lualib.h>

#include <map>
#include <unordered_set>
#include <vector>
#include <tuple>
#include <string>
#include <cassert>
#include <cstdlib>

class robdd
{
public:
    using node_handle = uint32_t;
    static const node_handle invalid_handle = -1;

    struct opcode
    {
        enum {
            bdd_and,
            bdd_or,
            bdd_xor
        };
    };

private:
    class unique_table
    {
        struct node
        {
            uint32_t var;
            node_handle lo;
            node_handle hi;
        };

        static const uint32_t capacity = 0x8000000;
        static_assert((capacity & (capacity - 1)) == 0, "capacity must be power of two");

        static const uint32_t bddutmask = capacity - 1;

        union poolnode
        {
            node data;
            node_handle next;
        };
        std::unique_ptr<poolnode[]> pool;
        node_handle pool_head;

        node* pool_alloc()
        {
            if (pool_head == invalid_handle)
            {
                printf("pool_alloc failed\n");
                std::abort();
            }

            node_handle head = pool_head;
            pool_head = pool[head].next;

            return &pool[head].data;
        }

        void pool_free(const node* n)
        {
            poolnode* p = (poolnode*)n;
            p->next = pool_head;
            pool_head = to_handle(n);
        }

        std::unique_ptr<node_handle[]> table;

        node* false_node;
        node* true_node;

        node_handle to_handle(const node* n) const
        {
            return node_handle((poolnode*)n - &pool[0]);
        }

    public:
        unique_table()
        {
            pool.reset(new poolnode[capacity]);
            for (uint32_t i = 0; i < capacity; i++)
            {
                pool[i].next = i + 1;
            }
            pool[capacity - 1].next = invalid_handle;
            pool_head = 0;

            table.reset(new node_handle[capacity]);
            for (uint32_t i = 0; i < capacity; i++)
            {
                table[i] = invalid_handle;
            }

            false_node = pool_alloc();
            false_node->var = INT_MAX;
            false_node->lo = false_node->hi = to_handle(false_node);

            true_node = pool_alloc();
            true_node->var = INT_MAX - 1;
            true_node->lo = true_node->hi = to_handle(true_node);
        }

        node_handle get_false() const
        {
            return to_handle(false_node);
        }

        node_handle get_true() const
        {
            return to_handle(true_node);
        }

        uint32_t get_var(node_handle h) const
        {
            return pool[h].data.var;
        }

        node_handle get_lo(node_handle h) const
        {
            return pool[h].data.lo;
        }

        node_handle get_hi(node_handle h) const
        {
            return pool[h].data.hi;
        }

        node_handle insert(uint32_t var, node_handle lo, node_handle hi)
        {
            uint32_t p = bddutmask & (var + lo + hi);
            
            while (table[p] != invalid_handle)
            {
                const node* curr = &pool[table[p]].data;
                if (curr->var == var && curr->lo == lo && curr->hi == hi)
                {
                    return to_handle(curr);
                }
                p = (p + 1) & bddutmask;
            }

            node* new_node = pool_alloc();
            new_node->var = var;
            new_node->lo = lo;
            new_node->hi = hi;
            node_handle handle = to_handle(new_node);
            table[p] = handle;
            return handle;
        }
    };

    class computed_table
    {
        static const uint32_t capacity = 0x100000;
        static_assert((capacity & (capacity - 1)) == 0, "capacity must be power of two");

        static const uint32_t bddctmask = capacity - 1;

        struct ctnode
        {
            node_handle bdd1;
            node_handle bdd2;
            node_handle op;
            node_handle result;
        };

        std::unique_ptr<ctnode[]> table;

        static constexpr uint32_t hash(node_handle bdd1, node_handle bdd2, uint32_t op)
        {
            return bddctmask & (bdd1 + bdd2 + op);
        }

    public:
        computed_table()
        {
            table.reset(new ctnode[capacity]);
            for (uint32_t i = 0; i < capacity; i++)
            {
                table[i].bdd1 = invalid_handle;
            }
        }

        node_handle find(node_handle bdd1, node_handle bdd2, uint32_t op)
        {
            uint32_t h = hash(bdd1, bdd2, op);
            ctnode found = table[h];
            if (found.bdd1 == bdd1 &&
                found.bdd2 == bdd2 &&
                found.op == op) 
            {
                return found.result;
            }
            else
            {
                return invalid_handle;
            }
        }

        void insert(node_handle bdd1, node_handle bdd2, uint32_t op, node_handle r)
        {
            uint32_t h = hash(bdd1, bdd2, op);
            table[h] = ctnode{ bdd1, bdd2, op, r };
        }
    };

    unique_table uniquetb;

    node_handle false_node;
    node_handle true_node;

    computed_table computedtb;

public:
    robdd()
    {
        false_node = uniquetb.get_false();
        true_node = uniquetb.get_true();
    }

    node_handle get_false() const
    {
        return false_node;
    }

    node_handle get_true() const
    {
        return true_node;
    }

    uint32_t get_var(node_handle h) const
    {
        return uniquetb.get_var(h);
    }

    node_handle get_lo(node_handle h) const
    {
        return uniquetb.get_lo(h);
    }

    node_handle get_hi(node_handle h) const
    {
        return uniquetb.get_hi(h);
    }

    node_handle make_node(uint32_t var, node_handle lo, node_handle hi)
    {
        // enforce no-redundance constraint of ROBDD
        if (lo == hi) return lo;
        // enforce uniqueness constraint of ROBDD
        // hash table returns the node if it exists
        // and inserts the node if it doesn't
        return uniquetb.insert(var, lo, hi);
    }

    node_handle apply(node_handle bdd1, node_handle bdd2, uint32_t op)
    {
        node_handle found = computedtb.find(bdd1, bdd2, op);
        if (found != invalid_handle)
        {
            return found;
        }
        
        // handle terminal vs terminal op
        if ((bdd1 == false_node || bdd1 == true_node) &&
            (bdd2 == false_node || bdd2 == true_node))
        {
            bool bdd1_value = bdd1 == true_node;
            bool bdd2_value = bdd2 == true_node;

            switch (op)
            {
            case opcode::bdd_and:
                return (bdd1_value && bdd2_value) ? true_node : false_node;
            case opcode::bdd_or:
                return (bdd1_value || bdd2_value) ? true_node : false_node;
            case opcode::bdd_xor:
                return (bdd1_value != bdd2_value) ? true_node : false_node;
            }
            return invalid_handle;
        }
        
        node_handle n;
        if (get_var(bdd1) == get_var(bdd2))
        {
            node_handle lo = apply(get_lo(bdd1), get_lo(bdd2), op);
            node_handle hi = apply(get_hi(bdd1), get_hi(bdd2), op);
            n = make_node(get_var(bdd1), lo, hi);
        }
        else if (get_var(bdd1) < get_var(bdd2))
        {
            node_handle lo = apply(get_lo(bdd1), bdd2, op);
            node_handle hi = apply(get_hi(bdd1), bdd2, op);
            n = make_node(get_var(bdd1), lo, hi);
        }
        else {
            node_handle lo = apply(bdd1, get_lo(bdd2), op);
            node_handle hi = apply(bdd1, get_hi(bdd2), op);
            n = make_node(get_var(bdd2), lo, hi);
        }

        computedtb.insert(bdd1, bdd2, op, n);

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
    robdd::node_handle* roots)
{
    robdd::node_handle false_node = r->get_false();
    robdd::node_handle true_node = r->get_true();

    std::vector<robdd::node_handle> astnode2bddnode(ast_id_user + num_user_ast_nodes);
    astnode2bddnode[ast_id_false] = false_node;
    astnode2bddnode[ast_id_true] = true_node;

    for (int root_ast_idx = 0; root_ast_idx < num_root_ast_ids; root_ast_idx++)
    {
        if (root_ast_ids[root_ast_idx] == ast_id_true)
            roots[root_ast_idx] = true_node;
        else if (root_ast_ids[root_ast_idx] == ast_id_false)
            roots[root_ast_idx] = false_node;
    }

    robdd::node_handle* ast2bdd = astnode2bddnode.data();

    for (int i = 0; i < num_instrs; i++)
    {
        const bdd_instr& inst = instrs[i];

        int inst_dst_ast_id = -1;
        robdd::node_handle inst_dst_node = robdd::invalid_handle;

        switch (inst.opcode)
        {
        case bdd_instr::opcode_newinput:
        {
            int ast_id = inst.operand_newinput_id;
            const char* ast_name = inst.operand_newinput_name->c_str();

            printf("new %d (%s)\n", ast_id, ast_name);

            robdd::node_handle new_bdd = r->make_node(ast_id, false_node, true_node);

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

            robdd::node_handle src1_bdd = ast2bdd[src1_ast_id];
            robdd::node_handle src2_bdd = ast2bdd[src2_ast_id];
            robdd::node_handle new_bdd = r->apply(src1_bdd, src2_bdd, robdd::opcode::bdd_and);

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

            robdd::node_handle src1_bdd = ast2bdd[src1_ast_id];
            robdd::node_handle src2_bdd = ast2bdd[src2_ast_id];
            robdd::node_handle new_bdd = r->apply(src1_bdd, src2_bdd, robdd::opcode::bdd_or);

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

            robdd::node_handle src1_bdd = ast2bdd[src1_ast_id];
            robdd::node_handle src2_bdd = ast2bdd[src2_ast_id];
            robdd::node_handle new_bdd = r->apply(src1_bdd, src2_bdd, robdd::opcode::bdd_xor);

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

            robdd::node_handle src_bdd = ast2bdd[src_ast_id];
            robdd::node_handle new_bdd = r->apply(src_bdd, true_node, robdd::opcode::bdd_xor);

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
    int num_roots, const robdd::node_handle* roots, const std::string* root_names,
    const robdd* r,
    const char* fn)
{
    robdd::node_handle false_node = r->get_false();
    robdd::node_handle true_node = r->get_true();

    FILE* f = fopen(fn, "w");
    
    if (!f)
    {
        printf("failed to open %s\n", fn);
    }

    fprintf(f, "digraph {\n");

    fprintf(f, "  labelloc=\"t\";\n");
    fprintf(f, "  label=\"%s\";\n", title);

    std::vector<robdd::node_handle> nodes2add;
    nodes2add.insert(nodes2add.end(), roots, roots + num_roots);

    std::unordered_set<robdd::node_handle> added;
    added.insert(false_node);
    added.insert(true_node);

    std::unordered_set<robdd::node_handle> declared;

    for (int root_idx = 0; root_idx < num_roots; root_idx++)
    {
        if (declared.find(roots[root_idx]) != end(declared))
        {
            continue;
        }

        if (roots[root_idx] == false_node)
        {
            fprintf(f, "  n%x [label=\"0\",shape=box];\n", false_node);
        }
        else if (roots[root_idx] == true_node)
        {
            fprintf(f, "  n%x [label=\"1\",shape=box];\n", true_node);
        }
        else
        {
            fprintf(f, "  n%x [label=\"%s\"];\n", roots[root_idx], g_astid2name.at(r->get_var(roots[root_idx])).c_str());
        }

        declared.insert(roots[root_idx]);
    }

    for (int root_idx = 0; root_idx < num_roots; root_idx++)
    {
        fprintf(f, "  r%d [label=\"%s\",style=filled];\n", root_idx, root_names[root_idx].c_str());
        fprintf(f, "  r%d -> n%x [style=solid];\n", root_idx, roots[root_idx]);
    }

    while (!nodes2add.empty())
    {
        robdd::node_handle n = nodes2add.back();
        nodes2add.pop_back();

        if (added.find(n) != added.end())
            continue;

        const robdd::node_handle children[] = { r->get_lo(n), r->get_hi(n) };
        for (robdd::node_handle child : children)
        {
            if (declared.insert(child).second)
            {
                if (child == false_node)
                {
                    fprintf(f, "  n%x [label=\"0\",shape=box];\n", false_node);
                }
                else if (child == true_node)
                {
                    fprintf(f, "  n%x [label=\"1\",shape=box];\n", true_node);
                }
                else
                {
                    fprintf(f, "  n%x [label=\"%s\"];\n", child, g_astid2name.at(r->get_var(child)).c_str());
                }
            }

            fprintf(f, "  n%x -> n%x [style=%s];\n", n, child, child == r->get_lo(n) ? "dotted" : "solid");

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
    std::vector<robdd::node_handle> roots(root_ast_ids.size());

    decode(
        (int)g_bdd_instructions.size(), g_bdd_instructions.data(),
        g_next_ast_id - ast_id_user, // num user ast nodes
        (int)root_ast_ids.size(), root_ast_ids.data(),
        &bdd,
        roots.data());

    write_dot(
        title,
        (int)roots.size(), roots.data(), root_ast_names.data(),
        &bdd,
        outfile);
}
