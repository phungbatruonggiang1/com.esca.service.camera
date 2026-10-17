#include "esca/camera/client.hpp"

#include <chrono>
#include <unistd.h>

#include <sdbus-c++/sdbus-c++.h>

namespace esca::camera {

struct CameraClient::Impl {
    std::unique_ptr<sdbus::IProxy> proxy;
    Subscriber subscriber;
    int32_t handle = -1;
    int sync_fd = -1; ///< our own dup of the service's eventfd
    FrameFormat format;

    ~Impl()
    {
        if (sync_fd >= 0)
            ::close(sync_fd);
    }

    std::string service_error()
    {
        std::string message;
        try {
            proxy->callMethod("GetErrorMessage").onInterface(kInterface).storeResultsTo(message);
        } catch (const sdbus::Error &e) {
            message = e.getMessage();
        }
        return message;
    }
};

CameraClient::CameraClient() : impl_(std::make_unique<Impl>())
{
    // The system bus, explicitly. The plain createProxy() would ask sd-bus
    // for the default bus, which is the *session* bus whenever
    // DBUS_SESSION_BUS_ADDRESS is set -- and it is, for every application
    // the display manager starts. The service lives on the system bus, so a
    // client run as weston would otherwise look for it on the wrong bus and
    // be told, confusingly, that no camera exists.
    impl_->proxy = sdbus::createProxy(sdbus::createSystemBusConnection(),
                                      sdbus::ServiceName{ kServiceName },
                                      sdbus::ObjectPath{ kObjectPath });
}

CameraClient::~CameraClient()
{
    if (impl_ && impl_->handle >= 0)
        close();
}

std::vector<std::string> CameraClient::device_list()
{
    std::vector<std::string> ids;
    try {
        impl_->proxy->callMethod("DeviceList").onInterface(kInterface).storeResultsTo(ids);
    } catch (const sdbus::Error &) {
        // An absent service is a normal state for a client that starts early.
    }
    return ids;
}

int CameraClient::open(const std::string &camera_id, std::string &error)
{
    int32_t handle = -1;
    try {
        impl_->proxy->callMethod("Open")
            .onInterface(kInterface)
            .withArguments(camera_id)
            .storeResultsTo(handle);
    } catch (const sdbus::Error &e) {
        error = e.getMessage();
        return -1;
    }

    if (handle < 0) {
        error = impl_->service_error();
        return -1;
    }
    impl_->handle = handle;
    return handle;
}

void CameraClient::close()
{
    if (impl_->handle < 0)
        return;
    stop();
    try {
        int32_t ignored = 0;
        impl_->proxy->callMethod("Close")
            .onInterface(kInterface)
            .withArguments(impl_->handle)
            .storeResultsTo(ignored);
    } catch (const sdbus::Error &) {
    }
    impl_->handle = -1;
}

bool CameraClient::set_format(const FrameFormat &format, std::string &error)
{
    if (impl_->handle < 0) {
        error = "no camera open";
        return false;
    }

    int32_t rc = -1;
    sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t> want(format.width, format.height,
                                                               format.fps, format.fourcc);
    try {
        impl_->proxy->callMethod("SetFormat")
            .onInterface(kInterface)
            .withArguments(impl_->handle, want)
            .storeResultsTo(rc);
    } catch (const sdbus::Error &e) {
        error = e.getMessage();
        return false;
    }

    if (rc != 0) {
        error = impl_->service_error();
        return false;
    }
    return true;
}

FrameFormat CameraClient::get_format()
{
    FrameFormat out;
    if (impl_->handle < 0)
        return out;

    sdbus::Struct<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> f;
    try {
        impl_->proxy->callMethod("GetFormat")
            .onInterface(kInterface)
            .withArguments(impl_->handle)
            .storeResultsTo(f);
    } catch (const sdbus::Error &) {
        return out;
    }

    out.width = f.get<0>();
    out.height = f.get<1>();
    out.stride = f.get<2>();
    out.fourcc = f.get<3>();
    out.fps = f.get<4>();
    return out;
}

bool CameraClient::start(std::string &error)
{
    if (impl_->handle < 0) {
        error = "no camera open";
        return false;
    }

    int32_t rc = -1;
    try {
        impl_->proxy->callMethod("StartCamera")
            .onInterface(kInterface)
            .withArguments(impl_->handle)
            .storeResultsTo(rc);
    } catch (const sdbus::Error &e) {
        error = e.getMessage();
        return false;
    }
    if (rc != 0) {
        error = impl_->service_error();
        return false;
    }

    // Starting the camera and attaching to its buffer are one step for the
    // caller: StartCamera alone leaves them with nothing to read.
    sdbus::Struct<std::string, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t> info;
    try {
        impl_->proxy->callMethod("GetBufferInfo")
            .onInterface(kInterface)
            .withArguments(impl_->handle)
            .storeResultsTo(info);
    } catch (const sdbus::Error &e) {
        error = e.getMessage();
        return false;
    }

    const std::string shm_name = info.get<0>();
    if (shm_name.empty()) {
        error = impl_->service_error();
        return false;
    }

    impl_->format.width = info.get<1>();
    impl_->format.height = info.get<2>();
    impl_->format.stride = info.get<3>();
    impl_->format.fourcc = info.get<4>();

    sdbus::UnixFd sync;
    try {
        impl_->proxy->callMethod("GetFd")
            .onInterface(kInterface)
            .withArguments(impl_->handle, std::string("sync"))
            .storeResultsTo(sync);
    } catch (const sdbus::Error &e) {
        error = e.getMessage();
        return false;
    }

    // sync closes its fd when it goes out of scope, so keep our own copy.
    if (sync.get() >= 0)
        impl_->sync_fd = ::dup(sync.get());

    if (!impl_->subscriber.open(shm_name, impl_->sync_fd)) {
        error = "cannot attach to " + shm_name;
        return false;
    }
    return true;
}

void CameraClient::stop()
{
    impl_->subscriber.close();
    if (impl_->sync_fd >= 0) {
        ::close(impl_->sync_fd);
        impl_->sync_fd = -1;
    }
    if (impl_->handle < 0)
        return;

    try {
        int32_t ignored = 0;
        impl_->proxy->callMethod("StopCamera")
            .onInterface(kInterface)
            .withArguments(impl_->handle, false)
            .storeResultsTo(ignored);
    } catch (const sdbus::Error &) {
    }
}

bool CameraClient::next_frame(std::vector<uint8_t> &out, FrameInfo &info, int timeout_ms)
{
    // read() returns false both for "nothing new" and for "the writer
    // overwrote that slot while I was copying it". The second is normal for a
    // slow consumer -- an inference client spends longer on a frame than the
    // camera takes to fill the ring -- and the answer is to take the next
    // frame, not to give up. Only the deadline ends the loop.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;

        const int remaining =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                                 .count());
        if (!impl_->subscriber.wait(remaining))
            return false;
        if (impl_->subscriber.read(out, info))
            return true;
    }
}

FrameFormat CameraClient::frame_format() const
{
    const FrameFormat from_buffer = impl_->subscriber.format();
    return from_buffer.valid() ? from_buffer : impl_->format;
}

} // namespace esca::camera
