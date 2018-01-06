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

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include "UCTNode.h"
#include "FastBoard.h"
#include "FastState.h"
#include "FullBoard.h"
#include "GTP.h"
#include "GameState.h"
#include "KoState.h"
#include "Network.h"
#include "Random.h"
#include "Utils.h"

using namespace Utils;

UCTNode::UCTNode(int vertex, float score, float init_eval)
    : m_move(vertex), m_score(score), m_init_eval(init_eval) {
}

bool UCTNode::first_visit() const {
    return m_visits == 0;
}

bool UCTNode::create_children(std::atomic<int> & nodecount,
                              GameState & state,
                              float & eval) {
    // check whether somebody beat us to it (atomic)
    if (has_children()) {
        return false;
    }
    // acquire the lock
    LOCK(m_nodemutex, lock);
    // no successors in final state
    if (state.get_passes() >= 2) {
        return false;
    }
    // check whether somebody beat us to it (after taking the lock)
    if (has_children()) {
        return false;
    }
    // Someone else is running the expansion
    if (m_is_expanding) {
        return false;
    }
    // We'll be the one queueing this node for expansion, stop others
    m_is_expanding = true;
    lock.unlock();

    const auto raw_netlist = Network::get_scored_moves(
        &state, Network::Ensemble::RANDOM_ROTATION);

    // DCNN returns winrate as side to move
    auto net_eval = raw_netlist.second;
    const auto to_move = state.board.get_to_move();
    // our search functions evaluate from black's point of view
    if (state.board.white_to_move()) {
        net_eval = 1.0f - net_eval;
    }
    eval = net_eval;

    std::vector<Network::scored_node> nodelist;

    auto legal_sum = 0.0f;
    for (const auto& node : raw_netlist.first) {
        auto vertex = node.second;
        if (state.is_move_legal(to_move, vertex)) {
            nodelist.emplace_back(node);
            legal_sum += node.first;
        }
    }

    // If the sum is 0 or a denormal, then don't try to normalize.
    if (legal_sum > std::numeric_limits<float>::min()) {
        // re-normalize after removing illegal moves.
        for (auto& node : nodelist) {
            node.first /= legal_sum;
        }
    }

    link_nodelist(nodecount, nodelist, net_eval);
    return true;
}

void UCTNode::link_nodelist(std::atomic<int> & nodecount,
                            std::vector<Network::scored_node> & nodelist,
                            float init_eval) {
    if (nodelist.empty()) {
        return;
    }

    // Use best to worst order, so highest go first
    std::stable_sort(rbegin(nodelist), rend(nodelist));

    LOCK(m_nodemutex, lock);

    for (const auto& node : nodelist) {
        m_child_scores.push_back({node.second, node.first});
    }
    m_child_init_eval = init_eval;

    nodecount += m_child_scores.size();
    m_has_children = true;
}

// Only safe to call prior to uct_select_child().
void UCTNode::kill_superkos(const KoState& state) {
    LOCK(m_nodemutex, lock);

    assert(m_expanded.size() == 0);

    std::vector<std::pair<int, float>> good_moves;
    for (auto& child : m_child_scores) {
        auto move = child.first;
        if (move != FastBoard::PASS) {
            KoState mystate = state;
            mystate.play_move(move);

            if (mystate.superko()) {
                // Skip moves that result in a superko
                continue;
            }
        }
        good_moves.push_back(child);
    }

    // Now do the actual deletion.
    m_child_scores = good_moves;
}

float UCTNode::eval_state(GameState& state) {
    auto raw_netlist = Network::get_scored_moves(
        &state, Network::Ensemble::RANDOM_ROTATION, -1, true);

    // DCNN returns winrate as side to move
    auto net_eval = raw_netlist.second;

    // But we score from black's point of view
    if (state.board.white_to_move()) {
        net_eval = 1.0f - net_eval;
    }

    return net_eval;
}

