#pragma once
// ============================================================================
//  EVDPState.h - Mutable globals
// ============================================================================
// EVDPFilters.h must come in BEFORE the array declaration below, so include
// it here directly rather than relying on it being dragged in transitively.
#include "EVDPFilters.h"

// [v1.1.2-config-inlined] EVDPConfig.h was merged into EnkelvoudESPPlayer.ino.
// The .ino guarantees its content is visible here before this header is
// parsed. Do not re-add `#include "EVDPConfig.h"` unless the file is
// reintroduced.
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <opus.h>
#include <vector>
#include <freertos/semphr.h>

// ---- Web server + DNS ------------------------------------------------------
WebServer  server(80);
DNSServer  dnsServer;

// Set true the first time registerAppRoutes()/registerCommonRoutes() runs.
// Guards against a second registration when startCaptivePortal() is called
// after setup() already ran (STA → AP transition via the reset button):
// WebServer::on() appends handlers rather than replacing them, so a second
// pass would leave duplicate entries in the route table.
bool       gRoutesRegistered = false;
// [web-server-begun] Set true the first time server.begin() is called.
// startCaptivePortal() is entered from setup() (fresh boot, server not
// yet begun) OR from networkTask() after a GPIO1 hold (server already
// begun by setup's STA path). A second begin() is a no-op on most
// cores but leaks the socket on some arduino-esp32 builds.
bool       g_web_server_started = false;

// ---- Serial log gating -----------------------------------------------------
// 3 = verbose (boot only). 1 = warnings/errors/non-optimal only (steady state).
// log_message() is gated by this; log_warn() always prints to serial. Both
// still append to system_logs so /api/logs on the status page shows the full
// history regardless of the gate. setup() ends with gPlayerLogLevel = 1.
volatile uint8_t gPlayerLogLevel = 3;

// ---- Preferences -----------------------------------------------------------
Preferences preferences;
// [prefs-mutex] `Preferences` is a single global object whose begin()/end()
// pairs use an internal NVS handle. Two tasks calling begin() concurrently
// leak the first handle and double-close on end(), silently failing all
// subsequent putX() calls. Every begin()/end() pair must hold this mutex.
SemaphoreHandle_t g_prefs_mutex = nullptr;

// ---- Opus decoder ----------------------------------------------------------
OpusDecoder* opus_decoder = nullptr;

// ---- UDP sockets -----------------------------------------------------------
WiFiUDP audioUdp;      // inbound multicast audio, port 4202
WiFiUDP ctrlUdp;       // inbound multicast config, port 4203
WiFiUDP clockSyncUdp;  // outbound request / inbound response, g_clock_sync_port
IPAddress multicastGroup(MULTICAST_GROUP_OCTETS);

// ---- Saved Wi-Fi / identity ------------------------------------------------
char wifi_ssid[64] = "";
char wifi_pass[64] = "";
char friendly_name[64] = "Enkelvoud Node";
char theme_mode[16] = "theme-black";
bool use_static_ip = false;
char static_ip_str[16] = "";
char static_gw_str[16] = "";
char static_sn_str[16] = "255.255.255.0";
char static_dns_str[16] = "";

// ---- I2S pin assignment (fixed) --------------------------------------------
// The Hardware tab was removed: these are now compile-time fixed.
//   DAC1 = LRC 4  / DOUT 5  / BCLK 6
//   DAC2 = LRC 17 / DOUT 18 / BCLK 8
const int pin_lrc_1  = 4;
const int pin_dout_1 = 5;
const int pin_bclk_1 = 6;
const int pin_lrc_2  = 17;
const int pin_dout_2 = 18;
const int pin_bclk_2 = 8;

// ---- Speaker array + filters -----------------------------------------------
// SpeakerFilter is now a complete type (see include above), so this is fine.
SpeakerConfig speakers[4] = {
    {"Tweeter", 20, 2000, 20000, 0},
    {"Mid",     20, 200,  5000,  1},
    {"Full",    20, 0,    20000, 0},
    {"Sub",     20, 0,    300,   1}
};
SpeakerFilter speaker_filters[4];

// ---- Live audio state ------------------------------------------------------
// These are all written by udpAudioTask on Core 0 (applyCtrlJson) and read
// by audioPlaybackTask on Core 1 (process_and_output_audio). Without
// volatile, -O2 is free to cache the read in a register inside the audio
// task's for(;;) loop, and a mid-stream change never takes effect until
// the loop body is re-compiled or the task is otherwise forced to
// re-read. That is exactly the "server says muted, node never mutes"
// symptom.
volatile float node_volume_multiplier   = 1.0f;
volatile bool  node_muted               = false;
volatile bool  master_muted             = false;
volatile float master_volume_multiplier = 1.0f;
volatile float zone_volume_multiplier   = 1.0f;
volatile bool  zone_muted               = false;

volatile int audio_prebuffer     = AUDIO_PREBUFFER;
volatile int audio_queue_target  = AUDIO_QUEUE_TARGET;
volatile uint32_t last_node_latency_ms = 0;

// Estimated offset between the server's millis() clock and ours.
// Updated by the time_sync control packet. Positive means the server's
// clock is ahead of ours by that many ms. Used to schedule playout so
// every node plays the same server-captured sample at the same wall-clock
// instant, which is the whole point of the sync mechanism.
//
// [offset-must-be-32-bit]
// int64_t, not int32_t? No — int32_t on purpose. This is read on Core 1
// (audioPlaybackTask deadline math) while Core 0 (applyCtrlJson /
// pollClockSyncResponse) is writing it. A 64-bit access compiles to two
// 32-bit loads/stores; the reader can land between them and see a torn
// value, producing a deadline hundreds of seconds out, which then trips
// the "wait_ms clamp: offset probably wrong" or "frame far past deadline"
// flush + rebuffer paths. Every torn read = one flush + rebuffer.
//
// All writers already constrain the value to the millis() range
// (uint32 subtraction cast to int32), and every reader already
// sign-extends to int64_t for the actual arithmetic. So narrowing the
// storage to int32_t is exact, and int32_t is one atomic load/store
// on this MCU. This is the actual fix, not a workaround.
// [offset-overflow] int32_t is exact only while the true (server_millis -
// local_millis) difference fits in ±2^31 ms (~24.85 days). Past that, the
// wrap-safe (int32_t)(serverMs - localMs) subtraction returns the wrong
// sign, deadlines land 49.7 days in the past, and the playback task
// rebuffer-loops until the server's millis() naturally rolls past 2^32
// (a further ~24 days). This pair captures a *reference point* at every
// successful sync so the playout path can compute elapsed server time
// directly (int32 delta from a recent anchor is always wrap-safe) instead
// of via the absolute offset.
//
// Read pattern: audioPlaybackTask does
// int32_t delta = (int32_t)(captured_ms - g_sync_server_ms);
// int64_t our_local_at_capture = (int64_t)g_sync_local_ms + delta;
// Both fields are 32-bit volatile writes on Core 0; per-field writes are
// atomic. The two-field window between them is a few ns and syncs are
// every 2 s, so a torn pair contributes < 1 sample-time error.
volatile uint32_t g_sync_local_ms  = 0;
volatile uint32_t g_sync_server_ms = 0;

