// signaling.h
#pragma once
#include <stdint.h>

#define SIG_ETHER_TYPE 0x88B5 // custom EtherType for signaling frames

// Application opcodes
#define SIG_OPCODE_DATA 0x10
#define SIG_OPCODE_ACK 0x11

#pragma pack(push, 1)
typedef struct {
  uint16_t version;     // e.g., 1
  uint16_t channel_id;  // logical FIFO channel
  uint32_t seq;         // per-channel sequence
  uint16_t opcode;      // app-defined
  uint16_t payload_len; // bytes of payload that follow
                        // followed by payload bytes (payload_len)
} sig_hdr_t;
#pragma pack(pop)

#define SIG_MAX_PAYLOAD 48 // keep small for signaling frames
