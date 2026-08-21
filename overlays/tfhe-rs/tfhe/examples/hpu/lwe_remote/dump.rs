use super::protocol::BatchMetadata;
use std::fs;
use std::path::Path;

const MAGIC: &[u8; 8] = b"LWEHLS01";
const VERSION: u64 = 1;

#[derive(Debug)]
pub struct LweHlsDump {
    pub clear_inputs: Vec<u8>,
    pub metadata: BatchMetadata,
    pub ciphertext_words: Vec<u64>,
}

pub fn read_lwehls01(path: &Path) -> Result<LweHlsDump, String> {
    let bytes = fs::read(path).map_err(|err| format!("unable to read {path:?}: {err}"))?;
    let mut cursor = 0usize;

    let magic = take(&bytes, &mut cursor, MAGIC.len())?;
    if magic != MAGIC {
        return Err(format!("{path:?} is not an LWEHLS01 dump"));
    }
    let version = read_u64(&bytes, &mut cursor)?;
    if version != VERSION {
        return Err(format!("unsupported LWEHLS01 version {version}"));
    }

    let mask_dimension = read_usize(&bytes, &mut cursor, "mask_dimension")?;
    let item_count = read_usize(&bytes, &mut cursor, "item_count")?;
    let radix_blocks_per_item = read_usize(&bytes, &mut cursor, "radix_blocks_per_item")?;
    let message_width = read_usize(&bytes, &mut cursor, "message_width")?;
    let carry_width = read_usize(&bytes, &mut cursor, "carry_width")?;
    let padding_bit_width = read_usize(&bytes, &mut cursor, "padding_bit_width")?;
    let delta_log2 = read_usize(&bytes, &mut cursor, "delta_log2")?;
    let ciphertext_word_count = read_usize(&bytes, &mut cursor, "ciphertext_word_count")?;

    let metadata = BatchMetadata {
        mask_dimension,
        item_count,
        radix_blocks_per_item,
        message_width,
        carry_width,
        padding_bit_width,
        delta_log2,
        ciphertext_word_count,
    };
    metadata.validate()?;

    let mut clear_inputs = Vec::with_capacity(item_count);
    for _ in 0..item_count {
        let value = read_u64(&bytes, &mut cursor)?;
        if value > u8::MAX as u64 {
            return Err(format!("clear u8 reference is out of range: {value}"));
        }
        clear_inputs.push(value as u8);
    }

    let mut ciphertext_words = Vec::with_capacity(ciphertext_word_count);
    for _ in 0..ciphertext_word_count {
        ciphertext_words.push(read_u64(&bytes, &mut cursor)?);
    }
    if cursor != bytes.len() {
        return Err(format!(
            "LWEHLS01 dump has {} trailing bytes",
            bytes.len() - cursor
        ));
    }

    Ok(LweHlsDump {
        clear_inputs,
        metadata,
        ciphertext_words,
    })
}

pub fn write_lwehls01(path: &Path, dump: &LweHlsDump) -> Result<(), String> {
    dump.metadata.validate()?;
    if dump.clear_inputs.len() != dump.metadata.item_count {
        return Err(format!(
            "clear reference count mismatch: clear={}, metadata={}",
            dump.clear_inputs.len(),
            dump.metadata.item_count
        ));
    }
    if dump.ciphertext_words.len() != dump.metadata.ciphertext_word_count {
        return Err(format!(
            "ciphertext word count mismatch: payload={}, metadata={}",
            dump.ciphertext_words.len(),
            dump.metadata.ciphertext_word_count
        ));
    }

    let total_bytes = 8usize
        .checked_add(9 * 8)
        .and_then(|size| size.checked_add(dump.clear_inputs.len() * 8))
        .and_then(|size| size.checked_add(dump.ciphertext_words.len() * 8))
        .ok_or_else(|| "LWEHLS01 output size overflow".to_string())?;
    let mut bytes = Vec::with_capacity(total_bytes);
    bytes.extend_from_slice(MAGIC);
    push_u64(&mut bytes, VERSION);
    push_usize(&mut bytes, dump.metadata.mask_dimension, "mask_dimension")?;
    push_usize(&mut bytes, dump.metadata.item_count, "item_count")?;
    push_usize(
        &mut bytes,
        dump.metadata.radix_blocks_per_item,
        "radix_blocks_per_item",
    )?;
    push_usize(&mut bytes, dump.metadata.message_width, "message_width")?;
    push_usize(&mut bytes, dump.metadata.carry_width, "carry_width")?;
    push_usize(
        &mut bytes,
        dump.metadata.padding_bit_width,
        "padding_bit_width",
    )?;
    push_usize(&mut bytes, dump.metadata.delta_log2, "delta_log2")?;
    push_usize(
        &mut bytes,
        dump.metadata.ciphertext_word_count,
        "ciphertext_word_count",
    )?;
    for value in &dump.clear_inputs {
        push_u64(&mut bytes, u64::from(*value));
    }
    for word in &dump.ciphertext_words {
        push_u64(&mut bytes, *word);
    }
    fs::write(path, bytes).map_err(|err| format!("unable to write {path:?}: {err}"))
}

fn take<'a>(bytes: &'a [u8], cursor: &mut usize, len: usize) -> Result<&'a [u8], String> {
    let end = cursor
        .checked_add(len)
        .ok_or_else(|| "LWEHLS01 offset overflow".to_string())?;
    let value = bytes
        .get(*cursor..end)
        .ok_or_else(|| "unexpected end of LWEHLS01 dump".to_string())?;
    *cursor = end;
    Ok(value)
}

fn read_u64(bytes: &[u8], cursor: &mut usize) -> Result<u64, String> {
    let raw = take(bytes, cursor, 8)?;
    Ok(u64::from_le_bytes(raw.try_into().unwrap()))
}

fn read_usize(bytes: &[u8], cursor: &mut usize, label: &str) -> Result<usize, String> {
    usize::try_from(read_u64(bytes, cursor)?).map_err(|_| format!("{label} does not fit in usize"))
}

fn push_u64(bytes: &mut Vec<u8>, value: u64) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

fn push_usize(bytes: &mut Vec<u8>, value: usize, label: &str) -> Result<(), String> {
    push_u64(
        bytes,
        u64::try_from(value).map_err(|_| format!("{label} does not fit in u64"))?,
    );
    Ok(())
}
