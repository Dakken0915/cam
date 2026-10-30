#include "http_server.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Throw when an assertion is false.
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Connect to the local server with a receive timeout.
int connectToServer(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    require(fd >= 0, "socket() failed");

    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    close(fd);
    throw std::runtime_error("connect() failed");
}

// Send an HTTP request and read its response.
std::string request(int port, const std::string& method, const std::string& path,
                    bool stopAfterJpeg = false) {
    int fd = connectToServer(port);
    const std::string payload = method + " " + path + " HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n\r\n";
    require(send(fd, payload.data(), payload.size(), 0) ==
                static_cast<ssize_t>(payload.size()),
            "send() failed");

    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        response.append(buffer, static_cast<size_t>(bytes));

        if (stopAfterJpeg && response.find("\xff\xd8\xff\xd9") != std::string::npos) {
            break;
        }
    }
    close(fd);
    return response;
}

// Send a raw request line for validation tests.
std::string rawRequest(int port, const std::string& requestLine) {
    int fd = connectToServer(port);
    const std::string payload = requestLine + "\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    require(send(fd, payload.data(), payload.size(), 0) == static_cast<ssize_t>(payload.size()),
            "raw request send() failed");
    std::string response;
    char buffer[4096];
    while (true) {
        const ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) break;
        response.append(buffer, static_cast<size_t>(bytes));
    }
    close(fd);
    return response;
}

// List regular snapshot files.
std::vector<std::string> regularFiles(const std::string& directory) {
    std::vector<std::string> result;
    DIR* dir = opendir(directory.c_str());
    require(dir != nullptr, "snapshot directory was not created");

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const std::string path = directory + "/" + entry->d_name;
        struct stat info{};
        if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
            result.push_back(path);
        }
    }
    closedir(dir);
    return result;
}

}  // namespace

// Test routes, snapshots, quotas, validation, and lifecycle.
int main() {
    char tempTemplate[] = "/tmp/cam-http-test-XXXXXX";
    char* tempDir = mkdtemp(tempTemplate);
    require(tempDir != nullptr, "mkdtemp() failed");
    const std::string snapshotDir = std::string(tempDir) + "/snapshots";

    const int port = 18080 + (static_cast<int>(getpid()) % 1000);
    HttpServer server(snapshotDir, 1);
    require(server.start(port), "HTTP server failed to start");

    const std::string page = request(port, "GET", "/");
    require(page.find("HTTP/1.1 200 OK") == 0, "root route must return 200");
    require(page.find("Content-Type: text/html; charset=utf-8") != std::string::npos,
            "root route must return HTML");
    require(page.find("/stream") != std::string::npos,
            "root page must show the MJPEG stream");
    require(page.find("/snapshot") != std::string::npos,
            "root page must provide a snapshot action");

    const std::string unavailable = request(port, "GET", "/snapshot");
    require(unavailable.find("HTTP/1.1 503 Service Unavailable") == 0,
            "snapshot without a frame must return 503");

    const std::vector<uint8_t> jpeg{0xff, 0xd8, 0xff, 0xd9};
    server.updateFrame(jpeg);
    server.updateFrame({});

    const std::string snapshot = request(port, "GET", "/snapshot");
    require(snapshot.find("HTTP/1.1 200 OK") == 0,
            "snapshot route must return 200");
    require(snapshot.find("Content-Type: image/jpeg") != std::string::npos,
            "snapshot route must return JPEG");
    require(snapshot.find("Content-Disposition: attachment; filename=\"snapshot_") !=
                std::string::npos,
            "snapshot response must have a download filename");
    require(snapshot.find("\xff\xd8\xff\xd9") != std::string::npos,
            "snapshot response must contain the current JPEG");

    const std::vector<std::string> saved = regularFiles(snapshotDir);
    require(saved.size() == 1, "snapshot route must save exactly one JPEG");
    require(saved.front().size() >= 4 &&
                saved.front().substr(saved.front().size() - 4) == ".jpg",
            "saved snapshot must use the .jpg extension");
    std::ifstream savedFile(saved.front(), std::ios::binary);
    const std::vector<uint8_t> savedJpeg((std::istreambuf_iterator<char>(savedFile)),
                                         std::istreambuf_iterator<char>());
    require(savedJpeg == jpeg, "saved snapshot must contain the current JPEG");

    const std::string full = request(port, "GET", "/snapshot");
    require(full.find("HTTP/1.1 507 Insufficient Storage") == 0,
            "snapshot route must reject writes after its storage limit");

    const std::string stream = request(port, "GET", "/stream", true);
    require(stream.find("Content-Type: multipart/x-mixed-replace; boundary=frame") !=
                std::string::npos,
            "stream route must return multipart MJPEG");
    require(stream.find("\xff\xd8\xff\xd9") != std::string::npos,
            "stream route must contain the current JPEG");

    const std::string notFound = request(port, "GET", "/missing");
    require(notFound.find("HTTP/1.1 404 Not Found") == 0,
            "unknown routes must return 404");

    const std::string wrongMethod = request(port, "POST", "/snapshot");
    require(wrongMethod.find("HTTP/1.1 405 Method Not Allowed") == 0,
            "non-GET requests must return 405");

    const std::string badRequest = rawRequest(port, "GET / HTTP/2.0 extra");
    require(badRequest.find("HTTP/1.1 400 Bad Request") == 0,
            "invalid HTTP request line must return 400");
    const std::string tooLarge = rawRequest(port, "GET /" + std::string(8300, 'x') + " HTTP/1.1");
    require(tooLarge.find("HTTP/1.1 431 Request Header Fields Too Large") == 0,
            "oversized request must return 431");

    server.stop();
    server.stop();
    require(server.start(port), "HTTP server restart failed");
    require(request(port, "GET", "/snapshot").find("HTTP/1.1 503 Service Unavailable") == 0,
            "restart must not serve an old camera frame");
    const int idle = connectToServer(port);
    server.stop();
    close(idle);

    HttpServer limited(snapshotDir, 1, 1);
    require(limited.start(port), "limited HTTP server failed to start");
    const int streamFd = connectToServer(port);
    const std::string streamRequest = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
    require(send(streamFd, streamRequest.data(), streamRequest.size(), MSG_NOSIGNAL) ==
                static_cast<ssize_t>(streamRequest.size()), "stream request failed");
    char streamHeader[1024];
    require(recv(streamFd, streamHeader, sizeof(streamHeader), 0) > 0,
            "empty stream did not send headers");
    require(request(port, "GET", "/").find("HTTP/1.1 503 Service Unavailable") == 0,
            "client limit was not enforced");
    close(streamFd);
    bool released = false;
    for (int attempt = 0; attempt < 50 && !released; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        released = request(port, "GET", "/").find("HTTP/1.1 200 OK") == 0;
    }
    require(released, "disconnected empty stream retained its client slot");
    limited.stop();

    for (const std::string& path : saved) unlink(path.c_str());
    rmdir(snapshotDir.c_str());
    rmdir(tempDir);

    std::cout << "HTTP server tests passed" << std::endl;
    return 0;
}
