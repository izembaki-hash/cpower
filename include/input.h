/**
 * @file input.h
 * @brief Native Dual-Mode Control Engine interface contracts and kinematics types.
 *
 * Implements Direct Instant Mode (pure C COM UIA patterns and deadlock-guarded
 * Win32 messaging) and Human Emulation Mode (cubic Bezier curves, 5th-order
 * minimum-jerk velocity profile, Ornstein-Uhlenbeck neuromuscular tremor,
 * Box-Muller log-normal cognitive delays via SendInput).
 */

#ifndef CPOWER_INPUT_H
#define CPOWER_INPUT_H

#include "common.h"
#include "perception.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Control Modes, Mouse Buttons & Key Modifiers
 * ------------------------------------------------------------------------- */

typedef enum ControlMode {
    CONTROL_MODE_DIRECT_INSTANT  = 0, /**< Instant COM pattern / Win32 message dispatch */
    CONTROL_MODE_HUMAN_EMULATION = 1  /**< Bio-realistic trajectory and timing via SendInput */
} ControlMode;

typedef enum MouseButton {
    MOUSE_BTN_LEFT   = 0,
    MOUSE_BTN_RIGHT  = 1,
    MOUSE_BTN_MIDDLE = 2
} MouseButton;

typedef enum KeyModifierFlags {
    KEY_MOD_NONE  = 0,
    KEY_MOD_CTRL  = (1U << 0),
    KEY_MOD_SHIFT = (1U << 1),
    KEY_MOD_ALT   = (1U << 2),
    KEY_MOD_WIN   = (1U << 3)
} KeyModifierFlags;

/* -------------------------------------------------------------------------
 * Kinematics & Trajectory Configuration
 * ------------------------------------------------------------------------- */

typedef struct BezierPoint {
    double x;
    double y;
} BezierPoint;

typedef struct TrajectoryConfig {
    double speedScale;     /**< 1.0 = normal, < 1.0 = faster, > 1.0 = slower */
    double tremorScale;    /**< 1.0 = natural O-U tremor, 0.0 = disabled */
    double curvatureScale; /**< 1.0 = natural anatomical curve bias */
    double ouTheta;        /**< Ornstein-Uhlenbeck mean-reversion rate (default: 15.0) */
    double ouSigma;        /**< Ornstein-Uhlenbeck noise volatility (default: 2.5) */
    int    minSteps;       /**< Minimum interpolation steps per move (default: 10) */
} TrajectoryConfig;

typedef struct ControlConfig {
    ControlMode      defaultMode;
    TrajectoryConfig trajectory;
    DWORD            sendMessageTimeoutMs; /**< Default timeout for SendMessageTimeoutW (1000 ms) */
    double           typingWpm;            /**< Target words per minute scale (default: 65.0) */
} ControlConfig;

/* -------------------------------------------------------------------------
 * Control Engine Lifecycle
 * ------------------------------------------------------------------------- */

/**
 * @brief Initializes the dual-mode control subsystem and default parameters.
 * @return S_OK on success, or an HRESULT error code.
 */
HRESULT Control_Init(void);

/**
 * @brief Shuts down the control subsystem and releases global state.
 */
void Control_Shutdown(void);

/**
 * @brief Updates the active control engine configuration.
 * @param pConfig Pointer to ControlConfig.
 * @return S_OK on success, E_POINTER if pConfig is NULL.
 */
HRESULT Control_Configure(const ControlConfig* pConfig);

/* -------------------------------------------------------------------------
 * Direct Instant Mode Primitives (COM Patterns & Win32 Messaging)
 * ------------------------------------------------------------------------- */

/**
 * @brief Programmatically invokes an element via IUIAutomationInvokePattern.
 */
HRESULT Control_DirectInvoke(IUIAutomationElement* pElement);

/**
 * @brief Programmatically sets text/value on an element via IUIAutomationValuePattern.
 */
HRESULT Control_DirectSetValue(IUIAutomationElement* pElement, const WCHAR* pszText);

/**
 * @brief Toggles a checkbox or switch via IUIAutomationTogglePattern.
 */
HRESULT Control_DirectToggle(IUIAutomationElement* pElement, enum ToggleState* pNewState);

/**
 * @brief Selects a list/tab/radio item via IUIAutomationSelectionItemPattern.
 */
HRESULT Control_DirectSelect(IUIAutomationElement* pElement, BOOL bAddToSelection);

/**
 * @brief Dispatches an asynchronous click directly to client coordinates of a window.
 */
HRESULT Control_DirectClickHwnd(HWND hwnd, int clientX, int clientY);

/**
 * @brief Sends BM_CLICK to a Win32 button HWND using deadlock-guarded SendMessageTimeoutW.
 */
HRESULT Control_DirectClickButtonHwnd(HWND hwnd);

/**
 * @brief Sets text on a Win32 control HWND via SendMessageTimeoutW(WM_SETTEXT).
 */
HRESULT Control_DirectSetTextHwnd(HWND hwnd, const WCHAR* pszText);

/* -------------------------------------------------------------------------
 * Human Emulation Mode Primitives (Bezier + Minimum-Jerk + O-U + SendInput)
 * ------------------------------------------------------------------------- */

