#ifndef SLATE_DOC_H
#define SLATE_DOC_H

#include <windows.h>

typedef enum { BUFFER_ORIGINAL, BUFFER_ADD } BufferType;

typedef struct Piece {
    BufferType buffer;
    size_t start;
    size_t length;
    struct Piece* next;
} Piece;

typedef struct UndoStep {
    Piece* pieces;        // Snapshot of the piece array
    size_t piece_count;   // How many pieces were in this version
    size_t cursor_hint;   // Where the cursor was
    struct UndoStep* next;
} UndoStep;

typedef struct {
    WCHAR* original_buffer;
    void* original_buffer_base;
    HANDLE  hMapFile;       // NEW: For memory mapping
    size_t  original_len;
    
    WCHAR* add_buffer;
    size_t  add_len;
    size_t  add_capacity;

    Piece* head;
    size_t  total_length;

    UndoStep* undo_stack;
    UndoStep* redo_stack;

    // NEW: Line map for O(1) vertical scrolling
    size_t* line_offsets;
    size_t  line_count;
    size_t  line_capacity;
} SlateDoc;

SlateDoc* Doc_CreateEmpty();
SlateDoc* Doc_CreateFromMap(const WCHAR* pMappedText, size_t len, HANDLE hMap, void* pBase);
void      Doc_GetOffsetInfo(SlateDoc* doc, size_t offset, int* out_line, int* out_col);
void      Doc_Destroy(SlateDoc* doc);
BOOL      Doc_Insert(SlateDoc* doc, size_t offset, const WCHAR* text, size_t len);
BOOL      Doc_Delete(SlateDoc* doc, size_t offset, size_t len);
size_t    Doc_GetText(SlateDoc* doc, size_t offset, size_t len, WCHAR* out_buffer);
void      Doc_StreamToBuffer(SlateDoc* doc, void (*callback)(const WCHAR*, size_t, void*), void* ctx);
void      Doc_UpdateLineMap(SlateDoc* doc); // Internal rebuild of line map
size_t    Doc_GetLineOffset(SlateDoc* doc, size_t lineIndex);
void      Doc_RefreshMetadata(SlateDoc* pDoc);
BOOL      Doc_Undo(SlateDoc* doc, size_t* outCursor);
BOOL      Doc_Redo(SlateDoc* doc, size_t* outCursor);

#endif