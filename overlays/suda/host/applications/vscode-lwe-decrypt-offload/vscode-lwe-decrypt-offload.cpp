#include <libnvme.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kLbaSize = 4096;
constexpr size_t kAxisBytes = 64;
constexpr size_t kSlmWriteChunkBytes = kLbaSize;
constexpr size_t kSlmReadChunkBytes = 128 * 1024;
constexpr int kEintrRetries = 16;
constexpr uint32_t kComputeNsid = 2;
constexpr uint32_t kMaskDimension = 2048;
constexpr uint32_t kRadixBlockCount = 4;
constexpr uint32_t kMessageWidth = 2;
constexpr uint32_t kCarryWidth = 2;
constexpr uint32_t kPaddingWidth = 1;
constexpr uint32_t kDeltaLog2 = 59;
constexpr uint64_t kDelta = uint64_t{1} << kDeltaLog2;
constexpr uint32_t kInputLayoutHpuNative = 1;
constexpr size_t kLogicalWordsPerLwe = kMaskDimension + 1;
constexpr size_t kHpuPcCount = 2;
constexpr size_t kHpuPcGroupWords = 16;
constexpr size_t kHpuPcDataWords = kMaskDimension / kHpuPcCount;
constexpr size_t kHpuPcSlotBytes = 3 * kLbaSize;
constexpr size_t kHpuPcSlotWords = kHpuPcSlotBytes / sizeof(uint64_t);
constexpr size_t kHpuNativeWordsPerLwe = kHpuPcCount * kHpuPcSlotWords;
constexpr size_t kHpuNativeBytesPerU8 =
    kRadixBlockCount * kHpuPcCount * kHpuPcSlotBytes;

const char* kDefaultKeyPath =
    "../../../device/operators/hls/lwe_encrypt/testdata/"
    "psi64_big_lwe_secret_key.bin";

enum class InputFormat {
    kAuto,
    kLweHls01,
    kHpuNative,
};

struct Options {
    const char* input_path = nullptr;
    const char* key_path = kDefaultKeyPath;
    const char* expected_path = nullptr;
    const char* output_path = "lwe_decrypt_fpga_plaintext.bin";
    const char* admin_device = "nvmq0";
    const char* io_device = "nvmq0n1";
    InputFormat input_format = InputFormat::kAuto;
    uint32_t plaintext_bytes = 0;
    uint32_t operator_type_id = 3;
    uint32_t program_id = 12;
    uint8_t expected_first = 0;
    bool expected_first_set = false;
    bool inspect_only = false;
    bool benchmark = false;
    bool skip_output = false;
};

struct CiphertextInput {
    std::vector<uint8_t> native_bytes;
    std::vector<uint8_t> expected;
    uint32_t plaintext_bytes = 0;
    std::string source_layout;
};

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

size_t round_up_to_lba(size_t value)
{
    return ((value + kLbaSize - 1) / kLbaSize) * kLbaSize;
}

