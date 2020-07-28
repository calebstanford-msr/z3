/*++
Copyright (c) 2011 Microsoft Corporation

Module Name:

    state_graph.h

Abstract:

    Data structure for incrementally tracking "live" and "dead" states in an
    abstract transition system.

Author:

    Caleb Stanford (calebstanford-msr / cdstanford) 2020-7

--*/

#pragma once

#include "util/map.h"
#include "util/uint_set.h"
#include "util/union_find.h"
#include "util/vector.h"

/*
    state_graph

    Data structure which is capable of incrementally tracking
    live states and dead states.

    "States" are integers. States and edges are added to the data
    structure incrementally.
    - States can be marked as "live" or "done".
      "Done" signals that (1) no more outgoing edges will be
      added and (2) the state will not be marked as live. The data
      structure then tracks
      which other states are live (can reach a live state), dead
      (can't reach a live state), or neither.
    - Some edges are labeled as not contained in a cycle. This is to
      optimize search if it is known by the user of the structure
      that no cycle will ever contain this edge.

    Internally, we use union_find to identify states within an SCC,
    and incrementally update SCCs, while propagating backwards
    live and dead SCCs.
*/
class state_graph {
public:
    typedef unsigned           state;
    typedef uint_set           state_set;
    typedef u_map<state_set>   edge_rel;
    typedef basic_union_find   state_ufind;

private:
    /*
        All states are internally exactly one of:
        - live:       known to reach a live state
        - dead:       known to never reach a live state
        - unknown:    all outgoing edges have been added, but the
                      state is not known to be live or dead
        - unexplored: not all outgoing edges have been added

        As SCCs are merged, some states become aliases, and a
        union find data structure collapses a now obsolete
        state to its current representative. m_seen keeps track
        of states we have seen, including obsolete states.

        Invariants:
        - TODO
    */
    state_set   m_live;
    state_set   m_dead;
    state_set   m_unknown;
    state_set   m_unexplored;

    state_set     m_seen;
    state_ufind   m_state_ufind;

    /*
        Edges are saved in both from and to maps.
        A subset of edges are also marked as possibly being
        part of a cycle by being stored in m_sources_maybecycle.

        Invariants:
        - TODO
    */
    edge_rel   m_sources;
    edge_rel   m_targets;
    edge_rel   m_sources_maybecycle;

    /*
        'Core' functions that modify the plain graph, without
        updating SCCs or propagating live/dead state information.
        These are for internal use only.
    */
    void add_state_core(state s);    // unexplored + seen
    void remove_state_core(state s); // unknown + seen -> seen
    void mark_unknown_core(state s); // unexplored -> unknown
    void mark_live_core(state s);    // unknown -> live
    void mark_dead_core(state s);    // unknown -> dead

    void add_edge_core(state s1, state s2, bool maybecycle);
    void remove_edge_core(state s1, state s2);
    void rename_edge_core(state old1, state old2, state new1, state new2);

    state merge_states(state s1, state s2);
    state merge_states(state_set& s_set);

    /*
        Algorithmic search routines
        - live state propagation
        - dead state propagation
        - cycle / strongly-connected component detection
    */
    void mark_live_recursive(state s);
    void mark_dead_recursive(state s);
    state merge_all_cycles(state s);

public:
    state_graph():
        m_live(), m_dead(), m_unknown(), m_unexplored(), m_seen(),
        m_state_ufind(), m_sources(), m_targets(), m_sources_maybecycle() {}

    /*
        Exposed methods

        These methods may be called in any order, as long as:
        - states are added before edges are added between them
        - outgoing edges are not added from a done state
        - a done state is not marked as live
        - edges are not added creating a cycle containing an edge with
          maybecycle = false (this is not necessary for soundness, but
          prevents completeness for successfully detecting dead states)
    */
    void add_state(state s);
    void add_edge(state s1, state s2, bool maybecycle);
    void mark_live(state s);
    void mark_done(state s);

    bool is_seen(state s);
    bool is_live(state s);
    bool is_dead(state s);
    bool is_done(state s);

    unsigned get_size();

    /*
        Pretty printing
    */
    void pretty_print(std::ostream& o);

};
