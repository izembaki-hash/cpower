/**
 * @file bezier_mouse.c
 * @brief Human Emulation Mode Mouse Engine (Cubic Bezier + Minimum-Jerk + O-U Tremor).
 *
 * Implements 5th-order minimum-jerk velocity pacing, curved cubic Bezier
 * trajectory synthesis, Ornstein-Uhlenbeck mean-reverting neuromuscular tremor
 * with target-proximity damping, virtual desktop coordinate normalization, and
 * SendInput physical dispatch with Box-Muller log-normal timing.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "input.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Internal config accessor defined in direct_control.c */
void Control_GetActiveConfigInternal(ControlConfig* pOutConfig);

#define CPOWER_STACK_TRAJECTORY_CAP 1024
#define CPOWER_MAX_TRAJECTORY_CAP   16384

static double Control_ClampDouble(double val, double minVal, double maxVal) {
    if (val < minVal) {
        return minVal;
    }
    if (val > maxVal) {
        return maxVal;
    }
    return val;
}

static HRESULT Control_SendMouseInput(DWORD dwFlags, LONG dx, LONG dy) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = dwFlags;

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

static HRESULT Control_ButtonToFlags(MouseButton button, DWORD* pDownFlag, DWORD* pUpFlag) {
    if (!pDownFlag || !pUpFlag) {
        return E_POINTER;
    }

    switch (button) {
        case MOUSE_BTN_LEFT:
            *pDownFlag = MOUSEEVENTF_LEFTDOWN;
            *pUpFlag   = MOUSEEVENTF_LEFTUP;
            return S_OK;
        case MOUSE_BTN_RIGHT:
            *pDownFlag = MOUSEEVENTF_RIGHTDOWN;
            *pUpFlag   = MOUSEEVENTF_RIGHTUP;
            return S_OK;
        case MOUSE_BTN_MIDDLE:
            *pDownFlag = MOUSEEVENTF_MIDDLEDOWN;
            *pUpFlag   = MOUSEEVENTF_MIDDLEUP;
            return S_OK;
        default:
            return E_INVALIDARG;
    }
}

static double Control_SampleClampedLogNormal(double medianMs, double sigma, double minMs, double maxMs) {
    if (medianMs <= 0.0) {
        return minMs;
    }
    double sample = Math_SampleLogNormal(medianMs, sigma);
    if (!isfinite(sample)) {
        sample = medianMs;
    }
    return Control_ClampDouble(sample, minMs, maxMs);
}

/* -------------------------------------------------------------------------
 * Pure Mathematical & Coordinate Helpers
 * ------------------------------------------------------------------------- */

double Control_MinimumJerk(double tau) {
    if (isnan(tau) || tau <= 0.0) {
        return 0.0;
    }
    if (tau >= 1.0) {
        return 1.0;
    }

    double t2 = tau * tau;
    double t3 = t2 * tau;
    double s  = t3 * (10.0 - 15.0 * tau + 6.0 * t2);
    return Control_ClampDouble(s, 0.0, 1.0);
}

BezierPoint Control_EvalCubicBezier(BezierPoint p0, BezierPoint p1, BezierPoint p2, BezierPoint p3, double s) {
    if (!isfinite(s)) {
        return p0;
    }

    double u  = 1.0 - s;
    double u2 = u * u;
    double s2 = s * s;

    double b0 = u2 * u;
    double b1 = 3.0 * u2 * s;
    double b2 = 3.0 * u * s2;
    double b3 = s2 * s;

    BezierPoint pt;
    pt.x = b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x;
    pt.y = b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y;
    return pt;
}

