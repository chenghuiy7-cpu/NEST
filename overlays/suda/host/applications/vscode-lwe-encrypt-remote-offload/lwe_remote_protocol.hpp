#ifndef LWE_REMOTE_PROTOCOL_HPP
#define LWE_REMOTE_PROTOCOL_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace lwe_remote {

constexpr uint64_t kFrameRequest = 1;
constexpr uint64_t kFrameResponse = 2;
constexpr uint64_t kFrameError = 3;
constexpr uint64_t kOperationEchoU8 = 0;
constexpr uint64_t kOperationAddScalarU8 = 1;
constexpr uint64_t kOperationAddScalarU8HpuNative = 2;
constexpr uint64_t kOperationAddScalarU8HpuNativeRoundTrip = 3;

struct BatchMetadata {
    uint64_t mask_dimension;
    uint64_t item_count;
    uint64_t radix_blocks_per_item;
    uint64_t message_width;
    uint64_t carry_width;
    uint64_t padding_bit_width;
    uint64_t delta_log2;
    uint64_t ciphertext_word_count;

    bool operator==(const BatchMetadata& other) const;
};

struct RpcResult {
    BatchMetadata metadata = {};
    std::vector<uint64_t> ciphertext_words;
    uint64_t request_id = 0;
    uint64_t operation = 0;
    uint64_t scalar = 0;
    double connect_ms = 0.0;
    double request_send_ms = 0.0;
    double wait_response_header_ms = 0.0;
    double response_receive_ms = 0.0;
    double telemetry_receive_ms = 0.0;
    double round_trip_ms = 0.0;
    double server_request_receive_ms = 0.0;
    double server_request_validate_ms = 0.0;
    double server_request_decode_ms = 0.0;
    double server_hpu_prepare_ms = 0.0;
    double server_hpu_enqueue_ms = 0.0;
    double server_hpu_wait_sync_ms = 0.0;
    double server_hpu_output_convert_ms = 0.0;
    double server_result_encode_ms = 0.0;
    double server_mem_sanitizer_ms = 0.0;
    double server_process_ms = 0.0;
    double server_response_send_ms = 0.0;
    double server_total_ms = 0.0;
    uint64_t server_response_payload_bytes = 0;
};

bool compute_u8(
    const std::string& host,
    uint16_t port,
    uint64_t request_id,
    uint64_t operation,
    uint8_t scalar,
    const BatchMetadata& metadata,
    const std::vector<uint64_t>& ciphertext_words,
    uint32_t connect_timeout_ms,
    uint32_t io_timeout_secs,
    size_t max_response_bytes,
    RpcResult* result,
    std::string* error);

}  // namespace lwe_remote

#endif
