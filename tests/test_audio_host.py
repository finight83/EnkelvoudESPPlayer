#!/usr/bin/env python3
"""Compile the production queue, discard-decode and DMA-write paths with host fakes."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
net = (root / "EVDPNet.h").read_text()
audio = (root / "EVDPAudio.h").read_text()
state = (root / "EVDPState.h").read_text()
queue = net[net.index("inline bool enqueueConcealedFrame"):
            net.index("// ---- UDP audio drain task")]
discard = audio[audio.index("inline void decodeDiscardedAudio"):
                audio.index("// Hardened flush:")]
count = audio[audio.index("    // Queue-depth telemetry remains useful"):
              audio.index("    static float fade_gain")]
write = audio[audio.index("    // Both DMA paths receive"):
              audio.index("// Keep the stateful decoder")]
flush = audio[audio.index("inline void flushAudioQueueAndRebuffer"):
              audio.index("// ---- Playout-delay slew")]
ota_reset = state[state.index("inline void resetAudioQueueForOta"):
                  state.index("// ---- Stats", state.index("inline void resetAudioQueueForOta"))]
start = audio.index("                int64_t our_local_at_capture =")
end = audio.index("            }\n", start)
latency = audio[start:end]

# Sequence reset and admission must remain inside these locked entry points.
for name, text, allowed in (
    ("EVDPAudio.h", audio, flush),
    ("EVDPNet.h", net, queue),
    ("EVDPState.h", state, ota_reset),
):
    writes = len(re.findall(r"\bseq_initialized\s*=(?!=)", text))
    expected = len(re.findall(r"\bseq_initialized\s*=(?!=)", allowed))
    if name == "EVDPState.h":
        expected += 1  # global initializer, before tasks start
    assert writes == expected, f"unreviewed sequence writer in {name}"
assert "xSemaphoreTake(g_audio_queue_mutex" in flush
assert "xSemaphoreTake(g_audio_queue_mutex" in ota_reset
admit = queue[queue.index("inline void enqueueAudioPacket("):]
assert (admit.index("xSemaphoreTake(g_audio_queue_mutex") <
        admit.index("enqueueAudioPacketLocked(data, len)") <
        admit.index("xSemaphoreGive(g_audio_queue_mutex)"))
epoch = net[net.index("inline void handleServerEpoch"):
            net.index("// ---- Encoder info")]
assert "seq_initialized" not in epoch and "g_force_resync" in epoch
for path in list(root.glob("*.h")) + list(root.glob("*.ino")):
    if path.name not in ("EVDPAudio.h", "EVDPNet.h", "EVDPState.h"):
        assert not re.search(r"\bseq_initialized\s*=(?!=)", path.read_text())

# Decoder creation is before task startup; resets/decodes stay on playback.
playback = audio[audio.index("inline void audioPlaybackTask"):]
assert audio.count("opus_decoder_ctl(") == discard.count("opus_decoder_ctl(") + flush.count("opus_decoder_ctl(") == 2
assert audio.count("opus_decode(") == discard.count("opus_decode(") + playback.count("opus_decode(") == 3
assert audio.count("decodeDiscardedAudio(") == playback.count("decodeDiscardedAudio(") + 1
assert audio.count("flushAudioQueueAndRebuffer(") == playback.count("flushAudioQueueAndRebuffer(") + 1
for path in list(root.glob("*.h")) + list(root.glob("*.ino")):
    if path.name != "EVDPAudio.h":
        assert "opus_decoder_ctl(" not in path.read_text()
        assert "opus_decode(" not in path.read_text()
main = (root / "EVDPMain.h").read_text()
assert main.index("opus_decoder_create(") < main.index("xTaskCreatePinnedToCore(audioPlaybackTask")

source = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "EVDPProtocolCompat.h"
using std::size_t;
constexpr int AUDIO_QUEUE_LEN=32, OPUS_FRAME_SIZE=960, SAMPLE_RATE=48000;
constexpr int AUDIO_MAX_CONCEAL_FRAMES=10, MAX_OPUS_PACKET_LEN=128, CHANNELS=2;
struct AudioMessage { uint8_t payload[128]; uint16_t length; uint32_t arrival_ms, captured_ms, seq; bool conceal; };
std::deque<AudioMessage> q;
void *audio_queue=&q;
std::mutex queueMutex;
void *g_audio_queue_mutex=&queueMutex;
const int portMAX_DELAY=-1, pdTRUE=1;
std::atomic<bool> pauseConceal(false), paused(false), resumeProducer(false), flushWaiting(false);
int xSemaphoreTake(void*, int timeout) {
    if (timeout==0) return queueMutex.try_lock() ? pdTRUE : 0;
    flushWaiting=true; queueMutex.lock(); return pdTRUE;
}
void xSemaphoreGive(void*) { queueMutex.unlock(); }
int pdPASS=1, failSend=0;
size_t uxQueueMessagesWaiting(void*) { return q.size(); }
int xQueueSend(void*, const AudioMessage* msg, int) {
    if (failSend || q.size() >= AUDIO_QUEUE_LEN) return 0;
    q.push_back(*msg);
    if (pauseConceal && msg->conceal) {
        paused=true;
        while (!resumeProducer) std::this_thread::yield();
    }
    return pdPASS;
}
int xQueueReceive(void*, AudioMessage* msg, int) {
    if(q.empty()) return 0;
    *msg=q.front(); q.pop_front(); return pdPASS;
}
void xQueueReset(void*) { q.clear(); }
bool otaNodeUpdating=false, g_udpSocketsReady=true;
uint32_t nowMs=1;
uint32_t millis() { return nowMs++; }
int g_audio_compat_state=AUDIO_COMPAT_COMPATIBLE, g_gap_stat_mux=0;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
bool audio_proto_v2_seen=false, seq_initialized=false, g_force_resync=false;
uint32_t expected_seq=0, last_seq_seen=0, last_conceal_captured_ms=0;
uint32_t ts_prev_audio_arrival_ms=0, ts_last_audio_packet_ms=0;
uint32_t stat_audio_withheld_incompatible=0, stat_audio_withheld_not_registered=0;
uint32_t stat_packets_oversize=0, stat_max_gap_ms_this_sec=0;
uint32_t stat_gap_gt_35ms_total=0, stat_gap_gt_50ms_total=0, stat_gap_gt_100ms_total=0;
uint32_t stat_packets_this_sec=0, stat_packets_dropped=0, stat_seq_gaps=0, stat_seq_missing=0;
uint32_t stat_seq_duplicate=0, stat_seq_reordered=0, stat_frames_discarded=0;
uint32_t stat_frames_concealed=0, stat_seq_received=0, stat_packets_received=0;
''' + ota_reset + queue + r'''
int decoder=1, *opus_decoder=&decoder;
int16_t pcm[1920], *pcm_decode_buffer=pcm;
uint32_t stat_opus_decode_errors=0;
int decodeCalls=0, resetCalls=0, lastLen=-1, decodeResult=960;
const uint8_t *lastPayload=nullptr;
int opus_decode(int*, const uint8_t* payload, int len, int16_t*, int, int) {
    decodeCalls++; lastPayload=payload; lastLen=len; return decodeResult;
}
#define OPUS_RESET_STATE 1
int opus_decoder_ctl(int*, int) { resetCalls++; return 0; }
int32_t g_playout_lead_ms=0;
uint32_t last_node_latency_ms=0, statLastCapturedMs=0, stat_rebuffers=0;
uint32_t ts_last_rebuffer_ms=0, stat_empty_flushes=0;
bool trim_unstable=false;
struct String {
    String(const char*) {}
    String(uint32_t) {}
};
String operator+(String, String) { return String(""); }
void audioLogThrottled(String) {}
''' + discard + flush + r'''
void measureLatency(const AudioMessage& msg, uint32_t sync_loc, uint32_t sync_srv) {
''' + latency + r'''
}
using esp_err_t=int;
using i2s_port_t=int;
constexpr int ESP_OK=0, I2S_NUM_DAC1=0, I2S_NUM_DAC2=1;
#define pdMS_TO_TICKS(x) (x)
bool g_i2s_dac1_ready=true, g_i2s_dac2_ready=true;
int16_t dac1[1940], dac2[1940], *dac1_output_buffer=dac1, *dac2_output_buffer=dac2;
uint32_t stat_i2s_write_errors=0, stat_i2s_timeouts=0;
int stat_queue_error=0, stat_correction_samples=0, audio_queue_target=0;
size_t written[2]={0,0};
int i2s_write(int port, const char*, size_t n, size_t* w, int) {
    *w=n; written[port]+=n; return ESP_OK;
}
void output(int samples_per_channel) {
    int out_samples;
''' + count + write + r'''
void packet(uint32_t seq, uint32_t cap) {
    uint8_t p[13]={0xd0,0xe1,2,0};
    for (int i=0;i<4;i++) { p[4+i]=seq>>(8*i); p[8+i]=cap>>(8*i); }
    p[12]=42; enqueueAudioPacket(p,sizeof(p));
}
void reset() {
    q.clear(); seq_initialized=false; expected_seq=0; last_seq_seen=0;
    last_conceal_captured_ms=0; g_force_resync=false; failSend=0;
}
int main() {
    // A pressure-dropped packet becomes PLC, with its own sequence and time.
    reset(); packet(10,1000);
    AudioMessage filler={};
    while (q.size()<24) q.push_back(filler);
    packet(11,1020); assert(expected_seq==11);
    q.clear(); packet(12,1040);
    assert(q.size()==2 && q[0].conceal && q[0].seq==11 && q[0].captured_ms==1020);
    assert(!q[1].conceal && q[1].seq==12 && expected_seq==13);
    // A full queue after PLC must not commit an unqueued real packet.
    reset(); packet(1,100); q.clear();
    while(q.size()<23) q.push_back(filler);
    packet(11,300);
    assert(q.size()==32 && expected_seq==11 && last_conceal_captured_ms==280);
    q.clear(); packet(12,320);
    assert(q.size()==2 && q[0].conceal && q[0].seq==11 && q[1].seq==12);
    // Excessive loss requires reset rather than silently bypassing PLC.
    reset(); packet(1,100); q.clear(); packet(13,340);
    assert(g_force_resync && q.empty() && expected_seq==2);
    // Sequence wrap and duplicate rejection.
    reset(); packet(0xfffffffeu,100); packet(0,140);
    assert(q.size()==3 && q[1].seq==0xffffffffu && q[1].conceal);
    packet(0,140); assert(q.size()==3);
    // Failed initial send must not create a false baseline.
    reset(); failSend=1; packet(10,100); assert(!seq_initialized);
    failSend=0; packet(11,120); assert(q.size()==1 && !q[0].conceal);
    // Compatibility reset occurs only while admission owns the queue mutex.
    reset(); packet(10,100); g_audio_compat_state=AUDIO_COMPAT_INCOMPATIBLE;
    queueMutex.lock(); packet(11,120); queueMutex.unlock();
    assert(seq_initialized && expected_seq==11);
    packet(11,120); assert(!seq_initialized);
    g_audio_compat_state=AUDIO_COMPAT_COMPATIBLE; q.clear();
    packet(20,300); assert(q.size()==1 && !q[0].conceal);
    // Discards advance Opus with the actual frame or PLC, and reset on error.
    AudioMessage m={}; m.length=1; m.payload[0]=42;
    decodeDiscardedAudio(m); assert(decodeCalls==1 && lastLen==1 && lastPayload==m.payload);
    m.conceal=true; decodeDiscardedAudio(m); assert(lastLen==0 && lastPayload==nullptr);
    decodeResult=-1; decodeDiscardedAudio(m); assert(resetCalls==1 && stat_opus_decode_errors==1);
    // Capture and arrival may lie on opposite sides of local millis wrap.
    m.captured_ms=1000; m.arrival_ms=4;
    measureLatency(m, 0xfffffffau, 1000); assert(last_node_latency_ms==10);
    m.arrival_ms=0xfffffff0u;
    measureLatency(m, 0xfffffffau, 1000); assert(last_node_latency_ms==0);
    // Queue depth never changes the PCM length delivered to either DAC.
    for (int depth : {0, 1, 8, 24}) {
        q.clear(); while ((int)q.size()<depth) q.push_back(filler);
        audio_queue_target=5; written[0]=written[1]=0; output(960);
        size_t expected=960*2*sizeof(int16_t);
        assert(written[0]==expected && written[1]==expected);
        assert(stat_queue_error==depth-5 && stat_correction_samples==0);
    }
    // An in-flight PLC enqueue must finish before flush resets queue/sequence.
    reset(); packet(10,1000); pauseConceal=true; paused=false; resumeProducer=false;
    flushWaiting=false;
    std::thread producer([]{ packet(12,1040); });
    while(!paused) std::this_thread::yield();
    std::thread flusher([]{ flushAudioQueueAndRebuffer("test"); });
    while(!flushWaiting) std::this_thread::yield();
    resumeProducer=true; producer.join(); flusher.join(); pauseConceal=false;
    assert(q.empty() && !seq_initialized && last_conceal_captured_ms==0);
    packet(13,1060); assert(q.size()==1 && !q[0].conceal && q[0].seq==13);
    // OTA reset also waits for an in-flight PLC transaction.
    reset(); packet(10,1000); pauseConceal=true; paused=false; resumeProducer=false;
    flushWaiting=false;
    std::thread producer2([]{ packet(12,1040); });
    while(!paused) std::this_thread::yield();
    std::thread ota([]{ resetAudioQueueForOta(); });
    while(!flushWaiting) std::this_thread::yield();
    resumeProducer=true; producer2.join(); ota.join(); pauseConceal=false;
    assert(q.empty() && !seq_initialized && last_conceal_captured_ms==0);
    otaNodeUpdating=true; packet(14,1080); assert(q.empty());
    otaNodeUpdating=false;
    // A nonblocking producer that loses the mutex must not mutate sequence.
    reset(); packet(20,2000); queueMutex.lock(); packet(21,2020); queueMutex.unlock();
    assert(expected_seq==21); packet(22,2040);
    assert(q.size()==3 && q[1].conceal && q[1].seq==21);
}
'''

binary = root / "tests" / ".audio_host_test"
try:
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-pthread", "-I", str(root),
                    "-x", "c++", "-", "-o", str(binary)],
                   input=source, text=True, check=True)
    subprocess.run([str(binary)], check=True)
    print("Audio host tests passed")
finally:
    binary.unlink(missing_ok=True)
