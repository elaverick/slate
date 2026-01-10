/**
 * slate.c - Implementation of the Slate application
 * Contains the core functionality of the application
 */

#include "slate.h"

// Global application state
SLATE_APP g_app = {0};
WNDPROC g_originalEditProc = NULL;

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
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_CUT, _T("Cu&t\tCtrl+X"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_COPY, _T("&Copy\tCtrl+C"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_PASTE, _T("&Paste\tCtrl+V"));
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_DELETE, _T("&Delete\tDel"));
    AppendMenu(hEditMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hEditMenu, MF_STRING, ID_EDIT_SELECT_ALL, _T("Select &All\tCtrl+A"));
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hEditMenu, _T("&Edit"));
    
    // Help menu
    HMENU hHelpMenu = CreatePopupMenu();
    AppendMenu(hHelpMenu, MF_STRING, ID_HELP_HELP, _T("&Help\tF1"));
    AppendMenu(hHelpMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hHelpMenu, MF_STRING, ID_HELP_ABOUT, _T("&About Slate"));
    AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hHelpMenu, _T("&Help"));
    
    return hMenuBar;
}

/**
 * Creates the application window and its controls
 */
void CreateMainWindow(HINSTANCE hInstance) {
    // Register the window class
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIconSm = LoadIcon(hInstance, IDI_APPLICATION);
    
    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, _T("Window Registration Failed!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return;
    }
    
    // Create the menu bar
    HMENU hMenu = CreateMenuBar();
    
    // Create the main window
    g_app.hwnd = CreateWindowEx(
        0,
        WINDOW_CLASS_NAME,
        APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, hMenu, hInstance, NULL
    );
    
    if (g_app.hwnd == NULL) {
        MessageBox(NULL, _T("Window Creation Failed!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return;
    }
    
    // Create child controls (edit control and status bar)
    CreateControls(&g_app);
    
    // Initialize application state
    g_app.bIsModified = FALSE;
    g_app.bIsInsertMode = TRUE;
    g_app.szFileName[0] = _T('\0');
    
    // Update UI
    UpdateStatusBar(&g_app);
    UpdateTitleBar(&g_app);
    
    // Show the window
    ShowWindow(g_app.hwnd, SW_SHOW);
    UpdateWindow(g_app.hwnd);
}

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CHAR: {
            // Handle printable characters in overwrite mode
            if (!g_app.bIsInsertMode) {
                // Only for printable characters (not backspace, enter, etc.)
                if (wParam >= 0x20 && wParam <= 0x7E) {
                    // Get current selection
                    DWORD sel = (DWORD)SendMessage(hwnd, EM_GETSEL, 0, 0);
                    int selStart = LOWORD(sel);
                    int selEnd = HIWORD(sel);

                    // Only overwrite if no selection (i.e., caret is blinking)
                    if (selStart == selEnd) {
                        int textLen = (int)GetWindowTextLength(hwnd);
                        if (selStart < textLen) {
                            // Select the next character
                            SendMessage(hwnd, EM_SETSEL, selStart, selStart + 1);
                            // Replace it by allowing WM_CHAR to overwrite
                            // (the edit control will replace the selection)
                        }
                    }
                }
            }
            break;
        }

        case WM_KEYDOWN:
            if (wParam == VK_INSERT) {
                // Toggle mode
                g_app.bIsInsertMode = !g_app.bIsInsertMode;
                UpdateStatusBar(&g_app);
                return 0; // Swallow the key so edit control doesn't beep
            }
            // Also update on Caps Lock key? Optional, but safe:
            if (wParam == VK_CAPITAL) {
                UpdateStatusBar(&g_app);
                // Don't return — let edit control handle it (it does nothing, but harmless)
            }
            // Also update status on other keys that move cursor
            // (but you already do this in main wndproc — may be redundant)
            break;

        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            UpdateStatusBar(&g_app);
            break;
    }

    return CallWindowProc(g_originalEditProc, hwnd, uMsg, wParam, lParam);
}

/**
 * Creates the child controls for the main window
 */
