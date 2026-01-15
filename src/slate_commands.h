#ifndef SLATE_COMMANDS_H
#define SLATE_COMMANDS_H

#define MAX_COMMANDS 16
#define MAX_ARGS     8
#define MAX_ARG_LEN  260

// Menu command IDs
#define ID_FILE_NEW          1001
#define ID_FILE_OPEN         1002
#define ID_FILE_SAVE         1003
#define ID_FILE_SAVE_AS      1004
#define ID_FILE_EXIT         1005

#define ID_EDIT_UNDO         2001
#define ID_EDIT_REDO         2002
#define ID_EDIT_CUT          2003
#define ID_EDIT_COPY         2004
#define ID_EDIT_PASTE        2005
#define ID_EDIT_DELETE       2006
#define ID_EDIT_SELECT_ALL   2007

#define ID_VIEW_WORDWRAP     3001
#define ID_VIEW_NONPRINTABLE 3002
#define ID_VIEW_SYSTEMCOLORS 3003   

#define ID_HELP_HELP         4001
#define ID_HELP_ABOUT        4002

// Application command IDs

#define WM_APP_SAVE_FILE     8001
#define WM_APP_OPEN_FILE     8002
#define WM_APP_QUIT          8003

typedef struct
{
    WCHAR commands[MAX_COMMANDS];
    int   commandCount;

    WCHAR args[MAX_ARGS][MAX_ARG_LEN];
    int   argCount;
} ParsedCommand;

typedef enum
{
    EXCMD_NONE,
    EXCMD_WRITE,
    EXCMD_WRITE_QUIT,
    EXCMD_QUIT,
    EXCMD_EDIT
} ExCommandType;

typedef struct
{
    ExCommandType type;
    BOOL          force;
    const WCHAR*  arg;   // filename or NULL
} ExCommand;

#endif