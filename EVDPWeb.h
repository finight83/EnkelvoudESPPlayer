#pragma once
// ============================================================================
//  EVDPWeb.h - HTTP handlers, OTA receiver, config save/load
// ============================================================================
#include "EVDPState.h"
#include "EVDPNet.h"

// ---- OTA (receiver side) ---------------------------------------------------
inline void handleNodeFw() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", String("{\"version\":\"") + FW_VERSION + "\"}");
}

// [ota-trust-cascade]
// Was: otaSessionTrusted = serverFound && remoteIP()==serverIP;
// serverFound is a volatile cleared by networkTask() after 180 s of
// failed stats POSTs. A transient Wi-Fi blip that starves stats POSTs
// but leaves the TCP path healthy makes the NEXT OTA push rejected with
// "Refused untrusted uploader", even though the peer is the correct
// server. serverIP persists after discovery, so gate on it alone.
// Also cache the first trusted uploader IP so a DHCP renewal on the
// server (which changes serverIP without changing who is uploading)
// does not break subsequent pushes.
static IPAddress otaTrustedUploaderIp(0, 0, 0, 0);

inline void handleNodeOtaUpload() {
    HTTPUpload &up = server.upload();
    static bool otaSessionTrusted = false;
    static bool otaWriteFailed    = false;

    if (up.status == UPLOAD_FILE_START) {
        IPAddress peerIp = server.client().remoteIP();

        bool trusted = false;
        if (otaTrustedUploaderIp != IPAddress(0, 0, 0, 0) &&
            peerIp == otaTrustedUploaderIp) {
            trusted = true;
        } else if (peerIp != IPAddress(0, 0, 0, 0) &&
                   peerIp == serverIP) {
            trusted = true;
            otaTrustedUploaderIp = peerIp;
        } else if (mdns_target_host[0]) {
            IPAddress cfg;
            if (cfg.fromString(mdns_target_host) && peerIp == cfg) {
                trusted = true;
                otaTrustedUploaderIp = peerIp;
            }
        }

        otaSessionTrusted = trusted;
        otaWriteFailed    = false;   // fresh session; forget any prior failure

        if (!otaSessionTrusted) {
            log_warn("[OTA] Refused untrusted uploader " + peerIp.toString() +
                     " (server=" + serverIP.toString() +
                     " cached=" + otaTrustedUploaderIp.toString() + ")");
            return;
        }
        log_message("[OTA] Trusted uploader " + peerIp.toString());
    }

    if (up.status == UPLOAD_FILE_ABORTED) {
        bool wasTrusted = otaSessionTrusted;
        if (wasTrusted) Update.abort();
        otaSessionTrusted     = false;
        g_force_rebuffer      = true;
        otaNodeUpdating       = false;
        otaNodeFirmwareReady  = false;
        otaNodeLastActivityMs = 0;
        g_udpSocketsReady     = (WiFi.status() == WL_CONNECTED);
        if (wasTrusted) log_warn("[OTA] Aborted by peer.");
        return;
    }

    if (!otaSessionTrusted) return;

    if (up.status == UPLOAD_FILE_START) {
        // [ota-retry-preserves-committed-image]
        // A previous upload can leave a COMPLETE image in the inactive
        // partition without ever running handleNodeOtaDone(): the server's
        // otaPushToNode() treats "raw.print(tail) == 0" as a retryable
        // failure and reconnects, and any transient client-side drop between
        // UPLOAD_FILE_END and the final POST handler leaves the node in
        // exactly this state. Without the check below, the retry's
        // Update.begin() fails (an update is already in progress), then the
        // failure path's Update.abort() DISCARDS the fully-received
        // firmware — the exact bug this block fixes.
        if (otaNodeUpdating && otaNodeFirmwareReady) {
            log_warn("[OTA] New upload arrived while a complete image is "
                     "pending commit; committing and rebooting.");
            if (audio_queue) xQueueReset(audio_queue);
            g_udpSocketsReady = false;
            if (Update.end(true)) {
                bool notified = postOtaResult(true);
                log_message(String("[OTA] Pending image committed; "
                                   "server notification ") +
                            (notified ? "sent" : "FAILED (non-fatal)"));
                delay(500);
                log_message("[OTA] Rebooting into new firmware.");
                ESP.restart();
            }
            log_warn(String("[OTA] Update.end on pending image failed: ") +
                     Update.errorString());
            Update.abort();
            // Fall through: treat this attempt as a fresh upload.
        }
        // Partial-write retry: no complete image to protect, but the
        // in-progress Update still has to be aborted before a fresh
        // Update.begin() can succeed. Previously this consumed a retry
        // slot (begin failed, abort cleared, server retried again);
        // do it eagerly so the first retry works.
        else if (otaNodeUpdating) {
            log_warn("[OTA] Retry during partial upload; aborting "
                     "previous attempt and restarting.");
            Update.abort();
        }

        log_message("[OTA] Receiving " + up.filename);
        otaNodeUpdating       = true;
        otaNodeFirmwareReady  = false;
        otaNodeLastActivityMs = millis();
        g_udpSocketsReady     = false;
        if (audio_queue) xQueueReset(audio_queue);

        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            log_warn(String("[OTA] Update.begin failed: ") +
                     Update.errorString() + " — aborting, resuming audio.");
            Update.abort();
            otaNodeUpdating       = false;
            otaNodeFirmwareReady  = false;
            otaNodeLastActivityMs = 0;
            g_udpSocketsReady     = (WiFi.status() == WL_CONNECTED);
            return;
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        otaNodeLastActivityMs = millis();
        if (otaWriteFailed) return;
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            log_warn(String("[OTA] write failed: ") + Update.errorString() +
                     " — aborting now, will not commit.");
            otaWriteFailed = true;
            Update.abort();
            g_force_rebuffer      = true;
            otaNodeUpdating       = false;
            otaNodeFirmwareReady  = false;
            otaNodeLastActivityMs = 0;
            g_udpSocketsReady     = (WiFi.status() == WL_CONNECTED);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (otaWriteFailed) {
            log_warn("[OTA] Refusing to commit: a chunk failed to write.");
            Update.abort();
            g_force_rebuffer      = true;
            otaNodeUpdating       = false;
            otaNodeFirmwareReady  = false;
            otaNodeLastActivityMs = 0;
            g_udpSocketsReady     = (WiFi.status() == WL_CONNECTED);
            return;
        }
        otaNodeFirmwareReady = true;
        log_message(String("[OTA] Written, ") + up.totalSize +
                    " bytes. Awaiting server notification before commit.");
    }
}

inline void handleNodeOtaDone() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    bool ready = otaNodeFirmwareReady && !Update.hasError();

    // Not-ready / write-error path: report a real failure to the pusher.
    if (!ready) {
        server.send(500, "application/json",
                    "{\"ok\":false,\"status\":\"not_ready\"}");
        Update.abort();
        g_force_rebuffer      = true;
        otaNodeUpdating       = false;
        otaNodeFirmwareReady  = false;
        otaNodeLastActivityMs = 0;
        g_udpSocketsReady = (WiFi.status() == WL_CONNECTED);
        return;
    }

    // Commit BEFORE the 200 response. Previously the 200 was sent first
    // and Update.end(true) ran afterwards; a failed commit then left the
    // pushing server believing the node had updated, while the node kept
    // running the old firmware. Commit-first makes a failed commit a
    // 500 to otaPushToNode(), which correctly reports the node update
    // as failed. The pusher's 8 s response-wait cap means it does not
    // block on this flash write.
    if (!Update.end(true)) {
        log_warn(String("[OTA] Update.end failed: ") + Update.errorString());
        server.send(500, "application/json",
                    "{\"ok\":false,\"status\":\"commit_failed\"}");
        g_force_rebuffer      = true;
        otaNodeUpdating       = false;
        otaNodeFirmwareReady  = false;
        otaNodeLastActivityMs = 0;
        g_udpSocketsReady = (WiFi.status() == WL_CONNECTED);
        return;
    }

    // Commit is durable. Only now answer the uploader.
    server.send(200, "application/json",
                "{\"ok\":true,\"status\":\"committing\"}");

    // Best-effort server notification. Non-fatal if it fails: the next
    // /api/player/register heartbeat carries the new FW_VERSION anyway.
    bool notified = postOtaResult(true);
    log_message(String("[OTA] Committed; server notification ") +
                (notified ? "sent" : "FAILED (non-fatal)"));

    delay(500);
    log_message("[OTA] Rebooting into new firmware.");
    ESP.restart();
}

// EVDSET.h provides:
//   - evdJsonEscape()                — used by handleApiCfg() and handleSave()
//   - SpeakerConfig / TYPES metadata — shared with the JS tab builder
//   - EVDSET_STATUS_PAGE             — served verbatim by handleStatusPage()
//   - EVDSET_PAGE_HTML               — source-of-truth ONLY; compiled out and
//                                      regenerated into EVDSET_GZ.h by gen_gz.py
// Must be included after WebServer (declared in EVDPState.h) exists.
#include "EVDSET.h"

// [v1.1.2-cfg-fetch]
// EVDSET_GZ.h holds the gzipped dashboard, generated from EVDSET_PAGE_HTML
// by gen_gz.py. handleRoot() serves it with Content-Encoding: gzip.
#include "EVDSET_GZ.h"

// ---- GET /api/cfg ----------------------------------------------------------
// [v1.1.2-cfg-fetch]
// Returns the config JSON the page used to have inlined as `var CFG = {...}`.
// Field names and shapes are identical, so the JS change is a pure move:
// the page does fetch('/api/cfg') then calls bootFromCfg() with CFG set.
//
// JSON is built into a stack buffer, same as the previous version of this
// data. evdJsonEscape is applied to every string; pin numbers go through %d.
inline void handleApiCfg() {
    int vol = (int)(node_volume_multiplier * 100.0f + 0.5f);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    // [escape-size] Name-like fields (max 64 B, escaped up to 6x) need 385.
    // IPs are IPv4 literals (max 15 chars, no escapable chars) - 48 is fine.
    // Stack cost: 4*385 + 5*48 ≈ 1780 B plus cfg[4096] below, within the
    // WebServer task's budget (typically >= 8 KB).
    char eName[6 * 64 + 1], eServer[6 * 64 + 1],
         eSsid[6 * 64 + 1], ePass[6 * 64 + 1];
    char eServerIp[48];
    char eIp[48], eGw[48], eSn[48], eDns[48];
    String sipStr = serverIP.toString();
    evdJsonEscape(friendly_name,    eName,     sizeof(eName));
    evdJsonEscape(mdns_target_host, eServer,   sizeof(eServer));
    evdJsonEscape(sipStr.c_str(),   eServerIp, sizeof(eServerIp));
    evdJsonEscape(wifi_ssid,        eSsid,     sizeof(eSsid));
    evdJsonEscape(wifi_pass,        ePass,     sizeof(ePass));
    evdJsonEscape(static_ip_str,    eIp,       sizeof(eIp));
    evdJsonEscape(static_gw_str,    eGw,       sizeof(eGw));
    evdJsonEscape(static_sn_str,    eSn,       sizeof(eSn));
    evdJsonEscape(static_dns_str,   eDns,      sizeof(eDns));

    // [cfg-buf-static] static, not stack: the network task has 12 KB of
    // stack and this handler also owns four 385-byte escape buffers and
    // four 48-byte IP buffers. 4096 on the stack pushed peak stack to
    // ~6 KB on a 12 KB task; a slightly longer future field tips it.
    // Static is safe because WebServer is single-worker; if you ever
    // configure a multi-worker WebServer, either add a mutex around this
    // handler or revert to a stack buffer and raise the networkTask stack.
    static char cfg[4096];
    int n = snprintf(cfg, sizeof(cfg),
        "{\"nodeVolume\":%d,\"name\":\"%s\",\"server\":\"%s\","
        "\"serverIp\":\"%s\",\"serverFound\":%s,"
        "\"ssid\":\"%s\",\"pass\":\"%s\","
        "\"useStatic\":%s,\"ip\":\"%s\",\"gw\":\"%s\",\"sn\":\"%s\",\"dns\":\"%s\","
        "\"speakers\":[",
        vol, eName, eServer,
        eServerIp, serverFound ? "true" : "false",
        eSsid, ePass,
        use_static_ip ? "true" : "false", eIp, eGw, eSn, eDns);

    for (int i = 0; i < 4 && n > 0; i++) {
        // [cfgspeaker-escape] 80 B truncated any name that contained
        // quotes, backslashes, or UTF-8. 6*32+1 = 193 covers the worst
        // case (every byte escaped to \\uXXXX).
        char eSpk[6 * 32 + 1];
        evdJsonEscape(speakers[i].name, eSpk, sizeof(eSpk));
        int w = snprintf(cfg + n, sizeof(cfg) - n,
            "%s{\"name\":\"%s\",\"volume\":%d,\"low_freq\":%d,\"high_freq\":%d,\"source\":%d}",
            i ? "," : "", eSpk, speakers[i].volume,
            speakers[i].low_freq, speakers[i].high_freq, speakers[i].source);
        if (w < 0 || w >= (int)(sizeof(cfg) - n)) { n = 0; break; }
        n += w;
    }

    if (n > 0 && n + 2 < (int)sizeof(cfg)) {
        cfg[n++] = ']';
        cfg[n++] = '}';
        cfg[n]   = '\0';
    } else {
        // Fallback. Should be unreachable (each speaker name is capped at
        // 31 bytes), but emit a syntactically valid config so the page
        // still boots instead of failing the parse.
        n = snprintf(cfg, sizeof(cfg),
            "{\"nodeVolume\":%d,\"name\":\"\",\"server\":\"\","
            "\"serverIp\":\"\",\"serverFound\":false,"
            "\"ssid\":\"\",\"pass\":\"\","
            "\"useStatic\":false,\"ip\":\"\",\"gw\":\"\",\"sn\":\"\",\"dns\":\"\","
            "\"speakers\":[]}",
            vol);
        if (n < 0) n = 0;
    }

    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.send(200, "application/json", String(cfg, (size_t)n));
}

