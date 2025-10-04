/**
 * @file val_errors.h
 * @brief VAL Protocol Error Codes and Detail Masks
 * 
 * @version 0.7.0
 * @copyright MIT License - Copyright 2025 Arthur T Lee
 * 
 * Dedicated to Valerie Lee - for all her support over the years
 * allowing me to chase my ideas.
 */

#ifndef VAL_ERRORS_H
#define VAL_ERRORS_H

#include <stdint.h>

// Status codes (negative for errors). Kept in a common header so both MCU and host share numeric values.
typedef enum
{
    VAL_OK = 0,
    // Non-error informational status for per-file completion
    VAL_SKIPPED = 1,
    VAL_ERR_INVALID_ARG = -1,
    VAL_ERR_NO_MEMORY = -2,
    VAL_ERR_IO = -3,
    VAL_ERR_TIMEOUT = -4,
    VAL_ERR_PROTOCOL = -5,
    VAL_ERR_CRC = -6,
    VAL_ERR_RESUME_VERIFY = -7,
    VAL_ERR_INCOMPATIBLE_VERSION = -8,
    VAL_ERR_PACKET_SIZE_MISMATCH = -9,
    VAL_ERR_FEATURE_NEGOTIATION = -10,
    VAL_ERR_ABORTED = -11,
    // Adaptive TX related (Phase 1 scaffolding)
    VAL_ERR_MODE_NEGOTIATION_FAILED = -12,
    VAL_ERR_MODE_SYNC_FAILED = -13,
    VAL_ERR_UNSUPPORTED_TX_MODE = -14,
    // Connection quality (graceful failure)
    VAL_ERR_PERFORMANCE = -15,
} val_status_t;

// Error detail mask (32-bit) layout:
// Bits 0-7:   Network/Transport
// Bits 8-15:  CRC/Integrity
// Bits 16-23: Protocol/Feature
// Bits 24-27: Filesystem
// Bits 28-31: Context (payload selector)

// Category ranges
#define VAL_ERROR_DETAIL_NET_MASK ((uint32_t)0x000000FF)
#define VAL_ERROR_DETAIL_CRC_MASK ((uint32_t)0x0000FF00)
#define VAL_ERROR_DETAIL_PROTO_MASK ((uint32_t)0x00FF0000)
#define VAL_ERROR_DETAIL_FS_MASK ((uint32_t)0x0F000000)
#define VAL_ERROR_DETAIL_CONTEXT_MASK ((uint32_t)0xF0000000)

// Network (0-7)
#define VAL_ERROR_DETAIL_NETWORK_RESET ((uint32_t)0x00000001)
#define VAL_ERROR_DETAIL_TIMEOUT_ACK ((uint32_t)0x00000002)
#define VAL_ERROR_DETAIL_TIMEOUT_DATA ((uint32_t)0x00000004)
#define VAL_ERROR_DETAIL_TIMEOUT_META ((uint32_t)0x00000008)
#define VAL_ERROR_DETAIL_TIMEOUT_HELLO ((uint32_t)0x00000010)
#define VAL_ERROR_DETAIL_SEND_FAILED ((uint32_t)0x00000020)
#define VAL_ERROR_DETAIL_RECV_FAILED ((uint32_t)0x00000040)
#define VAL_ERROR_DETAIL_CONNECTION ((uint32_t)0x00000080)
// Note: Bit 0x00000100 is first CRC detail, leaving network category full

// CRC (8-15)
#define VAL_ERROR_DETAIL_CRC_HEADER ((uint32_t)0x00000100)
#define VAL_ERROR_DETAIL_CRC_TRAILER ((uint32_t)0x00000200)
#define VAL_ERROR_DETAIL_CRC_FILE ((uint32_t)0x00000400)
#define VAL_ERROR_DETAIL_CRC_RESUME ((uint32_t)0x00000800)
#define VAL_ERROR_DETAIL_SIZE_MISMATCH ((uint32_t)0x00001000)
#define VAL_ERROR_DETAIL_PACKET_CORRUPT ((uint32_t)0x00002000)
#define VAL_ERROR_DETAIL_SEQ_ERROR ((uint32_t)0x00004000)
#define VAL_ERROR_DETAIL_OFFSET_ERROR ((uint32_t)0x00008000)

