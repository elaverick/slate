/**
 * slate.c - Implementation of the Slate application
 * Coordinates between the Document Core and the Virtual Viewport.
 */

#include "slate.h"
#include "slate_doc.h"
#include "slate_view.h"

// Global application state
SLATE_APP g_app = {0};

/**
 * Creates the menu bar for the application
 */
HMENU CreateMenuBar(void) {
    HMENU hMenuBar = CreateMenu();
    
    // File menu
    HMENU hFileMenu = CreatePopupMenu();
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_NEW, _T("&New\tCtrl+N"));
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_OPEN, _T("&Open...\tCtrl+O"));
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_SAVE, _T("&Save\tCtrl+S"));
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_SAVE_AS, _T("Save &As..."));
    AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hFileMenu, MF_STRING, ID_FILE_EXIT, _T("E&xit"));
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFileMenu, _T("&File"));
    
    // Edit menu
    HMENU hEditMenu = CreatePopupMenu();
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_UNDO, _T("&Undo\tCtrl+Z"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_REDO, _T("&Redo\tCtrl+Y"));
    AppendMenu(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_CUT, _T("Cu&t\tCtrl+X"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_COPY, _T("&Copy\tCtrl+C"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_PASTE, _T("&Paste\tCtrl+V"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_DELETE, _T("De&lete\tDel"));
    AppendMenu(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_SELECT_ALL, _T("Select &All\tCtrl+A"));
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hEditMenu, _T("&Edit"));

    // Help menu
    HMENU hHelpMenu = CreatePopupMenu();
    AppendMenu(hHelpMenu, MF_STRING, ID_HELP_ABOUT, _T("&About Slate..."));
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hHelpMenu, _T("&Help"));
    
    return hMenuBar;
}

/**
 * Updates the window title with the current file name and modified status
 */
void UpdateTitleBar(SLATE_APP* app) {
    TCHAR szTitle[MAX_FILE_PATH + 64];
    const TCHAR* pszFile = (_tcslen(app->szFileName) > 0) ? app->szFileName : _T("Untitled");
    _stprintf_s(szTitle, _countof(szTitle), _T("%s%s - %s"), 
               pszFile, app->bIsModified ? _T("*") : _T(""), APP_NAME);
    SetWindowText(app->hwnd, szTitle);
}

/**
 * Updates the status bar parts (Line, Col, etc.)
 */
void UpdateStatusBar(SLATE_APP* app) {
    if (!app->hStatus) return;

    // In the new architecture, we query the View for cursor position
    // then translate that logical offset to Line/Col via the Doc
    int line = 1, col = 1;
    size_t offset = View_GetCursorOffset(app->hEdit);
    Doc_GetOffsetInfo(app->pDoc, offset, &line, &col);

    TCHAR szStatus[64];
    _stprintf_s(szStatus, _countof(szStatus), _T("Ln %d, Col %d"), line, col);
    SendMessage(app->hStatus, SB_SETTEXT, STATUS_PART_CURSOR, (LPARAM)szStatus);

    BOOL isInsert = View_IsInsertMode(app->hEdit);
    SendMessage(app->hStatus, SB_SETTEXT, STATUS_PART_INSERT, (LPARAM)(isInsert ? _T("INS") : _T("OVR")));
    
    // Caps lock check
    BOOL caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    SendMessage(app->hStatus, SB_SETTEXT, STATUS_PART_CAPS, (LPARAM)(caps ? _T("CAPS") : _T("")));
}

/**
 * Memory-Mapped File Loader
 */
