#include "jpeg_encoder.hpp"
#include <stdexcept>

std::vector<uint8_t> JpegEncoder::encode(const uint8_t* yuyv, int width, int height,
                                         int quality, size_t bytesPerLine) {
    return encodeBgr(convertToBgr(yuyv, width, height, bytesPerLine), quality);
}

const cv::Mat& JpegEncoder::convertToBgr(const uint8_t* yuyv, int width, int height,
                                      size_t bytesPerLine) {
    if (!yuyv || width <= 0 || height <= 0 || width % 2 ||
        (bytesPerLine && bytesPerLine < static_cast<size_t>(width) * 2))
        throw std::invalid_argument("Invalid YUYV image geometry");
    const size_t step = bytesPerLine == 0 ? static_cast<size_t>(cv::Mat::AUTO_STEP) : bytesPerLine;
    cv::Mat yuyv_mat(height, width, CV_8UC2, const_cast<uint8_t*>(yuyv), step);

    cv::cvtColor(yuyv_mat, bgr_, cv::COLOR_YUV2BGR_YUYV);
    return bgr_;
}

std::vector<uint8_t> JpegEncoder::encodeBgr(const cv::Mat& bgr, int quality) {
    if (bgr.empty() || bgr.type() != CV_8UC3 || quality < 1 || quality > 100)
        throw std::invalid_argument("Invalid BGR image or JPEG quality");
    std::vector<uint8_t> jpeg;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, quality };
    if (!cv::imencode(".jpg", bgr, jpeg, params))
        throw std::runtime_error("JPEG encoding failed");

    return jpeg;
}
