//! Send SUDA-generated radix/LWE ciphertexts to a remote HPU server and verify the result locally.

use clap::Parser;
use std::fs;
use std::net::{SocketAddr, TcpStream};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tfhe::integer::ClientKey;
use tfhe::shortint::client_key::atomic_pattern::{
    AtomicPatternClientKey, KS32AtomicPatternClientKey,
};
use tfhe::shortint::client_key::GenericClientKey;
use tfhe::shortint::{ClientKey as ShortintClientKey, ShortintParameterSet};

#[path = "lwe_remote/bridge.rs"]
mod bridge;
#[path = "lwe_remote/dump.rs"]
mod dump;
#[path = "lwe_remote/protocol.rs"]
mod protocol;

use dump::{read_lwehls01, write_lwehls01, LweHlsDump};
use protocol::{read_frame, write_ciphertext_frame, FRAME_ERROR, FRAME_RESPONSE, OP_ADD_SCALAR_U8};

const DEFAULT_RESULT_FILE: &str = "lwe_encrypt_remote_hpu_result.bin";

type KS32ClientKey = GenericClientKey<KS32AtomicPatternClientKey>;

#[derive(Parser, Debug)]
#[command(
    long_about = "Send an LWEHLS01 u8 radix ciphertext batch to a remote HPU server, execute ADDS, receive the ciphertext result, and decrypt it locally."
)]
struct Args {
    /// Remote HPU service address.
    #[arg(long, default_value = "127.0.0.1:19090")]
    server: SocketAddr,

    /// LWEHLS01 dump produced by vscode-lwe-encrypt-offload.
    #[arg(long)]
    ciphertext_dump: PathBuf,

    /// ClientKey stays on this machine and is used only for returned-ciphertext verification.
    #[arg(long)]
    client_key: PathBuf,

    /// Result LWEHLS01 dump written after the remote HPU operation.
    #[arg(long, default_value = DEFAULT_RESULT_FILE)]
    output: PathBuf,

    /// Clear scalar for the remote HPU ADDS operation.
    #[arg(long, default_value_t = 1)]
    scalar: u8,

    #[arg(long, default_value_t = 10_000)]
    connect_timeout_ms: u64,

    #[arg(long, default_value_t = 300)]
    io_timeout_secs: u64,

    /// Hard limit for a response payload.
    #[arg(long, default_value_t = 512 * 1024 * 1024)]
    max_response_bytes: usize,

    /// Receive and save the result without loading ClientKey or decrypting it.
    #[arg(long)]
    skip_decrypt_check: bool,

    /// Validate the local dump and ClientKey without opening a network connection.
    #[arg(long)]
    dry_run: bool,
}

fn main() -> ExitCode {
    match run(Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("remote HPU client failed: {err}");
            ExitCode::from(1)
        }
    }
}

