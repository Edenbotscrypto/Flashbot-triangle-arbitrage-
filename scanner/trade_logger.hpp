#pragma once
#include <sqlite3.h>
#include <string>
#include <iostream>

namespace logger {

class TradeLogger {
    sqlite3* db_{};
public:
    explicit TradeLogger(const std::string& path="db/trades.sqlite") {
        if (sqlite3_open(path.c_str(), &db_)) {
            std::cerr << "Cannot open DB: " << sqlite3_errmsg(db_) << std::endl;
            db_ = nullptr;
        } else {
            const char* ddl = "CREATE TABLE IF NOT EXISTS trades (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, path TEXT, amountIn TEXT, amountOut TEXT, gasUsed INTEGER, tipWei TEXT, profitWei TEXT, txHash TEXT);";
            char* err=nullptr; sqlite3_exec(db_, ddl, nullptr, nullptr, &err);
        }
    }
    ~TradeLogger() { if (db_) sqlite3_close(db_); }

    void log(int64_t ts, const std::string& path, const std::string& amountIn, const std::string& amountOut, uint64_t gasUsed, const std::string& tipWei, const std::string& profitWei, const std::string& txHash) {
        if (!db_) return;
        std::string sql = "INSERT INTO trades (ts,path,amountIn,amountOut,gasUsed,tipWei,profitWei,txHash) VALUES (" + std::to_string(ts) + ", '" + path + "', '" + amountIn + "', '" + amountOut + "', " + std::to_string(gasUsed) + ", '" + tipWei + "', '" + profitWei + "', '" + txHash + "');";
        char* err=nullptr; sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    }
};

} // namespace logger