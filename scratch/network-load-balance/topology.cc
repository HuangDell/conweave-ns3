#include "topology.h"

#include "monitoring.h"

#include <ns3/broadcom-node.h>
#include <ns3/error-model.h>
#include <ns3/ipv4-static-routing-helper.h>
#include <ns3/qbb-channel.h>
#include <ns3/qbb-helper.h>
#include <ns3/qbb-net-device.h>
#include <ns3/rdma-driver.h>
#include <ns3/settings.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace nlb {

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NetworkLoadBalanceTopology");

TopologyManager::TopologyManager(const ExperimentConfig& config, SimulationState& state)
    : config_(config), state_(state) {}

bool TopologyManager::Build(SimulationMonitor& monitor) {
    std::ifstream topology(config_.io.topology_file.c_str());
    if (!topology.is_open()) {
        std::cerr << "Error: cannot open topology file " << config_.io.topology_file << '\n';
        return false;
    }

    topology >> state_.node_num >> state_.switch_num >> state_.link_num;
    if (!topology) {
        std::cerr << "Error: malformed topology header in " << config_.io.topology_file << '\n';
        return false;
    }

    Settings::node_num = state_.node_num;
    Settings::host_num = state_.node_num - state_.switch_num;
    Settings::switch_num = state_.switch_num;
    Settings::lb_mode = config_.lb.mode;
    Settings::packet_payload = config_.packet_payload_size;

    std::vector<uint32_t> node_type(state_.node_num, 0);
    for (uint32_t i = 0; i < state_.switch_num; ++i) {
        uint32_t switch_id;
        topology >> switch_id;
        node_type[switch_id] = 1;
    }
    for (uint32_t i = 0; i < state_.node_num; ++i) {
        if (node_type[i] == 0) {
            state_.nodes.Add(CreateObject<Node>());
        } else {
            Ptr<SwitchNode> switch_node = CreateObject<SwitchNode>();
            state_.nodes.Add(switch_node);
            switch_node->SetAttribute("EcnEnabled", BooleanValue(config_.cc.enable_qcn));
        }
    }
    NS_LOG_INFO("Create nodes.");

    InternetStackHelper internet;
    internet.Install(state_.nodes);

    state_.server_addresses.resize(state_.node_num);
    for (uint32_t i = 0; i < state_.node_num; ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 0) {
            state_.server_addresses[i] = Settings::node_id_to_ip(i);
        }
    }

    NS_LOG_INFO("Create channels.");
    Ptr<RateErrorModel> default_error_model = CreateObject<RateErrorModel>();
    Ptr<UniformRandomVariable> default_random = CreateObject<UniformRandomVariable>();
    default_error_model->SetRandomVariable(default_random);
    default_random->SetStream(50);
    default_error_model->SetAttribute("ErrorRate", DoubleValue(config_.error_rate_per_link));
    default_error_model->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

    QbbHelper qbb;
    Ipv4AddressHelper ipv4;
    for (uint32_t i = 0; i < state_.link_num; ++i) {
        uint32_t source_id;
        uint32_t destination_id;
        std::string data_rate;
        std::string link_delay;
        double error_rate;
        topology >> source_id >> destination_id >> data_rate >> link_delay >> error_rate;
        if (!topology) {
            std::cerr << "Error: malformed link entry " << i << " in " << config_.io.topology_file
                      << '\n';
            return false;
        }

        assert(std::to_string(config_.one_hop_delay_ns) + "ns" == link_delay);
        state_.link_pairs.push_back(std::make_pair(source_id, destination_id));
        Ptr<Node> source = state_.nodes.Get(source_id);
        Ptr<Node> destination = state_.nodes.Get(destination_id);

        qbb.SetDeviceAttribute("DataRate", StringValue(data_rate));
        qbb.SetChannelAttribute("Delay", StringValue(link_delay));
        if (error_rate > 0) {
            Ptr<RateErrorModel> link_error_model = CreateObject<RateErrorModel>();
            Ptr<UniformRandomVariable> link_random = CreateObject<UniformRandomVariable>();
            link_error_model->SetRandomVariable(link_random);
            link_random->SetStream(50);
            link_error_model->SetAttribute("ErrorRate", DoubleValue(error_rate));
            link_error_model->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
            qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(link_error_model));
        } else {
            qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(default_error_model));
        }

        NetDeviceContainer devices = qbb.Install(source, destination);
        if (source->GetNodeType() == 0) {
            Ptr<Ipv4> source_ipv4 = source->GetObject<Ipv4>();
            source_ipv4->AddInterface(devices.Get(0));
            source_ipv4->AddAddress(
                1, Ipv4InterfaceAddress(state_.server_addresses[source_id], Ipv4Mask(0xff000000)));
        }
        if (destination->GetNodeType() == 0) {
            Ptr<Ipv4> destination_ipv4 = destination->GetObject<Ipv4>();
            destination_ipv4->AddInterface(devices.Get(1));
            destination_ipv4->AddAddress(
                1, Ipv4InterfaceAddress(state_.server_addresses[destination_id],
                                        Ipv4Mask(0xff000000)));
        }

        Ptr<QbbNetDevice> source_device = DynamicCast<QbbNetDevice>(devices.Get(0));
        Ptr<QbbNetDevice> destination_device = DynamicCast<QbbNetDevice>(devices.Get(1));
        Interface& source_interface = state_.neighbor_interfaces[source][destination];
        source_interface.idx = source_device->GetIfIndex();
        source_interface.up = true;
        source_interface.delay =
            DynamicCast<QbbChannel>(source_device->GetChannel())->GetDelay().GetTimeStep();
        source_interface.bw = source_device->GetDataRate().GetBitRate();
        Interface& destination_interface = state_.neighbor_interfaces[destination][source];
        destination_interface.idx = destination_device->GetIfIndex();
        destination_interface.up = true;
        destination_interface.delay =
            DynamicCast<QbbChannel>(destination_device->GetChannel())->GetDelay().GetTimeStep();
        destination_interface.bw = destination_device->GetDataRate().GetBitRate();

        char ip_string[16];
        sprintf(ip_string, "10.%d.%d.0", i / 254 + 1, i % 254 + 1);
        ipv4.SetBase(ip_string, "255.255.255.0");
        ipv4.Assign(devices);

        monitor.ConnectPfcTrace(source_device);
        monitor.ConnectPfcTrace(destination_device);
    }

    std::cout << "(AVG) NIC RATE: " << AverageNicRate() << std::endl;

    Ipv4Address empty_ip;
    for (uint32_t i = 0; i < state_.node_num; ++i) {
        if (state_.nodes.Get(i)->GetNodeType() != 0) {
            continue;
        }
        if (state_.server_addresses[i].IsEqual(empty_ip)) {
            printf("XXX ERROR %d\n", i);
            NS_FATAL_ERROR("An end-host belongs to no link");
        }
        Settings::hostId2IpMap[i] = state_.server_addresses[i].Get();
        Settings::hostIp2IdMap[state_.server_addresses[i].Get()] = i;
    }

    for (uint32_t i = 0; i < state_.node_num; ++i) {
        if (state_.nodes.Get(i)->GetNodeType() != 1) {
            continue;
        }
        Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(state_.nodes.Get(i));
        for (uint32_t j = 1; j < switch_node->GetNDevices(); ++j) {
            Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(switch_node->GetDevice(j));
            uint64_t rate = device->GetDataRate().GetBitRate();
            NS_ASSERT_MSG(config_.cc.rate_to_kmin.find(rate) != config_.cc.rate_to_kmin.end(),
                          "must set kmin for each link speed");
            NS_ASSERT_MSG(config_.cc.rate_to_kmax.find(rate) != config_.cc.rate_to_kmax.end(),
                          "must set kmax for each link speed");
            NS_ASSERT_MSG(config_.cc.rate_to_pmax.find(rate) != config_.cc.rate_to_pmax.end(),
                          "must set pmax for each link speed");
            switch_node->m_mmu->ConfigEcn(j, config_.cc.rate_to_kmin.at(rate),
                                          config_.cc.rate_to_kmax.at(rate),
                                          config_.cc.rate_to_pmax.at(rate));
            uint64_t delay =
                DynamicCast<QbbChannel>(device->GetChannel())->GetDelay().GetTimeStep();
            uint32_t headroom =
                rate * delay / 8 / 1000000000 * 2 + 2 * switch_node->m_mmu->MTU;
            switch_node->m_mmu->ConfigHdrm(j, headroom);
        }
        switch_node->m_mmu->ConfigNPort(switch_node->GetNDevices() - 1);
        switch_node->m_mmu->ConfigBufferSize(config_.buffer_size_mb * 1024 * 1024);
        switch_node->m_mmu->node_id = switch_node->GetId();
        NS_LOG_INFO("Node %u : Broadcom switch (%u ports / %gMB MMU)\n" %
                    (i, switch_node->GetNDevices() - 1,
                     switch_node->m_mmu->GetMmuBufferBytes() / 1000000.));
    }
    return true;
}

