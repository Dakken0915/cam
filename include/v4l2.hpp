#ifndef V4L2_HPP
#define V4L2_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

using namespace std;

class V4l2Capture {
public:
    enum class FrameResult {
        Frame,
        Timeout,
        Error
    };

    /**
     * @brief  Create an unopened camera capture object.
     */
    V4l2Capture();
    /**
     * @brief  Stop capture and release the camera resources.
     */
    ~V4l2Capture();
    /**
     * @brief  Prevent copying ownership of the camera descriptor and mapped buffers.
     */
    V4l2Capture(const V4l2Capture&) = delete;
    /**
     * @brief  Prevent copy assignment of camera resources.
     */
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    /**
     * @brief  Open a camera and negotiate progressive YUYV capture.
     * @param  [in] device Camera device path.
     * @param  [in] width Requested positive, even image width.
     * @param  [in] height Requested positive image height.
     * @retval true The camera is open and the negotiated format is valid.
     * @retval false Device access, capability checks, or format negotiation failed.
     * @note   Closes any previous device; width(), height(), and bytesPerLine() report the negotiated geometry.
     */
    bool open(const string& device = "/dev/video0", int width = 640, int height = 480);

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
     * @param  [out] out Owned copy of the captured YUYV payload.
     * @param  [out] sequence Sequence number reported by the camera driver.
     * @param  [in] timeoutMs Nonnegative poll timeout in milliseconds.
     * @retval Frame A complete frame and its sequence are available.
     * @retval Timeout No frame is ready, poll was interrupted, or a corrupt frame was discarded.
     * @retval Error Device access failed or the driver returned invalid buffer metadata.
     * @note   Use the output parameters only when Frame is returned.
     */
    FrameResult getFrame(vector<uint8_t>& out, uint32_t& sequence,
                         int timeoutMs = 250);

    /**
     * @brief  Return the image width negotiated with the camera driver.
     */
    int width() const { return width_; }
    /**
     * @brief  Return the image height negotiated with the camera driver.
     */
    int height() const { return height_; }
    /**
     * @brief  Return the negotiated YUYV row stride, including padding.
     */
    int bytesPerLine() const { return bytes_per_line_; }

    /**
     * @brief  Report whether the camera descriptor is open.
     */
    bool isOpened() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    int width_ = 0;
    int height_ = 0;
    int bytes_per_line_ = 0;
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
