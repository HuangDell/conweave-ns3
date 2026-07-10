#ifndef NETWORK_LOAD_BALANCE_LOAD_BALANCER_SETUP_H
#define NETWORK_LOAD_BALANCE_LOAD_BALANCER_SETUP_H

#include "experiment-config.h"
#include "simulation-state.h"

namespace nlb {

class SimulationMonitor;

void ConfigureLoadBalancer(const ExperimentConfig& config, SimulationState& state,
                           SimulationMonitor& monitor);

}  // namespace nlb

#endif  // NETWORK_LOAD_BALANCE_LOAD_BALANCER_SETUP_H
