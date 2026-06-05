#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "quantize.hpp"

using json = nlohmann::json;

namespace {
    constexpr std::uint32_t IvfReferencesMagic = 0x56494252; // "RBIV"
    constexpr std::uint32_t IvfVersion = 3; // v3: vetores de referencia em int16 (antes uint8)
    constexpr int VectorDimensions = 14;

    struct IvfConfig {
        std::uint32_t nlist = 1024;
        std::uint32_t sampleSize = 20000;
        std::uint32_t iterations = 8;
        std::uint32_t seed = 42;
    };

    struct ReferenceData {
        std::uint32_t count = 0;
        std::vector<std::int16_t> quantizedVectors;
        std::vector<std::uint8_t> labels;
        std::vector<float> sampleVectors;
    };

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

    std::string readStringEnv(const char* name, const std::string& fallback) {
        const char* value = std::getenv(name);
        if (value == nullptr || value[0] == '\0') {
            return fallback;
        }

        return value;
    }

    IvfConfig readIvfConfig() {
        IvfConfig config;
        config.nlist = readUIntEnv("RB_IVF_NLIST", config.nlist);
        config.sampleSize = readUIntEnv("RB_IVF_SAMPLE_SIZE", config.sampleSize);
        config.iterations = readUIntEnv("RB_IVF_ITERATIONS", config.iterations);
        config.seed = readUIntEnv("RB_IVF_SEED", config.seed);
        return config;
    }

    template <typename T>
    void writeValue(std::ofstream& output, T value) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    std::size_t align4(std::size_t value) {
        return (value + 3) & ~static_cast<std::size_t>(3);
    }

    int squaredDistanceFloat(const float* lhs, const float* rhs) {
        float distance = 0.0f;
        for (int dim = 0; dim < VectorDimensions; ++dim) {
            float diff = lhs[dim] - rhs[dim];
            distance += diff * diff;
        }

        return static_cast<int>(distance * 1000000.0f);
    }

    int squaredDistanceToScaledCentroid(const std::int16_t* vector, const std::int16_t* centroid) {
        int distance = 0;
        for (int dim = 0; dim < VectorDimensions; ++dim) {
            int diff = static_cast<int>(vector[dim]) - static_cast<int>(centroid[dim]);
            distance += diff * diff;
        }

        return distance;
    }

    std::uint32_t nearestFloatCentroid(const float* vector, const std::vector<float>& centroids, std::uint32_t nlist) {
        std::uint32_t best = 0;
        int bestDistance = std::numeric_limits<int>::max();

        for (std::uint32_t centroid = 0; centroid < nlist; ++centroid) {
            int distance = squaredDistanceFloat(vector, centroids.data() + static_cast<std::size_t>(centroid) * VectorDimensions);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = centroid;
            }
        }

