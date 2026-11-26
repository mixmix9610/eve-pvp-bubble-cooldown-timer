#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <commdlg.h>        // 文件选择对话框
#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#pragma comment(lib, "comdlg32.lib")   // 链接通用对话框库
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// 定义 REAL 类型（避免 GDI+ 编译问题）
typedef float REAL;

static const int COUNTDOWN_SEC = 120;
static const int CHECK_INTERVAL_MS = 300;
static const int GLOBAL_UPDATE_INTERVAL_MS = 100;
static const int BEEP_LAST_SECONDS = 5;
static const int FLASH_START_SECONDS = 30;
static const int GRID_SIZE = 20;
static const int WINDOW_WIDTH = 100;
static const int WINDOW_HEIGHT = 130;
static const COLORREF TRANSPARENT_COLOR = RGB(255, 0, 255);

// 窗口类名
static wchar_t TIMER_WINDOW_CLASS[64];
static wchar_t MAIN_WINDOW_CLASS[64];
static wchar_t REGION_WINDOW_CLASS[64];
static wchar_t SETTINGS_WINDOW_CLASS[64];

static const int TRAY_ICON_ID = 1001;
static const UINT WM_APP_TRAY = WM_APP + 1;
static const int HOTKEY_NEW_TIMER = 1;
static const int HOTKEY_RESET_ALL = 2;
static const int HOTKEY_TOGGLE_ALL = 3;

// 自定义消息
#define WM_USER_CREATE_TIMER (WM_USER + 100)

// ---- 用户可调参数 ----
static int g_zoomPercent = 100;
static int g_soundType = 0;               // 0=默认,1=感叹号,2=问号,3=星号,4=停止
static double g_matchThreshold = 0.65;
static int g_checkIntervalMs = 300;

// ---- 全局变量 ----
struct SearchRegion {
    int left, top, width, height;
    bool defined;
};

struct TimerState {
    int id;
    double remain;
    bool active;
    bool flashActive;
    bool flashState;
    bool hideScheduled;
    std::chrono::steady_clock::time_point hideAt;
    HWND hwnd;
    int x, y;
    int lastSeconds;
    bool markedForDeletion;   // 标记是否待删除
    TimerState(int timerId)
        : id(timerId), remain(COUNTDOWN_SEC), active(true), flashActive(false), flashState(false),
          hideScheduled(false), hwnd(NULL), x(0), y(0), lastSeconds(-1), markedForDeletion(false) {}
};

static HINSTANCE g_hInstance = nullptr;
static HWND g_mainWnd = nullptr;
static HWND g_settingsWnd = nullptr;
static SearchRegion g_searchRegion{0,0,0,0,false};
static std::wstring g_configPath;
static int g_screenWidth = 0, g_screenHeight = 0;
static std::mutex g_timerMutex;
static std::vector<std::unique_ptr<TimerState>> g_timers;
static std::mutex g_imageMutex;
static std::vector<std::pair<std::wstring, cv::Mat>> g_triggerImages;
static std::map<std::wstring, bool> g_triggerState;
static std::set<std::wstring> g_triggeredKeys;
static bool g_triggerInitialized = false;
static std::atomic<bool> g_running{true};
static bool g_timersVisible = true;
static Gdiplus::Bitmap* g_baseIcon = nullptr;
static HICON g_trayIcon = nullptr;
static std::vector<std::wstring> g_customImagePaths;   // 持久化自定义图片

// ---- 工具函数 ----
std::wstring StringFromCurrentDirectory() {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(NULL, buffer, MAX_PATH) > 0) {
        std::filesystem::path exePath(buffer);
        return exePath.parent_path().wstring();
    }
    return std::filesystem::current_path().wstring();
}

std::wstring LoadConfigString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue) {
    wchar_t buffer[256] = {};
    GetPrivateProfileStringW(section, key, defaultValue, buffer, _countof(buffer), g_configPath.c_str());
    return buffer;
}

int LoadConfigInt(const wchar_t* section, const wchar_t* key, int defaultValue) {
    return GetPrivateProfileIntW(section, key, defaultValue, g_configPath.c_str());
}

void SaveConfigInt(const wchar_t* section, const wchar_t* key, int value) {
    wchar_t buffer[32];
    swprintf_s(buffer, L"%d", value);
    WritePrivateProfileStringW(section, key, buffer, g_configPath.c_str());
}

void SaveRegionToConfig() {
    SaveConfigInt(L"SearchRegion", L"Left", g_searchRegion.left);
    SaveConfigInt(L"SearchRegion", L"Top", g_searchRegion.top);
    SaveConfigInt(L"SearchRegion", L"Width", g_searchRegion.width);
    SaveConfigInt(L"SearchRegion", L"Height", g_searchRegion.height);
}

void LoadRegionFromConfig() {
    int width = LoadConfigInt(L"SearchRegion", L"Width", 0);
    int height = LoadConfigInt(L"SearchRegion", L"Height", 0);
    if (width > 0 && height > 0) {
        g_searchRegion.left = LoadConfigInt(L"SearchRegion", L"Left", 0);
        g_searchRegion.top = LoadConfigInt(L"SearchRegion", L"Top", 0);
        g_searchRegion.width = width;
        g_searchRegion.height = height;
        g_searchRegion.defined = true;
    } else {
        g_searchRegion.defined = false;
    }
}

void EnsureTrayIcon() {
    if (g_trayIcon) return;
    std::filesystem::path basePath = StringFromCurrentDirectory();
    std::filesystem::path iconPath = basePath / L"paopao.png";
    if (std::filesystem::exists(iconPath)) {
        Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromFile(iconPath.c_str());
        if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
            HICON icon = nullptr;
            bitmap->GetHICON(&icon);
            if (icon) g_trayIcon = icon;
        }
        delete bitmap;
    }
    if (!g_trayIcon) g_trayIcon = LoadIcon(NULL, IDI_APPLICATION);
}

