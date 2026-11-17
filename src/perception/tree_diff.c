/**
 * @file tree_diff.c
 * @brief Semantic node locator and O(N) FNV-1a structural delta tree diff engine.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "perception.h"
#include "ipc.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

/* -------------------------------------------------------------------------
 * Case-Insensitive Bounded Wide-String Utilities
 * ------------------------------------------------------------------------- */

static size_t td_bounded_wcslen(const WCHAR* s, size_t maxChars) {
    if (!s || maxChars == 0U) {
        return 0U;
    }
    size_t len = 0U;
    while (len < maxChars && s[len] != L'\0') {
        len++;
    }
    return len;
}

static BOOL td_wcs_icontains(const WCHAR* haystack, size_t maxHaystack, const WCHAR* needle, size_t maxNeedle) {
    if (!needle || needle[0] == L'\0') {
        return TRUE;
    }
    if (!haystack) {
        return FALSE;
    }

    size_t nLen = td_bounded_wcslen(needle, maxNeedle);
    if (nLen == 0U) {
        return TRUE;
    }
    size_t hLen = td_bounded_wcslen(haystack, maxHaystack);
    if (hLen < nLen) {
        return FALSE;
    }

    for (size_t i = 0U; i <= hLen - nLen; i++) {
        BOOL match = TRUE;
        for (size_t j = 0U; j < nLen; j++) {
            wint_t hc = towlower((wint_t)haystack[i + j]);
            wint_t nc = towlower((wint_t)needle[j]);
            if (hc != nc) {
                match = FALSE;
                break;
            }
        }
        if (match) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL td_wcs_iequals(const WCHAR* a, size_t maxA, const WCHAR* b, size_t maxB) {
    if (!a || !b) {
        return FALSE;
    }
    size_t lenA = td_bounded_wcslen(a, maxA);
    size_t lenB = td_bounded_wcslen(b, maxB);
    if (lenA != lenB) {
        return FALSE;
    }
    for (size_t i = 0U; i < lenA; i++) {
        if (towlower((wint_t)a[i]) != towlower((wint_t)b[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

/* -------------------------------------------------------------------------
 * Semantic Node Locator
 * ------------------------------------------------------------------------- */

HRESULT Perception_FindNodes(
    const SemanticTree* pTree,
    const SemanticQuery* pQuery,
    SemanticMatch* pMatches,
    uint32_t maxMatches,
    uint32_t* pMatchCount
) {
    if (!pTree || !pQuery || !pMatchCount) {
        return E_POINTER;
    }
    if (maxMatches > 0U && !pMatches) {
        return E_POINTER;
    }
    *pMatchCount = 0U;

    if (pTree->nodeCount > 0U && !pTree->nodes) {
        return E_INVALIDARG;
    }

    uint32_t count = 0U;
    for (uint32_t i = 0U; i < pTree->nodeCount; i++) {
        const SemanticNode* n = &pTree->nodes[i];

        if (pQuery->controlTypeId != 0U && n->controlTypeId != pQuery->controlTypeId) {
            continue;
        }
        if (pQuery->onlyEnabled && !(n->flags & CPOWER_NODE_FLAG_ENABLED)) {
            continue;
        }
        if (pQuery->onlyVisible) {
            if (n->flags & CPOWER_NODE_FLAG_OFFSCREEN) {
                continue;
            }
            if (n->bounds.right <= n->bounds.left || n->bounds.bottom <= n->bounds.top) {
                continue;
            }
        }
        if (pQuery->automationId[0] != L'\0') {
            if (!td_wcs_iequals(n->automationId, 64U, pQuery->automationId, 64U)) {
                continue;
            }
        }
        if (pQuery->className[0] != L'\0') {
            if (!td_wcs_icontains(n->className, 64U, pQuery->className, 64U)) {
                continue;
            }
        }
        if (pQuery->nameSubstr[0] != L'\0') {
            if (!td_wcs_icontains(n->name, 128U, pQuery->nameSubstr, 128U)) {
                continue;
            }
        }

        if (pMatches && count < maxMatches) {
            pMatches[count].nodeId = n->id;
            pMatches[count].center.x = n->bounds.left + (n->bounds.right - n->bounds.left) / 2;
            pMatches[count].center.y = n->bounds.top + (n->bounds.bottom - n->bounds.top) / 2;
            pMatches[count].node = *n;
        }
        count++;
    }

    *pMatchCount = count;
    return S_OK;
}

/* -------------------------------------------------------------------------
 * FNV-1a Structural Fingerprinting & Open-Addressed Hash Map
 * ------------------------------------------------------------------------- */

#define TD_FNV1A_OFFSET 14695981039346656037ULL
#define TD_FNV1A_PRIME  1099511628211ULL

static uint64_t fnv1a_mix_bytes(uint64_t h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0U; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= TD_FNV1A_PRIME;
    }
    return h;
}

static uint64_t fnv1a_mix_u32(uint64_t h, uint32_t v) {
    return fnv1a_mix_bytes(h, &v, sizeof(v));
}

static uint64_t fnv1a_mix_u64(uint64_t h, uint64_t v) {
    return fnv1a_mix_bytes(h, &v, sizeof(v));
}

static uint64_t fnv1a_mix_wstr(uint64_t h, const WCHAR* ws, size_t maxChars) {
    size_t len = td_bounded_wcslen(ws, maxChars);
    h = fnv1a_mix_u32(h, (uint32_t)len);
    if (len > 0U) {
        h = fnv1a_mix_bytes(h, ws, len * sizeof(WCHAR));
    }
    return h;
}

static uint64_t splitmix64_hash(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

typedef struct TdSlot {
    uint64_t key;
    uint32_t val;
    uint8_t  occupied;
} TdSlot;

typedef struct TdMap {
    TdSlot* slots;
    size_t  cap;
} TdMap;

static BOOL td_map_init(TdMap* m, uint32_t expectedItems) {
#if (SIZE_MAX >> 32) == 0
    if ((size_t)expectedItems > (SIZE_MAX - 8U) / 2U) {
        m->slots = NULL;
        m->cap = 0U;
        return FALSE;
    }
#endif
    size_t cap = 16U;
    size_t target = ((size_t)expectedItems * 2U) + 8U;
    while (cap < target) {
        if (cap > (SIZE_MAX / 2U)) {
            return FALSE;
        }
        cap *= 2U;
    }
    m->slots = (TdSlot*)calloc(cap, sizeof(TdSlot));
    if (!m->slots) {
        m->cap = 0U;
        return FALSE;
    }
    m->cap = cap;
    return TRUE;
}

static void td_map_free(TdMap* m) {
    if (m->slots) {
        free(m->slots);
        m->slots = NULL;
    }
    m->cap = 0U;
}

static uint32_t td_map_inc_count(TdMap* m, uint64_t key) {
    size_t mask = m->cap - 1U;
    size_t idx = (size_t)splitmix64_hash(key) & mask;
    for (;;) {
        TdSlot* s = &m->slots[idx];
        if (!s->occupied) {
            s->occupied = 1U;
            s->key = key;
            s->val = 1U;
            return 0U;
        }
        if (s->key == key) {
            uint32_t prev = s->val;
            s->val++;
            return prev;
        }
        idx = (idx + 1U) & mask;
    }
}

static void td_map_put_if_absent(TdMap* m, uint64_t key, uint32_t val) {
    size_t mask = m->cap - 1U;
    size_t idx = (size_t)splitmix64_hash(key) & mask;
    for (;;) {
        TdSlot* s = &m->slots[idx];
        if (!s->occupied) {
            s->occupied = 1U;
            s->key = key;
            s->val = val;
            return;
        }
        if (s->key == key) {
            return;
        }
        idx = (idx + 1U) & mask;
    }
}

static BOOL td_map_get(const TdMap* m, uint64_t key, uint32_t* pValOut) {
    if (!m || !m->slots || m->cap == 0U) {
        return FALSE;
    }
    size_t mask = m->cap - 1U;
    size_t idx = (size_t)splitmix64_hash(key) & mask;
    for (;;) {
        const TdSlot* s = &m->slots[idx];
        if (!s->occupied) {
            return FALSE;
        }
        if (s->key == key) {
            *pValOut = s->val;
            return TRUE;
        }
        idx = (idx + 1U) & mask;
    }
}

static HRESULT compute_tree_fingerprints(
    const SemanticTree* pTree,
    uint64_t* pBaseStrict,
    uint64_t* pKeyStrict,
    uint64_t* pBaseLoose
) {
    uint32_t n = pTree->nodeCount;
    if (n == 0U) {
        return S_OK;
    }

    TdMap occMap = { 0 };
    if (!td_map_init(&occMap, n)) {
        return E_OUTOFMEMORY;
    }

    for (uint32_t i = 0U; i < n; i++) {
        const SemanticNode* node = &pTree->nodes[i];
        uint64_t parentStrict = TD_FNV1A_OFFSET;
        uint64_t parentLoose  = TD_FNV1A_OFFSET;

        if (node->parentId >= 0 && (uint32_t)node->parentId < i) {
            parentStrict = pBaseStrict[(uint32_t)node->parentId];
            parentLoose  = pBaseLoose[(uint32_t)node->parentId];
        }

        uint64_t hl = fnv1a_mix_u64(TD_FNV1A_OFFSET, parentLoose);
        hl = fnv1a_mix_u32(hl, (uint32_t)node->controlTypeId);
        hl = fnv1a_mix_wstr(hl, node->className, 64U);
        hl = fnv1a_mix_wstr(hl, node->automationId, 64U);
        pBaseLoose[i] = hl;

        uint64_t hs = fnv1a_mix_u64(TD_FNV1A_OFFSET, parentStrict);
        hs = fnv1a_mix_u32(hs, (uint32_t)node->controlTypeId);
        hs = fnv1a_mix_wstr(hs, node->className, 64U);
        hs = fnv1a_mix_wstr(hs, node->automationId, 64U);
        hs = fnv1a_mix_wstr(hs, node->name, 128U);
        pBaseStrict[i] = hs;

        uint32_t occ = td_map_inc_count(&occMap, hs);
        uint64_t ks = fnv1a_mix_u32(hs ^ 0xA5A5A5A55A5A5A5AULL, occ);
        pKeyStrict[i] = ks;
    }

    td_map_free(&occMap);
    return S_OK;
}

static uint32_t compute_node_diff_mask(const SemanticNode* a, const SemanticNode* b) {
    uint32_t mask = 0U;
    if (memcmp(&a->bounds, &b->bounds, sizeof(RECT)) != 0) {
        mask |= CPOWER_DIFF_CHANGE_BOUNDS;
    }
    if (td_bounded_wcslen(a->value, 128U) != td_bounded_wcslen(b->value, 128U) ||
        wmemcmp(a->value, b->value, td_bounded_wcslen(a->value, 128U)) != 0) {
        mask |= CPOWER_DIFF_CHANGE_VALUE;
    }
    if (a->flags != b->flags) {
        mask |= CPOWER_DIFF_CHANGE_FLAGS;
    }
    if (td_bounded_wcslen(a->name, 128U) != td_bounded_wcslen(b->name, 128U) ||
        wmemcmp(a->name, b->name, td_bounded_wcslen(a->name, 128U)) != 0) {
        mask |= CPOWER_DIFF_CHANGE_NAME;
    }
    return mask;
}

HRESULT Perception_DiffTrees(
    const SemanticTree* pPrev,
    const SemanticTree* pCurr,
    SemanticTreeDiff** ppDiffOut
) {
    if (!pCurr || !ppDiffOut) {
        return E_POINTER;
    }
    *ppDiffOut = NULL;

    if (pCurr->nodeCount > 0U && !pCurr->nodes) {
        return E_INVALIDARG;
    }
    if (pPrev && pPrev->nodeCount > 0U && !pPrev->nodes) {
        return E_INVALIDARG;
    }

    uint32_t prevN = pPrev ? pPrev->nodeCount : 0U;
    uint32_t currN = pCurr->nodeCount;
    if (prevN > UINT32_MAX - currN) {
        return E_INVALIDARG;
    }

    SemanticTreeDiff* pDiff = (SemanticTreeDiff*)calloc(1, sizeof(SemanticTreeDiff));
    if (!pDiff) {
        return E_OUTOFMEMORY;
    }
    pDiff->rootHwnd = pCurr->rootHwnd;
    pDiff->prevTimestampUtc = pPrev ? pPrev->timestampUtc : 0ULL;
    pDiff->currTimestampUtc = pCurr->timestampUtc;

    uint32_t maxEntries = prevN + currN;
    if (maxEntries == 0U) {
        *ppDiffOut = pDiff;
        return S_OK;
    }

    pDiff->entries = (TreeDiffEntry*)calloc(maxEntries, sizeof(TreeDiffEntry));
    if (!pDiff->entries) {
        free(pDiff);
        return E_OUTOFMEMORY;
    }

    uint64_t* prevBaseStrict = prevN ? (uint64_t*)calloc(prevN, sizeof(uint64_t)) : NULL;
    uint64_t* prevKeyStrict  = prevN ? (uint64_t*)calloc(prevN, sizeof(uint64_t)) : NULL;
    uint64_t* prevBaseLoose  = prevN ? (uint64_t*)calloc(prevN, sizeof(uint64_t)) : NULL;
    uint8_t*  prevMatched    = prevN ? (uint8_t*)calloc(prevN, sizeof(uint8_t)) : NULL;

    uint64_t* currBaseStrict = currN ? (uint64_t*)calloc(currN, sizeof(uint64_t)) : NULL;
    uint64_t* currKeyStrict  = currN ? (uint64_t*)calloc(currN, sizeof(uint64_t)) : NULL;
    uint64_t* currBaseLoose  = currN ? (uint64_t*)calloc(currN, sizeof(uint64_t)) : NULL;
    uint32_t* currMatchIdx   = currN ? (uint32_t*)malloc(currN * sizeof(uint32_t)) : NULL;

    TdMap prevStrictMap = { 0 };
    TdMap looseOccPrev = { 0 };
    TdMap loosePrevMap = { 0 };
    TdMap looseOccCurr = { 0 };
    HRESULT hr = S_OK;

    if ((prevN && (!prevBaseStrict || !prevKeyStrict || !prevBaseLoose || !prevMatched)) ||
        (currN && (!currBaseStrict || !currKeyStrict || !currBaseLoose || !currMatchIdx))) {
        hr = E_OUTOFMEMORY;
        goto cleanup;
    }

    for (uint32_t i = 0U; i < currN; i++) {
        currMatchIdx[i] = UINT32_MAX;
    }

    if (prevN > 0U) {
        hr = compute_tree_fingerprints(pPrev, prevBaseStrict, prevKeyStrict, prevBaseLoose);
        if (FAILED(hr)) {
            goto cleanup;
        }
    }
    if (currN > 0U) {
        hr = compute_tree_fingerprints(pCurr, currBaseStrict, currKeyStrict, currBaseLoose);
        if (FAILED(hr)) {
            goto cleanup;
        }
    }

    /* Pass 1: Strict match (includes name + sibling occurrence) */
    if (prevN > 0U && currN > 0U) {
        if (!td_map_init(&prevStrictMap, prevN)) {
            hr = E_OUTOFMEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0U; i < prevN; i++) {
            td_map_put_if_absent(&prevStrictMap, prevKeyStrict[i], i);
        }
        for (uint32_t j = 0U; j < currN; j++) {
            uint32_t pi = 0U;
            if (td_map_get(&prevStrictMap, currKeyStrict[j], &pi) && !prevMatched[pi]) {
                prevMatched[pi] = 1U;
                currMatchIdx[j] = pi;
            }
        }
        td_map_free(&prevStrictMap);

        /* Pass 2: Loose match for unmatched nodes (catches label/button text changes) */
        if (!td_map_init(&looseOccPrev, prevN) ||
            !td_map_init(&loosePrevMap, prevN) ||
            !td_map_init(&looseOccCurr, currN)) {
            hr = E_OUTOFMEMORY;
            goto cleanup;
        }

        for (uint32_t i = 0U; i < prevN; i++) {
            if (prevMatched[i]) {
                continue;
            }
            uint32_t occ = td_map_inc_count(&looseOccPrev, prevBaseLoose[i]);
            uint64_t lkey = fnv1a_mix_u32(prevBaseLoose[i] ^ 0x517CC1B727220A95ULL, occ);
            td_map_put_if_absent(&loosePrevMap, lkey, i);
        }

        for (uint32_t j = 0U; j < currN; j++) {
            if (currMatchIdx[j] != UINT32_MAX) {
                continue;
            }
            uint32_t occ = td_map_inc_count(&looseOccCurr, currBaseLoose[j]);
            uint64_t lkey = fnv1a_mix_u32(currBaseLoose[j] ^ 0x517CC1B727220A95ULL, occ);
            uint32_t pi = 0U;
            if (td_map_get(&loosePrevMap, lkey, &pi) && !prevMatched[pi]) {
                prevMatched[pi] = 1U;
                currMatchIdx[j] = pi;
            }
        }
        td_map_free(&looseOccPrev);
        td_map_free(&loosePrevMap);
        td_map_free(&looseOccCurr);
    }

    uint32_t outCount = 0U;

    /* Emit ADDED and MODIFIED in current tree order */
    for (uint32_t j = 0U; j < currN; j++) {
        uint32_t pi = currMatchIdx[j];
        if (pi == UINT32_MAX) {
            TreeDiffEntry* e = &pDiff->entries[outCount++];
            e->op = DIFF_OP_ADDED;
            e->nodeId = pCurr->nodes[j].id;
            e->prevNodeId = UINT32_MAX;
            e->changedMask = 0U;
            e->fingerprint = currKeyStrict[j];
            e->node = pCurr->nodes[j];
            pDiff->addedCount++;
        } else {
            uint32_t mask = compute_node_diff_mask(&pPrev->nodes[pi], &pCurr->nodes[j]);
            if (mask != 0U) {
                TreeDiffEntry* e = &pDiff->entries[outCount++];
                e->op = DIFF_OP_MODIFIED;
                e->nodeId = pCurr->nodes[j].id;
                e->prevNodeId = pPrev->nodes[pi].id;
                e->changedMask = mask;
                e->fingerprint = currKeyStrict[j];
                e->node = pCurr->nodes[j];
                pDiff->modifiedCount++;
            }
        }
    }

    /* Emit REMOVED from previous tree */
    for (uint32_t i = 0U; i < prevN; i++) {
        if (!prevMatched[i]) {
            TreeDiffEntry* e = &pDiff->entries[outCount++];
            e->op = DIFF_OP_REMOVED;
            e->nodeId = pPrev->nodes[i].id;
            e->prevNodeId = pPrev->nodes[i].id;
            e->changedMask = 0U;
            e->fingerprint = prevKeyStrict[i];
            e->node = pPrev->nodes[i];
            pDiff->removedCount++;
        }
    }

    pDiff->entryCount = outCount;
    *ppDiffOut = pDiff;
    pDiff = NULL;

cleanup:
    td_map_free(&prevStrictMap);
    td_map_free(&looseOccPrev);
    td_map_free(&loosePrevMap);
    td_map_free(&looseOccCurr);
    free(prevBaseStrict);
    free(prevKeyStrict);
    free(prevBaseLoose);
    free(prevMatched);
    free(currBaseStrict);
    free(currKeyStrict);
    free(currBaseLoose);
    free(currMatchIdx);
    if (pDiff) {
        Perception_FreeTreeDiff(pDiff);
    }
    return hr;
}

void Perception_FreeTreeDiff(SemanticTreeDiff* pDiff) {
    if (!pDiff) {
        return;
    }
    if (pDiff->entries) {
        free(pDiff->entries);
        pDiff->entries = NULL;
    }
    free(pDiff);
}

/* -------------------------------------------------------------------------
 * Diff Serialization (UTF-8 JSON & Binary DIFF Packet)
 * ------------------------------------------------------------------------- */

typedef struct TdStrBuf {
    char*  data;
    size_t cap;
    size_t len;
} TdStrBuf;

static void td_sb_putc(TdStrBuf* sb, char c) {
    if (sb->data && sb->len + 1U < sb->cap) {
        sb->data[sb->len] = c;
    }
    sb->len++;
}

static void td_sb_puts(TdStrBuf* sb, const char* s) {
    if (!s) {
        return;
    }
    while (*s != '\0') {
        td_sb_putc(sb, *s++);
    }
}

static void td_sb_printf(TdStrBuf* sb, const char* fmt, ...) {
    char tmp[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n <= 0) {
        return;
    }
    size_t written = (size_t)n;
    if (written >= sizeof(tmp)) {
        written = sizeof(tmp) - 1U;
    }
    for (size_t i = 0U; i < written; i++) {
        td_sb_putc(sb, tmp[i]);
    }
}

static void td_sb_append_json_wstr(TdStrBuf* sb, const WCHAR* ws, size_t maxWchars) {
    td_sb_putc(sb, '"');
    if (ws && maxWchars > 0U) {
        size_t wlen = td_bounded_wcslen(ws, maxWchars);
        if (wlen > 0U) {
            char utf8[640];
            int utf8Bytes = WideCharToMultiByte(
                CP_UTF8, 0, ws, (int)wlen, utf8, (int)sizeof(utf8), NULL, NULL
            );
            if (utf8Bytes > 0) {
                for (int i = 0; i < utf8Bytes; i++) {
                    unsigned char ch = (unsigned char)utf8[i];
                    switch (ch) {
                        case '"':  td_sb_puts(sb, "\\\""); break;
                        case '\\': td_sb_puts(sb, "\\\\"); break;
                        case '\b': td_sb_puts(sb, "\\b");  break;
                        case '\f': td_sb_puts(sb, "\\f");  break;
                        case '\n': td_sb_puts(sb, "\\n");  break;
                        case '\r': td_sb_puts(sb, "\\r");  break;
                        case '\t': td_sb_puts(sb, "\\t");  break;
                        default:
                            if (ch < 0x20U) {
                                td_sb_printf(sb, "\\u%04x", (unsigned int)ch);
                            } else {
                                td_sb_putc(sb, (char)ch);
                            }
                            break;
                    }
                }
            }
        }
    }
    td_sb_putc(sb, '"');
}

static const char* td_op_name(uint32_t op) {
    switch ((TreeDiffOp)op) {
        case DIFF_OP_ADDED:    return "added";
        case DIFF_OP_REMOVED:  return "removed";
        case DIFF_OP_MODIFIED: return "modified";
        default:               return "unknown";
    }
}

HRESULT Perception_SerializeDiffJson(
    const SemanticTreeDiff* pDiff,
    char* pBuffer,
    size_t bufferSize,
    size_t* pBytesWritten
) {
    if (!pDiff) {
        return E_POINTER;
    }
    if (!pBuffer && !pBytesWritten) {
        return E_POINTER;
    }
    if (pDiff->entryCount > 0U && !pDiff->entries) {
        return E_INVALIDARG;
    }

    TdStrBuf sb;
    sb.data = pBuffer;
    sb.cap = bufferSize;
    sb.len = 0U;

    td_sb_printf(
        &sb,
        "{\"rootHwnd\":\"0x%llx\",\"prevTimestampUtc\":%llu,\"currTimestampUtc\":%llu,"
        "\"entryCount\":%u,\"addedCount\":%u,\"removedCount\":%u,\"modifiedCount\":%u,\"entries\":[",
        (unsigned long long)(uintptr_t)pDiff->rootHwnd,
        (unsigned long long)pDiff->prevTimestampUtc,
        (unsigned long long)pDiff->currTimestampUtc,
        (unsigned int)pDiff->entryCount,
        (unsigned int)pDiff->addedCount,
        (unsigned int)pDiff->removedCount,
        (unsigned int)pDiff->modifiedCount
    );

    for (uint32_t i = 0U; i < pDiff->entryCount; i++) {
        const TreeDiffEntry* e = &pDiff->entries[i];
        const SemanticNode* n = &e->node;
        if (i > 0U) {
            td_sb_putc(&sb, ',');
        }
        td_sb_printf(
            &sb,
            "{\"op\":\"%s\",\"nodeId\":%u,\"prevNodeId\":%u,\"changedMask\":%u,"
            "\"fingerprint\":\"0x%016llx\",\"controlType\":\"%s\","
            "\"bounds\":{\"left\":%ld,\"top\":%ld,\"right\":%ld,\"bottom\":%ld},",
            td_op_name(e->op),
            (unsigned int)e->nodeId,
            (unsigned int)e->prevNodeId,
            (unsigned int)e->changedMask,
            (unsigned long long)e->fingerprint,
            Perception_ControlTypeIdToString((long)n->controlTypeId),
            (long)n->bounds.left,
            (long)n->bounds.top,
            (long)n->bounds.right,
            (long)n->bounds.bottom
        );
        td_sb_puts(&sb, "\"name\":");
        td_sb_append_json_wstr(&sb, n->name, 128U);
        td_sb_puts(&sb, ",\"automationId\":");
        td_sb_append_json_wstr(&sb, n->automationId, 64U);
        td_sb_puts(&sb, ",\"value\":");
        td_sb_append_json_wstr(&sb, n->value, 128U);
        td_sb_putc(&sb, '}');
    }

    td_sb_puts(&sb, "]}");

    if (!pBuffer && bufferSize > 0U) {
        return E_POINTER;
    }
    if (sb.len == SIZE_MAX) {
        return E_INVALIDARG;
    }

    size_t required = sb.len + 1U;
    if (pBytesWritten) {
        *pBytesWritten = required;
    }
    if (!pBuffer || bufferSize < required) {
        if (pBuffer && bufferSize > 0U) {
            pBuffer[bufferSize - 1U] = '\0';
        }
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    pBuffer[sb.len] = '\0';
    return S_OK;
}

static volatile LONG g_diffSequenceId = 0;

HRESULT Perception_SerializeDiffBinary(
    const SemanticTreeDiff* pDiff,
    void* pBuffer,
    size_t bufferSize,
    size_t* pBytesWritten
) {
    if (!pDiff) {
        return E_POINTER;
    }
    if (!pBuffer && !pBytesWritten) {
        return E_POINTER;
    }
    if (pDiff->entryCount > 0U && !pDiff->entries) {
        return E_INVALIDARG;
    }

    if ((uint64_t)pDiff->entryCount > (0xFFFFFFFFULL - (uint64_t)sizeof(DiffPacketHeader)) / (uint64_t)sizeof(TreeDiffEntry)) {
        return E_INVALIDARG;
    }

    size_t entriesBytes = (size_t)pDiff->entryCount * sizeof(TreeDiffEntry);
    size_t totalBytes = sizeof(DiffPacketHeader) + entriesBytes;
    if (totalBytes > (size_t)UINT32_MAX) {
        return E_INVALIDARG;
    }

    if (!pBuffer && bufferSize > 0U) {
        return E_POINTER;
    }
    if (pBytesWritten) {
        *pBytesWritten = totalBytes;
    }
    if (!pBuffer || bufferSize < totalBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    DiffPacketHeader hdr;
    ZeroMemory(&hdr, sizeof(hdr));
    hdr.packetMagic = CPOWER_DIFF_PACKET_MAGIC;
    hdr.sequenceId = (uint32_t)InterlockedIncrement(&g_diffSequenceId);
    hdr.prevTimestampUtc = pDiff->prevTimestampUtc;
    hdr.currTimestampUtc = pDiff->currTimestampUtc;
    hdr.totalPayloadSize = (uint32_t)totalBytes;
    hdr.entryCount = pDiff->entryCount;
    hdr.addedCount = pDiff->addedCount;
    hdr.removedCount = pDiff->removedCount;
    hdr.modifiedCount = pDiff->modifiedCount;

    uint8_t* dst = (uint8_t*)pBuffer;
    memcpy(dst, &hdr, sizeof(hdr));
    if (entriesBytes > 0U) {
        memcpy(dst + sizeof(hdr), pDiff->entries, entriesBytes);
    }
    return S_OK;
}
