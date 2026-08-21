#include <libnvme.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr size_t kLbaSize = 4096;
constexpr size_t kAxisBytes = 64;
constexpr uint32_t kComputeNsid = 2;
constexpr uint32_t kMaskDimension = 2048;
constexpr uint32_t kRadixBlockCount = 4;
constexpr uint32_t kMessageWidth = 2;
constexpr uint32_t kCarryWidth = 2;
constexpr uint32_t kPaddingWidth = 1;
constexpr uint32_t kDeltaLog2 = 59;
constexpr uint64_t kDelta = uint64_t{1} << kDeltaLog2;
constexpr uint32_t kNoiseBoundLog2 = 17;
constexpr uint32_t kInputModeU8Radix = 2;
constexpr uint32_t kNoiseModeInternal = 0;
constexpr uint32_t kNoiseModeZero = 2;
constexpr uint32_t kOutputLayoutCpuLwe = 0;
constexpr uint32_t kOutputLayoutHpuNative = 1;
constexpr uint32_t kDefaultPlaintextBytes = 1;
constexpr uint32_t kMaxCopyLbasPerRange = 32;
constexpr size_t kDefaultSlmReadChunkBytes = 128 * 1024;
constexpr size_t kMaxSlmReadChunkBytes = 128 * 1024;
constexpr int kSlmReadEintrMaxRetries = 16;
constexpr size_t kLogicalWordsPerCiphertext = kMaskDimension + 1;
constexpr size_t kPacketsPerCiphertext = (kMaskDimension / 8) + 1;
constexpr size_t kPhysicalWordsPerCiphertext = kPacketsPerCiphertext * 8;
constexpr size_t kOutputDonePacketBytes = kAxisBytes;
constexpr size_t kLogicalOutputBytes =
    kRadixBlockCount * kLogicalWordsPerCiphertext * sizeof(uint64_t);
constexpr size_t kPhysicalOutputBytes =
    kRadixBlockCount * kPacketsPerCiphertext * kAxisBytes;
constexpr size_t kHpuPcCount = 2;
constexpr size_t kHpuPcGroupWords = 16;
constexpr size_t kHpuPcDataWords = kMaskDimension / kHpuPcCount;
constexpr size_t kHpuPc0DataWords = kHpuPcDataWords + 1;
constexpr size_t kHpuPcSlotBytes = 3 * kLbaSize;
constexpr size_t kHpuPcSlotWords = kHpuPcSlotBytes / sizeof(uint64_t);
constexpr size_t kHpuNativeLweBytes = kHpuPcCount * kHpuPcSlotBytes;
constexpr size_t kHpuNativeOutputBytes = kRadixBlockCount * kHpuNativeLweBytes;

const char* kDefaultKeyPath =
    "../../../device/operators/hls/lwe_encrypt/testdata/"
    "psi64_big_lwe_secret_key.bin";

struct Options {
    const char* admin_device = "nvmq0";
    const char* io_device = "nvmq0n1";
    const char* key_path = kDefaultKeyPath;
    const char* output_path = "lwe_encrypt_fpga_ciphertexts.bin";
    const char* slm_read_trace_path = nullptr;
    uint8_t expected_value = 0;
    uint32_t storage_nsid = 1;
    uint32_t input_lbas = 0;
    uint32_t plaintext_bytes = kDefaultPlaintextBytes;
    size_t slm_read_chunk_bytes = kDefaultSlmReadChunkBytes;
    uint32_t slm_read_queue_depth = 1;
    uint64_t ssd_lba = 0;
    bool expected_value_set = false;
    bool ssd_lba_set = false;
    bool input_lbas_set = false;
    uint32_t operator_type_id = 2;
    uint32_t program_id = 11;
    uint64_t seed = 0;
    uint64_t nonce = 0;
    bool seed_set = false;
    bool nonce_set = false;
    bool zero_noise = false;
    bool benchmark = false;
    bool skip_dump = false;
    uint32_t output_layout = kOutputLayoutHpuNative;
};

void print_usage(const char* argv0)
{
    fprintf(
        stderr,
        "Usage: %s [options]\n"
        "  --expect N         optionally require decrypted value to equal N\n"
        "  --ssd-nsid N       source SSD namespace id (default: 1)\n"
        "  --ssd-lba N        source SSD logical block (required)\n"
        "  --plaintext-bytes N encrypt N consecutive u8 bytes (default: 1)\n"
        "  --input-lbas N      copy N 4KB SSD blocks to input SLM\n"
        "                      (default: minimum needed for plaintext bytes)\n"
        "  --slm-read-chunk-bytes N\n"
        "                      output SLM read size: 4KB..128KB, 4KB aligned\n"
        "                      (default: 131072; use 4096 for legacy mode)\n"
        "  --slm-read-queue-depth N\n"
        "                      concurrent SLM reads: 1, 2, or 4 (default: 1)\n"
        "  --slm-read-trace PATH\n"
        "                      append per-request SLM read latency samples to CSV\n"
        "  --encrypt-count N   deprecated alias for --plaintext-bytes\n"
        "  --key PATH         2048-byte Big-LWE key file\n"
        "  --output PATH      LWEHLS01 output dump\n"
        "  --output-layout hpu-native|cpu\n"
        "                      FPGA output layout (default: hpu-native)\n"
        "  --operator-type N  SUDA operator type id (default: 2)\n"
        "  --program-id N     compute program slot (default: 11)\n"
        "  --admin DEV        NVMe admin device (default: nvmq0)\n"
        "  --io DEV           NVMe I/O device (default: nvmq0n1)\n"
        "  --seed N           mask/noise PRNG seed\n"
        "  --nonce N          per-run PRNG nonce\n"
        "  --zero-noise       generate ciphertexts with exactly zero noise\n"
        "  --benchmark        print machine-readable stage timings and reduce log noise\n"
        "  --skip-dump        keep the verified result in memory without writing a dump\n"
        "  --help             show this message\n",
        argv0);
}

