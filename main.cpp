#include <tbb/task_scheduler_init.h>
#include <tbb/task_group.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

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
#include <array>

//#define SHOW_INSTRS

//#define SINGLETHREADED

//#define USE_APPLY_TASK

//#define USE_TSX

#define BENCHMARK

//#define ITTPROFILE

#ifdef ITTPROFILE
#include <C:\Program Files (x86)\IntelSWTools\VTune Amplifier 2016 for Systems\include\ittnotify.h>
#pragma comment(lib, "C:\\Program Files (x86)\\IntelSWTools\\VTune Amplifier 2016 for Systems\\lib64\\libittnotify.lib")
#endif

#ifdef ITTPROFILE
__itt_domain* robdd_itt_domain = __itt_domain_create(L"robdd");
__itt_string_handle* robdd_itt_decode_task = __itt_string_handle_create(L"decode");
#endif

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
            bdd_xor,
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
            uint64_t weight;
        };

        static const uint32_t capacity = 0x8000000;
        static_assert((capacity & (capacity - 1)) == 0, "capacity must be power of two");

        static const uint32_t bddutmask = capacity - 1;

        std::unique_ptr<node[]> data_pool;

        uint32_t pool_head;

        node* pool_alloc()
        {
#ifdef SINGLETHREADED
            uint32_t old_head = pool_head++;
#else
            uint32_t old_head = InterlockedExchangeAdd((LONG*)&pool_head, 1);
#endif
            
            if (old_head >= capacity)
            {
                printf("pool_alloc failed\n");
                std::abort();
            }

            return &data_pool[old_head];
        }

        std::unique_ptr<node_handle[]> table;

        node* false_node;
        node* true_node;

        node_handle to_handle(const node* n) const
        {
            return node_handle(n - &data_pool[0]);
        }
        
        const node* to_node(node_handle h) const
        {
            return (const node*)&data_pool[h];
        }

    public:
        void init(uint32_t num_vars)
        {
            data_pool.reset(new node[capacity]);
            pool_head = 0;

            table.reset(new node_handle[capacity]);
            for (uint32_t i = 0; i < capacity; i++)
            {
                table[i] = invalid_handle;
            }

            false_node = pool_alloc();
            false_node->var = num_vars;
            false_node->lo = false_node->hi = to_handle(false_node);
            false_node->weight = 0;

            true_node = pool_alloc();
            true_node->var = num_vars;
            true_node->lo = true_node->hi = to_handle(true_node);
            true_node->weight = 1;
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
            return to_node(h)->var;
        }

        node_handle get_lo(node_handle h) const
        {
            return to_node(h)->lo;
        }

        node_handle get_hi(node_handle h) const
        {
            return to_node(h)->hi;
        }

        uint64_t get_weight(node_handle h) const
        {
            return to_node(h)->weight;
        }

        node_handle insert(uint32_t var, node_handle lo, node_handle hi)
        {
            uint32_t p = bddutmask & (var + lo + hi);

            for (;;)
            {
                {
                    uint32_t tab = table[p];
                    if (tab != invalid_handle)
                    {
                        const node* curr = to_node(tab);
                        if (curr->var == var && curr->lo == lo && curr->hi == hi)
                        {
                            // note: potentially leaks a node
                            return to_handle(curr);
                        }
                        p = (p + 1) & bddutmask;
                        continue;
                    }
                }

                node* new_node = pool_alloc();
                new_node->var = var;
                new_node->lo = lo;
                new_node->hi = hi;
                // combine weights
                {
                    const node* lonode = to_node(lo);
                    const node* hinode = to_node(hi);
                    uint64_t loweight = lonode->weight << (lonode->var - var - 1);
                    uint64_t hiweight = hinode->weight << (hinode->var - var - 1);
                    new_node->weight = loweight + hiweight;
                }

                node_handle handle = to_handle(new_node);
#ifdef SINGLETHREADED
                table[p] = handle;
                return handle;
#else
                node_handle previous_handle = InterlockedCompareExchange(&table[p], handle, invalid_handle);

                if (previous_handle == invalid_handle)
                {
                    return handle;
                }
#endif
            }
        }
    };

    class computed_table
    {
        static const uint32_t capacity = 0x100000;
        static_assert((capacity & (capacity - 1)) == 0, "capacity must be power of two");

        static const uint32_t bddctmask = capacity - 1;

        struct ctnode
        {
            uint32_t op;
            node_handle bdd1;
            node_handle bdd2;
            node_handle result;

            ctnode() = default;

            ctnode(uint32_t op, node_handle bdd1, node_handle bdd2, node_handle result)
            {
                this->op = op;
                this->bdd1 = bdd1;
                this->bdd2 = bdd2;
                this->result = result;
            }
        };

        std::unique_ptr<ctnode[]> table;
        std::unique_ptr<uint32_t[]> locks;

        static constexpr uint32_t hash(node_handle bdd1, node_handle bdd2, uint32_t op)
        {
            return bddctmask & (bdd1 + bdd2 + op);
        }

        void acquire_read(uint32_t i)
        {
#ifndef SINGLETHREADED
            for (;;)
            {
                if (InterlockedIncrement((LONG*)&locks[i]) <= 255)
                {
                    break;
                }
            }
#endif
        }

        void release_read(uint32_t i)
        {
#ifndef SINGLETHREADED
            InterlockedDecrement((LONG*)&locks[i]);
#endif
        }

        void acquire_write(uint32_t i)
        {
#ifndef SINGLETHREADED
            for (;;)
            {
                if (InterlockedCompareExchange(&locks[i], 255, 0) == 0)
                {
                    break;
                }
            }
#endif
        }

        void release_write(uint32_t i)
        {
#ifndef SINGLETHREADED
            InterlockedExchange(&locks[i], 0);
#endif
        }

    public:
        computed_table()
        {
            table.reset(new ctnode[capacity]);
            for (uint32_t i = 0; i < capacity; i++)
            {
                table[i].bdd1 = invalid_handle;
            }

#ifndef SINGLETHREADED
            locks.reset(new uint32_t[capacity]);
            for (uint32_t i = 0; i < capacity; i++)
            {
                locks[i] = 0;
            }
#endif
        }

        node_handle find(node_handle bdd1, node_handle bdd2, uint32_t op)
        {
            uint32_t h = hash(bdd1, bdd2, op);

            ctnode found;

#if !defined(SINGLETHREADED) && defined(USE_TSX)
            if (_xbegin() == _XBEGIN_STARTED)
            {
                found = table[h];
                _xend();
            }
            else
#endif
            {
                acquire_read(h);
                {
                    found = table[h];
                }
                release_read(h);
            }

            node_handle result;

            if (found.bdd1 == bdd1 && found.bdd2 == bdd2 && found.op == op)
            {
                result = found.result;
            }
            else
            {
                result = invalid_handle;
            }

            return result;
        }

        void insert(node_handle bdd1, node_handle bdd2, uint32_t op, node_handle r)
        {
            uint32_t h = hash(bdd1, bdd2, op);
            
            ctnode newnode = ctnode(op, bdd1, bdd2, r);

#if !defined(SINGLETHREADED) && defined(USE_TSX)
            if (_xbegin() == _XBEGIN_STARTED)
            {
                table[h] = newnode;
                _xend();
            }
            else
#endif
            {
                acquire_write(h);
                {
                    table[h] = newnode;
                }
                release_write(h);
            }
        }
    };

    unique_table uniquetb;

    node_handle false_node;
    node_handle true_node;

    computed_table computedtb;

    uint32_t max_level;

