#![allow(dead_code)]

use super::protocol::BatchMetadata;
use tfhe::core_crypto::prelude::LweCiphertextOwned;
use tfhe::integer::{IntegerCiphertext, RadixCiphertext};
use tfhe::shortint::ciphertext::{Degree, NoiseLevel};
use tfhe::shortint::parameters::ShortintParameterSet;
use tfhe::shortint::Ciphertext;
use tfhe_hpu_backend::prelude::{HpuLweCiphertextOwned, HpuParameters};

pub const HPU_NATIVE_PC_COUNT: usize = 2;
pub const HPU_NATIVE_PC_SLOT_WORDS: usize = 1536;
pub const HPU_NATIVE_PC0_DATA_WORDS: usize = 1025;
pub const HPU_NATIVE_PC1_DATA_WORDS: usize = 1024;

pub fn validate_metadata(
    metadata: &BatchMetadata,
    params: &ShortintParameterSet,
) -> Result<(), String> {
    metadata.validate()?;
    if metadata.mask_dimension != params.encryption_lwe_dimension().0 {
        return Err(format!(
            "Big-LWE dimension mismatch: request={}, parameters={}",
            metadata.mask_dimension,
            params.encryption_lwe_dimension().0
        ));
    }
    let message_width = params.message_modulus().0.ilog2() as usize;
    let carry_width = params.carry_modulus().0.ilog2() as usize;
    if metadata.message_width != message_width || metadata.carry_width != carry_width {
        return Err(format!(
            "message/carry width mismatch: request={}/{}, parameters={message_width}/{carry_width}",
            metadata.message_width, metadata.carry_width
        ));
    }
    let expected_blocks = u8::BITS as usize / message_width;
    if metadata.radix_blocks_per_item != expected_blocks {
        return Err(format!(
            "u8 radix block count mismatch: request={}, expected={expected_blocks}",
            metadata.radix_blocks_per_item
        ));
    }
    let expected_delta_log2 =
        u64::BITS as usize - message_width - carry_width - metadata.padding_bit_width;
    if metadata.delta_log2 != expected_delta_log2 {
        return Err(format!(
            "delta_log2 mismatch: request={}, expected={expected_delta_log2}",
            metadata.delta_log2
        ));
    }
    Ok(())
}

pub fn words_to_radix_ciphertexts(
    metadata: &BatchMetadata,
    ciphertext_words: &[u64],
    params: &ShortintParameterSet,
) -> Result<Vec<RadixCiphertext>, String> {
    validate_metadata(metadata, params)?;
    metadata.validate_cpu_payload()?;
    if ciphertext_words.len() != metadata.ciphertext_word_count {
        return Err(format!(
            "ciphertext payload word count mismatch: payload={}, metadata={}",
            ciphertext_words.len(),
            metadata.ciphertext_word_count
        ));
    }

    let words_per_lwe = metadata.mask_dimension + 1;
    let mut radix_ciphertexts = Vec::with_capacity(metadata.item_count);
    for item_index in 0..metadata.item_count {
        let mut blocks = Vec::with_capacity(metadata.radix_blocks_per_item);
        for block_index in 0..metadata.radix_blocks_per_item {
            let ciphertext_index = item_index * metadata.radix_blocks_per_item + block_index;
            let start = ciphertext_index * words_per_lwe;
            let end = start + words_per_lwe;
            let lwe = LweCiphertextOwned::from_container(
                ciphertext_words[start..end].to_vec(),
                params.ciphertext_modulus(),
            );
            blocks.push(Ciphertext::new(
                lwe,
                Degree::new(params.message_modulus().0 - 1),
                NoiseLevel::NOMINAL,
                params.message_modulus(),
                params.carry_modulus(),
                params.atomic_pattern(),
            ));
        }
        radix_ciphertexts.push(RadixCiphertext::from(blocks));
    }
    Ok(radix_ciphertexts)
}

