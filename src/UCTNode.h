/*
    This file is part of Leela Zero.
    Copyright (C) 2017 Gian-Carlo Pascutto

    Leela Zero is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leela Zero is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef UCTNODE_H_INCLUDED
#define UCTNODE_H_INCLUDED

#include "config.h"

#include <atomic>
#include <limits>
#include <memory>
#include <vector>

#include "GameState.h"
#include "Network.h"
#include "SMP.h"

class UCTNode {
public:
    // When we visit a node, add this amount of virtual losses
    // to it to encourage other CPUs to explore other parts of the
    // search tree.
    static constexpr auto VIRTUAL_LOSS_COUNT = 3;

    using node_ptr_t = std::unique_ptr<UCTNode>;

    explicit UCTNode(int vertex, float score, float init_eval);
    UCTNode() = delete;
    bool first_visit() const;
    bool has_children() const;
    bool create_children(std::atomic<int>& nodecount,
                         GameState& state, float& eval);
    float eval_state(GameState& state);
    void kill_superkos(const KoState& state);
    void invalidate();
    bool valid() const;
    int get_move() const;

    void dirichlet_noise(float epsilon, float alpha);
    void randomize_first_proportionally();
    void update(float eval = std::numeric_limits<float>::quiet_NaN());

    UCTNode* uct_select_child(int color);
    UCTNode* get_first_child() const;
    UCTNode* get_nopass_child(FastState& state);
    const std::vector<node_ptr_t>& get_children() const;

    void sort_root_children(int color);
    UCTNode& get_best_root_child(int color);

    struct NodeStats {
        uint32_t visits = 0;
        double blackevals = 0;
        float score = 0;
        float init_eval = 0;
        int virtual_loss = 0;

        float get_eval(int color) const;
    };

    // Start walking down a node. This increments virtual loss, and
    // installs 'initial_visits' if it's larger than we have now.
    NodeStats enter_node(uint32_t initial_visits, double initial_eval_sum);
    // Finish walking a node. Accumulate any visit and eval sum
    // and decrement virtual loss.
    NodeStats leave_node(uint32_t visits, double eval_sum);

    // Atomically get a copy of the node statistics.
    NodeStats get_stats() const;

    float get_eval(int color) const { return get_stats().get_eval(color); }
    uint32_t get_visits() const { return get_stats().visits; }
    float get_score() const { return m_score; }

    // Expand all the child nodes out.
    void expand_all();

private:
    void link_nodelist(std::atomic<int>& nodecount,
                       std::vector<Network::scored_node>& nodelist,
                       float init_eval);
    NodeStats child_get_stats(size_t child);
    NodeStats get_all_stats() const;
    UCTNode* expand(size_t child);

    // Tree data
    // (move, score) pairs.
    std::vector<std::pair<int, float>> m_child_scores;
    // pointers to expanded nodes.
    std::vector<node_ptr_t> m_expanded;

    // Move
    const int m_move;
    // UCT
    uint32_t m_visits{0};
    std::atomic<int> m_virtual_loss{0};
    // UCT eval
    const float m_score;
    float m_init_eval;
    float m_child_init_eval;
    double m_blackevals{0};
    // node alive (not superko)
    std::atomic<bool> m_valid{true};
    // Is someone adding scores to this node?
    // We don't need to unset this.
    std::atomic<bool> m_has_children{false};
    bool m_is_expanding{false};
    mutable SMP::Mutex m_nodemutex;
};

#endif
