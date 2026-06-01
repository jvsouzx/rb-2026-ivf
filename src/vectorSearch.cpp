#include "vectorSearch.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <cmath>
#include <immintrin.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "quantize.hpp"

namespace {
    constexpr std::uint32_t ReferencesMagic = 0x32464252;
    constexpr int VectorDimensions = 14;
    constexpr std::size_t HeaderSize = sizeof(std::uint32_t) * 2;

    int horizontalSum8x32(__m256i values) {
        __m128i low = _mm256_castsi256_si128(values);
        __m128i high = _mm256_extracti128_si256(values, 1);
        __m128i sum = _mm_add_epi32(low, high);
        sum = _mm_hadd_epi32(sum, sum);
        sum = _mm_hadd_epi32(sum, sum);
        return _mm_cvtsi128_si32(sum);
    }

    __m128i paddedQueryBytes(const std::array<std::uint8_t, 14>& queryVector) {
        alignas(16) std::uint8_t padded[16] = {};
        std::memcpy(padded, queryVector.data(), queryVector.size());
        return _mm_load_si128(reinterpret_cast<const __m128i*>(padded));
    }

    int euclideanDistanceAvx2(__m128i queryBytes, const std::uint8_t* referenceVector) {
        const __m128i mask = _mm_set_epi8(
            0, 0,
            static_cast<char>(0xff), static_cast<char>(0xff),
            static_cast<char>(0xff), static_cast<char>(0xff),
            static_cast<char>(0xff), static_cast<char>(0xff),
            static_cast<char>(0xff), static_cast<char>(0xff),
            static_cast<char>(0xff), static_cast<char>(0xff),
            static_cast<char>(0xff), static_cast<char>(0xff),
            static_cast<char>(0xff), static_cast<char>(0xff));

        __m128i refBytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(referenceVector));
        refBytes = _mm_and_si128(refBytes, mask);

        __m256i refWords = _mm256_cvtepu8_epi16(refBytes);
        __m256i queryWords = _mm256_cvtepu8_epi16(queryBytes);
        __m256i diff = _mm256_sub_epi16(refWords, queryWords);
        __m256i squares = _mm256_madd_epi16(diff, diff);

        return horizontalSum8x32(squares);
    }

    int findWorstNeighborIndex(const std::array<Neighbor, 5>& neighbors) {
        int worstIndex = 0;
        for (int i = 1; i < 5; ++i) {
            if (neighbors[i].distance > neighbors[worstIndex].distance) {
                worstIndex = i;
            }
        }

        return worstIndex;
    }
}

ReferenceStore::~ReferenceStore() {
    if (mappedData != nullptr && mappedSize > 0) {
        munmap(const_cast<std::uint8_t*>(mappedData), mappedSize);
    }
}

ReferenceStore::ReferenceStore(ReferenceStore&& other) noexcept
    : count(other.count),
      mappedSize(other.mappedSize),
      mappedData(other.mappedData),
      vectors(other.vectors),
      labels(other.labels) {
    other.count = 0;
    other.mappedSize = 0;
    other.mappedData = nullptr;
    other.vectors = nullptr;
    other.labels = nullptr;
}

ReferenceStore& ReferenceStore::operator=(ReferenceStore&& other) noexcept {
    if (this != &other) {
        if (mappedData != nullptr && mappedSize > 0) {
            munmap(const_cast<std::uint8_t*>(mappedData), mappedSize);
        }

        count = other.count;
        mappedSize = other.mappedSize;
        mappedData = other.mappedData;
        vectors = other.vectors;
        labels = other.labels;

        other.count = 0;
        other.mappedSize = 0;
        other.mappedData = nullptr;
        other.vectors = nullptr;
        other.labels = nullptr;
    }

    return *this;
}

