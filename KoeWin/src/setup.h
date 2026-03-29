#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <vector>
#include <string>

class SetupWizardDelegate {
public:
    virtual ~SetupWizardDelegate() = default;
    virtual void setupWizardDidSaveConfig() = 0;
};

class SetupWizard {
public:
    SetupWizard(HINSTANCE hInstance, SetupWizardDelegate* delegate);
    ~SetupWizard();

    void show();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void createWindow();
    void handleCommand(WPARAM wParam, LPARAM lParam);
    void onTabChanged();
    void destroyPaneControls();

    void createAsrPane();
    void createLlmPane();
    void createControlsPane();
    void createDictionaryPane();
    void createPromptPane();

    void onAsrProviderChanged();
    void updateModelStatus();
    void startModelDownload();
    void cancelModelDownload();
    void deleteSelectedModel();

    void loadCurrentValues();
    void saveConfig();

    std::string readConfigFile();
    void writeConfigFile(const std::string& yaml);
    std::string readTextFile(const std::string& filename);
    void writeTextFile(const std::string& filename, const std::string& content);
    std::string configDir();

    HINSTANCE m_hInstance;
    HWND m_hwnd = nullptr;
    HWND m_tabCtrl = nullptr;
    SetupWizardDelegate* m_delegate;
    int m_currentTab = -1;

    // Controls per-pane (destroyed on tab switch)
    std::vector<HWND> m_paneControls;

    // ASR pane
    HWND m_asrProviderCombo = nullptr;
    // Doubao fields
    HWND m_asrAppKeyLabel = nullptr;
    HWND m_asrAppKeyEdit = nullptr;
    HWND m_asrAccessKeyLabel = nullptr;
    HWND m_asrAccessKeyEdit = nullptr;
    HWND m_asrAccessKeyToggle = nullptr;
    bool m_asrAccessKeyVisible = false;
    // Qwen fields
    HWND m_asrQwenApiKeyLabel = nullptr;
    HWND m_asrQwenApiKeyEdit = nullptr;
    HWND m_asrQwenApiKeyToggle = nullptr;
    bool m_asrQwenApiKeyVisible = false;
    // Local model fields
    HWND m_asrModelLabel = nullptr;
    HWND m_asrModelCombo = nullptr;
    HWND m_asrModelStatusLabel = nullptr;
    HWND m_asrDownloadBtn = nullptr;
    HWND m_asrDeleteBtn = nullptr;
    HWND m_asrProgressBar = nullptr;
    HWND m_asrProgressLabel = nullptr;
    bool m_asrDownloading = false;
    std::string m_downloadingModelPath;

    // LLM pane
    HWND m_llmEnabledCheck = nullptr;
    HWND m_llmBaseUrlEdit = nullptr;
    HWND m_llmApiKeyEdit = nullptr;
    bool m_llmApiKeyVisible = false;
    HWND m_llmModelEdit = nullptr;
    HWND m_llmTokenParamCombo = nullptr;

    // Controls pane
    HWND m_triggerKeyCombo = nullptr;
    HWND m_triggerKeyEdit = nullptr;
    HWND m_cancelKeyCombo = nullptr;
    HWND m_cancelKeyEdit = nullptr;
    HWND m_startSoundCheck = nullptr;
    HWND m_stopSoundCheck = nullptr;
    HWND m_errorSoundCheck = nullptr;

    // Dictionary pane
    HWND m_dictEdit = nullptr;

    // System Prompt pane
    HWND m_promptEdit = nullptr;

    // Shared buttons
    HWND m_saveButton = nullptr;
    HWND m_cancelButton = nullptr;

    HFONT m_font = nullptr;
};
