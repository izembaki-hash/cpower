/**
 * @file human_keyboard.c
 * @brief Human Emulation Mode Keyboard Engine via SendInput.
 *
 * Implements Unicode string typing with Box-Muller log-normal dwell and
 * QWERTY hand-alternation inter-keystroke intervals, plus modifier-aware
 * virtual keystroke synthesis with safe reverse-order modifier release.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "input.h"
#include <math.h>
#include <string.h>

/* Internal config accessor defined in direct_control.c */
void Control_GetActiveConfigInternal(ControlConfig* pOutConfig);

static double Control_KbdClampDouble(double val, double minVal, double maxVal) {
    if (val < minVal) {
        return minVal;
    }
    if (val > maxVal) {
        return maxVal;
    }
    return val;
}

static double Control_SampleKeyTiming(double medianMs, double sigma) {
    if (medianMs <= 0.0) {
        return 0.0;
    }
    double minMs = (medianMs * 0.25 < 0.5) ? (medianMs * 0.25) : 0.5;
    double maxMs = medianMs * 5.0;
    if (maxMs < minMs) {
        maxMs = minMs;
    }

    double sample = Math_SampleLogNormal(medianMs, sigma);
    if (!isfinite(sample)) {
        sample = medianMs;
    }
    return Control_KbdClampDouble(sample, minMs, maxMs);
}

static int Control_GetQwertyHand(WCHAR ch) {
    switch (ch) {
        /* Left-hand QWERTY keys */
        case L'q': case L'Q': case L'w': case L'W': case L'e': case L'E':
        case L'r': case L'R': case L't': case L'T':
        case L'a': case L'A': case L's': case L'S': case L'd': case L'D':
        case L'f': case L'F': case L'g': case L'G':
        case L'z': case L'Z': case L'x': case L'X': case L'c': case L'C':
        case L'v': case L'V': case L'b': case L'B':
        case L'1': case L'2': case L'3': case L'4': case L'5':
        case L'!': case L'@': case L'#': case L'$': case L'%':
        case L'`': case L'~':
            return 1;

        /* Right-hand QWERTY keys */
        case L'y': case L'Y': case L'u': case L'U': case L'i': case L'I':
        case L'o': case L'O': case L'p': case L'P':
        case L'h': case L'H': case L'j': case L'J': case L'k': case L'K':
        case L'l': case L'L':
        case L'n': case L'N': case L'm': case L'M':
        case L'6': case L'7': case L'8': case L'9': case L'0':
        case L'^': case L'&': case L'*': case L'(': case L')':
        case L'-': case L'_': case L'=': case L'+':
        case L'[': case L'{': case L']': case L'}':
        case L'\\': case L'|': case L';': case L':':
        case L'\'': case L'"': case L',': case L'<':
        case L'.': case L'>': case L'/': case L'?':
            return 2;

        default:
            return 0;
    }
}

static BOOL Control_IsExtendedVk(WORD vk) {
    switch (vk) {
        case VK_RMENU:
        case VK_RCONTROL:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_CANCEL:
        case VK_SNAPSHOT:
        case VK_DIVIDE:
            return TRUE;
        default:
            return FALSE;
    }
}

static HRESULT Control_SendUnicodeKeyEvent(WCHAR ch, BOOL keyUp) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type       = INPUT_KEYBOARD;
    input.ki.wVk     = 0;
    input.ki.wScan   = (WORD)ch;
    input.ki.dwFlags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0U);

    SetLastError(0);
    UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        DWORD err = GetLastError();
        if (err == 0) {
            err = ERROR_ACCESS_DENIED;
        }
        return HRESULT_FROM_WIN32(err);
    }
    return S_OK;
}

static HRESULT Control_SendUnicodeSurrogatePairEvent(WCHAR hi, WCHAR lo, BOOL keyUp) {
    INPUT inputs[2];
    memset(inputs, 0, sizeof(inputs));

    DWORD flags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0U);

    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = 0;
    inputs[0].ki.wScan   = (WORD)hi;
    inputs[0].ki.dwFlags = flags;

    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = 0;
    inputs[1].ki.wScan   = (WORD)lo;
    inputs[1].ki.dwFlags = flags;

    SetLastError(0);
    UINT sent = SendInput(2, inputs, sizeof(INPUT));
    if (sent != 2U) {
        DWORD err = GetLastError();
        if (err == 0) {
            err = ERROR_ACCESS_DENIED;
        }
        return HRESULT_FROM_WIN32(err);
    }
    return S_OK;
}

static HRESULT Control_SendVirtualKeyEvent(WORD vkCode, BOOL keyUp) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type     = INPUT_KEYBOARD;
    input.ki.wVk   = vkCode;
    input.ki.wScan = (WORD)MapVirtualKeyW((UINT)vkCode, MAPVK_VK_TO_VSC);

    DWORD flags = 0;
    if (Control_IsExtendedVk(vkCode)) {
        flags |= KEYEVENTF_EXTENDEDKEY;
    }
    if (keyUp) {
        flags |= KEYEVENTF_KEYUP;
    }
    input.ki.dwFlags = flags;

    SetLastError(0);
    UINT sent = SendInput(1, &input, sizeof(INPUT));
    if (sent != 1) {
        DWORD err = GetLastError();
        if (err == 0) {
            err = ERROR_ACCESS_DENIED;
        }
        return HRESULT_FROM_WIN32(err);
    }
    return S_OK;
}