// ---- Handlers --------------------------------------------------------------
inline void handleGetLogs() {
    static char snap[MAX_LOG_LINES][MAX_LOG_LINE_LEN];
    int count, start;
    portENTER_CRITICAL(&g_log_ring_mux);
    count = system_log_count;
    start = (system_log_head + MAX_LOG_LINES - count) % MAX_LOG_LINES;
    for (int i = 0; i < count; i++) {
        memcpy(snap[i], system_logs[(start + i) % MAX_LOG_LINES], MAX_LOG_LINE_LEN);
    }
    portEXIT_CRITICAL(&g_log_ring_mux);

    // [log-escape-size] Two-pass: exact escaped length first, then reserve.
    // 96 B/line (old) was too small for quote-heavy or non-ASCII logs;
    // 964 B/line (last patch) reserved ~48 KB per call - on a fragmented
    // heap that reserve is more likely to fail than the old under-estimate.
    // Exact-sizing has neither failure mode.
    // [log-est-slack] 8 bytes was "[\"\"]" + NUL exactly. A full ring
    // under-estimates by an unbounded amount if evdJsonEscape's expansion
    // table ever grows; a single reallocation on a fragmented heap next to
    // the audio task is exactly what the reserve() was added to prevent.
    // [log-est-cap-24k] Worst case is 50 × 160 × 6 ≈ 48 KB of pre-allocated
    // String, per 2 s poll, while the audio task is decoding Opus. That
    // reserve fails on a fragmented 97 KB heap. Cap the estimate so the
    // reserve almost always succeeds; if a full non-ASCII ring really
    // exceeds 24 KB the response is simply truncated to what fit, which
    // is the failure mode the browser's list rendering already tolerates.
    size_t est = 64;
    for (int i = 0; i < count; i++) {
        est += 3;
        for (const char* p = snap[i]; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if      (c == '"' || c == '\\')                        est += 2;
            else if (c < 0x20 || c == '<' || c == '>' || c == '&') est += 6;
            else                                                    est += 1;
        }
        if (est >= 24576) { est = 24576; break; }
    }

    String json;
    if (!json.reserve(est)) {
        // [logs-reserve-guard] On a fragmented heap a full-ring response
        // can need ~16 KB. If reserve() cannot satisfy it, send a small
        // JSON error rather than letting String grow through reallocations
        // and transiently starving the audio task's String usage.
        server.send(200, "application/json", "[\"[logs unavailable: oom]\"]");
        return;
    }
    json = "[";
    for (int i = 0; i < count; i++) {
        if (i > 0) json += ",";
        char esc[MAX_LOG_LINE_LEN * 6 + 1];
        evdJsonEscape(snap[i], esc, sizeof(esc));
        json += "\"" + String(esc) + "\"";
    }
    json += "]";
    // [logs-truncation-guard] String::concat silently no-ops on OOM,
    // so if the total escaped content exceeded the 24 KB reserve the
    // loop can leave json missing its closing ']' (or truncated
    // mid-entry). Detect that and send a syntactically valid sentinel
    // instead of a body the browser's JSON.parse() rejects.
    if (json.length() == 0 ||
        json[json.length() - 1] != ']' ||
        json[0] != '[') {
        server.send(200, "application/json", "[\"[logs truncated: oom]\"]");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

inline void handleCORS() {

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204);
}

inline void handleScanNetworks() {
    // [ota-gate-scan]
    // A synchronous WiFi.scanNetworks() parks the radio on each channel
    // for 1-3 s. During OTA the upload socket is on that same radio, so
    // a scan starting mid-push will drop the TCP stream, the server-side
    // pusher will treat it as retryable, and the node will end up doing
    // a partial-write abort that discards the whole push (see
    // [ota-retry-preserves-committed-image] for the receive-side cost).
    // Mirrors [ota-gate-scan] on the server side.
    //
    // Placed BEFORE the audio-busy gate: an OTA in progress is a strictly
    // stronger reason to reject a scan than "audio is streaming", and the
    // UI already handles a 503 from this endpoint by leaving the previous
    // list on screen.
    if (otaNodeUpdating) {
        server.sendHeader("Cache-Control", "no-store");
        server.send(503, "application/json",
                    "{\"error\":\"ota_active\","
                    "\"message\":\"firmware update in progress; retry after it finishes\"}");
        return;
    }
    // A synchronous WiFi scan suspends the STA radio for 1-3 s per channel;
    // every multicast audio datagram received during the window is lost and
    // audioPlaybackTask() will rebuffer (stat_rebuffers++, audible glitch).
    // Refuse the scan while audio is actively flowing so an operator clicking
    // "Scan" on the Network tab cannot trip the audio path into starvation.
    // ts_last_audio_packet_ms == 0 means we have never received audio since
    // boot; the 1000 ms window covers any plausible send cadence.
    if (ts_last_audio_packet_ms != 0 &&
        (uint32_t)(millis() - ts_last_audio_packet_ms) < 1000) {
        server.sendHeader("Cache-Control", "no-store");
        server.send(503, "application/json",
                    "{\"error\":\"scan_busy\","
                    "\"message\":\"audio is streaming; retry in a few seconds\"}");
        return;
    }

    int n = WiFi.scanNetworks();
    String json = "[";
    json.reserve((size_t)n * 64 + 8);
    for (int i = 0; i < n; ++i) {
        if (i > 0) json += ",";
        // [scan-json-escape] An SSID is an arbitrary 32-byte octet
        // string that routinely contains double quotes (e.g. the
        // common `"My"WiFi` form) and occasionally a backslash. String
        // concatenation with the raw SSID produced malformed JSON and
        // the client's JSON.parse() threw, so the scan list went
        // blank. evdJsonEscape() is the same helper handleApiCfg()
        // already uses.
        // [scan-ssid-size] 802.11 SSID is 32 B max; escaped worst case
        // 6*32+1 = 193 B. 80 was under even that (never mind the 64 B
        // storage this firmware allows).
        char ssid[6 * 32 + 1];
        evdJsonEscape(WiFi.SSID(i).c_str(), ssid, sizeof(ssid));
        json += "{\"ssid\":\"" + String(ssid) +
                "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
    WiFi.scanDelete();
}

inline void handleGetVolume() {
    DynamicJsonDocument doc(1400);
    doc["volume"] = node_volume_multiplier;
    doc["muted"]  = node_muted;
    JsonArray arr = doc.createNestedArray("speakers");
    for (int i = 0; i < 4; i++) {
        JsonObject s = arr.createNestedObject();
        s["name"]      = speakers[i].name;
        s["volume"]    = speakers[i].volume;
        s["low_freq"]  = speakers[i].low_freq;
        s["high_freq"] = speakers[i].high_freq;
        s["source"]    = speakers[i].source;
    }
    String response;
    serializeJson(doc, response);
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", response);
}

inline bool validate_and_update_crossover(float &low, float &high, float max_freq) {
    // Hard ceiling: anything >= 0.45*fs is silently bypassed by
    // Biquad::setLowpass / setHighpass (see EVDPFilters.h). 0.44*fs
    // keeps a margin so the requested cutoff is always representable.
    const float HARD_CAP = (float)SAMPLE_RATE * 0.44f;
    if (max_freq > HARD_CAP) max_freq = HARD_CAP;
    if (low < 0.0f) low = 0.0f;
    if (high > 20000.0f) high = 20000.0f;
    if (high > max_freq) high = max_freq;
    if (low > high) low = high;
    return true;
}

inline void handlePostVolume() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String body = "";
    if (server.hasArg("plain")) body = server.arg("plain");
    else if (server.args() > 0)  body = server.arg(0);

    if (body.length() > 0) {
        DynamicJsonDocument doc(512);
        if (!deserializeJson(doc, body)) {

            // [postvol-clamp-deferred-log]
            // Capture the clamp into locals here and report via
            // logRingPush() AFTER the mutex release. log_warn() would
            // Serial.println under g_audio_config_mutex, stalling the
            // audio task (Core 1, portMAX_DELAY) for ~6 ms per line.
            int   postvolClampIdx       = -1;
            float postvolClampLoBefore  = 0.0f, postvolClampHiBefore = 0.0f;
            float postvolClampLoAfter   = 0.0f, postvolClampHiAfter  = 0.0f;

            if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
            if (!doc["volume"].isNull()) {
                float v = doc["volume"].as<float>();
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                node_volume_multiplier = v;
                pending_volume_save = true;
                volume_change_timestamp = millis();
            }
            if (!doc["muted"].isNull()) {
                node_muted = doc["muted"].as<bool>();
            }
            if (!doc["speaker_index"].isNull() && !doc["speaker_volume"].isNull()) {
                int i = doc["speaker_index"].as<int>();
                if (i >= 0 && i < 4) {
                    int v = doc["speaker_volume"].as<int>();
                    if (v < 0)   v = 0;
                    if (v > 100) v = 100;
                    // [postvol-volume-skip-redundant-save]
                    // Only mark pending_speaker_save when the value
                    // actually changed. Slider drags that end on the
                    // same pixel no longer trigger 20 NVS writes.
                    if (speakers[i].volume != v) {
                        speakers[i].volume = v;
                        pending_speaker_save = true;
                        speaker_save_timestamp = millis();
                    }
                }
            }
            if (!doc["speaker_index"].isNull() &&
                (!doc["speaker_low"].isNull() || !doc["speaker_high"].isNull())) {
                int i = doc["speaker_index"].as<int>();
                if (i >= 0 && i < 4) {
                    float low  = doc["speaker_low"].isNull()  ? (float)speakers[i].low_freq  : doc["speaker_low"].as<float>();
                    float high = doc["speaker_high"].isNull() ? (float)speakers[i].high_freq : doc["speaker_high"].as<float>();
                    float origLow = low, origHigh = high;
                    validate_and_update_crossover(low, high, (float)SAMPLE_RATE * 0.44f);
                    if (low != origLow || high != origHigh) {
                        postvolClampIdx      = i;
                        postvolClampLoBefore = origLow;
                        postvolClampHiBefore = origHigh;
                        postvolClampLoAfter  = low;
                        postvolClampHiAfter  = high;
                    }
                    // [postvol-skip-redundant-reconfigure]
                    // SpeakerFilter::configure() resets the user-facing
                    // HP/LP biquad state (z1 = z2 = 0). On the sub
                    // channel (LP@300, playing a 100 Hz bass note) that
                    // truncates the wave to 0 mid-cycle. Mirror the
                    // freqChanged guard handleSave() already uses.
                    int newLowFreq  = (int)low;
                    int newHighFreq = (int)high;
                    bool xoverChanged = (speakers[i].low_freq  != newLowFreq) ||
                                        (speakers[i].high_freq != newHighFreq);
                    speakers[i].low_freq  = newLowFreq;
                    speakers[i].high_freq = newHighFreq;
                    if (xoverChanged) {
                        speaker_filters[i].configure(speakers[i].low_freq,
                                                     speakers[i].high_freq,
                                                     SAMPLE_RATE);
                        // [postvol-xover-skip-redundant-save]
                        // Only mark pending_speaker_save when the
                        // crossover actually changed. A slider drag that
                        // ends on the same value produced 20 NVS writes
                        // under g_prefs_mutex (~2 s of networkTask stall)
                        // for nothing.
                        pending_speaker_save = true;
                        speaker_save_timestamp = millis();
                    }
                }
            }
            if (!doc["speaker_index"].isNull() && doc["speaker_name"].is<const char*>()) {
                int i = doc["speaker_index"].as<int>();
                if (i >= 0 && i < 4) {
                    const char* sName = doc["speaker_name"];
                    // [postvol-name-skip-redundant-save]
                    // Rename to the same value (pencil click, Enter with
                    // no edit) no longer triggers 20 NVS writes.
                    if (strncmp(speakers[i].name, sName, sizeof(speakers[i].name) - 1) != 0) {
                        strncpy(speakers[i].name, sName, sizeof(speakers[i].name) - 1);
                        speakers[i].name[sizeof(speakers[i].name) - 1] = '\0';
                        pending_speaker_save = true;
                        speaker_save_timestamp = millis();
                    }
                }
            }
            if (!doc["speaker_index"].isNull() && !doc["speaker_source"].isNull()) {
                int i = doc["speaker_index"].as<int>();
                if (i >= 0 && i < 4) {
                    int src = doc["speaker_source"].as<int>();
                    // [postvol-source-skip-redundant-save]
                    // Re-selecting the same option no longer triggers
                    // 20 NVS writes.
                    if (src >= 0 && src <= 2 && speakers[i].source != src) {
                        speakers[i].source = src;
                        pending_speaker_save = true;
                        speaker_save_timestamp = millis();
                    }
                }
            }
            if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);

            // [postvol-clamp-deferred-log] Report outside the mutex,
            // ring only (no UART). Rate-limited to 1 Hz.
            if (postvolClampIdx >= 0) {
                static uint32_t lastPostVolClampLogMs = 0;
                uint32_t nowClamp = millis();
                if (nowClamp - lastPostVolClampLogMs >= 1000) {
                    lastPostVolClampLogMs = nowClamp;
                    logRingPush(String("[Volume] Crossover clamped (spk ") + String(postvolClampIdx) +
                                "): " + String((int)postvolClampLoBefore) + "-" + String((int)postvolClampHiBefore) +
                                " -> " + String((int)postvolClampLoAfter) + "-" + String((int)postvolClampHiAfter) + " Hz");
                }
            }

            // [vol-no-nvs] No NVS here. This handler only mutates
            // in-memory state and sets pending_*_save; the actual
            // debounced NVS write runs later in networkTask under
            // g_prefs_mutex.
        }
    }
    handleGetVolume();
}

