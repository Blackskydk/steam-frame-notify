#include "ui/typography.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <utility>

namespace frame_notify::ui {
namespace {

constexpr char kRegularFile[] = "Inter-Regular.ttf";
constexpr char kSemiBoldFile[] = "Inter-SemiBold.ttf";
constexpr int kSubpixelSteps = 4;

char32_t next_code_point(std::string_view text, std::size_t& offset) {
    const auto lead = static_cast<unsigned char>(text[offset++]);
    if (lead < 0x80U) return lead;

    int continuation_count = 0;
    char32_t value = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        continuation_count = 1;
        value = lead & 0x1FU;
    } else if ((lead & 0xF0U) == 0xE0U) {
        continuation_count = 2;
        value = lead & 0x0FU;
    } else if ((lead & 0xF8U) == 0xF0U) {
        continuation_count = 3;
        value = lead & 0x07U;
    } else {
        return U'?';
    }
    for (int index = 0; index < continuation_count; ++index) {
        if (offset >= text.size()) return U'?';
        const auto continuation = static_cast<unsigned char>(text[offset]);
        if ((continuation & 0xC0U) != 0x80U) return U'?';
        ++offset;
        value = (value << 6U) | (continuation & 0x3FU);
    }
    return value;
}

// Zero-width and joining characters (emoji sequences, variation selectors, bidi marks).
bool is_ignorable(char32_t code_point) {
    return code_point == 0x00ADU || (code_point >= 0x200BU && code_point <= 0x200FU) ||
           (code_point >= 0x202AU && code_point <= 0x202EU) ||
           (code_point >= 0x2060U && code_point <= 0x2064U) ||
           (code_point >= 0xFE00U && code_point <= 0xFE0FU) || code_point == 0xFEFFU ||
           (code_point >= 0xE0000U && code_point <= 0xE01EFU) ||
           (code_point >= 0x1F3FBU && code_point <= 0x1F3FFU) || code_point < 0x20U;
}

std::vector<std::size_t> code_point_offsets(std::string_view text) {
    std::vector<std::size_t> offsets;
    std::size_t offset = 0;
    while (offset < text.size()) {
        offsets.push_back(offset);
        static_cast<void>(next_code_point(text, offset));
    }
    offsets.push_back(text.size());
    return offsets;
}

const std::array<std::uint8_t, 256>& coverage_curve() {
    // Light text on a dark background reads thin with linear coverage; a mild curve restores weight.
    static const std::array<std::uint8_t, 256> table = [] {
        std::array<std::uint8_t, 256> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const double coverage = static_cast<double>(index) / 255.0;
            values[index] = static_cast<std::uint8_t>(std::lround(std::pow(coverage, 0.82) * 255.0));
        }
        return values;
    }();
    return table;
}

using LegacyGlyph = std::array<std::uint8_t, 7>;

