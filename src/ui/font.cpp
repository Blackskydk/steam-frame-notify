#include "ui/font.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace frame_notify::ui {
namespace {

constexpr std::size_t kMaximumFontBytes = 16U * 1024U * 1024U;
constexpr int kMaximumPoints = 20000;
constexpr int kMaximumCompositeDepth = 6;
constexpr std::uint32_t kKernTag = 0x6B65726EU;  // 'kern'

int popcount(unsigned value) {
    int count = 0;
    for (; value != 0U; value >>= 1U) count += static_cast<int>(value & 1U);
    return count;
}

int value_record_size(unsigned format) {
    return 2 * popcount(format & 0xFFU);
}

}  // namespace

std::unique_ptr<Font> Font::load(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return nullptr;
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
    return from_memory(std::move(data));
}

std::unique_ptr<Font> Font::from_memory(std::vector<std::uint8_t> data) {
    if (data.size() < 12U || data.size() > kMaximumFontBytes) return nullptr;
    std::unique_ptr<Font> font(new Font());
    font->data_ = std::move(data);
    if (!font->parse()) return nullptr;
    return font;
}

bool Font::parse() {
    const std::uint32_t version = u32(0);
    if (version != 0x00010000U && version != 0x74727565U) return false;  // 1.0 or 'true'

    std::size_t head = 0;
    std::size_t hhea = 0;
    std::size_t maxp = 0;
    std::size_t cmap = 0;
    std::size_t gpos = 0;
    std::size_t os2 = 0;
    std::size_t os2_length = 0;
    const int table_count = u16(4);
    for (int index = 0; index < table_count; ++index) {
        const std::size_t record = 12U + 16U * static_cast<std::size_t>(index);
        const std::uint32_t tag = u32(record);
        const std::size_t offset = u32(record + 8U);
        const std::size_t length = u32(record + 12U);
        if (offset > data_.size() || length > data_.size() - offset) continue;
        switch (tag) {
        case 0x68656164U: head = offset; break;                             // head
        case 0x68686561U: hhea = offset; break;                             // hhea
        case 0x6D617870U: maxp = offset; break;                             // maxp
        case 0x686D7478U: hmtx_ = offset; break;                            // hmtx
        case 0x6C6F6361U: loca_ = offset; break;                            // loca
        case 0x676C7966U: glyf_ = offset; glyf_length_ = length; break;    // glyf
        case 0x636D6170U: cmap = offset; break;                             // cmap
        case 0x47504F53U: gpos = offset; break;                             // GPOS
        case 0x4F532F32U: os2 = offset; os2_length = length; break;        // OS/2
        default: break;
        }
    }
    if (head == 0 || hhea == 0 || maxp == 0 || hmtx_ == 0 || loca_ == 0 || glyf_ == 0 ||
        cmap == 0) {
        return false;
    }

    units_per_em_ = u16(head + 18U);
    if (units_per_em_ < 16 || units_per_em_ > 16384) return false;
    long_loca_ = i16(head + 50U) != 0;
    ascender_ = i16(hhea + 4U);
    descender_ = i16(hhea + 6U);
    line_gap_ = i16(hhea + 8U);
    horizontal_metric_count_ = u16(hhea + 34U);
    glyph_count_ = u16(maxp + 4U);
    if (glyph_count_ == 0 || horizontal_metric_count_ == 0) return false;

    cap_height_ = units_per_em_ * 7 / 10;
    x_height_ = units_per_em_ / 2;
    if (os2 != 0 && os2_length >= 90U && u16(os2) >= 2U) {
        if (i16(os2 + 88U) > 0) cap_height_ = i16(os2 + 88U);
        if (i16(os2 + 86U) > 0) x_height_ = i16(os2 + 86U);
    }

    // Prefer full Unicode (format 12), then the BMP table (format 4).
    int best_score = 0;
    const int cmap_count = u16(cmap + 2U);
    for (int index = 0; index < cmap_count; ++index) {
        const std::size_t record = cmap + 4U + 8U * static_cast<std::size_t>(index);
        const unsigned platform = u16(record);
        const unsigned encoding = u16(record + 2U);
        const std::size_t subtable = cmap + u32(record + 4U);
        if (subtable >= data_.size()) continue;
        const int format = u16(subtable);
        int score = 0;
        if (format == 12 && ((platform == 3U && encoding == 10U) || platform == 0U)) score = 4;
        if (format == 4 && ((platform == 3U && encoding == 1U) || platform == 0U)) score = 2;
        if (score > best_score) {
            best_score = score;
            cmap_subtable_ = subtable;
            cmap_format_ = format;
        }
    }
    if (best_score == 0) return false;

    if (gpos != 0) parse_kerning(gpos);
    return true;
}

