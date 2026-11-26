
// Must match node_X.Y.Z.bin filename on the server side.
#define FW_VERSION "2.8.8.0"   // master recording (PSRAM) + glass UI + standby re-discovery

/*
  ============================================================================
  EnkelvoudESPPlayer (ESP32-S3)
  UDP MULTICAST Opus receiver -> Dual I2S DAC output + Web config UI

  Transport:
    Audio   : join 239.255.42.99:4202, receive the server's v2 wire format:
                [2B LE magic 0xE1D0][1B ver=2][1B flags][4B LE seq]
                [4B LE captured_ms][Opus frame]  (12-byte header total)
              See EVDPProtocolCompat.h for the exact layout (must match the
              server's EVDConfig.h AUDIO_PROTO_* docs) and the compatibility
              policy: audio is only interpreted once a /api/player/register
              response has confirmed this player audioCompatible — there is
              no silent v1 fallback. v2's per-frame sequence number drives
              gap/duplicate/reorder detection and Opus PLC.
    ClockSync: request/response RTT clock sync, UDP to the server's
              advertised clockSyncPort (see sendClockSyncRequest() /
              pollClockSyncResponse() in EVDPNet.h).
    Control : unicast to this node's IP on port 4203, receive JSON targeted
              at our IP (see applyCtrlJson() in EVDPNet.h).
    Stats   : HTTP POST /api/node_stats (matches server route)
    Register: HTTP POST /api/player/register (boot + heartbeat), declares
              "audioProtocolVersion" and parses back audioCompatible/
              negotiatedAudioProtocolVersion/audioCompatReason/epoch/
              clockSyncPort (see applyRegistrationResponse() in EVDPNet.h).

  ============================ Project files ================================

  --- Inline headers (all in the same translation unit) ---------------------

  EVDPProtocolCompat.h
      Audio wire-format parsing + player-side compatibility state. Kept
      deliberately free of Arduino/ESP32/opus dependencies (plain int/bool/
      const char* and fixed-width integers only) so it can be unit-tested
      on a host (tests/test_protocol_compat.cpp) without a hardware build.
      Defines AUDIO_PROTO_MAGIC (0xE1D0), AUDIO_PROTO_VERSION (2),
      AUDIO_HDR_*_BYTES, AUDIO_HEADER_BYTES (12), ParsedAudioHeaderV2,
      parseAudioHeaderV2(), AudioCompatState enum (UNKNOWN/COMPATIBLE/
      INCOMPATIBLE), AudioCompatInfo, evaluatePlayerAudioCompat(), and
      PLAYER_DECLARED_AUDIO_PROTOCOL_VERSION. This is the single source of
      truth for the header layout — the server's EVDConfig.h must match it.

  EVDPConfig.h  (merged into this .ino)
      Pin assignments (pin_lrc_1, pin_dout_1, pin_bclk_1, pin_lrc_2,
      pin_dout_2, pin_bclk_2, RESET_PIN), defaults (SERVER_AP_SSID,
      DEFAULT_MDNS_HOST), audio format (SAMPLE_RATE=48k, CHANNELS=2,
      OPUS_FRAME_SIZE=960), transport constants (MULTICAST_GROUP_OCTETS,
      AUDIO_UDP_PORT=4202, CTRL_UDP_PORT=4203, DISCOVERY_PORT=4201,
      CLOCK_SYNC_UDP_PORT_DEFAULT=4204), queue sizing (AUDIO_QUEUE_LEN=32,
      AUDIO_QUEUE_TARGET=3, AUDIO_PREBUFFER=3), receive cap
      (MAX_OPUS_PACKET_LEN=1024), stale-frame thresholds
      (AUDIO_STALE_DISCARD_MS=200, AUDIO_STALE_REANCHOR_MS=600,
      AUDIO_STALE_STREAK_LIMIT=5), clock resync thresholds
      (CLOCK_RESYNC_JUMP_MS=1500, CLOCK_DRIFT_REFRESH_MS=4),
      OTA_STUCK_TIMEOUT_MS=90s, PLAYOUT_DELAY_SLEW_MS_PER_FRAME=2, and the
      POD structs AudioMessage (the queue item: payload + arrival_ms +
      captured_ms + seq + conceal flag), SpeakerConfig (name[32], volume,
      low_freq, high_freq, source), plus forward declarations. No logic.

  EVDPState.h
      Every mutable global: WiFi credentials (wifi_ssid, wifi_pass,
      friendly_name, static IP fields), I2S pin constants
      (const int pin_lrc_1 etc. — duplicated from EVDPConfig.h by design,
      the values must stay identical), speaker array (speakers[4]) + filter
      array (speaker_filters[4]), live audio state (node_volume_multiplier,
      node_muted, master_volume_multiplier, master_muted,
      zone_volume_multiplier, zone_muted, audio_prebuffer,
      audio_queue_target), clock sync state (server_clock_offset_ms,
      g_sync_local_ms/g_sync_server_ms anchor pair written only through
      setServerClockOffset(), g_have_rtt_sync, g_current_offset_rtt,
      g_force_resync/reregister/flush_incompatible/rebuffer flags),
      playout delays (playout_delay_ms, target_playout_delay_ms),
      bufferMs cross-channel guard (last_server_buffer_ms,
      last_reg_buffer_ms/last_ctrl_buffer_ms + their timestamps),
      g_auto_expand_ms (cap is AUTO_EXPAND_MAX_MS = 0 — disabled by design,
      the operator's slider is the single authority), sequence/PLC state
      (seq_initialized, expected_seq, last_seq_seen,
      last_conceal_captured_ms), legacy corrector diagnostics
      (stat_total_correction_samples), captured-ms timeline counters
      (statCapturedMsGap*), packet/loss stats (stat_packets_*,
      stat_seq_*, stat_frames_*, stat_late_*), clock-sync stats
      (stat_clock_sync_rtt_ms, stat_clock_resyncs, stat_empty_flushes),
      post-queue diagnostics (stat_corrector_frames_*,
      stat_short_decode_frames_*, stat_lead_below20_*, stat_min_lead_*),
      per-second rollover counters (stat_packets_last_sec,
      stat_max_gap_ms_*, ts_prev_audio_arrival_ms), error counters
      (stat_opus_decode_errors, stat_i2s_*, stat_wifi_disconnects,
      stat_discovery_failures, stat_multicast_join_fails,
      stat_post_stats_fails, stat_post_register_fails,
      stat_ctrl_json_errors, stat_ctrl_packets_rx), stutter diagnostics
      (stat_gap_gt_35/50/100ms_total), AP-mode flags
      (is_in_ap_mode, g_enter_ap_mode_requested,
      g_web_server_started, gRoutesRegistered), network test state
      (g_netTestState, g_netTestError, pending_* buffers), discovery state
      (serverIP, serverFound), OTA state flags (otaNodeUpdating,
      otaNodeFirmwareReady, otaNodeLastActivityMs), audio task state
      (audio_task_handle, audio_queue, g_audio_config_mutex), the log ring
      (system_logs[MAX_LOG_LINES][MAX_LOG_LINE_LEN] + head/count), boot
      diagnostics (boot_count, boot_reason_str), the WebServer + DNSServer
      objects, Preferences, OpusDecoder*, and the two UDP sockets
      (audioUdp, ctrlUdp) + clockSyncUdp + multicastGroup. Also provides
      computeQueueTargetSmoothed(delayMs) — the one true queue-depth target
      shared by the playback task, applyRegistrationResponse(), and
      applyCtrlJson().

  EVDPFilters.h
      4th-order Linkwitz-Riley crossover primitives. Self-contained
      (only <Arduino.h> + <cmath>). Defines:
        struct Biquad      — setLowpass/setHighpass (with `active` gating
                             and state reset), setHighpassForce() (for
                             internal fixed sub-sonic HP below the user-
                             facing 20 Hz floor), process(x).
        struct SpeakerFilter — hp1/hp2 (LR2×2 = LR4 high-pass),
                               lp1/lp2 (LR2×2 = LR4 low-pass),
                               dc1/dc2 (LR4 sub-sonic HP at 15 Hz, always
                               on, state preserved across reconfigures),
                               configure(low, high, fs) with idempotency
                               gate (_prev_low/_prev_high) so a repeated
                               configure() does not reset live biquad
                               state mid-waveform, process(x) chaining
                               all six sections.
      The [configure-idempotent] invariant is load-bearing: handleSave,
      handleCancel, and the boot-time configure loop all call configure()
      even when the user's values did not change, and without the gate
      that would click the sub channel on every reboot / Cancel.

  EVDPNet.h
      Network bring-up, discovery, HTTP heartbeat, control packet handling,
      and the two high-priority network tasks. Contains:
        logRingPush / log_message / log_warn — the serial + ring logger
            (mutex-protected; log_message gates on gPlayerLogLevel >= 3,
            log_warn always prints).
        sanitizeNodeName, postJson (generic HTTP POST helper with optional
            failure counter + response body out), evdJsonEscape is in
            EVDSET.h.
        handleServerEpoch — debounced epoch-change handler that resets
            every cached clock/seq/stat state when the server reboots.
        applyEncoderInfoFromJson — parses the six encoder mirror fields
            the server sends (encBitrate, encComplexity, encVbr,
            encVbrConstraint, encSignal, encApplication,
            opusSampleRate, sourceSampleRate); raises g_force_resync on
            a real opus rate change (the "chipmunk fix") or a >500 Hz
            source-rate jump.
        applyRegistrationResponse — parses the /api/player/register reply:
            protocol/compat/epoch/clockSyncPort, masterVolume/muted/
            bufferMs, zoneVolume/zoneMuted, and the encoder mirror; drives
            the bufferMs cross-channel guard ([bufferMs-cross-channel-reg]).
        postRegister, postNodeStats, postOtaResult — the three heartbeat
            POSTs.
        discoverServerViaUdp, discover_server, tryJoinServerAccessPoint —
            full discovery cascade (configured IP → mDNS → UDP broadcast
            → on-server-AP → diagnostic dump).
        setupMulticastSockets — binds audioUdp/ctrlUdp (both unicast per
            subscriber, not actually multicast), clockSyncUdp, and sets
            g_udpSocketsReady.
        applyCtrlJson — parses a host_audio_sync or time_sync control
            datagram: volume/mute/zone/bufferMs/encoder-mirror push
            ([bufferMs-cross-channel-ctrl], [authoritative-first-lock],
            [v1.1.2-authoritative-nudge], [offset-route-through-setter]),
            and per-mode bufferMs handling.
        sendClockSyncRequest, pollClockSyncResponse, and the min-RTT-of-8
            filter (SYNC_RING_SIZE, g_syncRing) — the authoritative clock
            reference; also [drift-refresh] forces the anchor to refresh
            on any offset drift >= CLOCK_DRIFT_REFRESH_MS (4 ms).
        enqueueConcealedFrame, enqueueAudioPacket — sequence/PLC handling
            for the audio path.
        udpAudioTask (Core 0, pri 5) — the ONLY reader of audioUdp /
            ctrlUdp; drops untrusted sources by IP, enqueues Opus packets,
            applies ctrl JSON. Paused during OTA via Update.isRunning().
        networkTask (Core 0, pri 4) — server.handleClient(), discovery
            re-runs, postNodeStats cadence, clock-sync polling,
            WiFi disconnects, deferred NVS saves, the stuck-OTA watchdog,
            and the deferred AP-mode entry.
        pingServerTask (Core 0, pri 1) — the 30 s /api/player/register
            heartbeat (with g_force_reregister fast-path).
        serverWatcherTask (Core 0, pri 2) — captive-portal mode watcher
            that probes for the server's SoftAP and, on join, spawns
            pingServerTask.
        serverProbeTask — background scan-only probe when initial
            discovery fails; never changes the network itself.

  EVDPAudio.h
      I2S bring-up, Opus decode, crossovers, and the
      playback task. Contains:
        audioLogThrottled — ring-only, rate-limited logger for the audio
            task (avoids blocking UART inside the per-frame budget).
        init_i2s — installs both DACs (I2S_NUM_DAC1, I2S_NUM_DAC2) with
            DMA depth 8×256, APLL, and explicit mclk_io_num = NO_CHANGE
            (GPIO 0 is a boot strap pin). Retries DAC2 without APLL on
            APLL contention. Sets g_i2s_dac1_ready / g_i2s_dac2_ready.
        softLimitToInt16 — rational soft-knee limiter (exact passthrough
            <= 0.9 FS, asymptotic 1.0 above). NaN/Inf guard.
        AudioHzDiag struct + configureAudioHzDiag + audioHzDiagSample +
            audioHzDiagRollover — 4-band energy analyser on the pre-filter
            L channel with ZCR-derived centroid. Currently disabled at
            the sample call site ([audio-hz-diag-disabled]) for CPU;
            stat_hz_* stay at 0. Re-enable by restoring the
            audioHzDiagSample(left) call.
        process_and_output_audio — the hot-path function. The queue-depth
            corrector is disabled: deadlines do not measure DMA consumption.
            Applies node×zone gain (no double attenuation — the server already
            applied master), full-length PCM processing, the
            four crossovers, per-speaker gain, soft-limit, and two
            i2s_write drains. Holds g_audio_config_mutex only for the
            sample-production block; I2S writes are outside the mutex.
        flushAudioQueueAndRebuffer — clears audio_queue, resets the Opus
            decoder, re-arms seq detection, bumps
            stat_rebuffers (or stat_empty_flushes when the queue was
            already empty).
        slewPlayoutDelay — moves playout_delay_ms toward
            target_playout_delay_ms at PLAYOUT_DELAY_SLEW_MS_PER_FRAME.
        audioPlaybackTask (Core 1, pri 3) — the scheduler. Handles
            g_force_* flags, buffering, the 1 s policy block
            (applyDelayToQueueTarget + auto-expand ratchet + rollovers
            for stat_*_last_sec), the deadline wait loop with wrap-safe
            int32 subtraction, late-bucket accounting, concealment /
            PLC decode, and corrector counters.

  EVDPWeb.h
      HTTP handlers, OTA receiver, config save/load, AP-mode entry. Contains:
        handleNodeFw — GET /api/fw version stub.
        handleNodeOtaUpload, handleNodeOtaDone — the /api/ota POST path
            (trust-gated uploader, chunked Update.write, two-phase
            commit via otaNodeFirmwareReady, server notification via
            postOtaResult, ESP.restart).
        handleApiCfg — GET /api/cfg JSON config blob for the dashboard.
        handleGetLogs — GET /api/logs; uses exact-sizing + 24 KB cap;
            never sends a malformed body.
        handleCORS — OPTIONS preflight.
        handleScanNetworks — GET /scan; OTA-gated and audio-busy-gated;
            evdJsonEscape on SSIDs.
        handleGetVolume, handlePostVolume — GET/POST /api/volume (the
            per-speaker and node-level volume/mute/crossover/source
            endpoint). All writes under g_audio_config_mutex; NVS deferred
            via pending_*_save flags.
        handleGetHealth — GET /api/health; 114 keys, 6144 B doc pool,
            OOM-guarded serialize.
        handleStatusPage — GET /status (serves EVDSET_STATUS_PAGE).
        handleRoot — GET /; serves EVDSET_HTML_GZ (or the raw
            EVDSET_PAGE_HTML if the client does not Accept-Encoding:gzip).
        handleCancel — POST /cancel; OTA-gated, loadStoredPreferences() +
            per-speaker configure() under the audio mutex.
        wifiTestTask, saveTestedNetworkSettings, handleNetworkTestStatus —
            AP-mode Wi-Fi test path; on success, saves and reboots.
        handleSave — POST /save; NVS writes under g_prefs_mutex,
            Phase 2 audio changes under g_audio_config_mutex, Phase 3
            per-speaker NVS writes. Rebooting on Wi-Fi change (STA mode)
            or redirect-to-new-address (AP mode).
        loadStoredPreferences — the boot-time load; three-phase
            (NVS reads / publish Wi-Fi identity / publish audio-relevant
            state under g_audio_config_mutex); clamp crossovers on the way
            out.
        handleStatsReset — POST /api/stats/reset; zeroes every cumulative
            counter and re-anchors timestamps.
        registerCommonRoutes, registerAppRoutes — idempotent route table
            registration.
        startCaptivePortal — the only function that flips to AP mode; must
            run on networkTask, never from loop().

  EVDSET.h
      The HTML dashboard sources and the fixed-buffer JSON helper. Contains:
        evdJsonEscape — the shared fixed-buffer JSON string escaper.
        EVDSET_PAGE_HTML — the full main dashboard page (PROGMEM). The
            live page is rendered by the JS bootFromCfg() from GET /api/cfg,
            not by server-side string substitution, so this file is
            gzip-friendly.
        EVDSET_STATUS_PAGE — the standalone /status page (PROGMEM).
      Do not edit EVDSET_PAGE_HTML without regenerating EVDSET_GZ.h.

  EVDSET_GZ.h  (generated; not attached)
      Gzip-compressed copy of EVDSET_PAGE_HTML. Generated by gen_gz.py /
      gen_gz.bat. Served by handleRoot() with Content-Encoding: gzip.
      STALE-FILE HAZARD: if you edit EVDSET.h and forget to regenerate
      this header, the build succeeds and the OLD dashboard is served.
      The page-load size is printed by handleRoot() on any dashboard
      fetch, so a stale gzip is visible in the ring.

  EVDPMain.h
      setup() and loop(). setup_serial + boot diagnostics (reset reason
      + persistent boot count), first malloc of pcm_decode_buffer /
      dac*_output_buffer, node UUID from eFuse, g_prefs_mutex creation,
      isConfigured read, loadStoredPreferences, initial
      speaker_filters[i].configure, g_audio_config_mutex creation,
      init_i2s, configureAudioHzDiag, opus_decoder_create,
      audio_queue creation, audioPlaybackTask + udpAudioTask creation,
      GPIO1 boot check -> captive portal, STA join, tryJoinServerAccessPoint
      fallback, captive portal, mDNS, registerAppRoutes +
      registerCommonRoutes + server.begin, discover_server,
      setupMulticastSockets, networkTask + pingServerTask
      (+ serverProbeTask when discovery failed) creation, then
      gPlayerLogLevel = 1. loop() handles the 3 s GPIO1 hold
      (sets g_enter_ap_mode_requested, the actual transition runs on
      networkTask) and yields at 20 ms.

  --- Non-C++ project files ------------------------------------------------

  EnkelvoudESPPlayer.ino
      This file. Includes the headers in the load-bearing order below,
      defines the merged EVDPConfig.h block (pins, constants, POD structs,
      forward declarations), and provides only the `setup()` / `loop()`
      stubs — all logic lives in the headers. No arithmetic or I/O belongs
      here.

  build_opt.h
      Build flags: LWIP pool sizes (pbuf pool 256, max TCP 16, max sockets
      16), CORE_DEBUG_LEVEL 0, FreeRTOS run-time stats (used by the C0/C1
      pills). Changing any of these requires a full rebuild of the Arduino
      core, not just the sketch.

  ============================ Include order ================================

  The order below is load-bearing. Each header assumes the symbols
  introduced by everything above it in this list.

    EVDPProtocolCompat.h  no deps; must come before EVDPConfig.h block
    EVDPConfig.h (merged)   defines constants + POD structs; needs
                            EVDPProtocolCompat.h for AUDIO_HEADER_BYTES
    EVDPState.h             no deps beyond EVDPFilters.h; defines every
                            shared global. Included AFTER the merged
                            EVDPConfig.h block so SAMPLE_RATE / CHANNELS /
                            AUDIO_QUEUE_LEN etc. are visible.
    EVDPFilters.h           no deps; Biquad + SpeakerFilter types.
                            Included by EVDPState.h directly (BEFORE the
                            SpeakerFilter array declaration) so the
                            include order is enforced there, not here —
                            the extra include below is a belt-and-braces
                            redundancy.
    EVDPNet.h               uses EVDPState globals + postJson etc.;
                            defines udpAudioTask / networkTask /
                            pingServerTask / serverWatcherTask
    EVDPAudio.h             uses EVDPState + EVDPFilters; defines
                            init_i2s, process_and_output_audio,
                            audioPlaybackTask
    EVDPWeb.h               uses EVDPState + EVDPNet; pulls in EVDSET.h and
                            EVDSET_GZ.h; defines HTTP handlers + OTA +
                            startCaptivePortal
    EVDPMain.h              uses EVDPWeb (setup's route registration) +
                            EVDPAudio (init_i2s / task creation) +
                            EVDPNet (networkTask creation); defines
                            setup() + loop()

  ============================ Task layout ==================================

  Core 0:
    WiFi / lwIP       pri 18-23  system
    udpAudioTask      pri 5      audioUdp + ctrlUdp drain (never does
                                 NVS/HTTP/String work)
    networkTask       pri 4      HTTP server, discovery, heartbeat POSTs,
                                 clock-sync poll, OTA watchdog
    serverWatcherTask pri 2      captive-portal AP watcher (only in AP mode)
    pingServerTask    pri 1      /api/player/register heartbeat
    serverProbeTask   pri 1      diagnostics-only when discovery fails
    loop()            pri 1      factory reset pin (deferred to
                                 networkTask), otherwise idle

  Core 1:
    audioPlaybackTask pri 3      Opus decode + full-length PCM +
                                 crossovers + i2s_write
                                 (sole occupant of Core 1)

  ============================================================================
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <driver/i2s.h>
#include <opus.h>
#include <ArduinoJson.h>
#include <cmath>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <HTTPClient.h>
#include <Update.h>

// ============================================================================
//  MERGED FROM EVDPConfig.h - Pins, constants, POD structs, forward decls
//
//  Intentionally does NOT depend on EVDPFilters.h. SpeakerFilter is owned
//  by EVDPState.h, which includes EVDPFilters.h directly.
// ============================================================================
#include <Arduino.h>
#include <WebServer.h>
#include "EVDPProtocolCompat.h"

// Server endpoint that receives the OTA completion notification. The
// server must implement a POST handler for this path. Body:
//   {"name":"<friendly_name>","success":true|false}
// Sent by handleNodeOtaDone() after the new firmware is fully written to
// the inactive OTA partition, before Update.end() commits it.
#define OTA_RESULT_ENDPOINT "/api/player/ota_result"

// How long the node will wait between two successive OTA chunks before it
// concludes the upload died mid-stream and aborts. Refreshed on every
// UPLOAD_FILE_WRITE, so this is a "no-progress" timeout, not a total
// duration timeout. 90 s is a generous ceiling: a 1-2 MB firmware over a
// healthy LAN uploads in 5-30 s, so 90 s of silence from the uploader
// means the client is gone. Without this watchdog, otaNodeUpdating is
// only cleared by UPLOAD_FILE_ABORTED / handleNodeOtaDone, and a client
// that half-closes the socket without the WebServer firing ABORTED would
// leave audioPlaybackTask paused forever.
#define OTA_STUCK_TIMEOUT_MS 90000UL

// Fallbacks for a fresh node with no saved Wi-Fi credentials.
#define SERVER_AP_SSID      "Enkelvoud Server"
#define SERVER_AP_PASSWORD  ""
#define DEFAULT_MDNS_HOST   "Enkelvoud.local"

#define RESET_PIN 1

// ---- Multicast transport ---------------------------------------------------
#define MULTICAST_GROUP_OCTETS  239, 255, 42, 99
#define AUDIO_UDP_PORT          4202
#define CTRL_UDP_PORT           4203
#define DISCOVERY_PORT          4201
#define DISCOVERY_MAGIC_REQ     "ENKELVOUDP/1 DISCOVER"
#define DISCOVERY_MAGIC_RPY     "ENKELVOUDP/1 SERVER"
// Fallback only: used before the first /api/player/register response tells
// us the server's actual CLOCK_SYNC_UDP_PORT (see g_clock_sync_port in
// EVDPState.h). Matches the server's current default so a first-contact
// sync_req still lands on the right port even before negotiation completes.
#define CLOCK_SYNC_UDP_PORT_DEFAULT 4204

// ---- Audio format ----------------------------------------------------------
#define SAMPLE_RATE        48000
#define CHANNELS           2
#define OPUS_FRAME_SIZE    960
#define I2S_NUM_DAC1       I2S_NUM_0
#define I2S_NUM_DAC2       I2S_NUM_1
// [opus-packet-len-rebalance]
// 1024-byte payload cap + 32-slot queue is the sweet spot for this
// pipeline. It clears every practical server configuration (96 kbps
// CBR = ~240 B/frame; 128 kbps = ~320 B; 256 kbps = ~640 B; 400 kbps
// VBR peak = ~1000 B) with margin, and leaves the receive-path drop
// threshold comfortable at every server bufferMs value:
//     bufferMs=150  -> target  4/24  (margin 20)
//     bufferMs=300  -> target 12/24  (margin 12)
//     bufferMs=400  -> target 17/24  (margin  7)
// vs the 1500/24 build which left only 1-slot of margin at 400 ms,
// where a single 20 ms WiFi jitter spike silently dropped frames.
//
// Also halves the AudioMessage stack cost during sequence-gap PLC
// recovery in enqueueAudioPacket(): that path holds two AudioMessages
// on the udpAudioTask 6144-byte stack simultaneously (enqueueAudioPacket
// msg + enqueueConcealedFrame msg); 2*1516 + calls was ~5 KB, 2*1040
// + calls is ~4 KB.
//
// Heap: 32 * (1024 + 16) = 33,280 B — 3 KB BELOW the 1500/24 build
// (36,384 B) and only 3.7 KB above the original 600/48 build.
//
// If you actually need >400 kbps stereo, raise this AND OPUS_MAX_PACKET_BYTES
// on the server AND AUDIO_QUEUE_LEN together — never just this line.
#define MAX_OPUS_PACKET_LEN 1024


// ---- Audio queue -----------------------------------------------------------
// AUDIO_QUEUE_LEN is the absolute capacity (48 frames = 960 ms). The
// pipeline drains back to AUDIO_QUEUE_TARGET every frame, so the effective
// steady-state jitter buffer is <100 ms - the 960 ms figure is only the
// no-progress ceiling, not headroom an operator can rely on.
//
// QUEUE_TARGET must equal ceil(playout_delay_ms / 20) so a frame dequeued
// now has its deadline exactly playout_delay_ms after capture. Was 10 (200 ms
// vs 120 ms playout = 80 ms of permanent lag per frame). Prebuffer must equal
// target, otherwise buffering exits at a deeper level than the target and
// the first frame is already (prebuffer - target) × 20 ms late.
// [opus-queue-len-rebalance] 32 slots pairs with MAX_OPUS_PACKET_LEN 1024.
// 32 * (1024 + 16) = 33,280 B — slightly smaller than the 1500/24 build
// (36,384 B) despite having more slots, and materially better than the
// original 600/48 build (29,568 B) only by 3.7 KB. 32 slots = 640 ms max
// dwell, which comfortably covers the server's 400 ms bufferMs ceiling
// with the caps applied in applyDelayToQueueTarget() (target <= 24,
// prebuffer <= 24). The prebuffer / queue-target interaction in
// applyRegistrationResponse() and applyCtrlJson() both clamp to
// (AUDIO_QUEUE_LEN * 3) / 4 = 24, so the highest server-configurable
// bufferMs (400 ms -> target 17) leaves 7 slots of jitter margin instead
// of 1.
#define AUDIO_QUEUE_LEN     32
#define AUDIO_QUEUE_TARGET  3    // applyDelayToQueueTarget() yields 3 at
                                 // the default delayMs=120 / netLat=20;
                                 // 6 caused a one-time corrector catch-up
                                 // burst on every boot.
#define AUDIO_PREBUFFER     3    // was 12, then 6; now equals AUDIO_QUEUE_TARGET
                                 // so the boot-time corrector catch-up burst
                                 // (3 frames = 60 ms) no longer happens.

// ---- Audio packet header format ---------------------------------------------
// The wire format (12-byte v2 header: magic/version/flags/seq/captured_ms)
// and the AUDIO_PROTO_*/AUDIO_HDR_*_BYTES/AUDIO_HEADER_BYTES constants are
// defined in EVDPProtocolCompat.h, which is the single source of truth
// shared with the host-side unit test (tests/test_protocol_compat.cpp) and
// must exactly match the server's EVDConfig.h AUDIO_PROTO_* docs. This
// firmware is v2-only; there is no v1 fallback in the hot path.

