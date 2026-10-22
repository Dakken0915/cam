# Linux 本机构建与 HTTP 集成测试
CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -g
LDFLAGS  = -lpthread

OPENCV_CFLAGS = $(shell pkg-config --cflags opencv4)
OPENCV_LIBS = $(shell pkg-config --libs opencv4)
CXXFLAGS += -I ./include

SRC_DIR = src
OBJ_DIR   = build

COMMON_SRCS = $(SRC_DIR)/v4l2.cpp $(SRC_DIR)/jpeg_encoder.cpp $(SRC_DIR)/http_server.cpp $(SRC_DIR)/framebuffer.cpp $(SRC_DIR)/touchscreen.cpp
HEADERS = $(wildcard include/*.hpp)

cam_server: $(OBJ_DIR)/cam_server

$(OBJ_DIR)/cam_server: $(SRC_DIR)/main.cpp $(COMMON_SRCS) $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $< $(COMMON_SRCS) -o $@ $(LDFLAGS) $(OPENCV_LIBS)

test_http_server: $(OBJ_DIR)/test_http_server
	$(OBJ_DIR)/test_http_server

$(OBJ_DIR)/test_http_server: tests/http_server_test.cpp $(SRC_DIR)/http_server.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) tests/http_server_test.cpp $(SRC_DIR)/http_server.cpp -o $@ -lpthread

test_video: $(OBJ_DIR)/test_video
	$(OBJ_DIR)/test_video

test_v4l2: $(OBJ_DIR)/test_v4l2
	$(OBJ_DIR)/test_v4l2

$(OBJ_DIR)/test_v4l2: tests/v4l2_test.cpp $(SRC_DIR)/v4l2.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) tests/v4l2_test.cpp $(SRC_DIR)/v4l2.cpp -o $@ -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=poll,--wrap=mmap,--wrap=munmap

$(OBJ_DIR)/test_video: tests/video_test.cpp $(SRC_DIR)/framebuffer.cpp $(SRC_DIR)/jpeg_encoder.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) tests/video_test.cpp $(SRC_DIR)/framebuffer.cpp $(SRC_DIR)/jpeg_encoder.cpp -o $@ $(LDFLAGS) $(OPENCV_LIBS)

run: cam_server
	$(OBJ_DIR)/cam_server

test: test_http_server test_video test_v4l2

clean:
	rm -rf $(OBJ_DIR)

.PHONY: cam_server run test test_http_server test_video test_v4l2 clean