// Only safe to call prior to uct_select_child().
void UCTNode::dirichlet_noise(float epsilon, float alpha) {
    LOCK(m_nodemutex, lock);
    assert(m_expanded.size() == 0);

    auto child_cnt = m_child_scores.size();

    auto dirichlet_vector = std::vector<float>{};
    std::gamma_distribution<float> gamma(alpha, 1.0f);
    for (size_t i = 0; i < child_cnt; i++) {
        dirichlet_vector.emplace_back(gamma(Random::get_Rng()));
    }

    auto sample_sum = std::accumulate(begin(dirichlet_vector),
                                      end(dirichlet_vector), 0.0f);

    // If the noise vector sums to 0 or a denormal, then don't try to
    // normalize.
    if (sample_sum < std::numeric_limits<float>::min()) {
        return;
    }

    for (auto& v: dirichlet_vector) {
        v /= sample_sum;
    }

    child_cnt = 0;
    for (auto& child : m_child_scores) {
        auto score = child.second;
        auto eta_a = dirichlet_vector[child_cnt++];
        score = score * (1 - epsilon) + epsilon * eta_a;
        child.second = score;
    }
}

void UCTNode::randomize_first_proportionally() {
    LOCK(m_nodemutex, lock);

    auto accum = std::uint32_t{0};
    auto accum_vector = std::vector<decltype(accum)>{};
    for (const auto& child : m_expanded) {
        accum += child->get_visits();
        accum_vector.emplace_back(accum);
    }

    auto pick = Random::get_Rng().randuint32(accum);
    auto index = size_t{0};
    for (size_t i = 0; i < accum_vector.size(); i++) {
        if (pick < accum_vector[i]) {
            index = i;
            break;
        }
    }

    // Take the early out
    if (index == 0) {
        return;
    }

    assert(m_expanded.size() >= index);

    // Now swap the child at index with the first child
    std::iter_swap(begin(m_expanded), begin(m_expanded) + index);
}

int UCTNode::get_move() const {
    return m_move;
}

UCTNode::NodeStats UCTNode::leave_node(uint32_t visits, double eval_sum) {
    LOCK(m_nodemutex, lock);
    m_visits += visits;
    m_blackevals += eval_sum;
    m_virtual_loss -= VIRTUAL_LOSS_COUNT;

    return get_all_stats();
}

UCTNode::NodeStats UCTNode::enter_node(uint32_t visits, double eval_sum) {
    LOCK(m_nodemutex, lock);
    if (visits > m_visits) {
        m_visits = visits;
        m_blackevals = eval_sum;
    }
    m_virtual_loss += VIRTUAL_LOSS_COUNT;

    return get_all_stats();
}

bool UCTNode::has_children() const {
    return m_has_children;
}

float UCTNode::NodeStats::get_eval(int tomove) const {
    uint32_t total_visits = visits + virtual_loss;

    // If a node has not been visited yet, use the init eval.
    float score = init_eval;
    if (total_visits > 0) {
        auto blackeval = blackevals;
        if (tomove == FastBoard::WHITE) {
            blackeval += virtual_loss;
        }
        score = blackeval / (double)total_visits;
    }
    if (tomove == FastBoard::WHITE) {
        score = 1.0f - score;
    }
    return score;
}

UCTNode::NodeStats UCTNode::get_all_stats() const {
    NodeStats s;
    s.visits = m_visits;
    s.blackevals = m_blackevals;
    s.score = m_score;
    s.init_eval = m_init_eval;
    s.virtual_loss = m_virtual_loss;
    return s;
}

UCTNode::NodeStats UCTNode::get_stats() const {
    LOCK(m_nodemutex, lock);
    return get_all_stats();
}

UCTNode::NodeStats UCTNode::child_get_stats(size_t child) {
    if (child < m_expanded.size()) {
        return m_expanded[child]->get_stats();
    }

    // Node not yet expanded. Fill in default values;
    NodeStats s;
    s.visits = 0;
    s.blackevals = 0;
    s.score = m_child_scores[child].second;
    s.init_eval = m_child_init_eval;
    s.virtual_loss = 0;
    return s;
}

