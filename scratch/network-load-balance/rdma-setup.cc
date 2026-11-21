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

    bool real_int = config.cc.odin_int_share_mode != 0;
    if (real_int &&
        (config.cc.mode != CC_MODE_ODIN || config.cc.odin_int_share_mode > 4 ||
         !config.hrlb.enable || config.hrlb.policy != "wc" || config.hrlb.redirect ||
         config.cc.odin_resource_feedback || config.hrlb.bg_resource_feedback ||
         !config.cc.odin_int_window_ns || config.cc.odin_int_window_ns >= (1u << 23) ||
         !config.cc.odin_int_freshness_ns || !config.cc.odin_int_probe_interval_ns)) {
        std::cerr << "Extended INT requires ACK Odin and WC without periodic reports\n";
        return false;
    }
    if (config.cc.mode == CC_MODE_ODIN) {
        bool valid_odin_base =
            config.lb.mode == LbMode::kEcmp && !config.cc.enable_irn && config.cc.enable_pfc &&
            config.cc.has_win && config.cc.var_win && config.cc.rate_bound &&
            config.cc.global_t == 0 && (config.cc.odin_eta > 0 && config.cc.odin_eta <= 1) &&
            (config.cc.odin_gamma > 0 && config.cc.odin_gamma <= 1) &&
            (config.cc.odin_beta_mbps >= 0 && config.cc.odin_beta_mbps <= 1e6) &&
            (config.cc.odin_tbo_ratio >= 0 && config.cc.odin_tbo_ratio <= 1);
        bool valid_legacy_odin =
            !config.hrlb.enable && !config.traffic.v5_qp_pool && config.traffic.v5_nsub == 1 &&
            !config.traffic.v5_chunk_bytes && !config.traffic.v5_estimator_enable &&
            !config.host_sensing.enable;
        bool valid_ack_hrlb =
            config.cc.odin_resource_feedback == 0 &&
            (config.hrlb.policy == "uniform" || config.hrlb.policy == "wc") &&
            !config.hrlb.redirect && (!config.hrlb.gate_inflight0 || real_int) &&
            !config.hrlb.bg_resource_feedback;
        bool valid_hrlb_odin =
            config.hrlb.enable == 1 && (config.cc.odin_resource_feedback == 1 || valid_ack_hrlb) &&
            config.traffic.v5_qp_pool == 1 && config.traffic.v5_qp_pool_size == config.hrlb.k &&
            config.traffic.v5_nsub == 1 && !config.traffic.v5_chunk_bytes &&
            !config.traffic.v5_estimator_enable && !config.host_sensing.enable &&
            (config.hrlb.policy == "uniform" || config.hrlb.policy == "redirect" ||
             config.hrlb.policy == "coord" || (config.hrlb.policy == "wc" && !config.hrlb.redirect)) &&
            config.hrlb.k >= 1 && config.hrlb.chunk_bytes >= 1 && config.hrlb.depth >= 1 &&
            config.hrlb.theta > 0.0 && config.hrlb.coord_delta_psi >= 0.0 &&
            config.hrlb.coord_eps_t >= 0.0 && config.hrlb.coord_delta_guard >= 0.0;
        if (!valid_odin_base || (!valid_legacy_odin && !valid_hrlb_odin)) {
            std::cerr << "Invalid Odin configuration: ECMP, PFC, window and pacing required\n";
            return false;
        }
    }
    IntHop::multi = config.cc.int_multi;
    if (real_int) {
        IntHeader::mode = IntHeader::INT_WITH_RES_ID;
    } else if (config.cc.mode == 7) {
        IntHeader::mode = 1;
    } else if (config.cc.mode == 3 || config.cc.mode == CC_MODE_ODIN) {
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
        hardware->m_odinTraceEnabled = config.monitoring.odin_log != 0;
        hardware->m_odinTraceFlowIds = config.monitoring.odin_log_flow_ids;
        hardware->m_odinAllowPersistent = config.hrlb.enable != 0;
        hardware->m_odinIntShareMode = config.cc.odin_int_share_mode;
        hardware->m_odinIntMinUpdateNs = config.cc.odin_int_min_update_ns;
        hardware->m_odinIntWindowNs = config.cc.odin_int_window_ns;
        hardware->m_odinIntFreshnessNs = config.cc.odin_int_freshness_ns;
        hardware->m_odinIntProbeIntervalNs = config.cc.odin_int_probe_interval_ns;
        hardware->m_odinGateInflight0 =
            (config.hrlb.enable != 0 && config.hrlb.gate_inflight0 != 0);
        hardware->m_odinResourceFreshnessNs = config.hrlb.freshness_ns;
        hardware->m_odinResourceWindowNs = config.cc.odin_resource_window_ns;
        hardware->m_odinExpectedMappingVersion = config.hrlb.mapping_version;
        hardware->SetAttribute("OdinEta", DoubleValue(config.cc.odin_eta));
        hardware->SetAttribute("OdinGamma", DoubleValue(config.cc.odin_gamma));
        hardware->SetAttribute("OdinBeta", DataRateValue(DataRate(
            static_cast<uint64_t>(config.cc.odin_beta_mbps * 1e6))));
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
            switch_node->SetAttribute("OdinTboRatio", DoubleValue(config.cc.odin_tbo_ratio));
            switch_node->SetAttribute("AckHighPrio", UintegerValue(1));
            switch_node->m_odinIntAudit = !config.io.odin_int_output_file.empty();
        }
    }
}

}  // namespace nlb
