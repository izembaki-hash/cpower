/**
 * @file dxgi_capture.c
 * @brief Lazy D3D11 + DXGI 1.2 micro-region VRAM capture and zero-dependency BMP/PNG encoders.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef COBJMACROS
#define COBJMACROS
#endif

#include "perception.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <limits.h>
#include <string.h>

#ifndef CAPTUREBLT
#define CAPTUREBLT 0x40000000UL
#endif

#ifndef DXGI_ERROR_WAIT_TIMEOUT
#define DXGI_ERROR_WAIT_TIMEOUT ((HRESULT)0x887A0027L)
#endif

#ifndef DXGI_ERROR_ACCESS_LOST
#define DXGI_ERROR_ACCESS_LOST ((HRESULT)0x887A0026L)
#endif

#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002L)
#endif

/* Standalone GUID constants (zero dependency on uuid.lib) */
static const GUID CPOWER_IID_IDXGIFactory1 =
    { 0x770aae78U, 0xf26fU, 0x4dbaU, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID CPOWER_IID_IDXGIOutput1 =
    { 0x00cddea8U, 0x939bU, 0x4b83U, { 0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc } };
static const GUID CPOWER_IID_ID3D11Texture2D =
    { 0x6f15aaf2U, 0xd208U, 0x4e89U, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

typedef struct DxgiCaptureCtx {
    ID3D11Device*           pDevice;
    ID3D11DeviceContext*    pContext;
    IDXGIOutputDuplication* pDuplication;
    ID3D11Texture2D*        pStagingTex;
    int                     stagingW;
    int                     stagingH;
    UINT                    adapterIdx;
    UINT                    outputIdx;
    DXGI_MODE_ROTATION      rotation;
    RECT                    outputRect;
    uint8_t*                pCachedPixels;
    int                     cachedW;
    int                     cachedH;
    int                     cachedStride;
    BOOL                    hasCachedFrame;
} DxgiCaptureCtx;

static INIT_ONCE        g_dxgiCsOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_dxgiCs;
static DxgiCaptureCtx   g_dxgiCtx;
static volatile LONG    g_dxgiEnabled = 1;

static INIT_ONCE        g_crcOnce = INIT_ONCE_STATIC_INIT;
static uint32_t         g_crcTable[256];

static BOOL CALLBACK init_dxgi_cs(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    InitializeCriticalSection(&g_dxgiCs);
    ZeroMemory(&g_dxgiCtx, sizeof(g_dxgiCtx));
    return TRUE;
}

static BOOL CALLBACK init_crc32_table(PINIT_ONCE InitOnce, PVOID Parameter, PVOID *Context) {
    (void)InitOnce;
    (void)Parameter;
    (void)Context;
    for (uint32_t i = 0; i < 256U; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        g_crcTable[i] = c;
    }
    return TRUE;
}

static uint32_t compute_crc32(const uint8_t* data, size_t len) {
    InitOnceExecuteOnce(&g_crcOnce, init_crc32_table, NULL, NULL);
    uint32_t c = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        c = g_crcTable[(c ^ data[i]) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

static void release_dxgi_context_locked(void) {
    CPOWER_SAFE_RELEASE(g_dxgiCtx.pStagingTex);
    CPOWER_SAFE_RELEASE(g_dxgiCtx.pDuplication);
    CPOWER_SAFE_RELEASE(g_dxgiCtx.pContext);
    CPOWER_SAFE_RELEASE(g_dxgiCtx.pDevice);
    CPOWER_SAFE_FREE(g_dxgiCtx.pCachedPixels);
    g_dxgiCtx.stagingW = 0;
    g_dxgiCtx.stagingH = 0;
    g_dxgiCtx.adapterIdx = 0U;
    g_dxgiCtx.outputIdx = 0U;
    g_dxgiCtx.rotation = DXGI_MODE_ROTATION_UNSPECIFIED;
    g_dxgiCtx.cachedW = 0;
    g_dxgiCtx.cachedH = 0;
    g_dxgiCtx.cachedStride = 0;
    g_dxgiCtx.hasCachedFrame = FALSE;
    ZeroMemory(&g_dxgiCtx.outputRect, sizeof(g_dxgiCtx.outputRect));
}

void PerceptionInternal_DxgiSetEnabled(BOOL enabled) {
    InterlockedExchange(&g_dxgiEnabled, enabled ? 1L : 0L);
}

void PerceptionInternal_DxgiShutdown(void) {
    InitOnceExecuteOnce(&g_dxgiCsOnce, init_dxgi_cs, NULL, NULL);
    EnterCriticalSection(&g_dxgiCs);
    release_dxgi_context_locked();
    LeaveCriticalSection(&g_dxgiCs);
}

static HRESULT create_d3d11_device_for_adapter(
    IDXGIAdapter1* pAdapter,
    ID3D11Device** ppDevice,
    ID3D11DeviceContext** ppContext
) {
    static const D3D_FEATURE_LEVEL levels11_1[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    static const D3D_FEATURE_LEVEL levels11_0[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(
        (IDXGIAdapter*)pAdapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels11_1,
        (UINT)(sizeof(levels11_1) / sizeof(levels11_1[0])),
        D3D11_SDK_VERSION,
        ppDevice,
        &chosenLevel,
        ppContext
    );

    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            (IDXGIAdapter*)pAdapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels11_0,
            (UINT)(sizeof(levels11_0) / sizeof(levels11_0[0])),
            D3D11_SDK_VERSION,
            ppDevice,
            &chosenLevel,
            ppContext
        );
    }

    return hr;
}

#define CPOWER_DXGI_MAX_OUTPUTS 16U

typedef struct DxgiOutputMatch {
    UINT               adapterIdx;
    UINT               outputIdx;
    DXGI_MODE_ROTATION rotation;
    RECT               outputRect;
    RECT               overlapRect;
} DxgiOutputMatch;

static BOOL rect_contains_rect(const RECT* outer, const RECT* inner) {
    return (inner->left >= outer->left &&
            inner->top >= outer->top &&
            inner->right <= outer->right &&
            inner->bottom <= outer->bottom) ? TRUE : FALSE;
}

static HRESULT collect_intersecting_outputs_locked(
    const RECT* pTargetRect,
    DxgiOutputMatch* pOutMatches,
    UINT maxMatches,
    UINT* pMatchCount
) {
    *pMatchCount = 0U;

    IDXGIFactory1* pFactory = NULL;
    HRESULT hr = CreateDXGIFactory1(&CPOWER_IID_IDXGIFactory1, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    UINT count = 0U;
    for (UINT a = 0U; ; a++) {
        IDXGIAdapter1* pAdapter = NULL;
        HRESULT hrA = IDXGIFactory1_EnumAdapters1(pFactory, a, &pAdapter);
        if (hrA == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hrA) || !pAdapter) {
            continue;
        }

        for (UINT o = 0U; ; o++) {
            IDXGIOutput* pOutput = NULL;
            HRESULT hrO = IDXGIAdapter1_EnumOutputs(pAdapter, o, &pOutput);
            if (hrO == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hrO) || !pOutput) {
                continue;
            }

            DXGI_OUTPUT_DESC desc;
            ZeroMemory(&desc, sizeof(desc));
            if (SUCCEEDED(IDXGIOutput_GetDesc(pOutput, &desc)) && desc.AttachedToDesktop) {
                RECT overlap;
                if (IntersectRect(&overlap, &desc.DesktopCoordinates, pTargetRect)) {
                    if (count < maxMatches) {
                        pOutMatches[count].adapterIdx = a;
                        pOutMatches[count].outputIdx = o;
                        pOutMatches[count].rotation = desc.Rotation;
                        pOutMatches[count].outputRect = desc.DesktopCoordinates;
                        pOutMatches[count].overlapRect = overlap;
                        count++;
                    }
                }
            }
            IDXGIOutput_Release(pOutput);
        }

        IDXGIAdapter1_Release(pAdapter);
    }

    IDXGIFactory1_Release(pFactory);
    *pMatchCount = count;
    return (count > 0U) ? S_OK : DXGI_ERROR_NOT_FOUND;
}

static HRESULT init_dxgi_for_output_indices_locked(
    UINT adapterIdx,
    UINT outputIdx,
    const RECT* pOutRect,
    DXGI_MODE_ROTATION rotation
) {
    IDXGIFactory1* pFactory = NULL;
    HRESULT hr = CreateDXGIFactory1(&CPOWER_IID_IDXGIFactory1, (void**)&pFactory);
    if (FAILED(hr) || !pFactory) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    IDXGIAdapter1* pAdapter = NULL;
    hr = IDXGIFactory1_EnumAdapters1(pFactory, adapterIdx, &pAdapter);
    IDXGIFactory1_Release(pFactory);
    if (FAILED(hr) || !pAdapter) {
        return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    }

    IDXGIOutput* pOutput = NULL;
    hr = IDXGIAdapter1_EnumOutputs(pAdapter, outputIdx, &pOutput);
    if (FAILED(hr) || !pOutput) {
        IDXGIAdapter1_Release(pAdapter);
        return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    }

    DXGI_OUTPUT_DESC outDesc;
    ZeroMemory(&outDesc, sizeof(outDesc));
    RECT resolvedRect = *pOutRect;
    DXGI_MODE_ROTATION resolvedRot = rotation;
    if (SUCCEEDED(IDXGIOutput_GetDesc(pOutput, &outDesc))) {
        resolvedRect = outDesc.DesktopCoordinates;
        resolvedRot = outDesc.Rotation;
    }

    ID3D11Device* pDevice = NULL;
    ID3D11DeviceContext* pContext = NULL;
    hr = create_d3d11_device_for_adapter(pAdapter, &pDevice, &pContext);
    IDXGIAdapter1_Release(pAdapter);

    if (FAILED(hr) || !pDevice || !pContext) {
        IDXGIOutput_Release(pOutput);
        CPOWER_SAFE_RELEASE(pContext);
        CPOWER_SAFE_RELEASE(pDevice);
        return FAILED(hr) ? hr : E_FAIL;
    }

    IDXGIOutput1* pOutput1 = NULL;
    hr = IDXGIOutput_QueryInterface(pOutput, &CPOWER_IID_IDXGIOutput1, (void**)&pOutput1);
    IDXGIOutput_Release(pOutput);

    if (FAILED(hr) || !pOutput1) {
        ID3D11DeviceContext_Release(pContext);
        ID3D11Device_Release(pDevice);
        return FAILED(hr) ? hr : E_NOINTERFACE;
    }

    IDXGIOutputDuplication* pDuplication = NULL;
    hr = IDXGIOutput1_DuplicateOutput(pOutput1, (IUnknown*)pDevice, &pDuplication);
    IDXGIOutput1_Release(pOutput1);

    if (FAILED(hr) || !pDuplication) {
        ID3D11DeviceContext_Release(pContext);
        ID3D11Device_Release(pDevice);
        return FAILED(hr) ? hr : E_FAIL;
    }

    g_dxgiCtx.pDevice = pDevice;
    g_dxgiCtx.pContext = pContext;
    g_dxgiCtx.pDuplication = pDuplication;
    g_dxgiCtx.adapterIdx = adapterIdx;
    g_dxgiCtx.outputIdx = outputIdx;
    g_dxgiCtx.rotation = resolvedRot;
    g_dxgiCtx.outputRect = resolvedRect;
    g_dxgiCtx.hasCachedFrame = FALSE;
    return S_OK;
}

static HRESULT ensure_staging_texture_locked(int width, int height) {
    if (g_dxgiCtx.pStagingTex &&
        g_dxgiCtx.stagingW == width &&
        g_dxgiCtx.stagingH == height) {
        return S_OK;
    }

    CPOWER_SAFE_RELEASE(g_dxgiCtx.pStagingTex);
    g_dxgiCtx.stagingW = 0;
    g_dxgiCtx.stagingH = 0;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    HRESULT hr = ID3D11Device_CreateTexture2D(
        g_dxgiCtx.pDevice, &desc, NULL, &g_dxgiCtx.pStagingTex
    );
    if (SUCCEEDED(hr) && g_dxgiCtx.pStagingTex) {
        g_dxgiCtx.stagingW = width;
        g_dxgiCtx.stagingH = height;
    }
    return hr;
}

static HRESULT ensure_cached_buffer_locked(int outW, int outH) {
    if (outW <= 0 || outH <= 0 || outW > (INT_MAX / 4)) {
        return E_INVALIDARG;
    }

    int64_t stride64 = (int64_t)outW * 4;
    if ((uint64_t)stride64 > (uint64_t)SIZE_MAX / (uint64_t)outH) {
        return E_OUTOFMEMORY;
    }

    if (g_dxgiCtx.pCachedPixels &&
        g_dxgiCtx.cachedW == outW &&
        g_dxgiCtx.cachedH == outH) {
        return S_OK;
    }

    CPOWER_SAFE_FREE(g_dxgiCtx.pCachedPixels);
    g_dxgiCtx.cachedW = 0;
    g_dxgiCtx.cachedH = 0;
    g_dxgiCtx.cachedStride = 0;
    g_dxgiCtx.hasCachedFrame = FALSE;

    size_t bufBytes = (size_t)stride64 * (size_t)outH;
    uint8_t* pBuf = (uint8_t*)calloc(1, bufBytes);
    if (!pBuf) {
        return E_OUTOFMEMORY;
    }

    g_dxgiCtx.pCachedPixels = pBuf;
    g_dxgiCtx.cachedW = outW;
    g_dxgiCtx.cachedH = outH;
    g_dxgiCtx.cachedStride = (int)stride64;
    return S_OK;
}

static HRESULT update_cache_from_desktop_tex_locked(ID3D11Texture2D* pDesktopTex) {
    int64_t outW64 = (int64_t)g_dxgiCtx.outputRect.right - (int64_t)g_dxgiCtx.outputRect.left;
    int64_t outH64 = (int64_t)g_dxgiCtx.outputRect.bottom - (int64_t)g_dxgiCtx.outputRect.top;
    if (outW64 <= 0 || outH64 <= 0 || outW64 > (int64_t)(INT_MAX / 4) || outH64 > (int64_t)INT_MAX) {
        return E_INVALIDARG;
    }

    int outW = (int)outW64;
    int outH = (int)outH64;

    D3D11_TEXTURE2D_DESC desktopDesc;
    ID3D11Texture2D_GetDesc(pDesktopTex, &desktopDesc);
    if (desktopDesc.Width == 0U || desktopDesc.Height == 0U ||
        desktopDesc.Width > (UINT)INT_MAX || desktopDesc.Height > (UINT)INT_MAX) {
        return E_INVALIDARG;
    }

    HRESULT hr = ensure_staging_texture_locked((int)desktopDesc.Width, (int)desktopDesc.Height);
    if (FAILED(hr)) {
        return hr;
    }

    hr = ensure_cached_buffer_locked(outW, outH);
    if (FAILED(hr)) {
        return hr;
    }

    ID3D11DeviceContext_CopyResource(
        g_dxgiCtx.pContext,
        (ID3D11Resource*)g_dxgiCtx.pStagingTex,
        (ID3D11Resource*)pDesktopTex
    );

    D3D11_MAPPED_SUBRESOURCE mapped;
    ZeroMemory(&mapped, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(
        g_dxgiCtx.pContext,
        (ID3D11Resource*)g_dxgiCtx.pStagingTex,
        0,
        D3D11_MAP_READ,
        0,
        &mapped
    );
    if (FAILED(hr)) {
        return hr;
    }

    const uint8_t* srcBase = (const uint8_t*)mapped.pData;
    UINT texW = desktopDesc.Width;
    UINT texH = desktopDesc.Height;
    size_t dstStride = (size_t)g_dxgiCtx.cachedStride;

    switch (g_dxgiCtx.rotation) {
        case DXGI_MODE_ROTATION_ROTATE90:
            for (int dy = 0; dy < outH; dy++) {
                UINT tx = (UINT)dy;
                uint8_t* dRow = g_dxgiCtx.pCachedPixels + (size_t)dy * dstStride;
                if (tx >= texW) {
                    continue;
                }
                for (int dx = 0; dx < outW; dx++) {
                    UINT ty = (UINT)(outW - 1 - dx);
                    if (ty >= texH) {
                        continue;
                    }
                    const uint8_t* sPx = srcBase + (size_t)ty * mapped.RowPitch + ((size_t)tx * 4U);
                    uint8_t* dPx = dRow + ((size_t)dx * 4U);
                    dPx[0] = sPx[0];
                    dPx[1] = sPx[1];
                    dPx[2] = sPx[2];
                    dPx[3] = sPx[3];
                }
            }
            break;

        case DXGI_MODE_ROTATION_ROTATE180:
            for (int dy = 0; dy < outH; dy++) {
                UINT ty = (UINT)(outH - 1 - dy);
                uint8_t* dRow = g_dxgiCtx.pCachedPixels + (size_t)dy * dstStride;
                if (ty >= texH) {
                    continue;
                }
                const uint8_t* sRow = srcBase + (size_t)ty * mapped.RowPitch;
                for (int dx = 0; dx < outW; dx++) {
                    UINT tx = (UINT)(outW - 1 - dx);
                    if (tx >= texW) {
                        continue;
                    }
                    const uint8_t* sPx = sRow + ((size_t)tx * 4U);
                    uint8_t* dPx = dRow + ((size_t)dx * 4U);
                    dPx[0] = sPx[0];
                    dPx[1] = sPx[1];
                    dPx[2] = sPx[2];
                    dPx[3] = sPx[3];
                }
            }
            break;

        case DXGI_MODE_ROTATION_ROTATE270:
            for (int dy = 0; dy < outH; dy++) {
                UINT tx = (UINT)(outH - 1 - dy);
                uint8_t* dRow = g_dxgiCtx.pCachedPixels + (size_t)dy * dstStride;
                if (tx >= texW) {
                    continue;
                }
                for (int dx = 0; dx < outW; dx++) {
                    UINT ty = (UINT)dx;
                    if (ty >= texH) {
                        continue;
                    }
                    const uint8_t* sPx = srcBase + (size_t)ty * mapped.RowPitch + ((size_t)tx * 4U);
                    uint8_t* dPx = dRow + ((size_t)dx * 4U);
                    dPx[0] = sPx[0];
                    dPx[1] = sPx[1];
                    dPx[2] = sPx[2];
                    dPx[3] = sPx[3];
                }
            }
            break;

        case DXGI_MODE_ROTATION_IDENTITY:
        case DXGI_MODE_ROTATION_UNSPECIFIED:
        default: {
            int copyW = (outW < (int)texW) ? outW : (int)texW;
            int copyH = (outH < (int)texH) ? outH : (int)texH;
            size_t copyRowBytes = (size_t)copyW * 4U;
            for (int dy = 0; dy < copyH; dy++) {
                const uint8_t* sRow = srcBase + (size_t)dy * mapped.RowPitch;
                uint8_t* dRow = g_dxgiCtx.pCachedPixels + (size_t)dy * dstStride;
                memcpy(dRow, sRow, copyRowBytes);
            }
            break;
        }
    }

    ID3D11DeviceContext_Unmap(g_dxgiCtx.pContext, (ID3D11Resource*)g_dxgiCtx.pStagingTex, 0);
    g_dxgiCtx.hasCachedFrame = TRUE;
    return S_OK;
}

static HRESULT capture_output_gdi_fallback_locked(void) {
    int64_t outW64 = (int64_t)g_dxgiCtx.outputRect.right - (int64_t)g_dxgiCtx.outputRect.left;
    int64_t outH64 = (int64_t)g_dxgiCtx.outputRect.bottom - (int64_t)g_dxgiCtx.outputRect.top;
    if (outW64 <= 0 || outH64 <= 0 || outW64 > (int64_t)(INT_MAX / 4) || outH64 > (int64_t)INT_MAX) {
        return E_INVALIDARG;
    }

    int outW = (int)outW64;
    int outH = (int)outH64;
    HRESULT hr = ensure_cached_buffer_locked(outW, outH);
    if (FAILED(hr)) {
        return hr;
    }

    HDC hScreenDC = GetDC(NULL);
    if (!hScreenDC) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    if (!hMemDC) {
        DWORD err = GetLastError();
        ReleaseDC(NULL, hScreenDC);
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = outW;
    bmi.bmiHeader.biHeight = -outH; /* Top-down DIB */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hBmp = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    if (!hBmp || !pBits) {
        DWORD err = GetLastError();
        if (hBmp) {
            DeleteObject(hBmp);
        }
        DeleteDC(hMemDC);
        ReleaseDC(NULL, hScreenDC);
        return err ? HRESULT_FROM_WIN32(err) : E_OUTOFMEMORY;
    }

    HGDIOBJ hOld = SelectObject(hMemDC, hBmp);
    BOOL bltOk = BitBlt(
        hMemDC,
        0,
        0,
        outW,
        outH,
        hScreenDC,
        g_dxgiCtx.outputRect.left,
        g_dxgiCtx.outputRect.top,
        SRCCOPY | CAPTUREBLT
    );

    if (bltOk) {
        const uint8_t* src = (const uint8_t*)pBits;
        size_t totalPixels = (size_t)outW * (size_t)outH;
        uint8_t* dst = g_dxgiCtx.pCachedPixels;
        for (size_t p = 0U; p < totalPixels; p++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 0xFFU;
            src += 4;
            dst += 4;
        }
        g_dxgiCtx.hasCachedFrame = TRUE;
        hr = S_OK;
    } else {
        DWORD err = GetLastError();
        hr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    SelectObject(hMemDC, hOld);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hScreenDC);
    return hr;
}

static HRESULT crop_from_cached_frame_locked(const RECT* pOverlap, ImageBuffer* pImageOut) {
    if (!g_dxgiCtx.hasCachedFrame || !g_dxgiCtx.pCachedPixels) {
        return E_FAIL;
    }

    int64_t srcX64 = (int64_t)pOverlap->left - (int64_t)g_dxgiCtx.outputRect.left;
    int64_t srcY64 = (int64_t)pOverlap->top - (int64_t)g_dxgiCtx.outputRect.top;
    int64_t srcR64 = (int64_t)pOverlap->right - (int64_t)g_dxgiCtx.outputRect.left;
    int64_t srcB64 = (int64_t)pOverlap->bottom - (int64_t)g_dxgiCtx.outputRect.top;

    if (srcX64 < 0) srcX64 = 0;
    if (srcY64 < 0) srcY64 = 0;
    if (srcR64 > (int64_t)g_dxgiCtx.cachedW) srcR64 = (int64_t)g_dxgiCtx.cachedW;
    if (srcB64 > (int64_t)g_dxgiCtx.cachedH) srcB64 = (int64_t)g_dxgiCtx.cachedH;

    int64_t cropW64 = srcR64 - srcX64;
    int64_t cropH64 = srcB64 - srcY64;
    if (cropW64 <= 0 || cropH64 <= 0 || cropW64 > (int64_t)(INT_MAX / 4) || cropH64 > (int64_t)INT_MAX) {
        return E_INVALIDARG;
    }

    int64_t rowBytes64 = cropW64 * 4;
    if ((uint64_t)rowBytes64 > (uint64_t)SIZE_MAX / (uint64_t)cropH64) {
        return E_OUTOFMEMORY;
    }

    int cropWidth = (int)cropW64;
    int cropHeight = (int)cropH64;
    int rowBytes = (int)rowBytes64;
    size_t dataSize = (size_t)rowBytes64 * (size_t)cropH64;

    uint8_t* pixels = (uint8_t*)malloc(dataSize);
    if (!pixels) {
        return E_OUTOFMEMORY;
    }

    int srcX = (int)srcX64;
    int srcY = (int)srcY64;
    const uint8_t* srcRow = g_dxgiCtx.pCachedPixels +
                            ((size_t)srcY * (size_t)g_dxgiCtx.cachedStride) +
                            ((size_t)srcX * 4U);
    uint8_t* dstRow = pixels;
    for (int y = 0; y < cropHeight; y++) {
        memcpy(dstRow, srcRow, (size_t)rowBytes);
        srcRow += g_dxgiCtx.cachedStride;
        dstRow += rowBytes;
    }

    pImageOut->width = cropWidth;
    pImageOut->height = cropHeight;
    pImageOut->stride = rowBytes;
    pImageOut->dataSize = dataSize;
    pImageOut->pixels = pixels;
    return S_OK;
}

static HRESULT capture_frame_locked(const RECT* pReqRect, ImageBuffer* pImageOut) {
    RECT overlap;
    if (!IntersectRect(&overlap, pReqRect, &g_dxgiCtx.outputRect)) {
        return E_INVALIDARG;
    }

    int64_t cropW64 = (int64_t)overlap.right - (int64_t)overlap.left;
    int64_t cropH64 = (int64_t)overlap.bottom - (int64_t)overlap.top;
    if (cropW64 <= 0 || cropH64 <= 0) {
        return E_INVALIDARG;
    }

    IDXGIResource* pDesktopResource = NULL;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ZeroMemory(&frameInfo, sizeof(frameInfo));
    HRESULT hr = S_OK;

    if (!g_dxgiCtx.hasCachedFrame) {
        for (int attempt = 0; attempt < 2; attempt++) {
            pDesktopResource = NULL;
            ZeroMemory(&frameInfo, sizeof(frameInfo));
            hr = IDXGIOutputDuplication_AcquireNextFrame(
                g_dxgiCtx.pDuplication,
                250U,
                &frameInfo,
                &pDesktopResource
            );
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                break;
            }
            if (FAILED(hr)) {
                return hr;
            }
            if (frameInfo.LastPresentTime.QuadPart == 0 &&
                frameInfo.AccumulatedFrames == 0) {
                CPOWER_SAFE_RELEASE(pDesktopResource);
                IDXGIOutputDuplication_ReleaseFrame(g_dxgiCtx.pDuplication);
                if (attempt == 0) {
                    continue;
                }
                break;
            }
            break;
        }
    } else {
        hr = IDXGIOutputDuplication_AcquireNextFrame(
            g_dxgiCtx.pDuplication,
            16U,
            &frameInfo,
            &pDesktopResource
        );
        if (FAILED(hr) && hr != DXGI_ERROR_WAIT_TIMEOUT) {
            return hr;
        }
    }

    if (SUCCEEDED(hr) && pDesktopResource != NULL) {
        if (frameInfo.LastPresentTime.QuadPart != 0 || frameInfo.AccumulatedFrames > 0) {
            ID3D11Texture2D* pDesktopTex = NULL;
            HRESULT hrQi = IDXGIResource_QueryInterface(
                pDesktopResource, &CPOWER_IID_ID3D11Texture2D, (void**)&pDesktopTex
            );
            IDXGIResource_Release(pDesktopResource);
            pDesktopResource = NULL;

            if (SUCCEEDED(hrQi) && pDesktopTex != NULL) {
                HRESULT hrCache = update_cache_from_desktop_tex_locked(pDesktopTex);
                ID3D11Texture2D_Release(pDesktopTex);
                IDXGIOutputDuplication_ReleaseFrame(g_dxgiCtx.pDuplication);
                if (FAILED(hrCache)) {
                    return hrCache;
                }
            } else {
                IDXGIOutputDuplication_ReleaseFrame(g_dxgiCtx.pDuplication);
                if (!g_dxgiCtx.hasCachedFrame) {
                    return FAILED(hrQi) ? hrQi : E_NOINTERFACE;
                }
            }
        } else {
            IDXGIResource_Release(pDesktopResource);
            pDesktopResource = NULL;
            IDXGIOutputDuplication_ReleaseFrame(g_dxgiCtx.pDuplication);
        }
    } else if (SUCCEEDED(hr)) {
        IDXGIOutputDuplication_ReleaseFrame(g_dxgiCtx.pDuplication);
    }

    if (!g_dxgiCtx.hasCachedFrame) {
        HRESULT hrGdi = capture_output_gdi_fallback_locked();
        if (FAILED(hrGdi)) {
            return (hr == DXGI_ERROR_WAIT_TIMEOUT) ? hr : hrGdi;
        }
    }

    return crop_from_cached_frame_locked(&overlap, pImageOut);
}

static HRESULT capture_stitched_outputs_locked(
    const RECT* pReqRect,
    const DxgiOutputMatch* pMatches,
    UINT matchCount,
    ImageBuffer* pImageOut
) {
    (void)pReqRect;
    if (matchCount == 0U) {
        return DXGI_ERROR_NOT_FOUND;
    }

    /* Clamp canvas to union of matched output overlap rectangles before signed subtraction */
    RECT clampedRect = pMatches[0].overlapRect;
    for (UINT i = 1U; i < matchCount; i++) {
        if (pMatches[i].overlapRect.left < clampedRect.left) {
            clampedRect.left = pMatches[i].overlapRect.left;
        }
        if (pMatches[i].overlapRect.top < clampedRect.top) {
            clampedRect.top = pMatches[i].overlapRect.top;
        }
        if (pMatches[i].overlapRect.right > clampedRect.right) {
            clampedRect.right = pMatches[i].overlapRect.right;
        }
        if (pMatches[i].overlapRect.bottom > clampedRect.bottom) {
            clampedRect.bottom = pMatches[i].overlapRect.bottom;
        }
    }

    int64_t totalW64 = (int64_t)clampedRect.right - (int64_t)clampedRect.left;
    int64_t totalH64 = (int64_t)clampedRect.bottom - (int64_t)clampedRect.top;
    if (totalW64 <= 0 || totalH64 <= 0 ||
        totalW64 > (int64_t)(INT_MAX / 4) || totalH64 > (int64_t)INT_MAX) {
        return E_INVALIDARG;
    }

    int64_t fullStride64 = totalW64 * 4;
    if ((uint64_t)fullStride64 > (uint64_t)SIZE_MAX / (uint64_t)totalH64) {
        return E_OUTOFMEMORY;
    }

    int totalW = (int)totalW64;
    int totalH = (int)totalH64;
    int fullStride = (int)fullStride64;
    size_t fullBytes = (size_t)fullStride64 * (size_t)totalH64;
    uint8_t* fullPixels = (uint8_t*)calloc(1, fullBytes);
    if (!fullPixels) {
        return E_OUTOFMEMORY;
    }

    UINT succeededTiles = 0U;
    HRESULT firstError = E_FAIL;

    for (UINT i = 0U; i < matchCount; i++) {
        release_dxgi_context_locked();
        HRESULT hrTile = init_dxgi_for_output_indices_locked(
            pMatches[i].adapterIdx,
            pMatches[i].outputIdx,
            &pMatches[i].outputRect,
            pMatches[i].rotation
        );
        if (FAILED(hrTile)) {
            if (succeededTiles == 0U && firstError == E_FAIL) {
                firstError = hrTile;
            }
            continue;
        }

        ImageBuffer tile;
        ZeroMemory(&tile, sizeof(tile));
        hrTile = capture_frame_locked(&pMatches[i].overlapRect, &tile);
        if (hrTile == DXGI_ERROR_ACCESS_LOST) {
            release_dxgi_context_locked();
            hrTile = init_dxgi_for_output_indices_locked(
                pMatches[i].adapterIdx,
                pMatches[i].outputIdx,
                &pMatches[i].outputRect,
                pMatches[i].rotation
            );
            if (SUCCEEDED(hrTile)) {
                hrTile = capture_frame_locked(&pMatches[i].overlapRect, &tile);
            }
        }

        if (SUCCEEDED(hrTile) && tile.pixels) {
            int64_t dstX64 = (int64_t)pMatches[i].overlapRect.left - (int64_t)clampedRect.left;
            int64_t dstY64 = (int64_t)pMatches[i].overlapRect.top - (int64_t)clampedRect.top;
            if (dstX64 >= 0 && dstY64 >= 0 && dstX64 < totalW64 && dstY64 < totalH64) {
                int dstX = (int)dstX64;
                int dstY = (int)dstY64;
                int copyW = tile.width;
                int copyH = tile.height;
                if (copyW > totalW - dstX) copyW = totalW - dstX;
                if (copyH > totalH - dstY) copyH = totalH - dstY;

                if (copyW > 0 && copyH > 0) {
                    size_t copyRowBytes = (size_t)copyW * 4U;
                    for (int row = 0; row < copyH; row++) {
                        const uint8_t* sRow = tile.pixels + (size_t)row * (size_t)tile.stride;
                        uint8_t* dRow = fullPixels + ((size_t)(dstY + row) * (size_t)fullStride) + ((size_t)dstX * 4U);
                        memcpy(dRow, sRow, copyRowBytes);
                    }
                }
            }
            succeededTiles++;
        } else if (succeededTiles == 0U && firstError == E_FAIL) {
            firstError = hrTile;
        }

        Perception_FreeImageBuffer(&tile);
    }

    release_dxgi_context_locked();

    if (succeededTiles == 0U) {
        free(fullPixels);
        return firstError;
    }

    pImageOut->width = totalW;
    pImageOut->height = totalH;
    pImageOut->stride = fullStride;
    pImageOut->dataSize = fullBytes;
    pImageOut->pixels = fullPixels;
    return S_OK;
}

HRESULT Perception_CaptureMicroRegion(int left, int top, int right, int bottom, ImageBuffer* pImageOut) {
    if (!pImageOut) {
        return E_POINTER;
    }
    ZeroMemory(pImageOut, sizeof(*pImageOut));

    if (right <= left || bottom <= top) {
        return E_INVALIDARG;
    }

    if (InterlockedCompareExchange(&g_dxgiEnabled, 0, 0) == 0) {
        return E_ACCESSDENIED;
    }

    RECT reqRect;
    reqRect.left = left;
    reqRect.top = top;
    reqRect.right = right;
    reqRect.bottom = bottom;

    InitOnceExecuteOnce(&g_dxgiCsOnce, init_dxgi_cs, NULL, NULL);
    EnterCriticalSection(&g_dxgiCs);

    /* Fast path: cached single output completely covers requested region */
    if (g_dxgiCtx.pDuplication && rect_contains_rect(&g_dxgiCtx.outputRect, &reqRect)) {
        HRESULT hrFast = capture_frame_locked(&reqRect, pImageOut);
        if (hrFast != DXGI_ERROR_ACCESS_LOST) {
            LeaveCriticalSection(&g_dxgiCs);
            return hrFast;
        }
        release_dxgi_context_locked();
    }

    DxgiOutputMatch matches[CPOWER_DXGI_MAX_OUTPUTS];
    UINT matchCount = 0U;
    HRESULT hrEnum = collect_intersecting_outputs_locked(&reqRect, matches, CPOWER_DXGI_MAX_OUTPUTS, &matchCount);
    if (FAILED(hrEnum) || matchCount == 0U) {
        LeaveCriticalSection(&g_dxgiCs);
        return FAILED(hrEnum) ? hrEnum : DXGI_ERROR_NOT_FOUND;
    }

    HRESULT hr = E_FAIL;
    if (matchCount == 1U) {
        if (!g_dxgiCtx.pDuplication ||
            g_dxgiCtx.adapterIdx != matches[0].adapterIdx ||
            g_dxgiCtx.outputIdx != matches[0].outputIdx ||
            g_dxgiCtx.rotation != matches[0].rotation ||
            !EqualRect(&g_dxgiCtx.outputRect, &matches[0].outputRect)) {
            release_dxgi_context_locked();
            hr = init_dxgi_for_output_indices_locked(
                matches[0].adapterIdx,
                matches[0].outputIdx,
                &matches[0].outputRect,
                matches[0].rotation
            );
            if (FAILED(hr)) {
                release_dxgi_context_locked();
                LeaveCriticalSection(&g_dxgiCs);
                return hr;
            }
        }

        hr = capture_frame_locked(&reqRect, pImageOut);
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            release_dxgi_context_locked();
            hr = init_dxgi_for_output_indices_locked(
                matches[0].adapterIdx,
                matches[0].outputIdx,
                &matches[0].outputRect,
                matches[0].rotation
            );
            if (SUCCEEDED(hr)) {
                hr = capture_frame_locked(&reqRect, pImageOut);
            } else {
                release_dxgi_context_locked();
            }
        }
    } else {
        hr = capture_stitched_outputs_locked(&reqRect, matches, matchCount, pImageOut);
    }

    LeaveCriticalSection(&g_dxgiCs);
    return hr;
}

void Perception_FreeImageBuffer(ImageBuffer* pImage) {
    if (!pImage) {
        return;
    }
    CPOWER_SAFE_FREE(pImage->pixels);
    pImage->width = 0;
    pImage->height = 0;
    pImage->stride = 0;
    pImage->dataSize = 0;
}

#pragma pack(push, 1)
typedef struct CPowerBitmapHeader32 {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;

    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} CPowerBitmapHeader32;
#pragma pack(pop)

_Static_assert(sizeof(CPowerBitmapHeader32) == 54, "BMP header must be exactly 54 bytes");

HRESULT Perception_EncodeBmpInMemory(const ImageBuffer* pImage, uint8_t* pOutBmp, size_t maxBytes, size_t* pBytesWritten) {
    if (!pImage) {
        return E_POINTER;
    }
    if (!pImage->pixels || pImage->width <= 0 || pImage->height <= 0) {
        return E_INVALIDARG;
    }

    if (pImage->width > 0x0FFFFFFF || pImage->height <= 0) {
        return E_INVALIDARG;
    }

    uint64_t rowBytes64 = (uint64_t)pImage->width * 4ULL;
    uint64_t srcStride64 = (pImage->stride > 0) ? (uint64_t)pImage->stride : rowBytes64;
    if (srcStride64 < rowBytes64) {
        return E_INVALIDARG;
    }

    if ((uint64_t)pImage->height > (0xFFFFFFFFULL - (uint64_t)sizeof(CPowerBitmapHeader32)) / rowBytes64) {
        return E_INVALIDARG;
    }

    size_t rowBytes = (size_t)rowBytes64;
    size_t srcStride = (size_t)srcStride64;
    size_t pixelBytes = (size_t)(rowBytes64 * (uint64_t)pImage->height);
    size_t totalBytes = sizeof(CPowerBitmapHeader32) + pixelBytes;

    if (pBytesWritten) {
        *pBytesWritten = totalBytes;
    }
    if (!pOutBmp && maxBytes == 0) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (!pOutBmp) {
        return E_POINTER;
    }
    if (maxBytes < totalBytes) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    CPowerBitmapHeader32 hdr;
    ZeroMemory(&hdr, sizeof(hdr));
    hdr.bfType = 0x4D42U; /* "BM" */
    hdr.bfSize = (uint32_t)totalBytes;
    hdr.bfOffBits = sizeof(CPowerBitmapHeader32);

    hdr.biSize = 40U;
    hdr.biWidth = (int32_t)pImage->width;
    hdr.biHeight = -(int32_t)pImage->height; /* Top-down DIB matching DXGI BGRA */
    hdr.biPlanes = 1U;
    hdr.biBitCount = 32U;
    hdr.biCompression = 0U; /* BI_RGB */
    hdr.biSizeImage = (uint32_t)pixelBytes;
    hdr.biXPelsPerMeter = 3780;
    hdr.biYPelsPerMeter = 3780;

    memcpy(pOutBmp, &hdr, sizeof(hdr));
    uint8_t* dst = pOutBmp + sizeof(hdr);
    const uint8_t* src = pImage->pixels;
    for (int y = 0; y < pImage->height; y++) {
        memcpy(dst, src, rowBytes);
        src += srcStride;
        dst += rowBytes;
    }

    return S_OK;
}

static void write_be32(uint8_t* p, uint32_t val) {
    p[0] = (uint8_t)((val >> 24) & 0xFFU);
    p[1] = (uint8_t)((val >> 16) & 0xFFU);
    p[2] = (uint8_t)((val >> 8) & 0xFFU);
    p[3] = (uint8_t)(val & 0xFFU);
}

HRESULT Perception_EncodePngInMemory(const ImageBuffer* pImage, uint8_t* pOutPng, size_t maxBytes, size_t* pBytesWritten) {
    if (!pImage) {
        return E_POINTER;
    }
    if (!pImage->pixels || pImage->width <= 0 || pImage->height <= 0) {
        return E_INVALIDARG;
    }

    if (pImage->width > 0x0FFFFFFF || pImage->height <= 0) {
        return E_INVALIDARG;
    }

    uint64_t rowBytes64 = (uint64_t)pImage->width * 4ULL;
    uint64_t srcStride64 = (pImage->stride > 0) ? (uint64_t)pImage->stride : rowBytes64;
    if (srcStride64 < rowBytes64) {
        return E_INVALIDARG;
    }

    uint64_t rawRowBytes64 = 1ULL + rowBytes64;
    if ((uint64_t)pImage->height > 0xFFFFFFFFULL / rawRowBytes64) {
        return E_INVALIDARG;
    }
    uint64_t rawDataSize64 = (uint64_t)pImage->height * rawRowBytes64;
    uint64_t numBlocks64 = (rawDataSize64 + 65534ULL) / 65535ULL;
    if (numBlocks64 == 0ULL) {
        numBlocks64 = 1ULL;
    }

    /* Zlib header (2) + stored DEFLATE blocks (5 * numBlocks + rawDataSize) + Adler-32 (4) */
    uint64_t idatPayloadLen64 = 2ULL + (numBlocks64 * 5ULL) + rawDataSize64 + 4ULL;
    /* Signature (8) + IHDR chunk (25) + IDAT chunk (12 + payload) + IEND chunk (12) */
    uint64_t totalPngSize64 = 8ULL + 25ULL + 12ULL + idatPayloadLen64 + 12ULL;
    if (idatPayloadLen64 > 0xFFFFFFFFULL || totalPngSize64 > 0xFFFFFFFFULL) {
        return E_INVALIDARG;
    }

    size_t srcStride = (size_t)srcStride64;
    size_t rawDataSize = (size_t)rawDataSize64;
    size_t idatPayloadLen = (size_t)idatPayloadLen64;
    size_t totalPngSize = (size_t)totalPngSize64;

    if (pBytesWritten) {
        *pBytesWritten = totalPngSize;
    }
    if (!pOutPng && maxBytes == 0) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    if (!pOutPng) {
        return E_POINTER;
    }
    if (maxBytes < totalPngSize) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }

    uint8_t* p = pOutPng;

    /* 1. PNG Signature */
    static const uint8_t pngSig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(p, pngSig, 8);
    p += 8;

    /* 2. IHDR Chunk */
    write_be32(p, 13U);
    memcpy(p + 4, "IHDR", 4);
    write_be32(p + 8, (uint32_t)pImage->width);
    write_be32(p + 12, (uint32_t)pImage->height);
    p[16] = 8; /* Bit depth */
    p[17] = 6; /* Color type: RGBA */
    p[18] = 0; /* Compression method */
    p[19] = 0; /* Filter method */
    p[20] = 0; /* Interlace: none */
    write_be32(p + 21, compute_crc32(p + 4, 17U));
    p += 25;

    /* 3. IDAT Chunk */
    write_be32(p, (uint32_t)idatPayloadLen);
    uint8_t* idatTypeAndData = p + 4;
    memcpy(idatTypeAndData, "IDAT", 4);
    p += 8;

    /* RFC 1950 zlib header: CMF=0x78, FLG=0x01 */
    *p++ = 0x78U;
    *p++ = 0x01U;

    uint32_t adlerS1 = 1U;
    uint32_t adlerS2 = 0U;

    const uint8_t* srcRow = pImage->pixels;
    int curX = 0;
    int inRowPos = 0; /* 0 = PNG filter byte (0x00), >0 = RGBA byte index within row */
    size_t bytesLeft = rawDataSize;

    while (bytesLeft > 0U) {
        uint16_t blockLen = (bytesLeft > 65535U) ? 65535U : (uint16_t)bytesLeft;
        uint8_t bfinal = (bytesLeft == (size_t)blockLen) ? 1U : 0U;
        uint16_t nlen = (uint16_t)(~blockLen);

        *p++ = bfinal;
        *p++ = (uint8_t)(blockLen & 0xFFU);
        *p++ = (uint8_t)((blockLen >> 8) & 0xFFU);
        *p++ = (uint8_t)(nlen & 0xFFU);
        *p++ = (uint8_t)((nlen >> 8) & 0xFFU);

        for (uint16_t b = 0; b < blockLen; b++) {
            uint8_t outByte;
            if (inRowPos == 0) {
                outByte = 0U; /* Filter type 0: None */
                inRowPos = 1;
            } else {
                int pixelByte = inRowPos - 1;
                int channel = pixelByte & 3;
                const uint8_t* px = srcRow + ((size_t)curX * 4U);
                if (channel == 0) {
                    outByte = px[2]; /* R */
                } else if (channel == 1) {
                    outByte = px[1]; /* G */
                } else if (channel == 2) {
                    outByte = px[0]; /* B */
                } else {
                    outByte = px[3]; /* A */
                    curX++;
                }

                inRowPos++;
                if (curX >= pImage->width) {
                    curX = 0;
                    inRowPos = 0;
                    srcRow += srcStride;
                }
            }

            *p++ = outByte;
            adlerS1 = (adlerS1 + outByte) % 65521U;
            adlerS2 = (adlerS2 + adlerS1) % 65521U;
        }

        bytesLeft -= (size_t)blockLen;
    }

    write_be32(p, (adlerS2 << 16) | adlerS1);
    p += 4;

    write_be32(p, compute_crc32(idatTypeAndData, idatPayloadLen + 4U));
    p += 4;

    /* 4. IEND Chunk */
    write_be32(p, 0U);
    memcpy(p + 4, "IEND", 4);
    write_be32(p + 8, compute_crc32(p + 4, 4U));

    return S_OK;
}