void CreateControls(SLATE_APP* app) {
    // Create the edit control
    app->hEdit = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        _T("EDIT"),
        _T(""),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | 
        ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
        0, 0, 0, 0,
        app->hwnd,
        (HMENU)1,
        GetModuleHandle(NULL),
        NULL
    );

    // EM_SETLIMITTEXT with 0 sets the limit to the maximum possible (2GB-1)
    SendMessage(app->hEdit, EM_SETLIMITTEXT, 0, 0);

    // Subclass the edit control to intercept messages
    g_originalEditProc = (WNDPROC)SetWindowLongPtr(app->hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);
    
    // Create the status bar
    app->hStatus = CreateWindowEx(
        0,
        STATUSCLASSNAME,
        NULL,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        app->hwnd,
        (HMENU)2,
        GetModuleHandle(NULL),
        NULL
    );
    
    // Set status bar parts
    int parts[] = {200, 300, -1};
    SendMessage(app->hStatus, SB_SETPARTS, (WPARAM)_countof(parts), (LPARAM)parts);
}

/**
 * Updates the status bar with current cursor position, insert mode, and caps lock status
 */
void UpdateStatusBar(SLATE_APP* app) {
    if (app->hStatus == NULL || app->hEdit == NULL) return;
    
    // Get cursor position
    int pos = (int)SendMessageW(app->hEdit, EM_LINEFROMCHAR, -1, 0);
    int line = pos + 1;
    
    pos = (int)SendMessageW(app->hEdit, EM_LINEINDEX, -1, 0);
    DWORD sel = (DWORD)SendMessageW(app->hEdit, EM_GETSEL, 0, 0);
    int col = LOWORD(sel) - pos + 1;
    
    // Format cursor position text
    TCHAR szCursor[50];
    _stprintf_s(szCursor, _countof(szCursor), _T("Line %d, Col %d"), line, col);
    
    // Update cursor position part
    SendMessageW(app->hStatus, SB_SETTEXT, STATUS_PART_CURSOR, (LPARAM)szCursor);
    
    // Update insert/overwrite mode
    TCHAR* szMode = app->bIsInsertMode ? _T("INS") : _T("OVR");
    SendMessageW(app->hStatus, SB_SETTEXT, STATUS_PART_INSERT, (LPARAM)szMode);
    
    // Update caps lock status
    TCHAR* szCaps = (GetKeyState(VK_CAPITAL) & 0x0001) ? _T("CAPS") : _T("");
    SendMessageW(app->hStatus, SB_SETTEXT, STATUS_PART_CAPS, (LPARAM)szCaps);
}

/**
 * Updates the title bar to show document name and modified status
 */
void UpdateTitleBar(SLATE_APP* app) {
    TCHAR szTitle[300];
    
    if (app->szFileName[0] == _T('\0')) {
        _tcscpy_s(szTitle, _countof(szTitle), _T("Untitled - "));
    } else {
        // Extract file name from path
        TCHAR* pFileName = _tcsrchr(app->szFileName, _T('\\'));
        if (pFileName == NULL) {
            pFileName = app->szFileName;
        } else {
            pFileName++;
        }
        _stprintf_s(szTitle, _countof(szTitle), _T("%s - "), pFileName);
    }
    
    _tcscat_s(szTitle, _countof(szTitle), APP_NAME);
    
    if (app->bIsModified) {
        _tcscat_s(szTitle, _countof(szTitle), _T(" *"));
    }
    
    SetWindowText(app->hwnd, szTitle);
}

/**
 * Loads a file into the edit control
 */
