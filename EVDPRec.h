#pragma once
// ============================================================================
//  EVDPRec.h - master-recording capture buffer (server-driven, PSRAM backed)
//
//  The server pushes a rec_start / rec_stop ctrl datagram. While armed, every
//  log line is copied into a PSRAM buffer IN ADDITION to the existing ring and
//  Serial output - the normal logging path is untouched and keeps running.
//  After rec_stop the server fetches the buffer with
//  GET /api/rec_report?recId=..., streamed in small chunks so no large JSON is
//  ever built on the internal heap.
//
//  Why PSRAM: the N16R8 has 8 MB of it, unused by the rest of the firmware,
//  while the internal heap runs at 60-70 % during playback. 12000 lines x
//  160 B = 1.9 MB - free in PSRAM, impossible on the internal heap.
//
//  If PSRAM is not enabled or the allocation fails, capture stays off and the
//  server's 1 Hz /api/logs poll fallback still produces a report.
// ============================================================================

#include <esp_heap_caps.h>

#include "EVDPState.h"          // MAX_LOG_LINE_LEN, log_warn, millis

#define REC_MAX_LINES 12000
#define REC_LINE_LEN  MAX_LOG_LINE_LEN

static char            *g_recBuf      = nullptr;
static volatile uint32_t g_recCount     = 0;
static volatile uint32_t g_recDropped   = 0;
static volatile bool     g_recActive    = false;
static volatile bool     g_recReady     = false;
static String            g_recId;
static volatile uint32_t g_recStartMs   = 0;
static volatile uint32_t g_recEndMs     = 0;
static volatile bool     g_recAckPending = false;
static portMUX_TYPE      g_recMux = portMUX_INITIALIZER_UNLOCKED;

// Freeze the current capture. The buffer is kept for the server to fetch.
static inline void recStop() {
    bool had = false;
    uint32_t n = 0;
    portENTER_CRITICAL(&g_recMux);
    if (g_recActive) {
        g_recActive = false;
        g_recReady  = true;
        g_recEndMs  = millis();
        had = true;
        n = g_recCount;
    }
    portEXIT_CRITICAL(&g_recMux);
    if (had) log_message("[Rec] capture stopped, " + String(n) + " lines");
}

// Arm a new capture. Returns false when PSRAM is unavailable (server falls
// back to polling /api/logs for this node).
static inline bool recStart(const String &id) {
    if (g_recActive) recStop();
    if (g_recBuf == nullptr) {
        g_recBuf = (char *)heap_caps_malloc((size_t)REC_MAX_LINES * REC_LINE_LEN,
                                           MALLOC_CAP_SPIRAM);
        if (g_recBuf == nullptr) {
            log_warn("[Rec] no PSRAM buffer; server will poll /api/logs instead");
            return false;
        }
    }
    portENTER_CRITICAL(&g_recMux);
    g_recCount   = 0;
    g_recDropped = 0;
    g_recReady   = false;
    g_recStartMs = millis();
    g_recEndMs   = 0;
    g_recActive  = true;
    portEXIT_CRITICAL(&g_recMux);

    g_recId = id;
    g_recAckPending = true;
    log_message("[Rec] capture started id=" + id);
    return true;
}

// Called from logRingPush() AFTER the ring's own critical section, so the
// 160-byte PSRAM copy never extends the log ring's hold time.
static inline void recMaybeAppend(const char *line) {
    if (!g_recActive || g_recBuf == nullptr) return;
    portENTER_CRITICAL(&g_recMux);
    if (g_recActive && g_recCount < REC_MAX_LINES) {
        memcpy(g_recBuf + (size_t)g_recCount * REC_LINE_LEN, line, REC_LINE_LEN);
        g_recCount++;
    } else if (g_recActive) {
        g_recDropped++;                 // buffer full: keep counting, stop storing
    }
    portEXIT_CRITICAL(&g_recMux);
}

// ---- accessors for the /api/rec_report handler -----------------------------

static inline bool     recReady()   { return g_recReady; }
static inline uint32_t recCount()   { return g_recCount; }
static inline uint32_t recDropped() { return g_recDropped; }
static inline uint32_t recStartMs() { return g_recStartMs; }
static inline uint32_t recEndMs()   { return g_recEndMs; }
static inline const String &recId() { return g_recId; }
static inline const char *recLine(uint32_t i) {
    return g_recBuf ? (g_recBuf + (size_t)i * REC_LINE_LEN) : "";
}