LegacyGlyph legacy_glyph(char32_t code_point) {
    if (code_point >= U'a' && code_point <= U'z') code_point -= U'a' - U'A';
    if (code_point == U'æ') code_point = U'Æ';
    if (code_point == U'ø') code_point = U'Ø';
    if (code_point == U'å') code_point = U'Å';
    if (code_point == U'…') code_point = U'.';
    if (code_point == U'‘' || code_point == U'’' || code_point == U'`') code_point = U'\'';
    if (code_point == U'“' || code_point == U'”') code_point = U'"';
    if (code_point == U'–' || code_point == U'—' || code_point == U'−') code_point = U'-';
    if (code_point == U'•') code_point = U'·';

    switch (code_point) {
    case U' ': return {0, 0, 0, 0, 0, 0, 0};
    case U'"': return {10, 10, 10, 0, 0, 0, 0};
    case U'·': return {0, 0, 0, 12, 12, 0, 0};
    case U'×': return {0, 17, 10, 4, 10, 17, 0};
    case U'+': return {0, 4, 4, 31, 4, 4, 0};
    case U'_': return {0, 0, 0, 0, 0, 0, 31};
    case U'A': return {14, 17, 17, 31, 17, 17, 17};
    case U'B': return {30, 17, 17, 30, 17, 17, 30};
    case U'C': return {14, 17, 16, 16, 16, 17, 14};
    case U'D': return {30, 17, 17, 17, 17, 17, 30};
    case U'E': return {31, 16, 16, 30, 16, 16, 31};
    case U'F': return {31, 16, 16, 30, 16, 16, 16};
    case U'G': return {14, 17, 16, 23, 17, 17, 14};
    case U'H': return {17, 17, 17, 31, 17, 17, 17};
    case U'I': return {14, 4, 4, 4, 4, 4, 14};
    case U'J': return {7, 2, 2, 2, 18, 18, 12};
    case U'K': return {17, 18, 20, 24, 20, 18, 17};
    case U'L': return {16, 16, 16, 16, 16, 16, 31};
    case U'M': return {17, 27, 21, 21, 17, 17, 17};
    case U'N': return {17, 25, 21, 19, 17, 17, 17};
    case U'O': return {14, 17, 17, 17, 17, 17, 14};
    case U'P': return {30, 17, 17, 30, 16, 16, 16};
    case U'Q': return {14, 17, 17, 17, 21, 18, 13};
    case U'R': return {30, 17, 17, 30, 20, 18, 17};
    case U'S': return {15, 16, 16, 14, 1, 1, 30};
    case U'T': return {31, 4, 4, 4, 4, 4, 4};
    case U'U': return {17, 17, 17, 17, 17, 17, 14};
    case U'V': return {17, 17, 17, 17, 17, 10, 4};
    case U'W': return {17, 17, 17, 21, 21, 21, 10};
    case U'X': return {17, 17, 10, 4, 10, 17, 17};
    case U'Y': return {17, 17, 10, 4, 4, 4, 4};
    case U'Z': return {31, 1, 2, 4, 8, 16, 31};
    case U'0': return {14, 17, 19, 21, 25, 17, 14};
    case U'1': return {4, 12, 4, 4, 4, 4, 14};
    case U'2': return {14, 17, 1, 2, 4, 8, 31};
    case U'3': return {30, 1, 1, 14, 1, 1, 30};
    case U'4': return {2, 6, 10, 18, 31, 2, 2};
    case U'5': return {31, 16, 16, 30, 1, 1, 30};
    case U'6': return {14, 16, 16, 30, 17, 17, 14};
    case U'7': return {31, 1, 2, 4, 8, 8, 8};
    case U'8': return {14, 17, 17, 14, 17, 17, 14};
    case U'9': return {14, 17, 17, 15, 1, 1, 14};
    case U'.': return {0, 0, 0, 0, 0, 12, 12};
    case U',': return {0, 0, 0, 0, 0, 12, 8};
    case U':': return {0, 12, 12, 0, 12, 12, 0};
    case U'!': return {4, 4, 4, 4, 4, 0, 4};
    case U'?': return {14, 17, 1, 2, 4, 0, 4};
    case U'-': return {0, 0, 0, 31, 0, 0, 0};
    case U'/': return {1, 1, 2, 4, 8, 16, 16};
    case U'\'': return {4, 4, 2, 0, 0, 0, 0};
    case U'(': return {2, 4, 8, 8, 8, 4, 2};
    case U')': return {8, 4, 2, 2, 2, 4, 8};
    case U'Æ': return {15, 20, 20, 31, 20, 20, 23};
    case U'Ø': return {15, 19, 21, 21, 21, 25, 30};
    case U'Å': return {4, 10, 4, 14, 17, 31, 17};
    default: return {14, 17, 1, 2, 4, 0, 4};
    }
}

}  // namespace

Typography::Typography(const std::string& font_directory) {
    const std::filesystem::path directory(font_directory);
    regular_ = Font::load((directory / kRegularFile).string());
    semi_bold_ = Font::load((directory / kSemiBoldFile).string());
}

