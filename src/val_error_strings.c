#include "val_error_strings.h"

#if VAL_ENABLE_ERROR_STRINGS

#include <stdio.h>

const char *val_status_to_string(val_status_t status)
{
    switch (status)
    {
    case VAL_OK:
        return "OK";
    case VAL_SKIPPED:
        return "SKIPPED";
    case VAL_ERR_INVALID_ARG:
        return "INVALID_ARG";
    case VAL_ERR_NO_MEMORY:
        return "NO_MEMORY";
    case VAL_ERR_IO:
        return "IO_ERROR";
    case VAL_ERR_TIMEOUT:
        return "TIMEOUT";
    case VAL_ERR_PROTOCOL:
        return "PROTOCOL_ERROR";
    case VAL_ERR_CRC:
        return "CRC_ERROR";
    case VAL_ERR_RESUME_VERIFY:
        return "RESUME_VERIFY_FAIL";
    case VAL_ERR_INCOMPATIBLE_VERSION:
        return "INCOMPATIBLE_VERSION";
    case VAL_ERR_PACKET_SIZE_MISMATCH:
        return "PACKET_SIZE_MISMATCH";
    case VAL_ERR_FEATURE_NEGOTIATION:
        return "FEATURE_NEGOTIATION";
    case VAL_ERR_ABORTED:
        return "ABORTED";
    case VAL_ERR_MODE_NEGOTIATION_FAILED:
        return "MODE_NEGOTIATION_FAILED";
    case VAL_ERR_UNSUPPORTED_TX_MODE:
        return "UNSUPPORTED_TX_MODE";
    case VAL_ERR_PERFORMANCE:
        return "PERFORMANCE_UNACCEPTABLE";
    default:
        return "UNKNOWN";
    }
}

static void append_flag(char *buf, size_t cap, size_t *len, const char *s)
{
    if (*len >= cap)
        return;
    int n = 0;
#if defined(_MSC_VER)
    n = _snprintf_s(buf + *len, cap - *len, _TRUNCATE, "%s%s", (*len ? "|" : ""), s);
    if (n < 0)
        n = (int)(cap - *len);
#else
    n = snprintf(buf + *len, cap - *len, "%s%s", (*len ? "|" : ""), s);
#endif
    if (n > 0)
        *len += (size_t)n;
}

