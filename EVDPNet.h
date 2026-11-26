#pragma once
// ============================================================================
//  EVDPNet.h - WiFi bring-up, discovery, mDNS, network task
// ============================================================================
#include "EVDPState.h"
#include "EVDSET.h"
#include <ESPmDNS.h>
// [update-h-self-sufficient]
// udpAudioTask()'s OTA-pause guard now calls Update.isRunning() so the
// header pulls the class in directly instead of relying on the .ino
// having included <Update.h> first. Keeps the header compilable in
// isolation (host-side tooling, future .cpp extractions).
#include <Update.h>
// [master-rec] Server-driven recording buffer (EVDPRec.h, included below the
// log helpers it calls). Declared here so logRingPush() can feed it.
static inline void recMaybeAppend(const char *line);
static inline bool recStart(const String &id);
static inline void recStop();
// [log-ring-lock]
// logRingPush is called from four tasks across both cores:
//   - networkTask       (Core 0): registration / discovery / stats
//   - udpAudioTask      (Core 0): ctrl path applyCtrlJson()
//   - audioPlaybackTask (Core 1): audioLogThrottled + hot-path warnings
//   - Arduino loopTask  (Core 1): boot diagnostics
// Without a lock, two writers can read the same head index, both
// strncpy into the same slot, and both advance head — so one line is
// lost AND system_log_count drifts ahead of the number of slots that
// were actually written. handleGetLogs then serialises that
// uninitialised slot (which contains whatever the static initialiser
// left, i.e. all zeroes → an empty line), so the failure mode is
// cosmetic today; but if the ring is ever resized or the header
// layout changes, this becomes a real read of uninitialised memory.
// portENTER_CRITICAL (not taskENTER_CRITICAL) is the cross-core-safe
// spinlock: it disables interrupts on the calling core and blocks the
// peer core through the spinlock. The critical section is one 160-byte
// memcpy + three scalar updates — a few hundred nanoseconds — which is
// well under the audio DMA ISR's worst-case latency budget.

static portMUX_TYPE g_log_ring_mux = portMUX_INITIALIZER_UNLOCKED;
// [gap-stat-mux] Cross-core spinlock for stat_max_gap_ms_this_sec:
// written by udpAudioTask (Core 0, enqueueAudioPacket) via read-modify-write
// while audioPlaybackTask (Core 1) clears it at every 1 s rollover.
// Without it, a Core 0 write racing the Core 1 reset resurrects the previous
// second's max into the current second (stale diagnostic value leaks forever
// until a larger gap happens to overwrite it).
static portMUX_TYPE g_gap_stat_mux = portMUX_INITIALIZER_UNLOCKED;

inline void logRingPush(const String &msg) {
    // Copy the caller's payload outside the lock. msg.c_str() may walk
    // a String whose SSO/heap layout we do not control, and strncpy on
    // a 160-byte buffer is the slowest part of this function.
    char buf[MAX_LOG_LINE_LEN];
    strncpy(buf, msg.c_str(), MAX_LOG_LINE_LEN - 1);
    buf[MAX_LOG_LINE_LEN - 1] = '\0';

    portENTER_CRITICAL(&g_log_ring_mux);
    int idx = system_log_head;
    memcpy(system_logs[idx], buf, MAX_LOG_LINE_LEN);
    system_log_head = (system_log_head + 1) % MAX_LOG_LINES;
    if (system_log_count < MAX_LOG_LINES) system_log_count++;
    portEXIT_CRITICAL(&g_log_ring_mux);

    // [master-rec] Mirror the line into the recording buffer. Outside the log
    // ring's critical section: the 160-byte PSRAM copy must not extend the
    // hold time seen by the audio tasks.
    recMaybeAppend(buf);
}

// [v1.1.1-log-string-ref]
// Was: void log_message(String msg)  -- pass-by-value forced a copy-construct
// of the caller's temporary String on every call, i.e. two heap allocations
// and two frees per line even when the level gate discarded the message.
// applyCtrlJson() calls this twice per ctrl packet; at level 1 that was pure
// heap churn on Core 0 next to the audio UDP drain. const-ref removes the
// copy; the call sites below gate the source expression too, so the String
// is never constructed at all when the level gate would drop it.
inline void log_message(const String &msg) {
    if (gPlayerLogLevel >= 3) Serial.println(msg);
    logRingPush(msg);
}

inline void log_warn(const String &msg) {
    Serial.println(msg);
    logRingPush(msg);
}

#include "EVDPRec.h"        // [master-rec] needs log_warn/log_message above

// ---- Sanitize node name ----------------------------------------------------
inline String sanitizeNodeName(const String &value) {
    String result;
    for (size_t i = 0; i < value.length(); ++i)
        if (isAlphaNumeric(value[i]) || value[i] == '-') result += value[i];
    return result.length() ? result : "EnkelvoudNode";
}

