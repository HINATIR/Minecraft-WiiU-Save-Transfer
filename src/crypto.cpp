#include "crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <mutex>

namespace crypto
{
namespace
{
class RandomGenerator
{
public:
    RandomGenerator()
    {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&ctr_drbg_);
        const unsigned char personalization[] = "SavedataTransporter";
        ready_ = mbedtls_ctr_drbg_seed(&ctr_drbg_, mbedtls_entropy_func, &entropy_,
                                       personalization, sizeof(personalization) - 1) == 0;
    }

    ~RandomGenerator()
    {
        mbedtls_ctr_drbg_free(&ctr_drbg_);
        mbedtls_entropy_free(&entropy_);
    }

    bool Fill(void* output, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_ && mbedtls_ctr_drbg_random(&ctr_drbg_,
                                                 static_cast<unsigned char*>(output),
                                                 size) == 0;
    }

private:
    std::mutex mutex_;
    mbedtls_entropy_context entropy_;
    mbedtls_ctr_drbg_context ctr_drbg_;
    bool ready_ = false;
};
} // namespace

bool RandomBytes(void* output, size_t size)
{
    static RandomGenerator generator;
    return generator.Fill(output, size);
}

bool HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* input,
                size_t input_size, std::array<uint8_t, 32>& output)
{
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info)
        return false;
    return mbedtls_md_hmac(info, key, key_size, input, input_size, output.data()) == 0;
}

bool Sha256(const uint8_t* input, size_t input_size, std::array<uint8_t, 32>& output)
{
    return mbedtls_sha256(input, input_size, output.data(), 0) == 0;
}

bool AesEcbEncryptBlock(const uint8_t* key, const uint8_t* input,
                        std::array<uint8_t, 16>& output)
{
    mbedtls_aes_context context;
    mbedtls_aes_init(&context);
    const int set_key = mbedtls_aes_setkey_enc(&context, key, 128);
    const int crypt = set_key == 0 ?
        mbedtls_aes_crypt_ecb(&context, MBEDTLS_AES_ENCRYPT, input, output.data()) : set_key;
    mbedtls_aes_free(&context);
    return crypt == 0;
}

bool AesGcm(bool encrypt, const uint8_t* key, const uint8_t* nonce,
            const uint8_t* input, size_t input_size, const uint8_t* associated_data,
            size_t associated_data_size, uint8_t* tag, uint8_t* output)
{
    mbedtls_gcm_context context;
    mbedtls_gcm_init(&context);
    int result = mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (result == 0)
    {
        if (encrypt)
        {
            result = mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT,
                                               input_size, nonce, 12,
                                               associated_data, associated_data_size,
                                               input, output, 16, tag);
        }
        else
        {
            result = mbedtls_gcm_auth_decrypt(&context, input_size, nonce, 12,
                                              associated_data, associated_data_size,
                                              tag, 16, input, output);
        }
    }
    mbedtls_gcm_free(&context);
    return result == 0;
}
} // namespace crypto
