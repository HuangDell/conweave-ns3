/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * v4 sflowlet: capacity-proportional flowlet routing.
 *
 * Reuses LetFlow's order-preserving flowlet substrate (per-flow flowlet table,
 * timeout-bounded reaction period, pathId-encoded per-hop outports). The ONLY
 * substantive change vs LetFlow is the path-selection control law at a flowlet
 * boundary: instead of uniform-random, pick the local uplink in proportion to
 * its residual effective capacity (from ResidualCapacityEstimator).
 *
 * Each flowlet stays on a single path -> safe on commodity in-order RoCE NICs
 * (no OOO NIC, no in-network reorder buffer). Reuses ns3::LetflowTag to carry
 * the pathId, so agg/core forwarding is identical to LetFlow.
 *
 * Weight modes (selectable for ablation):
 *   RANDOM     : uniform-random (LetFlow parity; used for Step-1 bring-up)
 *   WEIGHTED   : flowlet-boundary weighted-random, P(path) ∝ ĉ_i  (core, v4 §6.3b)
 *   WCMP       : integer-quantized weights, P(path) ∝ round(ĉ_i)  (v4 §6.3a)
 */

#pragma once

#include <arpa/inet.h>

#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "ns3/event-id.h"
#include "ns3/letflow-routing.h"  // reuse LetflowTag + LETFLOW_NULL
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/residual-capacity-estimator.h"
#include "ns3/settings.h"
#include "ns3/simulator.h"

namespace ns3 {

struct FlowletSwitchEvent {
    uint64_t time_ns;
    uint32_t switch_id;
    uint64_t qpkey;
    uint32_t old_path;
    uint32_t new_path;
    double old_weight;
    double new_weight;
};

class CapacityProportionalRouting : public Object {
    friend class SwitchMmu;
    friend class SwitchNode;

   public:
    enum WeightMode { RANDOM = 0, WEIGHTED = 1, WCMP = 2 };

    CapacityProportionalRouting();

    static TypeId GetTypeId(void);
    static uint64_t GetQpKey(uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg);
    static uint32_t GetOutPortFromPath(const uint32_t& path, const uint32_t& hopCount);
    static void SetOutPortToPath(uint32_t& path, const uint32_t& hopCount, const uint32_t& outPort);
    static uint32_t nFlowletTimeout;
    static std::vector<FlowletSwitchEvent> s_switchLog;
    static bool s_enableSwitchLog;

    /* main function (returns outPort; mirrors LetflowRouting::RouteInput) */
    uint32_t RouteInput(Ptr<Packet> p, CustomHeader ch);

    /* path selection */
    uint32_t GetRandomPath(uint32_t dstToRId);
    uint32_t GetWeightedPath(uint32_t dstToRId);  // honors m_weightMode

    virtual void DoDispose();

    /* SET functions */
    void SetConstants(Time agingTime, Time flowletTimeout);
    void SetSwitchInfo(bool isToR, uint32_t switch_id);
    void SetWeightMode(uint32_t mode) { m_weightMode = mode; }
    void SetEstimator(ResidualCapacityEstimator* est) { m_estimator = est; }

    // periodic aging (keep flowlet table small) — same as LetFlow
    EventId m_agingEvent;
    void AgingEvent();

    // topological info (initialized at setup, like LetFlow's table)
    std::map<uint32_t, std::set<uint32_t> > m_letflowRoutingTable;  // ToRId -> {pathId}

    // residual-capacity estimator for this switch (not owned)
    ResidualCapacityEstimator* m_estimator;

   private:
    bool m_isToR;
    uint32_t m_switch_id;
    uint32_t m_weightMode;

    Time m_agingTime;
    Time m_flowletTimeout;

    std::map<uint64_t, Flowlet*> m_flowletTable;  // QpKey -> Flowlet (at SrcToR)
};

}  // namespace ns3