volatile int32_t server_clock_offset_ms = 0;
volatile bool    server_clock_synced    = false;

// [clock-pair-mux] g_sync_local_ms and g_sync_server_ms are a PAIR: the
// audio task computes deadline_local = sync_local + (captured - sync_server).
// If the writer interleaves between the two stores, the reader sees a
// torn pair → deadline wrong by |new_offset - old_offset| (tens of ms on
// a drifting link, NOT "a few ns"). One spinlock around the writes and the
// pair reads eliminates it. Also serialises setServerClockOffset() itself:
// it is called from networkTask (Core 0) and from applyCtrlJson (Core 0)
// and, on some paths, from audioPlaybackTask's flush (Core 1).
static portMUX_TYPE g_clock_pair_mux = portMUX_INITIALIZER_UNLOCKED;

// [offset-setter] Every writer MUST go through here so the reference pair
// stays in lock-step with the absolute offset. Writes are inside the
// critical section so the audio task's pair read is never torn.
inline void setServerClockOffset(int32_t offset) {
    uint32_t now = millis();
    portENTER_CRITICAL(&g_clock_pair_mux);
    g_sync_server_ms       = now + (uint32_t)offset;
    g_sync_local_ms        = now;
    server_clock_offset_ms = offset;
    portEXIT_CRITICAL(&g_clock_pair_mux);
}
// Smoothed absolute jitter of the raw per-sample offset estimate around the
// tracked EWMA offset. Pure diagnostics; not fed back into playout timing.
volatile int32_t server_clock_jitter_ms = 0;
// Set by applyCtrlJson() when a time_sync sample disagrees with the current
// offset by more than can be explained by jitter/drift (server reboot or a
// clock rollover the wrap-safe math could not fully absorb). The playback
// task clears this after flushing the queue and re-anchoring to the new
// timeline, rather than slowly chasing the jump frame by frame.
volatile bool    g_force_resync         = false;
// Set when a fresh registration should happen before the heartbeat timer's
// normal interval elapses (e.g. a server epoch change). Picked up by
// pingServerTask, which does the actual (blocking) HTTP POST -- this flag
// itself is just a volatile write, safe to set from any task.
volatile bool    g_force_reregister     = false;
// Set when the server reports this player audio-incompatible. Picked up by
// audioPlaybackTask, which performs the actual queue/decoder flush, so this
// write (settable from networkTask) never touches the decoder directly.
volatile bool    g_force_flush_incompatible = false;
// OTA exit rebuffer. During OTA, otaNodeUpdating pauses audioPlaybackTask
// and g_udpSocketsReady=false pauses udpAudioTask, so the queue is reset at
// UPLOAD_FILE_START and stays empty. But is_buffering (local to
// audioPlaybackTask) keeps its pre-OTA value (false if playing), so on
// failure/abort the task resumes playing fresh frames with no jitter buffer
// -> 1-2 s of choppy playback until the 200 ms starvation timer happens to
// fire. Set this flag on every non-reboot OTA exit so the task flushes and
// re-enters buffering immediately.
volatile bool    g_force_rebuffer = false;
// [servers-decide] Set when the current server sends the server_standby ctrl
// (a higher-priority server took over). networkTask drops the contact and
// re-discovers immediately instead of waiting for the 60 s stale path.
volatile bool    g_force_rediscovery = false;

// Round-trip (NTP-style) clock sync, authoritative once a sample has been
// collected (see sendClockSyncRequest()/pollClockSyncResponse() in
// EVDPNet.h). The passive "time_sync" ctrl push remains a fallback only for
// initial/recovery estimates, per the server's CLOCK_SYNC_UDP_PORT docs.
volatile bool     g_have_rtt_sync        = false;
// [epoch-invalidates-rtt-gate]
// RTT of the sample whose offset is currently loaded into
// server_clock_offset_ms. pollClockSyncResponse() only replaces the offset
// when the ring's best RTT is materially better than this value. An epoch
// change means the old offset is meaningless even if its RTT was perfect,
// so handleServerEpoch() resets this to UINT32_MAX so the next sample is
// adopted unconditionally. Without this reset, a server reboot leaves the
// node locked to a stale offset from the pre-reboot session and every
// playout deadline is scheduled at the wrong wall time.
volatile uint32_t g_current_offset_rtt   = 0xFFFFFFFFu;
// [v1.1.2-udp-ready]
// Set true by setupMulticastSockets() once audioUdp/ctrlUdp have been
// bound. udpAudioTask is created in setup() before those sockets exist
// (and before Wi-Fi is up on the captive-portal path), so it must not
// call parsePacket() until this flips. Without the guard, parsePacket()
// hits ioctl(FIONREAD) on an unopened fd and floods the log with
// "could not check for data in buffer length: 9" (EBADF) at ~110 Hz.
volatile bool     g_udpSocketsReady      = false;
volatile uint32_t stat_clock_sync_rtt_ms = 0xFFFFFFFFu; // last accepted RTT sample
volatile uint32_t g_pending_sync_client_tx_ms = 0;       // clientTxMs of the in-flight sync_req
volatile bool     g_sync_req_in_flight   = false;
volatile uint32_t ts_last_sync_req_sent_ms = 0;
volatile bool     g_force_sync_now       = false;

