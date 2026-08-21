#pragma once

#ifndef NO_CTOR
#define NO_CTOR
#endif

#include "hlsacc_types.hpp"

#define LWE_ENCRYPT_WORDS_PER_PKT (TDATA_WIDTH / 64)
#define LWE_ENCRYPT_MAX_LWE_DIMENSION 4096

// SUDA reserves the first 192 bytes (three 512-bit BRAM words) of AccContext
// for FIFO descriptors and runtime-private state. Operator static data starts
// at BRAM word 3; the packed LWE secret key follows the 64-byte config word.
#define LWE_ENCRYPT_STATIC_CONTEXT_BASE 3
#define LWE_ENCRYPT_KEY_CONTEXT_BASE (LWE_ENCRYPT_STATIC_CONTEXT_BASE + 1)

// Fixed HPU parameter preset from:
// tfhe-rs/mockups/tfhe-hpu-mockup/params/tuniform_64b_pfail128_psi64.toml
//
// For direct HPU boundary ciphertexts, use the Big LWE dimension and the
// GLWE noise bound, because tfhe-rs HPU conversion handles Big LWE over the
// hardware boundary.
#define LWE_ENCRYPT_HPU_SMALL_LWE_DIMENSION 879
#define LWE_ENCRYPT_HPU_GLWE_DIMENSION 1
#define LWE_ENCRYPT_HPU_POLYNOMIAL_SIZE 2048
#define LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION \
    (LWE_ENCRYPT_HPU_GLWE_DIMENSION * LWE_ENCRYPT_HPU_POLYNOMIAL_SIZE)
#define LWE_ENCRYPT_HPU_LWE_NOISE_BOUND_LOG2 3
#define LWE_ENCRYPT_HPU_GLWE_NOISE_BOUND_LOG2 17
#define LWE_ENCRYPT_HPU_MESSAGE_WIDTH 2
#define LWE_ENCRYPT_HPU_CARRY_WIDTH 2
#define LWE_ENCRYPT_HPU_CIPHERTEXT_WIDTH 64
#define LWE_ENCRYPT_HPU_PADDING_BIT_WIDTH 1
#define LWE_ENCRYPT_HPU_DELTA_LOG2 \
    (LWE_ENCRYPT_HPU_CIPHERTEXT_WIDTH - \
     (LWE_ENCRYPT_HPU_MESSAGE_WIDTH + LWE_ENCRYPT_HPU_CARRY_WIDTH + \
      LWE_ENCRYPT_HPU_PADDING_BIT_WIDTH))
#define LWE_ENCRYPT_HPU_DELTA (1ULL << LWE_ENCRYPT_HPU_DELTA_LOG2)
#define LWE_ENCRYPT_HPU_BIG_KEY_CONTEXT_WORDS \
    ((LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION + 511) / 512)

// Native V80/psi64 ciphertext-slot layout. HPU ciphertext memory has two
// processing-cluster cuts. Each cut is independently aligned to a 4KB page;
// 1025 u64 coefficients therefore require three pages in both cuts.
#define LWE_ENCRYPT_HPU_PEM_PC 2
#define LWE_ENCRYPT_HPU_REGF_COEF_NB 32
#define LWE_ENCRYPT_HPU_PC_GROUP_WORDS \
    (LWE_ENCRYPT_HPU_REGF_COEF_NB / LWE_ENCRYPT_HPU_PEM_PC)
#define LWE_ENCRYPT_HPU_PC0_DATA_WORDS \
    ((LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION / LWE_ENCRYPT_HPU_PEM_PC) + 1)
#define LWE_ENCRYPT_HPU_PC1_DATA_WORDS \
    (LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION / LWE_ENCRYPT_HPU_PEM_PC)
#define LWE_ENCRYPT_HPU_PC_SLOT_BYTES (3 * 4096)
#define LWE_ENCRYPT_HPU_PC_SLOT_WORDS (LWE_ENCRYPT_HPU_PC_SLOT_BYTES / 8)
#define LWE_ENCRYPT_HPU_NATIVE_LWE_BYTES \
    (LWE_ENCRYPT_HPU_PEM_PC * LWE_ENCRYPT_HPU_PC_SLOT_BYTES)
