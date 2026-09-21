#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace crypto
{
bool RandomBytes(void* output, size_t size);
bool HmacSha256(const uint8_t* key, size_t key_size, const uint8_t* input,
                size_t input_size, std::array<uint8_t, 32>& output);
bool Sha256(const uint8_t* input, size_t input_size, std::array<uint8_t, 32>& output);
bool AesEcbEncryptBlock(const uint8_t* key, const uint8_t* input,
                        std::array<uint8_t, 16>& output);
bool AesGcm(bool encrypt, const uint8_t* key, const uint8_t* nonce,
            const uint8_t* input, size_t input_size, const uint8_t* associated_data,
            size_t associated_data_size, uint8_t* tag, uint8_t* output);
} // namespace crypto
