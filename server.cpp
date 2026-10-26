#include <thread>
#include <print>

#include "ShmUtil.hpp"
#include "HashMap.hpp"

static std::string_view op_name(Op op) {
    switch (op) {
    case Op::Insert: return "Insert";
    case Op::Read: return "Read";
    case Op::Delete: return "Delete";
    case Op::DumpBucket: return "DumpBucket";
    }
    return "?";
}

static int fail(const Error& e) {
    std::println(stderr, "server: {}: {}", e.what, e.code.message());
    return 1;
}

static std::optional<std::size_t> parse_unsigned(std::string_view s) {
    std::size_t n{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (ec != std::errc{} || end != s.data() + s.size() || n == 0) {
        return std::nullopt;
    }
    return n;
}

static std::expected<void, Error> handle(ShmUtil<>& q, HashMap& map, const ShmUtil<>::Msg& msg) {
    if (msg.op == Op::Insert) {
        const auto value = msg.value();
        map.insert(msg.key, std::vector<std::byte>(value.begin(), value.end()));
        return {};
    }

    auto reply = q.reply(msg);
    if (!reply) {
        return std::unexpected(reply.error());
    }

    switch (msg.op) {
    case Op::Read: {
        const auto value = map.get(msg.key);
        if (!value) {
            return reply->send(Status::NotFound);
        }
        if (auto r = reply->add(msg.key, *value); !r) {
            return std::unexpected(r.error());
        }
        return reply->send(Status::Ok);
    }
    case Op::Delete:
        return reply->send(map.remove(msg.key) ? Status::Ok : Status::NotFound);
    case Op::DumpBucket: {
        const auto nodes = map.dump_bucket(msg.key);
        if (!nodes) {
            return reply->send(Status::NotFound);
        }
        for (const auto& [key, value] : *nodes) {
            const auto added = reply->add(key, value);
            if (!added) {
                return std::unexpected(added.error());
            }
            if (!*added) {
                break;
            }
        }
        return reply->send(Status::Ok);
    }
    case Op::Insert:
        break;
    }
    return reply->send(Status::Error);
}

static void worker(ShmUtil<>& q, HashMap& map) {
    for (;;) {
        const auto msg = q.receive();
        if (!msg) {
            fail(msg.error());
            return;
        }
        const std::string_view value{reinterpret_cast<const char*>(msg->data.data()), msg->size};
        std::println("{:<10} key={} value=\"{}\"", op_name(msg->op), msg->key, value);

        if (auto r = handle(q, map, *msg); !r) {
            fail(r.error());
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3) {
        std::println(stderr, "usage: {} <buckets> [threads=2]", argv[0]);
        return 1;
    }
    const auto buckets = parse_unsigned(argv[1]);
    const auto threads = argc == 3 ? parse_unsigned(argv[2]) : std::optional<std::size_t>{2};
    if (!buckets || !threads) {
        std::println(stderr, "specify buckets and threads as positive integers", argv[0]);
        return 1;
    }

    HashMap map(*buckets);
    ShmUtil<> q{SHM_NAME, Role::Server};
    if (const auto& e = q.error()) {
        return fail(*e);
    }
    std::println("server: ready on {} ({} buckets, {} threads)", SHM_NAME, *buckets, *threads);

    std::vector<std::jthread> workers;
    for (std::size_t i = 0; i < *threads; ++i) {
        workers.emplace_back([&] { worker(q, map); });
    }
}