// Deadline-scheduler feedback for the soft drift corrector. Set by
// audioPlaybackTask for every dequeued frame: ms until the frame's playout
// deadline (positive = early, we sleep; negative = already late).
volatile int32_t  g_playout_lead_ms = 0;
// True right after a (re)buffer: head-of-queue frames that are already past
// their deadline are skipped (decoder was just reset, so this is inaudible)
// until the first on-time frame, so the stream resumes on the shared timeline
// instead of permanently trailing it by the prebuffer depth.
volatile bool     g_resume_trim = false;

// ---- Audio protocol compatibility / server epoch ----------------------------
// Set from the most recent /api/player/register response (see
// applyRegistrationResponse() in EVDPNet.h) and mirrors the server's own
// fail-closed policy (EVDProtocolCompat.h) from the player's side: audio is
// only interpreted while g_audio_compat_state == AUDIO_COMPAT_COMPATIBLE.
volatile int      g_audio_compat_state                 = AUDIO_COMPAT_UNKNOWN;
volatile int      g_negotiated_audio_protocol_version   = 0;
char              g_audio_compat_reason[40]             = "unknown";
volatile uint32_t stat_audio_withheld_incompatible      = 0;
// [withheld-not-registered]
// Packets dropped because the node has not yet received its first
// successful /api/player/register response (g_audio_compat_state ==
// AUDIO_COMPAT_UNKNOWN). Every reboot parks the node in this state for
// ~5 s (discovery + HTTP round-trip) while the server — whose subscriber
// slot survived the reboot — is already streaming. Accounting those
// packets under stat_audio_withheld_incompatible made every boot look
// like a firmware-compat failure on the status page. Kept separate so
// "boot noise" is distinguishable from "node running old firmware".
volatile uint32_t stat_audio_withheld_not_registered    = 0;

// ---- Encoder info received from the server --------------------------------
// Mirror of the server's live encoder state, as sent in the registration
// response and on every ctrl push (see EVDPlayers.h on the server). Stored
// as libopus integer constants so the node's status page can render them
// with the same names the dashboard uses. -1 means "never received"; the
// status page renders that as "—" so an old registration snapshot is
// visibly missing rather than a plausible-looking zero.
volatile int32_t  g_last_recv_enc_bitrate        = -1;
volatile int32_t  g_last_recv_enc_complexity     = -1;
// [enc-sentinel] -1 = "never received". A bool with no sentinel made an
// old / non-VBR server render as "VBR: Off" on the status page, which is
// a factual lie. -1 lets the JSON emit null so the UI shows "—".
volatile int32_t  g_last_recv_enc_vbr            = -1;
volatile int32_t  g_last_recv_enc_vbr_constraint = -1;
volatile int32_t  g_last_recv_enc_signal         = -1;
volatile int32_t  g_last_recv_enc_application    = -1;
volatile uint32_t ts_last_recv_enc_ms            = 0;
// Latest opusSampleRate the server declared in a registration response
// or host_audio_sync ctrl packet. 0 = "never received" (older server
// build). The node's Opus decoder is always created at SAMPLE_RATE
// (48000): libopus upsamples a 24 kHz-encoded stream transparently
// during decode, so the I2S pipeline and every playout-math constant
// that divides by SAMPLE_RATE are unchanged. Diagnostic only — see
// serverOpusSampleRate in handleGetHealth() (EVDPWeb.h).
volatile uint32_t g_last_recv_opus_sample_rate   = 0;

// [source-rate-receive]
// Latest measured SOURCE rate (I2S clock) the server declared in a
// registration response or host_audio_sync ctrl packet. 0 = "never
// received" (older server build).
//
// The server broadcasts this whenever updateInputRateEstimate()
// (EVDAudio.h on the server) sees the measured rate move >3 % over a
// 3 s window — BT 44.1k <-> 48k codec renegotiation, AUX hot-plug to a
// different rate, source-board PLL relock. The server's own resampler
// adjusts smoothly and the Opus bitstream stays 48 kHz-equivalent, so
// nothing on this node re-creates, resamples, or flushes based on this
// value. It exists so the Status page and the 10 min recorder can show
// the actual source rate (44.1k vs 48k) alongside the encoder Fs, which
// makes a "briefly choppy after a BT device switch" report diagnosable
// instead of mysterious.
volatile uint32_t g_last_recv_source_hz           = 0;

// serverEpoch, see EVDState.h on the server: a random value generated once
// at server boot, changes on every reboot/reset. A change means the
// server's capture-timestamp timeline and sequence baseline reset, so any
// cached offset/seq state here must be rebuilt (flush + rebuffer + fresh
// registration), not slowly chased.
volatile bool     g_server_epoch_known = false;
volatile uint32_t g_server_epoch       = 0;
// [epoch-debounce] millis() of the last accepted epoch change; 0 = none.
// Used to reject a stale pre-reboot packet whose epoch arrives after we
// have moved to the post-reboot epoch. Without this, a delayed datagram
// flips the node back to the old epoch, flushes audio, and stalls until
// the next fresh packet. 5 s > any plausible LAN UDP delay and < any
// plausible server reboot time.
volatile uint32_t g_last_epoch_change_ms = 0;

// The server's advertised CLOCK_SYNC_UDP_PORT (see /api/player/register
// response "clockSyncPort"). Falls back to CLOCK_SYNC_UDP_PORT_DEFAULT until
// the first registration response is processed.
volatile uint16_t g_clock_sync_port = CLOCK_SYNC_UDP_PORT_DEFAULT;

// Last bufferMs value from the server that we acknowledged. Used by both
// applyRegistrationResponse() and applyCtrlJson() to ignore repeated
// heartbeat values, so the 30 s registration cycle cannot yank the
// self-tuned target_playout_delay_ms back down to the server's base
// value on every pass (the grow-then-snap sawtooth that manifests as
// periodic micro-stutter).
volatile int last_server_buffer_ms = -1;

