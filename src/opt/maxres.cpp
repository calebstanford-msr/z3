/*++
Copyright (c) 2014 Microsoft Corporation

Module Name:

    maxsres.cpp

Abstract:
   
    MaxRes (weighted) max-sat algorithms:

    - mus:     max-sat algorithm by Nina and Bacchus, AAAI 2014.
    - mus-mss: based on dual refinement of bounds.
    - mss:     based on maximal satisfying sets (only).

    MaxRes is a core-guided approach to maxsat.
    MusMssMaxRes extends the core-guided approach by
    leveraging both cores and satisfying assignments
    to make progress towards a maximal satisfying assignment.

    Given a (minimal) unsatisfiable core for the soft
    constraints the approach works like max-res.
    Given a (maximal) satisfying subset of the soft constraints
    the approach updates the upper bound if the current assignment
    improves the current best assignmet.
    Furthermore, take the soft constraints that are complements
    to the current satisfying subset. 
    E.g, if F are the hard constraints and 
    s1,...,sn, t1,..., tm are the soft clauses and 
    F & s1 & ... & sn is satisfiable, then the complement 
    of of the current satisfying subset is t1, .., tm.
    Update the hard constraint:
         F := F & (t1 or ... or tm)
    Replace t1, .., tm by m-1 new soft clauses:
         t1 & t2, t3 & (t1 or t2), t4 & (t1 or t2 or t3), ..., tn & (t1 or ... t_{n-1})
    Claim: 
       If k of these soft clauses are satisfied, then k+1 of 
       the previous soft clauses are satisfied.
       If k of these soft clauses are false in the satisfying assignment 
       for the updated F, then k of the original soft clauses are also false 
       under the assignment.
       In summary: any assignment to the new clauses that satsfies F has the
       same cost.
    Claim:
       If there are no satisfying assignments to F, then the current best assignment
       is the optimum.

Author:

    Nikolaj Bjorner (nbjorner) 2014-20-7

Notes:

--*/

#include "solver.h"
#include "maxsmt.h"
#include "maxres.h"
#include "ast_pp.h"
#include "mus.h"
#include "mss.h"
#include "inc_sat_solver.h"
#include "opt_context.h"
#include "pb_decl_plugin.h"
#include "opt_params.hpp"


using namespace opt;

class maxres : public maxsmt_solver_base {
public:
    enum strategy_t {
        s_mus,
        s_mus_mss,
        s_mus_mss2,
        s_mss
    };
private:
    expr_ref_vector  m_B;
    expr_ref_vector  m_asms;    
    obj_map<expr, rational> m_asm2weight;
    obj_map<expr, bool>     m_asm2value;
    ptr_vector<expr> m_new_core;
    mus              m_mus;
    mss              m_mss;
    expr_ref_vector  m_trail;
    strategy_t       m_st;
    rational         m_max_upper;
    bool             m_hill_climb;             // prefer large weight soft clauses for cores
    bool             m_add_upper_bound_block;  // restrict upper bound with constraint
    unsigned         m_max_num_cores;          // max number of cores per round.
    unsigned         m_max_core_size;          // max core size per round.
    bool             m_maximize_assignment;    // maximize assignment to find MCS
    unsigned         m_max_correction_set_size;// maximal set of correction set that is tolerated.
    bool             m_wmax;                   // Block upper bound using wmax
                                               // this option is disabled if SAT core is used.

    typedef ptr_vector<expr> exprs;

public:
    maxres(context& c,
           weights_t& ws, expr_ref_vector const& soft, 
           strategy_t st):
        maxsmt_solver_base(c, ws, soft),
        m_B(m), m_asms(m),
        m_mus(m_s, m),
        m_mss(m_s, m),
        m_trail(m),
        m_st(st),
        m_hill_climb(true),
        m_add_upper_bound_block(false),
        m_max_num_cores(UINT_MAX),
        m_max_core_size(3),
        m_maximize_assignment(false),
        m_max_correction_set_size(3)
    {
    }

    virtual ~maxres() {}

    bool is_literal(expr* l) {
        return 
            is_uninterp_const(l) ||
            (m.is_not(l, l) && is_uninterp_const(l));
    }

