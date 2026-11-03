#include "native-path-diagnostics.h"

#include "ns3/flow-id-num-tag.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace ns3 {

namespace {

struct PathKey {
    uint32_t source_tor;
    uint32_t destination_tor;
    uint32_t path_id;

    bool operator<(const PathKey& other) const {
        return std::tie(source_tor, destination_tor, path_id) <
               std::tie(other.source_tor, other.destination_tor, other.path_id);
    }
};

struct PathCounters {
    uint64_t assigned_data_bytes = 0;
    uint64_t tx_data_bytes = 0;
    uint64_t decision_count = 0;
    uint64_t assigned_attempt_data_bytes = 0;
    uint64_t retrans_assigned_data_bytes = 0;
};

struct FlowKey {
    int32_t app_id;
    uint32_t source_ip;
    uint32_t destination_ip;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t priority_group;

    bool operator<(const FlowKey& other) const {
        return std::tie(app_id, source_ip, destination_ip, source_port,
                        destination_port, priority_group) <
               std::tie(other.app_id, other.source_ip, other.destination_ip,
                        other.source_port, other.destination_port,
                        other.priority_group);
    }
};

struct PacketAssignment {
    PathKey path;
    uint32_t payload_bytes;
};

bool g_enabled = false;
std::string g_lb;
FILE* g_path_counter_file = NULL;
FILE* g_event_file = NULL;
FILE* g_path_map_file = NULL;
std::set<PathKey> g_registered_paths;
std::set<FlowKey> g_ecmp_flows;
Time g_interval = NanoSeconds(10000);
uint64_t g_last_sample_ns = 0;
std::map<PathKey, PathCounters> g_path_counters;
std::map<FlowKey, uint64_t> g_highest_assigned_end_seq;
std::unordered_map<uint64_t, PacketAssignment> g_packet_assignments;

uint32_t PayloadBytes(Ptr<Packet> packet) {
    uint32_t header_bytes = CustomHeader::GetStaticWholeHeaderSize();
    return packet->GetSize() > header_bytes ? packet->GetSize() - header_bytes : 0;
}

void WriteCounters() {
    if (!g_enabled || !g_path_counter_file) {
        return;
    }
    uint64_t now_ns = Simulator::Now().GetNanoSeconds();
    for (const auto& entry : g_path_counters) {
        fprintf(g_path_counter_file, "%lu,%s,%u,%u,%u,%lu,%lu,%lu,%lu,%lu\n",
                now_ns, g_lb.c_str(), entry.first.source_tor,
                entry.first.destination_tor, entry.first.path_id,
                entry.second.assigned_data_bytes, entry.second.tx_data_bytes,
                entry.second.decision_count,
                entry.second.assigned_attempt_data_bytes,
                entry.second.retrans_assigned_data_bytes);
    }
    g_last_sample_ns = now_ns;
}

void SampleCounters() {
    WriteCounters();
    if (g_enabled) {
        Simulator::Schedule(g_interval, &SampleCounters);
    }
}

}  // namespace

bool NativePathDiagnostics::Open(const std::string& lb,
                                 const std::string& path_counter_file,
                                 const std::string& event_file, Time interval,
                                 const std::string& path_map_file) {
    Close();
    g_lb = lb;
    g_interval = interval;
    g_path_counter_file = fopen(path_counter_file.c_str(), "w");
    g_event_file = fopen(event_file.c_str(), "w");
    if (!path_map_file.empty()) {
        g_path_map_file = fopen(path_map_file.c_str(), "w");
    }
    if (!g_path_counter_file || !g_event_file || (!path_map_file.empty() && !g_path_map_file)) {
        Close();
        return false;
    }
    fprintf(g_path_counter_file,
            "time_ns,lb,source_tor,destination_tor,path_id,assigned_data_bytes,"
            "tx_data_bytes,decision_count,assigned_attempt_data_bytes,"
            "retrans_assigned_data_bytes\n");
    fprintf(g_event_file,
            "time_ns,lb,source_tor,destination_tor,flow_key,event,old_path_id,"
            "new_path_id,signal_value,reason\n");
    if (g_path_map_file) {
        fprintf(g_path_map_file, "lb,source_tor,destination_tor,path_id,out_port,next_node\n");
    }
    g_enabled = true;
    return true;
}

void NativePathDiagnostics::Start(Time start, Time stop) {
    if (!g_enabled) {
        return;
    }
    (void)stop;
    Simulator::Schedule(start, &SampleCounters);
}

void NativePathDiagnostics::Close() {
    if (g_enabled && Simulator::Now().GetNanoSeconds() != g_last_sample_ns) {
        WriteCounters();
    }
    g_enabled = false;
    if (g_path_counter_file) {
        fclose(g_path_counter_file);
        g_path_counter_file = NULL;
    }
    if (g_event_file) {
        fclose(g_event_file);
        g_event_file = NULL;
    }
    if (g_path_map_file) {
        fclose(g_path_map_file);
        g_path_map_file = NULL;
    }
    g_registered_paths.clear();
    g_ecmp_flows.clear();
    g_lb.clear();
    g_path_counters.clear();
    g_highest_assigned_end_seq.clear();
    g_packet_assignments.clear();
    g_last_sample_ns = 0;
}

