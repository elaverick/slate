#include "slate_doc.h"
#include <stdlib.h>
#include <string.h>

#define LINE_MAP_GROW_STEP 1024
#define LINE_SCAN_STEP_BYTES (64 * 1024)

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

static BOOL Doc_GrowLineOffsets(SlateDoc* doc, size_t minExtra) {
    size_t needed = doc->line_count + minExtra;
    if (needed < doc->line_capacity) return TRUE;

    size_t newCap = doc->line_capacity;
    while (newCap <= needed) newCap += LINE_MAP_GROW_STEP;

    size_t* newOffsets = realloc(doc->line_offsets, newCap * sizeof(size_t));
    if (!newOffsets) return FALSE;
    doc->line_offsets = newOffsets;
    doc->line_capacity = newCap;
    return TRUE;
}

static void Doc_EnsureLineMapUpTo(SlateDoc* doc, size_t targetOffset) {
    if (!doc || doc->line_map_complete || !doc->head || targetOffset == 0) return;

    if (targetOffset > doc->total_length) targetOffset = doc->total_length;

    Piece* piece = doc->line_scan_piece;
    size_t pieceOff = doc->line_scan_piece_offset;
    size_t logical = doc->line_scan_offset;

    while (piece && logical <= targetOffset) {
        if (pieceOff >= piece->length) {
            piece = piece->next;
            pieceOff = 0;
            continue;
        }

        if (piece->buffer == BUFFER_ORIGINAL && piece->isUtf8) {
            const char* buf = (const char*)doc->original_buffer;
            size_t idx = pieceOff;
            while (idx < piece->length && logical <= targetOffset) {
                if (buf[piece->start + idx] == '\n') {
                    if (!Doc_GrowLineOffsets(doc, 1)) break;
                    doc->line_offsets[doc->line_count++] = logical + 1;
                }
                idx++;
                logical++;
            }
            pieceOff = idx;
        } else {
            const WCHAR* buf = (piece->buffer == BUFFER_ORIGINAL) ? 
                               (WCHAR*)doc->original_buffer : doc->add_buffer;
            size_t idx = pieceOff;
            while (idx < piece->length && logical <= targetOffset) {
                if (buf && buf[piece->start + idx] == L'\n') {
                    if (!Doc_GrowLineOffsets(doc, 1)) break;
                    doc->line_offsets[doc->line_count++] = logical + 1;
                }
                idx++;
                logical++;
            }
            pieceOff = idx;
        }

        if (pieceOff >= piece->length) {
            piece = piece->next;
            pieceOff = 0;
        }
    }

    doc->line_scan_piece = piece;
    doc->line_scan_piece_offset = pieceOff;
    doc->line_scan_offset = logical;

    if (!piece || logical >= doc->total_length) {
        doc->line_map_complete = TRUE;
        if (Doc_GrowLineOffsets(doc, 1)) {
            doc->line_offsets[doc->line_count] = doc->total_length;
        }
    }
}

static void Doc_EnsureLineForIndex(SlateDoc* doc, size_t lineIndex) {
    if (!doc) return;

    while (!doc->line_map_complete && doc->line_count <= lineIndex) {
        size_t nextTarget = doc->line_scan_offset + LINE_SCAN_STEP_BYTES;
        if (nextTarget > doc->total_length) nextTarget = doc->total_length;
        Doc_EnsureLineMapUpTo(doc, nextTarget);
        if (doc->line_scan_offset == nextTarget) break; // Avoid infinite loop on allocation failure
    }
}