static double Control_GetKeyboardScale(double speedScale) {
    ControlConfig cfg;
    Control_GetActiveConfigInternal(&cfg);

    double wpmFactor = (cfg.typingWpm > 0.0) ? (65.0 / cfg.typingWpm) : 1.0;
    double baseScale = (speedScale > 0.0)
        ? speedScale
        : ((cfg.trajectory.speedScale > 0.0) ? cfg.trajectory.speedScale : 1.0);

    return baseScale * wpmFactor;
}

/* -------------------------------------------------------------------------
 * Human Emulation Mode Primitives (Keyboard via SendInput)
 * ------------------------------------------------------------------------- */

HRESULT Control_EmulateTypeText(const WCHAR* pszText, double speedScale) {
    if (!pszText) {
        return E_POINTER;
    }
    if (!isfinite(speedScale) || speedScale < 0.0) {
        return E_INVALIDARG;
    }
    if (pszText[0] == L'\0') {
        return S_OK;
    }

    if (Control_CheckFailsafeTriggered()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    double effectiveScale = Control_GetKeyboardScale(speedScale);

    for (size_t i = 0; pszText[i] != L'\0'; ) {
        if (Control_CheckFailsafeTriggered()) {
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }

        WCHAR ch = pszText[i];
        WCHAR nextCh = pszText[i + 1];
        BOOL isSurrogatePair = (ch >= 0xD800U && ch <= 0xDBFFU) &&
                               (nextCh >= 0xDC00U && nextCh <= 0xDFFFU);
        size_t nextIdx = isSurrogatePair ? (i + 2U) : (i + 1U);

        HRESULT hr = isSurrogatePair
            ? Control_SendUnicodeSurrogatePairEvent(ch, nextCh, FALSE)
            : Control_SendUnicodeKeyEvent(ch, FALSE);
        if (FAILED(hr)) {
            return hr;
        }

        double dwellMs = Control_SampleKeyTiming(65.0 * effectiveScale, 0.25);
        Timer_AccurateSleep(dwellMs);

        hr = isSurrogatePair
            ? Control_SendUnicodeSurrogatePairEvent(ch, nextCh, TRUE)
            : Control_SendUnicodeKeyEvent(ch, TRUE);
        if (FAILED(hr)) {
            return hr;
        }

        if (pszText[nextIdx] != L'\0') {
            int curHand  = isSurrogatePair ? 0 : Control_GetQwertyHand(ch);
            int nextHand = Control_GetQwertyHand(pszText[nextIdx]);

            double ikiMedian = (curHand != 0 && curHand == nextHand) ? 140.0 : 95.0;
            double ikiSigma  = (curHand != 0 && curHand == nextHand) ? 0.38  : 0.32;
            double ikiMs     = Control_SampleKeyTiming(ikiMedian * effectiveScale, ikiSigma);

            Timer_AccurateSleep(ikiMs);
        }

        i = nextIdx;
    }

    return S_OK;
}

HRESULT Control_EmulateSendKey(WORD vkCode, uint32_t modifiers) {
    const uint32_t validModMask = KEY_MOD_CTRL | KEY_MOD_SHIFT | KEY_MOD_ALT | KEY_MOD_WIN;
    if (vkCode == 0 || (modifiers & ~validModMask) != 0U) {
        return E_INVALIDARG;
    }
    if (Control_CheckFailsafeTriggered()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    WORD modKeys[4];
    int modCount = 0;

    if ((modifiers & KEY_MOD_CTRL) != 0U) {
        modKeys[modCount++] = VK_CONTROL;
    }
    if ((modifiers & KEY_MOD_SHIFT) != 0U) {
        modKeys[modCount++] = VK_SHIFT;
    }
    if ((modifiers & KEY_MOD_ALT) != 0U) {
        modKeys[modCount++] = VK_MENU;
    }
    if ((modifiers & KEY_MOD_WIN) != 0U) {
        modKeys[modCount++] = VK_LWIN;
    }

    double effectiveScale = Control_GetKeyboardScale(0.0);
    HRESULT hr = S_OK;
    int pressedCount = 0;

    /* Press modifier keys in canonical order */
    for (int i = 0; i < modCount; ++i) {
        hr = Control_SendVirtualKeyEvent(modKeys[i], FALSE);
        if (FAILED(hr)) {
            goto release_modifiers;
        }
        pressedCount++;
        Timer_AccurateSleep(Control_SampleKeyTiming(18.0 * effectiveScale, 0.25));
    }

    /* Press and hold primary virtual key */
    hr = Control_SendVirtualKeyEvent(vkCode, FALSE);
    if (FAILED(hr)) {
        goto release_modifiers;
    }

    Timer_AccurateSleep(Control_SampleKeyTiming(65.0 * effectiveScale, 0.25));

    /* Release primary virtual key */
    hr = Control_SendVirtualKeyEvent(vkCode, TRUE);
    if (FAILED(hr)) {
        goto release_modifiers;
    }

    if (pressedCount > 0) {
        Timer_AccurateSleep(Control_SampleKeyTiming(18.0 * effectiveScale, 0.25));
    }

release_modifiers:
    /* Release modifiers in reverse order even if an intermediate step failed */
    for (int i = pressedCount - 1; i >= 0; --i) {
        HRESULT hrRel = Control_SendVirtualKeyEvent(modKeys[i], TRUE);
        if (SUCCEEDED(hr) && FAILED(hrRel)) {
            hr = hrRel;
        }
        if (i > 0) {
            Timer_AccurateSleep(Control_SampleKeyTiming(15.0 * effectiveScale, 0.25));
        }
    }

    return hr;
}
