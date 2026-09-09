#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace proto {
struct Uuid {
    std::array<uint8_t, 16> bytes{};
    static Uuid create();
    static Uuid parse(std::string_view text);
    std::string string() const;
    bool empty() const { return *this == Uuid{}; }
    auto operator<=>(const Uuid&) const = default;
};
template <class Tag> struct Id {
    Uuid uuid;
    static Id create() { return {Uuid::create()}; }
    static Id parse(std::string_view value) { return {Uuid::parse(value)}; }
    std::string string() const { return uuid.string(); }
    explicit operator bool() const { return !uuid.empty(); }
    auto operator<=>(const Id&) const = default;
};
using EntityId = Id<struct EntityTag>;
using AssetId = Id<struct AssetTag>;
using BehaviorTypeId = Id<struct BehaviorTypeTag>;
using BehaviorBindingId = Id<struct BehaviorBindingTag>;
struct EntityHandle {
    uint32_t slot{UINT32_MAX};
    uint32_t generation{};
    explicit operator bool() const { return generation != 0; }
    auto operator<=>(const EntityHandle&) const = default;
};
} // namespace proto
template <class Tag> struct std::hash<proto::Id<Tag>> {
    size_t operator()(const proto::Id<Tag>& id) const noexcept {
        size_t value = 14695981039346656037ull;
        for (const auto byte : id.uuid.bytes) {
            value ^= byte;
            value *= 1099511628211ull;
        }
        return value;
    }
};
