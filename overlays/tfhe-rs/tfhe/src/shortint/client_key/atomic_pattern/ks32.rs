use serde::{Deserialize, Serialize};
use tfhe_versionable::Versionize;

use crate::core_crypto::prelude::{
    allocate_and_generate_new_binary_glwe_secret_key,
    allocate_and_generate_new_binary_lwe_secret_key, allocate_and_generate_new_lwe_keyswitch_key,
    allocate_and_generate_new_seeded_lwe_keyswitch_key, LweKeyswitchKeyOwned,
    SeededLweKeyswitchKeyOwned,
};
use crate::shortint::backward_compatibility::client_key::atomic_pattern::KS32AtomicPatternClientKeyVersions;
use crate::shortint::client_key::secret_encryption_key::SecretEncryptionKeyView;
use crate::shortint::client_key::{GlweSecretKeyOwned, LweSecretKeyOwned, LweSecretKeyView};
use crate::shortint::engine::ShortintEngine;
use crate::shortint::list_compression::{
    CompressedCompressionKey, CompressedDecompressionKey, CompressionKey, CompressionPrivateKeys,
    DecompressionKey,
};
use crate::shortint::parameters::{
    CompressionParameters, DynamicDistribution, KeySwitch32PBSParameters,
    ShortintKeySwitchingParameters,
};
use crate::shortint::{AtomicPatternKind, EncryptionKeyChoice, ShortintParameterSet};

use super::EncryptionAtomicPattern;

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, Versionize)]
#[versionize(KS32AtomicPatternClientKeyVersions)]
pub struct KS32AtomicPatternClientKey {
    pub(crate) glwe_secret_key: GlweSecretKeyOwned<u64>,
    /// Key used as the output of the keyswitch operation
    pub(crate) lwe_secret_key: LweSecretKeyOwned<u32>,
    pub parameters: KeySwitch32PBSParameters,
}

impl KS32AtomicPatternClientKey {
    pub(crate) fn new_with_engine(
        parameters: KeySwitch32PBSParameters,
        engine: &mut ShortintEngine,
    ) -> Self {
        // generate the lwe secret key
        let lwe_secret_key = allocate_and_generate_new_binary_lwe_secret_key(
            parameters.lwe_dimension(),
            &mut engine.secret_generator,
        );

        // generate the rlwe secret key
        let glwe_secret_key = allocate_and_generate_new_binary_glwe_secret_key(
            parameters.glwe_dimension(),
            parameters.polynomial_size(),
            &mut engine.secret_generator,
        );

        // pack the keys in the client key set
        Self {
            glwe_secret_key,
            lwe_secret_key,
            parameters,
        }
    }

    pub fn new(parameters: KeySwitch32PBSParameters) -> Self {
        ShortintEngine::with_thread_local_mut(|engine| Self::new_with_engine(parameters, engine))
    }

    pub fn into_raw_parts(
        self,
    ) -> (
        GlweSecretKeyOwned<u64>,
        LweSecretKeyOwned<u32>,
        KeySwitch32PBSParameters,
    ) {
        let Self {
            glwe_secret_key,
            lwe_secret_key,
            parameters,
        } = self;

        (glwe_secret_key, lwe_secret_key, parameters)
    }

    pub fn from_raw_parts(
        glwe_secret_key: GlweSecretKeyOwned<u64>,
        lwe_secret_key: LweSecretKeyOwned<u32>,
        parameters: KeySwitch32PBSParameters,
    ) -> Self {
        assert_eq!(
            lwe_secret_key.lwe_dimension(),
            parameters.lwe_dimension(),
            "Mismatch between the LweSecretKey LweDimension ({:?}) \
            and the parameters LweDimension ({:?})",
            lwe_secret_key.lwe_dimension(),
            parameters.lwe_dimension()
        );
        assert_eq!(
            glwe_secret_key.glwe_dimension(),
            parameters.glwe_dimension(),
            "Mismatch between the GlweSecretKey GlweDimension ({:?}) \
            and the parameters GlweDimension ({:?})",
            glwe_secret_key.glwe_dimension(),
            parameters.glwe_dimension()
        );
        assert_eq!(
            glwe_secret_key.polynomial_size(),
            parameters.polynomial_size(),
            "Mismatch between the GlweSecretKey PolynomialSize ({:?}) \
            and the parameters PolynomialSize ({:?})",
            glwe_secret_key.polynomial_size(),
            parameters.polynomial_size()
        );

        Self {
            glwe_secret_key,
            lwe_secret_key,
            parameters,
        }
    }

