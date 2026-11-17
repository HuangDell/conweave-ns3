#include "host-resource-lb.h"

#include <ns3/custom-header.h>
#include <ns3/rdma-hw.h>
#include <ns3/settings.h>
#include <ns3/simulator.h>
#include <ns3/switch-node.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace nlb {

using namespace ns3;

HostResourceLb::HostResourceLb(const ExperimentConfig& config, SimulationState& state)
    : config_(config), state_(state) {}

HostResourceLb::~HostResourceLb() { Close(); }

bool HostResourceLb::LoadTupleSpec() {
    if (!IsEnabled()) return true;
    if (config_.io.hrlb_tuple_file.empty()) {
        std::cerr << "Error: HRLB_ENABLE=1 requires HRLB_TUPLE_FILE\n";
        return false;
    }
    std::ifstream in(config_.io.hrlb_tuple_file.c_str());
    if (!in.is_open()) {
        std::cerr << "Error: cannot open HRLB_TUPLE_FILE " << config_.io.hrlb_tuple_file << '\n';
        return false;
    }
    uint32_t n_records = 0;
    if (!(in >> n_records) || n_records == 0) {
        std::cerr << "Error: malformed HRLB_TUPLE_FILE header\n";
        return false;
    }
    bg_specs_.clear();
    target_flow_to_msg_.clear();
    tg_specs_by_msg_.clear();

    for (uint32_t i = 0; i < n_records; ++i) {
        std::string kind;
        if (!(in >> kind)) {
            std::cerr << "Error: truncated HRLB_TUPLE_FILE at record " << i << '\n';
            return false;
        }
        if (kind == "BG") {
            BgTupleSpec spec;
            uint32_t nhops = 0;
            if (!(in >> spec.flow_id >> spec.src >> spec.dst >> spec.pg >> spec.sport >>
                  spec.dport >> nhops) ||
                nhops < 3) {
                std::cerr << "Error: malformed BG tuple record\n";
                return false;
            }
            spec.expected_hops.resize(nhops);
            for (uint32_t h = 0; h < nhops; ++h) {
                if (!(in >> spec.expected_hops[h])) return false;
            }
            if (!bg_specs_.emplace(spec.flow_id, spec).second) {
                std::cerr << "Error: duplicate BG flow_id " << spec.flow_id << '\n';
                return false;
            }
        } else if (kind == "TG") {
            TgTupleSpec spec;
            uint32_t nhops = 0;
            if (!(in >> spec.flow_id >> spec.message_id >> spec.lane >> spec.src >> spec.dst >>
                  spec.pg >> spec.sport >> spec.dport >> spec.path_id >> nhops) ||
                nhops < 3 || spec.lane >= config_.hrlb.k) {
                std::cerr << "Error: malformed TG tuple record\n";
                return false;
            }
            spec.expected_hops.resize(nhops);
            for (uint32_t h = 0; h < nhops; ++h) {
                if (!(in >> spec.expected_hops[h])) return false;
            }
            auto it = target_flow_to_msg_.find(spec.flow_id);
            if (it != target_flow_to_msg_.end() && it->second != spec.message_id) {
                std::cerr << "Error: inconsistent message_id for flow_id " << spec.flow_id << '\n';
                return false;
            }
            target_flow_to_msg_[spec.flow_id] = spec.message_id;
            tg_specs_by_msg_[spec.message_id].push_back(spec);
        } else {
            std::cerr << "Error: unknown tuple record type " << kind << '\n';
            return false;
        }
    }
    for (auto& kv : tg_specs_by_msg_) {
        auto& vec = kv.second;
        std::sort(vec.begin(), vec.end(),
                  [](const TgTupleSpec& a, const TgTupleSpec& b) { return a.lane < b.lane; });
        if (vec.size() != config_.hrlb.k) {
            std::cerr << "Error: message " << kv.first << " does not have K=" << config_.hrlb.k
                      << " lanes\n";
            return false;
        }
        for (uint32_t l = 0; l < config_.hrlb.k; ++l) {
            if (vec[l].lane != l || vec[l].flow_id != vec[0].flow_id ||
                vec[l].src != vec[0].src || vec[l].dst != vec[0].dst || vec[l].pg != vec[0].pg) {
                std::cerr << "Error: invalid lane specs for message " << kv.first << '\n';
                return false;
            }
        }
    }
    return true;
}