void Doc_RefreshMetadata(SlateDoc* pDoc) {
    if (!pDoc) return;

    // Recompute total length without scanning characters
    size_t totalLen = 0;
    for (Piece* curr = pDoc->head; curr; curr = curr->next) {
        totalLen += curr->length;
    }
    pDoc->total_length = totalLen;

    // Reset line map storage
    if (pDoc->line_offsets) {
        free(pDoc->line_offsets);
    }
    pDoc->line_capacity = LINE_MAP_GROW_STEP;
    pDoc->line_offsets = malloc(pDoc->line_capacity * sizeof(size_t));
    if (!pDoc->line_offsets) {
        pDoc->line_count = 0;
        pDoc->line_map_complete = TRUE;
        pDoc->line_scan_offset = 0;
        pDoc->line_scan_piece = NULL;
        pDoc->line_scan_piece_offset = 0;
        return;
    }

    pDoc->line_offsets[0] = 0;
    pDoc->line_count = 1;
    pDoc->line_map_complete = (totalLen == 0);
    pDoc->line_scan_offset = 0;
    pDoc->line_scan_piece = pDoc->head;
    pDoc->line_scan_piece_offset = 0;

    if (pDoc->line_map_complete && pDoc->line_capacity > 1) {
        pDoc->line_offsets[1] = 0;
    }
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
    if (!doc) return 0;
    Doc_EnsureLineForIndex(doc, lineIndex);
    if (lineIndex >= doc->line_count) return doc->total_length;
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

    // Refresh metadata and line map
    Doc_RefreshMetadata(doc);
    
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

    Doc_EnsureLineMapUpTo(doc, offset);

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

// ------------------------------
// Rabin–Karp search
// ------------------------------

typedef struct {
    Piece* piece;
    size_t pieceOffset;
    size_t logicalOffset;
} DocCharIterator;

static WCHAR FoldAscii(WCHAR ch, BOOL caseSensitive) {
    if (caseSensitive) return ch;
    if (ch >= L'A' && ch <= L'Z') return ch + 32;
    return ch;
}

static BOOL DocIter_Seek(SlateDoc* doc, size_t targetOffset, DocCharIterator* it) {
    if (!doc || !it) return FALSE;

    size_t cumulative = 0;
    Piece* curr = doc->head;
    while (curr && (cumulative + curr->length) <= targetOffset) {
        cumulative += curr->length;
        curr = curr->next;
    }

    it->piece = curr;
    it->pieceOffset = curr ? (targetOffset - cumulative) : 0;
    it->logicalOffset = targetOffset;
    return TRUE;
}

static WCHAR Doc_ReadChar(const SlateDoc* doc, const Piece* piece, size_t pieceOffset) {
    if (!doc || !piece) return 0;

    if (piece->buffer == BUFFER_ORIGINAL && piece->isUtf8) {
        const unsigned char* buf = (const unsigned char*)doc->original_buffer;
        return (WCHAR)buf[piece->start + pieceOffset];
    }

    const WCHAR* buf = (piece->buffer == BUFFER_ORIGINAL) ? (WCHAR*)doc->original_buffer : doc->add_buffer;
    return buf ? buf[piece->start + pieceOffset] : 0;
}

static BOOL DocIter_Next(SlateDoc* doc, DocCharIterator* it, WCHAR* outChar) {
    if (!doc || !it || !it->piece) return FALSE;

    *outChar = Doc_ReadChar(doc, it->piece, it->pieceOffset);

    // Advance iterator
    it->logicalOffset++;
    it->pieceOffset++;
    if (it->pieceOffset >= it->piece->length) {
        it->piece = it->piece->next;
        it->pieceOffset = 0;
    }
    return TRUE;
}

static BOOL WindowEquals(const WCHAR* windowBuf, size_t startIdx, const WCHAR* pattern, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (windowBuf[(startIdx + i) % len] != pattern[i]) return FALSE;
    }
    return TRUE;
}

