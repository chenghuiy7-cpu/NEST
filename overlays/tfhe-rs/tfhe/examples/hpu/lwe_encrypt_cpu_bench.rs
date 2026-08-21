//! Benchmark the tfhe-rs software path equivalent to the SUDA u8 radix LWE operator.

use clap::{Parser, ValueEnum};
use rayon::prelude::*;
use std::fs::{self, File};
use std::hint::black_box;
use std::io::{BufWriter, Write};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Instant;
use tfhe::integer::{ClientKey, IntegerCiphertext, RadixCiphertext};
use tfhe::shortint::client_key::atomic_pattern::{
    AtomicPatternClientKey, KS32AtomicPatternClientKey,
};
use tfhe::shortint::client_key::GenericClientKey;
use tfhe::shortint::{ClientKey as ShortintClientKey, ShortintParameterSet};

const RADIX_BLOCKS: usize = 4;
const EXPECTED_MESSAGE_MODULUS: u64 = 4;
const EXPECTED_CARRY_MODULUS: u64 = 4;
const EXPECTED_BIG_LWE_DIMENSION: usize = 2048;
const LOGICAL_CIPHERTEXT_BYTES_PER_U8: usize =
    RADIX_BLOCKS * (EXPECTED_BIG_LWE_DIMENSION + 1) * size_of::<u64>();
const HPU_PC_COUNT: usize = 2;
const HPU_PC_GROUP_WORDS: usize = 16;
const HPU_PC_DATA_WORDS: usize = EXPECTED_BIG_LWE_DIMENSION / HPU_PC_COUNT;
const HPU_PC0_DATA_WORDS: usize = HPU_PC_DATA_WORDS + 1;
const HPU_PC_SLOT_BYTES: usize = 3 * 4096;
const HPU_PC_SLOT_WORDS: usize = HPU_PC_SLOT_BYTES / size_of::<u64>();
const HPU_NATIVE_LWE_WORDS: usize = HPU_PC_COUNT * HPU_PC_SLOT_WORDS;
const HPU_NATIVE_WORDS_PER_U8: usize = RADIX_BLOCKS * HPU_NATIVE_LWE_WORDS;
const HPU_NATIVE_BYTES_PER_U8: usize = HPU_NATIVE_WORDS_PER_U8 * size_of::<u64>();

type KS32ClientKey = GenericClientKey<KS32AtomicPatternClientKey>;

#[derive(Clone, Copy, Debug, ValueEnum)]
enum Mode {
    /// One CPU thread encrypts the u8 values consecutively.
    Serial,
    /// Rayon distributes independent u8 encryptions over its worker threads.
    Parallel,
}

impl Mode {
    fn as_str(self) -> &'static str {
        match self {
            Self::Serial => "serial",
            Self::Parallel => "parallel",
        }
    }
}

#[derive(Parser, Debug)]
#[command(about = "Benchmark tfhe-rs ClientKey::encrypt_radix::<u8>(value, 4)")]
struct Args {
    /// Saved KS32 ClientKey matching the psi64 HPU configuration.
    #[arg(long)]
    client_key: PathBuf,

    /// Comma-separated u8 batch sizes.
    #[arg(long, default_value = "1,2,4,8,16,32,64,128")]
    batch_sizes: String,

    /// Optional plaintext file; each batch uses its first N bytes.
    #[arg(long)]
    input_file: Option<PathBuf>,

    /// Untimed warm-up rounds for every batch size.
    #[arg(long, default_value_t = 3)]
    warmup: usize,

    /// Timed rounds for every batch size.
    #[arg(long, default_value_t = 20)]
    iterations: usize,

    /// Serial is the primary one-core comparison; parallel measures host throughput.
    #[arg(long, value_enum, default_value_t = Mode::Serial)]
    mode: Mode,

    /// Optional pure CSV output. Human-readable summaries still go to stdout.
    #[arg(long)]
    csv: Option<PathBuf>,
}

