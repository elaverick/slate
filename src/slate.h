/**
 * slate.h - Header file for the Slate application
 * Contains function declarations, constants, and data structures
 */

#ifndef SLATE_H
#define SLATE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <shellapi.h>

#include "slate_doc.h"
#include "slate_commands.h"

// Application constants
#define APP_NAME            _T("Slate")
#define WINDOW_CLASS_NAME   _T("SlateClass")
#define MAX_FILE_PATH       260
#define STATUS_BAR_HEIGHT   20

#define IDC_EDITOR           5001
#define IDC_STATUSBAR        5002

// Status bar parts
#define STATUS_PART_CURSOR   0
#define STATUS_PART_INSERT   1
#define STATUS_PART_CAPS     2
#define STATUS_PART_VIEWMODE 3

// Application state structure
typedef struct {
    HWND hwnd;
    HWND hEdit;
    HWND hStatus;
    SlateDoc* pDoc;      // Pointer to the piece-table document
    TCHAR szFileName[MAX_FILE_PATH];
    BOOL bIsModified;
    BOOL bIsInsertMode;
} SLATE_APP;

// Function declarations
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
BOOL InitializeApplication(HINSTANCE hInstance);
void CreateMainWindow(HINSTANCE hInstance);
HMENU CreateMenuBar(void);
void CreateControls(SLATE_APP* app);
void UpdateStatusBar(SLATE_APP* app);
void UpdateTitleBar(SLATE_APP* app);
BOOL LoadFile(SLATE_APP* app, const TCHAR* pszFileName);
BOOL SaveFile(SLATE_APP* app, const TCHAR* pszFileName);
void ShowAboutDialog(HWND hwndParent);
void ShowHelpDialog(HWND hwndParent);

extern SLATE_APP g_app;

#endif // SLATE_H
