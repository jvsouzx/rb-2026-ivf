#include "vectorize.hpp"

#include <nlohmann/json.hpp>
#include <array>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <fstream>
#include <vector>
#include <ctime>

using json = nlohmann::json;

const std::unordered_map<std::string, float>& getMccRisk() {
    static const std::unordered_map<std::string, float> riskMap = []() {
        std::ifstream file("resources/mcc_risk.json");
        if (!file) {
            throw std::runtime_error("Erro ao abrir resources/mcc_risk.json");
        }
        json mccRisk = json::parse(file);

        std::unordered_map<std::string, float> map;
        map.reserve(mccRisk.size());
        for (auto it = mccRisk.begin(); it != mccRisk.end(); ++it) {
            map.emplace(it.key(), it.value().get<float>());
        }
        return map;
    }();
    return riskMap;
}

namespace {

    float normalize(float value, float maxValue){
        return std::clamp((value/maxValue), 0.0f, 1.0f);
    }

    int hourOfDay(std::string_view timestamp){
        return std::stoi(std::string(timestamp.substr(11, 2)));
    }

    int dayOfWeek(std::string_view timestamp) {
        int y = std::stoi(std::string(timestamp.substr(0, 4)));
        int m = std::stoi(std::string(timestamp.substr(5, 2)));
        int d = std::stoi(std::string(timestamp.substr(8, 2)));

        static int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

        if (m < 3) {
            y -= 1;
        }

        int dow = (y + y / 4 - y / 100 + y / 400 + offsets[m - 1] + d) % 7;

        return (dow + 6) % 7;
    }

    std::tm parseTimestamp(std::string_view timestamp) {
        std::tm tm{};

        tm.tm_year = std::stoi(std::string(timestamp.substr(0, 4))) - 1900;
        tm.tm_mon  = std::stoi(std::string(timestamp.substr(5, 2))) - 1;
        tm.tm_mday = std::stoi(std::string(timestamp.substr(8, 2)));
        tm.tm_hour = std::stoi(std::string(timestamp.substr(11, 2)));
        tm.tm_min  = std::stoi(std::string(timestamp.substr(14, 2)));
        tm.tm_sec  = std::stoi(std::string(timestamp.substr(17, 2)));

        return tm;
    }

    int minutesBetween(std::string_view from, std::string_view to) {
        std::tm fromTm = parseTimestamp(from);
        std::tm toTm = parseTimestamp(to);

        std::time_t fromTime = timegm(&fromTm);
        std::time_t toTime = timegm(&toTm);

        return static_cast<int>((toTime - fromTime) / 60);
    }
}

std::array<float, 14> vectorizeTransaction(std::string_view body) {
    json payload = json::parse(body);
    std::array<float, 14> vector{};

    float transactionAmount = payload["transaction"]["amount"].get<float>();
    vector[0] = normalize(transactionAmount, 10000.0f);

    int transactionInstallments = payload["transaction"]["installments"].get<int>();
    vector[1] = normalize(transactionInstallments, 12.0f);

    std::string transactionRequestedAt = payload["transaction"]["requested_at"].get<std::string>();
    vector[3] = hourOfDay(transactionRequestedAt) / 23.0f;
    vector[4] = dayOfWeek(transactionRequestedAt) / 6.0f;

    float customerAvgAmount = payload["customer"]["avg_amount"].get<float>();
    vector[2] = std::clamp((transactionAmount / customerAvgAmount)/10.0f, 0.0f, 1.0f);

    int customerTxCount24h = payload["customer"]["tx_count_24h"].get<int>();
    vector[8] = normalize(customerTxCount24h, 20.0f);

    std::vector<std::string> customerKnownMerchants = payload["customer"]["known_merchants"].get<std::vector<std::string>>();
    std::string merchantId = payload["merchant"]["id"].get<std::string>();
    bool merchantIsKnown = std::find(customerKnownMerchants.begin(), customerKnownMerchants.end(), merchantId) != customerKnownMerchants.end();
    vector[11] = merchantIsKnown ? 0.0f : 1.0f;

    std::string merchantMcc = payload["merchant"]["mcc"].get<std::string>();
    const auto& mccRisk = getMccRisk();
    float risk = 0.5f;
    auto it = mccRisk.find(merchantMcc);
    if (it != mccRisk.end()) {
        risk = it->second;
    }
    vector[12] = risk;

    float merchantAvgAmount = payload["merchant"]["avg_amount"].get<float>();
    vector[13] = normalize(merchantAvgAmount, 10000.0f);

    bool terminalIsOnline = payload["terminal"]["is_online"].get<bool>();
    vector[9] = terminalIsOnline ? 1.0f : 0.0f;

    bool terminalCardPresent = payload["terminal"]["card_present"].get<bool>();
    vector[10] = terminalCardPresent ? 1.0f : 0.0f;

    float terminalKmFromHome = payload["terminal"]["km_from_home"].get<float>();
    vector[7] = normalize(terminalKmFromHome, 1000.0f);

    json lastTransaction = payload["last_transaction"];
    
    if (lastTransaction.is_null()){
        vector[5] = -1.0f;
        vector[6] = -1.0f;
    } else {
        std::string lastTransactionTimestamp = lastTransaction["timestamp"].get<std::string>();
        float lastTransactionKmFromCurrent = lastTransaction["km_from_current"].get<float>();  

        int minutes = minutesBetween(lastTransactionTimestamp, transactionRequestedAt);

        vector[5] = normalize(minutes, 1440.0f);
        vector[6] = normalize(lastTransactionKmFromCurrent, 1000.0f);
    }
    return vector;
}