static HRESULT Control_NormalizeTrajectoryConfig(const TrajectoryConfig* pInCfg, TrajectoryConfig* pOutCfg) {
    if (!pOutCfg) {
        return E_POINTER;
    }

    if (!pInCfg) {
        ControlConfig activeCfg;
        Control_GetActiveConfigInternal(&activeCfg);
        *pOutCfg = activeCfg.trajectory;
    } else {
        if (!isfinite(pInCfg->speedScale)     || pInCfg->speedScale < 0.0 ||
            !isfinite(pInCfg->tremorScale)    || pInCfg->tremorScale < 0.0 ||
            !isfinite(pInCfg->curvatureScale) || pInCfg->curvatureScale < 0.0 ||
            !isfinite(pInCfg->ouTheta)        || pInCfg->ouTheta < 0.0 ||
            !isfinite(pInCfg->ouSigma)        || pInCfg->ouSigma < 0.0 ||
            pInCfg->minSteps < 0) {
            return E_INVALIDARG;
        }
        *pOutCfg = *pInCfg;
    }

    if (pOutCfg->speedScale == 0.0) {
        pOutCfg->speedScale = 1.0;
    }
    if (pOutCfg->ouTheta == 0.0) {
        pOutCfg->ouTheta = 15.0;
    }
    if (pOutCfg->ouSigma == 0.0) {
        pOutCfg->ouSigma = 2.5;
    }
    if (pOutCfg->minSteps == 0) {
        pOutCfg->minSteps = 10;
    }

    return S_OK;
}

static int Control_EstimateStepCount(double dist, const TrajectoryConfig* pCfg) {
    double durationMs = (120.0 + 90.0 * log(1.0 + dist / 25.0)) * pCfg->speedScale;
    int steps = (int)(durationMs / 8.0 + 0.5);
    if (steps < pCfg->minSteps) {
        steps = pCfg->minSteps;
    }
    if (steps < 1) {
        steps = 1;
    }
    if (steps > CPOWER_MAX_TRAJECTORY_CAP) {
        steps = CPOWER_MAX_TRAJECTORY_CAP;
    }
    return steps;
}

