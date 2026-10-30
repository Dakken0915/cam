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

int V4l2Capture::xioctl(int fd, unsigned long request, void* arg) {
    int ret;

    do ret = ioctl(fd, request, arg);
    while(ret == -1 && errno == EINTR);  // Retry when a signal interrupts the request.

    return ret;
}

bool V4l2Capture::open(const string& device, int width, int height, PixelFormat format) {
    stop();
    if (width <= 0 || height <= 0 || width % 2 || width > INT_MAX / 2) return false;
    fd_ = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd_ < 0) {
        cerr << "[V4L2] 无法打开" << device << ':' << strerror(errno) << endl;
        return false;
    }
    
    struct v4l2_capability cap{};
    if(xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        cerr << "[V4L2] 查询设备能力失败" << endl;
        ::close(fd_);  // Close the device descriptor.
        fd_ = -1;
        return false;
    }
    cout << "[V4L2] 驱动: " << cap.driver << " 设备: " << cap.card << endl;

    const uint32_t capabilities = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
        ? cap.device_caps : cap.capabilities;
    if(!(capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        cerr << "[V4L2] 设备不支持视频采集" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (!(capabilities & V4L2_CAP_STREAMING)) {
        cerr << "[V4L2] 设备不支持流式采集" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    const uint32_t requestedFormat = format == PixelFormat::MJPEG
        ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.pixelformat = requestedFormat;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    if(xioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        cerr << "[V4L2] 设置格式失败" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if(fmt.fmt.pix.pixelformat != requestedFormat) {
        cerr << "[V4L2] Driver did not accept requested pixel format" << endl;
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (!fmt.fmt.pix.width || !fmt.fmt.pix.height || fmt.fmt.pix.width % 2 ||
        fmt.fmt.pix.width > INT_MAX / 2 || fmt.fmt.pix.height > INT_MAX ||
        fmt.fmt.pix.bytesperline > INT_MAX ||
        (format == PixelFormat::YUYV && fmt.fmt.pix.bytesperline &&
         fmt.fmt.pix.bytesperline < fmt.fmt.pix.width * 2) ||
        fmt.fmt.pix.field != V4L2_FIELD_NONE) {
        cerr << "[V4L2] Invalid geometry, stride or interlaced format" << endl;
        stop();
        return false;
    }
    width_ = fmt.fmt.pix.width;
    height_ = fmt.fmt.pix.height;
    bytes_per_line_ = fmt.fmt.pix.bytesperline;
    pixel_format_ = format;
    if (pixel_format_ == PixelFormat::YUYV && bytes_per_line_ <= 0)
        bytes_per_line_ = width_ * 2;
    cout << "[V4L2] 分辨率为" << width_ << "x" << height_ << ' '
         << (pixel_format_ == PixelFormat::MJPEG ? "MJPEG" : "YUYV") << endl;

    return true;
}

bool V4l2Capture::start() {
    if(fd_ < 0) return false;
    if(streaming_) return true;

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

    buffers_.resize(req.count);
    for(unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(v4l2_buffer));
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
            NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset
        );

        if(buffers_[i].start == MAP_FAILED) {
            cerr << "[V4L2] mmap " << i << " 失败" << endl;
            stop();
            return false;
        }
    }
    
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

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        cerr << "[V4L2] STREAMON 失败" << endl;
        stop();
        return false;
    }

    streaming_ = true;
    cout << "[V4L2] 采集已启动" << endl;
    return true;
}

V4l2Capture::FrameResult V4l2Capture::getFrame(vector<uint8_t>& out,
                                               uint32_t& sequence,
                                               int timeoutMs) {
    if (fd_ < 0 || !streaming_ || timeoutMs < 0) return FrameResult::Error;

    pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
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
    const uint64_t required = pixel_format_ == PixelFormat::YUYV
        ? static_cast<uint64_t>(bytes_per_line_) * (height_ - 1) +
              static_cast<uint64_t>(width_) * 2
        : 1;
    if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused < required) {
        CAM_DEBUG("[V4L2] Dropping incomplete or corrupt frame");
        return xioctl(fd_, VIDIOC_QBUF, &buf) < 0 ? FrameResult::Error : FrameResult::Timeout;
    }

    out.resize(buf.bytesused);
    memcpy(out.data(), buffers_[buf.index].start, buf.bytesused);
    sequence = buf.sequence;

    if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        cerr << "[V4L2] QBUF(循环) 失败" << endl;
        return FrameResult::Error;
    }

    return FrameResult::Frame;
}

void V4l2Capture::stop() {
    if (fd_ < 0) return;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (streaming_) xioctl(fd_, VIDIOC_STREAMOFF, &type);
    streaming_ = false;

    for (size_t i = 0; i < buffers_.size(); i++) {
        if (buffers_[i].start != nullptr && buffers_[i].start != MAP_FAILED) {
            munmap(buffers_[i].start, buffers_[i].length);
        }
    }
    buffers_.clear();

    ::close(fd_);
    fd_ = -1;
    width_ = height_ = bytes_per_line_ = 0;
    pixel_format_ = PixelFormat::YUYV;

    std::cout << "[V4L2] 设备已关闭" << std::endl;
}

V4l2Capture::V4l2Capture() {}

V4l2Capture::~V4l2Capture() {
    stop();
}
