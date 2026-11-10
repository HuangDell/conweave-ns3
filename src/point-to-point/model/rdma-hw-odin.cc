#include "rdma-hw.h"
#include "ns3/simulator.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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
}  // namespace ns3
