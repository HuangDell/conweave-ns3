/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * v4 sflowlet: residual effective-capacity estimator (implementation).
 */

#include "ns3/residual-capacity-estimator.h"

#include "ns3/log.h"
#include "ns3/qbb-net-device.h"

NS_LOG_COMPONENT_DEFINE("ResidualCapacityEstimator");

namespace ns3 {

ResidualCapacityEstimator::ResidualCapacityEstimator() {
    m_estTime = MicroSeconds(200);  // data-plane cadence, same order as CONGA DRE
    m_beta = 1.0 / 64.0;            // G1 EWMA note recommendation
    m_persistWindows = 3;          // consecutive low windows to confirm degradation
    m_degradeRatio = 0.85;         // below 85% of nominal counts as "low"
    m_backlogThreshBytes = 0;      // G1 gate: backlogged iff q_bytes > 0
    m_isToR = false;
    m_switch_id = (uint32_t)-1;
}

ResidualCapacityEstimator::~ResidualCapacityEstimator() {}

void ResidualCapacityEstimator::RegisterPort(uint32_t outPort, Ptr<QbbNetDevice> dev,
                                             uint64_t nominalBps) {
    m_devMap[outPort] = dev;
    PortState &st = m_portState[outPort];
    st.nominalBps = nominalBps;
    st.ewmaBps = static_cast<double>(nominalBps);  // start healthy
    st.lastBusyNs = dev ? dev->GetTxBusyTimeNs() : 0;
    st.lastDepartedBytes = dev ? dev->GetTxDepartedBytes() : 0;
}

uint64_t ResidualCapacityEstimator::GetResidualCapacity(uint32_t outPort) const {
    auto it = m_portState.find(outPort);
    if (it == m_portState.end()) return 0;
    if (it->second.nominalBps == 0) return 0;
    return static_cast<uint64_t>(it->second.ewmaBps);
}

bool ResidualCapacityEstimator::IsDegraded(uint32_t outPort) const {
    auto it = m_portState.find(outPort);
    if (it == m_portState.end()) return false;
    return it->second.degraded;
}

void ResidualCapacityEstimator::SetConstants(Time estTime, double beta, uint32_t persistWindows,
                                             double degradeRatio, uint64_t backlogThreshBytes) {
    m_estTime = estTime;
    m_beta = beta;
    m_persistWindows = persistWindows;
    m_degradeRatio = degradeRatio;
    m_backlogThreshBytes = backlogThreshBytes;
}

void ResidualCapacityEstimator::SetSwitchInfo(bool isToR, uint32_t switch_id) {
    m_isToR = isToR;
    m_switch_id = switch_id;
}

void ResidualCapacityEstimator::Start() {
    if (!m_estEvent.IsRunning()) {
        m_estEvent = Simulator::Schedule(m_estTime, &ResidualCapacityEstimator::EstimatorEvent,
                                         this);
    }
}

void ResidualCapacityEstimator::Stop() { m_estEvent.Cancel(); }

void ResidualCapacityEstimator::EstimatorEvent() {
    for (auto &kv : m_devMap) {
        uint32_t outPort = kv.first;
        Ptr<QbbNetDevice> dev = kv.second;
        if (!dev) continue;
        PortState &st = m_portState[outPort];

        uint64_t busyNs = dev->GetTxBusyTimeNs();
        uint64_t departed = dev->GetTxDepartedBytes();
        uint64_t qbytes = dev->GetQueue() ? dev->GetQueue()->GetNBytesTotal() : 0;

        uint64_t dBusy = busyNs - st.lastBusyNs;
        uint64_t dBytes = departed - st.lastDepartedBytes;
        st.lastBusyNs = busyNs;
        st.lastDepartedBytes = departed;

        // Backlog gate (G1): only trust the drain-rate sample when the egress
        // queue was non-empty this window, i.e. the link is the bottleneck.
        // Without backlog, c_inst reflects offered load, not capacity -> hold.
        if (qbytes <= m_backlogThreshBytes || dBusy == 0) {
            continue;
        }

        double cInst = static_cast<double>(dBytes) * 8.0 / (static_cast<double>(dBusy) * 1e-9);

        // EWMA on backlogged samples only.
        st.ewmaBps = (1.0 - m_beta) * st.ewmaBps + m_beta * cInst;
        st.initialized = true;

        // Persistence filter: confirm degradation only after K consecutive
        // low windows, to drop transition-boundary mistakes (G1 classifier).
        if (st.nominalBps > 0 &&
            st.ewmaBps < m_degradeRatio * static_cast<double>(st.nominalBps)) {
            if (st.degradeRun < m_persistWindows) st.degradeRun++;
            if (st.degradeRun >= m_persistWindows) st.degraded = true;
        } else {
            st.degradeRun = 0;
            st.degraded = false;
        }
    }
    m_estEvent = Simulator::Schedule(m_estTime, &ResidualCapacityEstimator::EstimatorEvent, this);
}

}  // namespace ns3
