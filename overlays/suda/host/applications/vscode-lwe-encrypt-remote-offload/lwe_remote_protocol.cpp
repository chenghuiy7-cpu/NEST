#include "lwe_remote_protocol.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

namespace lwe_remote {
namespace {

constexpr char kMagic[] = "LWERPC01";
constexpr char kTimingMagic[] = "LWEBEN01";
constexpr uint64_t kVersion = 2;
constexpr uint64_t kTimingVersion = 1;
constexpr size_t kHeaderFields = 14;
constexpr size_t kHeaderBytes = 8 + (kHeaderFields + 1) * sizeof(uint64_t);
constexpr size_t kTimingFields = 15;
constexpr size_t kTimingBytes = 8 + kTimingFields * sizeof(uint64_t);
constexpr size_t kWireChunkBytes = 64 * 1024;
constexpr uint64_t kHpuNativeWordsPerLwe = 2 * 1536;

void set_error(std::string* error, const std::string& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

void store_le64(uint8_t* bytes, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint64_t load_le64(const uint8_t* bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

bool send_all(int fd, const void* data, size_t bytes, std::string* error)
{
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (bytes != 0) {
        ssize_t sent = send(fd, cursor, bytes, MSG_NOSIGNAL);
        if (sent > 0) {
            cursor += sent;
            bytes -= static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        set_error(
            error,
            "TCP send failed: " + std::string(strerror(sent < 0 ? errno : EPIPE)));
        return false;
    }
    return true;
}

bool receive_all(int fd, void* data, size_t bytes, std::string* error)
{
    uint8_t* cursor = static_cast<uint8_t*>(data);
    while (bytes != 0) {
        ssize_t received = recv(fd, cursor, bytes, 0);
        if (received > 0) {
            cursor += received;
            bytes -= static_cast<size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received == 0) {
            set_error(error, "TCP peer closed the connection early");
        } else {
            set_error(error, "TCP receive failed: " + std::string(strerror(errno)));
        }
        return false;
    }
    return true;
}

int connect_with_timeout(
    const std::string& host,
    uint16_t port,
    uint32_t timeout_ms,
    std::string* error)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addresses = nullptr;
    std::string service = std::to_string(port);
    int gai = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
    if (gai != 0) {
        set_error(error, "unable to resolve " + host + ": " + gai_strerror(gai));
        return -1;
    }

    int connected_fd = -1;
    std::string last_error = "no usable address";
    for (struct addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            last_error = strerror(errno);
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            last_error = strerror(errno);
            close(fd);
            continue;
        }

        int ret = connect(fd, address->ai_addr, address->ai_addrlen);
        if (ret != 0 && errno == EINPROGRESS) {
            struct pollfd poll_fd = {};
            poll_fd.fd = fd;
            poll_fd.events = POLLOUT;
            do {
                ret = poll(&poll_fd, 1, static_cast<int>(timeout_ms));
            } while (ret < 0 && errno == EINTR);

            if (ret > 0) {
                int socket_error = 0;
                socklen_t socket_error_size = sizeof(socket_error);
                if (getsockopt(
                        fd,
                        SOL_SOCKET,
                        SO_ERROR,
                        &socket_error,
                        &socket_error_size) == 0 &&
                    socket_error == 0) {
                    ret = 0;
                } else {
                    errno = socket_error == 0 ? ECONNREFUSED : socket_error;
                    ret = -1;
                }
            } else {
                errno = ret == 0 ? ETIMEDOUT : errno;
                ret = -1;
            }
        }

        if (ret == 0 && fcntl(fd, F_SETFL, flags) == 0) {
            connected_fd = fd;
            break;
        }

        last_error = strerror(errno);
        close(fd);
    }
    freeaddrinfo(addresses);

    if (connected_fd < 0) {
        set_error(
            error,
            "unable to connect to " + host + ":" + service + ": " + last_error);
    }
    return connected_fd;
}

bool configure_socket(
    int fd,
    uint32_t io_timeout_secs,
    std::string* error)
{
    struct timeval timeout = {};
    timeout.tv_sec = io_timeout_secs;
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        set_error(error, "unable to configure TCP socket: " + std::string(strerror(errno)));
        return false;
    }
    return true;
}

bool validate_metadata(
    const BatchMetadata& metadata,
    size_t payload_words,
    uint64_t operation,
    std::string* error)
{
    if (metadata.mask_dimension == 0 || metadata.item_count == 0 ||
        metadata.radix_blocks_per_item == 0) {
        set_error(error, "invalid zero-sized LWE/radix metadata");
        return false;
    }

    if (metadata.item_count >
        std::numeric_limits<uint64_t>::max() /
            metadata.radix_blocks_per_item) {
        set_error(error, "ciphertext shape overflow");
        return false;
    }
    uint64_t block_count =
        metadata.item_count * metadata.radix_blocks_per_item;
    if (metadata.mask_dimension == std::numeric_limits<uint64_t>::max() ||
        block_count >
            std::numeric_limits<uint64_t>::max() /
                (metadata.mask_dimension + 1)) {
        set_error(error, "ciphertext word count overflow");
        return false;
    }
    uint64_t expected_cpu_words =
        block_count * (metadata.mask_dimension + 1);
    if (block_count >
        std::numeric_limits<uint64_t>::max() / kHpuNativeWordsPerLwe) {
        set_error(error, "HPU-native ciphertext word count overflow");
        return false;
    }
    uint64_t expected_native_words = block_count * kHpuNativeWordsPerLwe;
    uint64_t expected_words = expected_cpu_words;
    if (operation == kOperationAddScalarU8HpuNative ||
        operation == kOperationAddScalarU8HpuNativeRoundTrip) {
        expected_words = expected_native_words;
    } else if (operation == kOperationEchoU8 &&
               metadata.ciphertext_word_count == expected_native_words) {
        expected_words = expected_native_words;
    }
    if (metadata.ciphertext_word_count != expected_words ||
        metadata.ciphertext_word_count != payload_words) {
        set_error(error, "ciphertext metadata/payload word count mismatch");
        return false;
    }
    return true;
}

bool send_words(
    int fd,
    const std::vector<uint64_t>& words,
    std::string* error)
{
    std::array<uint8_t, kWireChunkBytes> bytes = {};
    constexpr size_t words_per_chunk = kWireChunkBytes / sizeof(uint64_t);
    size_t offset = 0;
    while (offset < words.size()) {
        size_t count = std::min(words_per_chunk, words.size() - offset);
        for (size_t i = 0; i < count; ++i) {
            store_le64(bytes.data() + i * sizeof(uint64_t), words[offset + i]);
        }
        if (!send_all(fd, bytes.data(), count * sizeof(uint64_t), error)) {
            return false;
        }
        offset += count;
    }
    return true;
}

bool receive_words(
    int fd,
    size_t word_count,
    std::vector<uint64_t>* words,
    std::string* error)
{
    std::array<uint8_t, kWireChunkBytes> bytes = {};
    constexpr size_t words_per_chunk = kWireChunkBytes / sizeof(uint64_t);
    words->assign(word_count, 0);
    size_t offset = 0;
    while (offset < word_count) {
        size_t count = std::min(words_per_chunk, word_count - offset);
        if (!receive_all(fd, bytes.data(), count * sizeof(uint64_t), error)) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            (*words)[offset + i] =
                load_le64(bytes.data() + i * sizeof(uint64_t));
        }
        offset += count;
    }
    return true;
}

}  // namespace

bool BatchMetadata::operator==(const BatchMetadata& other) const
{
    return mask_dimension == other.mask_dimension &&
           item_count == other.item_count &&
           radix_blocks_per_item == other.radix_blocks_per_item &&
           message_width == other.message_width &&
           carry_width == other.carry_width &&
           padding_bit_width == other.padding_bit_width &&
           delta_log2 == other.delta_log2 &&
           ciphertext_word_count == other.ciphertext_word_count;
}

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
    std::string* error)
{
    if (result == nullptr) {
        set_error(error, "null RPC result");
        return false;
    }
    if (!validate_metadata(metadata, ciphertext_words.size(), operation, error)) {
        return false;
    }
    if (operation != kOperationEchoU8 &&
        operation != kOperationAddScalarU8 &&
        operation != kOperationAddScalarU8HpuNative &&
        operation != kOperationAddScalarU8HpuNativeRoundTrip) {
        set_error(error, "unsupported remote operation");
        return false;
    }
    if (ciphertext_words.size() >
        std::numeric_limits<uint64_t>::max() / sizeof(uint64_t)) {
        set_error(error, "request payload byte count overflow");
        return false;
    }

    auto connect_start = std::chrono::steady_clock::now();
    int fd = connect_with_timeout(host, port, connect_timeout_ms, error);
    if (fd < 0) {
        return false;
    }
    result->connect_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - connect_start)
            .count();
    if (!configure_socket(fd, io_timeout_secs, error)) {
        close(fd);
        return false;
    }

    std::array<uint8_t, kHeaderBytes> header = {};
    memcpy(header.data(), kMagic, 8);
    const uint64_t fields[kHeaderFields] = {
        kVersion,
        kFrameRequest,
        request_id,
        operation,
        0,
        scalar,
        metadata.mask_dimension,
        metadata.item_count,
        metadata.radix_blocks_per_item,
        metadata.message_width,
        metadata.carry_width,
        metadata.padding_bit_width,
        metadata.delta_log2,
        metadata.ciphertext_word_count,
    };
    for (size_t i = 0; i < kHeaderFields; ++i) {
        store_le64(header.data() + 8 + i * sizeof(uint64_t), fields[i]);
    }
    store_le64(
        header.data() + 8 + kHeaderFields * sizeof(uint64_t),
        ciphertext_words.size() * sizeof(uint64_t));

    auto rpc_start = std::chrono::steady_clock::now();
    auto send_start = std::chrono::steady_clock::now();
    bool sent =
        send_all(fd, header.data(), header.size(), error) &&
        send_words(fd, ciphertext_words, error);
    result->request_send_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - send_start)
            .count();
    if (!sent) {
        close(fd);
        return false;
    }

    std::array<uint8_t, kHeaderBytes> response_header = {};
    auto wait_header_start = std::chrono::steady_clock::now();
    if (!receive_all(fd, response_header.data(), response_header.size(), error)) {
        close(fd);
        return false;
    }
    result->wait_response_header_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wait_header_start)
            .count();
    if (memcmp(response_header.data(), kMagic, 8) != 0) {
        set_error(error, "invalid LWERPC01 response magic");
        close(fd);
        return false;
    }

    uint64_t response_fields[kHeaderFields] = {};
    for (size_t i = 0; i < kHeaderFields; ++i) {
        response_fields[i] =
            load_le64(response_header.data() + 8 + i * sizeof(uint64_t));
    }
    uint64_t payload_bytes =
        load_le64(response_header.data() + 8 + kHeaderFields * sizeof(uint64_t));

    if (response_fields[0] != kVersion) {
        set_error(error, "unsupported LWERPC01 response version");
        close(fd);
        return false;
    }
    if (payload_bytes > max_response_bytes) {
        set_error(error, "remote response exceeds --max-response-bytes");
        close(fd);
        return false;
    }
    if (response_fields[1] == kFrameError) {
        std::string message(payload_bytes, '\0');
        if (!message.empty() &&
            !receive_all(fd, &message[0], message.size(), error)) {
            close(fd);
            return false;
        }
        set_error(error, "remote HPU error: " + message);
        close(fd);
        return false;
    }
    if (response_fields[1] != kFrameResponse) {
        set_error(error, "unexpected LWERPC01 response frame kind");
        close(fd);
        return false;
    }
    if (response_fields[2] != request_id ||
        response_fields[3] != operation ||
        response_fields[4] != 0 ||
        response_fields[5] != scalar) {
        set_error(error, "response request/operation/scalar/status mismatch");
        close(fd);
        return false;
    }

    BatchMetadata response_metadata = {
        response_fields[6],
        response_fields[7],
        response_fields[8],
        response_fields[9],
        response_fields[10],
        response_fields[11],
        response_fields[12],
        response_fields[13],
    };
    BatchMetadata expected_response_metadata = metadata;
    if (operation == kOperationAddScalarU8HpuNative) {
        expected_response_metadata.ciphertext_word_count =
            metadata.item_count * metadata.radix_blocks_per_item *
            (metadata.mask_dimension + 1);
    }
    if (!(response_metadata == expected_response_metadata)) {
        set_error(error, "remote response changed ciphertext metadata");
        close(fd);
        return false;
    }
    if (payload_bytes !=
        response_metadata.ciphertext_word_count * sizeof(uint64_t)) {
        set_error(error, "remote response payload byte count mismatch");
        close(fd);
        return false;
    }
    auto response_receive_start = std::chrono::steady_clock::now();
    if (!receive_words(
            fd,
            static_cast<size_t>(response_metadata.ciphertext_word_count),
            &result->ciphertext_words,
            error)) {
        close(fd);
        return false;
    }
    result->response_receive_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - response_receive_start)
            .count();

    std::array<uint8_t, kTimingBytes> timing = {};
    auto telemetry_receive_start = std::chrono::steady_clock::now();
    if (!receive_all(fd, timing.data(), timing.size(), error)) {
        close(fd);
        return false;
    }
    result->telemetry_receive_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - telemetry_receive_start)
            .count();
    if (memcmp(timing.data(), kTimingMagic, 8) != 0) {
        set_error(error, "invalid LWEBEN01 timing magic");
        close(fd);
        return false;
    }
    uint64_t timing_fields[kTimingFields] = {};
    for (size_t i = 0; i < kTimingFields; ++i) {
        timing_fields[i] = load_le64(timing.data() + 8 + i * sizeof(uint64_t));
    }
    if (timing_fields[0] != kTimingVersion || timing_fields[1] != request_id) {
        set_error(error, "timing trailer version/request_id mismatch");
        close(fd);
        return false;
    }
    auto ns_to_ms = [](uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / 1000000.0;
    };
    result->server_request_receive_ms = ns_to_ms(timing_fields[2]);
    result->server_request_validate_ms = ns_to_ms(timing_fields[3]);
    result->server_request_decode_ms = ns_to_ms(timing_fields[4]);
    result->server_hpu_prepare_ms = ns_to_ms(timing_fields[5]);
    result->server_hpu_enqueue_ms = ns_to_ms(timing_fields[6]);
    result->server_hpu_wait_sync_ms = ns_to_ms(timing_fields[7]);
    result->server_hpu_output_convert_ms = ns_to_ms(timing_fields[8]);
    result->server_result_encode_ms = ns_to_ms(timing_fields[9]);
    result->server_mem_sanitizer_ms = ns_to_ms(timing_fields[10]);
    result->server_process_ms = ns_to_ms(timing_fields[11]);
    result->server_response_send_ms = ns_to_ms(timing_fields[12]);
    result->server_total_ms = ns_to_ms(timing_fields[13]);
    result->server_response_payload_bytes = timing_fields[14];
    if (result->server_response_payload_bytes != payload_bytes) {
        set_error(error, "timing trailer response payload size mismatch");
        close(fd);
        return false;
    }
    close(fd);

    result->round_trip_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - rpc_start)
            .count();
    result->metadata = response_metadata;
    result->request_id = response_fields[2];
    result->operation = response_fields[3];
    result->scalar = response_fields[5];
    return true;
}

}  // namespace lwe_remote