bool parse_u64(const char* text, uint64_t* value)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        if (strcmp(arg, "--zero-noise") == 0) {
            options->zero_noise = true;
            continue;
        }
        if (strcmp(arg, "--benchmark") == 0) {
            options->benchmark = true;
            continue;
        }
        if (strcmp(arg, "--skip-dump") == 0) {
            options->skip_dump = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", arg);
            return false;
        }

        const char* text = argv[++i];
        uint64_t value = 0;
        if (strcmp(arg, "--key") == 0) {
            options->key_path = text;
        } else if (strcmp(arg, "--output") == 0) {
            options->output_path = text;
        } else if (strcmp(arg, "--slm-read-trace") == 0) {
            options->slm_read_trace_path = text;
        } else if (strcmp(arg, "--output-layout") == 0) {
            if (strcmp(text, "hpu-native") == 0) {
                options->output_layout = kOutputLayoutHpuNative;
            } else if (strcmp(text, "cpu") == 0) {
                options->output_layout = kOutputLayoutCpuLwe;
            } else {
                fprintf(stderr, "Invalid output layout: %s\n", text);
                return false;
            }
        } else if (strcmp(arg, "--admin") == 0) {
            options->admin_device = text;
        } else if (strcmp(arg, "--io") == 0) {
            options->io_device = text;
        } else if (strcmp(arg, "--expect") == 0) {
            if (!parse_u64(text, &value) || value > UINT8_MAX) {
                fprintf(stderr, "Invalid expected u8 value: %s\n", text);
                return false;
            }
            options->expected_value = static_cast<uint8_t>(value);
            options->expected_value_set = true;
        } else if (strcmp(arg, "--ssd-nsid") == 0) {
            if (!parse_u64(text, &value) || value == 0 || value > UINT32_MAX) {
                fprintf(stderr, "Invalid SSD namespace id: %s\n", text);
                return false;
            }
            options->storage_nsid = static_cast<uint32_t>(value);
        } else if (strcmp(arg, "--ssd-lba") == 0) {
            if (!parse_u64(text, &options->ssd_lba)) {
                fprintf(stderr, "Invalid SSD LBA: %s\n", text);
                return false;
            }
            options->ssd_lba_set = true;
        } else if (strcmp(arg, "--input-lbas") == 0) {
            if (!parse_u64(text, &value) || value == 0 || value > UINT32_MAX) {
                fprintf(stderr, "Invalid input LBA count: %s\n", text);
                return false;
            }
            options->input_lbas = static_cast<uint32_t>(value);
            options->input_lbas_set = true;
        } else if (strcmp(arg, "--plaintext-bytes") == 0 ||
                   strcmp(arg, "--encrypt-count") == 0) {
            if (!parse_u64(text, &value) || value == 0 || value > UINT32_MAX) {
                fprintf(stderr, "Invalid plaintext byte count: %s\n", text);
                return false;
            }
            options->plaintext_bytes = static_cast<uint32_t>(value);
        } else if (strcmp(arg, "--slm-read-chunk-bytes") == 0) {
            if (!parse_u64(text, &value) || value < kLbaSize ||
                value > kMaxSlmReadChunkBytes || value % kLbaSize != 0) {
                fprintf(
                    stderr,
                    "Invalid SLM read chunk: %s; expected a 4KB-aligned value from 4096 to %zu\n",
                    text,
                    kMaxSlmReadChunkBytes);
                return false;
            }
            options->slm_read_chunk_bytes = static_cast<size_t>(value);
        } else if (strcmp(arg, "--slm-read-queue-depth") == 0) {
            if (!parse_u64(text, &value) ||
                (value != 1 && value != 2 && value != 4)) {
                fprintf(
                    stderr,
                    "Invalid SLM read queue depth: %s; expected 1, 2, or 4\n",
                    text);
                return false;
            }
            options->slm_read_queue_depth = static_cast<uint32_t>(value);
        } else if (strcmp(arg, "--operator-type") == 0) {
            if (!parse_u64(text, &value) || value > UINT8_MAX) {
                fprintf(stderr, "Invalid operator type: %s\n", text);
                return false;
            }
            options->operator_type_id = static_cast<uint32_t>(value);
        } else if (strcmp(arg, "--program-id") == 0) {
            if (!parse_u64(text, &value) || value >= 64) {
                fprintf(stderr, "Invalid program id: %s\n", text);
                return false;
            }
            options->program_id = static_cast<uint32_t>(value);
        } else if (strcmp(arg, "--seed") == 0) {
            if (!parse_u64(text, &options->seed)) {
                fprintf(stderr, "Invalid seed: %s\n", text);
                return false;
            }
            options->seed_set = true;
        } else if (strcmp(arg, "--nonce") == 0) {
            if (!parse_u64(text, &options->nonce)) {
                fprintf(stderr, "Invalid nonce: %s\n", text);
                return false;
            }
            options->nonce_set = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return false;
        }
    }
    if (!options->ssd_lba_set) {
        fprintf(stderr, "--ssd-lba is required to select the source SSD block\n");
        return false;
    }
    uint64_t required_lbas =
        (static_cast<uint64_t>(options->plaintext_bytes) + kLbaSize - 1) / kLbaSize;
    if (!options->input_lbas_set) {
        options->input_lbas = static_cast<uint32_t>(required_lbas);
    }
    uint64_t input_bytes = static_cast<uint64_t>(options->input_lbas) * kLbaSize;
    if (input_bytes > INT_MAX) {
        fprintf(stderr, "Input SLM exceeds the runtime's signed 32-bit size limit\n");
        return false;
    }
    if (options->plaintext_bytes > input_bytes) {
        fprintf(
            stderr,
            "--plaintext-bytes %u exceeds the %llu bytes copied by --input-lbas %u\n",
            options->plaintext_bytes,
            static_cast<unsigned long long>(input_bytes),
            options->input_lbas);
        return false;
    }
    uint64_t physical_output =
        static_cast<uint64_t>(options->plaintext_bytes) *
        (options->output_layout == kOutputLayoutHpuNative
             ? kHpuNativeOutputBytes
             : kPhysicalOutputBytes);
    uint64_t output_with_done = physical_output + kOutputDonePacketBytes;
    uint64_t output_aligned =
        ((output_with_done + kLbaSize - 1) / kLbaSize) * kLbaSize;
    if (output_aligned > INT_MAX) {
        fprintf(
            stderr,
            "Requested batch needs %llu output bytes, exceeding the runtime's signed 32-bit SLM limit\n",
            static_cast<unsigned long long>(output_aligned));
        return false;
    }
    return true;
}

uint64_t random_u64()
{
    uint64_t value = 0;
    ssize_t bytes = getrandom(&value, sizeof(value), 0);
    if (bytes == static_cast<ssize_t>(sizeof(value))) {
        return value;
    }

    struct timeval now;
    gettimeofday(&now, nullptr);
    return (static_cast<uint64_t>(now.tv_sec) << 32) ^
           static_cast<uint64_t>(now.tv_usec) ^
           static_cast<uint64_t>(getpid());
}

bool read_binary_key(const char* path, std::vector<uint8_t>* key)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "Unable to open key file: %s\n", path);
        return false;
    }

    key->assign(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    if (key->size() != kMaskDimension) {
        fprintf(
            stderr,
            "Key file must contain exactly %u binary coefficients; got %zu bytes\n",
            kMaskDimension,
            key->size());
        return false;
    }

    for (size_t i = 0; i < key->size(); ++i) {
        if ((*key)[i] > 1) {
            fprintf(stderr, "Key coefficient %zu is not binary\n", i);
            return false;
        }
    }
    return true;
}

