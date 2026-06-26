/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * v4 sflowlet: residual effective-capacity estimator.
 *
 * Online (in-switch) port of the G1 offline signal: the backlogged service rate
 *   c_eff = Δtx_departed_bytes * 8 / Δtx_busy_ns
 * gated on egress backlog (queue non-empty). A healthy link drains at nominal
 * line rate whenever it is the bottleneck; a degraded link drains at c_eff < C.
 * This separates persistent capacity degradation from transient congestion
 * (which inflates queue/RTT but NOT the backlogged drain rate).
 *
 * One estimator instance per ToR switch. It is fed by a periodic timer that
 * reads the per-port QbbNetDevice serialization counters added in G1.
 */

#pragma once

#include <cstdint>
#include <map>

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"

namespace ns3 {

class QbbNetDevice;

/**
 * @brief Estimates each local uplink's residual effective capacity (bps) from
 * the backlogged service-rate signal. Scope: directly-measurable local egress
 * port only (v4 §6.2 phase one). Remote-hop degradation is out of scope here.
 */
class ResidualCapacityEstimator {
   public:
    ResidualCapacityEstimator();

    /** Register a local uplink: device handle + NOMINAL line rate (bps).
     * Nominal is captured at setup and never changes, so degradation (which
     * lowers the device's current DataRate) shows up as estimate < nominal. */
    void RegisterPort(uint32_t outPort, Ptr<QbbNetDevice> dev, uint64_t nominalBps);

    /** Residual effective capacity (bps) for a local uplink. Returns nominal
     * for unregistered/never-backlogged ports (treated as healthy). */
    uint64_t GetResidualCapacity(uint32_t outPort) const;

    /** Whether the port is currently classified as persistently degraded
     * (passed the persistence filter). */
    bool IsDegraded(uint32_t outPort) const;

    void SetConstants(Time estTime, double beta, uint32_t persistWindows,
                      double degradeRatio, uint64_t backlogThreshBytes);
    void SetSwitchInfo(bool isToR, uint32_t switch_id);

    /** Start the periodic estimator timer (idempotent). */
    void Start();
    void Stop();

   private:
    void EstimatorEvent();

    struct PortState {
        uint64_t nominalBps = 0;
        uint64_t lastBusyNs = 0;
        uint64_t lastDepartedBytes = 0;
        double ewmaBps = 0.0;     // residual-capacity estimate (bps)
        uint32_t degradeRun = 0;  // consecutive windows below degradeRatio*nominal
        bool degraded = false;    // persistence-filtered classification
        bool initialized = false;
    };

    std::map<uint32_t, Ptr<QbbNetDevice> > m_devMap;
    std::map<uint32_t, PortState> m_portState;

    Time m_estTime;                // sampling/update period (default 200us)
    double m_beta;                 // EWMA weight on new sample (default 1/64)
    uint32_t m_persistWindows;     // consecutive windows to confirm degradation
    double m_degradeRatio;         // below this fraction of nominal counts as low
    uint64_t m_backlogThreshBytes; // backlog gate (G1: q_bytes > 0)

    EventId m_estEvent;
    bool m_isToR;
    uint32_t m_switch_id;
};

}  // namespace ns3