void HostResourceLb::VerifyTuplePath(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                                     uint16_t pg, const std::vector<uint32_t>& expected_hops,
                                     std::vector<RdmaHw::OdinResourceKey>* out_resources,
                                     ExpectedFlowPath* out_flow_path) const {
    NS_ABORT_MSG_IF(expected_hops.size() < 3 || expected_hops.front() != src ||
                        expected_hops.back() != dst,
                    "invalid expected hop sequence endpoints");
    uint32_t sip = state_.server_addresses[src].Get();
    uint32_t dip = state_.server_addresses[dst].Get();
    NS_ABORT_MSG_IF(state_.nodes.Get(src)->GetNodeType() != 0 ||
                        state_.nodes.Get(dst)->GetNodeType() != 0,
                    "path endpoints must be hosts");
    Ptr<Node> first_sw = state_.nodes.Get(expected_hops[1]);
    NS_ABORT_MSG_IF(first_sw->GetNodeType() != 1 ||
                        state_.neighbor_interfaces.at(state_.nodes.Get(src)).find(first_sw) ==
                            state_.neighbor_interfaces.at(state_.nodes.Get(src)).end(),
                    "first hop is not an attached switch");

    for (size_t h = 1; h + 1 < expected_hops.size(); ++h) {
        uint32_t sw_id = expected_hops[h];
        uint32_t next_id = expected_hops[h + 1];
        Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(state_.nodes.Get(sw_id));
        Ptr<Node> next_node = state_.nodes.Get(next_id);
        NS_ABORT_MSG_IF(!sw || sw->GetNodeType() != 1, "expected switch hop is not a switch");
        auto it = state_.neighbor_interfaces.at(state_.nodes.Get(sw_id)).find(next_node);
        NS_ABORT_MSG_IF(it == state_.neighbor_interfaces.at(state_.nodes.Get(sw_id)).end() ||
                            !it->second.up,
                        "expected link is missing or down");
        uint32_t expected_port = it->second.idx;
        int actual_port = sw->PredictEcmpOutDev(sip, dip, sport, dport);
        NS_ABORT_MSG_IF(actual_port < 0 || static_cast<uint32_t>(actual_port) != expected_port,
                        "frozen tuple ECMP hop verification failed: switch "
                            << sw_id << " predicted port " << actual_port << " != expected "
                            << expected_port << " (sport=" << sport << ", dport=" << dport << ")");
        if (out_resources) {
            RdmaHw::OdinResourceKey resource;
            resource.switch_id = sw_id;
            resource.interface = expected_port;
            out_resources->push_back(resource);
        }
        if (out_flow_path) {
            out_flow_path->allowed_switch_ports.insert(std::make_pair(sw_id, expected_port));
        }
    }
    (void)pg;
}

bool HostResourceLb::Initialize() {
    if (!IsEnabled()) return true;
    NS_ABORT_MSG_IF(config_.hrlb.coord_tail_mode != "off" &&
                        config_.hrlb.coord_tail_mode != "all" &&
                        config_.hrlb.coord_tail_mode != "drain", "invalid tail mode");
    NS_ABORT_MSG_IF(config_.hrlb.coord_tail_chunks == 0, "tail chunk bound must be positive");
    monitored_resources_.clear();
    resource_hosts_.clear();
    expected_flow_paths_.clear();
    connected_hosts_.clear();
    std::set<uint32_t> host_set;

    for (const auto& kv : bg_specs_) {
        const BgTupleSpec& bg = kv.second;
        ExpectedFlowPath flow_path;
        flow_path.is_target = false;
        std::vector<RdmaHw::OdinResourceKey> bg_resources;
        VerifyTuplePath(bg.src, bg.dst, bg.sport, bg.dport, static_cast<uint16_t>(bg.pg),
                        bg.expected_hops,
                        config_.hrlb.bg_resource_feedback ? &bg_resources : nullptr,
                        &flow_path);
        if (config_.hrlb.bg_resource_feedback) {
            for (const auto& res : bg_resources) {
                monitored_resources_.insert(res);
            }
            Ptr<RdmaDriver> bg_driver = state_.nodes.Get(bg.src)->GetObject<RdmaDriver>();
            uint64_t bg_qp_key = RdmaHw::GetQpKey(
                state_.server_addresses[bg.dst].Get(), bg.sport, bg.dport,
                static_cast<uint16_t>(bg.pg));
            bg_driver->m_rdma->RegisterOdinResourcePath(
                bg_qp_key, 0, config_.hrlb.mapping_version, bg_resources);
            host_set.insert(bg.src);
        }
        FiveTupleKey key(state_.server_addresses[bg.src].Get(),
                         state_.server_addresses[bg.dst].Get(), bg.sport, bg.dport,
                         static_cast<uint16_t>(bg.pg));
        NS_ABORT_MSG_IF(!expected_flow_paths_.emplace(key, flow_path).second,
                        "duplicate frozen 5-tuple");
    }

    for (auto& kv : tg_specs_by_msg_) {
        for (TgTupleSpec& tg : kv.second) {
            ExpectedFlowPath flow_path;
            flow_path.is_target = true;
            tg.resources.clear();
            VerifyTuplePath(tg.src, tg.dst, tg.sport, tg.dport, static_cast<uint16_t>(tg.pg),
                            tg.expected_hops, &tg.resources, &flow_path);
            for (const auto& res : tg.resources) {
                monitored_resources_.insert(res);
            }
            host_set.insert(tg.src);
            FiveTupleKey key(state_.server_addresses[tg.src].Get(),
                             state_.server_addresses[tg.dst].Get(), tg.sport, tg.dport,
                             static_cast<uint16_t>(tg.pg));
            NS_ABORT_MSG_IF(!expected_flow_paths_.emplace(key, flow_path).second,
                            "duplicate frozen target 5-tuple");

            Ptr<RdmaDriver> driver = state_.nodes.Get(tg.src)->GetObject<RdmaDriver>();
            uint64_t qp_key = RdmaHw::GetQpKey(state_.server_addresses[tg.dst].Get(), tg.sport,
                                               tg.dport, static_cast<uint16_t>(tg.pg));
            driver->m_rdma->RegisterOdinResourcePath(qp_key, tg.path_id,
                                                     config_.hrlb.mapping_version, tg.resources);
            if (connected_hosts_.insert(tg.src).second) {
                driver->TraceConnectWithoutContext(
                    "WqeEvent", MakeBoundCallback(&HostResourceLb::WqeEventCallback, this));
                driver->m_rdma->m_odinResourceDeliveredCallback =
                    MakeBoundCallback(&HostResourceLb::ResourceDeliveredCallback, this, tg.src);
            }
        }
    }
    resource_hosts_.assign(host_set.begin(), host_set.end());

    // Connect runtime hop verification to all switch egress devices.
    for (uint32_t i = 0; i < state_.nodes.GetN(); ++i) {
        Ptr<Node> node = state_.nodes.Get(i);
        if (node->GetNodeType() != 1) continue;
        for (uint32_t j = 1; j < node->GetNDevices(); ++j) {
            Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(node->GetDevice(j));
            if (dev) {
                dev->TraceConnectWithoutContext(
                    "PhyTxBegin", MakeBoundCallback(&HostResourceLb::SwitchTxCallback, this, dev));
            }
        }
    }

    message_output_ = fopen(config_.io.hrlb_message_output_file.c_str(), "w");
    if (!message_output_) return false;
    fprintf(message_output_,
            "message_id,flow_id,src,dst,pg,bytes,total_chunks,start_ns,finish_ns,completion_ns\n");
    fflush(message_output_);

    if (config_.hrlb.decision_log) {
        decision_output_ = fopen(config_.io.hrlb_decision_output_file.c_str(), "w");
        if (!decision_output_) return false;
        fprintf(decision_output_,
                "time_ns,schedule_seq,epoch,trigger,event,message_id,chunk_id,offset,bytes,"
                "nominal_lane,chosen_lane,is_redirect,source_fresh,source_score,target_fresh,"
                "target_score,lane_scores,lane_fresh,resource_versions,resource_ages_ns,"
                "slots_before,slots_after,cursor_before,cursor_after,reason");
        if (config_.hrlb.audit_log) {
            fprintf(decision_output_, ",src,pending_chunks,completed_chunks,lane_slots,lane_rates_bps,"
                                      "lane_unsent_bytes,lane_inflight_bytes,lane_next_avail_ns");
        }
        fprintf(decision_output_, "\n");
        fflush(decision_output_);
    }
    if (config_.hrlb.audit_log && config_.hrlb.decision_log) {
        lane_output_ = fopen((config_.io.hrlb_decision_output_file + ".lanes.csv").c_str(), "w");
        if (!lane_output_) return false;
        fprintf(lane_output_, "time_ns,src,message_id,lane,pending_chunks,nominal_pending_chunks,slots,unsent_bytes,"
                              "inflight_bytes,rate_bps,next_avail_ns,window_bound,paused,"
                              "nic_busy,nic_busy_ns\n");
        tail_output_ = fopen((config_.io.hrlb_decision_output_file + ".tail.csv").c_str(), "w");
        if (!tail_output_) return false;
        fprintf(tail_output_, "time_ns,event,schedule_seq,message_id,chunk_id,pending_chunks,"
                             "committed_unsent_bytes,inflight_bytes,nominal_lane,chosen_lane,fast_lane_mask\n");
    }
    return true;
}