fn run(args: Args) -> Result<(), String> {
    let source_dump = read_lwehls01(&args.ciphertext_dump)?;
    println!("source_dump={}", args.ciphertext_dump.display());
    println!(
        "send_shape=u8_count:{} radix_blocks:{} big_lwe_dimension:{} ciphertext_bytes:{}",
        source_dump.metadata.item_count,
        source_dump.metadata.radix_blocks_per_item,
        source_dump.metadata.mask_dimension,
        source_dump.ciphertext_words.len() * 8
    );
    println!("clear_reference_transmitted=no");

    if args.dry_run {
        verify_result(&args.client_key, &source_dump, &source_dump.clear_inputs)?;
        println!("dry_run_network_connection=no");
        println!("local_lwehls01_compatibility=passed");
        return Ok(());
    }

    let request_id = request_id();
    let connect_start = Instant::now();
    let mut stream =
        TcpStream::connect_timeout(&args.server, Duration::from_millis(args.connect_timeout_ms))
            .map_err(|err| format!("unable to connect to {}: {err}", args.server))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(args.io_timeout_secs)))
        .map_err(|err| format!("unable to set read timeout: {err}"))?;
    stream
        .set_write_timeout(Some(Duration::from_secs(args.io_timeout_secs)))
        .map_err(|err| format!("unable to set write timeout: {err}"))?;
    println!(
        "connected_server={} connect_ms={:.3} request_id={request_id}",
        args.server,
        connect_start.elapsed().as_secs_f64() * 1000.0
    );

    let rpc_start = Instant::now();
    write_ciphertext_frame(
        &mut stream,
        protocol::FRAME_REQUEST,
        request_id,
        OP_ADD_SCALAR_U8,
        u64::from(args.scalar),
        &source_dump.metadata,
        &source_dump.ciphertext_words,
    )?;
    let response = read_frame(&mut stream, args.max_response_bytes)?;
    let rpc_ms = rpc_start.elapsed().as_secs_f64() * 1000.0;

    if response.kind == FRAME_ERROR {
        return Err(format!(
            "remote request {request_id} failed: {}",
            response
                .error_message
                .unwrap_or_else(|| "unspecified remote error".to_string())
        ));
    }
    if response.kind != FRAME_RESPONSE {
        return Err(format!("unexpected response frame kind {}", response.kind));
    }
    if response.request_id != request_id {
        return Err(format!(
            "response request_id mismatch: response={}, request={request_id}",
            response.request_id
        ));
    }
    if response.operation != OP_ADD_SCALAR_U8
        || response.scalar != u64::from(args.scalar)
        || response.status != 0
    {
        return Err("response operation/scalar/status mismatch".to_string());
    }
    if response.metadata != source_dump.metadata {
        return Err(format!(
            "response ciphertext metadata changed: response={:?}, request={:?}",
            response.metadata, source_dump.metadata
        ));
    }

    let expected_clear: Vec<u8> = source_dump
        .clear_inputs
        .iter()
        .map(|value| value.wrapping_add(args.scalar))
        .collect();
    let result_dump = LweHlsDump {
        clear_inputs: expected_clear.clone(),
        metadata: response.metadata,
        ciphertext_words: response.ciphertext_words,
    };
    write_lwehls01(&args.output, &result_dump)?;

    if !args.skip_decrypt_check {
        verify_result(&args.client_key, &result_dump, &expected_clear)?;
    }

    println!("remote_hpu_operation=ADDS");
    println!("scalar={}", args.scalar);
    println!(
        "result_ciphertext_bytes={}",
        result_dump.ciphertext_words.len() * 8
    );
    println!("rpc_round_trip_ms={rpc_ms:.3}");
    println!("result_dump={}", args.output.display());
    println!(
        "local_client_key_decrypt_checked={}",
        if args.skip_decrypt_check { "no" } else { "yes" }
    );
    println!("remote_hpu_ciphertext_compute=passed");
    Ok(())
}

fn verify_result(
    client_key_path: &PathBuf,
    result_dump: &LweHlsDump,
    expected_clear: &[u8],
) -> Result<(), String> {
    let serialized_key = fs::read(client_key_path)
        .map_err(|err| format!("unable to read {client_key_path:?}: {err}"))?;
    let ks32_client_key: KS32ClientKey = bincode::deserialize(&serialized_key)
        .map_err(|err| format!("unable to deserialize {client_key_path:?}: {err}"))?;
    let params: ShortintParameterSet = ks32_client_key.parameters();
    bridge::validate_metadata(&result_dump.metadata, &params)?;

    let shortint_client_key = ShortintClientKey {
        atomic_pattern: AtomicPatternClientKey::KeySwitch32(ks32_client_key.atomic_pattern),
    };
    let client_key = ClientKey::from_raw_parts(shortint_client_key);
    let ciphertexts = bridge::words_to_radix_ciphertexts(
        &result_dump.metadata,
        &result_dump.ciphertext_words,
        &params,
    )?;
    for (index, (ciphertext, expected)) in ciphertexts.iter().zip(expected_clear).enumerate() {
        let decrypted: u8 = client_key.decrypt_radix(ciphertext);
        if decrypted != *expected {
            return Err(format!(
                "remote result mismatch at item {index}: decrypted={decrypted}, expected={expected}"
            ));
        }
    }
    println!("decrypted_count={}", ciphertexts.len());
    println!(
        "decrypted_prefix={}",
        expected_clear
            .iter()
            .take(16)
            .map(|value| format!("{value:02x}"))
            .collect::<String>()
    );
    Ok(())
}

fn request_id() -> u64 {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    (nanos as u64) ^ ((nanos >> 64) as u64)
}
