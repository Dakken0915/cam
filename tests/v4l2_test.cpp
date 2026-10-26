#include "v4l2.hpp"
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <poll.h>
#include <cstdarg>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
uint8_t storage[2][16]{};
int openCount = 0;
int mappingCount = 0;
int queuedCount = 0;
int streamOffCount = 0;
int pollResult = 1;
unsigned long failRequest = 0;
int failMapping = -1;
bool formatInvalid = false;
bool acceptMjpeg = false;
bool deviceCapsInvalid = false;
v4l2_buffer dequeued{};

/**
 * @brief  Throw a test failure when a simulated driver assertion is false.
 */
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

/**
 * @brief  Reset simulated driver behavior and verify resource counters are clear.
 */
void reset() {
    require(openCount == 0 && mappingCount == 0, "capture resources leaked");
    queuedCount = streamOffCount = 0;
    pollResult = 1;
    failRequest = 0;
    failMapping = -1;
    formatInvalid = deviceCapsInvalid = acceptMjpeg = false;
    dequeued = v4l2_buffer{};
    dequeued.bytesused = 12;
    dequeued.sequence = 42;
}
}

/**
 * @brief  Simulate opening a camera and count owned descriptors.
 */
extern "C" int __wrap_open(const char*, int, ...) {
    ++openCount;
    return 123;
}

/**
 * @brief  Simulate closing a descriptor to detect resource leaks.
 */
extern "C" int __wrap_close(int) {
    --openCount;
    return 0;
}

/**
 * @brief  Simulate camera readiness, timeout, and signal interruption.
 */
extern "C" int __wrap_poll(pollfd* descriptors, nfds_t, int) {
    descriptors[0].revents = POLLIN;
    if (pollResult < 0) errno = EINTR;
    return pollResult;
}

/**
 * @brief  Simulate camera buffer mapping and inject a selected mapping failure.
 */
extern "C" void* __wrap_mmap(void*, size_t, int, int, int, off_t offset) {
    if (offset == failMapping) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    ++mappingCount;
    return storage[offset];
}

/**
 * @brief  Simulate unmapping a camera buffer and update the mapping count.
 */
extern "C" int __wrap_munmap(void*, size_t) {
    --mappingCount;
    return 0;
}

/**
 * @brief  Simulate V4L2 negotiation and buffer control with injectable failures.
 */
extern "C" int __wrap_ioctl(int, unsigned long request, ...) {
    va_list arguments;
    va_start(arguments, request);
    void* arg = va_arg(arguments, void*);
    va_end(arguments);
    if (request == failRequest) {
        errno = EIO;
        return -1;
    }
    if (request == VIDIOC_QUERYCAP) {
        auto* cap = static_cast<v4l2_capability*>(arg);
        cap->capabilities = V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
        cap->device_caps = deviceCapsInvalid ? 0 : V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    } else if (request == VIDIOC_S_FMT) {
        auto* format = static_cast<v4l2_format*>(arg);
        format->fmt.pix.width = 2;
        format->fmt.pix.height = 2;
        format->fmt.pix.bytesperline = formatInvalid ? 2 : 8;
        if (acceptMjpeg) format->fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    } else if (request == VIDIOC_REQBUFS) {
        static_cast<v4l2_requestbuffers*>(arg)->count = 2;
    } else if (request == VIDIOC_QUERYBUF) {
        auto* buffer = static_cast<v4l2_buffer*>(arg);
        buffer->length = sizeof(storage[0]);
        buffer->m.offset = buffer->index;
    } else if (request == VIDIOC_QBUF) {
        ++queuedCount;
    } else if (request == VIDIOC_DQBUF) {
        *static_cast<v4l2_buffer*>(arg) = dequeued;
    } else if (request == VIDIOC_STREAMOFF) {
        ++streamOffCount;
    }
    return 0;
}

/**
 * @brief  Test negotiated geometry, frame validation, timeouts, and resource cleanup.
 */
int main() {
    try {
        reset();
        {
            V4l2Capture capture;
            require(capture.open("mock", 640, 480), "valid camera open failed");
            require(capture.width() == 2 && capture.height() == 2 && capture.bytesPerLine() == 8,
                    "negotiated format ignored");
            require(capture.start() && capture.start(), "capture start failed");
            require(mappingCount == 2 && queuedCount == 2, "repeated start remapped buffers");
            std::vector<uint8_t> frame;
            uint32_t sequence = 0;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Frame &&
                    frame.size() == 12 && sequence == 42, "valid padded frame rejected");
            dequeued.bytesused = 11;
            const int previous = queuedCount;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Timeout &&
                    queuedCount == previous + 1, "short frame not returned to driver");
            dequeued.bytesused = 12;
            dequeued.flags = V4L2_BUF_FLAG_ERROR;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Timeout,
                    "corrupt frame accepted");
            dequeued.flags = 0;
            dequeued.index = 9;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Error,
                    "invalid buffer index accepted");
            dequeued.index = 0;
            dequeued.bytesused = 17;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Error,
                    "oversized payload accepted");
            pollResult = 0;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Timeout,
                    "poll timeout not propagated");
            pollResult = -1;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Timeout,
                    "signal interrupted poll not propagated");
        }
        require(streamOffCount == 1, "stream not stopped once");
        reset();
        {
            acceptMjpeg = true;
            V4l2Capture capture;
            require(capture.open("mock", 640, 480, V4l2Capture::PixelFormat::MJPEG),
                    "MJPEG camera negotiation failed");
            require(capture.start(), "MJPEG capture start failed");
            dequeued.bytesused = 5;
            std::vector<uint8_t> frame;
            uint32_t sequence = 0;
            require(capture.getFrame(frame, sequence) == V4l2Capture::FrameResult::Frame &&
                    frame.size() == 5, "short compressed MJPEG payload was rejected");
        }
        require(streamOffCount == 1, "MJPEG stream not stopped once");
        for (unsigned long request : {VIDIOC_REQBUFS, VIDIOC_QUERYBUF, VIDIOC_QBUF, VIDIOC_STREAMON}) {
            reset();
            V4l2Capture capture;
            require(capture.open("mock"), "camera open failed before injected error");
            failRequest = request;
            require(!capture.start() && openCount == 0 && mappingCount == 0,
                    "start failure leaked mappings or fd");
        }
        reset();
        {
            V4l2Capture capture;
            require(capture.open("mock"), "camera open failed before mmap error");
            failMapping = 1;
            require(!capture.start() && mappingCount == 0 && openCount == 0,
                    "partial mmap failure leaked resources");
        }
        reset();
        {
            V4l2Capture capture;
            formatInvalid = true;
            require(!capture.open("mock") && openCount == 0, "invalid stride accepted");
            formatInvalid = false;
            deviceCapsInvalid = true;
            require(!capture.open("mock") && openCount == 0, "device_caps ignored");
        }
        reset();
        std::cout << "V4L2 capture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