    void add_soft(expr* e, rational const& w) {
        TRACE("opt", tout << mk_pp(e, m) << "\n";);
        expr_ref asum(m), fml(m);
        app_ref cls(m);
        rational weight(0);
        if (m_asm2weight.find(e, weight)) {
            weight += w;
            m_asm2weight.insert(e, weight);
            return;
        }
        if (is_literal(e)) {
            asum = e;
        }
        else {
            asum = mk_fresh_bool("soft");
            fml = m.mk_iff(asum, e);
            s().assert_expr(fml);
        }
        new_assumption(asum, w);
        m_upper += w;
    }

    void new_assumption(expr* e, rational const& w) {
        TRACE("opt", tout << "insert: " << mk_pp(e, m) << " : " << w << "\n";);
        m_asm2weight.insert(e, w);
        m_asms.push_back(e);
        m_trail.push_back(e);        
    }

    lbool mus_solver() {
        init();
        init_local();
        while (true) {
            TRACE("opt", 
                  display_vec(tout, m_asms.size(), m_asms.c_ptr());
                  s().display(tout);
                  tout << "\n";
                  display(tout);
                  );
            lbool is_sat = s().check_sat(m_asms.size(), m_asms.c_ptr());
            if (m_cancel) {
                return l_undef;
            }
            switch (is_sat) {
            case l_true: 
                found_optimum();
                return l_true;
            case l_false:
                is_sat = process_unsat();
                if (is_sat != l_true) return is_sat;
                break;
            case l_undef:
                return l_undef;
            default:
                break;
            }
        }
        return l_true;
    }

    lbool mus_mss_solver() {
        init();
        init_local();
        sls();
        exprs mcs;
        vector<exprs> cores;
        while (m_lower < m_upper) {            
            TRACE("opt", 
                  display_vec(tout, m_asms.size(), m_asms.c_ptr());
                  s().display(tout);
                  tout << "\n";
                  display(tout);
                  );
            lbool is_sat = try_improve_bound(cores, mcs);
            if (m_cancel) {
                return l_undef;
            }
            switch (is_sat) {
            case l_undef:
                return l_undef;
            case l_false:
                SASSERT(cores.empty() && mcs.empty());
                m_lower = m_upper;
                return l_true;
            case l_true:
                SASSERT(cores.empty() || mcs.empty());
                for (unsigned i = 0; i < cores.size(); ++i) {
                    process_unsat(cores[i]);
                }
                if (cores.empty()) {
                    process_sat(mcs);
                }
                break;
            }
        }
        m_lower = m_upper;
        return l_true;
    }

    lbool mss_solver() {
        init();
        init_local();
        sls();
        set_mus(false);
        exprs mcs;
        lbool is_sat = l_true;
        while (m_lower < m_upper && is_sat == l_true) {            
            IF_VERBOSE(1, verbose_stream() << "(opt.maxres [" << m_lower << ":" << m_upper << "])\n";);
            vector<exprs> cores;
            exprs mss;       
            model_ref mdl;
            expr_ref tmp(m);
            mcs.reset();
            s().get_model(mdl);
            update_assignment(mdl.get());
            is_sat = get_mss(mdl.get(), cores, mss, mcs);
            
            switch (is_sat) {
            case l_undef:
                return l_undef;
            case l_false:
                m_lower = m_upper;
                return l_true;
            case l_true: {                
                process_sat(mcs);
                get_mss_model();
                break;
            }
            }
            if (m_cancel) {
                return l_undef;
            }
            if (m_lower < m_upper) {
                is_sat = s().check_sat(0, 0);
            }
        }
        m_lower = m_upper;
        return l_true;
    }