void print_usage(const char* argv0)
{
    fprintf(
        stderr,
        "Usage: %s --input PATH [options]\n"
        "  --input PATH       LWEHLS01 logical dump or raw HPU-native payload\n"
        "  --input-format F   auto|lwehls01|hpu-native (default: auto)\n"
        "  --plaintext-bytes N\n"
        "                     raw HPU-native u8 count; LWEHLS01 infers it\n"
        "  --key PATH         2048-byte binary Big-LWE key\n"
        "  --expect-file PATH expected packed u8 file for raw input\n"
        "  --expect N         optionally check the first decrypted u8\n"
        "  --output PATH      packed decrypted u8 output file\n"
        "  --operator-type N  SUDA operator type id (default: 3)\n"
        "  --program-id N     compute program slot (default: 12)\n"
        "  --admin DEV        NVMe admin device (default: nvmq0)\n"
        "  --io DEV           NVMe I/O device (default: nvmq0n1)\n"
        "  --inspect-only     parse and repack input without accessing FPGA\n"
        "  --benchmark        print stage timings\n"
        "  --skip-output      do not write the decrypted u8 file\n"
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
        if (strcmp(arg, "--inspect-only") == 0) {
            options->inspect_only = true;
            continue;
        }
        if (strcmp(arg, "--benchmark") == 0) {
            options->benchmark = true;
            continue;
        }
        if (strcmp(arg, "--skip-output") == 0) {
            options->skip_output = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", arg);
            return false;
        }

        const char* text = argv[++i];
        uint64_t value = 0;
        if (strcmp(arg, "--input") == 0) {
            options->input_path = text;
        } else if (strcmp(arg, "--key") == 0) {
            options->key_path = text;
        } else if (strcmp(arg, "--expect-file") == 0) {
            options->expected_path = text;
        } else if (strcmp(arg, "--output") == 0) {
            options->output_path = text;
        } else if (strcmp(arg, "--admin") == 0) {
            options->admin_device = text;
        } else if (strcmp(arg, "--io") == 0) {
            options->io_device = text;
        } else if (strcmp(arg, "--input-format") == 0) {
            if (strcmp(text, "auto") == 0) {
                options->input_format = InputFormat::kAuto;
            } else if (strcmp(text, "lwehls01") == 0) {
                options->input_format = InputFormat::kLweHls01;
            } else if (strcmp(text, "hpu-native") == 0) {
                options->input_format = InputFormat::kHpuNative;
            } else {
                fprintf(stderr, "Invalid input format: %s\n", text);
                return false;
            }
        } else if (strcmp(arg, "--plaintext-bytes") == 0) {
            if (!parse_u64(text, &value) || value == 0 || value > UINT32_MAX) {
                fprintf(stderr, "Invalid plaintext byte count: %s\n", text);
                return false;
            }
            options->plaintext_bytes = static_cast<uint32_t>(value);
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
        } else if (strcmp(arg, "--expect") == 0) {
            if (!parse_u64(text, &value) || value > UINT8_MAX) {
                fprintf(stderr, "Invalid expected u8 value: %s\n", text);
                return false;
            }
            options->expected_first = static_cast<uint8_t>(value);
            options->expected_first_set = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return false;
        }
    }

    if (options->input_path == nullptr) {
        fprintf(stderr, "--input is required\n");
        return false;
    }
    return true;
}

bool read_file(const char* path, std::vector<uint8_t>* bytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        fprintf(stderr, "Unable to open file: %s\n", path);
        return false;
    }
    std::streamoff size = file.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > SIZE_MAX) {
        fprintf(stderr, "Unable to determine file size: %s\n", path);
        return false;
    }
    bytes->resize(static_cast<size_t>(size));
    file.seekg(0);
    if (!bytes->empty()) {
        file.read(
            reinterpret_cast<char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    }
    if (!file) {
        fprintf(stderr, "Unable to read file: %s\n", path);
        return false;
    }
    return true;
}

bool read_binary_key(const char* path, std::vector<uint8_t>* key)
{
    if (!read_file(path, key)) {
        return false;
    }
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

bool read_le64(const std::vector<uint8_t>& bytes, size_t* cursor, uint64_t* value)
{
    if (*cursor > bytes.size() || bytes.size() - *cursor < sizeof(uint64_t)) {
        return false;
    }
    uint64_t parsed = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        parsed |= static_cast<uint64_t>(bytes[*cursor + i]) << (i * 8);
    }
    *cursor += sizeof(uint64_t);
    *value = parsed;
    return true;
}