public:
    robdd(uint32_t num_vars, uint32_t num_threads = -1)
    {
        uniquetb.init(num_vars);

        false_node = uniquetb.get_false();
        true_node = uniquetb.get_true();

        max_level = ((num_threads == -1 ? tbb::task_scheduler_init::default_num_threads() : num_threads) - 1) * 2;
#ifdef SINGLETHREADED
        max_level = 0;
#endif
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

    uint64_t get_weight(node_handle h) const
    {
        return uniquetb.get_weight(h);
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

#ifdef USE_APPLY_TASK
    class make_node_task : public tbb::task
    {
        robdd* m_bdd;
        node_handle m_bdd1;
        node_handle m_bdd2;
        uint32_t m_op;
        node_handle* m_n;

    public:
        uint32_t var;
        node_handle lo;
        node_handle hi;

        make_node_task(robdd* bdd, node_handle bdd1, node_handle bdd2, uint32_t op, node_handle* n)
            : m_bdd(bdd)
            , m_bdd1(bdd1)
            , m_bdd2(bdd2)
            , m_op(op)
            , m_n(n)
        { }

        tbb::task* execute() override
        {
            *m_n = m_bdd->make_node(var, lo, hi);
            m_bdd->computedtb.insert(m_bdd1, m_bdd2, m_op, *m_n);
            return NULL;
        }
    };

    class apply_task : public tbb::task
    {
        robdd* m_bdd;
        node_handle m_bdd1;
        node_handle m_bdd2;
        uint32_t m_op;
        uint32_t m_level;
        node_handle* m_n;

    public:
        apply_task(robdd* bdd, node_handle bdd1, node_handle bdd2, uint32_t op, uint32_t level, node_handle* n)
            : m_bdd(bdd)
            , m_bdd1(bdd1)
            , m_bdd2(bdd2)
            , m_op(op)
            , m_level(level)
            , m_n(n)
        { }

        tbb::task* execute() override
        {
            if (m_level >= m_bdd->max_level)
            {
                *m_n = m_bdd->apply_seq(m_bdd1, m_bdd2, m_op);
                return NULL;
            }

            node_handle found = m_bdd->computedtb.find(m_bdd1, m_bdd2, m_op);
            if (found != invalid_handle)
            {
                *m_n = found;
                return NULL;
            }

            // handle terminal vs terminal op
            if ((m_bdd1 == m_bdd->false_node || m_bdd1 == m_bdd->true_node) &&
                (m_bdd2 == m_bdd->false_node || m_bdd2 == m_bdd->true_node))
            {
                bool bdd1_value = m_bdd1 == m_bdd->true_node;
                bool bdd2_value = m_bdd2 == m_bdd->true_node;

                node_handle n;

                switch (m_op)
                {
                case opcode::bdd_and:
                    n = (bdd1_value && bdd2_value) ? m_bdd->true_node : m_bdd->false_node;
                    break;
                case opcode::bdd_or:
                    n = (bdd1_value || bdd2_value) ? m_bdd->true_node : m_bdd->false_node;
                    break;
                case opcode::bdd_xor:
                    n = (bdd1_value != bdd2_value) ? m_bdd->true_node : m_bdd->false_node;
                    break;
                }
                *m_n = n;
                return NULL;
            }

            make_node_task& c = *new (allocate_continuation()) make_node_task(m_bdd, m_bdd1, m_bdd2, m_op, m_n);

            apply_task* a;

            if (m_bdd->get_var(m_bdd1) == m_bdd->get_var(m_bdd2))
            {
                a = new (c.allocate_child()) apply_task(m_bdd, m_bdd->get_lo(m_bdd1), m_bdd->get_lo(m_bdd2), m_op, m_level + 1, &c.lo);
                c.var = m_bdd->get_var(m_bdd1);

                m_bdd1 = m_bdd->get_hi(m_bdd1);
                m_bdd2 = m_bdd->get_hi(m_bdd2);
                m_level = m_level + 1;
                m_n = &c.hi;
            }
            else if (m_bdd->get_var(m_bdd1) < m_bdd->get_var(m_bdd2))
            {
                a = new (c.allocate_child()) apply_task(m_bdd, m_bdd->get_lo(m_bdd1), m_bdd2, m_op, m_level + 1, &c.lo);
                c.var = m_bdd->get_var(m_bdd1);

                m_bdd1 = m_bdd->get_hi(m_bdd1);
                m_level = m_level + 1;
                m_n = &c.hi;
            }
            else
            {
                a = new (c.allocate_child()) apply_task(m_bdd, m_bdd1, m_bdd->get_lo(m_bdd2), m_op, m_level + 1, &c.lo);
                c.var = m_bdd->get_var(m_bdd2);

                m_bdd2 = m_bdd->get_hi(m_bdd2);
                m_level = m_level + 1;
                m_n = &c.hi;
            }

            recycle_as_child_of(c);
            c.set_ref_count(2);
            spawn(*a);
            return this;
        }
    };

    node_handle apply(node_handle bdd1, node_handle bdd2, uint32_t op, uint32_t level)
    {
        node_handle n;
        tbb::task::spawn_root_and_wait(*new(tbb::task::allocate_root()) apply_task(this, bdd1, bdd2, op, level, &n));
        return n;
    }

    node_handle apply_seq(node_handle bdd1, node_handle bdd2, uint32_t op)
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

        node_handle lo, hi;
        if (get_var(bdd1) == get_var(bdd2))
        {
            lo = apply_seq(get_lo(bdd1), get_lo(bdd2), op);
            hi = apply_seq(get_hi(bdd1), get_hi(bdd2), op);
            n = make_node(get_var(bdd1), lo, hi);
        }
        else if (get_var(bdd1) < get_var(bdd2))
        {
            lo = apply_seq(get_lo(bdd1), bdd2, op);
            hi = apply_seq(get_hi(bdd1), bdd2, op);
            n = make_node(get_var(bdd1), lo, hi);
        }
        else
        {
            lo = apply_seq(bdd1, get_lo(bdd2), op);
            hi = apply_seq(bdd1, get_hi(bdd2), op);
            n = make_node(get_var(bdd2), lo, hi);
        }

        computedtb.insert(bdd1, bdd2, op, n);

        return n;
    }
#else
    node_handle apply(node_handle bdd1, node_handle bdd2, uint32_t op, uint32_t level)
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

#ifndef SINGLETHREADED
        if (level < max_level)
        {
            tbb::task_group g;
            node_handle lo, hi;

            if (get_var(bdd1) == get_var(bdd2))
            {
                g.run([&] { lo = apply(get_lo(bdd1), get_lo(bdd2), op, level + 1); });
                g.run_and_wait([&] { hi = apply(get_hi(bdd1), get_hi(bdd2), op, level + 1); });
                n = make_node(get_var(bdd1), lo, hi);
            }
            else if (get_var(bdd1) < get_var(bdd2))
            {
                g.run([&] { lo = apply(get_lo(bdd1), bdd2, op, level + 1); });
                g.run_and_wait([&] { hi = apply(get_hi(bdd1), bdd2, op, level + 1); });
                n = make_node(get_var(bdd1), lo, hi);
            }
            else
            {
                g.run([&] { lo = apply(bdd1, get_lo(bdd2), op, level + 1); });
                g.run_and_wait([&] { hi = apply(bdd1, get_hi(bdd2), op, level + 1); });
                n = make_node(get_var(bdd2), lo, hi);
            }
        }
        else
#endif
        {
            node_handle lo, hi;
            if (get_var(bdd1) == get_var(bdd2))
            {
                lo = apply(get_lo(bdd1), get_lo(bdd2), op, level);
                hi = apply(get_hi(bdd1), get_hi(bdd2), op, level);
                n = make_node(get_var(bdd1), lo, hi);
            }
            else if (get_var(bdd1) < get_var(bdd2))
            {
                lo = apply(get_lo(bdd1), bdd2, op, level);
                hi = apply(get_hi(bdd1), bdd2, op, level);
                n = make_node(get_var(bdd1), lo, hi);
            }
            else
            {
                lo = apply(bdd1, get_lo(bdd2), op, level);
                hi = apply(bdd1, get_hi(bdd2), op, level);
                n = make_node(get_var(bdd2), lo, hi);
            }
        }

        computedtb.insert(bdd1, bdd2, op, n);

        return n;
    }