// ---- HTTP POST helpers -----------------------------------------------------
// failCounter is optional; when supplied it is incremented on any failure
// (begin failed, timeout, non-2xx response). Successful POSTs update the
// server-contact timestamp so the health endpoint can report how long
// ago we last reached the server. outResponse is optional; when supplied
// and the POST succeeds, it receives the response body (used by
// postRegister() to read back negotiated protocol/compat/epoch fields).
inline bool postJson(const String &path, const char *body, size_t len,
                     volatile uint32_t *failCounter, String *outResponse = nullptr) {
    if (!serverFound || WiFi.status() != WL_CONNECTED) {
        if (failCounter) (*failCounter)++;
        return false;
    }
    WiFiClient client;
    HTTPClient http;
    // [stats-post-timeout]
    // 1500 ms was inherited from a time when the server had 48 PBUFs and
    // occasional HTTP stalls lasting 1-1.4 s. That made every failed POST
    // hold networkTask for a full 1.5 s. On these nodes the diagnostic
    // shows 11-16 failed stats POSTs per 20 min uptime -- 16-24 seconds
    // of blocked networkTask per session, all of it spent inside
    // HTTPClient::POST while registration, clock-sync force, and
    // re-discovery timers were all frozen. On a LAN the server either
    // answers in <100 ms or it is not going to answer at all; 600 ms is
    // generous for a healthy path and cuts the worst-case block by 60 %.
    http.setTimeout(600);
        if (!http.begin(client, "http://" + serverIP.toString() + path)) {
        if (failCounter) (*failCounter)++;
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST((uint8_t *)body, len);
    bool ok = code >= 200 && code < 300;
    if (ok && outResponse) *outResponse = http.getString();
    http.end();
    if (ok) {
        ts_last_server_contact_ms = millis();
    } else if (failCounter) {
        (*failCounter)++;
    }
    return ok;
}

// ---- Server epoch change handling -------------------------------------------
// serverEpoch (see EVDState.h on the server) is a random value generated
// once at server boot; it changes on every reboot/reset. A change means the
// server's capture-timestamp timeline and sequence baseline have reset, so
// any cached offset/seq state here is now wrong and must be rebuilt rather
// than slowly chased. Callable from any task (networkTask's registration
// response handling, or pollClockSyncResponse()/applyCtrlJson() on the ctrl
// path) -- only sets flags/volatiles here, no blocking I/O, so it is safe to
// call from udpAudioTask's ctrl-packet handling too.
inline void handleServerEpoch(uint32_t epoch) {
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool firstObs = false, changed = false;
    uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    if (!g_server_epoch_known) {
        g_server_epoch_known = true;
        g_server_epoch = epoch;
        firstObs = true;
    } else if (epoch != g_server_epoch) {
        // [epoch-debounce] Ignore rapid flip-flops from delayed packets.
        // A real server reboot takes seconds; a stale pre-reboot datagram
        // arrives within ms of the post-reboot one.
        if (g_last_epoch_change_ms == 0 ||
            (uint32_t)(now - g_last_epoch_change_ms) >= 5000) {
            g_server_epoch = epoch;
            g_last_epoch_change_ms = now;
            changed = true;
        }
    }
    portEXIT_CRITICAL(&mux);

    if (firstObs) return;
    if (!changed) return;

    server_clock_synced   = false;
    g_have_rtt_sync        = false;
    g_audio_compat_state   = AUDIO_COMPAT_UNKNOWN;
    g_force_resync         = true;
    g_force_reregister     = true;
    g_session_fade_needed  = true;
    g_current_offset_rtt   = 0xFFFFFFFFu;
    stat_frames_late        = 0;
    stat_late_lt5           = 0;
    stat_late_5to20         = 0;
    stat_late_20to100       = 0;
    stat_late_100to200      = 0;
    stat_frames_discarded   = 0;
    stat_frames_concealed   = 0;
    // [epoch-resets-captured-ms-counters]
    // The epoch-change block above already clears the framesLate* /
    // framesDiscarded / framesConcealed counters, but it did NOT clear
    // the captured_ms-gap and corrector counters that the Status page
    // reads as proof that the node is healthy in steady state. Those
    // counters accumulated across server reboots, so a fresh reflash
    // that carried a real fix still showed cumulative totals from
    // before the patch in /api/health. Field example (post-patch
    // diagnostics 2026-11-06T15:41): node 210 reported
    // capturedMsGapGt20Total=156, Gt40Total=38 while capturedMsGapMax-
    // LastSec=20 (i.e. clean in the last second). The 156/38 were
    // pre-flash events; without this reset the operator cannot tell
    // "the fix didn't work" from "these are old numbers".
    //
    // statLastCapturedMs is reset because otherwise the first frame
    // after the epoch change computes a gap of "old captured_ms +
    // pre-reboot elapsed", spuriously tripping Gt20/Gt40 once per boot.
    statCapturedMsGapThisSec        = 0;
    statCapturedMsGapMaxLastSec     = 0;
    statCapturedMsGapGt20Total      = 0;
    statCapturedMsGapGt40Total      = 0;
    statLastCapturedMs              = 0;
    statCorrectorFramesTotal        = 0;
    statCorrectorFramesPeakThisSec  = 0;
    statCorrectorFramesPeakLastSec  = 0;
    stat_total_correction_samples   = 0;
}

// ---- Encoder info (shared by registration response and ctrl push) ----------
// Parses the encoder fields the server mirrors into both responses into
// the node's g_last_recv_enc_* fields. Returns true if any field changed,
// which the callers use to bump ts_last_recv_enc_ms so /api/health can
// report how stale the mirror is.
inline bool applyEncoderInfoFromJson(DynamicJsonDocument &doc) {
    bool changed = false;
    bool present = false;   // [enc-age-fix] at least one enc field was in the packet

    if (doc.containsKey("encBitrate") && doc["encBitrate"].is<int>()) {
        present = true;
        int32_t v = doc["encBitrate"].as<int32_t>();
        if (v != g_last_recv_enc_bitrate) { g_last_recv_enc_bitrate = v; changed = true; }
    }
    if (doc.containsKey("encComplexity") && doc["encComplexity"].is<int>()) {
        present = true;
        int32_t v = doc["encComplexity"].as<int32_t>();
        if (v != g_last_recv_enc_complexity) { g_last_recv_enc_complexity = v; changed = true; }
    }
    if (doc.containsKey("encVbr") && doc["encVbr"].is<bool>()) {
        present = true;
        int32_t v = doc["encVbr"].as<bool>() ? 1 : 0;
        if (v != g_last_recv_enc_vbr) { g_last_recv_enc_vbr = v; changed = true; }
    }
    if (doc.containsKey("encVbrConstraint") && doc["encVbrConstraint"].is<bool>()) {
        present = true;
        int32_t v = doc["encVbrConstraint"].as<bool>() ? 1 : 0;
        if (v != g_last_recv_enc_vbr_constraint) { g_last_recv_enc_vbr_constraint = v; changed = true; }
    }
    if (doc.containsKey("encSignal") && doc["encSignal"].is<const char*>()) {
        present = true;
        const char *s = doc["encSignal"];
        int32_t v = -1;
        if      (strcmp(s, "music") == 0) v = OPUS_SIGNAL_MUSIC;
        else if (strcmp(s, "voice") == 0) v = OPUS_SIGNAL_VOICE;
        else if (strcmp(s, "auto")  == 0) v = OPUS_AUTO;
        if (v != g_last_recv_enc_signal) { g_last_recv_enc_signal = v; changed = true; }
    }
    if (doc.containsKey("encApplication") && doc["encApplication"].is<const char*>()) {
        present = true;
        const char *s = doc["encApplication"];
        int32_t v = -1;
        if      (strcmp(s, "audio")    == 0) v = OPUS_APPLICATION_AUDIO;
        else if (strcmp(s, "voip")     == 0) v = OPUS_APPLICATION_VOIP;
        else if (strcmp(s, "lowdelay") == 0) v = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
        if (v != g_last_recv_enc_application) { g_last_recv_enc_application = v; changed = true; }
    }
    // [chipmunk-fix]
    // Parse opusSampleRate. The node's Opus decoder is fixed at SAMPLE_RATE
    // (48000) and libopus is supposed to upsample a 24 kHz stream transparently
    // during decode. In practice the transition window after a server-side
    // rate switch (48 kHz <-> 24 kHz via the Encoding tab) can leave stale
    // pipeline state that produces a brief 2x-speed "chipmunk" artifact
    // before the stream self-corrects.
    //
    // On the FIRST observed change (NOT the initial 0 -> N observation —
    // otherwise every fresh boot would trigger a rebuffer), drain the audio
    // queue, reset the Opus decoder, and restart the playout scheduler
    // against a fresh timeline. Cost: one audible ~40 ms blip per rate
    // switch. Benefit: no chipmunk. This is also what populates
    // g_last_recv_opus_sample_rate, so /api/health's serverOpusSampleRate
    // stops reporting null.
    if (doc.containsKey("opusSampleRate") && doc["opusSampleRate"].is<int>()) {
        present = true;
        uint32_t v = (uint32_t)(doc["opusSampleRate"].as<int>());
        if (v == 48000 || v == 24000) {
            if (g_last_recv_opus_sample_rate != 0 &&
                v != g_last_recv_opus_sample_rate) {
                log_message(String("[Opus] Encoder rate: ") +
                            String(g_last_recv_opus_sample_rate) + " -> " +
                            String(v) + " Hz (rebuffering)");
                if (!g_force_resync) {
                    g_force_resync = true;
                }
            }
            g_last_recv_opus_sample_rate = v;
            changed = true;
        }
    }
    // [source-rate-receive]
    // [source-rate-cascade-guard]
    // A drastic sourceSampleRate change (>20%) is almost always the
    // server's window-counter rate estimator misclassifying a BT pause
    // as a rate drop (see the Input rate anomaly logs on the server).
    // In that state the server is emitting frames whose captured_ms is
    // inconsistent with wall time by the size of the mis-classification
    // (up to 700 ms observed). Chasing that with the corrector takes
    // 30-40 s of audible stutter; flushing and re-buffering on a fresh
    // timeline takes one frame (~20 ms).
    //
    // The threshold is deliberately asymmetric: an opusSampleRate change
    // (48k↔24k) is a real encoder reconfiguration and libopus handles
    // it transparently during decode, so the existing chipmunk-fix
    // above already flushes for it. But a sourceSampleRate change that
    // is >20% off the previous value is not a rate change the node can
    // do anything useful with — the node always decodes at 48 kHz
    // regardless of what the server is doing internally.
    if (doc.containsKey("sourceSampleRate") && doc["sourceSampleRate"].is<int>()) {
        present = true;
        uint32_t v = (uint32_t)(doc["sourceSampleRate"].as<int>());
        if (v >= 8000 && v <= 96000) {
            if (v != g_last_recv_source_hz) {
                if (g_last_recv_source_hz != 0) {
                    // [source-rate-signed-delta-fix]
                    // The previous expression `(int64_t)(v - prev)` computes
                    // `v - prev` in uint32_t BEFORE the cast to int64_t. On a
                    // DECREASE (the common case — the server's rate estimator
                    // wobbles 1-30 Hz on every ctrl push) the unsigned
                    // subtraction wraps to ~4.29e9; the resulting percentage
                    // is ~9.7 million %, `absPct > 20` fires on every packet,
                    // and each firing sets g_force_resync -> flushAudioQueue
                    // AndRebuffer("clock resync"). That is the "hard drop
                    // every other second" symptom the operator reported:
                    // six consecutive 44148->44122->44110->...->44096
                    // decreases producing six flushes in ~20 s.
                    //
                    // Two independent corrections:
                    //
                    //   1. Cast BOTH operands to int64_t before the
                    //      subtraction so a decrease stays a small negative
                    //      int64 (44122 - 44148 = -26) instead of wrapping.
                    //
                    //   2. Add an absolute |deltaHz| >= 500 Hz floor on top
                    //      of the >20 % test. Real BT codec switches are
                    //      1500 Hz or more (44.1k<->48k = 3900 Hz,
                    //      48k<->32k = 16000 Hz). Server estimator wobble is
                    //      < 100 Hz. 500 Hz rejects the observed 1-30 Hz
                    //      jitter that produced 6+ flushes per 20 s while
                    //      still firing on every real source reconfiguration
                    //      or misclassified BT pause.
                    uint32_t prev     = g_last_recv_source_hz;
                    int64_t  delta64  = (int64_t)v - (int64_t)prev;
                    int32_t  deltaHz  = (int32_t)delta64;
                    int32_t  absDelta = deltaHz < 0 ? -deltaHz : deltaHz;
                    double   pctDelta = (prev > 0)
                                        ? (100.0 * (double)delta64 / (double)prev)
                                        : 0.0;

                    log_message(String("[Source] Rate: ") +
                                String(prev) + " -> " +
                                String(v) + " Hz (" +
                                String(deltaHz) + " Hz)");

                    if (absDelta >= 500) {
                        g_force_resync = true;
                        log_warn(String("[Source] Drastic rate change (") +
                                 String(deltaHz) + " Hz, " +
                                 String(pctDelta, 2) + "%); forcing rebuffer " +
                                 "to recover from misclassified BT pause.");
                    }
                }
                g_last_recv_source_hz = v;
                changed = true;
            }
        }
    }
    if (present) ts_last_recv_enc_ms = millis();
    return changed;
}

// ---- Registration response handling -----------------------------------------
// Parses the compatibility/control fields the server's /api/player/register
// response carries on every call (not just first sight), per
// handlePlayerRegistration on the server: audioProtocolVersion,
// negotiatedAudioProtocolVersion, audioCompatible, audioCompatReason, epoch,
// clockSyncPort. See EVDPProtocolCompat.h for the state this derives.
inline void applyRegistrationResponse(const String &body) {
    if (body.length() == 0) return;

    // The registration response carries ~15 fields (protocol/epoch/
    // clockSyncPort, masterVolume/masterMuted/bufferMs, and six
    // encoder fields). ArduinoJson 6 copies the const-String input
    // into the pool and allocates one member per key + value, so 512 B
    // is not enough — deserializeJson() fails with NoMemory and the
    // entire response is silently dropped. 1536 B fits the current
    // shape with headroom for any two extra fields the server might
    // add later.
    DynamicJsonDocument doc(1536);
    if (deserializeJson(doc, body)) {
        // [register-parse-counter] This is a register-response failure, not a
        // ctrl-channel one; the old code bumped stat_ctrl_json_errors and
        // sent the operator to look at udpAudioTask. Use the register counter.
        stat_post_register_fails++;
        return;
    }
    if (doc.overflowed()) {
        // A future server field that doesn't fit will land here.
        stat_post_register_fails++;
        log_warn("[Register] JSON overflow; response truncated.");
        return;
    }

    bool fieldsPresent = doc.containsKey("audioProtocolVersion") &&
                         doc.containsKey("audioCompatible");
    int  negotiated  = doc["negotiatedAudioProtocolVersion"] | 0;
    bool compatible  = doc["audioCompatible"] | false;
    const char *reasonStr = doc["audioCompatReason"] | "";

    AudioCompatInfo info = evaluatePlayerAudioCompat(fieldsPresent, compatible, negotiated);
    bool stateChanged = ((int)info.state != g_audio_compat_state);

    g_audio_compat_state                 = (int)info.state;
    g_negotiated_audio_protocol_version   = info.negotiatedProtocolVersion;

    const char *reasonToStore = (fieldsPresent && reasonStr[0]) ? reasonStr : info.reason;
    strncpy(g_audio_compat_reason, reasonToStore, sizeof(g_audio_compat_reason) - 1);
    g_audio_compat_reason[sizeof(g_audio_compat_reason) - 1] = '\0';

    if (stateChanged) {
        if (info.state == AUDIO_COMPAT_COMPATIBLE) {
            log_message(String("[Protocol] Audio COMPATIBLE (negotiated v") +
                        String(info.negotiatedProtocolVersion) + ").");
        } else if (info.state == AUDIO_COMPAT_INCOMPATIBLE) {
            log_warn(String("[Protocol] Audio INCOMPATIBLE (") + reasonToStore +
                     "). Withholding audio until a compatible registration.");
            g_force_flush_incompatible = true; // audioPlaybackTask flushes safely
        } else {
            log_message("[Protocol] Server did not declare protocol-negotiation "
                        "fields (old/pre-negotiation server).");
        }
    }

    if (doc.containsKey("epoch")) handleServerEpoch((uint32_t)(doc["epoch"] | 0UL));
    if (doc.containsKey("clockSyncPort")) {
        long port = doc["clockSyncPort"] | 0;
        if (port > 0 && port <= 65535) g_clock_sync_port = (uint16_t)port;
    }

    // [post-epoch-compat-restore]
    // handleServerEpoch() resets g_audio_compat_state to AUDIO_COMPAT_UNKNOWN
    // whenever the epoch changed (server rebooted), to fail-closed against
    // any out-of-band ctrl push that might be stale. But THIS code path just
    // parsed a fresh, authoritative compat decision out of an HTTP response
    // from the (new) server. Overriding the conservative UNKNOWN with the
    // value the response actually carried closes the 3-5 s silence window
    // that used to open on every server reboot, because without the override
    // the g_force_sync_now check two blocks below silently failed and the
    // node withheld audio until g_force_reregister (also set by
    // handleServerEpoch) delivered a fresh registration ~3 s later.
    //
    // Only override when the response actually carried the negotiation
    // fields; a pre-negotiation server (fieldsPresent == false) must keep
    // the node in UNKNOWN so the failure mode stays visible in
    // /api/health.audioCompatState.
    if (fieldsPresent) {
        g_audio_compat_state                = (int)info.state;
        g_negotiated_audio_protocol_version = info.negotiatedProtocolVersion;
        // g_audio_compat_reason was already populated above from either the
        // server's reason string or info.reason; leave it untouched.
    }

    // [new-node-sync-gate]
    // The server withholds audio until its subscriber slot has seen a
    // sync_req (see handleClockSyncRequests / udpSendTask on the server).
    // Request a sync now so the promotion happens on the next networkTask
    // pass instead of waiting up to 2 s for the periodic timer. Only fire
    // when we actually believe we are compatible; an incompatible node
    // never needs the gate to open.
    if (g_audio_compat_state == AUDIO_COMPAT_COMPATIBLE) {
        g_force_sync_now = true;
    }

    // ---- Config in the registration response -----------------------------
    // The ctrl-push UDP packet is a single fire-and-forget datagram; if it
    // is lost the node has no other way to learn the server's bufferMs /
    // mode / master volume until the operator touches the dashboard. These
    // fields ride on every HTTP heartbeat instead, so a node that reboots
    // (or a server that reboots) re-syncs its config automatically within
    // one registration interval.
    if (doc.containsKey("masterVolume") && doc["masterVolume"].is<float>()) {
        float v = doc["masterVolume"].as<float>();
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        master_volume_multiplier = v;
    }
    if (doc.containsKey("masterMuted") && doc["masterMuted"].is<bool>()) {
        master_muted = doc["masterMuted"].as<bool>();
    }
    if (doc.containsKey("bufferMs") && doc["bufferMs"].is<int>()) {
        int b = doc["bufferMs"].as<int>();
        if (b < 20)  b = 20;
        if (b > 400) b = 400;

        // [bufferMs-cross-channel-reg]
        // Decreases must be confirmed by the ctrl channel, OR the ctrl
        // channel must be stale enough (>10 s) that the register's value
        // is the more recent observation.
        //
        // [bufferMs-mode-change] A server-side playback mode change
        // (Off/Music/Movie) carries a *lower* bufferMs on both the ctrl
        // push AND the next register heartbeat. Because the ctrl channel
        // is authoritative for the change, the register's value must
        // always be accepted when the mode it reports differs from the
        // last one. Without this, a Music(150) -> Movie(80) switch was
        // rejected by the cross-channel gate for up to 30 s until the
        // ctrl channel aged out, so the operator heard the new movie's
        // audio at the OLD 150 ms latency for the first heartbeat
        // interval of every mode change.
        static String lastRegMode = "";
        String regMode = doc["mode"] | "";
        bool   regModeChanged = (regMode.length() > 0 && regMode != lastRegMode);
        if (regMode.length() > 0) lastRegMode = regMode;

        last_reg_buffer_ms    = b;
        ts_last_reg_buffer_ms = millis();
        uint32_t ctrlAgeMs = (ts_last_ctrl_buffer_ms == 0)
                             ? 0xFFFFFFFFu
                             : (millis() - ts_last_ctrl_buffer_ms);
        bool accept = regModeChanged ||                 // <-- NEW
                      (b >= target_playout_delay_ms) ||
                      (last_ctrl_buffer_ms == -1)    ||
                      (b == last_ctrl_buffer_ms)     ||
                      (ctrlAgeMs > 10000UL);

        // [bufferMs-re-adopt-when-target-dropped]
        // See the matching block in applyCtrlJson(). The registration
        // heartbeat is what a fresh node sees first; without the same
        // re-adopt rule here, the very first register after boot can be
        // rejected the same way if self-tune had already shrunk below 150.
        if (accept && (b != last_server_buffer_ms ||
                       b > target_playout_delay_ms)) {
        // [auto-expand-reset-on-server-change]
        // Reset g_auto_expand_ms to 0 ONLY when the server's bufferMs is
        // genuinely different from the last value we saw. The server re-sends
        // bufferMs=50 on every 30 s register heartbeat (see
        // [zone-state-on-every-heartbeat] on the server side); treating that
        // as a "change" and wiping the auto-expand every 30 s would undo the
        // safety margin 2 s after every expansion. If the operator moves the
        // slider (or a mode preset change ships a new value), that IS a
        // change and the accumulator should start from scratch.
        if (b != last_server_buffer_ms) {
            g_auto_expand_ms = 0;
        }
        last_server_buffer_ms = b;

        int effective = b + g_auto_expand_ms;
        if (effective > 400) effective = 400;
        if (effective < 20)  effective = 20;
        target_playout_delay_ms = effective;

        // Recompute queue target against the EFFECTIVE delay, not the raw
        // server value, so the prebuffer matches the actual deadline.
        uint32_t netLat = last_node_latency_ms;
        if (netLat == 0) netLat = 20;
        if (netLat > (uint32_t)effective / 2) netLat = effective / 2;

        // [queue-target-no-hardcoded-margin] Aligned with
        // applyDelayToQueueTarget() in EVDPAudio.h, which removed the
        // hardcoded -4 because it forces target=7 at delay=300, netLat=80
        // while the natural dwell is 11. The audio task recomputes every
        // 1 s, but for ~1 s after every register heartbeat the target
        // dropped to 7, dropping the effective headroom to ~6 ms during
        // any burst that landed in that window. Keep the two paths
        // identical so the target cannot flap 7↔11 every 30 s.
        // [queue-target-smoothed-latency] See computeQueueTargetSmoothed()
        // in EVDPState.h. Previous inline computation duplicated the target
        // math and disagreed with the audio task's 1 s policy by ±5 frames
        // because each read the instantaneous last_node_latency_ms.
        int tgt = computeQueueTargetSmoothed(effective);

        int pre = tgt;
        if (pre < 2) pre = 2;
        if (pre > AUDIO_QUEUE_LEN - 2) pre = AUDIO_QUEUE_LEN - 2;

        audio_prebuffer    = pre;
        audio_queue_target = tgt;
}
    }

    // [zone-state-on-every-heartbeat]
    // The server mirrors this node's current zoneVolume / zoneMuted into
    // every registration response (see [zone-state-on-every-heartbeat] in
    // EVDPlayers.h on the server side). Without this, a node that reboots
    // inside its own stale window re-registers as "not new", skips the
    // first-sight host_audio_sync push, and boots a muted zone unmuted
    // until the operator touches the dashboard.
    if (doc.containsKey("zoneVolume") && doc["zoneVolume"].is<float>()) {
        float zv = doc["zoneVolume"].as<float>();
        if (zv < 0.0f) zv = 0.0f;
        if (zv > 1.0f) zv = 1.0f;
        zone_volume_multiplier = zv;
    }
    if (doc.containsKey("zoneMuted") && doc["zoneMuted"].is<bool>()) {
        zone_muted = doc["zoneMuted"].as<bool>();
    }

    // [encoder-info-passthrough] Parse the six encoder fields the server
    // mirrors into every registration response. A missing field means an
    // older server build; applyEncoderInfoFromJson() leaves the mirror at
    // -1, and the status page renders that as "—".
    applyEncoderInfoFromJson(doc);
}

inline void postRegister() {
    // [register-buf] friendly_name is 63 chars max; evdJsonEscape emits up
    // to 6 bytes per input byte (\uXXXX for <, >, &, control bytes, or per
    // UTF-8 byte for non-ASCII). 63*6+1 = 379. 160 truncated mid-escape ->
    // malformed JSON -> registration silently never landed -> node showed
    // offline forever with a non-ASCII name.
    char eName[6 * 64 + 1];
    evdJsonEscape(friendly_name, eName, sizeof(eName));
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"version\":\"%s\",\"audioProtocolVersion\":%d}",
        eName, FW_VERSION, PLAYER_DECLARED_AUDIO_PROTOCOL_VERSION);
    if (n > 0 && n < (int)sizeof(buf)) {
        String resp;
        if (postJson("/api/player/register", buf, (size_t)n, &stat_post_register_fails, &resp)) {
            applyRegistrationResponse(resp);
        } else {
            // [register-retry]
            // A single POST failure (Wi-Fi transient, server-side GC
            // pause, brief ARP miss) previously meant a full 30 s gap
            // before the next heartbeat. During that gap the server's
            // PLAYER_STALE_TIMEOUT_MS could evict our subscriber slot
            // and subsequent udpSendTask passes withhold audio until
            // the next sync_req promotes the slot again. Arm the
            // g_force_reregister escape hatch so pingServerTask's next
            // 3 s poll retries immediately. Cheap: one volatile write,
            // pingServerTask does the (blocking) POST.
            g_force_reregister = true;
        }
    }

    // [master-rec] One-shot ack: tells the server this node accepted rec_start
    // and will serve /api/rec_report. networkTask (core 0) does the POST so
    // the audio task never blocks on HTTP.
    if (g_recAckPending) {
        g_recAckPending = false;
        char ack[96];
        int an = snprintf(ack, sizeof(ack), "{\"recId\":\"%s\"}", g_recId.c_str());
        if (an > 0 && an < (int)sizeof(ack)) {
            postJson("/api/player/rec_ack", ack, (size_t)an, nullptr);
        }
    }
}

