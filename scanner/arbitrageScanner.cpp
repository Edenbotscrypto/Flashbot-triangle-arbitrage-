/*
 * arbitrageScanner.cpp — high-level skeleton
 * ==========================================
 * - Fetches on-chain pool reserves (placeholder)
 * - Evaluates triangular paths in parallel
 * - Emits calldata to the Solidity executor when profitable
 *
 * Build: g++ -std=c++17 arbitrageScanner.cpp -I. -lcpr -lsqlite3 -lboost_system -lpthread -o scanner
 */

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <cstdlib>

#include <cpr/cpr.h>              // HTTP client
#include <nlohmann/json.hpp>      // JSON parsing

#include "eth.hpp"
#include "flashbots.hpp"
#include "trade_logger.hpp"
#include <boost/multiprecision/cpp_int.hpp>
#include <cmath>
#include "telegram.hpp"

using json = nlohmann::json;
using namespace std::chrono_literals;

using boost::multiprecision::uint256_t;

struct Token {
    std::string symbol;
    std::string address;
    uint8_t     decimals;
};

struct Hop {
    std::string dex;      // v2, v3, curve
    std::string pool;     // pair/pool address
    int       i;          // curve index (optional)
    int       j;
};

struct Opportunity {
    std::vector<Hop> hops;
    uint256_t        profitWei;
    uint256_t        gasCostWei;
    uint256_t        loanSize;
};

class ArbitrageScanner {
public:
    explicit ArbitrageScanner(std::string rpc) : rpcUrl_(std::move(rpc)) {}

    void addToken(const Token& t) { tokens_.push_back(t); }

    void runForever() {
        while (true) {
            scanOnce();
            std::this_thread::sleep_for(500ms); // tune as needed
        }
    }

private:
    const std::string rpcUrl_;
    std::vector<Token> tokens_;
    std::mutex logMtx_;
    logger::TradeLogger db_{"db/trades.sqlite"};

    // ------------ Utility Math ------------
    static uint256_t getAmountOutV2(uint256_t amountIn, uint256_t reserveIn, uint256_t reserveOut) {
        // UniswapV2 formula with 0.3% fee
        uint256_t amountInWithFee = amountIn * 997;
        uint256_t numerator = amountInWithFee * reserveOut;
        uint256_t denominator = reserveIn * 1000 + amountInWithFee;
        return numerator / denominator;
    }

    // Placeholder V3 price impact: assume 0.3% fee–only for demonstration
    static uint256_t getAmountOutV3(uint256_t amountIn, double price, int decimalsIn, int decimalsOut) {
        double inFloat = amountIn.convert_to<double>() / pow(10, decimalsIn);
        double outFloat = inFloat * price * 0.997; // assume 0.3% fee
        uint256_t outWei = static_cast<uint256_t>(outFloat * pow(10, decimalsOut));
        return outWei;
    }

    Opportunity evaluateTriangle(const Token& t0, const Token& t1, const Token& t2, uint256_t initAmount) {
        // Example: all hops on UniswapV2 (factory hard-coded) – extend as needed
        const std::string pair01 = "0x0000000000000000000000000000000000000000"; // TODO compute via factory
        const std::string pair12 = "0x0000000000000000000000000000000000000000";
        const std::string pair20 = "0x0000000000000000000000000000000000000000";

        using eth::getReservesV2;
        auto r01 = getReservesV2(rpcUrl_, pair01);
        auto r12 = getReservesV2(rpcUrl_, pair12);
        auto r20 = getReservesV2(rpcUrl_, pair20);

        uint256_t a1 = getAmountOutV2(initAmount, r01.reserve0, r01.reserve1);
        uint256_t a2 = getAmountOutV2(a1, r12.reserve0, r12.reserve1);
        uint256_t a3 = getAmountOutV2(a2, r20.reserve1, r20.reserve0);

        Opportunity opp;
        opp.loanSize = initAmount;
        opp.profitWei = a3 > initAmount ? a3 - initAmount : 0;
        opp.gasCostWei = 300000 * uint256_t(eth::hexToUint(eth::strip0x(eth::rpcCall(rpcUrl_, "eth_gasPrice", {}))));
        return opp;
    }

    void submitFlashbots(const Opportunity& opp) {
        // Build dummy calldata – placeholder
        std::string calldata = "0x"; // TODO ABI encode executeArb
        flashbots::SignedTx tx1{calldata};
        flashbots::Bundle bundle{{tx1}, /*target block*/ 0, opp.profitWei.convert_to<uint64_t>()/50};
        std::string pk = std::getenv("PRIVATE_KEY");
        if (pk.empty()) return;
        flashbots::sendBundle("https://relay.flashbots.net", bundle, pk);
    }

    void scanOnce() {
        std::vector<std::thread> threads;
        std::atomic<size_t> idx{0};
        size_t nWorkers = std::thread::hardware_concurrency();

        auto worker = [&]() {
            size_t i;
            while ((i = idx.fetch_add(1)) < tokens_.size()) {

                // Evaluate triangles starting with token i (t0)
                const Token& t0 = tokens_[i];
                for (size_t j = 0; j < tokens_.size(); ++j) if (j != i) {
                    for (size_t k = 0; k < tokens_.size(); ++k) if (k != i && k != j) {
                        auto opp = evaluateTriangle(t0, tokens_[j], tokens_[k], uint256_t(1'000'000) * pow(10, t0.decimals));
                        if (opp.profitWei > opp.gasCostWei) {
                            std::lock_guard<std::mutex> g(logMtx_);
                            std::string msg = "PROFIT " + t0.symbol + "->" + tokens_[j].symbol + "->" + tokens_[k].symbol + "->" + t0.symbol + " profit=" + opp.profitWei.convert_to<std::string>();
                            std::cout << msg << std::endl;
                            telegram::send(msg);

                            submitFlashbots(opp);
                            db_.log(time(nullptr), t0.symbol+"-"+tokens_[j].symbol+"-"+tokens_[k].symbol, "", "", 0, "", opp.profitWei.convert_to<std::string>(), "");
                        }
                    }
                }
            }
        };

        for (size_t i = 0; i < nWorkers; ++i) threads.emplace_back(worker);
        for (auto& th : threads) th.join();
    }
};

int main() {
    const char* rpc = std::getenv("RPC_URL");
    if (!rpc) {
        std::cerr << "Missing RPC_URL env var" << std::endl;
        return 1;
    }

    ArbitrageScanner scanner(rpc);
    scanner.addToken({"USDC", "0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48", 6});
    scanner.addToken({"DAI",  "0x6B175474E89094C44Da98b954EedeAC495271d0F", 18});
    scanner.addToken({"WETH", "0xC02aaA39b223FE8D0a0e5C4F27eAD9083C756Cc2", 18});

    scanner.runForever();
    return 0;
}