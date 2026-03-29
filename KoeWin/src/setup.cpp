#include "setup.h"
#include "yaml_config.h"
#include <fstream>
#include <sstream>
#include <commctrl.h>

extern "C" {
    char* sp_core_supported_local_providers();
    char* sp_core_scan_models_json();
    char* sp_config_get(const char* key_path);
    int32_t sp_config_set(const char* key_path, const char* value);
    void sp_core_free_string(char* s);
    int32_t sp_core_check_model_status(const char* model_path);
    int32_t sp_core_download_model(
        const char* model_path,
        void (*progress_cb)(void* ctx, uint32_t file_index, uint32_t file_count,
                            uint64_t bytes_downloaded, uint64_t bytes_total, const char* filename),
        void (*status_cb)(void* ctx, int32_t status, const char* message),
        void* ctx);
    int32_t sp_core_cancel_download(const char* model_path);
    int32_t sp_core_remove_model_files(const char* model_path);
}

#pragma comment(lib, "comctl32.lib")

static SetupWizard* g_setupWizard = nullptr;

// Layout constants
static const int kWinW = 560;
static const int kWinH = 440;
static const int kTabH = 24;
static const int kPaneTop = 36;
static const int kPaneLeft = 16;
static const int kPaneRight = kWinW - 32;
static const int kLabelW = 130;
static const int kEditH = 22;
static const int kComboH = 200;  // drop height
static const int kRowH = 30;
static const int kBtnW = 80;
static const int kBtnH = 28;

// Control IDs
enum {
    IDC_TAB = 100,
    IDC_SAVE = 101,
    IDC_CANCEL_BTN = 102,
    IDC_ASR_TOGGLE_KEY = 110,
    IDC_ASR_PROVIDER = 111,
    IDC_ASR_QWEN_TOGGLE_KEY = 112,
    IDC_ASR_DOWNLOAD = 113,
    IDC_ASR_DELETE = 114,
    IDC_LLM_TOGGLE_KEY = 120,
};

// Custom messages for async model download callbacks
static const UINT WM_MODEL_PROGRESS = WM_USER + 200;
static const UINT WM_MODEL_STATUS = WM_USER + 201;

// ASR provider options
struct AsrProviderOption {
    const char* configValue;
    const wchar_t* displayName;
    bool isLocal;
};
static const AsrProviderOption kAsrProviders[] = {
    { "doubao",      L"Doubao (\x8c46\x5305)",  false },
    { "qwen",        L"Qwen (\x963F\x91CC\x4E91)", false },
    { "sherpa-onnx", L"Sherpa-ONNX",            true },
};
static const int kAsrProviderCount = sizeof(kAsrProviders) / sizeof(kAsrProviders[0]);

// Hotkey option strings (config value, display name)
struct HotkeyOption {
    const wchar_t* configValue;
    const wchar_t* displayName;
};
static const HotkeyOption kHotkeyOptions[] = {
    { L"right_control", L"Right Ctrl" },
    { L"left_control",  L"Left Ctrl" },
    { L"right_option",  L"Right Alt" },
    { L"left_option",   L"Left Alt" },
    { L"right_command", L"Right Win" },
    { L"left_command",  L"Left Win" },
    { L"caps_lock",     L"Caps Lock" },
    { L"scroll_lock",   L"Scroll Lock" },
};
static const int kHotkeyOptionCount = sizeof(kHotkeyOptions) / sizeof(kHotkeyOptions[0]);

// ── Helpers ─────────────────────────────────────────────

static std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    return utf8;
}

static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), len);
    return wide;
}

static std::wstring getEditText(HWND edit) {
    int len = GetWindowTextLengthW(edit);
    if (len == 0) return L"";
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(edit, buf.data(), len + 1);
    buf.resize(len);
    return buf;
}

// ── Window Proc ─────────────────────────────────────────

struct ModelProgressMsg {
    uint32_t file_index;
    uint32_t file_count;
    uint64_t bytes_downloaded;
    uint64_t bytes_total;
};

struct ModelStatusMsg {
    int32_t status;
    std::string message;
};

LRESULT CALLBACK SetupWizard::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (g_setupWizard) g_setupWizard->handleCommand(wParam, lParam);
        return 0;

    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
        if (nmhdr->idFrom == IDC_TAB && nmhdr->code == TCN_SELCHANGE) {
            if (g_setupWizard) g_setupWizard->onTabChanged();
        }
        return 0;
    }

    case WM_MODEL_PROGRESS: {
        auto* p = reinterpret_cast<ModelProgressMsg*>(lParam);
        if (g_setupWizard && p) {
            double pct = p->bytes_total > 0 ? (double)p->bytes_downloaded / p->bytes_total * 100.0 : 0;
            if (g_setupWizard->m_asrProgressBar)
                SendMessageW(g_setupWizard->m_asrProgressBar, PBM_SETPOS, (WPARAM)(int)pct, 0);
            if (g_setupWizard->m_asrProgressLabel) {
                double dlMB = p->bytes_downloaded / (1024.0 * 1024.0);
                double totMB = p->bytes_total / (1024.0 * 1024.0);
                wchar_t buf[64];
                swprintf(buf, 64, L"%.1f / %.1f MB", dlMB, totMB);
                SetWindowTextW(g_setupWizard->m_asrProgressLabel, buf);
            }
        }
        delete p;
        return 0;
    }

    case WM_MODEL_STATUS: {
        auto* s = reinterpret_cast<ModelStatusMsg*>(lParam);
        if (g_setupWizard && s) {
            g_setupWizard->m_asrDownloading = false;
            g_setupWizard->m_downloadingModelPath.clear();
            g_setupWizard->updateModelStatus();
        }
        delete s;
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── SetupWizard ─────────────────────────────────────────

SetupWizard::SetupWizard(HINSTANCE hInstance, SetupWizardDelegate* delegate)
    : m_hInstance(hInstance), m_delegate(delegate) {
    g_setupWizard = this;

    // Create a font for controls
    m_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

SetupWizard::~SetupWizard() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_font) DeleteObject(m_font);
    if (g_setupWizard == this) g_setupWizard = nullptr;
}

