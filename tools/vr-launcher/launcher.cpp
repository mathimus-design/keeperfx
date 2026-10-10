// KeeperFX VR launcher: a small window next to the "vr" folder of a KeeperFX
// install. It checks the install, finds the VR runtimes (Virtual Desktop,
// SteamVR, Meta Quest Link), and starts vr\keeperfx_vr.exe with the chosen one.
//
// Build (MinGW 32-bit; static, so it needs no other files next to it):
//   windres launcher.rc -O coff -o launcher_res.o
//   g++ -std=c++17 -O2 -s -static -municode -mwindows launcher.cpp launcher_res.o
//       -lcomctl32 -lshell32 -ladvapi32 -o "KeeperFX VR.exe"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>

namespace {

const wchar_t* const k_title = L"KeeperFX VR";
const wchar_t* const k_issues_url = L"https://github.com/mathimus-design/keeperfx/issues/new";

enum RuntimeKind { RK_Auto, RK_VirtualDesktop, RK_SteamVR, RK_Meta, RK_Other };

struct Runtime {
    RuntimeKind kind;
    std::wstring name;  // shown in the list
    std::wstring json;  // manifest path; empty for Auto
};

enum ControlId {
    ID_RUNTIME = 100,
    ID_SKIP_INTRO,
    ID_RECORD,
    ID_PLAY,
    ID_LOG,
    ID_README,
    ID_REPORT,
};

struct Check {
    bool ok;
    std::wstring label;
    std::wstring detail;
};

std::wstring g_root;   // the KeeperFX folder (where this launcher is)
std::wstring g_ini;    // vr\launcher.ini
std::wstring g_version;
std::vector<Runtime> g_runtimes;
std::vector<Check> g_checks;
bool g_can_play = false;
HFONT g_font = nullptr, g_bold = nullptr, g_big = nullptr;
HWND g_runtime_box = nullptr, g_skip_intro = nullptr, g_record = nullptr, g_status = nullptr;
int g_dpi = 96;

int S(int v) { return MulDiv(v, g_dpi, 96); }

bool file_exists(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring lower(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring read_text_file(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return L"";
    std::string bytes;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
        bytes.append(buf, got);
    CloseHandle(f);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), &w[0], n);
    return w;
}

bool write_text_file(const std::wstring& path, const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &bytes[0], n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size();
    CloseHandle(f);
    return ok;
}

/** A registry string. The launcher is 32-bit like the game, so it reads the 32-bit\n *  (WOW6432Node) OpenXR runtime list, as the game does. */
std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}

std::vector<std::wstring> reg_value_names(HKEY root, const wchar_t* key)
{
    std::vector<std::wstring> names;
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ | KEY_WOW64_32KEY, &k) != ERROR_SUCCESS)
        return names;
    for (DWORD i = 0;; i++)
    {
        wchar_t name[1024];
        DWORD len = 1024;
        if (RegEnumValueW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        names.push_back(name);
    }
    RegCloseKey(k);
    return names;
}

RuntimeKind classify(const std::wstring& json)
{
    const std::wstring l = lower(json);
    if (l.find(L"virtualdesktop") != std::wstring::npos) return RK_VirtualDesktop;
    if (l.find(L"steamxr") != std::wstring::npos) return RK_SteamVR;
    if (l.find(L"oculus") != std::wstring::npos) return RK_Meta;
    return RK_Other;
}

std::wstring kind_name(RuntimeKind k)
{
    switch (k)
    {
    case RK_VirtualDesktop: return L"Virtual Desktop";
    case RK_SteamVR:        return L"SteamVR";
    case RK_Meta:           return L"Meta Quest Link / Air Link";
    default:                return L"Other OpenXR runtime";
    }
}

/** SteamVR's 32-bit runtime, found from SteamVR's own record of where it is. */
std::wstring find_steamvr_json()
{
    wchar_t local[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH))
        return L"";
    const std::wstring text = read_text_file(std::wstring(local) + L"\\openvr\\openvrpaths.vrpath");
    // { "runtime" : [ "C:\\...\\SteamVR" ], ... } - no JSON library needed for this.
    size_t at = text.find(L"\"runtime\"");
    if (at == std::wstring::npos)
        return L"";
    const size_t end = text.find(L']', at);
    size_t p = text.find(L'[', at);
    while (p != std::wstring::npos && p < end)
    {
        const size_t q0 = text.find(L'"', p + 1);
        if (q0 == std::wstring::npos || q0 > end) break;
        const size_t q1 = text.find(L'"', q0 + 1);
        if (q1 == std::wstring::npos) break;
        std::wstring dir;
        for (size_t i = q0 + 1; i < q1; i++)
        {
            if (text[i] == L'\\' && i + 1 < q1 && text[i + 1] == L'\\') { dir += L'\\'; i++; }
            else dir += text[i];
        }
        const std::wstring json = dir + L"\\steamxr_win32.json";
        if (file_exists(json))
            return json;
        p = q1 + 1;
    }
    return L"";
}