void store_u32(uint8_t* data, size_t offset, uint32_t value)
{
    memcpy(data + offset, &value, sizeof(value));
}

void store_u64(uint8_t* data, size_t offset, uint64_t value)
{
    memcpy(data + offset, &value, sizeof(value));
}

void build_context(
    uint8_t* context,
    const std::vector<uint8_t>& key,
    const Options& options)
{
    memset(context, 0, kLbaSize);

    // Offsets are relative to AccContext.static_data. OperatorController maps
    // this page to HLS context word LWE_ENCRYPT_STATIC_CONTEXT_BASE (word 3).
    store_u32(context, 0, kMaskDimension);
    store_u32(context, 4, options.plaintext_bytes);
    store_u32(context, 8, kInputModeU8Radix);
    store_u32(
        context,
        12,
        options.zero_noise ? kNoiseModeZero : kNoiseModeInternal);
    store_u32(context, 16, kNoiseBoundLog2);
    store_u32(context, 20, options.output_layout);
    store_u64(context, 32, kDelta);
    store_u64(context, 40, options.seed);
    store_u64(context, 48, options.nonce);

    // Host offset 64 becomes HLS context word 4. One key coefficient uses one bit.
    for (size_t i = 0; i < key.size(); ++i) {
        if (key[i] != 0) {
            context[kAxisBytes + (i / 8)] |= uint8_t{1} << (i % 8);
        }
    }
}

void build_program(
    hlsacccompute_program* program,
    uint32_t operator_type_id,
    uint32_t program_id,
    uint32_t plaintext_bytes)
{
    memset(program, 0, sizeof(*program));
    program->input_channum = 1;
    program->output_channum = 1;
    program->program_id = program_id;
    program->apply_operators_id_map[0] =
        static_cast<uint8_t>(operator_type_id);

    program->applyops[0].header.cid = 0;
    program->applyops[0].header.opc = APPLY_OPS;
    program->applyops[0].header.ops_num = 1;
    program->applyops[2].apply_ops_payload2.connections_num = 1;
    program->applyops[2].apply_ops_payload2.connections[0].from = 0x00;
    program->applyops[2].apply_ops_payload2.connections[0].to = 0xf0;

    program->pauseops[0].header.cid = 0;
    program->pauseops[0].header.opc = SUSPEND_OPS;
    program->pauseops[0].header.ops_num = 1;
    program->pauseops[1].generic_ops_payload.op_lists[0] = 0;

    program->freeops[0].header.cid = 0;
    program->freeops[0].header.opc = FORCE_FREE_OPS;
    program->freeops[0].header.ops_num = 1;
    program->freeops[1].generic_ops_payload.op_lists[0] = 0;

    program->input_channel_destination[0] = 0;
    program->apply_ops_size = 3;
    program->apply_operators_num = 1;
    uint64_t estimated_us = static_cast<uint64_t>(plaintext_bytes) * 200;
    uint64_t maximum_us = static_cast<uint64_t>(plaintext_bytes) * 1000;
    program->esti_executed_time =
        estimated_us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(estimated_us);
    program->max_responded_time =
        maximum_us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(maximum_us);
}

size_t round_up_to_lba(size_t value)
{
    return ((value + kLbaSize - 1) / kLbaSize) * kLbaSize;
}

size_t input_buffer_bytes(const Options& options)
{
    return static_cast<size_t>(options.input_lbas) * kLbaSize;
}

size_t stream_input_bytes(const Options& options)
{
    return static_cast<size_t>(options.plaintext_bytes);
}

size_t input_range_bytes(const Options& options)
{
    size_t stream_bytes = stream_input_bytes(options);
    return ((stream_bytes + kLbaSize - 1) / kLbaSize) * kLbaSize;
}

size_t physical_output_bytes(const Options& options)
{
    const size_t bytes_per_u8 = options.output_layout == kOutputLayoutHpuNative
        ? kHpuNativeOutputBytes
        : kPhysicalOutputBytes;
    return static_cast<size_t>(options.plaintext_bytes) * bytes_per_u8;
}

size_t output_buffer_bytes(const Options& options)
{
    return round_up_to_lba(physical_output_bytes(options) + kOutputDonePacketBytes);
}

struct SlmReadSample {
    size_t request_index;
    size_t worker_index;
    size_t offset_bytes;
    size_t length_bytes;
    double elapsed_ms;
    int result;
    int error_number;
    int eintr_retries;
    bool completed;
};

int read_slm_in_chunks(
    int io_fd,
    unsigned int mem_id,
    size_t offset,
    size_t bytes,
    size_t chunk_bytes,
    uint32_t queue_depth,
    void* output,
    size_t* failed_offset,
    size_t* failed_length,
    int* failed_errno,
    bool verbose,
    std::vector<SlmReadSample>* samples)
{
    uint8_t* out = static_cast<uint8_t*>(output);
    const size_t request_count = (bytes + chunk_bytes - 1) / chunk_bytes;
    const uint32_t active_queue_depth =
        std::min<uint32_t>(queue_depth, static_cast<uint32_t>(request_count));
    std::vector<SlmReadSample> local_samples;
    std::vector<SlmReadSample>* records =
        samples != nullptr ? samples : &local_samples;
    records->assign(
        request_count,
        SlmReadSample{0, 0, 0, 0, 0.0, INT_MIN, 0, 0, false});
    std::atomic<size_t> next_request{0};
    std::atomic<bool> stop{false};

    auto worker = [&](size_t worker_index) {
        while (!stop.load(std::memory_order_relaxed)) {
            const size_t request_index =
                next_request.fetch_add(1, std::memory_order_relaxed);
            if (request_index >= request_count) {
                return;
            }
            const size_t request_done = request_index * chunk_bytes;
            const size_t request_offset = offset + request_done;
            const size_t chunk = std::min(chunk_bytes, bytes - request_done);
            int ret = 0;
            int eintr_retries = 0;
            int request_errno = 0;
            if (verbose) {
                fprintf(
                    stderr,
                    "[lwe_encrypt] reading output SLM request=%zu worker=%zu "
                    "offset=%zu length=%zu\n",
                    request_index,
                    worker_index,
                    request_offset,
                    chunk);
                fflush(stderr);
            }
            const auto request_start = std::chrono::steady_clock::now();
            while (true) {
                errno = 0;
                ret = nvme_slm_read(
                    io_fd,
                    mem_id,
                    static_cast<int>(request_offset),
                    static_cast<int>(chunk),
                    out + request_done);
                request_errno = errno;
                if (ret == 0) {
                    break;
                }
                if (errno != EINTR ||
                    eintr_retries >= kSlmReadEintrMaxRetries) {
                    break;
                }
                ++eintr_retries;
                if (verbose) {
                    fprintf(
                        stderr,
                        "[lwe_encrypt] nvme_slm_read EINTR retry %d/%d "
                        "at request=%zu offset=%zu\n",
                        eintr_retries,
                        kSlmReadEintrMaxRetries,
                        request_index,
                        request_offset);
                    fflush(stderr);
                }
            }
            const auto request_end = std::chrono::steady_clock::now();
            (*records)[request_index] = SlmReadSample{
                request_index,
                worker_index,
                request_offset,
                chunk,
                std::chrono::duration<double, std::milli>(
                    request_end - request_start)
                    .count(),
                ret,
                request_errno,
                eintr_retries,
                true};
            if (ret != 0) {
                stop.store(true, std::memory_order_relaxed);
                return;
            }
        }
    };

    if (active_queue_depth == 1) {
        worker(0);
    } else {
        std::vector<std::thread> workers;
        workers.reserve(active_queue_depth);
        for (uint32_t worker_index = 0; worker_index < active_queue_depth;
             ++worker_index) {
            workers.emplace_back(worker, worker_index);
        }
        for (std::thread& thread : workers) {
            thread.join();
        }
    }

    for (const SlmReadSample& sample : *records) {
        if (sample.completed && sample.result != 0) {
            if (failed_offset != nullptr) {
                *failed_offset = sample.offset_bytes;
            }
            if (failed_length != nullptr) {
                *failed_length = sample.length_bytes;
            }
            if (failed_errno != nullptr) {
                *failed_errno = sample.error_number;
            }
            return sample.result;
        }
    }
    return 0;
}