#endif
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
            int operand_newinput_ast_id;
            int operand_newinput_var_id;
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

        struct {
            int operand_dontcare_dst_id;
            int operand_dontcare_src_id;
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
#ifdef ITTPROFILE
    __itt_task_begin(robdd_itt_domain, __itt_null, __itt_null, robdd_itt_decode_task);
#endif

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

        // initial level of depth
        uint32_t level = 0;

        int inst_dst_ast_id = -1;
        robdd::node_handle inst_dst_node = robdd::invalid_handle;

        switch (inst.opcode)
        {
        case bdd_instr::opcode_newinput:
        {
            int ast_id = inst.operand_newinput_ast_id;
            int var_id = inst.operand_newinput_var_id;
            const char* ast_name = inst.operand_newinput_name->c_str();

#ifdef SHOW_INSTRS
            printf("%d = new %d (%s)\n", ast_id, var_id, ast_name);
#endif

            robdd::node_handle new_bdd = r->make_node(var_id, false_node, true_node);

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

#ifdef SHOW_INSTRS
            printf("%d = %d AND %d\n", dst_ast_id, src1_ast_id, src2_ast_id);
#endif

            robdd::node_handle src1_bdd = ast2bdd[src1_ast_id];
            robdd::node_handle src2_bdd = ast2bdd[src2_ast_id];
            robdd::node_handle new_bdd = r->apply(src1_bdd, src2_bdd, robdd::opcode::bdd_and, level);

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

#ifdef SHOW_INSTRS
            printf("%d = %d OR %d\n", dst_ast_id, src1_ast_id, src2_ast_id);
#endif

            robdd::node_handle src1_bdd = ast2bdd[src1_ast_id];
            robdd::node_handle src2_bdd = ast2bdd[src2_ast_id];
            robdd::node_handle new_bdd = r->apply(src1_bdd, src2_bdd, robdd::opcode::bdd_or, level);

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

#ifdef SHOW_INSTRS
            printf("%d = %d XOR %d\n", dst_ast_id, src1_ast_id, src2_ast_id);
#endif

            robdd::node_handle src1_bdd = ast2bdd[src1_ast_id];
            robdd::node_handle src2_bdd = ast2bdd[src2_ast_id];
            robdd::node_handle new_bdd = r->apply(src1_bdd, src2_bdd, robdd::opcode::bdd_xor, level);

            ast2bdd[dst_ast_id] = new_bdd;

            inst_dst_ast_id = dst_ast_id;
            inst_dst_node = new_bdd;

            break;
        }
        case bdd_instr::opcode_not:
        {
            int dst_ast_id = inst.operand_not_dst_id;
            int src_ast_id = inst.operand_not_src_id;

#ifdef SHOW_INSTRS
            printf("%d = NOT %d\n", dst_ast_id, src_ast_id);
#endif

            robdd::node_handle src_bdd = ast2bdd[src_ast_id];
            robdd::node_handle new_bdd = r->apply(src_bdd, true_node, robdd::opcode::bdd_xor, level);

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

#ifdef ITTPROFILE
    __itt_task_end(robdd_itt_domain);
#endif
}

std::map<int, std::string> g_varid2name;

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
            fprintf(f, "  n%x [label=\"%s\"];\n", roots[root_idx], g_varid2name.at(r->get_var(roots[root_idx])).c_str());
        }

        declared.insert(roots[root_idx]);
    }

    for (int root_idx = 0; root_idx < num_roots; root_idx++)
    {
        fprintf(f, "  r%d [label=\"%s\\n%llu solutions\",style=filled];\n", root_idx, root_names[root_idx].c_str(), r->get_weight(roots[root_idx]));
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
                    fprintf(f, "  n%x [label=\"%s\"];\n", child, g_varid2name.at(r->get_var(child)).c_str());
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
int g_num_variables = 0;

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

    auto varid2name = g_varid2name.emplace(g_num_variables, luaL_checkstring(L, 2)).first;
    
    g_num_variables += 1;

    bdd_instr new_instr;
    new_instr.opcode = bdd_instr::opcode_newinput;
    new_instr.operand_newinput_ast_id = *ast_id;
    new_instr.operand_newinput_var_id = varid2name->first;
    new_instr.operand_newinput_name = &varid2name->second;
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

    lua_getglobal(L, "display");
    bool display = lua_isboolean(L, -1) ? lua_toboolean(L, -1) != 0: false;
    lua_pop(L, 1);

    int max_threads = tbb::task_scheduler_init::default_num_threads();
    
    int initial_num_threads = max_threads;
#ifdef BENCHMARK
    initial_num_threads = 0;
#endif

    for (int num_threads = initial_num_threads; num_threads <= max_threads; num_threads++)
    {
        robdd bdd(g_num_variables, num_threads == 0 ? -1 : num_threads);
        std::vector<robdd::node_handle> roots(root_ast_ids.size());

#ifndef BENCHMARK
        if (num_threads != 0)
        {
            printf("decoding with %d threads...\n", num_threads);
        }
#endif

        // num_threads == 0 does a warmup run to attempt to nullify cache effects that penalize the first run
        tbb::task_scheduler_init scheduler_init(num_threads == 0 ? -1 : num_threads);

        LARGE_INTEGER now, then, freq;
        QueryPerformanceFrequency(&freq);

        QueryPerformanceCounter(&then);

        decode(
            (int)g_bdd_instructions.size(), g_bdd_instructions.data(),
            g_next_ast_id - ast_id_user, // num user ast nodes
            (int)root_ast_ids.size(), root_ast_ids.data(),
            &bdd,
            roots.data());

        QueryPerformanceCounter(&now);

        if (num_threads == 0)
        {
            continue;
        }

        UINT64 seconds = (now.QuadPart - then.QuadPart) / freq.QuadPart;
        UINT64 milliseconds = (now.QuadPart - then.QuadPart) * 1000 / freq.QuadPart;
        UINT64 microseconds = (now.QuadPart - then.QuadPart) * 1000000 / freq.QuadPart;

#ifdef BENCHMARK
        printf("%d, %.3lf\n", num_threads, double(now.QuadPart - then.QuadPart) / freq.QuadPart);
#else
        if (seconds > 0)
        {
            printf("Finished in %.3lf seconds\n", double(now.QuadPart - then.QuadPart) / freq.QuadPart);
        }
        else if (milliseconds > 0)
        {
            printf("Finished in %.3lf milliseconds\n", double(now.QuadPart - then.QuadPart) * 1000.0 / freq.QuadPart);
        }
        else
        {
            printf("Finished in %.3lf microseconds\n", double(now.QuadPart - then.QuadPart) * 1000000.0 / freq.QuadPart);
        }

        for (int root_idx = 0; root_idx < (int)root_ast_ids.size(); root_idx++)
        {
            printf("Found %llu solutions to \"%s\"\n", bdd.get_weight(roots[root_idx]), root_ast_names[root_idx].c_str());
        }

        if (num_threads == max_threads)
        {
            if (display)
            {
                printf("writing dot file...\n");

                write_dot(
                    title,
                    (int)roots.size(), roots.data(), root_ast_names.data(),
                    &bdd,
                    outfile);
            }
        }
#endif
    }
}
