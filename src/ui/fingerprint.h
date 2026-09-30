#pragma once

#include <cstdint>
#include <string_view>

namespace frame_notify::ui {

// A 64-bit FNV-1a hash of everything fed to it. The panels hash what determines their pixels, so
// two equal fingerprints mean identical images and the second one need not be uploaded again.
class Fingerprint {
public:
    void add(std::string_view text) noexcept {
        for (const char character : text) mix(static_cast<unsigned char>(character));
        mix(0xFFU);  // keeps "ab","c" apart from "a","bc"
    }
    void add(std::int64_t number) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            mix(static_cast<unsigned char>((static_cast<std::uint64_t>(number) >> shift) & 0xFFU));
        }
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return state_; }

private:
    void mix(unsigned char byte) noexcept {
        state_ ^= byte;
        state_ *= 1099511628211ULL;
    }
    std::uint64_t state_ = 1469598103934665603ULL;
};

}  // namespace frame_notify::ui
