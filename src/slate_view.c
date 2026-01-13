#include "slate_view.h"
#include "slate_doc.h"
#include <tchar.h>

static ViewState* GetState(HWND hwnd) {
    return (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
}

void View_SetDefaultColors(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    if (!pState) return;
    
    pState->bUseSystemColors = FALSE;
    pState->colorBg   = RGB(0xE6, 0xE3, 0xDA); 
    pState->colorText = RGB(0x22, 0x22, 0x22); 
    pState->colorDim  = RGB(150, 150, 150);    
}

void View_UseSystemColors(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    if (!pState) return;

    pState->bUseSystemColors = TRUE;
    pState->colorBg   = GetSysColor(COLOR_WINDOW);
    pState->colorText = GetSysColor(COLOR_WINDOWTEXT);
    pState->colorDim  = RGB(180, 180, 180);
}

BOOL View_IsUsingSystemColors(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    return pState ? pState->bUseSystemColors : TRUE;
}

void EnsureCursorVisible(HWND hwnd, ViewState* pState) {
    int line, col;
    Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &line, &col);
    int cursorY = (line - 1) * pState->lineHeight;
    
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (cursorY < pState->scrollY) {
        pState->scrollY = cursorY;
    } else if (cursorY + pState->lineHeight > pState->scrollY + rc.bottom) {
        pState->scrollY = cursorY - rc.bottom + pState->lineHeight;
    }
    // Keep the scrollbar in sync with the visible caret
    SetScrollPos(hwnd, SB_VERT, pState->scrollY, TRUE);
}


