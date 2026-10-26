#ifndef LCD_PROCESS_HPP
#define LCD_PROCESS_HPP

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <opencv2/core.hpp>
#include "framebuffer.hpp"

struct LcdSharedState;

/**
 * @brief  Publish camera images to a dedicated LCD process through shared memory.
 * @note   The parent owns the shared-memory lifecycle; the child owns framebuffer rendering.
 */
class LcdProcess {
public:
    /**
     * @brief  Create an inactive LCD process controller.
     */
    LcdProcess() = default;

    /**
     * @brief  Stop the child process and release shared-memory resources.
     */
    ~LcdProcess();

    LcdProcess(const LcdProcess&) = delete;
    LcdProcess& operator=(const LcdProcess&) = delete;

    /**
     * @brief  Create shared buffers and fork the framebuffer rendering process.
     * @param  [in,out] display Open framebuffer display inherited by the child.
     * @param  [in] width Camera frame width in pixels.
     * @param  [in] height Camera frame height in pixels.
     * @retval true The LCD child process is running.
     * @retval false Shared-memory, synchronization, or fork initialization failed.
     * @warning Call before starting application threads; the child inherits the display mapping.
     */
    bool start(FramebufferDisplay& display, int width, int height);

    /**
     * @brief  Copy one BGR frame into a shared-memory slot for LCD rendering.
     * @param  [in] bgr Nonempty CV_8UC3 image with the negotiated camera geometry.
     * @param  [in] sequence Camera frame sequence used for diagnostics and ordering.
     * @retval true The frame was copied and published.
     * @retval false The frame was dropped because all slots were in use or input was invalid.
     * @note   Dropping an old READY frame is intentional to bound display latency.
     */
    bool publish(const cv::Mat& bgr, uint32_t sequence);

    /**
     * @brief  Publish the latest performance statistics to the LCD child.
     * @param  [in] stats Current capture, processing, LCD, and JPEG measurements.
     */
    void updateStats(const DisplayStats& stats);

    /**
     * @brief  Consume the LCD child's accumulated render time for the last statistics window.
     * @param  [out] averageMs Average framebuffer render time in milliseconds.
     * @retval true At least one LCD frame was rendered in the window.
     * @retval false No LCD frame was rendered in the window.
     */
    bool takeRenderStats(double& averageMs);

    /**
     * @brief  Check whether the LCD child is still alive.
     * @retval true The child has not exited.
     * @retval false The child exited or this controller is inactive.
     */
    bool healthy();

    /**
     * @brief  Signal the child to stop, wait for it, and release shared memory.
     */
    void stop();

    /**
     * @brief  Report whether the controller currently owns an LCD child.
     * @retval true A child process and shared-memory mapping are active.
     * @retval false The controller is inactive.
     */
    bool active() const;

private:
    static void childLoop(FramebufferDisplay& display, LcdSharedState* shared,
                          size_t slotBytes);
    void releaseMapping();

    LcdSharedState* shared_ = nullptr;
    size_t mappingLength_ = 0;
    size_t slotBytes_ = 0;
    pid_t childPid_ = -1;
    bool active_ = false;
};

#endif
