#pragma once
#include <array>
#include <cstring>

#include "core/types.h"

namespace morton {

/// Fixed-capacity map from a wrapping 16-bit sequence to T, indexed by sequence
/// modulo capacity. Entries age out implicitly as sequences advance, so there is
/// no allocation and no cleanup pass.
template <typename T, u32 Capacity>
class SequenceBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    SequenceBuffer() { reset(); }

    void reset() {
        keys_.fill(kEmpty);
        latest_ = 0;
        has_latest_ = false;
    }

    /// Inserts or overwrites the slot for `sequence`. Returns null if the sequence
    /// is too old to still be representable in the window.
    T* insert(u16 sequence) {
        if (has_latest_ && sequence_greater(latest_, sequence) &&
            static_cast<u16>(latest_ - sequence) >= Capacity) {
            return nullptr;
        }
        if (!has_latest_ || sequence_greater(sequence, latest_)) {
            remove_range(has_latest_ ? static_cast<u16>(latest_ + 1) : sequence, sequence);
            latest_ = sequence;
            has_latest_ = true;
        }
        u32 index = sequence % Capacity;
        keys_[index] = sequence;
        values_[index] = T{};
        return &values_[index];
    }

    T* find(u16 sequence) {
        u32 index = sequence % Capacity;
        return keys_[index] == sequence ? &values_[index] : nullptr;
    }

    const T* find(u16 sequence) const {
        u32 index = sequence % Capacity;
        return keys_[index] == sequence ? &values_[index] : nullptr;
    }

    bool exists(u16 sequence) const { return keys_[sequence % Capacity] == sequence; }

    void remove(u16 sequence) { keys_[sequence % Capacity] = kEmpty; }

    u16 latest() const { return latest_; }
    bool has_latest() const { return has_latest_; }
    static constexpr u32 capacity() { return Capacity; }

private:
    static constexpr u32 kEmpty = 0xffffffffu;

    /// Clears slots skipped by a sequence jump so stale entries cannot alias.
    void remove_range(u16 begin, u16 end) {
        u16 span = static_cast<u16>(end - begin);
        if (span >= Capacity) {
            keys_.fill(kEmpty);
            return;
        }
        for (u16 s = begin;; ++s) {
            keys_[s % Capacity] = kEmpty;
            if (s == end) break;
        }
    }

    std::array<u32, Capacity> keys_;
    std::array<T, Capacity> values_;
    u16 latest_ = 0;
    bool has_latest_ = false;
};

}  // namespace morton
