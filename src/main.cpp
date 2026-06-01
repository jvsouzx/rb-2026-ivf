#include "vectorize.hpp"
#include "vectorSearch.hpp"
#include <uwebsockets/App.h>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

int main(){
    getReferences();
    getMccRisk();

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

                nlohmann::json response;
                response["approved"] = fraudScoreResult.approved;
                response["fraud_score"] = fraudScoreResult.fraudScore;

                res->writeStatus("200 OK");
                res->writeHeader("content-type", "application/json");
                res->end(response.dump());
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
