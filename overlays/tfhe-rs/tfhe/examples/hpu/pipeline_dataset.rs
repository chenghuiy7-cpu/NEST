//! Dataset generator for hpu_pipeline_bench.
//!
//! It writes:
//! - a visible decimal plaintext file with one "lhs rhs" pair per line;
//! - a serialized CPU RadixCiphertext dataset file;
//! - the matching integer ClientKey and CompressedServerKey.

use clap::Parser;
use rand::rngs::StdRng;
use rand::{Rng, SeedableRng};
use serde::Serialize;
use std::fs::{self, File};
use std::io::Write;
use std::panic;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};
use tfhe::integer::{ClientKey, CompressedServerKey, RadixCiphertext};
use tfhe::shortint::parameters::KeySwitch32PBSParameters;
use tfhe_hpu_backend::prelude::*;

const CIPHERTEXT_MAGIC: &[u8; 8] = b"HPUCTX1\0";
const FORMAT_VERSION: u32 = 1;
const DEFAULT_PARAMS: &str =
    "${HPU_BACKEND_DIR}/../../mockups/tfhe-hpu-mockup/params/tuniform_64b_pfail128_psi64.toml";

#[derive(Parser, Debug, Clone, Serialize)]
#[command(
    long_about = "Generate visible plaintext and serialized ciphertext files for hpu_pipeline_bench."
)]
struct Args {
    /// HPU TOML top-level configuration file. Used to derive the TFHE parameters.
    #[arg(
        long,
        default_value = "${HPU_BACKEND_DIR}/config_store/${HPU_CONFIG}/hpu_config.toml"
    )]
    config: ShellString,

    /// Offline HPU RTL parameters TOML. This is used to derive the TFHE parameters without
    /// opening the V80 device or triggering the backend fresh-reload path.
    #[arg(long, default_value = DEFAULT_PARAMS)]
    params: ShellString,

    /// Visible decimal cleartext output path. One "lhs rhs" pair per line.
    #[arg(long)]
    plaintext_output: PathBuf,

    /// Optional fixed-width raw binary cleartext output path. The file contains lhs then rhs for
    /// each record, little-endian, using ceil(integer_width / 8) bytes per operand.
    #[arg(long)]
    plaintext_binary_output: Option<PathBuf>,

    /// Serialized CPU RadixCiphertext output path.
    #[arg(long)]
    ciphertext_output: PathBuf,

    /// Serialized integer ClientKey output path.
    #[arg(long)]
    client_key_output: PathBuf,

    /// Serialized integer CompressedServerKey output path.
    #[arg(long)]
    server_key_output: PathBuf,

    /// Number of binary operation records to generate.
    #[arg(long, default_value_t = 1024)]
    dataset_size: usize,

    /// Logical fixed-width binary plaintext payload size in bytes. When set, this overrides
    /// --dataset-size. The visible decimal plaintext file is still text and may have a different
    /// byte size.
    #[arg(long)]
    plaintext_bytes: Option<usize>,

    /// Unsigned integer width. Must be supported by the HPU firmware config.
    #[arg(long, default_value_t = 64)]
    integer_width: usize,

    /// Deterministic plaintext generator seed.
    #[arg(long, default_value_t = 0x5eed_u64)]
    seed: u64,

    /// Overwrite existing output files.
    #[arg(long)]
    force: bool,

    /// Call fsync on generated files.
    #[arg(long)]
    sync_write: bool,
}

#[derive(Clone, Copy, Debug)]
struct Record {
    lhs: u128,
    rhs: u128,
}

#[derive(Serialize)]
struct CiphertextPair {
    lhs: RadixCiphertext,
    rhs: RadixCiphertext,
}

fn main() -> ExitCode {
    let args = Args::parse();
    let run_result = panic::catch_unwind({
        let args = args.clone();
        move || run(args)
    });

    match run_result {
        Ok(Ok(())) => ExitCode::SUCCESS,
        Ok(Err(err)) => {
            eprintln!("dataset generation blocked: {err}");
            ExitCode::from(1)
        }
        Err(payload) => {
            let err = panic_payload_to_string(payload);
            eprintln!("dataset generation blocked by panic: {err}");
            ExitCode::from(2)
        }
    }
}

