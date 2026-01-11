#ifndef SLATE_DOC_H
#define SLATE_DOC_H

#include <windows.h>

typedef enum { BUFFER_ORIGINAL, BUFFER_ADD } BufferType;

typedef struct Piece {
    BufferType buffer;
    size_t start;
    size_t length;
    BOOL isUtf8;       // NEW: TRUE for original UTF-8 pieces, FALSE for ADD (always UTF-16)
    struct Piece* next;
} Piece;

typedef struct UndoStep {
    Piece* pieces;
    size_t piece_count;
    size_t cursor_hint;
    struct UndoStep* next;
} UndoStep;

typedef struct {
    void* original_buffer;      // void* handles char* or WCHAR*
    void* original_buffer_base;
    HANDLE hMapFile;
    size_t original_len;
    BOOL   original_is_utf8;     // Flag for the mapped file encoding
    
    WCHAR* add_buffer;
    size_t add_len;
    size_t add_capacity;

    Piece* head;
    size_t total_length;

    UndoStep* undo_stack;
    UndoStep* redo_stack;

    size_t* line_offsets;
    size_t  line_count;
    size_t  line_capacity;
} SlateDoc;

// Function declarations
SlateDoc* Doc_CreateEmpty();
SlateDoc* Doc_CreateFromMap(void* pMappedText, size_t len, HANDLE hMap, void* pBase, BOOL isUtf8);
void      Doc_Destroy(SlateDoc* doc);
void      Doc_RefreshMetadata(SlateDoc* pDoc);
void      Doc_StreamToBuffer(SlateDoc* doc, void (*callback)(const WCHAR*, size_t, void*), void* ctx);
size_t    Doc_GetText(SlateDoc* doc, size_t offset, size_t len, WCHAR* dest);
void      Doc_GetOffsetInfo(SlateDoc* doc, size_t offset, int* out_line, int* out_col);
size_t    Doc_GetLineOffset(SlateDoc* doc, size_t lineIndex);
void      Doc_UpdateLineMap(SlateDoc* doc);
BOOL      Doc_Insert(SlateDoc* doc, size_t offset, const WCHAR* text, size_t len);
BOOL      Doc_Delete(SlateDoc* doc, size_t offset, size_t len);
BOOL      Doc_Undo(SlateDoc* pDoc, size_t* outCursor);
BOOL      Doc_Redo(SlateDoc* pDoc, size_t* outCursor);

#endif