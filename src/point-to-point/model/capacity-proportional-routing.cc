/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * v4 sflowlet: capacity-proportional flowlet routing (implementation).
 *
 * Structure mirrors letflow-routing.cc; the divergence is GetWeightedPath().
 */

#include "ns3/capacity-proportional-routing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "assert.h"
#include "ns3/assert.h"
#include "ns3/ipv4-header.h"
#include "ns3/log.h"
#include "ns3/nstime.h"

NS_LOG_COMPONENT_DEFINE("CapacityProportionalRouting");

namespace ns3 {

uint32_t CapacityProportionalRouting::nFlowletTimeout = 0;

CapacityProportionalRouting::CapacityProportionalRouting() {
    m_estimator = nullptr;
    m_isToR = false;
    m_switch_id = (uint32_t)-1;
    m_weightMode = WEIGHTED;  // core mechanism by default
    m_flowletTimeout = Time(MicroSeconds(100));
    m_agingTime = Time(MilliSeconds(10));
}

uint64_t CapacityProportionalRouting::GetQpKey(uint32_t dip, uint16_t sport, uint16_t dport,
                                               uint16_t pg) {
    return ((uint64_t)dip << 32) | ((uint64_t)sport << 16) | (uint64_t)pg | (uint64_t)dport;
}

TypeId CapacityProportionalRouting::GetTypeId(void) {
    static TypeId tid = TypeId("ns3::CapacityProportionalRouting")
                            .SetParent<Object>()
                            .AddConstructor<CapacityProportionalRouting>();
    return tid;
}

void CapacityProportionalRouting::SetSwitchInfo(bool isToR, uint32_t switch_id) {
    m_isToR = isToR;
    m_switch_id = switch_id;
}

uint32_t CapacityProportionalRouting::RouteInput(Ptr<Packet> p, CustomHeader ch) {
    Time now = Simulator::Now();

    if (!m_agingEvent.IsRunning()) {
        m_agingEvent = Simulator::Schedule(m_agingTime, &CapacityProportionalRouting::AgingEvent,
                                           this);
    }

    assert(Settings::hostIp2SwitchId.find(ch.sip) != Settings::hostIp2SwitchId.end());
    assert(Settings::hostIp2SwitchId.find(ch.dip) != Settings::hostIp2SwitchId.end());
    uint32_t srcToRId = Settings::hostIp2SwitchId[ch.sip];
    uint32_t dstToRId = Settings::hostIp2SwitchId[ch.dip];

    NS_ASSERT_MSG(srcToRId != dstToRId, "Should not be in the same pod");
    NS_ASSERT_MSG(ch.l3Prot == 0x11, "Only supports UDP data packets");
    uint64_t qpkey = GetQpKey(ch.dip, ch.udp.sport, ch.udp.dport, ch.udp.pg);

    LetflowTag letflowTag;
    bool found = p->PeekPacketTag(letflowTag);

    if (m_isToR) {     // ToR switch
        if (!found) {  // sender-side
            struct Flowlet* flowlet = NULL;
            auto flowletItr = m_flowletTable.find(qpkey);
            uint32_t selectedPath;

            // 1) flowlet already exists
            if (flowletItr != m_flowletTable.end()) {
                flowlet = flowletItr->second;
                NS_ASSERT_MSG(flowlet != NULL, "flowlet is not correctly registered");

                if (now - flowlet->_activeTime <= m_flowletTimeout) {  // no timeout: keep path
                    flowlet->_activeTime = now;
                    flowlet->_nPackets++;

                    selectedPath = flowlet->_PathId;
                    uint32_t outPort = GetOutPortFromPath(selectedPath, 0);
                    letflowTag.SetPathId(selectedPath);
                    letflowTag.SetHopCount(0);
                    p->AddPacketTag(letflowTag);
                    return outPort;
                }

                /*---- Flowlet Timeout: pick a new path by capacity weight ----*/
                selectedPath = GetWeightedPath(dstToRId);
                CapacityProportionalRouting::nFlowletTimeout++;

                flowlet->_activatedTime = now;
                flowlet->_activeTime = now;
                flowlet->_nPackets++;
                flowlet->_PathId = selectedPath;

                letflowTag.SetPathId(selectedPath);
                letflowTag.SetHopCount(0);
                p->AddPacketTag(letflowTag);
                return GetOutPortFromPath(selectedPath, letflowTag.GetHopCount());
            }
            // 2) flowlet does not exist (first packet of flow)
            selectedPath = GetWeightedPath(dstToRId);
            struct Flowlet* newFlowlet = new Flowlet;
            newFlowlet->_activeTime = now;
            newFlowlet->_activatedTime = now;
            newFlowlet->_nPackets = 1;
            newFlowlet->_PathId = selectedPath;
            m_flowletTable[qpkey] = newFlowlet;

            letflowTag.SetPathId(selectedPath);
            letflowTag.SetHopCount(0);
            p->AddPacketTag(letflowTag);
            return GetOutPortFromPath(selectedPath, letflowTag.GetHopCount());
        }
        /*---- receiver-side ----*/
        p->RemovePacketTag(letflowTag);
        return LETFLOW_NULL;
    } else {  // agg/core switch
        NS_ASSERT_MSG(found, "If not ToR (leaf), letflowTag should be found");
        uint32_t hopCount = letflowTag.GetHopCount() + 1;
        letflowTag.SetHopCount(hopCount);

        uint32_t outPort = GetOutPortFromPath(letflowTag.GetPathId(), hopCount);

        LetflowTag temp_tag;
        p->RemovePacketTag(temp_tag);
        p->AddPacketTag(letflowTag);
        return outPort;
    }
    NS_ASSERT_MSG("false", "This should not be occured");
}

// uniform random (LetFlow parity)
uint32_t CapacityProportionalRouting::GetRandomPath(uint32_t dstToRId) {
    auto pathItr = m_letflowRoutingTable.find(dstToRId);
    assert(pathItr != m_letflowRoutingTable.end());
    auto innerPathItr = pathItr->second.begin();
    std::advance(innerPathItr, rand() % pathItr->second.size());
    return *innerPathItr;
}

// capacity-proportional path selection (honors m_weightMode)
uint32_t CapacityProportionalRouting::GetWeightedPath(uint32_t dstToRId) {
    auto pathItr = m_letflowRoutingTable.find(dstToRId);
    assert(pathItr != m_letflowRoutingTable.end());
    const std::set<uint32_t>& paths = pathItr->second;

    if (m_weightMode == RANDOM || m_estimator == nullptr || paths.empty()) {
        return GetRandomPath(dstToRId);
    }

    // weight per candidate path = residual capacity of its LOCAL uplink (hop0).
    std::vector<uint32_t> pathVec;
    std::vector<double> weights;
    pathVec.reserve(paths.size());
    weights.reserve(paths.size());
    double total = 0.0;
    for (uint32_t pathId : paths) {
        uint32_t localPort = GetOutPortFromPath(pathId, 0);
        double w = static_cast<double>(m_estimator->GetResidualCapacity(localPort));
        if (w <= 0.0) w = 1.0;  // never starve a path completely (avoid div-by-0)

        if (m_weightMode == WCMP) {
            // integer-quantized weight: round to 100Mbps units (coarse, Tofino-friendly).
            w = std::max(1.0, std::floor(w / 1e8));
        }
        pathVec.push_back(pathId);
        weights.push_back(w);
        total += w;
    }

    // weighted-random draw (flowlet-boundary): P(path) ∝ weight.
    double r = (static_cast<double>(rand()) / (static_cast<double>(RAND_MAX) + 1.0)) * total;
    double acc = 0.0;
    for (size_t i = 0; i < pathVec.size(); i++) {
        acc += weights[i];
        if (r < acc) return pathVec[i];
    }
    return pathVec.back();  // FP guard
}

uint32_t CapacityProportionalRouting::GetOutPortFromPath(const uint32_t& path,
                                                         const uint32_t& hopCount) {
    return ((uint8_t*)&path)[hopCount];
}

void CapacityProportionalRouting::SetOutPortToPath(uint32_t& path, const uint32_t& hopCount,
                                                   const uint32_t& outPort) {
    ((uint8_t*)&path)[hopCount] = outPort;
}

void CapacityProportionalRouting::SetConstants(Time agingTime, Time flowletTimeout) {
    m_agingTime = agingTime;
    m_flowletTimeout = flowletTimeout;
}

void CapacityProportionalRouting::DoDispose() {
    for (auto i : m_flowletTable) {
        delete (i.second);
    }
    m_agingEvent.Cancel();
}

void CapacityProportionalRouting::AgingEvent() {
    auto now = Simulator::Now();
    auto itr = m_flowletTable.begin();
    while (itr != m_flowletTable.end()) {
        if (now - ((itr->second)->_activeTime) > m_agingTime) {
            itr = m_flowletTable.erase(itr);
        } else {
            ++itr;
        }
    }
    m_agingEvent = Simulator::Schedule(m_agingTime, &CapacityProportionalRouting::AgingEvent, this);
}

}  // namespace ns3
