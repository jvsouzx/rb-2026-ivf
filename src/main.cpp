#include "vectorize.hpp"
#include "vectorSearch.hpp"
#include <uwebsockets/App.h>
#include <iostream>
#include <string>

namespace {
    const char* fraudScoreText(float score) {
        int bucket = static_cast<int>(score * 5.0f + 0.5f);
        switch (bucket) {
            case 0: return "0.0";
            case 1: return "0.2";
            case 2: return "0.4";
            case 3: return "0.6";
            case 4: return "0.8";
            default: return "1.0";
        }
    }
}

int main(){
    getReferences();
    getMccRisk();
    warmReferences();

    uWS::App app;

    app.get("/ready", [](auto *res, auto *req) {
        res->writeStatus("200 OK");
        res->writeHeader("content-type", "application/json");
        res->end(R"({"status":"ready"})");
    });
    
    app.post("/fraud-score", [](auto *res, auto *req){
        std::string body;

        res->onAborted([]() {});

        res->onData([res, body = std::move(body)](std::string_view chunk, bool isLast) mutable {
            body.append(chunk.data(), chunk.size());

            if (isLast){
                std::array<float, 14> normalizedVector = vectorizeTransaction(body);
                FraudScoreResult fraudScoreResult = transactionIsApproved(normalizedVector);

                std::string response;
                response.reserve(43);
                response.append(R"({"approved":)");
                response.append(fraudScoreResult.approved ? "true" : "false");
                response.append(R"(,"fraud_score":)");
                response.append(fraudScoreText(fraudScoreResult.fraudScore));
                response.push_back('}');

                res->writeStatus("200 OK");
                res->writeHeader("content-type", "application/json");
                res->end(response);
            }
        });
    });

    app.listen(8080, [](auto *socket){
        if(socket){
            std::cout << "Servidor rodando em http://localhost:8080\n";
        }
    });

    app.run();
    
    return 0;
}