bool append_slm_read_trace(
    const char* path,
    const Options& options,
    size_t expected_request_count,
    const std::vector<SlmReadSample>& samples)
{
    static const char kTraceHeader[] =
        "run_nonce,seed,plaintext_bytes,chunk_bytes,queue_depth,"
        "expected_request_count,request_index,worker_index,offset_bytes,"
        "length_bytes,elapsed_ms,ret,errno,eintr_retries\n";
    FILE* output = fopen(path, "a+");
    if (output == nullptr) {
        fprintf(stderr, "Unable to open SLM read trace: %s\n", path);
        return false;
    }
    if (fseek(output, 0, SEEK_END) != 0) {
        fprintf(stderr, "Unable to seek SLM read trace: %s\n", path);
        fclose(output);
        return false;
    }
    long output_size = ftell(output);
    if (output_size < 0) {
        fprintf(stderr, "Unable to query SLM read trace size: %s\n", path);
        fclose(output);
        return false;
    }
    if (output_size == 0) {
        fputs(kTraceHeader, output);
    } else {
        rewind(output);
        char existing_header[512] = {};
        if (fgets(existing_header, sizeof(existing_header), output) == nullptr ||
            strcmp(existing_header, kTraceHeader) != 0) {
            fprintf(
                stderr,
                "SLM read trace header does not match the current format: %s\n"
                "Use a new trace file for queue-depth measurements.\n",
                path);
            fclose(output);
            return false;
        }
        if (fseek(output, 0, SEEK_END) != 0) {
            fprintf(stderr, "Unable to seek SLM read trace: %s\n", path);
            fclose(output);
            return false;
        }
    }
    for (const SlmReadSample& sample : samples) {
        if (!sample.completed) {
            continue;
        }
        fprintf(
            output,
            "0x%016llx,0x%016llx,%u,%zu,%u,%zu,%zu,%zu,%zu,%zu,"
            "%.6f,%d,%d,%d\n",
            static_cast<unsigned long long>(options.nonce),
            static_cast<unsigned long long>(options.seed),
            options.plaintext_bytes,
            options.slm_read_chunk_bytes,
            options.slm_read_queue_depth,
            expected_request_count,
            sample.request_index,
            sample.worker_index,
            sample.offset_bytes,
            sample.length_bytes,
            sample.elapsed_ms,
            sample.result,
            sample.error_number,
            sample.eintr_retries);
    }
    if (fclose(output) != 0) {
        fprintf(stderr, "Unable to close SLM read trace: %s\n", path);
        return false;
    }
    return true;
}

void print_slm_read_request_stats(const std::vector<SlmReadSample>& samples)
{
    std::vector<double> successful;
    successful.reserve(samples.size());
    size_t failed = 0;
    const SlmReadSample* slowest = nullptr;
    double sum_ms = 0.0;
    for (const SlmReadSample& sample : samples) {
        if (!sample.completed) {
            continue;
        }
        if (sample.result != 0) {
            ++failed;
            continue;
        }
        successful.push_back(sample.elapsed_ms);
        sum_ms += sample.elapsed_ms;
        if (slowest == nullptr || sample.elapsed_ms > slowest->elapsed_ms) {
            slowest = &sample;
        }
    }
    if (successful.empty()) {
        printf("slm_read_request_stats count=0 failed=%zu\n", failed);
        return;
    }
    std::sort(successful.begin(), successful.end());
    const size_t count = successful.size();
    const double p50 = count % 2 == 0
        ? (successful[count / 2 - 1] + successful[count / 2]) / 2.0
        : successful[count / 2];
    const size_t p95_index = (count * 95 + 99) / 100 - 1;
    printf(
        "slm_read_request_stats count=%zu failed=%zu mean_ms=%.3f "
        "p50_ms=%.3f p95_ms=%.3f max_ms=%.3f max_index=%zu "
        "max_offset=%zu max_length=%zu\n",
        count,
        failed,
        sum_ms / static_cast<double>(count),
        p50,
        successful[p95_index],
        slowest->elapsed_ms,
        slowest->request_index,
        slowest->offset_bytes,
        slowest->length_bytes);
}

size_t source_range_count(const Options& options)
{
    return (static_cast<size_t>(options.input_lbas) + kMaxCopyLbasPerRange - 1) /
           kMaxCopyLbasPerRange;
}

uint64_t load_word(const uint8_t* data, size_t word_index)
{
    uint64_t value = 0;
    memcpy(&value, data + word_index * sizeof(uint64_t), sizeof(value));
    return value;
}

size_t reverse_psi64_mask_index(size_t index)
{
    size_t reversed = 0;
    for (size_t bit = 0; bit < 11; ++bit) {
        reversed = (reversed << 1) | ((index >> bit) & 1);
    }
    return reversed;
}

