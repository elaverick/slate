#include "slate_doc.h"
#include <stdlib.h>
#include <string.h>

static Piece* CreatePiece(BufferType buffer, size_t start, size_t length) {
    Piece* p = (Piece*)malloc(sizeof(Piece));
    if (p) {
        p->buffer = buffer;
        p->start = start;
        p->length = length;
        p->next = NULL;
    }
    return p;
}

static Piece* SplitPiece(SlateDoc* doc, size_t offset) {
    if (offset == 0) return doc->head;
    if (offset >= doc->total_length) return NULL;

    Piece* curr = doc->head;
    size_t cumulative = 0;

    while (curr) {
        // TRAP 1 FIX: If the offset is exactly at the start of this piece, 
        // return this piece! No split needed.
        if (offset == cumulative) {
            return curr;
        }

        if (offset > cumulative && offset < cumulative + curr->length) {
            size_t splitPoint = offset - cumulative;
            Piece* secondHalf = CreatePiece(curr->buffer, curr->start + splitPoint, curr->length - splitPoint);
            if (secondHalf) {
                secondHalf->next = curr->next;
                curr->length = splitPoint;
                curr->next = secondHalf;
            }
            return secondHalf;
        }
        cumulative += curr->length;
        curr = curr->next;
    }
    return NULL;
}

void Doc_RefreshMetadata(SlateDoc* pDoc) {
    // 1. Reset basic stats
    pDoc->total_length = 0;
    pDoc->line_count = 0;
    
    // Ensure we have a fresh line_offsets array
    // Start with a reasonable capacity if it's currently NULL
    if (pDoc->line_offsets == NULL) {
        pDoc->line_capacity = 1024;
        pDoc->line_offsets = malloc(pDoc->line_capacity * sizeof(size_t));
    }
    
    // The first line always starts at offset 0
    pDoc->line_offsets[pDoc->line_count++] = 0;

    Piece* curr = pDoc->head;
    size_t runningOffset = 0;

    while (curr) {
        // We need to scan the actual buffer text within this piece for newlines
        WCHAR* buffer = (curr->buffer == BUFFER_ORIGINAL) ? pDoc->original_buffer : pDoc->add_buffer;
        
        for (size_t i = 0; i < curr->length; i++) {
            WCHAR ch = buffer[curr->start + i];
            
            if (ch == L'\n') {
                // Grow line map if needed
                if (pDoc->line_count >= pDoc->line_capacity) {
                    pDoc->line_capacity *= 2;
                    pDoc->line_offsets = realloc(pDoc->line_offsets, pDoc->line_capacity * sizeof(size_t));
                }
                // The NEXT line starts after this newline
                pDoc->line_offsets[pDoc->line_count++] = runningOffset + i + 1;
            }
        }

        runningOffset += curr->length;
        curr = curr->next;
    }

    pDoc->total_length = runningOffset;
}

Piece* ClonePieceList(Piece* head) {
    if (!head) return NULL;

    Piece* newHead = malloc(sizeof(Piece));
    memcpy(newHead, head, sizeof(Piece));
    
    Piece* currentOld = head->next;
    Piece* currentNew = newHead;

    while (currentOld) {
        currentNew->next = malloc(sizeof(Piece));
        memcpy(currentNew->next, currentOld, sizeof(Piece));
        
        currentNew = currentNew->next;
        currentOld = currentOld->next;
    }
    currentNew->next = NULL; // Ensure the tail is terminated
    return newHead;
}

void FreePieceList(Piece* head) {
    while (head) {
        Piece* next = head->next;
        free(head);
        head = next;
    }
}

void Doc_ClearUndoStack(SlateDoc* pDoc) {
    if (!pDoc || !pDoc->undo_stack) return;

    UndoStep* current = pDoc->undo_stack;
    while (current) {
        UndoStep* nextStep = current->next;

        // 1. Free the snapshot of the pieces
        FreePieceList(current->pieces);

        // 2. Free the step container
        free(current);

        current = nextStep;
    }

    pDoc->undo_stack = NULL;
}

void Doc_ClearRedoStack(SlateDoc* pDoc) {
    if (!pDoc || !pDoc->redo_stack) return;

    UndoStep* current = pDoc->redo_stack;
    while (current) {
        UndoStep* nextStep = current->next;

        // 1. Free the deep-copied piece list for this step
        // This uses the helper we created earlier
        FreePieceList(current->pieces);

        // 2. Free the step container itself
        free(current);

        current = nextStep;
    }

    // 3. Mark the stack as empty
    pDoc->redo_stack = NULL;
}

