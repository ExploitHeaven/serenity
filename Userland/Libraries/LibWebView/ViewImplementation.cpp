/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/String.h>
#include <LibCore/DateTime.h>
#include <LibCore/StandardPaths.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

ViewImplementation::ViewImplementation()
{
    m_backing_store_shrink_timer = Core::Timer::create_single_shot(3000, [this] {
        resize_backing_stores_if_needed(WindowResizeInProgress::No);
    }).release_value_but_fixme_should_propagate_errors();

    m_repeated_crash_timer = Core::Timer::create_single_shot(1000, [this] {
        // Reset the "crashing a lot" counter after 1 second in case we just
        // happen to be visiting crashy websites a lot.
        this->m_crash_count = 0;
    }).release_value_but_fixme_should_propagate_errors();

    on_request_file = [this](auto const& path, auto request_id) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);

        if (file.is_error())
            client().async_handle_file_return(file.error().code(), {}, request_id);
        else
            client().async_handle_file_return(0, IPC::File(*file.value()), request_id);
    };
}

WebContentClient& ViewImplementation::client()
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

WebContentClient const& ViewImplementation::client() const
{
    VERIFY(m_client_state.client);
    return *m_client_state.client;
}

void ViewImplementation::server_did_paint(Badge<WebContentClient>, i32 bitmap_id, Gfx::IntSize size)
{
    if (m_client_state.back_bitmap.id == bitmap_id) {
        m_client_state.has_usable_bitmap = true;
        m_client_state.back_bitmap.last_painted_size = size.to_type<Web::DevicePixels>();
        swap(m_client_state.back_bitmap, m_client_state.front_bitmap);
        m_backup_bitmap = nullptr;
        if (on_ready_to_paint)
            on_ready_to_paint();
    }
}

void ViewImplementation::load(AK::URL const& url)
{
    m_url = url;
    client().async_load_url(url);
}

void ViewImplementation::load_html(StringView html)
{
    client().async_load_html(html);
}

void ViewImplementation::load_empty_document()
{
    load_html(""sv);
}

void ViewImplementation::zoom_in()
{
    if (m_zoom_level >= ZOOM_MAX_LEVEL)
        return;
    m_zoom_level += ZOOM_STEP;
    update_zoom();
}

void ViewImplementation::zoom_out()
{
    if (m_zoom_level <= ZOOM_MIN_LEVEL)
        return;
    m_zoom_level -= ZOOM_STEP;
    update_zoom();
}

void ViewImplementation::reset_zoom()
{
    m_zoom_level = 1.0f;
    update_zoom();
}

void ViewImplementation::set_preferred_color_scheme(Web::CSS::PreferredColorScheme color_scheme)
{
    client().async_set_preferred_color_scheme(color_scheme);
}

ByteString ViewImplementation::selected_text()
{
    return client().get_selected_text();
}

Optional<String> ViewImplementation::selected_text_with_whitespace_collapsed()
{
    auto selected_text = MUST(Web::Infra::strip_and_collapse_whitespace(this->selected_text()));
    if (selected_text.is_empty())
        return OptionalNone {};
    return selected_text;
}

void ViewImplementation::select_all()
{
    client().async_select_all();
}

void ViewImplementation::get_source()
{
    client().async_get_source();
}

void ViewImplementation::inspect_dom_tree()
{
    client().async_inspect_dom_tree();
}

void ViewImplementation::inspect_dom_node(i32 node_id, Optional<Web::CSS::Selector::PseudoElement::Type> pseudo_element)
{
    client().async_inspect_dom_node(node_id, move(pseudo_element));
}

void ViewImplementation::inspect_accessibility_tree()
{
    client().async_inspect_accessibility_tree();
}

void ViewImplementation::clear_inspected_dom_node()
{
    inspect_dom_node(0, {});
}

void ViewImplementation::get_hovered_node_id()
{
    client().async_get_hovered_node_id();
}

void ViewImplementation::set_dom_node_text(i32 node_id, String text)
{
    client().async_set_dom_node_text(node_id, move(text));
}

void ViewImplementation::set_dom_node_tag(i32 node_id, String name)
{
    client().async_set_dom_node_tag(node_id, move(name));
}

void ViewImplementation::add_dom_node_attributes(i32 node_id, Vector<Attribute> attributes)
{
    client().async_add_dom_node_attributes(node_id, move(attributes));
}

void ViewImplementation::replace_dom_node_attribute(i32 node_id, String name, Vector<Attribute> replacement_attributes)
{
    client().async_replace_dom_node_attribute(node_id, move(name), move(replacement_attributes));
}

void ViewImplementation::create_child_element(i32 node_id)
{
    client().async_create_child_element(node_id);
}