// ---- bufferMs cross-channel agreement -------------------------------------
// The server announces bufferMs on TWO channels (register response and ctrl
// push) that have been observed in the field sending DIFFERENT values for the
// same node at the same instant (register 150, ctrl 60). Adopting the
// lower value resets the node's self-tuned playout delay every few seconds
// and lets frames slip their deadline. Increases are always accepted; a
// DECREASE is only accepted when the OTHER channel reports the same lower
// value. -1 means "this channel has never sent a bufferMs".
volatile int last_reg_buffer_ms  = -1;
volatile int last_ctrl_buffer_ms = -1;

// [bufferMs-staleness]
// Millis() when each channel's bufferMs was last observed. Without these,
// the cross-channel guard cannot distinguish "the two channels disagree"
// from "the other channel simply hasn't been heard from since before a
// mode change". Concrete symptom: the operator switches to Movie mode,
// the server's broadcastNodeConfig fires immediately with bufferMs=80,
// but the register heartbeat won't arrive for up to 30 s. The guard sees
// last_reg_buffer_ms=150 and rejects the ctrl with 80, so the node plays
// the whole first heartbeat interval of Movie at Off's 150 ms latency.
// A >10 s register age is "stale, treat ctrl as authoritative".
volatile uint32_t ts_last_reg_buffer_ms  = 0;
volatile uint32_t ts_last_ctrl_buffer_ms = 0;

// How many ms after a frame's capture time it should be played. This is
// the same number the server calls "bufferMs". It is *not* the same as
// audio_prebuffer, which is a queue-depth target.
//
// Boot defaults. The live value is self-tuned by audioPlaybackTask()
// (see EVDPAudio.h) from this node's own late-frame histogram, and the
// live target_playout_delay_ms is what /api/health reports as
// "playoutDelayTargetMs". These two only define where the self-tuner
// starts before it has observed any late frames.
volatile int playout_delay_ms        = 120;
volatile int target_playout_delay_ms = 120;

// ---- Debounced saves -------------------------------------------------------
// Written by handlePostVolume (WebServer, Core 0) and read by networkTask
// (Core 0). Not under a mutex, so both the flags and the timestamps must be
// volatile or the compiler may cache them across the vTaskDelay() in
// networkTask's main loop.
volatile unsigned long volume_change_timestamp = 0;
volatile bool          pending_volume_save     = false;
volatile unsigned long speaker_save_timestamp  = 0;
volatile bool          pending_speaker_save    = false;

// ---- Reset button ----------------------------------------------------------
unsigned long reset_button_start_time = 0;
bool          reset_button_pressed    = false;

// [ap-mode-deferred]
// Set by loop() when RESET_PIN has been held for 3 s. Consumed by
// networkTask, which performs the actual transition to AP mode from
// a single-threaded context. Calling startCaptivePortal() directly
// from loop() (Arduino main task, Core 1) races networkTask (Core 0)
// on three non-thread-safe resources simultaneously:
//   - WebServer::on() / WebServer::begin()  (route table + socket)
//   - DNSServer::start()                    (UDP capture socket)
//   - Preferences::begin()/putX()/end()     (NVS read-modify-write)
// The symptom of that race is a boot-loop or a half-registered route
// table, which then makes the captive portal 404 on /, /save, etc.
// Deferring to networkTask collapses the transition onto the task
// that already owns all three resources.
volatile bool g_enter_ap_mode_requested = false;

// ---- Network Test (AP Mode) ------------------------------------------------
enum NetTestState { NET_TEST_IDLE, NET_TEST_RUNNING, NET_TEST_SUCCESS, NET_TEST_FAILED };
volatile NetTestState g_netTestState = NET_TEST_IDLE;
char g_netTestError[64] = "";

// [wifi-test-only] True when the Network tab's Test button started the task:
// the credentials are proven, then the stored profile is restored - nothing is
// written to NVS and the node does not reboot.
bool g_netTestOnly = false;

char pending_ssid[64] = "";
char pending_pass[64] = "";
char pending_name[64] = "";
bool pending_use_static = false;
char pending_ip[16] = "";
char pending_gw[16] = "";
char pending_sn[16] = "";
char pending_dns[16] = "";

// ---- Discovery -------------------------------------------------------------

// ---- Discovery -------------------------------------------------------------
// [volatile-cross-task] serverFound / is_in_ap_mode are written by
// serverWatcherTask (Core 0, its own task) and read by networkTask and
// loop(). Without volatile the compiler is free to cache across loop
// iterations. serverIP is written from serverWatcherTask / discover_server
// and read from networkTask; it is 4 bytes so access is atomic on this MCU,
// and .toString() re-reads through a function call, so it is left as-is.
// [no-server-address-config] There is no configurable "Server address" any
// more: the node always discovers the server by name (DEFAULT_MDNS_HOST),
// then UDP broadcast, then the server's SoftAP. This is what stops a node
// from being stranded on a dead hard-coded IP.
int  ws_port              = 80;
IPAddress serverIP;
volatile bool serverFound   = false;
volatile bool is_in_ap_mode = false;
String    node_uuid_str   = "";

// ---- Buffers ---------------------------------------------------------------
int16_t* pcm_decode_buffer  = nullptr;
int16_t* dac1_output_buffer = nullptr;
int16_t* dac2_output_buffer = nullptr;

// ---- Queues / tasks --------------------------------------------------------
QueueHandle_t audio_queue        = nullptr;
TaskHandle_t  audio_task_handle  = nullptr;
// Serializes queue admission/sequence updates against playback and OTA flushes.
SemaphoreHandle_t g_audio_queue_mutex = nullptr;

// [audio-config-mutex]
// Serialises writers (WebServer task: handleSave / handleCancel /
// handlePostVolume) against the audio task's per-frame read+state-update of
// speakers[] and speaker_filters[]. Speakers' coefficient writes are 5 floats
// per biquad × 4 biquads × 4 speakers — a torn read mid-update produces an
// invalid filter and an audible click.
SemaphoreHandle_t g_audio_config_mutex = nullptr;
TaskHandle_t  network_task_handle = nullptr;
TaskHandle_t  ping_task_handle    = nullptr;

