#include "rdma-hw.h"
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

void RdmaHw::HandleAckOdin(Ptr<RdmaQueuePair> qp, CustomHeader& ch) {
    OdinEvent event;
    event.ack_seq = ch.ack.seq;
    auto skip = [&](const char* reason) {
        event.reason = reason;
        TraceOdin(qp, event);
    };
    if (ch.l3Prot != 0xFC) { skip("nack"); return; }
    uint64_t qp_key = GetQpKey(qp->dip.Get(), qp->sport, qp->dport, qp->m_pg);
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
        psi = std::max(psi, value);
    }
    baseline();
    event.event = "update";
    event.control = m_nic[qp->odin.nic].odin.OnFeedback(
        GetQpKey(qp->dip.Get(), qp->sport, qp->dport, qp->m_pg), psi, ih.HasTbo(),
        m_odinEta, m_odinGamma, m_odinBeta.GetBitRate(), m_minRate.GetBitRate());
    event.next_before_ns = qp->m_nextAvail.GetNanoSeconds();
    ChangeRate(qp, DataRate(event.control.rate));
    event.next_after_ns = qp->m_nextAvail.GetNanoSeconds();
    TraceOdin(qp, event);
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

bool RdmaHw::QueryOdinPathScore(uint64_t qp_key, uint64_t base_rtt_ns, double* score,
                                  bool* tbo, uint64_t* max_age_ns, uint64_t* min_version,
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
    auto path_it = m_odinResourcePaths.find(qp_key);
    if (path_it == m_odinResourcePaths.end() || path_it->second.resources.empty()) {
        if (reason) *reason = "missing_path";
        return false;
    }
    const OdinPathSpec& spec = path_it->second;
    if (spec.mapping_version != m_odinExpectedMappingVersion) {
        if (reason) *reason = "mapping_mismatch";
        return false;
    }
    uint64_t now = Simulator::Now().GetNanoSeconds();
    double max_psi = -1.0;
    bool any_tbo = false;
    uint64_t worst_age = 0;
    uint64_t oldest_epoch = std::numeric_limits<uint64_t>::max();
    OdinResourceSample worst_sample;
    for (const auto& res_key : spec.resources) {
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

uint32_t RdmaHw::ApplyOdinMultiHopResourceFeedback(
    uint64_t version, const std::vector<OdinResourceSample>& samples) {
    NS_ABORT_MSG_IF(!m_odinResourceFeedback || m_cc_mode != CC_MODE_ODIN,
                    "multi-hop resource feedback requires explicit Odin opt-in");
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