void HostResourceLb::Close() {
    if (lane_sample_event_.IsRunning()) lane_sample_event_.Cancel();
    if (lane_output_) { fclose(lane_output_); lane_output_ = NULL; }
    if (tail_output_) { fclose(tail_output_); tail_output_ = NULL; }
    if (decision_output_) {
        fclose(decision_output_);
        decision_output_ = NULL;
    }
    if (message_output_) {
        fclose(message_output_);
        message_output_ = NULL;
    }
}

bool HostResourceLb::IsTargetFlow(uint32_t flow_id) const {
    return IsEnabled() && target_flow_to_msg_.count(flow_id) > 0;
}

bool HostResourceLb::GetBackgroundTuple(uint32_t flow_id, uint16_t* sport, uint16_t* dport) const {
    auto it = bg_specs_.find(flow_id);
    if (it == bg_specs_.end()) return false;
    *sport = it->second.sport;
    *dport = it->second.dport;
    return true;
}

uint64_t HostResourceLb::TargetChunkCount(uint32_t bytes) const {
    return (static_cast<uint64_t>(bytes) + config_.hrlb.chunk_bytes - 1) / config_.hrlb.chunk_bytes;
}

void HostResourceLb::SwitchTxCallback(HostResourceLb* self, Ptr<QbbNetDevice> device,
                                      Ptr<const Packet> packet) {
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(ch);
    if (ch.l3Prot != 0x11) return;
    FiveTupleKey key(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport, ch.udp.pg);
    auto it = self->expected_flow_paths_.find(key);
    NS_ABORT_MSG_IF(it == self->expected_flow_paths_.end(),
                    "unregistered data packet tuple observed at switch");
    std::pair<uint32_t, uint32_t> sw_port(device->GetNode()->GetId(), device->GetIfIndex());
    NS_ABORT_MSG_IF(!it->second.allowed_switch_ports.count(sw_port),
                    "data packet traversed unexpected switch/port: sw="
                        << sw_port.first << " port=" << sw_port.second
                        << " sport=" << ch.udp.sport << " dport=" << ch.udp.dport);
}

