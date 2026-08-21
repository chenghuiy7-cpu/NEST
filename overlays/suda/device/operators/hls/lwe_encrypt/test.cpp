#include <cassert>
#include <cstdint>
#include <iostream>

#include "lwe_encrypt.hpp"

static_assert(LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION == 2048, "HPU Big-LWE dimension mismatch");
static_assert(LWE_ENCRYPT_HPU_DELTA == (1ULL << 59), "HPU shortint delta mismatch");
static_assert(LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT == 4, "u8 radix block count mismatch");
static_assert(LWE_ENCRYPT_HPU_PC_SLOT_BYTES == 12288, "HPU PC slot size mismatch");
static_assert(LWE_ENCRYPT_HPU_NATIVE_LWE_BYTES == 24576, "HPU native LWE size mismatch");

static const uint32_t TEST_PLAINTEXT_BYTES = 65;
static const uint64_t TEST_SEED = 0x13579bdf2468ace0ULL;
static const uint64_t TEST_NONCE = 0x0eca8642fdb97531ULL;

static uint8_t test_plaintext_byte(uint32_t index)
{
    return static_cast<uint8_t>(0x5aU + index * 37U);
}

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

static uint64_t sample_noise_ref(uint64_t &state)
{
    uint64_t random = next_random_ref(state);
    uint64_t magnitude = random & ((uint64_t(1) << LWE_ENCRYPT_HPU_GLWE_NOISE_BOUND_LOG2) - 1);
    return (random >> 63) != 0 ? uint64_t(0 - magnitude) : magnitude;
}

static bool secret_key_bit_ref(uint32_t index)
{
    // A deterministic test key keeps CSIM/COSIM independent from external files.
    return (index % 7) == 0 || (index % 17) == 3;
}

static void clear_context(ap_uint<512> context[256])
{
    for (int i = 0; i < 256; i++) {
        context[i] = 0;
    }
}

static void set_context_u64(ap_uint<512> &word, int idx, uint64_t value)
{
    word.range(idx * 64 + 63, idx * 64) = value;
}

static void set_secret_key_bit(ap_uint<512> context[256], uint32_t index, bool value)
{
    const uint32_t context_word = LWE_ENCRYPT_KEY_CONTEXT_BASE + (index >> 9);
    const uint32_t bit_index = index & 511;
    assert(context_word < 256);
    context[context_word][bit_index] = value ? 1 : 0;
}

static void configure_context(ap_uint<512> context[256])
{
    ap_uint<512> &cfg = context[LWE_ENCRYPT_STATIC_CONTEXT_BASE];
    cfg.range(31, 0) = LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
    cfg.range(63, 32) = TEST_PLAINTEXT_BYTES;
    cfg.range(95, 64) = LWE_ENCRYPT_INPUT_U8_RADIX;
    cfg.range(127, 96) = LWE_ENCRYPT_NOISE_TUNIFORM;
    cfg.range(159, 128) = LWE_ENCRYPT_HPU_GLWE_NOISE_BOUND_LOG2;
    cfg.range(191, 160) = LWE_ENCRYPT_OUTPUT_HPU_NATIVE;
    set_context_u64(cfg, 4, 0);
    set_context_u64(cfg, 5, TEST_SEED);
    set_context_u64(cfg, 6, TEST_NONCE);

    for (uint32_t i = 0; i < LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION; i++) {
        set_secret_key_bit(context, i, secret_key_bit_ref(i));
    }
}

