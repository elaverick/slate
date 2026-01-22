#include "slate_view.h"
#include "slate_doc.h"
#include <tchar.h>
#include <limits.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <wchar.h>
#include <wctype.h>

static int GetTotalWrappedHeight(HWND hwnd, ViewState* pState, int wrapWidth);
static ViewState* GetState(HWND hwnd) {
    return (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
}

static int GetCommandSpaceHeight(const ViewState* pState) {
    if (!pState || !pState->bCommandMode) return 0;
    int lines = 1; // Prompt line
    if (pState->bCommandFeedback) lines++; // Optional feedback line
    return pState->lineHeight * lines;
}

// Allocates and trims a line; caller must free(*ppBuf). Returns trimmed length.
static BOOL View_LoadLine(ViewState* pState, size_t lineIdx, size_t* pLineStart, size_t* pLineEnd, WCHAR** ppBuf, size_t* pTrimLen) {
    if (!pState || !pState->pDoc || !ppBuf || !pTrimLen) return FALSE;

    size_t lineStart = Doc_GetLineOffset(pState->pDoc, lineIdx);
    size_t lineEnd = (lineIdx + 1 < pState->pDoc->line_count) ? 
                     Doc_GetLineOffset(pState->pDoc, lineIdx + 1) : pState->pDoc->total_length;
    size_t len = lineEnd - lineStart;
    WCHAR* buf = malloc((len + 1) * sizeof(WCHAR));
    if (!buf) return FALSE;

    Doc_GetText(pState->pDoc, lineStart, len, buf);
    size_t trimmed = len;
    while (trimmed > 0 && (buf[trimmed - 1] == L'\n' || buf[trimmed - 1] == L'\r')) trimmed--;
    buf[trimmed] = 0;

    *ppBuf = buf;
    *pTrimLen = trimmed;
    if (pLineStart) *pLineStart = lineStart;
    if (pLineEnd) *pLineEnd = lineEnd;
    return TRUE;
}

static int GetCommandPromptTopY(ViewState* pState, HDC hdc, RECT clientRc) {
    if (!pState || !pState->pDoc || !pState->bCommandMode) return INT_MIN;

    int cursorLine, cursorCol;
    Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &cursorLine, &cursorCol);

    // Unwrapped coordinates are simple
    if (!pState->bWordWrap) {
        return ((cursorLine - 1) * pState->lineHeight) - pState->scrollY;
    }

    // Wrapped: accumulate heights for lines before the cursor line
    int wrapWidth = clientRc.right - 10;
    int currentY = -pState->scrollY;
    for (size_t i = 0; i < pState->pDoc->line_count && (int)i < (cursorLine - 1); i++) {
        size_t lineStart = 0, lineEnd = 0;
        WCHAR* buf = NULL;
        size_t dLen = 0;
        if (!View_LoadLine(pState, i, &lineStart, &lineEnd, &buf, &dLen)) continue;

        RECT measureRect = { 0, 0, wrapWidth, 0 };
        DrawTextW(hdc, buf, (int)dLen, &measureRect, DT_WORDBREAK | DT_CALCRECT | DT_EXPANDTABS);
        int height = measureRect.bottom - measureRect.top;
        if (height < pState->lineHeight) height = pState->lineHeight;
        currentY += height;
        free(buf);
    }
    return currentY;
}

static void ClearCommandFeedback(ViewState* pState) {
    if (!pState) return;
    pState->bCommandFeedback = FALSE;
    pState->bCommandFeedbackHasCaret = FALSE;
    pState->commandFeedbackCaretCol = -1;
    pState->szCommandFeedback[0] = L'\0';
}

static void SetCommandFeedback(ViewState* pState, const WCHAR* text, int caretCol, BOOL hasCaret) {
    if (!pState || !text) return;
    wcsncpy(pState->szCommandFeedback, text, _countof(pState->szCommandFeedback) - 1);
    pState->szCommandFeedback[_countof(pState->szCommandFeedback) - 1] = L'\0';
    pState->bCommandFeedback = TRUE;
    pState->bCommandFeedbackHasCaret = hasCaret;
    pState->commandFeedbackCaretCol = hasCaret ? caretCol : -1;
}

// Maximum pixel width of unwrapped content (includes 5px inset on each side)
static int View_GetDocumentWidth(HWND hwnd, ViewState* pState) {
    if (!pState || !pState->pDoc || pState->bWordWrap) return 0;

    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int tabStops = tm.tmAveCharWidth * 4;

    int maxWidth = 0;
    for (size_t i = 0; i < pState->pDoc->line_count; i++) {
        WCHAR* buf = NULL;
        size_t dLen = 0;
        if (!View_LoadLine(pState, i, NULL, NULL, &buf, &dLen)) continue;
        DWORD extent = GetTabbedTextExtentW(hdc, buf, (int)dLen, 1, &tabStops);
        int width = (int)LOWORD(extent);
        if (width > maxWidth) maxWidth = width;
        free(buf);
    }

    ReleaseDC(hwnd, hdc);
    // 5px inset on each side to match draw origin of 5
    return maxWidth + 10;
}

// Returns TRUE when a non-empty selection exists; outputs start/len
static BOOL View_GetSelection(const ViewState* pState, size_t* pStart, size_t* pLen) {
    if (!pState || pState->cursorOffset == pState->selectionAnchor) return FALSE;
    size_t a = pState->cursorOffset;
    size_t b = pState->selectionAnchor;
    *pStart = (a < b) ? a : b;
    *pLen = (a < b) ? (b - a) : (a - b);
    return TRUE;
}

static BOOL IsWordChar(WCHAR ch) {
    return iswalnum(ch);
}

// Finds the word bounds surrounding 'offset'. Returns FALSE if no word is under/adjacent to the offset.
static BOOL View_GetWordBounds(SlateDoc* pDoc, size_t offset, size_t* pStart, size_t* pEnd) {
    if (!pDoc || !pStart || !pEnd) return FALSE;
    if (pDoc->total_length == 0) return FALSE;

    size_t totalLen = pDoc->total_length;
    if (offset > totalLen) offset = totalLen;
    size_t pos = offset;
    if (pos >= totalLen) pos = totalLen - 1;

    WCHAR ch = 0;
    Doc_GetText(pDoc, pos, 1, &ch);
    if (!IsWordChar(ch)) {
        if (pos == 0) return FALSE;
        Doc_GetText(pDoc, pos - 1, 1, &ch);
        if (!IsWordChar(ch)) return FALSE;
        pos--;
    }

    size_t start = pos;
    while (start > 0) {
        WCHAR prev = 0;
        Doc_GetText(pDoc, start - 1, 1, &prev);
        if (!IsWordChar(prev)) break;
        start--;
    }

    size_t end = pos + 1;
    while (end < totalLen) {
        WCHAR next = 0;
        Doc_GetText(pDoc, end, 1, &next);
        if (!IsWordChar(next)) break;
        end++;
    }

    *pStart = start;
    *pEnd = end;
    return TRUE;
}

// Shared document height calculation for scroll math (clamped to 32-bit)
static int View_GetDocumentHeight(HWND hwnd, ViewState* pState, int wrapWidth) {
    if (!pState || !pState->pDoc) return 0;
    if (pState->bWordWrap) {
        int totalHeight = GetTotalWrappedHeight(hwnd, pState, wrapWidth);
        if (totalHeight <= 0) totalHeight = pState->lineHeight;
        int extra = GetCommandSpaceHeight(pState);
        if (extra > 0) {
            if (totalHeight > INT_MAX - extra) totalHeight = INT_MAX;
            else totalHeight += extra;
        }
        return (totalHeight > INT_MAX) ? INT_MAX : totalHeight;
    }
    long long totalHeight64 = (long long)pState->pDoc->line_count * pState->lineHeight;
    totalHeight64 += GetCommandSpaceHeight(pState);
    return (totalHeight64 > 2147483647LL) ? 2147483647 : (int)totalHeight64;
}

void View_SetDefaultColors(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    if (!pState) return;
    
    pState->bUseSystemColors = FALSE;
    pState->colorBg    = RGB(0xE6, 0xE3, 0xDA);
    pState->colorBgDim = RGB(0xE6, 0xE6, 0xE6); 
    pState->colorText  = RGB(0x26, 0x25, 0x22); 
    pState->colorDim   = RGB(150, 150, 150);    
    InvalidateRect(hwnd, NULL, TRUE);
}

void View_UseSystemColors(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    if (!pState) return;

    pState->bUseSystemColors = TRUE;
    pState->colorBg   = GetSysColor(COLOR_WINDOW);
    pState->colorBgDim   = GetSysColor(COLOR_WINDOW);
    pState->colorText = GetSysColor(COLOR_WINDOWTEXT);
    pState->colorDim  = RGB(180, 180, 180);
    InvalidateRect(hwnd, NULL, TRUE);
}

BOOL View_IsUsingSystemColors(HWND hwnd) {
    ViewState* pState = GetState(hwnd);
    return pState ? pState->bUseSystemColors : TRUE;
}

