#include "mt-kahypar/partition/refinement/flows/flow_rebalancer.h"

#include <utility>

#include "kahypar-resources/macros.h"
#include "mt-kahypar/partition/metrics.h"
#include "mt-kahypar/partition/refinement/gains/gain_definitions.h"
#include "mt-kahypar/partition/refinement/rebalancing/advanced_rebalancer.h"
#include "mt-kahypar/utils/cast.h"

namespace mt_kahypar {

template <typename GraphAndGainTypes>
void FlowRebalancer<GraphAndGainTypes>::initialize(
    const PartitionedHypergraph &phg, const PartitionID block_0,
    const PartitionID block_1, const bool sequential,
    const FlowHypergraphBuilder &flow_hg,
    const whfc::HyperFlowCutter<whfc::SequentialPushRelabel> &sequential_hfc,
    const whfc::HyperFlowCutter<whfc::ParallelPushRelabel> &parallel_hfc,
    std::span<const HypernodeID> whfc_to_node) {
  _phg = &phg;

  _block_0 = block_0;
  _block_1 = block_1;

  _sequential = sequential;
  _flow_hg = &flow_hg;
  _sequential_hfc = &sequential_hfc;
  _parallel_hfc = &parallel_hfc;
  _whfc_to_node = whfc_to_node;

  _hg_rebalancing_copy = _phg->hypergraph().copy();
  _phg_rebalancing_copy = PartitionedHypergraph(_phg->k(), _hg_rebalancing_copy);

  const HypernodeID num_nodes = _phg_rebalancing_copy.initialNumNodes();
  tbb::parallel_for(ID(0), num_nodes, [&](const HypernodeID &hn) {
    _phg_rebalancing_copy.setOnlyNodePart(hn, _phg->partID(hn));
  });
  _phg_rebalancing_copy.initializePartition();

  _initial_quality = metrics::quality(_phg_rebalancing_copy, _context, false);
  _current_quality = _initial_quality;

  _gain_cache = std::make_unique<GainCache>();
  _gain_cache->initializeGainCache(_phg_rebalancing_copy);
  _rebalancer = std::make_unique<AdvancedRebalancer<GraphAndGainTypes>>(num_nodes, _context, *_gain_cache);
  _moved_nodes.resize(num_nodes);

  _total_iteration = 0;
  _rebalancing_result_iteration = 0;
  _best_rebalancing_value = 0;
  _best_rebalancing_gain = 0;
  _best_rebalancing_moves.clear();
}

template <typename GraphAndGainTypes>
void FlowRebalancer<GraphAndGainTypes>::rebalance() {
  _total_iteration += 1;

  apply_preliminary_moves();

  Metrics tmp_metrics;
  tmp_metrics.quality = _current_quality;
  tmp_metrics.imbalance = metrics::imbalance(_phg_rebalancing_copy, _context);

  mt_kahypar_partitioned_hypergraph_t phg_prime = utils::partitioned_hg_cast(_phg_rebalancing_copy);
  const bool balanced = _rebalancer->refineAndOutputMovesLinear(phg_prime, {}, _moves, tmp_metrics, 0.0);

  const HyperedgeWeight rebalanced_quality = tmp_metrics.quality;
  ASSERT(rebalanced_quality == metrics::quality(_phg_rebalancing_copy, _context));

  revert_moves();

  const HyperedgeWeight rebalancing_gain = _initial_quality - rebalanced_quality;
  if (balanced && rebalancing_gain > _best_rebalancing_gain) {
    _rebalancing_result_iteration = _total_iteration;
    _best_rebalancing_value = rebalanced_quality;
    _best_rebalancing_gain = rebalancing_gain;

    save_moves();
  }
}

template <typename GraphAndGainTypes>
void FlowRebalancer<GraphAndGainTypes>::apply_preliminary_moves() {
  HyperedgeWeight gain = 0;

  for (const whfc::Node &u : _flow_hg->nodeIDs()) {
    const HypernodeID hn = _whfc_to_node[u];
    if (hn == kInvalidHypernode) {
      continue;
    }

    const PartitionID from = _phg_rebalancing_copy.partID(hn);
    PartitionID to;
    if (_sequential) {
      to = _sequential_hfc->cs.flow_algo.isSource(u) ? _block_0 : _block_1;
    } else {
      to = _parallel_hfc->cs.flow_algo.isSource(u) ? _block_0 : _block_1;
    }

    if (from != to) {
      gain += _gain_cache->gain(hn, from, to);
      _phg_rebalancing_copy.changeNodePart(*_gain_cache, hn, from, to);
    }
  }

  _current_quality -= gain;
  ASSERT(_current_quality == metrics::quality(_phg_rebalancing_copy, _context));
}

template <typename GraphAndGainTypes>
void FlowRebalancer<GraphAndGainTypes>::revert_moves() {
  for (const Move &move : _moves) {
    _phg_rebalancing_copy.changeNodePart(*_gain_cache, move.node, move.to, move.from);
  }

  ASSERT(_current_quality == metrics::quality(_phg_rebalancing_copy, _context));
}

template <typename GraphAndGainTypes>
void FlowRebalancer<GraphAndGainTypes>::save_moves() {
  _best_rebalancing_moves.clear();

  _moved_nodes.reset();
  for (const Move &move : _moves) {
    const HypernodeID hn = move.node;
    _moved_nodes.set(move.node);

    const PartitionID from = _phg->partID(hn);
    const PartitionID to = move.to;
    if (from != to) {
      _best_rebalancing_moves.emplace_back(from, to, hn);
    }
  }

  for (const whfc::Node &u : _flow_hg->nodeIDs()) {
    const HypernodeID hn = _whfc_to_node[u];
    if (hn == kInvalidHypernode || _moved_nodes.isSet(hn)) {
      continue;
    }

    const PartitionID from = _phg->partID(hn);
    const PartitionID to = _phg_rebalancing_copy.partID(hn);
    if (from != to) {
      _best_rebalancing_moves.emplace_back(from, to, hn);
    }
  }
}

namespace {
#define FLOW_REBALANCER(X) FlowRebalancer<X>
} // namespace

INSTANTIATE_CLASS_WITH_VALID_TRAITS(FLOW_REBALANCER)

} // namespace mt_kahypar
