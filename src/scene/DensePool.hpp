#pragma once
#include "core/Id.hpp"
#include <span>
#include <stdexcept>
#include <vector>

namespace proto {
template<class T> class DensePool {
public:
    bool contains(EntityHandle handle) const {
        return handle.slot < sparse_.size() && sparse_[handle.slot] < owners_.size() && owners_[sparse_[handle.slot]] == handle;
    }
    T& get(EntityHandle handle) {
        if (!contains(handle)) throw std::runtime_error("Invalid or stale entity handle");
        return values_[sparse_[handle.slot]];
    }
    const T& get(EntityHandle handle) const { return const_cast<DensePool*>(this)->get(handle); }
    void insert(EntityHandle handle, T value) {
        if (contains(handle)) throw std::runtime_error("Duplicate component");
        if (sparse_.size() <= handle.slot) sparse_.resize(size_t(handle.slot) + 1, SIZE_MAX);
        sparse_[handle.slot] = owners_.size(); owners_.push_back(handle); values_.push_back(std::move(value));
    }
    void erase(EntityHandle handle) {
        if (!contains(handle)) return;
        const auto index = sparse_[handle.slot];
        if (index != owners_.size() - 1) {
            values_[index] = std::move(values_.back()); owners_[index] = owners_.back(); sparse_[owners_[index].slot] = index;
        }
        values_.pop_back(); owners_.pop_back(); sparse_[handle.slot] = SIZE_MAX;
    }
    std::span<const EntityHandle> owners() const { return owners_; }
    std::span<const T> values() const { return values_; }
    size_t size() const { return values_.size(); }
private:
    std::vector<T> values_;
    std::vector<EntityHandle> owners_;
    std::vector<size_t> sparse_;
};
}
