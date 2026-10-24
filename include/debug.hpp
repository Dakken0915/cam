#ifndef CAM_DEBUG_HPP
#define CAM_DEBUG_HPP

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

/**
 * @brief  Decide whether diagnostic logs should be visible for this process.
 * @retval true The process is foregrounded on a terminal or CAM_DEBUG=1 is set.
 * @retval false The process is backgrounded, redirected, or CAM_DEBUG=0 is set.
 * @note   Normal flow, error, and photo-save logs do not use this gate.
 */
inline bool camDebugEnabled() {
    const char* overrideValue = std::getenv("CAM_DEBUG");
    if (overrideValue && std::strcmp(overrideValue, "1") == 0) return true;
    if (overrideValue && std::strcmp(overrideValue, "0") == 0) return false;
    if (!::isatty(STDERR_FILENO)) return false;
    const pid_t foregroundGroup = ::tcgetpgrp(STDERR_FILENO);
    return foregroundGroup >= 0 && foregroundGroup == ::getpgrp();
}

/**
 * @brief  Emit a diagnostic message only while diagnostic output is enabled.
 * @param  [in] expression Stream expression containing the diagnostic message.
 * @note   The expression is not evaluated when the process is backgrounded.
 */
#define CAM_DEBUG(expression) do { \
    if (camDebugEnabled()) std::cerr << expression << std::endl; \
} while (false)

#endif
