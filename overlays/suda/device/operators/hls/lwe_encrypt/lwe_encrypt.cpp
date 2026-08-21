#include "lwe_encrypt.hpp"

static ap_uint<64> next_random(ap_uint<64> &state)
{
#pragma HLS INLINE
    if (state == 0) {
        state = 0x9e3779b97f4a7c15ULL;
    }
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static ap_uint<64> sample_tuniform_like_noise(ap_uint<64> &state, ap_uint<32> bound_log2)
{
#pragma HLS INLINE
    if (bound_log2 == 0) {
        return 0;
    }

    ap_uint<64> r = next_random(state);
    ap_uint<64> magnitude_mask = ~ap_uint<64>(0);
    if (bound_log2 < 64) {
        magnitude_mask = (ap_uint<64>(1) << bound_log2) - 1;
    }

    ap_uint<64> magnitude = r & magnitude_mask;
    bool negative = r[63];
    return negative ? ap_uint<64>(0 - magnitude) : magnitude;
}

static bool secret_key_bit(ap_uint<512> context[256], ap_uint<32> index)
{
#pragma HLS INLINE
    ap_uint<32> context_word = LWE_ENCRYPT_KEY_CONTEXT_BASE + (index >> 9);
    ap_uint<32> bit_index = index & 511;
    if (context_word >= 256) {
        return false;
    }
    return context[context_word][bit_index] != 0;
}

static void flush_output_packet(
    Acc_Data &data_out,
    ap_uint<512> &packet_data,
    ap_uint<4> &packet_word_count)
{
#pragma HLS INLINE off
    if (packet_word_count != 0) {
        Acc_Data_Pkt out_pkt;
        out_pkt.data = packet_data;
        // Real ciphertext payload must not carry SUDA's task-finish marker.
        // The invalid TUSER=0xff packet is forwarded separately after payload.
        out_pkt.keep = ~ap_uint<64>(0);
        out_pkt.strb = ~ap_uint<64>(0);
        out_pkt.user = 0;
        out_pkt.last = 0;
        out_pkt.id = 0;
        out_pkt.dest = 0;
        data_out.write(out_pkt);

        packet_data = 0;
        packet_word_count = 0;
    }
}

static void write_error_packet(Acc_Data &data_out, ap_uint<32> code)
{
#pragma HLS INLINE off
    Acc_Data_Pkt out_pkt;
    out_pkt.data = 0;
    out_pkt.data.range(63, 0) = 0x4c57454552524f52ULL;
    out_pkt.data.range(95, 64) = code;
    out_pkt.keep = ~ap_uint<64>(0);
    out_pkt.strb = ~ap_uint<64>(0);
    out_pkt.user = 0xff;
    out_pkt.last = 1;
    out_pkt.id = 0;
    out_pkt.dest = 0;
    data_out.write(out_pkt);
}

static bool is_suda_done_packet(Acc_Data_Pkt pkt)
{
#pragma HLS INLINE
    return pkt.user.range(7, 4) != 0;
}

static void forward_done_packet(Acc_Data &data_out, Acc_Data_Pkt done_pkt)
{
#pragma HLS INLINE off
    data_out.write(done_pkt);
}

static void write_synthetic_done_packet(Acc_Data &data_out)
{
#pragma HLS INLINE off
    Acc_Data_Pkt out_pkt;
    out_pkt.data = 0;
    out_pkt.keep = ~ap_uint<64>(0);
    out_pkt.strb = ~ap_uint<64>(0);
    out_pkt.user = 0xff;
    out_pkt.last = 1;
    out_pkt.id = 0;
    out_pkt.dest = 0;
    data_out.write(out_pkt);
}

static void write_output_word(
    Acc_Data &data_out,
    ap_uint<512> &packet_data,
    ap_uint<4> &packet_word_count,
    ap_uint<64> word,
    bool force_flush)
{
#pragma HLS INLINE off
    int word_index = packet_word_count.to_uint();
    packet_data.range(word_index * 64 + 63, word_index * 64) = word;
    packet_word_count++;

    if (packet_word_count == LWE_ENCRYPT_WORDS_PER_PKT || force_flush) {
        flush_output_packet(data_out, packet_data, packet_word_count);
    }
}

static ap_uint<11> reverse_psi64_mask_index(ap_uint<11> index)
{
#pragma HLS INLINE
    ap_uint<11> reversed = 0;
reverse_index_loop:
    for (int bit = 0; bit < 11; bit++) {
#pragma HLS UNROLL
        reversed[10 - bit] = index[bit];
    }
    return reversed;
}

static void write_hpu_native_pc_slot(
    Acc_Data &data_out,
    ap_uint<64> pc_data[LWE_ENCRYPT_HPU_PC1_DATA_WORDS],
    ap_uint<64> body,
    bool include_body)
{
#pragma HLS INLINE off
    ap_uint<512> packet_data = 0;
    ap_uint<4> packet_word_count = 0;

native_pc_data_loop:
    for (ap_uint<32> i = 0; i < LWE_ENCRYPT_HPU_PC1_DATA_WORDS; i++) {
#pragma HLS LOOP_TRIPCOUNT min=1024 max=1024
        write_output_word(data_out, packet_data, packet_word_count, pc_data[i], false);
    }

    ap_uint<32> written_words = LWE_ENCRYPT_HPU_PC1_DATA_WORDS;
    if (include_body) {
        write_output_word(data_out, packet_data, packet_word_count, body, false);
        written_words++;
    }

native_pc_padding_loop:
    for (ap_uint<32> i = 0; i < LWE_ENCRYPT_HPU_PC_SLOT_WORDS; i++) {
#pragma HLS LOOP_TRIPCOUNT min=511 max=512
        if (i >= LWE_ENCRYPT_HPU_PC_SLOT_WORDS - written_words) {
            break;
        }
        write_output_word(data_out, packet_data, packet_word_count, 0, false);
    }
}

static void encrypt_encoded_lwe_hpu_native(
    Acc_Data &data_out,
    ap_uint<512> context[256],
    ap_uint<32> noise_mode,
    ap_uint<32> noise_bound_log2,
    ap_uint<64> encoded,
    ap_uint<64> base_seed,
    ap_uint<64> nonce,
    ap_uint<32> ciphertext_index)
{
#pragma HLS INLINE off
    ap_uint<64> pc0[LWE_ENCRYPT_HPU_PC1_DATA_WORDS];
    ap_uint<64> pc1[LWE_ENCRYPT_HPU_PC1_DATA_WORDS];
#pragma HLS BIND_STORAGE variable=pc0 type=ram_2p impl=bram
#pragma HLS BIND_STORAGE variable=pc1 type=ram_2p impl=bram

    ap_uint<64> rng_state = base_seed ^ nonce ^ ap_uint<64>(ciphertext_index + 1);
    ap_uint<64> body = 0;

native_mask_loop:
    for (ap_uint<32> natural_index = 0;
         natural_index < LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION;
         natural_index++) {
#pragma HLS LOOP_TRIPCOUNT min=2048 max=2048
        ap_uint<64> mask_word = next_random(rng_state);
        if (secret_key_bit(context, natural_index)) {
            body += mask_word;
        }

        // tfhe-rs conversion stores cpu_mask[bit_reverse(dst)] at HPU index dst.
        // Bit reversal is involutive, so cpu index i goes to HPU index bit_reverse(i).
        ap_uint<11> hpu_index = reverse_psi64_mask_index(natural_index.range(10, 0));
        ap_uint<7> group = hpu_index.range(10, 4);
        ap_uint<4> lane = hpu_index.range(3, 0);
        ap_uint<10> pc_offset = (ap_uint<10>(group >> 1) << 4) | lane;
        if (group[0] == 0) {
            pc0[pc_offset] = mask_word;
        } else {
            pc1[pc_offset] = mask_word;
        }
    }

    ap_uint<64> noise = 0;
    if (noise_mode == LWE_ENCRYPT_NOISE_TUNIFORM) {
        noise = sample_tuniform_like_noise(rng_state, noise_bound_log2);
    }
    body += encoded;
    body += noise;

    write_hpu_native_pc_slot(data_out, pc0, body, true);
    write_hpu_native_pc_slot(data_out, pc1, 0, false);
}

static void encrypt_encoded_lwe(
    Acc_Data &data_out,
    ap_uint<512> context[256],
    ap_uint<32> mask_dimension,
    ap_uint<32> noise_mode,
    ap_uint<32> noise_bound_log2,
    ap_uint<64> encoded,
    ap_uint<64> input_noise,
    ap_uint<64> base_seed,
    ap_uint<64> nonce,
    ap_uint<32> ciphertext_index)
{
#pragma HLS INLINE off
    ap_uint<64> rng_state = base_seed ^ nonce ^ ap_uint<64>(ciphertext_index + 1);
    ap_uint<64> body = 0;
    ap_uint<512> packet_data = 0;
    ap_uint<4> packet_word_count = 0;

encrypt_mask_loop:
    for (ap_uint<32> i = 0; i < LWE_ENCRYPT_MAX_LWE_DIMENSION; i++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=4096
        if (i >= mask_dimension) {
            break;
        }

        ap_uint<64> mask_word = next_random(rng_state);
        if (secret_key_bit(context, i)) {
            body += mask_word;
        }
        write_output_word(data_out, packet_data, packet_word_count, mask_word, false);
    }

    ap_uint<64> noise = 0;
    if (noise_mode == LWE_ENCRYPT_NOISE_INPUT) {
        noise = input_noise;
    } else if (noise_mode == LWE_ENCRYPT_NOISE_TUNIFORM) {
        noise = sample_tuniform_like_noise(rng_state, noise_bound_log2);
    }
    body += encoded;
    body += noise;

    write_output_word(data_out, packet_data, packet_word_count, body, true);
}

static void encrypt_one_lwe(
    Acc_Data_Pkt input_pkt,
    Acc_Data &data_out,
    ap_uint<512> context[256],
    ap_uint<32> mask_dimension,
    ap_uint<32> input_mode,
    ap_uint<32> noise_mode,
    ap_uint<32> noise_bound_log2,
    ap_uint<64> delta,
    ap_uint<64> base_seed,
    ap_uint<64> nonce,
    ap_uint<32> request_index)
{
#pragma HLS INLINE off
    ap_uint<512> in_data = input_pkt.data;
    ap_uint<64> input_word = in_data.range(63, 0);
    ap_uint<64> input_noise = in_data.range(127, 64);
    ap_uint<64> encoded = input_word;
    if (input_mode == LWE_ENCRYPT_INPUT_CLEAR) {
        encoded = ap_uint<64>(input_word * delta);
    }

    encrypt_encoded_lwe(
        data_out,
        context,
        mask_dimension,
        noise_mode,
        noise_bound_log2,
        encoded,
        input_noise,
        base_seed,
        nonce,
        request_index);
}

static void encrypt_u8_radix(
    ap_uint<8> clear_u8,
    Acc_Data &data_out,
    ap_uint<512> context[256],
    ap_uint<32> mask_dimension,
    ap_uint<32> noise_mode,
    ap_uint<32> noise_bound_log2,
    ap_uint<32> output_layout,
    ap_uint<64> base_seed,
    ap_uint<64> nonce,
    ap_uint<32> request_index)
{
#pragma HLS INLINE off
u8_radix_loop:
    for (ap_uint<32> block_index = 0; block_index < LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT;
         block_index++) {
#pragma HLS LOOP_TRIPCOUNT min=4 max=4
        ap_uint<32> shift = block_index * LWE_ENCRYPT_HPU_MESSAGE_WIDTH;
        ap_uint<64> clear_block =
            (clear_u8 >> shift) & ((ap_uint<64>(1) << LWE_ENCRYPT_HPU_MESSAGE_WIDTH) - 1);
        ap_uint<64> encoded = clear_block * LWE_ENCRYPT_HPU_DELTA;
        ap_uint<32> ciphertext_index = request_index * LWE_ENCRYPT_U8_RADIX_BLOCK_COUNT +
                                       block_index;

        if (output_layout == LWE_ENCRYPT_OUTPUT_HPU_NATIVE) {
            encrypt_encoded_lwe_hpu_native(
                data_out,
                context,
                noise_mode,
                noise_bound_log2,
                encoded,
                base_seed,
                nonce,
                ciphertext_index);
        } else {
            encrypt_encoded_lwe(
                data_out,
                context,
                mask_dimension,
                noise_mode,
                noise_bound_log2,
                encoded,
                0,
                base_seed,
                nonce,
                ciphertext_index);
        }
    }
}

void lwe_encrypt(Acc_Data &data_in, Acc_Data &data_out, ap_uint<512> context[256])
{
#pragma HLS INTERFACE axis port = data_in
#pragma HLS INTERFACE axis port = data_out
#pragma HLS INTERFACE bram port = context
#pragma HLS INTERFACE ap_none port = return

    ap_uint<512> cfg = context[LWE_ENCRYPT_STATIC_CONTEXT_BASE];
    ap_uint<32> mask_dimension = cfg.range(31, 0);
    ap_uint<32> input_count = cfg.range(63, 32);
    ap_uint<32> input_mode = cfg.range(95, 64);
    ap_uint<32> noise_mode = cfg.range(127, 96);
    ap_uint<32> noise_bound_log2 = cfg.range(159, 128);
    ap_uint<32> output_layout = cfg.range(191, 160);
    ap_uint<64> delta = cfg.range(319, 256);
    ap_uint<64> base_seed = cfg.range(383, 320);
    ap_uint<64> nonce = cfg.range(447, 384);

    if (input_mode == LWE_ENCRYPT_INPUT_U8_RADIX) {
        delta = LWE_ENCRYPT_HPU_DELTA;
        if (noise_mode == LWE_ENCRYPT_NOISE_TUNIFORM) {
            noise_bound_log2 = LWE_ENCRYPT_HPU_GLWE_NOISE_BOUND_LOG2;
        }
    }

    if (mask_dimension == 0 || mask_dimension > LWE_ENCRYPT_MAX_LWE_DIMENSION) {
        write_error_packet(data_out, 1);
        return;
    }

    if (input_mode == LWE_ENCRYPT_INPUT_U8_RADIX &&
        mask_dimension != LWE_ENCRYPT_HPU_BIG_LWE_DIMENSION) {
        write_error_packet(data_out, 2);
        return;
    }

    if (input_mode == LWE_ENCRYPT_INPUT_U8_RADIX &&
        noise_mode == LWE_ENCRYPT_NOISE_INPUT) {
        write_error_packet(data_out, 3);
        return;
    }

    if (output_layout > LWE_ENCRYPT_OUTPUT_HPU_NATIVE) {
        write_error_packet(data_out, 5);
        return;
    }

    if (output_layout == LWE_ENCRYPT_OUTPUT_HPU_NATIVE &&
        input_mode != LWE_ENCRYPT_INPUT_U8_RADIX) {
        write_error_packet(data_out, 6);
        return;
    }

    ap_uint<32> processed_count = 0;

stream_loop:
    for (ap_uint<32> stream_index = 0; stream_index < 0xffffffff; stream_index++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=4096
        // SUDA sends normal data with TUSER=0 and terminates a task with an
        // invalid TUSER=0xff packet. Payload and finish marker must be separate.
        Acc_Data_Pkt input_pkt = data_in.read();

        if (is_suda_done_packet(input_pkt)) {
            if (input_count != 0 && processed_count < input_count) {
                write_error_packet(data_out, 4);
                return;
            }
            forward_done_packet(data_out, input_pkt);
            return;
        }

        if (input_count != 0 && processed_count >= input_count) {
            continue;
        }

        if (input_mode == LWE_ENCRYPT_INPUT_U8_RADIX) {
            ap_uint<512> packed_data = input_pkt.data;
            ap_uint<64> packed_keep = input_pkt.keep;
        packed_u8_loop:
            for (int byte_lane = 0; byte_lane < TDATA_WIDTH / 8; byte_lane++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=64
                if (input_count != 0 && processed_count >= input_count) {
                    break;
                }

                bool byte_valid = packed_keep[0] != 0;
                ap_uint<8> clear_u8 = packed_data.range(7, 0);
                packed_data >>= 8;
                packed_keep >>= 1;
                if (!byte_valid) {
                    continue;
                }

                encrypt_u8_radix(
                    clear_u8,
                    data_out,
                    context,
                    mask_dimension,
                    noise_mode,
                    noise_bound_log2,
                    output_layout,
                    base_seed,
                    nonce,
                    processed_count);
                processed_count++;
            }
        } else {
            encrypt_one_lwe(
                input_pkt,
                data_out,
                context,
                mask_dimension,
                input_mode,
                noise_mode,
                noise_bound_log2,
                delta,
                base_seed,
                nonce,
                processed_count);
            processed_count++;
        }

        if (input_count == 0 && input_pkt.last) {
            write_synthetic_done_packet(data_out);
            return;
        }
    }
}