inline void handleGetHealth() {
    uint32_t now = millis();
    auto ageOf = [now](uint32_t t) -> uint32_t {
        if (t == 0) return 0xFFFFFFFFu;
        int32_t d = (int32_t)(now - t);
        return d < 0 ? 0u : (uint32_t)d;
    };

    // ---- CPU / RAM / PSRAM stats (stat pills + status cards) -------------
    int8_t cpu0Pct = -1, cpu1Pct = -1;
    {
      // [cpu-stat-task-array-64] Was 48. On a node running audioPlayback +
      // udpAudio + network + ping + WebServer + WiFi + lwIP + AsyncTCP +
      // system timers + both idle tasks the live count regularly reaches
      // 45-50. When uxTaskGetSystemState fills the array it silently
      // truncates the remainder INCLUDING the idle tasks (usually last in
      // the snapshot). currIdle0/1 then stay 0, the delta underflows, and
      // the gotIdle0/gotIdle1 guard below suppresses the sample — the CPU
      // pills stick at "n/a" until reboot. 64 matches the server-side
      // EVDSTS.h; the static array costs 1.5 KB of BSS and is reused per
      // call, so the cost is paid once at link time.
      static TaskStatus_t taskSnap[64];
      static uint32_t      prevIdle0 = 0;
      static uint32_t      prevIdle1 = 0;
      static uint32_t      prevTotal = 0;
      static bool          haveBase  = false;

      uint32_t totalRuntime = 0;
      UBaseType_t nTasks = uxTaskGetSystemState(taskSnap, 64, &totalRuntime);
      if (nTasks > 64) nTasks = 64;
      if (nTasks > 0 && totalRuntime > 0) {
        TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
        TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
        uint32_t currIdle0 = 0, currIdle1 = 0;
        // [cpu-idle-found]
        // If the 48-entry taskSnap[] fills up (many tasks + software
        // timers + HTTP/OTA workers during a POST), uxTaskGetSystemState
        // can omit the idle tasks. currIdle0/1 then stay 0, the
        // unsigned deltas underflow, and every subsequent sample reports
        // 0% CPU on both cores until the next boot. Track whether both
        // idle tasks were actually seen and skip the update if not —
        // the last valid percentages stay on the dashboard and the
        // baseline is preserved for the next successful sample.
        bool gotIdle0 = false, gotIdle1 = false;
        for (UBaseType_t i = 0; i < nTasks; i++) {
          if (taskSnap[i].xHandle == idle0) { currIdle0 = taskSnap[i].ulRunTimeCounter; gotIdle0 = true; }
          if (taskSnap[i].xHandle == idle1) { currIdle1 = taskSnap[i].ulRunTimeCounter; gotIdle1 = true; }
        }
        if (!gotIdle0 || !gotIdle1) {
          // Snapshot truncated; keep last-known percentages, do not
          // re-baseline (prevIdle* / prevTotal stay untouched).
        } else if (haveBase) {
          uint32_t dIdle0 = currIdle0 - prevIdle0;
          uint32_t dIdle1 = currIdle1 - prevIdle1;
          uint32_t dTotal = totalRuntime - prevTotal;
          if (dTotal >= 0x80000000u) {
            // [cpu-rebase] Dashboard was closed (or document.hidden) for
            // > ~36 min, so totalRuntime - prevTotal exceeds 31 bits and
            // the unsigned delta underflowed. Re-seed the baseline from
            // the current sample; otherwise the guard below rejects on
            // every call and C0/C1 stick at the pre-gap value until the
            // next reboot (the "n/a" pill behaviour the comment in the
            // 64-entry block was meant to fix).
            prevIdle0 = currIdle0;
            prevIdle1 = currIdle1;
            prevTotal = totalRuntime;
          } else if (dTotal >= 100000u &&
                     dIdle0 <= dTotal && dIdle1 <= dTotal) {
            uint32_t idle0Pct = (uint32_t)((uint64_t)dIdle0 * 100ULL / dTotal);
            uint32_t idle1Pct = (uint32_t)((uint64_t)dIdle1 * 100ULL / dTotal);
            if (idle0Pct > 100) idle0Pct = 100;
            if (idle1Pct > 100) idle1Pct = 100;
            cpu0Pct = (int8_t)(100 - idle0Pct);
            cpu1Pct = (int8_t)(100 - idle1Pct);
            prevIdle0 = currIdle0;
            prevIdle1 = currIdle1;
            prevTotal = totalRuntime;
          }
          // else: short interval; keep last-known, no re-baseline.
        } else {
          prevIdle0 = currIdle0;
          prevIdle1 = currIdle1;
          prevTotal = totalRuntime;
          haveBase  = true;
        }
      }
    }

    uint32_t heapTotal = ESP.getHeapSize();
    uint32_t heapFree  = ESP.getFreeHeap();
    uint32_t heapUsed  = heapTotal > heapFree ? heapTotal - heapFree : 0;
    uint8_t  heapPct   = heapTotal
                         ? (uint8_t)((uint64_t)heapUsed * 100 / heapTotal)
                         : 0;

    uint32_t psramTotal = ESP.getPsramSize();
    uint32_t psramFree  = ESP.getFreePsram();
    uint32_t psramUsed  = psramTotal > psramFree ? psramTotal - psramFree : 0;
    uint8_t  psramPct   = psramTotal
                          ? (uint8_t)((uint64_t)psramUsed * 100 / psramTotal)
                          : 0;


    // [health-doc-size-6144] BUGFIX: 114 keys at ~16 B/slot + ~19 B/key +
    // string-value copies = ~4.7 KB minimum pool consumption; 4096
    // overflowed by ~15 %, dropping the last ~25 fields (encoder mirror,
    // DAC readiness, emptyFlushes, corrector totals) from /api/health.
    // Those are exactly the fields the Status page and 10-min recorder
    // read to distinguish "pipeline healthy" from "pipeline silent but
    // not reporting". The earlier 4 KB "optimisation" reasoned from the
    // SERIALISED length (2.8 KB) not the POOL length, which are two
    // different numbers in ArduinoJson 6.
    //
    // 6144 leaves ~22 % headroom for one more encoder/source-rate pair
    // without returning to the 8 KB doc + 8 KB String = 16 KB peak that
    // the 4 KB change was trying to escape. The doc is freed after
    // serializeJson() returns, so the peak is transient.
    //
    // If a future field tips 6144, bump to 8192 rather than dropping
    // fields — the observability cost of missing fields is worse than
    // the ~2 KB extra transient heap.
    DynamicJsonDocument doc(6144);
    doc["ok"]        = true;
    doc["uptimeSec"] = (uint32_t)(now / 1000UL);

    // ---- System ----------------------------------------------------------
    doc["heapFree"]     = (uint32_t)ESP.getFreeHeap();
    doc["heapMin"]      = (uint32_t)ESP.getMinFreeHeap();
    doc["heapMaxAlloc"] = (uint32_t)ESP.getMaxAllocHeap();
    doc["bootReason"]   = boot_reason_str;
    doc["bootCount"]    = (uint32_t)boot_count;
    doc["version"]      = FW_VERSION;

    // ---- CPU / RAM / PSRAM -----------------------------------------------
    doc["cpuCore0Pct"]     = (int)cpu0Pct;
    doc["cpuCore1Pct"]     = (int)cpu1Pct;
    doc["heapUsedPct"]     = (int)heapPct;
    doc["heapTotalBytes"]  = (uint32_t)heapTotal;
    doc["psramUsedPct"]    = (int)psramPct;
    doc["psramFreeBytes"]  = (uint32_t)psramFree;
    doc["psramTotalBytes"] = (uint32_t)psramTotal;


    // ---- Network ---------------------------------------------------------
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    doc["wifiConnected"]   = wifiUp;
    doc["wifiRssi"]        = wifiUp ? WiFi.RSSI() : 0;
    doc["wifiChannel"]     = wifiUp ? WiFi.channel() : 0;
    doc["wifiDisconnects"] = (uint32_t)stat_wifi_disconnects;
    // [wifi-ps-exposed] WiFi.getSleep() returns true when modem sleep is
    // ENABLED. A node with gap>100ms periodicity but this reporting false
    // has disproved the DTIM hypothesis and should look at L2 retries
    // (auth/deauth, A-MPDU, roaming) instead of PS.
    doc["wifiSleepEnabled"] = (bool)WiFi.getSleep();
    doc["ip"]              = wifiUp ? WiFi.localIP().toString()
                                    : WiFi.softAPIP().toString();

    // ---- Server link -----------------------------------------------------
    doc["serverFound"]           = serverFound;
    doc["serverIp"]              = serverIP.toString();
    doc["lastServerContactAgeMs"] = ageOf(ts_last_server_contact_ms);
    doc["discoveryFailures"]     = (uint32_t)stat_discovery_failures;
    doc["multicastJoinFails"]    = (uint32_t)stat_multicast_join_fails;

    // ---- Audio protocol compatibility -------------------------------------
    // Mirrors the server's EVDProtocolCompat.h fail-closed policy from the
    // player's side; see EVDPProtocolCompat.h and applyRegistrationResponse()
    // in EVDPNet.h for how this state is derived.
    doc["declaredAudioProtocolVersion"] = (int)PLAYER_DECLARED_AUDIO_PROTOCOL_VERSION;
    doc["negotiatedAudioProtocolVersion"] = (int)g_negotiated_audio_protocol_version;
    doc["audioCompatState"] = (g_audio_compat_state == AUDIO_COMPAT_COMPATIBLE)   ? "compatible"   :
                               (g_audio_compat_state == AUDIO_COMPAT_INCOMPATIBLE) ? "incompatible" :
                                                                                      "unknown";
    doc["audioCompatible"]   = (g_audio_compat_state == AUDIO_COMPAT_COMPATIBLE);
    doc["audioCompatReason"] = g_audio_compat_reason;
    doc["audioWithheldIncompatible"] = (uint32_t)stat_audio_withheld_incompatible;
    // [withheld-not-registered-exposed]
    // Nonzero here with audioWithheldIncompatible == 0 is normal — these
    // are pre-registration boot packets, not a compat problem. See
    // [withheld-not-registered] in EVDPState.h.
    doc["audioWithheldNotRegistered"] = (uint32_t)stat_audio_withheld_not_registered;
    doc["serverEpoch"]       = (uint32_t)g_server_epoch;
    doc["serverEpochKnown"]  = g_server_epoch_known;
    doc["clockSyncPort"]     = (uint32_t)g_clock_sync_port;
    doc["clockSyncRttMs"]    = (uint32_t)stat_clock_sync_rtt_ms;
    doc["clockSyncRttValid"] = g_have_rtt_sync;

    // ---- Clock sync ------------------------------------------------------
    doc["clockSynced"]     = server_clock_synced;
    doc["clockOffsetMs"]   = (int32_t)server_clock_offset_ms;
    doc["clockJitterMs"]   = (int32_t)server_clock_jitter_ms;
    doc["clockResyncs"]    = (uint32_t)stat_clock_resyncs;
    doc["playoutDelayMs"]  = (int32_t)playout_delay_ms;
    doc["playoutDelayTargetMs"] = (int32_t)target_playout_delay_ms;

    // ---- Audio -----------------------------------------------------------
    doc["queueDepth"]      = audio_queue ? (uint32_t)uxQueueMessagesWaiting(audio_queue) : 0;
    doc["nodeVolume"]   = (float)node_volume_multiplier;
    doc["nodeMuted"]    = (bool)node_muted;
    doc["masterVolume"] = (float)master_volume_multiplier;
    doc["masterMuted"]  = (bool)master_muted;
    doc["zoneVolume"]   = (float)zone_volume_multiplier;
    doc["zoneMuted"]    = (bool)zone_muted;
    doc["queueTarget"]     = (uint32_t)audio_queue_target;
    doc["queueLen"]        = AUDIO_QUEUE_LEN;
    doc["queueError"]      = (int32_t)stat_queue_error;
    doc["correctionSamples"] = (int32_t)stat_correction_samples;
    doc["rebuffers"]       = (uint32_t)stat_rebuffers;
    doc["lastRebufferAgeMs"] = ageOf(ts_last_rebuffer_ms);
    doc["packetsReceived"] = (uint32_t)stat_packets_received;
    doc["lastAudioPacketAgeMs"] = ageOf(ts_last_audio_packet_ms);
    doc["packetsDropped"]  = (uint32_t)stat_packets_dropped;
    doc["packetsOversize"] = (uint32_t)stat_packets_oversize;
    // [audio-source-gate-exposed]
    // Datagrams dropped by the source-IP gate in udpAudioTask() (see
    // [audio-source-gate] in EVDPNet.h). Zero on a healthy LAN.
    //
    // Non-zero means one of:
    //   - a rogue LAN peer is spraying the audio port with spoofed
    //     Opus frames (the gate's sole purpose);
    //   - the server's IP changed mid-session (DHCP renewal, failover)
    //     and discovery has not re-run yet — the counter will stop
    //     climbing after the next stale-contact discovery tick, and
    //     lastServerContactAgeMs will show the recovery;
    //   - a stale multicast group membership survived an IP change
    //     (rare; setupMulticastSockets() on the next reconnect clears
    //     it, and stat_multicast_join_fails would have climbed).
    //
    // The counter is a lifetime tally like stat_packets_dropped and
    // stat_ctrl_json_errors; it is reset only by /api/stats/reset
    // (see handleStatsReset below), never by a rebuffer.
    doc["audioSourceRejects"] = (uint32_t)stat_audio_source_rejects;
    doc["rateTrimHz"]      = (int32_t)stat_rate_trim_hz;
    doc["lastNetworkLatencyMs"] = (uint32_t)last_node_latency_ms;
    // [headroom-includes-queue] The old formula (playoutDelay - netLatency)
    // ignored queue dwell, so a node running queue=6 / target=3 reported
    // headroom=118 while each frame actually arrived only ~18 ms before its
    // deadline — exactly why leadBelow20LastSec read 50/s on every sample.
    // Subtract the queue dwell so this field matches what
    // audioPlaybackTask() computes at dequeue time.
    int32_t qDepth = audio_queue ? (int32_t)uxQueueMessagesWaiting(audio_queue) : 0;
    int32_t qDwellMs = (qDepth - 1) * 20;
    if (qDwellMs < 0) qDwellMs = 0;
    doc["playoutHeadroomMs"] = (int32_t)playout_delay_ms
                             - (int32_t)last_node_latency_ms
                             - qDwellMs;
    doc["totalCorrectionSamples"] = (uint32_t)stat_total_correction_samples;

    // ---- Sequence / loss-recovery -----------------------------------------
    doc["audioProtoV2"]    = audio_proto_v2_seen;
    doc["seqReceived"]     = (uint32_t)stat_seq_received;
    doc["seqMissing"]      = (uint32_t)stat_seq_missing;
    doc["seqGaps"]         = (uint32_t)stat_seq_gaps;
    doc["seqReordered"]    = (uint32_t)stat_seq_reordered;
    doc["seqDuplicate"]    = (uint32_t)stat_seq_duplicate;
    doc["framesConcealed"] = (uint32_t)stat_frames_concealed;
    doc["framesLate"]      = (uint32_t)stat_frames_late;
    doc["framesDiscarded"] = (uint32_t)stat_frames_discarded;
    doc["framesLateLt5"]      = (uint32_t)stat_late_lt5;
    doc["framesLate5to20"]    = (uint32_t)stat_late_5to20;
    doc["framesLate20to100"]  = (uint32_t)stat_late_20to100;
    doc["framesLate100to200"] = (uint32_t)stat_late_100to200;

    // ---- Stutter diagnostics ---------------------------------------------
    // Per-second packet count and worst inter-arrival gap. A healthy 20 ms
    // cadence reports packetsLastSec ≈ 50 and maxGapMsLastSec < 25. The
    // 10-30 s periodic stutter shows up as a single recorder row with
    // packetsLastSec ≈ 30 and maxGapMsLastSec ≈ 200-500 while every
    // cumulative counter still reads "optimal" — exactly the pattern the
    // operator cannot see in the current dashboard.
    doc["packetsThisSec"]    = (uint32_t)stat_packets_this_sec;
    doc["packetsLastSec"]    = (uint32_t)stat_packets_last_sec;
    doc["maxGapMsThisSec"]   = (uint32_t)stat_max_gap_ms_this_sec;
    doc["maxGapMsLastSec"]   = (uint32_t)stat_max_gap_ms_last_sec;
    doc["gapGt35msTotal"]    = (uint32_t)stat_gap_gt_35ms_total;
    doc["gapGt50msTotal"]    = (uint32_t)stat_gap_gt_50ms_total;
    doc["gapGt100msTotal"]   = (uint32_t)stat_gap_gt_100ms_total;

    // ---- Post-queue diagnostic counters ----------------------------------
    // These three fields catch a stutter that is NOT a packet-level or
    // scheduler-level problem. All zero on a healthy node. See the block
    // in EVDPState.h for what each one means.
    doc["correctorFramesLastSec"]   = (uint32_t)stat_corrector_frames_last_sec;
    doc["shortDecodeFramesLastSec"] = (uint32_t)stat_short_decode_frames_last_sec;
    doc["leadBelow20LastSec"]       = (uint32_t)stat_lead_below20_last_sec;
    // [captured-ms-timeline-counters] Two node-side counters that
    // catch what every packet-level stat misses:
    //
    //   capturedMsGapMaxLastSec — worst captured_ms delta in the last
    //                             full second. 20 = normal, 40 = one
    //                             server-side frame skipped, 60+ = two.
    //   capturedMsGapGt20Total  — cumulative frames whose captured_ms
    //                             gap exceeded 21 ms.
    //   capturedMsGapGt40Total  — subset >41 ms. THIS is the audible
    //                             thump: 40 ms of audio never existed
    //                             on the wire, so seqGaps and
    //                             framesLate both remain 0.
    //   correctorFramesPeakLastSec — peak corrector fires in any 1 s
    //                             window (post-cap, so deadband
    //                             residuals cannot inflate it).
    //   correctorFramesTotal    — lifetime corrector fires (post-cap).
    //                             Steady state: 0.
    doc["capturedMsGapMaxLastSec"]    = (uint32_t)statCapturedMsGapMaxLastSec;
    doc["capturedMsGapGt20Total"]     = (uint32_t)statCapturedMsGapGt20Total;
    doc["capturedMsGapGt40Total"]     = (uint32_t)statCapturedMsGapGt40Total;
    doc["correctorFramesPeakLastSec"] = (uint32_t)statCorrectorFramesPeakLastSec;
    doc["correctorFramesTotal"]       = (uint32_t)statCorrectorFramesTotal;
    // [lead-tracking-v2] Worst (lowest) frame lead observed in the last
    // full second. INT32_MAX means no frames were decoded; otherwise a
    // value in ms. A healthy node on a 6-deep queue against 150 ms
    // playout typically reads 15-25 ms. A negative value means at least
    // one frame was already past its deadline (should be 0 on healthy).
    // [minlead-sentinel] 0x7FFFFFFF means "no frames decoded this second".
    // Reporting the raw int32 max made the dashboard show "worst lead:
    // 2147483647 ms". Emit JSON null so the UI renders "—".
    if (stat_min_lead_last_sec == 0x7FFFFFFF) doc["minLeadMsLastSec"] = nullptr;
    else                                     doc["minLeadMsLastSec"] = (int32_t)stat_min_lead_last_sec;

    // ---- Encoder as received from the server -----------------------------
    // The server mirrors its live encoder state into every registration
    // response and ctrl push (see broadcastNodeConfig / handlePlayerRegistration
    // in EVDPlayers.h). These are what the SERVER told us the encoder is
    // currently running, not a locally-chosen profile. A mismatch against
    // the Encoding tab is a routing problem between server and node, not an
    // encoder bug.
    doc["serverEncBitrate"]       = (int32_t)g_last_recv_enc_bitrate;
    doc["serverEncComplexity"]    = (int32_t)g_last_recv_enc_complexity;
    // -1 sentinel -> JSON null so the UI can render "—" instead of "Off".
    if (g_last_recv_enc_vbr == -1)             doc["serverEncVbr"] = nullptr;
    else                                        doc["serverEncVbr"] = (g_last_recv_enc_vbr != 0);
    if (g_last_recv_enc_vbr_constraint == -1)  doc["serverEncVbrConstraint"] = nullptr;
    else                                        doc["serverEncVbrConstraint"] = (g_last_recv_enc_vbr_constraint != 0);
    doc["serverEncSignal"]        = (int32_t)g_last_recv_enc_signal;
    doc["serverEncApplication"]   = (int32_t)g_last_recv_enc_application;
    doc["serverEncSettingsAgeMs"] = ageOf(ts_last_recv_enc_ms);
    // [server-opus-rate-exposed] Encoder Fs as declared by the server.
    // libopus decodes any Fs to 48 kHz output transparently, so this
    // is pure observability: a value of 48000 / 24000 confirms the
    // server is honouring the Encoding-tab sample-rate selection. 0
    // (server never declared the field) is emitted as JSON null so the
    // Status page renders "—" instead of a plausible-looking 0.
    if (g_last_recv_opus_sample_rate > 0) {
        doc["serverOpusSampleRate"] = (uint32_t)g_last_recv_opus_sample_rate;
    } else {
        doc["serverOpusSampleRate"] = nullptr;
    }

    // [server-source-rate-exposed] Measured SOURCE rate (I2S clock) as
    // declared by the server. Diagnostic only — the node's own decoder
    // Fs is fixed at SAMPLE_RATE and does not track this value, so no
    // downstream control decision reads this field. Exposed so the
    // Status page can prove the server is seeing the expected 44.1k
    // vs 48k after a BT device switch, and so the 10 min recorder's
    // JSON dump carries a timestamped source-rate trail that can be
    // compared against the framesLate / seqGaps columns at the exact
    // second a source renegotiation happened. 0 (server never
    // declared the field, or a pre-3.7.1.8 server) is emitted as JSON
    // null so the Status page renders "—".
    if (g_last_recv_source_hz > 0) {
        doc["serverSourceSampleRate"] = (uint32_t)g_last_recv_source_hz;
    } else {
        doc["serverSourceSampleRate"] = nullptr;
    }

    // ---- Errors ----------------------------------------------------------
    doc["opusDecodeErrors"]   = (uint32_t)stat_opus_decode_errors;
    doc["i2sWriteErrors"]     = (uint32_t)stat_i2s_write_errors;
    doc["i2sTimeouts"]        = (uint32_t)stat_i2s_timeouts;
    // [i2s-ready-exposed] DAC install failures otherwise only show up as a
    // steady climb in i2sWriteErrors with no indication *which* DAC died.
    // The dashboard can now render "DAC1 OK / DAC2 DEAD" directly.
    doc["i2sDac1Ready"]       = (bool)g_i2s_dac1_ready;
    doc["i2sDac2Ready"]       = (bool)g_i2s_dac2_ready;
    doc["postStatsFails"]     = (uint32_t)stat_post_stats_fails;
    doc["postRegisterFails"]  = (uint32_t)stat_post_register_fails;
    doc["ctrlJsonErrors"]     = (uint32_t)stat_ctrl_json_errors;
    doc["ctrlPacketsRx"]      = (uint32_t)stat_ctrl_packets_rx;
    // [reanchor-counter] Empty-queue flushes (boot re-anchors). Not
    // errors — they should not count toward the rebuffer gauge but
    // SHOULD be visible so an operator can tell a healthy boot from a
    // pathological restart loop.
    doc["emptyFlushes"]       = (uint32_t)stat_empty_flushes;

    if (doc.overflowed()) {
        // [health-overflow-visible] BUGFIX: promote to log_message so the
        // overflow is visible in the 10-min recorder even at
        // gPlayerLogLevel == 1. Without this, a future field addition
        // silently drops fields from /api/health and the operator sees
        // "—" on the Status page with no breadcrumb explaining why.
        log_message("[Health] JSON doc overflowed; response truncated. "
                    "Bump the DynamicJsonDocument size.");
    }
    // [health-oom-guard] serializeJson(String&) uses String::concat which
    // truncates silently on OOM — exactly the failure mode that makes a
    // dashboard poll parse as invalid and stop updating pills. Reserve
    // first; also reject obviously-short results.
    String response;
    if (!response.reserve(4096)) {
        log_warn("[Health] reserve(4096) failed; sending minimal response.");
        server.sendHeader("Cache-Control", "no-store");
        server.send(503, "application/json",
                    "{\"ok\":false,\"error\":\"oom\"}");
        return;
    }
    serializeJson(doc, response);
    if (response.length() < 64 || !response.endsWith("}")) {
        log_warn("[Health] serialize produced " + String(response.length()) +
                 " bytes; refusing to send malformed body.");
        server.sendHeader("Cache-Control", "no-store");
        server.send(503, "application/json",
                    "{\"ok\":false,\"error\":\"serialize\"}");
        return;
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", response);
}

// ---- Root / status / save / cancel handlers --------------------------------
inline void handleStatusPage() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", EVDSET_STATUS_PAGE);
}