size_t hpu_native_word_index(size_t lwe_index, size_t pc, size_t pc_offset)
{
    return lwe_index * kHpuPcCount * kHpuPcSlotWords +
           pc * kHpuPcSlotWords + pc_offset;
}

bool extract_hpu_native_and_verify(
    const uint8_t* raw,
    size_t raw_bytes,
    size_t clear_count,
    const std::vector<uint8_t>& key,
    bool expected_value_set,
    uint8_t expected_value,
    bool zero_noise,
    std::vector<uint8_t>* clears,
    std::vector<uint64_t>* logical_words)
{
    const size_t lwe_count = clear_count * kRadixBlockCount;
    const size_t required_words =
        lwe_count * kHpuPcCount * kHpuPcSlotWords;
    if (raw_bytes / sizeof(uint64_t) < required_words) {
        return false;
    }

    clears->clear();
    clears->reserve(clear_count);
    logical_words->clear();
    logical_words->reserve(lwe_count * kLogicalWordsPerCiphertext);

    for (size_t clear_index = 0; clear_index < clear_count; ++clear_index) {
        uint8_t reconstructed = 0;

        for (size_t block = 0; block < kRadixBlockCount; ++block) {
            const size_t lwe_index = clear_index * kRadixBlockCount + block;
            uint64_t dot = 0;

            for (size_t natural_index = 0;
                 natural_index < kMaskDimension;
                 ++natural_index) {
                const size_t hpu_index =
                    reverse_psi64_mask_index(natural_index);
                const size_t group = hpu_index / kHpuPcGroupWords;
                const size_t lane = hpu_index % kHpuPcGroupWords;
                const size_t pc = group % kHpuPcCount;
                const size_t pc_offset =
                    (group / kHpuPcCount) * kHpuPcGroupWords + lane;
                const uint64_t mask = load_word(
                    raw,
                    hpu_native_word_index(lwe_index, pc, pc_offset));
                logical_words->push_back(mask);
                if (key[natural_index] != 0) {
                    dot += mask;
                }
            }

            const uint64_t body = load_word(
                raw,
                hpu_native_word_index(lwe_index, 0, kHpuPcDataWords));
            logical_words->push_back(body);

            for (size_t i = kHpuPc0DataWords; i < kHpuPcSlotWords; ++i) {
                if (load_word(raw, hpu_native_word_index(lwe_index, 0, i)) != 0) {
                    return false;
                }
            }
            for (size_t i = kHpuPcDataWords; i < kHpuPcSlotWords; ++i) {
                if (load_word(raw, hpu_native_word_index(lwe_index, 1, i)) != 0) {
                    return false;
                }
            }

            const uint64_t phase = body - dot;
            const uint64_t decoded =
                ((phase + (kDelta / 2)) >> kDeltaLog2) &
                ((uint64_t{1} << kMessageWidth) - 1);
            const uint64_t encoded = decoded * kDelta;
            const int64_t error = static_cast<int64_t>(phase - encoded);
            const int64_t max_error =
                zero_noise ? 0 : ((int64_t{1} << kNoiseBoundLog2) - 1);
            if (error < -max_error || error > max_error) {
                return false;
            }

            if (expected_value_set && clear_index == 0) {
                const uint64_t expected_block =
                    (expected_value >> (block * kMessageWidth)) &
                    ((uint64_t{1} << kMessageWidth) - 1);
                if (decoded != expected_block) {
                    return false;
                }
            }
            reconstructed |=
                static_cast<uint8_t>(decoded << (block * kMessageWidth));
        }

        if (expected_value_set && clear_index == 0 &&
            reconstructed != expected_value) {
            return false;
        }
        clears->push_back(reconstructed);
    }
    return true;
}

bool extract_and_verify(
    const uint8_t* raw,
    size_t raw_bytes,
    size_t clear_count,
    size_t stride_words,
    const std::vector<uint8_t>& key,
    bool expected_value_set,
    uint8_t expected_value,
    bool zero_noise,
    std::vector<uint8_t>* clears,
    std::vector<uint64_t>* logical_words)
{
    const size_t required_words =
        (clear_count * kRadixBlockCount - 1) * stride_words +
        kLogicalWordsPerCiphertext;
    if (raw_bytes / sizeof(uint64_t) < required_words) {
        return false;
    }

    clears->clear();
    clears->reserve(clear_count);
    logical_words->clear();
    logical_words->reserve(
        clear_count * kRadixBlockCount * kLogicalWordsPerCiphertext);

    for (size_t clear_index = 0; clear_index < clear_count; ++clear_index) {
        uint8_t reconstructed = 0;

        for (size_t block = 0; block < kRadixBlockCount; ++block) {
            const size_t base =
                (clear_index * kRadixBlockCount + block) * stride_words;
            uint64_t dot = 0;

            for (size_t i = 0; i < kMaskDimension; ++i) {
                uint64_t mask = load_word(raw, base + i);
                logical_words->push_back(mask);
                if (key[i] != 0) {
                    dot += mask;
                }
            }

            uint64_t body = load_word(raw, base + kMaskDimension);
            logical_words->push_back(body);
            uint64_t phase = body - dot;
            uint64_t decoded =
                ((phase + (kDelta / 2)) >> kDeltaLog2) &
                ((uint64_t{1} << kMessageWidth) - 1);
            uint64_t encoded = decoded * kDelta;
            int64_t error = static_cast<int64_t>(phase - encoded);
            int64_t max_error =
                zero_noise ? 0 : ((int64_t{1} << kNoiseBoundLog2) - 1);
            if (error < -max_error || error > max_error) {
                return false;
            }

            if (expected_value_set && clear_index == 0) {
                uint64_t expected_block =
                    (expected_value >> (block * kMessageWidth)) &
                    ((uint64_t{1} << kMessageWidth) - 1);
                if (decoded != expected_block) {
                    return false;
                }
            }
            reconstructed |=
                static_cast<uint8_t>(decoded << (block * kMessageWidth));
        }

        if (expected_value_set && clear_index == 0 &&
            reconstructed != expected_value) {
            return false;
        }
        clears->push_back(reconstructed);
    }
    return true;
}

void write_le64(std::ofstream* file, uint64_t value)
{
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
    file->write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

bool write_dump(
    const char* path,
    const std::vector<uint8_t>& clears,
    const std::vector<uint64_t>& ciphertext_words)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "Unable to open output file: %s\n", path);
        return false;
    }

    file.write("LWEHLS01", 8);
    write_le64(&file, 1);
    write_le64(&file, kMaskDimension);
    write_le64(&file, clears.size());
    write_le64(&file, kRadixBlockCount);
    write_le64(&file, kMessageWidth);
    write_le64(&file, kCarryWidth);
    write_le64(&file, kPaddingWidth);
    write_le64(&file, kDeltaLog2);
    write_le64(&file, ciphertext_words.size());
    for (uint8_t clear : clears) {
        write_le64(&file, clear);
    }
    for (uint64_t word : ciphertext_words) {
        write_le64(&file, word);
    }
    return file.good();
}

