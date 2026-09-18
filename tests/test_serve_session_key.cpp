#include "serve/request.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace ninfer::serve;
using ninfer::ChatRole;

int check(bool condition, const char* message) {
    if (condition) { return 0; }
    std::cerr << message << '\n';
    return 1;
}

ChatTurn system_turn(const std::string& text) {
    ChatTurn turn;
    turn.role = ChatRole::System;
    ContentPart part;
    part.text = text;
    turn.content.push_back(std::move(part));
    return turn;
}

ChatTurn user_turn(const std::string& text) {
    ChatTurn turn;
    turn.role = ChatRole::User;
    ContentPart part;
    part.text = text;
    turn.content.push_back(std::move(part));
    return turn;
}

} // namespace

int main() {
    int failures = 0;

    // Same conversation (same system + same first user) -> same key, regardless of
    // how many turns have accumulated since.
    {
        GenerationRequest a;
        a.messages = {system_turn("SYS"), user_turn("first user message")};
        GenerationRequest b = a;
        b.messages.push_back(user_turn("second turn"));
        b.messages.push_back(ChatTurn{}); // trailing assistant turn
        const auto ka = derive_session_key(a);
        const auto kb = derive_session_key(b);
        failures += check(ka.has_value() && ka == kb, "stable across turns");
    }

    // Different first user message -> different key.
    {
        GenerationRequest a;
        a.messages = {system_turn("SYS"), user_turn("first user message")};
        GenerationRequest c;
        c.messages = {system_turn("SYS"), user_turn("a different first message")};
        const auto ka = derive_session_key(a);
        const auto kc = derive_session_key(c);
        failures += check(ka.has_value() && kc.has_value() && *ka != *kc,
                          "distinct first user -> distinct key");
    }

    // Key format is inspectable and fits the PreparedSessionKey capacity (256 bytes).
    {
        GenerationRequest a;
        a.messages = {system_turn("SYS"), user_turn("hello")};
        const auto ka = derive_session_key(a);
        failures += check(ka.has_value() && ka->size() == 19 && ka->substr(0, 3) == "cs-",
                          "key format 'cs-' + 16 hex");
        failures += check(ka->size() <= 256, "fits the 256-byte session-key capacity");
    }

    // No user text (system-only, or empty first user) -> no key (pre-fix behavior).
    {
        GenerationRequest a;
        a.messages = {system_turn("SYS")};
        failures += check(!derive_session_key(a).has_value(), "no user turn -> no key");
        GenerationRequest b;
        b.messages = {system_turn("SYS"), user_turn("")};
        failures += check(!derive_session_key(b).has_value(), "empty first user -> no key");
    }

    // Non-text parts are excluded from the fingerprint.
    {
        GenerationRequest a;
        a.messages = {system_turn("SYS"), user_turn("text")};
        GenerationRequest b;
        b.messages = {system_turn("SYS"), user_turn("text")};
        ChatTurn extra = b.messages[1];
        ContentPart image;
        image.kind = ContentKind::Image;
        extra.content.push_back(std::move(image));
        b.messages[1] = std::move(extra);
        const auto ka = derive_session_key(a);
        const auto kb = derive_session_key(b);
        failures += check(ka.has_value() && ka == kb, "non-text parts do not change the key");
    }

    if (failures == 0) {
        std::fprintf(stderr, "PASS: serve_session_key\n");
        return 0;
    }
    std::fprintf(stderr, "FAIL: %d check(s)\n", failures);
    return 1;
}