void HostResourceLb::AddPendingTargetMessage(uint32_t flow_id, uint32_t src, uint32_t dst,
                                             uint32_t pg, uint64_t bytes, double start_s) {
    auto map_it = target_flow_to_msg_.find(flow_id);
    NS_ABORT_MSG_IF(map_it == target_flow_to_msg_.end(), "missing target message mapping");
    uint32_t msg_id = map_it->second;
    const auto& lane_specs = tg_specs_by_msg_.at(msg_id);
    NS_ABORT_MSG_IF(lane_specs[0].src != src || lane_specs[0].dst != dst || lane_specs[0].pg != pg,
                    "target flow endpoints mismatch tuple spec");

    MessageState msg;
    msg.message_id = msg_id;
    msg.flow_id = flow_id;
    msg.src = src;
    msg.dst = dst;
    msg.pg = pg;
    msg.total_bytes = bytes;
    msg.start_ns = Simulator::Now().GetNanoSeconds();
    (void)start_s;
    msg.target_cursor = msg_id % config_.hrlb.k;

    uint64_t offset = 0;
    uint32_t chunk_id = 0;
    while (offset < bytes) {
        uint32_t len = static_cast<uint32_t>(
            std::min<uint64_t>(config_.hrlb.chunk_bytes, bytes - offset));
        PendingChunk chunk;
        chunk.chunk_id = chunk_id;
        chunk.offset = offset;
        chunk.bytes = len;
        msg.pending_chunks.push_back(chunk);
        offset += len;
        ++chunk_id;
    }
    msg.total_chunks = chunk_id;
    NS_ABORT_MSG_IF(msg.total_chunks < config_.hrlb.k,
                    "target message has fewer chunks than K bootstrap lanes");

    msg.lanes.resize(config_.hrlb.k);
    std::map<RdmaHw::OdinResourceKey, uint32_t> res_freq;
    for (uint32_t l = 0; l < config_.hrlb.k; ++l) {
        msg.lanes[l].spec = lane_specs[l];
        msg.lanes[l].qp_key = RdmaHw::GetQpKey(state_.server_addresses[dst].Get(),
                                               lane_specs[l].sport, lane_specs[l].dport,
                                               static_cast<uint16_t>(pg));
        for (const auto& res : lane_specs[l].resources) {
            res_freq[res] += 1;
        }
    }
    for (uint32_t l = 0; l < config_.hrlb.k; ++l) {
        msg.lanes[l].div_resources.clear();
        for (const auto& res : lane_specs[l].resources) {
            if (res_freq[res] < config_.hrlb.k) {
                msg.lanes[l].div_resources.push_back(res);
            }
        }
        if (msg.lanes[l].div_resources.empty()) {
            msg.lanes[l].div_resources = lane_specs[l].resources;
        }
    }

    NS_ABORT_MSG_IF(!messages_.emplace(msg_id, std::move(msg)).second, "duplicate message_id");
    pending_start_msgs_.push_back(msg_id);
}

void HostResourceLb::StartAddedMessages() {
    if (pending_start_msgs_.empty()) return;
    std::sort(pending_start_msgs_.begin(), pending_start_msgs_.end());
    std::map<uint32_t, std::vector<Ptr<RdmaQueuePair>>> created_by_host;

    for (uint32_t msg_id : pending_start_msgs_) {
        MessageState& msg = messages_.at(msg_id);
        active_message_ids_.push_back(msg_id);
        uint32_t window = config_.cc.has_win
                              ? (config_.cc.global_t == 1
                                     ? state_.max_bdp
                                     : state_.pair_bdp[state_.nodes.Get(msg.src)]
                                                      [state_.nodes.Get(msg.dst)])
                              : 0;
        uint64_t base_rtt = config_.cc.global_t == 1
                                ? state_.max_rtt
                                : state_.pair_rtt[state_.nodes.Get(msg.src)]
                                                 [state_.nodes.Get(msg.dst)];
        Ptr<RdmaDriver> driver = state_.nodes.Get(msg.src)->GetObject<RdmaDriver>();

        std::vector<bool> fresh(config_.hrlb.k, false);
        std::vector<double> scores(config_.hrlb.k, 0.0);
        std::vector<uint64_t> versions(config_.hrlb.k, 0);
        std::vector<uint64_t> ages(config_.hrlb.k, 0);
        for (uint32_t l = 0; l < config_.hrlb.k; ++l) {
            bool tbo = false;
            const char* r = nullptr;
            fresh[l] = driver->m_rdma->QueryOdinPathScore(
                msg.lanes[l].qp_key, base_rtt, &scores[l], &tbo, &ages[l], &versions[l], nullptr,
                &r);
        }

        // Submit first K real chunks, one per lane, without notifying NIC yet.
        for (uint32_t c = 0; c < config_.hrlb.k; ++c) {
            decision_state_ = DecisionState(msg);
            PendingChunk chunk = msg.pending_chunks.front();
            msg.pending_chunks.pop_front();
            uint32_t lane = (msg.message_id + chunk.chunk_id) % config_.hrlb.k;
            LaneState& ls = msg.lanes[lane];
            uint32_t slots_before = ls.outstanding_wqes;
            Ptr<RdmaQueuePair> qp = driver->AppendPersistentWqe(
                static_cast<uint16_t>(msg.pg), state_.server_addresses[msg.src],
                state_.server_addresses[msg.dst], ls.spec.sport, ls.spec.dport, window, base_rtt,
                static_cast<int32_t>(msg.message_id), chunk.chunk_id, chunk.bytes, lane,
                RdmaQueuePair::V5_DATA, false);
            ls.qp = qp;
            ls.outstanding_wqes += 1;
            ls.scheduled_wqes += 1;
            created_by_host[msg.src].push_back(qp);
            LogDecision(0, "message_start", "bootstrap", msg, chunk, lane, lane, false, fresh,
                        scores, versions, ages, slots_before, ls.outstanding_wqes,
                        msg.target_cursor, msg.target_cursor, "bootstrap");
        }
        if (msg.pending_chunks.empty() && !msg.sealed) {
            SealMessage(msg);
        }
    }
    std::sort(active_message_ids_.begin(), active_message_ids_.end());
    pending_start_msgs_.clear();

    for (const auto& kv : created_by_host) {
        Ptr<RdmaDriver> driver = state_.nodes.Get(kv.first)->GetObject<RdmaDriver>();
        driver->NotifyPersistentQps(kv.second);
    }

    for (const auto& kv : created_by_host) RunSchedule("message_start", kv.first);
    if (lane_output_ && !lane_sample_event_.IsRunning()) SampleLanes();
}

