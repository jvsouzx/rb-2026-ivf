#include "vectorSearch.hpp"

#include "quantize.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const ReferenceStore& getReferences();

namespace {
    constexpr std::uint32_t IvfReferencesMagic = 0x56494252; // "RBIV"
    constexpr std::uint32_t IvfVersion = 3; // v3: vetores de referencia em int16
    constexpr int VectorDimensions = 14;
    constexpr std::size_t HeaderSize = sizeof(std::uint32_t) * 5;
    constexpr std::uint32_t MaxCentroidCandidates = 128;

    struct CentroidCandidate {
        int distance = std::numeric_limits<int>::max();
        std::uint32_t id = 0;
    };

    struct AnnSearchParams {
        std::uint32_t nprobe;
        std::size_t candidateCap;
    };

    struct AnnSearchResult {
        std::array<bool, 5> fraudLabels{};
        int fraudCount = 0;
    };

    std::size_t align4(std::size_t value) {
        return (value + 3) & ~static_cast<std::size_t>(3);
    }

    std::uint32_t readUInt32(const std::uint8_t* data) {
        std::uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    std::uint32_t readUIntEnv(const char* name, std::uint32_t fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr) {
            return fallback;
        }

        try {
            unsigned long parsed = std::stoul(value);
            if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
                return fallback;
            }
            return static_cast<std::uint32_t>(parsed);
        } catch (...) {
            return fallback;
        }
    }

    std::size_t readSizeEnv(const char* name, std::size_t fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr) {
            return fallback;
        }

        try {
            unsigned long long parsed = std::stoull(value);
            if (parsed == 0) {
                return fallback;
            }
            return static_cast<std::size_t>(parsed);
        } catch (...) {
            return fallback;
        }
    }

    std::string readStringEnv(const char* name, const std::string& fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return fallback;
        }

        return value;
    }

    std::uint32_t configuredNProbe(std::uint32_t nlist) {
        static const std::uint32_t requested = readUIntEnv("RB_IVF_NPROBE", 4);
        return std::clamp<std::uint32_t>(requested, 1, std::min(nlist, MaxCentroidCandidates));
    }

    std::uint32_t configuredFallbackNProbe(std::uint32_t nlist, std::uint32_t firstPassNProbe) {
        const std::uint32_t requested = readUIntEnv("RB_IVF_FALLBACK_NPROBE", firstPassNProbe);
        std::uint32_t fallback = std::max(requested, firstPassNProbe);
        return std::clamp<std::uint32_t>(fallback, 1, std::min(nlist, MaxCentroidCandidates));
    }

    std::size_t configuredCandidateCap() {
        static const std::size_t value = std::max<std::size_t>(5, readSizeEnv("RB_IVF_CANDIDATE_CAP", 2048));
        return value;
    }

    std::size_t configuredFallbackCandidateCap(std::size_t firstPassCandidateCap) {
        const std::size_t requested = readSizeEnv("RB_IVF_FALLBACK_CANDIDATE_CAP", firstPassCandidateCap);
        return std::max<std::size_t>(std::max<std::size_t>(5, requested), firstPassCandidateCap);
    }

    int horizontalSum8x32(__m256i values) {
        __m128i low = _mm256_castsi256_si128(values);
        __m128i high = _mm256_extracti128_si256(values, 1);
        __m128i sum = _mm_add_epi32(low, high);
        sum = _mm_hadd_epi32(sum, sum);
        sum = _mm_hadd_epi32(sum, sum);
        return _mm_cvtsi128_si32(sum);
    }

    // Mascara que zera as 2 lanes de padding (dims 14 e 15 do registro de 16
    // int16) — os vetores tem 14 dims, mas carregamos/processamos 16 por vez.
    const __m256i kLaneMask = _mm256_set_epi16(
        0, 0,
        -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1);

    __m256i paddedQueryWords(const std::array<std::int16_t, 14>& queryVector) {
        alignas(32) std::int16_t padded[16] = {};
        std::memcpy(padded, queryVector.data(), 14 * sizeof(std::int16_t));
        return _mm256_load_si256(reinterpret_cast<const __m256i*>(padded));
    }

    int euclideanDistanceAvx2(__m256i queryWords, const std::int16_t* referenceVector) {
        // Os valores ja vem em int16 (faltante = -kQuantizeLevels), entao a
        // distancia e euclidiana direta — sem remapeamento de sentinela.
        __m256i refWords = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(referenceVector));
        __m256i diff = _mm256_sub_epi16(refWords, queryWords);
        diff = _mm256_and_si256(diff, kLaneMask); // anula as 2 lanes de padding
        __m256i squares = _mm256_madd_epi16(diff, diff);

        return horizontalSum8x32(squares);
    }

    int euclideanDistanceScalar(const std::int16_t* lhs, const std::int16_t* rhs) {
        int distance = 0;
        for (int dim = 0; dim < VectorDimensions; ++dim) {
            int diff = static_cast<int>(lhs[dim]) - static_cast<int>(rhs[dim]);
            distance += diff * diff;
        }

        return distance;
    }

    int euclideanDistanceToCentroidBounded(
        const std::int16_t* queryVector,
        const std::int16_t* centroidVector,
        int maxDistance) {
        int distance = 0;
        for (int dim = 0; dim < VectorDimensions; ++dim) {
            int diff = static_cast<int>(queryVector[dim]) - static_cast<int>(centroidVector[dim]);
            distance += diff * diff;
            if (distance >= maxDistance) {
                return distance;
            }
        }

        return distance;
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

    void updateTop5(std::array<Neighbor, 5>& nearest, int& nearestSize, int& worstIndex, Neighbor candidate) {
        if (nearestSize < 5) {
            nearest[nearestSize] = candidate;
            nearestSize++;
            if (nearestSize == 5) {
                worstIndex = findWorstNeighborIndex(nearest);
            }
            return;
        }

        if (candidate.distance < nearest[worstIndex].distance) {
            nearest[worstIndex] = candidate;
            worstIndex = findWorstNeighborIndex(nearest);
        }
    }

    int findWorstCentroidIndex(const std::array<CentroidCandidate, MaxCentroidCandidates>& candidates, std::uint32_t size) {
        int worstIndex = 0;
        for (std::uint32_t i = 1; i < size; ++i) {
            if (candidates[i].distance > candidates[worstIndex].distance) {
                worstIndex = static_cast<int>(i);
            }
        }

        return worstIndex;
    }

    std::uint32_t selectCentroids(
        const ReferenceStore& refs,
        const std::array<std::int16_t, VectorDimensions>& queryVector,
        std::array<CentroidCandidate, MaxCentroidCandidates>& selected,
        std::uint32_t nprobe) {
        const std::uint32_t desired = std::min<std::uint32_t>(refs.nlist, std::min<std::uint32_t>(MaxCentroidCandidates, nprobe));

        std::uint32_t selectedSize = 0;
        int worstIndex = 0;

        for (std::uint32_t centroid = 0; centroid < refs.nlist; ++centroid) {
            const std::int16_t* centroidVector = refs.centroids + static_cast<std::size_t>(centroid) * VectorDimensions;
            int maxDistance = selectedSize == desired ? selected[worstIndex].distance : std::numeric_limits<int>::max();
            CentroidCandidate candidate{euclideanDistanceToCentroidBounded(queryVector.data(), centroidVector, maxDistance), centroid};

            if (selectedSize < desired) {
                selected[selectedSize] = candidate;
                selectedSize++;
                if (selectedSize == desired) {
                    worstIndex = findWorstCentroidIndex(selected, selectedSize);
                }
                continue;
            }

            if (candidate.distance < selected[worstIndex].distance) {
                selected[worstIndex] = candidate;
                worstIndex = findWorstCentroidIndex(selected, selectedSize);
            }
        }

        std::sort(
            selected.begin(),
            selected.begin() + selectedSize,
            [](const CentroidCandidate& lhs, const CentroidCandidate& rhs) {
                return lhs.distance < rhs.distance;
            });

        return selectedSize;
    }

    AnnSearchResult searchApproximateNeighbors(const std::array<std::int16_t, 14>& queryVector, AnnSearchParams params) {
        const ReferenceStore& refs = getReferences();
        std::array<Neighbor, 5> nearest{};
        std::array<CentroidCandidate, MaxCentroidCandidates> centroids{};
        int nearestSize = 0;
        int worstIndex = 0;
        std::size_t candidatesScanned = 0;
        __m256i queryWords = paddedQueryWords(queryVector);
        const std::array<std::int16_t, VectorDimensions>& centroidQuery = queryVector;

        params.nprobe = std::clamp<std::uint32_t>(params.nprobe, 1, std::min(refs.nlist, MaxCentroidCandidates));
        params.candidateCap = std::max<std::size_t>(5, params.candidateCap);

        std::uint32_t centroidCount = selectCentroids(refs, centroidQuery, centroids, params.nprobe);

        for (std::uint32_t probeIndex = 0; probeIndex < centroidCount; ++probeIndex) {
            if (probeIndex >= params.nprobe && nearestSize == 5) {
                break;
            }

            std::uint32_t centroid = centroids[probeIndex].id;
            std::uint32_t begin = refs.offsets[centroid];
            std::uint32_t end = refs.offsets[centroid + 1];

            for (std::uint32_t position = begin; position < end; ++position) {
                if (nearestSize == 5 && candidatesScanned >= params.candidateCap) {
                    break;
                }

                const std::int16_t* vector = refs.vectors + static_cast<std::size_t>(position) * VectorDimensions;
                int distance = euclideanDistanceAvx2(queryWords, vector);
                updateTop5(nearest, nearestSize, worstIndex, Neighbor{distance, refs.labels[position] == 1});
                candidatesScanned++;
            }

            if (nearestSize == 5 && candidatesScanned >= params.candidateCap) {
                break;
            }
        }

        AnnSearchResult result;
        for (int i = 0; i < nearestSize; ++i) {
            result.fraudLabels[i] = nearest[i].fraud;
            if (nearest[i].fraud) {
                result.fraudCount++;
            }
        }

        return result;
    }
}

