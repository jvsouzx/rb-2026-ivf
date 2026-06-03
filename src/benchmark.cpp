#include "quantize.hpp"
#include "vectorSearch.hpp"
#include "vectorize.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

using json = nlohmann::json;

namespace {
    std::size_t readSizeEnv(const char* name, std::size_t fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr) {
            return fallback;
        }

        try {
            return static_cast<std::size_t>(std::stoull(value));
        } catch (...) {
            return fallback;
        }
    }

    double toMilliseconds(std::chrono::steady_clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }
}

int main() {
    const std::size_t validationSamples = readSizeEnv("BENCH_VALIDATE_SAMPLES", 100000);
    const std::size_t requestLimit = readSizeEnv("BENCH_REQUEST_LIMIT", 50);

    std::ifstream payloadFile("resources/example-payloads.json");
    if (!payloadFile) {
        std::cerr << "Erro ao abrir resources/example-payloads.json\n";
        return 1;
    }

    json payloads = json::parse(payloadFile);
    if (!payloads.is_array() || payloads.empty()) {
        std::cerr << "resources/example-payloads.json nao contem payloads\n";
        return 1;
    }

    std::array<float, 14> firstVector = vectorizeTransaction(payloads[0].dump());
    std::array<std::uint8_t, 14> firstQuantizedVector = quantizeVector(firstVector);

    DistanceValidationResult validation = validateDistanceImplementations(firstQuantizedVector, validationSamples);
    std::cout << "distance_validation.checked=" << validation.checked << "\n";
    std::cout << "distance_validation.mismatches=" << validation.mismatches << "\n";

    if (validation.mismatches > 0) {
        std::cout << "distance_validation.first_mismatch_index=" << validation.firstMismatchIndex << "\n";
        std::cout << "distance_validation.first_scalar=" << validation.firstScalarDistance << "\n";
        std::cout << "distance_validation.first_avx2=" << validation.firstAvx2Distance << "\n";
        return 1;
    }

    std::size_t requests = std::min<std::size_t>(requestLimit, payloads.size());
    double totalMs = 0.0;
    double minMs = std::numeric_limits<double>::max();
    double maxMs = 0.0;
    int denied = 0;

    for (std::size_t i = 0; i < requests; ++i) {
        std::array<float, 14> vector = vectorizeTransaction(payloads[i].dump());

        auto start = std::chrono::steady_clock::now();
        FraudScoreResult result = transactionIsApproved(vector);
        auto end = std::chrono::steady_clock::now();

        double elapsedMs = toMilliseconds(end - start);
        totalMs += elapsedMs;
        minMs = std::min(minMs, elapsedMs);
        maxMs = std::max(maxMs, elapsedMs);

        if (!result.approved) {
            denied++;
        }
    }

    std::cout << "requests=" << requests << "\n";
    std::cout << "denied=" << denied << "\n";
    std::cout << "ann_avg_ms=" << (totalMs / requests) << "\n";
    std::cout << "ann_min_ms=" << minMs << "\n";
    std::cout << "ann_max_ms=" << maxMs << "\n";

    return 0;
}