#define LWE_ENCRYPT_HPU_NATIVE_U8_BYTES \
    (LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT * LWE_ENCRYPT_HPU_NATIVE_LWE_BYTES)

#define LWE_ENCRYPT_INPUT_ENCODED 0
#define LWE_ENCRYPT_INPUT_CLEAR 1
#define LWE_ENCRYPT_INPUT_U8_RADIX 2
#define LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT \
    (8 / LWE_ENCRYPT_HPU_MESSAGE_WIDTH)

#define LWE_ENCRYPT_NOISE_TUNIFORM 0
#define LWE_ENCRYPT_NOISE_INPUT 1
#define LWE_ENCRYPT_NOISE_ZERO 2

#define LWE_ENCRYPT_OUTPUT_CPU_LWE 0
#define LWE_ENCRYPT_OUTPUT_HPU_NATIVE 1

// context[LWE_ENCRYPT_STATIC_CONTEXT_BASE] layout:
// [ 31:  0] mask_dimension; number of mask words in the output LWE ciphertext
// [ 63: 32] input_count; 0 means consume valid input until the stream ends
//           In u8 radix mode this is the exact number of consecutive u8 bytes.
//           In the legacy single-LWE modes this remains the packet/request count.
// [ 95: 64] input_mode; 0 = encoded plaintext, 1 = clear block * delta,
//                         2 = u8 clear input split into four 2-bit radix blocks
// [127: 96] noise_mode; 0 = internal toy t-uniform-like noise,
//                       1 = input noise word (single-LWE modes only),
//                       2 = zero noise
// [159:128] noise_bound_log2; used only when noise_mode == 0
// [191:160] output_layout; 0 = CPU/tfhe-rs LWE stream,
//                          1 = native V80/psi64 HPU ciphertext slots
// [319:256] delta; used only when input_mode == 1
// [383:320] rng_seed
// [447:384] nonce
//
// Secret key bits start at context[LWE_ENCRYPT_KEY_CONTEXT_BASE], packed one
// binary coefficient per bit.
// HPU direct mode should use:
//   mask_dimension = LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION
//   delta = LWE_ENCRYPT_HPU_DELTA
//   noise_bound_log2 = LWE_ENCRYPT_HPU_GLWE_NOISE_BOUND_LOG2
//   secret key = GLWE secret key flattened as the Big LWE key
//
// Input packet layout:
// Single-block modes:
//   [ 63:  0] encoded plaintext or clear block
//   [127: 64] noise, used only when noise_mode == 1
//
// u8 radix mode:
//   Every valid TKEEP byte is one consecutive clear u8 value. A 512-bit beat
//   therefore carries up to 64 plaintext bytes. input_count limits the exact
//   number consumed, so the final beat may be only partially used.
//   External per-ciphertext noise is not representable in this packed mode;
//   use LWE_ENCRYPT_NOISE_TUNIFORM or LWE_ENCRYPT_NOISE_ZERO.
//   Output order is least-significant radix block first.
//
// CPU output layout:
//   [mask_0, ..., mask_n-1, body], packed as eight u64 words per 512-bit beat.
//
// HPU-native output layout (u8 radix mode and fixed psi64 parameters only):
//   For each radix-block Big-LWE:
//     PC0 slot: 1024 bit-reversed mask words, body, then zero padding to 12KB
//     PC1 slot: 1024 bit-reversed mask words, then zero padding to 12KB
//   The 2048-point mask uses the same 11-bit reversal and 16-coefficient PC
//   interleave as HpuLweCiphertextOwned::create_from in tfhe-rs. With ct_width
//   equal to 64, the HPU msb2lsb conversion is the identity.
//
// Every ciphertext payload beat uses TUSER=0/TLAST=0. SUDA's task-finish marker
// is a separate invalid input packet with TUSER=0xff and is forwarded only after
// all requested plaintext bytes have been encrypted.
void lwe_encrypt(Acc_Data &data_in, Acc_Data &data_out, ap_uint<512> context[256]);
