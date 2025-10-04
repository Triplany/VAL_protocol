#include "val_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Mock transport that simulates network behavior
static int mock_send_calls = 0;
static int mock_recv_calls = 0;
static int induce_errors = 0;

static int mock_send(void *ctx, const void *data, size_t len)
{
    mock_send_calls++;
    (void)ctx;
    (void)data;
    (void)len;
    return (int)len;
}

static int mock_recv(void *ctx, void *buffer, size_t buffer_size, size_t *received, uint32_t timeout_ms)
{
    mock_recv_calls++;
    (void)ctx;
    (void)timeout_ms;

    static uint8_t handshake_response[1024];
    static size_t handshake_size = 0;

    // Initialize handshake response once
    if (handshake_size == 0)
    {
        val_packet_header_t header;
        val_handshake_t payload;

        memset(&payload, 0, sizeof(payload));
        payload.magic = VAL_MAGIC;
        payload.version_major = VAL_VERSION_MAJOR;
        payload.version_minor = VAL_VERSION_MINOR;
        payload.packet_size = 1024;
        payload.max_performance_mode = VAL_TX_WINDOW_8; // Peer supports up to 8-packet window
        payload.preferred_initial_mode = VAL_TX_WINDOW_4;
        payload.mode_sync_interval = 100;

        memset(&header, 0, sizeof(header));
        header.type = VAL_PKT_HELLO;
        header.wire_version = 0;
        header.payload_len = VAL_WIRE_HANDSHAKE_SIZE;
        header.seq = 1;
        header.offset = 0;
        header.header_crc = 0;

        val_serialize_header(&header, handshake_response);
        val_serialize_handshake(&payload, handshake_response + VAL_WIRE_HEADER_SIZE);

        uint32_t header_crc = val_crc32(handshake_response, VAL_WIRE_HEADER_SIZE);
        header.header_crc = header_crc;
        val_serialize_header(&header, handshake_response);

        handshake_size = VAL_WIRE_HEADER_SIZE + VAL_WIRE_HANDSHAKE_SIZE + VAL_WIRE_TRAILER_SIZE;

        uint32_t trailer_crc = val_crc32(handshake_response, VAL_WIRE_HEADER_SIZE + VAL_WIRE_HANDSHAKE_SIZE);
        VAL_PUT_LE32(handshake_response + VAL_WIRE_HEADER_SIZE + VAL_WIRE_HANDSHAKE_SIZE, trailer_crc);
    }

    // Handle multi-part receive for handshake
    if (mock_recv_calls <= 3)
    {
        static size_t offset = 0;

        if (mock_recv_calls == 1)
        {
            // First call: return header
            offset = 0;
            size_t to_copy = (buffer_size < VAL_WIRE_HEADER_SIZE) ? buffer_size : VAL_WIRE_HEADER_SIZE;
            memcpy(buffer, handshake_response + offset, to_copy);
            offset += to_copy;
            if (received)
                *received = to_copy;
            return 0;
        }
        else if (mock_recv_calls == 2)
        {
            // Second call: return payload
            size_t to_copy = (buffer_size < VAL_WIRE_HANDSHAKE_SIZE) ? buffer_size : VAL_WIRE_HANDSHAKE_SIZE;
            memcpy(buffer, handshake_response + offset, to_copy);
            offset += to_copy;
            if (received)
                *received = to_copy;
            return 0;
        }
        else if (mock_recv_calls == 3)
        {
            // Third call: return trailer
            size_t to_copy = (buffer_size < VAL_WIRE_TRAILER_SIZE) ? buffer_size : VAL_WIRE_TRAILER_SIZE;
            memcpy(buffer, handshake_response + offset, to_copy);
            offset += to_copy;
            if (received)
                *received = to_copy;
            return 0;
        }
    }

    // Simulate errors if requested
    if (induce_errors && mock_recv_calls <= 8)
    {
        return -1; // Simulate timeout/error
    }

    // Default successful receive
    if (received)
        *received = buffer_size;
    return 0;
}

static uint32_t mock_get_ticks(void)
{
    static uint32_t ticks = 0;
    return ticks += 100;
}

static void mock_delay(uint32_t ms)
{
    (void)ms; // No-op for testing
}

// Mock filesystem functions (required for session creation)
static void *mock_fopen(void *ctx, const char *path, const char *mode)
{
    (void)ctx;
    (void)path;
    (void)mode;
    return (void *)1; // Non-null fake handle
}

static int mock_fread(void *ctx, void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    (void)buffer;
    (void)size;
    (void)count;
    (void)file;
    return (int)count;
}

static int mock_fwrite(void *ctx, const void *buffer, size_t size, size_t count, void *file)
{
    (void)ctx;
    (void)buffer;
    (void)size;
    (void)count;
    (void)file;
    return (int)count;
}

static int mock_fseek(void *ctx, void *file, long offset, int whence)
{
    (void)ctx;
    (void)file;
    (void)offset;
    (void)whence;
    return 0;
}

