// Windows window for the launcher (plain Win32). The logic is in core.c.
#include "core.h"
#include "ui.h"
#include <windows.h>
#include <shlobj.h>
#include <string.h>

enum { ID_FOLDER = 100, ID_BROWSE, ID_PLAY, ID_UNINSTALL, ID_LOG, ID_CLOSE };
static const char *const *T = UI_EN;
static HWND g_window, g_folder, g_log;

static wchar_t *wide(const char *utf8) {
    static wchar_t buf[4][1024];
    static int slot;
    wchar_t *w = buf[slot++ & 3];
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024);
    return w;
}

static void log_line(const char *line) {
    int end = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, end, end);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)wide(line));
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    MSG msg;  // keep the window alive during long steps
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static const char *game_dir(void) {
    static char dir[1024];
    wchar_t w[1024];
    GetWindowTextW(g_folder, w, 1024);
    WideCharToMultiByte(CP_ACP, 0, w, -1, dir, sizeof dir, NULL, NULL);
    return dir;
}

static void browse(void) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_window;
    bi.lpszTitle = wide(T[T_PICK]);
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&bi);
    wchar_t path[MAX_PATH];
    if (pid && SHGetPathFromIDListW(pid, path)) {
        SetWindowTextW(g_folder, path);
        log_line(core_check_game(game_dir()) ? T[T_FOUND] : T[T_BAD_FOLDER]);
    }
    if (pid) CoTaskMemFree(pid);
}

static void play(void) {
    char save[1024];
    if (!core_check_game(game_dir())) { log_line(T[T_BAD_FOLDER]); return; }
    EnableWindow(GetDlgItem(g_window, ID_PLAY), FALSE);
    if (core_find_save_dir(game_dir(), save, sizeof save)) core_backup_save(save);
    else log_line(T[T_NO_SAVE]);
    if (core_build(game_dir())) core_launch();
    EnableWindow(GetDlgItem(g_window, ID_PLAY), TRUE);
}

static void uninstall(void) {
    if (MessageBoxW(g_window, wide(T[T_CONFIRM_UNINSTALL]), wide(T[T_TITLE]), MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    char save[1024] = "";
    core_find_save_dir(game_dir(), save, sizeof save);
    core_uninstall(save);
}

static HWND child(const wchar_t *cls, const char *text, DWORD style, int id) {
    HWND h = CreateWindowExW(wcscmp(cls, L"EDIT") ? 0 : WS_EX_CLIENTEDGE, cls, wide(text), WS_CHILD | WS_VISIBLE | style,
                             0, 0, 0, 0, g_window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    return h;
}

static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {  // label, folder row, log, button row
        int cx = LOWORD(lp), cy = HIWORD(lp), m = 10;
        MoveWindow(GetDlgItem(w, 1), m, m, cx - 2 * m, 18, TRUE);
        MoveWindow(g_folder, m, 34, cx - 3 * m - 90, 24, TRUE);
        MoveWindow(GetDlgItem(w, ID_BROWSE), cx - m - 90, 33, 90, 26, TRUE);
        MoveWindow(g_log, m, 68, cx - 2 * m, cy - 68 - 50, TRUE);
        MoveWindow(GetDlgItem(w, ID_PLAY), m, cy - 40, cx - 4 * m - 220, 30, TRUE);
        MoveWindow(GetDlgItem(w, ID_UNINSTALL), cx - 2 * m - 220, cy - 40, 110, 30, TRUE);
        MoveWindow(GetDlgItem(w, ID_CLOSE), cx - m - 110, cy - 40, 110, 30, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BROWSE) browse();
        if (LOWORD(wp) == ID_PLAY) play();
        if (LOWORD(wp) == ID_UNINSTALL) uninstall();
        if (LOWORD(wp) == ID_CLOSE) DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_POLISH) T = UI_PL;
    CoInitialize(NULL);

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    core_set_app_dir(exe);
    core_set_log(log_line);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NightreignInvasions";
    RegisterClassW(&wc);
    g_window = CreateWindowW(wc.lpszClassName, wide(T[T_TITLE]), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             660, 440, NULL, NULL, inst, NULL);
    child(L"STATIC", T[T_GAME_FOLDER], 0, 1);
    g_folder = child(L"EDIT", "", ES_AUTOHSCROLL, ID_FOLDER);
    child(L"BUTTON", T[T_BROWSE], BS_PUSHBUTTON, ID_BROWSE);
    g_log = child(L"EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_LOG);
    child(L"BUTTON", T[T_PLAY], BS_DEFPUSHBUTTON, ID_PLAY);
    child(L"BUTTON", T[T_UNINSTALL], BS_PUSHBUTTON, ID_UNINSTALL);
    child(L"BUTTON", T[T_CLOSE], BS_PUSHBUTTON, ID_CLOSE);
    ShowWindow(g_window, show);
    RECT rc;
    GetClientRect(g_window, &rc);
    SendMessageW(g_window, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));

    char game[1024];
    if (core_find_game(game, sizeof game)) {
        SetWindowTextW(g_folder, wide(game));
        log_line(T[T_FOUND]);
    } else {
        log_line(T[T_NOT_FOUND]);
    }
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