void UpdateScrollbars(HWND hwnd, ViewState* pState) {
    if (!pState || !pState->pDoc) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientHeight = rc.bottom;

    // Use 64-bit math to calculate the true document height
    long long totalHeight64 = (long long)pState->pDoc->line_count * pState->lineHeight;
    
    // Windows scrollbars are limited to 32-bit values; clamp extremely tall documents to avoid overflow
    int totalDocHeight;
    if (totalHeight64 > 2147483647) {
        totalDocHeight = 2147483647;
    } else {
        totalDocHeight = (int)totalHeight64;
    }

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nPos = pState->scrollY;
    if (pState->bWordWrap) {
        // Line-based scrolling
        // Range = Number of Logical Lines
        // Page = Approximate lines per screen (e.g., 20)
        si.nMax = (int)pState->pDoc->line_count; 
        si.nPage = rc.bottom / pState->lineHeight; 
    } else {
        // Pixel-based scrolling (Safe 64-bit clamp)
        long long totalHeight = (long long)pState->pDoc->line_count * pState->lineHeight;
        si.nMax = (totalHeight > 2147483647) ? 2147483647 : (int)totalHeight;
        si.nPage = rc.bottom;
    }

    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void View_SetDocument(HWND hwnd, SlateDoc* pDoc) {
    ViewState* pState = GetState(hwnd);
    if (pState) {
        pState->pDoc = pDoc;
        pState->cursorOffset = 0;
        pState->scrollY = 0;
        
        if (pDoc) {
            Doc_UpdateLineMap(pDoc); 
        }
        
        // Reclaim caret and focus
        if (GetFocus() == hwnd) {
            DestroyCaret();
            CreateCaret(hwnd, NULL, 1, pState->lineHeight);
            SetCaretPos(5, 0);
            ShowCaret(hwnd);
        }

        // Force scrollbar and paint update
        RECT rc;
        GetClientRect(hwnd, &rc);
        SendMessage(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
        UpdateScrollbars(hwnd, pState);
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

// LINKER FIX: Definition for View_GetCursorOffset
size_t View_GetCursorOffset(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    return pState ? pState->cursorOffset : 0;
}

// Helper to convert mouse coordinates to logical offset
size_t GetOffsetFromPoint(HWND hwnd, ViewState* pState, int x, int y) {
    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);
    
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int charWidth = tm.tmAveCharWidth;
    ReleaseDC(hwnd, hdc);

    // Map the y coordinate to a line index
    int lineIndex = (y + pState->scrollY) / pState->lineHeight;
    
    // Clamp to valid line range
    if (lineIndex < 0) lineIndex = 0;
    if (lineIndex >= (int)pState->pDoc->line_count) 
        lineIndex = (int)pState->pDoc->line_count - 1;

    // Map the x coordinate to a column, subtracting the left margin and rounding to the nearest char
    int col = (x - 5 + (charWidth / 2)) / charWidth;
    
    size_t lineStart = Doc_GetLineOffset(pState->pDoc, lineIndex);
    size_t lineEnd = Doc_GetLineOffset(pState->pDoc, lineIndex + 1);
    size_t lineLen = lineEnd - lineStart;

    // Ignore trailing newline characters when positioning the cursor
    if (lineLen > 0) {
        WCHAR last;
        Doc_GetText(pState->pDoc, lineEnd - 1, 1, &last);
        if (last == L'\n' || last == L'\r') {
            lineLen--;
            // Double check for \r\n pairs
            if (lineLen > 0) {
                Doc_GetText(pState->pDoc, lineEnd - 2, 1, &last);
                if (last == L'\r') lineLen--;
            }
        }
    }

    // Clamp within the line bounds
    if (col < 0) col = 0;
    if ((size_t)col > lineLen) col = (int)lineLen;

    return lineStart + col;
}

BOOL View_IsInsertMode(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    return pState ? pState->bInsertMode : TRUE;
}

void View_SetShowNonPrintable(HWND hwnd, BOOL bShow) {
    ViewState* pState = GetState(hwnd);
    if (pState) {
        pState->bShowNonPrintable = bShow;
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

BOOL View_GetShowNonPrintable(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    return pState ? pState->bShowNonPrintable : FALSE;
}

void NotifyParent(HWND hwnd, UINT code) {
    // Send EN_CHANGE to parent, mimicking standard Edit control behavior
    SendMessage(GetParent(hwnd), WM_COMMAND, 
                MAKEWPARAM(GetWindowLongPtr(hwnd, GWLP_ID), code), 
                (LPARAM)hwnd);
}

void View_Undo(HWND hwnd) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!pState || !pState->pDoc) return;

    size_t restoredCursor;
    if (Doc_Undo(pState->pDoc, &restoredCursor)) {
        pState->cursorOffset = restoredCursor;
        pState->selectionAnchor = restoredCursor;
        
        // Ensure the screen follows the cursor after the undo
        EnsureCursorVisible(hwnd, pState);
        InvalidateRect(hwnd, NULL, FALSE);
        NotifyParent(hwnd, EN_CHANGE);
    }
}

void View_Redo(HWND hwnd) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!pState || !pState->pDoc) return;

    size_t restoredCursor;
    if (Doc_Redo(pState->pDoc, &restoredCursor)) {
        pState->cursorOffset = restoredCursor;
        pState->selectionAnchor = restoredCursor;
        
        // Ensure the screen follows the cursor after the redo
        EnsureCursorVisible(hwnd, pState);
        InvalidateRect(hwnd, NULL, FALSE);
        NotifyParent(hwnd, EN_CHANGE);
    }
}

void View_SelectAll(HWND hwnd) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!pState || !pState->pDoc) return;

    pState->selectionAnchor = 0;
    pState->cursorOffset = pState->pDoc->total_length;

    InvalidateRect(hwnd, NULL, FALSE);
}

void View_Copy(HWND hwnd) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!pState || !pState->pDoc) return;

    if (pState->cursorOffset == pState->selectionAnchor) return;

    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                 (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(WCHAR));
        if (hMem) {
            WCHAR* pMem = (WCHAR*)GlobalLock(hMem);
            Doc_GetText(pState->pDoc, start, len, pMem);
            pMem[len] = L'\0';
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
        CloseClipboard();
    }
}

void View_Cut(HWND hwnd) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (pState->cursorOffset == pState->selectionAnchor) return;

    // Copy the selection to the clipboard
    View_Copy(hwnd);

    // Delete the selection from the document
    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                 (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);

    Doc_Delete(pState->pDoc, start, len);
    
    // Collapse selection and update the view
    pState->cursorOffset = pState->selectionAnchor = start;
    NotifyParent(hwnd, EN_CHANGE);
    InvalidateRect(hwnd, NULL, FALSE);
}