cv::Mat CaptureScreenRegion() {
    int captureLeft = 0, captureTop = 0, captureWidth = g_screenWidth, captureHeight = g_screenHeight;
    if (g_searchRegion.defined) {
        captureLeft = g_searchRegion.left;
        captureTop = g_searchRegion.top;
        captureWidth = g_searchRegion.width;
        captureHeight = g_searchRegion.height;
    }
    HDC hScreen = GetDC(NULL);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreen, captureWidth, captureHeight);
    HGDIOBJ oldBitmap = SelectObject(hMem, hBitmap);
    BitBlt(hMem, 0, 0, captureWidth, captureHeight, hScreen, captureLeft, captureTop, SRCCOPY | CAPTUREBLT);
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = captureWidth;
    bi.biHeight = -captureHeight;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    std::vector<BYTE> buffer(captureWidth * captureHeight * 4);
    GetDIBits(hMem, hBitmap, 0, captureHeight, buffer.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    cv::Mat image(captureHeight, captureWidth, CV_8UC4, buffer.data());
    cv::Mat result;
    cv::cvtColor(image, result, cv::COLOR_BGRA2BGR);
    SelectObject(hMem, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);
    return result;
}

bool FindTemplate(const cv::Mat& screenshot, const cv::Mat& templ, cv::Point& matchLocation, double threshold) {
    if (screenshot.cols < templ.cols || screenshot.rows < templ.rows) return false;
    cv::Mat screenGray, templGray;
    cv::cvtColor(screenshot, screenGray, cv::COLOR_BGR2GRAY);
    if (templ.channels() == 3) cv::cvtColor(templ, templGray, cv::COLOR_BGR2GRAY);
    else templGray = templ;
    cv::Mat result;
    cv::matchTemplate(screenGray, templGray, result, cv::TM_CCOEFF_NORMED);
    double minVal, maxVal;
    cv::Point minLoc, maxLoc;
    cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
    if (maxVal >= threshold) { matchLocation = maxLoc; return true; }
    if (templGray.cols > 8 && templGray.rows > 8) {
        cv::Mat screenEdges, templEdges;
        cv::Canny(screenGray, screenEdges, 50, 150);
        cv::Canny(templGray, templEdges, 50, 150);
        cv::matchTemplate(screenEdges, templEdges, result, cv::TM_CCOEFF_NORMED);
        cv::minMaxLoc(result, &minVal, &maxVal, &minLoc, &maxLoc);
        if (maxVal >= std::min(threshold + 0.1, 0.6)) { matchLocation = maxLoc; return true; }
    }
    return false;
}

void PlayBeep(int freq, int duration) {
    Beep(freq, duration);
}

UINT GetSelectedSoundType() {
    switch (g_soundType) {
        case 0: return MB_OK;
        case 1: return MB_ICONEXCLAMATION;
        case 2: return MB_ICONQUESTION;
        case 3: return MB_ICONASTERISK;
        case 4: return MB_ICONHAND;
        default: return MB_OK;
    }
}

