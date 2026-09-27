#include <Windows.h>
#include <CommCtrl.h>
#include <dwmapi.h>
#include <Uxtheme.h>
#include <windowsx.h>

#include "DeviceWorker.h"
#include "RadxaSvcPublic.h"
#include "../resource.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace
{
constexpr wchar_t kWindowClass[] = L"RadxaControlCenterWindow";
constexpr wchar_t kSettingsKey[] = L"Software\\Radxa\\RadxaControlCenter";
constexpr wchar_t kLanguageValue[] = L"Language";
constexpr LANGID kEnglish = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
constexpr LANGID kChinese = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
constexpr UINT WM_APP_WORKER_STOPPED = WM_APP + 2;
constexpr UINT WM_APP_FAN_SELECTION_COMMITTED = WM_APP + 3;

constexpr COLORREF kBackground = RGB(246, 248, 251);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(224, 228, 235);
constexpr COLORREF kText = RGB(31, 37, 48);
constexpr COLORREF kMuted = RGB(102, 112, 128);
constexpr COLORREF kAccent = RGB(197, 40, 80);
constexpr COLORREF kSuccess = RGB(25, 135, 84);
constexpr COLORREF kWarning = RGB(185, 112, 20);
constexpr COLORREF kDanger = RGB(190, 53, 53);

constexpr COLORREF kCurveGrid = RGB(228, 231, 236);
constexpr COLORREF kCurveAxis = RGB(180, 186, 196);

constexpr std::array<double, 6> kCurveTemperatures = {50.0, 60.0, 70.0, 80.0, 85.0, 90.0};
constexpr std::array<int, 6> kDefaultCurvePercents = {25, 40, 55, 70, 85, 100};

constexpr std::array<const wchar_t*, 6> kCurveValueNames = {
    L"FanCurve50", L"FanCurve60", L"FanCurve70",
    L"FanCurve80", L"FanCurve85", L"FanCurve90",
};

constexpr std::array<int, 6> kCurveEditIds = {
    IDC_FAN_CURVE_EDIT_1, IDC_FAN_CURVE_EDIT_2, IDC_FAN_CURVE_EDIT_3,
    IDC_FAN_CURVE_EDIT_4, IDC_FAN_CURVE_EDIT_5, IDC_FAN_CURVE_EDIT_6};

constexpr std::array<int, 6> kCurveLabelIds = {
    IDC_FAN_CURVE_LABEL_1, IDC_FAN_CURVE_LABEL_2, IDC_FAN_CURVE_LABEL_3,
    IDC_FAN_CURVE_LABEL_4, IDC_FAN_CURVE_LABEL_5, IDC_FAN_CURVE_LABEL_6};

bool IsChineseSystemLanguage()
{
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

LANGID LoadLanguagePreference()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            kLanguageValue,
            RRF_RT_REG_DWORD,
            nullptr,
            &value,
            &size) == ERROR_SUCCESS &&
        (value == kEnglish || value == kChinese))
    {
        return static_cast<LANGID>(value);
    }
    return IsChineseSystemLanguage() ? kChinese : kEnglish;
}

void SaveLanguagePreference(LANGID language)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            0,
            nullptr,
            0,
            KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr) == ERROR_SUCCESS)
    {
        const DWORD value = language;
        RegSetValueExW(
            key,
            kLanguageValue,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));
        RegCloseKey(key);
    }
}

std::array<int, 6> LoadCustomCurvePercents()
{
    std::array<int, 6> percents = kDefaultCurvePercents;
    for (int i = 0; i < 6; ++i)
    {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegGetValueW(
                HKEY_CURRENT_USER, kSettingsKey, kCurveValueNames[i],
                RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
            value <= 100)
        {
            percents[i] = static_cast<int>(value);
        }
    }
    return percents;
}

void SaveCustomCurvePercents(const std::array<int, 6>& percents)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        for (int i = 0; i < 6; ++i)
        {
            const DWORD value = static_cast<DWORD>(
                std::clamp(percents[i], 0, 100));
            RegSetValueExW(
                key, kCurveValueNames[i], 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&value), sizeof(value));
        }
        RegCloseKey(key);
    }
}

std::wstring LoadLocalizedString(HINSTANCE instance, UINT id, LANGID language)
{
    const UINT block = (id / 16) + 1;
    HRSRC resource = FindResourceExW(
        instance, RT_STRING, MAKEINTRESOURCEW(block), language);
    if (resource == nullptr && language != kEnglish)
    {
        resource = FindResourceExW(
            instance, RT_STRING, MAKEINTRESOURCEW(block), kEnglish);
    }
    if (resource == nullptr)
    {
        return {};
    }
    HGLOBAL data = LoadResource(instance, resource);
    if (data == nullptr)
    {
        return {};
    }
    const WORD* cursor = static_cast<const WORD*>(LockResource(data));
    if (cursor == nullptr)
    {
        return {};
    }
    for (UINT index = 0; index < (id & 15u); ++index)
    {
        cursor += 1 + *cursor;
    }
    return std::wstring(
        reinterpret_cast<const wchar_t*>(cursor + 1),
        static_cast<std::size_t>(*cursor));
}

bool IsCardStaticControl(int id)
{
    switch (id)
    {
    case IDC_STATUS_TITLE:
    case IDC_STATUS_DESC:
    case IDC_PROFILE_HEADING:
    case IDC_PROFILE_HINT:
    case IDC_FAN_HEADING:
    case IDC_FAN_PERCENT:
    case IDC_FAN_HINT:
    case IDC_FAN_CURRENT:
    case IDC_TEMP1_LABEL:
    case IDC_TEMP1_VALUE:
    case IDC_TEMP2_LABEL:
    case IDC_TEMP2_VALUE:
    case IDC_POWER1_LABEL:
    case IDC_POWER1_VALUE:
    case IDC_POWER1_DETAIL:
    case IDC_POWER2_LABEL:
    case IDC_POWER2_VALUE:
    case IDC_POWER2_DETAIL:
    case IDC_POWER_SYSTEM_LABEL:
    case IDC_POWER_SYSTEM_VALUE:
    case IDC_POWER_SYSTEM_DETAIL:
    case IDC_FAN_CURVE_HINT:
    case IDC_FAN_CURVE_LABEL_1:
    case IDC_FAN_CURVE_LABEL_2:
    case IDC_FAN_CURVE_LABEL_3:
    case IDC_FAN_CURVE_LABEL_4:
    case IDC_FAN_CURVE_LABEL_5:
    case IDC_FAN_CURVE_LABEL_6:
        return true;
    default:
        return false;
    }
}

void SetControlText(HWND control, const std::wstring& text)
{
    if (control == nullptr)
    {
        return;
    }
    const int length = GetWindowTextLengthW(control);
    std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, current.data(), length + 1);
    current.resize(static_cast<std::size_t>(length));
    if (current != text)
    {
        SetWindowTextW(control, text.c_str());
    }
}

void SetControlText(HWND parent, int id, const std::wstring& text)
{
    SetControlText(GetDlgItem(parent, id), text);
}

