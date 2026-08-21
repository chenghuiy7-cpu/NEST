//! Import a SUDA `lwe_encrypt` dump and use it as an HPU mockup operand.

use clap::Parser;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use tfhe::core_crypto::prelude::LweCiphertextOwned;
use tfhe::integer::hpu::ciphertext::HpuRadixCiphertext;
use tfhe::integer::{ClientKey, CompressedServerKey, RadixCiphertext};
use tfhe::shortint::ciphertext::{Degree, NoiseLevel};
use tfhe::shortint::client_key::atomic_pattern::{
    AtomicPatternClientKey, KS32AtomicPatternClientKey,
};
use tfhe::shortint::client_key::GenericClientKey;
use tfhe::shortint::{Ciphertext, ClientKey as ShortintClientKey};
use tfhe_hpu_backend::prelude::*;

type KS32ClientKey = GenericClientKey<KS32AtomicPatternClientKey>;

#[derive(Parser, Debug)]
#[command(
    long_about = "Import a SUDA lwe_encrypt LWEHLS01 dump, execute scalar addition in the HPU mockup, and decrypt the result. Start hpu_mockup before this program."
)]
struct Args {
    /// HPU simulation configuration. The mockup must use the same file.
    #[arg(
        long,
        default_value = "${HPU_BACKEND_DIR}/config_store/${HPU_CONFIG}/hpu_config.toml"
    )]
    config: ShellString,

    /// Serialized shortint KS32 ClientKey used to create the HLS secret key.
    #[arg(long)]
    client_key: PathBuf,

    /// LWEHLS01 dump produced by vscode-lwe-encrypt-offload.
    #[arg(long)]
    ciphertext_dump: PathBuf,

    /// Scalar added by the HPU ADDS operation.
    #[arg(long, default_value_t = 1)]
    scalar: u8,
}

#[derive(Debug)]
struct U8RadixDump {
    mask_dimension: usize,
    inputs: Vec<u8>,
    radix_blocks_per_input: usize,
    message_width: usize,
    carry_width: usize,
    padding_bit_width: usize,
    delta_log2: usize,
    ciphertext_words: Vec<u64>,
}

fn main() -> ExitCode {
    let args = Args::parse();
    match run(args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("HPU mockup LWE import test failed: {err}");
            ExitCode::from(1)
        }
    }
}

fn run(args: Args) -> Result<(), String> {
    let dump = read_u8_radix_dump(&args.ciphertext_dump)?;
    let serialized_key = fs::read(&args.client_key)
        .map_err(|err| format!("unable to read {:?}: {err}", args.client_key))?;
    let ks32_client_key: KS32ClientKey = bincode::deserialize(&serialized_key)
        .map_err(|err| format!("unable to deserialize {:?}: {err}", args.client_key))?;
    let key_params = ks32_client_key.parameters();

    validate_dump(&dump, &key_params)?;

    let shortint_client_key = ShortintClientKey {
        atomic_pattern: AtomicPatternClientKey::KeySwitch32(ks32_client_key.atomic_pattern),
    };
    let client_key = ClientKey::from_raw_parts(shortint_client_key);
    let cpu_inputs = dump_to_radix_ciphertexts(&dump, &key_params)?;

    for (index, (cpu_ct, expected)) in cpu_inputs.iter().zip(&dump.inputs).enumerate() {
        let decrypted: u8 = client_key.decrypt_radix(cpu_ct);
        if decrypted != *expected {
            return Err(format!(
                "CPU import check failed at input {index}: got={decrypted}, expected={expected}"
            ));
        }
    }

    println!("ciphertext_dump={}", args.ciphertext_dump.display());
    println!("client_key_file={}", args.client_key.display());
    println!("imported_u8_count={}", cpu_inputs.len());
    println!("imported_mask_dimension={}", dump.mask_dimension);
    println!("cpu_radix_import_decrypt_checked=yes");

    let hpu_device = HpuDevice::from_config(&args.config.expand());
    validate_device(&dump, &key_params, &hpu_device)?;

    let compressed_server_key = CompressedServerKey::new_radix_compressed_server_key(&client_key);
    tfhe::integer::hpu::init_device(&hpu_device, compressed_server_key)
        .map_err(|err| format!("HPU init_device failed: {err:?}"))?;

    for (index, (cpu_ct, clear)) in cpu_inputs.iter().zip(&dump.inputs).enumerate() {
        let hpu_input = HpuRadixCiphertext::from_radix_ciphertext(cpu_ct, &hpu_device);
        let hpu_output = &hpu_input + u128::from(args.scalar);
        let cpu_output = hpu_output.to_radix_ciphertext();
        let decrypted: u8 = client_key.decrypt_radix(&cpu_output);
        let expected = clear.wrapping_add(args.scalar);

        println!(
            "input[{index}]={clear} scalar={} decrypted_result={decrypted} expected={expected}",
            args.scalar
        );
        if decrypted != expected {
            return Err(format!(
                "HPU ADDS mismatch at input {index}: got={decrypted}, expected={expected}"
            ));
        }
    }

    hpu_device.mem_sanitizer();
    println!("hpu_operation=ADDS");
    println!("hpu_mockup_ciphertext_compute_checked=yes");
    println!("lwe_encrypt_mockup_compatibility=passed");
    Ok(())
}

