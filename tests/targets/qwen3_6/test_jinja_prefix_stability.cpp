// Prefix-stability probe for the jinja chat renderer: the context cache can only
// re-use a rendered prefix if rendering the conversation with its last message
// removed produces a byte-prefix of the full render. This test renders a
// realistic multi-turn agentic conversation (system + tools, user query,
// assistant with reasoning and tool call, tool response, final assistant) and
// checks, for each production-relevant option combination:
//
//   1. determinism  — two renders of the same conversation are byte-identical
//   2. prefix       — render(messages[0..N-1]) is a byte-prefix of render(messages[0..N])
//   3. boundaries   — the truncated render's message boundaries equal the first
//                     N-1 boundaries of the full render
//
// On failure it prints the first divergence point with context so the cause is
// visible without a second run.

#include "targets/qwen3_6/impl/frontend/jinja_chat_render.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fi = ninfer::targets::qwen3_6::frontend_internal;

namespace {

int failures = 0;

void fail(const std::string& message) {
    std::cerr << "FAIL " << message << '\n';
    ++failures;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { throw std::runtime_error("cannot read " + path); }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

fi::ChatMessage user_message(std::string text) {
    fi::ChatMessage message;
    message.role = ninfer::ChatRole::User;
    message.parts.push_back(fi::ChatPart::text_part(std::move(text)));
    return message;
}

fi::ChatMessage system_message(std::string text) {
    fi::ChatMessage message;
    message.role = ninfer::ChatRole::System;
    message.parts.push_back(fi::ChatPart::text_part(std::move(text)));
    return message;
}

fi::ChatMessage assistant_message(std::string reasoning, std::string text,
                                  fi::ToolCall call = {}) {
    fi::ChatMessage message;
    message.role = ninfer::ChatRole::Assistant;
    message.reasoning_content = std::move(reasoning);
    message.parts.push_back(fi::ChatPart::text_part(std::move(text)));
    if (!call.name.empty()) { message.tool_calls.push_back(std::move(call)); }
    return message;
}

fi::ChatMessage tool_message(std::string text) {
    fi::ChatMessage message;
    message.role = ninfer::ChatRole::Tool;
    message.parts.push_back(fi::ChatPart::text_part(std::move(text)));
    return message;
}

// A realistic agentic conversation: system + tools, user query, an assistant
// turn that calls a tool, the tool response, and the final assistant answer.
std::vector<fi::ChatMessage> conversation() {
    std::vector<fi::ChatMessage> messages;
    messages.push_back(system_message("You are a coding agent. Use the tools to inspect files."));
    messages.push_back(user_message("Find the definition of select_kv_pressure_actions."));
    messages.push_back(assistant_message(
        "I need to locate the symbol in the runtime sources.",
        "",
        fi::ToolCall{"call_1", "grep", R"({"pattern": "select_kv_pressure_actions"})"}));
    messages.push_back(tool_message("src/targets/qwen3_6/impl/runtime/program_impl.h:343:select_kv_pressure_actions"));
    messages.push_back(assistant_message(
        "The symbol is a free function in program_impl.h taking the address space and page store.",
        "It is defined at program_impl.h:343."));
    return messages;
}

// The same conversation after the user sends a second query (turn 2): the
// production Claude-Code shape where a new user message follows a completed
// agent turn.
std::vector<fi::ChatMessage> conversation_turn2() {
    std::vector<fi::ChatMessage> messages = conversation();
    messages.push_back(user_message("Now show me the callers of that function."));
    messages.push_back(assistant_message(
        "I need to search for call sites.",
        "",
        fi::ToolCall{"call_2", "grep", R"({"pattern": "select_kv_pressure_actions\\("})"}));
    messages.push_back(tool_message("src/targets/qwen3_6/impl/runtime/program_impl.h:1472: KVPressureSelection selected = select_kv_pressure_actions("));
    messages.push_back(assistant_message(
        "There is one call site in the pressure path.",
        "It is called at program_impl.h:1472."));
    return messages;
}

// A conversation whose later message carries a thinking control token: the
// template scans the whole conversation for <|think_*|> markers, so a marker in
// a LATE message must not change the rendering of earlier messages.
std::vector<fi::ChatMessage> conversation_think_token() {
    std::vector<fi::ChatMessage> messages = conversation();
    messages.push_back(user_message("Stop thinking so much. <|think_off|>"));
    messages.push_back(assistant_message("", "Understood."));
    return messages;
}

// An assistant message with two tool calls in one turn: the nested tool_calls
// loop inside the message loop is the failure class PR #5 fixed.
std::vector<fi::ChatMessage> conversation_multi_tool_call() {
    std::vector<fi::ChatMessage> messages;
    messages.push_back(system_message("You are a coding agent."));
    messages.push_back(user_message("Check both files."));
    fi::ChatMessage assistant;
    assistant.role = ninfer::ChatRole::Assistant;
    assistant.reasoning_content = "Two independent lookups.";
    assistant.tool_calls.push_back(fi::ToolCall{"call_1", "read", R"({"path": "a.cpp"})"});
    assistant.tool_calls.push_back(fi::ToolCall{"call_2", "read", R"({"path": "b.cpp"})"});
    messages.push_back(std::move(assistant));
    messages.push_back(tool_message("a.cpp contents"));
    messages.push_back(tool_message("b.cpp contents"));
    messages.push_back(assistant_message("", "Both files checked."));
    return messages;
}

struct Options {
    std::string label;
    bool preserve_thinking;
    bool with_tools;
};

// Checks that render(base) is a byte-prefix of render(extended), that the render
// is deterministic, and that the message boundaries agree. `base` and
// `extended` are the two consecutive conversation shapes the engine sees across
// two requests.
void check_pair(const fi::JinjaChatTemplate& template_, const std::string& label,
                const Options& option_set, const std::vector<fi::ChatMessage>& base,
                const std::vector<fi::ChatMessage>& extended) {
    fi::ChatRenderOptions options;
    // The generation prompt is a per-request suffix appended after the last
    // message; the engine caches up to the message boundaries, so the prefix
    // property is checked on the message body without it.
    options.add_generation_prompt = false;
    options.enable_thinking       = true;
    options.preserve_thinking     = option_set.preserve_thinking;
    if (option_set.with_tools) {
        options.tool_jsons.push_back(
            R"({"name": "grep", "description": "Search files", "parameters": {"type": "object", "properties": {"pattern": {"type": "string"}}, "required": ["pattern"]}})");
        options.tool_jsons.push_back(
            R"({"name": "read", "description": "Read a file", "parameters": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}})");
    }

    auto render = [&](const std::vector<fi::ChatMessage>& msgs) {
        return template_.render(msgs, options);
    };

    const fi::RenderedChat full = render(extended);
    const fi::RenderedChat again = render(extended);
    const std::string full_label = label + " [" + option_set.label + "]";

    // 1. Determinism.
    if (full.text != again.text) {
        std::size_t at = 0;
        while (at < full.text.size() && at < again.text.size() && full.text[at] == again.text[at]) {
            ++at;
        }
        fail(full_label + ": render is not deterministic (diverges at byte " + std::to_string(at) +
             ")");
        return;
    }

    // 2. Prefix stability: the base conversation must render to a byte-prefix.
    const fi::RenderedChat short_render = render(base);
    const std::string& a = short_render.text;
    const std::string& b = full.text;
    if (a.size() > b.size() || b.compare(0, a.size(), a) != 0) {
        const std::size_t limit = std::min(a.size(), b.size());
        std::size_t at = 0;
        while (at < limit && a[at] == b[at]) { ++at; }
        const auto window = [](const std::string& s, std::size_t at) {
            const std::size_t from = at > 60 ? at - 60 : 0;
            const std::size_t to   = std::min(s.size(), at + 60);
            return s.substr(from, to - from);
        };
        fail(full_label + ": base render is not a prefix of the extended render "
             "(base=" + std::to_string(a.size()) + " extended=" + std::to_string(b.size()) +
             " bytes, first divergence at byte " + std::to_string(at) + ")\n"
             "  base[...]:     " + window(a, at) + "\n"
             "  extended[...]: " + window(b, at));
        return;
    }

    // 3. Boundary stability: the base render's boundaries must equal the first
    // base.size()+1 boundaries of the extended render. The renderer emits one
    // frontier per message plus a trailing one (index n = after the first n
    // messages), so a conversation of N messages yields N+1 boundaries.
    const auto& b_full = full.message_boundaries;
    const auto& b_short = short_render.message_boundaries;
    if (b_short.size() != base.size() + 1 || b_full.size() != extended.size() + 1) {
        fail(full_label + ": boundary count mismatch (base=" + std::to_string(b_short.size()) +
             " extended=" + std::to_string(b_full.size()) + ")");
        return;
    }
    for (std::size_t i = 0; i < b_short.size(); ++i) {
        if (b_short[i] != b_full[i]) {
            fail(full_label + ": message boundary " + std::to_string(i) + " moved between renders "
                 "(base=" + (b_short[i] ? std::to_string(*b_short[i]) : "nullopt") +
                 " extended=" + (b_full[i] ? std::to_string(*b_full[i]) : "nullopt") + ")");
            break;
        }
    }
}

} // namespace

int main() {
    try {
        const std::string template_source =
            read_file(NINFER_SOURCE_DIR "/tests/fixtures/frontend/froggeric_v225_chat_template.jinja");
        const fi::JinjaChatTemplate template_ = fi::JinjaChatTemplate::compile(template_source);

        const std::vector<Options> option_sets = {
            {"preserve_thinking+tools", true, true},
            {"preserve_thinking", true, false},
            {"no-preserve+tools", false, true},
            {"no-preserve", false, false},
        };

        const std::vector<fi::ChatMessage> turn1 = conversation();
        const std::vector<fi::ChatMessage> turn2 = conversation_turn2();
        const std::vector<fi::ChatMessage> think = conversation_think_token();
        const std::vector<fi::ChatMessage> multi_tool = conversation_multi_tool_call();

        for (const Options& option_set : option_sets) {
            // Within turn 1: each request appends one message.
            for (std::size_t i = 2; i < turn1.size(); ++i) {
                check_pair(template_, "turn1", option_set,
                           std::vector<fi::ChatMessage>(turn1.begin(), turn1.begin() + i - 1),
                           std::vector<fi::ChatMessage>(turn1.begin(), turn1.begin() + i));
            }
            // Turn 2: the new user query is appended to the completed turn 1.
            check_pair(template_, "turn1->turn2-query", option_set, turn1,
                       std::vector<fi::ChatMessage>(turn2.begin(), turn2.begin() + 6));
            for (std::size_t i = 7; i < turn2.size(); ++i) {
                check_pair(template_, "turn2", option_set,
                           std::vector<fi::ChatMessage>(turn2.begin(), turn2.begin() + i - 1),
                           std::vector<fi::ChatMessage>(turn2.begin(), turn2.begin() + i));
            }
            // A later message carrying a thinking control token.
            check_pair(template_, "think-token", option_set, turn1,
                       std::vector<fi::ChatMessage>(think.begin(), think.begin() + 6));
            check_pair(template_, "think-token", option_set,
                       std::vector<fi::ChatMessage>(think.begin(), think.begin() + 6), think);
            // Two tool calls in one assistant message.
            for (std::size_t i = 2; i < multi_tool.size(); ++i) {
                check_pair(template_, "multi-tool-call", option_set,
                           std::vector<fi::ChatMessage>(multi_tool.begin(), multi_tool.begin() + i - 1),
                           std::vector<fi::ChatMessage>(multi_tool.begin(), multi_tool.begin() + i));
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "probe error: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0) {
        std::cout << "ok (prefix stability)\n";
    }
    return failures == 0 ? 0 : 1;
}