void SetControlVisible(HWND control, bool visible)
{
    if (control != nullptr && (IsWindowVisible(control) != FALSE) != visible)
    {
        ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
    }
}

void SetControlEnabled(HWND control, bool enabled)
{
    if (control != nullptr && (IsWindowEnabled(control) != FALSE) != enabled)
    {
        EnableWindow(control, enabled);
    }
}

int CALLBACK FindFontCallback(
    const LOGFONTW*,
    const TEXTMETRICW*,
    DWORD,
    LPARAM context)
{
    *reinterpret_cast<bool*>(context) = true;
    return 0;
}

bool IsFontInstalled(const wchar_t* faceName)
{
    HDC dc = GetDC(nullptr);
    if (dc == nullptr)
    {
        return false;
    }
    LOGFONTW font{};
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, faceName);
    bool found = false;
    EnumFontFamiliesExW(
        dc,
        &font,
        reinterpret_cast<FONTENUMPROCW>(FindFontCallback),
        reinterpret_cast<LPARAM>(&found),
        0);
    ReleaseDC(nullptr, dc);
    return found;
}

const wchar_t* PreferredFontFace()
{
    static const bool variableFontAvailable =
        IsFontInstalled(L"Segoe UI Variable Text");
    return variableFontAvailable ? L"Segoe UI Variable Text" : L"Segoe UI";
}
}