void SetupWizard::show() {
    if (!m_hwnd) createWindow();

    loadCurrentValues();

    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(m_hwnd);
}

void SetupWizard::createWindow() {
    // Init common controls for tab
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = L"KoeSetupWizard";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    RegisterClassExW(&wc);

    // Center on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - kWinW) / 2;
    int y = (screenH - kWinH) / 2;

    m_hwnd = CreateWindowExW(
        0, L"KoeSetupWizard", L"Koe Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, kWinW, kWinH,
        nullptr, nullptr, m_hInstance, nullptr
    );

    // Tab control
    m_tabCtrl = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        4, 4, kWinW - 24, kTabH + 4,
        m_hwnd, reinterpret_cast<HMENU>(IDC_TAB), m_hInstance, nullptr);
    SendMessageW(m_tabCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

    const wchar_t* tabs[] = { L"ASR", L"LLM", L"Controls", L"Dictionary", L"System Prompt" };
    for (int i = 0; i < 5; i++) {
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(tabs[i]);
        SendMessageW(m_tabCtrl, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&item));
    }

    // Save/Cancel buttons at bottom
    int btnY = kWinH - kBtnH - 44;
    m_saveButton = CreateWindowExW(0, L"BUTTON", L"Save",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        kWinW - 2 * kBtnW - 32 - 8, btnY, kBtnW, kBtnH,
        m_hwnd, reinterpret_cast<HMENU>(IDC_SAVE), m_hInstance, nullptr);
    SendMessageW(m_saveButton, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

    m_cancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        kWinW - kBtnW - 24, btnY, kBtnW, kBtnH,
        m_hwnd, reinterpret_cast<HMENU>(IDC_CANCEL_BTN), m_hInstance, nullptr);
    SendMessageW(m_cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);

    // Show first tab
    m_currentTab = 0;
    createAsrPane();
}

void SetupWizard::handleCommand(WPARAM wParam, LPARAM lParam) {
    int id = LOWORD(wParam);
    int code = HIWORD(wParam);
    switch (id) {
    case IDC_SAVE:
        saveConfig();
        ShowWindow(m_hwnd, SW_HIDE);
        if (m_delegate) m_delegate->setupWizardDidSaveConfig();
        break;
    case IDC_CANCEL_BTN:
        ShowWindow(m_hwnd, SW_HIDE);
        break;
    case IDC_ASR_PROVIDER:
        if (code == CBN_SELCHANGE) onAsrProviderChanged();
        break;
    case IDC_ASR_TOGGLE_KEY:
        m_asrAccessKeyVisible = !m_asrAccessKeyVisible;
        if (m_asrAccessKeyEdit) {
            SendMessageW(m_asrAccessKeyEdit, EM_SETPASSWORDCHAR,
                         m_asrAccessKeyVisible ? 0 : L'*', 0);
            InvalidateRect(m_asrAccessKeyEdit, nullptr, TRUE);
        }
        break;
    case IDC_ASR_QWEN_TOGGLE_KEY:
        m_asrQwenApiKeyVisible = !m_asrQwenApiKeyVisible;
        if (m_asrQwenApiKeyEdit) {
            SendMessageW(m_asrQwenApiKeyEdit, EM_SETPASSWORDCHAR,
                         m_asrQwenApiKeyVisible ? 0 : L'*', 0);
            InvalidateRect(m_asrQwenApiKeyEdit, nullptr, TRUE);
        }
        break;
    case IDC_ASR_DOWNLOAD:
        if (m_asrDownloading) cancelModelDownload();
        else startModelDownload();
        break;
    case IDC_ASR_DELETE:
        deleteSelectedModel();
        break;
    case IDC_LLM_TOGGLE_KEY:
        m_llmApiKeyVisible = !m_llmApiKeyVisible;
        if (m_llmApiKeyEdit) {
            SendMessageW(m_llmApiKeyEdit, EM_SETPASSWORDCHAR,
                         m_llmApiKeyVisible ? 0 : L'*', 0);
            InvalidateRect(m_llmApiKeyEdit, nullptr, TRUE);
        }
        break;
    }
}

void SetupWizard::onTabChanged() {
    int sel = static_cast<int>(SendMessageW(m_tabCtrl, TCM_GETCURSEL, 0, 0));
    if (sel == m_currentTab) return;
    m_currentTab = sel;

    destroyPaneControls();
    switch (sel) {
    case 0: createAsrPane(); break;
    case 1: createLlmPane(); break;
    case 2: createControlsPane(); break;
    case 3: createDictionaryPane(); break;
    case 4: createPromptPane(); break;
    }
    loadCurrentValues();
}