    /**
       Plan:
       - Get maximal set of disjoint cores.
       - Update the lower bound using the cores.
       - As a side-effect find a satisfying assignment that has maximal weight.
         (during core minimization several queries are bound to be SAT,
         those can be used to boot-strap the MCS search).
       - Use the best satisfying assignment from the MUS search to find an MCS of least weight.
       - Update the upper bound using the MCS.
       - Update the soft constraints using first the cores.
       - Then update the resulting soft constraints using the evaluation of the MCS/MSS 
       - Add a cardinality constraint to force new satisfying assignments to improve 
         the new upper bound.
       - In every iteration, the lower bound is improved using the cores.
       - In every iteration, the upper bound is improved using the MCS.
       - Optionally: add a cardinality constraint to prune the upper bound.
       
       What are the corner cases:
       - suppose that cost of cores adds up to current upper bound.
         -> it means that each core is a unit (?)

       TBD:
       - Block upper bound using wmax or pb constraint, or in case of
         unweighted constraints using incremental tricks.
       - Throttle when correction set gets added based on its size.
         Suppose correction set is huge. Do we really need it?
       
    */
    lbool mus_mss2_solver() {
        init();
        init_local();
        sls();
        vector<exprs> cores;
        m_mus.set_soft(m_soft.size(), m_soft.c_ptr(), m_weights.c_ptr());
        lbool is_sat = l_true;
        while (m_lower < m_upper && is_sat == l_true) {            
            TRACE("opt", 
                  display_vec(tout, m_asms.size(), m_asms.c_ptr());
                  s().display(tout);
                  tout << "\n";
                  display(tout);
                  );
            lbool is_sat = s().check_sat(m_asms.size(), m_asms.c_ptr());
            if (m_cancel) {
                return l_undef;
            }
            switch (is_sat) {
            case l_true: 
                found_optimum();
                return l_true;
            case l_false:
                is_sat = get_cores(cores);
                break;
            default:
                break;
            }
            if (is_sat == l_undef) {
                return l_undef;
            }
            SASSERT((is_sat == l_false) == cores.empty());
            SASSERT((is_sat == l_true) == !cores.empty());
            if (cores.empty()) {
                break;
            }           

            //
            // There is a best model, retrieve 
            // it from the previous core calls.
            //
            model_ref mdl;
            get_mus_model(mdl);

            // 
            // Extend the current model to a (maximal)
            // assignment extracting the ss and cs.
            // ss - satisfying subset
            // cs - correction set (complement of ss).
            //
            if (m_maximize_assignment && mdl.get()) {
                exprs ss, cs;
                is_sat = get_mss(mdl.get(), cores, ss, cs);
                if (is_sat != l_true) return is_sat;
                get_mss_model();
            }
            //
            // block the hard constraints corresponding to the cores.
            // block the soft constraints corresponding to the cs
            // obtained from the current best model.
            // 

            //
            // TBD: throttle blocking on correction sets if they are too big.
            // likewise, if the cores are too big, don't block the cores.
            //

            process_unsat(cores);

            exprs cs;
            get_current_correction_set(cs);
            unsigned max_core = max_core_size(cores);
            if (cs.size() <= std::max(max_core, m_max_correction_set_size)) {
                process_sat(cs);                        
            }
        }
        
        m_lower = m_upper;
        return l_true;
    }

    void found_optimum() {
        s().get_model(m_model);
        m_asm2value.reset();
        DEBUG_CODE(
            for (unsigned i = 0; i < m_asms.size(); ++i) {
                SASSERT(is_true(m_asms[i].get()));
            });
        for (unsigned i = 0; i < m_soft.size(); ++i) {
            m_assignment[i] = is_true(m_soft[i].get());
        }
        m_upper = m_lower;
    }


    lbool operator()() {
        switch(m_st) {
        case s_mus:
            return mus_solver();
        case s_mus_mss:
            return mus_mss_solver();
        case s_mus_mss2:
            return mus_mss2_solver();
        case s_mss:
            return mss_solver();
        }
        return l_undef;
    }

