/**
 * @file cpower.h
 * @brief Master header, version definitions, and unified error codes for cpower.
 *
 * Windows-Native Pure C Computer-Use Agent Foundation.
 * Zero third-party dependencies; strictly native Win32/COM/DXGI.
 */

#ifndef CPOWER_H
#define CPOWER_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Version Information
 * ------------------------------------------------------------------------- */
#define CPOWER_VERSION_MAJOR 1
#define CPOWER_VERSION_MINOR 1
#define CPOWER_VERSION_PATCH 0
#define CPOWER_VERSION_STRING "1.1.0"

/* -------------------------------------------------------------------------
 * Unified Status and Error Codes
 * ------------------------------------------------------------------------- */
typedef enum cpower_status_t {
    CPOWER_OK                       = 0,   /**< Operation completed successfully */
    CPOWER_ERR_INVALID_ARG          = -1,  /**< Invalid argument or null pointer supplied */
    CPOWER_ERR_OUT_OF_MEMORY        = -2,  /**< Memory allocation failed */
    CPOWER_ERR_NOT_INITIALIZED      = -3,  /**< Subsystem or component not initialized */
    CPOWER_ERR_ALREADY_INITIALIZED  = -4,  /**< Component has already been initialized */
    CPOWER_ERR_COM_FAILURE          = -5,  /**< COM runtime or CoCreateInstance failure */
    CPOWER_ERR_ELEMENT_NOT_FOUND    = -6,  /**< UI Automation element could not be found */
    CPOWER_ERR_PATTERN_NOT_SUPPORTED= -7,  /**< Target element does not support requested pattern */
    CPOWER_ERR_TIMEOUT              = -8,  /**< Operation exceeded maximum allowable time */
    CPOWER_ERR_BUFFER_TOO_SMALL     = -9,  /**< Destination buffer capacity is insufficient */
    CPOWER_ERR_IPC_FAILURE          = -10, /**< Shared memory or Named Pipe transaction failed */
    CPOWER_ERR_SECURITY_DENIED      = -11, /**< Access denied by UIPI or missing uiAccess privilege */
    CPOWER_ERR_DXGI_FAILURE         = -12, /**< Desktop duplication or D3D11 capture failed */
    CPOWER_ERR_NOT_SUPPORTED        = -13, /**< Feature or mode is not supported on this platform */
    CPOWER_ERR_SYSTEM_FAILURE       = -14  /**< General Win32 OS API failure */
} cpower_status_t;

/* -------------------------------------------------------------------------
 * Master Engine Lifecycle & Diagnostics API
 * ------------------------------------------------------------------------- */

/**
 * @brief Initializes the unified cpower engine, COM MTA apartment, and timers.
 * @return CPOWER_OK on success, or an error code.
 */
cpower_status_t CPower_Init(void);

/**
 * @brief Shuts down all active cpower subsystems and releases global resources.
 */
void CPower_Shutdown(void);

/**
 * @brief Retrieves human-readable string representation of a cpower_status_t.
 * @param status The status code to convert.
 * @return Constant string describing the status.
 */
const char* CPower_GetErrorString(cpower_status_t status);

/**
 * @brief Retrieves version string of the cpower engine.
 * @return Semantic version string (e.g., "1.0.0").
 */
const char* CPower_GetVersion(void);

#ifdef __cplusplus
}
#endif

#endif /* CPOWER_H */