// ---- 前向声明 ----
LRESULT CALLBACK TimerWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK RegionWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);class TimerWindow {
public:
    TimerWindow(int id, HWND parent)
        : state(id), parentWnd(parent), dragging(false), winStartX(0), winStartY(0), dragStartX(0), dragStartY(0), hideAfterDraw(false) {
        LoadPosition();

        double scale = g_zoomPercent / 100.0;
        int winW = (int)(WINDOW_WIDTH * scale);
        int winH = (int)(WINDOW_HEIGHT * scale);

        hwnd_ = CreateWindowExW(
            WS_EX_LAYERED,
            TIMER_WINDOW_CLASS,
            L"TimerWindow",
            WS_POPUP,
            state.x, state.y,
            winW, winH,
            NULL,
            NULL,
            g_hInstance,
            this
        );
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            SetLayeredWindowAttributes(hwnd_, TRANSPARENT_COLOR, 0, LWA_COLORKEY);
            // 裁剪窗口区域，去除边缘像素

            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOCOPYBITS);
            ShowWindow(hwnd_, SW_SHOW);
            UpdateWindow(hwnd_);
        } else {
            DWORD err = GetLastError();
            wchar_t buf[256];
            swprintf_s(buf, L"窗口创建失败！错误码: %d (0x%08X)", err, err);
            MessageBoxW(NULL, buf, L"错误", MB_OK);
        }
    }

    ~TimerWindow() {
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    }

    // 移动构造和移动赋值（显式实现，确保窗口句柄正确转移）
    TimerWindow(TimerWindow&& other) noexcept
        : state(std::move(other.state)),
          parentWnd(other.parentWnd),
          dragging(other.dragging),
          winStartX(other.winStartX),
          winStartY(other.winStartY),
          dragStartX(other.dragStartX),
          dragStartY(other.dragStartY),
          hideAfterDraw(other.hideAfterDraw),
          hwnd_(other.hwnd_)
    {
        other.hwnd_ = nullptr;
    }

    TimerWindow& operator=(TimerWindow&& other) noexcept {
        if (this != &other) {
            if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
            state = std::move(other.state);
            parentWnd = other.parentWnd;
            dragging = other.dragging;
            winStartX = other.winStartX;
            winStartY = other.winStartY;
            dragStartX = other.dragStartX;
            dragStartY = other.dragStartY;
            hideAfterDraw = other.hideAfterDraw;
            hwnd_ = other.hwnd_;
            other.hwnd_ = nullptr;
        }
        return *this;
    }

    // 删除拷贝
    TimerWindow(const TimerWindow&) = delete;
    TimerWindow& operator=(const TimerWindow&) = delete;

    void DestroyTimerWindow() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        state.markedForDeletion = true;
    }

    void LoadPosition() {
        std::wstring section = L"TimerPos";
        std::wstring keyX = L"Timer" + std::to_wstring(state.id) + L"_X";
        std::wstring keyY = L"Timer" + std::to_wstring(state.id) + L"_Y";
        int x = LoadConfigInt(section.c_str(), keyX.c_str(), -1);
        int y = LoadConfigInt(section.c_str(), keyY.c_str(), -1);
        if (x >= 0 && y >= 0) {
            state.x = x;
            state.y = y;
            return;
        }
        int index = static_cast<int>(g_timers.size());
        double scale = g_zoomPercent / 100.0;
        int winW = (int)(WINDOW_WIDTH * scale);
        int winH = (int)(WINDOW_HEIGHT * scale);
        state.x = std::max(0, g_screenWidth - winW - 8);
        state.y = 8 + index * (winH + 4);
        if (state.y + winH > g_screenHeight) state.y = 8;
    }

    void SavePosition() {
        std::wstring section = L"TimerPos";
        std::wstring keyX = L"Timer" + std::to_wstring(state.id) + L"_X";
        std::wstring keyY = L"Timer" + std::to_wstring(state.id) + L"_Y";
        SaveConfigInt(section.c_str(), keyX.c_str(), state.x);
        SaveConfigInt(section.c_str(), keyY.c_str(), state.y);
    }

    void Start() {
        state.active = true;
        state.remain = COUNTDOWN_SEC;
        state.flashActive = false;
        state.flashState = false;
        state.hideScheduled = false;
        if (hwnd_) { ShowWindow(hwnd_, SW_SHOW); UpdateWindow(hwnd_); }
    }

    void Update(double deltaSeconds) {
        if (state.markedForDeletion) return;

        // 检查是否到达销毁时间
        if (state.hideScheduled && std::chrono::steady_clock::now() >= state.hideAt) {
            if (hwnd_) {
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
            }
            state.markedForDeletion = true;
            return;
        }

        if (!state.active) return;

        state.remain -= deltaSeconds;
        bool needRedraw = false;
        if (state.remain <= 0.0) {
            state.remain = 0.0;
            state.active = false;
            state.flashActive = false;
            if (!state.hideScheduled) {
                state.hideScheduled = true;
                state.hideAt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                MessageBeep(GetSelectedSoundType());
            }
            needRedraw = true;
        } else {
            int newSeconds = static_cast<int>(std::ceil(state.remain));
            bool secondsChanged = (newSeconds != state.lastSeconds);
            if (secondsChanged) {
                state.lastSeconds = newSeconds;
                needRedraw = true;
            }
            if (state.remain <= FLASH_START_SECONDS) {
                state.flashActive = true;
                if (secondsChanged && (newSeconds % 2 == 0)) {
                    MessageBeep(GetSelectedSoundType());
                }
            }
            if (state.remain <= BEEP_LAST_SECONDS && secondsChanged) {
                MessageBeep(GetSelectedSoundType());
            }
        }

        if (state.active || state.flashActive) {
            state.flashState = !state.flashState;
            needRedraw = true;
        }

        if (needRedraw && hwnd_) {
            InvalidateRect(hwnd_, NULL, TRUE);
        }
    }

    void ToggleVisibility() { if (hwnd_) ShowWindow(hwnd_, IsWindowVisible(hwnd_) ? SW_HIDE : SW_SHOW); }
    void Hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }
    void Show() { if (hwnd_) ShowWindow(hwnd_, SW_SHOW); }
    HWND hwnd() const { return hwnd_; }

    TimerState state;

void OnMouseDown(int x, int y) {
    dragging = true;
    POINT pt = { x, y };
    ClientToScreen(hwnd_, &pt);           // 转为屏幕坐标
    dragStartX = pt.x;
    dragStartY = pt.y;
    RECT rect;
    GetWindowRect(hwnd_, &rect);
    winStartX = rect.left;
    winStartY = rect.top;
}

void OnMouseMove(int x, int y) {
    if (!dragging) return;
    POINT pt = { x, y };
    ClientToScreen(hwnd_, &pt);           // 转为屏幕坐标
    int newX = winStartX + (pt.x - dragStartX);
    int newY = winStartY + (pt.y - dragStartY);
    // 边界限制
    int maxX = g_screenWidth - (int)(WINDOW_WIDTH * g_zoomPercent / 100.0);
    int maxY = g_screenHeight - (int)(WINDOW_HEIGHT * g_zoomPercent / 100.0);
    newX = std::max(0, std::min(maxX, newX));
    newY = std::max(0, std::min(maxY, newY));
    state.x = newX;
    state.y = newY;
    SetWindowPos(hwnd_, HWND_TOPMOST, newX, newY, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
}

void OnMouseUp() {
    dragging = false;
    SavePosition();
    if (hwnd_) {
        InvalidateRect(hwnd_, NULL, TRUE);
        UpdateWindow(hwnd_);
    }
}

    void Paint(HDC hdc) {
        Gdiplus::Graphics graphics(hdc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);

        double scale = g_zoomPercent / 100.0;
        int winW = (int)(WINDOW_WIDTH * scale);
        int winH = (int)(WINDOW_HEIGHT * scale);
        int iconSize = (int)(96 * scale);
        int fontSize = (int)(20 * scale);
        int textY = (int)(108 * scale);

        // 透明背景 (SourceCopy 避免边缘混合)
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, GetRValue(TRANSPARENT_COLOR), GetGValue(TRANSPARENT_COLOR), GetBValue(TRANSPARENT_COLOR)));
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.FillRectangle(&bgBrush, -1, -1, winW + 2, winH + 2);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);

        // 图标
        Gdiplus::Rect iconRect((winW - iconSize) / 2, (int)(8 * scale), iconSize, iconSize);
        if (g_baseIcon && g_baseIcon->GetLastStatus() == Gdiplus::Ok) {
            graphics.DrawImage(g_baseIcon, iconRect);
        } else {
            Gdiplus::SolidBrush circleBrush(Gdiplus::Color(255, 200, 200, 200));
            graphics.FillEllipse(&circleBrush, iconRect);
        }

        // 进度圈
        double progress = state.active ? (state.remain / COUNTDOWN_SEC) : 0.0;
        int sweep = (int)(360.0 * progress);
        if (sweep > 0) {
            Gdiplus::SolidBrush overlayBrush(Gdiplus::Color(128, 0, 255, 0));
            graphics.FillPie(&overlayBrush, iconRect, -90, -sweep);
        }

        // 文字
        std::wstring text;
        if (!state.active) text = L"就绪";
        else {
            int seconds = (int)std::ceil(state.remain);
            if (seconds != state.lastSeconds) state.lastSeconds = seconds;
            text = std::to_wstring(state.lastSeconds);
        }

        Gdiplus::FontFamily fontFamily(L"Arial");
        Gdiplus::Font font(&fontFamily, (float)fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush textBrush(state.flashActive && state.flashState ? Gdiplus::Color(255, 255, 255, 0) : Gdiplus::Color(255, 0, 255, 0));
        Gdiplus::StringFormat format;
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF textRect(0.0f, (float)textY, (float)winW, (float)(winH - textY));
        graphics.DrawString(text.c_str(), (int)text.size(), &font, textRect, &format, &textBrush);
    }

