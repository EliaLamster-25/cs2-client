#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// Decrypt CRME (AES-256-GCM) blob into plaintext CRMG archive bytes.
bool payloadDecryptCrme(const std::uint8_t* data, std::size_t size,
    const std::string& payloadKeyBase64, std::vector<std::uint8_t>& outPlain,
    std::string& error);

bool payloadDecryptCrmeBytes(const std::uint8_t* data, std::size_t size,
    const std::uint8_t* key32, std::vector<std::uint8_t>& outPlain, std::string& error);

/// Unwrap release DEK from session-bound dek_wrap blob (base64).
bool payloadUnwrapDek(const std::string& sessionKeyBase64, const std::string& dekWrapBase64,
    std::vector<std::uint8_t>& outDek, std::string& error);