/**
 * @brief Moves the cursor smoothly from its current position to (targetX, targetY).
 * @param targetX Target screen coordinate X.
 * @param targetY Target screen coordinate Y.
 * @param speedScale Duration multiplier (1.0 = normal, 0.5 = fast, 0.0 = instant config default).
 * @return S_OK on success.
 */
HRESULT Control_EmulateMouseMove(int targetX, int targetY, double speedScale);

/**
 * @brief Moves to (targetX, targetY) with human kinematics and executes a realistic click.
 */
HRESULT Control_EmulateMouseClick(int targetX, int targetY, MouseButton button);

/**
 * @brief Moves to (targetX, targetY) and executes a human-timed double-click.
 */
HRESULT Control_EmulateMouseDoubleClick(int targetX, int targetY, MouseButton button);

/**
 * @brief Drags from (startX, startY) to (endX, endY) using human Bezier curves.
 */
HRESULT Control_EmulateMouseDrag(int startX, int startY, int endX, int endY, double speedScale);

/**
 * @brief Types a Unicode string with Box-Muller log-normal inter-keystroke delays.
 */
HRESULT Control_EmulateTypeText(const WCHAR* pszText, double speedScale);

/**
 * @brief Dispatches a single virtual key press/release with optional modifiers (Ctrl/Shift/Alt/Win).
 */
HRESULT Control_EmulateSendKey(WORD vkCode, uint32_t modifiers);

/* -------------------------------------------------------------------------
 * Pure Mathematical & Coordinate Helpers (Exposed for Diagnostics/Testing)
 * ------------------------------------------------------------------------- */

/**
 * @brief Computes 5th-order minimum-jerk spatial progress s(tau) for tau in [0, 1].
 */
double Control_MinimumJerk(double tau);

/**
 * @brief Evaluates a cubic Bezier curve at parameter s in [0, 1].
 */
BezierPoint Control_EvalCubicBezier(BezierPoint p0, BezierPoint p1, BezierPoint p2, BezierPoint p3, double s);

/**
 * @brief Generates trajectory points between two coordinates without dispatching OS input.
 */
HRESULT Control_GenerateTrajectoryPoints(
    int startX,
    int startY,
    int targetX,
    int targetY,
    const TrajectoryConfig* pCfg,
    BezierPoint* pOutPts,
    int maxPts,
    int* pOutCount
);

/**
 * @brief Converts physical screen coordinates to SendInput [0..65535] virtual desktop space.
 */
void Control_ScreenToVirtualDesktop(int screenX, int screenY, LONG* pNormX, LONG* pNormY);

/* -------------------------------------------------------------------------
 * Window Management, Scrolling, Failsafe & Closed-Loop Verification (v1.1.0)
 * ------------------------------------------------------------------------- */

typedef enum WindowShowAction {
    WND_RESTORE  = 0,
    WND_MINIMIZE = 1,
    WND_MAXIMIZE = 2,
    WND_CLOSE    = 3
} WindowShowAction;

#pragma pack(push, 4)

typedef struct WindowInfo {
    uint64_t hwnd;         /**< Window handle cast to fixed 64-bit integer */
    uint32_t pid;          /**< Owning process ID */
    RECT     rect;         /**< Window bounding rectangle in screen coordinates */
    uint32_t isVisible;    /**< Non-zero if visible */
    uint32_t isMinimized;  /**< Non-zero if iconic/minimized */
    WCHAR    title[128];   /**< Window title */
    WCHAR    className[64];/**< Win32 window class */
} WindowInfo;

#pragma pack(pop)

_Static_assert(sizeof(WindowInfo) == 420, "WindowInfo packed size must be 420 bytes");

typedef HRESULT (*ControlActionCallback)(void* pUserData);

/**
 * @brief Enumerates visible top-level desktop windows.
 */
HRESULT Control_EnumWindows(WindowInfo* pOut, uint32_t maxCount, uint32_t* pOutCount);

/**
 * @brief Brings a target window to the foreground (using AttachThreadInput fallback when needed).
 */
HRESULT Control_FocusWindow(HWND hwnd);

/**
 * @brief Restores, minimizes, maximizes, or posts WM_CLOSE to a target window.
 */
HRESULT Control_ShowWindow(HWND hwnd, WindowShowAction action);

/**
 * @brief Moves to (x, y) if specified and sends vertical or horizontal mouse wheel scroll input.
 */
HRESULT Control_EmulateMouseScroll(int x, int y, int delta, BOOL horizontal);

/**
 * @brief Enables or disables emergency top-left corner / Pause-key failsafe checks before SendInput.
 */
void Control_SetFailsafeEnabled(BOOL enabled);

/**
 * @brief Checks if the emergency failsafe is currently triggered (returns FALSE when disabled).
 */
BOOL Control_CheckFailsafeTriggered(void);

/**
 * @brief Captures pre-action semantic tree, runs pfnAction, waits for UI delta, and returns diff.
 * @return S_OK if semantic change detected, S_FALSE if action succeeded without detected tree diff,
 *         or error HRESULT on action/capture failure.
 */
HRESULT Control_ExecuteAndVerify(
    HWND targetHwnd,
    ControlActionCallback pfnAction,
    void* pUserData,
    DWORD verifyTimeoutMs,
    SemanticTreeDiff** ppDiffOut
);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_INPUT_H */
