#ifndef CONWEAVE_CAUSAL_TRACE_H
#define CONWEAVE_CAUSAL_TRACE_H

#include <cstdint>
#include <string>
#include "ns3/custom-header.h"
#include "ns3/packet.h"
#include "ns3/rdma-queue-pair.h"

namespace ns3 {

// Observation only: never schedules events, draws randomness, or changes packets.
// Per-packet state belongs to the offline analyzer. Disabled calls return immediately.
class ConWeaveCausalTrace {
  public:
    static bool Open(const std::string& path);
    static void Close();
    static bool Enabled();
    static void Packet(const char* event, uint32_t node, Ptr<const ns3::Packet> packet,
                       const CustomHeader& header, uint64_t voq = 0,
                       uint64_t value = 0, uint64_t aux = 0, const char* reason = "none");
    static void Wire(uint32_t node, Ptr<const ns3::Packet> packet);
    static void Drop(uint32_t node, Ptr<const ns3::Packet> packet, const char* reason);
    static void Sender(const char* event, uint32_t node, Ptr<RdmaQueuePair> qp,
                       uint64_t value = 0, uint64_t aux = 0);
    static uint64_t CreateVoq(uint32_t node, Ptr<const ns3::Packet> packet,
                              const CustomHeader& header, uint64_t deadline,
                              uint64_t phase0Tx, uint64_t phase0Rx, const char* reason);
    static void UpdateVoq(uint64_t voq, uint64_t deadline, uint64_t phase0Tx,
                          uint64_t phase0Rx, const char* reason, uint32_t packets,
                          uint32_t triggerEpoch);
    static void Voq(const char* event, uint64_t voq, uint32_t packets);
    static void DeleteVoq(uint64_t voq);
};

}  // namespace ns3
#endif