// Bound how many concealment (PLC) frames we synthesize for a single
// sequence gap. A short gap (a few lost UDP packets) is worth concealing;
// a huge gap (server restart, long Wi-Fi outage) is not real packet loss in
// the audible sense and should just resync, not flood the queue with PLC.
#define AUDIO_MAX_CONCEAL_FRAMES  10

// [p2-conceal-watermark]
// Separate allowance for the PLC used as a stutter ABSORB (repeated-stale-frame
// path in audioPlaybackTask), distinct from AUDIO_MAX_CONCEAL_FRAMES, which
// caps how much PLC one sequence gap may synthesize. Six frames = 120 ms of
// concealment per second: enough to ride out a WiFi jitter burst without the
// audible crunch-then-gap of a flush+rebuffer, small enough that a genuinely
// late timeline still trips the flush.
#define PLC_ALLOWANCE_PER_SEC     6

// How many ms a frame's playout deadline may already have passed before we
// discard it outright instead of playing it immediately.
#define AUDIO_STALE_DISCARD_MS      200
// How late a frame may be before we conclude the whole timeline has drifted
// too far to recover incrementally, and force a full flush + rebuffer.
#define AUDIO_STALE_REANCHOR_MS     600
// Consecutive stale-discards (without a healthy frame in between) that also
// force a full flush + rebuffer, even if no single frame hit the reanchor
// threshold above.
#define AUDIO_STALE_STREAK_LIMIT    5