void find_runtimes()
{
    g_runtimes.clear();
    const wchar_t* const key = L"SOFTWARE\\Khronos\\OpenXR\\1";
    const std::wstring active = reg_string(HKEY_LOCAL_MACHINE, key, L"ActiveRuntime");
    std::vector<Runtime> found;
    auto add = [&](const std::wstring& json) {
        if (json.empty() || !file_exists(json))
            return;
        for (const Runtime& r : found)
            if (lower(r.json) == lower(json))
                return;
        const RuntimeKind k = classify(json);
        found.push_back({k, kind_name(k), json});
    };
    add(active);
    for (const std::wstring& j : reg_value_names(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes"))
        add(j);
    // Common places, in case a runtime is installed but not registered for 32-bit.
    wchar_t pf[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramW6432", pf, MAX_PATH))
    {
        add(std::wstring(pf) + L"\\Virtual Desktop Streamer\\OpenXR\\virtualdesktop-openxr-32.json");
        add(std::wstring(pf) + L"\\Meta Horizon\\Support\\oculus-runtime\\oculus_openxr_32.json");
        add(std::wstring(pf) + L"\\Oculus\\Support\\oculus-runtime\\oculus_openxr_32.json");
    }
    add(find_steamvr_json());

    // "Automatic": Windows' active runtime if it has a 32-bit side, else the first found.
    Runtime autosel = {RK_Auto, L"", L""};
    if (!active.empty() && file_exists(active))
        autosel.name = L"Automatic (" + kind_name(classify(active)) + L", the active one)";
    else if (!found.empty())
        autosel.name = L"Automatic (" + found[0].name + L")";
    else
        autosel.name = L"Automatic (none found)";
    g_runtimes.push_back(autosel);
    for (const Runtime& r : found)
        g_runtimes.push_back(r);
}

void run_checks()
{
    g_checks.clear();
    const bool vr_exe = file_exists(g_root + L"\\vr\\keeperfx_vr.exe");
    const bool kfx = file_exists(g_root + L"\\keeperfx.cfg") && file_exists(g_root + L"\\keeperfx.exe");
    g_checks.push_back({kfx && vr_exe, L"KeeperFX folder",
        !kfx ? L"This launcher must be in your KeeperFX folder: unzip the whole package there."
             : !vr_exe ? L"The vr folder is missing: unzip the whole package into the KeeperFX folder."
                       : L"Found, with the VR build in the vr folder."});
    const bool dk = file_exists(g_root + L"\\data\\creature.jty");
    g_checks.push_back({dk, L"Dungeon Keeper game files",
        dk ? L"Found." : L"Not found: install KeeperFX with your own copy of Dungeon Keeper first."});
    const bool alpha = file_exists(g_root + L"\\fxdata\\translation.toml");
    g_checks.push_back({alpha, L"KeeperFX alpha patch",
        alpha ? L"Found." : L"Too old: apply the latest alpha patch (keeperfx.net > Downloads > Alpha), or menu text will be missing."});
    const bool any_rt = g_runtimes.size() > 1;
    g_checks.push_back({any_rt, L"VR runtime (32-bit OpenXR)",
        any_rt ? L"Found: pick it below, or leave it on Automatic."
               : L"None found: install Virtual Desktop (Streamer), SteamVR or the Meta Quest Link app."});
    g_can_play = kfx && vr_exe;
}

int ini_int(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"launcher", key, def, g_ini.c_str()); }
void ini_set(const wchar_t* key, const std::wstring& v) { WritePrivateProfileStringW(L"launcher", key, v.c_str(), g_ini.c_str()); }
std::wstring ini_str(const wchar_t* key)
{
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"launcher", key, L"", buf, 1024, g_ini.c_str());
    return buf;
}

void set_status(const std::wstring& text) { SetWindowTextW(g_status, text.c_str()); }

/** For recording: a copy of keeperfx.cfg with a 1920x1080 window, passed with -config. */
std::wstring make_record_config()
{
    std::wstring text = read_text_file(g_root + L"\\keeperfx.cfg");
    if (text.empty())
        return L"";
    std::wstring out;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        const bool cr = !line.empty() && line.back() == L'\r';
        if (cr) line.pop_back();
        if (line.rfind(L"FRONTEND_RES=", 0) == 0)
            line = L"FRONTEND_RES=640x480w32 1920x1080w32 1920x1080w32";
        else if (line.rfind(L"INGAME_RES=", 0) == 0)
            line = L"INGAME_RES=1920x1080w32 DESKTOP DESKTOP_FULL";
        out += line;
        if (cr) out += L'\r';
        if (eol < text.size()) out += L'\n';
        pos = eol + 1;
    }
    const std::wstring path = g_root + L"\\vr\\keeperfx_vr_recording.cfg";
    return write_text_file(path, out) ? path : L"";
}