bool NativePathDiagnostics::IsEnabled() { return g_enabled; }

void NativePathDiagnostics::RegisterPath(uint32_t source_tor,
                                         uint32_t destination_tor,
                                         uint32_t path_id, uint32_t out_port, uint32_t next_node) {
    if (!g_enabled) {
        return;
    }
    PathKey key{source_tor, destination_tor, path_id};
    g_path_counters[key];
    if (g_registered_paths.insert(key).second && g_path_map_file) {
        fprintf(g_path_map_file, "%s,%u,%u,%u,%u,%u\n", g_lb.c_str(), source_tor,
                destination_tor, path_id, out_port, next_node);
    }
}

void NativePathDiagnostics::RecordEcmpAssignment(Ptr<Packet> packet, const CustomHeader& header,
                                                 uint32_t source_tor, uint32_t destination_tor,
                                                 uint32_t out_port) {
    if (!g_enabled || header.l3Prot != 0x11) return;
    FlowIDNUMTag tag;
    int32_t app = packet->PeekPacketTag(tag) ? tag.GetId() : -1;
    FlowKey key{app, header.sip, header.dip, header.udp.sport, header.udp.dport, header.udp.pg};
    if (g_ecmp_flows.insert(key).second) {
        RecordDecision(source_tor, destination_tor, out_port);
        RecordEvent(source_tor, destination_tor, header, "flow_start", -1, out_port, 0, "ecmp_hash");
    }
    RecordAssignment(packet, header, source_tor, destination_tor, out_port);
}

void NativePathDiagnostics::RecordAssignment(Ptr<Packet> packet,
                                             const CustomHeader& header,
                                             uint32_t source_tor,
                                             uint32_t destination_tor,
                                             uint32_t path_id) {
    if (!g_enabled || header.l3Prot != 0x11) {
        return;
    }
    uint32_t payload_bytes = PayloadBytes(packet);
    PathKey path = {source_tor, destination_tor, path_id};
    PathCounters& counters = g_path_counters[path];
    counters.assigned_attempt_data_bytes += payload_bytes;

    FlowIDNUMTag flow_tag;
    int32_t app_id = packet->PeekPacketTag(flow_tag) ? flow_tag.GetId() : -1;
    FlowKey flow = {app_id, header.sip, header.dip, header.udp.sport,
                    header.udp.dport, header.udp.pg};
    uint64_t begin = header.udp.seq;
    uint64_t end = begin + payload_bytes;
    uint64_t& highest_end = g_highest_assigned_end_seq[flow];
    uint64_t unique_bytes = end > highest_end ? end - std::max(begin, highest_end) : 0;
    highest_end = std::max(highest_end, end);
    counters.assigned_data_bytes += unique_bytes;
    counters.retrans_assigned_data_bytes += payload_bytes - unique_bytes;
    g_packet_assignments[packet->GetUid()] = PacketAssignment{path, payload_bytes};
}

void NativePathDiagnostics::RecordDecision(uint32_t source_tor,
                                           uint32_t destination_tor,
                                           uint32_t path_id) {
    if (!g_enabled) {
        return;
    }
    g_path_counters[PathKey{source_tor, destination_tor, path_id}].decision_count++;
}

void NativePathDiagnostics::RecordEvent(uint32_t source_tor,
                                        uint32_t destination_tor,
                                        const CustomHeader& header,
                                        const char* event, int64_t old_path_id,
                                        int64_t new_path_id,
                                        uint64_t signal_value,
                                        const char* reason) {
    if (!g_enabled || !g_event_file) {
        return;
    }
    uint16_t source_port = header.l3Prot == 0x11 ? header.udp.sport : header.ack.sport;
    uint16_t destination_port =
        header.l3Prot == 0x11 ? header.udp.dport : header.ack.dport;
    uint16_t priority_group = header.l3Prot == 0x11 ? header.udp.pg : header.ack.pg;
    fprintf(g_event_file,
            "%lu,%s,%u,%u,%u:%u:%u:%u:%u,%s,%ld,%ld,%lu,%s\n",
            Simulator::Now().GetNanoSeconds(), g_lb.c_str(), source_tor,
            destination_tor, header.sip, header.dip, source_port,
            destination_port, priority_group, event, old_path_id,
            new_path_id, signal_value, reason);
}

void NativePathDiagnostics::RecordTx(Ptr<Packet> packet) {
    if (!g_enabled) {
        return;
    }
    auto assignment = g_packet_assignments.find(packet->GetUid());
    if (assignment == g_packet_assignments.end()) {
        return;
    }
    g_path_counters[assignment->second.path].tx_data_bytes +=
        assignment->second.payload_bytes;
    g_packet_assignments.erase(assignment);
}

void NativePathDiagnostics::ForgetPacket(Ptr<Packet> packet) {
    if (g_enabled) {
        g_packet_assignments.erase(packet->GetUid());
    }
}

}  // namespace ns3
