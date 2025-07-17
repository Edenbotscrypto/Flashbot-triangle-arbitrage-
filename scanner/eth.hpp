#pragma once
#include <string>
#include <vector>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <boost/multiprecision/cpp_int.hpp>

using json = nlohmann::json;
using boost::multiprecision::uint256_t;

namespace eth {

inline std::string rpcCall(const std::string& rpcUrl, const std::string& method, const json& params, const std::string& id="1") {
    json request = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
        {"id", id}
    };
    auto res = cpr::Post(cpr::Url{rpcUrl}, cpr::Body{request.dump()}, cpr::Header{{"Content-Type","application/json"}});
    if (res.error) throw std::runtime_error("RPC error: " + res.error.message);
    auto j = json::parse(res.text);
    if (j.contains("error")) throw std::runtime_error("RPC returned error: " + j["error"].dump());
    return j["result"].get<std::string>();
}

inline std::string callContract(const std::string& rpcUrl, const std::string& to, const std::string& data, const std::string& block="latest") {
    json params = { { {"to", to}, {"data", data} }, block };
    return rpcCall(rpcUrl, "eth_call", params);
}

inline std::string strip0x(const std::string& s) {
    if (s.rfind("0x", 0) == 0) return s.substr(2);
    return s;
}

inline uint256_t hexToUint(const std::string& hex) {
    std::string clean = strip0x(hex);
    uint256_t val = 0;
    for (char c : clean) {
        val <<= 4;
        if (c >= '0' && c <= '9') val += (c - '0');
        else if (c >= 'a' && c <= 'f') val += 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') val += 10 + (c - 'A');
        else throw std::runtime_error("Invalid hex char");
    }
    return val;
}

// ABI-encoded selector helpers
inline std::string padLeft64(const std::string& hex) {
    std::string padded(64 - hex.size(), '0');
    return padded + hex;
}

inline std::string encodeAddress(const std::string& addr) { return padLeft64(strip0x(addr)); }
inline std::string encodeUint(uint64_t v) {
    std::stringstream ss; ss << std::hex << v;
    return padLeft64(ss.str());
}

// ---------------- Uniswap V2 -----------------
struct ReservesV2 { uint256_t reserve0; uint256_t reserve1; };

inline ReservesV2 getReservesV2(const std::string& rpc, const std::string& pairAddr) {
    // selector getReserves() => 0x0902f1ac
    std::string data = "0x0902f1ac";
    std::string result = callContract(rpc, pairAddr, data);
    std::string clean = strip0x(result);
    if (clean.size() < 192) throw std::runtime_error("Unexpected reserves len");
    ReservesV2 r;
    r.reserve0 = hexToUint(clean.substr(0, 64));
    r.reserve1 = hexToUint(clean.substr(64, 64));
    return r;
}

// ---------------- Uniswap V3 -----------------
inline int64_t hexToInt64(const std::string& h) {
    uint64_t val = std::stoull(h, nullptr, 16);
    return static_cast<int64_t>(val);
}

struct Slot0V3 { uint256_t sqrtPriceX96; int32_t tick; };

inline Slot0V3 getSlot0(const std::string& rpc, const std::string& pool) {
    std::string data = "0x3850c7bd"; // slot0()
    std::string result = callContract(rpc, pool, data);
    std::string clean = strip0x(result);
    Slot0V3 s{};
    s.sqrtPriceX96 = hexToUint(clean.substr(0,64));
    std::string tickHex = clean.substr(128, 64);
    s.tick = hexToInt64(tickHex);
    return s;
}

inline double tickToPrice(int32_t tick, int decimals0, int decimals1) {
    double ratio = pow(1.0001, tick);
    double scale = pow(10, decimals0 - decimals1);
    return ratio * scale;
}

// ---------------- Curve -----------------
inline uint256_t getDy(const std::string& rpc, const std::string& pool, int i, int j, uint256_t dx) {
    std::string selector = "0x555b73a6"; // get_dy_underlying(uint256,uint256,uint256)
    std::stringstream ss; ss << selector << encodeUint(i) << encodeUint(j) << padLeft64(dx.convert_to<std::string>());
    std::string res = callContract(rpc, pool, "0x" + ss.str());
    return hexToUint(res);
}

// ---------------- Factory helpers -----------------
inline std::string getPairV2(const std::string& rpc, const std::string& factory, const std::string& tokenA, const std::string& tokenB) {
    // getPair(address,address) => 0xe6a43905
    std::string selector = "0xe6a43905";
    std::string data = selector + encodeAddress(tokenA) + encodeAddress(tokenB);
    std::string res = callContract(rpc, factory, "0x" + data);
    return "0x" + padLeft64(strip0x(res)).substr(24); // last 40 hex chars -> address
}

inline std::string getPoolV3(const std::string& rpc, const std::string& factory, const std::string& tokenA, const std::string& tokenB, uint32_t fee) {
    // getPool(address,address,uint24) => 0x1698ee82
    std::string selector = "0x1698ee82";
    std::stringstream ss; ss << selector << encodeAddress(tokenA) << encodeAddress(tokenB) << encodeUint(fee);
    std::string res = callContract(rpc, factory, "0x" + ss.str());
    return "0x" + padLeft64(strip0x(res)).substr(24);
}

} // namespace eth