HRESULT Control_GenerateTrajectoryPoints(
    int startX,
    int startY,
    int targetX,
    int targetY,
    const TrajectoryConfig* pCfg,
    BezierPoint* pOutPts,
    int maxPts,
    int* pOutCount
) {
    if (!pOutPts || !pOutCount) {
        return E_POINTER;
    }
    *pOutCount = 0;

    if (maxPts <= 0) {
        return E_INVALIDARG;
    }

    TrajectoryConfig cfg;
    HRESULT hr = Control_NormalizeTrajectoryConfig(pCfg, &cfg);
    if (FAILED(hr)) {
        return hr;
    }

    double dx   = (double)targetX - (double)startX;
    double dy   = (double)targetY - (double)startY;
    double dist = hypot(dx, dy);

    if (dist < 1e-6) {
        pOutPts[0].x = (double)targetX;
        pOutPts[0].y = (double)targetY;
        *pOutCount = 1;
        return S_OK;
    }

    /* Unit tangent and unit normal vectors */
    double ux = dx / dist;
    double uy = dy / dist;
    double nx = -uy;
    double ny = ux;

    /* Longitudinal fractions and lateral anatomical curvature offsets */
    double u1 = Math_RandomUniform(0.20, 0.35);
    double u2 = Math_RandomUniform(0.65, 0.85);
    double sign = (Math_RandomUniform(0.0, 1.0) < 0.5) ? -1.0 : 1.0;
    double kappa1 = Math_RandomUniform(0.05, 0.20);
    double w1 = kappa1 * dist * sign * cfg.curvatureScale;
    double w2 = w1 * Math_RandomUniform(0.40, 0.85) +
                Math_RandomUniform(-0.05, 0.05) * dist * cfg.curvatureScale;

    BezierPoint p0 = { (double)startX, (double)startY };
    BezierPoint p3 = { (double)targetX, (double)targetY };
    BezierPoint p1 = { p0.x + u1 * dx + w1 * nx, p0.y + u1 * dy + w1 * ny };
    BezierPoint p2 = { p0.x + u2 * dx + w2 * nx, p0.y + u2 * dy + w2 * ny };

    int steps = Control_EstimateStepCount(dist, &cfg);
    if (steps > maxPts) {
        steps = maxPts;
    }

    /* Euler-Maruyama O-U tremor state (ensure stability for custom high theta) */
    double jx = 0.0;
    double jy = 0.0;
    double dt = 0.008;
    if (cfg.ouTheta * dt > 0.5) {
        dt = 0.5 / cfg.ouTheta;
    }
    double decay = 1.0 - cfg.ouTheta * dt;
    double noiseGain = cfg.ouSigma * sqrt(dt);

    for (int step = 1; step <= steps; ++step) {
        if (step == steps) {
            /* Guarantee exact target arrival on the final step */
            pOutPts[step - 1].x = (double)targetX;
            pOutPts[step - 1].y = (double)targetY;
            break;
        }

        double tau = (double)step / (double)steps;
        double s   = Control_MinimumJerk(tau);
        BezierPoint bPt = Control_EvalCubicBezier(p0, p1, p2, p3, s);

        if (cfg.tremorScale > 0.0 && cfg.ouSigma > 0.0) {
            double xiX = Math_BoxMullerNormal();
            double xiY = Math_BoxMullerNormal();
            if (!isfinite(xiX)) xiX = 0.0;
            if (!isfinite(xiY)) xiY = 0.0;

            jx = jx * decay + noiseGain * xiX;
            jy = jy * decay + noiseGain * xiY;

            double remDx = (double)targetX - bPt.x;
            double remDy = (double)targetY - bPt.y;
            double remainingDist = hypot(remDx, remDy);

            double proximity = remainingDist / 20.0;
            if (proximity > 1.0) {
                proximity = 1.0;
            }
            double omega = (1.0 - pow(s, 1.2)) * proximity;
            if (omega < 0.0) {
                omega = 0.0;
            }

            pOutPts[step - 1].x = bPt.x + jx * omega * cfg.tremorScale;
            pOutPts[step - 1].y = bPt.y + jy * omega * cfg.tremorScale;
        } else {
            pOutPts[step - 1] = bPt;
        }
    }

    *pOutCount = steps;
    return S_OK;
}

void Control_ScreenToVirtualDesktop(int screenX, int screenY, LONG* pNormX, LONG* pNormY) {
    if (!pNormX && !pNormY) {
        return;
    }

    int vLeft   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vWidth <= 0) {
        vLeft  = 0;
        vWidth = GetSystemMetrics(SM_CXSCREEN);
    }
    if (vHeight <= 0) {
        vTop    = 0;
        vHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    if (pNormX) {
        if (vWidth <= 1) {
            *pNormX = 0;
        } else {
            double fx = ((double)(screenX - vLeft) * 65535.0) / (double)(vWidth - 1);
            fx = Control_ClampDouble(fx, 0.0, 65535.0);
            *pNormX = (LONG)llround(fx);
        }
    }

    if (pNormY) {
        if (vHeight <= 1) {
            *pNormY = 0;
        } else {
            double fy = ((double)(screenY - vTop) * 65535.0) / (double)(vHeight - 1);
            fy = Control_ClampDouble(fy, 0.0, 65535.0);
            *pNormY = (LONG)llround(fy);
        }
    }
}

/* -------------------------------------------------------------------------
 * Human Emulation Mode Primitives (Mouse via SendInput)
 * ------------------------------------------------------------------------- */

