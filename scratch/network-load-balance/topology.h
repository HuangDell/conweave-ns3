#ifndef NETWORK_LOAD_BALANCE_TOPOLOGY_H
#define NETWORK_LOAD_BALANCE_TOPOLOGY_H

#include "experiment-config.h"
#include "simulation-state.h"

namespace nlb {

class SimulationMonitor;

class TopologyManager {
  public:
    TopologyManager(const ExperimentConfig& config, SimulationState& state);

    bool Build(SimulationMonitor& monitor);
    void CalculateRoutesAndMetrics();
    void ScheduleLinkEvents();
    void BuildTorInterfaceMaps();
    uint64_t GetIrnBdp() const;

  private:
    void CalculateRoute(ns3::Ptr<ns3::Node> host);
    void SetRoutingEntries();
    void TakeDownLink(uint32_t node_a, uint32_t node_b);
    void DegradeLink(uint32_t node_a, uint32_t node_b, double fraction);
    uint64_t AverageNicRate() const;

    const ExperimentConfig& config_;
    SimulationState& state_;
};

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_TOPOLOGY_H