#[derive(Debug)]
struct Sample {
    batch_size: usize,
    iteration: usize,
    encrypt_ms: f64,
    native_pack_ms: f64,
    encrypt_and_pack_ms: f64,
}

fn main() -> ExitCode {
    match run(Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("CPU encryption benchmark failed: {err}");
            ExitCode::from(1)
        }
    }
}

fn run(args: Args) -> Result<(), String> {
    if args.iterations == 0 {
        return Err("--iterations must be greater than zero".to_string());
    }
    let batch_sizes = parse_batch_sizes(&args.batch_sizes)?;
    let max_batch_size = *batch_sizes.iter().max().expect("non-empty batch sizes");
    let input_values = if let Some(path) = &args.input_file {
        let values = fs::read(path)
            .map_err(|err| format!("unable to read plaintext {}: {err}", path.display()))?;
        if values.len() < max_batch_size {
            return Err(format!(
                "plaintext {} has {} bytes, but the largest batch needs {max_batch_size}",
                path.display(),
                values.len()
            ));
        }
        Some(values)
    } else {
        None
    };

    let key_load_start = Instant::now();
    let serialized_key = fs::read(&args.client_key)
        .map_err(|err| format!("unable to read {}: {err}", args.client_key.display()))?;
    let ks32_client_key: KS32ClientKey = bincode::deserialize(&serialized_key)
        .map_err(|err| format!("unable to deserialize {}: {err}", args.client_key.display()))?;
    let params: ShortintParameterSet = ks32_client_key.parameters();
    validate_parameters(&params)?;
    let shortint_client_key = ShortintClientKey {
        atomic_pattern: AtomicPatternClientKey::KeySwitch32(ks32_client_key.atomic_pattern),
    };
    let client_key = ClientKey::from_raw_parts(shortint_client_key);
    let key_load_ms = key_load_start.elapsed().as_secs_f64() * 1000.0;

    println!("benchmark=tfhe-rs ClientKey::encrypt_radix::<u8>(value, 4) + psi64 HPU-native pack");
    println!("client_key={}", args.client_key.display());
    println!(
        "parameters=lwe_dimension:{} glwe_dimension:{} polynomial_size:{} big_lwe_dimension:{} message_modulus:{} carry_modulus:{}",
        params.lwe_dimension().0,
        params.glwe_dimension().0,
        params.polynomial_size().0,
        params.glwe_dimension().0 * params.polynomial_size().0,
        params.message_modulus().0,
        params.carry_modulus().0
    );
    println!(
        "mode={} warmup={} iterations={} key_load_ms={key_load_ms:.3}",
        args.mode.as_str(),
        args.warmup,
        args.iterations
    );
    println!("key_load_is_timed=no");
    match &args.input_file {
        Some(path) => println!("input_source=file:{}", path.display()),
        None => println!("input_source=deterministic-generated"),
    }
    println!(
        "output_layout=hpu-native-psi64-v80 logical_bytes_per_u8={} physical_bytes_per_u8={}",
        LOGICAL_CIPHERTEXT_BYTES_PER_U8, HPU_NATIVE_BYTES_PER_U8
    );

    let mut samples = Vec::with_capacity(batch_sizes.len() * args.iterations);
    for &batch_size in &batch_sizes {
        let clear_values = input_values
            .as_ref()
            .map(|values| values[..batch_size].to_vec())
            .unwrap_or_else(|| deterministic_values(batch_size));
        for _ in 0..args.warmup {
            let ciphertexts = encrypt_batch(&client_key, &clear_values, args.mode);
            let mut native_words = vec![0; batch_size * HPU_NATIVE_WORDS_PER_U8];
            pack_batch_hpu_native(&ciphertexts, args.mode, &mut native_words);
            black_box(native_words);
        }

        let mut last_ciphertexts = None;
        let mut last_native_words = None;
        for iteration in 0..args.iterations {
            // Match FPGA execute, whose output SLM is allocated before timing.
            let mut native_words = vec![0; batch_size * HPU_NATIVE_WORDS_PER_U8];
            let total_start = Instant::now();
            let ciphertexts = encrypt_batch(&client_key, &clear_values, args.mode);
            let encrypt_ms = total_start.elapsed().as_secs_f64() * 1000.0;
            let pack_start = Instant::now();
            pack_batch_hpu_native(&ciphertexts, args.mode, &mut native_words);
            let native_pack_ms = pack_start.elapsed().as_secs_f64() * 1000.0;
            let encrypt_and_pack_ms = total_start.elapsed().as_secs_f64() * 1000.0;
            black_box(&ciphertexts);
            black_box(&native_words);
            if iteration + 1 == args.iterations {
                last_ciphertexts = Some(ciphertexts);
                last_native_words = Some(native_words);
            }
            samples.push(Sample {
                batch_size,
                iteration,
                encrypt_ms,
                native_pack_ms,
                encrypt_and_pack_ms,
            });
        }

        let last_ciphertexts = last_ciphertexts.expect("at least one measured iteration");
        let last_native_words = last_native_words.expect("at least one measured iteration");
        verify_hpu_native_batch(&last_ciphertexts, &last_native_words)?;
        for (index, (ciphertext, expected)) in
            last_ciphertexts.iter().zip(&clear_values).enumerate()
        {
            let decrypted: u8 = client_key.decrypt_radix(ciphertext);
            if decrypted != *expected {
                return Err(format!(
                    "correctness check failed at batch {batch_size}, item {index}: decrypted={decrypted}, expected={expected}"
                ));
            }
        }

        let batch_samples: Vec<&Sample> = samples
            .iter()
            .filter(|sample| sample.batch_size == batch_size)
            .collect();
        let encrypt_values: Vec<f64> = batch_samples
            .iter()
            .map(|sample| sample.encrypt_ms)
            .collect();
        let pack_values: Vec<f64> = batch_samples
            .iter()
            .map(|sample| sample.native_pack_ms)
            .collect();
        let total_values: Vec<f64> = batch_samples
            .iter()
            .map(|sample| sample.encrypt_and_pack_ms)
            .collect();
        let encrypt_median_ms = median(&encrypt_values);
        let pack_median_ms = median(&pack_values);
        let total_median_ms = median(&total_values);
        println!(
            "CPU_SUMMARY mode={} batch_size={} encrypt_median_ms={encrypt_median_ms:.6} native_pack_median_ms={pack_median_ms:.6} encrypt_and_pack_median_ms={total_median_ms:.6} encrypt_and_pack_p95_ms={:.6} median_per_u8_us={:.3} plaintext_Bps={:.3}",
            args.mode.as_str(),
            batch_size,
            percentile(&total_values, 0.95),
            total_median_ms * 1000.0 / batch_size as f64,
            batch_size as f64 * 1000.0 / total_median_ms
        );
    }

    if let Some(csv_path) = &args.csv {
        write_csv(csv_path, args.mode, &samples)?;
        println!("csv={}", csv_path.display());
    }
    println!("software_encrypt_decrypt_check=passed");
    println!("hpu_native_pack_check=passed");
    Ok(())
}

