// One camera, its HAL and its frame buffer, with the client bookkeeping that
// lets several applications share it.
//
// In the ATLAS design this lives in a process of its own, spawned per camera,
// so a HAL crash cannot take the service with it. Phase 1 keeps it in-process:
// the seam is this class, and moving it out is a packaging change rather than
// a redesign. Until then, a HAL fault takes the service down and systemd
// restarts it -- which the clients already have to cope with.

#pragma once

#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "esca/camera/buffer.hpp"
#include "esca/camera/hal.hpp"

namespace esca::camera {

class CameraDevice {
public:
    CameraDevice(std::unique_ptr<IHal> hal, DeviceInfo info);
    ~CameraDevice();

    const DeviceInfo &info() const { return info_; }

    /// Register a client. Returns a handle, or -1 if the HAL refuses.
    int add_client(std::string &error);
    void remove_client(int handle);
    bool has_client(int handle) const;

    bool set_format(int handle, const FrameFormat &format, std::string &error);
    FrameFormat get_format() const;

    /// Starts the camera on the first streaming client; later clients just
    /// attach to the buffer that already exists.
    bool start(int handle, std::string &error);
    /// Stops on the last streaming client, unless force is set.
    void stop(int handle, bool force);

    bool streaming() const;
    BufferInfo buffer_info() const;
    int signal_fd() const;

private:
    void on_frame(const uint8_t *data, uint32_t size, uint64_t timestamp_ns);

    mutable std::mutex mutex_;
    std::unique_ptr<IHal> hal_;
    DeviceInfo info_;
    std::unique_ptr<Publisher> publisher_;

    std::set<int> clients_;
    std::set<int> streamers_;
    int next_handle_ = 1;
    std::string last_error_;
};

} // namespace esca::camera