    lbool get_cores(vector<exprs>& cores) {
        // assume m_s is unsat.
        lbool is_sat = l_false;
        expr_ref_vector asms(m_asms);
        cores.reset();
        exprs core;
        while (is_sat == l_false) {
            core.reset();
            s().get_unsat_core(core);
            is_sat = minimize_core(core);
            if (is_sat != l_true) {
                break;
            }
            if (core.empty()) {
                cores.reset();
                return l_false;
            }
            cores.push_back(core);
            if (core.size() >= m_max_core_size) {
                break;
            }
            if (cores.size() >= m_max_num_cores) {
                break;
            }
            remove_soft(core, asms);
            TRACE("opt",
                  display_vec(tout << "core: ", core.size(), core.c_ptr());
                  display_vec(tout << "assumptions: ", asms.size(), asms.c_ptr()););

            if (m_hill_climb) {
                /**
                   Give preference to cores that have large minmal values.
                */
                sort_assumptions(asms);            
                unsigned index = 0;
                while (index < asms.size() && is_sat != l_false) {
                    index = next_index(asms, index);
                    is_sat = s().check_sat(index, asms.c_ptr());
                }            
            }
            else {
                is_sat = s().check_sat(asms.size(), asms.c_ptr());            
            }
        }
        TRACE("opt", 
              tout << "num cores: " << cores.size() << "\n";
              for (unsigned i = 0; i < cores.size(); ++i) {
                  for (unsigned j = 0; j < cores[i].size(); ++j) {
                      tout << mk_pp(cores[i][j], m) << " ";
                  }
                  tout << "\n";
              }
              tout << "num satisfying: " << asms.size() << "\n";);
        
        return is_sat;
    }

    void get_current_correction_set(exprs& cs) {
        cs.reset();
        for (unsigned i = 0; i < m_asms.size(); ++i) {
            if (!is_true(m_asms[i].get())) {
                cs.push_back(m_asms[i].get());
            }
        }
        TRACE("opt", display_vec(tout << "new correction set: ", cs.size(), cs.c_ptr()););
    }

    struct compare_asm {
        maxres& mr;
        compare_asm(maxres& mr):mr(mr) {}
        bool operator()(expr* a, expr* b) const {
            return mr.get_weight(a) > mr.get_weight(b);
        }
    };

    void sort_assumptions(expr_ref_vector& _asms) {
        compare_asm comp(*this);
        exprs asms(_asms.size(), _asms.c_ptr());
        expr_ref_vector trail(_asms);
        std::sort(asms.begin(), asms.end(), comp);
        _asms.reset();
        _asms.append(asms.size(), asms.c_ptr());
        DEBUG_CODE(
            for (unsigned i = 0; i + 1 < asms.size(); ++i) {
                SASSERT(get_weight(asms[i]) >= get_weight(asms[i+1]));
            });
    }

    unsigned next_index(expr_ref_vector const& asms, unsigned index) {
        if (index < asms.size()) {
            rational w = get_weight(asms[index]);
            ++index;
            for (; index < asms.size() && w == get_weight(asms[index]); ++index);
        }
        return index;
    }

    void process_sat(exprs const& corr_set) {
        expr_ref fml(m), tmp(m);
        TRACE("opt", display_vec(tout << "corr_set: ", corr_set.size(), corr_set.c_ptr()););
        remove_core(corr_set);
        rational w = split_core(corr_set);
        cs_max_resolve(corr_set, w);        
    }

    lbool process_unsat() {
        vector<exprs> cores;
        lbool is_sat = get_cores(cores);
        if (is_sat != l_true) {
            return is_sat;
        }
        if (cores.empty()) {
            return l_false;
        }
        else {
            process_unsat(cores);
            return l_true;
        }
    }

    unsigned max_core_size(vector<exprs> const& cores) {
        unsigned result = 0;
        for (unsigned i = 0; i < cores.size(); ++i) {
            result = std::max(cores[i].size(), result);
        }
        return result;
    }

    void process_unsat(vector<exprs> const& cores) {
        for (unsigned i = 0; i < cores.size(); ++i) {
            process_unsat(cores[i]);
        }
    }
    
    void process_unsat(exprs const& core) {
        expr_ref fml(m);
        remove_core(core);
        SASSERT(!core.empty());
        rational w = split_core(core);
        TRACE("opt", display_vec(tout << "minimized core: ", core.size(), core.c_ptr()););
        max_resolve(core, w);
        fml = m.mk_not(m.mk_and(m_B.size(), m_B.c_ptr()));
        s().assert_expr(fml);
        m_lower += w;
        IF_VERBOSE(1, verbose_stream() << "(opt.maxres [" << m_lower << ":" << m_upper << "])\n";);
    }

    void get_mus_model(model_ref& mdl) {
        rational w(0);
        if (m_c.sat_enabled()) {
            // SAT solver core extracts some model 
            // during unsat core computation.
            s().get_model(mdl);            
        }
        else {
            w = m_mus.get_best_model(mdl);
        }
        if (mdl.get() && w < m_upper) {
            update_assignment(mdl.get());
        }
    }

