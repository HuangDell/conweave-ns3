#include "monitoring.h"

#include <ns3/capacity-proportional-routing.h>
#include <ns3/conga-routing.h>
#include <ns3/conweave-voq.h>
#include <ns3/custom-header.h>
#include <ns3/int-header.h>
#include <ns3/letflow-routing.h>
#include <ns3/settings.h>

#include <cassert>
#include <iostream>

namespace nlb {

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NetworkLoadBalanceMonitoring");

SimulationMonitor::SimulationMonitor(const ExperimentConfig& config, SimulationState& state)
    : config_(config), state_(state) {}

SimulationMonitor::~SimulationMonitor() { CloseOutputs(); }

bool SimulationMonitor::OpenCoreOutputs() {
    pfc_output_ = fopen(config_.io.pfc_output_file.c_str(), "w");
    fct_output_ = fopen(config_.io.fct_output_file.c_str(), "w");
    flow_input_output_ = fopen(config_.io.flow_input_file.c_str(), "w");
    if (config_.cc.mode == 1) {
        cnp_output_ = fopen(config_.io.cnp_output_file.c_str(), "w");
    }
    if (config_.traffic.v5_qp_pool && config_.traffic.v5_wqe_log) {
        v5_wqe_output_ = fopen(config_.io.v5_wqe_output_file.c_str(), "w");
        if (v5_wqe_output_) {
            fprintf(v5_wqe_output_,
                    "time_ns,event,app_id,chunk_id,qp_key,lane,bytes,commit_time_ns,"
                    "complete_time_ns,cumulative_end_seq,snd_nxt,snd_una\n");
        }
    }
    if (config_.traffic.v5_qp_pool && config_.traffic.v5_qp_state_log) {
        v5_qp_state_output_ = fopen(config_.io.v5_qp_state_output_file.c_str(), "w");
        if (v5_qp_state_output_) {
            fprintf(v5_qp_state_output_,
                    "time_ns,event,qp_key,src,dst,pg,lane,sport,dport,snd_nxt,snd_una,"
                    "receiver_next_expected_seq,dcqcn_rate_bps,dcqcn_target_rate_bps,"
                    "dcqcn_alpha,completed_wqes,expected_wqes\n");
        }
    }
    return pfc_output_ && fct_output_ && flow_input_output_ &&
           (config_.cc.mode != 1 || cnp_output_) &&
           (!config_.traffic.v5_qp_pool || !config_.traffic.v5_wqe_log || v5_wqe_output_) &&
           (!config_.traffic.v5_qp_pool || !config_.traffic.v5_qp_state_log ||
            v5_qp_state_output_);
}

bool SimulationMonitor::OpenLateOutputs() {
    if (config_.lb.mode == LbMode::kConweave) {
        voq_output_ = fopen(config_.io.voq_mon_file.c_str(), "w");
        voq_detail_output_ = fopen(config_.io.voq_mon_detail_file.c_str(), "w");
    }
    if (config_.lb.mode == LbMode::kSflowlet) {
        CapacityProportionalRouting::s_enableSwitchLog = config_.lb.sflowlet_switch_log != 0;
        if (config_.lb.sflowlet_ooo_log) {
            ooo_event_output_ = fopen(config_.io.ooo_event_output_file.c_str(), "w");
            RdmaHw::s_oooEventLog = ooo_event_output_;
        }
    }
    if (config_.traffic.v5_chunk_log && config_.traffic.v5_chunk_bytes > 0 &&
        config_.traffic.v5_nsub > 1) {
        v5_chunk_output_ = fopen(config_.io.v5_chunk_output_file.c_str(), "w");
    }

    uplink_output_ = fopen(config_.io.uplink_mon_file.c_str(), "w");
    conn_output_ = fopen(config_.io.conn_mon_file.c_str(), "w");
    path_delay_output_ = fopen(config_.io.path_delay_mon_file.c_str(), "w");

    return (config_.lb.mode != LbMode::kConweave || (voq_output_ && voq_detail_output_)) &&
           (!config_.lb.sflowlet_ooo_log || config_.lb.mode != LbMode::kSflowlet ||
            ooo_event_output_) &&
           (!config_.traffic.v5_chunk_log || config_.traffic.v5_chunk_bytes == 0 ||
            config_.traffic.v5_nsub <= 1 || v5_chunk_output_) &&
           uplink_output_ && conn_output_ && path_delay_output_;
}

void SimulationMonitor::CloseFile(FILE** file) {
    if (*file) {
        fclose(*file);
        *file = NULL;
    }
}

void SimulationMonitor::CloseOutputs() {
    if (RdmaHw::s_oooEventLog == ooo_event_output_) {
        RdmaHw::s_oooEventLog = NULL;
    }
    CloseFile(&pfc_output_);
    CloseFile(&fct_output_);
    CloseFile(&flow_input_output_);
    CloseFile(&cnp_output_);
    CloseFile(&voq_output_);
    CloseFile(&voq_detail_output_);
    CloseFile(&uplink_output_);
    CloseFile(&conn_output_);
    CloseFile(&path_delay_output_);
    CloseFile(&ooo_event_output_);
    CloseFile(&v5_chunk_output_);
    CloseFile(&v5_wqe_output_);
    CloseFile(&v5_qp_state_output_);
}

void SimulationMonitor::PfcTraceCallback(SimulationMonitor* monitor, Ptr<QbbNetDevice> device,
                                         uint32_t type) {
    monitor->RecordPfc(device, type);
}

void SimulationMonitor::QpCompleteCallback(SimulationMonitor* monitor,
                                            Ptr<RdmaQueuePair> queue_pair) {
    monitor->RecordQpFinish(queue_pair);
}

void SimulationMonitor::PersistentQpEventCallback(SimulationMonitor* monitor,
                                                   Ptr<RdmaQueuePair> queue_pair,
                                                   uint32_t event) {
    monitor->RecordPersistentQpEvent(queue_pair, event);
}

void SimulationMonitor::WqeEventCallback(SimulationMonitor* monitor,
                                          Ptr<RdmaQueuePair> queue_pair, uint32_t event,
                                          RdmaQueuePair::WqeBoundary boundary) {
    monitor->RecordWqeEvent(queue_pair, event, boundary);
}

void SimulationMonitor::ConnectPfcTrace(Ptr<QbbNetDevice> device) {
    device->TraceConnectWithoutContext(
        "QbbPfc", MakeBoundCallback(&SimulationMonitor::PfcTraceCallback, this, device));
}

void SimulationMonitor::ConnectQpComplete(Ptr<RdmaDriver> driver) {
    driver->TraceConnectWithoutContext(
        "QpComplete", MakeBoundCallback(&SimulationMonitor::QpCompleteCallback, this));
    driver->TraceConnectWithoutContext(
        "PersistentQpEvent",
        MakeBoundCallback(&SimulationMonitor::PersistentQpEventCallback, this));
    driver->TraceConnectWithoutContext(
        "WqeEvent", MakeBoundCallback(&SimulationMonitor::WqeEventCallback, this));
}

void SimulationMonitor::StartCnpMonitoring(Ptr<RdmaHw> hardware) {
    Simulator::Schedule(NanoSeconds(config_.monitoring.cnp_start_ns),
                        &SimulationMonitor::MonitorCnp, this, hardware);
}

void SimulationMonitor::StartPeriodicMonitoring() {
    Simulator::Schedule(Seconds(config_.flowgen_start_time),
                        &SimulationMonitor::MonitorPeriodic, this);
}

void SimulationMonitor::StartCompletionMonitoring(uint64_t target_completion_count) {
    target_completion_count_ = target_completion_count;
    Simulator::Schedule(Seconds(config_.flowgen_start_time),
                        &SimulationMonitor::MonitorCompletion, this);
}

void SimulationMonitor::ScheduleModeHistory() {
    if (config_.lb.mode == LbMode::kConga || config_.lb.mode == LbMode::kLetflow ||
        config_.lb.mode == LbMode::kConweave || config_.lb.mode == LbMode::kSflowlet) {
        Simulator::Schedule(Seconds(config_.flowgen_stop_time + config_.simulator_extra_time),
                            &SimulationMonitor::PrintModeHistory, this);
    }
}

void SimulationMonitor::RecordPfc(Ptr<QbbNetDevice> device, uint32_t type) {
    fprintf(pfc_output_, "%lu %u %u %u %u\n", Simulator::Now().GetTimeStep(),
            device->GetNode()->GetId(), device->GetNode()->GetNodeType(), device->GetIfIndex(),
            type);
}

void SimulationMonitor::RecordQpFinish(Ptr<RdmaQueuePair> queue_pair) {
    uint32_t source_id = Settings::ip_to_node_id(queue_pair->sip);
    uint32_t destination_id = Settings::ip_to_node_id(queue_pair->dip);
    Ptr<Node> destination = state_.nodes.Get(destination_id);
    Ptr<RdmaDriver> rdma = destination->GetObject<RdmaDriver>();
    rdma->m_rdma->DeleteRxQp(queue_pair->sip.Get(), queue_pair->sport, queue_pair->dport,
                             queue_pair->m_pg);

    if (queue_pair->m_persistent) {
        return;
    }

    uint64_t base_rtt = state_.pair_rtt[state_.nodes.Get(source_id)][state_.nodes.Get(destination_id)];
    uint64_t bandwidth = state_.pair_bw[state_.nodes.Get(source_id)][state_.nodes.Get(destination_id)];
    uint32_t total_bytes =
        queue_pair->m_size +
        ((queue_pair->m_size - 1) / config_.packet_payload_size + 1) *
            (CustomHeader::GetStaticWholeHeaderSize() - IntHeader::GetStaticSize());
    uint64_t standalone_fct = base_rtt + total_bytes * 8000000000lu / bandwidth;

    fprintf(fct_output_, "%u %u %u %u %lu %lu %lu %lu %d\n", source_id, destination_id,
            queue_pair->sport, queue_pair->dport, queue_pair->m_size,
            queue_pair->startTime.GetTimeStep(),
            (Simulator::Now() - queue_pair->startTime).GetTimeStep(), standalone_fct,
            queue_pair->m_flow_id);
    NS_LOG_DEBUG("%u %u %u %u %lu %lu %lu %lu\n" %
                 (source_id, destination_id, queue_pair->sport, queue_pair->dport,
                  queue_pair->m_size, queue_pair->startTime.GetTimeStep(),
                  (Simulator::Now() - queue_pair->startTime).GetTimeStep(), standalone_fct));
    Settings::cnt_finished_flows++;
    fflush(fct_output_);
}

void SimulationMonitor::RecordPersistentQpEvent(Ptr<RdmaQueuePair> queue_pair, uint32_t event) {
    static const char* names[] = {"create", "wake", "idle", "teardown", "packet", "ack"};
    NS_ASSERT_MSG(event <= RdmaHw::PERSISTENT_QP_ACK,
                  "unknown persistent QP lifecycle event");
    RecordQpState(queue_pair, names[event]);
}

void SimulationMonitor::RecordQpState(Ptr<RdmaQueuePair> queue_pair, const char* event) {
    if (!v5_qp_state_output_) {
        return;
    }
    uint32_t source_id = Settings::ip_to_node_id(queue_pair->sip);
    uint32_t destination_id = Settings::ip_to_node_id(queue_pair->dip);
    uint32_t receiver_expected = 0;
    Ptr<RdmaDriver> destination = state_.nodes.Get(destination_id)->GetObject<RdmaDriver>();
    Ptr<RdmaRxQueuePair> rx_qp = destination->m_rdma->GetRxQp(
        queue_pair->dip.Get(), queue_pair->sip.Get(), queue_pair->dport, queue_pair->sport,
        queue_pair->m_pg, false);
    if (rx_qp) {
        receiver_expected = rx_qp->ReceiverNextExpectedSeq;
    }
    uint64_t qp_key = RdmaHw::GetQpKey(queue_pair->dip.Get(), queue_pair->sport,
                                      queue_pair->dport, queue_pair->m_pg);
    fprintf(v5_qp_state_output_,
            "%lu,%s,%lu,%u,%u,%u,%u,%u,%u,%lu,%lu,%u,%lu,%lu,%.17g,%lu,%lu\n",
            Simulator::Now().GetNanoSeconds(), event, qp_key, source_id, destination_id,
            queue_pair->m_pg, queue_pair->m_v5_lane, queue_pair->sport, queue_pair->dport,
            queue_pair->snd_nxt, queue_pair->snd_una, receiver_expected,
            queue_pair->m_rate.GetBitRate(), queue_pair->mlx.m_targetRate.GetBitRate(),
            queue_pair->mlx.m_alpha, queue_pair->m_completed_wqes,
            queue_pair->m_expected_wqes);
    fflush(v5_qp_state_output_);
}

void SimulationMonitor::RecordWqeEvent(Ptr<RdmaQueuePair> queue_pair, uint32_t event,
                                       RdmaQueuePair::WqeBoundary boundary) {
    NS_ASSERT_MSG(event == RdmaHw::WQE_COMMIT || event == RdmaHw::WQE_COMPLETE,
                  "unknown persistent WQE event");
    uint64_t qp_key = RdmaHw::GetQpKey(queue_pair->dip.Get(), queue_pair->sport,
                                      queue_pair->dport, queue_pair->m_pg);
    uint64_t complete_time = event == RdmaHw::WQE_COMPLETE
                                 ? Simulator::Now().GetNanoSeconds()
                                 : 0;
    if (v5_wqe_output_) {
        fprintf(v5_wqe_output_, "%lu,%s,%u,%u,%lu,%u,%lu,%lu,%lu,%lu,%lu,%lu\n",
                Simulator::Now().GetNanoSeconds(),
                event == RdmaHw::WQE_COMMIT ? "commit" : "complete", boundary.app_id,
                boundary.chunk_id, qp_key, queue_pair->m_v5_lane, boundary.bytes,
                boundary.commit_time.GetNanoSeconds(), complete_time,
                boundary.cumulative_end_seq, queue_pair->snd_nxt, queue_pair->snd_una);
        fflush(v5_wqe_output_);
    }
    RecordQpState(queue_pair, event == RdmaHw::WQE_COMMIT ? "wqe_commit" : "wqe_complete");
    if (event != RdmaHw::WQE_COMPLETE) {
        return;
    }

    uint32_t source_id = Settings::ip_to_node_id(queue_pair->sip);
    uint32_t destination_id = Settings::ip_to_node_id(queue_pair->dip);
    uint64_t base_rtt = state_.pair_rtt[state_.nodes.Get(source_id)][state_.nodes.Get(destination_id)];
    uint64_t bandwidth = state_.pair_bw[state_.nodes.Get(source_id)][state_.nodes.Get(destination_id)];
    uint64_t total_bytes =
        boundary.bytes +
        ((boundary.bytes - 1) / config_.packet_payload_size + 1) *
            (CustomHeader::GetStaticWholeHeaderSize() - IntHeader::GetStaticSize());
    uint64_t standalone_fct = base_rtt + total_bytes * 8000000000lu / bandwidth;
    fprintf(fct_output_, "%u %u %u %u %lu %lu %lu %lu %u\n", source_id, destination_id,
            queue_pair->sport, queue_pair->dport, boundary.bytes,
            boundary.commit_time.GetTimeStep(),
            (Simulator::Now() - boundary.commit_time).GetTimeStep(), standalone_fct,
            boundary.app_id);
    Settings::cnt_finished_flows++;
    fflush(fct_output_);
}

void SimulationMonitor::MonitorCnp(Ptr<RdmaHw> hardware) {
    if (hardware->cnp_total > 0) {
        fprintf(cnp_output_, "%lu %u %u %u %u\n", Simulator::Now().GetNanoSeconds(),
                hardware->m_node->GetId(), hardware->cnp_by_ecn, hardware->cnp_by_ooo,
                hardware->cnp_total);
        fflush(cnp_output_);
        hardware->cnp_by_ecn = 0;
        hardware->cnp_by_ooo = 0;
        hardware->cnp_total = 0;
    }
    Simulator::Schedule(NanoSeconds(config_.monitoring.cnp_bucket_ns),
                        &SimulationMonitor::MonitorCnp, this, hardware);
}

void SimulationMonitor::MonitorPeriodic() {
    uint64_t now = Simulator::Now().GetNanoSeconds();
    for (const auto& tor_to_interfaces : state_.tor_uplink_interfaces) {
        Ptr<Node> node = state_.nodes.Get(tor_to_interfaces.first);
        Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(node);
        assert(switch_node->m_isToR);

        if (config_.lb.mode == LbMode::kConweave) {
            uint32_t voq_count = switch_node->m_mmu->m_conweaveRouting.GetNumVOQ();
            uint32_t voq_volume = switch_node->m_mmu->m_conweaveRouting.GetVolumeVOQ();
            fprintf(voq_output_, "%lu,%u,%u,%u\n", now, tor_to_interfaces.first, voq_count,
                    voq_volume);
            std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> destination_totals;
            for (auto voq : switch_node->m_mmu->m_conweaveRouting.GetVOQMap()) {
                std::pair<uint32_t, uint32_t>& totals = destination_totals[voq.second.getDIP()];
                totals.first += 1;
                totals.second += voq.second.getQueueSize();
            }
            for (const auto& item : destination_totals) {
                fprintf(voq_detail_output_, "%lu,%u,%u,%u\n", now, item.first,
                        item.second.first, item.second.second);
            }
        }

        for (const auto& interface : tor_to_interfaces.second) {
            uint64_t uplink_bytes = switch_node->GetTxBytesOutDev(interface);
            fprintf(uplink_output_, "%lu,%u,%u,%lu\n", now, tor_to_interfaces.first, interface,
                    uplink_bytes);

            Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(switch_node->GetDevice(interface));
            uint64_t queue_bytes = device ? device->GetQueue()->GetNBytesTotal() : 0;
            uint64_t rate_bps = device ? device->GetDataRate().GetBitRate() : 0;
            uint64_t tx_busy_ns = device ? device->GetTxBusyTimeNs() : 0;
            uint64_t tx_departed = device ? device->GetTxDepartedBytes() : 0;
            uint64_t residual_bps =
                config_.lb.mode == LbMode::kSflowlet
                    ? switch_node->m_mmu->m_residualEstimator.GetResidualCapacity(interface)
                    : 0;
            fprintf(path_delay_output_, "%lu,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu\n", now,
                    tor_to_interfaces.first, interface, queue_bytes, uplink_bytes, rate_bps,
                    tx_busy_ns, tx_departed, residual_bps);
        }
    }

    for (uint32_t i = 0; i < Settings::node_num; ++i) {
        if (state_.nodes.Get(i)->GetNodeType() != 0) {
            continue;
        }
        Ptr<RdmaHw> hardware = state_.nodes.Get(i)->GetObject<RdmaDriver>()->m_rdma;
        uint64_t qp_count = hardware->m_qpMap.size();
        uint64_t active_count = 0;
        for (const auto& item : hardware->m_qpMap) {
            if (item.second->GetBytesLeft() > 0) {
                ++active_count;
            }
        }
        fprintf(conn_output_, "%lu,%u,%lu,%lu\n", now, i, qp_count, active_count);
    }

    if (Simulator::Now() < Seconds(config_.flowgen_stop_time + 0.05)) {
        Simulator::Schedule(NanoSeconds(config_.monitoring.switch_interval_ns),
                            &SimulationMonitor::MonitorPeriodic, this);
    }
}

#if 0
// Legacy queue-distribution monitor. It remains disabled exactly as in the original driver.
struct QlenDistribution {
    std::vector<uint32_t> count;
    void Add(uint32_t queue_length) {
        uint32_t kb = queue_length / 1000;
        if (count.size() < kb + 1) {
            count.resize(kb + 1);
        }
        count[kb]++;
    }
};

void MonitorBuffer(FILE* output, SimulationState* state, uint64_t monitor_end,
                   uint64_t dump_interval, uint64_t monitor_interval) {
    static std::map<uint32_t, std::map<uint32_t, QlenDistribution>> results;
    for (uint32_t i = 0; i < state->nodes.GetN(); ++i) {
        if (state->nodes.Get(i)->GetNodeType() != 1) {
            continue;
        }
        Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(state->nodes.Get(i));
        for (uint32_t j = 1; j < switch_node->GetNDevices(); ++j) {
            uint32_t size = 0;
            for (uint32_t k = 0; k < SwitchMmu::qCnt; ++k) {
                size += switch_node->m_mmu->egress_bytes[j][k];
            }
            results[i][j].Add(size);
        }
    }
    if (Simulator::Now().GetTimeStep() % dump_interval == 0) {
        fprintf(output, "time: %lu\n", Simulator::Now().GetTimeStep());
        for (const auto& node : results) {
            for (const auto& interface : node.second) {
                fprintf(output, "%u %u", node.first, interface.first);
                for (const auto& value : interface.second.count) {
                    fprintf(output, " %u", value);
                }
                fprintf(output, "\n");
            }
        }
        fflush(output);
    }
    if (Simulator::Now().GetTimeStep() < monitor_end) {
        Simulator::Schedule(NanoSeconds(monitor_interval), &MonitorBuffer, output, state,
                            monitor_end, dump_interval, monitor_interval);
    }
}
#endif

void SimulationMonitor::MonitorCompletion() {
    if (Settings::cnt_finished_flows >= target_completion_count_) {
        std::cout << "\n*** Simulator is enforced to be finished, finished so far: "
                  << Settings::cnt_finished_flows << "/ total: " << target_completion_count_
                  << ", Time:" << Simulator::Now() << std::endl;
        PrintModeHistory();
        Simulator::Stop(NanoSeconds(1));
        return;
    }
    Simulator::Schedule(MicroSeconds(100), &SimulationMonitor::MonitorCompletion, this);
}

void SimulationMonitor::PrintModeHistory() {
    if (history_printed_) {
        return;
    }
    history_printed_ = true;
    if (config_.lb.mode == LbMode::kConga) {
        PrintCongaHistory();
    } else if (config_.lb.mode == LbMode::kLetflow) {
        PrintLetflowHistory();
    } else if (config_.lb.mode == LbMode::kConweave) {
        PrintConweaveHistory();
    } else if (config_.lb.mode == LbMode::kSflowlet) {
        PrintSflowletHistory();
    }
}

void SimulationMonitor::PrintCongaHistory() {
    std::cout << "\n------------CONGA History---------------" << std::endl;
    std::cout << "Number of flowlet's timeout:" << CongaRouting::nFlowletTimeout
              << "Conga's timeout: " << config_.lb.conga_flowlet_timeout << std::endl;
}

void SimulationMonitor::PrintLetflowHistory() {
    std::cout << "\n------------Letflow History---------------" << std::endl;
    std::cout << "Number of flowlet's timeout:" << LetflowRouting::nFlowletTimeout
              << "\nLetflow's timeout: " << config_.lb.letflow_flowlet_timeout << std::endl;
    CloseFile(&v5_chunk_output_);
}

void SimulationMonitor::PrintConweaveHistory() {
    std::cout << "\n------ConWeave parameters-----" << std::endl;
    std::cout << "Param - extraReplyDeadline:" << config_.lb.conweave_extra_reply_deadline
              << std::endl;
    std::cout << "Param - extraVOQFlushTime:" << config_.lb.conweave_extra_voq_flush_time
              << std::endl;
    std::cout << "Param - txExpiryTime:" << config_.lb.conweave_tx_expiry_time << std::endl;
    std::cout << "Param - defaultVOQWaitingTime:"
              << config_.lb.conweave_default_voq_waiting_time << std::endl;
    std::cout << "Param - pathPauseTime:" << config_.lb.conweave_path_pause_time << std::endl;
    std::cout << "Param - pathAwareRerouting:"
              << config_.lb.conweave_path_aware_rerouting << std::endl;

    std::cout << "\n------------ConWeave History---------------" << std::endl;
    std::cout << "Number of INIT's Reply sent (RTT_REPLY):" << ConWeaveRouting::m_nReplyInitSent
              << "\nNumber of Timely RTT_REPLY (INIT's Reply):"
              << ConWeaveRouting::m_nTimelyInitReplied
              << "\nNumber of TAIL's Reply Sent (CLEAR):" << ConWeaveRouting::m_nReplyTailSent
              << "\nNumber of Timely CLEAR (TAIL's Reply):" << ConWeaveRouting::m_nTimelyTailReplied
              << "\nNumber of NOTIFY Sent:" << ConWeaveRouting::m_nNotifySent
              << "\nNumber of Rerouting:" << ConWeaveRouting::m_nReRoute
              << "\nNumber of OoO enqueued pkts:" << ConWeaveRouting::m_nOutOfOrderPkts
              << "\nNumber of VOQ Flush Total:" << ConWeaveRouting::m_nFlushVOQTotal
              << "\nNumber of VOQ Flush From History:" << ConWeaveRouting::m_historyVOQSize.size()
              << "\nNumber of VOQ Flush by TAIL:" << ConWeaveRouting::m_nFlushVOQByTail
              << std::endl;
    std::cout << "--------------------------" << std::endl;

    for (size_t tor_id = 0; tor_id < Settings::node_num; ++tor_id) {
        Ptr<Node> node = state_.nodes.Get(tor_id);
        if (node->GetNodeType() != 1) {
            continue;
        }
        Ptr<SwitchNode> switch_node = DynamicCast<SwitchNode>(node);
        if (!switch_node->m_isToR) {
            continue;
        }
        uint32_t remaining = switch_node->m_mmu->m_conweaveRouting.GetNumVOQ();
        if (remaining > 0) {
            printf("*******************************\n");
            printf("*** WARNING - Tor Sw (%lu) - VOQ (num=%u) is not flushed yet!! ***\n",
                   tor_id, remaining);
            printf(" -- Probably the history print is too early so simulation might not be finished?");
            printf("********************************\n");
        }
    }
}

void SimulationMonitor::PrintSflowletHistory() {
    std::cout << "\n---------sflowlet History---------" << std::endl;
    std::cout << "Flowlet timeouts: " << CapacityProportionalRouting::nFlowletTimeout
              << "\nFlowlet timeout value: " << config_.lb.sflowlet_flowlet_timeout
              << "\nPath switches logged: " << CapacityProportionalRouting::s_switchLog.size()
              << std::endl;

    if (CapacityProportionalRouting::s_enableSwitchLog &&
        !CapacityProportionalRouting::s_switchLog.empty()) {
        FILE* switch_output = fopen(config_.io.flowlet_switch_output_file.c_str(), "w");
        if (switch_output) {
            for (const auto& event : CapacityProportionalRouting::s_switchLog) {
                fprintf(switch_output, "%lu %u %lu %u %u %.0f %.0f\n", event.time_ns,
                        event.switch_id, static_cast<unsigned long>(event.qpkey), event.old_path,
                        event.new_path, event.old_weight, event.new_weight);
            }
            fclose(switch_output);
        }
    }
    CloseFile(&ooo_event_output_);
    RdmaHw::s_oooEventLog = NULL;
    CloseFile(&v5_chunk_output_);
}

}  // namespace nlb
