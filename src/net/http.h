#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "core/types.h"
#include "net/address.h"

namespace morton {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;
    std::string body;

    std::string query_value(const std::string& key, const std::string& fallback = "") const {
        auto it = query.find(key);
        return it == query.end() ? fallback : it->second;
    }
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;

    static HttpResponse text(const std::string& body) { return HttpResponse{200, "text/plain; charset=utf-8", body, {}}; }
    static HttpResponse json(const std::string& body) {
        return HttpResponse{200, "application/json", body, {}};
    }
    static HttpResponse html(const std::string& body) {
        return HttpResponse{200, "text/html; charset=utf-8", body, {}};
    }
    static HttpResponse not_found() {
        return HttpResponse{404, "text/plain; charset=utf-8", "not found", {}};
    }
    static HttpResponse bad_request(const std::string& reason) {
        return HttpResponse{400, "text/plain; charset=utf-8", reason, {}};
    }
};

/// Small blocking HTTP/1.1 server on a background thread, used for metrics
/// scraping and the matchmaker API. Request sizes are hard-capped because this
/// listener is reachable from outside the cluster.
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    ~HttpServer();

    bool start(const Address& bind);
    void stop();

    void route(std::string method, std::string path, Handler handler);
    void set_fallback(Handler handler) { fallback_ = std::move(handler); }

    Address local_address() const { return local_; }
    bool running() const { return running_.load(std::memory_order_relaxed); }
    u64 requests_served() const { return requests_served_.load(std::memory_order_relaxed); }

private:
    void serve();
    void handle_client(int client_fd);
    HttpResponse dispatch(const HttpRequest& request);

    int listen_fd_ = -1;
    Address local_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::map<std::string, Handler> routes_;
    Handler fallback_;
    std::atomic<u64> requests_served_{0};
};

/// Percent-decodes a URL component.
std::string url_decode(const std::string& text);

/// Minimal JSON string escaping for values the services emit.
std::string json_escape(const std::string& text);

}  // namespace morton