pub fn hpu_native_words_to_lwe_ciphertexts(
    metadata: &BatchMetadata,
    ciphertext_words: &[u64],
    params: &HpuParameters,
) -> Result<Vec<Vec<HpuLweCiphertextOwned<u64>>>, String> {
    metadata.validate_hpu_native_payload()?;
    if metadata.mask_dimension
        != params.pbs_params.glwe_dimension * params.pbs_params.polynomial_size
    {
        return Err(format!(
            "HPU-native Big-LWE dimension mismatch: request={}, HPU={}",
            metadata.mask_dimension,
            params.pbs_params.glwe_dimension * params.pbs_params.polynomial_size
        ));
    }
    if params.pc_params.pem_pc != HPU_NATIVE_PC_COUNT
        || params.regf_params.coef_nb / params.pc_params.pem_pc != 16
    {
        return Err(format!(
            "unsupported HPU PC geometry: pem_pc={} coef_nb={}",
            params.pc_params.pem_pc, params.regf_params.coef_nb
        ));
    }
    if params.ntt_params.radix != 2
        || params.ntt_params.stg_nb != 11
        || params.ntt_params.ct_width != 64
        || params.pbs_params.ciphertext_width != 64
    {
        return Err(format!(
            "unsupported HPU-native coefficient format: radix={} stages={} ntt_ct_width={} pbs_ct_width={}",
            params.ntt_params.radix,
            params.ntt_params.stg_nb,
            params.ntt_params.ct_width,
            params.pbs_params.ciphertext_width
        ));
    }
    if ciphertext_words.len() != metadata.ciphertext_word_count {
        return Err(format!(
            "HPU-native payload word count mismatch: payload={}, metadata={}",
            ciphertext_words.len(),
            metadata.ciphertext_word_count
        ));
    }

    let words_per_lwe = HPU_NATIVE_PC_COUNT * HPU_NATIVE_PC_SLOT_WORDS;
    let mut items = Vec::with_capacity(metadata.item_count);
    for item_index in 0..metadata.item_count {
        let mut blocks = Vec::with_capacity(metadata.radix_blocks_per_item);
        for block_index in 0..metadata.radix_blocks_per_item {
            let lwe_index = item_index * metadata.radix_blocks_per_item + block_index;
            let base = lwe_index * words_per_lwe;
            let pc0 = &ciphertext_words[base..base + HPU_NATIVE_PC_SLOT_WORDS];
            let pc1 = &ciphertext_words[base + HPU_NATIVE_PC_SLOT_WORDS..base + words_per_lwe];
            if pc0[HPU_NATIVE_PC0_DATA_WORDS..]
                .iter()
                .chain(pc1[HPU_NATIVE_PC1_DATA_WORDS..].iter())
                .any(|word| *word != 0)
            {
                return Err(format!(
                    "non-zero HPU slot padding at item={item_index} block={block_index}"
                ));
            }
            blocks.push(HpuLweCiphertextOwned::from_container(
                vec![
                    pc0[..HPU_NATIVE_PC0_DATA_WORDS].to_vec(),
                    pc1[..HPU_NATIVE_PC1_DATA_WORDS].to_vec(),
                ],
                params.clone(),
            ));
        }
        items.push(blocks);
    }
    Ok(items)
}

