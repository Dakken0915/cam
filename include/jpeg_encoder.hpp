#ifndef JPEG_ENCODER_HPP
#define JPEG_ENCODER_HPP

#include <vector>
#include <cstdint>
#include <cstddef>
#include <opencv2/opencv.hpp>

class JpegEncoder {
public:
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
    std::vector<uint8_t> encode(const uint8_t* yuyv, int width, int height,
                                int quality = 80, size_t bytesPerLine = 0);

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
    const cv::Mat& convertToBgr(const uint8_t* yuyv, int width, int height,
                              size_t bytesPerLine = 0);
    /**
     * @brief  Encode a BGR image as JPEG.
     * @param  [in] bgr Nonempty CV_8UC3 input image.
     * @param  [in] quality JPEG quality in the range 1 to 100.
     * @retval JPEG Encoded image bytes.
     * @note   Invalid input throws std::invalid_argument; encoding failures propagate as exceptions.
     */
    std::vector<uint8_t> encodeBgr(const cv::Mat& bgr, int quality = 80);

private:
    cv::Mat bgr_;  // Reused BGR storage avoids allocating a new buffer for each frame.
};

#endif