void Font::parse_kerning(std::size_t gpos) {
    if (u16(gpos) != 1U) return;
    const std::size_t feature_list = gpos + u16(gpos + 6U);
    const std::size_t lookup_list = gpos + u16(gpos + 8U);
    std::set<int> lookup_indices;
    const int feature_count = u16(feature_list);
    for (int index = 0; index < feature_count; ++index) {
        const std::size_t record = feature_list + 2U + 6U * static_cast<std::size_t>(index);
        if (u32(record) != kKernTag) continue;
        const std::size_t feature = feature_list + u16(record + 4U);
        const int lookup_count = u16(feature + 2U);
        for (int item = 0; item < lookup_count; ++item) {
            lookup_indices.insert(u16(feature + 4U + 2U * static_cast<std::size_t>(item)));
        }
    }

    const int lookup_total = u16(lookup_list);
    for (const int lookup_index : lookup_indices) {
        if (lookup_index >= lookup_total) continue;
        const std::size_t lookup =
            lookup_list + u16(lookup_list + 2U + 2U * static_cast<std::size_t>(lookup_index));
        const int lookup_type = u16(lookup);
        const int subtable_count = u16(lookup + 4U);
        std::vector<std::size_t> subtables;
        for (int item = 0; item < subtable_count; ++item) {
            std::size_t subtable = lookup + u16(lookup + 6U + 2U * static_cast<std::size_t>(item));
            if (lookup_type == 9) {
                if (u16(subtable) != 1U || u16(subtable + 2U) != 2U) continue;
                subtable += u32(subtable + 4U);
            } else if (lookup_type != 2) {
                continue;
            }
            if (subtable < data_.size()) subtables.push_back(subtable);
        }
        if (!subtables.empty()) kerning_lookups_.push_back(std::move(subtables));
    }
}

std::uint16_t Font::glyph_index(char32_t code_point) const {
    if (cmap_format_ == 4) {
        if (code_point > 0xFFFFU) return 0;
        const auto target = static_cast<unsigned>(code_point);
        const std::size_t table = cmap_subtable_;
        const std::size_t segment_count = u16(table + 6U) / 2U;
        const std::size_t end_codes = table + 14U;
        const std::size_t start_codes = end_codes + segment_count * 2U + 2U;
        const std::size_t deltas = start_codes + segment_count * 2U;
        const std::size_t range_offsets = deltas + segment_count * 2U;
        for (std::size_t segment = 0; segment < segment_count; ++segment) {
            if (target > u16(end_codes + segment * 2U)) continue;
            const unsigned start = u16(start_codes + segment * 2U);
            if (target < start) return 0;
            const unsigned delta = u16(deltas + segment * 2U);
            const unsigned range_offset = u16(range_offsets + segment * 2U);
            if (range_offset == 0U) return static_cast<std::uint16_t>((target + delta) & 0xFFFFU);
            const std::size_t address =
                range_offsets + segment * 2U + range_offset + 2U * (target - start);
            const unsigned glyph = u16(address);
            return glyph == 0U ? std::uint16_t{0}
                               : static_cast<std::uint16_t>((glyph + delta) & 0xFFFFU);
        }
        return 0;
    }
    if (cmap_format_ == 12) {
        const std::size_t table = cmap_subtable_;
        std::size_t low = 0;
        std::size_t high = u32(table + 12U);
        while (low < high) {
            const std::size_t middle = low + (high - low) / 2U;
            const std::size_t group = table + 16U + 12U * middle;
            if (code_point < u32(group)) {
                high = middle;
            } else if (code_point > u32(group + 4U)) {
                low = middle + 1U;
            } else {
                return static_cast<std::uint16_t>(u32(group + 8U) + (code_point - u32(group)));
            }
        }
    }
    return 0;
}

