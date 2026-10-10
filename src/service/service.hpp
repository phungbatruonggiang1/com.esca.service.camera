#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "device.hpp"

namespace esca::camera {

class CameraService {
public:
    explicit CameraService(sdbus::IConnection &connection);
    ~CameraService();

    /// Enumerate cameras and build a CameraDevice for each. Called once at
    /// startup; a udev notifier would call it again, which is why it is not
    /// folded into the constructor.
    bool scan(std::string &error);

private:
    std::vector<std::string> device_list();
    std::map<std::string, sdbus::Variant> device_info(const std::string &id);
    int32_t open(const std::string &id);
    int32_t close(int32_t handle);
    sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> get_format(int32_t handle);
    int32_t set_format(int32_t handle,
                       const sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t> &format);
    int32_t start(int32_t handle);
    int32_t stop(int32_t handle, bool force);
    sdbus::Struct<std::string, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>
    buffer_info(int32_t handle);
    sdbus::UnixFd get_fd(int32_t handle, const std::string &type);

    void emit_fault(const std::string &camera_id, const std::string &message);
    CameraDevice *device_for(int32_t handle);

    sdbus::IConnection &connection_;
    std::unique_ptr<sdbus::IObject> object_;
    std::map<std::string, std::unique_ptr<CameraDevice>> devices_;
    std::map<int32_t, std::string> handles_; ///< handle -> camera id
    std::string last_error_;
};

} // namespace esca::camera