void HostResourceLb::WqeEventCallback(HostResourceLb* self, Ptr<RdmaQueuePair> qp, uint32_t event,
                                      RdmaQueuePair::WqeBoundary boundary) {
    if (event != RdmaHw::WQE_COMPLETE || qp->m_v5_traffic_class != RdmaQueuePair::V5_DATA) {
        return;
    }
    self->OnWqeComplete(qp, boundary);
}

void HostResourceLb::OnWqeComplete(Ptr<RdmaQueuePair> qp, RdmaQueuePair::WqeBoundary boundary) {
    auto it = messages_.find(boundary.app_id);
    if (it == messages_.end()) return;
    MessageState& msg = it->second;
    uint32_t lane = qp->m_v5_lane;
    NS_ABORT_MSG_IF(lane >= msg.lanes.size() || msg.lanes[lane].outstanding_wqes == 0,
                    "WQE completion underflow");
    msg.lanes[lane].outstanding_wqes -= 1;
    msg.completed_chunks += 1;
    msg.completed_bytes += boundary.bytes;
    if (msg.completed_chunks == msg.total_chunks) {
        NS_ABORT_MSG_IF(msg.completed_bytes != msg.total_bytes,
                        "message completed byte count mismatch");
        msg.finish_ns = Simulator::Now().GetNanoSeconds();
        if (message_output_) {
            fprintf(message_output_, "%u,%u,%u,%u,%u,%lu,%u,%lu,%lu,%lu\n",
                    msg.message_id, msg.flow_id, msg.src, msg.dst, msg.pg, msg.total_bytes,
                    msg.total_chunks, msg.start_ns, msg.finish_ns, msg.finish_ns - msg.start_ns);
            fflush(message_output_);
        }
    }
    if (post_ack_hosts_.insert(msg.src).second) {
        Simulator::ScheduleNow(&HostResourceLb::OnPostAck, this, msg.src);
    }
}

void HostResourceLb::OnPostAck(uint32_t host) {
    post_ack_hosts_.erase(host);
    RunSchedule("wqe_complete", host);
}

void HostResourceLb::ResourceDeliveredCallback(HostResourceLb* self, uint32_t host, uint64_t epoch) {
    self->OnResourceFrameDelivered(host, epoch);
}

void HostResourceLb::OnResourceFrameDelivered(uint32_t host, uint64_t epoch) {
    delivered_epochs_[host] = epoch;
    RunSchedule("resource_frame", host);
}

void HostResourceLb::RunSchedule(const char* trigger, uint32_t host) {
    std::vector<uint32_t> ids;
    for (uint32_t mid : active_message_ids_) {
        if (messages_.at(mid).src == host) ids.push_back(mid);
    }
    if (ids.empty()) return;
    uint64_t seq = ++schedule_seq_;
    bool any_progress = true;
    size_t& rr_cursor = msg_rr_cursors_[host];
    while (any_progress) {
        any_progress = false;
        size_t m = ids.size();
        size_t start = rr_cursor % m;
        size_t next_cursor = rr_cursor;
        for (size_t step = 0; step < m; ++step) {
            size_t idx = (start + step) % m;
            MessageState& msg = messages_.at(ids[idx]);
            if (msg.pending_chunks.empty() || msg.held_schedule_seq == seq) continue;
            if (TryCommitOneChunk(msg, trigger, seq)) {
                any_progress = true;
                next_cursor = (idx + 1) % m;
                if (msg.pending_chunks.empty() && !msg.sealed) SealMessage(msg);
            }
        }
        rr_cursor = next_cursor;
    }
}