fn validate_dump(
    dump: &U8RadixDump,
    params: &tfhe::shortint::ShortintParameterSet,
) -> Result<(), String> {
    if dump.inputs.is_empty() {
        return Err("ciphertext dump contains no u8 input".to_string());
    }
    if dump.mask_dimension != params.encryption_lwe_dimension().0 {
        return Err(format!(
            "mask dimension mismatch: dump={}, client_key={}",
            dump.mask_dimension,
            params.encryption_lwe_dimension().0
        ));
    }
    if dump.message_width != params.message_modulus().0.ilog2() as usize {
        return Err(format!(
            "message width mismatch: dump={}, client_key={}",
            dump.message_width,
            params.message_modulus().0.ilog2()
        ));
    }
    if dump.carry_width != params.carry_modulus().0.ilog2() as usize {
        return Err(format!(
            "carry width mismatch: dump={}, client_key={}",
            dump.carry_width,
            params.carry_modulus().0.ilog2()
        ));
    }
    let expected_blocks = u8::BITS as usize / dump.message_width;
    if dump.radix_blocks_per_input != expected_blocks {
        return Err(format!(
            "radix block count mismatch: dump={}, expected={expected_blocks}",
            dump.radix_blocks_per_input
        ));
    }
    let expected_delta_log2 =
        u64::BITS as usize - dump.message_width - dump.carry_width - dump.padding_bit_width;
    if dump.delta_log2 != expected_delta_log2 {
        return Err(format!(
            "delta_log2 mismatch: dump={}, expected={expected_delta_log2}",
            dump.delta_log2
        ));
    }
    Ok(())
}

fn validate_device(
    dump: &U8RadixDump,
    key_params: &tfhe::shortint::ShortintParameterSet,
    device: &HpuDevice,
) -> Result<(), String> {
    let device_params = device.params();
    if dump.mask_dimension
        != device_params.pbs_params.glwe_dimension * device_params.pbs_params.polynomial_size
    {
        return Err(format!(
            "mask dimension {} does not match HPU Big-LWE dimension {}",
            dump.mask_dimension,
            device_params.pbs_params.glwe_dimension * device_params.pbs_params.polynomial_size
        ));
    }
    if key_params.lwe_dimension().0 != device_params.pbs_params.lwe_dimension
        || key_params.glwe_dimension().0 != device_params.pbs_params.glwe_dimension
        || key_params.polynomial_size().0 != device_params.pbs_params.polynomial_size
        || key_params.message_modulus().0.ilog2() as usize != device_params.pbs_params.message_width
        || key_params.carry_modulus().0.ilog2() as usize != device_params.pbs_params.carry_width
    {
        return Err("saved ClientKey parameters do not match HPU mockup parameters".to_string());
    }
    if !device
        .config()
        .firmware
        .integer_w
        .contains(&(u8::BITS as usize))
    {
        return Err(format!(
            "HPU firmware does not enable 8-bit integers: {:?}",
            device.config().firmware.integer_w
        ));
    }
    Ok(())
}