void save_settings()
{
    const int sel = (int)SendMessageW(g_runtime_box, CB_GETCURSEL, 0, 0);
    ini_set(L"runtime", (sel > 0 && sel < (int)g_runtimes.size()) ? g_runtimes[sel].json : L"auto");
    ini_set(L"skip_intro", SendMessageW(g_skip_intro, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
    ini_set(L"record_window", SendMessageW(g_record, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0");
}

bool launch(HWND wnd)
{
    if (!g_can_play)
    {
        MessageBoxW(wnd, L"The VR build was not found next to this launcher.\n\nUnzip the whole KeeperFX VR package into your KeeperFX folder, then run the launcher from there.", k_title, MB_ICONWARNING);
        return false;
    }
    save_settings();
    const int sel = (int)SendMessageW(g_runtime_box, CB_GETCURSEL, 0, 0);
    if (sel > 0 && sel < (int)g_runtimes.size())
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", g_runtimes[sel].json.c_str());
    else if (g_runtimes.size() > 1 && reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime").empty())
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", g_runtimes[1].json.c_str());  // no active one: the first found
    else
        SetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr);

    const std::wstring exe = g_root + L"\\vr\\keeperfx_vr.exe";
    std::wstring cmd = L"\"" + exe + L"\" -vr";
    if (SendMessageW(g_skip_intro, BM_GETCHECK, 0, 0) == BST_CHECKED)
        cmd += L" -nointro";
    if (SendMessageW(g_record, BM_GETCHECK, 0, 0) == BST_CHECKED)
    {
        const std::wstring cfg = make_record_config();
        if (!cfg.empty())
            cmd += L" -config \"" + cfg + L"\"";
    }
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, g_root.c_str(), &si, &pi))
    {
        MessageBoxW(wnd, (L"Could not start the game (error " + std::to_wstring(GetLastError()) + L").").c_str(), k_title, MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void open_path(HWND wnd, const std::wstring& path)
{
    if (!file_exists(path))
    {
        MessageBoxW(wnd, (L"Not found:\n" + path).c_str(), k_title, MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(wnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/** The newer of the two places the game writes its log. */
std::wstring log_path()
{
    const std::wstring a = g_root + L"\\keeperfx.log", b = g_root + L"\\vr\\keeperfx.log";
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    const bool ha = GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa);
    const bool hb = GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb);
    if (ha && hb)
        return CompareFileTime(&fb.ftLastWriteTime, &fa.ftLastWriteTime) > 0 ? b : a;
    return hb ? b : a;
}

std::wstring url_encode(const std::wstring& s)
{
    std::string utf8;
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    utf8.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &utf8[0], n, nullptr, nullptr);
    std::wstring out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : utf8)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (wchar_t)c;
        else { out += L'%'; out += (wchar_t)hex[c >> 4]; out += (wchar_t)hex[c & 15]; }
    }
    return out;
}

void report_problem(HWND wnd)
{
    const int sel = (int)SendMessageW(g_runtime_box, CB_GETCURSEL, 0, 0);
    std::wstring rt = (sel >= 0 && sel < (int)g_runtimes.size()) ? g_runtimes[sel].name : L"?";
    std::wstring ver = g_version;
    for (wchar_t& c : ver) if (c == L'\r' || c == L'\n') c = L' ';
    const std::wstring body =
        L"**What happened:**\n\n\n**What I expected:**\n\n\n"
        L"**Headset and controllers:**\n\n"
        L"**VR software:** " + rt + L"\n"
        L"**Build:** " + ver + L"\n\n"
        L"(If you can, attach keeperfx.log from your KeeperFX folder - the launcher's Open log button shows it.)";
    const std::wstring url = std::wstring(k_issues_url) + L"?body=" + url_encode(body);
    ShellExecuteW(wnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

HWND make(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
    return c;
}

void build_ui(HWND wnd)
{
    int y = 14;
    make(wnd, L"STATIC", L"KeeperFX VR", SS_LEFT, 16, y, 300, 30, 0, g_big);
    std::wstring ver = g_version;
    const size_t nl = ver.find(L"Commit:");
    ver = (nl != std::wstring::npos) ? L"Test build, " + ver.substr(nl) : L"Test build";
    for (wchar_t& c : ver) if (c == L'\r' || c == L'\n') c = L' ';
    make(wnd, L"STATIC", ver.c_str(), SS_LEFT, 18, y + 32, 420, 18, 0, nullptr);
    y += 60;

    make(wnd, L"BUTTON", L"Setup", BS_GROUPBOX, 12, y, 436, 26 + 50 * (int)g_checks.size(), 0, g_bold);
    y += 22;
    for (const Check& c : g_checks)
    {
        make(wnd, L"STATIC", c.ok ? L"\u2714" : L"\u2716", SS_CENTER, 22, y, 18, 18, c.ok ? 1 : 2, g_bold);
        make(wnd, L"STATIC", c.label.c_str(), SS_LEFT, 44, y, 390, 18, 0, g_bold);
        make(wnd, L"STATIC", c.detail.c_str(), SS_LEFT, 44, y + 17, 396, 32, 0, nullptr);  // up to two lines
        y += 50;
    }
    y += 14;

    make(wnd, L"STATIC", L"VR software:", SS_LEFT, 16, y + 4, 100, 20, 0, nullptr);
    g_runtime_box = make(wnd, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 116, y, 332, 200, ID_RUNTIME, nullptr);
    for (const Runtime& r : g_runtimes)
        SendMessageW(g_runtime_box, CB_ADDSTRING, 0, (LPARAM)r.name.c_str());
    int sel = 0;
    const std::wstring saved = ini_str(L"runtime");
    for (size_t i = 1; i < g_runtimes.size(); i++)
        if (lower(g_runtimes[i].json) == lower(saved))
            sel = (int)i;
    SendMessageW(g_runtime_box, CB_SETCURSEL, sel, 0);
    y += 34;

    g_skip_intro = make(wnd, L"BUTTON", L"Skip the intro movies", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 420, 20, ID_SKIP_INTRO, nullptr);
    SendMessageW(g_skip_intro, BM_SETCHECK, ini_int(L"skip_intro", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 24;
    g_record = make(wnd, L"BUTTON", L"PC window 1920x1080, for recording videos (your settings are not changed)",
                    BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 432, 20, ID_RECORD, nullptr);
    SendMessageW(g_record, BM_SETCHECK, ini_int(L"record_window", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    y += 34;

    HWND play = make(wnd, L"BUTTON", L"Play in VR", BS_DEFPUSHBUTTON | WS_TABSTOP, 12, y, 436, 44, ID_PLAY, g_big);
    EnableWindow(play, g_can_play);
    y += 54;
    make(wnd, L"BUTTON", L"Open log", BS_PUSHBUTTON | WS_TABSTOP, 12, y, 140, 28, ID_LOG, nullptr);
    make(wnd, L"BUTTON", L"Read me", BS_PUSHBUTTON | WS_TABSTOP, 160, y, 140, 28, ID_README, nullptr);
    make(wnd, L"BUTTON", L"Report a problem", BS_PUSHBUTTON | WS_TABSTOP, 308, y, 140, 28, ID_REPORT, nullptr);
    y += 38;
    g_status = make(wnd, L"STATIC", L"Connect your headset (and start SteamVR if you use it), then press Play.", SS_LEFT, 14, y, 436, 18, 0, nullptr);
    y += 26;

    RECT r = {0, 0, S(460), S(y)};
    AdjustWindowRectEx(&r, GetWindowLongW(wnd, GWL_STYLE), FALSE, GetWindowLongW(wnd, GWL_EXSTYLE));
    SetWindowPos(wnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        build_ui(wnd);
        return 0;
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        const int id = GetDlgCtrlID((HWND)lp);
        if (id == 1) SetTextColor(dc, RGB(20, 140, 40));
        if (id == 2) SetTextColor(dc, RGB(200, 30, 30));
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_PLAY:
            set_status(L"Starting...");
            if (launch(wnd))
                DestroyWindow(wnd);
            else
                set_status(L"Not started.");
            return 0;
        case ID_LOG:    open_path(wnd, log_path()); return 0;
        case ID_README: open_path(wnd, g_root + L"\\README-VR.txt"); return 0;
        case ID_REPORT: report_problem(wnd); return 0;
        case ID_RUNTIME:
        case ID_SKIP_INTRO:
        case ID_RECORD:
            if (HIWORD(wp) == CBN_SELCHANGE || HIWORD(wp) == BN_CLICKED)
                save_settings();
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_root = self;
    g_root = g_root.substr(0, g_root.find_last_of(L"\\/"));
    g_ini = g_root + L"\\vr\\launcher.ini";
    g_version = read_text_file(g_root + L"\\vr\\version.txt");

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    g_bold = CreateFontIndirectW(&bold);
    LOGFONTW big = ncm.lfMessageFont;
    big.lfHeight = -MulDiv(15, g_dpi, 72);
    big.lfWeight = FW_SEMIBOLD;
    g_big = CreateFontIndirectW(&big);

    find_runtimes();
    run_checks();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"KeeperFXVRLauncher";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, k_title, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               CW_USEDEFAULT, CW_USEDEFAULT, S(460), S(600), nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(wnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