bool HostResourceLb::TryCommitOneChunk(MessageState& msg, const char* trigger, uint64_t seq) {
    uint32_t k = config_.hrlb.k;
    uint32_t d = config_.hrlb.depth;
    Ptr<RdmaDriver> driver = state_.nodes.Get(msg.src)->GetObject<RdmaDriver>();
    uint64_t base_rtt = config_.cc.global_t == 1
                            ? state_.max_rtt
                            : state_.pair_rtt[state_.nodes.Get(msg.src)][state_.nodes.Get(msg.dst)];
    uint32_t window = config_.cc.has_win
                          ? (config_.cc.global_t == 1
                                 ? state_.max_bdp
                                 : state_.pair_bdp[state_.nodes.Get(msg.src)]
                                                  [state_.nodes.Get(msg.dst)])
                          : 0;

    bool redirect_active = (config_.hrlb.policy == "redirect") && (config_.hrlb.redirect != 0);
    bool coord_active = (config_.hrlb.policy == "coord") && (config_.hrlb.redirect != 0);

    std::vector<bool> fresh(k, false);
    std::vector<double> scores(k, 0.0);
    std::vector<uint64_t> versions(k, 0);
    std::vector<uint64_t> ages(k, 0);
    std::vector<uint64_t> backlog_bytes(k, 0);
    std::vector<uint64_t> rate_bps(k, 1);
    for (uint32_t l = 0; l < k; ++l) {
        bool tbo = false;
        const char* r = nullptr;
        if (coord_active) {
            fresh[l] = driver->m_rdma->QueryOdinResourceSetScore(
                msg.lanes[l].div_resources, config_.hrlb.mapping_version, base_rtt, &scores[l],
                &tbo, &ages[l], &versions[l], nullptr, &r);
        } else {
            fresh[l] = driver->m_rdma->QueryOdinPathScore(msg.lanes[l].qp_key, base_rtt,
                                                          &scores[l], &tbo, &ages[l], &versions[l],
                                                          nullptr, &r);
        }
        if (msg.lanes[l].qp) {
            backlog_bytes[l] =
                msg.lanes[l].qp->GetBytesLeft() + msg.lanes[l].qp->GetOnTheFly();
            rate_bps[l] = std::max<uint64_t>(1, msg.lanes[l].qp->m_rate.GetBitRate());
        }
    }

    decision_state_ = DecisionState(msg);
    bool logged_tail_hold = false;
    for (auto it = msg.pending_chunks.begin(); it != msg.pending_chunks.end(); ++it) {
        uint32_t nominal = (msg.message_id + it->chunk_id) % k;
        uint32_t chosen = nominal;
        bool is_redirect = false;
        bool hold_tail = false;
        const char* reason = "nominal";

        if (coord_active) {
            if (!fresh[nominal]) {
                reason = "source_unknown_fallback";
            } else {
                std::vector<double> t_fin(k, 0.0);
                double min_all_div = std::numeric_limits<double>::infinity();
                double min_all_t = std::numeric_limits<double>::infinity();
                for (uint32_t l = 0; l < k; ++l) {
                    t_fin[l] = static_cast<double>(backlog_bytes[l] + it->bytes) * 8.0 /
                               static_cast<double>(rate_bps[l]);
                    if (fresh[l]) {
                        min_all_div = std::min(min_all_div, scores[l]);
                        min_all_t = std::min(min_all_t, t_fin[l]);
                    }
                }
                bool nom_has_slot = (msg.lanes[nominal].outstanding_wqes < d);
                bool nom_div_ok = (scores[nominal] <= min_all_div + config_.hrlb.coord_delta_psi);
                bool nom_t_ok = (t_fin[nominal] <= (1.0 + config_.hrlb.coord_eps_t) * min_all_t);

                if (nom_has_slot && nom_div_ok && nom_t_ok) {
                    chosen = nominal;
                    reason = "coord_nominal_ok";
                } else {
                    uint32_t best_tier1 = k;
                    double best_tier1_t = std::numeric_limits<double>::infinity();
                    for (uint32_t offset = 0; offset < k; ++offset) {
                        uint32_t cand = (msg.target_cursor + offset) % k;
                        if (!fresh[cand] || msg.lanes[cand].outstanding_wqes >= d) continue;
                        if (scores[cand] <= min_all_div + config_.hrlb.coord_delta_psi &&
                            t_fin[cand] <= (1.0 + config_.hrlb.coord_eps_t) * min_all_t &&
                            t_fin[cand] < best_tier1_t) {
                            best_tier1_t = t_fin[cand];
                            best_tier1 = cand;
                        }
                    }
                    if (best_tier1 < k) {
                        chosen = best_tier1;
                        is_redirect = (chosen != nominal);
                        reason = is_redirect ? "coord_redirect_tier1" : "coord_nominal_tier1";
                    } else {
                        uint32_t best_tier2 = k;
                        double best_tier2_t = std::numeric_limits<double>::infinity();
                        for (uint32_t offset = 0; offset < k; ++offset) {
                            uint32_t cand = (msg.target_cursor + offset) % k;
                            if (!fresh[cand] || msg.lanes[cand].outstanding_wqes >= d) continue;
                            bool lighter =
                                (scores[cand] < scores[nominal] - config_.hrlb.coord_delta_guard);
                            bool faster = (t_fin[cand] < t_fin[nominal]);
                            bool not_heavier =
                                (scores[cand] <= scores[nominal] + config_.hrlb.coord_delta_guard);
                            bool not_tail_slow =
                                (t_fin[cand] <= (1.0 + config_.hrlb.coord_eps_t) * min_all_t);
                            if ((lighter || faster) && not_heavier && not_tail_slow &&
                                t_fin[cand] < best_tier2_t) {
                                best_tier2_t = t_fin[cand];
                                best_tier2 = cand;
                            }
                        }
                        if (best_tier2 < k) {
                            chosen = best_tier2;
                            is_redirect = (chosen != nominal);
                            reason = is_redirect ? "coord_redirect_tier2" : "coord_nominal_tier2";
                        } else if (nom_has_slot) {
                            bool tail_enabled = config_.hrlb.coord_tail_mode == "all" ||
                                (config_.hrlb.coord_tail_mode == "drain" &&
                                 msg.pending_chunks.size() <= config_.hrlb.coord_tail_chunks);
                            bool fast_full_exists = false;
                            for (uint32_t l = 0; l < k; ++l) {
                                if (!fresh[l] || msg.lanes[l].outstanding_wqes < d) continue;
                                bool healthy = config_.hrlb.coord_healthy_guard &&
                                               scores[l] < config_.hrlb.theta;
                                if (tail_enabled &&
                                    t_fin[l] * (1.0 + config_.hrlb.coord_eps_t) < t_fin[nominal] &&
                                    (healthy || scores[l] <= scores[nominal] + config_.hrlb.coord_delta_guard)) {
                                    fast_full_exists = true;
                                    break;
                                }
                            }
                            if (fast_full_exists) {
                                hold_tail = true;
                                reason = "coord_tail_guard_hold";
                            } else {
                                chosen = nominal;
                                reason = "coord_nominal_fallback";
                            }
                        }
                    }
                }
            }
        } else if (redirect_active) {
            if (!fresh[nominal]) {
                reason = "source_unknown_fallback";
            } else if (scores[nominal] > config_.hrlb.theta) {
                uint32_t best_cand = k;
                double best_score = std::numeric_limits<double>::infinity();
                bool any_below_theta = false;
                bool any_unknown = false;
                for (uint32_t offset = 0; offset < k; ++offset) {
                    uint32_t cand = (msg.target_cursor + offset) % k;
                    if (!fresh[cand]) {
                        any_unknown = true;
                        continue;
                    }
                    if (scores[cand] < config_.hrlb.theta) {
                        any_below_theta = true;
                        if (msg.lanes[cand].outstanding_wqes < d && scores[cand] < best_score) {
                            best_score = scores[cand];
                            best_cand = cand;
                        }
                    }
                }
                if (best_cand < k) {
                    chosen = best_cand;
                    is_redirect = (chosen != nominal);
                    reason = "redirect_min_pressure";
                } else if (any_below_theta) {
                    reason = "target_slots_full_fallback";
                } else if (any_unknown) {
                    reason = "target_unknown_fallback";
                } else {
                    reason = "all_paths_high_fallback";
                }
            } else {
                reason = "below_or_equal_theta";
            }
        }

        // Independent passive transitions preserve residence times even when decision rows
        // are deduplicated, or a later chunk is visited after an old-rule hold.
        if (tail_output_) {
            bool was_held = msg.observed_tail_holds.count(it->chunk_id) != 0;
            if (hold_tail != was_held) {
                uint64_t unsent = 0, inflight = 0;
                uint32_t fast_mask = 0;
                for (uint32_t l = 0; l < k; ++l) {
                    unsent += msg.lanes[l].qp->GetBytesLeft();
                    inflight += msg.lanes[l].qp->GetOnTheFly();
                    double tf = static_cast<double>(backlog_bytes[l] + it->bytes) * 8.0 / rate_bps[l];
                    double tn = static_cast<double>(backlog_bytes[nominal] + it->bytes) * 8.0 / rate_bps[nominal];
                    if (fresh[l] && msg.lanes[l].outstanding_wqes == d &&
                        tf * (1.0 + config_.hrlb.coord_eps_t) < tn &&
                        ((config_.hrlb.coord_healthy_guard && scores[l] < config_.hrlb.theta) ||
                         scores[l] <= scores[nominal] + config_.hrlb.coord_delta_guard)) {
                        if (l < 32) fast_mask |= 1u << l;
                    }
                }
                fprintf(tail_output_, "%lu,%s,%lu,%u,%u,%lu,%lu,%lu,%u,%u,%u\n",
                    Simulator::Now().GetNanoSeconds(), hold_tail ? "hold" : "release", seq,
                    msg.message_id, it->chunk_id, static_cast<uint64_t>(msg.pending_chunks.size()),
                    unsent, inflight, nominal, chosen, fast_mask);
                if (hold_tail) msg.observed_tail_holds.insert(it->chunk_id);
                else msg.observed_tail_holds.erase(it->chunk_id);
            }
        }

        if (!hold_tail && msg.lanes[chosen].outstanding_wqes < d) {
            PendingChunk chunk = *it;
            msg.pending_chunks.erase(it);
            LaneState& ls = msg.lanes[chosen];
            uint32_t slots_before = ls.outstanding_wqes;
            uint32_t cursor_before = msg.target_cursor;
            driver->AppendPersistentWqe(
                static_cast<uint16_t>(msg.pg), state_.server_addresses[msg.src],
                state_.server_addresses[msg.dst], ls.spec.sport, ls.spec.dport, window, base_rtt,
                static_cast<int32_t>(msg.message_id), chunk.chunk_id, chunk.bytes, chosen,
                RdmaQueuePair::V5_DATA, true);
            ls.outstanding_wqes += 1;
            ls.scheduled_wqes += 1;
            if (is_redirect) {
                msg.target_cursor = (chosen + 1) % k;
            }
            LogDecision(seq, trigger, "commit", msg, chunk, nominal, chosen, is_redirect, fresh,
                        scores, versions, ages, slots_before, ls.outstanding_wqes, cursor_before,
                        msg.target_cursor, reason);
            return true;
        } else if (hold_tail) {
            if (!logged_tail_hold) {
                uint32_t slots = msg.lanes[nominal].outstanding_wqes;
                LogDecision(seq, trigger, "blocked", msg, *it, nominal, nominal, false, fresh, scores,
                            versions, ages, slots, slots, msg.target_cursor, msg.target_cursor, reason);
                logged_tail_hold = true;
            }
            if (config_.hrlb.coord_tail_break) {
                msg.held_schedule_seq = seq;
                break;
            }
        } else if (redirect_active && fresh[nominal] && scores[nominal] > config_.hrlb.theta) {
            uint32_t slots = msg.lanes[chosen].outstanding_wqes;
            LogDecision(seq, trigger, "blocked", msg, *it, nominal, chosen, false, fresh, scores,
                        versions, ages, slots, slots, msg.target_cursor, msg.target_cursor, reason);
        }
    }
    return false;
}