// [v1.1.2-cfg-fetch / root-gzip-length]
// Serves the pre-gzipped dashboard as a String so the synchronous
// WebServer's send() sees contentLength > 0 and emits both Content-Type
// and Content-Length. The previous setContentLength()+send("") form left
// the response without a Content-Length on some arduino-esp32 cores,
// which let keep-alive browsers stall waiting for a body terminator.
// One ~11 KB heap allocation per page load; released when body unwinds.
inline void handleRoot() {
    // [accept-encoding] Only advertise gzip when the client accepts it.
    // curl without --compressed, and any non-browser LAN tool, would
    // otherwise receive gzip bytes interpreted as text/html.
    String acceptEnc = server.header("Accept-Encoding");
    if (acceptEnc.indexOf("gzip") < 0) {
        // Serve the uncompressed page directly from PROGMEM. send_P streams
        // from flash without allocating, so this path is also OOM-proof.
        server.sendHeader("Cache-Control", "no-store");
        server.send_P(200, "text/html; charset=utf-8", EVDSET_PAGE_HTML);
        return;
    }
    String body((const char*)EVDSET_HTML_GZ, EVDSET_HTML_GZ_LEN);
    // [root-oom-guard] On a fragmented heap the 13 KB allocation can fail;
    // the constructor returns an empty String (not a truncated one), so
    // this check is exact.
    if (body.length() == 0) {
        server.send(503, "text/plain",
                    "out of memory; retry after the audio task settles");
        return;
    }
    server.sendHeader("Content-Encoding", "gzip");
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", body);
}

