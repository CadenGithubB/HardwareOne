#ifndef HW1_MILESTONE_WIRE_H
#define HW1_MILESTONE_WIRE_H

/* Standalone transport probe. Layout matches HardwareOne V4, while opcodes
 * 200..202 occupy its experiment range. This is NOT application pairing. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HW1_MILESTONE_MAGIC 0x3148u
#define HW1_MILESTONE_VERSION 4u
#define HW1_MILESTONE_HEADER_SIZE 32u
#define HW1_MILESTONE_MAX_FRAME 250u
#define HW1_MILESTONE_DISCOVERY 200u
#define HW1_MILESTONE_REQUEST 201u
#define HW1_MILESTONE_ECHO 202u
#define HW1_MILESTONE_MESH_LABEL "p4-milestone"
#define HW1_MILESTONE_DEFAULT_CHANNEL 6u

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t ver;
    uint8_t type;
    uint16_t flags;
    uint8_t headerLen;
    uint8_t reserved1;
    uint32_t msgId;
    uint8_t origin[6];
    uint8_t ttl;
    uint8_t fragIndex;
    uint8_t fragCount;
    uint8_t reserved2;
    uint16_t meshFingerprint;
    uint16_t sessionId;
    uint32_t frameSeq;
    uint16_t crc16;
} hw1_milestone_header_t;

#ifdef __cplusplus
static_assert(sizeof(hw1_milestone_header_t) == HW1_MILESTONE_HEADER_SIZE,
              "HardwareOne V4 wire header must be 32 bytes");
#else
_Static_assert(sizeof(hw1_milestone_header_t) == HW1_MILESTONE_HEADER_SIZE,
               "HardwareOne V4 wire header must be 32 bytes");
#endif

static inline uint16_t hw1_milestone_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static inline uint16_t hw1_milestone_fingerprint(void)
{
    return hw1_milestone_crc16((const uint8_t *)HW1_MILESTONE_MESH_LABEL,
                             sizeof(HW1_MILESTONE_MESH_LABEL) - 1);
}

/* Payload byte index starts at zero immediately after the 32-byte header. */
static inline uint8_t hw1_milestone_payload_byte(uint32_t msg_id, size_t index)
{
    return (uint8_t)(msg_id * 31u + (uint32_t)index * 17u + 0x5au);
}

static inline void hw1_milestone_init_header(hw1_milestone_header_t *h,
                                            uint8_t type, uint32_t msg_id,
                                            const uint8_t origin[6],
                                            const uint8_t *payload,
                                            size_t payload_len)
{
    memset(h, 0, sizeof(*h));
    h->magic = HW1_MILESTONE_MAGIC;
    h->ver = HW1_MILESTONE_VERSION;
    h->type = type;
    h->headerLen = sizeof(*h);
    h->msgId = msg_id;
    memcpy(h->origin, origin, 6);
    h->ttl = 1;
    h->fragCount = 1;
    h->meshFingerprint = hw1_milestone_fingerprint();
    h->crc16 = payload_len ? hw1_milestone_crc16(payload, payload_len) : 0;
}

static inline bool hw1_milestone_valid_frame(const uint8_t *frame, size_t len)
{
    if (!frame || len < sizeof(hw1_milestone_header_t) || len > HW1_MILESTONE_MAX_FRAME)
        return false;
    hw1_milestone_header_t h;
    memcpy(&h, frame, sizeof(h));
    size_t payload_len = len - sizeof(h);
    return h.magic == HW1_MILESTONE_MAGIC && h.ver == HW1_MILESTONE_VERSION &&
           h.headerLen == sizeof(h) && h.flags == 0 && h.reserved1 == 0 &&
           h.reserved2 == 0 && h.ttl == 1 && h.fragIndex == 0 && h.fragCount == 1 &&
           h.meshFingerprint == hw1_milestone_fingerprint() &&
           h.sessionId == 0 && h.frameSeq == 0 &&
           h.crc16 == (payload_len ? hw1_milestone_crc16(frame + sizeof(h), payload_len) : 0);
}

static inline bool hw1_milestone_valid_pattern(const uint8_t *frame, size_t len)
{
    if (!hw1_milestone_valid_frame(frame, len)) return false;
    hw1_milestone_header_t h;
    memcpy(&h, frame, sizeof(h));
    for (size_t i = 0; i < len - sizeof(h); ++i) {
        if (frame[sizeof(h) + i] != hw1_milestone_payload_byte(h.msgId, i)) return false;
    }
    return true;
}

static inline bool hw1_milestone_probe_size(size_t len)
{
    return len == 32 || len == 64 || len == 128 || len == 250;
}

#endif
