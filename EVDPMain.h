#pragma once
// ============================================================================
//  EVDPMain.h - setup() and loop()
//
//  NOTE: setup() and loop() MUST NOT be 'inline'. Arduino's main.cpp calls
//  them as external symbols, and the compiler will not emit an out-of-line
//  definition for an inline function nothing else calls.
// ============================================================================
#include "EVDPWeb.h"

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

void setup() {
    Serial.begin(115200);
    delay(1500);

    pinMode(RESET_PIN, INPUT_PULLUP);


    // ---- Boot diagnostics --------------------------------------------------
    // Capture the reset reason before anything else can trigger another
    // reset, and bump a persistent boot counter so the user can tell a
    // single power cycle from a device that has been rebooting in a loop.
    {
        esp_reset_reason_t reason = esp_reset_reason();
        const char* reason_str = "UNKNOWN";
        switch (reason) {
            case ESP_RST_POWERON:   reason_str = "POWERON";   break;
            case ESP_RST_SW:        reason_str = "SOFTWARE";  break;
            case ESP_RST_PANIC:     reason_str = "PANIC";     break;
            case ESP_RST_INT_WDT:   reason_str = "INT_WDT";   break;
            case ESP_RST_TASK_WDT:  reason_str = "TASK_WDT";  break;
            case ESP_RST_WDT:       reason_str = "WDT";       break;
            case ESP_RST_DEEPSLEEP: reason_str = "DEEPSLEEP"; break;
            case ESP_RST_BROWNOUT:  reason_str = "BROWNOUT";  break;
            case ESP_RST_SDIO:      reason_str = "SDIO";      break;
            default: break;
        }
        strncpy(boot_reason_str, reason_str, sizeof(boot_reason_str) - 1);
        boot_reason_str[sizeof(boot_reason_str) - 1] = '\0';

        log_message(String("[Boot] Reason: ") + boot_reason_str);
    }


    // Output buffers are eight samples larger than a decoded frame; the
    // corrector clamps correction to ±1 sample per frame, so eight leaves
    // headroom for the cubic-interp resampler's tail reads (i0, i1, i2).
    pcm_decode_buffer  = (int16_t*)malloc(OPUS_FRAME_SIZE * CHANNELS * sizeof(int16_t));
    dac1_output_buffer = (int16_t*)malloc((OPUS_FRAME_SIZE + 8) * CHANNELS * sizeof(int16_t));
    dac2_output_buffer = (int16_t*)malloc((OPUS_FRAME_SIZE + 8) * CHANNELS * sizeof(int16_t));
    // [boot-alloc-guard] A silent OOM here leaves the node permanently mute
    // with no trace on the dashboard (i2sDac*Ready still true, queue=0).
    // Log loudly at boot; ring buffer captures it even at gPlayerLogLevel=1.
    if (!pcm_decode_buffer || !dac1_output_buffer || !dac2_output_buffer) {
        log_warn(String("[Boot] CRITICAL: audio buffer malloc failed "
                        "(pcm=") + (pcm_decode_buffer  ? "ok" : "NULL") +
                 " dac1=" + (dac1_output_buffer ? "ok" : "NULL") +
                 " dac2=" + (dac2_output_buffer ? "ok" : "NULL") + ")");
    }


    uint64_t chipid = ESP.getEfuseMac();
    char uuid_buf[32];
    sprintf(uuid_buf, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    node_uuid_str = String(uuid_buf);
    log_message(String("[Boot] Node UUID: ") + node_uuid_str);

    // [prefs-mutex] Must exist before any task can reach preferences.
    // setup() itself is single-threaded, but networkTask and the WebServer
    // task start below and both touch NVS.
    g_prefs_mutex = xSemaphoreCreateMutex();
    if (g_prefs_mutex == nullptr)
        log_warn("[Boot] CRITICAL: failed to create prefs mutex.");

    // ---- Persistent boot counter ------------------------------------------
    // The status page renders "boot #N" from /api/health's "bootCount" so an
    // operator can tell a single power cycle from a boot loop (e.g. a bad
    // OTA that panics on start). Stored under its own NVS key so the
    // "configured" block below can stay read-only.
    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    preferences.begin("audio_node", false);
    boot_count = preferences.getUInt("boot_count", 0) + 1;
    preferences.putUInt("boot_count", boot_count);
    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
    log_message(String("[Boot] Count: ") + String(boot_count));

    // Single source of truth: loadStoredPreferences() populates wifi_ssid
    // from NVS. The boot branch below must consult wifi_ssid, not a
    // separate pre-load read, so the two can never disagree.
    if (g_prefs_mutex) xSemaphoreTake(g_prefs_mutex, portMAX_DELAY);
    preferences.begin("audio_node", true);
    bool isConfigured = preferences.getBool("configured", false);
    preferences.end();
    if (g_prefs_mutex) xSemaphoreGive(g_prefs_mutex);
    log_message(String("[Prefs] Is configured: ") + (isConfigured ? "true" : "false"));

    if (isConfigured) {
        loadStoredPreferences();
        log_message(String("[Prefs] Loaded. ssid_len=") + String(strlen(wifi_ssid)) +
                    " name='" + String(friendly_name) + "'");
    }

    for (int i = 0; i < 4; i++)
        speaker_filters[i].configure(speakers[i].low_freq, speakers[i].high_freq, SAMPLE_RATE);

    // [audio-config-mutex] Must exist before the audio task is created.
    g_audio_config_mutex = xSemaphoreCreateMutex();
    if (g_audio_config_mutex == nullptr)
        log_warn("[Boot] CRITICAL: failed to create audio config mutex; "
                 "live speaker edits may glitch.");

    // ---- Audio subsystem (started unconditionally) -------------------------
    // We bring up I2S, the Opus decoder, the audio queue, the playback task
    // and the UDP drain task *before* deciding how we'll reach the network.
    // That way the captive-portal fallback path can go live the moment the
    // node manages to associate to the Enkelvoud server AP, without a reboot.
    init_i2s();

    // [audio-hz-diagnostics]
    // Configure the per-second band analyser. Pure coefficient setup —
    // no I2S access, no queue touch, no allocations. Safe before audio
    // starts. Logs one line to the ring so the recorder shows it was
    // enabled on this boot.
    configureAudioHzDiag();

    int error;
    opus_decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
    if (error == OPUS_OK) log_message("[Opus] Decoder initialized successfully.");
    else                   log_message(String("[Opus] Failed to initialize decoder. Error: ") + error);

    audio_queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(AudioMessage));
    if (audio_queue == nullptr) log_message("[Audio] FAILED to create audio queue!");

    // Core 1: audio, priority 3. networkTask is priority 4 (higher) on Core 0,
    // and pingServerTask is priority 1 on Core 0. Higher number = higher
    // priority in FreeRTOS. The two high-priority tasks are on different
    // cores, so there is no preemption conflict today; moving either task
    // across cores would invert the intended "audio wins over networking"
    // ordering.
    if (xTaskCreatePinnedToCore(audioPlaybackTask, "AudioPlayback", 16384, NULL, 3,
                                &audio_task_handle, 1) != pdPASS)
        log_warn("[Boot] FAILED to create audioPlaybackTask (no audio).");

    // Core 0: UDP drain. Idle until setupMulticastSockets() binds the socket.
    if (xTaskCreatePinnedToCore(udpAudioTask, "UdpAudio", 6144, NULL, 5, NULL, 0) != pdPASS)
        log_warn("[Boot] FAILED to create udpAudioTask (no audio RX).");

    // ---- Network selection -------------------------------------------------
    bool networkUp = false;

    // GPIO 1 grounded at boot -> AP mode (captive portal).
    delay(30);   // let the pull-up settle before reading
    if (digitalRead(RESET_PIN) == LOW) {
        log_warn("[Boot] GPIO1 grounded at boot -> entering AP mode.");
        startCaptivePortal();
        return;
    }

    if (isConfigured && strlen(wifi_ssid) > 0) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        if (use_static_ip) {
            IPAddress ip, gw, sn, dns;
            if (ip.fromString(static_ip_str) && gw.fromString(static_gw_str)) {
                sn.fromString(static_sn_str);
                dns.fromString(static_dns_str);
                WiFi.config(ip, gw, sn, dns);
                log_message("[WiFi] Configured with static IP.");
            }
        }
        log_message(String("[WiFi] Connecting to SSID: ") + wifi_ssid);
        WiFi.begin(wifi_ssid, wifi_pass);

        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) { delay(300); attempts++; }
        networkUp = (WiFi.status() == WL_CONNECTED);
        if (networkUp) {
            // [wifi-ps-reassert]
            // setSleep(false) ran before begin(), but some arduino-esp32
            // cores re-derive the PS mode at association-complete and fall
            // back to WIFI_PS_MIN_MODEM. Re-assert once we are actually
            // associated; this is the exact "packet 50, gap 170 ms" trigger.
            WiFi.setSleep(false);
            log_message(String("[WiFi] Connected! IP: ") + WiFi.localIP().toString());
        } else {
            log_message("[WiFi] Saved network unavailable.");
        }
    } else {
        log_message("[Boot] Node unconfigured or missing SSID.");
    }

    // Saved SSID but not connected: look for the server on its own SoftAP
    // at 192.168.4.1 by joining that AP.
    if (!networkUp && isConfigured && strlen(wifi_ssid) > 0) {
        log_message("[Net] Saved SSID offline; trying server AP 192.168.4.1...");
        networkUp = tryJoinServerAccessPoint();
        if (networkUp) {
            serverIP    = IPAddress(192, 168, 4, 1);
            serverFound = true;
        }
    }

    if (!networkUp) {
        // tryJoinServerAccessPoint() left the driver mid-connect. If we go
        // straight to AP+STA without cancelling that attempt, the watcher's
        // first WiFi.begin() is rejected with "sta is connecting". Abort it
        // cleanly and wait for WL_DISCONNECTED before the mode switch.
        WiFi.disconnect(false, false);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_DISCONNECTED && millis() - t0 < 2000) delay(50);

        log_message("[Boot] No network reachable. Launching Captive Portal + server-AP watcher.");
        startCaptivePortal();
        return;
    }

    // ---- STA path ----------------------------------------------------------
    {
        // [mdns-label-63] mDNS hostname label caps at 63 bytes. friendly_name
        // is 64 B; "-XXXX" pushes the combined label past 63 and MDNS.begin()
        // silently fails (return value ignored before this fix), leaving the
        // node unreachable via .local even though IP works.
        String safeName = sanitizeNodeName(String(friendly_name));
        if (safeName.length() > 56) safeName = safeName.substring(0, 56);
        String fullMdns = safeName + "-" + node_uuid_str.substring(node_uuid_str.length() - 4);
        if (!MDNS.begin(fullMdns.c_str()))
            log_warn(String("[mDNS] MDNS.begin('") + fullMdns + "') failed");
        else
            log_message(String("[mDNS] Broadcasting as ") + fullMdns + ".local");
    }

    registerAppRoutes();
    registerCommonRoutes();
    server.begin();
    g_web_server_started = true;
    log_message("[Web] Main web server started.");

    discover_server();
    if (serverFound) {
        log_message(String("[Net] Server at ") + serverIP.toString());
    }
    if (!setupMulticastSockets()) {
        log_message("[UDP] Multicast setup failed - no audio will be received");
    }

     // [v1.1.1-nettask-stack]
    // networkTask runs HTTPClient + WiFiClient + DynamicJsonDocument in
    // applyRegistrationResponse(), plus postNodeStats() with its own snprintf
    // buffer and String concatenations. 8 KB was 1-2 KB away from the ceiling
    // on the registration path. An overflow there is silent corruption or a
    // panic rather than a clean failure, which matched the "intermittent
    // stutter" profile. 12 KB costs nothing on the S3 and removes the risk.
    if (xTaskCreatePinnedToCore(networkTask, "Network", 12288, NULL, 4, &network_task_handle, 0) != pdPASS)
        log_warn("[Boot] FAILED to create networkTask.");
    // [ping-stack] 4 KB was chosen for "just delay(3s) in a loop" but this
    // task actually calls postRegister() -> postJson() -> HTTPClient (≈2.5 KB
    // on stack alone) + a 512 B snprintf buffer. The sibling networkTask
    // uses 12 KB for the identical path. Match at 8 KB minimum.
    if (xTaskCreatePinnedToCore(pingServerTask, "PingTask", 8192, NULL, 1, &ping_task_handle, 0) != pdPASS)
        log_warn("[Boot] FAILED to create pingServerTask.");
    // If the very first discovery round failed, start a slow background probe
    // that scans for the server's SoftAP and reports what to do about it.
    // This task does NOT change the network by itself — it only makes the
    // root cause visible in the serial log.
    if (!serverFound) {
        if (xTaskCreatePinnedToCore(serverProbeTask, "SrvProbe", 4096, NULL, 1, NULL, 0) != pdPASS)
            log_warn("[Boot] FAILED to create serverProbeTask.");
        log_message("[Boot] Server discovery failed; background probe armed.");
    }

    // Boot diagnostics are done. From here on log_message() is silently
    // buffered but not printed; only log_warn() reaches the serial monitor
    // (rebuffers, link loss, protocol mismatches, decode failures). The
    // status page's Log card still shows the full ring buffer via /api/logs,
    // so nothing is lost, it is only kept off the terminal.
    log_warn("[Boot] Setup complete. Steady state: warnings/errors only.");
    gPlayerLogLevel = 1;
}

void loop() {
    if (digitalRead(RESET_PIN) == LOW) {
        if (!reset_button_pressed) {
            reset_button_pressed = true;
            reset_button_start_time = millis();
        } else if (!is_in_ap_mode && millis() - reset_button_start_time > 3000) {
            // [ap-mode-deferred] Do NOT call startCaptivePortal() here —
            // this task runs on Core 1 while networkTask owns the
            // WebServer/DNS/Preferences state on Core 0. Set the request
            // flag instead; networkTask picks it up on its next pass.
            log_warn("[System] GPIO1 held 3s. Requesting AP mode.");
            g_enter_ap_mode_requested = true;
            reset_button_pressed = false;
        }
    } else {
        reset_button_pressed = false;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
}