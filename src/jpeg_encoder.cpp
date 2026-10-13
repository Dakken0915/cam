#include "jpeg_encoder.hpp"
#include <stdexcept>

/**
 * @brief  Convert a complete YUYV image and encode it as JPEG.
 * @param  [in] yuyv Pointer to valid YUYV input storage.
 * @param  [in] width Positive, even image width.
 * @param  [in] height Positive image height.
 * @param  [in] quality JPEG quality in the range 1 to 100.
 * @param  [in] bytesPerLine Input row stride; zero selects width * 2.
 * @retval JPEG Encoded image bytes.
 * @note   Invalid geometry or quality and conversion or encoding errors propagate as exceptions.
 * @warning The caller must provide enough input bytes for the requested geometry and stride.
 */
std::vector<uint8_t> JpegEncoder::encode(const uint8_t* yuyv, int width, int height,
                                         int quality, size_t bytesPerLine) {
    return encodeBgr(convertToBgr(yuyv, width, height, bytesPerLine), quality);
}

/**
 * @brief  Convert YUYV rows into a reusable BGR image.
 * @param  [in] yuyv Pointer to valid YUYV input storage.
 * @param  [in] width Positive, even image width.
 * @param  [in] height Positive image height.
 * @param  [in] bytesPerLine Input row stride; zero selects width * 2.
 * @retval BGR Reference to the internal CV_8UC3 image buffer.
 * @note   Invalid input geometry throws std::invalid_argument; OpenCV errors propagate.
 * @warning The next conversion can overwrite the returned image; supply sufficient input storage and consume it in the same thread.
 */
const cv::Mat& JpegEncoder::convertToBgr(const uint8_t* yuyv, int width, int height,
                                      size_t bytesPerLine) {
    if (!yuyv || width <= 0 || height <= 0 || width % 2 ||
        (bytesPerLine && bytesPerLine < static_cast<size_t>(width) * 2))
        throw std::invalid_argument("Invalid YUYV image geometry");
    // Pass the driver row stride to OpenCV to account for padding.
    const size_t step = bytesPerLine == 0 ? static_cast<size_t>(cv::Mat::AUTO_STEP) : bytesPerLine;
    cv::Mat yuyv_mat(height, width, CV_8UC2, const_cast<uint8_t*>(yuyv), step);

    // Convert YUYV to BGR.
    cv::cvtColor(yuyv_mat, bgr_, cv::COLOR_YUV2BGR_YUYV);
    return bgr_;
}

/**
 * @brief  Encode a BGR image as JPEG.
 * @param  [in] bgr Nonempty CV_8UC3 input image.
 * @param  [in] quality JPEG quality in the range 1 to 100.
 * @retval JPEG Encoded image bytes.
 * @note   Invalid input throws std::invalid_argument; encoding failures propagate as exceptions.
 */
std::vector<uint8_t> JpegEncoder::encodeBgr(const cv::Mat& bgr, int quality) {
    if (bgr.empty() || bgr.type() != CV_8UC3 || quality < 1 || quality > 100)
        throw std::invalid_argument("Invalid BGR image or JPEG quality");
    // Compress BGR as JPEG.
    std::vector<uint8_t> jpeg;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, quality };
    if (!cv::imencode(".jpg", bgr, jpeg, params))
        throw std::runtime_error("JPEG encoding failed");

    return jpeg;
}
