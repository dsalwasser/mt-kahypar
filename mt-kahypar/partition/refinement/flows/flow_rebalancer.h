#pragma once

#include <memory>
#include <span>

#include "WHFC/algorithm/hyperflowcutter.h"
#include "WHFC/algorithm/parallel_push_relabel.h"
#include "WHFC/algorithm/sequential_push_relabel.h"

#include "mt-kahypar/datastructures/bitset.h"
#include "mt-kahypar/datastructures/hypergraph_common.h"
#include "mt-kahypar/parallel/stl/scalable_vector.h"
#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/partition/refinement/flows/flow_hypergraph_builder.h"
#include "mt-kahypar/partition/refinement/i_rebalancer.h"

namespace mt_kahypar {

template <typename GraphAndGainTypes> class FlowRebalancer {

  using PartitionedHypergraph = typename GraphAndGainTypes::PartitionedHypergraph;
  using Hypergraph = typename GraphAndGainTypes::Hypergraph;
  using GainCache = typename GraphAndGainTypes::GainCache;

public:
  struct Result {
    size_t result_iteration;
    HyperedgeWeight value;
    HyperedgeWeight gain;
    std::span<const Move> moves;
  };

public:
  explicit FlowRebalancer(const Context &context)
      : _context(context), _best_rebalancing_gain(0) {}

  void initialize(
      const PartitionedHypergraph &phg, PartitionID block_0,
      PartitionID block_1, bool sequential,
      const FlowHypergraphBuilder &flow_hg,
      const whfc::HyperFlowCutter<whfc::SequentialPushRelabel> &sequential_hfc,
      const whfc::HyperFlowCutter<whfc::ParallelPushRelabel> &parallel_hfc,
      std::span<const HypernodeID> whfc_to_node);

  void rebalance();

  [[nodiscard]] bool success() const {
    return _best_rebalancing_gain > 0;
  }

  [[nodiscard]] HyperedgeWeight gain() const {
    return _best_rebalancing_gain;
  }

  [[nodiscard]] Result result() const {
    return {
        _rebalancing_result_iteration,
        _best_rebalancing_gain,
        _best_rebalancing_value,
        _best_rebalancing_moves,
    };
  }

private:
  void apply_preliminary_moves();

  void revert_moves();

  void save_moves();

private:
  const Context &_context;
  const PartitionedHypergraph *_phg;

  PartitionID _block_0;
  PartitionID _block_1;

  bool _sequential;
  const FlowHypergraphBuilder *_flow_hg;
  const whfc::HyperFlowCutter<whfc::SequentialPushRelabel> *_sequential_hfc;
  const whfc::HyperFlowCutter<whfc::ParallelPushRelabel> *_parallel_hfc;
  std::span<const HypernodeID> _whfc_to_node;

  Hypergraph _hg_rebalancing_copy;
  PartitionedHypergraph _phg_rebalancing_copy;

  HyperedgeWeight _initial_quality;
  HyperedgeWeight _current_quality;

  std::unique_ptr<GainCache> _gain_cache;
  std::unique_ptr<IRebalancer> _rebalancer;
  ds::Bitset _moved_nodes;
  vec<Move> _moves;

  size_t _total_iteration;
  size_t _rebalancing_result_iteration;
  HyperedgeWeight _best_rebalancing_value;
  HyperedgeWeight _best_rebalancing_gain;
  vec<Move> _best_rebalancing_moves;
};

} // namespace mt_kahypar
