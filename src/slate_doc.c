#include "slate_doc.h"
#include <stdlib.h>
#include <string.h>

#define LINE_MAP_GROW_STEP 1024
#define LINE_SCAN_STEP_BYTES (64 * 1024)

static Piece* CreatePiece(BufferType buffer, size_t start, size_t length, size_t rawLength, BOOL isUtf8) {
    Piece* p = (Piece*)malloc(sizeof(Piece));
    if (p) {
        p->buffer = buffer;
        p->start = start;
        p->length = length;
        p->rawLength = rawLength;
        p->isUtf8 = isUtf8;
        p->next = NULL;
    }
    return p;
}

// ------------------------------
// UTF-8 <-> UTF-16 unit accounting
//
// Piece::length is always a count of logical UTF-16 code units (matching the
// units the view, cursor, line map, and search all work in). For UTF-8
// original pieces, the physical storage is raw bytes, so we need to walk the
// byte stream ourselves to translate between "N logical units" and "N bytes".
// A UTF-8 sequence of 1-3 bytes decodes to a single UTF-16 unit; a 4-byte
// sequence encodes a codepoint above the BMP and decodes to a surrogate pair
// (2 units).
// ------------------------------

// Inspects the UTF-8 sequence starting at buf[i] (bounded by limit) and
// returns its byte length, storing the number of UTF-16 units it decodes to
// in *outUnits. Malformed/stray bytes are treated as a single byte/unit so
// callers always make forward progress.
static int Utf8SeqInfo(const unsigned char* buf, size_t i, size_t limit, int* outUnits) {
    unsigned char b0 = buf[i];
    int seqLen;
    int units = 1;

    if (b0 < 0x80) {
        seqLen = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        seqLen = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        seqLen = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        seqLen = 4;
        units = 2; // Above the BMP -> encoded as a UTF-16 surrogate pair
    } else {
        seqLen = 1; // Stray continuation byte or invalid lead byte
    }

    if (i + (size_t)seqLen > limit) seqLen = (int)(limit - i); // Truncated at a piece boundary
    if (seqLen < 1) seqLen = 1;

    *outUnits = units;
    return seqLen;
}

// Walks forward from byteStart (up to byteLimit), consuming up to maxUnits
// logical UTF-16 units, and returns the byte offset reached. Never splits a
// surrogate pair: if consuming the next character would overshoot maxUnits,
// it stops before that character. *outUnits (optional) receives the number
// of units actually consumed, which can be less than maxUnits if the piece
// runs out of bytes, or if the target falls between the two halves of a
// surrogate pair.
static size_t Utf8ByteOffsetForUnits(const unsigned char* buf, size_t byteStart, size_t byteLimit, size_t maxUnits, size_t* outUnits) {
    size_t i = byteStart;
    size_t units = 0;
    while (i < byteLimit && units < maxUnits) {
        int u;
        int seqLen = Utf8SeqInfo(buf, i, byteLimit, &u);
        if (units + (size_t)u > maxUnits) break;
        units += (size_t)u;
        i += (size_t)seqLen;
    }
    if (outUnits) *outUnits = units;
    return i;
}

// Counts the total number of UTF-16 units a (possibly huge) UTF-8 byte buffer decodes
// to. MultiByteToWideChar takes `int` lengths, so for buffers at or beyond INT_MAX bytes
// (a real case for the multi-gigabyte files this editor is meant to handle) a single call
// would either fail outright or silently misbehave from the byte count truncating/
// wrapping when cast to int. Process in bounded chunks instead, each backed off to a
// UTF-8 character boundary so no multi-byte sequence gets split across chunk edges.
static size_t Utf8CountUnitsChunked(const unsigned char* buf, size_t byteLen) {
    // MultiByteToWideChar hard-fails (ERROR_INVALID_PARAMETER) for a single call at or
    // above 1 GiB, well under INT_MAX - empirically confirmed (512 MiB succeeds, 1 GiB
    // does not). Stay well clear of that undocumented cliff.
    const size_t CHUNK_LIMIT = 0x10000000; // 256 MiB
    size_t total = 0;
    size_t pos = 0;

    while (pos < byteLen) {
        size_t remaining = byteLen - pos;
        size_t chunkEnd = (remaining > CHUNK_LIMIT) ? (pos + CHUNK_LIMIT) : byteLen;

        if (chunkEnd < byteLen) {
            size_t original = chunkEnd;
            while (chunkEnd > pos && (buf[chunkEnd] & 0xC0) == 0x80) chunkEnd--;
            if (chunkEnd == pos) chunkEnd = original; // pathological run of continuation bytes - just cut here
        }

        int units = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + pos, (int)(chunkEnd - pos), NULL, 0);
        if (units > 0) total += (size_t)units;

        pos = chunkEnd;
    }

    return total;
}