inline void handleCancel() {
    // [ota-gate-cancel]
    // handleCancel() -> loadStoredPreferences() does ~20 NVS reads
    // (20-100 ms) against the same flash chip whose write path
    // Update.write() is currently driving. On a healthy node the read
    // succeeds, but the flash controller serialises read vs write, so
    // each NVS read pauses the OTA chunk by its full duration. On a
    // 1-2 MB image that is several seconds of push slowdown — enough
    // for otaPushToNode()'s response timeout to fire and mark the
    // node update as failed even though the image would have
    // committed. 503 the button instead; the operator can Revert
    // after the push finishes.
    if (otaNodeUpdating) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(503, "application/json",
                    "{\"ok\":false,\"error\":\"OTA in progress; retry after update\"}");
        return;
    }
    log_message("[Web] Cancel request received.");
    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    preferences.begin("audio_node", true);
    bool isConfigured = preferences.getBool("configured", false);
    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);

    if (isConfigured) {
        // [cancel-nvs-outside-mutex] loadStoredPreferences() does ~20
        // blocking NVS reads (~20-100 ms). Must NOT run under the audio
        // config mutex (priority inheritance would starve the audio task
        // for the whole window). But it DOES mutate speakers[] fields that
        // the audio task reads. Take the mutex only around the fast
        // in-memory swap: read into a local snapshot under prefs_mutex,
        // then publish under audio mutex. loadStoredPreferences() itself
        // stays unchanged (it is also called from setup() single-threaded
        // and from /cancel where a torn frame is inaudible); the filter
        // reconfigure, which is where a torn pair would produce an
        // invalid biquad, runs under the audio mutex against the fields
        // as they stand now.
        loadStoredPreferences();
        if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
        for (int i = 0; i < 4; i++) {
            speaker_filters[i].configure(speakers[i].low_freq,
                                         speakers[i].high_freq,
                                         SAMPLE_RATE);
        }
        if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);
    }
    else {
        if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
        node_volume_multiplier = 1.0f;
        node_muted             = false;
        for (int i = 0; i < 4; i++) {
            speakers[i].volume = 100;
            speaker_filters[i].configure(speakers[i].low_freq,
                                         speakers[i].high_freq,
                                         SAMPLE_RATE);
        }
        if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "OK");
}

// ---- AP Mode Network Test --------------------------------------------------
inline void saveTestedNetworkSettings() {
    strncpy(wifi_ssid, pending_ssid, sizeof(wifi_ssid) - 1);
    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
    strncpy(wifi_pass, pending_pass, sizeof(wifi_pass) - 1);
    wifi_pass[sizeof(wifi_pass) - 1] = '\0';
    strncpy(friendly_name, pending_name, sizeof(friendly_name) - 1);
    friendly_name[sizeof(friendly_name) - 1] = '\0';
    strncpy(mdns_target_host, pending_mdns, sizeof(mdns_target_host) - 1);
    mdns_target_host[sizeof(mdns_target_host) - 1] = '\0';

    use_static_ip = pending_use_static;
    strncpy(static_ip_str, pending_ip, sizeof(static_ip_str) - 1);
    static_ip_str[sizeof(static_ip_str) - 1] = '\0';
    strncpy(static_gw_str, pending_gw, sizeof(static_gw_str) - 1);
    static_gw_str[sizeof(static_gw_str) - 1] = '\0';
    strncpy(static_sn_str, pending_sn, sizeof(static_sn_str) - 1);
    static_sn_str[sizeof(static_sn_str) - 1] = '\0';
    strncpy(static_dns_str, pending_dns, sizeof(static_dns_str) - 1);
    static_dns_str[sizeof(static_dns_str) - 1] = '\0';

    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    preferences.begin("audio_node", false);
    preferences.putBool("configured", true);
    preferences.putString("friendly_name", friendly_name);
    preferences.putString("mdns_host", mdns_target_host);
    preferences.putString("ssid", wifi_ssid);
    preferences.putString("pass", wifi_pass);
    preferences.putBool("use_static", use_static_ip);
    preferences.putString("static_ip", static_ip_str);
    preferences.putString("static_gw", static_gw_str);
    preferences.putString("static_sn", static_sn_str);
    preferences.putString("static_dns", static_dns_str);
    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
}