void View_Paste(HWND hwnd) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;

    if (OpenClipboard(hwnd)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            WCHAR* pText = (WCHAR*)GlobalLock(hData);
            if (pText) {
                // If there is a selection, delete it first
                if (pState->cursorOffset != pState->selectionAnchor) {
                    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
                    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                                 (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);
                    Doc_Delete(pState->pDoc, start, len);
                    pState->cursorOffset = pState->selectionAnchor = start;
                }

                // Insert the clipboard text
                size_t pasteLen = wcslen(pText);
                Doc_Insert(pState->pDoc, pState->cursorOffset, pText, pasteLen);
                pState->cursorOffset += pasteLen;
                pState->selectionAnchor = pState->cursorOffset;
                
                GlobalUnlock(hData);
                NotifyParent(hwnd, EN_CHANGE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        CloseClipboard();
    }
}

void View_SetWordWrap(HWND hwnd, BOOL bWrap) {
    ViewState* pState = GetState(hwnd);
    if (pState && pState->bWordWrap != bWrap) {
        pState->bWordWrap = bWrap;
        pState->scrollY = 0; // Reset scroll to avoid getting lost
        UpdateScrollbars(hwnd, pState);
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    ViewState* pState = GetState(hwnd);

    switch (uMsg) {
        case WM_CREATE: {
            pState = (ViewState*)calloc(1, sizeof(ViewState));
            pState->lineHeight = 20;
            pState->bInsertMode = TRUE; // Default to insert mode
            pState->hFont = CreateFont(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                                       CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pState);
            View_SetDefaultColors(hwnd);
            UpdateScrollbars(hwnd, pState);
            CreateCaret(hwnd, NULL, 2, pState->lineHeight);
            ShowCaret(hwnd);
            return 0;
        }
        case WM_ERASEBKGND:
            // Return non-zero to tell Windows we handled it and avoid the pre-paint flash
            return 1;

        case WM_CHAR: {
            WCHAR c = (WCHAR)wParam;
            
            // Skip control characters that are processed in WM_KEYDOWN
            if (c == 8 || c == 127) return 0;
            
            if (c == L'\r' || c == L'\n' || (c >= 32) || c == L'\t') {
                if (c == L'\r') c = L'\n'; // Normalize to LF

                // Replace any highlighted selection with the typed character
                if (pState->cursorOffset != pState->selectionAnchor) {
                    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
                    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                                (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);
                    
                    Doc_Delete(pState->pDoc, start, len);
                    pState->cursorOffset = pState->selectionAnchor = start;
                } else if (!pState->bInsertMode && c != L'\n') {
                    // Overtype the next character when not in insert mode
                    if (pState->cursorOffset < pState->pDoc->total_length) {
                        WCHAR nextChar;
                        Doc_GetText(pState->pDoc, pState->cursorOffset, 1, &nextChar);
                        // Don't overtype the newline; it breaks the line map logic
                        if (nextChar != L'\n') {
                            Doc_Delete(pState->pDoc, pState->cursorOffset, 1);
                        }
                    }
                }

                // Insert the character and collapse the selection
                Doc_Insert(pState->pDoc, pState->cursorOffset, &c, 1);
                pState->cursorOffset++;
                pState->selectionAnchor = pState->cursorOffset;

                // Keep the caret visible after newline insertion
                if (c == L'\n') {
                    EnsureCursorVisible(hwnd, pState);
                }

                // Notify the parent for title/status updates
                NotifyParent(hwnd, EN_CHANGE);

                InvalidateRect(hwnd, NULL, TRUE);
                UpdateWindow(hwnd); // Ensure immediate visual feedback
            }
            UpdateScrollbars(hwnd, pState);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            UINT scrollLines = 3;
            SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scrollLines, 0);
            
            int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            int linesToScroll = (zDelta / WHEEL_DELTA) * scrollLines;
            
            RECT rc;
            GetClientRect(hwnd, &rc);
            int clientHeight = rc.bottom;
            
            // Calculate safe limits using 64-bit math
            long long totalHeight64 = (long long)pState->pDoc->line_count * pState->lineHeight;
            int totalDocHeight = (totalHeight64 > 2147483647) ? 2147483647 : (int)totalHeight64;
            
            // Determine the maximum scroll position
            int maxScroll = totalDocHeight - clientHeight;
            if (maxScroll < 0) maxScroll = 0;

            // Apply the scroll delta
            pState->scrollY -= (linesToScroll * pState->lineHeight);

            // Clamp within the document
            if (pState->scrollY < 0) pState->scrollY = 0;
            if (pState->scrollY > maxScroll) pState->scrollY = maxScroll;

            UpdateScrollbars(hwnd, pState);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_KEYDOWN: {
            int line, col;
            Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &line, &col);
            BOOL isShiftPressed = (GetKeyState(VK_SHIFT) & 0x8000);
            BOOL isCtrlPressed = (GetKeyState(VK_CONTROL) & 0x8000);

            switch (wParam) {
                case 'Z':
                    if (isCtrlPressed) { View_Undo(hwnd); return 0; }
                    break;
                case 'Y':
                    if (isCtrlPressed) { View_Redo(hwnd); return 0; }
                    break;
                case 'X':
                    if (isCtrlPressed) { View_Cut(hwnd); return 0; }
                    break;
                case 'C':
                    if (isCtrlPressed) { View_Copy(hwnd); return 0; }
                    break;
                case 'V':
                    if (isCtrlPressed) { View_Paste(hwnd); return 0; }
                    break;
                case 'A':
                    if (isCtrlPressed) { View_SelectAll(hwnd); return 0; }
                    break;
                case VK_LEFT:
                    if (pState->cursorOffset > 0) pState->cursorOffset--;
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;

                case VK_RIGHT:
                    if (pState->cursorOffset < pState->pDoc->total_length) pState->cursorOffset++;
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;

                case VK_UP:
                    if (line > 1) {
                        size_t prevLineStart = Doc_GetLineOffset(pState->pDoc, line - 2);
                        size_t prevLineEnd = Doc_GetLineOffset(pState->pDoc, line - 1);
                        size_t prevLen = prevLineEnd - prevLineStart;
                        size_t newCol = (col > (int)prevLen) ? prevLen : (size_t)col;
                        pState->cursorOffset = prevLineStart + (newCol > 0 ? newCol - 1 : 0);
                    }
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;

                case VK_DOWN:
                    if (line < (int)pState->pDoc->line_count) {
                        size_t nextLineStart = Doc_GetLineOffset(pState->pDoc, line);
                        size_t nextLineEnd = Doc_GetLineOffset(pState->pDoc, line + 1);
                        size_t nextLen = nextLineEnd - nextLineStart;
                        size_t newCol = (col > (int)nextLen) ? nextLen : (size_t)col;
                        pState->cursorOffset = nextLineStart + (newCol > 0 ? newCol - 1 : 0);
                    }
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;

                case VK_HOME:
                    pState->cursorOffset = Doc_GetLineOffset(pState->pDoc, line - 1);
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;

                case VK_END:
                    pState->cursorOffset = Doc_GetLineOffset(pState->pDoc, line);
                    // Don't land past the newline
                    if (pState->cursorOffset > 0) {
                        WCHAR last;
                        Doc_GetText(pState->pDoc, pState->cursorOffset - 1, 1, &last);
                        if (last == L'\n' || last == L'\r') pState->cursorOffset--;
                    }
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;

                case VK_BACK:
                case VK_DELETE:
                    if (pState->cursorOffset != pState->selectionAnchor) {
                        // Delete selection
                        size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
                        size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                                    (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);
                        Doc_Delete(pState->pDoc, start, len);
                        pState->cursorOffset = pState->selectionAnchor = start;
                        NotifyParent(hwnd, EN_CHANGE);
                    } else {
                        // Standard single char delete
                        if (wParam == VK_BACK && pState->cursorOffset > 0) {
                            Doc_Delete(pState->pDoc, --pState->cursorOffset, 1);
                            pState->selectionAnchor = pState->cursorOffset;
                            NotifyParent(hwnd, EN_CHANGE);
                        } else if (wParam == VK_DELETE && pState->cursorOffset < pState->pDoc->total_length) {
                            Doc_Delete(pState->pDoc, pState->cursorOffset, 1);
                            NotifyParent(hwnd, EN_CHANGE);
                        }
                    }
                    break;

                case VK_INSERT:
                    if (isCtrlPressed) { View_Copy(hwnd); return 0; }
                    if (isShiftPressed) { View_Paste(hwnd); return 0; }

                    // If no modifiers, just toggle insert/overwrite mode
                    pState->bInsertMode = !pState->bInsertMode;
                    NotifyParent(hwnd, EN_SELCHANGE);
                    break;
            }
            UpdateScrollbars(hwnd, pState);
            EnsureCursorVisible(hwnd, pState);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Initialize double buffering
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

            // Use the colors from state instead of system defaults
            COLORREF currentBg   = pState->colorBg;
            COLORREF currentText = pState->colorText;
            COLORREF currentDim  = pState->colorDim;
            
            SelectObject(memDC, pState->hFont);

            // Prepare font metrics
            TEXTMETRIC tm;
            GetTextMetrics(memDC, &tm);
            int charWidth = tm.tmAveCharWidth;
            int tabStops = charWidth * 4; // Standard 4-character tab width

            size_t selStart = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
            size_t selEnd = (pState->cursorOffset < pState->selectionAnchor) ? pState->selectionAnchor : pState->cursorOffset;
            BOOL hasFocus = (GetFocus() == hwnd);

            // Clear the back buffer
            HBRUSH hBg = CreateSolidBrush(currentBg);
            FillRect(memDC, &rc, hBg);
            DeleteObject(hBg);

            if (pState->pDoc && pState->pDoc->line_count > 0) {
                if (pState->bWordWrap) {
                    // --- WORD WRAP MODE ---
                    int currentY = 0;
                    size_t startLine = (size_t)pState->scrollY;
                    if (startLine >= pState->pDoc->line_count) startLine = pState->pDoc->line_count - 1;

                    RECT textRect = rc;
                    textRect.left += 5; 
                    textRect.right -= 5;

                    for (size_t i = startLine; i < pState->pDoc->line_count; i++) {
                        size_t lineStart = Doc_GetLineOffset(pState->pDoc, i);
                        size_t lineEnd = (i + 1 < pState->pDoc->line_count) ? 
                                         Doc_GetLineOffset(pState->pDoc, i + 1) : pState->pDoc->total_length;
                        size_t len = lineEnd - lineStart;
                        
                        WCHAR* buf = malloc((len + 1) * sizeof(WCHAR));
                        if (buf) {
                            Doc_GetText(pState->pDoc, lineStart, len, buf);
                            size_t dLen = len;
                            while (dLen > 0 && (buf[dLen-1] == L'\n' || buf[dLen-1] == L'\r')) dLen--;
                            buf[dLen] = 0;

                            // Measure text height (respecting tabs)
                            RECT measureRect = textRect;
                            measureRect.top = currentY;
                            DrawTextW(memDC, buf, (int)dLen, &measureRect, DT_WORDBREAK | DT_CALCRECT | DT_EXPANDTABS);
                            int height = measureRect.bottom - measureRect.top;

                            RECT drawRect = textRect;
                            drawRect.top = currentY;
                            drawRect.bottom = currentY + height;
                            
                            // Draw actual text first
                            DrawTextW(memDC, buf, (int)dLen, &drawRect, DT_WORDBREAK | DT_EXPANDTABS);

                            // Overlay symbols separately to prevent shifting layout
                            if (pState->bShowNonPrintable) {
                                COLORREF oldClr = SetTextColor(memDC, currentDim);
                                int oldBk = SetBkMode(memDC, TRANSPARENT);
                                
                                if (lineEnd < pState->pDoc->total_length) {
                                    WCHAR pilcrow = 0x00B6;
                                    DrawTextW(memDC, &pilcrow, 1, &drawRect, DT_WORDBREAK | DT_EXPANDTABS | DT_RIGHT | DT_BOTTOM);
                                }
                                if (i == pState->pDoc->line_count - 1) {
                                    DrawTextW(memDC, L"[EOF]", 5, &drawRect, DT_WORDBREAK | DT_EXPANDTABS | DT_RIGHT | DT_BOTTOM);
                                }
                                SetTextColor(memDC, oldClr);
                                SetBkMode(memDC, oldBk);
                                SetBkMode(memDC, OPAQUE);
                            }
                            free(buf);
                            currentY += height;
                        }
                        if (currentY > rc.bottom) break;
                    }
                } else {
                    // --- NO WORD WRAP MODE ---
                    size_t first = (size_t)(pState->scrollY / pState->lineHeight);
                    size_t last = (size_t)((pState->scrollY + rc.bottom) / pState->lineHeight);
                    
                    for (size_t i = first; i <= last && i < pState->pDoc->line_count; i++) {
                        size_t lineStart = Doc_GetLineOffset(pState->pDoc, i);
                        size_t lineEnd = (i + 1 < pState->pDoc->line_count) ? 
                                         Doc_GetLineOffset(pState->pDoc, i + 1) : pState->pDoc->total_length;
                        size_t len = lineEnd - lineStart;

                        WCHAR* buf = malloc((len + 1) * sizeof(WCHAR));
                        if (!buf) continue;
                        Doc_GetText(pState->pDoc, lineStart, len, buf);
                        
                        size_t dLen = len;
                        while (dLen > 0 && (buf[dLen-1] == L'\n' || buf[dLen-1] == L'\r')) dLen--;
                        int lineY = (int)(((long long)i * pState->lineHeight) - (long long)pState->scrollY);

                        // Pass 1: Draw the background text (Tabs expand correctly)
                        SetTextColor(memDC, currentText);
                        SetBkColor(memDC, currentBg);
                        TabbedTextOutW(memDC, 5, lineY, buf, (int)dLen, 1, &tabStops, 5);

                        // Pass 2: Overlay Symbols (Non-Printable)
                        if (pState->bShowNonPrintable) {
                            COLORREF oldClr = SetTextColor(memDC, currentDim);
                            SetBkMode(memDC, TRANSPARENT);
                            
                            for (size_t k = 0; k < dLen; k++) {
                                if (buf[k] == L' ' || buf[k] == L'\t') {
                                    WCHAR sym = (buf[k] == L' ') ? 0x00B7 : 0x00BB;
                                    // Measure extent WITHOUT the current character to find the starting X
                                    DWORD extent = GetTabbedTextExtentW(memDC, buf, (int)k, 1, &tabStops);
                                    TextOutW(memDC, 5 + LOWORD(extent), lineY, &sym, 1);
                                }
                            }
                            DWORD lineExtent = GetTabbedTextExtentW(memDC, buf, (int)dLen, 1, &tabStops);
                            WCHAR pilcrow = 0x00B6;
                            TextOutW(memDC, 5 + LOWORD(lineExtent), lineY, &pilcrow, 1);

                            if (i == pState->pDoc->line_count - 1) {
                                TextOutW(memDC, 10 + LOWORD(lineExtent), lineY, L"[EOF]", 5);
                            }
                            SetTextColor(memDC, oldClr);
                        }

                        // Pass 3: Selection Overlay (Tab aware)
                        if (selStart != selEnd && selStart < lineEnd && selEnd > lineStart) {
                            size_t intersectStart = (selStart > lineStart) ? selStart : lineStart;
                            size_t intersectEnd = (selEnd < lineEnd) ? selEnd : lineEnd;

                            if (intersectStart < intersectEnd) {
                                DWORD ext1 = GetTabbedTextExtentW(memDC, buf, (int)(intersectStart - lineStart), 1, &tabStops);
                                DWORD ext2 = GetTabbedTextExtentW(memDC, buf, (int)(intersectEnd - lineStart), 1, &tabStops);
                                
                                int x1 = 5 + LOWORD(ext1);
                                int x2 = 5 + LOWORD(ext2);
                                RECT selRect = { x1, lineY, x2, lineY + pState->lineHeight };

                                HBRUSH hSelBrush = CreateSolidBrush(hasFocus ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_3DFACE));
                                FillRect(memDC, &selRect, hSelBrush);
                                DeleteObject(hSelBrush);

                                SetTextColor(memDC, hasFocus ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_BTNTEXT));
                                SetBkMode(memDC, TRANSPARENT);
                                
                                int selTextLen = (int)(intersectEnd - intersectStart);
                                WCHAR* selTextPtr = buf + (intersectStart - lineStart);
                                while (selTextLen > 0 && (selTextPtr[selTextLen-1] == L'\n' || selTextPtr[selTextLen-1] == L'\r')) selTextLen--;
                                
                                if (selTextLen > 0) {
                                    TabbedTextOutW(memDC, x1, lineY, selTextPtr, selTextLen, 1, &tabStops, x1);
                                }
                            }
                        }

                        // Update Caret Position
                        if (pState->cursorOffset >= lineStart && pState->cursorOffset <= lineEnd) {
                            BOOL isLastLine = (i == pState->pDoc->line_count - 1);
                            BOOL isWithinLine = (pState->cursorOffset < lineEnd) || (isLastLine && pState->cursorOffset == lineEnd);
                            
                            if (isWithinLine) {
                                DWORD caretExtent = GetTabbedTextExtentW(memDC, buf, (int)(pState->cursorOffset - lineStart), 1, &tabStops);
                                int caretX = 5 + LOWORD(caretExtent);
                                if (lineY >= 0 && lineY < rc.bottom) SetCaretPos(caretX, lineY);
                            }
                        }
                        free(buf);
                    }
                }
            }

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBM);
            DeleteObject(memBM);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (pState->isDragging) {
                size_t newOffset = GetOffsetFromPoint(hwnd, pState, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (newOffset != pState->cursorOffset) {
                    pState->cursorOffset = newOffset;
                    EnsureCursorVisible(hwnd, pState);
                    
                    // Use FALSE here because we are handling the background in WM_PAINT
                    InvalidateRect(hwnd, NULL, FALSE); 
                    UpdateWindow(hwnd); 
                }
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            pState->isDragging = FALSE;
            ReleaseCapture();
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si = { sizeof(si), SIF_ALL };
            GetScrollInfo(hwnd, SB_VERT, &si);
            
            int oldY = pState->scrollY;
            int newY = oldY;

            RECT rc;
            GetClientRect(hwnd, &rc);
            int clientHeight = rc.bottom;
            
            // Calculate safe scroll bounds
            long long totalHeight64 = (long long)pState->pDoc->line_count * pState->lineHeight;
            int totalDocHeight = (totalHeight64 > 2147483647) ? 2147483647 : (int)totalHeight64;

            int maxScroll = totalDocHeight - clientHeight;
            if (maxScroll < 0) maxScroll = 0;

            switch (LOWORD(wParam)) {
                case SB_TOP:        newY = 0; break;
                case SB_BOTTOM:     newY = maxScroll; break;
                case SB_LINEUP:     newY -= pState->lineHeight; break;
                case SB_LINEDOWN:   newY += pState->lineHeight; break;
                case SB_PAGEUP:     newY -= clientHeight; break;
                case SB_PAGEDOWN:   newY += clientHeight; break;
                case SB_THUMBTRACK: newY = si.nTrackPos; break;
                default: break;
            }

            // Clamp to the valid range
            if (newY < 0) newY = 0;
            if (newY > maxScroll) newY = maxScroll;

            if (newY != oldY) {
                pState->scrollY = newY;
                UpdateScrollbars(hwnd, pState);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        case WM_SIZE: {
            if (pState && pState->pDoc) {
                SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE };
                si.nMin = 0;
                si.nMax = (int)pState->pDoc->line_count * pState->lineHeight;
                si.nPage = HIWORD(lParam);
                SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
                UpdateScrollbars(hwnd, pState);
            }
            return 0;
        }
        case WM_SETFOCUS:
            CreateCaret(hwnd, NULL, 2, pState->lineHeight);
            ShowCaret(hwnd);
            // Force caret to correct position immediately
            InvalidateRect(hwnd, NULL, FALSE); 
            return 0;

        case WM_KILLFOCUS:
            DestroyCaret();
            InvalidateRect(hwnd, NULL, FALSE); // Redraw to turn selection grey
            return 0;

        case WM_LBUTTONDOWN: {
            // Ensure the window has focus so the caret appears
            SetFocus(hwnd);

            // Convert click coordinates to a logical document offset
            size_t offset = GetOffsetFromPoint(hwnd, pState, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            
            // Handle selection extension vs. simple cursor move
            if (GetKeyState(VK_SHIFT) & 0x8000) {
                // Shift is held: update the active end of the selection only
                pState->cursorOffset = offset;
            } else {
                // Normal click: reset selection by moving both anchor and cursor to the click point
                pState->selectionAnchor = offset;
                pState->cursorOffset = offset;
            }
            
            // Begin drag selection
            pState->isDragging = TRUE;
            SetCapture(hwnd); // Directs all mouse input to this window even if the pointer leaves the client area

            // Notify the main window to update the status bar
            NotifyParent(hwnd, EN_SELCHANGE);
            
            // Trigger a repaint
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        case WM_DESTROY: {
            DeleteObject(pState->hFont);
            free(pState);
            return 0;
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

BOOL View_Register(HINSTANCE hInstance) {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = ViewportProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = _T("SlateView");
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    return RegisterClass(&wc);
}
