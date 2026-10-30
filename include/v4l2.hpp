#ifndef V4L2_HPP
#define V4L2_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

using namespace std;

class V4l2Capture {
public:
    enum class PixelFormat {
        YUYV,
        MJPEG
    };

    enum class FrameResult {
        Frame,
        Timeout,
        Error
    };

    // Create an unopened camera capture object.
    V4l2Capture();
    // Stop capture on destruction.
    ~V4l2Capture();
    // Camera ownership is non-copyable.
    V4l2Capture(const V4l2Capture&) = delete;
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    /**
     * @brief  Open a camera and negotiate progressive YUYV or MJPEG capture.
     * @param  [in] device Camera device path.
     * @param  [in] width Requested positive, even image width.
     * @param  [in] height Requested positive image height.
     * @param  [in] format Requested camera pixel format.
     * @retval true The camera is open and the negotiated format is valid.
     * @retval false Device access, capability checks, or format negotiation failed.
     * @note   Closes any previous device; width(), height(), and bytesPerLine() report the negotiated geometry.
     */
    bool open(const string& device = "/dev/video0", int width = 640, int height = 480,
              PixelFormat format = PixelFormat::YUYV);

    /**
     * @brief  Map the camera buffers and start streaming capture.
     * @retval true Streaming started or was already active.
     * @retval false The device is unopened or initialization failed.
     * @note   Initialization failure releases existing mappings and closes the device.
     */
    bool start();

    /**
     * @brief  Stop streaming, unmap capture buffers, and close the camera.
     * @note   Safe to call repeatedly; call open() again before restarting.
     */
    void stop();

    /**
     * @brief  Wait for and copy a complete YUYV frame before returning its buffer.
     * @param  [out] out Owned copy of the captured YUYV or MJPEG payload.
     * @param  [out] sequence Sequence number reported by the camera driver.
     * @param  [in] timeoutMs Nonnegative poll timeout in milliseconds.
     * @retval Frame A complete frame and its sequence are available.
     * @retval Timeout No frame is ready, poll was interrupted, or a corrupt frame was discarded.
     * @retval Error Device access failed or the driver returned invalid buffer metadata.
     * @note   Use the output parameters only when Frame is returned.
     */
    FrameResult getFrame(vector<uint8_t>& out, uint32_t& sequence,
                         int timeoutMs = 250);

    // Return the negotiated image width.
    int width() const { return width_; }
    // Return the negotiated image height.
    int height() const { return height_; }
    // Return the negotiated row stride.
    int bytesPerLine() const { return bytes_per_line_; }

    // Report whether the camera is open.
    bool isOpened() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    int bytes_per_line_ = 0;
    PixelFormat pixel_format_ = PixelFormat::YUYV;
    bool streaming_ = false;

    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    vector<Buffer> buffers_;

    /**
     * @brief  Execute an ioctl request and retry interrupted system calls.
     * @param  [in] fd Device descriptor.
     * @param  [in] request V4L2 ioctl request identifier.
     * @param  [in,out] arg Request-specific control structure.
     * @retval 0 The V4L2 request completed successfully.
     * @retval -1 The request failed; errno describes the failure.
     */
    int xioctl(int fd, unsigned long request, void* arg);
};

#endif
