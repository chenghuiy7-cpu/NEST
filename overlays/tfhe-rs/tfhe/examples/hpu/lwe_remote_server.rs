//! TCP service that executes scalar addition over SUDA-generated ciphertexts on a real HPU.

use clap::Parser;
use std::fs;
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::process::ExitCode;
use std::time::{Duration, Instant};
use tfhe::integer::hpu::ciphertext::HpuRadixCiphertext;
use tfhe::integer::CompressedServerKey;
use tfhe::shortint::parameters::{KeySwitch32PBSParameters, ShortintParameterSet};
use tfhe_hpu_backend::prelude::*;

#[path = "lwe_remote/bridge.rs"]
mod bridge;
#[path = "lwe_remote/protocol.rs"]
mod protocol;

use protocol::{
    read_frame, write_ciphertext_frame_version, write_error_frame_version, write_server_timing,
    BatchMetadata, ServerTiming, FRAME_REQUEST, FRAME_RESPONSE, OP_ADD_SCALAR_U8,
    OP_ADD_SCALAR_U8_HPU_NATIVE, OP_ADD_SCALAR_U8_HPU_NATIVE_ROUNDTRIP, OP_ECHO_U8, VERSION_TIMING,
};

const DEFAULT_SERVER_KEY_FILE: &str = "psi64_integer_compressed_server_key.bincode";

#[derive(Parser, Debug)]
#[command(
    long_about = "Listen for LWE remote-compute requests, execute u8 ADDS on a real HPU, and return ciphertext results. This process never loads ClientKey."
)]
struct Args {
    #[arg(long, default_value = "0.0.0.0:19090")]
    bind: SocketAddr,

    #[arg(
        long,
        default_value = "${HPU_BACKEND_DIR}/config_store/${HPU_CONFIG}/hpu_config.toml"
    )]
    config: ShellString,

    /// Integer CompressedServerKey generated from the same ClientKey as the FPGA HLS secret key.
    #[arg(long, default_value = DEFAULT_SERVER_KEY_FILE)]
    server_key: PathBuf,

    #[arg(long, default_value_t = 300)]
    io_timeout_secs: u64,

    #[arg(long, default_value_t = 512 * 1024 * 1024)]
    max_request_bytes: usize,

    /// Exit after serving one connection.
    #[arg(long)]
    once: bool,
}

fn main() -> ExitCode {
    match run(Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("remote HPU server failed: {err}");
            ExitCode::from(1)
        }
    }
}

fn run(args: Args) -> Result<(), String> {
    let device_open_start = Instant::now();
    let config_path = args.config.expand();
    require_reload_disabled(&config_path)?;
    let hpu_device = HpuDevice::from_config(&config_path);
    let params = ShortintParameterSet::new_ks32_pbs_param_set(KeySwitch32PBSParameters::from(
        hpu_device.params(),
    ));
    if !hpu_device
        .config()
        .firmware
        .integer_w
        .contains(&(u8::BITS as usize))
    {
        return Err(format!(
            "HPU firmware does not enable 8-bit integers: {:?}",
            hpu_device.config().firmware.integer_w
        ));
    }

    let serialized_server_key = fs::read(&args.server_key)
        .map_err(|err| format!("unable to read {:?}: {err}", args.server_key))?;
    let compressed_server_key: CompressedServerKey =
        bincode::deserialize(&serialized_server_key)
            .map_err(|err| format!("unable to deserialize {:?}: {err}", args.server_key))?;
    tfhe::integer::hpu::init_device(&hpu_device, compressed_server_key)
        .map_err(|err| format!("HPU init_device failed: {err:?}"))?;
    println!("hpu_device_ready=yes");
    println!("server_key_file={}", args.server_key.display());
    println!("client_key_loaded=no");
    println!(
        "hpu_init_ms={:.3}",
        device_open_start.elapsed().as_secs_f64() * 1000.0
    );

    let listener = TcpListener::bind(args.bind)
        .map_err(|err| format!("unable to bind {}: {err}", args.bind))?;
    println!("listen_addr={}", args.bind);
    for connection in listener.incoming() {
        match connection {
            Ok(mut stream) => {
                let peer = stream.peer_addr().ok();
                println!("client_connected={peer:?}");
                configure_stream(&stream, args.io_timeout_secs)?;
                if let Err(err) =
                    handle_connection(&mut stream, &hpu_device, &params, args.max_request_bytes)
                {
                    eprintln!("request_failed peer={peer:?} error={err}");
                }
            }
            Err(err) => eprintln!("accept_failed={err}"),
        }
        if args.once {
            break;
        }
    }
    Ok(())
}

