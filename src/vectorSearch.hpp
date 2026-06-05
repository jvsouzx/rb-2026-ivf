#ifndef VECTORSEARCH_HPP
#define VECTORSEARCH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

struct ReferenceStore {
    std::uint32_t count = 0;
    std::uint32_t nlist = 0;
    std::size_t mappedSize = 0;
    const std::uint8_t* mappedData = nullptr;
    const std::int16_t* centroids = nullptr;
    const std::uint32_t* offsets = nullptr;
    const std::int16_t* vectors = nullptr;
    const std::uint8_t* labels = nullptr;

    ReferenceStore() = default;
    ~ReferenceStore();

    ReferenceStore(const ReferenceStore&) = delete;
    ReferenceStore& operator=(const ReferenceStore&) = delete;

    ReferenceStore(ReferenceStore&& other) noexcept;
    ReferenceStore& operator=(ReferenceStore&& other) noexcept;
};

struct Neighbor {
    int distance;
    bool fraud;
};

struct FraudScoreResult {
    bool approved;
    float fraudScore;
};

struct DistanceValidationResult {
    std::size_t checked = 0;
    std::size_t mismatches = 0;
    std::uint32_t firstMismatchIndex = 0;
    int firstScalarDistance = 0;
    int firstAvx2Distance = 0;
};

std::array<bool, 5> approximateNearestFraudLabels(const std::array<std::int16_t, 14>& queryVector);
FraudScoreResult transactionIsApproved(const std::array<float, 14>& queryVector);
const ReferenceStore& getReferences();
void warmReferences();
ReferenceStore loadIvfReferences(const std::string& path);
int euclideanDistance(const std::array<std::int16_t, 14>& queryVector, const std::int16_t* referenceVector);
DistanceValidationResult validateDistanceImplementations(const std::array<std::int16_t, 14>& queryVector, std::size_t sampleCount);

#endif