    void get_mss_model() {
        model_ref mdl;
        m_mss.get_model(mdl); // last model is best way to reduce search space.
        update_assignment(mdl.get());
    }

    lbool get_mss(model* mdl, vector<exprs> const& cores, exprs& literals, exprs& mcs) {
        literals.reset();
        mcs.reset();
        literals.append(m_asms.size(), m_asms.c_ptr());
        set_mus(false);
        lbool is_sat = m_mss(mdl, cores, literals, mcs);
        set_mus(true);
        return is_sat;
    }

    lbool minimize_core(exprs& core) {
        if (m_c.sat_enabled() || core.empty()) {
            return l_true;
        }
        m_mus.reset();
        for (unsigned i = 0; i < core.size(); ++i) {
            m_mus.add_soft(core[i]);
        }
        unsigned_vector mus_idx;
        lbool is_sat = m_mus.get_mus(mus_idx);
        if (is_sat != l_true) {
            return is_sat;
        }
        m_new_core.reset();
        for (unsigned i = 0; i < mus_idx.size(); ++i) {
            m_new_core.push_back(core[mus_idx[i]]);
        }
        core.reset();
        core.append(m_new_core);
        return l_true;
    }

    rational get_weight(expr* e) const {
        return m_asm2weight.find(e);
    }

    void sls() {
        vector<rational> ws;
        for (unsigned i = 0; i < m_asms.size(); ++i) {
            ws.push_back(get_weight(m_asms[i].get()));
        }
        enable_sls(m_asms, ws);
    }

    rational split_core(exprs const& core) {
        if (core.empty()) return rational(0);
        // find the minimal weight:
        rational w = get_weight(core[0]);
        for (unsigned i = 1; i < core.size(); ++i) {
            rational w2 = get_weight(core[i]);
            if (w2 < w) {
                w = w2;
            }
        }
        // add fresh soft clauses for weights that are above w.
        for (unsigned i = 0; i < core.size(); ++i) {
            rational w2 = get_weight(core[i]);
            if (w2 > w) {
                rational w3 = w2 - w;
                new_assumption(core[i], w3);
            }
        }
        return w;
    }

    void display_vec(std::ostream& out, unsigned sz, expr* const* args) {
        for (unsigned i = 0; i < sz; ++i) {
            out << mk_pp(args[i], m) << " : " << get_weight(args[i]) << " ";
        }
        out << "\n";
    }

    void display(std::ostream& out) {
        for (unsigned i = 0; i < m_asms.size(); ++i) {
            expr* a = m_asms[i].get();
            out << mk_pp(a, m) << " : " << get_weight(a) << "\n";
        }
    }

    void max_resolve(exprs const& core, rational const& w) {
        SASSERT(!core.empty());
        expr_ref fml(m), asum(m);
        app_ref cls(m), d(m), dd(m);
        m_B.reset();
        m_B.append(core.size(), core.c_ptr());
        d = m.mk_true();
        //
        // d_0 := true
        // d_i := b_{i-1} and d_{i-1}    for i = 1...sz-1
        // soft (b_i or !d_i) 
        //   == (b_i or !(!b_{i-1} or d_{i-1}))
        //   == (b_i or b_0 & b_1 & ... & b_{i-1})
        // 
        // Soft constraint is satisfied if previous soft constraint
        // holds or if it is the first soft constraint to fail.
        // 
        // Soundness of this rule can be established using MaxRes
        // 
        for (unsigned i = 1; i < core.size(); ++i) {
            expr* b_i = m_B[i-1].get();
            expr* b_i1 = m_B[i].get();
            if (i > 2) {
                dd = mk_fresh_bool("d");
                fml = m.mk_implies(dd, d);
                s().assert_expr(fml);
                fml = m.mk_implies(dd, b_i);
                s().assert_expr(fml);
                m_asm2value.insert(dd, is_true(d) && is_true(b_i));
                d = dd;
            }
            else {
                dd = m.mk_and(b_i, d);
                m_asm2value.insert(dd, is_true(d) && is_true(b_i));
                m_trail.push_back(dd);
                d = dd;
            }
            asum = mk_fresh_bool("a");
            m_asm2value.insert(asum, is_true(b_i1) || is_true(d));
            cls = m.mk_or(b_i1, d);
            fml = m.mk_implies(asum, cls);
            new_assumption(asum, w);
            s().assert_expr(fml);
        }
    }

