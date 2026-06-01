#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <cstdint>
#include <iostream>
#include "quantize.hpp"

using json = nlohmann::json;

int main() {
    std::ifstream input("resources/references.json.gz", std::ios::binary);
    if (!input) {
        std::cerr << "Erro ao abrir references.json.gz\n";
        return 1;
    }
    boost::iostreams::filtering_istream gzipStream;
    gzipStream.push(boost::iostreams::gzip_decompressor());
    gzipStream.push(input);

    json references = json::parse(gzipStream);
    std::ofstream output("resources/references.bin", std::ios::binary);
    if (!output) {
        std::cerr << "Erro ao criar references.bin\n";
        return 1;
    }

    uint32_t magic = 0x32464252; // "RBF2"
    uint32_t count = static_cast<uint32_t>(references.size());

    output.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    output.write(reinterpret_cast<const char*>(&count), sizeof(count));

    for (const auto& item : references) {
        const auto& vector = item["vector"];
        for (int i = 0; i < 14; ++i) {
            float value = vector[i].get<float>();
            uint8_t quantized = quantize(value);
            output.write(reinterpret_cast<const char*>(&quantized), sizeof(quantized));
        }
    }

    for (const auto& item : references) {
        std::string label = item["label"].get<std::string>();
        uint8_t fraud = label == "fraud" ? 1 : 0;
        output.write(reinterpret_cast<const char*>(&fraud), sizeof(fraud));
    }

    std::cout << "Gerado references.bin com " << count << " referencias\n";
    return 0;

}
