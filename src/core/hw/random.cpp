// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hw/random.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <mutex>

#include "common/logging/log.h"

#ifdef __SWITCH__
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#else
#include <openssl/rand.h>
#endif

namespace {

#ifdef __SWITCH__
[[noreturn]] void FatalMbedTlsError(const char* operation, int result) {
    LOG_CRITICAL(HW, "mbedTLS {} failed: -0x{:04X}", operation, -result);
    std::abort();
}

class MbedTlsRandomGenerator {
public:
    MbedTlsRandomGenerator() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);

        static constexpr unsigned char personalization[] = "azahar-switch";
        const int result =
            mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, personalization,
                                  sizeof(personalization) - 1);
        if (result != 0) {
            FatalMbedTlsError("ctr_drbg_seed", result);
        }
    }

    ~MbedTlsRandomGenerator() {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }

    void Generate(u8* data, std::size_t size) {
        std::scoped_lock lock{mutex};
        while (size > 0) {
            const std::size_t chunk = std::min<std::size_t>(size, MBEDTLS_CTR_DRBG_MAX_REQUEST);
            const int result = mbedtls_ctr_drbg_random(&ctr_drbg, data, chunk);
            if (result != 0) {
                FatalMbedTlsError("ctr_drbg_random", result);
            }
            data += chunk;
            size -= chunk;
        }
    }

private:
    std::mutex mutex;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
};

MbedTlsRandomGenerator& GetMbedTlsRandomGenerator() {
    static MbedTlsRandomGenerator generator;
    return generator;
}
#else
[[noreturn]] void FatalOpenSslError() {
    LOG_CRITICAL(HW, "OpenSSL RAND_bytes failed");
    std::abort();
}
#endif

} // namespace

namespace HW {

void GenerateRandomBytes(u8* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return;
    }

#ifdef __SWITCH__
    GetMbedTlsRandomGenerator().Generate(data, size);
#else
    while (size > 0) {
        const auto chunk = static_cast<int>(
            std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (RAND_bytes(data, chunk) != 1) {
            FatalOpenSslError();
        }
        data += chunk;
        size -= static_cast<std::size_t>(chunk);
    }
#endif
}

} // namespace HW