void ViewImplementation::create_child_text_node(i32 node_id)
{
    client().async_create_child_text_node(node_id);
}

void ViewImplementation::clone_dom_node(i32 node_id)
{
    client().async_clone_dom_node(node_id);
}

void ViewImplementation::remove_dom_node(i32 node_id)
{
    client().async_remove_dom_node(node_id);
}

void ViewImplementation::get_dom_node_html(i32 node_id)
{
    client().async_get_dom_node_html(node_id);
}

void ViewImplementation::debug_request(ByteString const& request, ByteString const& argument)
{
    client().async_debug_request(request, argument);
}

void ViewImplementation::run_javascript(StringView js_source)
{
    client().async_run_javascript(js_source);
}

void ViewImplementation::js_console_input(ByteString const& js_source)
{
    client().async_js_console_input(js_source);
}

void ViewImplementation::js_console_request_messages(i32 start_index)
{
    client().async_js_console_request_messages(start_index);
}

void ViewImplementation::alert_closed()
{
    client().async_alert_closed();
}

void ViewImplementation::confirm_closed(bool accepted)
{
    client().async_confirm_closed(accepted);
}

void ViewImplementation::prompt_closed(Optional<String> response)
{
    client().async_prompt_closed(move(response));
}

void ViewImplementation::color_picker_closed(Optional<Color> picked_color)
{
    client().async_color_picker_closed(picked_color);
}

void ViewImplementation::select_dropdown_closed(Optional<String> value)
{
    client().async_select_dropdown_closed(value);
}

void ViewImplementation::toggle_media_play_state()
{
    client().async_toggle_media_play_state();
}

void ViewImplementation::toggle_media_mute_state()
{
    client().async_toggle_media_mute_state();
}

void ViewImplementation::toggle_media_loop_state()
{
    client().async_toggle_media_loop_state();
}

void ViewImplementation::toggle_media_controls_state()
{
    client().async_toggle_media_controls_state();
}

void ViewImplementation::handle_resize()
{
    resize_backing_stores_if_needed(WindowResizeInProgress::Yes);
    m_backing_store_shrink_timer->restart();
}

void ViewImplementation::resize_backing_stores_if_needed(WindowResizeInProgress window_resize_in_progress)
{
    if (m_client_state.has_usable_bitmap) {
        // NOTE: We keep the outgoing front bitmap as a backup so we have something to paint until we get a new one.
        m_backup_bitmap = m_client_state.front_bitmap.bitmap;
        m_backup_bitmap_size = m_client_state.front_bitmap.last_painted_size;
    }

    m_client_state.has_usable_bitmap = false;

    auto viewport_rect = this->viewport_rect();
    if (viewport_rect.is_empty())
        return;

    Web::DevicePixelSize minimum_needed_size;

    if (window_resize_in_progress == WindowResizeInProgress::Yes) {
        // Pad the minimum needed size so that we don't have to keep reallocating backing stores while the window is being resized.
        minimum_needed_size = { viewport_rect.width() + 256, viewport_rect.height() + 256 };
    } else {
        // If we're not in the middle of a resize, we can shrink the backing store size to match the viewport size.
        minimum_needed_size = viewport_rect.size();
        m_client_state.front_bitmap = {};
        m_client_state.back_bitmap = {};
    }

    auto old_front_bitmap_id = m_client_state.front_bitmap.id;
    auto old_back_bitmap_id = m_client_state.back_bitmap.id;

    auto reallocate_backing_store_if_needed = [&](SharedBitmap& backing_store) {
        if (!backing_store.bitmap || !backing_store.bitmap->size().contains(minimum_needed_size.to_type<int>())) {
            if (auto new_bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, minimum_needed_size.to_type<int>()); !new_bitmap_or_error.is_error()) {
                backing_store.bitmap = new_bitmap_or_error.release_value();
                backing_store.id = m_client_state.next_bitmap_id++;
            }
            backing_store.last_painted_size = viewport_rect.size();
        }
    };

    reallocate_backing_store_if_needed(m_client_state.front_bitmap);
    reallocate_backing_store_if_needed(m_client_state.back_bitmap);

    auto& front_bitmap = m_client_state.front_bitmap;
    auto& back_bitmap = m_client_state.back_bitmap;

    if (front_bitmap.id != old_front_bitmap_id || back_bitmap.id != old_back_bitmap_id) {
        client().async_add_backing_store(front_bitmap.id, front_bitmap.bitmap->to_shareable_bitmap(), back_bitmap.id,
            back_bitmap.bitmap->to_shareable_bitmap());
        client().async_set_viewport_rect(viewport_rect);
    }
}