const char *val_error_detail_to_string(uint32_t d)
{
    // Thread-safe: use thread-local static if available, else a static small ring
#if defined(_MSC_VER)
    __declspec(thread) static char tls[256];
#else
    static __thread char tls[256];
#endif
    tls[0] = '\0';
    size_t len = 0;
    (void)len;
    // Network
    if (d & VAL_ERROR_DETAIL_NETWORK_RESET)
        append_flag(tls, sizeof(tls), &len, "NETWORK_RESET");
    if (d & VAL_ERROR_DETAIL_TIMEOUT_ACK)
        append_flag(tls, sizeof(tls), &len, "TIMEOUT_ACK");
    if (d & VAL_ERROR_DETAIL_TIMEOUT_DATA)
        append_flag(tls, sizeof(tls), &len, "TIMEOUT_DATA");
    if (d & VAL_ERROR_DETAIL_TIMEOUT_META)
        append_flag(tls, sizeof(tls), &len, "TIMEOUT_META");
    if (d & VAL_ERROR_DETAIL_TIMEOUT_HELLO)
        append_flag(tls, sizeof(tls), &len, "TIMEOUT_HELLO");
    if (d & VAL_ERROR_DETAIL_SEND_FAILED)
        append_flag(tls, sizeof(tls), &len, "SEND_FAILED");
    if (d & VAL_ERROR_DETAIL_RECV_FAILED)
        append_flag(tls, sizeof(tls), &len, "RECV_FAILED");
    if (d & VAL_ERROR_DETAIL_CONNECTION)
        append_flag(tls, sizeof(tls), &len, "CONNECTION");
    // CRC
    if (d & VAL_ERROR_DETAIL_CRC_HEADER)
        append_flag(tls, sizeof(tls), &len, "CRC_HEADER");
    if (d & VAL_ERROR_DETAIL_CRC_TRAILER)
        append_flag(tls, sizeof(tls), &len, "CRC_TRAILER");
    if (d & VAL_ERROR_DETAIL_CRC_RESUME)
        append_flag(tls, sizeof(tls), &len, "CRC_RESUME");
    if (d & VAL_ERROR_DETAIL_SIZE_MISMATCH)
        append_flag(tls, sizeof(tls), &len, "SIZE_MISMATCH");
    if (d & VAL_ERROR_DETAIL_PACKET_CORRUPT)
        append_flag(tls, sizeof(tls), &len, "PACKET_CORRUPT");
    if (d & VAL_ERROR_DETAIL_SEQ_ERROR)
        append_flag(tls, sizeof(tls), &len, "SEQ_ERROR");
    if (d & VAL_ERROR_DETAIL_OFFSET_ERROR)
        append_flag(tls, sizeof(tls), &len, "OFFSET_ERROR");
    // Protocol
    if (d & VAL_ERROR_DETAIL_VERSION)
        append_flag(tls, sizeof(tls), &len, "VERSION");
    if (d & VAL_ERROR_DETAIL_PACKET_SIZE)
        append_flag(tls, sizeof(tls), &len, "PACKET_SIZE");
    if (d & VAL_ERROR_DETAIL_FEATURE_MISSING)
        append_flag(tls, sizeof(tls), &len, "FEATURE_MISSING");
    if (d & VAL_ERROR_DETAIL_INVALID_STATE)
        append_flag(tls, sizeof(tls), &len, "INVALID_STATE");
    if (d & VAL_ERROR_DETAIL_MALFORMED_PKT)
        append_flag(tls, sizeof(tls), &len, "MALFORMED_PKT");
    if (d & VAL_ERROR_DETAIL_UNKNOWN_TYPE)
        append_flag(tls, sizeof(tls), &len, "UNKNOWN_TYPE");
    if (d & VAL_ERROR_DETAIL_PAYLOAD_SIZE)
        append_flag(tls, sizeof(tls), &len, "PAYLOAD_SIZE");
    if (d & VAL_ERROR_DETAIL_EXCESSIVE_RETRIES)
        append_flag(tls, sizeof(tls), &len, "EXCESSIVE_RETRIES");
    // Filesystem
    if (d & VAL_ERROR_DETAIL_FILE_NOT_FOUND)
        append_flag(tls, sizeof(tls), &len, "FILE_NOT_FOUND");
    if (d & VAL_ERROR_DETAIL_FILE_LOCKED)
        append_flag(tls, sizeof(tls), &len, "FILE_LOCKED");
    if (d & VAL_ERROR_DETAIL_DISK_FULL)
        append_flag(tls, sizeof(tls), &len, "DISK_FULL");
    if (d & VAL_ERROR_DETAIL_PERMISSION)
        append_flag(tls, sizeof(tls), &len, "PERMISSION");

    // Context
    if (VAL_ERROR_CONTEXT(d) == VAL_ERROR_CONTEXT_MISSING_FEATURES)
    {
        unsigned mf = VAL_GET_MISSING_FEATURE(d);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "MISSING_FEATURES=0x%06X", mf);
        append_flag(tls, sizeof(tls), &len, tmp);
    }
    if (len == 0)
        return "NONE";
    return tls;
}

int val_analyze_error(val_status_t code, uint32_t detail, val_error_info_t *info)
{
    if (!info)
        return -1;
    info->category = "GENERAL";
    info->description = val_status_to_string(code);
    info->suggestion = "Check logs and detail mask";
    if (VAL_ERROR_IS_NETWORK_RELATED(detail))
    {
        info->category = "NETWORK";
        info->suggestion = "Verify connectivity, timeouts, and transport reliability";
    }
    else if (VAL_ERROR_IS_CRC_RELATED(detail))
    {
        info->category = "INTEGRITY";
        info->suggestion = "Investigate data corruption or resume window size";
    }
    else if (VAL_ERROR_IS_PROTOCOL_RELATED(detail))
    {
        info->category = "PROTOCOL";
        if (VAL_ERROR_CONTEXT(detail) == VAL_ERROR_CONTEXT_MISSING_FEATURES)
            info->suggestion = "Adjust required/requested features or upgrade peer";
        else
            info->suggestion = "Validate version/packet size and payload formatting";
    }
    else if (VAL_ERROR_IS_FILESYSTEM_RELATED(detail))
    {
        info->category = "FILESYSTEM";
        info->suggestion = "Check file path, permissions, and disk space";
    }
    return 0;
}

void val_format_error_report(val_status_t code, uint32_t detail, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0)
        return;
    const char *st = val_status_to_string(code);
    const char *dm = val_error_detail_to_string(detail);
#if defined(_MSC_VER)
    _snprintf_s(buffer, buffer_size, _TRUNCATE, "%s (%d); %s (0x%08X)", st, (int)code, dm, detail);
#else
    snprintf(buffer, buffer_size, "%s (%d); %s (0x%08X)", st, (int)code, dm, detail);
#endif
}

#endif // VAL_ENABLE_ERROR_STRINGS