// A time_sync sample disagreeing from the tracked offset EWMA by more than
// this many ms cannot be explained by ordinary jitter/drift -- treat it as a
// server reboot or 32-bit millis() rollover and force a full resync instead
// of slowly chasing a bad average.
#define CLOCK_RESYNC_JUMP_MS        1500
// [drift-refresh] The min-RTT filter freezes the anchor on a stable link
// (RTT improvement never fires, jump threshold is reboot-only). Crystal
// drift accumulates unbounded: 20 ppm = 72 ms/hour, 100 ppm = 360 ms/hour,
// both well past the 20 ms frame duration. This threshold makes
// pollClockSyncResponse() ALSO re-adopt the current min-RTT sample's
// offset when it has drifted this far from the tracked offset. 4 ms sits
// above LAN RTT jitter bias (~1–2 ms) so it never churns, and well below
// the visible-lateness threshold so drift is bounded to ~200 s of
// accumulation at 20 ppm / ~40 s at 100 ppm.
#define CLOCK_DRIFT_REFRESH_MS      4

// Maximum ms/frame the live playout_delay_ms is allowed to step toward
// target_playout_delay_ms, so a mid-stream bufferMs change from the server
// never moves an in-flight deadline abruptly.
#define PLAYOUT_DELAY_SLEW_MS_PER_FRAME  2

