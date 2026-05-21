// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#ifdef __SWITCH__

#include <cryptopp/cryptlib.h>

#include "core/hw/random.h"

namespace HW {

class CryptoRandomNumberGenerator final : public CryptoPP::RandomNumberGenerator {
public:
    void GenerateBlock(CryptoPP::byte* output, std::size_t size) override {
        GenerateRandomBytes(output, size);
    }
};

} // namespace HW

#else

#include <cryptopp/osrng.h>

namespace HW {
using CryptoRandomNumberGenerator = CryptoPP::AutoSeededRandomPool;
}

#endif