uint64_t TopologyManager::AverageNicRate() const {
    uint64_t total_nic_rate = 0;
    uint64_t server_count = 0;
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 0) {
            total_nic_rate += DynamicCast<QbbNetDevice>(state_.nodes.Get(i)->GetDevice(1))
                                  ->GetDataRate()
                                  .GetBitRate();
            ++server_count;
        }
    }
    NS_ASSERT_MSG(server_count > 0, "topology must contain at least one server");
    return total_nic_rate / server_count;
}

void TopologyManager::CalculateRoute(Ptr<Node> host) {
    std::vector<Ptr<Node>> queue;
    std::map<Ptr<Node>, int> distance;
    std::map<Ptr<Node>, uint64_t> delay;
    std::map<Ptr<Node>, uint64_t> tx_delay;
    std::map<Ptr<Node>, uint64_t> bandwidth;
    queue.push_back(host);
    distance[host] = 0;
    delay[host] = 0;
    tx_delay[host] = 0;
    bandwidth[host] = std::numeric_limits<uint64_t>::max();

    for (size_t i = 0; i < queue.size(); ++i) {
        Ptr<Node> current = queue[i];
        int current_distance = distance[current];
        for (const auto& neighbor : state_.neighbor_interfaces[current]) {
            if (!neighbor.second.up) {
                continue;
            }
            Ptr<Node> next = neighbor.first;
            if (distance.find(next) == distance.end()) {
                distance[next] = current_distance + 1;
                delay[next] = delay[current] + neighbor.second.delay;
                tx_delay[next] = tx_delay[current] +
                                 config_.packet_payload_size * 1000000000lu * 8 /
                                     neighbor.second.bw;
                bandwidth[next] = std::min(bandwidth[current], neighbor.second.bw);
                if (next->GetNodeType() == 1) {
                    queue.push_back(next);
                }
            }
            if (current_distance + 1 == distance[next]) {
                state_.next_hops[next][host].push_back(current);
            }
        }
    }
    for (const auto& item : delay) {
        state_.pair_delay[item.first][host] = item.second;
    }
    for (const auto& item : tx_delay) {
        state_.pair_tx_delay[item.first][host] = item.second;
    }
    for (const auto& item : bandwidth) {
        state_.pair_bw[item.first][host] = item.second;
    }
}