int Font::advance(std::uint16_t glyph) const {
    const int metric = std::min<int>(glyph, horizontal_metric_count_ - 1);
    return u16(hmtx_ + 4U * static_cast<std::size_t>(metric));
}

int Font::coverage_index(std::size_t coverage, std::uint16_t glyph) const {
    const int format = u16(coverage);
    const int count = u16(coverage + 2U);
    if (format == 1) {
        int low = 0;
        int high = count - 1;
        while (low <= high) {
            const int middle = (low + high) / 2;
            const unsigned value = u16(coverage + 4U + 2U * static_cast<std::size_t>(middle));
            if (glyph == value) return middle;
            if (glyph < value) high = middle - 1; else low = middle + 1;
        }
    } else if (format == 2) {
        int low = 0;
        int high = count - 1;
        while (low <= high) {
            const int middle = (low + high) / 2;
            const std::size_t record = coverage + 4U + 6U * static_cast<std::size_t>(middle);
            if (glyph < u16(record)) {
                high = middle - 1;
            } else if (glyph > u16(record + 2U)) {
                low = middle + 1;
            } else {
                return u16(record + 4U) + (glyph - u16(record));
            }
        }
    }
    return -1;
}

int Font::glyph_class(std::size_t class_def, std::uint16_t glyph) const {
    const int format = u16(class_def);
    if (format == 1) {
        const unsigned start = u16(class_def + 2U);
        const unsigned count = u16(class_def + 4U);
        if (glyph >= start && glyph < start + count) {
            return u16(class_def + 6U + 2U * (glyph - start));
        }
    } else if (format == 2) {
        const int count = u16(class_def + 2U);
        for (int index = 0; index < count; ++index) {
            const std::size_t record = class_def + 4U + 6U * static_cast<std::size_t>(index);
            if (glyph >= u16(record) && glyph <= u16(record + 2U)) return u16(record + 4U);
        }
    }
    return 0;
}

bool Font::pair_adjustment(std::size_t subtable, std::uint16_t left, std::uint16_t right,
                           int& value) const {
    const int format = u16(subtable);
    const int covered = coverage_index(subtable + u16(subtable + 2U), left);
    if (covered < 0) return false;
    const unsigned format1 = u16(subtable + 4U);
    const unsigned format2 = u16(subtable + 6U);
    const auto first_size = static_cast<std::size_t>(value_record_size(format1));
    const auto second_size = static_cast<std::size_t>(value_record_size(format2));
    const auto x_advance = [this, format1](std::size_t position) {
        if ((format1 & 0x0004U) == 0U) return 0;
        return static_cast<int>(i16(position + 2U * static_cast<std::size_t>(popcount(format1 & 0x3U))));
    };

    if (format == 1) {
        if (covered >= u16(subtable + 8U)) return false;
        const std::size_t pair_set =
            subtable + u16(subtable + 10U + 2U * static_cast<std::size_t>(covered));
        const std::size_t record_size = 2U + first_size + second_size;
        int low = 0;
        int high = static_cast<int>(u16(pair_set)) - 1;
        while (low <= high) {
            const int middle = (low + high) / 2;
            const std::size_t record = pair_set + 2U + record_size * static_cast<std::size_t>(middle);
            const unsigned second = u16(record);
            if (right == second) {
                value = x_advance(record + 2U);
                return true;
            }
            if (right < second) high = middle - 1; else low = middle + 1;
        }
        return false;
    }
    if (format == 2) {
        const int first_class = glyph_class(subtable + u16(subtable + 8U), left);
        const int second_class = glyph_class(subtable + u16(subtable + 10U), right);
        const int first_count = u16(subtable + 12U);
        const int second_count = u16(subtable + 14U);
        if (first_class >= first_count || second_class >= second_count) return false;
        const std::size_t record_size = first_size + second_size;
        const std::size_t record =
            subtable + 16U +
            (static_cast<std::size_t>(first_class) * static_cast<std::size_t>(second_count) +
             static_cast<std::size_t>(second_class)) *
                record_size;
        value = x_advance(record);
        return true;
    }
    return false;
}