DocSearchResult Doc_Search(SlateDoc* doc, const WCHAR* pattern, size_t patternLen, size_t cursorOffset, BOOL searchBackwards, BOOL caseSensitive) {
    DocSearchResult result = {0};
    result.status = DOC_SEARCH_NO_PATTERN;
    result.match_length = patternLen;
    result.line = 1;
    result.column = 1;

    if (!doc || !pattern || patternLen == 0) {
        return result; // No-op for empty pattern or null inputs
    }

    size_t docLen = doc->total_length;
    if (docLen == 0 || patternLen > docLen) {
        result.status = searchBackwards ? DOC_SEARCH_REACHED_BOF : DOC_SEARCH_REACHED_EOF;
        return result;
    }

    if (cursorOffset > docLen) cursorOffset = docLen;

    WCHAR* patternNorm = (WCHAR*)malloc(patternLen * sizeof(WCHAR));
    if (!patternNorm) {
        result.status = searchBackwards ? DOC_SEARCH_REACHED_BOF : DOC_SEARCH_REACHED_EOF;
        return result;
    }

    const unsigned int base = 257;
    const unsigned int mod  = 1000000007;
    unsigned long long patternHash = 0;
    unsigned long long windowHash = 0;
    unsigned long long highestPow = 1; // base^(patternLen-1)

    for (size_t i = 0; i < patternLen; i++) {
        patternNorm[i] = FoldAscii(pattern[i], caseSensitive);
        patternHash = (patternHash * base + patternNorm[i]) % mod;
        if (i < patternLen - 1) {
            highestPow = (highestPow * base) % mod;
        }
    }

    if (!searchBackwards) {
        // Forward search from cursorOffset to EOF
        if (cursorOffset + patternLen > docLen) {
            free(patternNorm);
            result.status = DOC_SEARCH_REACHED_EOF;
            return result;
        }

        DocCharIterator it;
        DocIter_Seek(doc, cursorOffset, &it);

        WCHAR* window = (WCHAR*)malloc(patternLen * sizeof(WCHAR));
        if (!window) {
            free(patternNorm);
            result.status = DOC_SEARCH_REACHED_EOF;
            return result;
        }

        // Prime the first window
        for (size_t i = 0; i < patternLen; i++) {
            WCHAR ch;
            if (!DocIter_Next(doc, &it, &ch)) {
                free(window);
                free(patternNorm);
                result.status = DOC_SEARCH_REACHED_EOF;
                return result;
            }
            window[i] = FoldAscii(ch, caseSensitive);
            windowHash = (windowHash * base + window[i]) % mod;
        }

        size_t windowStartIdx = 0;
        size_t currentStart = cursorOffset;
        size_t lastStart = docLen - patternLen;

        while (1) {
            if (windowHash == patternHash && WindowEquals(window, windowStartIdx, patternNorm, patternLen)) {
                result.status = DOC_SEARCH_MATCH;
                result.match_offset = currentStart;
                Doc_GetOffsetInfo(doc, currentStart, &result.line, &result.column);
                free(window);
                free(patternNorm);
                return result;
            }

            if (currentStart >= lastStart) break;

            WCHAR nextChar;
            if (!DocIter_Next(doc, &it, &nextChar)) break;
            WCHAR foldedNext = FoldAscii(nextChar, caseSensitive);

            WCHAR outgoing = window[windowStartIdx];
            windowStartIdx = (windowStartIdx + 1) % patternLen;
            size_t insertIdx = (windowStartIdx + patternLen - 1) % patternLen;
            window[insertIdx] = foldedNext;

            // Rolling hash: remove outgoing, add incoming
            unsigned long long temp = (windowHash + mod - (outgoing * highestPow) % mod) % mod;
            windowHash = (temp * base + foldedNext) % mod;

            currentStart++;
        }

        free(window);
        result.status = DOC_SEARCH_REACHED_EOF;
    } else {
        // Backward search: scan from start, keep the last match <= cursorOffset
        size_t lastAllowedStart = (cursorOffset + patternLen > docLen) ? (docLen - patternLen) : cursorOffset;

        DocCharIterator it;
        DocIter_Seek(doc, 0, &it);

        WCHAR* window = (WCHAR*)malloc(patternLen * sizeof(WCHAR));
        if (!window) {
            free(patternNorm);
            result.status = DOC_SEARCH_REACHED_BOF;
            return result;
        }

        for (size_t i = 0; i < patternLen; i++) {
            WCHAR ch;
            if (!DocIter_Next(doc, &it, &ch)) {
                free(window);
                free(patternNorm);
                result.status = DOC_SEARCH_REACHED_BOF;
                return result;
            }
            window[i] = FoldAscii(ch, caseSensitive);
            windowHash = (windowHash * base + window[i]) % mod;
        }

        size_t windowStartIdx = 0;
        size_t currentStart = 0;
        size_t bestMatch = (size_t)-1;

        while (currentStart <= lastAllowedStart) {
            if (windowHash == patternHash && WindowEquals(window, windowStartIdx, patternNorm, patternLen)) {
                bestMatch = currentStart;
            }

            if (currentStart == lastAllowedStart) break;

            WCHAR nextChar;
            if (!DocIter_Next(doc, &it, &nextChar)) break;
            WCHAR foldedNext = FoldAscii(nextChar, caseSensitive);

            WCHAR outgoing = window[windowStartIdx];
            windowStartIdx = (windowStartIdx + 1) % patternLen;
            size_t insertIdx = (windowStartIdx + patternLen - 1) % patternLen;
            window[insertIdx] = foldedNext;

            unsigned long long temp = (windowHash + mod - (outgoing * highestPow) % mod) % mod;
            windowHash = (temp * base + foldedNext) % mod;

            currentStart++;
        }

        free(window);

        if (bestMatch != (size_t)-1) {
            result.status = DOC_SEARCH_MATCH;
            result.match_offset = bestMatch;
            Doc_GetOffsetInfo(doc, bestMatch, &result.line, &result.column);
        } else {
            result.status = DOC_SEARCH_REACHED_BOF;
        }
    }

    free(patternNorm);
    return result;
}
