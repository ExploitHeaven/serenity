/*
 * Copyright (c) 2023-2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Debug.h>
#include <AK/IntegralMath.h>
#include <AK/Types.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/EdgeFlagPathRasterizer.h>

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

// This an implementation of edge-flag scanline AA, as described in:
// https://mlab.taik.fi/~kkallio/antialiasing/EdgeFlagAA.pdf

namespace Gfx {

static Vector<Detail::Edge> prepare_edges(ReadonlySpan<FloatLine> lines, unsigned samples_per_pixel, FloatPoint origin,
    int top_clip_scanline, int bottom_clip_scanline, int& min_edge_y, int& max_edge_y)
{
    Vector<Detail::Edge> edges;
    edges.ensure_capacity(lines.size());
    // The first visible y value.
    auto top_clip = top_clip_scanline * int(samples_per_pixel);
    // The last visible y value.
    auto bottom_clip = (bottom_clip_scanline + 1) * int(samples_per_pixel) - 1;
    min_edge_y = bottom_clip;
    max_edge_y = top_clip;

    for (auto& line : lines) {
        auto p0 = line.a() - origin;
        auto p1 = line.b() - origin;

        p0.scale_by(1, samples_per_pixel);
        p1.scale_by(1, samples_per_pixel);

        i8 winding = -1;
        if (p0.y() > p1.y()) {
            swap(p0, p1);
        } else {
            winding = 1;
        }

        if (p0.y() == p1.y())
            continue;

        auto min_y = static_cast<int>(p0.y());
        auto max_y = static_cast<int>(p1.y());

        // Clip edges that start below the bottom clip...
        if (min_y > bottom_clip)
            continue;
        // ...and edges that end before the top clip.
        if (max_y < top_clip)
            continue;

        auto start_x = p0.x();
        auto end_x = p1.x();
        auto dx = end_x - start_x;
        auto dy = max_y - min_y;

        if (dy == 0)
            continue;

        auto dxdy = dx / dy;

        // Trim off the non-visible portions of the edge.
        if (min_y < top_clip) {
            start_x += dxdy * (top_clip - min_y);
            min_y = top_clip;
        }
        if (max_y > bottom_clip) {
            max_y = bottom_clip;
        }

        min_edge_y = min(min_y, min_edge_y);
        max_edge_y = max(max_y, max_edge_y);

        edges.unchecked_append(Detail::Edge {
            start_x,
            min_y,
            max_y,
            dxdy,
            winding,
            nullptr });
    }
    return edges;
}

template<unsigned SamplesPerPixel>
EdgeFlagPathRasterizer<SamplesPerPixel>::EdgeFlagPathRasterizer(IntSize size)
    : m_size(size.width() + 1, size.height() + 1)
{
    // FIXME: Clip the scanline width to the visible section (tricky).
    m_scanline.resize(m_size.width());
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fill(Painter& painter, Path const& path, Color color, Painter::WindingRule winding_rule, FloatPoint offset)
{
    fill_internal(painter, path, color, winding_rule, offset);
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fill(Painter& painter, Path const& path, PaintStyle const& style, float opacity, Painter::WindingRule winding_rule, FloatPoint offset)
{
    style.paint(enclosing_int_rect(path.bounding_box()), [&](PaintStyle::SamplerFunction sampler) {
        if (opacity == 0.0f)
            return;
        if (opacity != 1.0f) {
            return fill_internal(
                painter, path, [=, sampler = move(sampler)](IntPoint point) {
                    return sampler(point).with_opacity(opacity);
                },
                winding_rule, offset);
        }
        return fill_internal(painter, path, move(sampler), winding_rule, offset);
    });
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fill_internal(Painter& painter, Path const& path, auto color_or_function, Painter::WindingRule winding_rule, FloatPoint offset)
{
    // FIXME: Figure out how painter scaling works here...
    VERIFY(painter.scale() == 1);

    auto bounding_box = enclosing_int_rect(path.bounding_box().translated(offset));
    auto dest_rect = bounding_box.translated(painter.translation());
    auto origin = bounding_box.top_left().to_type<float>() - offset;
    m_blit_origin = dest_rect.top_left();
    m_clip = dest_rect.intersected(painter.clip_rect());

    if (m_clip.is_empty())
        return;

    auto& lines = path.split_lines();
    if (lines.is_empty())
        return;

    int min_edge_y = 0;
    int max_edge_y = 0;
    auto top_clip_scanline = m_clip.top() - m_blit_origin.y();
    auto bottom_clip_scanline = m_clip.bottom() - m_blit_origin.y() - 1;
    auto edges = prepare_edges(lines, SamplesPerPixel, origin, top_clip_scanline, bottom_clip_scanline, min_edge_y, max_edge_y);
    if (edges.is_empty())
        return;

    int min_scanline = min_edge_y / SamplesPerPixel;
    int max_scanline = max_edge_y / SamplesPerPixel;
    m_edge_table.set_scanline_range(min_scanline, max_scanline);
    for (auto& edge : edges) {
        // Create a linked-list of edges starting on this scanline:
        int start_scanline = edge.min_y / SamplesPerPixel;
        edge.next_edge = m_edge_table[start_scanline];
        m_edge_table[start_scanline] = &edge;
    }

    auto empty_edge_extent = [&] {
        return EdgeExtent { m_size.width() - 1, 0 };
    };

    auto for_each_sample = [&](Detail::Edge& edge, int start_subpixel_y, int end_subpixel_y, EdgeExtent& edge_extent, auto callback) {
        for (int y = start_subpixel_y; y < end_subpixel_y; y++) {
            auto xi = static_cast<int>(edge.x + SubpixelSample::nrooks_subpixel_offsets[y]);
            if (xi < 0 || size_t(xi) >= m_scanline.size()) {
                // FIXME: For very low dxdy values, floating point error can push the sample outside the scanline.
                // This does not seem to make a visible difference most of the time (and is more likely from generated
                // paths, such as this 3D canvas demo: https://www.kevs3d.co.uk/dev/html5logo/).
                dbgln_if(FILL_PATH_DEBUG, "fill_path: Sample out of bounds: {} not  in [0, {})", xi, m_scanline.size());
                return;
            }
            SampleType sample = 1 << y;
            callback(xi, y, sample);
            edge.x += edge.dxdy;
            edge_extent.min_x = min(edge_extent.min_x, xi);
            edge_extent.max_x = max(edge_extent.max_x, xi);
        }
    };

    Detail::Edge* active_edges = nullptr;

    if (winding_rule == Painter::WindingRule::EvenOdd) {
        auto plot_edge = [&](Detail::Edge& edge, int start_subpixel_y, int end_subpixel_y, EdgeExtent& edge_extent) {
            for_each_sample(edge, start_subpixel_y, end_subpixel_y, edge_extent, [&](int xi, int, SampleType sample) {
                m_scanline[xi] ^= sample;
            });
        };
        for (int scanline = min_scanline; scanline <= max_scanline; scanline++) {
            auto edge_extent = empty_edge_extent();
            active_edges = plot_edges_for_scanline(scanline, plot_edge, edge_extent, active_edges);
            write_scanline<Painter::WindingRule::EvenOdd>(painter, scanline, edge_extent, color_or_function);
        }
    } else {
        VERIFY(winding_rule == Painter::WindingRule::Nonzero);
        // Only allocate the winding buffer if needed.
        // NOTE: non-zero fills are a fair bit less efficient. So if you can do an even-odd fill do that :^)
        if (m_windings.is_empty())
            m_windings.resize(m_size.width());

        auto plot_edge = [&](Detail::Edge& edge, int start_subpixel_y, int end_subpixel_y, EdgeExtent& edge_extent) {
            for_each_sample(edge, start_subpixel_y, end_subpixel_y, edge_extent, [&](int xi, int y, SampleType sample) {
                m_scanline[xi] |= sample;
                m_windings[xi].counts[y] += edge.winding;
            });
        };
        for (int scanline = min_scanline; scanline <= max_scanline; scanline++) {
            auto edge_extent = empty_edge_extent();
            active_edges = plot_edges_for_scanline(scanline, plot_edge, edge_extent, active_edges);
            write_scanline<Painter::WindingRule::Nonzero>(painter, scanline, edge_extent, color_or_function);
        }
    }
}

ALWAYS_INLINE static auto switch_on_color_or_function(auto& color_or_function, auto color_case, auto function_case)
{
    using ColorOrFunction = decltype(color_or_function);
    constexpr bool has_constant_color = IsSame<RemoveCVReference<ColorOrFunction>, Color>;
    if constexpr (has_constant_color) {
        return color_case(color_or_function);
    } else {
        return function_case(color_or_function);
    }
}

template<unsigned SamplesPerPixel>
Color EdgeFlagPathRasterizer<SamplesPerPixel>::scanline_color(int scanline, int offset, u8 alpha, auto& color_or_function)
{
    auto color = switch_on_color_or_function(
        color_or_function, [](Color color) { return color; },
        [&](auto& function) {
            return function({ offset, scanline });
        });
    return color.with_alpha(color.alpha() * alpha / 255);
}

template<unsigned SamplesPerPixel>
Detail::Edge* EdgeFlagPathRasterizer<SamplesPerPixel>::plot_edges_for_scanline(int scanline, auto plot_edge, EdgeExtent& edge_extent, Detail::Edge* active_edges)
{
    auto y_subpixel = [](int y) {
        return y & (SamplesPerPixel - 1);
    };

    auto* current_edge = active_edges;
    Detail::Edge* prev_edge = nullptr;

    // First iterate over the edge in the active edge table, these are edges added on earlier scanlines,
    // that have not yet reached their end scanline.
    while (current_edge) {
        int end_scanline = current_edge->max_y / SamplesPerPixel;
        if (scanline == end_scanline) {
            // This edge ends this scanline.
            plot_edge(*current_edge, 0, y_subpixel(current_edge->max_y), edge_extent);
            // Remove this edge from the AET
            current_edge = current_edge->next_edge;
            if (prev_edge)
                prev_edge->next_edge = current_edge;
            else
                active_edges = current_edge;
        } else {
            // This edge sticks around for a few more scanlines.
            plot_edge(*current_edge, 0, SamplesPerPixel, edge_extent);
            prev_edge = current_edge;
            current_edge = current_edge->next_edge;
        }
    }

    // Next, iterate over new edges for this line. If active_edges was null this also becomes the new
    // AET. Edges new will be appended here.
    current_edge = m_edge_table[scanline];
    while (current_edge) {
        int end_scanline = current_edge->max_y / SamplesPerPixel;
        if (scanline == end_scanline) {
            // This edge will end this scanlines (no need to add to AET).
            plot_edge(*current_edge, y_subpixel(current_edge->min_y), y_subpixel(current_edge->max_y), edge_extent);
        } else {
            // This edge will live on for a few more scanlines.
            plot_edge(*current_edge, y_subpixel(current_edge->min_y), SamplesPerPixel, edge_extent);
            // Add this edge to the AET
            if (prev_edge)
                prev_edge->next_edge = current_edge;
            else
                active_edges = current_edge;
            prev_edge = current_edge;
        }
        current_edge = current_edge->next_edge;
    }

    if (prev_edge)
        prev_edge->next_edge = nullptr;

    m_edge_table[scanline] = nullptr;
    return active_edges;
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::accumulate_even_odd_scanline(EdgeExtent edge_extent, auto sample_callback)
{
    SampleType sample = 0;
    for (int x = edge_extent.min_x; x <= edge_extent.max_x; x += 1) {
        sample ^= m_scanline[x];
        sample_callback(x, sample);
        m_scanline[x] = 0;
    }
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::accumulate_non_zero_scanline(EdgeExtent edge_extent, auto sample_callback)
{
    SampleType sample = 0;
    WindingCounts sum_winding = {};
    for (int x = edge_extent.min_x; x <= edge_extent.max_x; x += 1) {
        if (auto edges = m_scanline[x]) {
            // We only need to process the windings when we hit some edges.
            for (auto y_sub = 0u; y_sub < SamplesPerPixel; y_sub++) {
                auto subpixel_bit = 1 << y_sub;
                if (edges & subpixel_bit) {
                    auto winding = m_windings[x].counts[y_sub];
                    auto previous_winding_count = sum_winding.counts[y_sub];
                    sum_winding.counts[y_sub] += winding;
                    // Toggle fill on change to/from zero.
                    if (bool(previous_winding_count) ^ bool(sum_winding.counts[y_sub]))
                        sample ^= subpixel_bit;
                }
            }
        }
        sample_callback(x, sample);
        m_scanline[x] = 0;
        m_windings[x] = {};
    }
}

template<unsigned SamplesPerPixel>
template<Painter::WindingRule WindingRule, typename Callback>
void EdgeFlagPathRasterizer<SamplesPerPixel>::accumulate_scanline(EdgeExtent edge_extent, Callback callback)
{
    if constexpr (WindingRule == Painter::WindingRule::EvenOdd)
        accumulate_even_odd_scanline(edge_extent, callback);
    else
        accumulate_non_zero_scanline(edge_extent, callback);
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::write_pixel(Painter& painter, int scanline, int offset, SampleType sample, auto& color_or_function)
{
    if (!sample)
        return;
    auto dest = IntPoint { offset, scanline } + m_blit_origin;
    if (!m_clip.contains_horizontally(dest.x()))
        return;
    auto coverage = SubpixelSample::compute_coverage(sample);
    auto paint_color = scanline_color(scanline, offset, coverage_to_alpha(coverage), color_or_function);
    painter.set_physical_pixel(dest, paint_color, true);
}

template<unsigned SamplesPerPixel>
void EdgeFlagPathRasterizer<SamplesPerPixel>::fast_fill_solid_color_span(Painter& painter, int scanline, int start, int end, Color color)
{
    auto dest_y = scanline + m_blit_origin.y();
    auto start_x = max(m_clip.left(), start + m_blit_origin.x());
    auto end_x = min(m_clip.right() - 1, end + m_blit_origin.x());
    if (start_x > end_x)
        return;
    auto* dest_ptr = painter.target()->scanline(dest_y) + start_x;
    fast_u32_fill(dest_ptr, color.value(), end_x - start_x + 1);
}

template<unsigned SamplesPerPixel>
template<Painter::WindingRule WindingRule>
void EdgeFlagPathRasterizer<SamplesPerPixel>::write_scanline(Painter& painter, int scanline, EdgeExtent edge_extent, auto& color_or_function)
{
    // Simple case: Handle each pixel individually.
    // Used for PaintStyle fills and semi-transparent colors.
    auto write_scanline_pixelwise = [&](auto& color_or_function) {
        accumulate_scanline<WindingRule>(edge_extent, [&](int x, SampleType sample) {
            write_pixel(painter, scanline, x, sample, color_or_function);
        });
    };
    // Fast fill case: Track spans of solid color and set the entire span via a fast_u32_fill().
    // Used for opaque colors (i.e. alpha == 255).
    auto write_scanline_with_fast_fills = [&](Color color) {
        if (color.alpha() != 255)
            return write_scanline_pixelwise(color);
        constexpr SampleType full_converage = NumericLimits<SampleType>::max();
        int full_converage_count = 0;
        accumulate_scanline<WindingRule>(edge_extent, [&](int x, SampleType sample) {
            if (sample == full_converage) {
                full_converage_count++;
                return;
            } else {
                write_pixel(painter, scanline, x, sample, color);
            }
            if (full_converage_count > 0) {
                fast_fill_solid_color_span(painter, scanline, x - full_converage_count, x - 1, color);
                full_converage_count = 0;
            }
        });
        if (full_converage_count > 0)
            fast_fill_solid_color_span(painter, scanline, edge_extent.max_x - full_converage_count, edge_extent.max_x, color);
    };
    switch_on_color_or_function(
        color_or_function, write_scanline_with_fast_fills, write_scanline_pixelwise);
}

static IntSize path_bounds(Gfx::Path const& path)
{
    return enclosing_int_rect(path.bounding_box()).size();
}

// Note: The AntiAliasingPainter and Painter now perform the same antialiasing,
// since it would be harder to turn it off for the standard painter.
// The samples are reduced to 8 for Gfx::Painter though as a "speedy" option.

void Painter::fill_path(Path const& path, Color color, WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<8> rasterizer(path_bounds(path));
    rasterizer.fill(*this, path, color, winding_rule);
}

void Painter::fill_path(Path const& path, PaintStyle const& paint_style, float opacity, Painter::WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<8> rasterizer(path_bounds(path));
    rasterizer.fill(*this, path, paint_style, opacity, winding_rule);
}

void AntiAliasingPainter::fill_path(Path const& path, Color color, Painter::WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<32> rasterizer(path_bounds(path));
    rasterizer.fill(m_underlying_painter, path, color, winding_rule, m_transform.translation());
}

void AntiAliasingPainter::fill_path(Path const& path, PaintStyle const& paint_style, float opacity, Painter::WindingRule winding_rule)
{
    EdgeFlagPathRasterizer<32> rasterizer(path_bounds(path));
    rasterizer.fill(m_underlying_painter, path, paint_style, opacity, winding_rule, m_transform.translation());
}

template class EdgeFlagPathRasterizer<8>;
template class EdgeFlagPathRasterizer<16>;
template class EdgeFlagPathRasterizer<32>;

}