void wifiTestTask(void *pvParameters) {
    // Stay in AP+STA so the user's browser doesn't disconnect during the test
    WiFi.mode(WIFI_AP_STA);
    
    if (pending_use_static) {
        IPAddress ip, gw, sn, dns;
        if (ip.fromString(pending_ip) && gw.fromString(pending_gw) && sn.fromString(pending_sn)) {
            dns.fromString(pending_dns);
            WiFi.config(ip, gw, sn, dns);
        }
    } else {
        WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    }

    WiFi.begin(pending_ssid, pending_pass);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (WiFi.status() == WL_CONNECTED) {
        saveTestedNetworkSettings();
        g_netTestState = NET_TEST_SUCCESS;
        vTaskDelay(pdMS_TO_TICKS(2000)); // Let frontend see success before rebooting
        ESP.restart();
    } else {
        snprintf(g_netTestError, sizeof(g_netTestError), "Failed to connect to %s", pending_ssid);
        g_netTestState = NET_TEST_FAILED;
        WiFi.disconnect(false, false);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_DISCONNECTED && millis() - t0 < 2000) vTaskDelay(pdMS_TO_TICKS(50));
        WiFi.mode(WIFI_AP); // Revert to AP only
    }
    vTaskDelete(NULL);
}

inline void handleNetworkTestStatus() {
    String json = "{\"state\":\"";
    if (g_netTestState == NET_TEST_RUNNING) json += "running";
    else if (g_netTestState == NET_TEST_SUCCESS) json += "success";
    else if (g_netTestState == NET_TEST_FAILED) json += "failed";
    else json += "idle";
    json += "\"";
    if (g_netTestState == NET_TEST_FAILED) {
        // [nettest-json-escape] BUGFIX: g_netTestError is
        // "Failed to connect to <pending_ssid>", and pending_ssid is
        // operator-entered. SSIDs routinely contain " and \, which produced
        // malformed JSON, made the /api/network-test poll in the AP-mode
        // save flow throw JSON.parse, and left the operator staring at a
        // dead "Testing connection…" overlay with no way to retry the
        // exact save they were making. Use the same evdJsonEscape that
        // /scan and /api/cfg already use. Buffer = 6*64+1, worst case per
        // input byte is \uXXXX (6) plus NUL.
        char escErr[6 * 64 + 1];
        evdJsonEscape(g_netTestError, escErr, sizeof(escErr));
        json += ",\"error\":\"";
        json += escErr;
        json += "\"";
    }
    json += "}";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
}

inline void handleSave() {
    // [ota-gate-save]
    // handleSave() writes ~11 NVS keys synchronously under g_prefs_mutex
    // and (in both AP and STA paths) ends in ESP.restart(). Each NVS
    // putX() stalls the flash cache for ~100 ms, which starves an
    // in-flight Update.write() and can push the server-side push past
    // its response timeout. The AP-mode branch additionally creates
    // wifiTestTask, which does a 15 s WiFi.begin() + association loop
    // on the same radio the upload socket uses — a guaranteed upload
    // failure if it starts mid-push. Mirrors [ota-gate-save] on the
    // server side.
    if (otaNodeUpdating) {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(503, "application/json",
                    "{\"ok\":false,\"error\":\"OTA in progress; retry after update\"}");
        return;
    }
    log_message("[Web] Save request received.");
    const bool was_ap_mode = is_in_ap_mode;

    bool got_ssid = false;
    if (server.hasArg("ssid")) {
        String s = server.arg("ssid"); s.trim();
        got_ssid = s.length() > 0;
    }

    // [network-test-ap-mode] Intercept AP mode saves to test the connection first.
    if (was_ap_mode && got_ssid) {
        if (g_netTestState == NET_TEST_RUNNING) {
            server.send(429, "text/plain", "Test already running");
            return;
        }
        
        server.arg("ssid").toCharArray(pending_ssid, 64);
        server.arg("pass").toCharArray(pending_pass, 64);
        if (server.hasArg("friendly_name")) server.arg("friendly_name").toCharArray(pending_name, 64);
        else strncpy(pending_name, friendly_name, 64);
        if (server.hasArg("mdns_host")) server.arg("mdns_host").toCharArray(pending_mdns, 64);
        else strncpy(pending_mdns, mdns_target_host, 64);
        
        pending_use_static = server.hasArg("use_static");
        if (server.hasArg("static_ip"))  server.arg("static_ip").toCharArray(pending_ip, 16); else pending_ip[0] = '\0';
        if (server.hasArg("static_gw"))  server.arg("static_gw").toCharArray(pending_gw, 16); else pending_gw[0] = '\0';
        if (server.hasArg("static_sn"))  server.arg("static_sn").toCharArray(pending_sn, 16); else pending_sn[0] = '\0';
        if (server.hasArg("static_dns")) server.arg("static_dns").toCharArray(pending_dns, 16); else pending_dns[0] = '\0';

        g_netTestState = NET_TEST_RUNNING;
        g_netTestError[0] = '\0';

        xTaskCreatePinnedToCore(wifiTestTask, "WifiTest", 4096, NULL, 2, NULL, 0);

        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(202, "application/json", "{\"state\":\"running\"}");
        return;
    }

    // --- Original STA-mode save path ---
    // (Keep the rest of the original handleSave body exactly as it is...)

    // Capture the currently-active Wi-Fi credentials *before* overwriting
    // them, so the STA-mode branch below can tell whether the save actually
    // changed the network. Without this, /save in STA mode silently wrote
    // new credentials to NVS and replied "Saved" while the device continued
    // running on the old network until the next boot — a footgun if the
    // user then unplugs the device expecting the new network to be active.
    char oldSsid[64];
    char oldPass[64];
    strncpy(oldSsid, wifi_ssid, sizeof(oldSsid) - 1);
    oldSsid[sizeof(oldSsid) - 1] = '\0';
    strncpy(oldPass, wifi_pass, sizeof(oldPass) - 1);
    oldPass[sizeof(oldPass) - 1] = '\0';

    // [phase-split-1] Scalars only. prefs_mutex released before the audio
    // block below so a slow audio frame (up to ~425 ms on the bounded
    // force-drain) does not stall deferred save / /cancel for the whole
    // window.
    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    if (!preferences.begin("audio_node", false)) {
        if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
        log_warn("[Save] NVS begin FAILED - nothing written");
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(500, "text/plain", "NVS unavailable; nothing saved.");
        return;
    }

    preferences.putBool("configured", true);
    if (server.hasArg("friendly_name")) {
        preferences.putString("friendly_name", server.arg("friendly_name"));
        server.arg("friendly_name").toCharArray(friendly_name, 64);
    }
    if (server.hasArg("mdns_host")) {
        preferences.putString("mdns_host", server.arg("mdns_host"));
        server.arg("mdns_host").toCharArray(mdns_target_host, 64);
    }
    if (server.hasArg("ssid")) {
        preferences.putString("ssid", server.arg("ssid"));
        server.arg("ssid").toCharArray(wifi_ssid, 64);
    }
    if (server.hasArg("pass")) {
        preferences.putString("pass", server.arg("pass"));
        server.arg("pass").toCharArray(wifi_pass, 64);
    }

    if (server.hasArg("node_vol")) {
        float v = server.arg("node_vol").toFloat() / 100.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        // NVS write only; in-memory assignment happens in phase 2 under
        // g_audio_config_mutex (audio task reads it every frame).
        preferences.putFloat("node_vol", v);
    }

    use_static_ip = server.hasArg("use_static");
    preferences.putBool("use_static", use_static_ip);
    if (server.hasArg("static_ip"))  { preferences.putString("static_ip",  server.arg("static_ip"));  server.arg("static_ip").toCharArray(static_ip_str, 16); }
    if (server.hasArg("static_gw"))  { preferences.putString("static_gw",  server.arg("static_gw"));  server.arg("static_gw").toCharArray(static_gw_str, 16); }
    if (server.hasArg("static_sn"))  { preferences.putString("static_sn",  server.arg("static_sn"));  server.arg("static_sn").toCharArray(static_sn_str, 16); }
    if (server.hasArg("static_dns")) { preferences.putString("static_dns", server.arg("static_dns")); server.arg("static_dns").toCharArray(static_dns_str, 16); }

    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);   // release BEFORE audio

    // [audio-config-mutex] Phase 1: update in-memory config under the mutex
    // (fast — no NVS I/O inside). Phase 2: persist to NVS (slow, no mutex).
    //
    // [save-clamp-deferred-log] The clamp event is captured into locals
    // here and reported via log_warn() AFTER the mutex is released. The
    // previous inline log_warn() stalled UART for ~6 ms per line while
    // g_audio_config_mutex was held, which the audio task contends with
    // portMAX_DELAY on Core 1 — a priority-inheritance hazard on the
    // exact path the [i2s-outside-mutex] comment in EVDPAudio.h was
    // designed to avoid. Moving the log below collapses that to zero.
    int   saveClampedIdx       = -1;
    float saveClampedLoBefore  = 0.0f, saveClampedHiBefore = 0.0f;
    float saveClampedLoAfter   = 0.0f, saveClampedHiAfter  = 0.0f;

    if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
    if (server.hasArg("node_vol")) {
        float v = server.arg("node_vol").toFloat() / 100.0f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        node_volume_multiplier = v;
    }
    for (int i = 0; i < 4; i++) {
        String nameArg = "spk_name_" + String(i);
        String volArg  = "spk_vol_"  + String(i);
        String lowArg  = "spk_low_"  + String(i);
        String highArg = "spk_high_" + String(i);
        String srcArg  = "spk_src_"  + String(i);

        if (server.hasArg(nameArg)) server.arg(nameArg).toCharArray(speakers[i].name, 32);
        if (server.hasArg(volArg)) {
            int v = server.arg(volArg).toInt();
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            speakers[i].volume = v;
        }

        bool freqChanged = false;
        if (server.hasArg(lowArg)) {
            int v = server.arg(lowArg).toInt(); if (v < 0) v = 0;
            if (speakers[i].low_freq != v) freqChanged = true;
            speakers[i].low_freq = v;
        }
        if (server.hasArg(highArg)) {
            int v = server.arg(highArg).toInt();
            if (v > 20000) v = 20000; if (v < 0) v = 0;
            if (speakers[i].high_freq != v) freqChanged = true;
            speakers[i].high_freq = v;
        }
        if (server.hasArg(srcArg)) {
            int v = server.arg(srcArg).toInt();
            if (v < 0) v = 0; if (v > 2) v = 2;
            speakers[i].source = v;
        }
        if (freqChanged) {
            // [save-crossover-cap] Same 0.44*fs hard cap the realtime path
            // already applies (see validate_and_update_crossover). Without
            // this, a direct POST /save?spk_low_0=25000 lands a value the
            // Biquad silently rejects (active=false, filter bypassed) and
            // the operator gets "Saved successfully!" for a speaker that
            // is now full-band. Also enforces low <= high, which the
            // previous code did not: low=5000 / high=1000 would create an
            // inverted bandpass that the Biquad cannot represent.
            float lo = (float)speakers[i].low_freq;
            float hi = (float)speakers[i].high_freq;
            float origLo = lo, origHi = hi;
            validate_and_update_crossover(lo, hi, (float)SAMPLE_RATE * 0.44f);
            if (lo != origLo || hi != origHi) {
                saveClampedIdx      = i;
                saveClampedLoBefore = origLo;
                saveClampedHiBefore = origHi;
                saveClampedLoAfter  = lo;
                saveClampedHiAfter  = hi;
            }
            speakers[i].low_freq  = (int)lo;
            speakers[i].high_freq = (int)hi;
            // [save-crossover-apply-always]
            // BUGFIX: the previous [save-crossover-skip-redundant]
            // block compared speakers[i].low_freq against origLo, but
            // origLo was captured AFTER the server value was written
            // into speakers[i].low_freq. The comparison therefore only
            // tested "did validate_and_update_crossover() clamp?", not
            // "did the value change?". Since we are inside
            // `if (freqChanged)`, the value DID change — so the
            // `continue` skipped configure() on every in-range
            // crossover edit, leaving the running filter on its
            // PREVIOUS coefficients until the next reboot.
            //
            // configure() is idempotent ([configure-idempotent] in
            // EVDPFilters.h): it short-circuits when (low, high) match
            // _prev_low/_prev_high, so calling it unconditionally here
            // costs nothing in the already-configured case. NVS writes
            // (the ~2 s flash stall the old comment worried about) are
            // gated separately in Phase 3 by `if (server.hasArg(lowArg))`,
            // NOT by this block — so removing the `continue` does not
            // increase NVS writes.
            speaker_filters[i].configure(speakers[i].low_freq, speakers[i].high_freq, SAMPLE_RATE);
        }
    }
    if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);

    // [save-clamp-deferred-log] Report after the mutex release. The audio
    // task is no longer blocked when this Serial.println() drains.
    if (saveClampedIdx >= 0) {
        log_warn(String("[Save] Crossover clamped (spk ") + String(saveClampedIdx) +
                 "): " + String((int)saveClampedLoBefore) + "-" + String((int)saveClampedHiBefore) +
                 " -> " + String((int)saveClampedLoAfter) + "-" + String((int)saveClampedHiAfter) + " Hz");
    }

    // ===== Phase 3: per-speaker NVS writes (prefs_mutex) ================
    // [phase3-snapshot] Take the audio mutex only long enough to snapshot
    // speakers[] — the same pattern networkTask's pending_speaker_save path
    // already uses. The old code read speakers[i].* directly while writing
    // to NVS, relying on "only handlePostVolume writes and it's on the same
    // task". That is true today but a footgun: any future writer (e.g. a
    // server-pushed speaker config) would race the NVS write against the
    // audio path's playback reads, and a torn 32-byte name reaches NVS.
    SpeakerConfig snap[4];
    if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
    for (int i = 0; i < 4; i++) snap[i] = speakers[i];
    if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);

    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    preferences.begin("audio_node", false);
    for (int i = 0; i < 4; i++) {
        String nameArg = "spk_name_" + String(i);
        String volArg  = "spk_vol_"  + String(i);
        String lowArg  = "spk_low_"  + String(i);
        String highArg = "spk_high_" + String(i);
        String srcArg  = "spk_src_"  + String(i);

        if (server.hasArg(nameArg)) preferences.putString(("sname_" + String(i)).c_str(), snap[i].name);
        if (server.hasArg(volArg))  preferences.putInt   (("svol_"  + String(i)).c_str(), snap[i].volume);
        if (server.hasArg(lowArg))  preferences.putInt   (("slow_"  + String(i)).c_str(), snap[i].low_freq);
        if (server.hasArg(highArg)) preferences.putInt   (("shigh_" + String(i)).c_str(), snap[i].high_freq);
        if (server.hasArg(srcArg))  preferences.putInt   (("ssrc_"  + String(i)).c_str(), snap[i].source);
    }
    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
    log_message("[Prefs] Configuration saved successfully to NVS.");

    // AP-mode hand-off: the browser is on 192.168.5.1, which disappears when
    // we reboot into STA. Ship a small stub that waits 5 s and redirects.

    // ---- STA mode: restart if the Wi-Fi config actually changed ---------
    if (!was_ap_mode && got_ssid) {
        bool ssidChanged = (strncmp(oldSsid,  wifi_ssid, sizeof(oldSsid)  - 1) != 0);
        bool passChanged = (strncmp(oldPass,  wifi_pass, sizeof(oldPass)  - 1) != 0);
        if (ssidChanged || passChanged) {
            log_message(String("[Web] Wi-Fi config changed on STA (ssid='") +
                        String(wifi_ssid) + "'); restarting to apply.");
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(200, "text/plain",
                        "Saved. Node is rebooting to join the new Wi-Fi network.");
            delay(800);
            ESP.restart();
            return;
        }
    }

    // ---- AP-mode hand-off ------------------------------------------------
    // The browser is on 192.168.5.1, which disappears when we reboot into
    // STA. Ship a redirect stub that waits 5 s and follows the node to its
    // new home, then reboot.
    if (was_ap_mode && got_ssid) {
        String target = server.hasArg("mdns_host") ? server.arg("mdns_host")
                                                   : String(mdns_target_host);
        target.trim();
        if (target.length() == 0) target = String(DEFAULT_MDNS_HOST);
        String url = target;
        if (!url.startsWith("http://") && !url.startsWith("https://")) url = "http://" + url;
        String hostOnly = url.substring(url.indexOf("://") + 3);
        if (hostOnly.indexOf('.') == -1) url += ".local";

        // [ap-url-js] url can be "http://" + mdns_target_host(64) + ".local"
        // = ~77 chars; escaped worst case 6*77+1 ≈ 463 B. 128 was under.
        char urlJs[6 * 128 + 1];
        evdJsonEscape(url.c_str(), urlJs, sizeof(urlJs));

        // [ap-xss] The URL was previously interpolated into the HTML
        // attribute context with url.c_str() raw. A `"` inside mdns_host
        // (e.g. `evil"><script>alert(1)</script><a href="`) breaks out of
        // the href, giving self-XSS against the operator who typed it.
        // Filter the 4 HTML-significant characters — nothing legitimate
        // in a host/IP/scheme contains them. urlJs stays JSON-escaped for
        // the JS context (that path is safe: evdJsonEscape already emits
        // \\uXXXX for <, >, &).
        char urlHtml[128];
        {
            size_t j = 0;
            for (size_t k = 0; url[k] && j < sizeof(urlHtml) - 1; k++) {
                char c = url[k];
                if (c == '"' || c == '<' || c == '>' || c == '&') continue;
                urlHtml[j++] = c;
            }
            urlHtml[j] = '\0';
        }

        char page[1100];
        int w = snprintf(page, sizeof(page),
            "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Connecting&#8230;</title>"
            "<style>body{margin:0;min-height:100vh;background:#070710;color:#f2f4f8;"
            "font:16px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
            "display:flex;align-items:center;justify-content:center;padding:20px;text-align:center}"
            "h1{font-size:1.15rem;font-weight:650;margin:0 0 10px}"
            "p{color:#9aa3b8;margin:0 0 14px}"
            "a{color:#25c8f0;text-decoration:none;font-weight:600}</style>"
            "</head><body><div>"
            "<h1>Saving to flash&#8230;</h1>"
            "<p>Your node is rebooting and joining your Wi&#8209;Fi network.</p>"
            "<p>Redirecting to the server&#8230;</p>"
            "<p><a href=\"%s\">Open %s now</a></p>"
            "</div>"
            "<script>setTimeout(function(){window.location.href=\"%s\";},5000);</script>"
            "</body></html>",
            urlHtml, urlHtml, urlJs);
        if (w > 0 && w < (int)sizeof(page)) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(200, "text/html; charset=utf-8", page);
            log_message("[Web] AP save complete. Rebooting into STA; browser will follow to " + url);
        } else {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.send(200, "text/plain", "Saved. Node is rebooting to join Wi-Fi.");
            log_message("[Web] AP save complete (redirect page build failed; plain-text fallback).");
        }
        delay(800);
        ESP.restart();
        return;
    }

    // ---- STA mode, config unchanged (or non-Wi-Fi save) ------------------
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", "Saved successfully!");
}