std::string Typography::find_font_directory() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (const char* configured = std::getenv("FRAME_NOTIFY_FONT_DIR");
        configured != nullptr && *configured != '\0') {
        candidates.emplace_back(configured);
    }
    std::error_code error;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    if (!error && executable.has_parent_path()) {
        candidates.push_back(executable.parent_path() / "fonts");
    }
#ifdef FRAME_NOTIFY_FONT_DIR
    candidates.emplace_back(FRAME_NOTIFY_FONT_DIR);
#endif
    candidates.emplace_back("fonts");
    candidates.emplace_back("src/ui/fonts");
    candidates.emplace_back("../src/ui/fonts");
    for (const auto& candidate : candidates) {
        error.clear();
        if (fs::exists(candidate / kRegularFile, error) && !error) return candidate.string();
    }
    return {};
}

const Typography& Typography::shared() {
    static const Typography instance = [] {
        const std::string directory = find_font_directory();
        Typography typography(directory);
        if (typography.has_fonts()) {
            std::cout << "[UI] Fonts loaded from " << directory << '\n';
        } else {
            std::cerr << "[UI] Inter fonts not found; using the built-in bitmap font. Expected "
                      << kRegularFile << " in a 'fonts' directory beside the executable.\n";
        }
        return typography;
    }();
    return instance;
}

const Font* Typography::font_for(FontWeight weight) const noexcept {
    if (weight == FontWeight::kSemiBold && semi_bold_ != nullptr) return semi_bold_.get();
    return regular_.get();
}

float Typography::legacy_scale(const TextStyle& style) const noexcept {
    return std::max(1.0F, std::round(style.size / 11.0F));
}

float Typography::ascent(const TextStyle& style) const {
    if (const Font* font = font_for(style.weight)) {
        return static_cast<float>(font->ascender()) * style.size /
               static_cast<float>(font->units_per_em());
    }
    return 7.0F * legacy_scale(style);
}

float Typography::cap_height(const TextStyle& style) const {
    if (const Font* font = font_for(style.weight)) {
        return static_cast<float>(font->cap_height()) * style.size /
               static_cast<float>(font->units_per_em());
    }
    return 7.0F * legacy_scale(style);
}

float Typography::shape(std::string_view text, const TextStyle& style,
                        std::vector<Shaped>* output) const {
    const Font* font = font_for(style.weight);
    const float scale = font != nullptr ? style.size / static_cast<float>(font->units_per_em()) : 0.0F;
    const float legacy_advance = 6.0F * legacy_scale(style);

    float pen = 0.0F;
    std::uint16_t previous = 0;
    bool previous_missing = false;
    std::size_t offset = 0;
    while (offset < text.size()) {
        char32_t code_point = next_code_point(text, offset);
        if (is_ignorable(code_point) && code_point != U'\t' && code_point != U'\n' &&
            code_point != U'\r') {
            continue;
        }
        if (code_point == U'\t' || code_point == U'\n' || code_point == U'\r') code_point = U' ';

        if (font == nullptr) {
            if (output != nullptr) output->push_back({code_point, 0, pen});
            pen += legacy_advance + style.tracking;
            continue;
        }

        std::uint16_t glyph = font->glyph_index(code_point);
        if (glyph == 0) {
            // Emoji and other unsupported symbols: one placeholder box per run, not one per part.
            if (previous_missing) continue;
            previous_missing = true;
        } else {
            previous_missing = false;
        }
        if (previous != 0 && glyph != 0) {
            pen += static_cast<float>(font->kerning(previous, glyph)) * scale;
        }
        if (output != nullptr) output->push_back({code_point, glyph, pen});
        pen += static_cast<float>(font->advance(glyph)) * scale + style.tracking;
        previous = glyph;
    }
    return std::max(0.0F, pen - (pen > 0.0F ? style.tracking : 0.0F));
}

float Typography::measure(std::string_view text, const TextStyle& style) const {
    return shape(text, style, nullptr);
}