void Doc_PushUndo(SlateDoc* pDoc, size_t currentCursor, BOOL isNewAction) {
    UndoStep* newStep = malloc(sizeof(UndoStep));
    if (!newStep) return;

    // Save the metadata and a deep copy of the linked list
    newStep->cursor_hint = currentCursor;
    newStep->pieces = ClonePieceList(pDoc->head); 
    
    // Standard stack push
    newStep->next = pDoc->undo_stack;
    pDoc->undo_stack = newStep;
    if (isNewAction &&pDoc->redo_stack) {
        Doc_ClearRedoStack(pDoc);
    }
}

BOOL Doc_Undo(SlateDoc* pDoc, size_t* outCursor) {
    if (!pDoc->undo_stack) return FALSE;

    // 1. Before we restore the old state, save the CURRENT state to Redo
    // This allows us to "undo the undo"
    UndoStep* redoStep = malloc(sizeof(UndoStep));
    redoStep->cursor_hint = *outCursor; // Save current cursor position
    redoStep->pieces = ClonePieceList(pDoc->head);
    redoStep->next = pDoc->redo_stack;
    pDoc->redo_stack = redoStep;

    // 2. Now perform the restoration as before
    UndoStep* step = pDoc->undo_stack;
    pDoc->undo_stack = step->next;

    FreePieceList(pDoc->head);
    pDoc->head = step->pieces;
    
    if (outCursor) *outCursor = step->cursor_hint;

    Doc_RefreshMetadata(pDoc);
    free(step);
    return TRUE;
}

BOOL Doc_Redo(SlateDoc* pDoc, size_t* outCursor) {
    if (!pDoc || !pDoc->redo_stack) return FALSE;

    // 1. POP from the Redo stack
    UndoStep* step = pDoc->redo_stack;
    pDoc->redo_stack = step->next;

    // 2. PUSH current state to Undo stack before we overwrite it
    // Use your existing PushUndo which clones the list
    Doc_PushUndo(pDoc, *outCursor, FALSE);

    // 3. CLEAN UP current active piece list
    FreePieceList(pDoc->head);

    // 4. RESTORE the pieces from the Redo step
    // We take ownership of the pieces stored in the 'step'
    pDoc->head = step->pieces; 
    
    // Update the cursor hint
    if (outCursor) {
        *outCursor = step->cursor_hint;
    }

    // 5. REBUILD the line map for the restored state
    Doc_RefreshMetadata(pDoc);

    // 6. Free ONLY the container, not the pieces (pDoc owns them now)
    free(step);
    
    return TRUE;
}

void Doc_UpdateLineMap(SlateDoc* doc) {
    if (!doc) return;

    // Reset line map
    if (doc->line_offsets) {
        free(doc->line_offsets);
        doc->line_offsets = NULL;
    }
    doc->line_count = 0;
    doc->line_capacity = 1024;
    doc->line_offsets = (size_t*)malloc(doc->line_capacity * sizeof(size_t));
    
    if (!doc->line_offsets) return; // OOM check

    doc->line_offsets[doc->line_count++] = 0; 

    Piece* curr = doc->head;
    size_t logical_base = 0;

    while (curr) {
        // SAFETY: Determine which buffer to look at
        const WCHAR* src = (curr->buffer == BUFFER_ORIGINAL) ? doc->original_buffer : doc->add_buffer;
        
        // CRITICAL: If original_buffer is NULL (mapping failed), skip to avoid crash
        if (src) {
            for (size_t i = 0; i < curr->length; i++) {
                if (src[curr->start + i] == L'\n') {
                    if (doc->line_count >= doc->line_capacity) {
                        size_t new_cap = doc->line_capacity * 2;
                        size_t* new_offsets = (size_t*)realloc(doc->line_offsets, new_cap * sizeof(size_t));
                        if (!new_offsets) return; // Handle realloc failure
                        doc->line_offsets = new_offsets;
                        doc->line_capacity = new_cap;
                    }
                    doc->line_offsets[doc->line_count++] = logical_base + i + 1;
                }
            }
        }
        logical_base += curr->length;
        curr = curr->next;
    }

    // If the last character wasn't a newline, 
    // we still need an "end of file" boundary for the last line calculation.
    if (doc->line_count >= doc->line_capacity) {
        doc->line_offsets = realloc(doc->line_offsets, (doc->line_count + 1) * sizeof(size_t));
    }
    doc->line_offsets[doc->line_count] = doc->total_length;
}

