#pragma once

#include "serve/generation_service.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ninfer::serve {

class ClientDisconnected final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override { return "client disconnected"; }
};

struct HttpGenerationStream {
    explicit HttpGenerationStream(PreparedRequest request) : prepared(std::move(request)) {}

    PreparedRequest prepared;
    std::atomic<bool> cancelled{false};
    bool started = false;
};

class SseTransport final {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::chrono::seconds kHeartbeatInterval{5};
    static constexpr std::string_view kHeartbeatComment = ": keep-alive\n\n";
    // The Anthropic SSE spec defines a `ping` event: a REAL stream event (not a
    // comment) that client stream parsers recognize and ignore. Comment
    // keep-alives do not reset a client's "first event" timer — a queued
    // request whose prefill takes longer than that budget gets abandoned by
    // the client (observed 2026-09-16: cancels at ~29s of queue wait despite
    // comment heartbeats flowing). The anthropic handler heartbeats with the
    // comment (TCP_USER_TIMEOUT probe) plus the ping (client-visible progress).
    static constexpr std::string_view kHeartbeatAnthropic =
        ": keep-alive\n\nevent: ping\ndata: {\"type\": \"ping\"}\n\n";

    explicit SseTransport(httplib::DataSink& sink, std::atomic<bool>& cancelled,
                          Clock::duration heartbeat_interval = kHeartbeatInterval,
                          Clock::time_point now              = Clock::now(),
                          std::string_view heartbeat_payload = kHeartbeatComment);

    void write(std::string_view item, Clock::time_point now = Clock::now());
    void write(const std::vector<std::string>& items, Clock::time_point now = Clock::now());

    // Called by the Engine wait loop. Besides observing an already-closed socket, a quiet stream
    // periodically writes an SSE comment so TCP_USER_TIMEOUT has traffic with which to detect an
    // unacknowledged peer. A failed probe marks the request for cancellation.
    [[nodiscard]] bool poll(Clock::time_point now = Clock::now());

private:
    bool mark_cancelled() noexcept;

    httplib::DataSink& sink_;
    std::atomic<bool>& cancelled_;
    Clock::duration heartbeat_interval_;
    std::string_view heartbeat_payload_;
    Clock::time_point last_write_;
};

nlohmann::json parse_json_body(const httplib::Request& request);
[[nodiscard]] bool client_disconnected(const httplib::Request& request);

void prepare_sse_response(httplib::Response& response);
void configure_http_server_socket(socket_t socket) noexcept;
void set_owned_json_content(httplib::Response& response, std::string body,
                            std::shared_ptr<RequestLifetime> lifetime);

} // namespace ninfer::serve
