#include <iostream>
#include <fstream>
#include <fcntl.h>     // open
#include <unistd.h>    // close
#include <sys/ioctl.h> // ioctl
#include <sys/mman.h>  // mmap
#include <linux/videodev2.h>  // v4l2
#include <poll.h>
#include "v4l2.hpp"
#include <cerrno>
#include <cstring>
#include <climits>
#include "debug.hpp"

using namespace std;

/**
 * @brief  Execute an ioctl request and retry interrupted system calls.
 * @param  [in] fd Device descriptor.
 * @param  [in] request V4L2 ioctl request identifier.
 * @param  [in,out] arg Request-specific control structure.
 * @retval 0 The V4L2 request completed successfully.
 * @retval -1 The request failed; errno describes the failure.
 */
int V4l2Capture::xioctl(int fd, unsigned long request, void* arg) {
    int ret;

    do ret = ioctl(fd, request, arg);
    while(ret == -1 && errno == EINTR);  // Retry when a signal interrupts the request.

    return ret;
}

/**
 * @brief  Open a camera and negotiate progressive YUYV capture.
 * @param  [in] device Camera device path.
 * @param  [in] width Requested positive, even image width.
 * @param  [in] height Requested positive image height.
 * @retval true The camera is open and the negotiated format is valid.
 * @retval false Device access, capability checks, or format negotiation failed.
 * @note   Closes any previous device; width(), height(), and bytesPerLine() report the negotiated geometry.
 */