size_t Doc_GetLineOffset(SlateDoc* doc, size_t lineIndex) {
    if (!doc || lineIndex >= doc->line_count) return doc->total_length;
    return doc->line_offsets[lineIndex];
}

SlateDoc* Doc_CreateFromMap(const WCHAR* pMappedText, size_t len, HANDLE hMap, void* pBase) {
    // 1. Allocate and zero-initialize the document structure
    SlateDoc* doc = (SlateDoc*)calloc(1, sizeof(SlateDoc));
    if (!doc) return NULL;

    // 2. Store the mapping handles and pointers
    doc->hMapFile = hMap;
    doc->original_buffer = (WCHAR*)pMappedText;     // Pointer to actual text
    doc->original_buffer_base = pBase;             // Pointer to start of mapping (for Unmap)
    doc->original_len = len;
    doc->total_length = len;

    // 3. Create the initial piece representing the entire original file
    if (len > 0) {
        doc->head = (Piece*)malloc(sizeof(Piece));
        if (doc->head) {
            doc->head->buffer = BUFFER_ORIGINAL;
            doc->head->start = 0;
            doc->head->length = len;
            doc->head->next = NULL;
        }
    }

    // 4. Initialize the 'Add' buffer for future edits
    doc->add_capacity = 8192;
    doc->add_buffer = (WCHAR*)malloc(doc->add_capacity * sizeof(WCHAR));
    doc->add_len = 0;

    // 5. Build the line map so the View can calculate scrollbars and line positions
    // This is the part that will take a few seconds for a 2.32GB file.
    Doc_RefreshMetadata(doc); 
    
    return doc;
}

void Doc_Destroy(SlateDoc* doc) {
    if (!doc) return;

    Doc_ClearUndoStack(doc);
    Doc_ClearRedoStack(doc);

    Piece* curr = doc->head;
    while (curr) {
        Piece* next = curr->next;
        free(curr);
        curr = next;
    }
    if (doc->hMapFile) {
        UnmapViewOfFile(doc->original_buffer_base);
        CloseHandle(doc->hMapFile);
    } else {
        free(doc->original_buffer);
    }
    free(doc->add_buffer);
    free(doc->line_offsets);
    free(doc);
}

BOOL Doc_Insert(SlateDoc* doc, size_t offset, const WCHAR* text, size_t len) {
    if (!doc || offset > doc->total_length) return FALSE;

    Doc_PushUndo(doc, offset, TRUE);

    // 1. Ensure space in the ADD buffer
    if (doc->add_len + len > doc->add_capacity) {
        size_t new_cap = (doc->add_len + len) * 2;
        WCHAR* new_buf = (WCHAR*)realloc(doc->add_buffer, new_cap * sizeof(WCHAR));
        if (!new_buf) return FALSE;
        doc->add_buffer = new_buf;
        doc->add_capacity = new_cap;
    }

    // 2. Copy new text to the end of the ADD buffer
    size_t add_start_index = doc->add_len;
    memcpy(doc->add_buffer + add_start_index, text, len * sizeof(WCHAR));
    doc->add_len += len;

    // 3. Piece Table Manipulation
    if (offset == 0) {
        // Insert at very beginning
        Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len);
        newP->next = doc->head;
        doc->head = newP;
    } else if (offset == doc->total_length) {
        // Append to very end
        Piece* curr = doc->head;
        while (curr && curr->next) curr = curr->next;
        Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len);
        if (curr) curr->next = newP;
        else doc->head = newP;
    } else {
        Piece* curr = doc->head;
        size_t cumulative = 0;

        while (curr) {
            if (offset > cumulative && offset < cumulative + curr->length) {
                // Split this piece in two
                size_t splitPoint = offset - cumulative;
                Piece* secondHalf = CreatePiece(curr->buffer, curr->start + splitPoint, curr->length - splitPoint);
                secondHalf->next = curr->next;

                // Create the new text piece
                Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len);
                newP->next = secondHalf;

                // Link the first half to the new piece
                curr->length = splitPoint;
                curr->next = newP;
                break;
            } else if (offset == cumulative + curr->length) {
                // Lucky break: Insert exactly between two existing pieces
                Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len);
                newP->next = curr->next;
                curr->next = newP;
                break;
            }
            cumulative += curr->length;
            curr = curr->next;
        }
    }

    doc->total_length += len;
    Doc_UpdateLineMap(doc); // Rebuild the index so 'Enter' works immediately
    Doc_RefreshMetadata(doc);
    return TRUE;
}