        return best;
    }

    std::uint32_t nearestScaledCentroid(const std::int16_t* vector, const std::vector<std::int16_t>& centroids, std::uint32_t nlist) {
        std::uint32_t best = 0;
        int bestDistance = std::numeric_limits<int>::max();

        for (std::uint32_t centroid = 0; centroid < nlist; ++centroid) {
            int distance = squaredDistanceToScaledCentroid(vector, centroids.data() + static_cast<std::size_t>(centroid) * VectorDimensions);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = centroid;
            }
        }

        return best;
    }

    ReferenceData extractReferenceData(const json& references, std::uint32_t requestedSampleSize, std::uint32_t seed) {
        if (!references.is_array() || references.empty()) {
            throw std::runtime_error("references.json.gz nao contem referencias");
        }

        const auto count = static_cast<std::uint32_t>(references.size());
        const std::uint32_t sampleSize = std::min<std::uint32_t>(requestedSampleSize, count);

        ReferenceData data;
        data.count = count;
        data.quantizedVectors.resize(static_cast<std::size_t>(count) * VectorDimensions);
        data.labels.resize(count);
        data.sampleVectors.resize(static_cast<std::size_t>(sampleSize) * VectorDimensions);

        std::mt19937 rng(seed);
        std::uniform_int_distribution<std::uint32_t> reservoirPick;

        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& item = references[i];
            const auto& vector = item["vector"];
            std::array<float, VectorDimensions> values{};

            for (int dim = 0; dim < VectorDimensions; ++dim) {
                float value = vector[dim].get<float>();
                values[dim] = value;
                data.quantizedVectors[static_cast<std::size_t>(i) * VectorDimensions + dim] = quantize(value);
            }

            std::string label = item["label"].get<std::string>();
            data.labels[i] = label == "fraud" ? 1 : 0;

            std::uint32_t sampleSlot = i;
            if (i >= sampleSize) {
                sampleSlot = reservoirPick(rng, decltype(reservoirPick)::param_type(0, i));
            }

            if (sampleSlot < sampleSize) {
                std::copy(values.begin(), values.end(), data.sampleVectors.begin() + static_cast<std::size_t>(sampleSlot) * VectorDimensions);
            }
        }

        return data;
    }

    std::vector<float> trainKMeans(const std::vector<float>& sampleVectors, std::uint32_t sampleSize, std::uint32_t nlist, std::uint32_t iterations, std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::vector<std::uint32_t> shuffled(sampleSize);
        for (std::uint32_t i = 0; i < sampleSize; ++i) {
            shuffled[i] = i;
        }
        std::shuffle(shuffled.begin(), shuffled.end(), rng);

        std::vector<float> centroids(static_cast<std::size_t>(nlist) * VectorDimensions);
        for (std::uint32_t centroid = 0; centroid < nlist; ++centroid) {
            const float* source = sampleVectors.data() + static_cast<std::size_t>(shuffled[centroid]) * VectorDimensions;
            std::copy(source, source + VectorDimensions, centroids.begin() + static_cast<std::size_t>(centroid) * VectorDimensions);
        }

        std::vector<float> sums(static_cast<std::size_t>(nlist) * VectorDimensions);
        std::vector<std::uint32_t> counts(nlist);
        std::uniform_int_distribution<std::uint32_t> samplePick(0, sampleSize - 1);

        for (std::uint32_t iteration = 0; iteration < iterations; ++iteration) {
            std::fill(sums.begin(), sums.end(), 0.0f);
            std::fill(counts.begin(), counts.end(), 0);

            for (std::uint32_t sample = 0; sample < sampleSize; ++sample) {
                const float* vector = sampleVectors.data() + static_cast<std::size_t>(sample) * VectorDimensions;
                std::uint32_t centroid = nearestFloatCentroid(vector, centroids, nlist);
                counts[centroid]++;

                float* sum = sums.data() + static_cast<std::size_t>(centroid) * VectorDimensions;
                for (int dim = 0; dim < VectorDimensions; ++dim) {
                    sum[dim] += vector[dim];
                }
            }

            std::uint32_t emptyClusters = 0;
            for (std::uint32_t centroid = 0; centroid < nlist; ++centroid) {
                float* target = centroids.data() + static_cast<std::size_t>(centroid) * VectorDimensions;
                if (counts[centroid] == 0) {
                    const float* replacement = sampleVectors.data() + static_cast<std::size_t>(samplePick(rng)) * VectorDimensions;
                    std::copy(replacement, replacement + VectorDimensions, target);
                    emptyClusters++;
                    continue;
                }

                const float* sum = sums.data() + static_cast<std::size_t>(centroid) * VectorDimensions;
                float invCount = 1.0f / static_cast<float>(counts[centroid]);
                for (int dim = 0; dim < VectorDimensions; ++dim) {
                    target[dim] = sum[dim] * invCount;
                }
            }

            std::cout << "kmeans.iteration=" << (iteration + 1)
                      << " empty_clusters=" << emptyClusters << "\n";
        }

        return centroids;
    }

    std::vector<std::int16_t> scaleCentroids(const std::vector<float>& centroids) {
        std::vector<std::int16_t> scaled(centroids.size());
        for (std::size_t i = 0; i < centroids.size(); ++i) {
            int value = static_cast<int>(std::lround(centroids[i] / kQuantizeScale));
            scaled[i] = static_cast<std::int16_t>(std::clamp(value, -kQuantizeLevels, kQuantizeLevels));
        }

        return scaled;
    }

    void writeIvfReferences(const ReferenceData& data, const std::vector<std::int16_t>& scaledCentroids, std::uint32_t nlist, const std::string& path) {
        std::vector<std::uint32_t> assignments(data.count);
        std::vector<std::uint32_t> offsets(static_cast<std::size_t>(nlist) + 1, 0);

        for (std::uint32_t i = 0; i < data.count; ++i) {
            const std::int16_t* vector = data.quantizedVectors.data() + static_cast<std::size_t>(i) * VectorDimensions;
            std::uint32_t centroid = nearestScaledCentroid(vector, scaledCentroids, nlist);
            assignments[i] = centroid;
            offsets[centroid + 1]++;
        }

        for (std::uint32_t i = 1; i <= nlist; ++i) {
            offsets[i] += offsets[i - 1];
        }

        std::vector<std::uint32_t> writePositions = offsets;
        std::vector<std::int16_t> sortedVectors(data.quantizedVectors.size());
        std::vector<std::uint8_t> sortedLabels(data.labels.size());

        for (std::uint32_t i = 0; i < data.count; ++i) {
            std::uint32_t centroid = assignments[i];
            std::uint32_t position = writePositions[centroid]++;
            const std::int16_t* source = data.quantizedVectors.data() + static_cast<std::size_t>(i) * VectorDimensions;
            std::int16_t* target = sortedVectors.data() + static_cast<std::size_t>(position) * VectorDimensions;
            std::copy(source, source + VectorDimensions, target);
            sortedLabels[position] = data.labels[i];
        }

        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Erro ao criar " + path);
        }

        writeValue(output, IvfReferencesMagic);
        writeValue(output, IvfVersion);
        writeValue(output, data.count);
        writeValue(output, static_cast<std::uint32_t>(VectorDimensions));
        writeValue(output, nlist);
        output.write(reinterpret_cast<const char*>(scaledCentroids.data()), static_cast<std::streamsize>(scaledCentroids.size() * sizeof(std::int16_t)));

        std::size_t centroidBytes = scaledCentroids.size() * sizeof(std::int16_t);
        std::size_t offsetsStart = align4((sizeof(std::uint32_t) * 5) + centroidBytes);
        std::size_t paddingSize = offsetsStart - ((sizeof(std::uint32_t) * 5) + centroidBytes);
        if (paddingSize > 0) {
            const char padding[3] = {};
            output.write(padding, static_cast<std::streamsize>(paddingSize));
        }

        output.write(reinterpret_cast<const char*>(offsets.data()), static_cast<std::streamsize>(offsets.size() * sizeof(std::uint32_t)));
        output.write(reinterpret_cast<const char*>(sortedVectors.data()), static_cast<std::streamsize>(sortedVectors.size() * sizeof(std::int16_t)));
        output.write(reinterpret_cast<const char*>(sortedLabels.data()), static_cast<std::streamsize>(sortedLabels.size()));

        if (!output) {
            throw std::runtime_error("Erro ao escrever " + path);
        }

        std::uint32_t maxListSize = 0;
        for (std::uint32_t i = 0; i < nlist; ++i) {
            maxListSize = std::max(maxListSize, offsets[i + 1] - offsets[i]);
        }

        std::cout << "Gerado " << path
                  << " com nlist=" << nlist
                  << " max_list_size=" << maxListSize << "\n";
    }
}