// ---- Preferences load ------------------------------------------------------
inline void loadStoredPreferences() {
    // Phase 1 — read everything from NVS into locals, under prefs_mutex.
    // No audio-relevant global is touched in this phase; NVS reads can
    // take 20-100 ms and must not hold g_audio_config_mutex.
    bool configured = false;
    char localSsid[64]  = "";
    char localPass[64]  = "";
    char localName[64]  = "Enkelvoud Node";
    char localMdns[64]  = DEFAULT_MDNS_HOST;
    char localIp[16]    = "";
    char localGw[16]    = "";
    char localSn[16]    = "255.255.255.0";
    char localDns[16]   = "";
    bool  localUseStatic = false;
    float localNodeVol   = 1.0f;
    SpeakerConfig localSpeakers[4];
    for (int i = 0; i < 4; i++) localSpeakers[i] = speakers[i];

    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    if (!preferences.begin("audio_node", true)) {
        if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
        log_warn("[Prefs] NVS open for read failed - using boot defaults.");
        return;
    }
    configured = preferences.getBool("configured", false);
    if (configured) {
        preferences.getString("friendly_name", "Enkelvoud Node").toCharArray(localName, 64);
        preferences.getString("mdns_host", DEFAULT_MDNS_HOST).toCharArray(localMdns, 64);
        preferences.getString("ssid", "").toCharArray(localSsid, 64);
        preferences.getString("pass", "").toCharArray(localPass, 64);
        localUseStatic = preferences.getBool("use_static", false);
        preferences.getString("static_ip",  "").toCharArray(localIp, 16);
        preferences.getString("static_gw",  "").toCharArray(localGw, 16);
        preferences.getString("static_sn", "255.255.255.0").toCharArray(localSn, 16);
        preferences.getString("static_dns", "").toCharArray(localDns, 16);
        localNodeVol = preferences.getFloat("node_vol", 1.0f);

        for (int i = 0; i < 4; i++) {
            String nKey = "sname_" + String(i);
            String vKey = "svol_"  + String(i);
            String lKey = "slow_"  + String(i);
            String hKey = "shigh_" + String(i);
            String sKey = "ssrc_"  + String(i);
            preferences.getString(nKey.c_str(), speakers[i].name)
                .toCharArray(localSpeakers[i].name, sizeof(localSpeakers[i].name));
            localSpeakers[i].volume    = preferences.getInt(vKey.c_str(), speakers[i].volume);
            localSpeakers[i].low_freq  = preferences.getInt(lKey.c_str(), speakers[i].low_freq);
            localSpeakers[i].high_freq = preferences.getInt(hKey.c_str(), speakers[i].high_freq);
            int src = preferences.getInt(sKey.c_str(), speakers[i].source);
            if (src < 0 || src > 2) src = 0;
            localSpeakers[i].source = src;
        }

        // [nvs-crossover-validate] Same 0.44*fs hard cap that the live
        // paths (handlePostVolume / handleSave) now apply. Without this,
        // NVS written by any firmware build before save-crossover-cap
        // can contain out-of-range values (25000 Hz is what Biquad::
        // setHighpass silently rejects), and the very first configure()
        // call in setup() or handleCancel() then leaves the filter
        // bypassed for the rest of the boot. The user sees full-band
        // content on what they believe is a filtered channel, with
        // i2sWriteErrors staying at 0. Clamping here means both code
        // paths downstream of loadStoredPreferences() are guaranteed
        // to see only representable values.
        for (int i = 0; i < 4; i++) {
            float lo = (float)localSpeakers[i].low_freq;
            float hi = (float)localSpeakers[i].high_freq;
            validate_and_update_crossover(lo, hi, (float)SAMPLE_RATE * 0.44f);
            localSpeakers[i].low_freq  = (int)lo;
            localSpeakers[i].high_freq = (int)hi;
        }
    }
    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);

    // Phase 2 — publish. Wi-Fi/identity fields are read only by network code,
    // so they are written directly. Audio-relevant fields (speakers[],
    // node_volume_multiplier) go under g_audio_config_mutex so the playback
    // task never sees a half-updated speaker or volume while it is sampling
    // them at the top of process_and_output_audio().
    if (!configured) return;

    strncpy(wifi_ssid, localSsid, sizeof(wifi_ssid) - 1);
    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
    strncpy(wifi_pass, localPass, sizeof(wifi_pass) - 1);
    wifi_pass[sizeof(wifi_pass) - 1] = '\0';
    strncpy(friendly_name, localName, sizeof(friendly_name) - 1);
    friendly_name[sizeof(friendly_name) - 1] = '\0';
    strncpy(mdns_target_host, localMdns, sizeof(mdns_target_host) - 1);
    mdns_target_host[sizeof(mdns_target_host) - 1] = '\0';
    use_static_ip = localUseStatic;
    strncpy(static_ip_str,  localIp,  sizeof(static_ip_str)  - 1); static_ip_str[sizeof(static_ip_str)  - 1] = '\0';
    strncpy(static_gw_str,  localGw,  sizeof(static_gw_str)  - 1); static_gw_str[sizeof(static_gw_str)  - 1] = '\0';
    strncpy(static_sn_str,  localSn,  sizeof(static_sn_str)  - 1); static_sn_str[sizeof(static_sn_str)  - 1] = '\0';
    strncpy(static_dns_str, localDns, sizeof(static_dns_str) - 1); static_dns_str[sizeof(static_dns_str) - 1] = '\0';

    if (g_audio_config_mutex) xSemaphoreTake(g_audio_config_mutex, portMAX_DELAY);
    node_volume_multiplier = localNodeVol;
    for (int i = 0; i < 4; i++) speakers[i] = localSpeakers[i];
    if (g_audio_config_mutex) xSemaphoreGive(g_audio_config_mutex);
}