double elapsed_ms(const timeval& start, const timeval& end)
{
    return (end.tv_sec - start.tv_sec) * 1000.0 +
           (end.tv_usec - start.tv_usec) / 1000.0;
}

void log_stage(const char* stage)
{
    fprintf(stderr, "[lwe_encrypt] %s\n", stage);
    fflush(stderr);
}

}  // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return 1;
    }
    struct timeval application_start = {};
    gettimeofday(&application_start, nullptr);
    if (!options.seed_set) {
        options.seed = random_u64();
    }
    if (!options.nonce_set) {
        options.nonce = random_u64();
    }

    std::vector<uint8_t> key;
    if (!read_binary_key(options.key_path, &key)) {
        return 1;
    }

    const size_t input_bytes = input_buffer_bytes(options);
    const size_t compute_input_bytes = stream_input_bytes(options);
    const size_t compute_input_range_bytes = input_range_bytes(options);
    const size_t cipher_physical_bytes = physical_output_bytes(options);
    const size_t output_bytes = output_buffer_bytes(options);
    const size_t slm_read_request_count =
        (output_bytes + options.slm_read_chunk_bytes - 1) /
        options.slm_read_chunk_bytes;
    const size_t copy_range_count = source_range_count(options);
    if (copy_range_count * sizeof(union nvme_source_range) > kLbaSize) {
        fprintf(
            stderr,
            "Too many SSD source ranges for one source range page: %zu\n",
            copy_range_count);
        return 1;
    }
    if (copy_range_count > UINT8_MAX) {
        fprintf(
            stderr,
            "Too many SSD source ranges for nvme_slm_copy: %zu\n",
            copy_range_count);
        return 1;
    }

    void* output_buffer = nullptr;
    void* context_page = nullptr;
    void* source_range_page = nullptr;
    if (posix_memalign(&output_buffer, kLbaSize, output_bytes) != 0 ||
        posix_memalign(&context_page, kLbaSize, kLbaSize) != 0 ||
        posix_memalign(&source_range_page, kLbaSize, kLbaSize) != 0) {
        fprintf(stderr, "Unable to allocate aligned host buffers\n");
        free(output_buffer);
        free(context_page);
        free(source_range_page);
        return 1;
    }
    memset(output_buffer, 0, output_bytes);
    memset(source_range_page, 0, kLbaSize);
    build_context(static_cast<uint8_t*>(context_page), key, options);

    int admin_fd = nvme_open(options.admin_device);
    int io_fd = nvme_open(options.io_device);
    if (admin_fd < 0 || io_fd < 0) {
        fprintf(
            stderr,
            "Unable to open NVMe devices admin=%s io=%s\n",
            options.admin_device,
            options.io_device);
        free(output_buffer);
        free(context_page);
        free(source_range_page);
        return 1;
    }

    unsigned int input_mem_id = 0;
    unsigned int output_mem_id = 0;
    unsigned int rsid = 0;
    bool input_created = false;
    bool output_created = false;
    bool range_created = false;
    bool program_loaded = false;
    bool program_activated = false;
    int status = 1;
    union memory_range_set_decriptor ranges[2];
    struct hlsacccompute_program program;
    struct timeval start = {};
    struct timeval end = {};
    struct timeval pipeline_start = {};
    struct timeval slm_create_start = {};
    struct timeval slm_create_end = {};
    struct timeval ssd_copy_start = {};
    struct timeval ssd_copy_end = {};
    struct timeval program_setup_start = {};
    struct timeval program_setup_end = {};
    struct timeval slm_read_start = {};
    struct timeval slm_read_end = {};
    struct timeval verify_start = {};
    struct timeval verify_end = {};
    struct timeval dump_start = {};
    struct timeval dump_end = {};
    struct timeval cleanup_start = {};
    struct timeval cleanup_end = {};
    unsigned int result_bytes = 0;
    size_t failed_slm_offset = 0;
    size_t failed_slm_length = 0;
    int failed_slm_errno = 0;
    union nvme_source_range* source_range =
        static_cast<union nvme_source_range*>(source_range_page);
    std::vector<uint8_t> clears;
    std::vector<uint64_t> logical_words;
    std::vector<SlmReadSample> slm_read_samples;
    const uint8_t* raw = static_cast<const uint8_t*>(output_buffer);
    const char* layout = nullptr;

    memset(ranges, 0, sizeof(ranges));
    memset(&program, 0, sizeof(program));

    fprintf(
        stderr,
        "[lwe_encrypt] sizing input_bytes=%zu compute_input_range_bytes=%zu "
        "cipher_physical_bytes=%zu output_slm_bytes=%zu read_chunk_bytes=%zu "
        "read_requests=%zu read_queue_depth=%u\n",
        input_bytes,
        compute_input_range_bytes,
        cipher_physical_bytes,
        output_bytes,
        options.slm_read_chunk_bytes,
        slm_read_request_count,
        options.slm_read_queue_depth);

    int ret = 0;
    gettimeofday(&pipeline_start, nullptr);
    gettimeofday(&slm_create_start, nullptr);
    log_stage("creating input SLM");
    ret = nvme_create_slm_ns(admin_fd, &input_mem_id, input_bytes);
    if (ret != 0) {
        fprintf(stderr, "nvme_create_slm_ns(input) failed: %d\n", ret);
        goto cleanup;
    }
    input_created = true;

    log_stage("creating output SLM");
    ret = nvme_create_slm_ns(admin_fd, &output_mem_id, output_bytes);
    if (ret != 0) {
        fprintf(stderr, "nvme_create_slm_ns(output) failed: %d\n", ret);
        goto cleanup;
    }
    output_created = true;
    gettimeofday(&slm_create_end, nullptr);

    for (size_t i = 0; i < copy_range_count; ++i) {
        uint32_t remaining_lbas =
            options.input_lbas - static_cast<uint32_t>(i * kMaxCopyLbasPerRange);
        uint32_t lbas_this_range =
            remaining_lbas > kMaxCopyLbasPerRange ? kMaxCopyLbasPerRange : remaining_lbas;

        source_range[i].scc.snsid = options.storage_nsid;
        source_range[i].scc.slba =
            options.ssd_lba + static_cast<uint64_t>(i * kMaxCopyLbasPerRange);
        source_range[i].scc.nlb = lbas_this_range - 1;
    }

    log_stage("copying SSD blocks to input SLM");
    gettimeofday(&ssd_copy_start, nullptr);
    ret = nvme_slm_copy(
        io_fd,
        source_range_page,
        sizeof(*source_range) * copy_range_count,
        0,
        0x3,
        static_cast<unsigned char>(copy_range_count),
        input_mem_id);
    if (ret != 0) {
        fprintf(
            stderr,
            "nvme_slm_copy SSD nsid=%u lba=%llu lbas=%u to input SLM failed: %d\n",
            options.storage_nsid,
            static_cast<unsigned long long>(options.ssd_lba),
            options.input_lbas,
            ret);
        goto cleanup;
    }
    gettimeofday(&ssd_copy_end, nullptr);

    gettimeofday(&program_setup_start, nullptr);
    ranges[0].payload.mnsid = input_mem_id;
    ranges[0].payload.length = compute_input_range_bytes;
    ranges[0].payload.starting_byte = 0;
    ranges[0].payload.flag =
        memory_range_descriptor::mdes_flag::MEM_RANGE_DEVICE_MEM;
    ranges[1].payload.mnsid = output_mem_id;
    ranges[1].payload.length = output_bytes;
    ranges[1].payload.starting_byte = 0;
    ranges[1].payload.flag =
        memory_range_descriptor::mdes_flag::MEM_RANGE_DEVICE_MEM;

    log_stage("creating memory range set");
    ret = nvme_create_memory_range_set(
        admin_fd,
        kComputeNsid,
        &rsid,
        2,
        ranges);
    if (ret != 0) {
        fprintf(stderr, "nvme_create_memory_range_set failed: %d\n", ret);
        goto cleanup;
    }
    range_created = true;

    build_program(
        &program,
        options.operator_type_id,
        options.program_id,
        options.plaintext_bytes);
    log_stage("clearing stale FPGA program slot");
    ret = nvme_unload_hlsacc_program(
        admin_fd,
        options.program_id,
        kComputeNsid);
    if (ret != 0) {
        fprintf(
            stderr,
            "nvme_unload_hlsacc_program(pre-load) failed: %d\n",
            ret);
        goto cleanup;
    }
    log_stage("loading FPGA program");
    ret = nvme_load_hlsacc_program(
        admin_fd,
        sizeof(program),
        options.program_id,
        kComputeNsid,
        &program);
    if (ret != 0) {
        fprintf(stderr, "nvme_load_hlsacc_program failed: %d\n", ret);
        goto cleanup;
    }
    program_loaded = true;

    log_stage("activating FPGA program");
    ret = nvme_activate_program(
        admin_fd,
        options.program_id,
        kComputeNsid);
    if (ret != 0) {
        fprintf(stderr, "nvme_activate_program failed: %d\n", ret);
        goto cleanup;
    }
    program_activated = true;
    gettimeofday(&program_setup_end, nullptr);

    gettimeofday(&start, nullptr);

    // SUDA copies the first 2048 bytes of this 4KB page into operator BRAM.
    log_stage("executing lwe_encrypt on FPGA");
    ret = nvme_execute_hlsacc_program(
        io_fd,
        kComputeNsid,
        rsid,
        options.program_id,
        reinterpret_cast<struct AccContext*>(context_page),
        1,
        0,
        0,
        &result_bytes);
    gettimeofday(&end, nullptr);
    if (ret != 0) {
        fprintf(stderr, "nvme_execute_hlsacc_program failed: %d\n", ret);
        goto cleanup;
    }
    fprintf(
        stderr,
        "[lwe_encrypt] FPGA execution returned result_bytes=%u expected_payload_bytes=%zu output_slm_bytes=%zu\n",
        result_bytes,
        cipher_physical_bytes,
        output_bytes);
    if (result_bytes < cipher_physical_bytes) {
        fprintf(
            stderr,
            "FPGA returned too few output bytes; skip output SLM read to avoid a stale/empty SLM timeout\n");
        ret = 1;
        goto cleanup;
    }
    log_stage("FPGA execution completed; reading output SLM");

    gettimeofday(&slm_read_start, nullptr);
    ret = read_slm_in_chunks(
        io_fd,
        output_mem_id,
        0,
        output_bytes,
        options.slm_read_chunk_bytes,
        options.slm_read_queue_depth,
        output_buffer,
        &failed_slm_offset,
        &failed_slm_length,
        &failed_slm_errno,
        !options.benchmark,
        &slm_read_samples);
    if (options.slm_read_trace_path != nullptr) {
        if (!append_slm_read_trace(
                options.slm_read_trace_path,
                options,
                slm_read_request_count,
                slm_read_samples)) {
            fprintf(
                stderr,
                "warning: failed to write SLM request trace; encryption result is unchanged\n");
        }
        print_slm_read_request_stats(slm_read_samples);
        printf("slm_read_trace=%s\n", options.slm_read_trace_path);
    }
    if (ret != 0) {
        fprintf(
            stderr,
            "nvme_slm_read failed: ret=%d mem_id=%u offset=%zu length=%zu errno=%d (%s)\n",
            ret,
            output_mem_id,
            failed_slm_offset,
            failed_slm_length,
            failed_slm_errno,
            strerror(failed_slm_errno));
        goto cleanup;
    }
    gettimeofday(&slm_read_end, nullptr);
    log_stage("output read completed; verifying ciphertext");

    gettimeofday(&verify_start, nullptr);
    if (options.output_layout == kOutputLayoutHpuNative) {
        if (!extract_hpu_native_and_verify(
                raw,
                output_bytes,
                options.plaintext_bytes,
                key,
                options.expected_value_set,
                options.expected_value,
                options.zero_noise,
                &clears,
                &logical_words)) {
            fprintf(stderr, "FPGA HPU-native ciphertext verification failed\n");
            goto cleanup;
        }
        layout = "hpu-native-psi64-v80";
    // The current OperatorController forces TKEEP to all ones, so each CPU
    // ciphertext body occupies a full final 64-byte beat. Keep support for
    // a future controller that preserves the HLS partial TKEEP as well.
    } else if (extract_and_verify(
            raw,
            output_bytes,
            options.plaintext_bytes,
            kPhysicalWordsPerCiphertext,
            key,
            options.expected_value_set,
            options.expected_value,
            options.zero_noise,
            &clears,
            &logical_words)) {
        layout = "64-byte-padded";
    } else if (extract_and_verify(
                   raw,
                   output_bytes,
                   options.plaintext_bytes,
                   kLogicalWordsPerCiphertext,
                   key,
                   options.expected_value_set,
                   options.expected_value,
                   options.zero_noise,
                   &clears,
                   &logical_words)) {
        layout = "compact";
    } else {
        fprintf(
            stderr,
            "FPGA CPU-LWE ciphertext verification failed for both stream layouts\n");
        goto cleanup;
    }
    gettimeofday(&verify_end, nullptr);

    gettimeofday(&dump_start, nullptr);
    if (!options.skip_dump) {
        if (!write_dump(options.output_path, clears, logical_words)) {
            goto cleanup;
        }
    }
    gettimeofday(&dump_end, nullptr);

    printf("lwe_encrypt FPGA execution passed\n");
    printf("decrypted_count=%zu\n", clears.size());
    printf("decrypted_first_u8=0x%02x (%u)\n", clears[0], clears[0]);
    printf("decrypted_prefix=");
    for (size_t i = 0; i < clears.size() && i < 16; ++i) {
        printf("%02x", clears[i]);
    }
    if (clears.size() > 16) {
        printf("...");
    }
    printf("\n");
    printf("input_path=SSD(nsid=%u,lba=%llu,lbas=%u)->SLM->lwe_encrypt\n",
           options.storage_nsid,
           static_cast<unsigned long long>(options.ssd_lba),
           options.input_lbas);
    printf("ssd_copy_bytes=%zu plaintext_bytes=%zu\n",
           input_bytes,
           compute_input_bytes);
    printf("compute_input_range_bytes=%zu\n", compute_input_range_bytes);
    printf("slm_read_chunk_bytes=%zu slm_read_requests=%zu "
           "slm_read_queue_depth=%u\n",
           options.slm_read_chunk_bytes,
           slm_read_request_count,
           options.slm_read_queue_depth);
    printf("operator_type_id=%u program_id=%u rsid=%u\n",
           options.operator_type_id,
           options.program_id,
           rsid);
    printf("seed=0x%016llx nonce=0x%016llx noise=%s\n",
           static_cast<unsigned long long>(options.seed),
           static_cast<unsigned long long>(options.nonce),
           options.zero_noise ? "zero" : "internal-prototype");
    printf("exec_result=%u expected_logical_bytes=%zu physical_bytes=%zu\n",
           result_bytes,
           static_cast<size_t>(options.plaintext_bytes) * kLogicalOutputBytes,
           cipher_physical_bytes);
    printf("output_layout=%s elapsed_ms=%.3f\n",
           layout,
           elapsed_ms(start, end));
    printf("ciphertext_dump=%s\n",
           options.skip_dump ? "skipped" : options.output_path);
    printf("host_key_decrypt_checked=yes\n");
    if (!options.zero_noise) {
        printf("warning=internal PRNG/noise is a prototype, not bit-exact tfhe-rs randomness\n");
    }
    status = 0;

