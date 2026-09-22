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

typedef struct VisualLineInfo {
    size_t logicalLine;      // Which logical line this belongs to
    size_t startOffset;      // Start offset within the logical line
    size_t length;           // Number of characters in this visual line
    int yPosition;           // Y position in document space
} VisualLineInfo;

// Documents longer than this (in UTF-16 units, ~8 MB of ASCII) open unwrapped: the wrap
// layout is built for the whole document, which takes a few seconds at this size.
#define VIEW_MAX_WRAP_UNITS ((size_t)8 * 1024 * 1024)

typedef struct {
    SlateDoc* pDoc;
    size_t docGeneration;  // Track when document changes
    size_t cachedDocGeneration;  // Track which doc the cache is for
    int scrollY;
    int scrollX;
    int lineHeight;
    HFONT hFont;
    size_t cursorOffset;    // This is the active end of the selection
    size_t selectionAnchor; // This is where the selection started
    BOOL isDragging;
    BOOL bInsertMode;
    BOOL bWordWrap;         // Effective wrap state
    BOOL bWrapAllowed;      // FALSE when the document is too large to lay out wrapped
    BOOL bWrapSuppressed;   // Wrap was requested but is held off for a too-large document
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
    // Manual wrap cache
    VisualLineInfo* visualLines;
    size_t visualLineCount;
    size_t visualLineCapacity;
    int cachedWrapWidth;
    BOOL wrapCacheValid;
    
    // Incremental wrap cache state. Pending edits since the last rebuild replaced the
    // cached logical lines [firstDirtyLine, dirtyOldEndLine) (in the cache's own line
    // numbering) with (dirtyOldEndLine - firstDirtyLine + dirtyLineDelta) new lines.
    // firstDirtyLine == SIZE_MAX means no incremental information is pending: either
    // the cache is clean, or (with wrapCacheValid FALSE) a full rebuild is needed.
    size_t firstDirtyLine;
    size_t dirtyOldEndLine;
    long dirtyLineDelta;       // Change in document line count (+N or -N)

    // Cached max line width (unwrapped mode's horizontal scrollbar range). Only ever safe
    // to grow incrementally without a full rescan - see View_GetDocumentWidth.
    int cachedDocWidth;
    BOOL docWidthValid;
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
BOOL View_GetWordWrap(HWND hwnd);
BOOL View_IsWordWrapAllowed(HWND hwnd);
void View_SetShowNonPrintable(HWND hwnd, BOOL bShow);
void View_SetDefaultColors(HWND hwnd);
void View_UseSystemColors(HWND hwnd);
void View_SetInsertMode(HWND hwnd, BOOL bInsert);
BOOL View_ApplySearchResult(HWND hwnd, const DocSearchResult* result);

BOOL View_GetShowNonPrintable(HWND hwnd);
BOOL View_IsInsertMode(HWND hwnd);
BOOL View_IsUsingSystemColors(HWND hwnd);

#endif