BOOL LoadFile(SLATE_APP* app, const TCHAR* pszFileName) {
    HANDLE hFile = CreateFile(
        pszFileName,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBox(app->hwnd, _T("Could not open file!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    DWORD dwFileSize = GetFileSize(hFile, NULL);
    if (dwFileSize == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        MessageBox(app->hwnd, _T("Could not get file size!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    // For empty files
    if (dwFileSize == 0) {
        SetWindowText(app->hEdit, _T(""));
        CloseHandle(hFile);
        _tcscpy_s(app->szFileName, _countof(app->szFileName), pszFileName);
        app->bIsModified = FALSE;
        UpdateTitleBar(app);
        UpdateStatusBar(app);
        return TRUE;
    }

    // Read raw bytes
    BYTE* pBuffer = (BYTE*)GlobalAlloc(GPTR, dwFileSize + 2);
    if (!pBuffer) {
        CloseHandle(hFile);
        MessageBox(app->hwnd, _T("Not enough memory!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    DWORD dwBytesRead;
    if (!ReadFile(hFile, pBuffer, dwFileSize, &dwBytesRead, NULL)) {
        GlobalFree(pBuffer);
        CloseHandle(hFile);
        MessageBox(app->hwnd, _T("Could not read file!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }
    CloseHandle(hFile);

    pBuffer[dwBytesRead] = 0;
    pBuffer[dwBytesRead + 1] = 0;

    WCHAR* wszText = NULL;
    int textLen = 0;

    // Check for UTF-8 BOM
    if (dwBytesRead >= 3 && pBuffer[0] == 0xEF && pBuffer[1] == 0xBB && pBuffer[2] == 0xBF) {
        // UTF-8
        textLen = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)(pBuffer + 3), dwBytesRead - 3, NULL, 0);
        wszText = (WCHAR*)GlobalAlloc(GPTR, (textLen + 1) * sizeof(WCHAR));
        if (wszText) {
            MultiByteToWideChar(CP_UTF8, 0, (LPCCH)(pBuffer + 3), dwBytesRead - 3, wszText, textLen);
            wszText[textLen] = L'\0';
        }
    } else {
        // Attempt conversion as UTF-8 first
        textLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)pBuffer, dwBytesRead, NULL, 0);
        
        if (textLen > 0) {
            // Success! It's valid UTF-8 without a BOM
            wszText = (WCHAR*)GlobalAlloc(GPTR, (textLen + 1) * sizeof(WCHAR));
            if (wszText) {
                MultiByteToWideChar(CP_UTF8, 0, (LPCCH)pBuffer, dwBytesRead, wszText, textLen);
            }
        } else {
            // Fallback: Treat as ANSI (Current System Code Page)
            textLen = MultiByteToWideChar(CP_ACP, 0, (LPCCH)pBuffer, dwBytesRead, NULL, 0);
            wszText = (WCHAR*)GlobalAlloc(GPTR, (textLen + 1) * sizeof(WCHAR));
            if (wszText) {
                MultiByteToWideChar(CP_ACP, 0, (LPCCH)pBuffer, dwBytesRead, wszText, textLen);
            }
        }
    }

    GlobalFree(pBuffer);

    if (!wszText) {
        MessageBox(app->hwnd, _T("Conversion failed!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    SetWindowTextW(app->hEdit, wszText);
    GlobalFree(wszText);

    _tcscpy_s(app->szFileName, _countof(app->szFileName), pszFileName);
    app->bIsModified = FALSE;
    UpdateTitleBar(app);
    UpdateStatusBar(app);

    return TRUE;
}

/**
 * Saves the content of the edit control to a file
 */
BOOL SaveFile(SLATE_APP* app, const TCHAR* pszFileName) {
    HANDLE hFile = CreateFile(
        pszFileName,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBox(app->hwnd, _T("Could not create file!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    // Get text length in wide chars
    int len = GetWindowTextLengthW(app->hEdit);
    if (len == 0) {
        // Write UTF-8 BOM + nothing
        BYTE bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written;
        WriteFile(hFile, bom, 3, &written, NULL);
        CloseHandle(hFile);
        _tcscpy_s(app->szFileName, _countof(app->szFileName), pszFileName);
        app->bIsModified = FALSE;
        UpdateTitleBar(app);
        return TRUE;
    }

    WCHAR* wszText = (WCHAR*)GlobalAlloc(GPTR, (len + 1) * sizeof(WCHAR));
    if (!wszText) {
        CloseHandle(hFile);
        MessageBox(app->hwnd, _T("Not enough memory!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    GetWindowTextW(app->hEdit, wszText, len + 1);

    // Convert to UTF-8
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wszText, -1, NULL, 0, NULL, NULL);
    char* utf8Text = (char*)GlobalAlloc(GPTR, utf8Len);
    if (!utf8Text) {
        GlobalFree(wszText);
        CloseHandle(hFile);
        MessageBox(app->hwnd, _T("Conversion failed!"), _T("Error"), MB_ICONEXCLAMATION | MB_OK);
        return FALSE;
    }

    WideCharToMultiByte(CP_UTF8, 0, wszText, -1, utf8Text, utf8Len, NULL, NULL);

    // Write UTF-8 BOM
    BYTE bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written;
    WriteFile(hFile, bom, 3, &written, NULL);
    WriteFile(hFile, utf8Text, utf8Len - 1, &written, NULL); // -1 to skip null terminator

    CloseHandle(hFile);
    GlobalFree(wszText);
    GlobalFree(utf8Text);

    _tcscpy_s(app->szFileName, _countof(app->szFileName), pszFileName);
    app->bIsModified = FALSE;
    UpdateTitleBar(app);

    return TRUE;
}

/**
 * Shows the About dialog
 */
void ShowAboutDialog(HWND hwndParent) {
    MessageBox(
        hwndParent,
        _T("Slate\n\nA simple text editor written in C using Win32 API"),
        _T("About Slate"),
        MB_ICONINFORMATION | MB_OK
    );
}

/**
 * Shows the Help dialog
 */
void ShowHelpDialog(HWND hwndParent) {
    MessageBox(
        hwndParent,
        _T("Slate Help\n\nThis is a simple text editor that supports:\n")
        _T("- Creating, opening, and saving text files\n")
        _T("- Basic editing operations (cut, copy, paste, etc.)\n")
        _T("- Line and column position display\n")
        _T("- Insert/overwrite mode toggle (Insert key)\n")
        _T("- Caps lock indicator"),
        _T("Slate Help"),
        MB_ICONINFORMATION | MB_OK
    );
}

/**
 * Window procedure for the main window
 */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            // Initialize common controls
            INITCOMMONCONTROLSEX icex;
            icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
            icex.dwICC = ICC_BAR_CLASSES;
            InitCommonControlsEx(&icex);
            return 0;
        }

        case WM_NOTIFY: {
            LPNMHDR pnmh = (LPNMHDR)lParam;

            // Check if the notification is from the status bar
            if (pnmh->hwndFrom == g_app.hStatus) {
                if (pnmh->code == NM_DBLCLK) {
                    // Cast to NMMOUSE to see which part was clicked
                    LPNMMOUSE lpnmm = (LPNMMOUSE)lParam;
                    
                    // STATUS_PART_INSERT is part index 1
                    if (lpnmm->dwItemSpec == STATUS_PART_INSERT) {
                        // Toggle the mode
                        g_app.bIsInsertMode = !g_app.bIsInsertMode;
                        
                        // Refresh the status bar text to show the change
                        UpdateStatusBar(&g_app);
                    }
                }
            }
            return 0;
        }
        
        case WM_SIZE: {
            // Resize the edit control and status bar
            if (g_app.hEdit == NULL || g_app.hStatus == NULL) break;

            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            // Get the current height of the status bar by asking for its client area
            RECT rc;
            GetClientRect(g_app.hStatus, &rc);
            int statusHeight = (rc.bottom > 0) ? (rc.bottom - rc.top) : 22;

            // Move status bar to bottom
            MoveWindow(g_app.hStatus, 0, height - statusHeight, width, statusHeight, TRUE);

            // Edit control fills the rest (below menu, above status)
            // Menu height is already accounted for in client area
            MoveWindow(g_app.hEdit, 0, 0, width, height - statusHeight, TRUE);

            // Resize status bar parts
            int parts[] = {200, 300, -1};
            SendMessage(g_app.hStatus, SB_SETPARTS, _countof(parts), (LPARAM)parts);

            break;
        }
        
        case WM_COMMAND: {
            // Check for Edit control notifications
            if (lParam == (LPARAM)g_app.hEdit && HIWORD(wParam) == EN_CHANGE) {
                if (!g_app.bIsModified) {
                    g_app.bIsModified = TRUE;
                    UpdateTitleBar(&g_app);
                }
            }

            switch (LOWORD(wParam)) {
                // File menu commands
                case ID_FILE_NEW: {
                    if (g_app.bIsModified) {
                        int result = MessageBox(
                            g_app.hwnd,
                            _T("Do you want to save changes to the current document?"),
                            _T("Slate"),
                            MB_ICONQUESTION | MB_YESNOCANCEL
                        );
                        
                        if (result == IDYES) {
                            if (g_app.szFileName[0] == _T('\0')) {
                                // Save As dialog
                                OPENFILENAME ofn = {0};
                                TCHAR szFile[MAX_FILE_PATH] = {0};
                                
                                ofn.lStructSize = sizeof(OPENFILENAME);
                                ofn.hwndOwner = g_app.hwnd;
                                ofn.lpstrFile = szFile;
                                ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
                                ofn.lpstrFilter = _T("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
                                ofn.nFilterIndex = 1;
                                ofn.lpstrFileTitle = NULL;
                                ofn.nMaxFileTitle = 0;
                                ofn.lpstrInitialDir = NULL;
                                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                                
                                if (GetSaveFileName(&ofn)) {
                                    SaveFile(&g_app, ofn.lpstrFile);
                                } else {
                                    return 0; // User canceled save
                                }
                            } else {
                                SaveFile(&g_app, g_app.szFileName);
                            }
                        } else if (result == IDCANCEL) {
                            return 0; // User canceled new operation
                        }
                    }
                    
                    // Clear the edit control
                    SetWindowText(g_app.hEdit, _T(""));
                    g_app.szFileName[0] = _T('\0');
                    g_app.bIsModified = FALSE;
                    UpdateTitleBar(&g_app);
                    UpdateStatusBar(&g_app);
                    return 0;
                }
                
                case ID_FILE_OPEN: {
                    if (g_app.bIsModified) {
                        int result = MessageBox(
                            g_app.hwnd,
                            _T("Do you want to save changes to the current document?"),
                            _T("Slate"),
                            MB_ICONQUESTION | MB_YESNOCANCEL
                        );
                        
                        if (result == IDYES) {
                            if (g_app.szFileName[0] == _T('\0')) {
                                // Save As dialog
                                OPENFILENAME ofn = {0};
                                TCHAR szFile[MAX_FILE_PATH] = {0};
                                
                                ofn.lStructSize = sizeof(OPENFILENAME);
                                ofn.hwndOwner = g_app.hwnd;
                                ofn.lpstrFile = szFile;
                                ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
                                ofn.lpstrFilter = _T("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
                                ofn.nFilterIndex = 1;
                                ofn.lpstrFileTitle = NULL;
                                ofn.nMaxFileTitle = 0;
                                ofn.lpstrInitialDir = NULL;
                                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                                
                                if (GetSaveFileName(&ofn)) {
                                    SaveFile(&g_app, ofn.lpstrFile);
                                } else {
                                    return 0; // User canceled save
                                }
                            } else {
                                SaveFile(&g_app, g_app.szFileName);
                            }
                        } else if (result == IDCANCEL) {
                            return 0; // User canceled open operation
                        }
                    }
                    
                    // Open file dialog
                    OPENFILENAME ofn = {0};
                    TCHAR szFile[MAX_FILE_PATH] = {0};
                    
                    ofn.lStructSize = sizeof(OPENFILENAME);
                    ofn.hwndOwner = g_app.hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
                    ofn.lpstrFilter = _T("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
                    ofn.nFilterIndex = 1;
                    ofn.lpstrFileTitle = NULL;
                    ofn.nMaxFileTitle = 0;
                    ofn.lpstrInitialDir = NULL;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
                    
                    if (GetOpenFileName(&ofn)) {
                        LoadFile(&g_app, ofn.lpstrFile);
                    }
                    return 0;
                }
                
                case ID_FILE_SAVE: {
                    if (g_app.szFileName[0] == _T('\0')) {
                        // Save As dialog
                        OPENFILENAME ofn = {0};
                        TCHAR szFile[MAX_FILE_PATH] = {0};
                        
                        ofn.lStructSize = sizeof(OPENFILENAME);
                        ofn.hwndOwner = g_app.hwnd;
                        ofn.lpstrFile = szFile;
                        ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
                        ofn.lpstrFilter = _T("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
                        ofn.nFilterIndex = 1;
                        ofn.lpstrFileTitle = NULL;
                        ofn.nMaxFileTitle = 0;
                        ofn.lpstrInitialDir = NULL;
                        ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                        
                        if (GetSaveFileName(&ofn)) {
                            SaveFile(&g_app, ofn.lpstrFile);
                        }
                    } else {
                        SaveFile(&g_app, g_app.szFileName);
                    }
                    return 0;
                }
                
                case ID_FILE_SAVE_AS: {
                    OPENFILENAME ofn = {0};
                    TCHAR szFile[MAX_FILE_PATH] = {0};
                    
                    ofn.lStructSize = sizeof(OPENFILENAME);
                    ofn.hwndOwner = g_app.hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
                    ofn.lpstrFilter = _T("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
                    ofn.nFilterIndex = 1;
                    ofn.lpstrFileTitle = NULL;
                    ofn.nMaxFileTitle = 0;
                    ofn.lpstrInitialDir = NULL;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                    
                    if (GetSaveFileName(&ofn)) {
                        SaveFile(&g_app, ofn.lpstrFile);
                    }
                    return 0;
                }
                
                case ID_FILE_EXIT: {
                    PostMessage(g_app.hwnd, WM_CLOSE, 0, 0);
                    return 0;
                }
                
                // Edit menu commands
                case ID_EDIT_CUT: {
                    SendMessage(g_app.hEdit, WM_CUT, 0, 0);
                    return 0;
                }
                
                case ID_EDIT_COPY: {
                    SendMessage(g_app.hEdit, WM_COPY, 0, 0);
                    return 0;
                }
                
                case ID_EDIT_PASTE: {
                    SendMessage(g_app.hEdit, WM_PASTE, 0, 0);
                    return 0;
                }
                
                case ID_EDIT_DELETE: {
                    SendMessage(g_app.hEdit, WM_CLEAR, 0, 0);
                    return 0;
                }
                
                case ID_EDIT_SELECT_ALL: {
                    SendMessage(g_app.hEdit, EM_SETSEL, 0, -1);
                    return 0;
                }
                
                // Help menu commands
                case ID_HELP_HELP: {
                    ShowHelpDialog(g_app.hwnd);
                    return 0;
                }
                
                case ID_HELP_ABOUT: {
                    UpdateStatusBar(&g_app);
                    ShowAboutDialog(g_app.hwnd);
                    return 0;
                }
            }
            break;
        }

        // Add these new message handlers:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            if ((HWND)wParam == g_app.hEdit) {
                UpdateStatusBar(&g_app);
            }
            break;
        
        case WM_KEYDOWN: {
            switch (wParam) {           
                case VK_CAPITAL: {
                    // Update caps lock indicator
                    UpdateStatusBar(&g_app);
                    return 0;
                }
            }
            break;
        }
        
        case WM_CHAR: {
            // Update status bar on character input
            UpdateStatusBar(&g_app);
            break;
        }
        
        case WM_LBUTTONUP:
        case WM_KEYUP:
        case WM_LBUTTONDOWN: {
            // Update status bar on mouse click
            UpdateStatusBar(&g_app);
            break;
        }
        
        case WM_CONTEXTMENU: {
            // Handle right-click to show context menu
            if ((HWND)wParam == g_app.hEdit) {
                HMENU hMenu = CreatePopupMenu();
                AppendMenu(hMenu, MF_STRING, ID_EDIT_CUT, _T("Cut"));
                AppendMenu(hMenu, MF_STRING, ID_EDIT_COPY, _T("Copy"));
                AppendMenu(hMenu, MF_STRING, ID_EDIT_PASTE, _T("Paste"));
                AppendMenu(hMenu, MF_STRING, ID_EDIT_DELETE, _T("Delete"));
                AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenu(hMenu, MF_STRING, ID_EDIT_SELECT_ALL, _T("Select All"));
                
                // Get cursor position
                POINT pt;
                GetCursorPos(&pt);
                
                // Track the popup menu
                TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hwnd, NULL);
                
                DestroyMenu(hMenu);
                return 0;
            }
            break;
        }

        case WM_ACTIVATE: {
            // When app becomes active, refresh status bar (including Caps Lock)
            if (LOWORD(wParam) != WA_INACTIVE) {
                UpdateStatusBar(&g_app);
            }
            break;
        }
        
        case WM_CLOSE: {
            if (g_app.bIsModified) {
                int result = MessageBox(
                    g_app.hwnd,
                    _T("Do you want to save changes to the current document?"),
                    _T("Slate"),
                    MB_ICONQUESTION | MB_YESNOCANCEL
                );
                
                if (result == IDYES) {
                    if (g_app.szFileName[0] == _T('\0')) {
                        // Save As dialog
                        OPENFILENAME ofn = {0};
                        TCHAR szFile[MAX_FILE_PATH] = {0};
                        
                        ofn.lStructSize = sizeof(OPENFILENAME);
                        ofn.hwndOwner = g_app.hwnd;
                        ofn.lpstrFile = szFile;
                        ofn.nMaxFile = sizeof(szFile) / sizeof(TCHAR);
                        ofn.lpstrFilter = _T("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
                        ofn.nFilterIndex = 1;
                        ofn.lpstrFileTitle = NULL;
                        ofn.nMaxFileTitle = 0;
                        ofn.lpstrInitialDir = NULL;
                        ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
                        
                        if (GetSaveFileName(&ofn)) {
                            SaveFile(&g_app, ofn.lpstrFile);
                        } else {
                            return 0; // User canceled save
                        }
                    } else {
                        SaveFile(&g_app, g_app.szFileName);
                    }
                } else if (result == IDCANCEL) {
                    return 0; // User canceled close operation
                }
            }
            
            DestroyWindow(g_app.hwnd);
            return 0;
        }
        
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

/**
 * Initializes the application by creating menus and other resources
 */
BOOL InitializeApplication(HINSTANCE hInstance) {
    // Create the main window
    CreateMainWindow(hInstance);
    return TRUE;
}