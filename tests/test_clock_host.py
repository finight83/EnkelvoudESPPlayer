#!/usr/bin/env python3
"""Exercise production clock-sync paths with a fake UDP socket and JSON."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
net = (root / "EVDPNet.h").read_text()
start = net.index('    if (strcmp(typeStr, "time_sync") == 0)')
end = net.index("    // ---------- host_audio_sync:", start)
oneway = net[start:end]
start = net.index("inline void pollClockSyncResponse()")
end = net.index("// ---- Concealment frame enqueue", start)
response = net[start:end]

source = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
struct String {
    std::string s;
    String(const char* x):s(x) {}
    String(long x):s(std::to_string(x)) {}
    String(unsigned long x):s(std::to_string(x)) {}
    String(int x):s(std::to_string(x)) {}
};
String operator+(const String& a, const String& b) { return String((a.s+b.s).c_str()); }
void log_message(String) {}
void log_warn(String) {}
struct Value {
    int64_t value=0; bool present=false;
    int operator|(int x) const { return present ? (int)value : x; }
    unsigned long operator|(unsigned long x) const { return present ? (unsigned long)value : x; }
    bool operator|(bool x) const { return present ? value != 0 : x; }
    const char* operator|(const char* x) const { return present ? "sync_resp" : x; }
};
struct Doc {
    std::map<std::string,Value> values;
    Value& operator[](const char* k) { return values[k]; }
    bool containsKey(const char* k) { return values.count(k)!=0; }
};
template<int N> using StaticJsonDocument=Doc;
Doc doc, incoming;
struct DeserializationError { operator bool() const { return false; } const char* c_str() const { return ""; } };
uint32_t nowMs=0;
uint32_t millis() { return nowMs; }
DeserializationError deserializeJson(Doc& target, const char*, size_t) {
    target=incoming; nowMs+=50; return {};
}
struct IPAddress { bool operator!=(const IPAddress&) const { return false; } String toString() { return String("server"); } };
IPAddress serverIP;
struct Socket {
    int parsePacket() { return 1; }
    IPAddress remoteIP() { return {}; }
    int read(uint8_t* b, size_t) { *b=0; return 1; }
} clockSyncUdp;
int gPlayerLogLevel=0;
bool g_have_rtt_sync=false, server_clock_synced=false, g_force_resync=false;
int64_t server_clock_offset_ms=0;
int32_t server_clock_jitter_ms=0;
uint32_t stat_clock_resyncs=0, g_current_offset_rtt=UINT32_MAX;
uint32_t stat_clock_sync_rtt_ms=UINT32_MAX, g_pending_sync_client_tx_ms=0, ts_last_sync_req_sent_ms=0;
bool g_sync_req_in_flight=false;
const int CLOCK_RESYNC_JUMP_MS=1500, CLOCK_DRIFT_REFRESH_MS=4, AUDIO_PROTO_VERSION=2;
const int SYNC_RING_SIZE=8;
struct SyncSample { uint32_t rtt; int64_t offset; bool valid; };
SyncSample g_syncRing[SYNC_RING_SIZE]={};
int g_syncRingHead=0, g_syncRingCount=0;
void setServerClockOffset(int64_t x) { server_clock_offset_ms=x; }
void handleServerEpoch(uint32_t) {}
void timeSync() { const char* typeStr="time_sync";
''' + oneway + r'''
}
''' + response + r'''
void set(Doc& d, const char* key, int64_t val) { d[key].present=true; d[key].value=val; }
void reply(uint32_t tx, uint32_t rx, uint32_t srx, uint32_t stx) {
    incoming=Doc(); set(incoming,"type",1); set(incoming,"protoVersion",2);
    set(incoming,"clientTxMs",tx); set(incoming,"serverRxMs",srx); set(incoming,"serverTxMs",stx);
    nowMs=rx; g_pending_sync_client_tx_ms=tx; g_sync_req_in_flight=true;
    pollClockSyncResponse();
}
int main() {
    // Both kinds of one-way broadcast are fallback only after RTT lock.
    set(doc,"serverMs",6000); set(doc,"authoritative",1); nowMs=1000;
    g_have_rtt_sync=true; server_clock_synced=true; server_clock_offset_ms=100;
    timeSync(); assert(server_clock_offset_ms==100 && !g_force_resync);
    set(doc,"authoritative",0); timeSync(); assert(server_clock_offset_ms==100);
    // Before RTT, a one-way sample may still seed startup.
    g_have_rtt_sync=false; server_clock_synced=false; timeSync();
    assert(server_clock_synced && server_clock_offset_ms==5000);
    // Parsing delay must not be added to the measured receive timestamp.
    server_clock_synced=false; reply(100,120,1100,1100);
    assert(g_have_rtt_sync && server_clock_offset_ms==990 && stat_clock_sync_rtt_ms==20);
    // Replayed response with no request in flight cannot alter the lock.
    auto old=server_clock_offset_ms; nowMs=200; pollClockSyncResponse();
    assert(server_clock_offset_ms==old);
    // A same-RTT newer ring entry wins, allowing drift refresh.
    server_clock_offset_ms=1000; g_current_offset_rtt=10; g_syncRingCount=1;
    g_syncRingHead=1; g_syncRing[0]={10,1000,true};
    reply(200,210,1215,1215);
    assert(server_clock_offset_ms==1010);
}
'''

binary = root / "tests" / ".clock_host_test"
try:
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-x", "c++", "-",
                    "-o", str(binary)], input=source, text=True, check=True)
    subprocess.run([str(binary)], check=True)
    print("Clock host tests passed")
finally:
    binary.unlink(missing_ok=True)
