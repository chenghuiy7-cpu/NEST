#include "lwe_decrypt.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#ifndef LWE_DECRYPT_TEST_U8_COUNT
#define LWE_DECRYPT_TEST_U8_COUNT 3
#endif

static const uint32_t TEST_U8_COUNT = LWE_DECRYPT_TEST_U8_COUNT;

static bool test_key_bit(uint32_t index)
{
    uint32_t mixed = index * 0x9e3779b9U + 0x7f4a7c15U;
    return ((mixed ^ (mixed >> 11) ^ (index >> 3)) & 1U) != 0;
}

static uint64_t next_random(uint64_t &state)
{
    if (state == 0) {
        state = 0x9e3779b97f4a7c15ULL;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static uint32_t reverse_11(uint32_t index)
{
    uint32_t reversed = 0;
    for (uint32_t bit = 0; bit < 11; bit++) {
        reversed |= ((index >> bit) & 1U) << (10 - bit);
    }
    return reversed;
}

static size_t native_word_index(uint32_t natural_index)
{
    uint32_t hpu_index = reverse_11(natural_index);
    uint32_t group = hpu_index >> 4;
    uint32_t lane = hpu_index & 15U;
    uint32_t pc = group & 1U;
    uint32_t pc_offset = ((group >> 1) << 4) | lane;
    return size_t(pc) * LWE_DECRYPT_HPU_PC_SLOT_WORDS + pc_offset;
}

static uint8_t expected_clear(uint32_t index)
{
    return uint8_t(index * 37U + 11U);
}

static void append_native_lwe(
    Acc_Data &input,
    uint8_t clear_block,
    uint32_t ciphertext_index)
{
    const size_t words_per_lwe =
        LWE_DECRYPT_HPU_PC_COUNT * LWE_DECRYPT_HPU_PC_SLOT_WORDS;
    std::vector<uint64_t> words(words_per_lwe, 0);
    uint64_t state = 0xd1b54a32d192ed03ULL ^
                     (uint64_t(ciphertext_index + 1) * 0x94d049bb133111ebULL);
    uint64_t dot = 0;

    for (uint32_t natural_index = 0;
         natural_index < LWE_DECRYPT_HPU_BIG_LWE_DIMENSION;
         natural_index++) {
        uint64_t mask = next_random(state);
        words[native_word_index(natural_index)] = mask;
        if (test_key_bit(natural_index)) {
            dot += mask;
        }
    }

    int64_t signed_noise = int64_t(ciphertext_index % 17U) - 8;
    uint64_t body = dot +
                    uint64_t(clear_block) * LWE_DECRYPT_HPU_DELTA +
                    uint64_t(signed_noise);
    words[LWE_DECRYPT_HPU_PC_DATA_WORDS] = body;

    for (size_t packet_index = 0;
         packet_index < words_per_lwe / (TDATA_WIDTH / 64);
         packet_index++) {
        Acc_Data_Pkt pkt;
        pkt.data = 0;
        for (int lane = 0; lane < TDATA_WIDTH / 64; lane++) {
            pkt.data.range(lane * 64 + 63, lane * 64) =
                words[packet_index * (TDATA_WIDTH / 64) + lane];
        }
        pkt.keep = ~ap_uint<64>(0);
        pkt.strb = ~ap_uint<64>(0);
        pkt.user = 0;
        pkt.last = 0;
        pkt.id = 0;
        pkt.dest = 0;
        input.write(pkt);
    }
}

int main()
{
    Acc_Data input;
    Acc_Data output;
    ap_uint<512> context[256];

    for (int i = 0; i < 256; i++) {
        context[i] = 0;
    }

    ap_uint<512> cfg = 0;
    cfg.range(31, 0) = TEST_U8_COUNT;
    cfg.range(63, 32) = LWE_DECRYPT_HPU_BIG_LWE_DIMENSION;
    cfg.range(127, 64) = LWE_DECRYPT_HPU_DELTA;
    cfg.range(159, 128) = LWE_DECRYPT_HPU_MESSAGE_WIDTH;
    cfg.range(191, 160) = LWE_DECRYPT_U8_RADIX_BLOCK_COUNT;
    cfg.range(223, 192) = LWE_DECRYPT_INPUT_HPU_NATIVE;
    context[LWE_DECRYPT_STATIC_CONTEXT_BASE] = cfg;

    for (uint32_t index = 0;
         index < LWE_DECRYPT_HPU_BIG_LWE_DIMENSION;
         index++) {
        if (test_key_bit(index)) {
            uint32_t word = LWE_DECRYPT_KEY_CONTEXT_BASE + (index >> 9);
            context[word][index & 511U] = 1;
        }
    }

    uint32_t ciphertext_index = 0;
    for (uint32_t clear_index = 0; clear_index < TEST_U8_COUNT; clear_index++) {
        uint8_t clear_u8 = expected_clear(clear_index);
        for (uint32_t block_index = 0;
             block_index < LWE_DECRYPT_U8_RADIX_BLOCK_COUNT;
             block_index++) {
            uint8_t clear_block =
                (clear_u8 >> (block_index * LWE_DECRYPT_HPU_MESSAGE_WIDTH)) & 3U;
            append_native_lwe(input, clear_block, ciphertext_index++);
        }
    }

    Acc_Data_Pkt done;
    done.data = 0;
    done.keep = ~ap_uint<64>(0);
    done.strb = ~ap_uint<64>(0);
    done.user = 0xff;
    done.last = 1;
    done.id = 0;
    done.dest = 0;
    input.write(done);

    lwe_decrypt(input, output, context);

    uint32_t clear_index = 0;
    bool saw_done = false;
    while (!output.empty()) {
        Acc_Data_Pkt pkt = output.read();
        if (pkt.user.range(7, 4) != 0) {
            assert(clear_index == TEST_U8_COUNT);
            saw_done = true;
            continue;
        }

        for (int lane = 0; lane < TDATA_WIDTH / 8; lane++) {
            bool valid = pkt.keep[lane] != 0;
            if (!valid) {
                continue;
            }
            assert(clear_index < TEST_U8_COUNT);
            uint8_t actual = uint8_t(pkt.data.range(lane * 8 + 7, lane * 8));
            assert(actual == expected_clear(clear_index));
            clear_index++;
        }
    }

    assert(saw_done);
    assert(clear_index == TEST_U8_COUNT);
    std::cout << "lwe_decrypt HLS smoke test passed. input_layout=hpu-native"
              << " mask_dimension=" << LWE_DECRYPT_HPU_BIG_LWE_DIMENSION
              << " decrypted_u8=" << clear_index
              << " output_packing=64-u8-per-beat"
              << std::endl;
    return 0;
}