    // cs is a correction set (a complement of a (maximal) satisfying assignment).
    void cs_max_resolve(exprs const& cs, rational const& w) {
        if (cs.empty()) return;
        TRACE("opt", display_vec(tout << "correction set: ", cs.size(), cs.c_ptr()););
        expr_ref fml(m), asum(m);
        app_ref cls(m), d(m), dd(m);
        m_B.reset();
        m_B.append(cs.size(), cs.c_ptr());
        d = m.mk_false();
        //
        // d_0 := false
        // d_i := b_{i-1} or d_{i-1}    for i = 1...sz-1
        // soft (b_i and d_i) 
        //   == (b_i and (b_0 or b_1 or ... or b_{i-1}))
        //
        // asm => b_i
        // asm => d_{i-1} or b_{i-1}
        // d_i => d_{i-1} or b_{i-1}
        //
        for (unsigned i = 1; i < cs.size(); ++i) {
            expr* b_i = m_B[i-1].get();
            expr* b_i1 = m_B[i].get();
            cls = m.mk_or(b_i, d);
            if (i > 2) {
                d = mk_fresh_bool("d");
                fml = m.mk_implies(d, cls);
                s().assert_expr(fml);
            }
            else {
                d = cls;
            }
            asum = mk_fresh_bool("a");
            fml = m.mk_implies(asum, b_i1);
            s().assert_expr(fml);
            fml = m.mk_implies(asum, cls);
            s().assert_expr(fml);
            new_assumption(asum, w);
        }
        fml = m.mk_or(m_B.size(), m_B.c_ptr());
        s().assert_expr(fml);
    }

    lbool try_improve_bound(vector<exprs>& cores, exprs& mcs) {
        cores.reset();
        mcs.reset();
        exprs core;
        expr_ref_vector asms(m_asms);
        while (true) {
            rational upper = m_max_upper;
            unsigned sz = 0;
            for (unsigned i = 0; m_upper <= rational(2)*upper && i < asms.size(); ++i, ++sz) {
                upper -= get_weight(asms[i].get());
            }
            lbool is_sat = s().check_sat(sz, asms.c_ptr());
            switch (is_sat) {
            case l_true: {
                model_ref mdl;
                s().get_model(mdl); // last model is best way to reduce search space.
                update_assignment(mdl.get());
                exprs mss;
                mss.append(asms.size(), asms.c_ptr());
                set_mus(false);
                is_sat = m_mss(m_model.get(), cores, mss, mcs);
                set_mus(true);
                if (is_sat != l_true) {
                    return is_sat;
                }
                get_mss_model();
                if (!cores.empty() && mcs.size() > cores.back().size()) {
                    mcs.reset();
                }
                else {
                    cores.reset();
                }
                return l_true;
            }
            case l_undef:
                return l_undef;
            case l_false:
                core.reset();
                s().get_unsat_core(core);
                is_sat = minimize_core(core);
                if (is_sat != l_true) {
                    break;
                }
                if (core.empty()) {
                    cores.reset();
                    mcs.reset();
                    return l_false;
                }
                cores.push_back(core);
                if (core.size() >= 3) {
                    return l_true;
                }
                //
                // check arithmetic: cannot improve upper bound
                //
                if (m_upper <= upper) {
                    return l_true;
                }
                
                remove_soft(core, asms);
                break;
            }
        }
        
        return l_undef;
    }


