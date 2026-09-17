// Segment Routing Fabric 1.0.0
// Copyright 2026 Summon Software Labs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace srf {

using ByteBuffer = std::vector<std::byte>;

/// Explicit little-endian byte writer. Nothing in this runtime ever serializes a
/// raw C++ object representation; every field is written field by field here.
class ByteWriter {
public:
    ByteWriter() = default;

    void u8(std::uint8_t v) { raw(&v, 1); }
    void u16(std::uint16_t v) { raw(&v, 2); }
    void u32(std::uint32_t v) { raw(&v, 4); }
    void u64(std::uint64_t v) { raw(&v, 8); }

    void bytes(std::span<const std::byte> s) { raw(s.data(), s.size()); }

    /// Write exactly width bytes: the payload left-aligned and zero-padded, or
    /// rejected when it is longer than width. Padding is what makes two accepted
    /// semantic spellings of the same segment canonicalize identically.
    [[nodiscard]] bool fixed_bytes(std::span<const std::byte> s, std::size_t width) {
        if (s.size() > width) {
            return false;
        }
        raw(s.data(), s.size());
        for (std::size_t i = s.size(); i < width; ++i) {
            buf_.push_back(std::byte{0});
        }
        return true;
    }

    void reserve(std::size_t n) { buf_.reserve(n); }
    [[nodiscard]] std::size_t size() const noexcept { return buf_.size(); }
    [[nodiscard]] const ByteBuffer& buffer() const noexcept { return buf_; }
    [[nodiscard]] ByteBuffer take() && { return std::move(buf_); }
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {buf_.data(), buf_.size()};
    }

private:
    void raw(const void* p, std::size_t n) {
        if (n == 0) {
            return;
        }
        const auto* b = static_cast<const std::byte*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    ByteBuffer buf_{};
};

/// Explicit little-endian, bounds-checked byte reader. A reader that has not
/// consumed every byte is a defect: callers must check at_end().
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == data_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

    [[nodiscard]] bool u8(std::uint8_t& out) noexcept { return raw(&out, 1); }
    [[nodiscard]] bool u16(std::uint16_t& out) noexcept { return raw(&out, 2); }
    [[nodiscard]] bool u32(std::uint32_t& out) noexcept { return raw(&out, 4); }
    [[nodiscard]] bool u64(std::uint64_t& out) noexcept { return raw(&out, 8); }

    [[nodiscard]] bool bytes(std::span<std::byte> out) noexcept {
        return raw(out.data(), out.size());
    }

    [[nodiscard]] bool skip(std::size_t n) noexcept {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return false;
        }
        pos_ += n;
        return true;
    }

    [[nodiscard]] std::span<const std::byte> rest() const noexcept {
        return data_.subspan(pos_);
    }

private:
    [[nodiscard]] bool raw(void* out, std::size_t n) noexcept {
        if (!ok_ || remaining() < n) {
            ok_ = false;
            return false;
        }
        if (n > 0) {
            std::memcpy(out, data_.data() + pos_, n);
        }
        pos_ += n;
        return true;
    }

    std::span<const std::byte> data_{};
    std::size_t pos_{0};
    bool ok_{true};
};

[[nodiscard]] inline std::span<const std::byte> as_bytes(const ByteBuffer& b) noexcept {
    return {b.data(), b.size()};
}

[[nodiscard]] inline std::span<const std::byte> as_bytes(std::string_view s) noexcept {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

} // namespace srf
