#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

#define TIMER_OVERLAY_ANIM 3002

class OverlayPanel {
public:
    explicit OverlayPanel(HINSTANCE hInstance);
    ~OverlayPanel();

    void updateState(const char* state);
    void updateInterimText(const wchar_t* text);
    void onAnimationTimer();

private:
    void createWindow(HINSTANCE hInstance);
    void show();
    void hide();
    void render();
    void resizeAndCenter(bool animated);

    HWND m_hwnd = nullptr;
    const char* m_currentState = "idle";
    int m_tick = 0;
    float m_alpha = 0.0f;
    bool m_visible = false;
    bool m_hiding = false;

    // State-derived rendering params
    const wchar_t* m_statusText = L"";
    std::wstring m_interimText;
    int m_mode = 0;  // 0=none, 1=waveform, 2=processing, 3=success, 4=error
    COLORREF m_accentColor = RGB(255, 255, 255);

    // Current window geometry (updated immediately by resizeAndCenter)
    int m_currentX = 0, m_currentY = 0, m_currentW = 200, m_currentH = 36;

    // Stabilization: only-grow during a recording session
    float m_sessionMaxWidth = 0.0f;
    float m_sessionMaxHeight = 0.0f;

    // Layout width for text wrapping (prevents wrapping artifacts during animated resize)
    float m_layoutWidth = 200.0f;

    static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
