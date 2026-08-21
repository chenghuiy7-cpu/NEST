#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#include "lwe_encrypt.hpp"

static const char *PSI64_BIG_LWE_SECRET_KEY_FILE =
    "testdata/"
    "psi64_big_lwe_secret_key.bin";
static const char *PSI64_U8_RADIX_CIPHERTEXT_FILE =
    "testdata/"
    "psi64_u8_radix_hls_ciphertexts.bin";

static_assert(LWE_ENCRYPT_HPU_SMALL_LWE_DIMENSION == 879, "HPU small LWE dimension mismatch");
static_assert(LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION == 2048, "HPU Big LWE dimension mismatch");
static_assert(LWE_ENCRYPT_HPU_DELTA == (1ULL << 59), "HPU shortint delta mismatch");

static uint64_t next_random_ref(uint64_t &state)
{
    if (state == 0) {
        state = 0x9e3779b97f4a7c15ULL;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static void set_context_u64(ap_uint<512> &word, int idx, uint64_t value)
{
    word.range(idx * 64 + 63, idx * 64) = value;
}

static uint64_t get_pkt_u64(const Acc_Data_Pkt &pkt, int idx)
{
    ap_uint<512> data = pkt.data;
    return data.range(idx * 64 + 63, idx * 64).to_uint64();
}

static void clear_context(ap_uint<512> context[256])
{
    for (int i = 0; i < 256; i++) {
        context[i] = 0;
    }
}

static void set_secret_key_bit(ap_uint<512> context[256], uint32_t index, bool value)
{
    const uint32_t context_word = LWE_ENCRYPT_KEY_CONTEXT_BASE + (index >> 9);
    const uint32_t bit_index = index & 511;
    assert(context_word < 256);
    context[context_word][bit_index] = value ? 1 : 0;
}

static std::vector<unsigned> load_saved_secret_key(const char *path, uint32_t expected_dimension)
{
    std::ifstream file(path, std::ios::binary);
    assert(file.good());

    std::vector<unsigned> secret_key(expected_dimension);
    for (uint32_t i = 0; i < expected_dimension; i++) {
        char byte = 0;
        file.read(&byte, 1);
        assert(file.gcount() == 1);

        unsigned bit = static_cast<unsigned>(static_cast<unsigned char>(byte));
        assert(bit == 0 || bit == 1);
        secret_key[i] = bit;
    }

    char extra = 0;
    file.read(&extra, 1);
    assert(file.gcount() == 0);
    return secret_key;
}

static size_t count_secret_key_ones(const std::vector<unsigned> &secret_key)
{
    size_t ones = 0;
    for (size_t i = 0; i < secret_key.size(); i++) {
        if (secret_key[i] != 0) {
            ones++;
        }
    }
    return ones;
}

static void configure_context(
    ap_uint<512> context[256],
    uint32_t mask_dimension,
    uint32_t request_count,
    uint32_t input_mode,
    uint32_t noise_mode,
    uint32_t noise_bound_log2,
    uint64_t delta,
    uint64_t seed,
    uint64_t nonce,
    uint32_t output_layout = LWE_ENCRYPT_OUTPUT_CPU_LWE)
{
    ap_uint<512> &cfg = context[LWE_ENCRYPT_STATIC_CONTEXT_BASE];
    cfg.range(31, 0) = mask_dimension;
    cfg.range(63, 32) = request_count;
    cfg.range(95, 64) = input_mode;
    cfg.range(127, 96) = noise_mode;
    cfg.range(159, 128) = noise_bound_log2;
    cfg.range(191, 160) = output_layout;
    set_context_u64(cfg, 4, delta);
    set_context_u64(cfg, 5, seed);
    set_context_u64(cfg, 6, nonce);
}

static void write_requests(
    Acc_Data &data_in,
    const std::vector<uint64_t> &inputs,
    const std::vector<uint64_t> &noises)
{
    assert(inputs.size() == noises.size());

    for (size_t req = 0; req < inputs.size(); req++) {
        Acc_Data_Pkt in_pkt;
        in_pkt.data = 0;
        in_pkt.keep = -1;
        in_pkt.strb = -1;
        in_pkt.user = 0;
        in_pkt.last = 0;
        in_pkt.id = 0;
        in_pkt.dest = 0;
        in_pkt.data.range(63, 0) = inputs[req];
        in_pkt.data.range(127, 64) = noises[req];
        data_in.write(in_pkt);
    }

    Acc_Data_Pkt done_pkt;
    done_pkt.data = 0;
    done_pkt.keep = -1;
    done_pkt.strb = -1;
    done_pkt.user = 0xff;
    done_pkt.last = 1;
    done_pkt.id = 0;
    done_pkt.dest = 0;
    data_in.write(done_pkt);
}

static void write_u8_radix_requests(
    Acc_Data &data_in,
    const std::vector<uint8_t> &inputs)
{
    const size_t packet_count = (inputs.size() + 63) / 64;
    for (size_t packet_index = 0; packet_index < packet_count; packet_index++) {
        Acc_Data_Pkt in_pkt;
        in_pkt.data = 0;
        in_pkt.keep = 0;
        in_pkt.strb = 0;
        in_pkt.user = 0;
        in_pkt.last = 0;
        in_pkt.id = 0;
        in_pkt.dest = 0;

        for (size_t byte_lane = 0; byte_lane < 64; byte_lane++) {
            size_t clear_index = packet_index * 64 + byte_lane;
            if (clear_index >= inputs.size()) {
                break;
            }
            in_pkt.data.range(byte_lane * 8 + 7, byte_lane * 8) = inputs[clear_index];
            in_pkt.keep[byte_lane] = 1;
            in_pkt.strb[byte_lane] = 1;
        }
        data_in.write(in_pkt);
    }

    Acc_Data_Pkt done_pkt;
    done_pkt.data = 0;
    done_pkt.keep = -1;
    done_pkt.strb = -1;
    done_pkt.user = 0xff;
    done_pkt.last = 1;
    done_pkt.id = 0;
    done_pkt.dest = 0;
    data_in.write(done_pkt);
}

static void write_le64(std::ofstream &file, uint64_t value)
{
    char bytes[8];
    for (int i = 0; i < 8; i++) {
        bytes[i] = static_cast<char>((value >> (i * 8)) & 0xff);
    }
    file.write(bytes, sizeof(bytes));
}

static void dump_u8_radix_ciphertexts(
    const char *path,
    const std::vector<uint8_t> &inputs,
    const std::vector<uint64_t> &ciphertext_words,
    uint32_t mask_dimension)
{
    std::ofstream file(path, std::ios::binary);
    assert(file.good());

    file.write("LWEHLS01", 8);
    write_le64(file, 1);
    write_le64(file, mask_dimension);
    write_le64(file, static_cast<uint64_t>(inputs.size()));
    write_le64(file, LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT);
    write_le64(file, LWE_ENCRYPT_HPU_MESSAGE_WIDTH);
    write_le64(file, LWE_ENCRYPT_HPU_CARRY_WIDTH);
    write_le64(file, LWE_ENCRYPT_HPU_PADDING_BIT_WIDTH);
    write_le64(file, LWE_ENCRYPT_HPU_DELTA_LOG2);
    write_le64(file, static_cast<uint64_t>(ciphertext_words.size()));

    for (size_t i = 0; i < inputs.size(); i++) {
        write_le64(file, inputs[i]);
    }
    for (size_t i = 0; i < ciphertext_words.size(); i++) {
        write_le64(file, ciphertext_words[i]);
    }

    assert(file.good());
}

static std::vector<uint64_t> read_ciphertext_words(
    Acc_Data &data_out,
    size_t expected_words,
    size_t words_per_ciphertext)
{
    std::vector<uint64_t> actual;
    bool saw_done = false;
    size_t ciphertext_offset = 0;

    while (!data_out.empty()) {
        Acc_Data_Pkt out_pkt = data_out.read();
        if (out_pkt.user.range(7, 4) != 0) {
            assert(out_pkt.user == 0xff);
            assert(out_pkt.last);
            saw_done = true;
            break;
        }

        assert(out_pkt.user == 0);
        assert(out_pkt.last == 0);
        ap_uint<64> keep = out_pkt.keep;
        for (int i = 0; i < LWE_ENCRYPT_WORDS_PER_PKT; i++) {
            if (keep.range(i * 8 + 7, i * 8) == 0) {
                continue;
            }
            actual.push_back(get_pkt_u64(out_pkt, i));
            ciphertext_offset++;
            if (ciphertext_offset == words_per_ciphertext) {
                ciphertext_offset = 0;
                break;
            }
        }
    }

    assert(saw_done);
    assert(actual.size() == expected_words);
    return actual;
}

static std::vector<uint64_t> read_all_payload_words(
    Acc_Data &data_out,
    size_t expected_words)
{
    std::vector<uint64_t> actual;
    bool saw_done = false;

    while (!data_out.empty()) {
        Acc_Data_Pkt out_pkt = data_out.read();
        if (out_pkt.user.range(7, 4) != 0) {
            assert(out_pkt.user == 0xff);
            assert(out_pkt.last);
            saw_done = true;
            break;
        }

        assert(out_pkt.user == 0);
        assert(out_pkt.last == 0);
        ap_uint<64> keep = out_pkt.keep;
        for (int i = 0; i < LWE_ENCRYPT_WORDS_PER_PKT; i++) {
            if (keep.range(i * 8 + 7, i * 8) != 0) {
                actual.push_back(get_pkt_u64(out_pkt, i));
            }
        }
    }

    assert(saw_done);
    assert(actual.size() == expected_words);
    return actual;
}

static size_t reverse_psi64_mask_index(size_t index)
{
    size_t reversed = 0;
    for (size_t bit = 0; bit < 11; bit++) {
        reversed = (reversed << 1) | ((index >> bit) & 1);
    }
    return reversed;
}

static std::vector<uint64_t> hpu_native_to_cpu_lwe_words(
    const std::vector<uint64_t> &native_words,
    size_t lwe_count)
{
    const size_t words_per_slot = LWE_ENCRYPT_HPU_PC_SLOT_WORDS;
    const size_t native_words_per_lwe =
        LWE_ENCRYPT_HPU_PEM_PC * words_per_slot;
    assert(native_words.size() == lwe_count * native_words_per_lwe);

    std::vector<uint64_t> cpu_words;
    cpu_words.reserve(lwe_count * (LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION + 1));

    for (size_t lwe_index = 0; lwe_index < lwe_count; lwe_index++) {
        const size_t base = lwe_index * native_words_per_lwe;
        for (size_t natural_index = 0;
             natural_index < LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
             natural_index++) {
            const size_t hpu_index = reverse_psi64_mask_index(natural_index);
            const size_t group = hpu_index / LWE_ENCRYPT_HPU_PC_GROUP_WORDS;
            const size_t lane = hpu_index % LWE_ENCRYPT_HPU_PC_GROUP_WORDS;
            const size_t pc = group % LWE_ENCRYPT_HPU_PEM_PC;
            const size_t pc_offset =
                (group / LWE_ENCRYPT_HPU_PEM_PC) *
                    LWE_ENCRYPT_HPU_PC_GROUP_WORDS +
                lane;
            cpu_words.push_back(
                native_words[base + pc * words_per_slot + pc_offset]);
        }
        cpu_words.push_back(
            native_words[base + LWE_ENCRYPT_HPU_PC1_DATA_WORDS]);

        for (size_t i = LWE_ENCRYPT_HPU_PC0_DATA_WORDS;
             i < words_per_slot;
             i++) {
            assert(native_words[base + i] == 0);
        }
        for (size_t i = LWE_ENCRYPT_HPU_PC1_DATA_WORDS;
             i < words_per_slot;
             i++) {
            assert(native_words[base + words_per_slot + i] == 0);
        }
    }
    return cpu_words;
}

static uint64_t decode_shortint_message(uint64_t decrypted_plaintext, uint64_t delta)
{
    assert(delta != 0);
    const uint64_t full_cleartext_space =
        1ULL << (LWE_ENCRYPT_HPU_MESSAGE_WIDTH + LWE_ENCRYPT_HPU_CARRY_WIDTH +
                 LWE_ENCRYPT_HPU_PADDING_BIT_WIDTH);
    const uint64_t message_modulus = 1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH;
    __uint128_t rounded =
        (__uint128_t(decrypted_plaintext) + __uint128_t(delta / 2)) / __uint128_t(delta);
    return uint64_t(rounded % full_cleartext_space) % message_modulus;
}

static void verify_ciphertexts(
    const std::vector<uint64_t> &actual,
    uint32_t mask_dimension,
    const std::vector<unsigned> &secret_key,
    const std::vector<uint64_t> &inputs,
    const std::vector<uint64_t> &noises,
    uint32_t input_mode,
    uint64_t delta,
    uint64_t seed,
    uint64_t nonce)
{
    assert(secret_key.size() == mask_dimension);
    assert(inputs.size() == noises.size());

    for (size_t req = 0; req < inputs.size(); req++) {
        uint64_t rng_state = seed ^ nonce ^ uint64_t(req + 1);
        uint64_t expected_body = 0;
        uint64_t actual_dot = 0;
        const size_t base = req * (mask_dimension + 1);

        for (uint32_t i = 0; i < mask_dimension; i++) {
            uint64_t mask_word = next_random_ref(rng_state);
            assert(actual[base + i] == mask_word);
            if (secret_key[i]) {
                expected_body += mask_word;
                actual_dot += actual[base + i];
            }
        }

        uint64_t encoded = inputs[req];
        if (input_mode == LWE_ENCRYPT_INPUT_CLEAR) {
            encoded *= delta;
        }

        expected_body += encoded;
        expected_body += noises[req];
        assert(actual[base + mask_dimension] == expected_body);

        uint64_t decrypted_plaintext = actual[base + mask_dimension] - actual_dot;
        assert(decrypted_plaintext == encoded + noises[req]);

        if (input_mode == LWE_ENCRYPT_INPUT_CLEAR) {
            uint64_t decrypted_message = decode_shortint_message(decrypted_plaintext, delta);
            const uint64_t message_modulus = 1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH;
            assert(decrypted_message == (inputs[req] % message_modulus));
        }
    }
}

static void verify_u8_radix_ciphertexts(
    const std::vector<uint64_t> &actual,
    uint32_t mask_dimension,
    const std::vector<unsigned> &secret_key,
    const std::vector<uint8_t> &inputs,
    const std::vector<uint64_t> &noises,
    uint64_t seed,
    uint64_t nonce)
{
    assert(secret_key.size() == mask_dimension);
    assert(noises.size() == inputs.size() * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT);
    for (size_t i = 0; i < noises.size(); i++) {
        assert(noises[i] == 0);
    }

    for (size_t req = 0; req < inputs.size(); req++) {
        uint8_t reconstructed = 0;

        for (uint32_t block_index = 0; block_index < LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
             block_index++) {
            const size_t ciphertext_index = req * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT + block_index;
            uint64_t rng_state = seed ^ nonce ^ uint64_t(ciphertext_index + 1);
            uint64_t expected_body = 0;
            uint64_t actual_dot = 0;
            const size_t base = ciphertext_index * (mask_dimension + 1);

            for (uint32_t i = 0; i < mask_dimension; i++) {
                uint64_t mask_word = next_random_ref(rng_state);
                assert(actual[base + i] == mask_word);
                if (secret_key[i]) {
                    expected_body += mask_word;
                    actual_dot += actual[base + i];
                }
            }

            const uint64_t clear_block =
                (inputs[req] >> (block_index * LWE_ENCRYPT_HPU_MESSAGE_WIDTH)) &
                ((1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH) - 1);
            const uint64_t encoded = clear_block * LWE_ENCRYPT_HPU_DELTA;
            const uint64_t noise = noises[ciphertext_index];
            expected_body += encoded;
            expected_body += noise;
            assert(actual[base + mask_dimension] == expected_body);

            uint64_t decrypted_plaintext = actual[base + mask_dimension] - actual_dot;
            assert(decrypted_plaintext == encoded + noise);
            uint64_t decrypted_block =
                decode_shortint_message(decrypted_plaintext, LWE_ENCRYPT_HPU_DELTA);
            assert(decrypted_block == clear_block);
            reconstructed |= uint8_t(decrypted_block << (block_index * LWE_ENCRYPT_HPU_MESSAGE_WIDTH));
        }

        assert(reconstructed == inputs[req]);
    }
}

static void run_reference_case(
    const char *name,
    uint32_t mask_dimension,
    const std::vector<unsigned> &secret_key,
    const std::vector<uint64_t> &inputs,
    const std::vector<uint64_t> &noises,
    uint32_t input_mode,
    uint64_t delta,
    uint64_t seed,
    uint64_t nonce)
{
    ap_uint<512> context[256];
    clear_context(context);
    configure_context(
        context,
        mask_dimension,
        static_cast<uint32_t>(inputs.size()),
        input_mode,
        LWE_ENCRYPT_NOISE_INPUT,
        0,
        delta,
        seed,
        nonce);

    for (uint32_t i = 0; i < mask_dimension; i++) {
        set_secret_key_bit(context, i, secret_key[i] != 0);
    }

    Acc_Data data_in;
    Acc_Data data_out;
    write_requests(data_in, inputs, noises);
    lwe_encrypt(data_in, data_out, context);

    const size_t expected_words = inputs.size() * (mask_dimension + 1);
    std::vector<uint64_t> actual =
        read_ciphertext_words(data_out, expected_words, mask_dimension + 1);
    verify_ciphertexts(
        actual,
        mask_dimension,
        secret_key,
        inputs,
        noises,
        input_mode,
        delta,
        seed,
        nonce);

    std::cout << name << " passed. dimension=" << mask_dimension
              << " ciphertext_words=" << actual.size()
              << " decrypt_checked=yes" << std::endl;
}

static void run_u8_radix_case(
    const char *name,
    const std::vector<unsigned> &secret_key,
    const std::vector<uint8_t> &inputs,
    const std::vector<uint64_t> &noises,
    uint64_t seed,
    uint64_t nonce,
    uint32_t output_layout)
{
    const uint32_t mask_dimension = LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
    ap_uint<512> context[256];
    clear_context(context);
    configure_context(
        context,
        mask_dimension,
        static_cast<uint32_t>(inputs.size()),
        LWE_ENCRYPT_INPUT_U8_RADIX,
        LWE_ENCRYPT_NOISE_ZERO,
        0,
        0,
        seed,
        nonce,
        output_layout);

    for (uint32_t i = 0; i < mask_dimension; i++) {
        set_secret_key_bit(context, i, secret_key[i] != 0);
    }

    Acc_Data data_in;
    Acc_Data data_out;
    write_u8_radix_requests(data_in, inputs);
    lwe_encrypt(data_in, data_out, context);

    const size_t logical_words =
        inputs.size() * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT * (mask_dimension + 1);
    std::vector<uint64_t> actual;
    if (output_layout == LWE_ENCRYPT_OUTPUT_HPU_NATIVE) {
        const size_t native_words =
            inputs.size() * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT *
            LWE_ENCRYPT_HPU_PEM_PC * LWE_ENCRYPT_HPU_PC_SLOT_WORDS;
        actual = hpu_native_to_cpu_lwe_words(
            read_all_payload_words(data_out, native_words),
            inputs.size() * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT);
    } else {
        actual = read_ciphertext_words(
            data_out,
            logical_words,
            mask_dimension + 1);
    }
    verify_u8_radix_ciphertexts(
        actual,
        mask_dimension,
        secret_key,
        inputs,
        noises,
        seed,
        nonce);
    if (output_layout == LWE_ENCRYPT_OUTPUT_CPU_LWE) {
        dump_u8_radix_ciphertexts(
            PSI64_U8_RADIX_CIPHERTEXT_FILE,
            inputs,
            actual,
            mask_dimension);
    }

    std::cout << name << " passed. u8_inputs=" << inputs.size()
              << " radix_blocks=" << inputs.size() * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT
              << " ciphertext_words=" << actual.size()
              << " output_layout="
              << (output_layout == LWE_ENCRYPT_OUTPUT_HPU_NATIVE
                      ? "hpu-native"
                      : "cpu-lwe")
              << " decrypt_checked=yes"
              << (output_layout == LWE_ENCRYPT_OUTPUT_CPU_LWE
                      ? " dump="
                      : "")
              << (output_layout == LWE_ENCRYPT_OUTPUT_CPU_LWE
                      ? PSI64_U8_RADIX_CIPHERTEXT_FILE
                      : "")
              << std::endl;
}

int main()
{
    {
        const uint32_t mask_dimension = 16;
        std::vector<unsigned> secret_key(mask_dimension);
        for (uint32_t i = 0; i < mask_dimension; i++) {
            secret_key[i] = (i % 3) == 0 ? 1 : 0;
        }

        run_reference_case(
            "small encoded-input LWE reference",
            mask_dimension,
            secret_key,
            {0x123456789abcdef0ULL, 0x0fedcba987654321ULL},
            {0x10ULL, 0x20ULL},
            LWE_ENCRYPT_INPUT_ENCODED,
            0,
            0xfeedfacecafebeefULL,
            0x1122334455667788ULL);
    }

    {
        const uint32_t mask_dimension = LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
        std::vector<unsigned> secret_key =
            load_saved_secret_key(PSI64_BIG_LWE_SECRET_KEY_FILE, mask_dimension);

        run_reference_case(
            "HPU Big-LWE clear-input saved-key reference",
            mask_dimension,
            secret_key,
            {0, 1, 2, 3},
            {0, 7, uint64_t(-3), 17},
            LWE_ENCRYPT_INPUT_CLEAR,
            LWE_ENCRYPT_HPU_DELTA,
            0x0123456789abcdefULL,
            0xfedcba9876543210ULL);
    }

    {
        const uint32_t mask_dimension = LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
        std::vector<unsigned> secret_key =
            load_saved_secret_key(PSI64_BIG_LWE_SECRET_KEY_FILE, mask_dimension);

        std::cout << "loaded saved psi64 Big-LWE secret key. dimension=" << secret_key.size()
                  << " ones=" << count_secret_key_ones(secret_key) << std::endl;

        run_u8_radix_case(
            "HPU u8-radix saved-key reference",
            secret_key,
            {0x00, 0x01, 0x5a, 0xff},
            std::vector<uint64_t>(
                4 * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT,
                0),
            0x13579bdf2468ace0ULL,
            0x0eca8642fdb97531ULL,
            LWE_ENCRYPT_OUTPUT_CPU_LWE);

        run_u8_radix_case(
            "HPU-native u8-radix saved-key reference",
            secret_key,
            {0x00, 0x01, 0x5a, 0xff},
            std::vector<uint64_t>(
                4 * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT,
                0),
            0x13579bdf2468ace0ULL,
            0x0eca8642fdb97531ULL,
            LWE_ENCRYPT_OUTPUT_HPU_NATIVE);
    }

    return 0;
}