    pub fn try_from_lwe_encryption_key(
        encryption_key: LweSecretKeyOwned<u64>,
        parameters: KeySwitch32PBSParameters,
    ) -> crate::Result<Self> {
        let expected_lwe_dimension = parameters.encryption_lwe_dimension();
        if encryption_key.lwe_dimension() != expected_lwe_dimension {
            return Err(
                crate::Error::new(
                    format!(
                        "The given encryption key does not have the correct LweDimension, expected: {:?}, got: {:?}",
                        encryption_key.lwe_dimension(),
                        expected_lwe_dimension)));
        }

        // The key we got is the one used to encrypt,
        // we have to generate the other key. The KS32 ap only support KS-PBS order so we need to
        // generate the small key.
        let small_key = ShortintEngine::with_thread_local_mut(|engine| {
            allocate_and_generate_new_binary_lwe_secret_key(
                parameters.lwe_dimension(),
                &mut engine.secret_generator,
            )
        });

        Ok(Self {
            glwe_secret_key: GlweSecretKeyOwned::from_container(
                encryption_key.into_container(),
                parameters.polynomial_size(),
            ),
            lwe_secret_key: small_key,
            parameters,
        })
    }

    pub fn large_lwe_secret_key(&self) -> LweSecretKeyView<'_, u64> {
        self.glwe_secret_key.as_lwe_secret_key()
    }

    pub fn small_lwe_secret_key(&self) -> LweSecretKeyView<'_, u32> {
        self.lwe_secret_key.as_view()
    }

    pub fn new_compression_key(
        &self,
        private_compression_key: &CompressionPrivateKeys,
    ) -> CompressionKey {
        private_compression_key.new_compression_key(&self.glwe_secret_key, self.parameters())
    }

    pub fn new_compressed_compression_key(
        &self,
        private_compression_key: &CompressionPrivateKeys,
    ) -> CompressedCompressionKey {
        private_compression_key
            .new_compressed_compression_key(&self.glwe_secret_key, self.parameters())
    }

    pub fn new_decompression_key(
        &self,
        private_compression_key: &CompressionPrivateKeys,
    ) -> DecompressionKey {
        private_compression_key.new_decompression_key(&self.glwe_secret_key, self.parameters())
    }

    pub fn new_decompression_key_with_params(
        &self,
        private_compression_key: &CompressionPrivateKeys,
        compression_params: CompressionParameters,
    ) -> DecompressionKey {
        private_compression_key.new_decompression_key_with_params(
            &self.glwe_secret_key,
            self.parameters(),
            compression_params,
        )
    }

    pub fn new_decompression_key_with_params_and_engine(
        &self,
        private_compression_key: &CompressionPrivateKeys,
        compression_params: CompressionParameters,
        engine: &mut ShortintEngine,
    ) -> DecompressionKey {
        private_compression_key.new_decompression_key_with_params_and_engine(
            &self.glwe_secret_key,
            self.parameters(),
            compression_params,
            engine,
        )
    }

    pub fn new_compressed_decompression_key(
        &self,
        private_compression_key: &CompressionPrivateKeys,
    ) -> CompressedDecompressionKey {
        private_compression_key
            .new_compressed_decompression_key(&self.glwe_secret_key, self.parameters())
    }

    pub(crate) fn new_keyswitching_key_with_engine(
        &self,
        input_secret_key: &SecretEncryptionKeyView<'_>,
        params: ShortintKeySwitchingParameters,
        engine: &mut ShortintEngine,
    ) -> LweKeyswitchKeyOwned<u64> {
        match params.destination_key {
            EncryptionKeyChoice::Big => allocate_and_generate_new_lwe_keyswitch_key(
                &input_secret_key.lwe_secret_key,
                &self.large_lwe_secret_key(),
                params.ks_base_log,
                params.ks_level,
                self.parameters.glwe_noise_distribution(),
                self.parameters.ciphertext_modulus(),
                &mut engine.encryption_generator,
            ),
            EncryptionKeyChoice::Small => {
                let ksk = allocate_and_generate_new_lwe_keyswitch_key(
                    &input_secret_key.lwe_secret_key,
                    &self.small_lwe_secret_key(),
                    params.ks_base_log,
                    params.ks_level,
                    self.parameters.lwe_noise_distribution(),
                    self.parameters.post_keyswitch_ciphertext_modulus(),
                    &mut engine.encryption_generator,
                );
                let shift = u64::BITS - u32::BITS;

                LweKeyswitchKeyOwned::from_container(
                    ksk.as_ref()
                        .iter()
                        .map(|elem| (*elem as u64) << shift)
                        .collect(),
                    ksk.decomposition_base_log(),
                    ksk.decomposition_level_count(),
                    ksk.output_lwe_size(),
                    ksk.ciphertext_modulus()
                        .try_to()
                        // Ok to unwrap because converting a 32b modulus into a 64b one
                        // should not fail
                        .unwrap(),
                )
            }
        }
    }

    pub(crate) fn new_seeded_keyswitching_key_with_engine(
        &self,
        input_secret_key: &SecretEncryptionKeyView<'_>,
        params: ShortintKeySwitchingParameters,
        engine: &mut ShortintEngine,
    ) -> SeededLweKeyswitchKeyOwned<u64> {
        match params.destination_key {
            EncryptionKeyChoice::Big => allocate_and_generate_new_seeded_lwe_keyswitch_key(
                &input_secret_key.lwe_secret_key,
                &self.large_lwe_secret_key(),
                params.ks_base_log,
                params.ks_level,
                self.parameters.glwe_noise_distribution(),
                self.parameters.ciphertext_modulus(),
                &mut engine.seeder,
            ),
            EncryptionKeyChoice::Small => {
                let ksk = allocate_and_generate_new_seeded_lwe_keyswitch_key(
                    &input_secret_key.lwe_secret_key,
                    &self.small_lwe_secret_key(),
                    params.ks_base_log,
                    params.ks_level,
                    self.parameters.lwe_noise_distribution(),
                    self.parameters.post_keyswitch_ciphertext_modulus(),
                    &mut engine.seeder,
                );
                let shift = u64::BITS - u32::BITS;

                SeededLweKeyswitchKeyOwned::from_container(
                    ksk.as_ref()
                        .iter()
                        .map(|elem| (*elem as u64) << shift)
                        .collect(),
                    ksk.decomposition_base_log(),
                    ksk.decomposition_level_count(),
                    ksk.output_lwe_size(),
                    ksk.compression_seed(),
                    ksk.ciphertext_modulus()
                        .try_to()
                        // Ok to unwrap because converting a 32b modulus into a 64b one
                        // should not fail
                        .unwrap(),
                )
            }
        }
    }
}