void SetupWizard::destroyPaneControls() {
    for (HWND h : m_paneControls) DestroyWindow(h);
    m_paneControls.clear();
    m_asrProviderCombo = nullptr;
    m_asrAppKeyLabel = m_asrAppKeyEdit = nullptr;
    m_asrAccessKeyLabel = m_asrAccessKeyEdit = m_asrAccessKeyToggle = nullptr;
    m_asrQwenApiKeyLabel = m_asrQwenApiKeyEdit = m_asrQwenApiKeyToggle = nullptr;
    m_asrModelLabel = m_asrModelCombo = m_asrModelStatusLabel = nullptr;
    m_asrDownloadBtn = m_asrDeleteBtn = m_asrProgressBar = m_asrProgressLabel = nullptr;
    m_llmEnabledCheck = m_llmBaseUrlEdit = m_llmApiKeyEdit = m_llmModelEdit = m_llmTokenParamCombo = nullptr;
    m_triggerKeyCombo = m_cancelKeyCombo = nullptr;
    m_startSoundCheck = m_stopSoundCheck = m_errorSoundCheck = nullptr;
    m_dictEdit = m_promptEdit = nullptr;
}

// ── Pane creation helpers ───────────────────────────────

static HWND makeLabel(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                              x, y, w, h, parent, nullptr, hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

static HWND makeEdit(HWND parent, HFONT font, int x, int y, int w, int h, DWORD style, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | style,
                              x, y, w, h, parent, nullptr, hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

static HWND makeMultiEdit(HWND parent, HFONT font, int x, int y, int w, int h, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL | ES_AUTOVSCROLL,
                              x, y, w, h, parent, nullptr, hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

static HWND makeCombo(HWND parent, HFONT font, int x, int y, int w, int h, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                              x, y, w, h, parent, nullptr, hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

static HWND makeCheck(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              x, y, w, h, parent, nullptr, hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

static HWND makeButton(HWND parent, HFONT font, const wchar_t* text, int x, int y, int w, int h, int id, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

#define ADDCTL(x) m_paneControls.push_back(x)

static HWND makeComboWithId(HWND parent, HFONT font, int x, int y, int w, int h, int id, HINSTANCE hInst) {
    HWND h_ = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                              x, y, w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    SendMessageW(h_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return h_;
}

void SetupWizard::createAsrPane() {
    int y = kPaneTop;
    int editX = kPaneLeft + kLabelW + 8;
    int editW = kPaneRight - editX;

    // Row 0: Provider selector
    ADDCTL(makeLabel(m_hwnd, m_font, L"Provider:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_asrProviderCombo = makeComboWithId(m_hwnd, m_font, editX, y, 180, kComboH, IDC_ASR_PROVIDER, m_hInstance);
    for (int i = 0; i < kAsrProviderCount; i++)
        SendMessageW(m_asrProviderCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kAsrProviders[i].displayName));
    SendMessageW(m_asrProviderCombo, CB_SETCURSEL, 0, 0);
    ADDCTL(m_asrProviderCombo);
    y += kRowH;

    // Row 1: Doubao App Key / Local Model selector
    m_asrAppKeyLabel = makeLabel(m_hwnd, m_font, L"App Key:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance);
    ADDCTL(m_asrAppKeyLabel);
    m_asrAppKeyEdit = makeEdit(m_hwnd, m_font, editX, y, editW, kEditH, 0, m_hInstance);
    ADDCTL(m_asrAppKeyEdit);

    m_asrModelLabel = makeLabel(m_hwnd, m_font, L"Model:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance);
    ADDCTL(m_asrModelLabel);
    m_asrModelCombo = makeCombo(m_hwnd, m_font, editX, y, editW - 30, kComboH, m_hInstance);
    ADDCTL(m_asrModelCombo);
    m_asrDownloadBtn = makeButton(m_hwnd, m_font, L"\x2B07", editX + editW - 26, y, 26, kEditH, IDC_ASR_DOWNLOAD, m_hInstance);
    ADDCTL(m_asrDownloadBtn);
    y += kRowH;

    // Row 2: Model status + delete (local only)
    m_asrModelStatusLabel = makeLabel(m_hwnd, m_font, L"", editX, y + 2, editW - 30, 20, m_hInstance);
    ADDCTL(m_asrModelStatusLabel);
    m_asrDeleteBtn = makeButton(m_hwnd, m_font, L"\x2716", editX + editW - 26, y, 26, kEditH, IDC_ASR_DELETE, m_hInstance);
    ADDCTL(m_asrDeleteBtn);
    y += kRowH;

    // Row 3: Progress bar + size label (local only, hidden by default)
    m_asrProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | PBS_SMOOTH, editX, y + 2, editW - 100, 16,
        m_hwnd, nullptr, m_hInstance, nullptr);
    SendMessageW(m_asrProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    ADDCTL(m_asrProgressBar);
    m_asrProgressLabel = makeLabel(m_hwnd, m_font, L"", editX + editW - 96, y + 2, 96, 16, m_hInstance);
    ADDCTL(m_asrProgressLabel);
    y += kRowH;

    // Row 4: Doubao Access Key
    m_asrAccessKeyLabel = makeLabel(m_hwnd, m_font, L"Access Key:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance);
    ADDCTL(m_asrAccessKeyLabel);
    m_asrAccessKeyEdit = makeEdit(m_hwnd, m_font, editX, y, editW - 60, kEditH, ES_PASSWORD, m_hInstance);
    ADDCTL(m_asrAccessKeyEdit);
    m_asrAccessKeyToggle = makeButton(m_hwnd, m_font, L"Show", kPaneRight - 52, y, 52, kEditH, IDC_ASR_TOGGLE_KEY, m_hInstance);
    ADDCTL(m_asrAccessKeyToggle);
    m_asrAccessKeyVisible = false;

    // Row 4 (same row): Qwen API Key
    m_asrQwenApiKeyLabel = makeLabel(m_hwnd, m_font, L"API Key:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance);
    ADDCTL(m_asrQwenApiKeyLabel);
    m_asrQwenApiKeyEdit = makeEdit(m_hwnd, m_font, editX, y, editW - 60, kEditH, ES_PASSWORD, m_hInstance);
    ADDCTL(m_asrQwenApiKeyEdit);
    m_asrQwenApiKeyToggle = makeButton(m_hwnd, m_font, L"Show", kPaneRight - 52, y, 52, kEditH, IDC_ASR_QWEN_TOGGLE_KEY, m_hInstance);
    ADDCTL(m_asrQwenApiKeyToggle);
    m_asrQwenApiKeyVisible = false;

    // Default: show Doubao fields, hide others
    onAsrProviderChanged();
}

void SetupWizard::onAsrProviderChanged() {
    int sel = static_cast<int>(SendMessageW(m_asrProviderCombo, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= kAsrProviderCount) sel = 0;

    bool isDoubao = (sel == 0);
    bool isQwen = (sel == 1);
    bool isLocal = kAsrProviders[sel].isLocal;

    auto show = [](HWND h, bool visible) {
        if (h) ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
    };

    // Doubao fields
    show(m_asrAppKeyLabel, isDoubao);
    show(m_asrAppKeyEdit, isDoubao);
    show(m_asrAccessKeyLabel, isDoubao);
    show(m_asrAccessKeyEdit, isDoubao);
    show(m_asrAccessKeyToggle, isDoubao);

    // Qwen fields
    show(m_asrQwenApiKeyLabel, isQwen);
    show(m_asrQwenApiKeyEdit, isQwen);
    show(m_asrQwenApiKeyToggle, isQwen);

    // Local model fields
    show(m_asrModelLabel, isLocal);
    show(m_asrModelCombo, isLocal);
    show(m_asrDownloadBtn, isLocal);
    show(m_asrDeleteBtn, isLocal);
    show(m_asrModelStatusLabel, isLocal);
    show(m_asrProgressBar, false);  // hidden until downloading
    show(m_asrProgressLabel, false);

    if (isLocal) {
        // Populate model list
        SendMessageW(m_asrModelCombo, CB_RESETCONTENT, 0, 0);
        char* json = sp_core_scan_models_json();
        if (json) {
            // Simple JSON array parsing: find objects with matching provider
            std::string js(json);
            sp_core_free_string(json);
            // Parse each model entry - look for "provider":"sherpa-onnx" entries
            // We do minimal parsing: find "description":"..." and "path":"..."
            const char* provider = kAsrProviders[sel].configValue;
            size_t pos = 0;
            while ((pos = js.find("\"provider\"", pos)) != std::string::npos) {
                // Find provider value
                size_t vstart = js.find(':', pos) + 1;
                size_t qs = js.find('"', vstart) + 1;
                size_t qe = js.find('"', qs);
                std::string prov = js.substr(qs, qe - qs);
                // Find the enclosing object start
                size_t objStart = js.rfind('{', pos);
                size_t objEnd = js.find('}', pos);

                if (prov == provider && objStart != std::string::npos && objEnd != std::string::npos) {
                    std::string obj = js.substr(objStart, objEnd - objStart + 1);
                    // Extract "description"
                    std::string desc, path;
                    size_t dp = obj.find("\"description\"");
                    if (dp != std::string::npos) {
                        size_t ds = obj.find('"', dp + 13) + 1;
                        size_t de = obj.find('"', ds);
                        desc = obj.substr(ds, de - ds);
                    }
                    size_t pp = obj.find("\"path\"");
                    if (pp != std::string::npos) {
                        size_t ps = obj.find('"', pp + 6) + 1;
                        size_t pe = obj.find('"', ps);
                        path = obj.substr(ps, pe - ps);
                    }
                    if (!desc.empty()) {
                        int idx = static_cast<int>(SendMessageW(m_asrModelCombo, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(utf8ToWide(desc).c_str())));
                        // Store path as item data (we'll use a parallel vector)
                        SendMessageW(m_asrModelCombo, CB_SETITEMDATA, idx,
                            reinterpret_cast<LPARAM>(_strdup(path.c_str())));
                    }
                }
                pos = qe + 1;
            }
        }

        // Select current model from config
        std::string yaml = readConfigFile();
        std::string configKey = std::string("asr.") + kAsrProviders[sel].configValue + ".model";
        std::string currentModel = YamlConfig::read(yaml, configKey.c_str());
        int count = static_cast<int>(SendMessageW(m_asrModelCombo, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; i++) {
            char* path = reinterpret_cast<char*>(SendMessageW(m_asrModelCombo, CB_GETITEMDATA, i, 0));
            if (path && currentModel == path) {
                SendMessageW(m_asrModelCombo, CB_SETCURSEL, i, 0);
                break;
            }
        }
        if (SendMessageW(m_asrModelCombo, CB_GETCURSEL, 0, 0) == CB_ERR && count > 0)
            SendMessageW(m_asrModelCombo, CB_SETCURSEL, 0, 0);

        updateModelStatus();
    }
}

void SetupWizard::updateModelStatus() {
    if (!m_asrModelCombo || !m_asrModelStatusLabel) return;

    int sel = static_cast<int>(SendMessageW(m_asrModelCombo, CB_GETCURSEL, 0, 0));
    if (sel == CB_ERR) {
        SetWindowTextW(m_asrModelStatusLabel, L"");
        EnableWindow(m_asrDownloadBtn, FALSE);
        EnableWindow(m_asrDeleteBtn, FALSE);
        return;
    }

    char* path = reinterpret_cast<char*>(SendMessageW(m_asrModelCombo, CB_GETITEMDATA, sel, 0));
    if (!path) return;

    if (m_asrDownloading && m_downloadingModelPath == path) {
        SetWindowTextW(m_asrModelStatusLabel, L"Downloading...");
        SetWindowTextW(m_asrDownloadBtn, L"\x23F9");  // stop symbol
        EnableWindow(m_asrDownloadBtn, TRUE);
        EnableWindow(m_asrDeleteBtn, FALSE);
        ShowWindow(m_asrProgressBar, SW_SHOW);
        ShowWindow(m_asrProgressLabel, SW_SHOW);
        return;
    }

    ShowWindow(m_asrProgressBar, SW_HIDE);
    ShowWindow(m_asrProgressLabel, SW_HIDE);
    SetWindowTextW(m_asrDownloadBtn, L"\x2B07");  // download arrow

    int status = sp_core_check_model_status(path);
    switch (status) {
    case 2:  // installed
        SetWindowTextW(m_asrModelStatusLabel, L"\x25CF Installed");
        EnableWindow(m_asrDownloadBtn, FALSE);
        EnableWindow(m_asrDeleteBtn, TRUE);
        break;
    case 1:  // incomplete
        SetWindowTextW(m_asrModelStatusLabel, L"\x25D0 Incomplete");
        EnableWindow(m_asrDownloadBtn, TRUE);
        EnableWindow(m_asrDeleteBtn, TRUE);
        break;
    default:  // not installed
        SetWindowTextW(m_asrModelStatusLabel, L"\x25CB Not installed");
        EnableWindow(m_asrDownloadBtn, TRUE);
        EnableWindow(m_asrDeleteBtn, FALSE);
        break;
    }
}

static void modelProgressCallback(void* ctx, uint32_t file_index, uint32_t file_count,
                                   uint64_t bytes_downloaded, uint64_t bytes_total, const char*) {
    HWND hwnd = reinterpret_cast<HWND>(ctx);
    auto* msg = new ModelProgressMsg{ file_index, file_count, bytes_downloaded, bytes_total };
    PostMessageW(hwnd, WM_MODEL_PROGRESS, 0, reinterpret_cast<LPARAM>(msg));
}

static void modelStatusCallback(void* ctx, int32_t status, const char* message) {
    HWND hwnd = reinterpret_cast<HWND>(ctx);
    auto* msg = new ModelStatusMsg{ status, message ? message : "" };
    PostMessageW(hwnd, WM_MODEL_STATUS, 0, reinterpret_cast<LPARAM>(msg));
}

void SetupWizard::startModelDownload() {
    if (!m_asrModelCombo || m_asrDownloading) return;
    int sel = static_cast<int>(SendMessageW(m_asrModelCombo, CB_GETCURSEL, 0, 0));
    if (sel == CB_ERR) return;

    char* path = reinterpret_cast<char*>(SendMessageW(m_asrModelCombo, CB_GETITEMDATA, sel, 0));
    if (!path) return;

    m_asrDownloading = true;
    m_downloadingModelPath = path;
    SendMessageW(m_asrProgressBar, PBM_SETPOS, 0, 0);
    SetWindowTextW(m_asrProgressLabel, L"");
    updateModelStatus();

    sp_core_download_model(path, modelProgressCallback, modelStatusCallback, reinterpret_cast<void*>(m_hwnd));
}

void SetupWizard::cancelModelDownload() {
    if (!m_downloadingModelPath.empty()) {
        sp_core_cancel_download(m_downloadingModelPath.c_str());
    }
}

void SetupWizard::deleteSelectedModel() {
    if (!m_asrModelCombo) return;
    int sel = static_cast<int>(SendMessageW(m_asrModelCombo, CB_GETCURSEL, 0, 0));
    if (sel == CB_ERR) return;

    char* path = reinterpret_cast<char*>(SendMessageW(m_asrModelCombo, CB_GETITEMDATA, sel, 0));
    if (!path) return;

    int result = MessageBoxW(m_hwnd, L"Downloaded model files will be deleted.\nThe model can be re-downloaded later.",
                             L"Remove Model Files?", MB_OKCANCEL | MB_ICONWARNING);
    if (result == IDOK) {
        sp_core_remove_model_files(path);
        updateModelStatus();
    }
}

void SetupWizard::createLlmPane() {
    int y = kPaneTop;
    int editW = kPaneRight - kPaneLeft - kLabelW - 8;

    m_llmEnabledCheck = makeCheck(m_hwnd, m_font, L"Enable LLM correction", kPaneLeft, y, 300, 20, m_hInstance);
    ADDCTL(m_llmEnabledCheck);
    y += kRowH;

    ADDCTL(makeLabel(m_hwnd, m_font, L"Base URL:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_llmBaseUrlEdit = makeEdit(m_hwnd, m_font, kPaneLeft + kLabelW + 8, y, editW, kEditH, 0, m_hInstance);
    ADDCTL(m_llmBaseUrlEdit);
    y += kRowH;

    ADDCTL(makeLabel(m_hwnd, m_font, L"API Key:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_llmApiKeyEdit = makeEdit(m_hwnd, m_font, kPaneLeft + kLabelW + 8, y, editW - 60, kEditH, ES_PASSWORD, m_hInstance);
    ADDCTL(m_llmApiKeyEdit);
    ADDCTL(makeButton(m_hwnd, m_font, L"Show", kPaneRight - 52, y, 52, kEditH, IDC_LLM_TOGGLE_KEY, m_hInstance));
    m_llmApiKeyVisible = false;
    y += kRowH;

    ADDCTL(makeLabel(m_hwnd, m_font, L"Model:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_llmModelEdit = makeEdit(m_hwnd, m_font, kPaneLeft + kLabelW + 8, y, editW, kEditH, 0, m_hInstance);
    ADDCTL(m_llmModelEdit);
    y += kRowH;

    ADDCTL(makeLabel(m_hwnd, m_font, L"Max Token Param:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_llmTokenParamCombo = makeCombo(m_hwnd, m_font, kPaneLeft + kLabelW + 8, y, editW, kComboH, m_hInstance);
    SendMessageW(m_llmTokenParamCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"max_completion_tokens"));
    SendMessageW(m_llmTokenParamCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"max_tokens"));
    ADDCTL(m_llmTokenParamCombo);
}

void SetupWizard::createControlsPane() {
    int y = kPaneTop;
    int comboW = 180;

    ADDCTL(makeLabel(m_hwnd, m_font, L"Trigger Key:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_triggerKeyCombo = makeCombo(m_hwnd, m_font, kPaneLeft + kLabelW + 8, y, comboW, kComboH, m_hInstance);
    for (int i = 0; i < kHotkeyOptionCount; i++) {
        SendMessageW(m_triggerKeyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kHotkeyOptions[i].displayName));
    }
    ADDCTL(m_triggerKeyCombo);
    y += kRowH;

    ADDCTL(makeLabel(m_hwnd, m_font, L"Cancel Key:", kPaneLeft, y + 2, kLabelW, 20, m_hInstance));
    m_cancelKeyCombo = makeCombo(m_hwnd, m_font, kPaneLeft + kLabelW + 8, y, comboW, kComboH, m_hInstance);
    for (int i = 0; i < kHotkeyOptionCount; i++) {
        SendMessageW(m_cancelKeyCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kHotkeyOptions[i].displayName));
    }
    ADDCTL(m_cancelKeyCombo);
    y += kRowH + 10;

    ADDCTL(makeLabel(m_hwnd, m_font, L"Sound Feedback:", kPaneLeft, y, 200, 20, m_hInstance));
    y += 24;
    m_startSoundCheck = makeCheck(m_hwnd, m_font, L"Play sound on recording start", kPaneLeft + 16, y, 350, 20, m_hInstance);
    ADDCTL(m_startSoundCheck);
    y += 24;
    m_stopSoundCheck = makeCheck(m_hwnd, m_font, L"Play sound on recording stop", kPaneLeft + 16, y, 350, 20, m_hInstance);
    ADDCTL(m_stopSoundCheck);
    y += 24;
    m_errorSoundCheck = makeCheck(m_hwnd, m_font, L"Play sound on error", kPaneLeft + 16, y, 350, 20, m_hInstance);
    ADDCTL(m_errorSoundCheck);
}

void SetupWizard::createDictionaryPane() {
    int editH = kWinH - kPaneTop - kBtnH - 80;
    ADDCTL(makeLabel(m_hwnd, m_font, L"Dictionary (one term per line, # for comments):",
                     kPaneLeft, kPaneTop, kPaneRight - kPaneLeft, 20, m_hInstance));
    m_dictEdit = makeMultiEdit(m_hwnd, m_font, kPaneLeft, kPaneTop + 24,
                               kPaneRight - kPaneLeft, editH, m_hInstance);
    ADDCTL(m_dictEdit);
}

void SetupWizard::createPromptPane() {
    int editH = kWinH - kPaneTop - kBtnH - 80;
    ADDCTL(makeLabel(m_hwnd, m_font, L"System prompt sent to LLM during correction:",
                     kPaneLeft, kPaneTop, kPaneRight - kPaneLeft, 20, m_hInstance));
    m_promptEdit = makeMultiEdit(m_hwnd, m_font, kPaneLeft, kPaneTop + 24,
                                 kPaneRight - kPaneLeft, editH, m_hInstance);
    ADDCTL(m_promptEdit);
}

// ── Config I/O ──────────────────────────────────────────

std::string SetupWizard::configDir() {
    wchar_t appdata[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (len == 0) return "";
    std::wstring dir = std::wstring(appdata) + L"\\koe";
    CreateDirectoryW(dir.c_str(), nullptr);
    return wideToUtf8(dir);
}

std::string SetupWizard::readConfigFile() {
    std::string path = configDir() + "\\config.yaml";
    std::ifstream f(path);
    if (!f.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void SetupWizard::writeConfigFile(const std::string& yaml) {
    std::string path = configDir() + "\\config.yaml";
    std::ofstream f(path, std::ios::trunc);
    f << yaml;
}

std::string SetupWizard::readTextFile(const std::string& filename) {
    std::string path = configDir() + "\\" + filename;
    std::ifstream f(path);
    if (!f.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void SetupWizard::writeTextFile(const std::string& filename, const std::string& content) {
    std::string path = configDir() + "\\" + filename;
    std::ofstream f(path, std::ios::trunc);
    f << content;
}

// ── Load/Save ───────────────────────────────────────────

static int findHotkeyIndex(const std::string& configValue) {
    std::wstring wide = utf8ToWide(configValue);
    for (int i = 0; i < kHotkeyOptionCount; i++) {
        if (wide == kHotkeyOptions[i].configValue) return i;
    }
    return 0;  // default: first option
}

void SetupWizard::loadCurrentValues() {
    std::string yaml = readConfigFile();

    switch (m_currentTab) {
    case 0: { // ASR
        // Select provider
        std::string provider = YamlConfig::read(yaml, "asr.provider");
        int provIdx = 0;
        for (int i = 0; i < kAsrProviderCount; i++) {
            if (provider == kAsrProviders[i].configValue) { provIdx = i; break; }
        }
        if (m_asrProviderCombo) {
            SendMessageW(m_asrProviderCombo, CB_SETCURSEL, provIdx, 0);
            onAsrProviderChanged();
        }
        // Doubao fields
        if (m_asrAppKeyEdit)
            SetWindowTextW(m_asrAppKeyEdit, utf8ToWide(YamlConfig::read(yaml, "asr.doubao.app_key")).c_str());
        if (m_asrAccessKeyEdit)
            SetWindowTextW(m_asrAccessKeyEdit, utf8ToWide(YamlConfig::read(yaml, "asr.doubao.access_key")).c_str());
        // Qwen fields
        if (m_asrQwenApiKeyEdit)
            SetWindowTextW(m_asrQwenApiKeyEdit, utf8ToWide(YamlConfig::read(yaml, "asr.qwen.api_key")).c_str());
        break;
    }
    case 1: { // LLM
        std::string enabled = YamlConfig::read(yaml, "llm.enabled");
        if (m_llmEnabledCheck)
            SendMessageW(m_llmEnabledCheck, BM_SETCHECK, (enabled != "false") ? BST_CHECKED : BST_UNCHECKED, 0);
        if (m_llmBaseUrlEdit)
            SetWindowTextW(m_llmBaseUrlEdit, utf8ToWide(YamlConfig::read(yaml, "llm.base_url")).c_str());
        if (m_llmApiKeyEdit)
            SetWindowTextW(m_llmApiKeyEdit, utf8ToWide(YamlConfig::read(yaml, "llm.api_key")).c_str());
        if (m_llmModelEdit)
            SetWindowTextW(m_llmModelEdit, utf8ToWide(YamlConfig::read(yaml, "llm.model")).c_str());
        if (m_llmTokenParamCombo) {
            std::string param = YamlConfig::read(yaml, "llm.max_token_parameter");
            SendMessageW(m_llmTokenParamCombo, CB_SETCURSEL, (param == "max_tokens") ? 1 : 0, 0);
        }
        break;
    }
    case 2: { // Controls
        std::string trigger = YamlConfig::read(yaml, "hotkey.trigger_key");
        std::string cancel = YamlConfig::read(yaml, "hotkey.cancel_key");
        if (m_triggerKeyCombo)
            SendMessageW(m_triggerKeyCombo, CB_SETCURSEL, findHotkeyIndex(trigger), 0);
        if (m_cancelKeyCombo)
            SendMessageW(m_cancelKeyCombo, CB_SETCURSEL, findHotkeyIndex(cancel), 0);

        auto boolVal = [&](const char* key) -> bool {
            std::string v = YamlConfig::read(yaml, key);
            return v == "true";
        };
        if (m_startSoundCheck)
            SendMessageW(m_startSoundCheck, BM_SETCHECK, boolVal("feedback.start_sound") ? BST_CHECKED : BST_UNCHECKED, 0);
        if (m_stopSoundCheck)
            SendMessageW(m_stopSoundCheck, BM_SETCHECK, boolVal("feedback.stop_sound") ? BST_CHECKED : BST_UNCHECKED, 0);
        if (m_errorSoundCheck)
            SendMessageW(m_errorSoundCheck, BM_SETCHECK, boolVal("feedback.error_sound") ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
    }
    case 3: { // Dictionary
        std::string dict = readTextFile("dictionary.txt");
        if (m_dictEdit) SetWindowTextW(m_dictEdit, utf8ToWide(dict).c_str());
        break;
    }
    case 4: { // System Prompt
        std::string prompt = readTextFile("system_prompt.txt");
        if (m_promptEdit) SetWindowTextW(m_promptEdit, utf8ToWide(prompt).c_str());
        break;
    }
    }
}

void SetupWizard::saveConfig() {
    std::string yaml = readConfigFile();

    // ASR
    // We need to collect values from all panes, but controls may be destroyed.
    // Re-read yaml for current state, then apply what's visible.
    // The simplest approach: save all panes by re-reading config and updating only
    // what the user changed on the current tab. But the macOS version saves all at once.
    // Since we destroy controls on tab switch, we'll save on Save button click
    // only the current pane's values. Other panes retain their file values.
    // This is acceptable since the user must click Save while on each tab they edit.
    //
    // Actually, let's save all tabs at once by reading the current visible pane
    // and leaving others unchanged. This is what users expect.

    switch (m_currentTab) {
    case 0: { // ASR
        int provIdx = m_asrProviderCombo
            ? static_cast<int>(SendMessageW(m_asrProviderCombo, CB_GETCURSEL, 0, 0)) : 0;
        if (provIdx < 0 || provIdx >= kAsrProviderCount) provIdx = 0;
        yaml = YamlConfig::write(yaml, "asr.provider", kAsrProviders[provIdx].configValue);

        // Doubao
        if (m_asrAppKeyEdit)
            yaml = YamlConfig::write(yaml, "asr.doubao.app_key", wideToUtf8(getEditText(m_asrAppKeyEdit)));
        if (m_asrAccessKeyEdit)
            yaml = YamlConfig::write(yaml, "asr.doubao.access_key", wideToUtf8(getEditText(m_asrAccessKeyEdit)));
        // Qwen
        if (m_asrQwenApiKeyEdit)
            yaml = YamlConfig::write(yaml, "asr.qwen.api_key", wideToUtf8(getEditText(m_asrQwenApiKeyEdit)));
        // Local model
        if (kAsrProviders[provIdx].isLocal && m_asrModelCombo) {
            int msel = static_cast<int>(SendMessageW(m_asrModelCombo, CB_GETCURSEL, 0, 0));
            if (msel != CB_ERR) {
                char* path = reinterpret_cast<char*>(SendMessageW(m_asrModelCombo, CB_GETITEMDATA, msel, 0));
                if (path) {
                    std::string configKey = std::string("asr.") + kAsrProviders[provIdx].configValue + ".model";
                    yaml = YamlConfig::write(yaml, configKey.c_str(), path);
                }
            }
        }
        break;
    }
    case 1: { // LLM
        if (m_llmEnabledCheck) {
            bool enabled = SendMessageW(m_llmEnabledCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            yaml = YamlConfig::write(yaml, "llm.enabled", enabled ? "true" : "false");
        }
        if (m_llmBaseUrlEdit)
            yaml = YamlConfig::write(yaml, "llm.base_url", wideToUtf8(getEditText(m_llmBaseUrlEdit)));
        if (m_llmApiKeyEdit)
            yaml = YamlConfig::write(yaml, "llm.api_key", wideToUtf8(getEditText(m_llmApiKeyEdit)));
        if (m_llmModelEdit)
            yaml = YamlConfig::write(yaml, "llm.model", wideToUtf8(getEditText(m_llmModelEdit)));
        if (m_llmTokenParamCombo) {
            int sel = static_cast<int>(SendMessageW(m_llmTokenParamCombo, CB_GETCURSEL, 0, 0));
            yaml = YamlConfig::write(yaml, "llm.max_token_parameter",
                                     sel == 1 ? "max_tokens" : "max_completion_tokens");
        }
        break;
    }
    case 2: { // Controls
        if (m_triggerKeyCombo && m_cancelKeyCombo) {
            int triggerIdx = static_cast<int>(SendMessageW(m_triggerKeyCombo, CB_GETCURSEL, 0, 0));
            int cancelIdx = static_cast<int>(SendMessageW(m_cancelKeyCombo, CB_GETCURSEL, 0, 0));
            // Validate: trigger and cancel must be different
            if (triggerIdx == cancelIdx) {
                // Swap cancel to next option
                cancelIdx = (cancelIdx + 1) % kHotkeyOptionCount;
            }
            yaml = YamlConfig::write(yaml, "hotkey.trigger_key",
                                     wideToUtf8(kHotkeyOptions[triggerIdx].configValue));
            yaml = YamlConfig::write(yaml, "hotkey.cancel_key",
                                     wideToUtf8(kHotkeyOptions[cancelIdx].configValue));
        }
        if (m_startSoundCheck)
            yaml = YamlConfig::write(yaml, "feedback.start_sound",
                SendMessageW(m_startSoundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false");
        if (m_stopSoundCheck)
            yaml = YamlConfig::write(yaml, "feedback.stop_sound",
                SendMessageW(m_stopSoundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false");
        if (m_errorSoundCheck)
            yaml = YamlConfig::write(yaml, "feedback.error_sound",
                SendMessageW(m_errorSoundCheck, BM_GETCHECK, 0, 0) == BST_CHECKED ? "true" : "false");
        break;
    }
    case 3: { // Dictionary
        if (m_dictEdit) writeTextFile("dictionary.txt", wideToUtf8(getEditText(m_dictEdit)));
        break;
    }
    case 4: { // System Prompt
        if (m_promptEdit) writeTextFile("system_prompt.txt", wideToUtf8(getEditText(m_promptEdit)));
        break;
    }
    }

    // Write config yaml (for tabs 0-2)
    if (m_currentTab <= 2) {
        writeConfigFile(yaml);
    }
}