fn parse_batch_sizes(text: &str) -> Result<Vec<usize>, String> {
    let mut values = Vec::new();
    for part in text.split(',') {
        let value = part
            .trim()
            .parse::<usize>()
            .map_err(|err| format!("invalid batch size {part:?}: {err}"))?;
        if value == 0 {
            return Err("batch sizes must be greater than zero".to_string());
        }
        if !values.contains(&value) {
            values.push(value);
        }
    }
    if values.is_empty() {
        return Err("--batch-sizes cannot be empty".to_string());
    }
    Ok(values)
}

fn validate_parameters(params: &ShortintParameterSet) -> Result<(), String> {
    let big_lwe_dimension = params.glwe_dimension().0 * params.polynomial_size().0;
    if params.message_modulus().0 != EXPECTED_MESSAGE_MODULUS
        || params.carry_modulus().0 != EXPECTED_CARRY_MODULUS
        || big_lwe_dimension != EXPECTED_BIG_LWE_DIMENSION
    {
        return Err(format!(
            "ClientKey does not match the HLS shape: message_modulus={}, carry_modulus={}, big_lwe_dimension={big_lwe_dimension}",
            params.message_modulus().0,
            params.carry_modulus().0
        ));
    }
    Ok(())
}

fn deterministic_values(batch_size: usize) -> Vec<u8> {
    (0..batch_size)
        .map(|index| (59_u64.wrapping_add(index as u64 * 73) & 0xff) as u8)
        .collect()
}