pub fn hpu_lwe_ciphertexts_to_native_words(
    metadata: &BatchMetadata,
    ciphertexts: Vec<Vec<HpuLweCiphertextOwned<u64>>>,
) -> Result<Vec<u64>, String> {
    metadata.validate_hpu_native_payload()?;
    if ciphertexts.len() != metadata.item_count {
        return Err(format!(
            "HPU-native result item count mismatch: result={}, expected={}",
            ciphertexts.len(),
            metadata.item_count
        ));
    }

    let mut words = Vec::with_capacity(metadata.ciphertext_word_count);
    for (item_index, item) in ciphertexts.into_iter().enumerate() {
        if item.len() != metadata.radix_blocks_per_item {
            return Err(format!(
                "HPU-native result radix count mismatch at item={item_index}: result={}, expected={}",
                item.len(),
                metadata.radix_blocks_per_item
            ));
        }
        for (block_index, block) in item.into_iter().enumerate() {
            let cuts = block.into_container();
            if cuts.len() != HPU_NATIVE_PC_COUNT
                || cuts[0].len() != HPU_NATIVE_PC0_DATA_WORDS
                || cuts[1].len() != HPU_NATIVE_PC1_DATA_WORDS
            {
                return Err(format!(
                    "unsupported HPU-native result geometry at item={item_index} block={block_index}"
                ));
            }
            words.extend_from_slice(&cuts[0]);
            words.resize(words.len() + HPU_NATIVE_PC_SLOT_WORDS - cuts[0].len(), 0);
            words.extend_from_slice(&cuts[1]);
            words.resize(words.len() + HPU_NATIVE_PC_SLOT_WORDS - cuts[1].len(), 0);
        }
    }
    if words.len() != metadata.ciphertext_word_count {
        return Err(format!(
            "HPU-native result word count mismatch: result={}, expected={}",
            words.len(),
            metadata.ciphertext_word_count
        ));
    }
    Ok(words)
}

pub fn radix_ciphertexts_to_words(ciphertexts: &[RadixCiphertext]) -> Vec<u64> {
    let word_count = ciphertexts
        .iter()
        .map(|ct| {
            ct.blocks()
                .iter()
                .map(|block| block.ct.as_ref().len())
                .sum::<usize>()
        })
        .sum();
    let mut words = Vec::with_capacity(word_count);
    for ciphertext in ciphertexts {
        for block in ciphertext.blocks() {
            words.extend_from_slice(block.ct.as_ref());
        }
    }
    words
}

#[cfg(test)]
mod tests {
    use super::*;
    use tfhe::core_crypto::prelude::{CiphertextModulus, CreateFrom, LweCiphertextOwned};

    #[test]
    fn native_slot_import_matches_tfhe_cpu_conversion() {
        let config = format!(
            "{}/../mockups/tfhe-hpu-mockup/params/tuniform_64b_pfail128_psi64.toml",
            env!("CARGO_MANIFEST_DIR")
        );
        let params = HpuParameters::from_toml(&config);
        let cpu_words = (0..=2048_u64).collect::<Vec<_>>();
        let cpu_lwe =
            LweCiphertextOwned::from_container(cpu_words, CiphertextModulus::new_native());
        let expected = HpuLweCiphertextOwned::create_from(cpu_lwe.as_view(), params.clone());
        let expected_cuts = expected.clone().into_container();

        let mut native_words = Vec::new();
        for _ in 0..4 {
            native_words.extend_from_slice(&expected_cuts[0]);
            native_words.resize(
                native_words.len() + HPU_NATIVE_PC_SLOT_WORDS - HPU_NATIVE_PC0_DATA_WORDS,
                0,
            );
            native_words.extend_from_slice(&expected_cuts[1]);
            native_words.resize(
                native_words.len() + HPU_NATIVE_PC_SLOT_WORDS - HPU_NATIVE_PC1_DATA_WORDS,
                0,
            );
        }
        let metadata = BatchMetadata {
            mask_dimension: 2048,
            item_count: 1,
            radix_blocks_per_item: 4,
            message_width: 2,
            carry_width: 2,
            padding_bit_width: 1,
            delta_log2: 59,
            ciphertext_word_count: native_words.len(),
        };
        let imported =
            hpu_native_words_to_lwe_ciphertexts(&metadata, &native_words, &params).unwrap();
        assert_eq!(imported.len(), 1);
        assert_eq!(imported[0], vec![expected; 4]);
        let exported = hpu_lwe_ciphertexts_to_native_words(&metadata, imported).unwrap();
        assert_eq!(exported, native_words);
    }
}