UCTNode* UCTNode::expand(size_t child) {
    assert(child < m_child_scores.size());

    // Skip if already expanded.
    if (child < m_expanded.size()) {
        return m_expanded[child].get();
    }

    // Need to expand a node.
    size_t dest = m_expanded.size();
    // swap score into the right place.
    std::iter_swap(m_child_scores.begin() + dest,
                   m_child_scores.begin() + child);

    // add the new node.
    m_expanded.emplace_back(std::make_unique<UCTNode>(
          m_child_scores[dest].first, // move
          m_child_scores[dest].second, // score
          m_child_init_eval));

    return m_expanded[dest].get();
}

void UCTNode::expand_all() {
    // Expand all the child nodes.
    for (size_t child = 0; child < m_child_scores.size(); ++child) {
        expand(child);  // Does nothing if already expanded.
    }
}

UCTNode* UCTNode::uct_select_child(int color) {

    LOCK(m_nodemutex, lock);

    // Count parentvisits.
    // We do this manually to avoid issues with transpositions.
    auto parentvisits = size_t{0};
    for (const auto& child : m_expanded) {
        if (!child->valid()) continue;
        parentvisits += child->get_visits();
    }
    auto numerator = static_cast<float>(std::sqrt((double)parentvisits));

    size_t best = -1;
    double best_value = -1000.0;
    for (size_t i = 0; i < m_child_scores.size(); ++i) {
        if (i < m_expanded.size() && !m_expanded[i]->valid()) continue;

        auto stats = child_get_stats(i);

        // get_eval() will automatically set first-play-urgency
        double winrate = stats.get_eval(color);
        double psa = stats.score;
        double denom = 1.0f + stats.visits;
        double puct = cfg_puct * psa * (numerator / denom);
        double value = winrate + puct;
        assert(value > -1000.0);

        if (value > best_value) {
            best_value = value;
            best = i;
        }
    }

    assert(best != -1);

    if (best < m_expanded.size()) {
      return m_expanded[best].get();
    }

    return expand(best);
}

class NodeComp : public std::binary_function<UCTNode::node_ptr_t&,
                                             UCTNode::node_ptr_t&, bool> {
public:
    NodeComp(int color) : m_color(color) {};
    bool operator()(const UCTNode::node_ptr_t& a,
                    const UCTNode::node_ptr_t& b) {
        // if visits are not same, sort on visits
        if (a->get_visits() != b->get_visits()) {
            return a->get_visits() < b->get_visits();
        }

        // neither has visits, sort on prior score
        if (a->get_visits() == 0) {
            return a->get_score() < b->get_score();
        }

        // both have same non-zero number of visits
        return a->get_eval(m_color) < b->get_eval(m_color);
    }
private:
    int m_color;
};

void UCTNode::sort_root_children(int color) {
    LOCK(m_nodemutex, lock);

    for (size_t i = 0; i < m_child_scores.size(); ++i) {
        expand(i);  // Does nothing if already expanded.
    }

    assert(!m_expanded.empty());
    std::stable_sort(begin(m_expanded), end(m_expanded), NodeComp(color));
    std::reverse(begin(m_expanded), end(m_expanded));
}

UCTNode& UCTNode::get_best_root_child(int color) {
    LOCK(m_nodemutex, lock);

    // Expand all the child nodes.
    expand_all();

    assert(!m_expanded.empty());

    return *(std::max_element(begin(m_expanded), end(m_expanded),
                              NodeComp(color))->get());
}

UCTNode* UCTNode::get_first_child() const {
    if (m_expanded.empty()) {
        return nullptr;
    }
    return m_expanded.front().get();
}

const std::vector<UCTNode::node_ptr_t>& UCTNode::get_children() const {
    return m_expanded;
}

UCTNode* UCTNode::get_nopass_child(FastState& state) {
    expand_all();
    for (const auto& child : m_expanded) {
        /* If we prevent the engine from passing, we must bail out when
           we only have unreasonable moves to pick, like filling eyes.
           Note that this isn't knowledge isn't required by the engine,
           we require it because we're overruling its moves. */
        if (child->m_move != FastBoard::PASS
            && !state.board.is_eye(state.get_to_move(), child->m_move)) {
            return child.get();
        }
    }
    return nullptr;
}

void UCTNode::invalidate() {
    m_valid = false;
}

bool UCTNode::valid() const {
    return m_valid;
}
