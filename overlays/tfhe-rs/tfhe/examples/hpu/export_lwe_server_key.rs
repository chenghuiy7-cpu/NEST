//! Derive a remote-compute CompressedServerKey from the existing lwe_encrypt ClientKey.

use clap::Parser;
use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;
use tfhe::integer::{ClientKey, CompressedServerKey};
use tfhe::shortint::client_key::atomic_pattern::{
    AtomicPatternClientKey, KS32AtomicPatternClientKey,
};
use tfhe::shortint::client_key::GenericClientKey;
use tfhe::shortint::ClientKey as ShortintClientKey;

type KS32ClientKey = GenericClientKey<KS32AtomicPatternClientKey>;

#[derive(Parser, Debug)]
#[command(
    long_about = "Read the existing KS32 ClientKey used by SUDA lwe_encrypt and derive an integer CompressedServerKey for the remote HPU server. The source key is never modified."
)]
struct Args {
    #[arg(long)]
    client_key: PathBuf,

    #[arg(long)]
    output: PathBuf,

    #[arg(long)]
    force: bool,

    #[arg(long)]
    sync_write: bool,
}

fn main() -> ExitCode {
    match run(Args::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("server key export failed: {err}");
            ExitCode::from(1)
        }
    }
}

fn run(args: Args) -> Result<(), String> {
    if args.output.exists() && !args.force {
        return Err(format!(
            "{:?} already exists; pass --force to overwrite only the derived ServerKey",
            args.output
        ));
    }
    let serialized_client_key = fs::read(&args.client_key)
        .map_err(|err| format!("unable to read {:?}: {err}", args.client_key))?;
    let ks32_client_key: KS32ClientKey = bincode::deserialize(&serialized_client_key)
        .map_err(|err| format!("unable to deserialize {:?}: {err}", args.client_key))?;
    let params = ks32_client_key.parameters();

    let shortint_client_key = ShortintClientKey {
        atomic_pattern: AtomicPatternClientKey::KeySwitch32(ks32_client_key.atomic_pattern),
    };
    let integer_client_key = ClientKey::from_raw_parts(shortint_client_key);
    let compressed_server_key =
        CompressedServerKey::new_radix_compressed_server_key(&integer_client_key);
    let bytes = bincode::serialize(&compressed_server_key)
        .map_err(|err| format!("unable to serialize CompressedServerKey: {err}"))?;

    let mut output = fs::File::create(&args.output)
        .map_err(|err| format!("unable to create {:?}: {err}", args.output))?;
    output
        .write_all(&bytes)
        .map_err(|err| format!("unable to write {:?}: {err}", args.output))?;
    if args.sync_write {
        output
            .sync_all()
            .map_err(|err| format!("unable to fsync {:?}: {err}", args.output))?;
    }

    println!("source_client_key={}", args.client_key.display());
    println!("source_client_key_modified=no");
    println!("server_key_output={}", args.output.display());
    println!("server_key_bytes={}", bytes.len());
    println!("lwe_dimension={}", params.lwe_dimension().0);
    println!("big_lwe_dimension={}", params.encryption_lwe_dimension().0);
    println!("message_modulus={}", params.message_modulus().0);
    println!("carry_modulus={}", params.carry_modulus().0);
    Ok(())
}
