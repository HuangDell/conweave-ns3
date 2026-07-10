/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "experiment-config.h"
#include "load-balancer-setup.h"
#include "monitoring.h"
#include "rdma-setup.h"
#include "simulation-state.h"
#include "topology.h"
#include "traffic-scheduler.h"

#include <ns3/core-module.h>
#include <ns3/global-route-manager.h>
#include <ns3/internet-module.h>
#include <ns3/rdma-hw.h>

#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GENERIC_SIMULATION");

int main(int argc, char* argv[]) {
    clock_t begin_time = clock();

#ifndef PGO_TRAINING
    if (argc <= 1) {
        std::cerr << "Error: require a config file\n";
        fflush(stdout);
        return 1;
    }
    std::string config_path = argv[1];
#else
    std::string config_path = PATH_TO_PGO_CONFIG;
#endif

    nlb::ExperimentConfig config;
    if (!nlb::LoadExperimentConfig(config_path, &config)) {
        return 1;
    }
    if (!nlb::ConfigureSimulationDefaults(config)) {
        return 1;
    }

    nlb::SimulationState state;
    nlb::SimulationMonitor monitor(config, state);
    if (!monitor.OpenCoreOutputs()) {
        std::cerr << "Error: cannot open one or more core output files\n";
        return 1;
    }

    nlb::TopologyManager topology(config, state);
    if (!topology.Build(monitor)) {
        return 1;
    }

    nlb::TrafficScheduler traffic(config, state, monitor);
    if (!traffic.LoadFlowFile()) {
        return 1;
    }

    nlb::InstallRdmaHosts(config, state, topology.GetIrnBdp(), monitor);
    nlb::ConfigureSwitchTransport(config, state);
    topology.CalculateRoutesAndMetrics();
    nlb::ConfigureLoadBalancer(config, state, monitor);

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    traffic.InitializePorts();
    traffic.Start();
    topology.ScheduleLinkEvents();

    if (!monitor.OpenLateOutputs()) {
        std::cerr << "Error: cannot open one or more monitoring output files\n";
        return 1;
    }
    topology.BuildTorInterfaceMaps();
    monitor.StartPeriodicMonitoring();

    std::cout << "------------------------------------------" << std::endl;
    std::cout << "Running Simulation.\n";
    fflush(stdout);
    NS_LOG_INFO("Run Simulation.");
    monitor.StartCompletionMonitoring(traffic.TargetCompletionCount());
    Simulator::Stop(Seconds(config.flowgen_stop_time + 10.0));
    Simulator::Run();
    Simulator::Destroy();
    monitor.CloseOutputs();

    NS_LOG_INFO("Total number of packets: " << RdmaHw::nAllPkts);
    NS_LOG_INFO("Done.");
    clock_t end_time = clock();
    std::cerr << static_cast<double>(end_time - begin_time) / CLOCKS_PER_SEC << "\n";
    return 0;
}
