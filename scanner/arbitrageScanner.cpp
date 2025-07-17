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
#include <ctime>
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

    static uint256_t pow10(unsigned int exp) {
        uint256_t result = 1;
        for(unsigned int i=0;i<exp;++i) result *= 10;
        return result;
    }
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
        const std::string uniV2Factory = "0x5C69bEe701ef814a2B6a3EDD4B1652CB9cc5aA6f";
        const std::string uniV3Factory = "0x1F98431c8aD98523631AE4a59f267346ea31F984";

        using namespace eth;

        auto priceGas = hexToUint(strip0x(rpcCall(rpcUrl_, "eth_gasPrice", {})));

        uint256_t amount = initAmount;
        uint totalGas = 0;

        struct Step { const Token* in; const Token* out; } steps[3] = {{&t0,&t1},{&t1,&t2},{&t2,&t0}};

        for(int s=0;s<3;++s){
            const Token* A = steps[s].in;
            const Token* B = steps[s].out;
            std::string pair = getPairV2(rpcUrl_, uniV2Factory, A->address, B->address);
            if(pair != "0x0000000000000000000000000000000000000000"){
                auto r = getReservesV2(rpcUrl_, pair);
                uint256_t reserveIn, reserveOut;
                if (A->address < B->address){ reserveIn=r.reserve0; reserveOut=r.reserve1;} else {reserveIn=r.reserve1; reserveOut=r.reserve0;}
                amount = getAmountOutV2(amount, reserveIn, reserveOut);
                totalGas += 110000;
            } else {
                std::string pool = getPoolV3(rpcUrl_, uniV3Factory, A->address, B->address, 3000); // 0.3% fee
                if(pool != "0x0000000000000000000000000000000000000000"){
                    auto slot = getSlot0(rpcUrl_, pool);
                    double price = (slot.sqrtPriceX96.convert_to<long double>() * slot.sqrtPriceX96.convert_to<long double>()) / pow(2,192);
                    amount = getAmountOutV3(amount, price, A->decimals, B->decimals);
                    amount = amount * 997 / 1000; // 0.3% fee assumed
                    totalGas += 140000;
                } else {
                    // fallback skip if no liquidity
                    return Opportunity{}; // zero profit
                }
            }
        }

        uint256_t gasCostWei = priceGas * totalGas;
        Opportunity opp;
        opp.loanSize = initAmount;
        opp.profitWei = amount > initAmount ? amount - initAmount : 0;
        opp.gasCostWei = gasCostWei;
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
                        auto opp = evaluateTriangle(t0, tokens_[j], tokens_[k], uint256_t(1'000'000) * pow10(t0.decimals));
                        uint256_t net = opp.profitWei > opp.gasCostWei ? opp.profitWei - opp.gasCostWei : 0;
                        if (net > 0 && (net * 1000 / opp.loanSize) > 2) {
                            std::lock_guard<std::mutex> g(logMtx_);
                            std::string msg = "PROFIT " + t0.symbol + "->" + tokens_[j].symbol + "->" + tokens_[k].symbol + "->" + t0.symbol + " net=" + net.convert_to<std::string>();
                            std::cout << msg << std::endl;
                            telegram::send(msg);

                            submitFlashbots(opp);
                            db_.log(time(nullptr), t0.symbol+"-"+tokens_[j].symbol+"-"+tokens_[k].symbol, "", "", 0, "", net.convert_to<std::string>(), "");
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
    scanner.addToken({"WBTC", "0x2260FAC5E5542a773Aa44fBCfeDf7C193bc2C599", 8});

    scanner.runForever();
    return 0;
}