fn require_reload_disabled(config_path: &str) -> Result<(), String> {
    let config = HpuConfig::from_toml(config_path);
    let FFIMode::V80 { force_reload, .. } = &config.fpga.ffi else {
        return Err(format!(
            "remote HPU server requires a V80 configuration: {config_path}"
        ));
    };
    let policy = force_reload
        .as_ref()
        .map(ShellString::expand)
        .unwrap_or_else(|| "false".to_string());
    if policy != "never" {
        return Err(format!(
            "unsafe V80 reload policy {policy:?} in {config_path}; \
             use a dedicated config with force_reload=\"never\". \
             This server must never unload AMI/QDMA or remove PCIe functions automatically"
        ));
    }
    println!("hardware_reload_policy=never");
    Ok(())
}

fn configure_stream(stream: &TcpStream, timeout_secs: u64) -> Result<(), String> {
    stream
        .set_read_timeout(Some(Duration::from_secs(timeout_secs)))
        .map_err(|err| format!("unable to set read timeout: {err}"))?;
    stream
        .set_write_timeout(Some(Duration::from_secs(timeout_secs)))
        .map_err(|err| format!("unable to set write timeout: {err}"))
}

fn handle_connection(
    stream: &mut TcpStream,
    hpu_device: &HpuDevice,
    params: &ShortintParameterSet,
    max_request_bytes: usize,
) -> Result<(), String> {
    let server_start = Instant::now();
    let receive_start = Instant::now();
    let request = read_frame(stream, max_request_bytes)?;
    let request_receive = receive_start.elapsed();
    let request_id = request.request_id;
    let operation = request.operation;
    println!(
        "request_received request_id={request_id} operation={} items={} payload_bytes={} receive_ms={:.3}",
        operation_name(operation),
        request.metadata.item_count,
        request.ciphertext_words.len() * 8,
        request_receive.as_secs_f64() * 1000.0,
    );
    let result = process_request(&request, hpu_device, params);
    match result {
        Ok(processed) => {
            let response_payload_bytes = processed.words.len() * 8;
            let response_send_start = Instant::now();
            write_ciphertext_frame_version(
                stream,
                request.version,
                FRAME_RESPONSE,
                request_id,
                operation,
                request.scalar,
                &processed.metadata,
                &processed.words,
            )?;
            let response_send = response_send_start.elapsed();
            let server_total = server_start.elapsed();
            let timing = ServerTiming {
                request_id,
                request_receive_ns: duration_ns(request_receive),
                request_validate_ns: duration_ns(processed.timing.request_validate),
                request_decode_ns: duration_ns(processed.timing.request_decode),
                hpu_prepare_ns: duration_ns(processed.timing.hpu_prepare),
                hpu_enqueue_ns: duration_ns(processed.timing.hpu_enqueue),
                hpu_wait_sync_ns: duration_ns(processed.timing.hpu_wait_sync),
                hpu_output_convert_ns: duration_ns(processed.timing.hpu_output_convert),
                result_encode_ns: duration_ns(processed.timing.result_encode),
                mem_sanitizer_ns: duration_ns(processed.timing.mem_sanitizer),
                remote_process_ns: duration_ns(processed.timing.remote_process),
                response_send_ns: duration_ns(response_send),
                server_total_ns: duration_ns(server_total),
                response_payload_bytes: response_payload_bytes as u64,
            };
            if request.version == VERSION_TIMING {
                write_server_timing(stream, &timing)?;
            }
            println!(
                "request_complete request_id={request_id} operation={} items={} result_bytes={} server_total_ms={:.3}",
                operation_name(operation),
                request.metadata.item_count,
                response_payload_bytes,
                ns_ms(timing.server_total_ns),
            );
            println!(
                "BENCH_REMOTE_SERVER_CSV,{request_id},{},{},{},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6},{:.6}",
                operation_name(operation),
                request.metadata.item_count,
                response_payload_bytes,
                ns_ms(timing.request_receive_ns),
                ns_ms(timing.request_validate_ns),
                ns_ms(timing.request_decode_ns),
                ns_ms(timing.hpu_prepare_ns),
                ns_ms(timing.hpu_enqueue_ns),
                ns_ms(timing.hpu_wait_sync_ns),
                ns_ms(timing.hpu_output_convert_ns),
                ns_ms(timing.result_encode_ns),
                ns_ms(timing.mem_sanitizer_ns),
                ns_ms(timing.remote_process_ns),
                ns_ms(timing.response_send_ns),
                ns_ms(timing.server_total_ns),
                timing.response_payload_bytes,
            );
            Ok(())
        }
        Err(err) => {
            write_error_frame_version(stream, request.version, request_id, operation, &err)?;
            Err(err)
        }
    }
}

