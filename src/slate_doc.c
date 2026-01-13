#include "slate_doc.h"
#include <stdlib.h>
#include <string.h>

static Piece* CreatePiece(BufferType buffer, size_t start, size_t length, BOOL isUtf8) {
    Piece* p = (Piece*)malloc(sizeof(Piece));
    if (p) {
        p->buffer = buffer;
        p->start = start;
        p->length = length;
        p->isUtf8 = isUtf8;
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
        if (offset == cumulative) return curr;

        if (offset > cumulative && offset < cumulative + curr->length) {
            size_t splitPoint = offset - cumulative;
            
            // Preserve encoding flag when splitting the piece
            Piece* secondHalf = CreatePiece(curr->buffer, curr->start + splitPoint, 
                                            curr->length - splitPoint, curr->isUtf8);
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
    pDoc->total_length = 0;
    pDoc->line_count = 0;
    
    if (pDoc->line_offsets == NULL) {
        pDoc->line_capacity = 1024;
        pDoc->line_offsets = malloc(pDoc->line_capacity * sizeof(size_t));
    }
    pDoc->line_offsets[pDoc->line_count++] = 0;

    Piece* curr = pDoc->head;
    size_t logicalOffset = 0;

    while (curr) {
        if (curr->buffer == BUFFER_ORIGINAL && curr->isUtf8) {
            // UTF-8 Scan
            const char* buf = (const char*)pDoc->original_buffer;
            for (size_t i = 0; i < curr->length; i++) {
                if (buf[curr->start + i] == '\n') {
                    if (pDoc->line_count >= pDoc->line_capacity) {
                        pDoc->line_capacity *= 2;
                        pDoc->line_offsets = realloc(pDoc->line_offsets, pDoc->line_capacity * sizeof(size_t));
                    }
                    pDoc->line_offsets[pDoc->line_count++] = logicalOffset + i + 1;
                }
            }
        } else {
            // UTF-16 Scan
            const WCHAR* buf = (curr->buffer == BUFFER_ORIGINAL) ? 
                               (WCHAR*)pDoc->original_buffer : pDoc->add_buffer;
            for (size_t i = 0; i < curr->length; i++) {
                if (buf[curr->start + i] == L'\n') {
                    if (pDoc->line_count >= pDoc->line_capacity) {
                        pDoc->line_capacity *= 2;
                        pDoc->line_offsets = realloc(pDoc->line_offsets, pDoc->line_capacity * sizeof(size_t));
                    }
                    pDoc->line_offsets[pDoc->line_count++] = logicalOffset + i + 1;
                }
            }
        }
        logicalOffset += curr->length;
        curr = curr->next;
    }
    pDoc->total_length = logicalOffset;
}

Piece* ClonePieceList(Piece* head) {
    if (!head) return NULL;

    Piece* newHead = malloc(sizeof(Piece));
    // memcpy is perfect here because it clones buffer type, start, length, AND isUtf8
    memcpy(newHead, head, sizeof(Piece));
    
    Piece* currentOld = head->next;
    Piece* currentNew = newHead;

    while (currentOld) {
        currentNew->next = malloc(sizeof(Piece));
        memcpy(currentNew->next, currentOld, sizeof(Piece));
        
        currentNew = currentNew->next;
        currentOld = currentOld->next;
    }
    currentNew->next = NULL;
    return newHead;
}

void FreePieceList(Piece* head) {
    while (head) {
        Piece* next = head->next;
        free(head);
        head = next;
    }
}

void Doc_UpdateLineMap(SlateDoc* doc) {
    if (!doc) return;

    // Reset line map storage
    if (doc->line_offsets) {
        free(doc->line_offsets);
    }
    doc->line_count = 0;
    doc->line_capacity = 1024;
    doc->line_offsets = (size_t*)malloc(doc->line_capacity * sizeof(size_t));
    if (!doc->line_offsets) return;

    doc->line_offsets[doc->line_count++] = 0; 

    Piece* curr = doc->head;
    size_t logical_base = 0;

    while (curr) {
        if (curr->buffer == BUFFER_ORIGINAL && curr->isUtf8) {
            // Scan UTF-8 data by treating original_buffer as char*
            const char* buf = (const char*)doc->original_buffer;
            for (size_t i = 0; i < curr->length; i++) {
                if (buf[curr->start + i] == '\n') {
                    if (doc->line_count >= doc->line_capacity) {
                        doc->line_capacity *= 2;
                        doc->line_offsets = realloc(doc->line_offsets, doc->line_capacity * sizeof(size_t));
                    }
                    doc->line_offsets[doc->line_count++] = logical_base + i + 1;
                }
            }
        } else {
            // Scan UTF-16 data by treating the buffer as WCHAR*
            const WCHAR* buf = (curr->buffer == BUFFER_ORIGINAL) ? 
                               (WCHAR*)doc->original_buffer : doc->add_buffer;
            if (buf) {
                for (size_t i = 0; i < curr->length; i++) {
                    if (buf[curr->start + i] == L'\n') {
                        if (doc->line_count >= doc->line_capacity) {
                            doc->line_capacity *= 2;
                            doc->line_offsets = realloc(doc->line_offsets, doc->line_capacity * sizeof(size_t));
                        }
                        doc->line_offsets[doc->line_count++] = logical_base + i + 1;
                    }
                }
            }
        }
        logical_base += curr->length;
        curr = curr->next;
    }

    // Update total length and cap the line map
    doc->total_length = logical_base;
    if (doc->line_count >= doc->line_capacity) {
        doc->line_offsets = realloc(doc->line_offsets, (doc->line_count + 1) * sizeof(size_t));
    }
    doc->line_offsets[doc->line_count] = doc->total_length;
}

void Doc_ClearUndoStack(SlateDoc* pDoc) {
    if (!pDoc || !pDoc->undo_stack) return;

    UndoStep* current = pDoc->undo_stack;
    while (current) {
        UndoStep* nextStep = current->next;

        // Free the snapshot of the pieces
        FreePieceList(current->pieces);

        // Free the step container
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

        // Free the deep-copied piece list for this step
        FreePieceList(current->pieces);

        // Free the step container itself
        free(current);

        current = nextStep;
    }

    // Mark the stack as empty
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

    // Before restoring the old state, save the current state to Redo so we can undo the undo
    UndoStep* redoStep = malloc(sizeof(UndoStep));
    redoStep->cursor_hint = *outCursor; // Save current cursor position
    redoStep->pieces = ClonePieceList(pDoc->head);
    redoStep->next = pDoc->redo_stack;
    pDoc->redo_stack = redoStep;

    // Restore the previous state
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

    // Pop from the redo stack
    UndoStep* step = pDoc->redo_stack;
    pDoc->redo_stack = step->next;

    // Push current state to the undo stack before overwriting it
    Doc_PushUndo(pDoc, *outCursor, FALSE);

    // Clear the current active piece list
    FreePieceList(pDoc->head);

    // Restore the pieces from the redo step and take ownership of them
    pDoc->head = step->pieces; 
    
    // Update the cursor hint
    if (outCursor) {
        *outCursor = step->cursor_hint;
    }

    // Rebuild the line map for the restored state
    Doc_RefreshMetadata(pDoc);

    // Free only the container; the document now owns the pieces
    free(step);
    
    return TRUE;
}

size_t Doc_GetLineOffset(SlateDoc* doc, size_t lineIndex) {
    if (!doc || lineIndex >= doc->line_count) return doc->total_length;
    return doc->line_offsets[lineIndex];
}

SlateDoc* Doc_CreateFromMap(void* pMappedText, size_t len, HANDLE hMap, void* pBase, BOOL isUtf8) {
    SlateDoc* doc = (SlateDoc*)calloc(1, sizeof(SlateDoc));
    if (!doc) return NULL;

    doc->original_buffer = pMappedText;
    doc->original_buffer_base = pBase;
    doc->hMapFile = hMap;
    doc->original_len = len;
    doc->original_is_utf8 = isUtf8;

    doc->add_capacity = 8192;
    doc->add_buffer = (WCHAR*)malloc(doc->add_capacity * sizeof(WCHAR));

    doc->head = CreatePiece(BUFFER_ORIGINAL, 0, len, isUtf8);

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

    // Maintain undo history
    Doc_PushUndo(doc, offset, TRUE);

    // Ensure space in the ADD buffer (the buffer for new typing)
    if (doc->add_len + len > doc->add_capacity) {
        size_t new_cap = (doc->add_len + len) * 2;
        WCHAR* new_buf = (WCHAR*)realloc(doc->add_buffer, new_cap * sizeof(WCHAR));
        if (!new_buf) return FALSE;
        doc->add_buffer = new_buf;
        doc->add_capacity = new_cap;
    }

    // Copy new text to the end of the ADD buffer
    size_t add_start_index = doc->add_len;
    memcpy(doc->add_buffer + add_start_index, text, len * sizeof(WCHAR));
    doc->add_len += len;

    // Insert a new piece into the table
    if (offset == 0) {
        // Insert at very beginning
        // All new additions are UTF-16, so isUtf8 is FALSE
        Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len, FALSE);
        newP->next = doc->head;
        doc->head = newP;
    } else if (offset == doc->total_length) {
        // Append to very end
        Piece* curr = doc->head;
        while (curr && curr->next) curr = curr->next;
        Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len, FALSE);
        if (curr) curr->next = newP;
        else doc->head = newP;
    } else {
        Piece* curr = doc->head;
        size_t cumulative = 0;

        while (curr) {
            if (offset > cumulative && offset < cumulative + curr->length) {
                // Split this piece in two
                size_t splitPoint = offset - cumulative;
                
                // The split pieces maintain the encoding of the original piece (curr->isUtf8)
                Piece* secondHalf = CreatePiece(curr->buffer, curr->start + splitPoint, curr->length - splitPoint, curr->isUtf8);
                secondHalf->next = curr->next;

                // Create the new text piece (always UTF-16)
                Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len, FALSE);
                newP->next = secondHalf;

                // Link the first half to the new piece
                curr->length = splitPoint;
                curr->next = newP;
                break;
            } else if (offset == cumulative + curr->length) {
                // Lucky break: Insert exactly between two existing pieces
                Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len, FALSE);
                newP->next = curr->next;
                curr->next = newP;
                break;
            }
            cumulative += curr->length;
            curr = curr->next;
        }
    }

    // Update metadata and line map
    Doc_RefreshMetadata(doc);
    
    // Explicitly update line map if it's a separate UI requirement
    Doc_UpdateLineMap(doc); 

    return TRUE;
}