fn run(args: Args) -> Result<(), String> {
    validate_args(&args)?;
    ensure_outputs_can_be_written(&args)?;

    let config_path = args.config.expand();
    let params_path = args.params.expand();
    let hpu_config = HpuConfig::from_toml(&config_path);
    let hpu_params = HpuParameters::from_toml(&params_path);

    let message_width = hpu_params.pbs_params.message_width;
    if args.integer_width % message_width != 0 {
        return Err(format!(
            "--integer-width {} is not divisible by HPU message width {}",
            args.integer_width, message_width
        ));
    }
    if !hpu_config.firmware.integer_w.contains(&args.integer_width) {
        return Err(format!(
            "--integer-width {} is not enabled in HPU firmware config {:?}",
            args.integer_width, hpu_config.firmware.integer_w
        ));
    }
    let num_blocks = args.integer_width / message_width;

    let resolved_dataset = resolve_dataset_shape(&args)?;
    let records = generate_records(resolved_dataset.records, args.integer_width, args.seed);
    write_plaintext(&args.plaintext_output, &records, args.sync_write)?;
    if let Some(path) = args.plaintext_binary_output.as_ref() {
        write_plaintext_binary(
            path,
            &records,
            resolved_dataset.operand_bytes,
            args.sync_write,
        )?;
    }

    let cks = ClientKey::new(KeySwitch32PBSParameters::from(&hpu_params));
    let sks_compressed = CompressedServerKey::new_radix_compressed_server_key(&cks);
    write_bincode(&args.client_key_output, &cks, args.sync_write)?;
    write_bincode(&args.server_key_output, &sks_compressed, args.sync_write)?;
    write_ciphertext_dataset(
        &args.ciphertext_output,
        &records,
        &cks,
        num_blocks,
        args.integer_width,
        args.seed,
        args.sync_write,
    )?;

    println!("generated_at_unix_ms={}", unix_ms());
    println!("records={}", records.len());
    println!(
        "logical_plaintext_bytes={}",
        resolved_dataset.logical_plaintext_bytes
    );
    println!("logical_operands={}", resolved_dataset.operands);
    println!("operand_bytes={}", resolved_dataset.operand_bytes);
    println!("integer_width={}", args.integer_width);
    println!("config_file={config_path}");
    println!("params_file={params_path}");
    println!("plaintext_file={}", args.plaintext_output.display());
    println!(
        "plaintext_bytes={}",
        file_len(&args.plaintext_output).unwrap_or(0)
    );
    if let Some(path) = args.plaintext_binary_output.as_ref() {
        println!("plaintext_binary_file={}", path.display());
        println!("plaintext_binary_bytes={}", file_len(path).unwrap_or(0));
    }
    println!("ciphertext_file={}", args.ciphertext_output.display());
    println!(
        "ciphertext_bytes={}",
        file_len(&args.ciphertext_output).unwrap_or(0)
    );
    println!("client_key_file={}", args.client_key_output.display());
    println!(
        "client_key_bytes={}",
        file_len(&args.client_key_output).unwrap_or(0)
    );
    println!("server_key_file={}", args.server_key_output.display());
    println!(
        "server_key_bytes={}",
        file_len(&args.server_key_output).unwrap_or(0)
    );
    Ok(())
}

fn validate_args(args: &Args) -> Result<(), String> {
    if args.dataset_size == 0 {
        return Err("--dataset-size must be greater than 0".to_string());
    }
    if matches!(args.plaintext_bytes, Some(0)) {
        return Err("--plaintext-bytes must be greater than 0".to_string());
    }
    if args.integer_width == 0 || args.integer_width > 128 {
        return Err("--integer-width must be in 1..=128".to_string());
    }
    Ok(())
}

#[derive(Clone, Copy, Debug)]
struct DatasetShape {
    records: usize,
    operands: usize,
    operand_bytes: usize,
    logical_plaintext_bytes: usize,
}

fn resolve_dataset_shape(args: &Args) -> Result<DatasetShape, String> {
    let operand_bytes = args.integer_width.div_ceil(8);
    if let Some(plaintext_bytes) = args.plaintext_bytes {
        let record_bytes = operand_bytes * 2;
        if plaintext_bytes % record_bytes != 0 {
            return Err(format!(
                "--plaintext-bytes {plaintext_bytes} is not divisible by one lhs/rhs record ({record_bytes} bytes for {}-bit integers)",
                args.integer_width
            ));
        }
        let records = plaintext_bytes / record_bytes;
        if records == 0 {
            return Err(format!(
                "--plaintext-bytes {plaintext_bytes} is too small for one lhs/rhs record ({record_bytes} bytes)"
            ));
        }
        Ok(DatasetShape {
            records,
            operands: records * 2,
            operand_bytes,
            logical_plaintext_bytes: plaintext_bytes,
        })
    } else {
        Ok(DatasetShape {
            records: args.dataset_size,
            operands: args.dataset_size * 2,
            operand_bytes,
            logical_plaintext_bytes: args.dataset_size * 2 * operand_bytes,
        })
    }
}

fn ensure_outputs_can_be_written(args: &Args) -> Result<(), String> {
    for path in [
        &args.plaintext_output,
        &args.ciphertext_output,
        &args.client_key_output,
        &args.server_key_output,
    ] {
        ensure_output_can_be_written(path, args.force)?;
    }
    if let Some(path) = args.plaintext_binary_output.as_ref() {
        ensure_output_can_be_written(path, args.force)?;
    }
    Ok(())
}

