#include "int-header.h"
#include "ns3/abort.h"
#include <cstddef>

namespace ns3 {

static_assert(sizeof(IntHop) == 8, "INT hop size");
static_assert(sizeof(IntHeader) == 64, "INT object size including tail padding");
static_assert(offsetof(IntHeader, hop) == 0, "INT hop offset");
static_assert(offsetof(IntHeader, nhop) == 40, "INT hop count offset");
static_assert(offsetof(IntHeader, res_sw) == 42, "INT switch offset");
static_assert(offsetof(IntHeader, res_port) == 52, "INT port offset");
static_assert(offsetof(IntHeader, res_port) + sizeof(IntHeader::res_port) ==
              IntHeader::resourceWireSize, "INT resource wire length");

const uint64_t IntHop::lineRateValues[8] = {25000000000lu, 50000000000lu, 100000000000lu, 200000000000lu, 400000000000lu, 40000000000lu, 0, 0};
uint32_t IntHop::multi = 1;

uint32_t IntHeader::mode = 0;

IntHeader::IntHeader() : nhop(0) {
	for (uint32_t i = 0; i < maxHop; i++) {
		hop[i] = {0};
		res_sw[i] = res_port[i] = 0;
	}
}

uint32_t IntHeader::GetStaticSize(){
	if (mode == 0){
		return sizeof(hop) + sizeof(nhop);
	}else if (mode == 1){
		return sizeof(ts);
	}else if (mode == INT_WITH_RES_ID){
		return resourceWireSize;
	}else {
		return 0;
	}
}

void IntHeader::PushHopWithRes(uint64_t time, uint64_t bytes, uint32_t qlen, uint64_t rate,
                              uint32_t sw, uint32_t port, bool tbo) {
	uint32_t i = GetHopCount();
	NS_ABORT_MSG_IF(i >= maxHop || sw > UINT16_MAX || port >= tboMask,
	                "Odin INT resource exceeds wire field range");
	hop[i].Set(time, bytes, qlen, rate);
	NS_ABORT_MSG_IF(hop[i].GetLineRate() != rate, "unsupported resource INT line rate");
	res_sw[i] = sw;
	res_port[i] = port | (tbo ? tboMask : 0);
	nhop = (nhop & (tboMask | probeMask)) | (i + 1);
	if (tbo) MarkTbo();
}

void IntHeader::PushHop(uint64_t time, uint64_t bytes, uint32_t qlen, uint64_t rate){
	// only do this in INT mode
	if (mode == 0){
		uint32_t idx = GetHopCount() % maxHop;
		hop[idx].Set(time, bytes, qlen, rate);
		nhop = (nhop & tboMask) | (GetHopCount() + 1);
	}
}

void IntHeader::Serialize (Buffer::Iterator start) const{
	Buffer::Iterator i = start;
	if (mode == 0 || mode == INT_WITH_RES_ID){
		for (uint32_t j = 0; j < maxHop; j++){
			i.WriteU32(hop[j].buf[0]);
			i.WriteU32(hop[j].buf[1]);
		}
		i.WriteU16(nhop);
		if (mode == INT_WITH_RES_ID) {
			for (uint32_t j = 0; j < maxHop; ++j) i.WriteU16(res_sw[j]);
			for (uint32_t j = 0; j < maxHop; ++j) i.WriteU16(res_port[j]);
		}
	}else if (mode == 1){
		i.WriteU64(ts);
	}
}

uint32_t IntHeader::Deserialize (Buffer::Iterator start){
	Buffer::Iterator i = start;
	if (mode == 0 || mode == INT_WITH_RES_ID){
		for (uint32_t j = 0; j < maxHop; j++){
			hop[j].buf[0] = i.ReadU32();
			hop[j].buf[1] = i.ReadU32();
		}
		nhop = i.ReadU16();
		if (mode == INT_WITH_RES_ID) {
			for (uint32_t j = 0; j < maxHop; ++j) res_sw[j] = i.ReadU16();
			for (uint32_t j = 0; j < maxHop; ++j) res_port[j] = i.ReadU16();
		}
	}else if (mode == 1){
		ts = i.ReadU64();
	}
	return GetStaticSize();
}

uint64_t IntHeader::GetTs(void){
	if (mode == 1)
		return ts;
	return 0;
}

}