private:
    HWND hwnd_ = nullptr;
    HWND parentWnd = nullptr;
    bool dragging;
    int winStartX, winStartY, dragStartX, dragStartY;
    bool hideAfterDraw;
};std::vector<std::unique_ptr<TimerWindow>> g_timerWindows;

TimerWindow* FindTimerWindow(HWND hwnd) {
    for (auto& timer : g_timerWindows) if (timer->hwnd() == hwnd) return timer.get();
    return nullptr;
}

void CreateTimerWindow() {
    int timerId = static_cast<int>(g_timerWindows.size()) + 1;
    auto timer = std::make_unique<TimerWindow>(timerId, g_mainWnd);
    timer->Start();
    g_timerWindows.push_back(std::move(timer));
}

void ResetAllTimerWindows() {
    for (auto& timer : g_timerWindows) {
        timer->DestroyTimerWindow();
        timer->state.active = false;
    }
    g_timerWindows.clear();
    g_triggeredKeys.clear();
    g_triggerState.clear();
}

void ToggleAllTimerWindows() { for (auto& timer : g_timerWindows) timer->ToggleVisibility(); }
void ShowAllTimerWindows() { for (auto& timer : g_timerWindows) timer->Show(); }
void HideAllTimerWindows() { for (auto& timer : g_timerWindows) timer->Hide(); }

void UpdateAllTimerWindows(double deltaSeconds) {
    for (auto& timer : g_timerWindows) {
        timer->Update(deltaSeconds);
    }
    auto it = g_timerWindows.begin();
    while (it != g_timerWindows.end()) {
        if ((*it)->state.markedForDeletion) {
            it = g_timerWindows.erase(it);
        } else {
            ++it;
        }
    }
}

void DrawTimerWindow(HWND hwnd, HDC hdc) {
    TimerWindow* timerWnd = FindTimerWindow(hwnd);
    if (timerWnd) timerWnd->Paint(hdc);
}