static void write_plaintext(Acc_Data &data_in)
{
    const uint32_t packet_count = (TEST_PLAINTEXT_BYTES + 63) / 64;
    for (uint32_t packet_index = 0; packet_index < packet_count; packet_index++) {
        Acc_Data_Pkt in_pkt;
        in_pkt.data = 0;
        // SUDA streams a page-aligned SLM range, so even the final payload
        // beat has all byte lanes marked valid. TEST_PLAINTEXT_BYTES must stop
        // the operator after byte 65 rather than relying on TKEEP.
        in_pkt.keep = -1;
        in_pkt.strb = -1;
        in_pkt.user = 0;
        in_pkt.last = 0;
        in_pkt.id = 0;
        in_pkt.dest = 0;

        for (uint32_t byte_lane = 0; byte_lane < 64; byte_lane++) {
            uint32_t clear_index = packet_index * 64 + byte_lane;
            if (clear_index >= TEST_PLAINTEXT_BYTES) {
                break;
            }
            in_pkt.data.range(byte_lane * 8 + 7, byte_lane * 8) =
                test_plaintext_byte(clear_index);
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

static uint64_t pkt_word(const Acc_Data_Pkt &pkt, int idx)
{
    ap_uint<512> data = pkt.data;
    return data.range(idx * 64 + 63, idx * 64).to_uint64();
}

static uint32_t reverse_11_ref(uint32_t value)
{
    uint32_t reversed = 0;
    for (int bit = 0; bit < 11; bit++) {
        reversed |= ((value >> bit) & 1U) << (10 - bit);
    }
    return reversed;
}

static uint32_t hpu_index_from_pc_offset(uint32_t pc, uint32_t pc_offset)
{
    const uint32_t group = pc_offset / LWE_ENCRYPT_HPU_PC_GROUP_WORDS;
    const uint32_t lane = pc_offset % LWE_ENCRYPT_HPU_PC_GROUP_WORDS;
    return group * LWE_ENCRYPT_HPU_REGF_COEF_NB +
           pc * LWE_ENCRYPT_HPU_PC_GROUP_WORDS + lane;
}

static void verify_output_stream(Acc_Data &data_out)
{
    const uint32_t mask_dimension = LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
    const uint32_t words_per_ciphertext =
        LWE_ENCRYPT_HPU_PEM_PC * LWE_ENCRYPT_HPU_PC_SLOT_WORDS;
    const uint32_t expected_ciphertexts =
        TEST_PLAINTEXT_BYTES * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
    const uint32_t expected_words = expected_ciphertexts * words_per_ciphertext;

    uint32_t total_words = 0;
    uint32_t current_ciphertext = 0;
    uint32_t current_offset = 0;
    uint64_t expected_mask[LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION];
    uint64_t expected_body = 0;
    uint8_t reconstructed = 0;
    bool saw_done = false;

    uint64_t rng_state = TEST_SEED ^ TEST_NONCE ^ uint64_t(1);
    for (uint32_t i = 0; i < mask_dimension; i++) {
        expected_mask[i] = next_random_ref(rng_state);
        if (secret_key_bit_ref(i)) {
            expected_body += expected_mask[i];
        }
    }
    const uint8_t first_clear = test_plaintext_byte(0);
    const uint64_t first_block =
        first_clear & ((1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH) - 1);
    expected_body += first_block * LWE_ENCRYPT_HPU_DELTA;
    expected_body += sample_noise_ref(rng_state);

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

        for (int lane = 0; lane < LWE_ENCRYPT_WORDS_PER_PKT; lane++) {
            if (keep.range(lane * 8 + 7, lane * 8) == 0) {
                continue;
            }

            uint64_t actual = pkt_word(out_pkt, lane);
            assert(current_ciphertext < expected_ciphertexts);

            uint64_t expected = 0;
            if (current_offset < LWE_ENCRYPT_HPU_PC_SLOT_WORDS) {
                if (current_offset < LWE_ENCRYPT_HPU_PC1_DATA_WORDS) {
                    uint32_t hpu_index = hpu_index_from_pc_offset(0, current_offset);
                    expected = expected_mask[reverse_11_ref(hpu_index)];
                } else if (current_offset == LWE_ENCRYPT_HPU_PC1_DATA_WORDS) {
                    expected = expected_body;
                }
            } else {
                uint32_t pc1_offset = current_offset - LWE_ENCRYPT_HPU_PC_SLOT_WORDS;
                if (pc1_offset < LWE_ENCRYPT_HPU_PC1_DATA_WORDS) {
                    uint32_t hpu_index = hpu_index_from_pc_offset(1, pc1_offset);
                    expected = expected_mask[reverse_11_ref(hpu_index)];
                }
            }
            assert(actual == expected);

            current_offset++;
            total_words++;
            if (current_offset == words_per_ciphertext) {
                const uint32_t clear_index =
                    current_ciphertext / LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
                const uint32_t block_index =
                    current_ciphertext % LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
                const uint32_t shift = block_index * LWE_ENCRYPT_HPU_MESSAGE_WIDTH;
                const uint8_t clear_u8 = test_plaintext_byte(clear_index);
                const uint64_t clear_block =
                    (clear_u8 >> shift) & ((1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH) - 1);
                uint64_t dot = 0;
                for (uint32_t i = 0; i < mask_dimension; i++) {
                    if (secret_key_bit_ref(i)) {
                        dot += expected_mask[i];
                    }
                }
                uint64_t phase = expected_body - dot;
                uint64_t rounded = uint64_t(
                    (__uint128_t(phase) + (LWE_ENCRYPT_HPU_DELTA / 2)) /
                    LWE_ENCRYPT_HPU_DELTA);
                uint64_t decrypted_block = rounded & ((1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH) - 1);
                assert(decrypted_block == clear_block);
                reconstructed |= uint8_t(decrypted_block << shift);

                current_ciphertext++;
                if (block_index == LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT - 1) {
                    assert(reconstructed == clear_u8);
                    reconstructed = 0;
                }
                current_offset = 0;
                rng_state = TEST_SEED ^ TEST_NONCE ^ uint64_t(current_ciphertext + 1);
                expected_body = 0;
                for (uint32_t i = 0; i < mask_dimension; i++) {
                    expected_mask[i] = next_random_ref(rng_state);
                    if (secret_key_bit_ref(i)) {
                        expected_body += expected_mask[i];
                    }
                }
                if (current_ciphertext < expected_ciphertexts) {
                    const uint32_t next_clear_index =
                        current_ciphertext / LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
                    const uint32_t next_block_index =
                        current_ciphertext % LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
                    const uint8_t next_clear = test_plaintext_byte(next_clear_index);
                    const uint64_t next_block =
                        (next_clear >> (next_block_index * LWE_ENCRYPT_HPU_MESSAGE_WIDTH)) &
                        ((1ULL << LWE_ENCRYPT_HPU_MESSAGE_WIDTH) - 1);
                    expected_body += next_block * LWE_ENCRYPT_HPU_DELTA;
                    expected_body += sample_noise_ref(rng_state);
                }
            }
        }
    }

    assert(saw_done);
    assert(total_words == expected_words);
    assert(current_ciphertext == expected_ciphertexts);
    assert(reconstructed == 0);
}

int main()
{
    ap_uint<512> context[256];
    clear_context(context);
    configure_context(context);

    Acc_Data data_in;
    Acc_Data data_out;
    write_plaintext(data_in);

    lwe_encrypt(data_in, data_out, context);
    verify_output_stream(data_out);

    std::cout << "lwe_encrypt HLS smoke test passed. mask_dimension="
              << LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION
              << " plaintext_bytes=" << TEST_PLAINTEXT_BYTES
              << " input_packets=" << (TEST_PLAINTEXT_BYTES + 63) / 64
              << " radix_blocks="
              << TEST_PLAINTEXT_BYTES * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT
              << " output_layout=hpu-native"
              << " decrypt_checked=yes" << std::endl;

    return 0;
}