void store_le64(std::vector<uint8_t>* bytes, size_t word_index, uint64_t value)
{
    size_t offset = word_index * sizeof(uint64_t);
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        (*bytes)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint64_t load_le64(const std::vector<uint8_t>& bytes, size_t word_index)
{
    size_t offset = word_index * sizeof(uint64_t);
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
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

bool parse_lwehls01(
    const std::vector<uint8_t>& file_bytes,
    const Options& options,
    CiphertextInput* input)
{
    if (file_bytes.size() < 8 ||
        memcmp(file_bytes.data(), "LWEHLS01", 8) != 0) {
        fprintf(stderr, "Input is not an LWEHLS01 dump\n");
        return false;
    }

    size_t cursor = 8;
    uint64_t version = 0;
    uint64_t mask_dimension = 0;
    uint64_t item_count = 0;
    uint64_t radix_blocks = 0;
    uint64_t message_width = 0;
    uint64_t carry_width = 0;
    uint64_t padding_width = 0;
    uint64_t delta_log2 = 0;
    uint64_t word_count = 0;
    if (!read_le64(file_bytes, &cursor, &version) ||
        !read_le64(file_bytes, &cursor, &mask_dimension) ||
        !read_le64(file_bytes, &cursor, &item_count) ||
        !read_le64(file_bytes, &cursor, &radix_blocks) ||
        !read_le64(file_bytes, &cursor, &message_width) ||
        !read_le64(file_bytes, &cursor, &carry_width) ||
        !read_le64(file_bytes, &cursor, &padding_width) ||
        !read_le64(file_bytes, &cursor, &delta_log2) ||
        !read_le64(file_bytes, &cursor, &word_count)) {
        fprintf(stderr, "Truncated LWEHLS01 header\n");
        return false;
    }

    if (version != 1 || mask_dimension != kMaskDimension ||
        radix_blocks != kRadixBlockCount || message_width != kMessageWidth ||
        carry_width != kCarryWidth || padding_width != kPaddingWidth ||
        delta_log2 != kDeltaLog2 || item_count == 0 ||
        item_count > UINT32_MAX) {
        fprintf(
            stderr,
            "LWEHLS01 parameters do not match psi64 u8 radix format\n");
        return false;
    }
    if (options.plaintext_bytes != 0 && options.plaintext_bytes != item_count) {
        fprintf(
            stderr,
            "--plaintext-bytes %u disagrees with dump item_count=%llu\n",
            options.plaintext_bytes,
            static_cast<unsigned long long>(item_count));
        return false;
    }

    const uint64_t expected_word_count =
        item_count * kRadixBlockCount * kLogicalWordsPerLwe;
    if (word_count != expected_word_count) {
        fprintf(
            stderr,
            "LWEHLS01 word count mismatch: got=%llu expected=%llu\n",
            static_cast<unsigned long long>(word_count),
            static_cast<unsigned long long>(expected_word_count));
        return false;
    }
    const uint64_t native_bytes = item_count * kHpuNativeBytesPerU8;
    if (native_bytes > INT_MAX) {
        fprintf(stderr, "HPU-native input exceeds the runtime size limit\n");
        return false;
    }

    input->expected.clear();
    input->expected.reserve(static_cast<size_t>(item_count));
    for (uint64_t i = 0; i < item_count; ++i) {
        uint64_t clear = 0;
        if (!read_le64(file_bytes, &cursor, &clear) || clear > UINT8_MAX) {
            fprintf(stderr, "Invalid clear reference in LWEHLS01 dump\n");
            return false;
        }
        input->expected.push_back(static_cast<uint8_t>(clear));
    }

    std::vector<uint64_t> logical_words;
    logical_words.reserve(static_cast<size_t>(word_count));
    for (uint64_t i = 0; i < word_count; ++i) {
        uint64_t word = 0;
        if (!read_le64(file_bytes, &cursor, &word)) {
            fprintf(stderr, "Truncated LWEHLS01 ciphertext payload\n");
            return false;
        }
        logical_words.push_back(word);
    }
    if (cursor != file_bytes.size()) {
        fprintf(
            stderr,
            "LWEHLS01 dump has %zu trailing bytes\n",
            file_bytes.size() - cursor);
        return false;
    }

    input->native_bytes.assign(static_cast<size_t>(native_bytes), 0);
    const size_t lwe_count =
        static_cast<size_t>(item_count) * kRadixBlockCount;
    for (size_t lwe_index = 0; lwe_index < lwe_count; ++lwe_index) {
        const size_t logical_base = lwe_index * kLogicalWordsPerLwe;
        const size_t native_base = lwe_index * kHpuNativeWordsPerLwe;
        for (size_t natural_index = 0;
             natural_index < kMaskDimension;
             ++natural_index) {
            const size_t hpu_index = reverse_psi64_mask_index(natural_index);
            const size_t group = hpu_index / kHpuPcGroupWords;
            const size_t lane = hpu_index % kHpuPcGroupWords;
            const size_t pc = group % kHpuPcCount;
            const size_t pc_offset =
                (group / kHpuPcCount) * kHpuPcGroupWords + lane;
            store_le64(
                &input->native_bytes,
                native_base + pc * kHpuPcSlotWords + pc_offset,
                logical_words[logical_base + natural_index]);
        }
        store_le64(
            &input->native_bytes,
            native_base + kHpuPcDataWords,
            logical_words[logical_base + kMaskDimension]);
    }

    input->plaintext_bytes = static_cast<uint32_t>(item_count);
    input->source_layout = "LWEHLS01-logical->hpu-native-psi64-v80";
    return true;
}

bool parse_hpu_native(
    const std::vector<uint8_t>& file_bytes,
    const Options& options,
    CiphertextInput* input)
{
    uint64_t item_count = options.plaintext_bytes;
    if (item_count == 0) {
        if (file_bytes.empty() || file_bytes.size() % kHpuNativeBytesPerU8 != 0) {
            fprintf(
                stderr,
                "Raw HPU-native size is not an exact u8 multiple; pass --plaintext-bytes\n");
            return false;
        }
        item_count = file_bytes.size() / kHpuNativeBytesPerU8;
    }
    if (item_count == 0 || item_count > UINT32_MAX ||
        item_count * kHpuNativeBytesPerU8 > INT_MAX) {
        fprintf(stderr, "Raw HPU-native input size is unsupported\n");
        return false;
    }
    const size_t payload_bytes =
        static_cast<size_t>(item_count) * kHpuNativeBytesPerU8;
    if (file_bytes.size() < payload_bytes) {
        fprintf(
            stderr,
            "Raw HPU-native file is too short: got=%zu expected=%zu\n",
            file_bytes.size(),
            payload_bytes);
        return false;
    }
    if (std::any_of(
            file_bytes.begin() + payload_bytes,
            file_bytes.end(),
            [](uint8_t value) { return value != 0; })) {
        fprintf(stderr, "Raw HPU-native file has non-zero trailing bytes\n");
        return false;
    }

    input->native_bytes.assign(
        file_bytes.begin(),
        file_bytes.begin() + payload_bytes);
    input->plaintext_bytes = static_cast<uint32_t>(item_count);
    input->source_layout = "raw-hpu-native-psi64-v80";
    return true;
}

bool load_ciphertext_input(const Options& options, CiphertextInput* input)
{
    std::vector<uint8_t> file_bytes;
    if (!read_file(options.input_path, &file_bytes)) {
        return false;
    }

    InputFormat format = options.input_format;
    if (format == InputFormat::kAuto) {
        format = file_bytes.size() >= 8 &&
                         memcmp(file_bytes.data(), "LWEHLS01", 8) == 0
            ? InputFormat::kLweHls01
            : InputFormat::kHpuNative;
    }
    if (format == InputFormat::kLweHls01) {
        return parse_lwehls01(file_bytes, options, input);
    }
    return parse_hpu_native(file_bytes, options, input);
}

bool apply_expected_options(const Options& options, CiphertextInput* input)
{
    if (options.expected_path != nullptr) {
        std::vector<uint8_t> expected;
        if (!read_file(options.expected_path, &expected)) {
            return false;
        }
        if (expected.size() != input->plaintext_bytes) {
            fprintf(
                stderr,
                "Expected file size mismatch: got=%zu expected=%u\n",
                expected.size(),
                input->plaintext_bytes);
            return false;
        }
        if (!input->expected.empty() && input->expected != expected) {
            fprintf(stderr, "Expected file disagrees with LWEHLS01 clear references\n");
            return false;
        }
        input->expected = std::move(expected);
    }
    if (options.expected_first_set && !input->expected.empty() &&
        input->expected[0] != options.expected_first) {
        fprintf(
            stderr,
            "--expect %u disagrees with input reference %u\n",
            options.expected_first,
            input->expected[0]);
        return false;
    }
    return true;
}

bool verify_with_host_reference(
    const CiphertextInput& input,
    const std::vector<uint8_t>& key,
    const Options& options,
    std::vector<uint8_t>* clear_values)
{
    clear_values->assign(input.plaintext_bytes, 0);
    for (size_t item_index = 0;
         item_index < input.plaintext_bytes;
         ++item_index) {
        uint8_t clear_u8 = 0;
        for (size_t block = 0; block < kRadixBlockCount; ++block) {
            const size_t lwe_index = item_index * kRadixBlockCount + block;
            const size_t native_base = lwe_index * kHpuNativeWordsPerLwe;
            uint64_t dot = 0;
            for (size_t natural_index = 0;
                 natural_index < kMaskDimension;
                 ++natural_index) {
                const size_t hpu_index = reverse_psi64_mask_index(natural_index);
                const size_t group = hpu_index / kHpuPcGroupWords;
                const size_t lane = hpu_index % kHpuPcGroupWords;
                const size_t pc = group % kHpuPcCount;
                const size_t pc_offset =
                    (group / kHpuPcCount) * kHpuPcGroupWords + lane;
                if (key[natural_index] != 0) {
                    dot += load_le64(
                        input.native_bytes,
                        native_base + pc * kHpuPcSlotWords + pc_offset);
                }
            }
            const uint64_t body = load_le64(
                input.native_bytes,
                native_base + kHpuPcDataWords);
            const uint64_t phase = body - dot;
            const uint64_t decoded =
                ((phase + (kDelta / 2)) >> kDeltaLog2) & 3U;
            clear_u8 |= static_cast<uint8_t>(
                decoded << (block * kMessageWidth));
        }
        (*clear_values)[item_index] = clear_u8;
    }

    if (!input.expected.empty() && *clear_values != input.expected) {
        size_t mismatch = 0;
        while (mismatch < input.expected.size() &&
               (*clear_values)[mismatch] == input.expected[mismatch]) {
            ++mismatch;
        }
        fprintf(
            stderr,
            "Host reference decryption mismatch at byte %zu: got=%u expected=%u\n",
            mismatch,
            (*clear_values)[mismatch],
            input.expected[mismatch]);
        return false;
    }
    if (options.expected_first_set &&
        (*clear_values)[0] != options.expected_first) {
        fprintf(
            stderr,
            "Host reference first-byte mismatch: got=%u expected=%u\n",
            (*clear_values)[0],
            options.expected_first);
        return false;
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
    uint32_t plaintext_bytes)
{
    memset(context, 0, kLbaSize);
    // Host offset zero maps to HLS static context word 3 in OperatorController.
    store_u32(context, 0, plaintext_bytes);
    store_u32(context, 4, kMaskDimension);
    store_u64(context, 8, kDelta);
    store_u32(context, 16, kMessageWidth);
    store_u32(context, 20, kRadixBlockCount);
    store_u32(context, 24, kInputLayoutHpuNative);

    // Host offset 64 maps to HLS context word 4.
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
    uint64_t estimated_us = std::max<uint64_t>(1000, plaintext_bytes * 100ULL);
    uint64_t maximum_us = std::max<uint64_t>(10000, plaintext_bytes * 2000ULL);
    program->esti_executed_time = static_cast<uint32_t>(
        std::min<uint64_t>(estimated_us, UINT32_MAX));
    program->max_responded_time = static_cast<uint32_t>(
        std::min<uint64_t>(maximum_us, UINT32_MAX));
}

int transfer_slm(
    int io_fd,
    unsigned int mem_id,
    size_t bytes,
    void* buffer,
    bool write)
{
    uint8_t* data = static_cast<uint8_t*>(buffer);
    const size_t chunk_limit =
        write ? kSlmWriteChunkBytes : kSlmReadChunkBytes;
    for (size_t offset = 0; offset < bytes;) {
        size_t chunk = std::min(chunk_limit, bytes - offset);
        int retries = 0;
        while (true) {
            errno = 0;
            int ret = write
                ? nvme_slm_write(
                      io_fd,
                      mem_id,
                      static_cast<int>(offset),
                      static_cast<int>(chunk),
                      data + offset)
                : nvme_slm_read(
                      io_fd,
                      mem_id,
                      static_cast<int>(offset),
                      static_cast<int>(chunk),
                      data + offset);
            if (ret == 0) {
                break;
            }
            if (ret == -1 && errno == EINTR && retries < kEintrRetries) {
                ++retries;
                continue;
            }
            fprintf(
                stderr,
                "nvme_slm_%s failed: ret=%d mem_id=%u offset=%zu length=%zu "
                "errno=%d (%s)\n",
                write ? "write" : "read",
                ret,
                mem_id,
                offset,
                chunk,
                errno,
                strerror(errno));
            return ret == 0 ? -1 : ret;
        }
        offset += chunk;
    }
    return 0;
}

bool write_plaintext(const char* path, const uint8_t* data, size_t bytes)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "Unable to open output file: %s\n", path);
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), bytes);
    if (!file) {
        fprintf(stderr, "Unable to write output file: %s\n", path);
        return false;
    }
    return true;
}

