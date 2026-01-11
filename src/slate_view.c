#include "slate_view.h"
#include "slate_doc.h"
#include <tchar.h>

static ViewState* GetState(HWND hwnd) {
    return (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
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
    // Update scrollbar position to match
    SetScrollPos(hwnd, SB_VERT, pState->scrollY, TRUE);
}


void UpdateScrollbars(HWND hwnd, ViewState* pState) {
    if (!pState || !pState->pDoc) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientHeight = rc.bottom;

    // USE 64-BIT MATH to calculate true document height
    long long totalHeight64 = (long long)pState->pDoc->line_count * pState->lineHeight;
    
    // Windows Scrollbars are limited to 32-bit (2,147,483,647).
    // If the document is taller than ~2 billion pixels, we clamp it.
    // This prevents the negative overflow that blanks the screen.
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
    si.nMax = totalDocHeight;
    si.nPage = clientHeight;
    si.nPos = pState->scrollY;

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

    // 1. Determine the Line (Vertical)
    int lineIndex = (y + pState->scrollY) / pState->lineHeight;
    
    // Clamp to valid line range
    if (lineIndex < 0) lineIndex = 0;
    if (lineIndex >= (int)pState->pDoc->line_count) 
        lineIndex = (int)pState->pDoc->line_count - 1;

    // 2. Determine the Column (Horizontal)
    // Subtract the 5px left margin and use rounding (+ charWidth/2)
    int col = (x - 5 + (charWidth / 2)) / charWidth;
    
    size_t lineStart = Doc_GetLineOffset(pState->pDoc, lineIndex);
    size_t lineEnd = Doc_GetLineOffset(pState->pDoc, lineIndex + 1);
    size_t lineLen = lineEnd - lineStart;

    // 3. Logic to "Snap" to end of line
    // We want to ignore the trailing \r or \n for the sake of cursor placement
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

    // 4. Clamping
    // If col is negative (clicked in left margin), set to 0
    if (col < 0) col = 0;
    // If col is beyond the text (clicked in whitespace), set to lineLen
    if ((size_t)col > lineLen) col = (int)lineLen;

    return lineStart + col;
}

BOOL View_IsInsertMode(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    return pState ? pState->bInsertMode : TRUE;
}

void NotifyChanged(HWND hwnd) {
    // Send EN_CHANGE to parent, mimicking standard Edit control behavior
    SendMessage(GetParent(hwnd), WM_COMMAND, 
                MAKEWPARAM(GetWindowLongPtr(hwnd, GWLP_ID), EN_CHANGE), 
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
        NotifyChanged(hwnd);
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
        NotifyChanged(hwnd);
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

    // 1. Copy the selection to clipboard
    View_Copy(hwnd);

    // 2. Delete the selection from document
    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                 (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);

    Doc_Delete(pState->pDoc, start, len);
    
    // 3. Collapse selection and update UI
    pState->cursorOffset = pState->selectionAnchor = start;
    NotifyChanged(hwnd);
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
                // 1. If there is a selection, delete it first
                if (pState->cursorOffset != pState->selectionAnchor) {
                    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
                    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                                 (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);
                    Doc_Delete(pState->pDoc, start, len);
                    pState->cursorOffset = pState->selectionAnchor = start;
                }

                // 2. Insert the clipboard text
                size_t pasteLen = wcslen(pText);
                Doc_Insert(pState->pDoc, pState->cursorOffset, pText, pasteLen);
                pState->cursorOffset += pasteLen;
                pState->selectionAnchor = pState->cursorOffset;
                
                GlobalUnlock(hData);
                NotifyChanged(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        CloseClipboard();
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
            UpdateScrollbars(hwnd, pState);
            CreateCaret(hwnd, NULL, 2, pState->lineHeight);
            ShowCaret(hwnd);
            return 0;
        }
        case WM_ERASEBKGND:
            // Return non-zero to tell Windows we handled it.
            // This stops the automatic "white flash" before painting.
            return 1;

        case WM_CHAR: {
            WCHAR c = (WCHAR)wParam;
            
            // 1. Validation & Normalization
            // 8 is Backspace, 127 is Delete (both handled in WM_KEYDOWN)
            if (c == 8 || c == 127) return 0;
            
            if (c == L'\r' || c == L'\n' || (c >= 32) || c == L'\t') {
                if (c == L'\r') c = L'\n'; // Normalize to LF

                // 2. Selection Replacement Logic
                // If text is highlighted, any keypress replaces the entire selection
                if (pState->cursorOffset != pState->selectionAnchor) {
                    size_t start = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
                    size_t len = (pState->cursorOffset > pState->selectionAnchor) ? 
                                (pState->cursorOffset - pState->selectionAnchor) : (pState->selectionAnchor - pState->cursorOffset);
                    
                    Doc_Delete(pState->pDoc, start, len);
                    pState->cursorOffset = pState->selectionAnchor = start;
                } 
                else if (!pState->bInsertMode && c != L'\n') {
                    // 3. Overtype Logic (Only if no selection exists)
                    if (pState->cursorOffset < pState->pDoc->total_length) {
                        WCHAR nextChar;
                        Doc_GetText(pState->pDoc, pState->cursorOffset, 1, &nextChar);
                        // Don't overtype the newline; it breaks the line map logic
                        if (nextChar != L'\n') {
                            Doc_Delete(pState->pDoc, pState->cursorOffset, 1);
                        }
                    }
                }

                // 4. Perform Insertion
                Doc_Insert(pState->pDoc, pState->cursorOffset, &c, 1);
                pState->cursorOffset++;
                pState->selectionAnchor = pState->cursorOffset; // Collapse anchor to cursor

                // 5. Post-Insertion UI Updates
                if (c == L'\n') {
                    EnsureCursorVisible(hwnd, pState);
                }

                // Notify parent (for title bar '*' and status bar updates)
                SendMessage(GetParent(hwnd), WM_COMMAND, 
                            MAKEWPARAM(GetWindowLongPtr(hwnd, GWLP_ID), EN_CHANGE), 
                            (LPARAM)hwnd);

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
            
            // 1. Calculate Safe Limits using 64-bit math
            long long totalHeight64 = (long long)pState->pDoc->line_count * pState->lineHeight;
            int totalDocHeight = (totalHeight64 > 2147483647) ? 2147483647 : (int)totalHeight64;
            
            // 2. Determine Max Scroll Position
            int maxScroll = totalDocHeight - clientHeight;
            if (maxScroll < 0) maxScroll = 0;

            // 3. Apply Scroll
            pState->scrollY -= (linesToScroll * pState->lineHeight);

            // 4. Safe Clamping
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
                    NotifyChanged(hwnd);
                    break;

                case VK_RIGHT:
                    if (pState->cursorOffset < pState->pDoc->total_length) pState->cursorOffset++;
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyChanged(hwnd);
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
                    NotifyChanged(hwnd);
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
                    NotifyChanged(hwnd);
                    break;

                case VK_HOME:
                    pState->cursorOffset = Doc_GetLineOffset(pState->pDoc, line - 1);
                    if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                    NotifyChanged(hwnd);
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
                    NotifyChanged(hwnd);
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
                        NotifyChanged(hwnd);
                    } else {
                        // Standard single char delete
                        if (wParam == VK_BACK && pState->cursorOffset > 0) {
                            Doc_Delete(pState->pDoc, --pState->cursorOffset, 1);
                            pState->selectionAnchor = pState->cursorOffset;
                            NotifyChanged(hwnd);
                        } else if (wParam == VK_DELETE && pState->cursorOffset < pState->pDoc->total_length) {
                            Doc_Delete(pState->pDoc, pState->cursorOffset, 1);
                            NotifyChanged(hwnd);
                        }
                    }
                    break;

                case VK_INSERT:
                    if (isCtrlPressed) { View_Copy(hwnd); return 0; }
                    if (isShiftPressed) { View_Paste(hwnd); return 0; }

                    // If no modifiers, just toggle insert/overwrite mode
                    pState->bInsertMode = !pState->bInsertMode;
                    NotifyChanged(hwnd);
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

            // 1. Initialize Double Buffering
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);
            
            SelectObject(memDC, pState->hFont);

            // 2. Setup Metrics
            TEXTMETRIC tm;
            GetTextMetrics(memDC, &tm);
            int charWidth = tm.tmAveCharWidth;

            size_t selStart = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
            size_t selEnd = (pState->cursorOffset < pState->selectionAnchor) ? pState->selectionAnchor : pState->cursorOffset;
            BOOL hasFocus = (GetFocus() == hwnd);

            // 3. Clear Back Buffer
            HBRUSH hBg = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
            FillRect(memDC, &rc, hBg);
            DeleteObject(hBg);

            if (pState->pDoc && pState->pDoc->line_count > 0) {
                // --- 4. Update Caret Position ---
                // Use 64-bit intermediate math for large files
                int line, col;
                Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &line, &col);
                
                // Subtract scrollY (size_t) BEFORE casting to int to stay in GDI range
                long long relativeCaretY = ((long long)(line - 1) * pState->lineHeight) - (long long)pState->scrollY;
                int caretX = 5 + (col - 1) * charWidth;
                
                // Hide caret if it's off-screen to avoid GDI artifacts
                if (relativeCaretY >= 0 && relativeCaretY < rc.bottom) {
                    SetCaretPos(caretX, (int)relativeCaretY);
                }

                // --- 5. Draw Visible Content (Lazy Paint) ---
                // Calculate visible range using 64-bit math
                size_t first = (size_t)(pState->scrollY / pState->lineHeight);
                size_t last = (size_t)((pState->scrollY + rc.bottom) / pState->lineHeight);
                
                for (size_t i = first; i <= last && i < pState->pDoc->line_count; i++) {
                    size_t lineStart = Doc_GetLineOffset(pState->pDoc, i);
                    size_t lineEnd = (i + 1 < pState->pDoc->line_count) ? 
                                    Doc_GetLineOffset(pState->pDoc, i + 1) : pState->pDoc->total_length;
                    
                    size_t len = lineEnd - lineStart;
                    if (len == 0) continue;

                    WCHAR* buf = malloc((len + 1) * sizeof(WCHAR));
                    if (!buf) continue;

                    Doc_GetText(pState->pDoc, lineStart, len, buf);
                    
                    // Clean up newlines for display
                    size_t dLen = len;
                    while (dLen > 0 && (buf[dLen-1] == L'\n' || buf[dLen-1] == L'\r')) dLen--;

                    // CRITICAL: Calculate Y relative to the top of the CLIENT area
                    // This ensures lineY is always a small, safe integer
                    int lineY = (int)(((long long)i * pState->lineHeight) - (long long)pState->scrollY);

                    // A. Draw Normal Text
                    SetTextColor(memDC, GetSysColor(COLOR_WINDOWTEXT));
                    SetBkMode(memDC, OPAQUE);
                    SetBkColor(memDC, GetSysColor(COLOR_WINDOW));
                    TextOutW(memDC, 5, lineY, buf, (int)dLen);

                    // B. Draw Selection Overlay
                    if (selStart != selEnd && selStart < lineEnd && selEnd > lineStart) {
                        size_t intersectStart = (selStart > lineStart) ? selStart : lineStart;
                        size_t intersectEnd = (selEnd < lineEnd) ? selEnd : lineEnd;

                        if (intersectStart < intersectEnd) {
                            int x1 = 5 + (int)(intersectStart - lineStart) * charWidth;
                            int x2 = 5 + (int)(intersectEnd - lineStart) * charWidth;
                            RECT selRect = { x1, lineY, x2, lineY + pState->lineHeight };

                            COLORREF bgColor = hasFocus ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_3DFACE);
                            COLORREF textColor = hasFocus ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_BTNTEXT);

                            HBRUSH hSelBrush = CreateSolidBrush(bgColor);
                            FillRect(memDC, &selRect, hSelBrush);
                            DeleteObject(hSelBrush);

                            SetTextColor(memDC, textColor);
                            SetBkMode(memDC, TRANSPARENT);
                            
                            WCHAR* selTextPtr = buf + (intersectStart - lineStart);
                            int selTextLen = (int)(intersectEnd - intersectStart);
                            while (selTextLen > 0 && (selTextPtr[selTextLen-1] == L'\n' || selTextPtr[selTextLen-1] == L'\r')) selTextLen--;
                            
                            if (selTextLen > 0) {
                                TextOutW(memDC, x1, lineY, selTextPtr, selTextLen);
                            }
                        }
                    }
                    free(buf);
                }
            }

            // 6. Flip
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

            // 7. Cleanup
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
            
            // 1. Safe Limit Calculation
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

            // 2. Safe Clamping
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
            // 1. Ensure the window has focus so the caret appears
            SetFocus(hwnd);

            // 2. Convert click coordinates (x, y) to a logical document offset
            // This uses the helper function we built that calculates line and column
            size_t offset = GetOffsetFromPoint(hwnd, pState, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            
            // 3. Handle Selection vs. Simple Cursor Move
            if (GetKeyState(VK_SHIFT) & 0x8000) {
                // Shift is held: Update the 'active' end of the selection only
                pState->cursorOffset = offset;
            } else {
                // Normal click: Reset selection by moving both anchor and cursor to the click point
                pState->selectionAnchor = offset;
                pState->cursorOffset = offset;
            }
            
            // 4. Start the drag operation
            pState->isDragging = TRUE;
            SetCapture(hwnd); // Directs all mouse input to this window even if mouse leaves client area

            // IMPORTANT: Notify the main window to update the status bar
            NotifyChanged(hwnd);
            
            // 5. Visual update
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