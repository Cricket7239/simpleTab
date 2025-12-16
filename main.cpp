#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <iostream>
#include <string>
#include <unistd.h> 


std::string
get_window_title_by_window_id(xcb_ewmh_connection_t& ewmh, xcb_window_t win) {
	xcb_ewmh_get_utf8_strings_reply_t title;
	if (xcb_ewmh_get_wm_name_reply(&ewmh, xcb_ewmh_get_wm_name(&ewmh, win), &title, nullptr)) {
		std::string windowTitle(title.strings, title.strings_len);
		xcb_ewmh_get_utf8_strings_reply_wipe(&title); // free memory
		return windowTitle;
	}
	return "<no title>";
}

xcb_window_t 
get_active_window(xcb_connection_t* conn, xcb_ewmh_connection_t& ewmh) {
	xcb_window_t active_window = 0;

	// Get the active window
	if (xcb_ewmh_get_active_window_reply(&ewmh, 
				xcb_ewmh_get_active_window(&ewmh, 0), &active_window, nullptr))
	{
		return active_window;
	}

	return 0; // No active window found
}


void focus_window(xcb_connection_t* conn, xcb_ewmh_connection_t& ewmh, xcb_window_t win) {
    if (win == 0) return;

    // 1. Ensure the window is mapped (visible)
    xcb_map_window(conn, win);
    xcb_flush(conn);   // send the map request
    usleep(10 * 1000); // wait 10ms to ensure it's mapped

    // 2. Check which desktop the window is on
    xcb_get_property_cookie_t desktop_cookie = xcb_ewmh_get_wm_desktop(&ewmh, win);
    uint32_t desktop;
    if (xcb_ewmh_get_wm_desktop_reply(&ewmh, desktop_cookie, &desktop, nullptr)) {
        // 3. Switch desktop if the window is not on the current desktop
        int screen = 0; // default screen
        xcb_get_property_cookie_t current_cookie = xcb_ewmh_get_current_desktop(&ewmh, screen);
        uint32_t current_desktop;
        if (xcb_ewmh_get_current_desktop_reply(&ewmh, current_cookie, &current_desktop, nullptr)) {
            if (current_desktop != desktop) {
                // Request the window manager to switch desktops
                xcb_ewmh_request_change_wm_desktop(
                    &ewmh,
                    screen,        // screen (0 for default)
                    XCB_NONE,      // no specific window
                    desktop,       // the target desktop
                    XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER
                );
                xcb_flush(conn); // send the desktop switch request
                usleep(50 * 1000); // wait 50ms for WM to process the desktop change
            }
        }
    }

    // 4. Request the window manager to focus/raise the window
    xcb_window_t active_win = 0;
    int screen = 0; // default screen
    xcb_get_property_cookie_t active_cookie = xcb_ewmh_get_active_window(&ewmh, screen);
    xcb_ewmh_get_active_window_reply(&ewmh, active_cookie, &active_win, nullptr);

    // Request the WM to focus the window (does not guarantee focus due to WM policies)
    xcb_ewmh_request_change_active_window(
        &ewmh,
        screen,         // screen
        win,            // the window to focus
        XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
        XCB_CURRENT_TIME, // timestamp for the action
        active_win       // previous active window (for context)
    );
    xcb_flush(conn); // send the focus request

    // Optionally, retry a few times to ensure focus is set
    for (int i = 0; i < 3; ++i) {
        xcb_ewmh_request_change_active_window(
            &ewmh,
            screen,
            win,
            XCB_EWMH_CLIENT_SOURCE_TYPE_OTHER,
            XCB_CURRENT_TIME,
            active_win
        );
        xcb_flush(conn);
        usleep(50 * 1000); // wait 50ms between retries
    }
}


int main() {
	// Connect to X server
	xcb_connection_t* conn = xcb_connect(nullptr, nullptr);
	if (xcb_connection_has_error(conn)) {
		std::cerr << "Cannot connect to X server\n";
		return 1;
	}

	// Initialize EWMH connection
	xcb_ewmh_connection_t ewmh;
	xcb_intern_atom_cookie_t* cookies = xcb_ewmh_init_atoms(conn, &ewmh);
	if (!xcb_ewmh_init_atoms_replies(&ewmh, cookies, nullptr)) {
		std::cerr << "Failed to initialize EWMH\n";
		return 1;
	}

	// Get the client list
	xcb_ewmh_get_windows_reply_t clients;
	xcb_ewmh_get_client_list_reply(
			&ewmh,
			xcb_ewmh_get_client_list(&ewmh, 0),
			&clients,
			nullptr);
	
	std::cout << "Top-level windows (clients):\n";
	for (uint32_t i = 0; i < clients.windows_len; ++i) {
		bool is_aktive = get_active_window(conn, ewmh) == clients.windows[i];
		std::cout << i << " |Window title: " << get_window_title_by_window_id(ewmh, clients.windows[i]) << "\n"
		<< "  is aktive: " << is_aktive << "\n";
	}

	// Free memory
	focus_window(conn, ewmh, clients.windows[2] );
	xcb_ewmh_get_windows_reply_wipe(&clients);

	// Cleanup
	xcb_ewmh_connection_wipe(&ewmh);
	xcb_disconnect(conn);

	return 0;
}