class Application final
{
public:
    int Run(HINSTANCE instance, int showCommand)
    {
        instance_ = instance;
        language_ = LoadLanguagePreference();
        customPercents_ = LoadCustomCurvePercents();

        WNDCLASSEXW windowClass{sizeof(windowClass)};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &Application::WindowProcedure;
        windowClass.hInstance = instance_;
        windowClass.hIcon = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 16, 16, 0));
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&windowClass) == 0)
        {
            return 1;
        }

        const UINT initialDpi = GetDpiForSystem();
        RECT windowRect{0, 0, MulDiv(940, initialDpi, 96), MulDiv(940, initialDpi, 96)};
        AdjustWindowRectExForDpi(
            &windowRect,
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            FALSE,
            WS_EX_APPWINDOW,
            initialDpi);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = static_cast<int>(workArea.left) +
            std::max(0, (static_cast<int>(workArea.right - workArea.left) - width) / 2);
        const int y = static_cast<int>(workArea.top) +
            std::max(0, (static_cast<int>(workArea.bottom - workArea.top) - height) / 2);

        window_ = CreateWindowExW(
            WS_EX_APPWINDOW,
            kWindowClass,
            Localize(IDS_APP_TITLE).c_str(),
            WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN,
            x, y, width, height,
            nullptr, nullptr, instance_, this);
        if (window_ == nullptr)
        {
            return 2;
        }

        ShowWindow(window_, showCommand);
        UpdateWindow(window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (shutdownThread_.joinable())
        {
            shutdownThread_.join();
        }
        if (worker_ != nullptr)
        {
            worker_->Stop();
        }
        DestroyFonts();
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Application* app = reinterpret_cast<Application*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            app = static_cast<Application*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app != nullptr ? app->HandleMessage(message, wParam, lParam) :
                                DefWindowProcW(window, message, wParam, lParam);
    }

    std::wstring Localize(UINT id) const
    {
        return LoadLocalizedString(instance_, id, language_);
    }

    int Scale(int value) const
    {
        return MulDiv(value, static_cast<int>(dpi_), 96);
    }

    HWND AddControl(
        LPCWSTR className, DWORD style, int id, DWORD extendedStyle = 0)
    {
        HWND control = CreateWindowExW(
            extendedStyle,
            className,
            L"",
            WS_CHILD | WS_VISIBLE | style,
            0, 0, 10, 10,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_,
            nullptr);
        if (control != nullptr)
        {
            SetWindowTheme(control, L"Explorer", nullptr);
        }
        return control;
    }

    void CreateControls()
    {
        AddControl(L"STATIC", SS_LEFT, IDC_TITLE);
        AddControl(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, IDC_LANGUAGE);
        AddControl(L"STATIC", SS_LEFT, IDC_STATUS_TITLE);
        AddControl(L"STATIC", SS_LEFT, IDC_STATUS_DESC);
        AddControl(L"BUTTON", BS_PUSHBUTTON | WS_TABSTOP, IDC_RETRY);

        AddControl(L"STATIC", SS_LEFT, IDC_PROFILE_HEADING);
        AddControl(L"BUTTON", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, IDC_PROFILE_QUIET);
        AddControl(L"BUTTON", BS_AUTORADIOBUTTON | WS_TABSTOP, IDC_PROFILE_PERFORMANCE);
        AddControl(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_PROFILE_APPLY);
        AddControl(L"STATIC", SS_LEFT, IDC_PROFILE_HINT);

        AddControl(L"STATIC", SS_LEFT, IDC_FAN_HEADING);
        AddControl(WC_COMBOBOXW, CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP, IDC_FAN_MODE);
        AddControl(L"STATIC", SS_RIGHT, IDC_FAN_CURRENT);
        AddControl(TRACKBAR_CLASSW, TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IDC_FAN_SLIDER);
        AddControl(L"STATIC", SS_RIGHT, IDC_FAN_PERCENT);
        AddControl(L"BUTTON", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_FAN_APPLY);
        AddControl(L"STATIC", SS_LEFT, IDC_FAN_HINT);

        // 自定义曲线的 6 组标签和编辑框
        for (int i = 0; i < 6; ++i)
        {
            AddControl(L"STATIC", SS_LEFT | SS_CENTERIMAGE, kCurveLabelIds[i]);
            AddControl(
                L"EDIT",
                ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                kCurveEditIds[i]);
        }

        AddControl(L"STATIC", SS_LEFT, IDC_FAN_CURVE_HEADING);
        AddControl(L"STATIC", SS_LEFT, IDC_FAN_CURVE_HINT);

        AddControl(L"STATIC", SS_LEFT, IDC_TEMP_HEADING);
        AddControl(L"STATIC", SS_LEFT, IDC_TEMP1_LABEL);
        AddControl(L"STATIC", SS_LEFT, IDC_TEMP1_VALUE);
        AddControl(L"STATIC", SS_LEFT, IDC_TEMP2_LABEL);
        AddControl(L"STATIC", SS_LEFT, IDC_TEMP2_VALUE);

        AddControl(L"STATIC", SS_LEFT, IDC_POWER_HEADING);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER1_LABEL);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER1_VALUE);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER1_DETAIL);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER2_LABEL);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER2_VALUE);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER2_DETAIL);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER_SYSTEM_LABEL);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER_SYSTEM_VALUE);
        AddControl(L"STATIC", SS_LEFT, IDC_POWER_SYSTEM_DETAIL);
        AddControl(L"STATIC", SS_LEFT, IDC_NOTICE);

        SendDlgItemMessageW(window_, IDC_FAN_SLIDER, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(window_, IDC_FAN_SLIDER, TBM_SETPAGESIZE, 0, 10);
        SendDlgItemMessageW(window_, IDC_FAN_SLIDER, TBM_SETPOS, TRUE, 15);

        // 初始化曲线编辑框
        for (int i = 0; i < 6; ++i)
        {
            wchar_t buffer[16]{};
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d", customPercents_[i]);
            SetDlgItemTextW(window_, kCurveEditIds[i], buffer);
        }

        backgroundBrush_ = CreateSolidBrush(kBackground);
        cardBrush_ = CreateSolidBrush(kCard);
        ApplyLanguage();
        UpdateDpi(GetDpiForWindow(window_));
        ApplySnapshot();

        constexpr int roundPreference = 2;
        DwmSetWindowAttribute(
            window_,
            static_cast<DWMWINDOWATTRIBUTE>(33),
            &roundPreference,
            sizeof(roundPreference));

        worker_ = std::make_unique<DeviceWorker>(window_);
        // 把注册表里保存的自定义曲线加载到 worker
        worker_->SetFanCurve(BuildCurveFromPercents(customPercents_));
        worker_->Start();
    }

    void DestroyFonts()
    {
        for (HFONT& font : fonts_)
        {
            if (font != nullptr) { DeleteObject(font); font = nullptr; }
        }
        if (backgroundBrush_ != nullptr) { DeleteObject(backgroundBrush_); backgroundBrush_ = nullptr; }
        if (cardBrush_ != nullptr) { DeleteObject(cardBrush_); cardBrush_ = nullptr; }
    }

    HFONT MakeFont(int pointSize, int weight)
    {
        return CreateFontW(
            -MulDiv(pointSize, static_cast<int>(dpi_), 72),
            0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            PreferredFontFace());
    }

    void AssignFont(int id, HFONT font)
    {
        SendDlgItemMessageW(window_, id, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    void UpdateDpi(UINT newDpi)
    {
        dpi_ = newDpi == 0 ? 96 : newDpi;
        for (HFONT& font : fonts_)
        {
            if (font != nullptr) DeleteObject(font);
        }
        fonts_[0] = MakeFont(22, FW_SEMIBOLD);
        fonts_[1] = MakeFont(13, FW_SEMIBOLD);
        fonts_[2] = MakeFont(10, FW_NORMAL);
        fonts_[3] = MakeFont(19, FW_SEMIBOLD);
        fonts_[4] = MakeFont(9, FW_NORMAL);

        AssignFont(IDC_TITLE, fonts_[0]);
        for (int id : {IDC_STATUS_TITLE, IDC_PROFILE_HEADING, IDC_FAN_HEADING,
                 IDC_TEMP_HEADING, IDC_POWER_HEADING, IDC_FAN_CURVE_HEADING})
        {
            AssignFont(id, fonts_[1]);
        }
        for (int id : {IDC_TEMP1_VALUE, IDC_TEMP2_VALUE, IDC_POWER1_VALUE,
                 IDC_POWER2_VALUE, IDC_POWER_SYSTEM_VALUE})
        {
            AssignFont(id, fonts_[3]);
        }
        for (int id : {IDC_PROFILE_HINT, IDC_FAN_HINT, IDC_POWER1_DETAIL,
                 IDC_POWER2_DETAIL, IDC_POWER_SYSTEM_DETAIL, IDC_NOTICE,
                 IDC_FAN_CURVE_HINT})
        {
            AssignFont(id, fonts_[4]);
        }
        for (int id : {IDC_LANGUAGE, IDC_STATUS_DESC, IDC_RETRY,
                 IDC_PROFILE_QUIET, IDC_PROFILE_PERFORMANCE, IDC_PROFILE_APPLY,
                 IDC_FAN_MODE, IDC_FAN_PERCENT, IDC_FAN_APPLY, IDC_FAN_CURRENT,
                 IDC_TEMP1_LABEL, IDC_TEMP2_LABEL, IDC_POWER1_LABEL,
                 IDC_POWER2_LABEL, IDC_POWER_SYSTEM_LABEL})
        {
            AssignFont(id, fonts_[2]);
        }
        for (int id : kCurveEditIds) AssignFont(id, fonts_[2]);
        for (int id : kCurveLabelIds) AssignFont(id, fonts_[2]);

        LayoutControls();
    }

    void ApplyLanguage()
    {
        SetWindowTextW(window_, Localize(IDS_APP_TITLE).c_str());
        SetControlText(window_, IDC_TITLE, Localize(IDS_APP_TITLE));
        SetControlText(window_, IDC_LANGUAGE, Localize(IDS_LANGUAGE));
        SetControlText(window_, IDC_RETRY, Localize(IDS_RETRY));
        SetControlText(window_, IDC_PROFILE_HEADING, Localize(IDS_PERFORMANCE_MODE));
        SetControlText(window_, IDC_PROFILE_QUIET, Localize(IDS_QUIET));
        SetControlText(window_, IDC_PROFILE_PERFORMANCE, Localize(IDS_PERFORMANCE));
        SetControlText(window_, IDC_PROFILE_APPLY, Localize(IDS_APPLY));
        SetControlText(window_, IDC_FAN_HEADING, Localize(IDS_COOLING));
        SetControlText(window_, IDC_FAN_APPLY, Localize(IDS_APPLY));
        SetControlText(window_, IDC_FAN_CURVE_HEADING, Localize(IDS_FAN_CURVE));
        SetControlText(window_, IDC_FAN_CURVE_HINT, Localize(IDS_FAN_CURVE_HINT));
        SetControlText(window_, IDC_TEMP_HEADING, Localize(IDS_TEMPERATURES));
        SetControlText(window_, IDC_POWER_HEADING, Localize(IDS_POWER));
        SetControlText(window_, IDC_TEMP1_LABEL, Localize(IDS_CPU));
        SetControlText(window_, IDC_TEMP2_LABEL, Localize(IDS_GPU));
        SetControlText(window_, IDC_POWER1_LABEL, Localize(IDS_USB_C_1));
        SetControlText(window_, IDC_POWER2_LABEL, Localize(IDS_USB_C_2));
        SetControlText(window_, IDC_POWER_SYSTEM_LABEL, Localize(IDS_SYSTEM_TOTAL));

        // 更新曲线标签（50°C、60°C...）
        for (int i = 0; i < 6; ++i)
        {
            wchar_t buffer[16]{};
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
                         L"%d°C", static_cast<int>(kCurveTemperatures[i]));
            SetControlText(window_, kCurveLabelIds[i], buffer);
        }

        HWND combo = GetDlgItem(window_, IDC_FAN_MODE);
        const int previous = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (UINT id : {IDS_AUTOMATIC, IDS_FULL_SPEED, IDS_MANUAL})
        {
            const std::wstring value = Localize(id);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        SendMessageW(combo, CB_SETCURSEL, previous >= 0 ? previous : 0, 0);
        ApplySnapshot();
        LayoutControls();
    }

    void ShowLanguageMenu()
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (language_ == kChinese ? MF_CHECKED : 0),
                    IDM_LANGUAGE_CHINESE, Localize(IDS_LANGUAGE_CHINESE).c_str());
        AppendMenuW(menu, MF_STRING | (language_ == kEnglish ? MF_CHECKED : 0),
                    IDM_LANGUAGE_ENGLISH, Localize(IDS_LANGUAGE_ENGLISH).c_str());
        RECT rect{};
        GetWindowRect(GetDlgItem(window_, IDC_LANGUAGE), &rect);
        TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN,
                       rect.right, rect.bottom, 0, window_, nullptr);
        DestroyMenu(menu);
    }

    std::wstring FormatOne(UINT id, double value) const
    {
        const std::wstring format = Localize(id);
        wchar_t buffer[96]{};
        _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, format.c_str(), value);
        return buffer;
    }

    std::wstring FormatTwo(UINT id, double first, double second) const
    {
        const std::wstring format = Localize(id);
        wchar_t buffer[128]{};
        _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, format.c_str(), first, second);
        return buffer;
    }

    std::wstring FormatPercent(int value) const
    {
        const std::wstring format = Localize(IDS_PERCENT_FORMAT);
        wchar_t buffer[32]{};
        _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, format.c_str(), value);
        return buffer;
    }

    std::wstring FormatCurrent(int value) const
    {
        const std::wstring format = Localize(IDS_FAN_CURRENT_FORMAT);
        wchar_t buffer[32]{};
        _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, format.c_str(), value);
        return buffer;
    }

    static int FanModeToCombo(std::uint32_t mode)
    {
        if (mode == RADXA_SVC_FAN_CONTROL_FULL_SPEED) return 1;
        if (mode == RADXA_SVC_FAN_CONTROL_MANUAL) return 2;
        return 0;
    }

    static std::uint32_t ComboToFanMode(int selection)
    {
        if (selection == 1) return RADXA_SVC_FAN_CONTROL_FULL_SPEED;
        if (selection == 2) return RADXA_SVC_FAN_CONTROL_MANUAL;
        return RADXA_SVC_FAN_CONTROL_AUTO;
    }

    // 读取编辑框的 6 个百分比
    std::array<int, 6> ReadCurveEditors() const
    {
        std::array<int, 6> percents = kDefaultCurvePercents;
        for (int i = 0; i < 6; ++i)
        {
            wchar_t buffer[16]{};
            GetDlgItemTextW(window_, kCurveEditIds[i], buffer, _countof(buffer));
            int value = _wtoi(buffer);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            percents[i] = value;
        }
        return percents;
    }

    std::array<FanCurvePoint, 6> BuildCurveFromPercents(
        const std::array<int, 6>& percents) const
    {
        std::array<FanCurvePoint, 6> curve{};
        for (int i = 0; i < 6; ++i)
        {
            curve[i].Temperature = kCurveTemperatures[i];
            curve[i].Pwm = DeviceWorker::PercentToPwm(percents[i]);
        }
        return curve;
    }

    // 当前曲线图应该显示的百分比
    std::array<int, 6> GetDisplayedPercents() const
    {
        if (window_ == nullptr) return kDefaultCurvePercents;
        const int selection = static_cast<int>(
            SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_GETCURSEL, 0, 0));
        if (selection == 2)  // 自定义
        {
            return ReadCurveEditors();
        }
        // 自动 / 全速 → 显示默认曲线
        std::array<int, 6> percents{};
        for (int i = 0; i < 6; ++i)
        {
            percents[i] = DeviceWorker::PwmToPercent(DeviceWorker::kDefaultFanCurve[i].Pwm);
        }
        return percents;
    }

    void ApplySnapshot()
    {
        if (window_ == nullptr) return;
        if (worker_ != nullptr)
        {
            snapshot_ = worker_->GetSnapshot();
        }

        const COLORREF previousStatusColor = statusColor_;
        UINT statusTitle = IDS_STATUS_CONNECTING;
        UINT statusDescription = IDS_STATUS_CONNECTING_DESC;
        statusColor_ = kWarning;
        switch (snapshot_.Connection)
        {
        case ConnectionState::Ready:
            statusTitle = IDS_STATUS_READY;
            statusDescription = IDS_STATUS_READY_DESC;
            statusColor_ = kSuccess;
            break;
        case ConnectionState::DriverMissing:
            statusTitle = IDS_STATUS_MISSING;
            statusDescription = IDS_STATUS_MISSING_DESC;
            statusColor_ = kDanger;
            break;
        case ConnectionState::UpdateRequired:
            statusTitle = IDS_STATUS_UPDATE;
            statusDescription = IDS_STATUS_UPDATE_DESC;
            statusColor_ = kDanger;
            break;
        case ConnectionState::Unavailable:
            statusTitle = IDS_STATUS_UNAVAILABLE;
            statusDescription = IDS_STATUS_UNAVAILABLE_DESC;
            statusColor_ = kDanger;
            break;
        case ConnectionState::Connecting:
            break;
        }
        SetControlText(window_, IDC_STATUS_TITLE, Localize(statusTitle));
        SetControlText(window_, IDC_STATUS_DESC, Localize(statusDescription));
        SetControlVisible(GetDlgItem(window_, IDC_RETRY),
                          snapshot_.Connection != ConnectionState::Ready);

        const bool ready = snapshot_.Connection == ConnectionState::Ready;
        const bool profileEnabled = ready && snapshot_.ProfileSupported && !snapshot_.Busy;
        for (int id : {IDC_PROFILE_QUIET, IDC_PROFILE_PERFORMANCE, IDC_PROFILE_APPLY})
        {
            SetControlEnabled(GetDlgItem(window_, id), profileEnabled);
        }
        SetControlText(window_, IDC_PROFILE_HINT,
            ready && !snapshot_.ProfileSupported ? Localize(IDS_NOT_AVAILABLE) :
                                                   Localize(IDS_PROFILE_HINT));
        if (!profileDirty_ && snapshot_.ProfileValid)
        {
            const int desired =
                snapshot_.EffectiveProfile == RADXA_SVC_PROFILE_QUIET ? IDC_PROFILE_QUIET :
                snapshot_.EffectiveProfile == RADXA_SVC_PROFILE_PERFORMANCE ? IDC_PROFILE_PERFORMANCE : 0;
            const bool quietChecked =
                IsDlgButtonChecked(window_, IDC_PROFILE_QUIET) == BST_CHECKED;
            const bool performanceChecked =
                IsDlgButtonChecked(window_, IDC_PROFILE_PERFORMANCE) == BST_CHECKED;
            if ((desired == IDC_PROFILE_QUIET && !quietChecked) ||
                (desired == IDC_PROFILE_PERFORMANCE && !performanceChecked) ||
                (desired == 0 && (quietChecked || performanceChecked)))
            {
                CheckDlgButton(window_, IDC_PROFILE_QUIET,
                    desired == IDC_PROFILE_QUIET ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(window_, IDC_PROFILE_PERFORMANCE,
                    desired == IDC_PROFILE_PERFORMANCE ? BST_CHECKED : BST_UNCHECKED);
            }
        }

        const bool fanEnabled = ready && snapshot_.FanSupported && !snapshot_.Busy;
        SetControlEnabled(GetDlgItem(window_, IDC_FAN_MODE), fanEnabled);
        SetControlEnabled(GetDlgItem(window_, IDC_FAN_APPLY), fanEnabled);
        SetControlText(window_, IDC_FAN_HINT,
            ready && !snapshot_.FanSupported ? Localize(IDS_NOT_AVAILABLE) :
                                               Localize(IDS_FAN_HINT));

        // 当前转速显示
        int currentPwm = static_cast<int>(snapshot_.CurrentFanPwm);
        if (currentPwm < 0) currentPwm = 0;
        if (currentPwm > static_cast<int>(RADXA_SVC_FAN_PWM_MAX))
            currentPwm = static_cast<int>(RADXA_SVC_FAN_PWM_MAX);
        const int currentPercent = DeviceWorker::PwmToPercent(
            static_cast<std::uint32_t>(currentPwm));
        SetControlText(window_, IDC_FAN_CURRENT, FormatCurrent(currentPercent));

        // 下拉框
        const bool fanComboIsOpen = fanComboDropped_ ||
            SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
        if (!fanDirty_ && !fanComboIsOpen && snapshot_.FanValid)
        {
            const int desiredMode = FanModeToCombo(snapshot_.FanMode);
            if (SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_GETCURSEL, 0, 0) != desiredMode)
            {
                SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_SETCURSEL, desiredMode, 0);
            }
        }

        // 模式切换时的可见性
        bool manualVisibilityChanged = false;
        if (!fanComboIsOpen)
        {
            const int fanSelection = static_cast<int>(
                SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_GETCURSEL, 0, 0));
            const bool showManual = (fanSelection == 2);  // 自定义
            manualVisibilityChanged = showManual != manualControlsVisible_;
            manualControlsVisible_ = showManual;

            // 旧的滑块和百分比：自定义模式下不再使用，隐藏
            SetControlVisible(GetDlgItem(window_, IDC_FAN_SLIDER), false);
            SetControlVisible(GetDlgItem(window_, IDC_FAN_PERCENT), false);

            // 曲线编辑框和标签：只在自定义模式显示
            for (int i = 0; i < 6; ++i)
            {
                SetControlVisible(GetDlgItem(window_, kCurveLabelIds[i]), showManual);
                SetControlVisible(GetDlgItem(window_, kCurveEditIds[i]), showManual);
            }
        }

        const std::wstring dash = L"—";
        SetControlText(window_, IDC_TEMP1_VALUE,
            snapshot_.Temperatures[0].Valid ?
                FormatOne(IDS_TEMPERATURE_FORMAT, snapshot_.Temperatures[0].Celsius) : dash);
        SetControlText(window_, IDC_TEMP2_VALUE,
            snapshot_.Temperatures[1].Valid ?
                FormatOne(IDS_TEMPERATURE_FORMAT, snapshot_.Temperatures[1].Celsius) : dash);
        UpdatePowerCard(IDC_POWER1_VALUE, IDC_POWER1_DETAIL, snapshot_.UsbPower[0]);
        UpdatePowerCard(IDC_POWER2_VALUE, IDC_POWER2_DETAIL, snapshot_.UsbPower[1]);
        UpdatePowerCard(IDC_POWER_SYSTEM_VALUE, IDC_POWER_SYSTEM_DETAIL, snapshot_.SystemPower);

        if (snapshot_.Notice == UserNotice::Applied)
        {
            noticeColor_ = kSuccess;
            SetControlText(window_, IDC_NOTICE, Localize(IDS_APPLIED));
        }
        else if (snapshot_.Notice == UserNotice::ActionFailed)
        {
            noticeColor_ = kDanger;
            SetControlText(window_, IDC_NOTICE, Localize(IDS_ACTION_FAILED));
        }
        else
        {
            SetControlText(window_, IDC_NOTICE, L"");
        }

        if (manualVisibilityChanged) LayoutControls();
        if (previousStatusColor != statusColor_)
            InvalidateRect(window_, &statusCard_, FALSE);
    }

    void UpdatePowerCard(int valueId, int detailId, const PowerValue& power)
    {
        const std::wstring dash = L"—";
        SetControlText(window_, valueId,
            power.PowerValid ? FormatOne(IDS_POWER_FORMAT, power.Watts) : dash);
        SetControlText(window_, detailId,
            power.VoltageValid && power.CurrentValid ?
                FormatTwo(IDS_ELECTRICAL_FORMAT, power.Volts, power.Amps) : L"");
    }

    void Move(int id, int x, int y, int width, int height)
    {
        MoveWindow(GetDlgItem(window_, id), x, y - scrollPosition_, width, height, TRUE);
    }

    void LayoutControls()
    {
        if (window_ == nullptr || GetDlgItem(window_, IDC_TITLE) == nullptr)
        {
            return;
        }
        RECT client{};
        GetClientRect(window_, &client);
        const int clientWidth = client.right;
        const int margin = Scale(24);
        const int gap = Scale(16);
        const int inner = Scale(20);
        const int available = std::max(Scale(300), clientWidth - margin * 2);
        const bool twoColumns = available >= Scale(760);
        int y = Scale(18);

        Move(IDC_TITLE, margin, y, available - Scale(150), Scale(38));
        Move(IDC_LANGUAGE, clientWidth - margin - Scale(128), y, Scale(128), Scale(34));
        y += Scale(52);

        statusCard_ = {margin, y - scrollPosition_, clientWidth - margin, y + Scale(88) - scrollPosition_};
        Move(IDC_STATUS_TITLE, margin + inner, y + Scale(14), available - Scale(170), Scale(26));
        Move(IDC_STATUS_DESC, margin + inner, y + Scale(43), available - Scale(170), Scale(24));
        Move(IDC_RETRY, clientWidth - margin - inner - Scale(112), y + Scale(24), Scale(112), Scale(36));
        y += Scale(88) + gap;

        // 根据当前模式决定散热卡片高度
        const bool customMode = manualControlsVisible_;
        const int panelHeight = customMode ? Scale(280) : Scale(188);
        const int panelWidth = twoColumns ? (available - gap) / 2 : available;
        profileCard_ = {margin, y - scrollPosition_, margin + panelWidth, y + panelHeight - scrollPosition_};
        LayoutProfileCard(margin, y, panelWidth, panelHeight, inner);

        int fanX = twoColumns ? margin + panelWidth + gap : margin;
        int fanY = twoColumns ? y : y + panelHeight + gap;
        fanCard_ = {fanX, fanY - scrollPosition_, fanX + panelWidth, fanY + panelHeight - scrollPosition_};
        LayoutFanCard(fanX, fanY, panelWidth, panelHeight, inner);
        y = (twoColumns ? y : fanY) + panelHeight + Scale(24);

        // 曲线图卡片
        Move(IDC_FAN_CURVE_HEADING, margin, y, available, Scale(28));
        y += Scale(36);
        const int curveHeight = Scale(260);
        curveCard_ = {margin, y - scrollPosition_, margin + available, y + curveHeight - scrollPosition_};
        Move(IDC_FAN_CURVE_HINT,
             margin + inner, y + curveHeight - Scale(34),
             available - inner * 2, Scale(24));
        y += curveHeight + Scale(24);

        Move(IDC_TEMP_HEADING, margin, y, available, Scale(28));
        y += Scale(36);
        const int tempWidth = (available - gap) / 2;
        for (int index = 0; index < 2; ++index)
        {
            const int x = margin + index * (tempWidth + gap);
            tempCards_[index] = {x, y - scrollPosition_, x + tempWidth, y + Scale(100) - scrollPosition_};
            const int labelId = index == 0 ? IDC_TEMP1_LABEL : IDC_TEMP2_LABEL;
            const int valueId = index == 0 ? IDC_TEMP1_VALUE : IDC_TEMP2_VALUE;
            Move(labelId, x + inner, y + Scale(15), tempWidth - inner * 2, Scale(22));
            Move(valueId, x + inner, y + Scale(43), tempWidth - inner * 2, Scale(38));
        }
        y += Scale(100) + Scale(24);

        Move(IDC_POWER_HEADING, margin, y, available, Scale(28));
        y += Scale(36);
        const int powerColumns = available >= Scale(700) ? 3 : 1;
        const int powerWidth = powerColumns == 3 ? (available - gap * 2) / 3 : available;
        const std::array<int, 3> labels{
            IDC_POWER1_LABEL, IDC_POWER2_LABEL, IDC_POWER_SYSTEM_LABEL};
        const std::array<int, 3> values{
            IDC_POWER1_VALUE, IDC_POWER2_VALUE, IDC_POWER_SYSTEM_VALUE};
        const std::array<int, 3> details{
            IDC_POWER1_DETAIL, IDC_POWER2_DETAIL, IDC_POWER_SYSTEM_DETAIL};
        for (int index = 0; index < 3; ++index)
        {
            const int x = margin + (index % powerColumns) * (powerWidth + gap);
            const int cardY = y + (index / powerColumns) * (Scale(116) + gap);
            powerCards_[index] = {x, cardY - scrollPosition_, x + powerWidth,
                                  cardY + Scale(116) - scrollPosition_};
            Move(labels[index], x + inner, cardY + Scale(13), powerWidth - inner * 2, Scale(22));
            Move(values[index], x + inner, cardY + Scale(39), powerWidth - inner * 2, Scale(36));
            Move(details[index], x + inner, cardY + Scale(82), powerWidth - inner * 2, Scale(20));
        }
        y += ((3 + powerColumns - 1) / powerColumns) * (Scale(116) + gap) - gap;
        y += Scale(16);
        Move(IDC_NOTICE, margin, y, available, Scale(24));
        y += Scale(40);
        contentHeight_ = y;

        SCROLLINFO scrollInfo{sizeof(scrollInfo)};
        scrollInfo.fMask = SIF_PAGE | SIF_RANGE | SIF_POS;
        scrollInfo.nMin = 0;
        scrollInfo.nMax = std::max(0, contentHeight_ - 1);
        scrollInfo.nPage = static_cast<UINT>(std::max(0L, client.bottom));
        scrollInfo.nPos = scrollPosition_;
        SetScrollInfo(window_, SB_VERT, &scrollInfo, TRUE);
        SCROLLINFO actual{sizeof(actual)};
        actual.fMask = SIF_POS;
        GetScrollInfo(window_, SB_VERT, &actual);
        if (actual.nPos != scrollPosition_)
        {
            scrollPosition_ = actual.nPos;
            LayoutControls();
        }
    }

    void LayoutProfileCard(int x, int y, int width, int, int inner)
    {
        Move(IDC_PROFILE_HEADING, x + inner, y + Scale(14), width - inner * 2, Scale(25));
        Move(IDC_PROFILE_QUIET, x + inner, y + Scale(51), Scale(112), Scale(28));
        Move(IDC_PROFILE_PERFORMANCE, x + inner + Scale(122), y + Scale(51), Scale(130), Scale(28));
        Move(IDC_PROFILE_APPLY, x + inner, y + Scale(92), Scale(104), Scale(34));
        Move(IDC_PROFILE_HINT, x + inner, y + Scale(139), width - inner * 2, Scale(36));
    }

    void LayoutFanCard(int x, int y, int width, int height, int inner)
    {
        Move(IDC_FAN_HEADING, x + inner, y + Scale(14), width - inner * 2, Scale(25));
        Move(IDC_FAN_MODE, x + inner, y + Scale(48), Scale(154), Scale(200));
        Move(IDC_FAN_CURRENT,
             x + inner + Scale(166), y + Scale(51),
             width - inner * 2 - Scale(166), Scale(24));

        // 曲线编辑区：2 行 3 列
        if (manualControlsVisible_)
        {
            const int editorTop = y + Scale(88);
            const int rowGap = Scale(34);
            const int cellWidth = (width - inner * 2 - Scale(2 * 12)) / 3;
            for (int i = 0; i < 6; ++i)
            {
                const int col = i % 3;
                const int row = i / 3;
                const int cellX = x + inner + col * (cellWidth + Scale(12));
                const int cellY = editorTop + row * rowGap;
                Move(kCurveLabelIds[i], cellX, cellY, Scale(50), Scale(24));
                Move(kCurveEditIds[i], cellX + Scale(52), cellY, Scale(60), Scale(24));
            }
        }

        // 应用按钮位置：自定义模式下移，避免和编辑区重叠
        const int applyY = manualControlsVisible_ ? (y + Scale(178)) : (y + Scale(92));
        Move(IDC_FAN_APPLY, x + inner, applyY, Scale(104), Scale(34));

        // 提示文字：自定义模式下移
        const int hintY = manualControlsVisible_ ? (y + Scale(228)) : (y + Scale(139));
        Move(IDC_FAN_HINT, x + inner, hintY, width - inner * 2, Scale(36));

        (void)height;
    }

    void DrawCard(HDC dc, const RECT& rect)
    {
        if (rect.bottom <= 0 || rect.top >= contentHeight_) return;
        HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, cardBrush_);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Scale(12), Scale(12));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void DrawFanCurveChart(HDC dc)
    {
        if (curveCard_.right <= curveCard_.left ||
            curveCard_.bottom <= curveCard_.top) return;
        if (curveCard_.bottom <= 0 || curveCard_.top >= contentHeight_) return;

        const int pad = Scale(16);
        const int titleArea = Scale(20);
        const int hintArea = Scale(40);
        const int axisLabelLeft = Scale(40);
        const int axisLabelBottom = Scale(24);

        RECT chart{};
        chart.left = curveCard_.left + pad + axisLabelLeft;
        chart.right = curveCard_.right - pad - Scale(12);
        chart.top = curveCard_.top + pad + titleArea;
        chart.bottom = curveCard_.bottom - hintArea - axisLabelBottom;
        if (chart.right <= chart.left + Scale(20) ||
            chart.bottom <= chart.top + Scale(20)) return;

        // X 轴：40~90°C
        const double tempMin = 40.0;
        const double tempMax = 90.0;
        const double pctMin = 0.0;
        const double pctMax = 100.0;

        auto tempToX = [&](double t) -> int {
            const double ratio = (t - tempMin) / (tempMax - tempMin);
            return chart.left + static_cast<int>(ratio * (chart.right - chart.left) + 0.5);
        };
        auto pctToY = [&](double p) -> int {
            const double ratio = (p - pctMin) / (pctMax - pctMin);
            return chart.bottom - static_cast<int>(ratio * (chart.bottom - chart.top) + 0.5);
        };

        // 网格
        HPEN gridPen = CreatePen(PS_DOT, 1, kCurveGrid);
        HGDIOBJ oldPen = SelectObject(dc, gridPen);
        for (int t = 40; t <= 90; t += 10)
        {
            const int x = tempToX(static_cast<double>(t));
            MoveToEx(dc, x, chart.top, nullptr);
            LineTo(dc, x, chart.bottom);
        }
        for (int p = 0; p <= 100; p += 20)
        {
            const int y = pctToY(static_cast<double>(p));
            MoveToEx(dc, chart.left, y, nullptr);
            LineTo(dc, chart.right, y);
        }
        SelectObject(dc, oldPen);
        DeleteObject(gridPen);

        // 坐标轴
        HPEN axisPen = CreatePen(PS_SOLID, 1, kCurveAxis);
        oldPen = SelectObject(dc, axisPen);
        MoveToEx(dc, chart.left, chart.top, nullptr);
        LineTo(dc, chart.left, chart.bottom);
        LineTo(dc, chart.right, chart.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(axisPen);

        // 数据点：从当前显示的曲线取
        const auto percents = GetDisplayedPercents();
        std::array<POINT, 6> points{};
        for (int i = 0; i < 6; ++i)
        {
            points[i].x = tempToX(kCurveTemperatures[i]);
            points[i].y = pctToY(static_cast<double>(percents[i]));
        }

        HPEN curvePen = CreatePen(PS_SOLID, 2, kAccent);
        oldPen = SelectObject(dc, curvePen);
        Polyline(dc, points.data(), static_cast<int>(points.size()));
        SelectObject(dc, oldPen);
        DeleteObject(curvePen);

        HBRUSH pointBrush = CreateSolidBrush(kAccent);
        HGDIOBJ oldBrush = SelectObject(dc, pointBrush);
        const int radius = std::max(2, Scale(4));
        for (const POINT& pt : points)
        {
            Ellipse(dc, pt.x - radius, pt.y - radius, pt.x + radius, pt.y + radius);
        }
        SelectObject(dc, oldBrush);
        DeleteObject(pointBrush);

        // 标签
        HFONT labelFont = MakeFont(9, FW_NORMAL);
        HGDIOBJ oldFont = SelectObject(dc, labelFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kMuted);

        for (int t = 40; t <= 90; t += 10)
        {
            wchar_t buffer[16]{};
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d°C", t);
            const int x = tempToX(static_cast<double>(t));
            RECT textRect{x - Scale(20), chart.bottom + Scale(4),
                          x + Scale(20), chart.bottom + Scale(22)};
            DrawTextW(dc, buffer, -1, &textRect, DT_CENTER | DT_SINGLELINE | DT_TOP);
        }
        for (int p = 0; p <= 100; p += 20)
        {
            wchar_t buffer[16]{};
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d%%", p);
            const int y = pctToY(static_cast<double>(p));
            RECT textRect{chart.left - axisLabelLeft, y - Scale(9),
                          chart.left - Scale(6), y + Scale(9)};
            DrawTextW(dc, buffer, -1, &textRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }

        SelectObject(dc, oldFont);
        DeleteObject(labelFont);
    }

    void Paint()
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window_, &paint);
        RECT client{};
        GetClientRect(window_, &client);
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(
            dc, std::max(1L, client.right), std::max(1L, client.bottom));
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        FillRect(buffer, &client, backgroundBrush_);
        DrawCard(buffer, statusCard_);
        DrawCard(buffer, profileCard_);
        DrawCard(buffer, fanCard_);
        DrawCard(buffer, curveCard_);
        DrawFanCurveChart(buffer);
        for (const RECT& rect : tempCards_) DrawCard(buffer, rect);
        for (const RECT& rect : powerCards_) DrawCard(buffer, rect);

        HBRUSH accent = CreateSolidBrush(statusColor_);
        RECT marker{statusCard_.left,
                    statusCard_.top + Scale(14),
                    statusCard_.left + Scale(4),
                    statusCard_.bottom - Scale(14)};
        FillRect(buffer, &marker, accent);
        DeleteObject(accent);
        BitBlt(dc, paint.rcPaint.left, paint.rcPaint.top,
               paint.rcPaint.right - paint.rcPaint.left,
               paint.rcPaint.bottom - paint.rcPaint.top,
               buffer, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(window_, &paint);
    }

    void Scroll(int requestedPosition)
    {
        RECT client{};
        GetClientRect(window_, &client);
        const int maximum = std::max(0, contentHeight_ - static_cast<int>(client.bottom));
        const int newPosition = std::clamp(requestedPosition, 0, maximum);
        if (newPosition != scrollPosition_)
        {
            scrollPosition_ = newPosition;
            LayoutControls();
            InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void HandleVerticalScroll(WPARAM wParam)
    {
        int position = scrollPosition_;
        RECT client{};
        GetClientRect(window_, &client);
        switch (LOWORD(wParam))
        {
        case SB_LINEUP: position -= Scale(32); break;
        case SB_LINEDOWN: position += Scale(32); break;
        case SB_PAGEUP: position -= client.bottom; break;
        case SB_PAGEDOWN: position += client.bottom; break;
        case SB_THUMBTRACK:
        {
            SCROLLINFO info{sizeof(info)};
            info.fMask = SIF_TRACKPOS;
            GetScrollInfo(window_, SB_VERT, &info);
            position = info.nTrackPos;
            break;
        }
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = contentHeight_; break;
        default: return;
        }
        Scroll(position);
    }

    void ApplyProfile()
    {
        if (worker_ == nullptr) return;
        std::uint32_t profile = RADXA_SVC_PROFILE_QUIET;
        if (IsDlgButtonChecked(window_, IDC_PROFILE_PERFORMANCE) == BST_CHECKED)
        {
            profile = RADXA_SVC_PROFILE_PERFORMANCE;
        }
        else if (IsDlgButtonChecked(window_, IDC_PROFILE_QUIET) != BST_CHECKED)
        {
            return;
        }
        if (snapshot_.FanValid && snapshot_.FanMode != RADXA_SVC_FAN_CONTROL_AUTO &&
            MessageBoxW(window_,
                Localize(IDS_CONFIRM_PROFILE).c_str(),
                Localize(IDS_CONFIRM_TITLE).c_str(),
                MB_ICONINFORMATION | MB_OKCANCEL) != IDOK)
        {
            return;
        }
        profileDirty_ = false;
        fanDirty_ = false;
        worker_->SetProfile(profile);
    }

    void ApplyFan()
    {
        if (worker_ == nullptr) return;
        const int selection = static_cast<int>(
            SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_GETCURSEL, 0, 0));

        if (selection == 1)  // 全速
        {
            worker_->SetFan(RADXA_SVC_FAN_CONTROL_FULL_SPEED, 0);
        }
        else if (selection == 2)  // 自定义
        {
            customPercents_ = ReadCurveEditors();
            SaveCustomCurvePercents(customPercents_);
            worker_->SetFanCurve(BuildCurveFromPercents(customPercents_));
            worker_->SetFan(RADXA_SVC_FAN_CONTROL_AUTO, 0);
        }
        else  // 自动
        {
            worker_->SetFanCurve(DeviceWorker::kDefaultFanCurve);
            worker_->SetFan(RADXA_SVC_FAN_CONTROL_AUTO, 0);
        }
        fanDirty_ = false;
    }

    void BeginClose()
    {
        if (closing_) return;
        closing_ = true;
        SetControlText(window_, IDC_NOTICE, Localize(IDS_CLOSE_PENDING));
        EnableWindow(window_, FALSE);
        UpdateWindow(window_);
        if (worker_ == nullptr)
        {
            DestroyWindow(window_);
            return;
        }
        shutdownThread_ = std::thread([this] {
            worker_->Stop();
            PostMessageW(window_, WM_APP_WORKER_STOPPED, 0, 0);
        });
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            const int notification = HIWORD(wParam);
            if (id == IDC_LANGUAGE && notification == BN_CLICKED)
            {
                ShowLanguageMenu();
            }
            else if (id == IDM_LANGUAGE_CHINESE || id == IDM_LANGUAGE_ENGLISH)
            {
                language_ = id == IDM_LANGUAGE_CHINESE ? kChinese : kEnglish;
                SaveLanguagePreference(language_);
                ApplyLanguage();
            }
            else if (id == IDC_RETRY && notification == BN_CLICKED && worker_ != nullptr)
            {
                worker_->Retry();
            }
            else if ((id == IDC_PROFILE_QUIET || id == IDC_PROFILE_PERFORMANCE) &&
                     notification == BN_CLICKED)
            {
                profileDirty_ = true;
            }
            else if (id == IDC_PROFILE_APPLY && notification == BN_CLICKED)
            {
                ApplyProfile();
            }
            else if (id == IDC_FAN_MODE && notification == CBN_DROPDOWN)
            {
                fanComboDropped_ = true;
                fanSelectionCommitted_ = false;
                fanSelectionBeforeDropdown_ = static_cast<int>(
                    SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_GETCURSEL, 0, 0));
            }
            else if (id == IDC_FAN_MODE && notification == CBN_SELENDOK)
            {
                fanDirty_ = true;
                fanSelectionCommitted_ = true;
            }
            else if (id == IDC_FAN_MODE && notification == CBN_SELENDCANCEL)
            {
                fanSelectionCommitted_ = false;
            }
            else if (id == IDC_FAN_MODE && notification == CBN_CLOSEUP)
            {
                fanComboDropped_ = false;
                if (fanSelectionCommitted_)
                {
                    PostMessageW(window_, WM_APP_FAN_SELECTION_COMMITTED, 0, 0);
                }
                else if (fanSelectionBeforeDropdown_ >= 0)
                {
                    SendDlgItemMessageW(window_, IDC_FAN_MODE, CB_SETCURSEL,
                                        fanSelectionBeforeDropdown_, 0);
                }
                fanSelectionCommitted_ = false;
            }
            else if (id == IDC_FAN_MODE && notification == CBN_SELCHANGE)
            {
                if (!fanComboDropped_)
                {
                    fanDirty_ = true;
                    ApplySnapshot();
                }
            }
            else if (id == IDC_FAN_APPLY && notification == BN_CLICKED)
            {
                ApplyFan();
            }
            else if (notification == EN_CHANGE)
            {
                // 编辑框改动 → 实时更新曲线图
                for (int editId : kCurveEditIds)
                {
                    if (id == editId)
                    {
                        InvalidateRect(window_, &curveCard_, FALSE);
                        break;
                    }
                }
            }
            return 0;
        }
        case WM_HSCROLL:
            return 0;
        case WM_VSCROLL:
            HandleVerticalScroll(wParam);
            return 0;
        case WM_MOUSEWHEEL:
            Scroll(scrollPosition_ - GET_WHEEL_DELTA_WPARAM(wParam) * Scale(64) / WHEEL_DELTA);
            return 0;
        case WM_SIZE:
            LayoutControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED:
        {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            UpdateDpi(HIWORD(wParam));
            return 0;
        }
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(700);
            info->ptMinTrackSize.y = Scale(560);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_CTLCOLORSTATIC:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            const int id = GetDlgCtrlID(control);
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, IsCardStaticControl(id) ? kCard : kBackground);
            COLORREF color = kText;
            if (id == IDC_STATUS_TITLE) color = statusColor_;
            else if (id == IDC_PROFILE_HINT || id == IDC_FAN_HINT ||
                     id == IDC_POWER1_DETAIL || id == IDC_POWER2_DETAIL ||
                     id == IDC_POWER_SYSTEM_DETAIL || id == IDC_FAN_CURVE_HINT)
                color = kMuted;
            else if (id == IDC_NOTICE) color = noticeColor_;
            SetTextColor(dc, color);
            return reinterpret_cast<LRESULT>(
                IsCardStaticControl(id) ? cardBrush_ : backgroundBrush_);
        }
        case WM_CTLCOLORBTN:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, kCard);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(cardBrush_);
        }
        case WM_CTLCOLOREDIT:
        {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, kCard);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(cardBrush_);
        }
        case WM_APP_DEVICE_UPDATE:
            ApplySnapshot();
            return 0;
        case WM_APP_FAN_SELECTION_COMMITTED:
            ApplySnapshot();
            return 0;
        case WM_APP_WORKER_STOPPED:
            if (shutdownThread_.joinable()) shutdownThread_.join();
            DestroyWindow(window_);
            return 0;
        case WM_CLOSE:
            BeginClose();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    LANGID language_ = kEnglish;
    UINT dpi_ = 96;
    std::array<HFONT, 5> fonts_{};
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    std::unique_ptr<DeviceWorker> worker_;
    std::thread shutdownThread_;
    DeviceSnapshot snapshot_{};
    bool profileDirty_ = false;
    bool fanDirty_ = false;
    bool fanComboDropped_ = false;
    bool fanSelectionCommitted_ = false;
    int fanSelectionBeforeDropdown_ = -1;
    bool manualControlsVisible_ = false;
    bool closing_ = false;
    int scrollPosition_ = 0;
    int contentHeight_ = 0;
    COLORREF statusColor_ = kWarning;
    COLORREF noticeColor_ = kSuccess;
    RECT statusCard_{};
    RECT profileCard_{};
    RECT fanCard_{};
    RECT curveCard_{};
    std::array<RECT, 2> tempCards_{};
    std::array<RECT, 3> powerCards_{};
    std::array<int, 6> customPercents_ = kDefaultCurvePercents;
};

int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE previousInstance,
    _In_ PWSTR commandLine,
    _In_ int showCommand)
{
    UNREFERENCED_PARAMETER(previousInstance);
    UNREFERENCED_PARAMETER(commandLine);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{sizeof(controls)};
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&controls))
    {
        return 1;
    }

    Application application;
    return application.Run(instance, showCommand);
}