inline void postNodeStats() {
    char buf[256];
    // [stats-carry-head-and-late]
    // [stats-carry-head-and-late] The server's effectiveNodeBufferMsForIp()
    // computes per-node boosts from latencyMs and rebuffers only. The
    // 2026-11-15 17:30 recording shows the node sitting at head ~ 0,
    // ld20 == pkt, and 30 discards over 365 s -- but the server saw a
    // healthy node (rbuf == 0) and never raised the target. head / ld20
    // / disc / conc are already in the node's /api/health payload; send
    // them alongside the existing stats so the server-side log can
    // surface the node's struggle and (once handleNodeStats's warn
    // path is applied) so a future change to effectiveNodeBufferMsForIp
    // can boost on disc / conc directly instead of proxying through
    // rbuf.
    int n = snprintf(buf, sizeof(buf),
        "{\"latencyMs\":%u,\"queue\":%u,\"target\":%u,\"rebuffers\":%u,"
        "\"rateTrimHz\":%d,\"audioSourceRejects\":%u,"
        "\"head\":%d,\"leadBelow20\":%u,"
        "\"framesDiscarded\":%u,\"framesConcealed\":%u,"
        "\"version\":\"%s\"}",
        (uint32_t)last_node_latency_ms,
        (uint32_t)(audio_queue ? uxQueueMessagesWaiting(audio_queue) : 0),
        (uint32_t)audio_queue_target,
        (uint32_t)stat_rebuffers,
        (int32_t)stat_rate_trim_hz,
        (uint32_t)stat_audio_source_rejects,
        (int)g_playout_lead_ms,
        (uint32_t)stat_lead_below20_last_sec,
        (uint32_t)stat_frames_discarded,
        (uint32_t)stat_frames_concealed,
        FW_VERSION);
    if (n > 0 && n < (int)sizeof(buf))
        postJson("/api/node_stats", buf, (size_t)n, &stat_post_stats_fails);
}

// ---- OTA result notification -----------------------------------------------
// Called from handleNodeOtaDone() (EVDPWeb.h) after the whole firmware has
// been written into the inactive OTA partition but BEFORE Update.end()
// commits it. Lets the server mark the deployment complete without waiting
// for the next /api/player/register heartbeat, and works even if the
// uploader's HTTP connection dropped before our 200 response was read.
// A failure here is cosmetic: local flash already holds the firmware.
inline bool postOtaResult(bool success) {
    if (!serverFound || WiFi.status() != WL_CONNECTED) return false;
    char eName[80];
    evdJsonEscape(friendly_name, eName, sizeof(eName));
    char buf[192];
    int n = snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\",\"success\":%s}",
        eName, success ? "true" : "false");
    if (n <= 0 || n >= (int)sizeof(buf)) return false;
    return postJson(OTA_RESULT_ENDPOINT, buf, (size_t)n, nullptr);
}

// ---- Discovery -------------------------------------------------------------
inline bool discoverServerViaUdp(uint32_t timeoutMs) {
    WiFiUDP udp;
    if (!udp.begin(0)) {
        log_message("[Discovery] UDP socket open failed");
        return false;
    }

    // Build a small target list. Some home routers pass the directed
    // subnet broadcast but drop 255.255.255.255, and some do the reverse.
    // Sending to both maximises the odds of at least one getting through.
    IPAddress targets[3];
    int nTargets = 0;

    IPAddress directed = WiFi.broadcastIP();        // e.g. 192.168.100.255
    IPAddress limited (255, 255, 255, 255);

    if ((uint32_t)directed != 0 && (uint32_t)directed != 0xFFFFFFFF)
        targets[nTargets++] = directed;
    targets[nTargets++] = limited;

    // Also try the gateway's subnet broadcast in case WiFi.broadcastIP()
    // is wrong because the mask hasn't propagated yet.
    IPAddress gw = WiFi.gatewayIP();
    IPAddress mask = WiFi.subnetMask();
    if ((uint32_t)gw != 0 && (uint32_t)mask != 0) {
        uint32_t d = ((uint32_t)gw & (uint32_t)mask) | ~(uint32_t)mask;
        IPAddress dir2(d);   // IPAddress's uint32 is little-endian (first octet in low byte)
        bool dup = false;
        for (int i = 0; i < nTargets; ++i)
            if (targets[i] == dir2) dup = true;
        if (!dup && nTargets < 3) targets[nTargets++] = dir2;
    }

    uint32_t start  = millis();
    uint32_t lastTx = start - 300;   // fire first burst immediately
    bool found = false;

    while (millis() - start < timeoutMs) {
        // Retry every 300 ms instead of 400 ms — more chances in the same
        // window when the AP is dropping a fraction of broadcasts.
        if (millis() - lastTx >= 300) {
            lastTx = millis();
            for (int i = 0; i < nTargets; ++i) {
                udp.beginPacket(targets[i], DISCOVERY_PORT);
                udp.write((const uint8_t*)DISCOVERY_MAGIC_REQ,
                          strlen(DISCOVERY_MAGIC_REQ));
                udp.endPacket();
            }
        }

        int sz = udp.parsePacket();
        if (sz > 0) {
            // [discovery-src-ip] Trust the datagram SOURCE IP, not the IP
            // the reply body claims. The body field is attacker-controlled;
            // udp.remoteIP() is the kernel-verified source address. Any
            // mismatch is logged (diagnostic: server behind NAT, or spoof).
            IPAddress srcIp = udp.remoteIP();
            char buf[192];
            int n = udp.read(buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                String s(buf);
                s.trim();

                String magic(DISCOVERY_MAGIC_RPY);
                if (s.startsWith(magic + " ")) {
                    int lastSp = s.lastIndexOf(' ');
                    if (lastSp > (int)magic.length()) {
                        IPAddress bodyIp;
                        String body = s.substring(magic.length() + 1, lastSp);
                        if (bodyIp.fromString(s.substring(lastSp + 1)) &&
                            bodyIp != srcIp) {
                            log_message("[Discovery] Reply body IP (" +
                                        bodyIp.toString() + ") != source (" +
                                        srcIp.toString() + "); using source.");
                        }
                        serverIP    = srcIp;
                        serverFound = true;
                        log_message("[Discovery] Found server [" + body +
                                    "] at " + srcIp.toString());
                        found = true;
                        break;
                    }
                }
            }
        }
        delay(15);
    }
    udp.stop();

    if (!found) {
        log_message("[Discovery] No reply in " + String(timeoutMs) +
                    " ms (tried " + String(nTargets) + " broadcast addr(s))");
    }
    return found;
}

inline void discover_server() {
    serverFound = false;
    ts_last_discovery_attempt_ms = millis();

    // [no-server-address-config] There is no user-configurable server
    // address any more (it was how a node ended up stranded on a dead static
    // IP). Discovery is always: fixed mDNS name, UDP broadcast, SoftAP.

    // ---- 1. Fixed mDNS server name ----------------------------------------
    {
        String host = String(DEFAULT_MDNS_HOST);
        if (!host.endsWith(".local") && host.indexOf('.') == -1)
            host += ".local";
        IPAddress mdnsIp = MDNS.queryHost(host.c_str());
        if (mdnsIp != IPAddress(0, 0, 0, 0)) {
            serverIP    = mdnsIp;
            serverFound = true;
            log_message(String("[Discovery] mDNS ") + host +
                        " -> " + serverIP.toString());
            return;
        }
        log_message(String("[Discovery] mDNS did not resolve ") + host +
                    ", falling back to UDP");
    }

    // ---- 2. UDP broadcast --------------------------------------------------
    log_message("[Discovery] Broadcasting UDP DISCOVER (5 s)...");
    if (discoverServerViaUdp(5000)) return;

    // ---- 3. We are already on the server's AP? -----------------------------
    if (WiFi.SSID() == String(SERVER_AP_SSID)) {
        serverIP    = IPAddress(192, 168, 4, 1);
        serverFound = true;
        log_message("[Discovery] On server AP, using 192.168.4.1");
        return;
    }

    // ---- 4. Full diagnostic dump ------------------------------------------
    // If we got here, none of the above worked. Print everything a user
    // needs to figure out whether the server is on another network, the
    // AP is isolating clients, or the server process simply isn't up.
    log_message("[Discovery] No server found on this network.");
    log_message("[Discovery] --- diagnostics ---");
    log_message(String("[Discovery]   our SSID     : ") + WiFi.SSID());
    log_message(String("[Discovery]   our IP       : ") +
                WiFi.localIP().toString());
    log_message(String("[Discovery]   our subnet   : ") +
                WiFi.subnetMask().toString());
    log_message(String("[Discovery]   our gateway  : ") +
                WiFi.gatewayIP().toString());
    log_message(String("[Discovery]   broadcast    : ") +
                WiFi.broadcastIP().toString());
    log_message(String("[Discovery]   our RSSI     : ") +
                String(WiFi.RSSI()) + " dBm");
    log_message("[Discovery] Check that:");
    log_message("[Discovery]   a) the server is powered on and past its own boot,");
    log_message("[Discovery]   b) the server is on this same Wi-Fi, not its own AP,");
    log_message("[Discovery]   c) the AP is not doing client isolation.");
    log_message("[Discovery] --- end diagnostics ---");
    stat_discovery_failures++;
}