HRESULT Control_EmulateMouseMove(int targetX, int targetY, double speedScale) {
    if (!isfinite(speedScale) || speedScale < 0.0) {
        return E_INVALIDARG;
    }
    if (Control_CheckFailsafeTriggered()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    ControlConfig activeCfg;
    Control_GetActiveConfigInternal(&activeCfg);

    TrajectoryConfig trajCfg = activeCfg.trajectory;
    if (speedScale > 0.0) {
        trajCfg.speedScale = speedScale;
    } else if (trajCfg.speedScale <= 0.0) {
        trajCfg.speedScale = 1.0;
    }
    if (trajCfg.ouTheta <= 0.0) {
        trajCfg.ouTheta = 15.0;
    }
    if (trajCfg.ouSigma <= 0.0) {
        trajCfg.ouSigma = 2.5;
    }
    if (trajCfg.minSteps <= 0) {
        trajCfg.minSteps = 10;
    }

    POINT curPt;
    memset(&curPt, 0, sizeof(curPt));
    SetLastError(0);
    if (!GetCursorPos(&curPt)) {
        DWORD err = GetLastError();
        return HRESULT_FROM_WIN32(err != 0 ? err : ERROR_GEN_FAILURE);
    }

    if (curPt.x == targetX && curPt.y == targetY) {
        return S_OK;
    }

    double dx   = (double)targetX - (double)curPt.x;
    double dy   = (double)targetY - (double)curPt.y;
    double dist = hypot(dx, dy);

    int neededPts = Control_EstimateStepCount(dist, &trajCfg);
    BezierPoint stackPts[CPOWER_STACK_TRAJECTORY_CAP];
    BezierPoint* pPts = stackPts;
    int maxPts = CPOWER_STACK_TRAJECTORY_CAP;

    if (neededPts > CPOWER_STACK_TRAJECTORY_CAP) {
        pPts = (BezierPoint*)malloc((size_t)neededPts * sizeof(BezierPoint));
        if (!pPts) {
            pPts = stackPts;
            maxPts = CPOWER_STACK_TRAJECTORY_CAP;
        } else {
            maxPts = neededPts;
        }
    }

    int ptCount = 0;
    HRESULT hr = Control_GenerateTrajectoryPoints(
        curPt.x,
        curPt.y,
        targetX,
        targetY,
        &trajCfg,
        pPts,
        maxPts,
        &ptCount
    );

    if (SUCCEEDED(hr)) {
        for (int i = 0; i < ptCount; ++i) {
            if (Control_CheckFailsafeTriggered()) {
                hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                break;
            }
            int px = (i == ptCount - 1) ? targetX : (int)llround(pPts[i].x);
            int py = (i == ptCount - 1) ? targetY : (int)llround(pPts[i].y);

            LONG normX = 0;
            LONG normY = 0;
            Control_ScreenToVirtualDesktop(px, py, &normX, &normY);

            hr = Control_SendMouseInput(
                MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                normX,
                normY
            );
            if (FAILED(hr)) {
                break;
            }

            if (i + 1 < ptCount) {
                double medianStep = 8.0 * trajCfg.speedScale;
                double minStep    = (medianStep < 0.5) ? medianStep : 0.5;
                double maxStep    = (medianStep * 4.0 > 5.0) ? (medianStep * 4.0) : 5.0;
                double delayMs    = Control_SampleClampedLogNormal(medianStep, 0.15, minStep, maxStep);
                Timer_AccurateSleep(delayMs);
            }
        }
    }

    if (pPts != stackPts) {
        free(pPts);
    }

    return hr;
}

HRESULT Control_EmulateMouseClick(int targetX, int targetY, MouseButton button) {
    DWORD downFlag = 0;
    DWORD upFlag   = 0;
    HRESULT hr = Control_ButtonToFlags(button, &downFlag, &upFlag);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Control_EmulateMouseMove(targetX, targetY, 0.0);
    if (FAILED(hr)) {
        return hr;
    }

    ControlConfig cfg;
    Control_GetActiveConfigInternal(&cfg);
    double scale = (cfg.trajectory.speedScale > 0.0) ? cfg.trajectory.speedScale : 1.0;

    hr = Control_SendMouseInput(downFlag, 0, 0);
    if (FAILED(hr)) {
        return hr;
    }

    double dwellMs = Control_SampleClampedLogNormal(75.0 * scale, 0.22, 1.0, 250.0 * scale + 10.0);
    Timer_AccurateSleep(dwellMs);

    hr = Control_SendMouseInput(upFlag, 0, 0);
    return hr;
}

HRESULT Control_EmulateMouseDoubleClick(int targetX, int targetY, MouseButton button) {
    DWORD downFlag = 0;
    DWORD upFlag   = 0;
    HRESULT hr = Control_ButtonToFlags(button, &downFlag, &upFlag);
    if (FAILED(hr)) {
        return hr;
    }

    hr = Control_EmulateMouseMove(targetX, targetY, 0.0);
    if (FAILED(hr)) {
        return hr;
    }

    ControlConfig cfg;
    Control_GetActiveConfigInternal(&cfg);
    double scale = (cfg.trajectory.speedScale > 0.0) ? cfg.trajectory.speedScale : 1.0;

    UINT osDblClickMs = GetDoubleClickTime();
    double maxInterMs = (osDblClickMs > 40U) ? ((double)osDblClickMs * 0.75) : 300.0;

    /* First click */
    hr = Control_SendMouseInput(downFlag, 0, 0);
    if (FAILED(hr)) {
        return hr;
    }
    Timer_AccurateSleep(Control_SampleClampedLogNormal(65.0 * scale, 0.22, 1.0, 180.0));

    hr = Control_SendMouseInput(upFlag, 0, 0);
    if (FAILED(hr)) {
        return hr;
    }

    /* Inter-click interval guaranteed within system double-click threshold */
    double interMedian = 110.0 * scale;
    if (interMedian > maxInterMs * 0.7) {
        interMedian = maxInterMs * 0.7;
    }
    double interMs = Control_SampleClampedLogNormal(interMedian, 0.28, 1.0, maxInterMs);
    Timer_AccurateSleep(interMs);

    /* Second click */
    hr = Control_SendMouseInput(downFlag, 0, 0);
    if (FAILED(hr)) {
        return hr;
    }
    Timer_AccurateSleep(Control_SampleClampedLogNormal(65.0 * scale, 0.22, 1.0, 180.0));

    hr = Control_SendMouseInput(upFlag, 0, 0);
    return hr;
}

HRESULT Control_EmulateMouseDrag(int startX, int startY, int endX, int endY, double speedScale) {
    if (!isfinite(speedScale) || speedScale < 0.0) {
        return E_INVALIDARG;
    }

    ControlConfig cfg;
    Control_GetActiveConfigInternal(&cfg);
    double effScale = (speedScale > 0.0) ? speedScale : ((cfg.trajectory.speedScale > 0.0) ? cfg.trajectory.speedScale : 1.0);

    HRESULT hr = Control_EmulateMouseMove(startX, startY, speedScale);
    if (FAILED(hr)) {
        return hr;
    }

    Timer_AccurateSleep(Control_SampleClampedLogNormal(40.0 * effScale, 0.20, 1.0, 150.0 * effScale + 5.0));

    if (Control_CheckFailsafeTriggered()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    hr = Control_SendMouseInput(MOUSEEVENTF_LEFTDOWN, 0, 0);
    if (FAILED(hr)) {
        return hr;
    }

    Timer_AccurateSleep(Control_SampleClampedLogNormal(75.0 * effScale, 0.22, 1.0, 200.0 * effScale + 5.0));

    hr = Control_EmulateMouseMove(endX, endY, speedScale);

    /* Always release left mouse button even if move fails */
    Timer_AccurateSleep(Control_SampleClampedLogNormal(50.0 * effScale, 0.20, 1.0, 150.0 * effScale + 5.0));
    HRESULT hrUp = Control_SendMouseInput(MOUSEEVENTF_LEFTUP, 0, 0);

    return FAILED(hr) ? hr : hrUp;
}
