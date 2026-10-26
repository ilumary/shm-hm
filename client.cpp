#include <iostream>
#include <sstream>
#include <vector>
#include <print>

#include "ShmUtil.hpp"

static std::string_view status_name(Status s) {
    switch (s) {
    case Status::Ok: return "Ok";
    case Status::NotFound: return "NotFound";
    case Status::Error: return "Error";
    }
    return "?";
}

static int fail(const Error& e) {
    std::println(stderr, "client: {}: {}", e.what, e.code.message());
    return 1;
}

static std::vector<std::byte> unescape(std::string_view s) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s.substr(i, 2) == "\\x" && i + 3 < s.size()) {
            unsigned v{};
            const char* hex = s.data() + i + 2;
            if (const auto [end, ec] = std::from_chars(hex, hex + 2, v, 16); ec == std::errc{} && end == hex + 2) {
                out.push_back(static_cast<std::byte>(v));
                i += 3;
                continue;
            }
        }
        if (s.substr(i, 2) == "\\\\") {
            ++i;
        }
        out.push_back(static_cast<std::byte>(s[i]));
    }
    return out;
}

static std::string escape(std::span<const std::byte> data) {
    std::string out;
    for (const std::byte b : data) {
        const auto c = static_cast<unsigned char>(b);
        if (c == '\\') {
            out += "\\\\";
        } else if (std::isprint(c)) {
            out += static_cast<char>(c);
        } else {
            out += std::format("\\x{:02x}", c);
        }
    }
    return out;
}

static void print_cmd(std::string_view cmd, const std::expected<ShmUtil<>::Result, Error>& r) {
    if (!r) {
        std::println("{} -> failed: {}: {}", cmd, r.error().what, r.error().code.message());
        return;
    }
    std::println("{} -> {}{}", cmd, status_name(r->status), r->truncated ? " (truncated)" : "");
    for (const auto& entry : r->items()) {
        std::println("    {} = \"{}\"", entry.key, escape(entry.value()));
    }
}

int main() {
    ShmUtil<> q{SHM_NAME, Role::Client};
    if (const auto& e = q.error()) {
        return fail(*e);
    }

    constexpr auto get = [](auto future) { return future.get(); };

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in{line};
        char op{};
        Key key{};

        if (!(in >> op >> key)) {
            if (!line.empty()) {
                std::println(stderr, "skipping: {}", line);
            }
            continue;
        }

        switch (op) {
            case 'I': {
                std::string data;
                in.get();
                std::getline(in, data);
                if (auto r = q.insert(key, unescape(data)); !r) {
                    std::println("{} -> failed: {}: {}", line, r.error().what, r.error().code.message());
                }
                break;
            }
            case 'G': print_cmd(line, q.request(Op::Read, key).and_then(get)); break;
            case 'D': print_cmd(line, q.request(Op::Delete, key).and_then(get)); break;
            case 'B': print_cmd(line, q.request(Op::DumpBucket, key).and_then(get)); break;
            default: std::println(stderr, "unknown command: {}", line); break;
        }
    }
}