// ---- Try joining the server's own SoftAP -----------------------------------
inline bool tryJoinServerAccessPoint() {
    log_message("[WiFi] Trying to join server AP '" SERVER_AP_SSID "'...");
    WiFi.disconnect(false, false);
    // [tryjoin-wait] disconnect(false,false) is asynchronous: it returns
    // before status() flips to WL_DISCONNECTED. Calling begin() while the
    // driver is still STA_CONNECTING is silently rejected ("sta is
    // connecting, cannot set config"). serverWatcherTask() documents this
    // exact failure; tryJoinServerAccessPoint() is called from networkTask
    // too and was missing the same wait, so 15 s stalls went silent.
    unsigned long t0 = millis();
    while (WiFi.status() != WL_DISCONNECTED && millis() - t0 < 2000) delay(50);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.begin(SERVER_AP_SSID, SERVER_AP_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(300); attempts++; }
    if (WiFi.status() != WL_CONNECTED) { log_message("[WiFi] Server AP not reachable."); return false; }

    log_message(String("[WiFi] Joined server AP. IP: ") + WiFi.localIP().toString());
    return true;
}

// ---- Multicast sockets -----------------------------------------------------
inline bool setupMulticastSockets() {
    // [rebind-stop-first] On a Wi-Fi drop lwIP tears down the underlying
    // fds, but WiFiUDP's internal state still thinks the sockets are bound.
    // A subsequent beginMulticast()/begin() on that state returns false
    // silently — the exact failure mode the [ota-abort-on-begin-fail]
    // comment in EVDPWeb.h warns about. stop() is a no-op if already
    // stopped, so it is safe to call unconditionally on the first bind.
    // [udp-ready-before-teardown] Clear the ready flag BEFORE the stop()
    // calls. udpAudioTask polls it every 1 ms; if it reads true between
    // stop() and the eventual rebuild it calls parsePacket() on a closed
    // fd, which on some arduino-esp32 builds silently breaks the subsequent
    // begin() (the exact failure mode [rebind-stop-first] below warns about).
    g_udpSocketsReady = false;

    audioUdp.stop();
    ctrlUdp.stop();
    clockSyncUdp.stop();

    IPAddress iface = WiFi.localIP();
    if (iface == IPAddress(0, 0, 0, 0)) iface = WiFi.softAPIP();
    if (iface == IPAddress(0, 0, 0, 0)) {
        log_message("[UDP] No interface IP yet, cannot join multicast");
        g_udpSocketsReady = false;
        return false;
    }

    if (!audioUdp.beginMulticast(multicastGroup, AUDIO_UDP_PORT)) {
        log_message("[UDP] Failed to join audio multicast group");
        stat_multicast_join_fails++;
        g_udpSocketsReady = false;
        return false;
    }
    // ctrlUdp is unicast-only: the server's sendCtrlToSubscriber() calls
    // ctrlUdp.beginPacket(dst, CTRL_UDP_PORT) with dst being this node's own
    // IP. Bind a plain socket on the port; do NOT join the multicast group.
    if (!ctrlUdp.begin(CTRL_UDP_PORT)) {
        log_message("[UDP] Failed to open ctrl socket on port " + String(CTRL_UDP_PORT));
        stat_multicast_join_fails++;
        audioUdp.stop();
        g_udpSocketsReady = false;
        return false;
    }

    // Ephemeral local port: we are the client side of the request/response
    // clock-sync protocol (see CLOCK_SYNC_UDP_PORT docs on the server). The
    // OS picks a free port; the server replies to whatever source port our
    // sync_req arrived from, so we don't need a fixed one here.
    if (!clockSyncUdp.begin(0)) {
        log_warn("[UDP] Failed to open clock-sync socket; RTT sync "
                 "disabled, passive time_sync fallback only.");
        stat_multicast_join_fails++;
    }

     log_message(String("[UDP] Audio multicast ") + multicastGroup.toString() +
                ":" + String(AUDIO_UDP_PORT) +
                ", ctrl unicast port " + String(CTRL_UDP_PORT));
    g_udpSocketsReady = true;
    return true;
}

// Hardened control / sync path: ignore tiny one-way drift, only adopt real
// clock jumps, and do not nudge the offset for every small authoritative
// packet. This removes the periodic micro-stutter caused by offset drift
// chasing network jitter.
inline void applyCtrlJson(const uint8_t *data, size_t len) {
    // [ctrl-doc-size] A host_audio_sync packet carries the same
    // protocol/compat/epoch/clockSyncPort + volume/muted/bufferMs +
    // six encoder fields that the registration response carries.
    // 1024 B is a coin-flip for that shape on ArduinoJson 6; a
    // NoMemory failure here silently drops the packet (only
    // stat_ctrl_json_errors is bumped), so a mid-stream bufferMs or
    // masterVolume push just never lands.
    DynamicJsonDocument doc(1536);
    if (deserializeJson(doc, data, len)) {
        stat_ctrl_json_errors++;
        return;
    }

    const char* typeStr = doc["type"] | "";

    // ---------- master recording: start/stop the PSRAM capture ----------
    // Unicast like server_standby and handled before the host_audio_sync
    // target gate. Old firmware ignores the unknown type.
    if (strcmp(typeStr, "rec_start") == 0 || strcmp(typeStr, "rec_stop") == 0) {
        String recId = doc["recId"] | "";
        if (recId.length() == 0 || recId.length() > 64) return;
        if (strcmp(typeStr, "rec_start") == 0) recStart(recId);
        else                                    recStop();
        return;
    }

    // ---------- server_standby: the leader stepped down ----------
    // [servers-decide] A higher-priority server took over; the one we are
    // talking to is now a standby that stops answering discovery and audio.
    // Drop the contact and let networkTask re-discover immediately instead
    // of waiting for the 60 s stale-contact path.
    if (strcmp(typeStr, "server_standby") == 0) {
        if (serverFound) {
            serverFound          = false;
            g_force_rediscovery  = true;
            log_message("[Discovery] server went standby; re-discovering...");
        }
        return;
    }

    // ---------- time_sync: only react to real step changes ----------
    // [offset-route-through-setter] Every write to server_clock_offset_ms
    // MUST go through setServerClockOffset() so the (g_sync_local_ms,
    // g_sync_server_ms) reference pair stays current. audioPlaybackTask
    // uses those anchors — not the absolute offset — for its deadline
    // math. A direct write here leaves the anchors stale and every
    // deadline lands off by -server_clock_offset_ms until the first RTT
    // sync rebuilds the pair.
    if (strcmp(typeStr, "time_sync") == 0) {
        if (doc.containsKey("epoch")) handleServerEpoch((uint32_t)(doc["epoch"] | 0UL));

        uint32_t serverMs = doc["serverMs"] | 0;
        if (serverMs == 0) return;

        bool authoritative = doc["authoritative"] | false;

        // Even an "authoritative" broadcast is a one-way measurement:
        // delivery delay biases it. Never overwrite an acquired RTT estimate.
        if (g_have_rtt_sync) return;

        int32_t rawDelta = (int32_t)(serverMs - (uint32_t)millis());
        int64_t inst     = (int64_t)rawDelta;

        if (authoritative) {
            // [authoritative-first-lock] First authoritative sample always
            // seeds the timeline, even at |delta| < 5 ms; otherwise a fresh
            // node whose millis() happens to track server millis() closely
            // never sets server_clock_synced and stays off the deadline path.
            if (!server_clock_synced) {
                setServerClockOffset((int32_t)inst);
                server_clock_synced    = true;
                server_clock_jitter_ms = 0;
                stat_clock_resyncs++;
                return;
            }
            int64_t delta    = inst - server_clock_offset_ms;
            int64_t absDelta = delta < 0 ? -delta : delta;

            if (absDelta > CLOCK_RESYNC_JUMP_MS) {
                setServerClockOffset((int32_t)inst);
                server_clock_synced    = true;
                server_clock_jitter_ms = 0;
                stat_clock_resyncs++;
                g_force_resync = true;
                log_warn(String("[Sync] Authoritative time_sync jump: ") +
                         String((long)delta) + " ms, forcing resync");
            } else if (absDelta >= 5) {
                // [v1.1.2-authoritative-nudge] Pull the offset toward the
                // sample 10 % per packet; anchors refresh every time so the
                // deadline math stays continuous.
                int64_t nudge = delta / 10;
                if (nudge == 0) nudge = (delta > 0) ? 1 : -1;
                setServerClockOffset((int32_t)((int64_t)server_clock_offset_ms + nudge));
                server_clock_jitter_ms = (int32_t)absDelta;
                if (gPlayerLogLevel >= 3) {
                    log_message(String("[Sync] Authoritative nudge: ") +
                                String((long)delta) + " ms, applied " +
                                String((long)nudge) + " ms");
                }
            }
            return;
        }

        // Pre-RTT seed path: allow only a real jump to reset the offset.
        if (!server_clock_synced) {
            setServerClockOffset((int32_t)inst);
            server_clock_synced    = true;
            server_clock_jitter_ms = 0;
            stat_clock_resyncs++;
            return;
        }

        int64_t delta    = inst - server_clock_offset_ms;
        int64_t absDelta = delta < 0 ? -delta : delta;

        if (absDelta > CLOCK_RESYNC_JUMP_MS * 2) {
            setServerClockOffset((int32_t)inst);
            server_clock_jitter_ms = 0;
            stat_clock_resyncs++;
            g_force_resync = true;
        } else {
            // Ignore tiny offsets from network jitter.
        }
        return;
    }

    // ---------- host_audio_sync: process config only if targeted ----------
    if (strcmp(typeStr, "host_audio_sync") != 0) return;

    // [ctrl-target-strict] Require an explicit target. An empty/missing
    // field used to pass and apply to EVERY node on the LAN: a server-side
    // serialization bug, or a spoofed datagram, becomes a broadcast of
    // volume/mute/bufferMs. Explicit "all" or our exact IP only.
    // [ctrl-target-after-dhcp]
    // The server's subscriber slot holds the old IP until the next
    // heartbeat (~30 s). During that window the packet's target field
    // cannot match WiFi.localIP(), so every ctrl push (bufferMs, mode,
    // encoder fields, epoch) is silently dropped. Cache the last accepted
    // target and accept it too; the next heartbeat re-points the server
    // at the new IP and the cache becomes redundant.
    static String lastAcceptedTarget;
    String tgt = doc["target"] | "";
    if (tgt != "all" &&
        tgt != WiFi.localIP().toString() &&
        tgt != lastAcceptedTarget) return;
    if (tgt != "all") lastAcceptedTarget = tgt;

    if (doc.containsKey("audioProtocolVersion") || doc.containsKey("audioCompatible")) {
        bool fieldsPresent = doc.containsKey("audioProtocolVersion") &&
                             doc.containsKey("audioCompatible");
        int  negotiated  = doc["negotiatedAudioProtocolVersion"] | 0;
        bool compatible  = doc["audioCompatible"] | false;

        AudioCompatInfo info = evaluatePlayerAudioCompat(fieldsPresent, compatible, negotiated);
        bool stateChanged = ((int)info.state != g_audio_compat_state);

        g_audio_compat_state                = (int)info.state;
        g_negotiated_audio_protocol_version = info.negotiatedProtocolVersion;

        if (stateChanged && info.state == AUDIO_COMPAT_INCOMPATIBLE) {
            g_force_flush_incompatible = true;
        }
    }

    if (doc.containsKey("epoch")) handleServerEpoch((uint32_t)(doc["epoch"] | 0UL));
    if (doc.containsKey("clockSyncPort")) {
        long port = doc["clockSyncPort"] | 0;
        if (port > 0 && port <= 65535) g_clock_sync_port = (uint16_t)port;
    }

    // Keep raw ctrl logging at debug-level only. This prevents the hot path
    // from doing String concatenation on each control packet.
    if (!doc["volume"].isNull()) {
        float v = doc["volume"].as<float>();
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        master_volume_multiplier = v;
    }
    if (!doc["muted"].isNull())  master_muted = doc["muted"].as<bool>();
    if (!doc["zoneVolume"].isNull() || !doc["zoneMuted"].isNull()) {
        float newZoneVol = doc["zoneVolume"].isNull()
                            ? zone_volume_multiplier
                            : doc["zoneVolume"].as<float>();
        bool  newZoneMute = doc["zoneMuted"].isNull()
                            ? zone_muted
                            : doc["zoneMuted"].as<bool>();

        if (newZoneVol < 0.0f) newZoneVol = 0.0f;
        if (newZoneVol > 1.0f) newZoneVol = 1.0f;

        zone_volume_multiplier = newZoneVol;
        zone_muted             = newZoneMute;
    }
    if (!doc["bufferMs"].isNull() && doc["bufferMs"].is<int>()) {
        int b = doc["bufferMs"].as<int>();
        if (b < 20)  b = 20;
        if (b > 400) b = 400;

        // [bufferMs-cross-channel-ctrl]
        // Mirror of applyRegistrationResponse. Increases are adopted
        // immediately (the ctrl channel exists to react faster than the
        // 30 s register heartbeat). Decreases require agreement from the
        // register side OR a register that has gone stale (see
        // [bufferMs-staleness] in EVDPState.h).
        //
        // [bufferMs-mode-change] A server-side mode change originates on
        // THIS channel (broadcastNodeConfig fires on the same handleApiConfig
        // call that flipped the mode), so a ctrl-push bufferMs that arrives
        // together with a new mode field is authoritative even when it is
        // lower than the register's last value. Without this, Music(150) ->
        // Movie(80) was rejected for up to 30 s (until the register heartbeat
        // caught up) and the operator heard the first heartbeat interval of
        // the new movie mode at the previous mode's latency.
        static String lastCtrlMode = "";
        String ctrlMode = doc["mode"] | "";
        bool   ctrlModeChanged = (ctrlMode.length() > 0 && ctrlMode != lastCtrlMode);
        if (ctrlMode.length() > 0) lastCtrlMode = ctrlMode;

        last_ctrl_buffer_ms    = b;
        ts_last_ctrl_buffer_ms = millis();
        uint32_t regAgeMs = (ts_last_reg_buffer_ms == 0)
                            ? 0xFFFFFFFFu
                            : (millis() - ts_last_reg_buffer_ms);
        bool accept = ctrlModeChanged ||                // <-- NEW
                      (b >= target_playout_delay_ms) ||
                      (last_reg_buffer_ms == -1)     ||
                      (b == last_reg_buffer_ms)      ||
                      (regAgeMs > 10000UL);

        // [bufferMs-re-adopt-when-target-dropped]
        // Also re-adopt when the live target has drifted BELOW the server's
        // value (self-tuner shrank past it). Without this, a node whose
        // target self-tuned down to 80 ms rejected the server's next 150 ms
        // ctrl (b == last_server_buffer_ms) and stayed starved forever.
        // A target ABOVE b is preserved (self-tuned up for a bad network),
        // which is the exact sawtooth the original "b != last" check was
        // added to fix.
        if (accept && (b != last_server_buffer_ms ||
                       b > target_playout_delay_ms)) {
    // [auto-expand-reset-on-server-change]
    // Reset g_auto_expand_ms to 0 ONLY when the server's bufferMs is
    // genuinely different from the last value we saw. The server re-sends
    // bufferMs=50 on every 30 s register heartbeat (see
    // [zone-state-on-every-heartbeat] on the server side); treating that
    // as a "change" and wiping the auto-expand every 30 s would undo the
    // safety margin 2 s after every expansion. If the operator moves the
    // slider (or a mode preset change ships a new value), that IS a
    // change and the accumulator should start from scratch.
    if (b != last_server_buffer_ms) {
        g_auto_expand_ms = 0;
    }
    last_server_buffer_ms = b;

    int effective = b + g_auto_expand_ms;
    if (effective > 400) effective = 400;
    if (effective < 20)  effective = 20;
    target_playout_delay_ms = effective;

    // Recompute queue target against the EFFECTIVE delay, not the raw
    // server value, so the prebuffer matches the actual deadline.
    uint32_t netLat = last_node_latency_ms;
    if (netLat == 0) netLat = 20;
    if (netLat > (uint32_t)effective / 2) netLat = effective / 2;

    // [queue-target-smoothed-latency] Same helper as the register path and
    // the audio task's 1 s policy. See computeQueueTargetSmoothed() in
    // EVDPState.h for why the raw last_node_latency_ms is not used.
    int tgt = computeQueueTargetSmoothed(effective);

    int pre = tgt;
    if (pre < 2) pre = 2;
    if (pre > AUDIO_QUEUE_LEN - 2) pre = AUDIO_QUEUE_LEN - 2;

    audio_prebuffer    = pre;
    audio_queue_target = tgt;

    // [buffer-log-scope-fix]
    // Moved INSIDE the accept-if: `tgt` and `pre` are locals of this
    // block, so referencing them from outside produced
    // "'pre' was not declared in this scope". Removing the extra `}`
    // that followed also stops applyCtrlJson() from being closed early,
    // which is what pushed applyEncoderInfoFromJson(doc) up to global
    // scope ("expected constructor, destructor, or type conversion").
    if (gPlayerLogLevel >= 3) {
        log_message(String("[Buffer] bufferMs=") + String(b) +
                    " target=" + String(tgt) + " prebuffer=" + String(pre));
            }
        }
    }

    // [encoder-info-passthrough] Same six fields the registration response
    // carries, repeated here so a mid-stream encoder change reaches a
    // running node without waiting for its own 30 s heartbeat.
    applyEncoderInfoFromJson(doc);
}

