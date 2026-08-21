#include "lwe_decrypt.hpp"

static bool is_suda_done_packet(Acc_Data_Pkt pkt)
{
#pragma HLS INLINE
    return pkt.user.range(7, 4) != 0;
}

static bool secret_key_bit(ap_uint<512> context[256], ap_uint<11> natural_index)
{
#pragma HLS INLINE
    ap_uint<32> context_word =
        LWE_DECRYPT_KEY_CONTEXT_BASE + (natural_index >> 9);
    ap_uint<9> bit_index = natural_index.range(8, 0);
    return context[context_word][bit_index] != 0;
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

static ap_uint<11> natural_index_from_pc_offset(
    ap_uint<1> pc_index,
    ap_uint<10> pc_offset)
{
#pragma HLS INLINE
    ap_uint<7> group_in_pc = pc_offset >> 4;
    ap_uint<4> lane = pc_offset.range(3, 0);
    ap_uint<7> interleaved_group = (group_in_pc << 1) | pc_index;
    ap_uint<11> hpu_index = (ap_uint<11>(interleaved_group) << 4) | lane;
    return reverse_psi64_mask_index(hpu_index);
}

static void write_error_packet(Acc_Data &data_out, ap_uint<32> code)
{
#pragma HLS INLINE off
    Acc_Data_Pkt out_pkt;
    out_pkt.data = 0;
    out_pkt.data.range(63, 0) = 0x4c57454445435252ULL;
    out_pkt.data.range(95, 64) = code;
    out_pkt.keep = ~ap_uint<64>(0);
    out_pkt.strb = ~ap_uint<64>(0);
    out_pkt.user = 0xff;
    out_pkt.last = 1;
    out_pkt.id = 0;
    out_pkt.dest = 0;
    data_out.write(out_pkt);
}

static void flush_clear_packet(
    Acc_Data &data_out,
    ap_uint<512> &packet_data,
    ap_uint<7> &packet_byte_count)
{
#pragma HLS INLINE off
    if (packet_byte_count == 0) {
        return;
    }

    Acc_Data_Pkt out_pkt;
    out_pkt.data = packet_data;
    out_pkt.keep = 0;
    out_pkt.strb = 0;
output_keep_loop:
    for (int byte_lane = 0; byte_lane < TDATA_WIDTH / 8; byte_lane++) {
#pragma HLS UNROLL
        if (byte_lane < packet_byte_count) {
            out_pkt.keep[byte_lane] = 1;
            out_pkt.strb[byte_lane] = 1;
        }
    }
    out_pkt.user = 0;
    out_pkt.last = 0;
    out_pkt.id = 0;
    out_pkt.dest = 0;
    data_out.write(out_pkt);
    packet_data = 0;
    packet_byte_count = 0;
}

static void write_clear_byte(
    Acc_Data &data_out,
    ap_uint<512> &packet_data,
    ap_uint<7> &packet_byte_count,
    ap_uint<8> clear_u8)
{
#pragma HLS INLINE off
    int byte_index = packet_byte_count.to_uint();
    packet_data.range(byte_index * 8 + 7, byte_index * 8) = clear_u8;
    packet_byte_count++;
    if (packet_byte_count == TDATA_WIDTH / 8) {
        flush_clear_packet(data_out, packet_data, packet_byte_count);
    }
}

static void forward_done_packet(Acc_Data &data_out, Acc_Data_Pkt done_pkt)
{
#pragma HLS INLINE off
    data_out.write(done_pkt);
}

static ap_uint<2> decode_radix_block(ap_uint<64> body, ap_uint<64> dot)
{
#pragma HLS INLINE
    ap_uint<64> phase = body - dot;
    ap_uint<65> rounded_phase = phase;
    rounded_phase += ap_uint<64>(LWE_DECRYPT_HPU_DELTA >> 1);
    ap_uint<64> rounded = rounded_phase >> LWE_DECRYPT_HPU_DELTA_LOG2;
    return rounded.range(LWE_DECRYPT_HPU_MESSAGE_WIDTH - 1, 0);
}

void lwe_decrypt(Acc_Data &data_in, Acc_Data &data_out, ap_uint<512> context[256])
{
#pragma HLS INTERFACE axis register_mode = off port = data_in
#pragma HLS INTERFACE axis register_mode = off port = data_out
#pragma HLS INTERFACE bram port = context

    ap_uint<512> cfg = context[LWE_DECRYPT_STATIC_CONTEXT_BASE];
    ap_uint<32> input_count = cfg.range(31, 0);
    ap_uint<32> mask_dimension = cfg.range(63, 32);
    ap_uint<64> delta = cfg.range(127, 64);
    ap_uint<32> message_width = cfg.range(159, 128);
    ap_uint<32> radix_blocks = cfg.range(191, 160);
    ap_uint<32> input_layout = cfg.range(223, 192);

    if (delta == 0) {
        delta = LWE_DECRYPT_HPU_DELTA;
    }
    if (message_width == 0) {
        message_width = LWE_DECRYPT_HPU_MESSAGE_WIDTH;
    }
    if (radix_blocks == 0) {
        radix_blocks = LWE_DECRYPT_U8_RADIX_BLOCK_COUNT;
    }

    if (mask_dimension != LWE_DECRYPT_HPU_BIG_LWE_DIMENSION) {
        write_error_packet(data_out, 1);
        return;
    }
    if (delta != LWE_DECRYPT_HPU_DELTA) {
        write_error_packet(data_out, 2);
        return;
    }
    if (message_width != LWE_DECRYPT_HPU_MESSAGE_WIDTH ||
        radix_blocks != LWE_DECRYPT_U8_RADIX_BLOCK_COUNT) {
        write_error_packet(data_out, 3);
        return;
    }
    if (input_layout != LWE_DECRYPT_INPUT_HPU_NATIVE) {
        write_error_packet(data_out, 4);
        return;
    }

    ap_uint<32> processed_count = 0;
    ap_uint<3> block_index = 0;
    ap_uint<1> pc_index = 0;
    ap_uint<8> packet_index_in_pc = 0;
    ap_uint<64> dot = 0;
    ap_uint<64> body = 0;
    ap_uint<8> reconstructed_u8 = 0;
    ap_uint<512> clear_packet_data = 0;
    ap_uint<7> clear_packet_byte_count = 0;

decrypt_stream_loop:
    for (ap_uint<32> stream_index = 0; stream_index < 0xffffffff; stream_index++) {
#pragma HLS LOOP_TRIPCOUNT min=1 max=1572864
        Acc_Data_Pkt input_pkt = data_in.read();

        if (is_suda_done_packet(input_pkt)) {
            if ((input_count != 0 && processed_count != input_count) ||
                block_index != 0 || pc_index != 0 || packet_index_in_pc != 0) {
                write_error_packet(data_out, 5);
                return;
            }
            flush_clear_packet(data_out, clear_packet_data, clear_packet_byte_count);
            forward_done_packet(data_out, input_pkt);
            return;
        }

        if (input_count != 0 && processed_count >= input_count) {
            write_error_packet(data_out, 6);
            return;
        }

        if (packet_index_in_pc <
            LWE_DECRYPT_HPU_PC_DATA_WORDS / (TDATA_WIDTH / 64)) {
            ap_uint<10> packet_base = ap_uint<10>(packet_index_in_pc) << 3;
        mask_word_loop:
            for (int word_lane = 0; word_lane < TDATA_WIDTH / 64; word_lane++) {
#pragma HLS PIPELINE II = 1
                ap_uint<10> pc_offset = packet_base + word_lane;
                ap_uint<11> natural_index =
                    natural_index_from_pc_offset(pc_index, pc_offset);
                ap_uint<64> mask_word =
                    input_pkt.data.range(word_lane * 64 + 63, word_lane * 64);
                if (secret_key_bit(context, natural_index)) {
                    dot += mask_word;
                }
            }
        } else if (pc_index == 0 &&
                   packet_index_in_pc ==
                       LWE_DECRYPT_HPU_PC_DATA_WORDS / (TDATA_WIDTH / 64)) {
            body = input_pkt.data.range(63, 0);
        }

        packet_index_in_pc++;
        if (packet_index_in_pc == LWE_DECRYPT_HPU_PC_SLOT_PACKETS) {
            packet_index_in_pc = 0;
            if (pc_index == 0) {
                pc_index = 1;
            } else {
                pc_index = 0;
                ap_uint<2> clear_block = decode_radix_block(body, dot);
                ap_uint<4> shift = block_index * LWE_DECRYPT_HPU_MESSAGE_WIDTH;
                reconstructed_u8 |= ap_uint<8>(clear_block) << shift;
                dot = 0;
                body = 0;
                block_index++;

                if (block_index == LWE_DECRYPT_U8_RADIX_BLOCK_COUNT) {
                    write_clear_byte(
                        data_out,
                        clear_packet_data,
                        clear_packet_byte_count,
                        reconstructed_u8);
                    reconstructed_u8 = 0;
                    block_index = 0;
                    processed_count++;
                }
            }
        }
    }
}