BOOL LoadFile(SLATE_APP* app, const TCHAR* pszFileName) {
    HANDLE hFile = CreateFile(pszFileName, GENERIC_READ, FILE_SHARE_READ, NULL, 
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    LARGE_INTEGER liSize;
    GetFileSizeEx(hFile, &liSize);
    if (liSize.QuadPart == 0) { 
        CloseHandle(hFile); 
        return (BOOL)SendMessage(app->hwnd, WM_COMMAND, ID_FILE_NEW, 0); 
    }

    // 1. Detect Encoding
    BYTE bom[3] = {0};
    DWORD dwRead;
    ReadFile(hFile, bom, 3, &dwRead, NULL);
    
    BOOL isUtf8 = TRUE;
    size_t skip = 0;

    if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        // UTF-8 with BOM
        isUtf8 = TRUE;
        skip = 3;
    } else if (bom[0] == 0xFF && bom[1] == 0xFE) {
        // UTF-16 LE
        isUtf8 = FALSE;
        skip = 2;
    } else {
        // Default to UTF-8/ANSI (No BOM)
        isUtf8 = TRUE;
        skip = 0;
    }

    // 2. Map the file into memory
    HANDLE hMap = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) {
        CloseHandle(hFile);
        return FALSE;
    }

    void* pMapViewBase = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!pMapViewBase) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    // 3. Initialize Document with Lazy-Loading pointers
    void* pTextStart = (BYTE*)pMapViewBase + skip;
    size_t rawLen = (size_t)(liSize.QuadPart - skip);
    
    // For UTF-8, char count initially equals byte count.
    // For UTF-16, char count is half the byte count.
    size_t charLen = isUtf8 ? rawLen : (rawLen / sizeof(WCHAR));
    
    // Call the updated CreateFromMap that stores the base pointer and encoding flag
    SlateDoc* pNewDoc = Doc_CreateFromMap(pTextStart, charLen, hMap, pMapViewBase, isUtf8);
    
    if (!pNewDoc) {
        UnmapViewOfFile(pMapViewBase);
        CloseHandle(hMap);
        CloseHandle(hFile);
        return FALSE;
    }

    // 4. Update Application State
    if (app->pDoc) Doc_Destroy(app->pDoc);
    app->pDoc = pNewDoc;

    View_SetDocument(app->hEdit, app->pDoc);
    _tcscpy_s(app->szFileName, _countof(app->szFileName), pszFileName);
    app->bIsModified = FALSE;
    
    UpdateTitleBar(app);
    CloseHandle(hFile); // We can close the file handle; the mapping keeps the data accessible

    return TRUE;
}

/**
 * Persistence Strategy
 */
typedef struct { HANDLE hFile; BOOL bSuccess; } SAVE_CONTEXT;

void SaveStreamingCallback(const WCHAR* text, size_t len, void* ctx) {
    SAVE_CONTEXT* pCtx = (SAVE_CONTEXT*)ctx;
    if (!pCtx->bSuccess) return;
    DWORD dwWritten;
    if (!WriteFile(pCtx->hFile, text, (DWORD)(len * sizeof(WCHAR)), &dwWritten, NULL)) {
        pCtx->bSuccess = FALSE;
    }
}

