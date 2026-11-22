#include "rdma-hw.h"
#include "odin-int-tag.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>

namespace ns3 {
void RdmaHw::TraceOdin(Ptr<RdmaQueuePair> qp, OdinEvent event) {
    if (!m_odinTraceEnabled || (!m_odinTraceFlowIds.empty() &&
        !m_odinTraceFlowIds.count(qp->m_flow_id))) return;
    auto& controller = m_nic[qp->odin.nic].odin;
    event.nic = qp->odin.nic;
    event.active = controller.Active();
    event.capacity = controller.Capacity();
    event.base_rtt_ns = qp->m_baseRtt;
    event.window = qp->GetWin();
    event.last_packet_bytes = qp->lastPktSize;
    event.inflight_bytes = qp->GetOnTheFly();
    if (std::strcmp(event.event, "update") != 0) {
        event.control.old_rate = event.control.rate = qp->m_rate.GetBitRate();
        event.control.sum_before = event.control.sum_after = controller.Sum();
        event.control.u_before = event.control.u_after = controller.Utilization();
        if (std::strcmp(event.event, "tx") != 0)
            event.next_before_ns = event.next_after_ns = qp->m_nextAvail.GetNanoSeconds();
    }
    m_traceOdin(qp, event);
}

void RdmaHw::HandleAckOdin(Ptr<RdmaQueuePair> qp, CustomHeader& ch, Ptr<const Packet> packet) {
    OdinEvent event;
    event.ack_seq = ch.ack.seq;
    uint64_t qp_key = GetQpKey(qp->dip.Get(), qp->sport, qp->dport, qp->m_pg);
    auto skip = [&](const char* reason) {
        event.reason = reason;
        TraceOdin(qp, event);
        if (m_odinIntShareMode == 1) TraceInt("pace", packet->GetUid(), qp_key, reason);
    };
    if (ch.l3Prot != 0xFC) { skip("nack"); return; }
    if (m_odinIntShareMode == 1)
        TraceInt("ack", packet->GetUid(), qp_key, "data", OdinResourceKey(), 0, 0, 0, ch.ack.seq);
    if (m_odinResourceFeedback &&
        (m_odinResourcePaths.count(qp_key) ||
         (Simulator::Now().GetNanoSeconds() >= m_odinResourceStartNs &&
          m_odinResourceDestinations.count(qp->dip.Get())))) {
        // ReceiveAck has already retained the original reliability/window work.
        // Exactly one source owns CC updates for these QPs after activation.
        skip("resource_report_only");
        return;
    }
    IntHeader& ih = ch.ack.ih;
    uint32_t hops = ih.GetHopCount();
    if (!hops || hops > IntHeader::maxHop) { skip("invalid_hops"); return; }
    uint64_t now = Simulator::Now().GetNanoSeconds();
    auto baseline = [&]() {
        qp->odin.previous = ih;
        qp->odin.received_ns = now;
        qp->odin.sampled = true;
    };
    if (!qp->odin.sampled || hops != qp->odin.previous.GetHopCount()) {
        baseline(); skip("baseline"); return;
    }
    double psi = 0;
    OdinIntTag tag;
    if (m_odinIntShareMode == 1 && m_odinIntAudit) packet->PeekPacketTag(tag);
    for (uint32_t i = 0; i < hops; ++i) {
        IntHop& current = ih.hop[i];
        IntHop& previous = qp->odin.previous.hop[i];
        uint64_t rate = current.GetLineRate();
        if (!rate || rate != previous.GetLineRate()) {
            baseline(); skip("line_rate_changed"); return;
        }
        // Compression can represent one wrap only. Rebaseline long feedback gaps.
        double byte_wrap_ns = std::ldexp(double(IntHop::byteUnit * IntHop::multi),
                                         IntHop::bytesWidth) * 8e9 / rate;
        double wrap_ns = std::min(std::ldexp(1.0, IntHop::timeWidth), byte_wrap_ns);
        if (now - qp->odin.received_ns + qp->m_baseRtt >= wrap_ns) {
            baseline(); skip("sample_gap"); return;
        }
        uint64_t dt = current.GetTimeDelta(previous);
        if (!dt) { skip("duplicate_time"); return; }
        double mu = current.GetBytesDelta(previous) * 8e9 / dt;
        double value = mu / rate + double(current.GetQlen()) * 8e9 /
                                        (double(rate) * qp->m_baseRtt);
        if (!std::isfinite(value) || value <= 0) {
            baseline(); skip("invalid_psi"); return;
        }
        OdinEvent sample;
        sample.event = "sample";
        sample.ack_seq = ch.ack.seq;
        sample.control.psi = value;
        sample.control.tbo = ih.HasTbo();
        sample.sample_hop = i;
        sample.sample_queue_bytes = current.GetQlen();
        sample.sample_dt_ns = dt;
        sample.sample_delta_bytes = current.GetBytesDelta(previous);
        sample.sample_line_bps = rate;
        sample.sample_time_mod_ns = current.GetTime();
        TraceOdin(qp, sample);
        if (m_odinIntShareMode == 1)
            TraceInt("pace_sample", packet->GetUid(), qp_key, "accepted",
                     OdinResourceKey(ih.res_sw[i], ih.GetResourcePort(i)), now, dt,
                     tag.measured_ns[i], current.GetTime(), &current, ih.HasHopTbo(i));
        psi = std::max(psi, value);
    }
    baseline();
    if (m_odinIntShareMode == 1) {
        const bool applied = ApplyIntFeedback(qp, qp_key, m_odinIntMinUpdateNs, psi,
                                              ih.HasTbo(), event, false);
        TraceInt("pace", packet->GetUid(), qp_key, applied ? "update" : event.reason);
        return;
    }
    event.event = "update";
    event.control = m_nic[qp->odin.nic].odin.OnFeedback(
        GetQpKey(qp->dip.Get(), qp->sport, qp->dport, qp->m_pg), psi, ih.HasTbo(),
        m_odinEta, m_odinGamma, m_odinBeta.GetBitRate(), m_minRate.GetBitRate());
    event.next_before_ns = qp->m_nextAvail.GetNanoSeconds();
    ChangeRate(qp, DataRate(event.control.rate));
    event.next_after_ns = qp->m_nextAvail.GetNanoSeconds();
    TraceOdin(qp, event);
}

void RdmaHw::TraceInt(const char* event, uint64_t ack_uid, uint64_t key, const char* reason,
                      OdinResourceKey resource, uint64_t rx_ns, uint64_t dt_ns,
                      uint64_t measured_ns, uint64_t value, const IntHop* hop, bool tbo) const {
    if (!m_odinIntAudit) return;
    fprintf(m_odinIntAudit, "%lu,%s,%u,%lu,%lu,%u,%u,%s,%lu,%lu,%lu,%lu,%u,%u,%lu,%u\n",
            Simulator::Now().GetNanoSeconds(), event, m_node->GetId(), ack_uid, key,
            resource.switch_id, resource.interface, reason, rx_ns, dt_ns, measured_ns, value,
            hop ? static_cast<unsigned>(hop->bytes) : 0, hop ? hop->GetQlen() : 0,
            hop ? hop->GetLineRate() : 0, unsigned(tbo));
}

bool RdmaHw::ApplyIntFeedback(Ptr<RdmaQueuePair> qp, uint64_t key, uint64_t interval_ns,
                              double psi, bool tbo, OdinEvent& event, bool is_async_trigger) {
    uint64_t now = Simulator::Now().GetNanoSeconds();
    auto skip = [&](const char* reason) {
        event.reason = reason;
        TraceOdin(qp, event);
        return false;
    };
    event.control.psi = psi;
    event.control.tbo = tbo;
    if (!qp->odin.active || !qp->GetBytesLeft()) return skip("no_unsent_data");
    if (now - qp->odin.last_update_ns < interval_ns) return skip("cooldown");
    if (!std::isfinite(psi) || psi <= 0) return skip("resource_zero_psi");
    auto& controller = m_nic[qp->odin.nic].odin;
    const uint64_t beta = m_odinBeta.GetBitRate();
    const uint64_t minimum = m_minRate.GetBitRate();
    const bool gate_active =
        m_odinGateInflight0 == 1 || (m_odinGateInflight0 == 2 && is_async_trigger);
    if (gate_active && !qp->GetOnTheFly() &&
        controller.PreviewFeedback(key, psi, tbo, m_odinEta, m_odinGamma, beta,
                                   minimum).rate < qp->m_rate.GetBitRate()) {
        return skip("inflight0_no_decrease");
    }
    event.event = "update";
    event.control = controller.OnFeedback(key, psi, tbo, m_odinEta, m_odinGamma, beta, minimum);
    qp->odin.last_update_ns = now;
    event.next_before_ns = qp->m_nextAvail.GetNanoSeconds();
    ChangeRate(qp, DataRate(event.control.rate));
    event.next_after_ns = qp->m_nextAvail.GetNanoSeconds();
    TraceOdin(qp, event);
    return true;
}

void RdmaHw::ForgetSharedIntPath(uint64_t key) {
    auto path = m_odinIntPaths.find(key);
    if (path == m_odinIntPaths.end()) return;
    for (const auto& resource : path->second) {
        auto members = m_odinIntSubscribers.find(resource);
        if (members == m_odinIntSubscribers.end()) continue;
        members->second.erase(key);
        if (members->second.empty()) m_odinIntSubscribers.erase(members);
    }
    m_odinIntPaths.erase(path);
}

const char* RdmaHw::ReadOdinIntHop(const OdinResourceKey& resource, uint64_t now,
                                  uint64_t base_rtt_ns, double* score, bool* tbo,
                                  uint64_t* rx, uint64_t* dt, uint64_t* measured) const {
    auto cached = m_odinIntCache.find(resource);
    if (cached == m_odinIntCache.end() || cached->second.empty()) return "stale_or_missing_hop";
    const auto& history = cached->second;
    const auto& current = history.back();
    *rx = current.rx_ns;
    *measured = current.true_measured_ns;
    *dt = current.time_ns - history.front().time_ns;
    if (now - *rx > m_odinIntFreshnessNs) return "stale_or_missing_hop";
    if (*dt < m_odinIntWindowNs) return "cache_not_ready";
    double mu = current.hop.GetBytesDelta(history.front().hop) * 8e9 / *dt;
    double line = current.hop.GetLineRate();
    *score = std::max(*score, mu / line + current.hop.GetQlen() * 8e9 / (line * base_rtt_ns));
    *tbo |= current.tbo;
    return nullptr;
}

bool RdmaHw::QueryOdinLearnedFirstHop(uint64_t qp_key, OdinResourceKey* first_hop) const {
    auto path = m_odinIntPaths.find(qp_key);
    if (path == m_odinIntPaths.end() || path->second.empty()) return false;
    *first_hop = path->second.front();
    return true;
}

bool RdmaHw::QueryOdinIntScore(const std::vector<OdinResourceKey>& resources, uint64_t base_rtt_ns,
                               double* score, bool* tbo, uint64_t* max_age_ns) const {
    *score = 0.0;
    *tbo = false;
    *max_age_ns = 0;
    if (resources.empty() || !base_rtt_ns) return false;
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    double psi = 0.0;
    bool saw_tbo = false;
    uint64_t age = 0;
    for (const auto& resource : resources) {
        uint64_t rx = 0, dt = 0, measured = 0;
        if (ReadOdinIntHop(resource, now, base_rtt_ns, &psi, &saw_tbo, &rx, &dt, &measured))
            return false;
        age = std::max(age, now - rx);
    }
    *score = psi;
    *tbo = saw_tbo;
    *max_age_ns = age;
    return true;
}

bool RdmaHw::TrySharedIntFeedback(Ptr<RdmaQueuePair> qp, uint64_t key, uint64_t ack_uid,
                                  const char* trigger, uint32_t seq, bool apply) {
    if (!qp || !qp->odin.active || !qp->GetBytesLeft()) return false;
    auto path = m_odinIntPaths.find(key);
    if (path == m_odinIntPaths.end()) return false;
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    const char* blocked = nullptr;
    double psi = 0;
    bool tbo = false;
    for (const auto& resource : path->second) {
        uint64_t rx = 0, dt = 0, measured = 0;
        const char* reason = ReadOdinIntHop(resource, now, qp->m_baseRtt, &psi, &tbo,
                                            &rx, &dt, &measured);
        TraceInt(trigger, ack_uid, key, reason ? reason : "ready", resource, rx, dt, measured);
        if (reason && (!blocked || std::strcmp(reason, "stale_or_missing_hop") == 0))
            blocked = reason;
    }
    if (!apply || blocked) {
        TraceInt("candidate", ack_uid, key, blocked ? blocked : "all_ready");
        return false;
    }
    OdinEvent event;
    event.reason = trigger;
    event.ack_seq = seq;
    // The frozen foreign threshold T_share is the same as T_min.
    const bool is_async = std::strcmp(trigger, "self") != 0;
    const bool updated = ApplyIntFeedback(qp, key, m_odinIntMinUpdateNs, psi, tbo, event, is_async);
    TraceInt("candidate", ack_uid, key, updated ? "update" : event.reason);
    return updated;
}

void RdmaHw::HandleSharedIntAck(Ptr<RdmaQueuePair> qp, uint64_t key, Ptr<const Packet> packet,
                                const CustomHeader& ch) {
    const IntHeader& ih = ch.ack.ih;
    const uint64_t uid = packet->GetUid();
    const bool probe = ih.IsProbe();
    const uint32_t hops = ih.GetHopCount();
    if (m_odinIntAudit) ++m_odinIntAuditAckCount;
    TraceInt("ack", uid, key, probe ? "probe" : "data", OdinResourceKey(), 0, 0, 0, ch.ack.seq);
    if (!hops || hops > IntHeader::maxHop) {
        TraceInt("sample", uid, key, "invalid_hops");
        return;
    }
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    const uint64_t half = uint64_t(1) << (IntHop::timeWidth - 1);
    OdinIntTag tag;
    if (m_odinIntAudit) packet->PeekPacketTag(tag);
    OdinIntPath path;
    std::set<uint64_t> candidates;
    for (uint32_t i = 0; i < hops; ++i) {
        OdinResourceKey resource(ih.res_sw[i], ih.GetResourcePort(i));
        path.push_back(resource);
        const IntHop& hop = ih.hop[i];
        const uint64_t rate = hop.GetLineRate();
        if (!rate) {
            TraceInt("sample", uid, key, "invalid_line_rate", resource, now, 0,
                     tag.measured_ns[i]);
            continue;
        }
        auto& history = m_odinIntCache[resource];
        const double byte_wrap = std::ldexp(double(IntHop::byteUnit) * IntHop::multi,
                                            IntHop::bytesWidth) * 8e9 / rate;
        const double guard = std::min(double(half), byte_wrap);
        uint64_t unwrapped = hop.GetTime();
        const char* reason = "baseline";
        if (!history.empty()) {
            const auto& last = history.back();
            if (rate != last.hop.GetLineRate() || now - last.rx_ns + m_odinIntMaxRttNs >= guard) {
                history.clear();
                reason = "rebaseline";
            } else {
                uint64_t dt = hop.GetTimeDelta(last.hop);
                if (!dt || dt >= half) {
                    TraceInt("sample", uid, key, "ooo_sample_dropped", resource, now, 0,
                             tag.measured_ns[i], hop.GetTime(), &hop, ih.HasHopTbo(i));
                    continue;
                }
                unwrapped = last.time_ns + dt;
                reason = "accepted";
            }
        }
        history.push_back({hop, now, tag.measured_ns[i], unwrapped, ih.HasHopTbo(i)});
        while (history.size() >= 2 && unwrapped - history[1].time_ns >= m_odinIntWindowNs)
            history.pop_front();
        // The anchor must still admit an unambiguous byte delta.
        if (unwrapped - history.front().time_ns >= guard) {
            auto latest = history.back();
            history.clear();
            history.push_back(latest);
            reason = "rebaseline";
        }
        NS_ABORT_MSG_IF(history.size() > 2048, "Odin INT history exceeds safety bound");
        TraceInt("sample", uid, key, reason, resource, now,
                 unwrapped - history.front().time_ns, tag.measured_ns[i], hop.GetTime(), &hop,
                 ih.HasHopTbo(i));
        if (m_odinIntShareMode >= 3 || m_odinIntAudit) {
            auto members = m_odinIntSubscribers.find(resource);
            if (members != m_odinIntSubscribers.end())
                candidates.insert(members->second.begin(), members->second.end());
        }
    }

    if (qp && GetQp(key) == qp) {
        auto learned = m_odinIntPaths.emplace(key, path);
        NS_ABORT_MSG_IF(learned.first->second != path, "Odin INT changed a fixed QP path");
        if (learned.second) {
            for (const auto& resource : path) m_odinIntSubscribers[resource].insert(key);
        }
        TrySharedIntFeedback(qp, key, uid, probe ? "probe_self" : "self", ch.ack.seq, true);
    }
    candidates.erase(key);
    std::vector<std::pair<uint64_t, Ptr<RdmaQueuePair>>> ordered;
    for (auto other : candidates) ordered.emplace_back(other, m_qpMap.at(other));
    std::sort(ordered.begin(), ordered.end(), [](const std::pair<uint64_t, Ptr<RdmaQueuePair>>& a,
                                                const std::pair<uint64_t, Ptr<RdmaQueuePair>>& b) {
        return std::tie(a.second->m_flow_id, a.second->m_v5_lane, a.first) <
               std::tie(b.second->m_flow_id, b.second->m_v5_lane, b.first);
    });
    std::set<uint32_t> extra_nics;
    uint32_t wakeups = 0;
    for (const auto& candidate : ordered) {
        auto other = candidate.second;
        if (TrySharedIntFeedback(other, candidate.first, uid, "foreign", ch.ack.seq,
                                 m_odinIntShareMode >= 3)) {
            ++wakeups;
            if (!qp || other->odin.nic != qp->odin.nic) extra_nics.insert(other->odin.nic);
        }
    }
    TraceInt("wakeups", uid, key, "", OdinResourceKey(), 0, 0, 0, wakeups);
    for (uint32_t nic : extra_nics) m_nic[nic].dev->TriggerTransmit();
}

void RdmaHw::ArmIntProbeCheck() {
    if (!m_odinIntProbeCheck.IsRunning())
        m_odinIntProbeCheck = Simulator::Schedule(MicroSeconds(4), &RdmaHw::CheckIntProbes, this);
}

void RdmaHw::CheckIntProbes() {
    const uint64_t now = Simulator::Now().GetNanoSeconds();
    bool pending = false;
    std::set<uint32_t> nics;
    // QP-key order deterministically selects one request per learned path.
    for (const auto& entry : m_qpMap)
        pending |= entry.second->odin.active && entry.second->GetBytesLeft() > 0;
    if (!pending) return;
    for (const auto& entry : m_odinIntPaths) {
        const auto qp = m_qpMap.at(entry.first);
        const auto& path = entry.second;
        if (!qp->odin.active || !qp->GetBytesLeft() ||
            now - qp->odin.last_update_ns < m_odinIntProbeIntervalNs) continue;
        auto last = m_odinIntLastProbe.find(path);
        if (last != m_odinIntLastProbe.end() && now - last->second <= m_odinIntProbeIntervalNs)
            continue;
        bool blind = std::any_of(path.begin(), path.end(), [&](const OdinResourceKey& resource) {
            auto cached = m_odinIntCache.find(resource);
            if (cached == m_odinIntCache.end()) return true;
            const auto& history = cached->second;
            return now - history.back().rx_ns > m_odinIntProbeIntervalNs ||
                   history.back().time_ns - history.front().time_ns < m_odinIntWindowNs;
        });
        if (!blind) continue;
        auto packet = BuildRdmaUdp(qp, 0, 0, true);
        const uint32_t nic = GetNicIdxOfQp(qp);
        m_odinIntLastProbe[path] = now;
        TraceInt("probe_enqueue", packet->GetUid(), entry.first, "");
        m_nic[nic].dev->RdmaEnqueueHighPrioQ(packet);
        nics.insert(nic);
    }
    for (uint32_t nic : nics) m_nic[nic].dev->TriggerTransmit();
    ArmIntProbeCheck();
}

uint32_t RdmaHw::ApplyOdinResourceFeedback(
    uint64_t version, const std::vector<OdinResourceSample>& samples) {
    NS_ABORT_MSG_IF(!m_odinResourceFeedback || m_cc_mode != CC_MODE_ODIN,
                    "resource feedback requires explicit Odin opt-in");
    if (version <= m_odinResourceVersion) return 0;
    m_odinResourceVersion = version;
    std::map<uint32_t, const OdinResourceSample*> by_destination;
    uint64_t now = Simulator::Now().GetNanoSeconds();
    for (const auto& s : samples) {
        NS_ABORT_MSG_IF(!s.line_bps || s.measured_ns <= s.window_start_ns ||
                        s.measured_ns > now || !m_odinResourceDestinations.count(s.destination_ip) ||
                        by_destination.count(s.destination_ip), "invalid resource observation");
        by_destination[s.destination_ip] = &s;
    }
    std::vector<Ptr<RdmaQueuePair>> eligible;
    for (const auto& entry : m_qpMap) {
        Ptr<RdmaQueuePair> q = entry.second;
        if (q->odin.active && q->GetBytesLeft() && by_destination.count(q->dip.Get()) &&
            q->startTime.GetNanoSeconds() <= by_destination[q->dip.Get()]->measured_ns)
            eligible.push_back(q);
    }
    // The Algorithm 1 EWMA is shared by the NIC. Specify the sequential order;
    // never freeze U and silently replace it by a simultaneous group update.
    std::sort(eligible.begin(), eligible.end(), [](Ptr<RdmaQueuePair> a, Ptr<RdmaQueuePair> b) {
        return a->m_flow_id < b->m_flow_id;
    });
    for (Ptr<RdmaQueuePair> q : eligible) {
        const auto& s = *by_destination[q->dip.Get()];
        double psi = double(s.delta_bytes) * 8e9 / (s.measured_ns - s.window_start_ns) / s.line_bps +
                     double(s.queue_bytes) * 8e9 / (double(s.line_bps) * q->m_baseRtt);
        // An idle, empty port has psi=0. Do not invent an epsilon/line-rate jump
        // absent from Algorithm 1: log and leave its rate unchanged.
        OdinEvent e;
        e.reason = "resource_report";
        e.resource_version = version;
        e.resource_measured_ns = s.measured_ns;
        e.resource_window_start_ns = s.window_start_ns;
        e.resource_switch = s.switch_id;
        e.resource_interface = s.interface;
        e.sample_dt_ns = s.measured_ns - s.window_start_ns;
        e.sample_delta_bytes = s.delta_bytes;
        e.sample_line_bps = s.line_bps;
        e.sample_queue_bytes = s.queue_bytes;
        e.control.psi = psi;
        e.control.tbo = s.tbo;
        if (!std::isfinite(psi) || psi <= 0) {
            e.reason = "resource_zero_psi";
            TraceOdin(q, e);
            continue;
        }
        e.event = "sample";
        TraceOdin(q, e);
        e.event = "update";
        e.control = m_nic[q->odin.nic].odin.OnFeedback(
            GetQpKey(q->dip.Get(), q->sport, q->dport, q->m_pg), psi, s.tbo,
            m_odinEta, m_odinGamma, m_odinBeta.GetBitRate(), m_minRate.GetBitRate());
        e.next_before_ns = q->m_nextAvail.GetNanoSeconds();
        ChangeRate(q, DataRate(e.control.rate));
        e.next_after_ns = q->m_nextAvail.GetNanoSeconds();
        TraceOdin(q, e);
    }
    return eligible.size();
}

void RdmaHw::RegisterOdinResourcePath(uint64_t qp_key, uint32_t path_id, uint32_t mapping_version,
                                      const std::vector<OdinResourceKey>& resources) {
    NS_ABORT_MSG_IF(resources.empty(), "Odin resource path cannot be empty");
    OdinPathSpec spec;
    spec.path_id = path_id;
    spec.mapping_version = mapping_version;
    spec.resources = resources;
    m_odinResourcePaths[qp_key] = spec;
}

bool RdmaHw::QueryOdinResourceSetScore(const std::vector<OdinResourceKey>& resources,
                                         uint32_t mapping_version, uint64_t base_rtt_ns,
                                         double* score, bool* tbo, uint64_t* max_age_ns,
                                         uint64_t* min_version,
                                         OdinResourceSample* bottleneck_sample,
                                         const char** reason) const {
    *score = 0.0;
    *tbo = false;
    *max_age_ns = 0;
    *min_version = 0;
    if (reason) *reason = "unknown";
    if (!base_rtt_ns) {
        if (reason) *reason = "invalid_rtt";
        return false;
    }
    if (resources.empty()) {
        if (reason) *reason = "missing_path";
        return false;
    }
    if (mapping_version != m_odinExpectedMappingVersion) {
        if (reason) *reason = "mapping_mismatch";
        return false;
    }
    uint64_t now = Simulator::Now().GetNanoSeconds();
    double max_psi = -1.0;
    bool any_tbo = false;
    uint64_t worst_age = 0;
    uint64_t oldest_epoch = std::numeric_limits<uint64_t>::max();
    OdinResourceSample worst_sample;
    for (const auto& res_key : resources) {
        auto cached_it = m_odinResourceCache.find(res_key);
        if (cached_it == m_odinResourceCache.end()) {
            if (reason) *reason = "missing_resource";
            return false;
        }
        const OdinCachedResource& cached = cached_it->second;
        const OdinResourceSample& s = cached.sample;
        if (!s.line_bps || s.measured_ns <= s.window_start_ns || s.measured_ns > now) {
            if (reason) *reason = "invalid_sample";
            return false;
        }
        uint64_t dt_ns = s.measured_ns - s.window_start_ns;
        if (m_odinResourceWindowNs && dt_ns != m_odinResourceWindowNs) {
            if (reason) *reason = "incomplete_window";
            return false;
        }
        uint64_t age_ns = now - s.measured_ns;
        if (age_ns > m_odinResourceFreshnessNs) {
            if (reason) *reason = "stale_resource";
            return false;
        }
        double psi_e = double(s.delta_bytes) * 8e9 / (double(dt_ns) * double(s.line_bps)) +
                       double(s.queue_bytes) * 8e9 / (double(s.line_bps) * double(base_rtt_ns));
        if (!std::isfinite(psi_e) || psi_e < 0.0) {
            if (reason) *reason = "nonfinite_psi";
            return false;
        }
        auto tbo_it = m_odinSwitchTboCache.find(res_key.switch_id);
        if (tbo_it == m_odinSwitchTboCache.end() || tbo_it->second.measured_ns != s.measured_ns) {
            if (reason) *reason = "missing_tbo";
            return false;
        }
        any_tbo = any_tbo || tbo_it->second.tbo;
        worst_age = std::max(worst_age, age_ns);
        oldest_epoch = std::min(oldest_epoch, cached.epoch);
        if (psi_e > max_psi) {
            max_psi = psi_e;
            worst_sample = s;
        }
    }
    *score = max_psi;
    *tbo = any_tbo;
    *max_age_ns = worst_age;
    *min_version = oldest_epoch == std::numeric_limits<uint64_t>::max() ? 0 : oldest_epoch;
    if (bottleneck_sample) *bottleneck_sample = worst_sample;
    if (reason) *reason = "fresh";
    return true;
}

bool RdmaHw::QueryOdinPathScore(uint64_t qp_key, uint64_t base_rtt_ns, double* score,
                                  bool* tbo, uint64_t* max_age_ns, uint64_t* min_version,
                                  OdinResourceSample* bottleneck_sample,
                                  const char** reason) const {
    auto path_it = m_odinResourcePaths.find(qp_key);
    if (path_it == m_odinResourcePaths.end() || path_it->second.resources.empty()) {
        *score = 0.0;
        *tbo = false;
        *max_age_ns = 0;
        *min_version = 0;
        if (reason) *reason = "missing_path";
        return false;
    }
    const OdinPathSpec& spec = path_it->second;
    return QueryOdinResourceSetScore(spec.resources, spec.mapping_version, base_rtt_ns, score, tbo,
                                     max_age_ns, min_version, bottleneck_sample, reason);
}

uint32_t RdmaHw::ApplyOdinMultiHopResourceFeedback(
    uint64_t version, const std::vector<OdinResourceSample>& samples) {
    NS_ABORT_MSG_IF(!m_odinResourceFeedback || m_cc_mode != CC_MODE_ODIN,
                    "multi-hop resource feedback requires explicit Odin opt-in");
    NS_ABORT_MSG_IF(m_odinGateInflight0 > 1,
                    "periodic resource feedback supports gate_inflight0 in {0,1} only");
    if (version <= m_odinResourceVersion) return 0;
    m_odinResourceVersion = version;
    uint64_t now = Simulator::Now().GetNanoSeconds();

    // Update the full resource cache before executing any CC or data decision.
    std::set<OdinResourceKey> seen_keys;
    for (const auto& s : samples) {
        OdinResourceKey key;
        key.switch_id = s.switch_id;
        key.interface = s.interface;
        NS_ABORT_MSG_IF(!s.line_bps || s.measured_ns <= s.window_start_ns ||
                        s.measured_ns > now || !seen_keys.insert(key).second,
                        "invalid multi-hop resource observation");
        OdinCachedResource cached;
        cached.sample = s;
        cached.sample.epoch = version;
        cached.epoch = version;
        cached.received_ns = now;
        m_odinResourceCache[key] = cached;

        auto tbo_it = m_odinSwitchTboCache.find(s.switch_id);
        if (tbo_it != m_odinSwitchTboCache.end() && tbo_it->second.epoch == version) {
            NS_ABORT_MSG_IF(tbo_it->second.tbo != s.tbo ||
                            tbo_it->second.measured_ns != s.measured_ns,
                            "inconsistent switch TBO in same epoch");
        } else {
            OdinSwitchTboState state;
            state.tbo = s.tbo;
            state.epoch = version;
            state.measured_ns = s.measured_ns;
            m_odinSwitchTboCache[s.switch_id] = state;
        }
    }

    std::vector<Ptr<RdmaQueuePair>> eligible;
    for (const auto& entry : m_qpMap) {
        Ptr<RdmaQueuePair> q = entry.second;
        uint64_t qp_key = GetQpKey(q->dip.Get(), q->sport, q->dport, q->m_pg);
        if (q->odin.active && q->GetBytesLeft() > 0 && m_odinResourcePaths.count(qp_key)) {
            eligible.push_back(q);
        }
    }
    std::sort(eligible.begin(), eligible.end(), [this](Ptr<RdmaQueuePair> a, Ptr<RdmaQueuePair> b) {
        uint64_t ka = GetQpKey(a->dip.Get(), a->sport, a->dport, a->m_pg);
        uint64_t kb = GetQpKey(b->dip.Get(), b->sport, b->dport, b->m_pg);
        return std::tie(a->m_flow_id, a->m_v5_lane, ka) <
               std::tie(b->m_flow_id, b->m_v5_lane, kb);
    });

    uint32_t updated = 0;
    for (Ptr<RdmaQueuePair> q : eligible) {
        uint64_t qp_key = GetQpKey(q->dip.Get(), q->sport, q->dport, q->m_pg);
        double psi = 0.0;
        bool tbo = false;
        uint64_t max_age_ns = 0;
        uint64_t min_version = 0;
        OdinResourceSample bottleneck;
        const char* score_reason = "unknown";
        bool fresh = QueryOdinPathScore(qp_key, q->m_baseRtt, &psi, &tbo, &max_age_ns,
                                        &min_version, &bottleneck, &score_reason);

        OdinEvent e;
        e.reason = "resource_report";
        e.resource_version = version;
        if (!fresh) {
            e.event = "skip";
            e.reason = score_reason;
            TraceOdin(q, e);
            continue;
        }
        e.resource_measured_ns = bottleneck.measured_ns;
        e.resource_window_start_ns = bottleneck.window_start_ns;
        e.resource_switch = bottleneck.switch_id;
        e.resource_interface = bottleneck.interface;
        e.sample_dt_ns = bottleneck.measured_ns - bottleneck.window_start_ns;
        e.sample_delta_bytes = bottleneck.delta_bytes;
        e.sample_line_bps = bottleneck.line_bps;
        e.sample_queue_bytes = bottleneck.queue_bytes;
        e.control.psi = psi;
        e.control.tbo = tbo;
        if (!std::isfinite(psi) || psi <= 0.0) {
            e.event = "skip";
            e.reason = "resource_zero_psi";
            TraceOdin(q, e);
            continue;
        }
        if (m_odinGateInflight0 == 1 && q->m_persistent && q->GetOnTheFly() == 0) {
            OdinController::Update preview = m_nic[q->odin.nic].odin.PreviewFeedback(
                qp_key, psi, tbo, m_odinEta, m_odinGamma, m_odinBeta.GetBitRate(),
                m_minRate.GetBitRate());
            if (preview.rate < q->m_rate.GetBitRate()) {
                e.event = "skip";
                e.reason = "inflight0_no_decrease";
                TraceOdin(q, e);
                continue;
            }
        }
        e.event = "sample";
        TraceOdin(q, e);
        e.event = "update";
        e.control = m_nic[q->odin.nic].odin.OnFeedback(
            qp_key, psi, tbo, m_odinEta, m_odinGamma, m_odinBeta.GetBitRate(),
            m_minRate.GetBitRate());
        e.next_before_ns = q->m_nextAvail.GetNanoSeconds();
        ChangeRate(q, DataRate(e.control.rate));
        e.next_after_ns = q->m_nextAvail.GetNanoSeconds();
        TraceOdin(q, e);
        ++updated;
    }
    if (!m_odinResourceDeliveredCallback.IsNull()) {
        m_odinResourceDeliveredCallback(version);
    }
    return eligible.size();
}
}  // namespace ns3
