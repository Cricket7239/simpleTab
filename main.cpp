#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>

#include <gtkmm.h>
#include <gdkmm/pixbuf.h>

#include <iostream>
#include <vector>
#include <thread>
#include <unistd.h>

/* ============================================================
 *  XCB / EWMH HELPERS
 * ============================================================ */

struct WindowInfo {
    xcb_window_t id;
    std::string title;
};

std::string get_window_title(xcb_ewmh_connection_t& ewmh, xcb_window_t win) {
    xcb_ewmh_get_utf8_strings_reply_t reply;
    if (xcb_ewmh_get_wm_name_reply(
            &ewmh,
            xcb_ewmh_get_wm_name(&ewmh, win),
            &reply,
            nullptr))
    {
        std::string title(reply.strings, reply.strings_len);
        xcb_ewmh_get_utf8_strings_reply_wipe(&reply);
        return title;
    }
    return {};
}

xcb_window_t get_active_window(xcb_ewmh_connection_t& ewmh) {
    xcb_window_t win = XCB_NONE;
    xcb_ewmh_get_active_window_reply(
        &ewmh,
        xcb_ewmh_get_active_window(&ewmh, 0),
        &win,
        nullptr);
    return win;
}

std::vector<WindowInfo> get_window_list(
    xcb_connection_t* conn,
    xcb_ewmh_connection_t& ewmh)
{
    std::vector<WindowInfo> out;

    xcb_ewmh_get_windows_reply_t clients;
    if (!xcb_ewmh_get_client_list_reply(
            &ewmh,
            xcb_ewmh_get_client_list(&ewmh, 0),
            &clients,
            nullptr))
        return out;

    for (int i = 0; i < clients.windows_len; ++i) {
        auto win = clients.windows[i];
        auto title = get_window_title(ewmh, win);
        if (!title.empty())
            out.push_back({win, title});
    }

    xcb_ewmh_get_windows_reply_wipe(&clients);
    return out;
}

void focus_window(
    xcb_connection_t* conn,
    xcb_ewmh_connection_t& ewmh,
    xcb_window_t win)
{
    if (!win) return;

    xcb_map_window(conn, win);
    xcb_flush(conn);

    xcb_ewmh_request_change_active_window(
        &ewmh,
        0,
        win,
        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
        XCB_CURRENT_TIME,
        XCB_NONE);

    xcb_flush(conn);
}

/* ============================================================
 *  ICON EXTRACTION
 * ============================================================ */

Glib::RefPtr<Gdk::Pixbuf> get_window_icon(
    xcb_connection_t* conn,
    xcb_ewmh_connection_t& ewmh,
    xcb_window_t win,
    int size = 32)
{
    auto cookie = xcb_ewmh_get_wm_icon(&ewmh, win);
    auto* prop = xcb_get_property_reply(conn, cookie, nullptr);
    if (!prop) return {};

    auto len = xcb_get_property_value_length(prop) / 4;
    if (len < 2) {
        free(prop);
        return {};
    }

    auto* data = static_cast<uint32_t*>(xcb_get_property_value(prop));
    uint32_t w = data[0];
    uint32_t h = data[1];
    if (len < 2 + w * h) {
        free(prop);
        return {};
    }

    std::vector<uint8_t> rgba(w * h * 4);
    auto* pixels = &data[2];

    for (uint32_t i = 0; i < w * h; ++i) {
        uint32_t p = pixels[i];
        rgba[i*4+0] = (p >> 16) & 0xff;
        rgba[i*4+1] = (p >>  8) & 0xff;
        rgba[i*4+2] =  p        & 0xff;
        rgba[i*4+3] = (p >> 24) & 0xff;
    }

    free(prop);

    auto pixbuf = Gdk::Pixbuf::create_from_data(
        rgba.data(),
        Gdk::COLORSPACE_RGB,
        true,
        8,
        w, h,
        w * 4);

    if (w != size || h != size)
        pixbuf = pixbuf->scale_simple(size, size, Gdk::INTERP_BILINEAR);

    return pixbuf;
}

/* ============================================================
 *  GTK OVERLAY
 * ============================================================ */

Gtk::Window* overlay_window = nullptr;
std::vector<Gtk::Button*> overlay_buttons;

