#ifndef NATIVE_PATH_DIAGNOSTICS_H
#define NATIVE_PATH_DIAGNOSTICS_H

#include "ns3/custom-header.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"

#include <cstdint>
#include <string>

namespace ns3 {

class NativePathDiagnostics {
  public:
    static bool Open(const std::string& lb, const std::string& path_counter_file,
                     const std::string& event_file, Time interval);
    static void Start(Time start, Time stop);
    static void Close();
    static bool IsEnabled();

    static void RegisterPath(uint32_t source_tor, uint32_t destination_tor,
                             uint32_t path_id);
    static void RecordAssignment(Ptr<Packet> packet, const CustomHeader& header,
                                 uint32_t source_tor, uint32_t destination_tor,
                                 uint32_t path_id);
    static void RecordDecision(uint32_t source_tor, uint32_t destination_tor,
                               uint32_t path_id);
    static void RecordEvent(uint32_t source_tor, uint32_t destination_tor,
                            const CustomHeader& header, const char* event,
                            int64_t old_path_id, int64_t new_path_id,
                            uint64_t signal_value, const char* reason);
    static void RecordTx(Ptr<Packet> packet);
    static void ForgetPacket(Ptr<Packet> packet);
};

}  // namespace ns3

#endif  // NATIVE_PATH_DIAGNOSTICS_H