BOOL SaveFile(SLATE_APP* app, const TCHAR* pszFileName) {
    if (!app->pDoc) return FALSE;

    HANDLE hFile = CreateFile(pszFileName, GENERIC_WRITE, 0, NULL, 
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    WCHAR bom = 0xFEFF;
    DWORD dwWritten;
    WriteFile(hFile, &bom, sizeof(WCHAR), &dwWritten, NULL);

    SAVE_CONTEXT ctx = { hFile, TRUE };
    Doc_StreamToBuffer(app->pDoc, SaveStreamingCallback, &ctx);
    CloseHandle(hFile);

    if (ctx.bSuccess) {
        _tcscpy_s(app->szFileName, _countof(app->szFileName), pszFileName);
        app->bIsModified = FALSE;
        UpdateTitleBar(app);
    }
    return ctx.bSuccess;
}

/**
 * Prompt for saving if modified
 */
int PromptSaveIfModified(SLATE_APP* app) {
    if (!app->bIsModified) return IDNO;

    TCHAR szMsg[MAX_FILE_PATH + 64];
    const TCHAR* pszFile = (_tcslen(app->szFileName) > 0) ? app->szFileName : _T("Untitled");
    _stprintf_s(szMsg, _countof(szMsg), _T("Do you want to save changes to %s?"), pszFile);

    int result = MessageBox(app->hwnd, szMsg, APP_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (result == IDYES) {
        if (_tcslen(app->szFileName) == 0) {
            SendMessage(app->hwnd, WM_COMMAND, ID_FILE_SAVE_AS, 0);
            return app->bIsModified ? IDCANCEL : IDYES; 
        } else {
            SaveFile(app, app->szFileName);
        }
    }
    return result;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            g_app.pDoc = Doc_CreateEmpty();
            g_app.bIsInsertMode = TRUE;
            
            // Create the Status Bar
            g_app.hStatus = CreateStatusWindow(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 
                                             _T("Ready"), hwnd, IDC_STATUSBAR);
            int parts[] = { 150, 250, 350 };
            SendMessage(g_app.hStatus, SB_SETPARTS, 3, (LPARAM)parts);

            // Create the Virtual Viewport
            HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
            g_app.hEdit = CreateWindowEx(
                WS_EX_CLIENTEDGE, _T("SlateView"), NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
                0, 0, 0, 0, hwnd, (HMENU)IDC_EDITOR, hInst, NULL
            );
            
            View_SetDocument(g_app.hEdit, g_app.pDoc);
            UpdateStatusBar(&g_app);
            return 0;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            
            SendMessage(g_app.hStatus, WM_SIZE, 0, 0);
            RECT rcStatus;
            GetWindowRect(g_app.hStatus, &rcStatus);
            int statusHeight = rcStatus.bottom - rcStatus.top;
            
            MoveWindow(g_app.hEdit, 0, 0, width, height - statusHeight, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_EDITOR && HIWORD(wParam) == EN_CHANGE) {
                g_app.bIsModified = TRUE;
                UpdateTitleBar(&g_app);
                UpdateStatusBar(&g_app);
                return 0;
            }
            
            switch (LOWORD(wParam)) {
                case ID_FILE_NEW:
                    if (PromptSaveIfModified(&g_app) != IDCANCEL) {
                        if (g_app.pDoc) Doc_Destroy(g_app.pDoc);
                        g_app.pDoc = Doc_CreateEmpty();
                        
                        // This is the handshake
                        View_SetDocument(g_app.hEdit, g_app.pDoc);
                        
                        // CRITICAL: Force the window to reclaim the caret
                        SetFocus(g_app.hEdit); 
                        
                        g_app.szFileName[0] = _T('\0');
                        g_app.bIsModified = FALSE;
                        UpdateTitleBar(&g_app);
                    }
                    return 0;

                case ID_FILE_OPEN: {
                    if (PromptSaveIfModified(&g_app) != IDCANCEL) {
                        OPENFILENAME ofn = { sizeof(ofn) };
                        TCHAR szFile[MAX_FILE_PATH] = { 0 };
                        ofn.hwndOwner = hwnd;
                        ofn.lpstrFile = szFile;
                        ofn.nMaxFile = MAX_FILE_PATH;
                        ofn.lpstrFilter = _T("Text Files\0*.txt\0All Files\0*.*\0");
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileName(&ofn)) {
                            LoadFile(&g_app, szFile);
                        }
                    }
                    break;
                }

                case ID_FILE_SAVE:
                    if (_tcslen(g_app.szFileName) > 0) {
                        SaveFile(&g_app, g_app.szFileName);
                    } else {
                        SendMessage(hwnd, WM_COMMAND, ID_FILE_SAVE_AS, 0);
                    }
                    break;

                case ID_FILE_SAVE_AS: {
                    OPENFILENAME ofn = { sizeof(ofn) };
                    TCHAR szFile[MAX_FILE_PATH] = { 0 };
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = MAX_FILE_PATH;
                    ofn.lpstrFilter = _T("Text Files\0*.txt\0All Files\0*.*\0");
                    ofn.Flags = OFN_OVERWRITEPROMPT;
                    if (GetSaveFileName(&ofn)) {
                        SaveFile(&g_app, szFile);
                    }
                    break;
                }

                case ID_FILE_EXIT:
                    SendMessage(hwnd, WM_CLOSE, 0, 0);
                    break;
                
                case ID_EDIT_UNDO:
                // We'll call a View helper that handles both the doc change and the visual refresh
                View_Undo(g_app.hEdit); 
                break;

                case ID_EDIT_SELECT_ALL:
                    View_SelectAll(g_app.hEdit);
                    break;

                case ID_EDIT_CUT:
                    View_Cut(g_app.hEdit);
                    break;
                
                case ID_EDIT_COPY:
                    View_Copy(g_app.hEdit);
                    break;
                
                case ID_EDIT_PASTE:
                    View_Paste(g_app.hEdit);
                    break;

                case ID_HELP_ABOUT:
                    MessageBox(hwnd, _T("Slate Editor v2.0\nMemory-Mapped Piece Table Edition"), 
                               _T("About Slate"), MB_OK | MB_ICONINFORMATION);
                    break;
            }
            return 0;
        }

        case WM_SETFOCUS:
            SetFocus(g_app.hEdit);
            return 0;

        case WM_CLOSE:
            if (PromptSaveIfModified(&g_app) != IDCANCEL) {
                if (g_app.pDoc) Doc_Destroy(g_app.pDoc);
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

BOOL InitializeApplication(HINSTANCE hInstance) {
    InitCommonControls();
    
    if (!View_Register(hInstance)) return FALSE;

    WNDCLASS wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClass(&wc)) return FALSE;

    g_app.hwnd = CreateWindow(
        WINDOW_CLASS_NAME, APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        NULL, CreateMenuBar(), hInstance, NULL
    );

    if (!g_app.hwnd) return FALSE;

    ShowWindow(g_app.hwnd, SW_SHOW);
    UpdateWindow(g_app.hwnd);
    UpdateTitleBar(&g_app);

    return TRUE;
}