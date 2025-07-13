/*******************************************************************************
 * MIT License
 *
 * This file is part of Mt-KaHyPar.
 *
 * Copyright (C) 2021 Tobias Heuer <tobias.heuer@kit.edu>
 * Copyright (C) 2021 Lars Gottesbüren <lars.gottesbueren@kit.edu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ******************************************************************************/

#include "mt-kahypar/partition/refinement/flows/flow_refiner.h"
#include "mt-kahypar/partition/refinement/gains/gain_definitions.h"
#include "mt-kahypar/partition/refinement/rebalancing/advanced_rebalancer.h"
#include "mt-kahypar/utils/utilities.h"

#include <tbb/concurrent_queue.h>

#include "mt-kahypar/definitions.h"

namespace mt_kahypar {

template<typename GraphAndGainTypes>
MoveSequence FlowRefiner<GraphAndGainTypes>::refineImpl(mt_kahypar_partitioned_hypergraph_const_t& hypergraph,
                                                     const Subhypergraph& sub_hg,
                                                     const HighResClockTimepoint& start) {
  const PartitionedHypergraph& phg = utils::cast_const<PartitionedHypergraph>(hypergraph);
  MoveSequence sequence { { }, 0 };
  utils::Timer& timer = utils::Utilities::instance().getTimer(_context.utility_id);
  // Construct flow network that contains all vertices given in refinement nodes
  timer.start_timer("construct_flow_network", "Construct Flow Network", true);
  FlowProblem flow_problem = constructFlowHypergraph(phg, sub_hg);
  timer.stop_timer("construct_flow_network");
  if ( flow_problem.total_cut - flow_problem.non_removable_cut > 0 ) {

    // Solve max-flow min-cut problem
    bool time_limit_reached = false;
    timer.start_timer("hyper_flow_cutter", "HyperFlowCutter", true);
    bool flowcutter_succeeded = runFlowCutter(flow_problem, start, time_limit_reached);
    timer.stop_timer("hyper_flow_cutter");
    if ( flowcutter_succeeded ) {
      // We apply the solution if it either improves the cut or the balance of
      // the bipartition induced by the two blocks

      HyperedgeWeight new_cut = flow_problem.non_removable_cut;
      HypernodeWeight max_part_weight;
      const bool sequential = _context.shared_memory.num_threads == _context.refinement.flows.num_parallel_searches;
      if (sequential) {
        new_cut += _sequential_hfc.cs.flow_algo.flow_value;
        max_part_weight = std::max(_sequential_hfc.cs.source_weight, _sequential_hfc.cs.target_weight);
      } else {
        new_cut += _parallel_hfc.cs.flow_algo.flow_value;
        max_part_weight = std::max(_parallel_hfc.cs.source_weight, _parallel_hfc.cs.target_weight);
      }

      const HyperedgeWeight expected_gain = flow_problem.total_cut - new_cut;
      const bool improved_solution = expected_gain > 0 ||
        (expected_gain == 0 && max_part_weight < std::max(flow_problem.weight_of_block_0, flow_problem.weight_of_block_1));

      // Extract move sequence
      if (_rebalanced_gain > 0 && _rebalanced_gain > expected_gain) {
        sequence.expected_improvement = _rebalanced_gain;
        sequence.moves = std::move(_rebalanced_moves);
      } else if ( improved_solution ) {
        sequence.expected_improvement = expected_gain;
        for ( const whfc::Node& u : _flow_hg.nodeIDs() ) {
          const HypernodeID hn = _whfc_to_node[u];
          if ( hn != kInvalidHypernode ) {
            const PartitionID from = phg.partID(hn);
            PartitionID to;
            if (sequential) {
              to = _sequential_hfc.cs.flow_algo.isSource(u) ? _block_0 : _block_1;
            } else {
              to = _parallel_hfc.cs.flow_algo.isSource(u) ? _block_0 : _block_1;
            }

            if ( from != to ) {
              sequence.moves.push_back(Move { from, to, hn, kInvalidGain });
            }
          }
        }
      }
    } else if ( time_limit_reached ) {
      sequence.state = MoveSequenceState::TIME_LIMIT;
    }
  }
  return sequence;
}

#define NOW std::chrono::high_resolution_clock::now()
#define RUNNING_TIME(X) std::chrono::duration<double>(NOW - X).count();

template<typename GraphAndGainTypes>
bool FlowRefiner<GraphAndGainTypes>::runFlowCutter(const FlowProblem& flow_problem,
                                                const HighResClockTimepoint& start,
                                                bool& time_limit_reached) {
  const bool sequential = _context.shared_memory.num_threads == _context.refinement.flows.num_parallel_searches;

  Hypergraph hg = _phg->hypergraph().copy();
  PartitionedHypergraph phg(_phg->k(), hg);
  for (NodeID u = 0; u < hg.initialNumNodes(); ++u) {
    phg.setNodePart(u, _phg->partID(u));
  }

  GainCache gain_cache;
  gain_cache.initializeGainCache(phg);

  HyperedgeWeight initial_quality = metrics::quality(phg, _context, !sequential);
  AdvancedRebalancer<GraphAndGainTypes> rebalancer(hg.initialNumNodes(), _context, gain_cache);

  HyperedgeWeight best_rebalanced_gain = 0;
  vec<Move> best_rebalanced_moves;

  const auto apply_preliminary_moves = [&] {
    HyperedgeWeight gain = 0;

    for (const whfc::Node &u : _flow_hg.nodeIDs()) {
      const HypernodeID hn = _whfc_to_node[u];
      if (hn == kInvalidHypernode) {
        continue;
      }

      const PartitionID from = phg.partID(hn);
      PartitionID to;
      if (sequential) {
        to = _sequential_hfc.cs.flow_algo.isSource(u) ? _block_0 : _block_1;
      } else {
        to = _parallel_hfc.cs.flow_algo.isSource(u) ? _block_0 : _block_1;
      }

      if (from != to) {
        gain += gain_cache.gain(hn, from, to);
        phg.changeNodePart(gain_cache, hn, from, to);
      }
    }

    return gain;
  };

  const auto rebalance = [&] {
    const HyperedgeWeight gain = apply_preliminary_moves();
    initial_quality -= gain;
    ASSERT(initial_quality == metrics::quality(phg, _context, !sequential));

    Metrics tmp_metrics;
    tmp_metrics.quality = initial_quality;
    tmp_metrics.imbalance = metrics::imbalance(phg, _context);

    vec<Move> moves;
    mt_kahypar_partitioned_hypergraph_t phg_prime = utils::partitioned_hg_cast(phg);
    const bool balanced = rebalancer.refineAndOutputMovesLinear(phg_prime, {}, moves, tmp_metrics, 0.0);

    const HyperedgeWeight rebalanced_quality = tmp_metrics.quality;
    ASSERT(rebalanced_quality == metrics::quality(phg, _context, !sequential));

    for (const Move &move : moves) {
      phg.changeNodePart(gain_cache, move.node, move.to, move.from);
    }
    ASSERT(initial_quality == metrics::quality(phg, _context, !sequential));

    const HyperedgeWeight rebalanced_gain = initial_quality - rebalanced_quality;
    if (balanced && rebalanced_gain > best_rebalanced_gain) {
      best_rebalanced_gain = rebalanced_gain;
      best_rebalanced_moves = std::move(moves);
    }
  };

  size_t iteration = 0;
  const auto reached_time_limit = [&] {
    if (iteration == 25) {
      iteration = 0;

      const double elapsed = RUNNING_TIME(start);
      if (elapsed > _time_limit) {
        time_limit_reached = true;
        return true;
      }
    }

    return false;
  };

  const auto on_cut = [&](const auto &cs) {
    if (reached_time_limit()) {
      return false;
    }

    if (!cs.isBalanced() && _context.refinement.flows.rebalancing) {
      rebalance();
    }

    return true;
  };

  bool result = false;
  if (sequential) {
    _sequential_hfc.cs.setMaxBlockWeight(0, std::max(
            flow_problem.weight_of_block_0, _context.partition.max_part_weights[_block_0]));
    _sequential_hfc.cs.setMaxBlockWeight(1, std::max(
            flow_problem.weight_of_block_1, _context.partition.max_part_weights[_block_1]));

    _sequential_hfc.reset();
    _sequential_hfc.setFlowBound(flow_problem.total_cut - flow_problem.non_removable_cut);
    result = _sequential_hfc.enumerateCutsUntilBalancedOrFlowBoundExceeded(flow_problem.source, flow_problem.sink, on_cut);
  } else {
    _parallel_hfc.cs.setMaxBlockWeight(0, std::max(
            flow_problem.weight_of_block_0, _context.partition.max_part_weights[_block_0]));
    _parallel_hfc.cs.setMaxBlockWeight(1, std::max(
            flow_problem.weight_of_block_1, _context.partition.max_part_weights[_block_1]));

    _parallel_hfc.reset();
    _parallel_hfc.setFlowBound(flow_problem.total_cut - flow_problem.non_removable_cut);
    result = _parallel_hfc.enumerateCutsUntilBalancedOrFlowBoundExceeded(flow_problem.source, flow_problem.sink, on_cut);
  }

  _rebalanced_gain = best_rebalanced_gain;
  _rebalanced_moves = std::move(best_rebalanced_moves);

  return result;
}

template<typename GraphAndGainTypes>
FlowProblem FlowRefiner<GraphAndGainTypes>::constructFlowHypergraph(const PartitionedHypergraph& phg,
                                                                 const Subhypergraph& sub_hg) {
  _block_0 = sub_hg.block_0;
  _block_1 = sub_hg.block_1;
  ASSERT(_block_0 != kInvalidPartition && _block_1 != kInvalidPartition);
  FlowProblem flow_problem;


  const bool sequential = _context.shared_memory.num_threads == _context.refinement.flows.num_parallel_searches;
  if ( sequential ) {
    flow_problem = _sequential_construction.constructFlowHypergraph(
      phg, sub_hg, _block_0, _block_1, _whfc_to_node);
  } else {
    flow_problem = _parallel_construction.constructFlowHypergraph(
      phg, sub_hg, _block_0, _block_1, _whfc_to_node);
  }

  DBG << "Flow Hypergraph [ Nodes =" << _flow_hg.numNodes()
      << ", Edges =" << _flow_hg.numHyperedges()
      << ", Pins =" << _flow_hg.numPins()
      << ", Blocks = (" << _block_0 << "," << _block_1 << ") ]";

  return flow_problem;
}

namespace {
#define FLOW_REFINER(X) FlowRefiner<X>
}

INSTANTIATE_CLASS_WITH_VALID_TRAITS(FLOW_REFINER)

} // namespace mt_kahypar
