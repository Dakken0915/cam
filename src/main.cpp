#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include "v4l2.hpp"
#include "jpeg_encoder.hpp"
#include "framebuffer.hpp"
#include "touchscreen.hpp"
#include "face_analyzer.hpp"
#include "http_server.hpp"
#include "frame.hpp"
#include "bounded_queue.hpp"

namespace {

volatile sig_atomic_t signalRunning = 1;

struct Options {
    std::string device = "/dev/video1";
    std::string framebuffer = "/dev/fb0";
    std::string snapshots = "/root/photos";
    std::string touchDevice = "/dev/input/event1";
    std::string faceCascade = "assets/haarcascade_frontalface_default.xml";
    std::string faceReference;
    int width = 640;
    int height = 480;
    int port = 8080;
    int quality = 80;
    int grayThresholdPercent = 75;
    int faceInterval = 8;
    bool lcdEnabled = true;
    bool lcdRequired = false;
    bool touchEnabled = true;
    bool touchRequired = false;
    bool help = false;
};

struct PipelineState {
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    /**
     * @brief  Record a pipeline failure and request worker shutdown.
     * @param  [in] message Failure message to write to standard error.
     */
    void fail(const std::string& message) {
        std::cerr << "[Pipeline] " << message << '\n';
        failed = true;
        stop = true;
    }
};

/**
 * @brief  Request shutdown using only a signal-safe flag assignment.
 */
void onSignal(int) {
    signalRunning = 0;
}

/**
 * @brief  Return a monotonic frame timestamp in microseconds.
 */
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/**
 * @brief  Parse a bounded decimal command-line integer.
 * @param  [in] text Null-terminated numeric argument.
 * @param  [in] minimum Smallest accepted value.
 * @param  [in] maximum Largest accepted value.
 * @retval Integer Parsed value within the inclusive bounds.
 * @note   Throws std::invalid_argument on overflow, missing digits, trailing characters, or an out-of-range value.
 */
int parseNumber(const char* text, int minimum, int maximum) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("Invalid numeric option: ") + text);
    return static_cast<int>(value);
}

/**
 * @brief  Parse and validate camera, display, encoding, and HTTP options.
 * @param  [in] argc Argument count, including the executable name.
 * @param  [in] argv Argument strings.
 * @retval Options Validated configuration using defaults for omitted options.
 * @note   Throws std::invalid_argument for unknown options, invalid values, or conflicting LCD settings.
 */
