#ifndef SLATE_DOC_H
#define SLATE_DOC_H

#include <windows.h>

typedef enum { BUFFER_ORIGINAL, BUFFER_ADD } BufferType;

typedef struct Piece {
    BufferType buffer;
    size_t start;      // Index into the piece's storage buffer (bytes for UTF-8 original pieces, WCHAR units otherwise)
    size_t length;      // LOGICAL length in UTF-16 code units - the unit used for all document/cursor offset math
    size_t rawLength;   // Physical length in the storage buffer (bytes for UTF-8 original pieces, equal to length otherwise)
    BOOL isUtf8;       // TRUE for original UTF-8 pieces, FALSE for ADD buffer text (always UTF-16)
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

    // Sparse UTF-8 -> UTF-16 position index over original_buffer (UTF-8 files only).
    // Entry k is a character boundary at or just after k * UTF8_CKPT_STRIDE bytes, with
    // the number of UTF-16 units that precede it; the final entry is a sentinel at
    // original_len. Lets byte<->unit conversions scan at most one stride instead of
    // walking from the start of the file.
    size_t* utf8_ckpt_byte;
    size_t* utf8_ckpt_units;
    size_t  utf8_ckpt_count;

    WCHAR* add_buffer;
    size_t add_len;
    size_t add_capacity;

    Piece* head;
    size_t total_length;

    // Lazy line-map state
    BOOL    line_map_complete;      // TRUE once we've scanned to EOF
    size_t  line_scan_offset;       // Logical offset already scanned for newlines
    Piece*  line_scan_piece;        // Piece where scanning will resume
    size_t  line_scan_piece_offset; // Offset within that piece

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
BOOL      Doc_Insert(SlateDoc* doc, size_t offset, const WCHAR* text, size_t len);
BOOL      Doc_Delete(SlateDoc* doc, size_t offset, size_t len);
void      Doc_EnsureLineForIndex(SlateDoc* doc, size_t lineIndex);
BOOL      Doc_Undo(SlateDoc* pDoc, size_t currentCursor, size_t* outCursor);
BOOL      Doc_Redo(SlateDoc* pDoc, size_t currentCursor, size_t* outCursor);

typedef enum {
    DOC_SEARCH_NO_PATTERN,
    DOC_SEARCH_MATCH,
    DOC_SEARCH_REACHED_EOF,
    DOC_SEARCH_REACHED_BOF
} DocSearchStatus;

typedef struct {
    DocSearchStatus status;
    size_t match_offset;
    size_t match_length;
    int line;
    int column;
} DocSearchResult;

DocSearchResult Doc_Search(SlateDoc* doc, const WCHAR* pattern, size_t patternLen, size_t cursorOffset, BOOL searchBackwards, BOOL caseSensitive);

#endif