// ---- Stats soft-reset (status page "Clear" button) --------------------------
// Zeroes every cumulative counter and re-anchors the "last X at" timestamps
// to now, so the status page can start a fresh measurement window without
// rebooting the node. Nothing else is touched: WiFi, the Opus decoder, the
// playout scheduler, and the queue all keep running. Only the numbers change.
inline void handleStatsReset() {
    stat_packets_received    = 0;
    stat_packets_dropped     = 0;
    stat_packets_oversize    = 0;
    // [stats-reset-audio-source-rejects]
    // Lifetime tally in normal operation, but the Clear button
    // represents "give me a fresh measurement window" — resetting
    // this alongside stat_packets_dropped and stat_ctrl_json_errors
    // keeps every packet-path counter on the same window. A stale
    // pre-Clear value on this field was the exact class of bug
    // [stats-reset-rtt] and [stats-reset-minlead] already fixed.
    stat_audio_source_rejects = 0;
    stat_rebuffers           = 0;
    stat_opus_decode_errors  = 0;
    stat_i2s_write_errors    = 0;
    stat_i2s_timeouts        = 0;
    stat_wifi_disconnects    = 0;
    stat_discovery_failures  = 0;
    stat_multicast_join_fails= 0;
    stat_post_stats_fails    = 0;
    stat_post_register_fails = 0;
    stat_ctrl_json_errors    = 0;
    stat_ctrl_packets_rx     = 0;
    stat_seq_received        = 0;
    stat_seq_missing         = 0;
    stat_seq_gaps            = 0;
    stat_seq_reordered       = 0;
    stat_seq_duplicate       = 0;
    stat_frames_concealed    = 0;
    stat_frames_late         = 0;
    // [empty-flushes-reset] Boot re-anchors accumulated before the operator
    // pressed Clear inflated the "how healthy was my boot" gauge. Reset so
    // a fresh window shows only post-Clear behaviour.
    stat_empty_flushes       = 0;
    stat_late_lt5            = 0;
    stat_late_5to20          = 0;
    stat_late_20to100        = 0;
    stat_late_100to200       = 0;
    stat_frames_discarded    = 0;
    stat_clock_resyncs       = 0;
    // [stats-reset-rtt] Not cumulative but a "last value"; Clear should
    // null it so the operator sees a fresh window. 0xFFFFFFFF = "no sample".
    stat_clock_sync_rtt_ms   = 0xFFFFFFFFu;
    stat_audio_withheld_incompatible     = 0;
    stat_audio_withheld_not_registered   = 0;
    stat_gap_gt_35ms_total  = 0;
    stat_gap_gt_50ms_total  = 0;
    stat_gap_gt_100ms_total = 0;
    uint32_t now = millis();
    // Do NOT touch ts_last_audio_packet_ms here. That field is the
    // "have we received real audio recently?" signal consulted by
    // handleScanNetworks() (see [scan-busy]) and reported as
    // lastAudioPacketAgeMs on /api/health. Re-anchoring it to `now`
    // fabricates a fake 'audio just arrived' event, so a Clear
    // incorrectly blocks /scan for 1 s and makes the Last Packet
    // card read "0 ms" even when the stream is silent. The natural
    // udpAudioTask path keeps this field honest; leave it alone.
    ts_last_server_contact_ms  = now;
    ts_last_rebuffer_ms        = now;
    ts_last_wifi_disconnect_ms = now;

    stat_total_correction_samples = 0;

    // [stats-reset-per-second] Zero the post-queue diagnostic accumulators
    // too. They normally roll over on the next 1 s tick in audioPlaybackTask,
    // but zeroing here makes the Clear button visibly instantaneous instead
    // of leaving the previous second's spikes on the dashboard until the
    // next rollover.
    stat_packets_this_sec             = 0;
    stat_packets_last_sec             = 0;
    stat_max_gap_ms_this_sec          = 0;
    stat_max_gap_ms_last_sec          = 0;
    stat_corrector_frames_this_sec    = 0;
    stat_corrector_frames_last_sec    = 0;
    stat_short_decode_frames_this_sec = 0;
    stat_short_decode_frames_last_sec = 0;
    stat_lead_below20_this_sec        = 0;
    stat_lead_below20_last_sec        = 0;
    // [stats-reset-minlead] Reset the "worst lead" pair to the INT32_MAX
    // sentinel so the next 1 s rollover cannot republish a pre-Clear value.
    // stat_min_lead_last_sec is what handleGetHealth serialises as
    // minLeadMsLastSec; leaving it stale made the Clear button visibly
    // incomplete on that one pill.
    stat_min_lead_this_sec            = 0x7FFFFFFF;
    stat_min_lead_last_sec            = 0x7FFFFFFF;
    // [captured-ms-timeline-counters] Zero the node-side counters
    // alongside the existing Clear-window fields so the operator sees
    // post-Clear behaviour only. The server-side pendingBuf /
    // encodeOverruns fields are removed (they were never written on the
    // node).
    statCapturedMsGapThisSec         = 0;
    statCapturedMsGapMaxLastSec      = 0;
    statCapturedMsGapGt20Total       = 0;
    statCapturedMsGapGt40Total       = 0;
    statLastCapturedMs               = 0;
    statCorrectorFramesTotal         = 0;
    statCorrectorFramesPeakThisSec   = 0;
    statCorrectorFramesPeakLastSec   = 0;
    portENTER_CRITICAL(&g_log_ring_mux);
    for (int i = 0; i < MAX_LOG_LINES; i++) system_logs[i][0] = '\0';
    system_log_head  = 0;
    system_log_count = 0;
    portEXIT_CRITICAL(&g_log_ring_mux);

    log_warn("[Stats] Counters reset from status page.");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", "{\"ok\":true}");
}

// ---- Route registration helpers --------------------------------------------
// Both are idempotent. WebServer::on() appends handlers rather than
// replacing them, so calling these twice (once from setup() on the STA path,
// once from startCaptivePortal() when the reset button moves the device to
// AP mode) would leave duplicate entries in the route table. The
// gRoutesRegistered flag (EVDPState.h) makes the second call a no-op.
inline void registerCommonRoutes() {
    static bool done = false;
    if (done) return;
    done = true;

    server.on("/api/fw",  HTTP_GET,  handleNodeFw);
    server.on("/api/ota", HTTP_POST, handleNodeOtaDone, handleNodeOtaUpload);
}

inline void registerAppRoutes() {
    static bool done = false;
    if (done) return;
    done = true;

    server.on("/",            HTTP_GET,  handleRoot);
    server.on("/scan",        HTTP_GET,  handleScanNetworks);
    server.on("/save",        HTTP_POST, handleSave);
    server.on("/api/network-test", HTTP_GET, handleNetworkTestStatus);
    server.on("/cancel",      HTTP_POST, handleCancel);
    server.on("/api/volume",  HTTP_GET,  handleGetVolume);
    server.on("/api/volume",  HTTP_POST, handlePostVolume);
    server.on("/api/volume",  HTTP_OPTIONS, handleCORS);
    server.on("/api/logs",    HTTP_GET,  handleGetLogs);
    server.on("/api/health",  HTTP_GET,  handleGetHealth);
    server.on("/api/cfg",     HTTP_GET,  handleApiCfg);      // [v1.1.2-cfg-fetch]
    server.on("/api/stats/reset", HTTP_POST, handleStatsReset);
    server.on("/status",      HTTP_GET,  handleStatusPage);
}

// ---- Captive portal --------------------------------------------------------
inline void startCaptivePortal() {
    if (is_in_ap_mode) return;

    // [ap-log-level] setup()'s early-return AP paths never reach the
    // `gPlayerLogLevel = 1` line at the end of setup(), so the node stays
    // at level 3 (verbose) forever. serverWatcherTask's 15 s scans then
    // flood both serial (blocking the UART) and the 50-slot log ring, so
    // the operator sees "scan found N APs" instead of the warnings they
    // care about. Drop to warning-level as soon as AP mode is entered.
    gPlayerLogLevel = 1;

    if (WiFi.getMode() == WIFI_AP_STA || WiFi.getMode() == WIFI_AP) {
        // AP is already up; just re-arm captive-portal routing / DNS.
        is_in_ap_mode = true;
        dnsServer.start(53, "*", WiFi.softAPIP());
        return;
    }
    is_in_ap_mode = true;
    if (strlen(wifi_ssid) == 0) {
        // Read the persisted "configured" flag inside its own begin/end pair
        // — the surrounding function has no namespace open at this point.
        if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
        preferences.begin("audio_node", true);
        bool prefsConfigured = preferences.getBool("configured", false);
        preferences.end();
        if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
        log_warn("[WiFi] Captive portal: no SSID stored (configured=" +
                 String(prefsConfigured ? "true" : "false") +
                 "). Enter Wi-Fi credentials via http://192.168.5.1/");
    }
    log_message("[WiFi] Starting AP (Captive Portal) Mode: 'Enkelvoud Node Setup'");

    // Belt-and-braces: make sure the STA radio is quiet before we flip into
    // AP+STA. The caller (setup) already did this, but anyone reaching this
    // function another way needs the same guarantee.
    WiFi.disconnect(false, false);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_DISCONNECTED && millis() - t0 < 1500) delay(50);

    // AP+STA: the captive portal stays reachable for the user while the STA
    // side independently probes for the Enkelvoud server's SoftAP. These are
    // separate interfaces, so the user's session on our AP is undisturbed.
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);

    // Put the node's own AP on 192.168.5.0/24 so it can never collide with
    // the server's default SoftAP subnet 192.168.4.0/24 when both are live.
    IPAddress apIp(192, 168, 5, 1);
    IPAddress apGw(192, 168, 5, 1);
    IPAddress apSn(255, 255, 255, 0);
    WiFi.softAPConfig(apIp, apGw, apSn);
    WiFi.softAP("Enkelvoud Node Setup");

    dnsServer.start(53, "*", WiFi.softAPIP());

    registerAppRoutes();
    if (!gRoutesRegistered) {
        // onNotFound is not part of registerAppRoutes() because the STA
        // path does not install it. Register exactly once even if
        // startCaptivePortal() is re-entered from loop().
        server.onNotFound([]() { handleRoot(); });
        gRoutesRegistered = true;
    }
    registerCommonRoutes();
    // [web-server-begun] Only call begin() the FIRST time. On a deferred
    // AP-mode transition (GPIO1 hold → networkTask → startCaptivePortal),
    // server.begin() has already run in setup's STA path.
    if (!g_web_server_started) {
        server.begin();
        g_web_server_started = true;
        log_message("[Web] Captive portal web server started.");
    } else {
        log_message("[Web] Captive portal active; reusing running web server.");
    }

    if (network_task_handle == nullptr) {
        if (xTaskCreatePinnedToCore(networkTask, "Network", 12288, NULL, 4, &network_task_handle, 0) != pdPASS)
            log_warn("[Boot] FAILED to create networkTask (AP mode).");
    }
    if (xTaskCreatePinnedToCore(serverWatcherTask, "SrvWatch", 6144, NULL, 2, NULL, 0) != pdPASS)
        log_warn("[Boot] FAILED to create serverWatcherTask.");

    log_message("[Web] Captive portal armed (server watcher running).");
}