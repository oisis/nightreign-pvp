// Windows window for the launcher (plain Win32). The logic is in core.c.
#include "core.h"
#include "ui.h"
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

enum { ID_FOLDER = 100, ID_BROWSE, ID_PLAY, ID_UNINSTALL, ID_LOG, ID_CLOSE, ID_BACKUP, ID_HELP, ID_LANG, ID_LANG_LABEL, ID_DEBUG };
static const char *const *T = UI_TEXT[L_EN];
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

// Backup of the saves; a line in the window either way.
static void backup(void) {
    char save[1024] = "", where[256], line[600];
    int r = core_find_save_dir(game_dir(), save, sizeof save) ? core_backup_save(save, where, sizeof where) : 0;
    if (r > 0) snprintf(line, sizeof line, T[T_BACKUP_DONE], where), log_line(line);
    else log_line(r < 0 ? T[T_BACKUP_FAILED] : T[T_NO_SAVE]);
}

static void play(void) {
    if (!core_check_game(game_dir())) { log_line(T[T_BAD_FOLDER]); return; }
    EnableWindow(GetDlgItem(g_window, ID_PLAY), FALSE);
    backup();
    if (core_build(game_dir())) core_launch();
    EnableWindow(GetDlgItem(g_window, ID_PLAY), TRUE);
}

static void help(void) { MessageBoxW(g_window, wide(T[T_HELP_TEXT]), wide(T[T_HELP]), MB_OK | MB_ICONINFORMATION); }

static void set_texts(void) {
    SetWindowTextW(g_window, wide(T[T_TITLE]));
    SetWindowTextW(GetDlgItem(g_window, 1), wide(T[T_GAME_FOLDER]));
    SetWindowTextW(GetDlgItem(g_window, ID_LANG_LABEL), wide(T[T_LANGUAGE]));
    SetWindowTextW(GetDlgItem(g_window, ID_BROWSE), wide(T[T_BROWSE]));
    SetWindowTextW(GetDlgItem(g_window, ID_PLAY), wide(T[T_PLAY]));
    SetWindowTextW(GetDlgItem(g_window, ID_BACKUP), wide(T[T_BACKUP]));
    SetWindowTextW(GetDlgItem(g_window, ID_HELP), wide(T[T_HELP]));
    SetWindowTextW(GetDlgItem(g_window, ID_UNINSTALL), wide(T[T_UNINSTALL]));
    SetWindowTextW(GetDlgItem(g_window, ID_CLOSE), wide(T[T_CLOSE]));
    SetWindowTextW(GetDlgItem(g_window, ID_DEBUG), wide(T[T_DEBUG_LOG]));
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
        MoveWindow(GetDlgItem(w, 1), m, m, cx - 3 * m - 230, 18, TRUE);
        MoveWindow(GetDlgItem(w, ID_LANG_LABEL), cx - m - 230, m, 90, 18, TRUE);
        MoveWindow(GetDlgItem(w, ID_LANG), cx - m - 130, m - 3, 130, 200, TRUE);
        MoveWindow(g_folder, m, 34, cx - 3 * m - 90, 24, TRUE);
        MoveWindow(GetDlgItem(w, ID_BROWSE), cx - m - 90, 33, 90, 26, TRUE);
        MoveWindow(g_log, m, 68, cx - 2 * m, cy - 68 - 80, TRUE);
        MoveWindow(GetDlgItem(w, ID_DEBUG), m, cy - 70, cx - 2 * m, 22, TRUE);
        MoveWindow(GetDlgItem(w, ID_PLAY), m, cy - 40, cx - 6 * m - 4 * 110, 30, TRUE);
        MoveWindow(GetDlgItem(w, ID_BACKUP), cx - 4 * m - 4 * 110, cy - 40, 110, 30, TRUE);
        MoveWindow(GetDlgItem(w, ID_UNINSTALL), cx - 3 * m - 3 * 110, cy - 40, 110, 30, TRUE);
        MoveWindow(GetDlgItem(w, ID_HELP), cx - 2 * m - 2 * 110, cy - 40, 110, 30, TRUE);
        MoveWindow(GetDlgItem(w, ID_CLOSE), cx - m - 110, cy - 40, 110, 30, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_BROWSE) browse();
        if (LOWORD(wp) == ID_PLAY) play();
        if (LOWORD(wp) == ID_UNINSTALL) uninstall();
        if (LOWORD(wp) == ID_BACKUP) backup();
        if (LOWORD(wp) == ID_HELP) help();
        if (LOWORD(wp) == ID_DEBUG && HIWORD(wp) == BN_CLICKED)
            core_save_debug(SendMessageW(GetDlgItem(w, ID_DEBUG), BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (LOWORD(wp) == ID_LANG && HIWORD(wp) == CBN_SELCHANGE) {
            int l = (int)SendMessageW(GetDlgItem(w, ID_LANG), CB_GETCURSEL, 0, 0);
            if (l >= 0 && l < LANG_COUNT) T = UI_TEXT[l], core_save_lang(LANG_CODE[l]), set_texts();
        }
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
    CoInitialize(NULL);

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    core_set_app_dir(exe);
    core_set_log(log_line);
    // the saved choice, else the system language, else English
    char saved[8];
    core_load_lang(saved, sizeof saved);
    int lang_index = L_EN;
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_POLISH: lang_index = L_PL; break;
    case LANG_GERMAN: lang_index = L_DE; break;
    case LANG_FRENCH: lang_index = L_FR; break;
    case LANG_SPANISH: lang_index = L_ES; break;
    }
    if (*saved) lang_index = ui_lang_from_code(saved);
    T = UI_TEXT[lang_index];

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
    child(L"BUTTON", T[T_BACKUP], BS_PUSHBUTTON, ID_BACKUP);
    child(L"BUTTON", T[T_HELP], BS_PUSHBUTTON, ID_HELP);
    child(L"STATIC", T[T_LANGUAGE], SS_RIGHT, ID_LANG_LABEL);
    HWND debug_box = child(L"BUTTON", T[T_DEBUG_LOG], BS_AUTOCHECKBOX, ID_DEBUG);
    SendMessageW(debug_box, BM_SETCHECK, core_load_debug() ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND lang_box = child(L"COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, ID_LANG);
    for (int i = 0; i < LANG_COUNT; i++) SendMessageW(lang_box, CB_ADDSTRING, 0, (LPARAM)wide(LANG_NAME[i]));
    SendMessageW(lang_box, CB_SETCURSEL, lang_index, 0);
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
