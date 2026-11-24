#include <cui/cui_app.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>

static LRESULT CALLBACK titlebar_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                      UINT_PTR drag_region, DWORD_PTR reference) {
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, titlebar_proc, drag_region);
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message != WM_NCHITTEST || result != HTCLIENT) return result;

    POINT pointer = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (!ScreenToClient(window, &pointer)) return result;
    cui_app *app = (cui_app *)reference;
    RECT client;
    if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return result;
    cui_vec2 size = cui_app_size(app);
    cui_rect drag = cui_get_rect(cui_app_ui(app), (cui_id)drag_region);
    float x = pointer.x * size.x / client.right, y = pointer.y * size.y / client.bottom;
    if (x >= drag.x && x < drag.x + drag.w && y >= drag.y && y < drag.y + drag.h)
        return HTCAPTION;
    return result;
}

bool ced_windows_titlebar(cui_app *app, cui_id drag_region) {
    HWND window = cui_app_native_window(app);
    if (!window || !SetWindowSubclass(window, titlebar_proc, drag_region, (DWORD_PTR)app)) return false;
    /* Keep the sizing frame and system menu. CED draws the caption in the client area. */
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowLongPtrW(window, GWL_STYLE, style & ~WS_CAPTION) && GetLastError() != ERROR_SUCCESS)
        return false;
    if (!SetWindowPos(window, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED))
        return false;
    ShowWindow(window, SW_SHOW);
    return true;
}

void ced_windows_titlebar_buttons(cui_ctx *ui, cui_app *app) {
    HWND window = cui_app_native_window(app);
    bool maximized = IsZoomed(window);
    if (cui_toolbar_button(ui, CUI_ICON_MINUS, .tooltip = "Minimize"))
        PostMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    if (cui_toolbar_button(ui, maximized ? CUI_ICON_COPY : CUI_ICON_SQUARE,
            .tooltip = maximized ? "Restore" : "Maximize"))
        PostMessageW(window, WM_SYSCOMMAND, maximized ? SC_RESTORE : SC_MAXIMIZE, 0);
    if (cui_toolbar_button(ui, CUI_ICON_X, .tooltip = "Close"))
        PostMessageW(window, WM_CLOSE, 0, 0);
}