const GlyphImage& Typography::glyph_image(const Shaped& item, const TextStyle& style,
                                          int subpixel) const {
    const Font* font = font_for(style.weight);
    const auto size_key = static_cast<std::uint64_t>(std::lround(style.size * 8.0F)) & 0xFFFFU;
    const std::uint64_t key = static_cast<std::uint64_t>(item.code_point) |
                              (size_key << 24U) |
                              (static_cast<std::uint64_t>(subpixel) << 44U) |
                              (static_cast<std::uint64_t>(style.weight == FontWeight::kSemiBold) << 48U) |
                              (static_cast<std::uint64_t>(font == nullptr) << 49U);
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) return cached->second;

    GlyphImage image;
    if (font == nullptr) {
        const auto scale = static_cast<int>(legacy_scale(style));
        const LegacyGlyph rows = legacy_glyph(item.code_point);
        image.left = 0;
        image.top = -7 * scale;
        image.width = 5 * scale;
        image.height = 7 * scale;
        image.alpha.assign(static_cast<std::size_t>(image.width * image.height), 0);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((rows[static_cast<std::size_t>(row)] & (1U << (4 - column))) == 0U) continue;
                for (int y = 0; y < scale; ++y) {
                    for (int x = 0; x < scale; ++x) {
                        image.alpha[static_cast<std::size_t>((row * scale + y) * image.width +
                                                             column * scale + x)] = 255;
                    }
                }
            }
        }
    } else {
        const float scale = style.size / static_cast<float>(font->units_per_em());
        const float shift = static_cast<float>(subpixel) / static_cast<float>(kSubpixelSteps);
        Path scaled;
        if (item.glyph == 0) {
            // Emoji and other symbols the font lacks: a small smiley says "something goes here"
            // far better than the font's striped .notdef box.
            constexpr float kPi = 3.14159265358979F;
            const float advance = static_cast<float>(font->advance(0)) * scale;
            const float radius = std::min(0.37F * style.size, advance * 0.46F);
            const Point center{advance * 0.5F + shift, -0.36F * style.size};
            scaled.add_circle(center, radius);
            scaled.add_circle(center, radius - 0.07F * style.size, true);
            scaled.add_circle({center.x - radius * 0.36F, center.y - radius * 0.22F}, radius * 0.13F);
            scaled.add_circle({center.x + radius * 0.36F, center.y - radius * 0.22F}, radius * 0.13F);
            const float smile_radius = radius * 0.5F;
            const auto smile_point = [&](float degrees) {
                const float angle = degrees * kPi / 180.0F;
                return Point{center.x + smile_radius * std::cos(angle),
                             center.y - radius * 0.05F + smile_radius * std::sin(angle)};
            };
            for (int step = 0; step < 6; ++step) {
                const float from = 35.0F + static_cast<float>(step) * 18.33F;
                scaled.add_capsule(smile_point(from), smile_point(from + 18.33F), radius * 0.075F);
            }
        } else {
            const Path outline = font->outline(item.glyph);
            for (const auto& contour : outline.contours()) {
                if (contour.empty()) continue;
                scaled.move_to({contour[0].x * scale + shift, -contour[0].y * scale});
                for (std::size_t index = 1; index < contour.size(); ++index) {
                    scaled.line_to({contour[index].x * scale + shift, -contour[index].y * scale});
                }
                scaled.close();
            }
        }
        const Coverage coverage = rasterize(scaled, -4096, -4096, 4096, 4096);
        if (!coverage.empty()) {
            image.left = coverage.left;
            image.top = coverage.top;
            image.width = coverage.width;
            image.height = coverage.height;
            image.alpha.resize(coverage.alpha.size());
            const auto& curve = coverage_curve();
            for (std::size_t index = 0; index < coverage.alpha.size(); ++index) {
                image.alpha[index] = curve[static_cast<std::size_t>(
                    std::lround(std::clamp(coverage.alpha[index], 0.0F, 1.0F) * 255.0F))];
            }
        }
    }
    return cache_.emplace(key, std::move(image)).first->second;
}