fn ensure_output_can_be_written(path: &Path, force: bool) -> Result<(), String> {
    if path.exists() && !force {
        return Err(format!(
            "output {path:?} already exists; pass --force to overwrite it"
        ));
    }
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| format!("unable to create output parent {parent:?}: {err}"))?;
    }
    Ok(())
}

fn generate_records(count: usize, integer_width: usize, seed: u64) -> Vec<Record> {
    let mask = width_mask(integer_width);
    let mut rng = StdRng::seed_from_u64(seed);
    (0..count)
        .map(|_| Record {
            lhs: random_u128(&mut rng) & mask,
            rhs: random_u128(&mut rng) & mask,
        })
        .collect()
}

fn write_plaintext(path: &Path, records: &[Record], sync: bool) -> Result<(), String> {
    let mut text = String::new();
    for record in records {
        use std::fmt::Write as _;
        writeln!(&mut text, "{} {}", record.lhs, record.rhs).unwrap();
    }
    write_all(path, text.as_bytes(), sync)
}

fn write_plaintext_binary(
    path: &Path,
    records: &[Record],
    operand_bytes: usize,
    sync: bool,
) -> Result<(), String> {
    let mut bytes = Vec::with_capacity(records.len() * 2 * operand_bytes);
    for record in records {
        bytes.extend_from_slice(&record.lhs.to_le_bytes()[..operand_bytes]);
        bytes.extend_from_slice(&record.rhs.to_le_bytes()[..operand_bytes]);
    }
    write_all(path, &bytes, sync)
}

fn write_ciphertext_dataset(
    path: &Path,
    records: &[Record],
    cks: &ClientKey,
    num_blocks: usize,
    integer_width: usize,
    seed: u64,
    sync: bool,
) -> Result<(), String> {
    let mut file = File::create(path).map_err(|err| format!("unable to create {path:?}: {err}"))?;
    file.write_all(CIPHERTEXT_MAGIC)
        .map_err(|err| format!("unable to write ciphertext header to {path:?}: {err}"))?;
    file.write_all(&FORMAT_VERSION.to_le_bytes())
        .map_err(|err| format!("unable to write ciphertext version to {path:?}: {err}"))?;
    file.write_all(&(integer_width as u32).to_le_bytes())
        .map_err(|err| format!("unable to write ciphertext width to {path:?}: {err}"))?;
    file.write_all(&(records.len() as u64).to_le_bytes())
        .map_err(|err| format!("unable to write ciphertext count to {path:?}: {err}"))?;
    file.write_all(&(seed as u128).to_le_bytes())
        .map_err(|err| format!("unable to write ciphertext seed to {path:?}: {err}"))?;

    for record in records {
        let pair = CiphertextPair {
            lhs: cks.encrypt_radix(record.lhs, num_blocks),
            rhs: cks.encrypt_radix(record.rhs, num_blocks),
        };
        let payload = bincode::serialize(&pair)
            .map_err(|err| format!("unable to serialize ciphertext record: {err}"))?;
        file.write_all(&(payload.len() as u64).to_le_bytes())
            .map_err(|err| {
                format!("unable to write ciphertext record length to {path:?}: {err}")
            })?;
        file.write_all(&payload)
            .map_err(|err| format!("unable to write ciphertext record to {path:?}: {err}"))?;
    }
    if sync {
        file.sync_all()
            .map_err(|err| format!("unable to fsync {path:?}: {err}"))?;
    }
    Ok(())
}

fn write_bincode<T: Serialize>(path: &Path, value: &T, sync: bool) -> Result<(), String> {
    let bytes = bincode::serialize(value)
        .map_err(|err| format!("unable to serialize bincode {path:?}: {err}"))?;
    write_all(path, &bytes, sync)
}

fn write_all(path: &Path, bytes: &[u8], sync: bool) -> Result<(), String> {
    let mut file = File::create(path).map_err(|err| format!("unable to create {path:?}: {err}"))?;
    file.write_all(bytes)
        .map_err(|err| format!("unable to write {path:?}: {err}"))?;
    if sync {
        file.sync_all()
            .map_err(|err| format!("unable to fsync {path:?}: {err}"))?;
    }
    Ok(())
}

fn file_len(path: &Path) -> Option<u64> {
    fs::metadata(path).ok().map(|metadata| metadata.len())
}

fn width_mask(width: usize) -> u128 {
    if width == 128 {
        u128::MAX
    } else {
        (1_u128 << width) - 1
    }
}

fn random_u128(rng: &mut StdRng) -> u128 {
    ((rng.gen::<u64>() as u128) << 64) | (rng.gen::<u64>() as u128)
}

fn unix_ms() -> u128 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
}

fn panic_payload_to_string(payload: Box<dyn std::any::Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        "non-string panic payload".to_string()
    }
}