impl EncryptionAtomicPattern for KS32AtomicPatternClientKey {
    fn parameters(&self) -> ShortintParameterSet {
        self.parameters.into()
    }

    fn encryption_key(&self) -> LweSecretKeyView<'_, u64> {
        // The KS32 atomic pattern is only supported with the KsPbs order
        self.glwe_secret_key.as_lwe_secret_key()
    }

    fn encryption_noise(&self) -> DynamicDistribution<u64> {
        // The KS32 atomic pattern is only supported with the KsPbs order
        self.parameters.glwe_noise_distribution()
    }

    fn kind(&self) -> AtomicPatternKind {
        AtomicPatternKind::KeySwitch32
    }
}

#[cfg(test)]
mod hls_lwe_encrypt_verify_tests {
    use super::*;
    use crate::core_crypto::prelude::{Cleartext, LweCiphertextOwned};
    use crate::shortint::ciphertext::{Degree, NoiseLevel};
    use crate::shortint::client_key::GenericClientKey;
    use crate::shortint::parameters::{
        CarryModulus, CiphertextModulus, CiphertextModulus32, DecompositionBaseLog,
        DecompositionLevelCount, DynamicDistribution, GlweDimension, KeySwitch32PBSParameters,
        LweDimension, MaxNoiseLevel, MessageModulus, ModulusSwitchType, PolynomialSize,
    };
    use crate::shortint::{Ciphertext, PaddingBit, ShortintEncoding};