// ---- Sequence-number gap/reorder tracking (network task) --------------------
// Only meaningful once a v2-header packet has been seen; reset whenever the
// audio pipeline is flushed/rebuffered so stale expectations never leak
// across a discontinuity.
volatile bool     seq_initialized = false;
volatile uint32_t expected_seq    = 0;  // next seq we expect to accept in order
volatile uint32_t last_seq_seen   = 0;  // highest raw seq accepted so far
volatile uint32_t last_conceal_captured_ms = 0; // capture-time anchor for PLC

inline void resetAudioQueueForOta() {
    if (!g_audio_queue_mutex) return;
    xSemaphoreTake(g_audio_queue_mutex, portMAX_DELAY);
    if (audio_queue) xQueueReset(audio_queue);
    seq_initialized = false;
    last_conceal_captured_ms = 0;
    xSemaphoreGive(g_audio_queue_mutex);
}

// ---- Stats -----------------------------------------------------------------
volatile uint32_t stat_packets_oversize = 0;
// [wire-flag-unicast] Frames whose header said they were addressed to a
// different node. Non-zero on a healthy network is expected and harmless.
volatile uint32_t stat_packets_not_for_me = 0;
volatile uint32_t stat_packets_dropped  = 0;
volatile uint32_t stat_rebuffers        = 0;
// [stat-audio-source-rejects]
// Incremented once per audio datagram dropped by the source-IP gate in
// udpAudioTask() (see [audio-source-gate] in EVDPNet.h). The gate
// compares audioUdp.remoteIP() against serverIP; anything else is
// discarded without touching audio_queue.
//
// Should stay at 0 in normal operation. Any sustained value means one of:
//   - a rogue LAN peer is spraying the audio multicast port (spoofing);
//   - the server's IP changed mid-session (DHCP renewal, failover) and
//     discovery has not re-run yet — the node will re-discover on the
//     next stale-contact tick, after which this counter stops climbing;
//   - a stale multicast group membership survived an IP change (rare;
//     setupMulticastSockets() on the next reconnect clears it).
//
// 32-bit aligned store, single-writer (udpAudioTask, Core 0), single
// reader (/api/health handler on the AsyncWebServer task). No lock.
volatile uint32_t stat_audio_source_rejects = 0;
// [trim-vars-diagnostics-only] Both fields are still reported in
// /api/health ("rateTrimHz") and /api/node_stats for schema compatibility.
// Nothing writes them now that applyClockTrim() is gone; they always read 0.
volatile int32_t  stat_rate_trim_hz     = 0;


// [auto-expand-disabled]
// Operator requirement: the dashboard's Target Buffer Latency slider is the
// single authority for playout_delay_ms. Auto-expand used to grow
// target_playout_delay_ms by up to 40 ms ABOVE the slider value and never
// shrink it until the slider moved again, so a node that experienced a
// jitter burst stayed permanently 40 ms slower than the operator selected
// (perceived as a hardcoded +50/+60 once I2S DMA latency is added in).
// Setting AUTO_EXPAND_MAX_MS to 0 makes the growth guard in
// audioPlaybackTask() (`g_auto_expand_ms < AUTO_EXPAND_MAX_MS`) permanently
// false, so g_auto_expand_ms stays 0 for the whole uptime and
// target_playout_delay_ms == slider value at all times.
volatile int32_t g_auto_expand_ms = 0;

// [auto-expand-removed]
// The auto-expand feature is disabled by design: the operator's Target
// Buffer Latency slider (bufferMs in the register/ctrl packets) is the
// single authoritative source for target_playout_delay_ms. This matches
// the [auto-expand-disabled] comment on g_auto_expand_ms above.
//
// With the cap at 0, every growth guard in EVDPAudio.h of the form
//   `g_auto_expand_ms < AUTO_EXPAND_MAX_MS`
// evaluates `0 < 0` = false forever, and every reset in EVDPNet.h
// (`g_auto_expand_ms = 0`) is a no-op. The accumulator cannot leave 0,
// so:
//   * effective playout = last_server_buffer_ms + 0 = server's slider value
//   * [BufDiag] logs `autoExp=0` in every line, proving the feature is off
//   * the "streak of late frames" flush fallback (formerly pre-empted by
//     a +10 ms auto-expand bump) now runs as designed: after a real
//     pathology the pipeline flushes and re-buffers on a fresh timeline
//
// Field data from the 2026-11-04 recordings shows why: the 100 ms cap was
// adding 100 ms of latency that the slider said should not exist (playout
// 300 ms vs operator's 200 ms), and that surplus lengthened every recovery
// tail at a pacer stall without measurably reducing stutter. Correcter
// deadband fix + encoder complexity 2 eliminated the stutter the auto-
// expand was added to mask; the extra 100 ms has no remaining upside.
#define AUTO_EXPAND_MAX_MS 0

// [auto-correction] The "audio degrades and never recovers without a server
// restart" report. The server's bufferMs is the single authority for the
// TARGET, but when the pipeline ends up in a bad state nothing ever pulls it
// back: a flush leaves is_buffering = true, the rebuffer needs
// audio_queue_target frames, and the queue target is derived from a latency
// EMA that the flush itself just reset to 20 ms. If the arriving frames are
// timed against the pre-flush timeline the wait is long, the queue never
// refills, and the node sits silent until a reboot re-runs the whole
// handshake.
//
// Two independent correctors, both narrow:
//   * a buffering watchdog that re-anchors the timeline after
//     AUTO_RECOVER_REBUFFER_MS of an empty queue (the streaming
//     equivalents are the real fix; this is the floor), and
//   * a drain corrector that throws away the stale tail when the queue keeps
//     running away from the target instead of letting drop_at() trickle it
//     out one frame at a time (which is audible as a long, slow crunch).
//
// Neither touches target_playout_delay_ms, so the slider stays the authority.
#define AUTO_RECOVER_REBUFFER_MS      1200UL  // empty-queue rebuffer before re-anchor
#define AUTO_RECOVER_DRAIN_FRAMES     6       // queue above target+this for ...
#define AUTO_RECOVER_DRAIN_SECONDS    2       // ... this many consecutive seconds
volatile uint32_t stat_auto_recover_rebuffer = 0;  // timeline re-anchors
volatile uint32_t stat_auto_recover_drain    = 0;  // stale-tail drains
volatile uint32_t stat_packets_received = 0;
volatile uint32_t current_i2s_rate      = SAMPLE_RATE;

