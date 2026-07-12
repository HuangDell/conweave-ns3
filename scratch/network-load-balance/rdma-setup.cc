#include "rdma-setup.h"

#include "monitoring.h"

#include <ns3/qbb-net-device.h>
#include <ns3/rdma-driver.h>
#include <ns3/rdma-hw.h>
#include <ns3/sim-setting.h>
#include <ns3/int-header.h>
#include <ns3/switch-node.h>

#include <cstdlib>
#include <iostream>

namespace nlb {

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NetworkLoadBalanceRdmaSetup");

bool ConfigureSimulationDefaults(const ExperimentConfig& config) {
    LogComponentEnable("GENERIC_SIMULATION", LOG_LEVEL_DEBUG);
    LogComponentEnable("NetworkLoadBalanceMonitoring", LOG_LEVEL_DEBUG);
    LogComponentEnable("NetworkLoadBalanceRdmaSetup", LOG_LEVEL_DEBUG);
    LogComponentEnable("NetworkLoadBalanceTopology", LOG_LEVEL_DEBUG);
    LogComponentEnable("NetworkLoadBalanceTraffic", LOG_LEVEL_DEBUG);
    LogComponentEnable("NetworkLoadBalanceLbSetup", LOG_LEVEL_DEBUG);
    NS_LOG_INFO("Initialize random seed: " << config.random_seed);
    srand(static_cast<unsigned>(config.random_seed));
    SeedManager::SetSeed(config.random_seed);

    Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(config.cc.pause_time));
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(config.cc.enable_qcn));
    Config::SetDefault("ns3::QbbNetDevice::DynamicThreshold",
                       BooleanValue(config.cc.use_dynamic_pfc_threshold));
    Config::SetDefault("ns3::QbbNetDevice::QbbEnabled", BooleanValue(config.cc.enable_pfc));

    if (config.cc.mode != 1 && config.lb.mode == LbMode::kConweave) {
        std::cout << "Currently, ConWeave supports only DCQCN congestion control for RDMA. \nIf "
                     "you want to extend, the reordering delay at DstTor must be considered."
                  << std::endl;
        return false;
    }

    IntHop::multi = config.cc.int_multi;
    if (config.cc.mode == 7) {
        IntHeader::mode = 1;
    } else if (config.cc.mode == 3) {
        IntHeader::mode = 0;
    } else {
        IntHeader::mode = 5;
    }
    return true;
}

void InstallRdmaHosts(const ExperimentConfig& config, SimulationState& state, uint64_t irn_bdp,
                      SimulationMonitor& monitor) {
    for (uint32_t i = 0; i < state.node_num; ++i) {
        if (state.nodes.Get(i)->GetNodeType() != 0) {
            continue;
        }
        Ptr<RdmaHw> hardware = CreateObject<RdmaHw>();
        hardware->SetAttribute("ClampTargetRate", BooleanValue(config.cc.clamp_target_rate));
        hardware->SetAttribute("AlphaResumInterval",
                               DoubleValue(config.cc.alpha_resume_interval));
        hardware->SetAttribute("RPTimer", DoubleValue(config.cc.rp_timer));
        hardware->SetAttribute("FastRecoveryTimes",
                               UintegerValue(config.cc.fast_recovery_times));
        hardware->SetAttribute("EwmaGain", DoubleValue(config.cc.ewma_gain));
        hardware->SetAttribute("RateAI", DataRateValue(DataRate(config.cc.rate_ai)));
        hardware->SetAttribute("RateHAI", DataRateValue(DataRate(config.cc.rate_hai)));
        hardware->SetAttribute("L2BackToZero", BooleanValue(config.cc.l2_back_to_zero));
        hardware->SetAttribute("L2ChunkSize", UintegerValue(config.cc.l2_chunk_size));
        hardware->SetAttribute("L2AckInterval", UintegerValue(config.cc.l2_ack_interval));
        hardware->SetAttribute("CcMode", UintegerValue(config.cc.mode));
        hardware->SetAttribute("RateDecreaseInterval",
                               DoubleValue(config.cc.rate_decrease_interval));
        hardware->SetAttribute("MinRate", DataRateValue(DataRate(config.cc.min_rate)));
        hardware->SetAttribute("Mtu", UintegerValue(config.packet_payload_size));
        hardware->SetAttribute("MiThresh", UintegerValue(config.cc.mi_thresh));
        hardware->SetAttribute("VarWin", BooleanValue(config.cc.var_win));
        hardware->SetAttribute("FastReact", BooleanValue(config.cc.fast_react));
        hardware->SetAttribute("MultiRate", BooleanValue(config.cc.multi_rate));
        hardware->SetAttribute("SampleFeedback", BooleanValue(config.cc.sample_feedback));
        hardware->SetAttribute("TargetUtil", DoubleValue(config.cc.u_target));
        hardware->SetAttribute("RateBound", BooleanValue(config.cc.rate_bound));
        hardware->SetAttribute("DctcpRateAI", DataRateValue(DataRate(config.cc.dctcp_rate_ai)));
        hardware->SetAttribute("IrnEnable", BooleanValue(config.cc.enable_irn));
        hardware->SetAttribute("IrnRtoHigh", TimeValue(MicroSeconds(320)));
        hardware->SetAttribute("IrnRtoLow", TimeValue(MicroSeconds(100)));
        hardware->SetAttribute("IrnBdp", UintegerValue(irn_bdp));
        hardware->m_persistentPacketEvents =
            config.traffic.v5_qp_pool && config.traffic.v5_qp_state_log &&
            config.traffic.v5_qp_pool_size == 1 && config.traffic.v5_chunk_bytes == 0;

        if (config.cc.mode == 1) {
            monitor.StartCnpMonitoring(hardware);
        }

        Ptr<RdmaDriver> driver = CreateObject<RdmaDriver>();
        Ptr<Node> node = state.nodes.Get(i);
        driver->SetNode(node);
        driver->SetRdmaHw(hardware);
        node->AggregateObject(driver);
        driver->Init();
        monitor.ConnectQpComplete(driver);
    }
}

void ConfigureSwitchTransport(const ExperimentConfig& config, SimulationState& state) {
    for (uint32_t i = 0; i < state.node_num; ++i) {
        if (state.nodes.Get(i)->GetNodeType() == 1) {
            Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(state.nodes.Get(i));
            switch_node->SetAttribute("CcMode", UintegerValue(config.cc.mode));
            switch_node->SetAttribute("AckHighPrio", UintegerValue(1));
        }
    }
}

}  // namespace nlb