    const HPU_LWE_ENCRYPT_VERIFY_PARAMS: KeySwitch32PBSParameters = KeySwitch32PBSParameters {
        lwe_dimension: LweDimension(879),
        glwe_dimension: GlweDimension(1),
        polynomial_size: PolynomialSize(2048),
        lwe_noise_distribution: DynamicDistribution::new_t_uniform(3),
        glwe_noise_distribution: DynamicDistribution::new_t_uniform(17),
        pbs_base_log: DecompositionBaseLog(23),
        pbs_level: DecompositionLevelCount(1),
        ks_base_log: DecompositionBaseLog(2),
        ks_level: DecompositionLevelCount(8),
        message_modulus: MessageModulus(4),
        carry_modulus: CarryModulus(4),
        max_noise_level: MaxNoiseLevel::new(5),
        log2_p_fail: -128.0,
        post_keyswitch_ciphertext_modulus: CiphertextModulus32::new(1 << 21),
        ciphertext_modulus: CiphertextModulus::new_native(),
        modulus_switch_noise_reduction_params: ModulusSwitchType::CenteredMeanNoiseReduction,
    };

    fn next_random(state: &mut u64) -> u64 {
        let mut x = *state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *state = x;
        x
    }

    fn hls_style_lwe_encrypt(
        client_key: &GenericClientKey<KS32AtomicPatternClientKey>,
        clear: u64,
        noise: u64,
        seed: u64,
    ) -> Ciphertext {
        let params = client_key.parameters();
        let encryption_key = client_key.encryption_key();
        let encoding = ShortintEncoding::from_parameters(params, PaddingBit::Yes);
        let encoded = encoding.encode(Cleartext(clear));
        let mut rng_state = seed;
        let mut raw_lwe = Vec::with_capacity(encryption_key.lwe_dimension().to_lwe_size().0);
        let mut body = encoded.0.wrapping_add(noise);

        for &secret_bit in encryption_key.as_ref() {
            let mask = next_random(&mut rng_state);
            raw_lwe.push(mask);

            if secret_bit != 0 {
                body = body.wrapping_add(mask.wrapping_mul(secret_bit));
            }
        }

        raw_lwe.push(body);

        let lwe = LweCiphertextOwned::from_container(raw_lwe, params.ciphertext_modulus());

        Ciphertext::new(
            lwe,
            Degree::new(params.message_modulus().0 - 1),
            NoiseLevel::NOMINAL,
            params.message_modulus(),
            params.carry_modulus(),
            params.atomic_pattern(),
        )
    }

    #[test]
    fn hls_style_big_lwe_ciphertext_decrypts_with_client_key() {
        let client_key = GenericClientKey {
            atomic_pattern: KS32AtomicPatternClientKey::new(HPU_LWE_ENCRYPT_VERIFY_PARAMS),
        };
        let params = client_key.parameters();
        let encryption_key = client_key.encryption_key();

        assert_eq!(params.lwe_dimension().0, 879);
        assert_eq!(params.encryption_lwe_dimension().0, 2048);
        assert_eq!(
            encryption_key.lwe_dimension(),
            params.encryption_lwe_dimension()
        );

        let encoding = ShortintEncoding::from_parameters(params, PaddingBit::Yes);
        let seed = 0x0123_4567_89ab_cdef;
        let test_noises = [0u64, 7, u64::MAX - 2];

        for clear in 0..params.message_modulus().0 {
            for noise in test_noises {
                let ct = hls_style_lwe_encrypt(&client_key, clear, noise, seed ^ clear ^ noise);
                let encoded = encoding.encode(Cleartext(clear));

                assert_eq!(
                    client_key.decrypt_no_decode(&ct).0,
                    encoded.0.wrapping_add(noise)
                );
                assert_eq!(client_key.decrypt(&ct), clear);
            }
        }
    }
}