// ---- Sequence / loss-recovery stats -----------------------------------------
volatile bool     audio_proto_v2_seen      = false; // at least one v2 header seen
volatile uint32_t stat_seq_received        = 0;     // in-order frames accepted
volatile uint32_t stat_seq_missing         = 0;      // total frames implied missing by gaps
volatile uint32_t stat_seq_gaps            = 0;      // number of gap *events*
volatile uint32_t stat_seq_reordered       = 0;      // arrived behind an already-seen seq
volatile uint32_t stat_seq_duplicate       = 0;      // exact repeat of the last seq seen
volatile uint32_t stat_frames_concealed    = 0;      // PLC frames synthesized
volatile uint32_t stat_frames_late       = 0;   // any late frame (kept for compat)
volatile uint32_t stat_late_lt5          = 0;   //   0 < late_ms <   5  (inaudible)
volatile uint32_t stat_late_5to20        = 0;   //   5 <= late_ms <  20  (borderline)
volatile uint32_t stat_late_20to100      = 0;   //  20 <= late_ms < 100  (audible tearing)
volatile uint32_t stat_late_100to200     = 0;   // 100 <= late_ms < 200  (near-discard)
volatile uint32_t stat_frames_discarded    = 0;      // frames dropped (stale/dup/reordered/flush)
volatile uint32_t stat_clock_resyncs       = 0;      // forced re-anchors from time_sync jumps
// [reanchor-counter] Flushes that ran against an empty queue (0 real
// frames drained). Not a rebuffer — the pipeline was already empty.
// Typical count on a clean boot: 2-4 (epoch change + first RTT + any
// subsequent jump within the first 2 s). Steady state: 0.
volatile uint32_t stat_empty_flushes       = 0;

// ---- Queue-correction diagnostics -------------------------------------------
volatile int32_t  stat_queue_error         = 0;      // last (depth - target)
volatile int32_t  stat_correction_samples  = 0;      // last applied sample correction

// Kept for API compatibility; no sample correction is currently applied.
volatile uint32_t stat_total_correction_samples = 0;

// ---- Error counters --------------------------------------------------------
volatile uint32_t stat_opus_decode_errors   = 0;
volatile uint32_t stat_i2s_write_errors     = 0;
volatile uint32_t stat_i2s_timeouts         = 0;
volatile uint32_t stat_wifi_disconnects     = 0;
volatile uint32_t stat_discovery_failures   = 0;
volatile uint32_t stat_multicast_join_fails = 0;
volatile uint32_t stat_post_stats_fails     = 0;
volatile uint32_t stat_post_register_fails  = 0;
volatile uint32_t stat_ctrl_json_errors     = 0;
volatile uint32_t stat_ctrl_packets_rx     = 0;

// ---- Stutter diagnostics ---------------------------------------------------
// The cumulative counters above (framesLate*, seqGaps, rebuffers) read
// "100 % optimal" during a periodic stutter because the glitch spans one or
// two seconds out of every 10-30, so cumulative tallies never look bad.
// These fields expose the per-second view that the cumulative counters hide:
//
//   stat_packets_this_sec       packets received since the last 1 s rollover
//   stat_packets_last_sec       snapshot of the previous full second, taken
//                               at rollover by audioPlaybackTask (Core 1)
//   stat_max_gap_ms_this_sec    worst inter-arrival gap since rollover, ms
//   stat_max_gap_ms_last_sec    snapshot of the previous full second
//   stat_gap_gt_35ms_total      lifetime gaps > 35 ms (>= 1 frame lost)
//   stat_gap_gt_50ms_total      lifetime gaps > 50 ms (>= 2 frames lost)
//   stat_gap_gt_100ms_total     lifetime gaps > 100 ms (audible burst)
//   ts_prev_audio_arrival_ms    arrival time of the previous valid datagram
//
// Written by udpAudioTask (Core 0) via enqueueAudioPacket(); the 1 s
// rollover runs on audioPlaybackTask (Core 1). Aligned 32-bit loads/stores
// are atomic on this MCU, so no lock is needed — a one-packet race on the
// rollover is irrelevant for a diagnostic. ts_prev_audio_arrival_ms is
// reset to 0 by flushAudioQueueAndRebuffer() so a rebuffer does not
// register as a 20-second gap.
volatile uint32_t stat_packets_this_sec     = 0;
volatile uint32_t stat_packets_last_sec     = 0;
volatile uint32_t stat_max_gap_ms_this_sec  = 0;
volatile uint32_t stat_max_gap_ms_last_sec  = 0;
volatile uint32_t stat_gap_gt_35ms_total    = 0;
volatile uint32_t stat_gap_gt_50ms_total    = 0;
volatile uint32_t stat_gap_gt_100ms_total   = 0;
volatile uint32_t ts_prev_audio_arrival_ms  = 0;

// ---- Post-queue diagnostic counters ---------------------------------------
// The packet/scheduler counters above (framesLate, framesConcealed,
// rebuffers, packetsLastSec, maxGapMs) are all zero when the audio
// pipeline is delivering frames correctly. If the operator still hears a
// stutter, the cause is downstream of those counters: the corrector, the
// Opus decoder, or the I2S DAC. These three per-second deltas expose each
// of those stages.
//
//   correctorFramesLastSec   frames in the last second where the soft
//                            drift corrector actually pulled/added a
//                            sample. A sudden burst of corrector activity
//                            is audible as a subtle "phase shift" glitch.
//                            Expected steady state: 0 on a healthy node.
//   shortDecodeFramesLastSec frames in the last second where Opus returned
//                            fewer than OPUS_FRAME_SIZE samples. A frame
//                            that decodes short is a decoder glitch; the
//                            missing samples become a click/pop.
//                            Expected steady state: 0.
//   leadBelow20LastSec       frames in the last second whose playout
//                            deadline was less than 20 ms away at dequeue
//                            time. Non-zero means the queue was running
//                            one frame from a missed deadline; the audible
//                            symptom is a single dropped or stretched
//                            frame. Expected steady state: 0.
//
// Rollover is in audioPlaybackTask, same 1 s window as the packet counters,
// so the timeline can be read across all fields in a single second.
volatile uint32_t stat_corrector_frames_last_sec   = 0;
volatile uint32_t stat_short_decode_frames_last_sec = 0;
volatile uint32_t stat_lead_below20_last_sec       = 0;
// Working accumulators, reset at the 1 s rollover.
volatile uint32_t stat_corrector_frames_this_sec   = 0;
volatile uint32_t stat_short_decode_frames_this_sec = 0;
volatile uint32_t stat_lead_below20_this_sec       = 0;
// [lead-tracking-v2] Worst (most negative, i.e. lowest) frame lead seen
// in the current 1 s window. +2147483647 = "no frames yet"; a healthy
// node reports 15-40 ms. Reset to INT32_MAX at each 1 s rollover. A
// snapshot field stat_min_lead_last_sec is produced at rollover so the
// dashboard can render "worst lead: 18 ms" alongside the count.
volatile int32_t  stat_min_lead_this_sec           = 0x7FFFFFFF;
volatile int32_t  stat_min_lead_last_sec           = 0x7FFFFFFF;