std::string HostResourceLb::DecisionState(const MessageState& msg) const {
    if (!config_.hrlb.audit_log) return "";
    std::ostringstream slots, rates, unsent, inflight, next;
    for (size_t l = 0; l < msg.lanes.size(); ++l) {
        if (l) { slots << ';'; rates << ';'; unsent << ';'; inflight << ';'; next << ';'; }
        const LaneState& ls = msg.lanes[l];
        slots << ls.outstanding_wqes;
        rates << (ls.qp ? ls.qp->m_rate.GetBitRate() : 1);
        unsent << (ls.qp ? ls.qp->GetBytesLeft() : 0);
        inflight << (ls.qp ? ls.qp->GetOnTheFly() : 0);
        next << (ls.qp ? ls.qp->m_nextAvail.GetNanoSeconds() : 0);
    }
    std::ostringstream out;
    out << msg.src << ',' << msg.pending_chunks.size() << ',' << msg.completed_chunks << ',' << slots.str() << ','
        << rates.str() << ',' << unsent.str() << ',' << inflight.str() << ',' << next.str();
    return out.str();
}

void HostResourceLb::SampleLanes() {
    if (!lane_output_) return;
    bool unfinished = false;
    uint64_t now = Simulator::Now().GetNanoSeconds();
    for (const auto& item : messages_) {
        const MessageState& msg = item.second;
        if (msg.finish_ns) continue;
        unfinished = true;
        Ptr<QbbNetDevice> nic = DynamicCast<QbbNetDevice>(state_.nodes.Get(msg.src)->GetDevice(1));
        for (uint32_t l = 0; l < msg.lanes.size(); ++l) {
            const LaneState& ls = msg.lanes[l];
            if (!ls.qp) continue;
            uint64_t nominal_pending = 0;
            for (const auto& chunk : msg.pending_chunks) {
                if ((msg.message_id + chunk.chunk_id) % config_.hrlb.k == l) ++nominal_pending;
            }
            fprintf(lane_output_, "%lu,%u,%u,%u,%lu,%lu,%u,%lu,%lu,%lu,%ld,%u,%u,%u,%lu\n",
                now, msg.src, msg.message_id, l, static_cast<uint64_t>(msg.pending_chunks.size()),
                nominal_pending, ls.outstanding_wqes, ls.qp->GetBytesLeft(), ls.qp->GetOnTheFly(),
                ls.qp->m_rate.GetBitRate(), ls.qp->m_nextAvail.GetNanoSeconds(),
                ls.qp->IsWinBound() ? 1 : 0, nic->IsPriorityPaused(msg.pg) ? 1 : 0,
                nic->IsSerializing() ? 1 : 0, nic->GetTxElapsedBusyTimeNs());
        }
    }
    if (unfinished) lane_sample_event_ = Simulator::Schedule(
        NanoSeconds(config_.monitoring.switch_interval_ns), &HostResourceLb::SampleLanes, this);
}