LRESULT CALLBACK TimerWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    TimerWindow* timer = reinterpret_cast<TimerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        if (cs && cs->lpCreateParams) {
            timer = reinterpret_cast<TimerWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(timer));
        }
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (timer) timer->OnMouseDown(pt.x, pt.y);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (wParam & MK_LBUTTON) {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TimerWindow* t = FindTimerWindow(hwnd);
            if (t) t->OnMouseMove(pt.x, pt.y);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        TimerWindow* t = FindTimerWindow(hwnd);
        if (t) t->OnMouseUp();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        DrawTimerWindow(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// ---- 区域选择相关 ----
static bool g_regionSelecting = false;
static RECT g_regionStartRect;
static HWND g_regionWindow = nullptr;

static void NormalizeRect(RECT* rc) {
    if (rc->left > rc->right) std::swap(rc->left, rc->right);
    if (rc->top > rc->bottom) std::swap(rc->top, rc->bottom);
}

void StartRegionSelection(HWND parent) {
    g_regionSelecting = true;
    g_regionStartRect = {0,0,0,0};
    g_regionWindow = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED,
                                     REGION_WINDOW_CLASS,
                                     L"RegionOverlay",
                                     WS_POPUP,
                                     0, 0, g_screenWidth, g_screenHeight,
                                     parent, NULL, g_hInstance, NULL);
    if (g_regionWindow) {
        SetLayeredWindowAttributes(g_regionWindow, 0, (BYTE)(255 * 0.35), LWA_ALPHA);
        ShowWindow(g_regionWindow, SW_SHOW);
        SetCapture(g_regionWindow);
    }
}

void EndRegionSelection(HWND overlay, bool saveRegion) {
    if (overlay) { ReleaseCapture(); DestroyWindow(overlay); g_regionWindow = nullptr; }
    if (saveRegion) {
        int left = std::min(g_regionStartRect.left, g_regionStartRect.right);
        int top = std::min(g_regionStartRect.top, g_regionStartRect.bottom);
        int right = std::max(g_regionStartRect.left, g_regionStartRect.right);
        int bottom = std::max(g_regionStartRect.top, g_regionStartRect.bottom);
        g_searchRegion.left = std::max(0, left);
        g_searchRegion.top = std::max(0, top);
        g_searchRegion.width = std::max(1, std::min(g_screenWidth, right) - g_searchRegion.left);
        g_searchRegion.height = std::max(1, std::min(g_screenHeight, bottom) - g_searchRegion.top);
        g_searchRegion.defined = true;
        SaveRegionToConfig();
    }
    g_regionSelecting = false;
}

LRESULT CALLBACK RegionWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_LBUTTONDOWN:
        g_regionStartRect.left = GET_X_LPARAM(lParam);
        g_regionStartRect.top = GET_Y_LPARAM(lParam);
        g_regionStartRect.right = g_regionStartRect.left;
        g_regionStartRect.bottom = g_regionStartRect.top;
        SetCapture(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (wParam & MK_LBUTTON) {
            g_regionStartRect.right = GET_X_LPARAM(lParam);
            g_regionStartRect.bottom = GET_Y_LPARAM(lParam);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_LBUTTONUP:
        EndRegionSelection(hwnd, true);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) EndRegionSelection(hwnd, false);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        HBRUSH brush = CreateSolidBrush(RGB(0,0,0));
        FillRect(hdc, &ps.rcPaint, brush);
        DeleteObject(brush);
        if (g_regionSelecting) {
            RECT drawRect = {g_regionStartRect.left, g_regionStartRect.top, g_regionStartRect.right, g_regionStartRect.bottom};
            NormalizeRect(&drawRect);
            HBRUSH selBrush = CreateSolidBrush(RGB(255,255,255));
            FrameRect(hdc, &drawRect, selBrush);
            DeleteObject(selBrush);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// ---- 辅助函数 ----
bool IsValidWindowClassName(const wchar_t* name) {
    return name && name[0] != L'\0';
}

std::wstring FormatRegionText() {
    if (!g_searchRegion.defined) return L"当前: 全屏";
    wchar_t buffer[128];
    swprintf_s(buffer, L"当前: %d,%d  宽度=%d  高度=%d", g_searchRegion.left, g_searchRegion.top, g_searchRegion.width, g_searchRegion.height);
    return buffer;
}

// ---- 设置窗口 ----
HWND g_labelRegion = nullptr;

void CreateSettingsWindow() {
    if (g_settingsWnd && IsWindow(g_settingsWnd)) {
        BringWindowToTop(g_settingsWnd);
        ShowWindow(g_settingsWnd, SW_SHOW);
        return;
    }

    g_settingsWnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                    SETTINGS_WINDOW_CLASS,
                                    L"程序设置",
                                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    420, 320,
                                    g_mainWnd, NULL, g_hInstance, NULL);
    if (!g_settingsWnd) return;

    int y = 20;
    int labelW = 80;
    int ctrlX = 100;

    CreateWindowW(L"STATIC", L"缩放 (%)", WS_CHILD | WS_VISIBLE,
                  20, y, labelW, 20, g_settingsWnd, NULL, g_hInstance, NULL);
    CreateWindowW(L"EDIT",
                  std::to_wstring(g_zoomPercent).c_str(),
                  WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                  ctrlX, y, 60, 22, g_settingsWnd,
                  reinterpret_cast<HMENU>(201), g_hInstance, NULL);
    CreateWindowW(L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  ctrlX + 70, y, 50, 22, g_settingsWnd,
                  reinterpret_cast<HMENU>(301), g_hInstance, NULL);
    y += 35;

    CreateWindowW(L"STATIC", L"声音类型", WS_CHILD | WS_VISIBLE,
                  20, y, labelW, 20, g_settingsWnd, NULL, g_hInstance, NULL);
    HWND hSoundCombo = CreateWindowW(L"COMBOBOX", NULL,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                                     ctrlX, y, 180, 100, g_settingsWnd,
                                     reinterpret_cast<HMENU>(202), g_hInstance, NULL);
    SendMessageW(hSoundCombo, CB_ADDSTRING, 0, (LPARAM)L"默认 (咚)");
    SendMessageW(hSoundCombo, CB_ADDSTRING, 0, (LPARAM)L"感叹号 (!)");
    SendMessageW(hSoundCombo, CB_ADDSTRING, 0, (LPARAM)L"问号 (?)");
    SendMessageW(hSoundCombo, CB_ADDSTRING, 0, (LPARAM)L"星号 (*)");
    SendMessageW(hSoundCombo, CB_ADDSTRING, 0, (LPARAM)L"停止 (⛔)");
    SendMessageW(hSoundCombo, CB_SETCURSEL, g_soundType, 0);
    y += 35;

    CreateWindowW(L"STATIC", L"匹配阈值", WS_CHILD | WS_VISIBLE,
                  20, y, labelW, 20, g_settingsWnd, NULL, g_hInstance, NULL);
    CreateWindowW(L"EDIT",
                  std::to_wstring(g_matchThreshold).c_str(),
                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                  ctrlX, y, 60, 22, g_settingsWnd,
                  reinterpret_cast<HMENU>(203), g_hInstance, NULL);
    y += 35;

    CreateWindowW(L"STATIC", L"检测间隔 (ms)", WS_CHILD | WS_VISIBLE,
                  20, y, labelW, 20, g_settingsWnd, NULL, g_hInstance, NULL);
    CreateWindowW(L"EDIT",
                  std::to_wstring(g_checkIntervalMs).c_str(),
                  WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                  ctrlX, y, 60, 22, g_settingsWnd,
                  reinterpret_cast<HMENU>(204), g_hInstance, NULL);
    y += 45;

    CreateWindowW(L"BUTTON", L"框选区域", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  40, y, 120, 28, g_settingsWnd,
                  reinterpret_cast<HMENU>(101), g_hInstance, NULL);
    CreateWindowW(L"BUTTON", L"全屏识别", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  180, y, 120, 28, g_settingsWnd,
                  reinterpret_cast<HMENU>(102), g_hInstance, NULL);
    y += 40;
    CreateWindowW(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  150, y, 80, 28, g_settingsWnd,
                  reinterpret_cast<HMENU>(103), g_hInstance, NULL);

    ShowWindow(g_settingsWnd, SW_SHOW);
}

void UpdateSettingsLabel() {
    if (g_labelRegion && IsWindow(g_labelRegion))
        SetWindowTextW(g_labelRegion, FormatRegionText().c_str());
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        switch (id) {
        case 101:
            ShowWindow(hwnd, SW_HIDE);
            StartRegionSelection(hwnd);
            break;
        case 102:
            g_searchRegion.defined = false;
            SaveRegionToConfig();
            UpdateSettingsLabel();
            break;
        case 103:
            DestroyWindow(hwnd);
            g_settingsWnd = NULL;
            break;
        case 301: {
            HWND hEdit = GetDlgItem(hwnd, 201);
            if (hEdit) {
                wchar_t buf[16];
                GetWindowTextW(hEdit, buf, 16);
                int val = _wtoi(buf);
                if (val >= 50 && val <= 200) {
                    g_zoomPercent = val;
                    SaveConfigInt(L"Settings", L"Zoom", g_zoomPercent);
                } else {
                    MessageBoxW(hwnd, L"请输入 50~200 之间的整数", L"提示", MB_OK);
                }
            }
            break;
        }
        default: break;
        }
        return 0;
    }
    case WM_CLOSE: {
        HWND hCombo = GetDlgItem(hwnd, 202);
        if (hCombo) {
            int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                g_soundType = sel;
                SaveConfigInt(L"Settings", L"SoundType", g_soundType);
            }
        }
        HWND hThreshold = GetDlgItem(hwnd, 203);
        if (hThreshold) {
            wchar_t buf[32];
            GetWindowTextW(hThreshold, buf, 32);
            double val = _wtof(buf);
            if (val >= 0.1 && val <= 1.0) {
                g_matchThreshold = val;
                SaveConfigInt(L"Settings", L"Threshold", (int)(g_matchThreshold * 1000));
            }
        }
        HWND hInterval = GetDlgItem(hwnd, 204);
        if (hInterval) {
            wchar_t buf[32];
            GetWindowTextW(hInterval, buf, 32);
            int val = _wtoi(buf);
            if (val >= 100 && val <= 5000) {
                g_checkIntervalMs = val;
                SaveConfigInt(L"Settings", L"Interval", g_checkIntervalMs);
            }
        }
        DestroyWindow(hwnd);
        g_settingsWnd = NULL;
        return 0;
    }
    case WM_DESTROY:
        g_settingsWnd = NULL;
        return 0;
    }
    return DefWindowProc(hwnd, message, wParam, lParam);
}

