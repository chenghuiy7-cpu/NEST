#pragma once

#ifndef NO_CTOR
#define NO_CTOR
#endif

#include "hlsacc_types.hpp"

#define LWE_DECRYPT_STATIC_CONTEXT_BASE 3
#define LWE_DECRYPT_KEY_CONTEXT_BASE (LWE_DECRYPT_STATIC_CONTEXT_BASE + 1)

#define LWE_DECRYPT_HPU_BIG_LWE_DIMENSION 2048
#define LWE_DECRYPT_HPU_MESSAGE_WIDTH 2
#define LWE_DECRYPT_HPU_CARRY_WIDTH 2
#define LWE_DECRYPT_HPU_PADDING_BIT_WIDTH 1
#define LWE_DECRYPT_HPU_DELTA_LOG2 \
    (64 - (LWE_DECRYPT_HPU_MESSAGE_WIDTH + LWE_DECRYPT_HPU_CARRY_WIDTH + \
           LWE_DECRYPT_HPU_PADDING_BIT_WIDTH))
#define LWE_DECRYPT_HPU_DELTA (1ULL << LWE_DECRYPT_HPU_DELTA_LOG2)
#define LWE_DECRYPT_U8_RADIX_BLOCK_COUNT (8 / LWE_DECRYPT_HPU_MESSAGE_WIDTH)

#define LWE_DECRYPT_HPU_PC_COUNT 2
#define LWE_DECRYPT_HPU_PC_DATA_WORDS \
    (LWE_DECRYPT_HPU_BIG_LWE_DIMENSION / LWE_DECRYPT_HPU_PC_COUNT)
#define LWE_DECRYPT_HPU_PC0_DATA_WORDS (LWE_DECRYPT_HPU_PC_DATA_WORDS + 1)
#define LWE_DECRYPT_HPU_PC_SLOT_BYTES (3 * 4096)
#define LWE_DECRYPT_HPU_PC_SLOT_WORDS (LWE_DECRYPT_HPU_PC_SLOT_BYTES / 8)
#define LWE_DECRYPT_HPU_PC_SLOT_PACKETS \
    (LWE_DECRYPT_HPU_PC_SLOT_BYTES / (TDATA_WIDTH / 8))
#define LWE_DECRYPT_HPU_NATIVE_LWE_BYTES \
    (LWE_DECRYPT_HPU_PC_COUNT * LWE_DECRYPT_HPU_PC_SLOT_BYTES)
#define LWE_DECRYPT_HPU_NATIVE_U8_BYTES \
    (LWE_DECRYPT_U8_RADIX_BLOCK_COUNT * LWE_DECRYPT_HPU_NATIVE_LWE_BYTES)

#define LWE_DECRYPT_INPUT_HPU_NATIVE 1

// context[LWE_DECRYPT_STATIC_CONTEXT_BASE] layout:
// [ 31:  0] input_count; exact number of u8 values, 0 means run until finish
// [ 63: 32] mask_dimension; fixed to 2048 for psi64 Big-LWE
// [127: 64] delta; fixed to 2^59 for the current shortint parameters
// [159:128] message_width; fixed to 2
// [191:160] radix_blocks_per_u8; fixed to 4
// [223:192] input_layout; fixed to LWE_DECRYPT_INPUT_HPU_NATIVE
//
// Secret key bits start at context[LWE_DECRYPT_KEY_CONTEXT_BASE], packed one
// binary Big-LWE secret-key coefficient per bit. The key therefore occupies
// four 512-bit context words.
//
// Input for each radix-block Big-LWE is the same native psi64/V80 layout that
// lwe_encrypt emits:
//   PC0: 1024 bit-reversed/interleaved mask words, body, padding to 12KB
//   PC1: 1024 bit-reversed/interleaved mask words, padding to 12KB
// Four consecutive Big-LWE ciphertexts represent one u8, least-significant
// 2-bit radix block first.
//
// Output is a packed stream of consecutive clear u8 values. Payload packets
// always use TUSER=0; SUDA's invalid TUSER=0xff task-finish packet is emitted
// separately after all plaintext payload.
void lwe_decrypt(Acc_Data &data_in, Acc_Data &data_out, ap_uint<512> context[256]);