// ---- Min-RTT offset filter -------------------------------------------------
// Rationale:
//   * Each RTT exchange gives an independent estimate of the offset, but
//     network jitter always biases the measured RTT upward. The offset
//     derived from a jittered sample is therefore also biased, by an
//     amount proportional to that sample's path asymmetry.
//   * The sample with the SMALLEST RTT in the recent window is the one
//     that saw the least jitter, so its offset estimate is closest to
//     the truth.
//   * The previous median-of-5 filter is robust against outliers but
//     still averages in the mean bias itself, so a node on a congested
//     link converged to an offset that was permanently wrong by the
//     average upstream/downstream delay difference (5-20 ms on 802.11).
//     That is exactly the "node sits behind and never catches up"
//     symptom: the node has no way to know its own estimate is biased,
//     because every sample it takes agrees with the bias.
//   * Min-RTT-of-8 converges to the true offset even under sustained
//     jitter, at the cost of being slower to reflect a genuine step
//     change in the server clock - which is what the authoritative
//     time_sync broadcast in EVDUDP.h is for.
#define SYNC_RING_SIZE 8
struct SyncSample {
    uint32_t rtt;
    int64_t  offset;
    bool     valid;
};
static SyncSample g_syncRing[SYNC_RING_SIZE] = {};
static int        g_syncRingHead  = 0;
static int        g_syncRingCount = 0;

// ---- Round-trip clock sync (request/response, UDP) --------------------------
// See the wire-format doc block in the server's EVDConfig.h for the JSON
// shape and the RTT/offset math below. This is the authoritative clock
// reference once a sample has been collected; the passive "time_sync" ctrl
// push above is retained only as a fallback (see g_have_rtt_sync gating
// there). Both functions only touch small stack buffers and volatiles/
// globals -- no heap allocation beyond ArduinoJson's StaticJsonDocument
// (stack-resident) -- and are called from networkTask, never from the
// audio/UDP hot-path tasks.
inline void sendClockSyncRequest() {
    if (!serverFound || WiFi.status() != WL_CONNECTED) return;
    if (g_sync_req_in_flight) return;

    static uint32_t entryCount = 0;
    entryCount++;
    // Diagnostics only; go through the level-3 gate so steady state stays quiet.
    if (gPlayerLogLevel >= 3 && (entryCount <= 5 || entryCount % 30 == 0)) {
        log_message(String("[SyncTX] entry #") + String(entryCount) +
                    " dst=" + serverIP.toString() + ":" + String(g_clock_sync_port));
    }

    uint32_t txMs = millis();
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"sync_req\",\"clientTxMs\":%lu}",
                      (unsigned long)txMs);
    if (n <= 0 || n >= (int)sizeof(buf)) return;

    bool bp = clockSyncUdp.beginPacket(serverIP, g_clock_sync_port);
    int  ep = 0;
    if (bp) {
        clockSyncUdp.write((const uint8_t *)buf, (size_t)n);
        ep = clockSyncUdp.endPacket();
    }
     if (gPlayerLogLevel >= 3 && (entryCount <= 5 || entryCount % 30 == 0)) {
        log_message(String("[SyncTX] beginPacket=") + String(bp ? 1 : 0) +
                    " endPacket=" + String(ep));
    }
    if (bp && ep == 1) {
        g_pending_sync_client_tx_ms = txMs;
        g_sync_req_in_flight        = true;
        ts_last_sync_req_sent_ms    = millis();
    }
}

inline void pollClockSyncResponse() {
    int sz = clockSyncUdp.parsePacket();
    uint32_t localRxMs = millis();
    if (sz > 0 && gPlayerLogLevel >= 3)
        log_message(String("[SyncRX] got ") + String(sz) + " bytes");
    if (sz <= 0) {
        if (g_sync_req_in_flight && (uint32_t)(millis() - ts_last_sync_req_sent_ms) > 2000) {
            g_sync_req_in_flight = false;
        }
        return;
    }
    if (gPlayerLogLevel >= 3)
        log_message(String("[Sync] got packet, sz=") + String(sz));
    IPAddress srcIp = clockSyncUdp.remoteIP();
    char buf[192];
    int n = clockSyncUdp.read((uint8_t *)buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    if (srcIp != serverIP) {
        static uint32_t lastBadSyncLogMs = 0;
        if (millis() - lastBadSyncLogMs > 5000) {
            lastBadSyncLogMs = millis();
            log_warn(String("[Sync] dropped from untrusted sender ") +
                     srcIp.toString() + " (server=" + serverIP.toString() + ")");
        }
        return;
    }

    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, buf, (size_t)n);
    if (err) { log_message(String("[Sync] parse fail: ") + err.c_str()); return; }

    const char *type = doc["type"] | "";
    if (strcmp(type, "sync_resp") != 0) return;

    uint32_t clientTxMs = (uint32_t)(doc["clientTxMs"] | 0UL);
    if (!g_sync_req_in_flight || clientTxMs != g_pending_sync_client_tx_ms) return;
    g_sync_req_in_flight = false;

    int protoVersion = doc["protoVersion"] | 0;
    if (protoVersion != AUDIO_PROTO_VERSION) return;

    uint32_t epoch      = (uint32_t)(doc["epoch"]      | 0UL);
    uint32_t serverRxMs = (uint32_t)(doc["serverRxMs"] | 0UL);
    uint32_t serverTxMs = (uint32_t)(doc["serverTxMs"] | 0UL);
    handleServerEpoch(epoch);

    int32_t rtt    = (int32_t)(localRxMs - clientTxMs) - (int32_t)(serverTxMs - serverRxMs);
    int64_t offset = ((int64_t)(int32_t)(serverRxMs - clientTxMs) +
                       (int64_t)(int32_t)(serverTxMs - localRxMs)) / 2;

    if (rtt < 0 || rtt > 500) return;

    if (!server_clock_synced) {
        for (int i = 0; i < SYNC_RING_SIZE; i++) g_syncRing[i].valid = false;
        g_syncRing[0].rtt    = (uint32_t)rtt;
        g_syncRing[0].offset = offset;
        g_syncRing[0].valid  = true;
        g_syncRingHead  = 1;
        g_syncRingCount = 1;
    } else {
        g_syncRing[g_syncRingHead].rtt    = (uint32_t)rtt;
        g_syncRing[g_syncRingHead].offset = offset;
        g_syncRing[g_syncRingHead].valid  = true;
        g_syncRingHead = (g_syncRingHead + 1) % SYNC_RING_SIZE;
        if (g_syncRingCount < SYNC_RING_SIZE) g_syncRingCount++;
    }

    uint32_t bestRtt = UINT32_MAX;
    int64_t  bestOff = offset;
    // Visit newest first so equal-RTT samples don't pin the offset to an
    // older clock reading for the full lifetime of the ring.
    for (int age = 0; age < g_syncRingCount; age++) {
        int i = (g_syncRingHead + SYNC_RING_SIZE - 1 - age) % SYNC_RING_SIZE;
        if (!g_syncRing[i].valid) continue;
        if (g_syncRing[i].rtt < bestRtt) {
            bestRtt = g_syncRing[i].rtt;
            bestOff = g_syncRing[i].offset;
        }
    }
    stat_clock_sync_rtt_ms = bestRtt;

    if (!server_clock_synced) {
        setServerClockOffset(bestOff);
        server_clock_synced    = true;
        server_clock_jitter_ms = 0;
        g_current_offset_rtt   = bestRtt;
        stat_clock_resyncs++;
    } else {
        int64_t delta    = bestOff - server_clock_offset_ms;
        int64_t absDelta = delta < 0 ? -delta : delta;

        if (absDelta > CLOCK_RESYNC_JUMP_MS) {
            for (int i = 0; i < SYNC_RING_SIZE; i++) g_syncRing[i].valid = false;
            g_syncRing[0] = { bestRtt, bestOff, true };
            g_syncRingHead  = 1;
            g_syncRingCount = 1;
            setServerClockOffset(bestOff);
            server_clock_jitter_ms = 0;
            g_current_offset_rtt   = bestRtt;
            stat_clock_resyncs++;
            g_force_resync = true;
        } else if (g_current_offset_rtt == UINT32_MAX ||
                   bestRtt + 4 < g_current_offset_rtt ||
                   absDelta > (int64_t)CLOCK_DRIFT_REFRESH_MS) {   // <-- NEW drift term
            // [drift-refresh] This branch now fires for THREE cases:
            //   1. No sample has ever been adopted (UINT32_MAX sentinel).
            //   2. The ring's best RTT is materially better than the
            //      currently-tracked sample's RTT (existing behaviour).
            //   3. The ring's best offset has drifted > CLOCK_DRIFT_REFRESH_MS
            //      from the tracked offset. This is the fix: previously a
            //      stable-RTT link NEVER refreshed the anchor, so
            //      20–100 ppm crystal drift accumulated unbounded
            //      (72–360 ms after one hour) and every frame slipped
            //      past its deadline -> flushAudioQueueAndRebuffer storm.
            setServerClockOffset(bestOff);
            server_clock_jitter_ms += (int32_t)((absDelta - server_clock_jitter_ms) / 4);
            g_current_offset_rtt   = bestRtt;
        }
    }
    if (!g_have_rtt_sync) {
        g_force_resync = true;
    }
    g_have_rtt_sync = true;
}