cleanup:
    gettimeofday(&cleanup_start, nullptr);
    if (program_activated) {
        int cleanup_ret =
            nvme_deactivate_program(admin_fd, options.program_id, kComputeNsid);
        if (cleanup_ret != 0) {
            fprintf(
                stderr,
                "warning: nvme_deactivate_program cleanup failed: %d\n",
                cleanup_ret);
        }
    }
    if (program_loaded) {
        int cleanup_ret =
            nvme_unload_hlsacc_program(admin_fd, options.program_id, kComputeNsid);
        if (cleanup_ret != 0) {
            fprintf(
                stderr,
                "warning: nvme_unload_hlsacc_program cleanup failed: %d\n",
                cleanup_ret);
        }
    }
    if (range_created) {
        nvme_delete_memory_range_set(admin_fd, kComputeNsid, rsid);
    }
    if (output_created) {
        nvme_delete_slm_ns(admin_fd, output_mem_id);
    }
    if (input_created) {
        nvme_delete_slm_ns(admin_fd, input_mem_id);
    }
    close(io_fd);
    close(admin_fd);
    free(output_buffer);
    free(context_page);
    free(source_range_page);
    gettimeofday(&cleanup_end, nullptr);
    if (status == 0 && options.benchmark) {
        const double slm_create_ms = elapsed_ms(slm_create_start, slm_create_end);
        const double ssd_copy_ms = elapsed_ms(ssd_copy_start, ssd_copy_end);
        const double program_setup_ms =
            elapsed_ms(program_setup_start, program_setup_end);
        const double fpga_execute_ms = elapsed_ms(start, end);
        const double slm_read_ms = elapsed_ms(slm_read_start, slm_read_end);
        const double host_verify_ms = elapsed_ms(verify_start, verify_end);
        const double dump_write_ms = elapsed_ms(dump_start, dump_end);
        const double cleanup_ms = elapsed_ms(cleanup_start, cleanup_end);
        const double transport_ready_ms =
            ssd_copy_ms + fpga_execute_ms + slm_read_ms;
        const double data_path_ms =
            transport_ready_ms + host_verify_ms;
        const double one_shot_transport_ready_ms =
            elapsed_ms(pipeline_start, slm_read_end);
        const double one_shot_pipeline_ms =
            elapsed_ms(pipeline_start, verify_end);
        const double process_ms = elapsed_ms(application_start, cleanup_end);
        const double count = static_cast<double>(options.plaintext_bytes);

        printf(
            "benchmark_stage_ms slm_create=%.3f ssd_to_slm=%.3f "
            "program_setup=%.3f fpga_execute=%.3f slm_to_host=%.3f "
            "host_verify=%.3f dump_write=%.3f cleanup=%.3f "
            "transport_ready=%.3f data_path=%.3f "
            "one_shot_transport_ready=%.3f one_shot_pipeline=%.3f "
            "process=%.3f\n",
            slm_create_ms,
            ssd_copy_ms,
            program_setup_ms,
            fpga_execute_ms,
            slm_read_ms,
            host_verify_ms,
            dump_write_ms,
            cleanup_ms,
            transport_ready_ms,
            data_path_ms,
            one_shot_transport_ready_ms,
            one_shot_pipeline_ms,
            process_ms);
        printf(
            "BENCH_FPGA_CSV,%u,%u,%zu,%u,%s,%zu,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%.3f,%.3f\n",
            options.plaintext_bytes,
            options.input_lbas,
            options.slm_read_chunk_bytes,
            options.slm_read_queue_depth,
            layout,
            cipher_physical_bytes / options.plaintext_bytes,
            slm_create_ms,
            ssd_copy_ms,
            program_setup_ms,
            fpga_execute_ms,
            slm_read_ms,
            host_verify_ms,
            dump_write_ms,
            cleanup_ms,
            transport_ready_ms,
            data_path_ms,
            one_shot_transport_ready_ms,
            one_shot_pipeline_ms,
            process_ms,
            fpga_execute_ms * 1000.0 / count,
            data_path_ms * 1000.0 / count,
            count * 1000.0 / fpga_execute_ms,
            count * 1000.0 / data_path_ms);
    }
    return status;
}
