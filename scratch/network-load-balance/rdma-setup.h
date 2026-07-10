#ifndef NETWORK_LOAD_BALANCE_RDMA_SETUP_H
#define NETWORK_LOAD_BALANCE_RDMA_SETUP_H

#include "experiment-config.h"
#include "simulation-state.h"

namespace nlb {

class SimulationMonitor;

bool ConfigureSimulationDefaults(const ExperimentConfig& config);
void InstallRdmaHosts(const ExperimentConfig& config, SimulationState& state, uint64_t irn_bdp,
                      SimulationMonitor& monitor);
void ConfigureSwitchTransport(const ExperimentConfig& config, SimulationState& state);

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_RDMA_SETUP_H
