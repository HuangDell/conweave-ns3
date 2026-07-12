#include "load-balancer-setup.h"

#include "monitoring.h"

#include <ns3/conga-routing.h>
#include <ns3/letflow-routing.h>
#include <ns3/qbb-net-device.h>
#include <ns3/settings.h>

#include <cassert>
#include <cstring>
#include <iostream>

namespace nlb {

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NetworkLoadBalanceLbSetup");

namespace {

uint32_t PackPath(const uint8_t path_ports[4]) {
    uint32_t path_id = 0;
    std::memcpy(&path_id, path_ports, sizeof(path_id));
    return path_id;
}

void RegisterPath(const ExperimentConfig& config, Ptr<SwitchNode> source_switch,
                  uint32_t destination_tor, uint32_t path_id, uint32_t hop_count) {
    if (config.lb.mode == LbMode::kConga) {
        source_switch->m_mmu->m_congaRouting.m_congaRoutingTable[destination_tor].insert(path_id);
    } else if (config.lb.mode == LbMode::kLetflow) {
        source_switch->m_mmu->m_letflowRouting.m_letflowRoutingTable[destination_tor].insert(
            path_id);
    } else if (config.lb.mode == LbMode::kSflowlet) {
        source_switch->m_mmu->m_cpRouting.m_letflowRoutingTable[destination_tor].insert(path_id);
    } else if (config.lb.mode == LbMode::kConweave) {
        source_switch->m_mmu->m_conweaveRouting.m_ConWeaveRoutingTable[destination_tor].insert(
            path_id);
        source_switch->m_mmu->m_conweaveRouting.m_rxToRId2BaseRTT[destination_tor] =
            config.one_hop_delay_ns * hop_count * 2;
    }
}

bool UsesTorToTorRouting(uint32_t mode) {
    return mode == LbMode::kConga || mode == LbMode::kLetflow ||
           mode == LbMode::kConweave || mode == LbMode::kSflowlet;
}

}  // namespace

void ConfigureLoadBalancer(const ExperimentConfig& config, SimulationState& state,
                           SimulationMonitor& monitor) {
    std::cout << "Configuring switches" << std::endl;
    for (const auto& link : state.link_pairs) {
        Ptr<Node> possible_host = state.nodes.Get(link.first);
        Ptr<Node> possible_switch = state.nodes.Get(link.second);
        if (possible_host->GetNodeType() == 0 && possible_switch->GetNodeType() == 1) {
            Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(possible_switch);
            switch_node->m_isToR = true;
            uint32_t host_ip = state.server_addresses[link.first].Get();
            switch_node->m_isToR_hostIP.insert(host_ip);
            state.tor_by_id[switch_node->GetId()] = switch_node;
        }
    }

    if (UsesTorToTorRouting(config.lb.mode)) {
        NS_LOG_INFO("Configuring Load Balancer's Switches");
        for (const auto& link : state.link_pairs) {
            Ptr<Node> possible_host = state.nodes.Get(link.first);
            Ptr<Node> possible_switch = state.nodes.Get(link.second);
            if (possible_host->GetNodeType() == 0 && possible_switch->GetNodeType() == 1) {
                uint32_t host_ip = state.server_addresses[link.first].Get();
                Settings::hostIp2SwitchId[host_ip] = possible_switch->GetId();
            }
        }

        for (const auto& source_entry : state.next_hops) {
            Ptr<Node> source_node = source_entry.first;
            if (source_node->GetNodeType() != 1) {
                continue;
            }
            Ptr<SwitchNode> source_switch = DynamicCast<SwitchNode>(source_node);
            uint32_t source_switch_id = source_switch->GetId();
            if (state.tor_by_id.find(source_switch_id) == state.tor_by_id.end()) {
                continue;
            }

            for (const auto& destination_entry : source_entry.second) {
                Ptr<Node> destination = destination_entry.first;
                uint32_t destination_ip = Settings::hostId2IpMap[destination->GetId()];
                uint32_t destination_tor = Settings::hostIp2SwitchId[destination_ip];
                if (source_switch_id == destination_tor) {
                    continue;
                }

                if (config.lb.mode == LbMode::kConga) {
                    source_switch->m_mmu->m_congaRouting.m_congaFromLeafTable[destination_tor];
                    source_switch->m_mmu->m_congaRouting.m_congaToLeafTable[destination_tor];
                }

                uint8_t path_ports[4] = {0, 0, 0, 0};
                for (const auto& next_1 : destination_entry.second) {
                    uint32_t output_1 = state.neighbor_interfaces[source_node][next_1].idx;
                    const std::vector<Ptr<Node>>& next_hops_2 = state.next_hops[next_1][destination];
                    if (next_hops_2.size() == 1 && next_hops_2[0]->GetId() == destination_tor) {
                        uint32_t output_2 =
                            state.neighbor_interfaces[next_1][next_hops_2[0]].idx;
                        path_ports[0] = static_cast<uint8_t>(output_1);
                        path_ports[1] = static_cast<uint8_t>(output_2);
                        RegisterPath(config, source_switch, destination_tor, PackPath(path_ports), 2);
                        continue;
                    }

                    for (const auto& next_2 : next_hops_2) {
                        uint32_t output_2 = state.neighbor_interfaces[next_1][next_2].idx;
                        const std::vector<Ptr<Node>>& next_hops_3 =
                            state.next_hops[next_2][destination];
                        if (next_hops_3.size() == 1 &&
                            next_hops_3[0]->GetId() == destination_tor) {
                            uint32_t output_3 =
                                state.neighbor_interfaces[next_2][next_hops_3[0]].idx;
                            path_ports[0] = static_cast<uint8_t>(output_1);
                            path_ports[1] = static_cast<uint8_t>(output_2);
                            path_ports[2] = static_cast<uint8_t>(output_3);
                            RegisterPath(config, source_switch, destination_tor,
                                         PackPath(path_ports), 3);
                            continue;
                        }

                        for (const auto& next_3 : next_hops_3) {
                            uint32_t output_3 = state.neighbor_interfaces[next_2][next_3].idx;
                            const std::vector<Ptr<Node>>& next_hops_4 =
                                state.next_hops[next_3][destination];
                            if (next_hops_4.size() == 1 &&
                                next_hops_4[0]->GetId() == destination_tor) {
                                uint32_t output_4 =
                                    state.neighbor_interfaces[next_3][next_hops_4[0]].idx;
                                path_ports[0] = static_cast<uint8_t>(output_1);
                                path_ports[1] = static_cast<uint8_t>(output_2);
                                path_ports[2] = static_cast<uint8_t>(output_3);
                                path_ports[3] = static_cast<uint8_t>(output_4);
                                RegisterPath(config, source_switch, destination_tor,
                                             PackPath(path_ports), 4);
                                continue;
                            }
                            printf("Too large topology?\n");
                            assert(false);
                        }
                    }
                }
            }
        }

        for (const auto& node_entry : state.next_hops) {
            if (node_entry.first->GetNodeType() != 1) {
                continue;
            }
            Ptr<Node> node = node_entry.first;
            Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(node);
            for (const auto& destination_entry : node_entry.second) {
                for (const auto& next : destination_entry.second) {
                    uint32_t output_port = state.neighbor_interfaces[node][next].idx;
                    uint64_t bandwidth = state.neighbor_interfaces[node][next].bw;
                    switch_node->m_mmu->m_congaRouting.SetLinkCapacity(output_port, bandwidth);
                    if ((config.lb.mode == LbMode::kSflowlet ||
                         config.traffic.v5_estimator_enable) &&
                        switch_node->m_isToR && next->GetNodeType() == 1) {
                        Ptr<QbbNetDevice> device =
                            DynamicCast<QbbNetDevice>(node->GetDevice(output_port));
                        switch_node->m_mmu->m_residualEstimator.RegisterPort(output_port, device,
                                                                             bandwidth);
                    }
                }
            }
        }

        for (const auto& node_entry : state.next_hops) {
            if (node_entry.first->GetNodeType() != 1) {
                continue;
            }
            Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(node_entry.first);
            NS_LOG_INFO("Switch Info - ID:%u, ToR:%d\n" %
                        (switch_node->GetId(), switch_node->m_isToR));
            if (config.lb.mode == LbMode::kConga) {
                switch_node->m_mmu->m_congaRouting.SetConstants(
                    config.lb.conga_dre_time, config.lb.conga_aging_time,
                    config.lb.conga_flowlet_timeout, config.lb.conga_quantize_bit,
                    config.lb.conga_alpha);
                switch_node->m_mmu->m_congaRouting.SetSwitchInfo(switch_node->m_isToR,
                                                                 switch_node->GetId());
            } else if (config.lb.mode == LbMode::kLetflow) {
                switch_node->m_mmu->m_letflowRouting.SetConstants(
                    config.lb.letflow_aging_time, config.lb.letflow_flowlet_timeout);
                switch_node->m_mmu->m_letflowRouting.SetSwitchInfo(switch_node->m_isToR,
                                                                   switch_node->GetId());
            } else if (config.lb.mode == LbMode::kConweave) {
                switch_node->m_mmu->m_conweaveRouting.SetConstants(
                    config.lb.conweave_extra_reply_deadline,
                    config.lb.conweave_extra_voq_flush_time,
                    config.lb.conweave_tx_expiry_time,
                    config.lb.conweave_default_voq_waiting_time,
                    config.lb.conweave_path_pause_time,
                    config.lb.conweave_path_aware_rerouting);
                switch_node->m_mmu->m_conweaveRouting.SetSwitchInfo(switch_node->m_isToR,
                                                                    switch_node->GetId());
            } else if (config.lb.mode == LbMode::kSflowlet) {
                switch_node->m_mmu->m_cpRouting.SetConstants(
                    config.lb.letflow_aging_time, config.lb.sflowlet_flowlet_timeout);
                switch_node->m_mmu->m_cpRouting.SetSwitchInfo(switch_node->m_isToR,
                                                              switch_node->GetId());
                switch_node->m_mmu->m_cpRouting.SetWeightMode(config.lb.sflowlet_weight_mode);
                switch_node->m_mmu->m_cpRouting.SetEstimator(
                    &switch_node->m_mmu->m_residualEstimator);
            }
            if (config.lb.mode == LbMode::kSflowlet ||
                config.traffic.v5_estimator_enable) {
                switch_node->m_mmu->m_residualEstimator.SetConstants(
                    config.lb.sflowlet_est_time, config.lb.sflowlet_ewma_beta,
                    config.lb.sflowlet_persist_windows, config.lb.sflowlet_degrade_ratio,
                    config.lb.sflowlet_backlog_thresh_bytes);
                switch_node->m_mmu->m_residualEstimator.SetSwitchInfo(switch_node->m_isToR,
                                                                      switch_node->GetId());
                if (switch_node->m_isToR) {
                    switch_node->m_mmu->m_residualEstimator.SetSnapshotCallback(
                        MakeCallback(&SimulationMonitor::RecordEstimatorBatch, &monitor));
                    switch_node->m_mmu->m_residualEstimator.Start();
                }
            }
        }
        monitor.ScheduleModeHistory();
    }
}

}  // namespace nlb
