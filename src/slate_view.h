#ifndef SLATE_VIEW_H
#define SLATE_VIEW_H

#include <windows.h>
#include <windowsx.h>
#include "slate_doc.h"
#include "slate_commands.h"

#ifndef EN_SELCHANGE
#define EN_SELCHANGE        0x8002
#endif

#ifndef IDT_CARET
#define IDT_CARET 1001
#endif

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

#define CARET_IDLE_TIMEOUT 12000 // ms before switching to idle caret animation

typedef struct {
    SlateDoc* pDoc;
    int scrollY;
    int scrollX;
    int lineHeight;
    HFONT hFont;
    size_t cursorOffset;    // This is the active end of the selection
    size_t selectionAnchor; // This is where the selection started
    BOOL isDragging;
    BOOL bInsertMode;
    BOOL bWordWrap;
    BOOL bShowNonPrintable;
    COLORREF colorBg;
    COLORREF colorBgDim;
    COLORREF colorText;
    COLORREF colorDim;  // For non-printables
    BOOL bUseSystemColors;
    BOOL bCommandMode;
    WCHAR szCommandBuf[256];
    size_t commandLen;
    size_t commandCaretPos;
    BOOL bCommandFeedback;
    BOOL bCommandFeedbackHasCaret;
    int  commandFeedbackCaretCol;
    WCHAR szCommandFeedback[256];
    HBITMAP hCaretBm;  // Persistent bitmap for the caret
    float caretAlpha;          // 0.0 to 1.0
    int   caretDirection;      // 1 for fading in, -1 for fading out
    double animationTime; // Total elapsed time in milliseconds
    DWORD lastActivity;        // Timestamp of last key press
    int   caretX, caretY;      // Current position
} ViewState;

// Register the custom "SlateView" window class
BOOL View_Register(HINSTANCE hInstance);

size_t View_GetCursorOffset(HWND hwnd);

// Viewport settings accessors
void View_SetDocument(HWND hwnd, SlateDoc* pDoc);
void View_ScrollTo(HWND hwnd, int yOffset);
void View_UpdateMetrics(HWND hwnd);
void View_Undo(HWND hwnd);
void View_SelectAll(HWND hwnd);
void View_Copy(HWND hwnd);
void View_Cut(HWND hwnd);
void View_Paste(HWND hwnd);
void View_SetWordWrap(HWND hwnd, BOOL bWrap);
void View_SetShowNonPrintable(HWND hwnd, BOOL bShow);
void View_SetDefaultColors(HWND hwnd);
void View_UseSystemColors(HWND hwnd);
void View_SetInsertMode(HWND hwnd, BOOL bInsert);
BOOL View_ApplySearchResult(HWND hwnd, const DocSearchResult* result);

BOOL View_GetShowNonPrintable(HWND hwnd);
BOOL View_IsInsertMode(HWND hwnd);
BOOL View_IsUsingSystemColors(HWND hwnd);

#endif