BOOL Doc_Delete(SlateDoc* doc, size_t offset, size_t len) {
    if (!doc || len == 0 || offset + len > doc->total_length) return FALSE;
    
    // Snapshot state before modification
    Doc_PushUndo(doc, offset, TRUE);

    // Split at the start and end of the range to delete
    SplitPiece(doc, offset);
    Piece* after = SplitPiece(doc, offset + len);

    // Unlink and free the pieces within the range
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

    // Refresh metadata and rebuild the line map
    Doc_RefreshMetadata(doc);
    Doc_UpdateLineMap(doc);
    
    return TRUE;
}

size_t Doc_GetText(SlateDoc* doc, size_t offset, size_t len, WCHAR* dest) {
    if (offset >= doc->total_length) return 0;
    if (offset + len > doc->total_length) len = doc->total_length - offset;

    Piece* curr = doc->head;
    size_t cumulative = 0;
    size_t destPos = 0;

    while (curr && destPos < len) {
        if (offset < cumulative + curr->length) {
            size_t startInPiece = (offset > cumulative) ? (offset - cumulative) : 0;
            size_t takeFromPiece = curr->length - startInPiece;
            if (takeFromPiece > (len - destPos)) takeFromPiece = (len - destPos);

            if (curr->buffer == BUFFER_ORIGINAL && curr->isUtf8) {
                // Convert UTF-8 on-the-fly for the view
                MultiByteToWideChar(CP_UTF8, 0, ((char*)doc->original_buffer) + curr->start + startInPiece, 
                                   (int)takeFromPiece, dest + destPos, (int)takeFromPiece);
            } else {
                const WCHAR* src = (curr->buffer == BUFFER_ORIGINAL) ? (WCHAR*)doc->original_buffer : doc->add_buffer;
                memcpy(dest + destPos, src + curr->start + startInPiece, takeFromPiece * sizeof(WCHAR));
            }
            destPos += takeFromPiece;
        }
        cumulative += curr->length;
        curr = curr->next;
    }
    return destPos;
}

void Doc_StreamToBuffer(SlateDoc* doc, void (*callback)(const WCHAR*, size_t, void*), void* ctx) {
    WCHAR temp[4096];
    size_t offset = 0;
    while (offset < doc->total_length) {
        size_t chunk = (doc->total_length - offset > 4096) ? 4096 : doc->total_length - offset;
        Doc_GetText(doc, offset, chunk, temp);
        callback(temp, chunk, ctx);
        offset += chunk;
    }
}

/**
 * Creates a blank document with no file backing.
 */
SlateDoc* Doc_CreateEmpty() {
    SlateDoc* doc = (SlateDoc*)calloc(1, sizeof(SlateDoc));
    if (!doc) return NULL;
    doc->original_is_utf8 = TRUE;
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
