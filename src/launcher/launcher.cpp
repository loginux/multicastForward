// 远程更新启动器（multicastForwardLauncher）
//
// 职责：在远端无人值守时守护主程序并完成 exe 的安全替换，替代批处理脚本。
//   --apply <主程序路径> <新文件路径>   替换、启动、存活检测、失败回滚，随后就地接管守护
//   不带参数                            守护模式：拉起主程序、常驻通知区域、退出后自动拉起
//
// 行为约定：
//   - 启动完成后不显示任何窗口，只留一个通知区域图标，避免被误关。
//   - 主程序被关闭后 3 秒自动拉起；连续启动失败时退避（6/12/24/30 秒）防止崩溃风暴。
//   - 只有通知区域右键【退出守护】才能结束启动器，且退出不影响正在运行的主程序。
//
// 不依赖 Qt / MFC：主程序因缺少运行库起不来时，启动器仍能回滚到旧版本。

#define WIN32_LEAN_AND_MEAN
#include "gitinfo.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>

namespace {

const wchar_t *const kMainExeName   = L"multicastForward.exe";
const wchar_t *const kGuardMutex    = L"Local\\McfwLauncherGuard";
const wchar_t *const kWndClass      = L"McfwLauncherWnd";
const wchar_t *const kAppTitle      = L"组播转发启动器";
const wchar_t *const kAppVersion    = MCFW_VERSION_FULL_W;

const UINT     kTrayMsg         = WM_APP + 1;
const UINT     kMsgQuit         = WM_APP + 2; // 另一个实例请求退出守护（对方已确认）
const UINT_PTR kTimerId         = 1;
const UINT     kTimerIntervalMs = 1000;
const DWORD    kRestartDelayMs  = 3000; // 主程序退出后多久自动拉起
const DWORD    kFastFailMs      = 5000; // 启动后短于该时长即退出，视为启动失败

const int kCmdShowMain = 101;
const int kCmdOpenLog  = 102;
const int kCmdExit     = 103;

HINSTANCE        g_hInst = nullptr;
HWND             g_hwnd = nullptr;
NOTIFYICONDATAW  g_nid = {};
bool             g_trayAdded = false;
UINT             g_taskbarCreated = 0;

// 资源 ID 转宽字符指针（本工程未定义 UNICODE，MAKEINTRESOURCE 会推出窄字符版本）
LPCWSTR resId(ULONG_PTR id)
{
    return reinterpret_cast<LPCWSTR>(id);
}

HANDLE    g_proc = nullptr;      // 当前主程序句柄
DWORD     g_startedAt = 0;
DWORD     g_nextStart = 0;
int       g_failStreak = 0;
int       g_quietSkips = 0;
bool      g_stopping = false;

std::wstring g_dir;        // 启动器所在目录
std::wstring g_mainPath;   // 被守护的主程序完整路径
std::wstring g_logPath;

// ---------------------------------------------------------------- 基础工具

std::wstring exeDir()
{
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring(L".") : p.substr(0, pos);
}

std::wstring baseName(const std::wstring &path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring dirName(const std::wstring &path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring(L".") : path.substr(0, pos);
}

void logMsg(const std::wstring &msg)
{
    FILE *fp = nullptr;
    if (_wfopen_s(&fp, g_logPath.c_str(), L"a, ccs=UTF-8") != 0 || !fp)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(fp, L"%04d-%02d-%02d %02d:%02d:%02d %s\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg.c_str());
    fclose(fp);
}

bool fileExists(const std::wstring &path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool moveFile(const std::wstring &from, const std::wstring &to)
{
    return MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

DWORD findPid(const std::wstring &exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (exeName == pe.szExeFile) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// 打开已在运行的同名进程（可等待、可查 pid），没有则返回 nullptr
HANDLE openRunning(const std::wstring &exeName)
{
    const DWORD pid = findPid(exeName);
    if (pid == 0)
        return nullptr;
    return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

// 等主程序完全退出（文件锁释放）后返回；超时也返回，交给后续 MoveFileEx 判定
void waitExit(const std::wstring &exeName, DWORD timeoutMs)
{
    const DWORD step = 100;
    for (DWORD waited = 0; waited < timeoutMs; waited += step) {
        if (findPid(exeName) == 0)
            return;
        Sleep(step);
    }
}

// 启动主程序，成功返回进程句柄（调用方负责关闭）
HANDLE startProcess(const std::wstring &exePath)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // 守护拉起时自动进入运行态，远端无人值守也能恢复服务
    std::wstring cmd = L"\"" + exePath + L"\" --autostart";
    std::wstring workDir = dirName(exePath);

    if (!CreateProcessW(exePath.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                        0, nullptr, workDir.c_str(), &si, &pi)) {
        logMsg(L"启动失败，错误码 " + std::to_wstring(GetLastError()));
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// 若有暂存文件则替换，然后启动并做存活检测；失败时回滚并启动旧版本
// 返回正在运行的主程序句柄（调用方负责关闭），失败返回 nullptr
HANDLE launchAndValidate(const std::wstring &target, const std::wstring &staged)
{
    const std::wstring backup = target + L".old";
    const std::wstring exeName = baseName(target);
    bool replaced = false;

    if (fileExists(staged)) {
        waitExit(exeName, 30000);

        if (fileExists(backup))
            DeleteFileW(backup.c_str());

        if (!moveFile(target, backup)) {
            logMsg(L"备份旧版本失败（" + std::to_wstring(GetLastError()) + L"），跳过本次更新");
        } else if (!moveFile(staged, target)) {
            logMsg(L"替换失败，恢复旧版本");
            moveFile(backup, target);
        } else {
            replaced = true;
            logMsg(L"已替换，准备启动新版本");
        }
    }

    HANDLE h = startProcess(target);
    if (!h) {
        if (replaced) {
            logMsg(L"新版本无法启动，回滚");
            DeleteFileW(target.c_str());
            moveFile(backup, target);
            h = startProcess(target);
        }
        return h;
    }

    if (!replaced)
        return h; // 不是更新启动，无需存活检测

    // 存活检测：4 秒内没退出认为启动成功
    const DWORD r = WaitForSingleObject(h, 4000);
    if (r == WAIT_TIMEOUT) {
        DeleteFileW(backup.c_str());
        logMsg(L"新版本运行正常，更新完成");
        return h;
    }

    logMsg(L"新版本启动后立即退出，回滚到旧版本");
    CloseHandle(h);
    DeleteFileW(target.c_str());
    if (!moveFile(backup, target))
        logMsg(L"回滚失败，需要人工处理");
    return startProcess(target);
}

// ---------------------------------------------------------------- 通知区域

void showBalloon();
void logTrayPosition();

void updateTip()
{
    if (!g_trayAdded)
        return;

    std::wstring tip = std::wstring(kAppTitle) + L" " + kAppVersion + L" · 守护中";
    if (g_proc)
        tip += L"（主程序运行中）";
    else if (g_failStreak > 0)
        tip += L"（等待重新拉起）";

    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void addTrayIcon()
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMsg;
    g_nid.hIcon = LoadIconW(g_hInst, resId(1));
    if (!g_nid.hIcon) {
        g_nid.hIcon = LoadIconW(nullptr, resId(32512));
        logMsg(L"警告：程序图标资源未载入，改用系统默认图标");
    }
    if (!g_nid.hIcon)
        logMsg(L"警告：图标句柄为空，通知区域会显示空白");
    wcsncpy_s(g_nid.szTip, (std::wstring(kAppTitle) + L" " + kAppVersion + L" · 守护中").c_str(), _TRUNCATE);

    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayAdded = true;
        updateTip();
        logMsg(L"通知区域图标已创建");
        logTrayPosition();
        showBalloon();
    } else {
        logMsg(L"警告：添加通知区域图标失败，错误码 " + std::to_wstring(GetLastError()));
    }
}

// 记录图标在屏幕上的实际位置：查不到说明 explorer 没接受这个图标；
// 查到坐标在屏幕外/极小，说明被收进了 ^ 隐藏区
void logTrayPosition()
{
    NOTIFYICONIDENTIFIER id;
    ZeroMemory(&id, sizeof(id));
    id.cbSize = sizeof(id);
    id.hWnd = g_hwnd;
    id.uID = 1;

    RECT rc;
    const HRESULT hr = Shell_NotifyIconGetRect(&id, &rc);
    if (SUCCEEDED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"图标位置: (%ld,%ld)-(%ld,%ld)",
                   rc.left, rc.top, rc.right, rc.bottom);
        logMsg(buf);
    } else {
        wchar_t buf[128];
        swprintf_s(buf, L"图标位置查询失败 hr=0x%08lX（explorer 未接受该图标）",
                   static_cast<unsigned long>(hr));
        logMsg(buf);
    }
}

// 首次加图标后弹一次气泡：Win10 默认把新图标放进 ^ 隐藏区，光有图标用户找不到
void showBalloon()
{
    if (!g_trayAdded)
        return;

    g_nid.uFlags = NIF_INFO;
    g_nid.dwInfoFlags = NIIF_INFO;
    g_nid.uTimeout = 5000;
    wcsncpy_s(g_nid.szInfoTitle, kAppTitle, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo,
              L"已在后台守护，主程序关闭后会自动拉起。\n"
              L"右键图标可显示主程序或退出；若看不到图标，请点任务栏 ^ 展开。",
              _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);

    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void removeTrayIcon()
{
    if (!g_trayAdded)
        return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

// ---------------------------------------------------------------- 主窗口定位

struct EnumCtx {
    DWORD pid;
    HWND hwnd;
};

BOOL CALLBACK enumWindowsProc(HWND h, LPARAM lp)
{
    auto *c = reinterpret_cast<EnumCtx *>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h))
        return TRUE;
    if (GetWindow(h, GW_OWNER) != nullptr)
        return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;
    c->hwnd = h;
    return FALSE;
}

void showMainWindow()
{
    DWORD pid = 0;
    if (g_proc)
        pid = GetProcessId(g_proc);
    if (pid == 0)
        pid = findPid(baseName(g_mainPath));

    if (pid == 0) {
        MessageBoxW(g_hwnd, L"主程序尚未启动，请稍候再试。", kAppTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    EnumCtx ctx{pid, nullptr};
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.hwnd) {
        MessageBoxW(g_hwnd, L"主程序窗口未找到（可能正在启动）。", kAppTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (IsIconic(ctx.hwnd))
        ShowWindow(ctx.hwnd, SW_RESTORE);
    ShowWindow(ctx.hwnd, SW_SHOW);
    SetForegroundWindow(ctx.hwnd);
}

void showTrayMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING | (g_proc ? MF_ENABLED : MF_GRAYED), kCmdShowMain,
                L"显示主程序窗口(&O)");
    AppendMenuW(menu, MF_STRING, kCmdOpenLog, L"打开运行日志(&L)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"退出守护(&X)");
    SetMenuDefaultItem(menu, kCmdShowMain, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd); // 否则菜单不会随点击外部消失
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------- 守护状态机

DWORD restartDelayMs()
{
    if (g_failStreak < 3)
        return kRestartDelayMs;
    int shift = g_failStreak - 3;
    if (shift > 3)
        shift = 3;
    DWORD d = kRestartDelayMs << shift; // 6 / 12 / 24 秒
    return (d > 30000) ? 30000 : d;
}

void startMain()
{
    const std::wstring exeName = baseName(g_mainPath);
    const std::wstring staged = g_mainPath + L".new";

    // 首次部署时常忘了把模板复制成 config.ini，这里留一条明确日志，省得反复重启排查
    if (!fileExists(dirName(g_mainPath) + L"\\config.ini"))
        logMsg(L"警告：同目录没有 config.ini，主程序将使用内置默认值");

    // 已有同名进程（人工启动，或 --apply 刚拉起新版）就直接接管，避免起第二个实例
    if (HANDLE h = openRunning(exeName)) {
        g_proc = h;
        g_startedAt = GetTickCount();
        g_failStreak = 0;
        logMsg(L"检测到主程序已在运行，接管监控");
        updateTip();
        return;
    }

    // 标记由启动器拉起：主程序更新时直接退出，等本进程发现 .new 完成替换
    SetEnvironmentVariableW(L"MCFW_LAUNCHED_BY_LAUNCHER", L"1");

    HANDLE h = launchAndValidate(g_mainPath, staged);
    if (!h) {
        if (g_failStreak < 2)
            ++g_failStreak;
        const DWORD delay = restartDelayMs();
        g_nextStart = GetTickCount() + delay;
        logMsg(L"无法启动主程序，" + std::to_wstring(delay / 1000) + L" 秒后重试");
        updateTip();
        return;
    }

    g_proc = h;
    g_startedAt = GetTickCount();
    g_failStreak = 0;
    logMsg(L"主程序已启动");
    updateTip();
}

void onTick()
{
    if (g_stopping)
        return;

    if (g_proc) {
        if (WaitForSingleObject(g_proc, 0) != WAIT_OBJECT_0)
            return;

        DWORD code = 0;
        GetExitCodeProcess(g_proc, &code);
        CloseHandle(g_proc);
        g_proc = nullptr;

        const DWORD ran = GetTickCount() - g_startedAt;
        g_failStreak = (ran < kFastFailMs) ? g_failStreak + 1 : 0;
        const DWORD delay = restartDelayMs();
        g_nextStart = GetTickCount() + delay;

        if (g_failStreak <= 3) {
            logMsg(L"主程序已退出（退出码 " + std::to_wstring(code) + L"），"
                   + std::to_wstring(delay / 1000) + L" 秒后自动拉起");
        } else if (++g_quietSkips % 10 == 1) {
            logMsg(L"主程序反复退出（连续 " + std::to_wstring(g_failStreak)
                   + L" 次），仍在每 " + std::to_wstring(delay / 1000) + L" 秒尝试拉起");
        }
        updateTip();
        return;
    }

    if (GetTickCount() < g_nextStart)
        return;
    startMain();
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
        // 资源管理器重启后图标会消失，需要重新添加
        g_trayAdded = false;
        addTrayIcon();
        return 0;
    }

    switch (msg) {
    case kTrayMsg:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            showTrayMenu();
        } else if (lp == WM_LBUTTONDBLCLK) {
            showMainWindow();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kCmdShowMain:
            showMainWindow();
            return 0;
        case kCmdOpenLog:
            ShellExecuteW(nullptr, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case kCmdExit:
            if (MessageBoxW(h,
                            L"退出后主程序将不再被自动拉起。\n主程序本身保持运行，需要时请自行关闭。\n\n确定要退出守护吗？",
                            kAppTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                logMsg(L"用户从通知区域退出守护");
                g_stopping = true;
                DestroyWindow(h);
            }
            return 0;
        default:
            break;
        }
        break;

    case kMsgQuit:
        // 另一个实例通过提示框确认过退出，这里不再二次确认
        logMsg(L"收到退出请求（另一次启动），守护结束");
        g_stopping = true;
        DestroyWindow(h);
        return 0;

    case WM_TIMER:
        if (wp == kTimerId)
            onTick();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {
            // 系统关机/注销：不再拉起主程序
            g_stopping = true;
            DestroyWindow(h);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(h, kTimerId);
        removeTrayIcon();
        g_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 重复启动时的兜底入口：即使托盘图标被收进隐藏区，也能操作正在运行的守护
void askExistingGuard()
{
    const INT_PTR r = MessageBoxW(
        nullptr,
        L"启动器已在后台运行。\n\n"
        L"【是】显示主程序窗口\n"
        L"【否】退出守护（主程序继续运行）\n"
        L"【取消】关闭本提示\n\n"
        L"提示：图标可能被 Windows 收进任务栏的 ^ 隐藏区，"
        L"可在“设置 → 个性化 → 任务栏 → 选择哪些图标显示在任务栏上”里打开。",
        kAppTitle, MB_YESNOCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON1);

    if (r == IDCANCEL || r == 0)
        return;

    HWND guard = FindWindowW(kWndClass, nullptr);
    if (!guard) {
        logMsg(L"未找到守护窗口，无法转发请求");
        return;
    }
    if (r == IDYES)
        PostMessageW(guard, WM_COMMAND, kCmdShowMain, 0);
    else
        PostMessageW(guard, kMsgQuit, 0, 0);
}

// 进入守护：建隐藏窗口 + 托盘图标 + 定时器，然后跑消息循环
// initial 为已经拉起的主程序句柄，可为 nullptr
int runGuard(HANDLE initial, bool notifyIfAlreadyRunning)
{
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kGuardMutex);
    if (!mutex)
        return 2;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        logMsg(L"已有守护实例在运行，本次不再重复启动");
        CloseHandle(mutex);
        if (notifyIfAlreadyRunning)
            askExistingGuard();
        return 0;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kWndClass;
    wc.hIcon = LoadIconW(g_hInst, resId(1));
    if (!wc.hIcon)
        wc.hIcon = LoadIconW(nullptr, resId(32512));
    RegisterClassExW(&wc);

    // 不显示任何窗口：整个守护只有通知区域一个图标
    g_hwnd = CreateWindowExW(0, kWndClass, kAppTitle, 0, 0, 0, 0, 0,
                             nullptr, nullptr, g_hInst, nullptr);
    if (!g_hwnd) {
        logMsg(L"创建守护窗口失败，错误码 " + std::to_wstring(GetLastError()));
        CloseHandle(mutex);
        return 2;
    }

    addTrayIcon();

    if (initial) {
        g_proc = initial;
        g_startedAt = GetTickCount();
        logMsg(L"守护已接管正在运行的主程序");
    } else {
        g_nextStart = 0;
        logMsg(std::wstring(L"守护已启动 ") + kAppVersion + L"，图标位于通知区域，右键可退出");
    }
    updateTip();

    SetTimer(g_hwnd, kTimerId, kTimerIntervalMs, nullptr);
    onTick();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_proc) {
        CloseHandle(g_proc);
        g_proc = nullptr;
    }
    CloseHandle(mutex);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    g_hInst = hInst;
    g_dir = exeDir();
    g_mainPath = g_dir + L"\\" + kMainExeName;
    g_logPath = g_dir + L"\\launcher.log";
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = 0;

    if (argv && argc >= 4 && std::wstring(argv[1]) == L"--apply") {
        const std::wstring target = argv[2];
        const std::wstring staged = argv[3];
        g_mainPath = target;
        logMsg(L"应用更新：" + baseName(staged));

        HANDLE h = launchAndValidate(target, staged);
        if (!h) {
            rc = 1;
        } else {
            // 替换完成后就地接管守护；若已有守护实例，则由它继续看护，这里退出
            rc = runGuard(h, false);
        }
    } else {
        rc = runGuard(nullptr, true);
    }

    if (argv)
        LocalFree(argv);
    return rc;
}