ReferenceStore::~ReferenceStore() {
    if (mappedData != nullptr && mappedSize > 0) {
        munmap(const_cast<std::uint8_t*>(mappedData), mappedSize);
    }
}

ReferenceStore::ReferenceStore(ReferenceStore&& other) noexcept
    : count(other.count),
      nlist(other.nlist),
      mappedSize(other.mappedSize),
      mappedData(other.mappedData),
      centroids(other.centroids),
      offsets(other.offsets),
      vectors(other.vectors),
      labels(other.labels) {
    other.count = 0;
    other.nlist = 0;
    other.mappedSize = 0;
    other.mappedData = nullptr;
    other.centroids = nullptr;
    other.offsets = nullptr;
    other.vectors = nullptr;
    other.labels = nullptr;
}

ReferenceStore& ReferenceStore::operator=(ReferenceStore&& other) noexcept {
    if (this != &other) {
        if (mappedData != nullptr && mappedSize > 0) {
            munmap(const_cast<std::uint8_t*>(mappedData), mappedSize);
        }

        count = other.count;
        nlist = other.nlist;
        mappedSize = other.mappedSize;
        mappedData = other.mappedData;
        centroids = other.centroids;
        offsets = other.offsets;
        vectors = other.vectors;
        labels = other.labels;

        other.count = 0;
        other.nlist = 0;
        other.mappedSize = 0;
        other.mappedData = nullptr;
        other.centroids = nullptr;
        other.offsets = nullptr;
        other.vectors = nullptr;
        other.labels = nullptr;
    }

    return *this;
}

