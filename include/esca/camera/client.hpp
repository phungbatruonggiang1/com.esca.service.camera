// What applications link against. Hides sdbus-c++ and the shared-memory
// layout behind a plain C++ API, so an application never speaks D-Bus.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "esca/camera/buffer.hpp"

namespace esca::camera {

constexpr const char *kServiceName = "vn.sparc.esca.Camera1";
constexpr const char *kObjectPath = "/vn/sparc/esca/Camera1";
constexpr const char *kInterface = "vn.sparc.esca.Camera1";

class CameraClient {
public:
    CameraClient();
    ~CameraClient();

    CameraClient(const CameraClient &) = delete;
    CameraClient &operator=(const CameraClient &) = delete;

    /// Camera ids the service knows about.
    std::vector<std::string> device_list();

    /// Claim a camera. The handle identifies this client to the service for
    /// every later call; it is not a file descriptor.
    int open(const std::string &camera_id, std::string &error);
    void close();

    /// Only honoured while nothing streams; see README on format arbitration.
    bool set_format(const FrameFormat &format, std::string &error);
    FrameFormat get_format();

    /// Start the camera and attach to its frame buffer in one step: on its own
    /// StartCamera leaves the caller with nothing to read from.
    bool start(std::string &error);
    void stop();

    /// Block for the next frame and copy it out.
    bool next_frame(std::vector<uint8_t> &out, FrameInfo &info, int timeout_ms = 2000);

    /// Format of the attached buffer, valid after start().
    FrameFormat frame_format() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esca::camera