Gtk::Box* build_overlay(
    Gtk::Window* parent,
    const std::vector<WindowInfo>& windows,
    xcb_connection_t* conn,
    xcb_ewmh_connection_t& ewmh)
{
    auto* box = Gtk::make_managed<Gtk::Box>(
        Gtk::ORIENTATION_VERTICAL, 6);

    auto active = get_active_window(ewmh);

    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& w = windows[i];
        if (w.title == "alt_tab" || w.title[0] == '@') continue;

        auto* btn = Gtk::make_managed<Gtk::Button>();
        auto* hbox = Gtk::make_managed<Gtk::Box>(
            Gtk::ORIENTATION_HORIZONTAL, 6);

        auto icon = get_window_icon(conn, ewmh, w.id);
        if (icon)
            hbox->pack_start(
                *Gtk::make_managed<Gtk::Image>(icon),
                Gtk::PACK_SHRINK);

        std::string label =
            (w.id == active ? "*[" : "[") +
            std::to_string(i) + "] " + w.title;

        hbox->pack_start(
            *Gtk::make_managed<Gtk::Label>(label),
            Gtk::PACK_EXPAND_WIDGET);

        btn->add(*hbox);
        btn->signal_clicked().connect([=, &ewmh]() {
            focus_window(conn, ewmh, w.id);
            parent->hide();
        });

        overlay_buttons.push_back(btn);
        box->pack_start(*btn, Gtk::PACK_SHRINK);
    }

    return box;
}

void show_overlay(
    xcb_connection_t* conn,
    xcb_ewmh_connection_t& ewmh)
{
    auto windows = get_window_list(conn, ewmh);

    if (!overlay_window) {
        overlay_window = new Gtk::Window();
        overlay_window->set_decorated(false);
        overlay_window->set_keep_above(true);
        overlay_window->set_position(Gtk::WIN_POS_CENTER);
        overlay_window->add_events(Gdk::KEY_PRESS_MASK);
    } else {
        overlay_window->remove();
        overlay_buttons.clear();
    }

    overlay_window->add(
        *build_overlay(overlay_window, windows, conn, ewmh));

    overlay_window->signal_key_press_event().connect(
        [](GdkEventKey* e) {
            if (e->keyval >= GDK_KEY_0 && e->keyval <= GDK_KEY_9) {
                int i = e->keyval - GDK_KEY_0;
                if (i < overlay_buttons.size())
                    overlay_buttons[i]->clicked();
                return true;
            }
            if (e->keyval == GDK_KEY_Escape) {
                overlay_window->hide();
                return true;
            }
            return false;
        },
        false);

    overlay_window->show_all();
}

/* ============================================================
 *  MAIN
 * ============================================================ */

int main(int argc, char* argv[]) {
    xcb_connection_t* conn = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(conn)) {
        std::cerr << "X connection failed\n";
        return 1;
    }

    xcb_ewmh_connection_t ewmh;
    auto* cookies = xcb_ewmh_init_atoms(conn, &ewmh);
    xcb_ewmh_init_atoms_replies(&ewmh, cookies, nullptr);

    auto app = Gtk::Application::create(
        argc, argv, "com.example.alt_tab");

    Gtk::Window dummy;
    dummy.set_opacity(0.0);
    dummy.set_default_size(1,1);
    dummy.show();

    // Alt+B global grab
    auto* screen = xcb_setup_roots_iterator(
        xcb_get_setup(conn)).data;

    auto* syms = xcb_key_symbols_alloc(conn);
    auto* codes = xcb_key_symbols_get_keycode(syms, XK_b);

    for (int i = 0; codes[i]; ++i)
        xcb_grab_key(
            conn, 1, screen->root,
            XCB_MOD_MASK_1, codes[i],
            XCB_GRAB_MODE_ASYNC,
            XCB_GRAB_MODE_ASYNC);

    xcb_flush(conn);

    std::thread([=, &ewmh]() {
        while (auto* ev = xcb_wait_for_event(conn)) {
            if ((ev->response_type & 0x7f) == XCB_KEY_PRESS) {
                Glib::signal_idle().connect_once(
                    [=, &ewmh]() { show_overlay(conn, ewmh); });
            }
            free(ev);
        }
    }).detach();

    return app->run(dummy);
}