BOOL Doc_Delete(SlateDoc* doc, size_t offset, size_t len) {
    if (!doc || len == 0 || offset + len > doc->total_length) return FALSE;
    
    Doc_PushUndo(doc, offset, TRUE);

    // 1. Split at the START first.
    SplitPiece(doc, offset);

    // 2. Split at the END. 
    // We must call this AFTER the first split to ensure we find 
    // the correct boundary for the text we want to keep at the end.
    Piece* after = SplitPiece(doc, offset + len);

    // 3. Perform the deletion
    if (offset == 0) {
        Piece* curr = doc->head;
        while (curr && curr != after) {
            Piece* next = curr->next;
            free(curr);
            curr = next;
        }
        doc->head = after;
    } else {
        Piece* prev = doc->head;
        size_t pos = 0;
        
        // Find the piece that ends exactly where our deletion starts
        while (prev && (pos + prev->length) < offset) {
            pos += prev->length;
            prev = prev->next;
        }

        if (prev) {
            Piece* curr = prev->next;
            while (curr && curr != after) {
                Piece* next = curr->next;
                free(curr);
                curr = next;
            }
            prev->next = after;
        }
    }

    // IMPORTANT: Refresh total_length and line_offsets immediately
    Doc_RefreshMetadata(doc);
    
    return TRUE;
}

size_t Doc_GetText(SlateDoc* doc, size_t offset, size_t len, WCHAR* out_buffer) {
    if (!doc || !out_buffer || len == 0) return 0;
    size_t cumulative = 0;
    size_t copied = 0;
    Piece* curr = doc->head;
    while (curr && copied < len) {
        if (offset < cumulative + curr->length) {
            size_t pieceOffset = (offset > cumulative) ? (offset - cumulative) : 0;
            size_t toCopy = curr->length - pieceOffset;
            if (copied + toCopy > len) toCopy = len - copied;
            const WCHAR* src = (curr->buffer == BUFFER_ORIGINAL) ? doc->original_buffer : doc->add_buffer;
            memcpy(out_buffer + copied, src + curr->start + pieceOffset, toCopy * sizeof(WCHAR));
            copied += toCopy;
            offset += toCopy;
        }
        cumulative += curr->length;
        curr = curr->next;
    }
    return copied;
}

void Doc_StreamToBuffer(SlateDoc* doc, void (*callback)(const WCHAR*, size_t, void*), void* ctx) {
    Piece* curr = doc->head;
    while (curr) {
        const WCHAR* src = (curr->buffer == BUFFER_ORIGINAL) ? doc->original_buffer : doc->add_buffer;
        callback(src + curr->start, curr->length, ctx);
        curr = curr->next;
    }
}

/**
 * Creates a blank document with no file backing.
 */
SlateDoc* Doc_CreateEmpty() {
    SlateDoc* doc = (SlateDoc*)calloc(1, sizeof(SlateDoc));
    if (!doc) return NULL;
    doc->add_capacity = 8192;
    doc->add_buffer = (WCHAR*)malloc(doc->add_capacity * sizeof(WCHAR));
    Doc_UpdateLineMap(doc); // Initializes line_offsets with 0
    Doc_RefreshMetadata(doc);
    return doc;
}

/**
 * Translates a logical offset into Line and Column numbers for the UI.
 */
void Doc_GetOffsetInfo(SlateDoc* doc, size_t offset, int* out_line, int* out_col) {
    if (!doc || !doc->line_offsets) {
        *out_line = 1; *out_col = 1;
        return;
    }

    // Binary search or linear scan through the line map
    int line = 1;
    for (size_t i = 0; i < doc->line_count; i++) {
        if (doc->line_offsets[i] <= offset) {
            line = (int)i + 1;
        } else {
            break;
        }
    }
    
    *out_line = line;
    *out_col = (int)(offset - doc->line_offsets[line - 1]) + 1;
}