// NOTE: SET_LOOP_TASK_STACK_SIZE(24*1024); is invoked once in EVDPMain.h.
// Do NOT #define it here -- it is provided by the ESP32 Arduino core.

// ---- SpeakerConfig (shared with EVDSET.h) ----------------------------------
#ifndef SPEAKER_CONFIG_STRUCT
#define SPEAKER_CONFIG_STRUCT
struct SpeakerConfig {
    char name[32];
    int volume;
    int low_freq;
    int high_freq;
    int source;            // 0 = Left, 1 = Right, 2 = Mono
};
#endif

// ---- Audio queue message ---------------------------------------------------
struct AudioMessage {
    uint8_t  payload[MAX_OPUS_PACKET_LEN];
    uint16_t length;         // 0 for a synthesized concealment (PLC) frame
    uint32_t arrival_ms;     // local millis() at reception
    uint32_t captured_ms;    // server capture stamp (real or estimated)
    uint32_t seq;            // frame sequence number (always present; v2-only)
    bool     conceal;        // true: ask Opus PLC to synthesize this frame
};

// ---- Forward declarations --------------------------------------------------
void loadStoredPreferences();
// [v1.1.1-log-string-ref] MUST match the const-ref inline definitions in
// EVDPNet.h. A by-value declaration here next to a const-ref definition
// there is two distinct overloads, and a call site passing a string literal
// or a StringSumHelper (i.e. any `log_message("a" + b)` expression) is
// ambiguous per the C++ standard: both candidates require exactly one
// user-defined conversion. GCC used to silently prefer one; on this toolchain
// it now errors out on every such call site.
void log_message(const String &msg);
void log_warn(const String &msg);