fn dump_to_radix_ciphertexts(
    dump: &U8RadixDump,
    params: &tfhe::shortint::ShortintParameterSet,
) -> Result<Vec<RadixCiphertext>, String> {
    let words_per_ciphertext = dump.mask_dimension + 1;
    let ciphertexts_per_input = dump.radix_blocks_per_input;
    let expected_words = dump.inputs.len() * ciphertexts_per_input * words_per_ciphertext;
    if dump.ciphertext_words.len() != expected_words {
        return Err(format!(
            "ciphertext word count mismatch: dump={}, expected={expected_words}",
            dump.ciphertext_words.len()
        ));
    }

    let mut radix_ciphertexts = Vec::with_capacity(dump.inputs.len());
    for input_index in 0..dump.inputs.len() {
        let mut blocks = Vec::with_capacity(ciphertexts_per_input);
        for block_index in 0..ciphertexts_per_input {
            let ciphertext_index = input_index * ciphertexts_per_input + block_index;
            let start = ciphertext_index * words_per_ciphertext;
            let end = start + words_per_ciphertext;
            let lwe = LweCiphertextOwned::from_container(
                dump.ciphertext_words[start..end].to_vec(),
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

fn read_u8_radix_dump(path: &Path) -> Result<U8RadixDump, String> {
    let bytes = fs::read(path).map_err(|err| format!("unable to read {path:?}: {err}"))?;
    let mut cursor = 0usize;

    if bytes.len() < 8 || &bytes[..8] != b"LWEHLS01" {
        return Err(format!("{path:?} is not a LWEHLS01 dump"));
    }
    cursor += 8;

    let version = read_le64(&bytes, &mut cursor)?;
    if version != 1 {
        return Err(format!("unsupported LWEHLS01 version {version}"));
    }

    let mask_dimension = read_le64(&bytes, &mut cursor)? as usize;
    let input_count = read_le64(&bytes, &mut cursor)? as usize;
    let radix_blocks_per_input = read_le64(&bytes, &mut cursor)? as usize;
    let message_width = read_le64(&bytes, &mut cursor)? as usize;
    let carry_width = read_le64(&bytes, &mut cursor)? as usize;
    let padding_bit_width = read_le64(&bytes, &mut cursor)? as usize;
    let delta_log2 = read_le64(&bytes, &mut cursor)? as usize;
    let ciphertext_word_count = read_le64(&bytes, &mut cursor)? as usize;

    let mut inputs = Vec::with_capacity(input_count);
    for _ in 0..input_count {
        let input = read_le64(&bytes, &mut cursor)?;
        if input > u8::MAX as u64 {
            return Err(format!("u8 input value out of range: {input}"));
        }
        inputs.push(input as u8);
    }

    let mut ciphertext_words = Vec::with_capacity(ciphertext_word_count);
    for _ in 0..ciphertext_word_count {
        ciphertext_words.push(read_le64(&bytes, &mut cursor)?);
    }
    if cursor != bytes.len() {
        return Err(format!(
            "ciphertext dump has {} trailing bytes",
            bytes.len() - cursor
        ));
    }

    Ok(U8RadixDump {
        mask_dimension,
        inputs,
        radix_blocks_per_input,
        message_width,
        carry_width,
        padding_bit_width,
        delta_log2,
        ciphertext_words,
    })
}

fn read_le64(bytes: &[u8], cursor: &mut usize) -> Result<u64, String> {
    let end = cursor
        .checked_add(8)
        .ok_or_else(|| "ciphertext dump offset overflow".to_string())?;
    let raw = bytes
        .get(*cursor..end)
        .ok_or_else(|| "unexpected end of ciphertext dump".to_string())?;
    *cursor = end;
    Ok(u64::from_le_bytes(raw.try_into().unwrap()))
}
