#ifndef LCD_PROCESS_HPP
#define LCD_PROCESS_HPP

#include <cstdint>
#include <cstddef>
#include <opencv2/core.hpp>
#include "framebuffer.hpp"

class LcdProcess {
public:
    /**
     * @brief  Create an inactive LCD process controller.
     */
    LcdProcess() = default;
    /**
     * @brief  Stop the LCD child and release shared synchronization resources.
     */
    ~LcdProcess();
    LcdProcess(const LcdProcess&) = delete;
    LcdProcess& operator=(const LcdProcess&) = delete;

    /**
     * @brief  Create shared BGR slots and fork the LCD rendering process.
     * @param  [in,out] display Open framebuffer inherited by the child process.
     * @param  [in] width Negotiated source image width.
     * @param  [in] height Negotiated source image height.
     * @retval true The shared state and LCD child were started.
     * @retval false Shared memory, synchronization objects, or process creation failed.
     */
    bool start(FramebufferDisplay& display, int width, int height);

    /**
     * @brief  Copy a BGR frame into a shared slot and notify the LCD child.
     * @param  [in] bgr Continuous or strided CV_8UC3 image matching the configured size.
     * @param  [in] sequence Camera sequence number used to select the newest frame.
     * @retval true The frame was published.
     * @retval false The child is stopped, the image is invalid, or all slots are busy.
     */
    bool publish(const cv::Mat& bgr, uint32_t sequence);

    /**
     * @brief  Report whether the LCD child is still running.
     * @retval true No child failure has been observed.
     * @retval false The child exited unexpectedly or was not started.
     */
    bool healthy();

    /**
     * @brief  Return and reset LCD render timing and frame count accumulated by the child.
     * @param  [out] averageMs Average render time in milliseconds for the interval.
     * @param  [out] renderedFrames Number of frames completed by the child in the interval.
     */
    void takeRenderStats(double& averageMs, uint64_t& renderedFrames);

    /**
     * @brief  Publish the latest pipeline statistics for the LCD overlay.
     * @param  [in] stats Current capture, processing, LCD, and JPEG statistics.
     */
    void updateStats(const DisplayStats& stats);

    /**
     * @brief  Wake and join the LCD child, then release shared memory.
     */
    void stop();

    struct SharedState;

private:
    SharedState* shared_ = nullptr;
    size_t sharedLength_ = 0;
    int width_ = 0;
    int height_ = 0;
    int childPid_ = -1;
};

#endif