static void GetCursorVisualPos(HWND hwnd, ViewState* pState, size_t targetOffset, int* outX, int* outY) {
    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int wrapWidth = clientRect.right - 10;

    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int tabStops = tm.tmAveCharWidth * 4;

    int currentYDoc = 0; // document-space
    size_t startLineIdx = 0;
    int finalX = 5, finalYDoc = 0;
    BOOL found = FALSE;

    for (size_t i = startLineIdx; i < pState->pDoc->line_count; i++) {
        size_t lineStart = 0, lineEnd = 0;
        WCHAR* buf = NULL;
        size_t dLen = 0;
        if (!View_LoadLine(pState, i, &lineStart, &lineEnd, &buf, &dLen)) continue;

        if (targetOffset >= lineStart && targetOffset <= lineStart + dLen) {
            size_t rel = targetOffset - lineStart;
            
            // Get height of the text block up to the cursor index
            RECT rcM = { 0, 0, wrapWidth, 0 };
            DrawTextW(hdc, buf, (int)rel, &rcM, DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT | DT_NOPREFIX);
            
            // Determine the visual line start by scanning forward
            size_t vLineStart = 0;
            int currentLineBottom = 0;
            for (size_t k = 0; k <= rel; k++) {
                RECT rcPartial = { 0, 0, wrapWidth, 0 };
                DrawTextW(hdc, buf, (int)k, &rcPartial, DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT | DT_NOPREFIX);
                if (rcPartial.bottom > currentLineBottom) {
                    currentLineBottom = rcPartial.bottom;
                    vLineStart = (k > 0) ? k - 1 : 0;
                    while (vLineStart < rel && (buf[vLineStart] == L' ' || buf[vLineStart] == L'\t')) vLineStart++;
                }
            }

            finalYDoc = currentYDoc + currentLineBottom - pState->lineHeight;

            DWORD extent = GetTabbedTextExtentW(hdc, buf + vLineStart, (int)(rel - vLineStart), 1, &tabStops);
            finalX = 5 + LOWORD(extent);
            found = TRUE;
        }

        RECT rcFull = { 0, 0, wrapWidth, 0 };
        DrawTextW(hdc, buf, (int)dLen, &rcFull, DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT | DT_NOPREFIX);
        currentYDoc += (rcFull.bottom <= 0) ? pState->lineHeight : rcFull.bottom;
        
        free(buf);
        if (found) break;
    }

    ReleaseDC(hwnd, hdc);
    *outX = finalX; 
    *outY = finalYDoc - pState->scrollY; // return client-relative Y
}

// Calculate total pixel height of wrapped content
static int GetTotalWrappedHeight(HWND hwnd, ViewState* pState, int wrapWidth) {
    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);
    long long total = 0;

    for (size_t i = 0; i < pState->pDoc->line_count; i++) {
        WCHAR* buf = NULL;
        size_t dLen = 0;
        if (!View_LoadLine(pState, i, NULL, NULL, &buf, &dLen)) continue;

        RECT rcMeasure = { 0, 0, wrapWidth, 0 };
        DrawTextW(hdc, buf, (int)dLen, &rcMeasure, DT_WORDBREAK | DT_CALCRECT | DT_EXPANDTABS | DT_NOPREFIX);
        int h = (rcMeasure.bottom <= 0) ? pState->lineHeight : rcMeasure.bottom;
        if (h < pState->lineHeight) h = pState->lineHeight;
        total += h;
        free(buf);
        if (total > INT_MAX) { total = INT_MAX; break; }
    }

    ReleaseDC(hwnd, hdc);
    return (total > INT_MAX) ? INT_MAX : (int)total;
}

// Maps X,Y back to a document offset
size_t View_XYToOffset(HWND hwnd, int targetX, int targetY) {
    ViewState* pState = (ViewState*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);

    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int tabStops = tm.tmAveCharWidth * 4;

    RECT clientRc;
    GetClientRect(hwnd, &clientRc);
    int wrapWidth = clientRc.right - 10;

    int targetYDoc = targetY + pState->scrollY; // convert to document-space Y
    if (targetYDoc < 0) targetYDoc = 0;
    int currentY = 0;
    size_t resultOffset = 0;

    for (size_t i = 0; i < pState->pDoc->line_count; i++) {
        size_t lineStart = 0, lineEnd = 0;
        size_t dLen = 0;
        WCHAR* buf = NULL;
        if (!View_LoadLine(pState, i, &lineStart, &lineEnd, &buf, &dLen)) {
            lineStart = Doc_GetLineOffset(pState->pDoc, i);
            lineEnd = (i + 1 < pState->pDoc->line_count) ? 
                         Doc_GetLineOffset(pState->pDoc, i + 1) : pState->pDoc->total_length;
            size_t len = lineEnd - lineStart;
            buf = malloc((len + 1) * sizeof(WCHAR));
            if (!buf) continue;
            Doc_GetText(pState->pDoc, lineStart, len, buf);
            dLen = len;
            while (dLen > 0 && (buf[dLen-1] == L'\n' || buf[dLen-1] == L'\r')) dLen--;
        }

        RECT rcFull = { 0, 0, wrapWidth, 0 };
        DrawTextW(hdc, buf, (int)dLen, &rcFull, DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT | DT_NOPREFIX);
        int totalH = (rcFull.bottom <= 0) ? pState->lineHeight : rcFull.bottom;

        if (targetYDoc >= currentY && targetYDoc < currentY + totalH) {
            int visualY = targetYDoc - currentY;
            int bestOffset = 0;
            int minDistance = 999999;
            int currentLineBottom = 0;
            size_t currentLineStart = 0;
            for (size_t k = 0; k <= dLen; k++) {
                RECT rcM = { 0, 0, wrapWidth, 0 };
                DrawTextW(hdc, buf, (int)k, &rcM, DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT | DT_NOPREFIX);
                if (rcM.bottom > currentLineBottom) {
                    currentLineBottom = rcM.bottom;
                    currentLineStart = (k > 0) ? k - 1 : 0;
                    while (currentLineStart < dLen && (buf[currentLineStart] == L' ' || buf[currentLineStart] == L'\t')) currentLineStart++;
                }
                int cy = (rcM.bottom <= 0) ? 0 : currentLineBottom - pState->lineHeight;

                DWORD extent = GetTabbedTextExtentW(hdc, buf + currentLineStart, (int)(k - currentLineStart), 1, &tabStops);
                int cx = 5 + LOWORD(extent);

                int dist = abs(cy - visualY) * 100 + abs(cx - targetX);
                if (dist < minDistance) {
                    minDistance = dist;
                    bestOffset = (int)k;
                }
            }
            resultOffset = lineStart + bestOffset;
            free(buf);
            break;
        }
        currentY += totalH;
        free(buf);
        resultOffset = lineStart + dLen; 
    }

    ReleaseDC(hwnd, hdc);
    return resultOffset;
}

void EnsureCursorVisible(HWND hwnd, ViewState* pState) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    if (pState->bWordWrap) {
        int cx, cy;
        GetCursorVisualPos(hwnd, pState, pState->cursorOffset, &cx, &cy); // cy is client-relative
        if (cy < 0) {
            pState->scrollY += cy;
        } else if (cy + pState->lineHeight > rc.bottom) {
            pState->scrollY += (cy + pState->lineHeight) - rc.bottom;
        }
        if (pState->scrollY < 0) pState->scrollY = 0;
        int wrapWidth = rc.right - 10;
        int totalH = View_GetDocumentHeight(hwnd, pState, wrapWidth);
        int maxScroll = (totalH > rc.bottom) ? (totalH - rc.bottom) : 0;
        if (pState->scrollY > maxScroll) pState->scrollY = maxScroll;
    } else {
        int line, col;
        Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &line, &col);
        int cursorY = (line - 1) * pState->lineHeight;
        
        if (cursorY < pState->scrollY) {
            pState->scrollY = cursorY;
        } else if (cursorY + pState->lineHeight > pState->scrollY + rc.bottom) {
            pState->scrollY = cursorY - rc.bottom + pState->lineHeight;
        }
        if (pState->scrollY < 0) pState->scrollY = 0;

        // Horizontal visibility for unwrapped mode
        HDC hdc = GetDC(hwnd);
        SelectObject(hdc, pState->hFont);
        TEXTMETRIC tm;
        GetTextMetrics(hdc, &tm);
        int tabStops = tm.tmAveCharWidth * 4;

        size_t lineStart = Doc_GetLineOffset(pState->pDoc, line - 1);
        size_t len = pState->cursorOffset - lineStart;
        WCHAR* lineBuf = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
        int cursorX = 5;
        if (lineBuf) {
            Doc_GetText(pState->pDoc, lineStart, len, lineBuf);
            lineBuf[len] = L'\0';
            cursorX += (int)LOWORD(GetTabbedTextExtentW(hdc, lineBuf, (int)len, 1, &tabStops));
            free(lineBuf);
        }

        ReleaseDC(hwnd, hdc);

        if (cursorX < pState->scrollX) {
            pState->scrollX = cursorX;
        } else if (cursorX > pState->scrollX + rc.right - 1) {
            pState->scrollX = cursorX - rc.right + 1;
        }
        int maxScrollX = View_GetDocumentWidth(hwnd, pState) - rc.right;
        if (maxScrollX < 0) maxScrollX = 0;
        if (pState->scrollX < 0) pState->scrollX = 0;
        if (pState->scrollX > maxScrollX) pState->scrollX = maxScrollX;
    }
    SetScrollPos(hwnd, SB_VERT, pState->scrollY, TRUE);
    SetScrollPos(hwnd, SB_HORZ, pState->scrollX, TRUE);
}