// Protocol/Feature (16-23)
#define VAL_ERROR_DETAIL_VERSION_MAJOR ((uint32_t)0x00010000)
#define VAL_ERROR_DETAIL_VERSION_MINOR ((uint32_t)0x00020000)
#define VAL_ERROR_DETAIL_PACKET_SIZE ((uint32_t)0x00040000)
#define VAL_ERROR_DETAIL_FEATURE_MISSING ((uint32_t)0x00080000)
#define VAL_ERROR_DETAIL_INVALID_STATE ((uint32_t)0x00100000)
#define VAL_ERROR_DETAIL_MALFORMED_PKT ((uint32_t)0x00200000)
#define VAL_ERROR_DETAIL_UNKNOWN_TYPE ((uint32_t)0x00400000)
#define VAL_ERROR_DETAIL_PAYLOAD_SIZE ((uint32_t)0x00800000)
// Connection health (still in protocol category)
#define VAL_ERROR_DETAIL_EXCESSIVE_RETRIES ((uint32_t)0x01000000)

// Filesystem (24-27)
#define VAL_ERROR_DETAIL_FILE_NOT_FOUND ((uint32_t)0x01000000)
#define VAL_ERROR_DETAIL_FILE_LOCKED ((uint32_t)0x02000000)
#define VAL_ERROR_DETAIL_DISK_FULL ((uint32_t)0x04000000)
#define VAL_ERROR_DETAIL_PERMISSION ((uint32_t)0x08000000)

// Context (28-31) — payload selector (no strings). If CONTEXT == MISSING_FEATURES, lower 24 bits carry feature bitmask
#define VAL_ERROR_CONTEXT_SHIFT 28
#define VAL_ERROR_CONTEXT_NONE 0u
#define VAL_ERROR_CONTEXT_MISSING_FEATURES 1u

#define VAL_ERROR_CONTEXT(detail) (((uint32_t)(detail) & VAL_ERROR_DETAIL_CONTEXT_MASK) >> VAL_ERROR_CONTEXT_SHIFT)

// Helper: encode/decode missing features into detail (also mark FEATURE_MISSING in protocol category)
#define VAL_SET_MISSING_FEATURE(mask)                                                                                            \
    (((uint32_t)(VAL_ERROR_CONTEXT_MISSING_FEATURES) << VAL_ERROR_CONTEXT_SHIFT) | (uint32_t)((mask) & 0x00FFFFFFu) |            \
     (uint32_t)VAL_ERROR_DETAIL_FEATURE_MISSING)

#define VAL_GET_MISSING_FEATURE(detail)                                                                                          \
    ((VAL_ERROR_CONTEXT(detail) == VAL_ERROR_CONTEXT_MISSING_FEATURES)                                                           \
         ? (((detail) & 0x00FFFFFFu) & ~VAL_ERROR_DETAIL_PROTO_MASK)                                                             \
         : 0u)

// Category checkers
#define VAL_ERROR_IS_NETWORK_RELATED(detail) (((detail) & VAL_ERROR_DETAIL_NET_MASK) != 0)
#define VAL_ERROR_IS_CRC_RELATED(detail) (((detail) & VAL_ERROR_DETAIL_CRC_MASK) != 0)
#define VAL_ERROR_IS_PROTOCOL_RELATED(detail) (((detail) & VAL_ERROR_DETAIL_PROTO_MASK) != 0)
#define VAL_ERROR_IS_FILESYSTEM_RELATED(detail) (((detail) & VAL_ERROR_DETAIL_FS_MASK) != 0)

#endif // VAL_ERRORS_H