ReferenceStore loadIvfReferences(const std::string& path) {
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
        throw std::runtime_error("Arquivo IVF muito pequeno: " + path);
    }

    std::size_t mappedSize = static_cast<std::size_t>(fileStat.st_size);
    void* mapping = mmap(nullptr, mappedSize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (mapping == MAP_FAILED) {
        throw std::runtime_error("Erro ao mapear " + path);
    }

    // Pede ao kernel para trazer as páginas do índice já no startup, evitando
    // page faults nas primeiras requisições (que concentrariam latência/erros
    // no início da rampa de carga). O warmup síncrono fica em warmReferences().
    madvise(mapping, mappedSize, MADV_WILLNEED);

    const auto* data = static_cast<const std::uint8_t*>(mapping);

    std::uint32_t magic = readUInt32(data);
    std::uint32_t version = readUInt32(data + sizeof(std::uint32_t));
    std::uint32_t count = readUInt32(data + sizeof(std::uint32_t) * 2);
    std::uint32_t dimensions = readUInt32(data + sizeof(std::uint32_t) * 3);
    std::uint32_t nlist = readUInt32(data + sizeof(std::uint32_t) * 4);

    if (magic != IvfReferencesMagic || version != IvfVersion || dimensions != VectorDimensions || count == 0 || nlist == 0) {
        munmap(mapping, mappedSize);
        throw std::runtime_error("Arquivo IVF invalido: " + path);
    }

    std::size_t centroidsSize = static_cast<std::size_t>(nlist) * VectorDimensions * sizeof(std::int16_t);
    std::size_t offsetsStart = align4(HeaderSize + centroidsSize);
    std::size_t offsetsSize = (static_cast<std::size_t>(nlist) + 1) * sizeof(std::uint32_t);
    std::size_t vectorsStart = offsetsStart + offsetsSize;
    std::size_t vectorsSize = static_cast<std::size_t>(count) * VectorDimensions * sizeof(std::int16_t);
    std::size_t labelsStart = vectorsStart + vectorsSize;
    std::size_t labelsSize = count;
    std::size_t expectedSize = labelsStart + labelsSize;

    if (mappedSize != expectedSize) {
        munmap(mapping, mappedSize);
        throw std::runtime_error("Tamanho inesperado do arquivo IVF: " + path);
    }

    const auto* offsets = reinterpret_cast<const std::uint32_t*>(data + offsetsStart);
    if (offsets[0] != 0 || offsets[nlist] != count) {
        munmap(mapping, mappedSize);
        throw std::runtime_error("Offsets invalidos no arquivo IVF: " + path);
    }

    ReferenceStore store;
    store.count = count;
    store.nlist = nlist;
    store.mappedSize = mappedSize;
    store.mappedData = data;
    store.centroids = reinterpret_cast<const std::int16_t*>(data + HeaderSize);
    store.offsets = offsets;
    store.vectors = reinterpret_cast<const std::int16_t*>(data + vectorsStart);
    store.labels = data + labelsStart;

    std::cout << "Carregadas " << store.count
              << " referencias IVF em " << store.nlist << " listas\n";
    return store;
}

