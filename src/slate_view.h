#ifndef SLATE_VIEW_H
#define SLATE_VIEW_H

#include <windows.h>
#include <windowsx.h>
#include "slate_doc.h"

#ifndef EN_SELCHANGE
#define EN_SELCHANGE        0x8002
#endif

typedef struct {
    SlateDoc* pDoc;
    int scrollY;
    int lineHeight;
    HFONT hFont;
    size_t cursorOffset;    // This is the active end of the selection
    size_t selectionAnchor; // This is where the selection started
    BOOL isDragging;
    BOOL bInsertMode;
    BOOL bWordWrap;
    BOOL bShowNonPrintable;
    COLORREF colorBg;
    COLORREF colorText;
    COLORREF colorDim;  // For non-printables
    BOOL bUseSystemColors;
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

BOOL View_GetShowNonPrintable(HWND hwnd);
BOOL View_IsInsertMode(HWND hwnd);
BOOL View_IsUsingSystemColors(HWND hwnd);

#endif