// ---- 图片加载与持久化 ----
void LoadTriggerImages() {
    std::wstring appPath = StringFromCurrentDirectory();

    // 默认图片
    std::vector<std::wstring> defaultNames = {L"trigger_1.png", L"trigger_2.png"};
    for (auto& name : defaultNames) {
        std::filesystem::path imagePath = std::filesystem::path(appPath) / name;
        if (!std::filesystem::exists(imagePath)) {
            std::wcout << L"[WARN] trigger image not found: " << imagePath.wstring() << L"\n";
            continue;
        }
        cv::Mat img = cv::imread(imagePath.string(), cv::IMREAD_COLOR);
        if (img.empty()) {
            std::wcout << L"[WARN] failed to load trigger image: " << imagePath.wstring() << L"\n";
            continue;
        }
        g_triggerImages.emplace_back(imagePath.wstring(), img);
        g_triggerState[imagePath.wstring()] = false;
        std::wcout << L"[INFO] loaded default image: " << imagePath.wstring() << L"\n";
    }

    // 自定义图片
    g_customImagePaths.clear();
    for (int i = 1; i <= 10; ++i) {
        wchar_t key[16];
        swprintf_s(key, L"Image%d", i);
        wchar_t path[MAX_PATH];
        GetPrivateProfileStringW(L"CustomImages", key, L"", path, MAX_PATH, g_configPath.c_str());
        if (path[0] == L'\0') break;

        if (!std::filesystem::exists(path)) {
            std::wcout << L"[WARN] custom image not found: " << path << L", skipping\n";
            continue;
        }
        bool exists = false;
        for (auto& pair : g_triggerImages) {
            if (pair.first == path) { exists = true; break; }
        }
        if (exists) continue;

        cv::Mat img = cv::imread(std::string(path, path + wcslen(path)), cv::IMREAD_COLOR);
        if (img.empty()) {
            std::wcout << L"[WARN] failed to load custom image: " << path << L"\n";
            continue;
        }
        g_triggerImages.emplace_back(path, img);
        g_triggerState[path] = false;
        g_customImagePaths.push_back(path);
        std::wcout << L"[INFO] loaded custom image: " << path << L"\n";
    }

    if (g_triggerImages.empty()) {
        std::wcout << L"[ERROR] no trigger images loaded, please place trigger_1.png and trigger_2.png in the executable directory.\n";
    }
}

void SaveCustomImages() {
    WritePrivateProfileStringW(L"CustomImages", NULL, NULL, g_configPath.c_str());
    for (size_t i = 0; i < g_customImagePaths.size(); ++i) {
        wchar_t key[16];
        swprintf_s(key, L"Image%d", (int)(i + 1));
        WritePrivateProfileStringW(L"CustomImages", key, g_customImagePaths[i].c_str(), g_configPath.c_str());
    }
}