const ReferenceStore& getReferences(){
    static const ReferenceStore references = loadIvfReferences(
        readStringEnv("RB_REFERENCES_IVF_PATH", "resources/references.ivf"));
    return references;
}

void warmReferences() {
    const ReferenceStore& refs = getReferences();

    // Toca uma posição por página para forçar a carga síncrona de todo o índice
    // mmap antes de aceitarmos tráfego.
    volatile std::uint64_t sink = 0;
    constexpr std::size_t pageSize = 4096;
    for (std::size_t offset = 0; offset < refs.mappedSize; offset += pageSize) {
        sink += refs.mappedData[offset];
    }

    // Algumas queries dummy aquecem caches de instrução/dados e o branch predictor
    // do caminho de busca antes da primeira requisição real.
    std::array<float, 14> dummy{};
    dummy.fill(0.5f);
    for (int i = 0; i < 64; ++i) {
        FraudScoreResult result = transactionIsApproved(dummy);
        sink += result.approved ? 1u : 0u;
    }

    (void)sink;
}

int euclideanDistance(const std::array<std::int16_t, 14>& queryVector, const std::int16_t* referenceVector) {
    return euclideanDistanceScalar(queryVector.data(), referenceVector);
}

DistanceValidationResult validateDistanceImplementations(const std::array<std::int16_t, 14>& queryVector, std::size_t sampleCount) {
    const ReferenceStore& refs = getReferences();
    __m256i queryWords = paddedQueryWords(queryVector);
    std::size_t checked = std::min<std::size_t>(sampleCount, refs.count);

    DistanceValidationResult result;
    result.checked = checked;

    for (std::size_t i = 0; i < checked; ++i) {
        const std::int16_t* vector = refs.vectors + i * VectorDimensions;
        int scalarDistance = euclideanDistance(queryVector, vector);
        int avx2Distance = euclideanDistanceAvx2(queryWords, vector);

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

std::array<bool, 5> approximateNearestFraudLabels(const std::array<std::int16_t, 14>& queryVector){
    const ReferenceStore& refs = getReferences();
    AnnSearchParams params{configuredNProbe(refs.nlist), configuredCandidateCap()};
    return searchApproximateNeighbors(queryVector, params).fraudLabels;
}

FraudScoreResult transactionIsApproved(const std::array<float, 14>& queryVector){
    const ReferenceStore& refs = getReferences();
    std::array<std::int16_t, 14> quantQuery = quantizeVector(queryVector);
    AnnSearchParams firstPassParams{configuredNProbe(refs.nlist), configuredCandidateCap()};
    AnnSearchResult searchResult = searchApproximateNeighbors(quantQuery, firstPassParams);

    if (searchResult.fraudCount == 2 || searchResult.fraudCount == 3) {
        AnnSearchParams fallbackParams{
            configuredFallbackNProbe(refs.nlist, firstPassParams.nprobe),
            configuredFallbackCandidateCap(firstPassParams.candidateCap)
        };
        if (fallbackParams.nprobe > firstPassParams.nprobe || fallbackParams.candidateCap > firstPassParams.candidateCap) {
            searchResult = searchApproximateNeighbors(quantQuery, fallbackParams);
        }
    }

    float fraudScore = searchResult.fraudCount / 5.0f;
    bool approved = fraudScore < 0.6f;

    return FraudScoreResult{approved, fraudScore};
}