void print_prefix(const char* label, const uint8_t* data, size_t bytes)
{
    printf("%s=", label);
    for (size_t i = 0; i < bytes && i < 16; ++i) {
        printf("%02x", data[i]);
    }
    if (bytes > 16) {
        printf("...");
    }
    printf("\n");
}

int run_fpga(
    const Options& options,
    const CiphertextInput& input,
    const std::vector<uint8_t>& key,
    double parse_pack_ms)
{
    const size_t input_bytes = input.native_bytes.size();
    const size_t output_bytes =
        round_up_to_lba(static_cast<size_t>(input.plaintext_bytes) + kAxisBytes);
    void* input_buffer = nullptr;
    void* output_buffer = nullptr;
    void* context_page = nullptr;
    if (posix_memalign(&input_buffer, kLbaSize, input_bytes) != 0 ||
        posix_memalign(&output_buffer, kLbaSize, output_bytes) != 0 ||
        posix_memalign(&context_page, kLbaSize, kLbaSize) != 0) {
        fprintf(stderr, "Unable to allocate aligned host buffers\n");
        free(input_buffer);
        free(output_buffer);
        free(context_page);
        return 1;
    }
    memcpy(input_buffer, input.native_bytes.data(), input_bytes);
    memset(output_buffer, 0, output_bytes);
    build_context(
        static_cast<uint8_t*>(context_page),
        key,
        input.plaintext_bytes);

    int admin_fd = nvme_open(options.admin_device);
    int io_fd = nvme_open(options.io_device);
    if (admin_fd < 0 || io_fd < 0) {
        fprintf(
            stderr,
            "Unable to open NVMe devices admin=%s io=%s\n",
            options.admin_device,
            options.io_device);
        if (admin_fd >= 0) {
            close(admin_fd);
        }
        if (io_fd >= 0) {
            close(io_fd);
        }
        free(input_buffer);
        free(output_buffer);
        free(context_page);
        return 1;
    }

    unsigned int input_mem_id = 0;
    unsigned int output_mem_id = 0;
    unsigned int rsid = 0;
    unsigned int result_bytes = 0;
    bool input_created = false;
    bool output_created = false;
    bool range_created = false;
    bool program_loaded = false;
    bool program_activated = false;
    int status = 1;
    int ret = 0;
    union memory_range_set_decriptor ranges[2];
    struct hlsacccompute_program program;
    memset(ranges, 0, sizeof(ranges));
    memset(&program, 0, sizeof(program));

    Clock::time_point pipeline_start = Clock::now();
    Clock::time_point slm_create_start = {};
    Clock::time_point slm_create_end = {};
    Clock::time_point host_to_slm_start = {};
    Clock::time_point host_to_slm_end = {};
    Clock::time_point setup_start = {};
    Clock::time_point setup_end = {};
    Clock::time_point execute_start = {};
    Clock::time_point execute_end = {};
    Clock::time_point slm_to_host_start = {};
    Clock::time_point slm_to_host_end = {};
    Clock::time_point verify_start = {};
    Clock::time_point verify_end = {};

    fprintf(
        stderr,
        "[lwe_decrypt] sizing plaintext_bytes=%u hpu_native_bytes=%zu "
        "output_slm_bytes=%zu slm_write_chunk_bytes=%zu "
        "slm_write_requests=%zu slm_read_chunk_bytes=%zu\n",
        input.plaintext_bytes,
        input_bytes,
        output_bytes,
        kSlmWriteChunkBytes,
        (input_bytes + kSlmWriteChunkBytes - 1) / kSlmWriteChunkBytes,
        kSlmReadChunkBytes);

    do {
        slm_create_start = Clock::now();
        fprintf(stderr, "[lwe_decrypt] creating input SLM\n");
        ret = nvme_create_slm_ns(
            admin_fd,
            &input_mem_id,
            static_cast<int>(input_bytes));
        if (ret != 0) {
            fprintf(stderr, "nvme_create_slm_ns(input) failed: %d\n", ret);
            break;
        }
        input_created = true;

        fprintf(stderr, "[lwe_decrypt] creating output SLM\n");
        ret = nvme_create_slm_ns(
            admin_fd,
            &output_mem_id,
            static_cast<int>(output_bytes));
        if (ret != 0) {
            fprintf(stderr, "nvme_create_slm_ns(output) failed: %d\n", ret);
            break;
        }
        output_created = true;
        slm_create_end = Clock::now();

        host_to_slm_start = Clock::now();
        fprintf(
            stderr,
            "[lwe_decrypt] writing HPU-native ciphertext to input SLM "
            "in %zu-byte chunks\n",
            kSlmWriteChunkBytes);
        ret = transfer_slm(
            io_fd,
            input_mem_id,
            input_bytes,
            input_buffer,
            true);
        if (ret != 0) {
            break;
        }
        host_to_slm_end = Clock::now();

        setup_start = Clock::now();
        ranges[0].payload.mnsid = input_mem_id;
        ranges[0].payload.length = static_cast<unsigned int>(input_bytes);
        ranges[0].payload.starting_byte = 0;
        ranges[0].payload.flag =
            memory_range_descriptor::mdes_flag::MEM_RANGE_DEVICE_MEM;
        ranges[1].payload.mnsid = output_mem_id;
        ranges[1].payload.length = static_cast<unsigned int>(output_bytes);
        ranges[1].payload.starting_byte = 0;
        ranges[1].payload.flag =
            memory_range_descriptor::mdes_flag::MEM_RANGE_DEVICE_MEM;

        fprintf(stderr, "[lwe_decrypt] creating memory range set\n");
        ret = nvme_create_memory_range_set(
            admin_fd,
            kComputeNsid,
            &rsid,
            2,
            ranges);
        if (ret != 0) {
            fprintf(stderr, "nvme_create_memory_range_set failed: %d\n", ret);
            break;
        }
        range_created = true;

        build_program(
            &program,
            options.operator_type_id,
            options.program_id,
            input.plaintext_bytes);
        fprintf(stderr, "[lwe_decrypt] clearing stale FPGA program slot\n");
        ret = nvme_unload_hlsacc_program(
            admin_fd,
            options.program_id,
            kComputeNsid);
        if (ret != 0) {
            fprintf(
                stderr,
                "nvme_unload_hlsacc_program(pre-load) failed: %d\n",
                ret);
            break;
        }

        fprintf(stderr, "[lwe_decrypt] loading FPGA program\n");
        ret = nvme_load_hlsacc_program(
            admin_fd,
            sizeof(program),
            options.program_id,
            kComputeNsid,
            &program);
        if (ret != 0) {
            fprintf(stderr, "nvme_load_hlsacc_program failed: %d\n", ret);
            break;
        }
        program_loaded = true;

        fprintf(stderr, "[lwe_decrypt] activating FPGA program\n");
        ret = nvme_activate_program(
            admin_fd,
            options.program_id,
            kComputeNsid);
        if (ret != 0) {
            fprintf(stderr, "nvme_activate_program failed: %d\n", ret);
            break;
        }
        program_activated = true;
        setup_end = Clock::now();

        execute_start = Clock::now();
        fprintf(stderr, "[lwe_decrypt] executing lwe_decrypt on FPGA\n");
        errno = 0;
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
        const int execute_errno = errno;
        execute_end = Clock::now();
        if (ret != 0) {
            fprintf(
                stderr,
                "nvme_execute_hlsacc_program failed: ret=%d errno=%d (%s) "
                "waited_ms=%.3f\n",
                ret,
                execute_errno,
                strerror(execute_errno),
                elapsed_ms(execute_start, execute_end));
            break;
        }
        fprintf(
            stderr,
            "[lwe_decrypt] FPGA execution returned result_bytes=%u "
            "expected_clear_bytes=%u\n",
            result_bytes,
            input.plaintext_bytes);
        if (result_bytes < input.plaintext_bytes || result_bytes > output_bytes) {
            fprintf(
                stderr,
                "FPGA returned an invalid result size; an HLS error/finish packet may have arrived before plaintext\n");
            break;
        }

        slm_to_host_start = Clock::now();
        fprintf(stderr, "[lwe_decrypt] reading packed u8 output SLM\n");
        ret = transfer_slm(
            io_fd,
            output_mem_id,
            output_bytes,
            output_buffer,
            false);
        if (ret != 0) {
            break;
        }
        slm_to_host_end = Clock::now();

        verify_start = Clock::now();
        const uint8_t* actual = static_cast<const uint8_t*>(output_buffer);
        if (!input.expected.empty() &&
            !std::equal(input.expected.begin(), input.expected.end(), actual)) {
            size_t mismatch = 0;
            while (mismatch < input.expected.size() &&
                   input.expected[mismatch] == actual[mismatch]) {
                ++mismatch;
            }
            fprintf(
                stderr,
                "Decryption mismatch at byte %zu: got=%u expected=%u\n",
                mismatch,
                actual[mismatch],
                input.expected[mismatch]);
            break;
        }
        if (options.expected_first_set && actual[0] != options.expected_first) {
            fprintf(
                stderr,
                "First decrypted byte mismatch: got=%u expected=%u\n",
                actual[0],
                options.expected_first);
            break;
        }
        if (!options.skip_output &&
            !write_plaintext(
                options.output_path,
                actual,
                input.plaintext_bytes)) {
            break;
        }
        verify_end = Clock::now();

        printf("lwe_decrypt FPGA execution passed\n");
        printf("decrypted_count=%u\n", input.plaintext_bytes);
        printf("decrypted_first_u8=0x%02x (%u)\n", actual[0], actual[0]);
        print_prefix("decrypted_prefix", actual, input.plaintext_bytes);
        printf("input_layout=%s\n", input.source_layout.c_str());
        printf("hpu_native_input_bytes=%zu output_clear_bytes=%u\n",
               input_bytes,
               input.plaintext_bytes);
        printf("operator_type_id=%u program_id=%u rsid=%u\n",
               options.operator_type_id,
               options.program_id,
               rsid);
        printf("exec_result=%u output_slm_bytes=%zu\n",
               result_bytes,
               output_bytes);
        printf("secret_key_context_loaded=yes\n");
        printf("correctness_checked=%s\n",
               (!input.expected.empty() || options.expected_first_set)
                   ? "yes"
                   : "no");
        printf("plaintext_output=%s\n",
               options.skip_output ? "skipped" : options.output_path);
        status = 0;
    } while (false);

    Clock::time_point cleanup_start = Clock::now();
    if (program_activated) {
        int cleanup_ret = nvme_deactivate_program(
            admin_fd,
            options.program_id,
            kComputeNsid);
        if (cleanup_ret != 0) {
            fprintf(
                stderr,
                "warning: nvme_deactivate_program cleanup failed: %d\n",
                cleanup_ret);
        }
    }
    if (program_loaded) {
        int cleanup_ret = nvme_unload_hlsacc_program(
            admin_fd,
            options.program_id,
            kComputeNsid);
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
    free(input_buffer);
    free(output_buffer);
    free(context_page);
    Clock::time_point cleanup_end = Clock::now();

    if (status == 0 && options.benchmark) {
        printf(
            "benchmark_stage_ms parse_pack=%.3f slm_create=%.3f "
            "host_to_slm=%.3f program_setup=%.3f fpga_execute=%.3f "
            "slm_to_host=%.3f verify_write=%.3f cleanup=%.3f "
            "fpga_pipeline=%.3f\n",
            parse_pack_ms,
            elapsed_ms(slm_create_start, slm_create_end),
            elapsed_ms(host_to_slm_start, host_to_slm_end),
            elapsed_ms(setup_start, setup_end),
            elapsed_ms(execute_start, execute_end),
            elapsed_ms(slm_to_host_start, slm_to_host_end),
            elapsed_ms(verify_start, verify_end),
            elapsed_ms(cleanup_start, cleanup_end),
            elapsed_ms(pipeline_start, slm_to_host_end));
    }
    return status;
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

    const Clock::time_point parse_start = Clock::now();
    CiphertextInput input;
    if (!load_ciphertext_input(options, &input) ||
        !apply_expected_options(options, &input)) {
        return 1;
    }
    std::vector<uint8_t> key;
    if (!read_binary_key(options.key_path, &key)) {
        return 1;
    }
    std::vector<uint8_t> host_reference;
    if (!verify_with_host_reference(input, key, options, &host_reference)) {
        return 1;
    }
    const Clock::time_point parse_end = Clock::now();

    printf("source_ciphertext=%s\n", options.input_path);
    printf("source_layout=%s\n", input.source_layout.c_str());
    printf("plaintext_bytes=%u hpu_native_bytes=%zu\n",
           input.plaintext_bytes,
           input.native_bytes.size());
    if (!input.expected.empty()) {
        print_prefix("expected_prefix", input.expected.data(), input.expected.size());
    }
    print_prefix(
        "host_reference_prefix",
        host_reference.data(),
        host_reference.size());
    printf("key_file=%s key_coefficients=%zu\n", options.key_path, key.size());
    printf("host_reference_decrypt_checked=yes\n");

    if (options.inspect_only) {
        printf("inspect_only=passed\n");
        return 0;
    }
    return run_fpga(
        options,
        input,
        key,
        elapsed_ms(parse_start, parse_end));
}
