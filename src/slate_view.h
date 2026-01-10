#ifndef SLATE_VIEW_H
#define SLATE_VIEW_H

#include <windows.h>
#include <windowsx.h>
#include "slate_doc.h"

typedef struct {
    SlateDoc* pDoc;
    int scrollY;
    int lineHeight;
    HFONT hFont;
    size_t cursorOffset;    // This is the active end of the selection
    size_t selectionAnchor; // This is where the selection started
    BOOL isDragging;
    BOOL bInsertMode;
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

BOOL View_IsInsertMode(HWND hwnd);

#endif