// [captured-ms-timeline-counters]
// These are the ONLY node-side counters for "green but stuttering".
// The pendingBuf / encodeOverruns / encodePeakUs counters that used to
// live here are SERVER-side: the node has no encoder and no pendingBuf,
// so those six always read 0. They have been moved out (they still
// exist on the server, where they actually fire).
//
// What remains here measures the two things the NODE can observe that
// the packet-level stats cannot:
//
//   capturedMsGap* — delta between consecutive frames' captured_ms,
//                    measured at dequeue. 20 ms = normal; 40 ms = one
//                    server-side frame skipped; 60 ms = two. This is
//                    the direct proof of the "1.2 s thump": the frame
//                    never existed on the wire, so seqGaps and
//                    framesLate both remain 0.
//
//   correctorFrames* — frames the soft drift corrector actually pulled
//                    or added samples to. Counted AFTER the ±3 deadband
//                    cap, so a deadband residual cannot inflate this.
//                    Steady state: 0.
volatile uint32_t statCapturedMsGapThisSec         = 0;   // working max
volatile uint32_t statCapturedMsGapMaxLastSec      = 0;
volatile uint32_t statCapturedMsGapGt20Total       = 0;
volatile uint32_t statCapturedMsGapGt40Total       = 0;
volatile uint32_t statLastCapturedMs               = 0;

volatile uint32_t statCorrectorFramesTotal         = 0;
volatile uint32_t statCorrectorFramesPeakThisSec   = 0;   // working max
volatile uint32_t statCorrectorFramesPeakLastSec   = 0;

// [audio-hz-diagnostics]
// Per-second spectral energy of the PRE-FILTER LEFT channel — i.e. the
// exact signal entering speaker_filters[0..3]. The tap sits after the
// corrector + gain stage but before the Linkwitz-Riley crossovers, so it
// shows what is actually fed into the four speaker filters and therefore
// what the DACs would emit if the filters were transparent.
//
// The point is to distinguish three distinct mechanisms that all sound
// like "a frequency range shifted out and came back":
//
//   1. High band (>= 4 kHz) loses energy while low/mid stay flat
//      -> treble "air" vanished. Either a filter problem (Linkwitz-Riley
//         instability) or a packet-level loss that PLC concealed with its
//         intrinsic low-pass.
//      Signal: stat_hz_high_db_x10 drops >= 6 dB vs previous second.
//
//   2. Centroid drifts smoothly across several seconds, bands flat
//      -> corrector pitch-shift. The soft corrector pulls or inserts
//         samples to track queue depth; a 2-sample correction is ~3.4 cents.
//      Signal: stat_hz_centroid_hz moves >20% over 2-3 seconds.
//
//   3. All bands drop together
//      -> upstream silence, network gap, server stall, or a truly dead
//         amp. Not a filter problem.
//
// Values are dB * 10 (e.g. -183 = -18.3 dB) so the log preserves one
// decimal place without putting floats in volatile globals. Peak is the
// per-second maximum |sample| measured before the soft limiter.
//
// Diagnostic only: no control path reads these, no flush/rebuffer decision
// depends on them. Deleting the entire block has zero effect on audio.
volatile int32_t  stat_hz_low_db_x10  = 0;   // LP@250 Hz, dB*10
volatile int32_t  stat_hz_mid_db_x10  = 0;   // HP@250 + LP@4000, dB*10
volatile int32_t  stat_hz_high_db_x10 = 0;   // HP@4000, dB*10
volatile int32_t  stat_hz_rms_db_x10  = 0;   // full-band RMS, dB*10
volatile int32_t  stat_hz_peak_db_x10 = 0;   // per-second peak, dB*10
volatile uint16_t stat_hz_centroid_hz = 0;   // ZCR proxy for spectral centroid, Hz