Options parseOptions(int argc, char** argv) {
    Options result;
    const option options[] = {
        {"device", required_argument, nullptr, 'd'},
        {"fb", required_argument, nullptr, 'f'},
        {"no-lcd", no_argument, nullptr, 'n'},
        {"width", required_argument, nullptr, 'w'},
        {"height", required_argument, nullptr, 'H'},
        {"port", required_argument, nullptr, 'p'},
        {"quality", required_argument, nullptr, 'q'},
        {"snapshot-dir", required_argument, nullptr, 's'},
        {"touch", required_argument, nullptr, 't'},
        {"no-touch", no_argument, nullptr, 'T'},
        {"face-cascade", required_argument, nullptr, 'c'},
        {"face-reference", required_argument, nullptr, 'r'},
        {"gray-threshold", required_argument, nullptr, 'g'},
        {"face-interval", required_argument, nullptr, 'i'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    int flag;
    while ((flag = getopt_long(argc, argv, "", options, nullptr)) != -1) {
        switch (flag) {
        case 'd': result.device = optarg; break;
        case 'f': result.framebuffer = optarg; result.lcdRequired = true; break;
        case 'n': result.lcdEnabled = false; break;
        case 'w': result.width = parseNumber(optarg, 2, 8192); break;
        case 'H': result.height = parseNumber(optarg, 1, 8192); break;
        case 'p': result.port = parseNumber(optarg, 1, 65535); break;
        case 'q': result.quality = parseNumber(optarg, 1, 100); break;
        case 's': result.snapshots = optarg; break;
        case 't': result.touchDevice = optarg; result.touchRequired = true; break;
        case 'T': result.touchEnabled = false; break;
        case 'c': result.faceCascade = optarg; break;
        case 'r': result.faceReference = optarg; break;
        case 'g': result.grayThresholdPercent = parseNumber(optarg, 0, 100); break;
        case 'i': result.faceInterval = parseNumber(optarg, 1, 120); break;
        case 'h': result.help = true; break;
        default: throw std::invalid_argument("Unknown or incomplete option; use --help");
        }
    }
    if (optind != argc || result.width % 2 ||
        (!result.lcdEnabled && result.lcdRequired) ||
        (!result.lcdEnabled && result.touchRequired) ||
        (!result.touchEnabled && result.touchRequired))
        throw std::invalid_argument("Unexpected arguments, odd YUYV width, or conflicting LCD/touch options");
    return result;
}

/**
 * @brief  Print command-line options and LCD initialization behavior.
 */
void printHelp() {
    std::cout << "Usage: cam_server [options]\n"
              << "  --device PATH       Camera device (default /dev/video1)\n"
              << "  --width N           Requested even capture width (default 640)\n"
              << "  --height N          Requested capture height (default 480)\n"
              << "  --fb PATH           Require this LCD framebuffer (default /dev/fb0 is optional)\n"
              << "  --no-lcd            Disable LCD output\n"
              << "  --port N            HTTP port (default 8080)\n"
              << "  --quality N         JPEG quality 1..100 (default 80)\n"
              << "  --snapshot-dir PATH Snapshot storage (default /root/photos)\n"
              << "  --touch PATH        Require this evdev touchscreen (default /dev/input/event1)\n"
              << "  --no-touch          Disable the LCD touch button\n"
              << "  --face-cascade PATH Haar cascade XML (default assets/haarcascade_frontalface_default.xml)\n"
              << "  --face-reference PATH Optional grayscale comparison image\n"
              << "  --gray-threshold N  Similarity threshold 0..100 (default 75)\n"
              << "  --face-interval N   Run face analysis every N frames (default 8)\n"
              << "  --help              Show this help\n";
}

/**
 * @brief  Capture camera frames and enqueue owned YUYV payloads.
 * @param  [in,out] capture Initialized, streaming camera object.
 * @param  [in,out] raw Destination queue for captured frames.
 * @param  [in,out] state Shared failure and shutdown state.
 * @note   Closes the raw queue on exit and records capture failures or exceptions.
 */
void captureLoop(V4l2Capture& capture, BoundedQueue<Frame>& raw, PipelineState& state) {
    try {
        while (signalRunning && !state.stop) {
            Frame frame;
            const auto result = capture.getFrame(frame.data, frame.sequence, 250);
            if (result == V4l2Capture::FrameResult::Timeout) continue;
            if (result == V4l2Capture::FrameResult::Error) {
                state.fail("Camera capture failed");
                break;
            }
            frame.width = capture.width();
            frame.height = capture.height();
            frame.bytes_per_line = capture.bytesPerLine();
            frame.timestamp_us = nowUs();
            raw.push(std::move(frame));
        }
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
    raw.close();
}

/**
 * @brief  Convert each frame once, optionally display it, and enqueue JPEG output.
 * @param  [in,out] raw Source queue for YUYV frames.
 * @param  [in,out] jpeg Destination queue for encoded frames.
 * @param  [in,out] display Display object used when LCD output is active.
 * @param  [in] lcdActive Whether the display was initialized.
 * @param  [in] quality JPEG quality in the range 1 to 100.
 * @param  [in,out] faceWorker Background Haar detector and optional comparator.
 * @param  [in] faceActive Whether face analysis was initialized successfully.
 * @param  [in,out] state Shared failure and shutdown state.
 * @note   Records processing exceptions and closes both queues on exit; LCD and JPEG work is serial.
 */
void encodeLoop(BoundedQueue<Frame>& raw, BoundedQueue<Frame>& jpeg,
                FramebufferDisplay& display, bool lcdActive, int quality,
                FaceAnalysisWorker& faceWorker, bool faceActive, PipelineState& state) {
    try {
        JpegEncoder encoder;
        Frame frame;
        while (raw.take(frame)) {
            if (!signalRunning || state.stop) break;
            const size_t required = static_cast<size_t>(frame.bytes_per_line) *
                                    (frame.height - 1) + static_cast<size_t>(frame.width) * 2;
            if (frame.data.size() < required)
                throw std::runtime_error("Incomplete YUYV frame");
            const cv::Mat& bgr = encoder.convertToBgr(frame.data.data(), frame.width,
                frame.height, static_cast<size_t>(frame.bytes_per_line));
            if (faceActive) faceWorker.submit(bgr);
            if (lcdActive && !display.render(bgr))
                throw std::runtime_error("LCD rendering failed");
            Frame encoded;
            encoded.sequence = frame.sequence;
            encoded.width = frame.width;
            encoded.height = frame.height;
            encoded.timestamp_us = frame.timestamp_us;
            encoded.data = encoder.encodeBgr(bgr, quality);
            jpeg.push(std::move(encoded));
        }
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
    raw.close();
    jpeg.close();
}

/**
 * @brief  Wait for LCD touch releases and save a photo when the button is tapped.
 * @param  [in,out] touch Open evdev touchscreen reader.
 * @param  [in] display LCD display exposing the button hit area.
 * @param  [in,out] server Snapshot service receiving the current JPEG.
 * @param  [in,out] state Shared failure and shutdown state.
 * @note   A tap before the first encoded frame reports NoFrame and does not create a file.
 */
void touchLoop(Touchscreen& touch, const FramebufferDisplay& display,
               HttpServer& server, PipelineState& state) {
    try {
        while (signalRunning && !state.stop) {
            TouchPoint point;
            if (!touch.waitForTap(point, 250)) continue;
            if (!display.isPhotoButton(point.x, point.y)) {
                std::cout << "[Touch] Tap outside photo button at " << point.x << ','
                          << point.y << std::endl;
                continue;
            }
            std::cout << "[Touch] Photo button activated at " << point.x << ','
                      << point.y << "; capturing latest JPEG" << std::endl;
            std::string path;
            const HttpServer::SnapshotResult result = server.captureSnapshot(path);
            if (result == HttpServer::SnapshotResult::Saved) {
                std::cout << "[Touch] Photo saved: " << path << std::endl;
            } else if (result == HttpServer::SnapshotResult::NoFrame) {
                std::cerr << "[Touch] Photo ignored: no camera frame is available\n";
            } else if (result == HttpServer::SnapshotResult::StorageFull) {
                std::cerr << "[Touch] Photo rejected: photo quota is full\n";
            } else {
                std::cerr << "[Touch] Photo failed: cannot write photo directory\n";
            }
        }
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
}

/**
 * @brief  Start the camera, LCD, and HTTP pipeline and coordinate cleanup.
 * @param  [in] options Validated runtime configuration.
 * @retval 0 The pipeline shut down without a recorded failure.
 * @retval 1 Required initialization or pipeline processing failed.
 * @note   Joins worker threads before releasing their devices; optional LCD initialization may fall back to HTTP only.
 */
int run(const Options& options) {
    FaceAnalyzer faceAnalyzer;
    const bool faceCascadeLoaded = faceAnalyzer.loadCascade(options.faceCascade);
    const bool faceReferenceLoaded = options.faceReference.empty() ||
                                     faceAnalyzer.loadReference(options.faceReference);
    const bool faceActive = faceCascadeLoaded && faceReferenceLoaded;
    if (!faceActive)
        std::cerr << "[Face] Face analysis disabled; video pipeline will continue\n";
    const double grayThreshold = options.grayThresholdPercent / 100.0;
    FaceAnalysisWorker faceWorker(faceAnalyzer, grayThreshold,
                                  static_cast<unsigned int>(options.faceInterval));
    if (faceActive) faceWorker.start();
    FramebufferDisplay display;
    bool lcdActive = false;
    if (options.lcdEnabled) {
        lcdActive = display.open(options.framebuffer);
        if (!lcdActive && options.lcdRequired) return 1;
        if (!lcdActive) std::cerr << "[LCD] Continuing with HTTP only; use --fb to require LCD\n";
    }
    V4l2Capture capture;
    if (!capture.open(options.device, options.width, options.height) || !capture.start())
        return 1;
    HttpServer server(options.snapshots);
    if (!server.start(options.port)) return 1;

    Touchscreen touch;
    bool touchActive = false;
    if (options.touchEnabled && lcdActive) {
        touchActive = touch.open(options.touchDevice);
        if (touchActive) {
            const auto screen = display.screenSize();
            touch.setScreenSize(screen.first, screen.second);
        } else if (options.touchRequired) {
            std::cerr << "[Touch] Required touchscreen could not be opened\n";
            server.stop();
            capture.stop();
            return 1;
        } else {
            std::cerr << "[Touch] No touchscreen found; LCD photo button is inactive\n";
        }
    }

    BoundedQueue<Frame> raw(3);
    BoundedQueue<Frame> jpeg(3);
    PipelineState state;
    std::thread captureThread;
    std::thread encodeThread;
    std::thread touchThread;
    try {
        captureThread = std::thread(captureLoop, std::ref(capture), std::ref(raw), std::ref(state));
        encodeThread = std::thread(encodeLoop, std::ref(raw), std::ref(jpeg),
            std::ref(display), lcdActive, options.quality, std::ref(faceWorker),
            faceActive, std::ref(state));
        if (touchActive)
            touchThread = std::thread(touchLoop, std::ref(touch), std::cref(display),
                                      std::ref(server), std::ref(state));
        Frame encoded;
        while (jpeg.take(encoded)) server.updateFrame(encoded.data);
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
    state.stop = true;
    raw.close();
    jpeg.close();
    faceWorker.stop();
    if (captureThread.joinable()) captureThread.join();
    if (encodeThread.joinable()) encodeThread.join();
    if (touchThread.joinable()) touchThread.join();
    server.stop();
    capture.stop();
    return state.failed ? 1 : 0;
}

} // namespace

/**
 * @brief  Parse startup options, install signal handlers, and run the application.
 * @param  [in] argc Argument count, including the executable name.
 * @param  [in] argv Argument strings.
 * @retval 0 Help was displayed or the application stopped normally.
 * @retval 1 Argument validation, initialization, or pipeline execution failed.
 */
int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.help) {
            printHelp();
            return 0;
        }
        struct sigaction action{};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, nullptr) < 0 ||
            sigaction(SIGTERM, &action, nullptr) < 0)
            throw std::runtime_error("Cannot install signal handlers");
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "[Startup] " << error.what() << '\n';
        return 1;
    }
}
