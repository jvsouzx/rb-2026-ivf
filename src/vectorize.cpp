#include "vectorize.hpp"

#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <fstream>

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

    int parse2(std::string_view value, std::size_t offset) {
        return (value[offset] - '0') * 10 + (value[offset + 1] - '0');
    }

    int parse4(std::string_view value, std::size_t offset) {
        return (value[offset] - '0') * 1000
            + (value[offset + 1] - '0') * 100
            + (value[offset + 2] - '0') * 10
            + (value[offset + 3] - '0');
    }

    int hourOfDay(std::string_view timestamp){
        return parse2(timestamp, 11);
    }

    int dayOfWeek(std::string_view timestamp) {
        int y = parse4(timestamp, 0);
        int m = parse2(timestamp, 5);
        int d = parse2(timestamp, 8);

        static int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

        if (m < 3) {
            y -= 1;
        }

        int dow = (y + y / 4 - y / 100 + y / 400 + offsets[m - 1] + d) % 7;

        return (dow + 6) % 7;
    }

    int daysFromCivil(int year, unsigned month, unsigned day) {
        year -= month <= 2;
        const int era = (year >= 0 ? year : year - 399) / 400;
        const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
        const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
        return era * 146097 + static_cast<int>(dayOfEra) - 719468;
    }

    std::int64_t secondsSinceEpoch(std::string_view timestamp) {
        int year = parse4(timestamp, 0);
        unsigned month = static_cast<unsigned>(parse2(timestamp, 5));
        unsigned day = static_cast<unsigned>(parse2(timestamp, 8));
        int hour = parse2(timestamp, 11);
        int minute = parse2(timestamp, 14);
        int second = parse2(timestamp, 17);

        return static_cast<std::int64_t>(daysFromCivil(year, month, day)) * 86400
            + hour * 3600
            + minute * 60
            + second;
    }

    int minutesBetween(std::string_view from, std::string_view to) {
        return static_cast<int>((secondsSinceEpoch(to) - secondsSinceEpoch(from)) / 60);
    }
}

std::array<float, 14> vectorizeTransaction(std::string_view body) {
    json payload = json::parse(body);
    std::array<float, 14> vector{};

    float transactionAmount = payload["transaction"]["amount"].get<float>();
    vector[0] = normalize(transactionAmount, 10000.0f);

    int transactionInstallments = payload["transaction"]["installments"].get<int>();
    vector[1] = normalize(transactionInstallments, 12.0f);

    const std::string& transactionRequestedAt = payload["transaction"]["requested_at"].get_ref<const std::string&>();
    vector[3] = hourOfDay(transactionRequestedAt) / 23.0f;
    vector[4] = dayOfWeek(transactionRequestedAt) / 6.0f;

    float customerAvgAmount = payload["customer"]["avg_amount"].get<float>();
    vector[2] = std::clamp((transactionAmount / customerAvgAmount)/10.0f, 0.0f, 1.0f);

    int customerTxCount24h = payload["customer"]["tx_count_24h"].get<int>();
    vector[8] = normalize(customerTxCount24h, 20.0f);

    const std::string& merchantId = payload["merchant"]["id"].get_ref<const std::string&>();
    bool merchantIsKnown = false;
    for (const auto& knownMerchant : payload["customer"]["known_merchants"]) {
        if (knownMerchant.get_ref<const std::string&>() == merchantId) {
            merchantIsKnown = true;
            break;
        }
    }
    vector[11] = merchantIsKnown ? 0.0f : 1.0f;

    const std::string& merchantMcc = payload["merchant"]["mcc"].get_ref<const std::string&>();
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
        const std::string& lastTransactionTimestamp = lastTransaction["timestamp"].get_ref<const std::string&>();
        float lastTransactionKmFromCurrent = lastTransaction["km_from_current"].get<float>();  

        int minutes = minutesBetween(lastTransactionTimestamp, transactionRequestedAt);

        vector[5] = normalize(minutes, 1440.0f);
        vector[6] = normalize(lastTransactionKmFromCurrent, 1000.0f);
    }
    return vector;
}