void HostResourceLb::SealMessage(MessageState& msg) {
    msg.sealed = true;
    Ptr<RdmaDriver> driver = state_.nodes.Get(msg.src)->GetObject<RdmaDriver>();
    for (const auto& ls : msg.lanes) {
        driver->SealPersistentQueuePair(state_.server_addresses[msg.dst].Get(), ls.spec.sport,
                                        ls.spec.dport, static_cast<uint16_t>(msg.pg),
                                        ls.scheduled_wqes);
    }
}

void HostResourceLb::LogDecision(uint64_t seq, const char* trigger, const char* event,
                                 const MessageState& msg, const PendingChunk& chunk,
                                 uint32_t nominal, uint32_t chosen, bool is_redirect,
                                 const std::vector<bool>& fresh, const std::vector<double>& scores,
                                 const std::vector<uint64_t>& versions,
                                 const std::vector<uint64_t>& ages, uint32_t slots_before,
                                 uint32_t slots_after, uint32_t cursor_before,
                                 uint32_t cursor_after, const char* reason) {
    if (!decision_output_) return;
    // Observation-only: no decision, slot, cursor or scheduling state depends on this.
    if (config_.hrlb.decision_dedup && std::string(reason) == "coord_tail_guard_hold") {
        MessageState& state = messages_.at(msg.message_id);
        if (state.logged_hold_seq == seq) return;
        state.logged_hold_seq = seq;
    }
    std::ostringstream s_scores, s_fresh, s_versions, s_ages;
    s_scores << std::setprecision(17);
    for (size_t i = 0; i < scores.size(); ++i) {
        if (i > 0) {
            s_scores << ';';
            s_fresh << ';';
            s_versions << ';';
            s_ages << ';';
        }
        if (fresh[i]) s_scores << scores[i];
        else s_scores << "nan";
        s_fresh << (fresh[i] ? 1 : 0);
        s_versions << versions[i];
        s_ages << ages[i];
    }
    fprintf(decision_output_,
            "%lu,%lu,%lu,%s,%s,%u,%u,%lu,%u,%u,%u,%u,%u,%.17g,%u,%.17g,%s,%s,%s,%s,%u,%u,%u,%u,%s",
            Simulator::Now().GetNanoSeconds(), seq, delivered_epochs_[msg.src], trigger, event,
            msg.message_id, chunk.chunk_id, chunk.offset, chunk.bytes, nominal, chosen,
            is_redirect ? 1 : 0, fresh[nominal] ? 1 : 0,
            fresh[nominal] ? scores[nominal] : std::numeric_limits<double>::quiet_NaN(),
            fresh[chosen] ? 1 : 0,
            fresh[chosen] ? scores[chosen] : std::numeric_limits<double>::quiet_NaN(),
            s_scores.str().c_str(), s_fresh.str().c_str(), s_versions.str().c_str(),
            s_ages.str().c_str(), slots_before, slots_after, cursor_before, cursor_after, reason);
    if (config_.hrlb.audit_log) fprintf(decision_output_, ",%s", decision_state_.c_str());
    fprintf(decision_output_, "\n");
}

}  // namespace nlb