#[derive(Default)]
struct ProcessTiming {
    request_validate: Duration,
    request_decode: Duration,
    hpu_prepare: Duration,
    hpu_enqueue: Duration,
    hpu_wait_sync: Duration,
    hpu_output_convert: Duration,
    result_encode: Duration,
    mem_sanitizer: Duration,
    remote_process: Duration,
}

struct ProcessedRequest {
    metadata: BatchMetadata,
    words: Vec<u64>,
    timing: ProcessTiming,
}

fn process_request(
    request: &protocol::CiphertextFrame,
    hpu_device: &HpuDevice,
    params: &ShortintParameterSet,
) -> Result<ProcessedRequest, String> {
    let process_start = Instant::now();
    let mut timing = ProcessTiming::default();
    let validate_start = Instant::now();
    if request.kind != FRAME_REQUEST {
        return Err(format!("expected request frame, got kind {}", request.kind));
    }
    if request.status != 0 {
        return Err(format!(
            "request status must be zero, got {}",
            request.status
        ));
    }
    if request.operation != OP_ADD_SCALAR_U8
        && request.operation != OP_ADD_SCALAR_U8_HPU_NATIVE
        && request.operation != OP_ADD_SCALAR_U8_HPU_NATIVE_ROUNDTRIP
        && request.operation != OP_ECHO_U8
    {
        return Err(format!("unsupported operation {}", request.operation));
    }
    let scalar = u8::try_from(request.scalar)
        .map_err(|_| format!("u8 scalar is out of range: {}", request.scalar))?;
    bridge::validate_metadata(&request.metadata, params)?;
    timing.request_validate = validate_start.elapsed();

    if request.operation == OP_ECHO_U8 {
        let encode_start = Instant::now();
        let words = request.ciphertext_words.clone();
        timing.result_encode = encode_start.elapsed();
        timing.remote_process = process_start.elapsed();
        return Ok(ProcessedRequest {
            metadata: request.metadata.clone(),
            words,
            timing,
        });
    }

    let decode_start = Instant::now();
    let native_roundtrip = request.operation == OP_ADD_SCALAR_U8_HPU_NATIVE_ROUNDTRIP;
    let native_inputs = if request.operation == OP_ADD_SCALAR_U8_HPU_NATIVE || native_roundtrip {
        Some(bridge::hpu_native_words_to_lwe_ciphertexts(
            &request.metadata,
            &request.ciphertext_words,
            hpu_device.params(),
        )?)
    } else {
        None
    };
    let cpu_inputs = if request.operation == OP_ADD_SCALAR_U8 {
        Some(bridge::words_to_radix_ciphertexts(
            &request.metadata,
            &request.ciphertext_words,
            params,
        )?)
    } else {
        None
    };
    timing.request_decode = decode_start.elapsed();
    println!(
        "request_decoded request_id={} operation={} items={} decode_ms={:.3}",
        request.request_id,
        operation_name(request.operation),
        request.metadata.item_count,
        timing.request_decode.as_secs_f64() * 1000.0,
    );

    // Keep only one input/output pair resident in the HPU ciphertext pool at a
    // time. Holding the complete batch before dispatch makes a large request
    // vulnerable to pool allocation stalls and delays the first HPU command.
    let mut cpu_outputs = Vec::with_capacity(request.metadata.item_count);
    let mut native_outputs = Vec::with_capacity(request.metadata.item_count);
    if let Some(cpu_inputs) = cpu_inputs {
        for (item_index, cpu_input) in cpu_inputs.into_iter().enumerate() {
            let prepare_start = Instant::now();
            let hpu_input = HpuRadixCiphertext::from_radix_ciphertext(&cpu_input, hpu_device);
            timing.hpu_prepare += prepare_start.elapsed();

            let enqueue_start = Instant::now();
            let hpu_output = &hpu_input + u128::from(scalar);
            timing.hpu_enqueue += enqueue_start.elapsed();

            let wait_start = Instant::now();
            hpu_output.wait();
            timing.hpu_wait_sync += wait_start.elapsed();

            let output_start = Instant::now();
            cpu_outputs.push(hpu_output.to_radix_ciphertext());
            timing.hpu_output_convert += output_start.elapsed();
            drop(hpu_output);
            drop(hpu_input);

            if (item_index + 1) % 16 == 0 || item_index + 1 == request.metadata.item_count {
                println!(
                    "hpu_progress request_id={} completed={}/{}",
                    request.request_id,
                    item_index + 1,
                    request.metadata.item_count,
                );
            }
        }
    }
    if let Some(native_inputs) = native_inputs {
        for (item_index, native_input) in native_inputs.into_iter().enumerate() {
            let prepare_start = Instant::now();
            let hpu_input = HpuRadixCiphertext::from_hpu_lwe_ciphertexts(native_input, hpu_device);
            timing.hpu_prepare += prepare_start.elapsed();

            let enqueue_start = Instant::now();
            let hpu_output = &hpu_input + u128::from(scalar);
            timing.hpu_enqueue += enqueue_start.elapsed();

            // HPU commands are asynchronous. wait() includes command completion
            // and the backend's device-to-host synchronization.
            let wait_start = Instant::now();
            hpu_output.wait();
            timing.hpu_wait_sync += wait_start.elapsed();

            let output_start = Instant::now();
            if native_roundtrip {
                native_outputs.push(hpu_output.to_hpu_lwe_ciphertexts());
            } else {
                cpu_outputs.push(hpu_output.to_radix_ciphertext());
            }
            timing.hpu_output_convert += output_start.elapsed();
            drop(hpu_output);
            drop(hpu_input);

            if (item_index + 1) % 16 == 0 || item_index + 1 == request.metadata.item_count {
                println!(
                    "hpu_progress request_id={} completed={}/{}",
                    request.request_id,
                    item_index + 1,
                    request.metadata.item_count,
                );
            }
        }
    }

    let encode_start = Instant::now();
    let mut response_metadata = request.metadata.clone();
    let result_words = if native_roundtrip {
        response_metadata.ciphertext_word_count =
            response_metadata.expected_hpu_native_word_count()?;
        bridge::hpu_lwe_ciphertexts_to_native_words(&response_metadata, native_outputs)?
    } else {
        response_metadata.ciphertext_word_count = response_metadata.expected_cpu_word_count()?;
        bridge::radix_ciphertexts_to_words(&cpu_outputs)
    };
    timing.result_encode = encode_start.elapsed();
    if result_words.len() != response_metadata.ciphertext_word_count {
        return Err(format!(
            "HPU result word count mismatch: result={}, expected={}",
            result_words.len(),
            response_metadata.ciphertext_word_count
        ));
    }
    drop(cpu_outputs);
    let sanitizer_start = Instant::now();
    hpu_device.mem_sanitizer();
    timing.mem_sanitizer = sanitizer_start.elapsed();
    timing.remote_process = process_start.elapsed();
    println!(
        "hpu_compute request_id={} operation=ADDS scalar={} items={} enqueue_ms={:.3} wait_sync_ms={:.3} process_ms={:.3}",
        request.request_id,
        scalar,
        request.metadata.item_count,
        timing.hpu_enqueue.as_secs_f64() * 1000.0,
        timing.hpu_wait_sync.as_secs_f64() * 1000.0,
        timing.remote_process.as_secs_f64() * 1000.0,
    );
    Ok(ProcessedRequest {
        metadata: response_metadata,
        words: result_words,
        timing,
    })
}

fn duration_ns(duration: Duration) -> u64 {
    u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX)
}

fn ns_ms(nanoseconds: u64) -> f64 {
    nanoseconds as f64 / 1_000_000.0
}

fn operation_name(operation: u64) -> &'static str {
    match operation {
        OP_ECHO_U8 => "echo",
        OP_ADD_SCALAR_U8 => "adds",
        OP_ADD_SCALAR_U8_HPU_NATIVE => "adds-hpu-native",
        OP_ADD_SCALAR_U8_HPU_NATIVE_ROUNDTRIP => "adds-hpu-native-roundtrip",
        _ => "unknown",
    }
}
