#pragma once
// ============================================================================
//  EVDPProtocolCompat.h - Audio wire-protocol parsing + compatibility state,
//  kept free of Arduino/ESP32/opus dependencies (plain int/bool/const char*
//  and fixed-width integer types only) so this single source of truth can be
//  exercised by a host-side unit test (tests/test_protocol_compat.cpp)
//  without a hardware build, mirroring EVDProtocolCompat.h on the server.
// ============================================================================
//
//  Wire format (server -> player, UDP unicast/multicast to AUDIO_UDP_PORT).
//  This MUST exactly match the AUDIO_PROTO_* docs in the server's
//  EnkelvoudServerV2/EVDConfig.h - that file is the authoritative source for
//  this format, not this comment. A fixed 12-byte header precedes exactly
//  one Opus frame payload:
//
//    offset  size  field          notes
//    ------  ----  -------------  ---------------------------------------
//     0       2    magic          little-endian AUDIO_PROTO_MAGIC (0xE1D0)
//     2       1    version        AUDIO_PROTO_VERSION (currently 2)
//     3       1    flags          reserved, ignored by this version
//     4       4    seq            little-endian uint32, monotonically
//                                 increasing per encoded frame (wraps)
//     8       4    captured_ms    little-endian uint32, server millis() at
//                                 the capture/frame instant
//    12       N    opus payload   exactly `len` bytes of Opus data
//
//  This firmware implements only the v2 parser: a datagram whose first two
//  bytes do not match AUDIO_PROTO_MAGIC + AUDIO_PROTO_VERSION is dropped by
//  parseAudioHeaderV2(). There is no v1 fallback.
// ============================================================================
#include <stdint.h>
#include <stddef.h>

// ---- Wire constants (must match server EVDConfig.h) -------------------------
#define AUDIO_PROTO_MAGIC   0xE1D0u  // uint16_t, little-endian on the wire
#define AUDIO_PROTO_VERSION 2        // server wire-format version this build parses

// Field widths, named individually (matches the server's naming) so
// AUDIO_HEADER_BYTES can be statically checked against the parsing code
// below instead of silently drifting if the layout ever changes.
#define AUDIO_HDR_MAGIC_BYTES       2
#define AUDIO_HDR_VERSION_BYTES     1
#define AUDIO_HDR_FLAGS_BYTES       1
#define AUDIO_HDR_SEQ_BYTES         4
#define AUDIO_HDR_CAPTURED_MS_BYTES 4
#define AUDIO_HEADER_BYTES (AUDIO_HDR_MAGIC_BYTES + AUDIO_HDR_VERSION_BYTES + \
                            AUDIO_HDR_FLAGS_BYTES + AUDIO_HDR_SEQ_BYTES +   \
                            AUDIO_HDR_CAPTURED_MS_BYTES) // = 12

// [wire-flag-unicast] Header flag byte (index 3), bit 0. Set by the server on
// every frame it unicasts to a specific node. A datagram with a NON-ZERO flag
// byte that lacks this bit was addressed to someone else; an all-zero flag
// byte comes from a server build that predates the flag and is accepted for
// compatibility. Mirrors AUDIO_FLAG_UNICAST in the server's EVDUDP.h.
#define AUDIO_FLAG_UNICAST 0x01
static_assert(AUDIO_HEADER_BYTES == 12,
              "AUDIO_HEADER_BYTES must stay in sync with parseAudioHeaderV2()");

// This player's declared/preferred audio protocol version, sent to the
// server as "audioProtocolVersion" in /api/player/register (see
// handlePlayerRegistration + EVDProtocolCompat.h on the server). Kept equal
// to AUDIO_PROTO_VERSION: this firmware only implements the v2 parser, so
// declaring anything else would just make the server negotiate it down (or
// fail closed) to a version we already match.
#define PLAYER_DECLARED_AUDIO_PROTOCOL_VERSION AUDIO_PROTO_VERSION

// ---- v2 header parsing -------------------------------------------------------
struct ParsedAudioHeaderV2 {
  uint8_t  flags;
  uint32_t seq;
  uint32_t captured_ms;
};

// Returns true and fills `out` iff `data`/`len` begins with a well-formed v2
// header (magic + version match exactly). Never partially trusts a buffer
// that fails the check - the caller must not fall back to reading seq/
// captured_ms from a buffer this rejected.
inline bool parseAudioHeaderV2(const uint8_t *data, size_t len, ParsedAudioHeaderV2 &out) {
  if (data == nullptr || len < (size_t)AUDIO_HEADER_BYTES) return false;

  uint16_t magic = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
  if (magic != AUDIO_PROTO_MAGIC) return false;
  if (data[2] != AUDIO_PROTO_VERSION) return false;

  out.flags = data[3];
  out.seq         = (uint32_t)data[4]  | ((uint32_t)data[5]  <<  8)
                  | ((uint32_t)data[6] << 16) | ((uint32_t)data[7]  << 24);
  out.captured_ms = (uint32_t)data[8]  | ((uint32_t)data[9]  <<  8)
                  | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);
  return true;
}

// ---- Player-side audio compatibility state -----------------------------------
// Tracks whether THIS player currently believes it is safe to interpret
// incoming audio datagrams, based on the most recent /api/player/register
// response from the server (see EVDProtocolCompat.h + handlePlayerRegistration
// on the server for the authoritative policy this mirrors).
enum AudioCompatState {
  AUDIO_COMPAT_UNKNOWN      = 0, // no registration response processed yet,
                                  // or the server never declared any
                                  // compatibility fields (pre-negotiation
                                  // server) - see legacy mode handling.
  AUDIO_COMPAT_COMPATIBLE   = 1, // server explicitly confirmed audioCompatible
  AUDIO_COMPAT_INCOMPATIBLE = 2  // server explicitly confirmed NOT compatible
};

struct AudioCompatInfo {
  AudioCompatState state;
  int              negotiatedProtocolVersion; // 0 unless state == COMPATIBLE
  const char      *reason;                    // short, stable machine-readable code
};

// declaredFieldsPresent: true iff the server's response JSON actually
// contained the negotiation fields (audioProtocolVersion/audioCompatible).
// Its absence means we are talking to a pre-negotiation ("old") server that
// has no concept of this policy at all - distinct from the server having
// evaluated us and said no.
inline AudioCompatInfo evaluatePlayerAudioCompat(bool declaredFieldsPresent,
                                                  bool audioCompatible,
                                                  int  negotiatedProtocolVersion) {
  if (!declaredFieldsPresent) {
    return AudioCompatInfo{ AUDIO_COMPAT_UNKNOWN, 0, "server_no_negotiation_fields" };
  }
  if (!audioCompatible) {
    return AudioCompatInfo{ AUDIO_COMPAT_INCOMPATIBLE, 0, "server_reports_incompatible" };
  }
  return AudioCompatInfo{ AUDIO_COMPAT_COMPATIBLE, negotiatedProtocolVersion, "ok" };
}