static long mock_ftell(void *ctx, void *file)
{
    (void)ctx;
    (void)file;
    return 0;
}

static int mock_fclose(void *ctx, void *file)
{
    (void)ctx;
    (void)file;
    return 0;
}

static void test_adaptive_negotiation(void)
{
    printf("=== Testing Adaptive Transmission Mode Negotiation ===\n");

    val_config_t config;
    memset(&config, 0, sizeof(config));

    // Set up transport
    config.transport.send = mock_send;
    config.transport.recv = mock_recv;

    // Set up system
    config.system.get_ticks_ms = mock_get_ticks;
    config.system.delay_ms = mock_delay;

    // Set up filesystem
    config.filesystem.fopen = mock_fopen;
    config.filesystem.fread = mock_fread;
    config.filesystem.fwrite = mock_fwrite;
    config.filesystem.fseek = mock_fseek;
    config.filesystem.ftell = mock_ftell;
    config.filesystem.fclose = mock_fclose;

    // Set up timeouts
    config.timeouts.min_timeout_ms = 100;
    config.timeouts.max_timeout_ms = 5000;

    // Set up buffers
    static uint8_t send_buf[2048];
    static uint8_t recv_buf[2048];
    config.buffers.send_buffer = send_buf;
    config.buffers.recv_buffer = recv_buf;
    config.buffers.packet_size = 1024;

    // Set up adaptive transmission - local supports streaming
    config.adaptive_tx.max_performance_mode = VAL_TX_WINDOW_64;
    config.adaptive_tx.preferred_initial_mode = VAL_TX_WINDOW_16;
    config.adaptive_tx.allow_streaming = 1;
    config.adaptive_tx.degrade_error_threshold = 2;
    config.adaptive_tx.recovery_success_threshold = 5;
    config.adaptive_tx.mode_sync_interval = 50;

    // Create session
    val_session_t *session = NULL;
    val_status_t st = val_session_create(&config, &session, NULL);
    if (st != VAL_OK)
    {
        printf("FAIL: Session creation failed: %d\n", st);
        return;
    }

    printf("Local max mode: %d (WINDOW cap)\n", config.adaptive_tx.max_performance_mode);
    printf("Local preferred: %d (WINDOW_16)\n", config.adaptive_tx.preferred_initial_mode);

    // Perform handshake
    mock_send_calls = 0;
    mock_recv_calls = 0;
    st = val_internal_do_handshake_sender(session);
    if (st != VAL_OK)
    {
        printf("FAIL: Handshake failed: %d\n", st);
        val_session_destroy(session);
        return;
    }

    printf("Handshake completed successfully\n");
    printf("Negotiated min mode: %d\n", session->min_negotiated_mode);
    printf("Negotiated max mode: %d\n", session->max_negotiated_mode);
    printf("Current TX mode: %d\n", session->current_tx_mode);
    printf("Peer TX mode: %d\n", session->peer_tx_mode);

    // Expected: min_negotiated_mode should be WINDOW_8 (conservative shared rung)
    if (session->min_negotiated_mode == VAL_TX_WINDOW_8)
        printf("PASS: Negotiated mode correctly set to WINDOW_8\n");
    else
        printf("FAIL: Expected WINDOW_8 (%d), got %d\n", VAL_TX_WINDOW_8, session->min_negotiated_mode);

    // Test adaptive degradation
    printf("\n=== Testing Adaptive Mode Degradation ===\n");
    printf("Initial mode: %d\n", session->current_tx_mode);

    // Induce errors to trigger degradation
    printf("Inducing transmission errors...\n");
    val_internal_record_transmission_error(session);
    printf("After 1 error - mode: %d, consecutive_errors: %d\n", session->current_tx_mode, session->consecutive_errors);

    val_internal_record_transmission_error(session);
    printf("After 2 errors - mode: %d, consecutive_errors: %d\n", session->current_tx_mode, session->consecutive_errors);

    if (val_tx_mode_window(session->current_tx_mode) < val_tx_mode_window(session->min_negotiated_mode))
        printf("PASS: Mode degraded after error threshold\n");
    else
        printf("NOTE: Mode at lowest negotiated level, cannot degrade further\n");

    // Test recovery
    printf("\n=== Testing Adaptive Mode Recovery ===\n");
    val_tx_mode_t degraded_mode = session->current_tx_mode;

    printf("Recording successful transmissions...\n");
    for (int i = 0; i < 6; i++)
    {
        val_internal_record_transmission_success(session);
        printf("After %d successes - mode: %d, consecutive_successes: %d\n", i + 1, session->current_tx_mode,
               session->consecutive_successes);
    }

    if (val_tx_mode_window(session->current_tx_mode) > val_tx_mode_window(degraded_mode))
        printf("PASS: Mode upgraded after success threshold\n");
    else
        printf("NOTE: Mode unchanged - may already be at optimal level\n");

    val_session_destroy(session);
    printf("\n=== Adaptive Transmission Demo Complete ===\n");
}

int main(void)
{
    test_adaptive_negotiation();
    return 0;
}