fn encrypt_batch(client_key: &ClientKey, clear_values: &[u8], mode: Mode) -> Vec<RadixCiphertext> {
    match mode {
        Mode::Serial => clear_values
            .iter()
            .map(|&value| client_key.encrypt_radix(value, RADIX_BLOCKS))
            .collect(),
        Mode::Parallel => clear_values
            .par_iter()
            .map(|&value| client_key.encrypt_radix(value, RADIX_BLOCKS))
            .collect(),
    }
}

fn reverse_psi64_mask_index(index: usize) -> usize {
    let mut value = index;
    let mut reversed = 0;
    for _ in 0..11 {
        reversed = (reversed << 1) | (value & 1);
        value >>= 1;
    }
    reversed
}

fn pack_lwe_hpu_native(logical_words: &[u64], native_words: &mut [u64]) {
    assert_eq!(logical_words.len(), EXPECTED_BIG_LWE_DIMENSION + 1);
    assert_eq!(native_words.len(), HPU_NATIVE_LWE_WORDS);
    native_words.fill(0);

    for (natural_index, &word) in logical_words[..EXPECTED_BIG_LWE_DIMENSION]
        .iter()
        .enumerate()
    {
        let hpu_index = reverse_psi64_mask_index(natural_index);
        let group = hpu_index / HPU_PC_GROUP_WORDS;
        let lane = hpu_index % HPU_PC_GROUP_WORDS;
        let pc = group % HPU_PC_COUNT;
        let pc_offset = (group / HPU_PC_COUNT) * HPU_PC_GROUP_WORDS + lane;
        native_words[pc * HPU_PC_SLOT_WORDS + pc_offset] = word;
    }
    native_words[HPU_PC_DATA_WORDS] = logical_words[EXPECTED_BIG_LWE_DIMENSION];
}

fn pack_radix_hpu_native(ciphertext: &RadixCiphertext, native_words: &mut [u64]) {
    assert_eq!(ciphertext.blocks().len(), RADIX_BLOCKS);
    assert_eq!(native_words.len(), HPU_NATIVE_WORDS_PER_U8);
    for (block, output) in ciphertext
        .blocks()
        .iter()
        .zip(native_words.chunks_exact_mut(HPU_NATIVE_LWE_WORDS))
    {
        pack_lwe_hpu_native(block.ct.as_ref(), output);
    }
}

fn pack_batch_hpu_native(ciphertexts: &[RadixCiphertext], mode: Mode, output: &mut [u64]) {
    assert_eq!(output.len(), ciphertexts.len() * HPU_NATIVE_WORDS_PER_U8);
    match mode {
        Mode::Serial => ciphertexts
            .iter()
            .zip(output.chunks_exact_mut(HPU_NATIVE_WORDS_PER_U8))
            .for_each(|(ciphertext, native)| pack_radix_hpu_native(ciphertext, native)),
        Mode::Parallel => ciphertexts
            .par_iter()
            .zip(output.par_chunks_exact_mut(HPU_NATIVE_WORDS_PER_U8))
            .for_each(|(ciphertext, native)| pack_radix_hpu_native(ciphertext, native)),
    }
}

