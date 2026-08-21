#include "lwe_remote_protocol.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kHeaderFields = 14;
constexpr size_t kHeaderBytes = 8 + (kHeaderFields + 1) * sizeof(uint64_t);
constexpr size_t kTimingFields = 15;
constexpr size_t kTimingBytes = 8 + kTimingFields * sizeof(uint64_t);

uint64_t load_le64(const uint8_t* bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

void store_le64(uint8_t* bytes, uint64_t value)
{
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

bool read_all(int fd, void* data, size_t bytes)
{
    uint8_t* cursor = static_cast<uint8_t*>(data);
    while (bytes != 0) {
        ssize_t result = recv(fd, cursor, bytes, 0);
        if (result <= 0) {
            return false;
        }
        cursor += result;
        bytes -= static_cast<size_t>(result);
    }
    return true;
}

bool write_all(int fd, const void* data, size_t bytes)
{
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    while (bytes != 0) {
        ssize_t result = send(fd, cursor, bytes, 0);
        if (result <= 0) {
            return false;
        }
        cursor += result;
        bytes -= static_cast<size_t>(result);
    }
    return true;
}

bool serve_one(int listen_fd, uint64_t expected_operation)
{
    int fd = accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
        return false;
    }

    std::array<uint8_t, kHeaderBytes> header = {};
    if (!read_all(fd, header.data(), header.size()) ||
        memcmp(header.data(), "LWERPC01", 8) != 0) {
        close(fd);
        return false;
    }

    uint64_t fields[kHeaderFields] = {};
    for (size_t i = 0; i < kHeaderFields; ++i) {
        fields[i] = load_le64(header.data() + 8 + i * 8);
    }
    uint64_t payload_bytes =
        load_le64(header.data() + 8 + kHeaderFields * 8);
    bool valid =
        fields[0] == 2 &&
        fields[1] == lwe_remote::kFrameRequest &&
        fields[3] == expected_operation &&
        fields[4] == 0 &&
        fields[5] == 7 &&
        payload_bytes == fields[13] * sizeof(uint64_t);
    if (!valid) {
        close(fd);
        return false;
    }

    std::vector<uint8_t> payload(payload_bytes);
    if (!read_all(fd, payload.data(), payload.size())) {
        close(fd);
        return false;
    }
    for (uint64_t i = 0; i < fields[13]; ++i) {
        if (load_le64(payload.data() + i * 8) != i) {
            close(fd);
            return false;
        }
        store_le64(payload.data() + i * 8, i + 100);
    }

    fields[1] = lwe_remote::kFrameResponse;
    for (size_t i = 0; i < kHeaderFields; ++i) {
        store_le64(header.data() + 8 + i * 8, fields[i]);
    }
    std::array<uint8_t, kTimingBytes> timing = {};
    memcpy(timing.data(), "LWEBEN01", 8);
    uint64_t timing_fields[kTimingFields] = {
        1,
        fields[2],
        1000000,
        2000000,
        3000000,
        4000000,
        5000000,
        6000000,
        7000000,
        8000000,
        9000000,
        10000000,
        11000000,
        12000000,
        payload_bytes,
    };
    for (size_t i = 0; i < kTimingFields; ++i) {
        store_le64(timing.data() + 8 + i * 8, timing_fields[i]);
    }
    bool result =
        write_all(fd, header.data(), header.size()) &&
        write_all(fd, payload.data(), payload.size()) &&
        write_all(fd, timing.data(), timing.size());
    close(fd);
    return result;
}

}  // namespace

int main()
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(
            listen_fd,
            reinterpret_cast<struct sockaddr*>(&address),
            sizeof(address)) != 0 ||
        listen(listen_fd, 1) != 0) {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(
            listen_fd,
            reinterpret_cast<struct sockaddr*>(&address),
        &address_size) != 0) {
        perror("getsockname");
        close(listen_fd);
        return 1;
    }

    bool server_ok = false;
    std::thread server([&]() {
        server_ok =
            serve_one(listen_fd, lwe_remote::kOperationAddScalarU8) &&
            serve_one(
                listen_fd,
                lwe_remote::kOperationAddScalarU8HpuNativeRoundTrip);
    });

    lwe_remote::BatchMetadata metadata = {
        3,
        2,
        2,
        4,
        1,
        1,
        58,
        16,
    };
    std::vector<uint64_t> request_words(16);
    for (uint64_t i = 0; i < request_words.size(); ++i) {
        request_words[i] = i;
    }

    lwe_remote::RpcResult result;
    std::string error;
    bool client_ok = lwe_remote::compute_u8(
        "127.0.0.1",
        ntohs(address.sin_port),
        42,
        lwe_remote::kOperationAddScalarU8,
        7,
        metadata,
        request_words,
        1000,
        5,
        4096,
        &result,
        &error);
    bool cpu_result_ok =
        client_ok && result.request_id == 42 &&
        result.ciphertext_words.size() == 16 &&
        result.server_request_receive_ms == 1.0 &&
        result.server_total_ms == 12.0 &&
        result.server_response_payload_bytes == 128;
    for (uint64_t i = 0; i < result.ciphertext_words.size(); ++i) {
        if (result.ciphertext_words[i] != i + 100) {
            cpu_result_ok = false;
            break;
        }
    }

    lwe_remote::BatchMetadata native_metadata = {
        3,
        1,
        1,
        4,
        1,
        1,
        58,
        2 * 1536,
    };
    std::vector<uint64_t> native_request_words(
        native_metadata.ciphertext_word_count);
    for (uint64_t i = 0; i < native_request_words.size(); ++i) {
        native_request_words[i] = i;
    }
    lwe_remote::RpcResult native_result;
    bool native_client_ok = lwe_remote::compute_u8(
        "127.0.0.1",
        ntohs(address.sin_port),
        43,
        lwe_remote::kOperationAddScalarU8HpuNativeRoundTrip,
        7,
        native_metadata,
        native_request_words,
        1000,
        5,
        64 * 1024,
        &native_result,
        &error);
    server.join();
    close(listen_fd);
    if (!cpu_result_ok) {
        fprintf(stderr, "protocol test failed: %s\n", error.c_str());
        return 1;
    }
    if (!native_client_ok || !server_ok || native_result.request_id != 43 ||
        native_result.metadata.ciphertext_word_count != 2 * 1536 ||
        native_result.ciphertext_words.size() != 2 * 1536) {
        fprintf(stderr, "native round-trip protocol test failed: %s\n", error.c_str());
        return 1;
    }
    for (uint64_t i = 0; i < native_result.ciphertext_words.size(); ++i) {
        if (native_result.ciphertext_words[i] != i + 100) {
            fprintf(stderr, "native protocol response mismatch at %llu\n",
                    static_cast<unsigned long long>(i));
            return 1;
        }
    }

    printf("lwe_remote_protocol_test=passed\n");
    printf("hpu_native_roundtrip_protocol_test=passed\n");
    printf("wire_magic=LWERPC01 header_bytes=%zu payload_bytes=128\n",
           kHeaderBytes);
    return 0;
}