// ---- Concealment frame enqueue ----------------------------------------------
// Push a zero-length "please run Opus PLC" message onto the queue for a
// frame we know we never received. captureMs anchors its playout deadline so
// the gap doesn't collapse the schedule for frames that follow it.
inline bool enqueueConcealedFrame(uint32_t captureMs, uint32_t seq) {
    if (audio_queue == nullptr) return false;
    // [conceal-guard-full-not-3q]
    // Was `>= (AUDIO_QUEUE_LEN * 3) / 4` = 24/32. Nodes 210/211 showed
    // seqMissing=4482 against framesConcealed=4, meaning enqueueConcealed-
    // Frame was rejected on almost every gap despite queue depth reading
    // 1-3 in the /api/health snapshots. Full-queue rejection is still
    // correct (a concealment must never evict a real frame), but 24/32
    // was leaving 8 slots of headroom unused. No-op if the actual cause
    // is elsewhere; safe in all cases.
    if (uxQueueMessagesWaiting(audio_queue) >= AUDIO_QUEUE_LEN) return false;
    
    AudioMessage msg;
    msg.length      = 0;
    msg.arrival_ms  = millis();
    msg.captured_ms = captureMs;
    msg.seq         = seq;
    msg.conceal     = true;

    // [conceal-no-evict] If the queue filled between the guard above and
    // this send, DROP the concealment - never evict a real frame to make
    // room for a synthetic one. The old drop-oldest path was unreachable
    // in practice (single writer, monotonic drain) but would have traded
    // a real Opus frame for a PLC guess if it ever fired.
    if (xQueueSend(audio_queue, &msg, 0) != pdPASS) return false;
    stat_frames_concealed++;
    return true;
}

// ---- Audio packet enqueue ----------------------------------------------------
// Enqueues one received audio datagram after validating the 12-byte v2
// header (see EVDPProtocolCompat.h for the layout — it must match the
// authoritative server-side doc it mirrors). There is no v1 fallback: a
// datagram that fails the v2 magic+version check is dropped, not
// reinterpreted as a bare 4-byte-timestamp packet.
inline void enqueueAudioPacketLocked(const uint8_t *data, size_t len) {
    if (audio_queue == nullptr) return;

    if (g_audio_compat_state != AUDIO_COMPAT_COMPATIBLE) {
        if (g_audio_compat_state == AUDIO_COMPAT_INCOMPATIBLE)
            stat_audio_withheld_incompatible++;
        else
            stat_audio_withheld_not_registered++;
        // [compat-gate-resets-seq]
        // While withheld, packets are not seq-tracked, so the first
        // COMPATIBLE packet after a >200 ms window (fresh boot, server
        // reboot, transient register failure) looks like a gap of 200+
        // frames. AUDIO_MAX_CONCEAL_FRAMES caps synthesis at 10 frames,
        // but their captured_ms is anchored to a last_conceal_captured_ms
        // from BEFORE the window — seconds in the past. audioPlaybackTask
        // then flushes on stale-reanchor, producing the "silent for ~2 s
        // after every boot" profile. Reset the seq baseline so the first
        // real frame starts a fresh timeline with no fabricated gap.
        seq_initialized = false;
        return;
    }

    ParsedAudioHeaderV2 v2hdr;
    if (!parseAudioHeaderV2(data, len, v2hdr)) return;

    // [wire-flag-unicast]
    // Audio is delivered by UNICAST from the server (each node gets its own
    // datagram, which is what isolates one node's loss from the others). Every
    // node on the LAN therefore also receives the frames addressed to every
    // OTHER node. The server sets AUDIO_FLAG_UNICAST in the header flag byte
    // for every frame it sends; frames without it are either from a build
    // that predates the flag (all-zero flags, treated as "no claim" and
    // accepted for compatibility) or addressed elsewhere.
    //
    // The address itself is the real discriminator and the socket layer
    // already filters on it (the unicast socket only receives datagrams sent
    // to this node's IP). This flag is the belt-and-braces check for the
    // multicast fallback path, where there is no address to filter on: a
    // node that decodes another node's frames does not just waste CPU, it
    // advances ITS seq/expected_seq baseline against a foreign stream and
    // desynchronises its own timeline. Drop those.
    if (v2hdr.flags != 0 && (v2hdr.flags & AUDIO_FLAG_UNICAST) == 0) {
        stat_packets_not_for_me++;
        return;
    }

    const uint32_t cap     = v2hdr.captured_ms;
    const uint32_t seq     = v2hdr.seq;
    const size_t   hdrLen  = (size_t)AUDIO_HEADER_BYTES;
    audio_proto_v2_seen = true;

    uint16_t audioLen = (uint16_t)(len - hdrLen);
    if (audioLen == 0) return;
    if (audioLen > MAX_OPUS_PACKET_LEN) { stat_packets_oversize++; return; }

    // [stutter-diagnostics] Measure the inter-arrival gap from the previous
    // VALID datagram before we touch ts_last_audio_packet_ms. Malformed or
    // stray packets never get here, so the histogram cannot be corrupted
    // by noise on the multicast group.
    uint32_t now_arrival = millis();
    if (ts_prev_audio_arrival_ms != 0) {
        uint32_t gap = now_arrival - ts_prev_audio_arrival_ms;
        portENTER_CRITICAL(&g_gap_stat_mux);
        if (gap > stat_max_gap_ms_this_sec) stat_max_gap_ms_this_sec = gap;
        portEXIT_CRITICAL(&g_gap_stat_mux);
        if (gap >  35) stat_gap_gt_35ms_total++;
        if (gap >  50) stat_gap_gt_50ms_total++;
        if (gap > 100) stat_gap_gt_100ms_total++;
    }
    ts_prev_audio_arrival_ms = now_arrival;
    portENTER_CRITICAL(&g_gap_stat_mux);
    stat_packets_this_sec++;
    portEXIT_CRITICAL(&g_gap_stat_mux);

    ts_last_audio_packet_ms = now_arrival;

    if (uxQueueMessagesWaiting(audio_queue) >= (AUDIO_QUEUE_LEN * 3) / 4) {
        stat_packets_dropped++;
        // Do not advance the baseline: a later accepted packet must account
        // for the discarded packet just like any other loss.
        return;
    }

    // ---- Sequence gap / duplicate / reorder detection ----------------
    if (seq_initialized) {
        int32_t delta = (int32_t)(seq - expected_seq); // wrap-safe
        if (delta > 0) {
            stat_seq_gaps++;
            stat_seq_missing += (uint32_t)delta;
            uint32_t toConceal = (uint32_t)delta;
            if (toConceal > AUDIO_MAX_CONCEAL_FRAMES) {
                g_force_resync = true;
                return;
            }
            uint32_t concealCap = last_conceal_captured_ms;
            const uint32_t frameMs = (OPUS_FRAME_SIZE * 1000UL) / SAMPLE_RATE;
            for (uint32_t i = 0; i < toConceal; i++) {
                concealCap += frameMs;
                if (!enqueueConcealedFrame(concealCap, expected_seq)) {
                    stat_packets_dropped++;
                    return;
                }
                last_seq_seen = expected_seq;
                expected_seq++;
                last_conceal_captured_ms = concealCap;
            }
        } else if (delta < 0) {
            if (seq == last_seq_seen) stat_seq_duplicate++;
            else                      stat_seq_reordered++;
            stat_frames_discarded++;
            return;
        }
    }

    AudioMessage msg;
    msg.length      = audioLen;
    msg.arrival_ms  = millis();
    msg.captured_ms = cap;
    msg.seq         = seq;
    msg.conceal     = false;
    memcpy(msg.payload, data + hdrLen, audioLen);

    // PLC may have filled the queue after the 75%-full guard. If the real
    // frame cannot be queued, leave the baseline at the last queued PLC.
    if (xQueueSend(audio_queue, &msg, 0) != pdPASS) {
        stat_packets_dropped++;
        return;
    }
    seq_initialized = true;
    expected_seq = seq + 1;
    last_seq_seen = seq;
    last_conceal_captured_ms = cap;
    stat_seq_received++;
    stat_packets_received++;
}

inline void enqueueAudioPacket(const uint8_t *data, size_t len) {
    // [p4-queue-mutex-wait]
    // This is a 0-tick take on the UDP drain task. A 0-tick take fails the
    // moment audioPlaybackTask holds the mutex — and that task holds it across
    // a whole Opus decode + I2S write, so every frame that arrived during a
    // frame of playback was thrown away and counted as a drop. On a healthy
    // link that is a steady, self-inflicted packet loss rate.
    //
    // 2 ticks = one frame period at 50 Hz scheduler ticks, so the drain task
    // waits at most one scheduling tick and never blocks the socket. If the
    // mutex is still held after that, the playback task is genuinely stuck and
    // dropping is the right answer.
    if (!g_audio_queue_mutex ||
        xSemaphoreTake(g_audio_queue_mutex, pdMS_TO_TICKS(2)) != pdTRUE) {
        stat_packets_dropped++;
        return;
    }
    // A packet may already have been read when OTA stopped the sockets.
    if (!otaNodeUpdating && g_udpSocketsReady)
        enqueueAudioPacketLocked(data, len);
    xSemaphoreGive(g_audio_queue_mutex);
}

// ---- UDP audio drain task (Core 0, high priority) --------------------------
// This task does nothing but pull Opus packets off the two multicast sockets
// and shove them into the audio queue. It must never do NVS, HTTP, UART, or
// String work — any of that blocks the drain and costs us packets.
inline void udpAudioTask(void *pvParameters) {
  static uint8_t rxAudioBuf[MAX_OPUS_PACKET_LEN + 16];
  // [ctrl-buf] 1024 was too small: the ctrl push (protocol + epoch +
  // clockSyncPort + volume/muted/bufferMs + 6 encoder fields) already
  // approaches 1 KB, and a future field tips it over. On the old guard
  // `sz < 1024` an oversize datagram was NEVER read -> parsePacket
  // returned the same size every loop -> all later ctrl traffic starved.
  // 2048 gives headroom; the oversize branch below DRAINS so a future
  // >2048 packet is dropped cleanly instead of livelocking.
  static uint8_t rxCmdbuf[2048];

  for (;;) {
    // [ota-pause-udp-drain-v2]
    // Three independent OTA signals, any of which pauses the drain:
    //   otaNodeUpdating      — application flag, set by the OTA handler
    //                          on START or first WRITE. Correct once set,
    //                          but useless until the handler runs.
    //   otaNodeFirmwareReady — application flag, set after every chunk
    //                          has been written but before Update.end()
    //                          commits. Covers the "committing, do not
    //                          enqueue 200 ms of stale frames that will
    //                          never be played" window.
    //   Update.isRunning()   — LIBRARY flag. Set by Update.begin(),
    //                          cleared by Update.end()/Update.abort().
    //                          Cannot be forgotten or misordered. This
    //                          is what closes the gap between the first
    //                          upload chunk arriving and the application
    //                          handler setting otaNodeUpdating.
    //
    // Without Update.isRunning() here, the sequence
    //   UPLOAD_FILE_WRITE #1 -> Update.begin() succeeds -> [gap]
    //   -> otaNodeUpdating = true
    // leaves udpAudioTask draining audio into a consumer-less queue
    // for the length of [gap]. When playback resumes those frames are
    // hundreds of ms stale (captured_ms is from before Update.begin
    // ran) and every one trips AUDIO_STALE_DISCARD_MS / _REANCHOR_MS,
    // producing a flush-rebuffer burst as the first audible event of
    // the post-OTA boot.
    if (!g_udpSocketsReady || otaNodeUpdating ||
        otaNodeFirmwareReady || Update.isRunning()) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    bool didWork = false;

    int sz = audioUdp.parsePacket();
    if (sz > 0) {
      if (sz > (int)sizeof(rxAudioBuf)) {
        // Datagram larger than our buffer. read() would truncate it and
        // enqueueAudioPacket() would then parse a stale header + partial
        // Opus payload -> opus_decode returns garbage. Drain and drop.
        uint8_t sink[64];
        while (audioUdp.available() > 0) audioUdp.read(sink, sizeof(sink));
        stat_packets_oversize++;
      } else {
        // [audio-source-gate]
        // The server sends audio via unicast to our IP on
        // AUDIO_UDP_PORT, and discovery (see [discovery-src-ip] in
        // discoverServerViaUdp) sets serverIP from the actual datagram
        // source, so on an AP+STA server it names the interface that
        // also delivers audio here. Anything arriving on this socket
        // from a different IP is not the server we registered with.
        //
        // Without this gate any LAN peer can spoof valid Opus frames
        // with arbitrary seq / captured_ms and force seq-reorder
        // rejection, concealment injection, or a full flush-churn on
        // audioPlaybackTask. Severity is LAN-only exposure, but that
        // includes every IoT device in the home.
        //
        // Fail-closed by construction: when serverIP is 0.0.0.0
        // (pre-discovery), no real source IP can match. In normal
        // operation the server is the only sender, so this rejects
        // only spoofed traffic.
        //
        // remoteIP() is captured BEFORE read(), mirroring the
        // [ctrl-sender-validation] pattern below — read() does not
        // clear the tracking state on this arduino-esp32 core, but
        // capturing early makes the contract independent of that
        // detail.
        IPAddress srcIp = audioUdp.remoteIP();
        int n = audioUdp.read(rxAudioBuf, (size_t)sz);
        if (n > 0) {
          if (srcIp == serverIP && serverIP != IPAddress(0, 0, 0, 0)) {
            enqueueAudioPacket(rxAudioBuf, (size_t)n);
          } else {
            // Rate-limit the log so a flood cannot stall the task on
            // Serial writes. The counter is the diagnostic signal —
            // operators should see this stay at 0 in a clean LAN.
            stat_audio_source_rejects++;
            static uint32_t lastBadAudioLogMs = 0;
            if (millis() - lastBadAudioLogMs > 1000) {
              lastBadAudioLogMs = millis();
              log_warn(String("[Audio] dropped from untrusted sender ") +
                       srcIp.toString() + " (server=" + serverIP.toString() + ")");
            }
          }
        }
      }
      didWork = true;
    }

    sz = ctrlUdp.parsePacket();
    if (sz > 0) {
      if (sz >= (int)sizeof(rxCmdbuf)) {
        // [ctrl-oversize] MUST drain, or every iteration parses the same
        // oversize datagram and the control channel goes dead forever.
        uint8_t sink[64];
        while (ctrlUdp.available() > 0) ctrlUdp.read(sink, sizeof(sink));
        stat_ctrl_json_errors++;
      } else {
        stat_ctrl_packets_rx++;
        // [ctrl-sender-validation] Any LAN host can craft a host_audio_sync
        // or time_sync datagram aimed at our ctrl port. Drop anything that
        // does not come from serverIP. serverIP is set by discovery from
        // the actual datagram source (see discoverServerViaUdp's
        // [discovery-src-ip] comment), so on AP+STA servers it always names
        // the interface that will also deliver ctrl pushes here.
        //
        // Capture remoteIP() before read(); read() does not clear the
        // tracking state on this arduino-esp32 core, but capturing early
        // makes the contract independent of that detail.
        IPAddress srcIp = ctrlUdp.remoteIP();
        int n = ctrlUdp.read(rxCmdbuf, sizeof(rxCmdbuf) - 1);
        if (n > 0) {
          rxCmdbuf[n] = '\0';
          if (srcIp == serverIP) {
            applyCtrlJson(rxCmdbuf, (size_t)n);
          } else if (strstr((const char *)rxCmdbuf, "\"server_ready\"") &&
                     (!serverFound ||
                      (ts_last_server_contact_ms != 0 &&
                       (millis() - ts_last_server_contact_ms) > 2500UL))) {
            // [servers-decide] Our server went quiet and the next one is
            // announcing itself. Adopt it and re-register immediately.
            // Only accepted while the current server is stale, so a LAN
            // spoof cannot hijack a node that is streaming normally.
            log_message(String("[Discovery] ") + srcIp.toString() +
                        " is the next server; re-registering");
            serverIP                  = srcIp;
            serverFound               = true;
            ts_last_server_contact_ms = millis();
            g_force_reregister        = true;
          } else {
            static uint32_t lastBadCtrlLogMs = 0;
            if (millis() - lastBadCtrlLogMs > 1000) {
              lastBadCtrlLogMs = millis();
              log_warn(String("[Ctrl] dropped from untrusted sender ") +
                       srcIp.toString() + " (server=" + serverIP.toString() + ")");
            }
          }
        }
      }
      didWork = true;
    }

    vTaskDelay(didWork ? 0 : pdMS_TO_TICKS(1));
  }
}