// [queue-target-smoothed-latency]
// Single source of truth for the queue-depth target. The three call sites
// that used to compute this independently — applyDelayToQueueTarget() in
// EVDPAudio.h, applyRegistrationResponse() in EVDPNet.h, and applyCtrlJson()
// in EVDPNet.h — all read last_node_latency_ms, which is written once per
// 50 Hz frame and carries 20-40 ms of per-frame noise. Each site snapped to
// the most-recent sample, so the three oscillated against each other and
// left the corrector firing 6×/sec at "err = -6, target = 7, depth = 1"
// (see node-diag 2026-11-13T15:48:54 for 3,348 consecutive fires across
// 552 s). Smoothing the latency with a 7:1 EMA removes the frame noise
// without adding perceptible control lag (the EMA has ~150 ms time constant
// at 50 Hz), and every caller now derives the identical target.
//
// The clamp to delayMs / 2 keeps the headroom floor: even if the smoothing
// lags a genuine latency step, netLat cannot exceed half of playout, so
// target cannot fall below 1 frame + the 2-frame floor below.
inline int computeQueueTargetSmoothed(int delayMs) {
    static uint32_t ema_lat = 0;
    uint32_t instantLat = last_node_latency_ms;
    if (instantLat == 0) instantLat = 20;         // boot / post-flush default
    if (ema_lat == 0)    ema_lat = instantLat;    // first observation
    else                 ema_lat = (ema_lat * 7 + instantLat) / 8;

    uint32_t netLat = ema_lat;
    if (netLat > (uint32_t)delayMs / 2) netLat = (uint32_t)delayMs / 2;
    if (netLat < 20)                    netLat = 20;   // matches drop_at floor

    // [target-margin] Reserve MARGIN_MS of per-frame deadline headroom
    // before deriving the queue target. Previously the target consumed the
    // entire (playout - latency) budget, leaving only
    //   (playout - latency) mod 20
    // ms of wait remaining when a frame reaches the head of the queue. At
    // playout=280 / latency=119 that remainder is 1 ms, which is exactly
    // the observed minLeadMsLastSec = -7 and leadBelow20LastSec = 50/s.
    // Subtracting a 15 ms margin gives the head frame ~15-35 ms of slack
    // (margin + integer remainder). Trade-off: one fewer frame of jitter
    // buffer (20 ms) in exchange for 20 ms of deadline slack on every
    // frame. Observed inter-arrival jitter is 25-28 ms, so a 7-frame
    // queue at playout=280 still absorbs the worst burst.
    const int MARGIN_MS = 15;
    int tgt = (delayMs - (int)netLat - MARGIN_MS) / 20;
    if (tgt < 2) tgt = 2;
    if (tgt > (AUDIO_QUEUE_LEN * 3) / 4) tgt = (AUDIO_QUEUE_LEN * 3) / 4;
    return tgt;
}

// ---- Timestamps (millis() of last event, 0 = never) ------------------------
volatile uint32_t ts_last_audio_packet_ms       = 0;
volatile uint32_t ts_last_server_contact_ms     = 0;
volatile uint32_t ts_last_discovery_attempt_ms  = 0;
volatile uint32_t ts_last_discovery_success_ms  = 0;
volatile uint32_t ts_last_wifi_disconnect_ms    = 0;
volatile uint32_t ts_last_rebuffer_ms           = 0;

// ---- Boot diagnostics ------------------------------------------------------
volatile uint32_t boot_count = 0;
char boot_reason_str[16] = "UNKNOWN";

volatile bool trim_unstable       = false;
volatile bool fade_on_resume      = true;
volatile bool g_have_played_audio = false;
volatile bool otaNodeUpdating     = false;

// Set when the entire uploaded firmware has been written to the inactive
// OTA partition (Update.write succeeded for every chunk). Update.end()
// has NOT been called yet at this point — the commit is deferred to
// handleNodeOtaDone() so it can happen after the server notification.
// This is the two-phase-commit signal: "bytes are safely in NVM, await
// finalize+reboot".
volatile bool otaNodeFirmwareReady = false;
// [ota-commit-single-shot] True from the moment Update.end(true) is entered
// until the restart. Declared here (not in EVDPWeb.h) because EVDPNet.h's
// stuck-OTA watchdog reads it and is compiled before the web handlers.
volatile bool otaCommitInProgress = false;

// Timestamp (millis()) of the last OTA upload activity: set on
// UPLOAD_FILE_START and refreshed on every UPLOAD_FILE_WRITE chunk. The
// networkTask watchdog uses this to detect an upload that has stalled
// (client disconnected mid-POST without the WebServer ever firing
// UPLOAD_FILE_ABORTED, or Update.begin()/write() failed and the client
// kept sending). Without this, otaNodeUpdating stays true forever and
// audioPlaybackTask's "pause during OTA" branch never releases, leaving
// the node permanently silent until a manual reboot.
volatile uint32_t otaNodeLastActivityMs = 0;

// Node-side soft start, in two flavours. Both are consumed by the mixer in
// EVDPAudio.h when fade_on_resume is seen; the flag below selects which.
//
//   g_session_fade_needed  true  -> SOFT start (FADE_SOFT_SECS, 2.0 s)
//                          false -> SHORT declick (FADE_SHORT_SECS, 5 ms)
//
// SOFT is set at boot, on every server epoch change (handleServerEpoch() in
// EVDPNet.h), and on every resume that follows a gap longer than
// FADE_RESUME_GAP_MS with no audio at all (see audioPlaybackTask's
// fade_on_resume site, [fade-resume-gap]). That third case is what makes a
// pause/play on the PC or phone fade in instead of snapping on.
//
// SHORT is the fallback: a rebuffer caused by a few dropped datagrams, where
// a 2 s ramp would be heard as a swell rather than as a soft start.
//
// [fade-comment-2s] This comment used to promise "ramp in over 2 s" while the
// code ramped over 200 ms, so anyone reading it drew the wrong conclusion
// about how the node sounded. The duration now lives in FADE_SOFT_SECS in
// EVDPAudio.h and is the single source of truth; this comment no longer
// states a number it can drift away from.
volatile bool g_session_fade_needed = true;

// ts_last_audio_packet_ms (defined above) is what audioPlaybackTask reads to
// tell a long silence from a short one: it is stamped in EVDPNet.h on every
// VALID datagram, so "now - ts_last_audio_packet_ms" at the moment playout
// resumes is exactly how long the node had no audio.

// How long the node was silent before the resume that is about to happen.
// Written by audioPlaybackTask at the fade_on_resume site and read once by
// the mixer. Kept here rather than as a function-local static so the length of
// the gap that produced a given ramp is visible in the debugger.
volatile uint32_t g_resume_gap_ms = 0;

// ---- Logging ring ----------------------------------------------------------
// Fixed-size ring of char arrays instead of std::vector<String>. The old
// erase(begin()) + push_back() pair was O(n) with an allocation + free on
// every log line, running on the same core that serves the UDP drain.
// Under fragmentation that heap churn can stall a Core 1 String alloc long
// enough to slip the audio task's playout deadline. A char ring is O(1)
// and heap-free after the initial static allocation.
#define MAX_LOG_LINES     50
#define MAX_LOG_LINE_LEN 160
char           system_logs[MAX_LOG_LINES][MAX_LOG_LINE_LEN];
volatile int   system_log_head  = 0;   // next write slot
volatile int   system_log_count = 0;   // valid entries (0..MAX_LOG_LINES)