int Font::kerning(std::uint16_t left, std::uint16_t right) const {
    if (kerning_lookups_.empty() || left == 0 || right == 0) return 0;
    const std::uint32_t key = (static_cast<std::uint32_t>(left) << 16U) | right;
    const auto cached = kerning_cache_.find(key);
    if (cached != kerning_cache_.end()) return cached->second;

    int total = 0;
    for (const auto& lookup : kerning_lookups_) {
        for (const std::size_t subtable : lookup) {
            int value = 0;
            if (pair_adjustment(subtable, left, right, value)) {
                total += value;
                break;
            }
        }
    }
    kerning_cache_.emplace(key, total);
    return total;
}

Path Font::outline(std::uint16_t glyph) const {
    Path path;
    append_glyph(glyph, Transform{}, path, 0);
    return path;
}

void Font::append_glyph(std::uint16_t glyph, const Transform& transform, Path& path,
                        int depth) const {
    if (glyph >= glyph_count_ || depth > kMaximumCompositeDepth) return;
    std::size_t begin = 0;
    std::size_t end = 0;
    if (long_loca_) {
        begin = u32(loca_ + 4U * static_cast<std::size_t>(glyph));
        end = u32(loca_ + 4U * static_cast<std::size_t>(glyph) + 4U);
    } else {
        begin = 2U * static_cast<std::size_t>(u16(loca_ + 2U * static_cast<std::size_t>(glyph)));
        end = 2U * static_cast<std::size_t>(u16(loca_ + 2U * static_cast<std::size_t>(glyph) + 2U));
    }
    if (end <= begin || end > glyf_length_) return;

    std::size_t position = glyf_ + begin;
    const int contour_count = i16(position);
    position += 10U;

    const auto apply = [&transform](float x, float y) {
        return Point{transform.a * x + transform.c * y + transform.e,
                     transform.b * x + transform.d * y + transform.f};
    };

    if (contour_count >= 0) {
        if (contour_count == 0) return;
        std::vector<int> ends(static_cast<std::size_t>(contour_count));
        for (int index = 0; index < contour_count; ++index) {
            ends[static_cast<std::size_t>(index)] =
                u16(position + 2U * static_cast<std::size_t>(index));
        }
        position += 2U * static_cast<std::size_t>(contour_count);
        const int point_count = ends.back() + 1;
        if (point_count <= 0 || point_count > kMaximumPoints) return;
        position += 2U + u16(position);  // skip the hinting program

        std::vector<std::uint8_t> flags;
        flags.reserve(static_cast<std::size_t>(point_count));
        while (static_cast<int>(flags.size()) < point_count) {
            const std::uint8_t flag = u8(position++);
            flags.push_back(flag);
            if ((flag & 8U) != 0U) {
                int repeat = u8(position++);
                while (repeat-- > 0 && static_cast<int>(flags.size()) < point_count) {
                    flags.push_back(flag);
                }
            }
        }
        std::vector<int> xs(static_cast<std::size_t>(point_count));
        std::vector<int> ys(static_cast<std::size_t>(point_count));
        int coordinate = 0;
        for (int index = 0; index < point_count; ++index) {
            const std::uint8_t flag = flags[static_cast<std::size_t>(index)];
            if ((flag & 2U) != 0U) {
                const int delta = u8(position++);
                coordinate += (flag & 16U) != 0U ? delta : -delta;
            } else if ((flag & 16U) == 0U) {
                coordinate += i16(position);
                position += 2U;
            }
            xs[static_cast<std::size_t>(index)] = coordinate;
        }
        coordinate = 0;
        for (int index = 0; index < point_count; ++index) {
            const std::uint8_t flag = flags[static_cast<std::size_t>(index)];
            if ((flag & 4U) != 0U) {
                const int delta = u8(position++);
                coordinate += (flag & 32U) != 0U ? delta : -delta;
            } else if ((flag & 32U) == 0U) {
                coordinate += i16(position);
                position += 2U;
            }
            ys[static_cast<std::size_t>(index)] = coordinate;
        }

        int first = 0;
        for (const int last : ends) {
            const int count = last - first + 1;
            if (count >= 2) {
                std::vector<GlyphPoint> points;
                points.reserve(static_cast<std::size_t>(count));
                for (int index = first; index <= last; ++index) {
                    const auto item = static_cast<std::size_t>(index);
                    const Point moved = apply(static_cast<float>(xs[item]), static_cast<float>(ys[item]));
                    points.push_back({moved.x, moved.y, (flags[item] & 1U) != 0U});
                }
                const auto midpoint = [](const GlyphPoint& a, const GlyphPoint& b) {
                    return Point{(a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F};
                };
                const auto size = points.size();
                Point start{};
                std::size_t next = 0;
                std::size_t remaining = size;
                if (points[0].on_curve) {
                    start = {points[0].x, points[0].y};
                    next = 1;
                    remaining = size - 1U;
                } else if (points[size - 1U].on_curve) {
                    start = {points[size - 1U].x, points[size - 1U].y};
                    remaining = size - 1U;
                } else {
                    start = midpoint(points[0], points[size - 1U]);
                }
                path.move_to(start);
                bool has_control = false;
                Point control{};
                for (std::size_t step = 0; step < remaining; ++step) {
                    const GlyphPoint& item = points[(next + step) % size];
                    if (item.on_curve) {
                        if (has_control) path.quad_to(control, {item.x, item.y});
                        else path.line_to({item.x, item.y});
                        has_control = false;
                    } else {
                        if (has_control) {
                            const Point middle = midpoint({control.x, control.y, false}, item);
                            path.quad_to(control, middle);
                        }
                        control = {item.x, item.y};
                        has_control = true;
                    }
                }
                if (has_control) path.quad_to(control, start);
                path.close();
            }
            first = last + 1;
        }
        return;
    }

    // Composite glyph: a list of transformed references to other glyphs.
    for (int guard = 0; guard < 64; ++guard) {
        const unsigned flags = u16(position);
        const std::uint16_t component = u16(position + 2U);
        position += 4U;
        float dx = 0.0F;
        float dy = 0.0F;
        if ((flags & 0x0001U) != 0U) {
            if ((flags & 0x0002U) != 0U) {
                dx = static_cast<float>(i16(position));
                dy = static_cast<float>(i16(position + 2U));
            }
            position += 4U;
        } else {
            if ((flags & 0x0002U) != 0U) {
                dx = static_cast<float>(static_cast<std::int8_t>(u8(position)));
                dy = static_cast<float>(static_cast<std::int8_t>(u8(position + 1U)));
            }
            position += 2U;
        }
        const auto fixed = [this](std::size_t at) {
            return static_cast<float>(i16(at)) / 16384.0F;
        };
        Transform local;
        local.e = dx;
        local.f = dy;
        if ((flags & 0x0008U) != 0U) {
            local.a = local.d = fixed(position);
            position += 2U;
        } else if ((flags & 0x0040U) != 0U) {
            local.a = fixed(position);
            local.d = fixed(position + 2U);
            position += 4U;
        } else if ((flags & 0x0080U) != 0U) {
            local.a = fixed(position);
            local.b = fixed(position + 2U);
            local.c = fixed(position + 4U);
            local.d = fixed(position + 6U);
            position += 8U;
        }
        Transform combined;
        combined.a = transform.a * local.a + transform.c * local.b;
        combined.b = transform.b * local.a + transform.d * local.b;
        combined.c = transform.a * local.c + transform.c * local.d;
        combined.d = transform.b * local.c + transform.d * local.d;
        combined.e = transform.a * local.e + transform.c * local.f + transform.e;
        combined.f = transform.b * local.e + transform.d * local.f + transform.f;
        append_glyph(component, combined, path, depth + 1);
        if ((flags & 0x0020U) == 0U) break;
    }
}

}  // namespace frame_notify::ui