/// ---- Network task (HTTP, discovery, stats, NVS) ----------------------------
// No UDP audio draining here anymore — that lives in udpAudioTask so HTTP
// stalls can never starve the audio queue.
inline void networkTask(void *pvParameters) {
  unsigned long last_reconnect_attempt = 0;
  unsigned long lastStatsPost = 0;
  unsigned long lastSyncReq   = 0;

  for (;;) {
    // ---- Stuck-OTA watchdog ------------------------------------------------
    // handleNodeOtaUpload() flips otaNodeUpdating = true on the first chunk
    // ...
    // [ota-final-handler-guard] Stage 1: upload stalled mid-stream.
    // Stage 2 (new): UPLOAD_FILE_END fired, bytes are in NVM, but the
    // final POST handler never ran (client dropped socket before the
    // terminating CRLF). Stage 1's condition excludes stage 2, so without
    // stage 2 the audio task stays paused forever until manual reboot.
    // [ota-commit-single-shot] Both stages below are skipped while a commit
    // owns the Update object: Update.abort() there would corrupt a partition
    // being verified, and Update.end(true) would be a second commit on an
    // Update that is already finishing. handleNodeOtaDone() clears the flag
    // on every failure path; success ends in ESP.restart().
    if (otaCommitInProgress) {
        // Let the commit finish. Nothing else in this task may run against
        // Update while it does.
    } else if (otaNodeUpdating && !otaNodeFirmwareReady &&
        (millis() - otaNodeLastActivityMs) > OTA_STUCK_TIMEOUT_MS) {
        log_warn(String("[OTA] No upload activity for ") +
                 String((millis() - otaNodeLastActivityMs) / 1000UL) +
                 " s; aborting update and resuming audio.");
        Update.abort();
        g_force_rebuffer      = true;
        otaNodeUpdating       = false;
        otaNodeFirmwareReady  = false;
        otaNodeLastActivityMs = 0;
        g_udpSocketsReady = (WiFi.status() == WL_CONNECTED);
    } else if (otaNodeUpdating && otaNodeFirmwareReady &&
               otaNodeLastActivityMs != 0 &&
               (millis() - otaNodeLastActivityMs) > OTA_STUCK_TIMEOUT_MS) {
        log_warn("[OTA] Bytes written but final handler never ran; "
                 "committing and rebooting.");
        otaCommitInProgress   = true;   // [ota-commit-single-shot]
        otaNodeLastActivityMs = millis();
        if (Update.end(true)) {
            delay(500);
            ESP.restart();
        } else {
            otaCommitInProgress = false;
            Update.abort();
            g_force_rebuffer      = true;
            otaNodeUpdating       = false;
            otaNodeFirmwareReady  = false;
            otaNodeLastActivityMs = 0;
            g_udpSocketsReady = (WiFi.status() == WL_CONNECTED);
        }
    }

    // [ap-mode-deferred] Consume the AP-mode request from loop() before
    // the main branch. startCaptivePortal() is now called from this task
    // only, so its WebServer / DNSServer / Preferences mutations cannot
    // race with this task's own handleClient()/processNextRequest()/
    // preferences calls. The transition blocks for < 2 s (WiFi.disconnect
    // wait); that is acceptable during an operator-initiated mode change.
    if (g_enter_ap_mode_requested) {
        g_enter_ap_mode_requested = false;
        log_warn("[System] Entering AP mode (deferred from loop).");
        startCaptivePortal();
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
    }

    if (is_in_ap_mode) {
      dnsServer.processNextRequest();
      server.handleClient();
    } else {
      server.handleClient();

      // [ota-pause-background-network]
      // server.handleClient() above MUST keep running during OTA — it
      // serves handleNodeOtaUpload()/handleNodeOtaDone(). Everything
      // BELOW this guard is background network housekeeping that would
      // race the upload:
      //   - discover_server(): broadcast UDP + WiFi scans stall chunks
      //   - postNodeStats():   HTTP POST shares the radio with the push
      //   - sendClockSyncRequest / pollClockSyncResponse: more UDP
      //   - WiFi.reconnect():  tears the socket the upload depends on
      //   - pending_volume_save / pending_speaker_save: NVS writes stall
      //     the flash cache for ~100 ms per key and can zero out an
      //     in-flight Update.write() acknowledgement window
      // The OTA watchdog ABOVE this guard still runs every iteration,
      // so a genuinely stuck upload is aborted on schedule.
      if (otaNodeUpdating) {
        // Match the loop's normal 2 ms cadence so the WebServer keeps
        // polling at full rate for upload chunks. `continue` skips the
        // bottom vTaskDelay(), so re-issue it here.
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }

      // [v1.1.3-stale-contact] Never clear serverFound purely on a stale
      // contact timestamp. postNodeStats() can silently miss a single 3 s
      // tick under ARP/WiFi retry, which previously tripped a 60 s timer,
      // cleared serverFound for the whole 5 s discovery window, and stalled
      // clock-sync polling for that duration. The offset error accumulated
      // during the stall then pushed every frame into the 20-100 ms late
      // bucket, which is the "perfect for 60 s then degrades" symptom.
      // Only re-discover if (a) the server has never been found, or (b)
      // stats POSTs are actually failing AND the contact timestamp has not
      // moved since the last check.
      static unsigned long lastDiscoverAttempt    = millis();
      static unsigned long lastServerApAttempt    = 0;
      static uint32_t      lastStatsFailsSeen     = 0;
      static uint32_t      lastContactSeenMs      = 0;
      // [discovery-stale-60s] 180 s -> 60 s. A node whose server IP changed
      // via DHCP used to sit silent for up to 3 minutes because
      // CONTACT_STALE_MS was longer than any healthy heartbeat gap and
      // serverFound never cleared. 60 s matches PLAYER_STALE_TIMEOUT_MS on
      // the server side.
      const  unsigned long CONTACT_STALE_MS       = 60000UL;

      bool wifiUp = (WiFi.status() == WL_CONNECTED);

      // [servers-decide] The standby notice asked for an immediate re-discovery.
      if (g_force_rediscovery) {
        g_force_rediscovery = false;
        lastDiscoverAttempt = 0;   // bypass the 10 s discovery rate limit
      }

      bool statsFailsGrew = (stat_post_stats_fails > lastStatsFailsSeen);
      bool contactMoved   = (ts_last_server_contact_ms != lastContactSeenMs);
      lastStatsFailsSeen  = stat_post_stats_fails;
      lastContactSeenMs   = ts_last_server_contact_ms;

      bool contactStale = serverFound &&
                          ts_last_server_contact_ms != 0 &&
                          (millis() - ts_last_server_contact_ms > CONTACT_STALE_MS) &&
                          statsFailsGrew && !contactMoved;

      // [discovery-audio-reject-trigger]
      // Independent trigger: if udpAudioTask's source-IP gate is rejecting
      // >= 50 packets/s, the server we POST to is not the server sending
      // audio (DHCP renewal, multi-homed server, AP/STA interface switch).
      // Without this, stats POSTs keep succeeding so contactStale never
      // fires and the node is silent until reboot. The 10 s floor above
      // prevents spoofed bursts from forcing repeat discoveries.
      // [audio-reject-signed-delta] BUGFIX: compute the delta in signed
      // 32-bit so a /api/stats/reset (which zeroes stat_audio_source_rejects
      // but cannot reach this function-local static) does not underflow
      // the unsigned subtraction to ~4.29e9 and trip a false re-discovery.
      // The delta is astronomically bounded by the packet rate (<2^31
      // rejects per 10 s = 200 Mpps, impossible on 802.11), so the cast is
      // exact. `stat_post_stats_fails > lastStatsFailsSeen` above is already
      // the correct form (comparison, not subtraction) — this brings the
      // audio-reject trigger to the same shape.
      static uint32_t lastAudioRejectsSeen = 0;
      int32_t audioRejectDelta = (int32_t)(stat_audio_source_rejects -
                                           lastAudioRejectsSeen);
      bool audioRejectsGrew = serverFound && (audioRejectDelta >= 50);
      lastAudioRejectsSeen = stat_audio_source_rejects;

      if (wifiUp) {
        if ((!serverFound || contactStale || audioRejectsGrew) &&
            millis() - lastDiscoverAttempt > 10000) {
          lastDiscoverAttempt = millis();
          if (contactStale || audioRejectsGrew) {
            log_message(String("[Discovery] ") +
                        (audioRejectsGrew
                           ? "audio source-IP rejects climbing"
                           : "server contact stale") +
                        "; re-discovering...");
            serverFound = false;
          } else {
            log_message("[Discovery] Looking for server on current network...");
          }
          discover_server();
        }
      } else if (strlen(wifi_ssid) > 0 && !is_in_ap_mode) {
        if (millis() - lastServerApAttempt > 15000) {
          lastServerApAttempt = millis();
          log_message("[Net] WiFi offline; trying server AP at 192.168.4.1...");
          if (tryJoinServerAccessPoint()) {
            serverIP    = IPAddress(192, 168, 4, 1);
            serverFound = true;
            setupMulticastSockets();
          }
        }
      }

      if (millis() - lastStatsPost >= 3000) {
        lastStatsPost = millis();
        // Post stats unconditionally when the server is known.
        // The old logic skipped posting if the queue was empty, which
        // caused the node to appear offline on the server dashboard
        // during silence or pause. A queue depth of 0 is valid data.
        if (serverFound) postNodeStats();
      }

      // Round-trip clock sync: poll for a response every iteration (cheap,
      // non-blocking parsePacket()) and send a fresh request every ~2 s.
      // Kept entirely off the audio/UDP hot-path tasks per EVDPProtocolCompat.h.
      pollClockSyncResponse();
      // [new-node-sync-gate] g_force_sync_now lets applyRegistrationResponse
      // fast-path a sync_req the moment it learns we are compatible, instead
      // of waiting up to 2 s for the periodic cadence to elapse. This is
      // what promotes the server-side subscriber's hasCompletedClockSync
      // flag on the *first* post-register pass, closing the initial silent
      // window.
      //
      // [force-sync-not-lost-on-inflight] Only clear g_force_sync_now when
      // the request actually leaves the socket. sendClockSyncRequest()
      // returns early if g_sync_req_in_flight is already set (a previous
      // request is still awaiting its response or the 2 s in-flight
      // timeout). Clearing the flag before that check silently dropped the
      // "force now" intent; the periodic timer re-armed the send 2 s
      // later, but that delay was the entire reason g_force_sync_now
      // exists. Next tick retries as soon as in-flight clears (response
      // arrives, or the 2 s timeout fires in pollClockSyncResponse()).
      if (serverFound &&
          ((millis() - lastSyncReq >= 2000) || g_force_sync_now)) {
        if (!g_sync_req_in_flight) {
          g_force_sync_now = false;
          lastSyncReq = millis();
          sendClockSyncRequest();
        }
        // else: leave g_force_sync_now set; retry next tick.
      }

      // Wi-Fi disconnect tracking. We detect the transition from
      // connected -> disconnected so the counter reflects real link
      // drops, not just the "not yet connected" boot state.
      static bool was_connected = false;
      bool is_connected = (WiFi.status() == WL_CONNECTED);
          if (was_connected && !is_connected) {
          stat_wifi_disconnects++;
          ts_last_wifi_disconnect_ms = millis();
          g_udpSocketsReady = false;  // [v1.1.2-udp-ready] fds are now stale
          log_warn("[WiFi] Link lost (disconnect #" +
                   String((uint32_t)stat_wifi_disconnects) + ")");
      }
      was_connected = is_connected;

      if (!is_connected) {
        if (millis() - last_reconnect_attempt > 5000) {
          last_reconnect_attempt = millis();
          log_message("[WiFi] Link down, attempting reconnect...");
          WiFi.reconnect();
        }
      } else if (!g_udpSocketsReady && serverFound) {
        // [wifi-ps-reassert-postreconnect]
        // WiFi.reconnect() can reset the PS mode on some cores. Re-assert
        // it the moment the link is back, before any audio can arrive.
        WiFi.setSleep(false);
        // [v1.1.2-udp-ready] Link is back but the sockets were torn down
        // with the interface. Rebind so udpAudioTask resumes.
        static unsigned long last_rebind_attempt = 0;
        if (millis() - last_rebind_attempt > 3000) {
          last_rebind_attempt = millis();
          log_message("[UDP] Rebinding sockets after reconnect...");
          setupMulticastSockets();
        }
      }

      if (pending_volume_save && (millis() - volume_change_timestamp > 30000)) {
        pending_volume_save = false;
        if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
        preferences.begin("audio_node", false);
        preferences.putFloat("node_vol", node_volume_multiplier);
        preferences.end();
        if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
      }

      if (pending_speaker_save && (millis() - speaker_save_timestamp > 10000)) {
        pending_speaker_save = false;
        // [speaker-snap] Snapshot speakers[] under the audio mutex, then
        // release it BEFORE the (slow) NVS write. handlePostVolume/`handleSave`
        // mutate speakers[] under the same mutex; without the snapshot,
        // a torn `name` (31 B strncpy mid-read) reaches NVS.
        SpeakerConfig snap[4];
        if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
        for (int i = 0; i < 4; i++) snap[i] = speakers[i];
        if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);

        if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
        preferences.begin("audio_node", false);
        for (int i = 0; i < 4; i++) {
          preferences.putString(("sname_" + String(i)).c_str(), snap[i].name);
          preferences.putInt   (("svol_"  + String(i)).c_str(), snap[i].volume);
          preferences.putInt   (("slow_"  + String(i)).c_str(), snap[i].low_freq);
          preferences.putInt   (("shigh_" + String(i)).c_str(), snap[i].high_freq);
          preferences.putInt   (("ssrc_"  + String(i)).c_str(), snap[i].source);
        }
        preferences.end();
        if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---- Registration heartbeat ------------------------------------------------
// Backdates lastRegister by one full interval so the first pass fires
// immediately. Without this the node sits silent for up to ~15 s waiting for
// its own timer to roll over, which on the server side means no audio because
// the multicast gate never opens.
inline void pingServerTask(void *pvParameters) {
    // Halved registration rate: was 15000 ms. The server was doing a full
    // handlePlayerRegistration() (JSON parse, playersMutex, pruneStale,
    // subsMutex, response JSON build) per node per 15 s. At 30 s the
    // server's PLAYER_STALE_TIMEOUT_MS had to be raised from 25 s to 45 s
    // to avoid pruning a healthy node between its own heartbeats - see
    // EVDConfig.h on the server side.
    //
    // The g_force_reregister escape hatch is unchanged: a server epoch
    // change (reboot/reset) still triggers an immediate registration so
    // compat/epoch state is rebuilt before any more audio is trusted.
    static const unsigned long HEARTBEAT_MS = 30000;
    unsigned long lastRegister = millis() - HEARTBEAT_MS;

    for (;;) {
        // [ota-pause-register]
        // postRegister() allocates a DynamicJsonDocument (≈1.5 KB) and does
        // a blocking HTTP POST that competes with upload chunks for radio
        // time and heap. The node is going to reboot the moment OTA
        // completes anyway, and if OTA aborts the watchdog re-arms
        // g_force_reregister on the next networkTask iteration — so
        // skipping the POST during OTA cannot leave us unregistered.
        if (serverFound && WiFi.status() == WL_CONNECTED && !otaNodeUpdating) {
            unsigned long now = millis();
            if (now - lastRegister > HEARTBEAT_MS || g_force_reregister) {
                g_force_reregister = false;
                lastRegister = now;
                postRegister();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---- Server AP watcher (runs only in captive-portal AP mode) ---------------
// The node's own SoftAP is up on 192.168.5.1/24 for configuration. In
// parallel we try to associate as a station with the Enkelvoud server's
// SoftAP. If we succeed, the server is reachable at 192.168.4.1 and we
// light up the normal multicast audio path. If not, we just keep retrying
// and the captive portal stays available. No reboot needed either way.
//
// Important: every attempt must first *fully abort* the previous one.
// WiFi.begin() while the driver is still in STA_CONNECTING is rejected
// with "sta is connecting, cannot set config" and silently does nothing,
// which is what caused the "Server AP not reachable" loop we kept seeing.
inline void serverWatcherTask(void *pvParameters) {
    (void)pvParameters;

    // Let the AP finish settling before touching the STA side.
    vTaskDelay(pdMS_TO_TICKS(3000));

    for (;;) {
        // Already connected? Idle and re-check.
        if (serverFound && WiFi.status() == WL_CONNECTED &&
            WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        // Keep the AP side alive across retries.
        if (WiFi.getMode() != WIFI_AP_STA) {
            WiFi.mode(WIFI_AP_STA);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        WiFi.setSleep(false);

        // ---- Abort any leftover STA attempt cleanly ----------------------
        // disconnect(false,false) aborts an in-progress esp_wifi_connect()
        // without turning the radio off, but it returns before the status
        // actually flips. Wait for WL_DISCONNECTED so the next begin() is
        // accepted by the driver.
        WiFi.disconnect(false, false);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_DISCONNECTED && millis() - t0 < 2500) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(250));

        // ---- Scan first ------------------------------------------------
        // A scan tells us two things: is the server AP actually in range,
        // and which channel is it on. In AP+STA mode the ESP32 forces both
        // interfaces onto the STA's channel, so handing begin() the right
        // channel up front avoids long stalls and channel flapping. It also
        // avoids holding the radio for 10 s trying to reach a dead channel.
        int n = WiFi.scanNetworks(false, false, false, 300);
        int bestCh   = 0;
        int bestRssi = -127;
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == String(SERVER_AP_SSID)) {
                int r = WiFi.RSSI(i);
                if (r > bestRssi) { bestRssi = r; bestCh = WiFi.channel(i); }
            }
        }
        WiFi.scanDelete();

        if (bestCh == 0) {
            log_message(String("[SrvWatch] Server AP not in range. Scan saw ") +
                        String(n) + " APs:");
            for (int i = 0; i < n; i++) {
                log_message(String("  - \"") + WiFi.SSID(i) + "\"  ch " +
                            String(WiFi.channel(i)) + "  " +
                            String(WiFi.RSSI(i)) + " dBm");
            }
            vTaskDelay(pdMS_TO_TICKS(15000));
            continue;
        }

        log_message(String("[SrvWatch] Server AP on ch ") + String(bestCh) +
                    " (" + String(bestRssi) + " dBm). Connecting...");

         WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

        WiFi.begin(SERVER_AP_SSID, SERVER_AP_PASSWORD, bestCh);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 60) {
            vTaskDelay(pdMS_TO_TICKS(200));
            attempts++;
        }

        if (WiFi.status() != WL_CONNECTED) {
            log_message("[SrvWatch] Connect failed. Returning to pure AP mode.");
            // Abort this attempt too, or the next loop iteration hits the
            // same "sta is connecting" wall.
            WiFi.disconnect(false, false);
            unsigned long t1 = millis();
            while (WiFi.status() != WL_DISCONNECTED && millis() - t1 < 2500) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        log_message(String("[SrvWatch] Joined server AP. STA IP: ") +
                    WiFi.localIP().toString());

        // The server's SoftAP is always at 192.168.4.1 -- that is the whole
        // point of the probe. No need to run the full discovery dance.
        serverIP    = IPAddress(192, 168, 4, 1);
        serverFound = true;

        if (!setupMulticastSockets()) {
            log_message("[SrvWatch] Multicast join failed on STA side.");
        } else {
            log_message("[SrvWatch] Multicast joined. Node is now live.");
        }

        // Leave captive-portal mode so networkTask() runs the full STA
        // path: postNodeStats(), sendClockSyncRequest() / pollClockSync
        // Response(), and the audio deadline scheduler. Without this the
        // node streams audio but stays invisible to the server dashboard
        // and never holds its clock offset, so multi-node sync drifts.
        // The AP interface itself stays up (mode is WIFI_AP_STA); only
        // DNS-driven captive-portal behaviour is dropped. Pressing the
        // reset button re-enters startCaptivePortal() if a user still
        // needs to reconfigure.
        is_in_ap_mode = false;

        if (ping_task_handle != nullptr) {
            vTaskDelete(ping_task_handle);
            ping_task_handle = nullptr;
        }

        // [watcher-recreate] Only store the new handle on success. If the
        // create fails (out of heap / task pool), ping_task_handle must stay
        // null — otherwise the next "Lost server AP" branch will call
        // vTaskDelete() on a stale handle and trip a FreeRTOS assert.
        // [ping-stack] Match EVDPMain.h's 8192 (HTTPClient on stack).
        TaskHandle_t newPing = nullptr;
        if (xTaskCreatePinnedToCore(pingServerTask, "PingTask", 8192, NULL, 1,
                                    &newPing, 0) == pdPASS) {
            ping_task_handle = newPing;
        } else {
            log_warn("[SrvWatch] FAILED to create ping task; will not register.");
        }

        // Stay attached until the link drops.
        while (WiFi.status() == WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

        log_message("[SrvWatch] Lost server AP link. Returning to AP mode.");
        serverFound = false;
        is_in_ap_mode = true;
        WiFi.disconnect(false, false);
        unsigned long t2 = millis();
        while (WiFi.status() != WL_DISCONNECTED && millis() - t2 < 2500) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---- Server-missing probe (STA mode only) ---------------------------------
// This task is only started when initial discovery fails in STA mode.
// It runs the ESP32's non-disruptive scan (STA association is preserved)
// every 60 s and reports:
//   - whether the server's own SoftAP "Enkelvoud Server" is in range
//   - what we actually need to do to fix it
// It never changes networks on its own. There is no Server address field any
// more, so the fix is either to bring the server onto this Wi-Fi, or let the
// node join the server's own SoftAP (tryJoinServerAccessPoint()).
inline void serverProbeTask(void *pvParameters) {
    (void)pvParameters;

    // Let the rest of setup settle before the first scan.
    vTaskDelay(pdMS_TO_TICKS(30000));

    for (;;) {
        if (!is_in_ap_mode && !serverFound &&
            WiFi.status() == WL_CONNECTED) {

            log_message("[Probe] Server still missing after retries. "
                        "Scanning for server AP...");

            // async=false, show_hidden=false, passive=false, 300 ms/channel
            int n = WiFi.scanNetworks(false, false, false, 300);

            int foundCh   = 0;
            int foundRssi = -127;
            int otherAps  = 0;
            for (int i = 0; i < n; ++i) {
                if (WiFi.SSID(i) == String(SERVER_AP_SSID)) {
                    foundCh   = WiFi.channel(i);
                    foundRssi = WiFi.RSSI(i);
                    break;
                }
                otherAps++;
            }

            if (foundCh != 0) {
                log_message(String("[Probe] Server AP \"") +
                            String(SERVER_AP_SSID) +
                            "\" is in range (ch " + String(foundCh) +
                            ", " + String(foundRssi) + " dBm).");
                log_message("[Probe] The server is NOT on this network. Either:");
                log_message(String("[Probe]   a) put the server on '") +
                            WiFi.SSID() + "', or");
                log_message(String("[Probe]   b) let the node join \"") +
                            String(SERVER_AP_SSID) +
                            "\" (it retries automatically), or move it here");
            } else {
                log_message(String("[Probe] Server AP not visible either ")
                            + "(scanned " + String(otherAps) +
                            " other APs). Is the server powered on?");
                // Print what we did see, so a wrong SSID is obvious.
                for (int i = 0; i < n && i < 8; ++i) {
                    log_message(String("[Probe]   saw \"") + WiFi.SSID(i) +
                                "\"  ch " + String(WiFi.channel(i)) +
                                "  " + String(WiFi.RSSI(i)) + " dBm");
                }
            }
            WiFi.scanDelete();
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}