void audioPlaybackTask(void *pvParameters);
void networkTask(void *pvParameters);
void pingServerTask(void *pvParameters);
void discover_server();

// [startCaptivePortal-fwd]
// startCaptivePortal() is defined in EVDPWeb.h, but networkTask() — which
// lives in EVDPNet.h and is included *before* EVDPWeb.h — calls it from
// the deferred-AP-mode branch. Without this forward declaration the
// compiler sees the call before the definition and errors out with
// "startCaptivePortal was not declared in this scope". Declaring it here
// is sufficient: the inline definition in EVDPWeb.h is still visible
// later in the same translation unit (the .ino is one TU), so the call
// binds and the compiler emits the inline body.
void startCaptivePortal();

void init_i2s();
void process_and_output_audio(const int16_t* in_pcm, int samples_per_channel);
// [applyClockTrim-removed] Function was a no-op (unconditionally reset
// stat_rate_trim_hz=0 every 4 s). No sample-clock drift controller is active.
void flushAudioQueueAndRebuffer(const char* reason);
void handleServerEpoch(uint32_t epoch);
void postRegister();
void sendClockSyncRequest();
void pollClockSyncResponse();
// ============================================================================
//  END MERGED FROM EVDPConfig.h
// ============================================================================

#include "EVDPState.h"
#include "EVDPFilters.h"
#include "EVDPNet.h"
#include "EVDPAudio.h"
#include "EVDPWeb.h"
#include "EVDPMain.h"