void UpdateScrollbars(HWND hwnd, ViewState* pState) {
    if (!pState || !pState->pDoc) return;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientHeight = rc.bottom;
    int wrapWidth = rc.right - 10;

    // Grow the line map just enough to cover the current viewport plus a margin,
    // instead of forcing a full-file scan on load.
    int visibleLines = (pState->lineHeight > 0) ? (clientHeight / pState->lineHeight) : 0;
    size_t targetLine = (size_t)(pState->scrollY / pState->lineHeight) + visibleLines + 200;
    Doc_GetLineOffset(pState->pDoc, targetLine);

    int totalHeight = View_GetDocumentHeight(hwnd, pState, wrapWidth);

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nPos = pState->scrollY;
    if (pState->bWordWrap) {
        // Pixel-based scrolling over wrapped content
        si.nMax = totalHeight;
        si.nPage = clientHeight;
        if (pState->scrollY > totalHeight - clientHeight) {
            pState->scrollY = (totalHeight > clientHeight) ? (totalHeight - clientHeight) : 0;
        }
        si.nPos = pState->scrollY;
    } else {
        // Pixel-based scrolling (Safe 64-bit clamp)
        si.nMax = totalHeight;
        si.nPage = rc.bottom;
        int maxScrollY = totalHeight - clientHeight;
        if (maxScrollY < 0) maxScrollY = 0;
        if (pState->scrollY > maxScrollY) pState->scrollY = maxScrollY;
        si.nPos = pState->scrollY;
    }

    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

    // Horizontal scroll bar only matters when not wrapping
    int clientWidth = rc.right;
    int docWidth = pState->bWordWrap ? clientWidth : View_GetDocumentWidth(hwnd, pState);
    SCROLLINFO siH = {0};
    siH.cbSize = sizeof(siH);
    siH.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    siH.nMin = 0;
    siH.nMax = docWidth;
    siH.nPage = clientWidth;
    if (pState->bWordWrap) {
        pState->scrollX = 0;
    } else {
        int maxScrollX = docWidth - clientWidth;
        if (maxScrollX < 0) maxScrollX = 0;
        if (pState->scrollX > maxScrollX) pState->scrollX = maxScrollX;
    }
    siH.nPos = pState->scrollX;
    SetScrollInfo(hwnd, SB_HORZ, &siH, TRUE);
}

void DrawCustomCaret(HDC hdc, ViewState* pState) {
    if (pState->caretAlpha <= 0.01f || !pState->hCaretBm) return;

    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, pState->hCaretBm);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(pState->caretAlpha * 255), 0 };

    int drawX = pState->caretX;

    AlphaBlend(hdc, drawX, pState->caretY, 1, pState->lineHeight, 
               hdcMem, 0, 0, 1, pState->lineHeight, bf);

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
}

void View_SetDocument(HWND hwnd, SlateDoc* pDoc) {
    ViewState* pState = GetState(hwnd);
    if (pState) {
        pState->pDoc = pDoc;
        pState->cursorOffset = 0;
        pState->scrollY = 0;
        pState->scrollX = 0;
        
        // Reclaim caret and focus
        if (GetFocus() == hwnd) {
            DestroyCaret();
            CreateCaret(hwnd, NULL, 1, pState->lineHeight);
            SetCaretPos(5, 0);
            //ShowCaret(hwnd);
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
    int targetY = y;
    if (pState->bCommandMode) {
        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        HDC hdcTmp = GetDC(hwnd);
        SelectObject(hdcTmp, pState->hFont);
        int promptTop = GetCommandPromptTopY(pState, hdcTmp, clientRc);
        ReleaseDC(hwnd, hdcTmp);
        int commandSpace = GetCommandSpaceHeight(pState);
        if (promptTop != INT_MIN && commandSpace > 0 && targetY >= promptTop + commandSpace) {
            targetY -= commandSpace;
        }
    }

    if (pState->bWordWrap) {
        return View_XYToOffset(hwnd, x, targetY);
    }

    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);
    
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int charWidth = tm.tmAveCharWidth;
    ReleaseDC(hwnd, hdc);

    // Map the y coordinate to a line index
    int lineIndex = (targetY + pState->scrollY) / pState->lineHeight;
    
    // Clamp to valid line range
    if (lineIndex < 0) lineIndex = 0;
    if (lineIndex >= (int)pState->pDoc->line_count) 
        lineIndex = (int)pState->pDoc->line_count - 1;

    // Map the x coordinate to a column, subtracting the left margin and rounding to the nearest char
    int col = (x + pState->scrollX - 5 + (charWidth / 2)) / charWidth;
    
    size_t lineStart = 0, lineEnd = 0;
    size_t lineLen = 0;
    WCHAR* buf = NULL;
    size_t dLen = 0;
    if (View_LoadLine(pState, lineIndex, &lineStart, &lineEnd, &buf, &dLen)) {
        lineLen = dLen;
    } else {
        lineStart = Doc_GetLineOffset(pState->pDoc, lineIndex);
        lineEnd = Doc_GetLineOffset(pState->pDoc, lineIndex + 1);
        lineLen = lineEnd - lineStart;

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
    }
    if (buf) free(buf);

    // Clamp within the line bounds
    if (col < 0) col = 0;
    if ((size_t)col > lineLen) col = (int)lineLen;

    return lineStart + col;
}

void UpdateCaretPosition(HWND hwnd, ViewState* pState) {
    if (!pState || !pState->pDoc || GetFocus() != hwnd) return;

    int x = pState->bWordWrap ? 5 : (5 - pState->scrollX);
    int y = 0; // y stays client-relative
    int cursorLine, cursorCol;
    Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &cursorLine, &cursorCol);

    if (pState->bCommandMode) {
        HDC hdc = GetDC(hwnd);
        SelectObject(hdc, pState->hFont);

        // Measure ":text" to place the caret correctly in the prompt
        WCHAR temp[260];
        int len = swprintf(temp, 260, L":%.*s", (int)pState->commandCaretPos, pState->szCommandBuf);
        SIZE sz = {0};
        GetTextExtentPoint32W(hdc, temp, len, &sz);
        RECT clientRc;
        GetClientRect(hwnd, &clientRc);
        y = GetCommandPromptTopY(pState, hdc, clientRc);
        if (y == INT_MIN) y = 0;
        ReleaseDC(hwnd, hdc);

        x += sz.cx;
    } else if (pState->bWordWrap) {
        GetCursorVisualPos(hwnd, pState, pState->cursorOffset, &x, &y);
    } else {
        HDC hdc = GetDC(hwnd);
        SelectObject(hdc, pState->hFont);
        
        TEXTMETRIC tm;
        GetTextMetrics(hdc, &tm);
        int tabStops = tm.tmAveCharWidth * 4; // MATCH WM_PAINT EXACTLY

        int visualLine = cursorLine - 1;

        y = (visualLine * pState->lineHeight) - pState->scrollY;

        size_t lineStart = Doc_GetLineOffset(pState->pDoc, cursorLine - 1);
        size_t len = pState->cursorOffset - lineStart;
        WCHAR* lineBuf = (WCHAR*)malloc((len + 1) * sizeof(WCHAR));
        if (lineBuf) {
            Doc_GetText(pState->pDoc, lineStart, len, lineBuf);
            lineBuf[len] = L'\0';
            x += LOWORD(GetTabbedTextExtentW(hdc, lineBuf, (int)len, 1, &tabStops));
            free(lineBuf);
        }
        ReleaseDC(hwnd, hdc);
    }
    
    // Save these so DrawCustomCaret knows where to go!
    pState->caretX = x;
    pState->caretY = y;
    
    SetCaretPos(x, y);
}