void AddCustomTriggerImage() {
    if (g_triggerImages.size() >= 10) {
        MessageBoxW(NULL, L"已达到最大图片数量（10张）", L"提示", MB_OK);
        return;
    }

    OPENFILENAMEW ofn = {};
    wchar_t filePath[MAX_PATH] = { 0 };
    wchar_t filter[] = L"图片文件\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tiff\0所有文件\0*.*\0";

    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"选择识别图片";

    if (!GetOpenFileNameW(&ofn)) return;

    // ----- 修正点：正确转换路径 -----
    int len = WideCharToMultiByte(CP_ACP, 0, filePath, -1, NULL, 0, NULL, NULL);
    std::string narrowPath;
    if (len > 0) {
        narrowPath.resize(len - 1);
        WideCharToMultiByte(CP_ACP, 0, filePath, -1, &narrowPath[0], len, NULL, NULL);
    }
    cv::Mat img = cv::imread(narrowPath, cv::IMREAD_COLOR);
    // --------------------------------

    if (img.empty()) {
        MessageBoxW(NULL, L"无法加载图片，请检查文件格式", L"错误", MB_OK);
        return;
    }

    for (auto& pair : g_triggerImages) {
        if (pair.first == filePath) {
            MessageBoxW(NULL, L"该图片已添加", L"提示", MB_OK);
            return;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        g_triggerImages.emplace_back(filePath, img);
        g_triggerState[filePath] = false;
        g_customImagePaths.push_back(filePath);
    }

    SaveCustomImages();
    std::wcout << L"[INFO] 添加自定义图片: " << filePath << L"\n";
    MessageBoxW(NULL, L"图片添加成功！", L"成功", MB_OK);
}

// ---- 托盘与菜单 ----
void ShowContextMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU mainMenu = CreatePopupMenu();
    HMENU sizeMenu = CreatePopupMenu();

    for (int zoom = 10; zoom <= 150; zoom += 10) {
        wchar_t buf[16];
        swprintf_s(buf, L"%d%%", zoom);
        AppendMenuW(sizeMenu, MF_STRING, 400 + zoom / 10, buf);
    }
    AppendMenuW(mainMenu, MF_POPUP, (UINT_PTR)sizeMenu, L"窗口大小");

    AppendMenuW(mainMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mainMenu, MF_STRING, 207, L"添加识别图片");

    AppendMenuW(mainMenu, MF_STRING, 201, L"显示所有");
    AppendMenuW(mainMenu, MF_STRING, 202, L"隐藏所有");
    AppendMenuW(mainMenu, MF_STRING, 203, L"重置所有");
    AppendMenuW(mainMenu, MF_STRING, 206, L"新建计时器");
    AppendMenuW(mainMenu, MF_STRING, 204, L"设置识别区域");
    AppendMenuW(mainMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(mainMenu, MF_STRING, 205, L"退出");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(mainMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(mainMenu);
}

void ProcessTrayMenuCommand(int command) {
    if (command >= 401 && command <= 415) {
        int zoom = (command - 400) * 10;
        g_zoomPercent = zoom;
        SaveConfigInt(L"Settings", L"Zoom", g_zoomPercent);
        return;
    }

    switch (command) {
    case 201: ShowAllTimerWindows(); break;
    case 202: HideAllTimerWindows(); break;
    case 203: ResetAllTimerWindows(); break;
    case 206: CreateTimerWindow(); break;
    case 204: CreateSettingsWindow(); break;
    case 205: PostQuitMessage(0); break;
    case 207: AddCustomTriggerImage(); break;
    default: break;
    }
}

void CreateTrayIcon() {
    EnsureTrayIcon();
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_trayIcon;
    wcscpy_s(nid.szTip, L"多计时器");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

void RegisterHotkeys(HWND hwnd) {
    RegisterHotKey(hwnd, HOTKEY_NEW_TIMER, 0, VK_F2);
    RegisterHotKey(hwnd, HOTKEY_RESET_ALL, 0, VK_F3);
    RegisterHotKey(hwnd, HOTKEY_TOGGLE_ALL, 0, VK_F4);
}

void UnregisterHotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, HOTKEY_NEW_TIMER);
    UnregisterHotKey(hwnd, HOTKEY_RESET_ALL);
    UnregisterHotKey(hwnd, HOTKEY_TOGGLE_ALL);
}

void DrawMainWindow(HDC hdc) {
    RECT rect;
    GetClientRect(g_mainWnd, &rect);
    FillRect(hdc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: return 0;
    case WM_COMMAND: ProcessTrayMenuCommand(LOWORD(wParam)); return 0;
    case WM_APP_TRAY:
        if (LOWORD(lParam) == WM_RBUTTONUP) ShowContextMenu(hwnd);
        else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) ToggleAllTimerWindows();
        return 0;
    case WM_HOTKEY:
        switch (wParam) {
        case HOTKEY_NEW_TIMER: CreateTimerWindow(); break;
        case HOTKEY_RESET_ALL: ResetAllTimerWindows(); break;
        case HOTKEY_TOGGLE_ALL: ToggleAllTimerWindows(); break;
        }
        return 0;
    case WM_USER_CREATE_TIMER:
        CreateTimerWindow();
        return 0;
    case WM_TIMER:
        if (wParam == 1) {
            static auto lastTime = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> delta = now - lastTime;
            lastTime = now;
            UpdateAllTimerWindows(delta.count());
        }
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon();
        UnregisterHotkeys(hwnd);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

// ---- 图像识别循环 ----
void ImageDetectionLoop() {
    while (g_running.load()) {
        if (g_triggerImages.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(g_checkIntervalMs));
            continue;
        }
        cv::Mat screen = CaptureScreenRegion();
        if (screen.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(g_checkIntervalMs));
            continue;
        }
        bool seenChange = false;
        for (auto& [path, image] : g_triggerImages) {
            bool found = false;
            cv::Point location;
            std::vector<double> thresholds = {g_matchThreshold,
                                              g_matchThreshold - 0.05,
                                              g_matchThreshold - 0.10};
            for (double t : thresholds) {
                if (FindTemplate(screen, image, location, t)) { found = true; break; }
            }
            std::wstring key = path;
            if (found) {
                int gridX = (location.x / GRID_SIZE) * GRID_SIZE;
                int gridY = (location.y / GRID_SIZE) * GRID_SIZE;
                key += L"|" + std::to_wstring(gridX) + L"|" + std::to_wstring(gridY);
            }
            bool previous = g_triggerState[path];
            g_triggerState[path] = found;
            if (found && !previous) {
                if (g_triggeredKeys.insert(key).second) {
                    PostMessage(g_mainWnd, WM_USER_CREATE_TIMER, 0, 0);
                }
                std::wcout << L"[INFO] matched " << path << L" at " << location.x << L"," << location.y << L"\n";
                seenChange = true;
            }
        }
        if (!g_triggerInitialized) g_triggerInitialized = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(g_checkIntervalMs));
    }
}