void TopologyManager::SetRoutingEntries() {
    for (const auto& node_entry : state_.next_hops) {
        Ptr<Node> node = node_entry.first;
        for (const auto& destination_entry : node_entry.second) {
            Ptr<Node> destination = destination_entry.first;
            Ipv4Address destination_address =
                destination->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
            for (const auto& next : destination_entry.second) {
                uint32_t interface = state_.neighbor_interfaces[node][next].idx;
                if (node->GetNodeType() == 1) {
                    DynamicCast<SwitchNode>(node)->AddTableEntry(destination_address, interface);
                } else {
                    node->GetObject<RdmaDriver>()->m_rdma->AddTableEntry(destination_address,
                                                                        interface);
                }
            }
        }
    }
}

void TopologyManager::CalculateRoutesAndMetrics() {
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 0) {
            CalculateRoute(state_.nodes.Get(i));
        }
    }
    SetRoutingEntries();

    state_.max_rtt = 0;
    state_.max_bdp = 0;
    fprintf(stderr, "node_num=%d\n", state_.node_num);
    for (uint32_t i = 0; i < state_.node_num; ++i) {
        if (state_.nodes.Get(i)->GetNodeType() != 0) {
            continue;
        }
        for (uint32_t j = i + 1; j < state_.node_num; ++j) {
            if (state_.nodes.Get(j)->GetNodeType() != 0) {
                continue;
            }
            uint64_t delay = state_.pair_delay[state_.nodes.Get(i)][state_.nodes.Get(j)];
            uint64_t tx_delay = state_.pair_tx_delay[state_.nodes.Get(i)][state_.nodes.Get(j)];
            uint64_t rtt = delay * 2 + tx_delay;
            uint64_t bandwidth = state_.pair_bw[state_.nodes.Get(i)][state_.nodes.Get(j)];
            uint64_t bdp = rtt * bandwidth / 1000000000 / 8;
            state_.pair_bdp[state_.nodes.Get(i)][state_.nodes.Get(j)] = bdp;
            state_.pair_bdp[state_.nodes.Get(j)][state_.nodes.Get(i)] = bdp;
            state_.pair_rtt[state_.nodes.Get(i)][state_.nodes.Get(j)] = rtt;
            state_.pair_rtt[state_.nodes.Get(j)][state_.nodes.Get(i)] = rtt;
            state_.max_bdp = std::max(state_.max_bdp, bdp);
            state_.max_rtt = std::max(state_.max_rtt, rtt);
        }
    }
    fprintf(stderr, "maxRtt: %lu, maxBdp: %lu\n", state_.max_rtt, state_.max_bdp);
    assert(state_.max_bdp == GetIrnBdp());
}