static Piece* SplitPiece(SlateDoc* doc, size_t offset) {
    if (offset == 0) return doc->head;
    if (offset >= doc->total_length) return NULL;

    Piece* curr = doc->head;
    size_t cumulative = 0;

    while (curr) {
        if (offset == cumulative) return curr;

        if (offset > cumulative && offset < cumulative + curr->length) {
            size_t splitPoint = offset - cumulative; // Logical (UTF-16 unit) offset into this piece

            size_t secondStart, secondLength, secondRawLength, firstRawLength;

            if (curr->buffer == BUFFER_ORIGINAL && curr->isUtf8) {
                // Translate the logical split point into a byte offset within the raw UTF-8 storage
                const unsigned char* buf = (const unsigned char*)doc->original_buffer;
                size_t unitsConsumed = 0;
                size_t byteOff = Utf8ByteOffsetForUnits(buf, curr->start, curr->start + curr->rawLength, splitPoint, &unitsConsumed);

                firstRawLength = byteOff - curr->start;
                secondStart = byteOff;
                secondRawLength = curr->rawLength - firstRawLength;
                secondLength = curr->length - unitsConsumed;
                splitPoint = unitsConsumed; // May be smaller than requested if it would split a surrogate pair
            } else {
                secondStart = curr->start + splitPoint;
                secondRawLength = curr->rawLength - splitPoint;
                secondLength = curr->length - splitPoint;
                firstRawLength = splitPoint;
            }

            // Preserve encoding flag when splitting the piece
            Piece* secondHalf = CreatePiece(curr->buffer, secondStart, secondLength, secondRawLength, curr->isUtf8);
            if (secondHalf) {
                secondHalf->next = curr->next;
                curr->length = splitPoint;
                curr->rawLength = firstRawLength;
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
        if (pieceOff >= piece->rawLength) {
            piece = piece->next;
            pieceOff = 0;
            continue;
        }

        if (piece->buffer == BUFFER_ORIGINAL && piece->isUtf8) {
            // pieceOff is a BYTE offset here (raw UTF-8 storage), not a logical unit count
            const unsigned char* buf = (const unsigned char*)doc->original_buffer;
            size_t idx = pieceOff;
            size_t byteLimit = piece->start + piece->rawLength;
            while (idx < piece->rawLength && logical <= targetOffset) {
                size_t absIdx = piece->start + idx;
                int units;
                int seqLen = Utf8SeqInfo(buf, absIdx, byteLimit, &units);

                // A newline is always a single-byte ASCII sequence
                if (seqLen == 1 && buf[absIdx] == '\n') {
                    if (!Doc_GrowLineOffsets(doc, 1)) break;
                    doc->line_offsets[doc->line_count++] = logical + 1;
                }
                idx += (size_t)seqLen;
                logical += (size_t)units;
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

        if (pieceOff >= piece->rawLength) {
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

// Recomputes total_length by summing piece lengths. O(piece count); never
// scans character data.
static void Doc_RecalculateTotalLength(SlateDoc* pDoc) {
    size_t totalLen = 0;
    for (Piece* curr = pDoc->head; curr; curr = curr->next) {
        totalLen += curr->length;
    }
    pDoc->total_length = totalLen;
}

// Locates the piece and raw storage offset within it that correspond to
// logical offset `target`, for resuming the lazy line scanner mid-document.
static void Doc_LocateScanResumePoint(SlateDoc* pDoc, size_t target, Piece** outPiece, size_t* outPieceOffset) {
    Piece* piece = pDoc->head;
    size_t cumulative = 0;
    while (piece && cumulative + piece->length <= target) {
        cumulative += piece->length;
        piece = piece->next;
    }

    *outPiece = piece;
    if (!piece) {
        *outPieceOffset = 0;
        return;
    }

    size_t logicalIntoPiece = target - cumulative;
    if (piece->buffer == BUFFER_ORIGINAL && piece->isUtf8) {
        const unsigned char* buf = (const unsigned char*)pDoc->original_buffer;
        size_t byteOff = Utf8ByteOffsetForUnits(buf, piece->start, piece->start + piece->rawLength, logicalIntoPiece, NULL);
        *outPieceOffset = byteOff - piece->start;
    } else {
        *outPieceOffset = logicalIntoPiece;
    }
}

// Full metadata refresh: recomputes total_length and resets the line map to
// scratch (rescanned lazily from offset 0 on demand). Used whenever the
// piece list may have changed in ways with no single "edit point" - initial
// load and undo/redo, where the whole piece list is swapped wholesale.
void Doc_RefreshMetadata(SlateDoc* pDoc) {
    if (!pDoc) return;

    Doc_RecalculateTotalLength(pDoc);

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
    pDoc->line_map_complete = (pDoc->total_length == 0);
    pDoc->line_scan_offset = 0;
    pDoc->line_scan_piece = pDoc->head;
    pDoc->line_scan_piece_offset = 0;

    if (pDoc->line_map_complete && pDoc->line_capacity > 1) {
        pDoc->line_offsets[1] = 0;
    }
}

// Incremental metadata refresh for a single edit (insert or delete) that
// starts at `editOffset`. Recomputes total_length, but instead of throwing
// away the whole line map, it only discards line-start entries at or after
// the edit point (anything before is untouched by the edit) and resumes lazy
// scanning from there. This keeps Doc_Insert/Doc_Delete - called on every
// keystroke - from forcing an O(document size) rescan on every edit.
static void Doc_RefreshMetadataFrom(SlateDoc* pDoc, size_t editOffset) {
    if (!pDoc) return;

    Doc_RecalculateTotalLength(pDoc);

    if (!pDoc->line_offsets) {
        Doc_RefreshMetadata(pDoc); // No map allocated yet - fall back to a full (cheap) reset
        return;
    }

    // Binary search for the first cached line-start offset after editOffset;
    // everything before that index remains valid and unchanged.
    size_t lo = 0, hi = pDoc->line_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pDoc->line_offsets[mid] <= editOffset) lo = mid + 1;
        else hi = mid;
    }
    pDoc->line_count = lo;

    size_t resumeOffset = (lo > 0) ? pDoc->line_offsets[lo - 1] : 0;
    if (resumeOffset > pDoc->total_length) resumeOffset = pDoc->total_length;

    Piece* piece;
    size_t pieceOffset;
    Doc_LocateScanResumePoint(pDoc, resumeOffset, &piece, &pieceOffset);

    pDoc->line_scan_offset = resumeOffset;
    pDoc->line_scan_piece = piece;
    pDoc->line_scan_piece_offset = pieceOffset;
    // Left FALSE unconditionally: Doc_EnsureLineMapUpTo already sets this back
    // to TRUE itself as soon as it discovers there's nothing left to scan.
    pDoc->line_map_complete = FALSE;
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

BOOL Doc_Undo(SlateDoc* pDoc, size_t currentCursor, size_t* outCursor) {
    if (!pDoc->undo_stack) return FALSE;

    // Before restoring the old state, save the current state to Redo so we can undo the undo
    UndoStep* redoStep = malloc(sizeof(UndoStep));
    redoStep->cursor_hint = currentCursor; // Save current cursor position
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

BOOL Doc_Redo(SlateDoc* pDoc, size_t currentCursor, size_t* outCursor) {
    if (!pDoc || !pDoc->redo_stack) return FALSE;

    // Pop from the redo stack
    UndoStep* step = pDoc->redo_stack;
    pDoc->redo_stack = step->next;

    // Push current state to the undo stack before overwriting it
    Doc_PushUndo(pDoc, currentCursor, FALSE);

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


void Doc_EnsureLineForIndex(SlateDoc* doc, size_t lineIndex) {
    if (!doc) return;

    while (!doc->line_map_complete && doc->line_count <= lineIndex) {
        size_t nextTarget = doc->line_scan_offset + LINE_SCAN_STEP_BYTES;
        if (nextTarget > doc->total_length) nextTarget = doc->total_length;
        Doc_EnsureLineMapUpTo(doc, nextTarget);
        if (doc->line_scan_offset == nextTarget) break; // Avoid infinite loop on allocation failure
    }
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

    // For UTF-8, `len` is the raw byte count; the piece's LOGICAL length (in UTF-16
    // units, as used by every offset calculation elsewhere) must be decoded up front.
    // Chunked to stay correct for files at or beyond INT_MAX bytes.
    size_t logicalLen = len;
    if (isUtf8 && len > 0) {
        logicalLen = Utf8CountUnitsChunked((const unsigned char*)pMappedText, len);
    }

    doc->head = CreatePiece(BUFFER_ORIGINAL, 0, logicalLen, len, isUtf8);

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

    if (offset == doc->total_length) {
        // Append to very end (also handles the empty-document case)
        Piece* curr = doc->head;
        while (curr && curr->next) curr = curr->next;

        // If the trailing piece is an ADD piece whose stored range ends exactly
        // where we just appended, extend it in place instead of allocating a new
        // node - this is the common case of typing forward continuously, and
        // keeps piece count (and everything that walks it) from growing by one
        // node per keystroke.
        if (curr && curr->buffer == BUFFER_ADD && curr->start + curr->rawLength == add_start_index) {
            curr->length += len;
            curr->rawLength += len;
        } else {
            Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len, len, FALSE);
            if (curr) curr->next = newP;
            else doc->head = newP;
        }
    } else {
        // SplitPiece guarantees a piece boundary starts exactly at `offset`,
        // splitting an existing piece if needed (encoding-aware for UTF-8 pieces).
        Piece* after = SplitPiece(doc, offset);
        Piece* prev = NULL;
        if (after != doc->head) {
            prev = doc->head;
            while (prev && prev->next != after) prev = prev->next;
        }

        // Same in-place extension as above, but for typing in the middle of a
        // document: the piece immediately preceding the cursor is the one most
        // recently typed, and repeated keystrokes land right after it.
        if (prev && prev->buffer == BUFFER_ADD && prev->start + prev->rawLength == add_start_index) {
            prev->length += len;
            prev->rawLength += len;
        } else {
            Piece* newP = CreatePiece(BUFFER_ADD, add_start_index, len, len, FALSE);
            if (prev) prev->next = newP;
            else doc->head = newP;
            newP->next = after;
        }
    }

    // Update metadata and line map (incrementally - only from the edit point onward)
    Doc_RefreshMetadataFrom(doc, offset);

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

    // Refresh metadata and line map (incrementally - only from the edit point onward)
    Doc_RefreshMetadataFrom(doc, offset);

    return TRUE;
}

size_t Doc_GetText(SlateDoc* doc, size_t offset, size_t len, WCHAR* dest) {
    if (offset >= doc->total_length) return 0;
    if (offset + len > doc->total_length) len = doc->total_length - offset;

    Piece* curr = doc->head;
    size_t cumulative = 0;
    size_t destPos = 0;
    size_t unitsConsumed = 0;

    while (curr && unitsConsumed < len) {
        if (destPos >= len) break;

        if (offset < cumulative + curr->length) {
            size_t startInPiece = (offset > cumulative) ? (offset - cumulative) : 0;
            size_t takeFromPiece = curr->length - startInPiece;
            size_t remaining = len - unitsConsumed;
            if (takeFromPiece > remaining) takeFromPiece = remaining;

            if (curr->buffer == BUFFER_ORIGINAL && curr->isUtf8) {
                // startInPiece/takeFromPiece are logical (UTF-16) units; translate them
                // into a byte range within the raw UTF-8 storage before decoding.
                const unsigned char* buf = (const unsigned char*)doc->original_buffer;
                size_t byteStart = Utf8ByteOffsetForUnits(buf, curr->start, curr->start + curr->rawLength, startInPiece, NULL);
                size_t byteEnd = Utf8ByteOffsetForUnits(buf, byteStart, curr->start + curr->rawLength, takeFromPiece, NULL);

                int written = 0;
                if (byteEnd > byteStart) {
                    written = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + byteStart,
                                       (int)(byteEnd - byteStart), dest + destPos, (int)(len - destPos));
                }
                // Advance both counters by the SAME (actual) amount so they never desync.
                if (written > 0) {
                    destPos += written;
                    unitsConsumed += written;
                } else {
                    unitsConsumed += takeFromPiece;
                }
            } else {
                const WCHAR* src = (curr->buffer == BUFFER_ORIGINAL) ? (WCHAR*)doc->original_buffer : doc->add_buffer;
                memcpy(dest + destPos, src + curr->start + startInPiece, takeFromPiece * sizeof(WCHAR));
                destPos += takeFromPiece;
                unitsConsumed += takeFromPiece;
            }
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
    size_t pieceOffset;   // Raw storage offset within the piece (bytes for UTF-8 original pieces, WCHAR units otherwise)
    size_t logicalOffset;
    WCHAR  pendingLow;    // Queued low surrogate from a just-decoded astral character, or 0
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
    it->logicalOffset = targetOffset;
    it->pendingLow = 0;

    if (curr) {
        size_t logicalIntoPiece = targetOffset - cumulative;
        if (curr->buffer == BUFFER_ORIGINAL && curr->isUtf8) {
            const unsigned char* buf = (const unsigned char*)doc->original_buffer;
            size_t byteOff = Utf8ByteOffsetForUnits(buf, curr->start, curr->start + curr->rawLength, logicalIntoPiece, NULL);
            it->pieceOffset = byteOff - curr->start;
        } else {
            it->pieceOffset = logicalIntoPiece;
        }
    } else {
        it->pieceOffset = 0;
    }
    return TRUE;
}

static BOOL DocIter_Next(SlateDoc* doc, DocCharIterator* it, WCHAR* outChar) {
    if (!doc || !it) return FALSE;

    // Emit a queued low surrogate before anything else; this doesn't consume
    // any more raw storage, but the piece it came from may now be exhausted.
    if (it->pendingLow) {
        *outChar = it->pendingLow;
        it->pendingLow = 0;
        it->logicalOffset++;
        if (it->piece && it->pieceOffset >= it->piece->rawLength) {
            it->piece = it->piece->next;
            it->pieceOffset = 0;
        }
        return TRUE;
    }

    if (!it->piece) return FALSE;
    Piece* piece = it->piece;

    if (piece->buffer == BUFFER_ORIGINAL && piece->isUtf8) {
        const unsigned char* buf = (const unsigned char*)doc->original_buffer;
        size_t idx = piece->start + it->pieceOffset;
        size_t byteLimit = piece->start + piece->rawLength;

        int units;
        int seqLen = Utf8SeqInfo(buf, idx, byteLimit, &units);

        WCHAR wbuf[2] = { 0, 0 };
        int written = MultiByteToWideChar(CP_UTF8, 0, (const char*)buf + idx, seqLen, wbuf, 2);
        if (written <= 0) {
            wbuf[0] = (WCHAR)buf[idx]; // Malformed byte - fall back to a literal unit so we still progress
            written = 1;
        }

        *outChar = wbuf[0];
        it->pieceOffset += (size_t)seqLen;

        if (written > 1) {
            it->pendingLow = wbuf[1]; // Piece-advance is deferred until this is consumed
        } else if (it->pieceOffset >= piece->rawLength) {
            it->piece = piece->next;
            it->pieceOffset = 0;
        }
    } else {
        const WCHAR* buf = (piece->buffer == BUFFER_ORIGINAL) ? (WCHAR*)doc->original_buffer : doc->add_buffer;
        *outChar = buf ? buf[piece->start + it->pieceOffset] : 0;
        it->pieceOffset++;
        if (it->pieceOffset >= piece->length) {
            it->piece = piece->next;
            it->pieceOffset = 0;
        }
    }

    it->logicalOffset++;
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
