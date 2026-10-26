#include "HashMap.hpp"

#include <mutex>

void HashMap::insert(std::uint64_t key, std::vector<std::byte> value) {
    auto& [mutex, list] = data_[mix64(key) % size_];
    std::unique_lock lock(mutex);
    if (const auto it = std::ranges::find(list, key, &Node::first); it != list.end()) {
        it->second = std::move(value);
        return;
    }
    list.emplace_front(key, std::move(value));
}

bool HashMap::remove(std::uint64_t key) {
    auto& [mutex, list] = data_[mix64(key) % size_];
    std::unique_lock lock(mutex);
    return list.remove_if([key](const Node& n) { return n.first == key; }) > 0;
}

std::optional<std::vector<std::byte>> HashMap::get(std::uint64_t key) {
    auto& [mutex, list] = data_[mix64(key) % size_];
    std::shared_lock lock(mutex);
    if (const auto it = std::ranges::find(list, key, &Node::first); it != list.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::vector<HashMap::Node>> HashMap::dump_bucket(std::size_t key) {
    auto& [mutex, list] = data_[mix64(key) % size_];
    if (list.empty()) { return std::nullopt; }
    std::shared_lock lock(mutex);
    return std::vector<Node>(list.begin(), list.end());
}