ReferenceStore loadBinaryReferences(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        throw std::runtime_error("Erro ao abrir " + path);
    }

    struct stat fileStat {};
    if (fstat(fd, &fileStat) == -1) {
        close(fd);
        throw std::runtime_error("Erro ao obter tamanho de " + path);
    }

    if (fileStat.st_size < static_cast<off_t>(HeaderSize)) {
        close(fd);
        throw std::runtime_error("Arquivo de referencias binario muito pequeno: " + path);
    }

    std::size_t mappedSize = static_cast<std::size_t>(fileStat.st_size);
    void* mapping = mmap(nullptr, mappedSize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapping == MAP_FAILED) {
        throw std::runtime_error("Erro ao mapear " + path);
    }

    const auto* data = static_cast<const std::uint8_t*>(mapping);

    std::uint32_t magic = 0;
    std::uint32_t count = 0;
    std::memcpy(&magic, data, sizeof(magic));
    std::memcpy(&count, data + sizeof(magic), sizeof(count));

    if (magic != ReferencesMagic) {
        munmap(mapping, mappedSize);
        throw std::runtime_error("Arquivo de referencias binario invalido: " + path);
    }

    std::size_t vectorsSize = static_cast<std::size_t>(count) * VectorDimensions;
    std::size_t labelsSize = count;
    std::size_t expectedSize = HeaderSize + vectorsSize + labelsSize;

    if (mappedSize != expectedSize) {
        munmap(mapping, mappedSize);
        throw std::runtime_error("Tamanho inesperado do arquivo de referencias: " + path);
    }

    ReferenceStore store;
    store.count = count;
    store.mappedSize = mappedSize;
    store.mappedData = data;
    store.vectors = data + HeaderSize;
    store.labels = store.vectors + vectorsSize;

    std::cout << "Carregadas " << store.count << " referencias binarias\n";
    return store;
}

const ReferenceStore& getReferences(){
    static const ReferenceStore references = loadBinaryReferences("resources/references.bin");
    return references;
}

int euclideanDistance(const std::array<uint8_t, 14>& queryVector, const uint8_t* referenceVector) {
    int distance = 0;
    for (int i = 0; i < VectorDimensions; i++){
        int diff = static_cast<int>(referenceVector[i]) - static_cast<int>(queryVector[i]);
        distance += diff * diff;
    }
    return distance;
}

DistanceValidationResult validateDistanceImplementations(const std::array<std::uint8_t, 14>& queryVector, std::size_t sampleCount) {
    const ReferenceStore& refs = getReferences();
    __m128i queryBytes = paddedQueryBytes(queryVector);
    std::size_t checked = std::min<std::size_t>(sampleCount, refs.count);

    DistanceValidationResult result;
    result.checked = checked;

    for (std::size_t i = 0; i < checked; ++i) {
        const std::uint8_t* vector = refs.vectors + i * VectorDimensions;
        int scalarDistance = euclideanDistance(queryVector, vector);
        int avx2Distance = euclideanDistanceAvx2(queryBytes, vector);

        if (scalarDistance != avx2Distance) {
            result.mismatches++;
            if (result.mismatches == 1) {
                result.firstMismatchIndex = static_cast<std::uint32_t>(i);
                result.firstScalarDistance = scalarDistance;
                result.firstAvx2Distance = avx2Distance;
            }
        }
    }

    return result;
}

std::array<bool, 5> kNearestNeighbor(const std::array<uint8_t, 14>& queryVector){
    const ReferenceStore& refs = getReferences();
    std::array<Neighbor, 5> nearest{};
    __m128i queryBytes = paddedQueryBytes(queryVector);

    for (std::uint32_t i = 0; i < 5; ++i) {
        const uint8_t* vector = refs.vectors + static_cast<std::size_t>(i) * VectorDimensions;
        int distance = euclideanDistanceAvx2(queryBytes, vector);
        nearest[i] = Neighbor{distance, refs.labels[i] == 1};
    }

    int worstIndex = findWorstNeighborIndex(nearest);

    for (std::uint32_t i = 5; i < refs.count; ++i){
        const uint8_t* vector = refs.vectors + static_cast<std::size_t>(i) * VectorDimensions;
        int distance = euclideanDistanceAvx2(queryBytes, vector);

        if (distance < nearest[worstIndex].distance) {
            nearest[worstIndex] = Neighbor{distance, refs.labels[i] == 1};
            worstIndex = findWorstNeighborIndex(nearest);
        }
    }

    std::array<bool, 5> result{};
    for (int i = 0; i < 5; ++i){
        result[i] = nearest[i].fraud;
    }
    
    return result;
}

FraudScoreResult transactionIsApproved(const std::array<float, 14>& queryVector){
    std::array<uint8_t, 14> quantQuery = quantizeVector(queryVector);
    std::array<bool, 5> nearest = kNearestNeighbor(quantQuery);
    int fraudCount = 0;

    for (bool isFraud : nearest) {
        if (isFraud) {
            fraudCount++;
        }
    }

    float fraudScore = fraudCount / 5.0f;
    bool approved = fraudScore < 0.6f;

    return FraudScoreResult{approved, fraudScore};
}
