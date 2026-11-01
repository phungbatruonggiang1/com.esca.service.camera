// The D-Bus face of the service. Control only: no pixel ever crosses the bus.

#include "service.hpp"

#include <cstdio>

#include "esca/camera/client.hpp" // for the name/path/interface constants

namespace esca::camera {

namespace {
// Negative returns are errors; the detail is in GetErrorMessage().
constexpr int32_t kOk = 0;
constexpr int32_t kErr = -1;
} // namespace

CameraService::CameraService(sdbus::IConnection &connection)
    : connection_(connection)
{
    object_ = sdbus::createObject(connection_, sdbus::ObjectPath{ kObjectPath });

    object_->addVTable(
        sdbus::registerMethod("DeviceList").implementedAs([this] { return device_list(); }),
        sdbus::registerMethod("GetDeviceInfo")
            .implementedAs([this](const std::string &id) { return device_info(id); }),
        sdbus::registerMethod("Open")
            .implementedAs([this](const std::string &id) { return open(id); }),
        sdbus::registerMethod("Close")
            .implementedAs([this](const int32_t &handle) { return close(handle); }),
        sdbus::registerMethod("GetFormat")
            .implementedAs([this](const int32_t &handle) { return get_format(handle); }),
        sdbus::registerMethod("SetFormat")
            .implementedAs([this](const int32_t &handle,
                                  const sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t> &f) {
                return set_format(handle, f);
            }),
        sdbus::registerMethod("StartCamera")
            .implementedAs([this](const int32_t &handle) { return start(handle); }),
        sdbus::registerMethod("StopCamera")
            .implementedAs([this](const int32_t &handle, const bool &force) {
                return stop(handle, force);
            }),
        sdbus::registerMethod("GetBufferInfo")
            .implementedAs([this](const int32_t &handle) { return buffer_info(handle); }),
        sdbus::registerMethod("GetFd")
            .implementedAs([this](const int32_t &handle, const std::string &type) {
                return get_fd(handle, type);
            }),
        sdbus::registerMethod("GetErrorMessage").implementedAs([this] { return last_error_; }),
        sdbus::registerSignal("FaultEvent").withParameters<std::string, std::string>(),
        sdbus::registerSignal("DeviceAdded").withParameters<std::string>(),
        sdbus::registerSignal("DeviceRemoved").withParameters<std::string>())
        .forInterface(kInterface);
}

CameraService::~CameraService() = default;

bool CameraService::scan(std::string &error)
{
    // One HAL instance enumerates; each device later gets its own, because a
    // HAL owns the camera it opened.
    auto probe = make_libcamera_hal();
    const auto found = probe->enumerate();
    if (found.empty()) {
        error = "no cameras found";
        return false;
    }

    for (const auto &info : found) {
        std::fprintf(stderr, "esca-camerad: camera %s (%s, %s)\n", info.id.c_str(),
                     info.model.c_str(), info.location.c_str());
        devices_.emplace(info.id, std::make_unique<CameraDevice>(make_libcamera_hal(), info));
    }
    return true;
}

CameraDevice *CameraService::device_for(int32_t handle)
{
    auto it = handles_.find(handle);
    if (it == handles_.end())
        return nullptr;
    auto dev = devices_.find(it->second);
    return dev == devices_.end() ? nullptr : dev->second.get();
}

std::vector<std::string> CameraService::device_list()
{
    std::vector<std::string> ids;
    ids.reserve(devices_.size());
    for (const auto &[id, dev] : devices_)
        ids.push_back(id);
    return ids;
}

std::map<std::string, sdbus::Variant> CameraService::device_info(const std::string &id)
{
    std::map<std::string, sdbus::Variant> out;
    auto it = devices_.find(id);
    if (it == devices_.end()) {
        last_error_ = "no such camera: " + id;
        return out;
    }

    const DeviceInfo &info = it->second->info();
    out["id"] = sdbus::Variant(info.id);
    out["model"] = sdbus::Variant(info.model);
    out["location"] = sdbus::Variant(info.location);
    out["streaming"] = sdbus::Variant(it->second->streaming());

    std::vector<sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t>> formats;
    for (const FrameFormat &f : info.formats)
        formats.emplace_back(f.width, f.height, f.fps, f.fourcc);
    out["formats"] = sdbus::Variant(formats);
    return out;
}

int32_t CameraService::open(const std::string &id)
{
    auto it = devices_.find(id);
    if (it == devices_.end()) {
        last_error_ = "no such camera: " + id;
        return kErr;
    }

    std::string error;
    const int handle = it->second->add_client(error);
    if (handle < 0) {
        last_error_ = error;
        emit_fault(id, error);
        return kErr;
    }

    handles_[handle] = id;
    return handle;
}

int32_t CameraService::close(int32_t handle)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return kErr;
    }
    device->remove_client(handle);
    handles_.erase(handle);
    return kOk;
}

sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>
CameraService::get_format(int32_t handle)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return { 0, 0, 0, 0, 0 };
    }
    const FrameFormat f = device->get_format();
    return { f.width, f.height, f.stride, f.fourcc, f.fps };
}

int32_t CameraService::set_format(int32_t handle,
                                  const sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t> &in)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return kErr;
    }

    FrameFormat want;
    want.width = in.get<0>();
    want.height = in.get<1>();
    want.fps = in.get<2>();
    want.fourcc = in.get<3>();

    std::string error;
    if (!device->set_format(handle, want, error)) {
        last_error_ = error;
        return kErr;
    }
    return kOk;
}

int32_t CameraService::start(int32_t handle)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return kErr;
    }

    std::string error;
    if (!device->start(handle, error)) {
        last_error_ = error;
        emit_fault(handles_[handle], error);
        return kErr;
    }
    return kOk;
}

int32_t CameraService::stop(int32_t handle, bool force)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return kErr;
    }
    device->stop(handle, force);
    return kOk;
}

sdbus::Struct<std::string, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>
CameraService::buffer_info(int32_t handle)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return { "", 0, 0, 0, 0, 0, 0 };
    }

    const BufferInfo info = device->buffer_info();
    if (info.shm_name.empty())
        last_error_ = "camera is not streaming";

    return { info.shm_name,   info.format.width,  info.format.height, info.format.stride,
             info.format.fourcc, info.slot_count, info.slot_size };
}

sdbus::UnixFd CameraService::get_fd(int32_t handle, const std::string &type)
{
    CameraDevice *device = device_for(handle);
    if (!device) {
        last_error_ = "unknown handle";
        return sdbus::UnixFd(-1);
    }
    if (type != "sync") {
        // "frame" will return the dmabuf once the zero-copy path exists.
        last_error_ = "unsupported fd type: " + type;
        return sdbus::UnixFd(-1);
    }

    const int fd = device->signal_fd();
    if (fd < 0)
        last_error_ = "camera is not streaming";
    // UnixFd duplicates: the publisher keeps owning its end.
    return sdbus::UnixFd(fd);
}

void CameraService::emit_fault(const std::string &camera_id, const std::string &message)
{
    object_->emitSignal(sdbus::SignalName{ "FaultEvent" })
        .onInterface(sdbus::InterfaceName{ kInterface })
        .withArguments(camera_id, message);
}

} // namespace esca::camera
