#ifndef __FRAME_H
#define __FRAME_H

#include <iostream>
#include <cstdint>
#include <vector>

struct Frame {
    std::vector<uint8_t> data;   /* YUYV or JPEG payload. */
    uint32_t     sequence = 0;
    int          width    = 0;
    int          height   = 0;
    int          bytes_per_line = 0;
    int64_t      timestamp_us = 0;
};

#endif
