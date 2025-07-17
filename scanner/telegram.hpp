#pragma once
#include <string>
#include <cpr/cpr.h>
#include <cstdlib>

namespace telegram {
inline void send(const std::string& message) {
    const char* token = std::getenv("TELEGRAM_BOT_TOKEN");
    const char* chat  = std::getenv("TELEGRAM_CHAT_ID");
    if (!token || !chat) return;
    std::string url = "https://api.telegram.org/bot" + std::string(token) + "/sendMessage";
    cpr::Response r = cpr::Post(cpr::Url{url}, cpr::Payload{{"chat_id", chat}, {"text", message}});
    (void)r;
}
}