bool V4l2Capture::open(const string& device, int width, int height) {
    stop();
    if (width <= 0 || height <= 0 || width % 2 || width > INT_MAX / 2) return false;
    // Open the camera device.
    fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd_ < 0) {
        cerr << "[V4L2] 无法打开" << device << ':' << strerror(errno) << endl;
        return false;
    }
    
    // Query device capabilities.
    struct v4l2_capability cap{};
    if(xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        cerr << "[V4L2] 查询设备能力失败" << endl;
        ::close(fd_);  // Close the device descriptor.
        fd_ = -1;
        return false;
    }
    cout << "[V4L2] 驱动: " << cap.driver << " 设备: " << cap.card << endl;

    // Require single-plane video capture.
    const uint32_t capabilities = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
        ? cap.device_caps : cap.capabilities;
    if(!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        cerr << "[V4L2] 设备不支持视频采集" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Require streaming capture for MMAP buffers.
    if (!(capabilities & V4L2_CAP_STREAMING)) {
        cerr << "[V4L2] 设备不支持流式采集" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Negotiate the capture format.
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;    // Progressive scan.
    if(xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        cerr << "[V4L2] 设置格式失败" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        cerr << "[V4L2] Driver did not accept YUYV format" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // Validate the geometry negotiated by the driver.
    if (!fmt.fmt.pix.width || !fmt.fmt.pix.height || fmt.fmt.pix.width % 2 ||
        fmt.fmt.pix.width > INT_MAX / 2 || fmt.fmt.pix.height > INT_MAX ||
        fmt.fmt.pix.bytesperline > INT_MAX ||
        (fmt.fmt.pix.bytesperline && fmt.fmt.pix.bytesperline < fmt.fmt.pix.width * 2) ||
        fmt.fmt.pix.field != V4L2_FIELD_NONE) {
        cerr << "[V4L2] Invalid geometry, stride or interlaced format" << endl;
        stop();
        return false;
    }
    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    bytes_per_line_ = fmt.fmt.pix.bytesperline;
    if (bytes_per_line_ <= 0) bytes_per_line_ = width_ * 2;
    cout << "[V4L2] 分辨率为" << width_ << "x" << height_ << " YUYV" << endl;

    return true;
}

/**
 * @brief  Map the camera buffers and start streaming capture.
 * @retval true Streaming started or was already active.
 * @retval false The device is unopened or initialization failed.
 * @note   Initialization failure releases existing mappings and closes the device.
 */
bool V4l2Capture::start() {
    if(fd_ < 0) return false;
    if(streaming_) return true;

    // Request driver-managed capture buffers.
    const int BUF_COUNT = 4;
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if(xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        cerr << "[V4L2] 申请缓冲区失败" << endl;
        stop();
        return false;
    }

    // Query and map each capture buffer.
    buffers_.resize(req.count);
    for(unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;  // Metadata for one capture buffer.
        memset(&buf, 0, sizeof(v4l2_buffer));
        // Select the buffer type, memory model, and index.
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            cerr << "[V4L2] QUERYBUF " << i << " 失败" << endl;
            stop();
            return false;
        }

        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(
            NULL,                    // Let the kernel select the virtual address.
            buf.length,              // Buffer length in bytes.
            PROT_READ | PROT_WRITE,  // Read and write permissions.
            MAP_SHARED,              // Share memory with the driver.
            fd_,                     // Camera device descriptor.
            buf.m.offset             // Driver-reported buffer offset.
        );

        if(buffers_[i].start == MAP_FAILED) {
            cerr << "[V4L2] mmap " << i << " 失败" << endl;
            stop();
            return false;
        }
    }
    
    // Queue all buffers before starting capture.
    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            cerr << "[V4L2] QBUF " << i << " 失败" << endl;
            stop();
            return false;
        }
    }

    // Start streaming capture.
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // Select single-plane video capture.
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        cerr << "[V4L2] STREAMON 失败" << endl;
        stop();
        return false;
    }

    streaming_ = true;
    cout << "[V4L2] 采集已启动" << endl;
    return true;
}

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
V4l2Capture::FrameResult V4l2Capture::getFrame(vector<uint8_t>& out,
                                               uint32_t& sequence,
                                               int timeoutMs) {
    if (fd_ < 0 || !streaming_ || timeoutMs < 0) return FrameResult::Error;

    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    // Combine poll with nonblocking DQBUF so missing frames do not prevent shutdown.
    const int pollResult = poll(&descriptor, 1, timeoutMs);
    if (pollResult < 0 && errno == EINTR) return FrameResult::Timeout;
    if (pollResult == 0) return FrameResult::Timeout;
    if (pollResult < 0 || !(descriptor.revents & POLLIN)) {
        cerr << "[V4L2] poll failed: " << strerror(errno) << endl;
        return FrameResult::Error;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    // Dequeue a completed capture buffer.
    if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return FrameResult::Timeout;
        cerr << "[V4L2] DQBUF 失败" << endl;
        return FrameResult::Error;
    }

    if (buf.index >= buffers_.size() || buf.bytesused > buffers_[buf.index].length) {
        cerr << "[V4L2] Invalid buffer index or payload length" << endl;
        if (buf.index < buffers_.size() && xioctl(fd_, VIDIOC_QBUF, &buf) < 0)
            return FrameResult::Error;
        return FrameResult::Error;
    }
    const uint64_t required = static_cast<uint64_t>(bytes_per_line_) * (height_ - 1) +
                              static_cast<uint64_t>(width_) * 2;
    if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused < required) {
        CAM_DEBUG("[V4L2] Dropping incomplete or corrupt frame");
        return xioctl(fd_, VIDIOC_QBUF, &buf) < 0 ? FrameResult::Error : FrameResult::Timeout;
    }

    // Copy the payload into caller-owned storage.
    out.resize(buf.bytesused);
    // Access the contiguous output storage.
    memcpy(out.data(), buffers_[buf.index].start, buf.bytesused);
    sequence = buf.sequence;

    // Return the buffer immediately so the driver can capture another frame.
    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        cerr << "[V4L2] QBUF(循环) 失败" << endl;
        return FrameResult::Error;
    }

    return FrameResult::Frame;
}

/**
 * @brief  Stop streaming, unmap capture buffers, and close the camera.
 * @note   Safe to call repeatedly; call open() again before restarting.
 */
void V4l2Capture::stop() {
    if (fd_ < 0) return;

    // Stop streaming before releasing mappings.
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (streaming_) xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;

    // Unmap all successfully mapped buffers.
    for (size_t i = 0; i < buffers_.size(); i++) {
        if (buffers_[i].start != nullptr && buffers_[i].start != MAP_FAILED) {
            munmap(buffers_[i].start, buffers_[i].length);
        }
    }
    buffers_.clear();

    // Close the camera device.
    ::close(fd_);
    fd_ = -1;
    width_ = height_ = bytes_per_line_ = 0;

    std::cout << "[V4L2] 设备已关闭" << std::endl;
}

/**
 * @brief  Create an unopened camera capture object.
 */
V4l2Capture::V4l2Capture() {}

/**
 * @brief  Stop capture and release the camera resources.
 */
V4l2Capture::~V4l2Capture() {
    stop();
}
