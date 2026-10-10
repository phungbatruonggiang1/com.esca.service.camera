#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>

#include <sdbus-c++/sdbus-c++.h>

#include "esca/camera/client.hpp" // kServiceName
#include "service.hpp"

namespace {

sdbus::IConnection *g_connection = nullptr;

void handle_signal(int)
{
    // Leaving the event loop unwinds the service, which releases the camera.
    // Dying with it held leaves the sensor unusable until the kernel reaps us.
    if (g_connection)
        g_connection->leaveEventLoop();
}

} // namespace

int main()
{
    using namespace esca::camera;

    try {
        auto connection = sdbus::createBusConnection(sdbus::ServiceName{ kServiceName });
        g_connection = connection.get();

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        CameraService service(*connection);

        std::string error;
        if (!service.scan(error)) {
            // Not fatal: the service still answers DeviceList with an empty
            // list, which is a better failure than a bus name that never
            // appears and clients that hang waiting for it.
            std::fprintf(stderr, "esca-camerad: %s\n", error.c_str());
        }

        std::fprintf(stderr, "esca-camerad: ready on %s\n", kServiceName);
        connection->enterEventLoop();
        std::fprintf(stderr, "esca-camerad: shutting down\n");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "esca-camerad: fatal: %s\n", e.what());
        return 1;
    }
}