void CreateBaseIcon() {
    std::filesystem::path imagePath = std::filesystem::path(StringFromCurrentDirectory()) / L"paopao.png";
    if (std::filesystem::exists(imagePath)) {
        g_baseIcon = Gdiplus::Bitmap::FromFile(imagePath.c_str());
        if (g_baseIcon && g_baseIcon->GetLastStatus() != Gdiplus::Ok) {
            delete g_baseIcon;
            g_baseIcon = nullptr;
        }
    }
    if (!g_baseIcon) {
        g_baseIcon = new Gdiplus::Bitmap(96, 96, PixelFormat32bppARGB);
        Gdiplus::Graphics graphics(g_baseIcon);
        graphics.Clear(Gdiplus::Color(0,0,0,0));
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 200, 200, 200));
        graphics.FillEllipse(&brush, 0, 0, 96, 96);
    }
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    g_hInstance = hInstance;
    g_screenWidth = GetSystemMetrics(SM_CXSCREEN);
    g_screenHeight = GetSystemMetrics(SM_CYSCREEN);
    g_configPath = StringFromCurrentDirectory() + L"\\CoolDownTimer.ini";

    DWORD pid = GetCurrentProcessId();
    DWORD tick = GetTickCount();
    wsprintfW(TIMER_WINDOW_CLASS, L"TimerClass_%d_%d", pid, tick);
    wsprintfW(MAIN_WINDOW_CLASS, L"MainClass_%d_%d", pid, tick);
    wsprintfW(REGION_WINDOW_CLASS, L"RegionClass_%d_%d", pid, tick);
    wsprintfW(SETTINGS_WINDOW_CLASS, L"SettingsClass_%d_%d", pid, tick);

    g_zoomPercent = LoadConfigInt(L"Settings", L"Zoom", 100);
    g_soundType = LoadConfigInt(L"Settings", L"SoundType", 0);
    int thresholdInt = LoadConfigInt(L"Settings", L"Threshold", 650);
    g_matchThreshold = thresholdInt / 1000.0;
    g_checkIntervalMs = LoadConfigInt(L"Settings", L"Interval", 300);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    CreateBaseIcon();
    LoadRegionFromConfig();
    LoadTriggerImages();

    // 注册 Timer 窗口类
    WNDCLASSEXW timerClass = {};
    timerClass.cbSize = sizeof(WNDCLASSEXW);
    timerClass.style = CS_HREDRAW | CS_VREDRAW;
    timerClass.lpfnWndProc = TimerWndProc;
    timerClass.hInstance = hInstance;
    timerClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    timerClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    timerClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    timerClass.lpszClassName = TIMER_WINDOW_CLASS;
    if (!RegisterClassExW(&timerClass)) {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"注册 Timer 类失败！错误码: %d", err);
        MessageBoxW(NULL, buf, L"错误", MB_OK);
        return 1;
    }

    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = MainWndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.lpszClassName = MAIN_WINDOW_CLASS;
    RegisterClassExW(&wcex);

    WNDCLASSEXW regionClass = {};
    regionClass.cbSize = sizeof(WNDCLASSEXW);
    regionClass.lpfnWndProc = RegionWndProc;
    regionClass.hInstance = hInstance;
    regionClass.hCursor = LoadCursor(NULL, IDC_CROSS);
    regionClass.lpszClassName = REGION_WINDOW_CLASS;
    RegisterClassExW(&regionClass);

    WNDCLASSEXW settingsClass = {};
    settingsClass.cbSize = sizeof(WNDCLASSEXW);
    settingsClass.lpfnWndProc = SettingsWndProc;
    settingsClass.hInstance = hInstance;
    settingsClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    settingsClass.lpszClassName = SETTINGS_WINDOW_CLASS;
    RegisterClassExW(&settingsClass);

    g_mainWnd = CreateWindowExW(0, MAIN_WINDOW_CLASS, L"CoolDownTimerHiddenWindow",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                300, 200, NULL, NULL, hInstance, NULL);
    if (!g_mainWnd) return 0;

    CreateTrayIcon();
    RegisterHotkeys(g_mainWnd);
    SetTimer(g_mainWnd, 1, GLOBAL_UPDATE_INTERVAL_MS, NULL);
    ShowWindow(g_mainWnd, SW_HIDE);

    std::thread detector(ImageDetectionLoop);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 退出时确保删除托盘图标
    RemoveTrayIcon();

    g_running.store(false);
    if (detector.joinable()) detector.join();

    if (g_baseIcon) { delete g_baseIcon; g_baseIcon = nullptr; }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}