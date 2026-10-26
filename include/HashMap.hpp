#pragma once

#include <shared_mutex>
#include <forward_list>
#include <optional>
#include <utility>
#include <cassert>
#include <memory>
#include <vector>

class HashMap {
    using Node = std::pair<uint64_t, std::vector<std::byte>>;
    using Bucket = std::pair<std::shared_mutex, std::forward_list<Node>>;
public:
    explicit HashMap(std::size_t N)
        : size_(N), data_(std::make_unique<Bucket[]>(N)) { assert(N > 0); };
    ~HashMap() noexcept = default;

    void insert(uint64_t key, std::vector<std::byte> value);
    bool remove(uint64_t key);

    std::optional<std::vector<std::byte>> get(uint64_t key);
    std::optional<std::vector<Node>> dump_bucket(std::size_t key);

private:
    static constexpr std::uint64_t mix64(std::uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdLLU;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53LLU;
        k ^= k >> 33;
        return k;
    }

    std::size_t size_;
    std::unique_ptr<Bucket[]> data_;
};
