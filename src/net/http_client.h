#pragma once
#include <string>

#include "core/types.h"
#include "net/address.h"

namespace morton {

struct HttpFetch {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

/// One-shot blocking HTTP/1.1 request. Used for matchmaker calls, which happen
/// once per session and never on the tick path.
HttpFetch http_fetch(const Address& server, const std::string& method, const std::string& target,
                     const std::string& body = "", u32 timeout_ms = 3000);

/// Reads a value out of a flat JSON object without pulling in a parser. Only
/// used for the small, self-produced matchmaker responses.
std::string json_lookup(const std::string& json, const std::string& key);

}  // namespace morton