void View_SetInsertMode(HWND hwnd, BOOL bInsert) {
    ViewState* pState = GetState(hwnd);
    if (pState) {
        pState->bInsertMode = bInsert;
        InvalidateRect(hwnd, NULL, TRUE);
    }
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

    size_t start = 0, len = 0;
    if (!View_GetSelection(pState, &start, &len)) return;

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
    size_t start = 0, len = 0;
    if (!View_GetSelection(pState, &start, &len)) return;

    // Copy the selection to the clipboard
    View_Copy(hwnd);

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
                size_t start = 0, len = 0;
                if (View_GetSelection(pState, &start, &len)) {
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

static BOOL ClipboardHasText(HWND hwnd) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return FALSE;

    BOOL hasText = FALSE;
    if (OpenClipboard(hwnd)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const WCHAR* pText = (const WCHAR*)GlobalLock(hData);
            if (pText) {
                hasText = (pText[0] != L'\0');
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }
    return hasText;
}

static void DeleteSelection(HWND hwnd, ViewState* pState) {
    if (!pState || !pState->pDoc) return;

    size_t start = 0, len = 0;
    if (!View_GetSelection(pState, &start, &len)) return;

    Doc_Delete(pState->pDoc, start, len);
    pState->cursorOffset = pState->selectionAnchor = start;

    NotifyParent(hwnd, EN_CHANGE);
    EnsureCursorVisible(hwnd, pState);
    UpdateCaretPosition(hwnd, pState);
    InvalidateRect(hwnd, NULL, FALSE);
}

void ResetCaretBlink(ViewState* pState) {
    pState->lastActivity = GetTickCount();
    pState->animationTime = 0.0;
    pState->caretAlpha = 1.0f;
    pState->caretDirection = -1;
}

void View_SetWordWrap(HWND hwnd, BOOL bWrap) {
    ViewState* pState = GetState(hwnd);
    if (pState && pState->bWordWrap != bWrap) {
        pState->bWordWrap = bWrap;
        pState->scrollY = 0; // Reset scroll to avoid getting lost
        pState->scrollX = 0;
        UpdateScrollbars(hwnd, pState);
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

BOOL View_ApplySearchResult(HWND hwnd, const DocSearchResult* result) {
    ViewState* pState = GetState(hwnd);
    if (!pState || !pState->pDoc || !result) return FALSE;
    if (result->status != DOC_SEARCH_MATCH) return FALSE;

    size_t start = result->match_offset;
    size_t end = start + result->match_length;

    if (start > pState->pDoc->total_length) return FALSE;
    if (end > pState->pDoc->total_length) end = pState->pDoc->total_length;

    pState->selectionAnchor = start;
    pState->cursorOffset = end;

    NotifyParent(hwnd, EN_SELCHANGE);
    EnsureCursorVisible(hwnd, pState);
    UpdateCaretPosition(hwnd, pState);
    InvalidateRect(hwnd, NULL, TRUE);
    return TRUE;
}

static void PaintWrappedContent(ViewState* pState, HDC memDC, RECT rc, int tabStops, COLORREF currentDim) {
    int currentY = -pState->scrollY;
    RECT textRect = rc;
    textRect.left += 5; 
    textRect.right -= 5;

    int cursorLine, cursorCol;
    Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &cursorLine, &cursorCol);

    SetBkMode(memDC, TRANSPARENT);
    int commandSpace = GetCommandSpaceHeight(pState);

    for (size_t i = 0; i < pState->pDoc->line_count; i++) {
        if (commandSpace > 0 && (int)i == (cursorLine - 1)) {
            currentY += commandSpace;
        }

        size_t lineStart = 0, lineEnd = 0;
        WCHAR* buf = NULL;
        size_t dLen = 0;
        if (!View_LoadLine(pState, i, &lineStart, &lineEnd, &buf, &dLen)) continue;

        // 1. Calculate height for this wrapped line
        RECT measureRect = textRect;
        measureRect.top = 0; // Use 0 for calculation to avoid overflow issues
        DrawTextW(memDC, buf, (int)dLen, &measureRect, DT_WORDBREAK | DT_CALCRECT | DT_EXPANDTABS);
        int height = measureRect.bottom - measureRect.top;

        // 2. Adjust height to be a multiple of our standard lineHeight
        // This fixes the spacing discrepancy between modes
        if (height < pState->lineHeight) height = pState->lineHeight;

        // 3. Draw the actual text
        RECT drawRect = textRect;
        drawRect.top = currentY;
        drawRect.bottom = currentY + height;

        // Skip blocks completely above the viewport but advance Y
        if (drawRect.bottom <= 0) {
            currentY += height;
            free(buf);
            if (currentY > rc.bottom) break;
            continue;
        }
        
        DrawTextW(memDC, buf, (int)dLen, &drawRect, DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX);

        // 4. Handle non-printable characters (Pilcrow)
        if (pState->bShowNonPrintable) {
            COLORREF oldClr = SetTextColor(memDC, currentDim);

            // Draw whitespace markers within the wrapped block
            int currentLineBottom = 0;
            size_t currentLineStart = 0;
            for (size_t k = 0; k < dLen; k++) {
                RECT rcPartial = { 0, 0, textRect.right - textRect.left, 0 };
                DrawTextW(memDC, buf, (int)(k + 1), &rcPartial, DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT | DT_NOPREFIX);
                if (rcPartial.bottom > currentLineBottom) {
                    currentLineBottom = rcPartial.bottom;
                    currentLineStart = (k > 0) ? k - 1 : 0;
                    while (currentLineStart < dLen && (buf[currentLineStart] == L' ' || buf[currentLineStart] == L'\t')) currentLineStart++;
                }
                if (buf[k] == L' ' || buf[k] == L'\t') {
                    DWORD extentBefore = GetTabbedTextExtentW(memDC, buf + currentLineStart, (int)(k - currentLineStart), 1, &tabStops);
                    int charX = textRect.left + LOWORD(extentBefore);
                    int lineY = currentY + currentLineBottom - pState->lineHeight;
                    WCHAR sym = (buf[k] == L' ') ? 0x00B7 : 0x00BB;
                    TextOutW(memDC, charX, lineY, &sym, 1);
                }
            }

            if (lineEnd < pState->pDoc->total_length) {
                WCHAR pilcrow = 0x00B6;
                DrawTextW(memDC, &pilcrow, 1, &drawRect, DT_SINGLELINE | DT_RIGHT | DT_BOTTOM | DT_NOPREFIX);
            }
            SetTextColor(memDC, oldClr);
        }

        free(buf);
        currentY += height;

        if (currentY > rc.bottom) break;
    }
}

static void PaintUnwrappedContent(ViewState* pState, HDC memDC, RECT rc, int tabStops, COLORREF currentBg, COLORREF currentText, COLORREF currentDim, size_t selStart, size_t selEnd, BOOL hasFocus) {
    size_t first = (size_t)(pState->scrollY / pState->lineHeight);
    size_t last = (size_t)((pState->scrollY + rc.bottom) / pState->lineHeight);
    int cursorLine, cursorCol;
    Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &cursorLine, &cursorCol);
    
    int baseX = 5 - pState->scrollX;
    int commandSpace = GetCommandSpaceHeight(pState);
    for (size_t i = first; i <= last && i < pState->pDoc->line_count; i++) {
        size_t lineStart = 0, lineEnd = 0;
        WCHAR* buf = NULL;
        size_t dLen = 0;
        if (!View_LoadLine(pState, i, &lineStart, &lineEnd, &buf, &dLen)) continue;
        
        // Calculate a stable Y coordinate based strictly on line index
        int lineY = (int)(((long long)i * pState->lineHeight) - (long long)pState->scrollY);
        if (commandSpace > 0 && (int)i >= (cursorLine - 1)) {
            lineY += commandSpace;
        }

        // Pass 1: Draw the background text
        SetTextColor(memDC, currentText);
        SetBkColor(memDC, currentBg);
        TabbedTextOutW(memDC, baseX, lineY, buf, (int)dLen, 1, &tabStops, baseX);

        // Pass 2: Overlay Symbols (Non-Printable)
        if (pState->bShowNonPrintable) {
            COLORREF oldClr = SetTextColor(memDC, currentDim);
            SetBkMode(memDC, TRANSPARENT);
            for (size_t k = 0; k < dLen; k++) {
                if (buf[k] == L' ' || buf[k] == L'\t') {
                    WCHAR sym = (buf[k] == L' ') ? 0x00B7 : 0x00BB;
                    DWORD extent = GetTabbedTextExtentW(memDC, buf, (int)k, 1, &tabStops);
                    TextOutW(memDC, baseX + LOWORD(extent), lineY, &sym, 1);
                }
            }
            DWORD lineExtent = GetTabbedTextExtentW(memDC, buf, (int)dLen, 1, &tabStops);
            WCHAR pilcrow = 0x00B6;
            TextOutW(memDC, baseX + LOWORD(lineExtent), lineY, &pilcrow, 1);
            if (i == pState->pDoc->line_count - 1) {
                TextOutW(memDC, baseX + 5 + LOWORD(lineExtent), lineY, L"[EOF]", 5);
            }
            SetTextColor(memDC, oldClr);
        }

        // Pass 3: Selection Overlay
        if (selStart != selEnd && selStart < lineEnd && selEnd > lineStart) {
            size_t intersectStart = (selStart > lineStart) ? selStart : lineStart;
            size_t intersectEnd = (selEnd < lineEnd) ? selEnd : lineEnd;

            if (intersectStart < intersectEnd) {
                DWORD ext1 = GetTabbedTextExtentW(memDC, buf, (int)(intersectStart - lineStart), 1, &tabStops);
                DWORD ext2 = GetTabbedTextExtentW(memDC, buf, (int)(intersectEnd - lineStart), 1, &tabStops);
                
                int x1 = baseX + LOWORD(ext1);
                int x2 = baseX + LOWORD(ext2);
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
        free(buf);
    }
}

static void PaintCommandOverlay(ViewState* pState, HDC memDC, RECT rc) {
    // Draw this LAST so it appears on top of the text, regardless of scroll position
    if (!pState->bCommandMode) return;

    int commandSpace = GetCommandSpaceHeight(pState);
    if (commandSpace <= 0) return;

    int promptY = GetCommandPromptTopY(pState, memDC, rc);
    if (promptY == INT_MIN) return;

    int baseX = pState->bWordWrap ? 5 : (5 - pState->scrollX);
    HBRUSH hPromptBg = CreateSolidBrush(pState->colorBg);

    // Prompt line
    RECT promptRect = { 0, promptY, rc.right, promptY + pState->lineHeight };
    if (promptRect.bottom > 0 && promptRect.top < rc.bottom) {
        FillRect(memDC, &promptRect, hPromptBg);

        WCHAR szFull[260];
        swprintf(szFull, 260, L":%s", pState->szCommandBuf);

        SetTextColor(memDC, pState->colorText);
        SetBkMode(memDC, TRANSPARENT);
        TabbedTextOutW(memDC, baseX, promptY, szFull, (int)wcslen(szFull), 0, NULL, baseX);
    }

    // Feedback line directly beneath the prompt
    if (pState->bCommandFeedback) {
        int feedbackY = promptY + pState->lineHeight;
        RECT feedbackRect = { 0, feedbackY, rc.right, feedbackY + pState->lineHeight };
        if (feedbackRect.bottom > 0 && feedbackRect.top < rc.bottom) {
            FillRect(memDC, &feedbackRect, hPromptBg);

            TEXTMETRIC tm;
            GetTextMetrics(memDC, &tm);
            int indentPx = tm.tmAveCharWidth * 2; // 1-2 space indent

            SetTextColor(memDC, pState->colorDim);
            SetBkMode(memDC, TRANSPARENT);

            int msgLen = (int)wcslen(pState->szCommandFeedback);
            if (msgLen > 0) {
                TextOutW(memDC, baseX + indentPx, feedbackY, pState->szCommandFeedback, msgLen);
            }

            if (pState->bCommandFeedbackHasCaret) {
                WCHAR promptBuf[260];
                swprintf(promptBuf, 260, L":%s", pState->szCommandBuf);
                int promptLen = (int)wcslen(promptBuf);
                int caretCol = pState->commandFeedbackCaretCol;
                if (caretCol < 0) caretCol = 0;
                if (caretCol > promptLen) caretCol = promptLen;
                SIZE caretExtent = {0};
                GetTextExtentPoint32W(memDC, promptBuf, caretCol, &caretExtent);
                int caretX = baseX + caretExtent.cx;
                TextOutW(memDC, caretX, feedbackY, L"^", 1);
            }
        }
    }

    DeleteObject(hPromptBg);
}

static LRESULT HandleCreate(HWND hwnd) {
    ViewState* pState = (ViewState*)calloc(1, sizeof(ViewState));
    if (!pState) return -1;

    // 1. Initial Defaults
    pState->bInsertMode = TRUE;
    pState->caretAlpha = 0.0f;     // Start transparent
    pState->caretDirection = 1;    // Prepare to fade in
    pState->scrollX = 0;
    
    // 2. Create Font
    pState->hFont = CreateFont(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, 
                               CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascade Mono");

    // 3. Use one HDC for all GDI initialization
    HDC hdc = GetDC(hwnd);
    
    // Get Font Metrics
    HGDIOBJ hOldFont = SelectObject(hdc, pState->hFont);
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    pState->lineHeight = tm.tmHeight + tm.tmExternalLeading;
    SelectObject(hdc, hOldFont); // Restore old font
    

    // Create and initialize the persistent Caret Bitmap
    // We reuse the 'hdc' here to ensure the bitmap is compatible with the window
    pState->hCaretBm = CreateCompatibleBitmap(hdc, 1, pState->lineHeight);
    
    HDC hMemDC = CreateCompatibleDC(hdc);
    HGDIOBJ hOldBm = SelectObject(hMemDC, pState->hCaretBm);
    // Clear then fill the 1px caret bitmap
    PatBlt(hMemDC, 0, 0, 1, pState->lineHeight, BLACKNESS);
    RECT rcFull = { 0, 0, 1, pState->lineHeight };
    FillRect(hMemDC, &rcFull, (HBRUSH)GetStockObject(BLACK_BRUSH));

    // Cleanup
    SelectObject(hMemDC, hOldBm);
    DeleteDC(hMemDC);

    // Now we are done with the HDC
    ReleaseDC(hwnd, hdc);

    // 4. Final View Setup
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)pState);

    View_SetDefaultColors(hwnd);
    UpdateScrollbars(hwnd, pState);

    // Create system caret for accessibility (but don't call ShowCaret)
    CreateCaret(hwnd, NULL, 1, pState->lineHeight);
    
    return 0;
}

static LRESULT HandleTimer(HWND hwnd, ViewState* pState, WPARAM wParam) {
    if (wParam != IDT_CARET) return 0;

    DWORD now = GetTickCount();
    DWORD idleTime = now - pState->lastActivity;
    
    // 1. Determine period
    double period = (idleTime < CARET_IDLE_TIMEOUT) ? 1000.0 : 3000.0;

    // 2. Increment and WRAP the clock
    pState->animationTime += 16.0;
    if (pState->animationTime >= period) {
        pState->animationTime -= period; // Keeps the clock within 0 to 'period'
    }

    // 3. Normalized time is now much more stable
    double t = pState->animationTime / period; 

    float alpha = 0.0f;
    if (idleTime < CARET_IDLE_TIMEOUT) {
        // --- ACTIVE HEARTBEAT ---
        if (t < 0.20)         alpha = (float)(t / 0.20);              // Beat 1: Fade Up
        else if (t < 0.22)    alpha = 0.0f;                           // Snap Out
        else if (t < 0.32)    alpha = 1.0f;                           // Beat 2: Pop In
        else if (t < 0.50)    alpha = (float)(1.0 - (t - 0.32)/0.18); // Fade Out
        else                  alpha = 0.0f;                           // Rest
    } 
    else {
        // --- IDLE MODE: SMOOTH FADE (Single Glow) ---
        period = 3000.0; 
        t = fmod(pState->animationTime, period) / period;
        
        // Sine wave mapped to 0.0 - 1.0 range
        alpha = (float)((sin(2.0 * M_PI * t - M_PI / 2.0) + 1.0) / 2.0);
    }

    pState->caretAlpha = alpha;

    // Trigger redraw for caret only
    int caretWidth = 1;
    RECT rcCaret = { 
        pState->caretX - (caretWidth / 2), 
        pState->caretY, 
        pState->caretX + (caretWidth / 2) + 1, 
        pState->caretY + pState->lineHeight 
    };
    InvalidateRect(hwnd, &rcCaret, FALSE);

    return 0;
}

// ------------------------------------------------------------
// Command resolution (short + long forms)
// ------------------------------------------------------------

typedef struct {
    const WCHAR* message; // Static text describing the issue
    int caretCol;         // 0-based column in the visible prompt (':' is column 0)
    BOOL showCaret;
} CommandError;

static ExCommandType ResolveCommand(const WCHAR* cmd)
{
    if (_wcsicmp(cmd, L"w") == 0 ||
        _wcsicmp(cmd, L"write") == 0)
        return EXCMD_WRITE;

    if (_wcsicmp(cmd, L"wq") == 0)
        return EXCMD_WRITE_QUIT;

    if (_wcsicmp(cmd, L"q") == 0 ||
        _wcsicmp(cmd, L"quit") == 0)
        return EXCMD_QUIT;

    if (_wcsicmp(cmd, L"e") == 0 ||
        _wcsicmp(cmd, L"edit") == 0)
        return EXCMD_EDIT;

    if (_wcsicmp(cmd, L"s") == 0 ||
        _wcsicmp(cmd, L"search") == 0)
        return EXCMD_SEARCH;

    return EXCMD_NONE;
}


// Ex command parser
// Grammar: : <command> [!] [args]
static BOOL ParseExCommand(WCHAR* text, ExCommand* out)
{
    ZeroMemory(out, sizeof(*out));

    WCHAR* p = text;

    // Skip leading whitespace
    while (iswspace(*p))
        p++;

    // Parse command word
    WCHAR cmd[32] = {0};
    int len = 0;

    while (*p && !iswspace(*p) && *p != L'!' && len < 31)
        cmd[len++] = *p++;

    cmd[len] = L'\0';

    out->type = ResolveCommand(cmd);
    if (out->type == EXCMD_NONE)
        return FALSE;

    out->searchBackwards = FALSE;
    out->searchCaseSensitive = FALSE;
    out->arg = NULL;

    // Optional force modifier
    if (*p == L'!')
    {
        out->force = TRUE;
        p++;
    }

    // Skip whitespace before arguments
    while (iswspace(*p))
        p++;

    // Optional argument (filename, possibly quoted)
    if (*p) {
        if (out->type == EXCMD_SEARCH) {
            // Pattern parsing (quoted or single token), then optional direction token
            if (*p == L'"') {
                p++; // move past opening quote
                out->arg = p;
                while (*p && *p != L'"') p++;
                if (*p == L'"') {
                    *p = L'\0';
                    p++; // move past closing quote for direction parsing
                }
            } else {
                out->arg = p;
                while (*p && !iswspace(*p)) p++;
                if (*p) {
                    *p = L'\0';
                    p++;
                }
            }

            // Skip whitespace before optional direction
            while (iswspace(*p)) p++;
            if (*p) {
                const WCHAR* dir = p;
                while (*p && !iswspace(*p)) p++;
                // No need to null-terminate; compare length-limited
                if (_wcsnicmp(dir, L"backward", 8) == 0 || _wcsnicmp(dir, L"b", 1) == 0) {
                    out->searchBackwards = TRUE;
                } else if (_wcsnicmp(dir, L"forward", 7) == 0 || _wcsnicmp(dir, L"f", 1) == 0) {
                    out->searchBackwards = FALSE;
                }
            }

            // Case sensitivity: uppercase command implies case-sensitive
            if (wcscmp(cmd, L"S") == 0 || wcscmp(cmd, L"SEARCH") == 0) {
                out->searchCaseSensitive = TRUE;
            }
        } else {
            if (*p == L'"')
            {
                // Quoted argument: strip surrounding quotes in-place
                p++; // move past opening quote
                out->arg = p;
                while (*p && *p != L'"')
                    p++;
                if (*p == L'"') {
                    *p = L'\0'; // terminate at closing quote
                }
            }
            else
            {
                out->arg = p;
            }
        }
    }

    return TRUE;
}

// Ex command execution
static void ExecuteExCommand(HWND hwnd, const ExCommand* cmd)
{
    if (cmd->type == EXCMD_SEARCH) {
        ViewState* pState = GetState(hwnd);
        if (!pState || !pState->pDoc) {
            MessageBoxW(hwnd, L"No document is open.", L"Find", MB_OK | MB_ICONINFORMATION);
            return;
        }

        if (!cmd->arg || wcslen(cmd->arg) == 0) {
            MessageBoxW(hwnd, L"Enter text to search for.", L"Find", MB_OK | MB_ICONINFORMATION);
            return;
        }

        size_t startOffset = pState->cursorOffset;
        DocSearchResult res = Doc_Search(pState->pDoc, cmd->arg, wcslen(cmd->arg), startOffset, cmd->searchBackwards, cmd->searchCaseSensitive);
        if (res.status == DOC_SEARCH_MATCH) {
            View_ApplySearchResult(hwnd, &res);
        } else {
            const WCHAR* msg = NULL;
            if (res.status == DOC_SEARCH_REACHED_EOF) msg = L"Reached end of file without a match.";
            else if (res.status == DOC_SEARCH_REACHED_BOF) msg = L"Reached beginning of file without a match.";
            else msg = L"Pattern not found.";
            MessageBoxW(hwnd, msg, L"Find", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    switch (cmd->type)
    {
        case EXCMD_WRITE:
        {
            SendMessage(GetParent(hwnd),
                        WM_APP_SAVE_FILE,
                        0,
                        (LPARAM)cmd->arg);
            break;
        }

        case EXCMD_WRITE_QUIT:
        {
            SendMessage(GetParent(hwnd),
                        WM_APP_SAVE_FILE,
                        0,
                        (LPARAM)cmd->arg);

            SendMessage(GetParent(hwnd),
                        WM_CLOSE,
                        cmd->force,
                        0);
            break;
        }

        case EXCMD_QUIT:
        {
            SendMessage(GetParent(hwnd),
                        WM_CLOSE,
                        cmd->force,
                        0);
            break;
        }

        case EXCMD_EDIT:
        {
            if (!cmd->arg)
                break;
            else
                SendMessage(GetParent(hwnd),
                            WM_APP_OPEN_FILE,
                            0,
                            (LPARAM)cmd->arg);
            break;
        }

        default:
            break;
    }
}


// Entry point: called when user submits a command line
static BOOL ProcessCommandText(HWND hwnd, WCHAR* text, CommandError* pError)
{
    if (!text)
        return FALSE;

    // Treat the command buffer as an Ex command; the visible ':' prompt is not stored in the buffer
    WCHAR* pCmd = (text[0] == L':') ? text + 1 : text;
    WCHAR* wordStart = pCmd;
    while (*wordStart && iswspace(*wordStart)) wordStart++;

    // Blank or whitespace-only command: treat as a no-op so Enter can dismiss cleanly
    if (!*wordStart)
        return TRUE;

    int cmdWordLen = 0;
    while (wordStart[cmdWordLen] && !iswspace(wordStart[cmdWordLen]) && wordStart[cmdWordLen] != L'!')
        cmdWordLen++;

    ExCommand cmd;
    if (!ParseExCommand(pCmd, &cmd))
    {
        if (pError) {
            pError->message = L"unknown command";
            pError->caretCol = 1 + (int)(wordStart - pCmd);
            pError->showCaret = TRUE;
        }
        return FALSE;
    }

    if (cmd.type == EXCMD_EDIT && !cmd.arg)
    {
        if (pError) {
            int caretCol = 1 + (int)(wordStart - pCmd) + cmdWordLen + (cmd.force ? 1 : 0);
            pError->message = L"file name required";
            pError->caretCol = caretCol;
            pError->showCaret = TRUE;
        }
        return FALSE;
    }

    if (pError) {
        pError->message = NULL;
        pError->showCaret = FALSE;
        pError->caretCol = 0;
    }

    ExecuteExCommand(hwnd, &cmd);
    return TRUE;
}

static void ExitCommandMode(HWND hwnd, ViewState* pState) {
    ClearCommandFeedback(pState);
    pState->bCommandMode = FALSE;
    pState->commandLen = 0;
    pState->commandCaretPos = 0;
    pState->szCommandBuf[0] = L'\0';

    InvalidateRect(hwnd, NULL, TRUE);
    UpdateScrollbars(hwnd, pState);
    UpdateCaretPosition(hwnd, pState);
}

static void SubmitCommand(HWND hwnd, ViewState* pState) {
    WCHAR commandCopy[256];
    size_t copyLen = pState->commandLen;
    if (copyLen >= _countof(commandCopy)) copyLen = _countof(commandCopy) - 1;
    if (copyLen > 0) {
        memmove(commandCopy, pState->szCommandBuf, copyLen * sizeof(WCHAR));
    }
    commandCopy[copyLen] = L'\0';

    ClearCommandFeedback(pState);

    CommandError err = {0};
    BOOL success = ProcessCommandText(hwnd, commandCopy, &err);

    if (success) {
        ExitCommandMode(hwnd, pState);
    } else {
        if (err.message) {
            int caretCol = (err.caretCol < 0) ? 0 : err.caretCol;
            SetCommandFeedback(pState, err.message, caretCol, err.showCaret);
        }
        UpdateScrollbars(hwnd, pState);
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateCaretPosition(hwnd, pState);
    }
}

static LRESULT HandleChar(HWND hwnd, ViewState* pState, WPARAM wParam) {
    WCHAR c = (WCHAR)wParam;

    ResetCaretBlink(pState);

    // --- 1. COMMAND MODE INTERCEPTOR ---
    if (pState->bCommandMode) {
        
        // Block Enter, Escape, and Tab from reaching the document logic
        if (c == L'\r' || c == L'\n') {
            SubmitCommand(hwnd, pState);
            return 0; // Character is eaten, document is safe
        } else if (c == 27) {
            ExitCommandMode(hwnd, pState);
            return 0; // Character is eaten, document is safe
        }
        
        // Skip control characters (Back, Enter, Esc) handled in WM_KEYDOWN
        // We only want to capture printable text (ASCII 32 and above)
        if (c < 32 || c == 127) return 0;

        // Append to the command buffer if there is space (leaving room for null terminator)
        if (pState->commandLen < 255) {
            if (pState->bCommandFeedback) ClearCommandFeedback(pState);
            // If the caret is not at the end, shift text to the right to make room (Insert mode behavior)
            if (pState->commandCaretPos < pState->commandLen) {
                memmove(&pState->szCommandBuf[pState->commandCaretPos + 1], 
                        &pState->szCommandBuf[pState->commandCaretPos], 
                        (pState->commandLen - pState->commandCaretPos) * sizeof(WCHAR));
            }
            
            pState->szCommandBuf[pState->commandCaretPos] = c;
            pState->commandLen++;
            pState->commandCaretPos++;
            pState->szCommandBuf[pState->commandLen] = L'\0';
            
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateCaretPosition(hwnd, pState);
        }
        return 0;
    }

    // --- 2. STANDARD EDITOR LOGIC ---
    
    // Skip control characters that are processed in WM_KEYDOWN (Backspace/Delete)
    if (c == 8 || c == 127) return 0;
    
    // Process printable characters, tabs, and newlines
    if (c == L'\r' || c == L'\n' || (c >= 32) || c == L'\t') {
        if (c == L'\r') c = L'\n'; // Normalize to LF

        // Replace any highlighted selection with the typed character
        size_t selStart = 0, selLen = 0;
        if (View_GetSelection(pState, &selStart, &selLen)) {
            Doc_Delete(pState->pDoc, selStart, selLen);
            pState->cursorOffset = pState->selectionAnchor = selStart;
        } else if (!pState->bInsertMode && c != L'\n') {
            // Overtype logic: Remove the next character if we aren't at EOF
            if (pState->cursorOffset < pState->pDoc->total_length) {
                WCHAR nextChar;
                Doc_GetText(pState->pDoc, pState->cursorOffset, 1, &nextChar);
                // Don't overtype the newline; it preserves the document's line structure
                if (nextChar != L'\n') {
                    Doc_Delete(pState->pDoc, pState->cursorOffset, 1);
                }
            }
        }

        // Insert the character and collapse the selection/anchor
        Doc_Insert(pState->pDoc, pState->cursorOffset, &c, 1);
        pState->cursorOffset++;
        pState->selectionAnchor = pState->cursorOffset;

        // Maintain visibility if the user typed a newline
        if (c == L'\n') {
            EnsureCursorVisible(hwnd, pState);
        }

        // Notify parent so the status bar and "Modified" flag update
        NotifyParent(hwnd, EN_CHANGE);

        UpdateCaretPosition(hwnd, pState);
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateWindow(hwnd); // Ensure immediate visual feedback
    }
    
    UpdateScrollbars(hwnd, pState);
    return 0;
}

static LRESULT HandleMouseWheel(HWND hwnd, ViewState* pState, WPARAM wParam) {
    UINT scrollLines = 3;
    SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &scrollLines, 0);
    
    int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
    int linesToScroll = (zDelta / WHEEL_DELTA) * scrollLines;
    
    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientHeight = rc.bottom;
    
    int wrapWidth = rc.right - 10;
    int totalDocHeight = View_GetDocumentHeight(hwnd, pState, wrapWidth);
    
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

static LRESULT HandleKeyDown(HWND hwnd, ViewState* pState, WPARAM wParam, LPARAM lParam) {
    int line, col;
    Doc_GetOffsetInfo(pState->pDoc, pState->cursorOffset, &line, &col);
    BOOL isShiftPressed = (GetKeyState(VK_SHIFT) & 0x8000);
    BOOL isCtrlPressed = (GetKeyState(VK_CONTROL) & 0x8000);

    ResetCaretBlink(pState);

    // --- 1. COMMAND MODE INTERCEPTOR ---
    if (pState->bCommandMode) {
        switch (wParam) {
            case VK_LEFT:
                if (pState->commandCaretPos > 0) pState->commandCaretPos--;
                break;
            case VK_RIGHT:
                if (pState->commandCaretPos < pState->commandLen) pState->commandCaretPos++;
                break;
            case VK_HOME:
                pState->commandCaretPos = 0;
                break;
            case VK_END:
                pState->commandCaretPos = pState->commandLen;
                break;
            case VK_BACK:
                if (pState->commandCaretPos > 0) {
                    if (pState->bCommandFeedback) ClearCommandFeedback(pState);
                    // Shift buffer left to overwrite the character before the caret
                    memmove(&pState->szCommandBuf[pState->commandCaretPos - 1], 
                            &pState->szCommandBuf[pState->commandCaretPos], 
                            (pState->commandLen - pState->commandCaretPos + 1) * sizeof(WCHAR));
                    pState->commandLen--;
                    pState->commandCaretPos--;
                }
                break;
            case VK_DELETE:
                if (pState->commandCaretPos < pState->commandLen) {
                    if (pState->bCommandFeedback) ClearCommandFeedback(pState);
                    // Shift buffer left to overwrite the character at the caret
                    memmove(&pState->szCommandBuf[pState->commandCaretPos], 
                            &pState->szCommandBuf[pState->commandCaretPos + 1], 
                            (pState->commandLen - pState->commandCaretPos) * sizeof(WCHAR));
                    pState->commandLen--;
                }
                break;
            case VK_OEM_1: // ';' key
                if (isCtrlPressed) {
                    ExitCommandMode(hwnd, pState);
                    return 0;
                }
            break;
            case VK_RETURN:
            case VK_ESCAPE:
                // DO NOT set bCommandMode = FALSE here. 
                // Let WM_CHAR handle the state transition to "eat" the character.
            break;
            default:
                // CRITICAL: Allow other keys (like Space) to pass through to TranslateMessage
                // so they generate a WM_CHAR message.
                return DefWindowProc(hwnd, WM_KEYDOWN, wParam, lParam);
        }
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateCaretPosition(hwnd, pState);
        return 0; // Block all other keys from reaching the editor
    }

    // --- 2. STANDARD EDITOR KEYS ---
    switch (wParam) {
        // Caps Lock Handler: Update status bar immediately
        case VK_CAPITAL:
            NotifyParent(hwnd, EN_SELCHANGE);
            break;

        // Trigger Colon Prompt: CTRL + ;
        case VK_OEM_1: // ';' key
            if (isCtrlPressed) {
                ClearCommandFeedback(pState);
                pState->bCommandMode = TRUE;
                pState->commandLen = 0;      // RESET LENGTH
                pState->commandCaretPos = 0; // RESET CARET
                pState->szCommandBuf[0] = L'\0';
                InvalidateRect(hwnd, NULL, TRUE);
                UpdateScrollbars(hwnd, pState);
                return 0;
            }
            break;

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
            if (pState->bWordWrap) {
                int x, y;
                GetCursorVisualPos(hwnd, pState, pState->cursorOffset, &x, &y);
                RECT rc;
                GetClientRect(hwnd, &rc);
                int wrapWidth = rc.right - 10;
                int totalH = GetTotalWrappedHeight(hwnd, pState, wrapWidth);
                int targetY = y - pState->lineHeight;
                if (targetY < 0) targetY = 0;
                int maxY = (totalH > pState->lineHeight) ? (totalH - pState->lineHeight) : 0;
                if (targetY > maxY) targetY = maxY;
                pState->cursorOffset = View_XYToOffset(hwnd, x, targetY);
            } else {
                if (line > 1) {
                    size_t prevLineStart = Doc_GetLineOffset(pState->pDoc, line - 2);
                    size_t prevLineEnd = Doc_GetLineOffset(pState->pDoc, line - 1);
                    size_t prevLen = prevLineEnd - prevLineStart;
                    size_t newCol = (col > (int)prevLen) ? prevLen : (size_t)col;
                    pState->cursorOffset = prevLineStart + (newCol > 0 ? newCol - 1 : 0);
                }
            }
            if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
            NotifyParent(hwnd, EN_SELCHANGE);
            break;

        case VK_DOWN:
            if (pState->bWordWrap) {
                int x, y;
                GetCursorVisualPos(hwnd, pState, pState->cursorOffset, &x, &y);
                RECT rc;
                GetClientRect(hwnd, &rc);
                int wrapWidth = rc.right - 10;
                int totalH = GetTotalWrappedHeight(hwnd, pState, wrapWidth);
                int targetY = y + pState->lineHeight;
                int maxY = (totalH > pState->lineHeight) ? (totalH - pState->lineHeight) : 0;
                if (targetY < 0) targetY = 0;
                if (targetY > maxY) targetY = maxY;
                pState->cursorOffset = View_XYToOffset(hwnd, x, targetY);
            } else{
                if (line < (int)pState->pDoc->line_count) {
                    size_t nextLineStart = Doc_GetLineOffset(pState->pDoc, line);
                    size_t nextLineEnd = Doc_GetLineOffset(pState->pDoc, line + 1);
                    size_t nextLen = nextLineEnd - nextLineStart;
                    size_t newCol = (col > (int)nextLen) ? nextLen : (size_t)col;
                    pState->cursorOffset = nextLineStart + (newCol > 0 ? newCol - 1 : 0);
                }
                if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
                NotifyParent(hwnd, EN_SELCHANGE);
            }
            break;

        case VK_HOME:
            pState->cursorOffset = Doc_GetLineOffset(pState->pDoc, line - 1);
            if (!isShiftPressed) pState->selectionAnchor = pState->cursorOffset;
            NotifyParent(hwnd, EN_SELCHANGE);
            break;

        case VK_END:
            pState->cursorOffset = Doc_GetLineOffset(pState->pDoc, line);
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
            size_t delStart = 0, delLen = 0;
            if (View_GetSelection(pState, &delStart, &delLen)) {
                Doc_Delete(pState->pDoc, delStart, delLen);
                pState->cursorOffset = pState->selectionAnchor = delStart;
                NotifyParent(hwnd, EN_CHANGE);
            } else {
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
            pState->bInsertMode = !pState->bInsertMode;
            NotifyParent(hwnd, EN_SELCHANGE);
            break;
    }
    UpdateScrollbars(hwnd, pState);
    EnsureCursorVisible(hwnd, pState);
    InvalidateRect(hwnd, NULL, TRUE);
    return 0;
}

static LRESULT HandlePaint(HWND hwnd, ViewState* pState) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    // Initialize double buffering
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP oldBM = (HBITMAP)SelectObject(memDC, memBM);

    // Use the colors from state
    COLORREF currentBg   = pState->bCommandMode ? pState->colorBgDim : pState->colorBg;
    COLORREF currentText = pState->colorText;
    COLORREF currentDim  = pState->colorDim;
    
    SelectObject(memDC, pState->hFont);

    // Prepare font metrics
    TEXTMETRIC tm;
    GetTextMetrics(memDC, &tm);
    int charWidth = tm.tmAveCharWidth;
    int tabStops = charWidth * 4; 

    size_t selStart = (pState->cursorOffset < pState->selectionAnchor) ? pState->cursorOffset : pState->selectionAnchor;
    size_t selEnd = (pState->cursorOffset < pState->selectionAnchor) ? pState->selectionAnchor : pState->cursorOffset;
    BOOL hasFocus = (GetFocus() == hwnd);

    // Clear the back buffer
    HBRUSH hBg = CreateSolidBrush(currentBg);
    SetBkMode(memDC, TRANSPARENT); 
    FillRect(memDC, &rc, hBg);
    DeleteObject(hBg);

    if (pState->pDoc && pState->pDoc->line_count > 0) {
        if (pState->bWordWrap) {
            PaintWrappedContent(pState, memDC, rc, tabStops, currentDim);
        } else {
            PaintUnwrappedContent(pState, memDC, rc, tabStops, currentBg, currentText, currentDim, selStart, selEnd, hasFocus);
        }
    }

    PaintCommandOverlay(pState, memDC, rc);

    // Update Caret Position and flip buffers
    UpdateCaretPosition(hwnd, pState);

    DrawCustomCaret(memDC, pState);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
    
    SelectObject(memDC, oldBM);
    DeleteObject(memBM);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
    return 0;
}

static LRESULT HandleContextMenu(HWND hwnd, ViewState* pState, LPARAM lParam) {
    if (!pState || !pState->pDoc) return 0;

    SetFocus(hwnd);
    ResetCaretBlink(pState);

    if (pState->bCommandMode) {
        ExitCommandMode(hwnd, pState);
    }

    POINT screenPt;
    BOOL keyboardInvoke = (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1);
    if (keyboardInvoke) {
        GetCaretPos(&screenPt);
        ClientToScreen(hwnd, &screenPt);
    } else {
        screenPt.x = GET_X_LPARAM(lParam);
        screenPt.y = GET_Y_LPARAM(lParam);

        POINT clientPt = screenPt;
        ScreenToClient(hwnd, &clientPt);

        size_t clickOffset = GetOffsetFromPoint(hwnd, pState, clientPt.x, clientPt.y);
        size_t selStart = 0, selLen = 0;
        BOOL hasSel = View_GetSelection(pState, &selStart, &selLen);
        BOOL insideSel = hasSel && clickOffset >= selStart && clickOffset < selStart + selLen;
        if (!insideSel) {
            pState->selectionAnchor = clickOffset;
            pState->cursorOffset = clickOffset;
            NotifyParent(hwnd, EN_SELCHANGE);
            EnsureCursorVisible(hwnd, pState);
            UpdateCaretPosition(hwnd, pState);
            InvalidateRect(hwnd, NULL, FALSE);
        }
    }

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_EDIT_CUT, L"Cut");
    AppendMenuW(hMenu, MF_STRING, ID_EDIT_COPY, L"Copy");
    AppendMenuW(hMenu, MF_STRING, ID_EDIT_PASTE, L"Paste");
    AppendMenuW(hMenu, MF_STRING, ID_EDIT_DELETE, L"Delete");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_EDIT_SELECT_ALL, L"Select All");

    size_t selStart = 0, selLen = 0;
    BOOL hasSelection = View_GetSelection(pState, &selStart, &selLen);
    UINT selFlags = hasSelection ? MF_ENABLED : MF_GRAYED;
    EnableMenuItem(hMenu, ID_EDIT_CUT, MF_BYCOMMAND | selFlags);
    EnableMenuItem(hMenu, ID_EDIT_COPY, MF_BYCOMMAND | selFlags);
    EnableMenuItem(hMenu, ID_EDIT_DELETE, MF_BYCOMMAND | selFlags);

    BOOL canPaste = ClipboardHasText(hwnd);
    EnableMenuItem(hMenu, ID_EDIT_PASTE, MF_BYCOMMAND | (canPaste ? MF_ENABLED : MF_GRAYED));

    UINT cmd = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPt.x, screenPt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);

    switch (cmd) {
        case ID_EDIT_CUT: 
            View_Cut(hwnd); 
            NotifyParent(hwnd, EN_SELCHANGE);
            break;
        case ID_EDIT_COPY: 
            View_Copy(hwnd); 
            break;
        case ID_EDIT_PASTE: 
            View_Paste(hwnd); 
            NotifyParent(hwnd, EN_SELCHANGE);
            break;
        case ID_EDIT_DELETE: 
            DeleteSelection(hwnd, pState); 
            NotifyParent(hwnd, EN_SELCHANGE);
            break;
        case ID_EDIT_SELECT_ALL:
            View_SelectAll(hwnd);
            NotifyParent(hwnd, EN_SELCHANGE);
            break;
        default:
            break;
    }

    if (cmd != 0) {
        EnsureCursorVisible(hwnd, pState);
        UpdateCaretPosition(hwnd, pState);
        UpdateScrollbars(hwnd, pState);
    }

    return 0;
}

static LRESULT HandleMouseMove(HWND hwnd, ViewState* pState, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
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

static LRESULT HandleLButtonUp(HWND hwnd, ViewState* pState) {
    (void)hwnd;
    pState->isDragging = FALSE;
    ReleaseCapture();
    return 0;
}

static LRESULT HandleVScroll(HWND hwnd, ViewState* pState, WPARAM wParam) {
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(hwnd, SB_VERT, &si);
    
    int oldY = pState->scrollY;
    int newY = oldY;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientHeight = rc.bottom;
    int wrapWidth = rc.right - 10;
    
    // Calculate safe scroll bounds
    int totalDocHeight = View_GetDocumentHeight(hwnd, pState, wrapWidth);

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

static LRESULT HandleHScroll(HWND hwnd, ViewState* pState, WPARAM wParam) {
    if (pState->bWordWrap) return 0; // no horizontal scroll when wrapping

    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetScrollInfo(hwnd, SB_HORZ, &si);

    int oldX = pState->scrollX;
    int newX = oldX;

    RECT rc;
    GetClientRect(hwnd, &rc);
    int clientWidth = rc.right;
    int docWidth = View_GetDocumentWidth(hwnd, pState);
    int maxScroll = docWidth - clientWidth;
    if (maxScroll < 0) maxScroll = 0;

    HDC hdc = GetDC(hwnd);
    SelectObject(hdc, pState->hFont);
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int charWidth = tm.tmAveCharWidth;
    ReleaseDC(hwnd, hdc);

    switch (LOWORD(wParam)) {
        case SB_LEFT:      newX = 0; break;
        case SB_RIGHT:     newX = maxScroll; break;
        case SB_LINELEFT:  newX -= charWidth; break;
        case SB_LINERIGHT: newX += charWidth; break;
        case SB_PAGELEFT:  newX -= clientWidth; break;
        case SB_PAGERIGHT: newX += clientWidth; break;
        case SB_THUMBTRACK:newX = si.nTrackPos; break;
        default: break;
    }

    if (newX < 0) newX = 0;
    if (newX > maxScroll) newX = maxScroll;

    if (newX != oldX) {
        pState->scrollX = newX;
        SetScrollPos(hwnd, SB_HORZ, pState->scrollX, TRUE);
        UpdateCaretPosition(hwnd, pState);
        InvalidateRect(hwnd, NULL, TRUE);
    }
    return 0;
}

static LRESULT HandleSize(HWND hwnd, ViewState* pState, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    if (pState && pState->pDoc) {
        SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE };
        si.nMin = 0;
        if (pState->bWordWrap) {
            RECT rcSize;
            GetClientRect(hwnd, &rcSize);
            int wrapWidth = rcSize.right - 10;
            si.nMax = View_GetDocumentHeight(hwnd, pState, wrapWidth);
            si.nPage = rcSize.bottom;
        } else {
            si.nMax = View_GetDocumentHeight(hwnd, pState, 0);
            si.nPage = HIWORD(lParam);
        }
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        UpdateScrollbars(hwnd, pState);
    }
    return 0;
}

static LRESULT HandleSetFocus(HWND hwnd, ViewState* pState) {
    CreateCaret(hwnd, NULL, 1, pState->lineHeight); //
    ResetCaretBlink(pState);
    
    // Start the animation only when focused
    SetTimer(hwnd, IDT_CARET, 16, NULL); 
    
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
}

static LRESULT HandleKillFocus(HWND hwnd, ViewState* pState) {
    // Stop the timer immediately to save CPU
    KillTimer(hwnd, IDT_CARET); 
    
    DestroyCaret(); //
    pState->caretAlpha = 0.0f; 
    InvalidateRect(hwnd, NULL, FALSE);
    return 0;
}

static LRESULT HandleLButtonDown(HWND hwnd, ViewState* pState, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    // Ensure the window has focus so the caret appears
    SetFocus(hwnd);

    ResetCaretBlink(pState);

    // Convert click coordinates to a logical document offset
    size_t offset = GetOffsetFromPoint(hwnd, pState, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
    
    // Dismiss command mode if active (after capturing position for accurate mapping)
    if (pState->bCommandMode) {
            ExitCommandMode(hwnd, pState);
    }
    
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

static LRESULT HandleLButtonDblClk(HWND hwnd, ViewState* pState, WPARAM wParam, LPARAM lParam) {
    (void)wParam;
    if (!pState || !pState->pDoc) return 0;

    SetFocus(hwnd);
    ResetCaretBlink(pState);

    size_t offset = GetOffsetFromPoint(hwnd, pState, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));

    if (pState->bCommandMode) {
        ExitCommandMode(hwnd, pState);
    }

    size_t wordStart = 0, wordEnd = 0;
    if (View_GetWordBounds(pState->pDoc, offset, &wordStart, &wordEnd)) {
        pState->selectionAnchor = wordStart;
        pState->cursorOffset = wordEnd;
    } else {
        pState->selectionAnchor = offset;
        pState->cursorOffset = offset;
    }

    pState->isDragging = FALSE;

    NotifyParent(hwnd, EN_SELCHANGE);
    EnsureCursorVisible(hwnd, pState);
    UpdateCaretPosition(hwnd, pState);
    InvalidateRect(hwnd, NULL, TRUE);
    return 0;
}

static LRESULT HandleDestroy(ViewState* pState) {
    if (pState->hCaretBm) DeleteObject(pState->hCaretBm);
    DeleteObject(pState->hFont);
    free(pState);
    return 0;
}

LRESULT CALLBACK ViewportProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    ViewState* pState = GetState(hwnd);

    switch (uMsg) {
        case WM_CREATE:      return HandleCreate(hwnd);
        case WM_TIMER:       return HandleTimer(hwnd, pState, wParam);
        case WM_ERASEBKGND:  return 1; // handled to avoid flash
        case WM_CHAR:        return HandleChar(hwnd, pState, wParam);
        case WM_MOUSEWHEEL:  return HandleMouseWheel(hwnd, pState, wParam);
        case WM_KEYDOWN:     return HandleKeyDown(hwnd, pState, wParam, lParam);
        case WM_PAINT:       return HandlePaint(hwnd, pState);
        case WM_MOUSEMOVE:   return HandleMouseMove(hwnd, pState, wParam, lParam);
        case WM_LBUTTONUP:   return HandleLButtonUp(hwnd, pState);
        case WM_HSCROLL:     return HandleHScroll(hwnd, pState, wParam);
        case WM_VSCROLL:     return HandleVScroll(hwnd, pState, wParam);
        case WM_SIZE:        return HandleSize(hwnd, pState, wParam, lParam);
        case WM_SETFOCUS:    return HandleSetFocus(hwnd, pState);
        case WM_KILLFOCUS:   return HandleKillFocus(hwnd, pState);
        case WM_LBUTTONDBLCLK:return HandleLButtonDblClk(hwnd, pState, wParam, lParam);
        case WM_LBUTTONDOWN: return HandleLButtonDown(hwnd, pState, wParam, lParam);
        case WM_CONTEXTMENU: return HandleContextMenu(hwnd, pState, lParam);
        case WM_DESTROY:     return HandleDestroy(pState);
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
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    return RegisterClass(&wc);
}
