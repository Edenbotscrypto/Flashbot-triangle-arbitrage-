#pragma once
#include <string>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <vector>

using json = nlohmann::json;

namespace flashbots {

struct SignedTx {
    std::string rawTx; // 0x-prefixed RLP encoded
};

struct Bundle {
    std::vector<SignedTx> txs;
    uint64_t targetBlockNumber;
    uint64_t coinbaseTipWei; // value to pay miner in wei
};

inline std::string sendBundle(const std::string& relayUrl, const Bundle& bundle, const std::string& signKey) {
    json params;
    std::vector<std::string> txsHex;
    for (auto& t : bundle.txs) txsHex.push_back(t.rawTx);

    json bundleObj = {
        {"txs", txsHex},
        {"blockNumber", "0x" + std::hex + std::to_string(bundle.targetBlockNumber)},
        {"minTimestamp", 0},
        {"maxTimestamp", 0},
        {"revertingTxHashes", json::array()}
    };

    params.push_back(bundleObj);

    json request = {
        {"jsonrpc", "2.0"},
        {"method", "eth_sendBundle"},
        {"params", params},
        {"id", "1"}
    };

    auto res = cpr::Post(cpr::Url{relayUrl}, cpr::Body{request.dump()}, cpr::Header{{"Content-Type","application/json"}, {"X-Flashbots-Signature", signKey}});
    if (res.error) throw std::runtime_error("Flashbots error: " + res.error.message);
    return res.text;
}

} // namespace flashbots