std::vector<PlacedGlyph> Typography::layout(std::string_view text, const TextStyle& style,
                                            float x) const {
    std::vector<Shaped> shaped;
    static_cast<void>(shape(text, style, &shaped));
    std::vector<PlacedGlyph> placed;
    placed.reserve(shaped.size());
    for (const Shaped& item : shaped) {
        const float position = x + item.pen;
        const int whole = static_cast<int>(std::floor(position));
        const int subpixel = std::clamp(
            static_cast<int>((position - static_cast<float>(whole)) * static_cast<float>(kSubpixelSteps)),
            0, kSubpixelSteps - 1);
        const GlyphImage& image = glyph_image(item, style, subpixel);
        if (image.width > 0 && image.height > 0) placed.push_back({&image, whole});
    }
    return placed;
}

std::string Typography::fit_with_ellipsis(std::string text, const TextStyle& style,
                                          float max_width) const {
    constexpr char kEllipsis[] = "\xE2\x80\xA6";
    while (true) {
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (measure(text + kEllipsis, style) <= max_width || text.empty()) {
            return text + kEllipsis;
        }
        const auto offsets = code_point_offsets(text);
        text.resize(offsets[offsets.size() - 2U]);
    }
}

std::string Typography::ellipsize(std::string_view text, const TextStyle& style,
                                  float max_width) const {
    if (measure(text, style) <= max_width) return std::string(text);
    return fit_with_ellipsis(std::string(text), style, max_width);
}

std::vector<std::string> Typography::wrap(std::string_view text, const TextStyle& style,
                                          float max_width, int max_lines) const {
    std::vector<std::string> lines;
    if (max_lines <= 0 || max_width <= 0.0F) return lines;
    const auto limit = static_cast<std::size_t>(max_lines);
    bool truncated = false;

    const auto push = [&](std::string line) {
        if (lines.size() >= limit) {
            truncated = true;
            return;
        }
        lines.push_back(std::move(line));
    };

    std::size_t paragraph_start = 0;
    while (paragraph_start <= text.size() && !truncated) {
        std::size_t paragraph_end = text.find('\n', paragraph_start);
        if (paragraph_end == std::string_view::npos) paragraph_end = text.size();
        const std::string_view paragraph = text.substr(paragraph_start, paragraph_end - paragraph_start);
        paragraph_start = paragraph_end + 1U;

        std::string line;
        std::size_t position = 0;
        while (position < paragraph.size() && !truncated) {
            while (position < paragraph.size() &&
                   (paragraph[position] == ' ' || paragraph[position] == '\r')) {
                ++position;
            }
            std::size_t word_end = position;
            while (word_end < paragraph.size() && paragraph[word_end] != ' ' &&
                   paragraph[word_end] != '\r') {
                ++word_end;
            }
            if (word_end == position) break;
            const std::string word(paragraph.substr(position, word_end - position));
            position = word_end;

            const std::string candidate = line.empty() ? word : line + " " + word;
            if (measure(candidate, style) <= max_width) {
                line = candidate;
                continue;
            }
            if (!line.empty()) {
                push(std::move(line));
                line.clear();
                if (truncated) break;
            }
            if (measure(word, style) <= max_width) {
                line = word;
                continue;
            }
            // A single word wider than the line, such as a URL: break between characters.
            std::string chunk;
            std::size_t offset = 0;
            while (offset < word.size() && !truncated) {
                const std::size_t begin = offset;
                static_cast<void>(next_code_point(word, offset));
                const std::string piece = word.substr(begin, offset - begin);
                if (!chunk.empty() && measure(chunk + piece, style) > max_width) {
                    push(std::move(chunk));
                    chunk.clear();
                }
                chunk += piece;
            }
            line = std::move(chunk);
        }
        if (!line.empty() && !truncated) push(std::move(line));
        if (paragraph_end == text.size()) break;
    }

    if (truncated && !lines.empty()) {
        lines.back() = fit_with_ellipsis(std::move(lines.back()), style, max_width);
    }
    return lines;
}

}  // namespace frame_notify::ui