    void update_assignment(model* mdl) {
        rational upper(0);
        expr_ref tmp(m);
        for (unsigned i = 0; i < m_soft.size(); ++i) {
            expr* n = m_soft[i].get();
            VERIFY(mdl->eval(n, tmp));
            if (!m.is_true(tmp)) {
                upper += m_weights[i];
            }
            CTRACE("opt", !m.is_true(tmp) && !m.is_false(tmp), 
                   tout << mk_pp(n, m) << " |-> " << mk_pp(tmp, m) << "\n";);
        }
        if (upper >= m_upper) {
            return;
        }
        m_model = mdl;
        m_asm2value.reset();

        for (unsigned i = 0; i < m_soft.size(); ++i) {
            m_assignment[i] = is_true(m_soft[i].get());
        }
        m_upper = upper;
        // verify_assignment();
        IF_VERBOSE(1, verbose_stream() << 
                   "(opt.maxres [" << m_lower << ":" << m_upper << "])\n";);

        add_upper_bound_block();
    }

    void add_upper_bound_block() {
        if (!m_add_upper_bound_block) return;
        pb_util u(m);
        expr_ref_vector nsoft(m);
        expr_ref fml(m);
        for (unsigned i = 0; i < m_soft.size(); ++i) {
            nsoft.push_back(m.mk_not(m_soft[i].get()));
        }            
        fml = u.mk_lt(nsoft.size(), m_weights.c_ptr(), nsoft.c_ptr(), m_upper);
        s().assert_expr(fml);        
    }

    bool is_true(expr* e) {
        bool truth_value;
        if (m_asm2value.find(e, truth_value)) {
            return truth_value;
        }
        expr_ref tmp(m);
        VERIFY(m_model->eval(e, tmp));
        return m.is_true(tmp);
    }

    void remove_soft(exprs const& core, expr_ref_vector& asms) {
        for (unsigned i = 0; i < asms.size(); ++i) {
            if (core.contains(asms[i].get())) {
                asms[i] = asms.back();
                asms.pop_back();
                --i;
            }
        }
    }

    void remove_core(exprs const& core) {
        remove_soft(core, m_asms);
    }

    virtual void set_cancel(bool f) {
        maxsmt_solver_base::set_cancel(f);
        m_mus.set_cancel(f);
    }

    virtual void updt_params(params_ref& p) {
        maxsmt_solver_base::updt_params(p);
        opt_params _p(p);

        m_hill_climb = _p.maxres_hill_climb();
        m_add_upper_bound_block = _p.maxres_add_upper_bound_block();
        m_max_num_cores = _p.maxres_max_num_cores();
        m_max_core_size = _p.maxres_max_core_size();
        m_maximize_assignment = _p.maxres_maximize_assignment();
        m_max_correction_set_size = _p.maxres_max_correction_set_size();
        m_wmax = _p.maxres_wmax();
    }

    void init_local() {
        m_upper.reset();
        m_lower.reset();
        m_trail.reset();
        for (unsigned i = 0; i < m_soft.size(); ++i) {
            add_soft(m_soft[i].get(), m_weights[i]);
        }
        m_max_upper = m_upper;
        add_upper_bound_block();
    }

    void verify_assignment() {
        IF_VERBOSE(0, verbose_stream() << "verify assignment\n";);        
        ref<solver> sat_solver = mk_inc_sat_solver(m, m_params);
        for (unsigned i = 0; i < s().get_num_assertions(); ++i) {
            sat_solver->assert_expr(s().get_assertion(i));
        }
        expr_ref n(m);
        for (unsigned i = 0; i < m_soft.size(); ++i) {
            n = m_soft[i].get();
            if (!m_assignment[i]) {
                n = m.mk_not(n);
            }
            sat_solver->assert_expr(n);
        }
        lbool is_sat = sat_solver->check_sat(0, 0);
        if (is_sat == l_false) {
            IF_VERBOSE(0, verbose_stream() << "assignment is infeasible\n";);
        }
    }

};

opt::maxsmt_solver_base* opt::mk_maxres(
    context& c, weights_t& ws, expr_ref_vector const& soft) {
    return alloc(maxres, c, ws, soft, maxres::s_mus);
}

opt::maxsmt_solver_base* opt::mk_mus_mss_maxres(
    context& c, weights_t& ws, expr_ref_vector const& soft) {
    return alloc(maxres, c, ws, soft, maxres::s_mus_mss2);
}

opt::maxsmt_solver_base* opt::mk_mss_maxres(
    context& c, weights_t& ws, expr_ref_vector const& soft) {
    return alloc(maxres, c, ws, soft, maxres::s_mss);
}