void ViewImplementation::handle_web_content_process_crash()
{
    dbgln("WebContent process crashed!");

    ++m_crash_count;
    constexpr size_t max_reasonable_crash_count = 5U;
    if (m_crash_count >= max_reasonable_crash_count) {
        dbgln("WebContent has crashed {} times in quick succession! Not restarting...", m_crash_count);
        m_repeated_crash_timer->stop();
        return;
    }
    m_repeated_crash_timer->restart();

    create_client();
    VERIFY(m_client_state.client);

    // Don't keep a stale backup bitmap around.
    m_backup_bitmap = nullptr;

    handle_resize();
    StringBuilder builder;
    builder.append("<html><head><title>Crashed: "sv);
    builder.append(escape_html_entities(m_url.to_byte_string()));
    builder.append("</title></head><body>"sv);
    builder.append("<h1>Web page crashed"sv);
    if (!m_url.host().has<Empty>()) {
        builder.appendff(" on {}", escape_html_entities(m_url.serialized_host().release_value_but_fixme_should_propagate_errors()));
    }
    builder.append("</h1>"sv);
    auto escaped_url = escape_html_entities(m_url.to_byte_string());
    builder.appendff("The web page <a href=\"{}\">{}</a> has crashed.<br><br>You can reload the page to try again.", escaped_url, escaped_url);
    builder.append("</body></html>"sv);
    load_html(builder.to_byte_string());
}

static ErrorOr<LexicalPath> save_screenshot(Gfx::ShareableBitmap const& bitmap)
{
    if (!bitmap.is_valid())
        return Error::from_string_view("Failed to take a screenshot"sv);

    LexicalPath path { Core::StandardPaths::downloads_directory() };
    path = path.append(TRY(Core::DateTime::now().to_string("screenshot-%Y-%m-%d-%H-%M-%S.png"sv)));

    auto encoded = TRY(Gfx::PNGWriter::encode(*bitmap.bitmap()));

    auto dump_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(dump_file->write_until_depleted(encoded));

    return path;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_screenshot(ScreenshotType type)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicitly, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    Gfx::ShareableBitmap bitmap;

    switch (type) {
    case ScreenshotType::Visible:
        if (auto* visible_bitmap = m_client_state.has_usable_bitmap ? m_client_state.front_bitmap.bitmap.ptr() : m_backup_bitmap.ptr()) {
            if (auto result = save_screenshot(visible_bitmap->to_shareable_bitmap()); result.is_error())
                promise->reject(result.release_error());
            else
                promise->resolve(result.release_value());
        }
        break;

    case ScreenshotType::Full:
        m_pending_screenshot = promise;
        client().async_take_document_screenshot();
        break;
    }

    return promise;
}

NonnullRefPtr<Core::Promise<LexicalPath>> ViewImplementation::take_dom_node_screenshot(i32 node_id)
{
    auto promise = Core::Promise<LexicalPath>::construct();

    if (m_pending_screenshot) {
        // For simplicitly, only allow taking one screenshot at a time for now. Revisit if we need
        // to allow spamming screenshot requests for some reason.
        promise->reject(Error::from_string_literal("A screenshot request is already in progress"));
        return promise;
    }

    m_pending_screenshot = promise;
    client().async_take_dom_node_screenshot(node_id);

    return promise;
}

void ViewImplementation::did_receive_screenshot(Badge<WebContentClient>, Gfx::ShareableBitmap const& screenshot)
{
    VERIFY(m_pending_screenshot);

    if (auto result = save_screenshot(screenshot); result.is_error())
        m_pending_screenshot->reject(result.release_error());
    else
        m_pending_screenshot->resolve(result.release_value());

    m_pending_screenshot = nullptr;
}

ErrorOr<LexicalPath> ViewImplementation::dump_gc_graph()
{
    auto gc_graph_json = client().dump_gc_graph();

    LexicalPath path { Core::StandardPaths::tempfile_directory() };
    path = path.append(TRY(Core::DateTime::now().to_string("gc-graph-%Y-%m-%d-%H-%M-%S.json"sv)));

    auto screenshot_file = TRY(Core::File::open(path.string(), Core::File::OpenMode::Write));
    TRY(screenshot_file->write_until_depleted(gc_graph_json.bytes()));

    return path;
}

void ViewImplementation::set_user_style_sheet(String source)
{
    client().async_set_user_style(move(source));
}

void ViewImplementation::use_native_user_style_sheet()
{
    extern StringView native_stylesheet_source;
    set_user_style_sheet(MUST(String::from_utf8(native_stylesheet_source)));
}

void ViewImplementation::enable_inspector_prototype()
{
    client().async_enable_inspector_prototype();
}

}