int main() {
    try {
        IvfConfig config = readIvfConfig();

        const std::string inputPath = readStringEnv("RB_REFERENCES_INPUT", "resources/references.json.gz");
        const std::string ivfOutputPath = readStringEnv("RB_REFERENCES_IVF_OUTPUT", "resources/references.ivf");

        std::ifstream input(inputPath, std::ios::binary);
        if (!input) {
            std::cerr << "Erro ao abrir " << inputPath << "\n";
            return 1;
        }

        boost::iostreams::filtering_istream gzipStream;
        gzipStream.push(boost::iostreams::gzip_decompressor());
        gzipStream.push(input);

        json references = json::parse(gzipStream);
        std::uint32_t count = static_cast<std::uint32_t>(references.size());

        config.nlist = std::clamp<std::uint32_t>(config.nlist, 1, count);
        config.sampleSize = std::clamp<std::uint32_t>(config.sampleSize, config.nlist, count);
        config.iterations = std::max<std::uint32_t>(config.iterations, 1);

        std::cout << "preprocess.count=" << count << "\n";
        std::cout << "ivf.nlist=" << config.nlist << "\n";
        std::cout << "ivf.sample_size=" << config.sampleSize << "\n";
        std::cout << "ivf.iterations=" << config.iterations << "\n";

        ReferenceData data = extractReferenceData(references, config.sampleSize, config.seed);
        references = json();

        std::vector<float> centroids = trainKMeans(data.sampleVectors, config.sampleSize, config.nlist, config.iterations, config.seed);
        std::vector<std::int16_t> scaledCentroids = scaleCentroids(centroids);
        writeIvfReferences(data, scaledCentroids, config.nlist, ivfOutputPath);

        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