fn verify_hpu_native_batch(
    ciphertexts: &[RadixCiphertext],
    native_words: &[u64],
) -> Result<(), String> {
    if native_words.len() != ciphertexts.len() * HPU_NATIVE_WORDS_PER_U8 {
        return Err(format!(
            "HPU-native output length mismatch: got={}, expected={}",
            native_words.len(),
            ciphertexts.len() * HPU_NATIVE_WORDS_PER_U8
        ));
    }

    for (item_index, (ciphertext, native_item)) in ciphertexts
        .iter()
        .zip(native_words.chunks_exact(HPU_NATIVE_WORDS_PER_U8))
        .enumerate()
    {
        for (block_index, (block, native_lwe)) in ciphertext
            .blocks()
            .iter()
            .zip(native_item.chunks_exact(HPU_NATIVE_LWE_WORDS))
            .enumerate()
        {
            let logical = block.ct.as_ref();
            for natural_index in 0..EXPECTED_BIG_LWE_DIMENSION {
                let hpu_index = reverse_psi64_mask_index(natural_index);
                let group = hpu_index / HPU_PC_GROUP_WORDS;
                let lane = hpu_index % HPU_PC_GROUP_WORDS;
                let pc = group % HPU_PC_COUNT;
                let pc_offset = (group / HPU_PC_COUNT) * HPU_PC_GROUP_WORDS + lane;
                if native_lwe[pc * HPU_PC_SLOT_WORDS + pc_offset] != logical[natural_index] {
                    return Err(format!(
                        "HPU-native mask mismatch at item={item_index} block={block_index} index={natural_index}"
                    ));
                }
            }
            if native_lwe[HPU_PC_DATA_WORDS] != logical[EXPECTED_BIG_LWE_DIMENSION] {
                return Err(format!(
                    "HPU-native body mismatch at item={item_index} block={block_index}"
                ));
            }
            if native_lwe[HPU_PC0_DATA_WORDS..HPU_PC_SLOT_WORDS]
                .iter()
                .chain(native_lwe[HPU_PC_SLOT_WORDS + HPU_PC_DATA_WORDS..].iter())
                .any(|word| *word != 0)
            {
                return Err(format!(
                    "HPU-native padding is non-zero at item={item_index} block={block_index}"
                ));
            }
        }
    }
    Ok(())
}

fn percentile(values: &[f64], fraction: f64) -> f64 {
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let rank = ((sorted.len() as f64 * fraction).ceil() as usize).saturating_sub(1);
    sorted[rank.min(sorted.len() - 1)]
}

fn median(values: &[f64]) -> f64 {
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let middle = sorted.len() / 2;
    if sorted.len() % 2 == 0 {
        (sorted[middle - 1] + sorted[middle]) / 2.0
    } else {
        sorted[middle]
    }
}

fn write_csv(path: &PathBuf, mode: Mode, samples: &[Sample]) -> Result<(), String> {
    let file =
        File::create(path).map_err(|err| format!("unable to create {}: {err}", path.display()))?;
    let mut output = BufWriter::new(file);
    writeln!(
        output,
        "backend,mode,batch_size,iteration,output_layout,physical_output_bytes_per_u8,encrypt_ms,native_pack_ms,encrypt_and_pack_ms,per_u8_encrypt_us,per_u8_encrypt_and_pack_us,plaintext_bytes_per_s,native_ciphertext_bytes_per_s"
    )
    .map_err(|err| format!("unable to write {}: {err}", path.display()))?;
    for sample in samples {
        let count = sample.batch_size as f64;
        let seconds = sample.encrypt_and_pack_ms / 1000.0;
        writeln!(
            output,
            "cpu,{},{},{},hpu-native-psi64-v80,{},{:.9},{:.9},{:.9},{:.6},{:.6},{:.3},{:.3}",
            mode.as_str(),
            sample.batch_size,
            sample.iteration,
            HPU_NATIVE_BYTES_PER_U8,
            sample.encrypt_ms,
            sample.native_pack_ms,
            sample.encrypt_and_pack_ms,
            sample.encrypt_ms * 1000.0 / count,
            sample.encrypt_and_pack_ms * 1000.0 / count,
            count / seconds,
            count * HPU_NATIVE_BYTES_PER_U8 as f64 / seconds
        )
        .map_err(|err| format!("unable to write {}: {err}", path.display()))?;
    }
    output
        .flush()
        .map_err(|err| format!("unable to flush {}: {err}", path.display()))
}