uint64_t TopologyManager::GetIrnBdp() const {
    static const std::map<std::string, uint32_t> topology_to_bdp = {
        {"leaf_spine_128_100G_OS2", 104000},
        {"leaf_spine_16_100G_OS2", 104000},
        {"fat_k8_100G_OS2", 156000},
    };
    for (const auto& item : topology_to_bdp) {
        if (config_.io.topology_file.find(item.first) != std::string::npos) {
            return item.second;
        }
    }
    std::cout << __FILE__ << "(" << __LINE__ << ") ERROR - topology BDP has no matched item with "
              << config_.io.topology_file << std::endl;
    assert(false);
    return 0;
}

void TopologyManager::TakeDownLink(uint32_t node_a, uint32_t node_b) {
    Ptr<Node> a = state_.nodes.Get(node_a);
    Ptr<Node> b = state_.nodes.Get(node_b);
    if (!state_.neighbor_interfaces[a][b].up) {
        return;
    }
    state_.neighbor_interfaces[a][b].up = false;
    state_.neighbor_interfaces[b][a].up = false;
    state_.next_hops.clear();
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 0) {
            CalculateRoute(state_.nodes.Get(i));
        }
    }
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 1) {
            DynamicCast<SwitchNode>(state_.nodes.Get(i))->ClearTable();
        } else {
            state_.nodes.Get(i)->GetObject<RdmaDriver>()->m_rdma->ClearTable();
        }
    }
    DynamicCast<QbbNetDevice>(a->GetDevice(state_.neighbor_interfaces[a][b].idx))->TakeDown();
    DynamicCast<QbbNetDevice>(b->GetDevice(state_.neighbor_interfaces[b][a].idx))->TakeDown();
    SetRoutingEntries();
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        if (state_.nodes.Get(i)->GetNodeType() == 0) {
            state_.nodes.Get(i)->GetObject<RdmaDriver>()->m_rdma->RedistributeQp();
        }
    }
}

void TopologyManager::DegradeLink(uint32_t node_a, uint32_t node_b, double fraction) {
    Ptr<Node> a = state_.nodes.Get(node_a);
    Ptr<Node> b = state_.nodes.Get(node_b);
    if (state_.neighbor_interfaces.find(a) == state_.neighbor_interfaces.end() ||
        state_.neighbor_interfaces[a].find(b) == state_.neighbor_interfaces[a].end()) {
        return;
    }
    uint64_t nominal_a = state_.neighbor_interfaces[a][b].bw;
    uint64_t nominal_b = state_.neighbor_interfaces[b][a].bw;
    Ptr<QbbNetDevice> device_a =
        DynamicCast<QbbNetDevice>(a->GetDevice(state_.neighbor_interfaces[a][b].idx));
    Ptr<QbbNetDevice> device_b =
        DynamicCast<QbbNetDevice>(b->GetDevice(state_.neighbor_interfaces[b][a].idx));
    device_a->SetDataRate(DataRate(static_cast<uint64_t>(nominal_a * fraction)));
    device_b->SetDataRate(DataRate(static_cast<uint64_t>(nominal_b * fraction)));
    std::cout << "*** LINK_DEGRADE @ " << Simulator::Now() << " : " << a->GetId() << "<->"
              << b->GetId() << " frac=" << fraction << " (rate="
              << static_cast<uint64_t>(nominal_a * fraction) << "bps)" << std::endl;
}

void TopologyManager::ScheduleLinkEvents() {
    if (config_.failures.link_down_time_us > 0) {
        Simulator::Schedule(Seconds(config_.flowgen_start_time) +
                                MicroSeconds(config_.failures.link_down_time_us),
                            &TopologyManager::TakeDownLink, this,
                            config_.failures.link_down_a, config_.failures.link_down_b);
    }
    for (const auto& event : config_.failures.link_degrade_events) {
        Simulator::Schedule(Seconds(config_.flowgen_start_time) + MicroSeconds(event.time_us),
                            &TopologyManager::DegradeLink, this, event.node_a, event.node_b,
                            event.fraction);
    }
}

void TopologyManager::BuildTorInterfaceMaps() {
    for (size_t tor_id = 0; tor_id < Settings::node_num; ++tor_id) {
        Ptr<Node> node = state_.nodes.Get(tor_id);
        if (node->GetNodeType() != 1) {
            continue;
        }
        Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(node);
        if (!switch_node->m_isToR) {
            continue;
        }
        for (const auto& neighbor : state_.neighbor_interfaces[node]) {
            if (neighbor.first->GetNodeType() == 1) {
                state_.tor_uplink_interfaces[tor_id].push_back(neighbor.second.idx);
            } else {
                state_.tor_downlink_interfaces[tor_id].push_back(neighbor.second.idx);
            }
        }
    }
}

}  // namespace nlb
