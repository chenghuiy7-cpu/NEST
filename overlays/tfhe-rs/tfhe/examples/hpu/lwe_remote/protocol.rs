#![allow(dead_code)]

use std::io::{Read, Write};

pub const OP_ADD_SCALAR_U8: u64 = 1;
pub const OP_ADD_SCALAR_U8_HPU_NATIVE: u64 = 2;
pub const OP_ADD_SCALAR_U8_HPU_NATIVE_ROUNDTRIP: u64 = 3;
pub const OP_ECHO_U8: u64 = 0;
pub const FRAME_REQUEST: u64 = 1;
pub const FRAME_RESPONSE: u64 = 2;
pub const FRAME_ERROR: u64 = 3;
pub const VERSION_V1: u64 = 1;
pub const VERSION_TIMING: u64 = 2;

const MAGIC: &[u8; 8] = b"LWERPC01";
const HEADER_U64S: usize = 14;
const TIMING_MAGIC: &[u8; 8] = b"LWEBEN01";
const TIMING_VERSION: u64 = 1;
const TIMING_U64S: usize = 15;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ServerTiming {
    pub request_id: u64,
    pub request_receive_ns: u64,
    pub request_validate_ns: u64,
    pub request_decode_ns: u64,
    pub hpu_prepare_ns: u64,
    pub hpu_enqueue_ns: u64,
    pub hpu_wait_sync_ns: u64,
    pub hpu_output_convert_ns: u64,
    pub result_encode_ns: u64,
    pub mem_sanitizer_ns: u64,
    pub remote_process_ns: u64,
    pub response_send_ns: u64,
    pub server_total_ns: u64,
    pub response_payload_bytes: u64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchMetadata {
    pub mask_dimension: usize,
    pub item_count: usize,
    pub radix_blocks_per_item: usize,
    pub message_width: usize,
    pub carry_width: usize,
    pub padding_bit_width: usize,
    pub delta_log2: usize,
    pub ciphertext_word_count: usize,
}

impl BatchMetadata {
    pub fn expected_cpu_word_count(&self) -> Result<usize, String> {
        self.item_count
            .checked_mul(self.radix_blocks_per_item)
            .and_then(|count| count.checked_mul(self.mask_dimension.checked_add(1)?))
            .ok_or_else(|| "ciphertext word count overflow".to_string())
    }

    pub fn expected_hpu_native_word_count(&self) -> Result<usize, String> {
        const HPU_NATIVE_WORDS_PER_LWE: usize = 2 * 1536;
        self.item_count
            .checked_mul(self.radix_blocks_per_item)
            .and_then(|count| count.checked_mul(HPU_NATIVE_WORDS_PER_LWE))
            .ok_or_else(|| "HPU-native ciphertext word count overflow".to_string())
    }

    pub fn validate_cpu_payload(&self) -> Result<(), String> {
        let expected = self.expected_cpu_word_count()?;
        if self.ciphertext_word_count != expected {
            return Err(format!(
                "CPU-LWE ciphertext word count mismatch: metadata={}, expected={expected}",
                self.ciphertext_word_count
            ));
        }
        Ok(())
    }

    pub fn validate_hpu_native_payload(&self) -> Result<(), String> {
        let expected = self.expected_hpu_native_word_count()?;
        if self.ciphertext_word_count != expected {
            return Err(format!(
                "HPU-native ciphertext word count mismatch: metadata={}, expected={expected}",
                self.ciphertext_word_count
            ));
        }
        Ok(())
    }

    pub fn validate(&self) -> Result<(), String> {
        if self.item_count == 0 {
            return Err("empty ciphertext batch".to_string());
        }
        if self.radix_blocks_per_item == 0 || self.mask_dimension == 0 {
            return Err("invalid zero-sized radix/LWE shape".to_string());
        }
        if self.validate_cpu_payload().is_err() && self.validate_hpu_native_payload().is_err() {
            return Err(format!(
                "ciphertext word count {} matches neither CPU-LWE nor HPU-native layout",
                self.ciphertext_word_count
            ));
        }
        Ok(())
    }
}

#[derive(Debug)]
pub struct CiphertextFrame {
    pub version: u64,
    pub kind: u64,
    pub request_id: u64,
    pub operation: u64,
    pub status: u64,
    pub scalar: u64,
    pub metadata: BatchMetadata,
    pub ciphertext_words: Vec<u64>,
    pub error_message: Option<String>,
}

pub fn write_ciphertext_frame(
    stream: &mut impl Write,
    kind: u64,
    request_id: u64,
    operation: u64,
    scalar: u64,
    metadata: &BatchMetadata,
    ciphertext_words: &[u64],
) -> Result<(), String> {
    write_ciphertext_frame_version(
        stream,
        VERSION_V1,
        kind,
        request_id,
        operation,
        scalar,
        metadata,
        ciphertext_words,
    )
}

pub fn write_ciphertext_frame_version(
    stream: &mut impl Write,
    version: u64,
    kind: u64,
    request_id: u64,
    operation: u64,
    scalar: u64,
    metadata: &BatchMetadata,
    ciphertext_words: &[u64],
) -> Result<(), String> {
    validate_version(version)?;
    metadata.validate()?;
    if ciphertext_words.len() != metadata.ciphertext_word_count {
        return Err(format!(
            "payload word count mismatch: payload={}, metadata={}",
            ciphertext_words.len(),
            metadata.ciphertext_word_count
        ));
    }
    let payload_bytes = ciphertext_words
        .len()
        .checked_mul(8)
        .ok_or_else(|| "payload byte count overflow".to_string())?;

    stream
        .write_all(MAGIC)
        .map_err(|err| format!("unable to write protocol magic: {err}"))?;
    let fields = [
        version,
        kind,
        request_id,
        operation,
        0,
        scalar,
        usize_to_u64(metadata.mask_dimension, "mask_dimension")?,
        usize_to_u64(metadata.item_count, "item_count")?,
        usize_to_u64(metadata.radix_blocks_per_item, "radix_blocks_per_item")?,
        usize_to_u64(metadata.message_width, "message_width")?,
        usize_to_u64(metadata.carry_width, "carry_width")?,
        usize_to_u64(metadata.padding_bit_width, "padding_bit_width")?,
        usize_to_u64(metadata.delta_log2, "delta_log2")?,
        usize_to_u64(metadata.ciphertext_word_count, "ciphertext_word_count")?,
    ];
    for field in fields {
        write_u64(stream, field)?;
    }
    write_u64(stream, usize_to_u64(payload_bytes, "payload_bytes")?)?;
    for word in ciphertext_words {
        write_u64(stream, *word)?;
    }
    stream
        .flush()
        .map_err(|err| format!("unable to flush ciphertext frame: {err}"))
}

pub fn write_error_frame(
    stream: &mut impl Write,
    request_id: u64,
    operation: u64,
    message: &str,
) -> Result<(), String> {
    write_error_frame_version(stream, VERSION_V1, request_id, operation, message)
}

pub fn write_error_frame_version(
    stream: &mut impl Write,
    version: u64,
    request_id: u64,
    operation: u64,
    message: &str,
) -> Result<(), String> {
    validate_version(version)?;
    let message_bytes = message.as_bytes();
    stream
        .write_all(MAGIC)
        .map_err(|err| format!("unable to write protocol magic: {err}"))?;
    let fields = [
        version,
        FRAME_ERROR,
        request_id,
        operation,
        1,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    ];
    for field in fields {
        write_u64(stream, field)?;
    }
    write_u64(
        stream,
        usize_to_u64(message_bytes.len(), "error payload bytes")?,
    )?;
    stream
        .write_all(message_bytes)
        .map_err(|err| format!("unable to write error frame: {err}"))?;
    stream
        .flush()
        .map_err(|err| format!("unable to flush error frame: {err}"))
}

pub fn read_frame(
    stream: &mut impl Read,
    max_payload_bytes: usize,
) -> Result<CiphertextFrame, String> {
    let mut magic = [0_u8; 8];
    stream
        .read_exact(&mut magic)
        .map_err(|err| format!("unable to read protocol magic: {err}"))?;
    if &magic != MAGIC {
        return Err(format!("invalid protocol magic: {magic:02x?}"));
    }

    let mut fields = [0_u64; HEADER_U64S];
    for field in &mut fields {
        *field = read_u64(stream)?;
    }
    let payload_bytes = u64_to_usize(read_u64(stream)?, "payload_bytes")?;

    validate_version(fields[0])?;
    if payload_bytes > max_payload_bytes {
        return Err(format!(
            "payload is too large: {payload_bytes} bytes, limit={max_payload_bytes}"
        ));
    }

    let kind = fields[1];
    let request_id = fields[2];
    let operation = fields[3];
    let status = fields[4];
    let scalar = fields[5];
    let metadata = BatchMetadata {
        mask_dimension: u64_to_usize(fields[6], "mask_dimension")?,
        item_count: u64_to_usize(fields[7], "item_count")?,
        radix_blocks_per_item: u64_to_usize(fields[8], "radix_blocks_per_item")?,
        message_width: u64_to_usize(fields[9], "message_width")?,
        carry_width: u64_to_usize(fields[10], "carry_width")?,
        padding_bit_width: u64_to_usize(fields[11], "padding_bit_width")?,
        delta_log2: u64_to_usize(fields[12], "delta_log2")?,
        ciphertext_word_count: u64_to_usize(fields[13], "ciphertext_word_count")?,
    };

    if kind == FRAME_ERROR {
        let mut payload = vec![0_u8; payload_bytes];
        stream
            .read_exact(&mut payload)
            .map_err(|err| format!("unable to read error payload: {err}"))?;
        let message = String::from_utf8(payload)
            .map_err(|err| format!("remote error payload is not UTF-8: {err}"))?;
        return Ok(CiphertextFrame {
            version: fields[0],
            kind,
            request_id,
            operation,
            status,
            scalar,
            metadata,
            ciphertext_words: Vec::new(),
            error_message: Some(message),
        });
    }

    if kind != FRAME_REQUEST && kind != FRAME_RESPONSE {
        return Err(format!("unsupported frame kind {kind}"));
    }
    metadata.validate()?;
    let expected_payload_bytes = metadata
        .ciphertext_word_count
        .checked_mul(8)
        .ok_or_else(|| "payload byte count overflow".to_string())?;
    if payload_bytes != expected_payload_bytes {
        return Err(format!(
            "payload byte count mismatch: header={payload_bytes}, expected={expected_payload_bytes}"
        ));
    }

    // Read the ciphertext in one buffered transfer. Calling read_exact once per
    // u64 turns a multi-megabyte native HPU request into millions of socket
    // reads and can keep the single-threaded server from reaching HPU dispatch.
    let mut payload = vec![0_u8; payload_bytes];
    stream
        .read_exact(&mut payload)
        .map_err(|err| format!("unable to read ciphertext payload: {err}"))?;
    let ciphertext_words = payload
        .chunks_exact(8)
        .map(|chunk| {
            u64::from_le_bytes([
                chunk[0], chunk[1], chunk[2], chunk[3], chunk[4], chunk[5], chunk[6], chunk[7],
            ])
        })
        .collect();
    Ok(CiphertextFrame {
        version: fields[0],
        kind,
        request_id,
        operation,
        status,
        scalar,
        metadata,
        ciphertext_words,
        error_message: None,
    })
}

pub fn write_server_timing(stream: &mut impl Write, timing: &ServerTiming) -> Result<(), String> {
    stream
        .write_all(TIMING_MAGIC)
        .map_err(|err| format!("unable to write timing magic: {err}"))?;
    let fields = [
        TIMING_VERSION,
        timing.request_id,
        timing.request_receive_ns,
        timing.request_validate_ns,
        timing.request_decode_ns,
        timing.hpu_prepare_ns,
        timing.hpu_enqueue_ns,
        timing.hpu_wait_sync_ns,
        timing.hpu_output_convert_ns,
        timing.result_encode_ns,
        timing.mem_sanitizer_ns,
        timing.remote_process_ns,
        timing.response_send_ns,
        timing.server_total_ns,
        timing.response_payload_bytes,
    ];
    debug_assert_eq!(fields.len(), TIMING_U64S);
    for field in fields {
        write_u64(stream, field)?;
    }
    stream
        .flush()
        .map_err(|err| format!("unable to flush timing trailer: {err}"))
}

fn validate_version(version: u64) -> Result<(), String> {
    if version == VERSION_V1 || version == VERSION_TIMING {
        Ok(())
    } else {
        Err(format!("unsupported protocol version {version}"))
    }
}

fn read_u64(stream: &mut impl Read) -> Result<u64, String> {
    let mut bytes = [0_u8; 8];
    stream
        .read_exact(&mut bytes)
        .map_err(|err| format!("unable to read u64 protocol field: {err}"))?;
    Ok(u64::from_le_bytes(bytes))
}

fn write_u64(stream: &mut impl Write, value: u64) -> Result<(), String> {
    stream
        .write_all(&value.to_le_bytes())
        .map_err(|err| format!("unable to write u64 protocol field: {err}"))
}

fn usize_to_u64(value: usize, label: &str) -> Result<u64, String> {
    u64::try_from(value).map_err(|_| format!("{label} does not fit in u64"))
}

fn u64_to_usize(value: u64, label: &str) -> Result<usize, String> {
    usize::try_from(value).map_err(|_| format!("{label} does not fit in usize"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn metadata() -> BatchMetadata {
        BatchMetadata {
            mask_dimension: 3,
            item_count: 2,
            radix_blocks_per_item: 2,
            message_width: 4,
            carry_width: 1,
            padding_bit_width: 1,
            delta_log2: 58,
            ciphertext_word_count: 16,
        }
    }

    #[test]
    fn ciphertext_frame_round_trip() {
        let metadata = metadata();
        let words: Vec<u64> = (0..metadata.ciphertext_word_count as u64).collect();
        let mut wire = Vec::new();
        write_ciphertext_frame(
            &mut wire,
            FRAME_REQUEST,
            42,
            OP_ADD_SCALAR_U8,
            7,
            &metadata,
            &words,
        )
        .unwrap();

        let frame = read_frame(&mut Cursor::new(wire), 4096).unwrap();
        assert_eq!(frame.kind, FRAME_REQUEST);
        assert_eq!(frame.version, VERSION_V1);
        assert_eq!(frame.request_id, 42);
        assert_eq!(frame.operation, OP_ADD_SCALAR_U8);
        assert_eq!(frame.scalar, 7);
        assert_eq!(frame.metadata, metadata);
        assert_eq!(frame.ciphertext_words, words);
    }

    #[test]
    fn timing_version_frame_round_trip() {
        let metadata = metadata();
        let words: Vec<u64> = (0..metadata.ciphertext_word_count as u64).collect();
        let mut wire = Vec::new();
        write_ciphertext_frame_version(
            &mut wire,
            VERSION_TIMING,
            FRAME_REQUEST,
            43,
            OP_ECHO_U8,
            0,
            &metadata,
            &words,
        )
        .unwrap();

        let frame = read_frame(&mut Cursor::new(wire), 4096).unwrap();
        assert_eq!(frame.version, VERSION_TIMING);
        assert_eq!(frame.operation, OP_ECHO_U8);
        assert_eq!(frame.ciphertext_words, words);
    }

    #[test]
    fn hpu_native_ciphertext_frame_round_trip() {
        let metadata = BatchMetadata {
            mask_dimension: 2048,
            item_count: 1,
            radix_blocks_per_item: 4,
            message_width: 2,
            carry_width: 2,
            padding_bit_width: 1,
            delta_log2: 59,
            ciphertext_word_count: 4 * 2 * 1536,
        };
        let words = vec![0_u64; metadata.ciphertext_word_count];
        let mut wire = Vec::new();
        write_ciphertext_frame_version(
            &mut wire,
            VERSION_TIMING,
            FRAME_REQUEST,
            44,
            OP_ADD_SCALAR_U8_HPU_NATIVE,
            1,
            &metadata,
            &words,
        )
        .unwrap();

        let frame = read_frame(&mut Cursor::new(wire), words.len() * 8).unwrap();
        assert_eq!(frame.operation, OP_ADD_SCALAR_U8_HPU_NATIVE);
        assert_eq!(frame.metadata, metadata);
        assert_eq!(frame.ciphertext_words, words);
    }

    #[test]
    fn server_timing_has_fixed_128_byte_wire_size() {
        let mut wire = Vec::new();
        write_server_timing(
            &mut wire,
            &ServerTiming {
                request_id: 7,
                response_payload_bytes: 128,
                ..ServerTiming::default()
            },
        )
        .unwrap();
        assert_eq!(wire.len(), 8 + TIMING_U64S * 8);
        assert_eq!(&wire[..8], TIMING_MAGIC);
    }

    #[test]
    fn error_frame_round_trip() {
        let mut wire = Vec::new();
        write_error_frame(&mut wire, 9, OP_ADD_SCALAR_U8, "bad request").unwrap();

        let frame = read_frame(&mut Cursor::new(wire), 4096).unwrap();
        assert_eq!(frame.kind, FRAME_ERROR);
        assert_eq!(frame.request_id, 9);
        assert_eq!(frame.error_message.as_deref(), Some("bad request"));
    }

    #[test]
    fn payload_limit_is_enforced_before_allocation() {
        let metadata = metadata();
        let words = vec![0_u64; metadata.ciphertext_word_count];
        let mut wire = Vec::new();
        write_ciphertext_frame(
            &mut wire,
            FRAME_REQUEST,
            1,
            OP_ADD_SCALAR_U8,
            1,
            &metadata,
            &words,
        )
        .unwrap();

        let error = read_frame(&mut Cursor::new(wire), 8).unwrap_err();
        assert!(error.contains("payload is too large"));
    }
}
