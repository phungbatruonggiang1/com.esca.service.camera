# ESCA Camera Service

One process owns the camera; several applications read its frames. A camera
admits a single owner, so without a service in the middle the dashboard, a
recorder and a vision job cannot coexist.

The design, and what it borrows from the ATLAS Camera Framework, is in
`meta-sparc-esca/docs/camera-service-design.md`. This is phase 1 of it.

## Shape

    application ──► libesca-camclient ──D-Bus──► esca-camerad ──► libesca-camhal ──► libcamera
                            │                         │
                            └────── shared memory ◄───┘  (libesca-cambuf)

Control messages — list, open, format, start, stop — go over D-Bus. Pixels
never do: the service publishes frames into a shared-memory ring and hands
clients an eventfd to wait on.

| Artifact | What it is |
| --- | --- |
| `/usr/sbin/esca-camerad` | The service. Owns the device list and the camera |
| `/usr/lib/libesca-camhal.so` | HAL, implemented on libcamera |
| `/usr/lib/libesca-cambuf.so` | The frame ring: publisher, subscriber, signal fd |
| `/usr/lib/libesca-camclient.so` | What applications link; hides D-Bus |
| `/usr/bin/esca-camctl` | CLI client, and the way to test without a UI |

## Trying it

    esca-camctl list                    # camera ids the service found
    esca-camctl watch                   # 30 frames, printing sequence and size
    esca-camctl grab /tmp/frame.yuv     # one frame to a file

Two `watch` processes at once is the point of the whole thing: both get
frames, and neither disturbs the other.

## D-Bus interface

`vn.sparc.esca.Camera1` on the system bus, object `/vn/sparc/esca/Camera1`.

    DeviceList()                     -> as
    GetDeviceInfo(s id)              -> a{sv}    id, model, location, streaming, formats
    Open(s id)                       -> i        handle, or negative on failure
    Close(i handle)                  -> i
    GetFormat(i handle)              -> (uuuuu)  width, height, stride, fourcc, fps
    SetFormat(i handle, (uuuu) fmt)  -> i        width, height, fps, fourcc
    StartCamera(i handle)            -> i
    StopCamera(i handle, b force)    -> i
    GetBufferInfo(i handle)          -> (suuuuuu) shm name, w, h, stride, fourcc, slots, slot size
    GetFd(i handle, s type)          -> h        "sync" eventfd today
    GetErrorMessage()                -> s        detail for the last failed call

Signals: `FaultEvent(s, s)`, `DeviceAdded(s)`, `DeviceRemoved(s)`.

Methods return a status rather than throwing a D-Bus error, and the detail
comes from `GetErrorMessage()` — the ATLAS client API works the same way, and
it keeps the client library free of exception translation.

## Decisions worth knowing about

**Frames are copied, once.** libcamera hands out dmabuf-backed buffers; the
service mmaps them and copies each frame into the ring. The zero-copy path —
passing the dmabuf fd to clients over `GetFd("frame")` — is phase 5. One copy
of a 1.3 MB frame is affordable; the complexity of fd lifetime across clients
is not, yet.

**Readers never block the camera.** The writer fills slot `seq % 4` and
publishes the sequence number; a reader takes the newest completed slot and
skips anything it missed. A dashboard wants the current frame, not a backlog.

**Format arbitration is blunt.** The first client to stream fixes the format;
a second `SetFormat` is refused while anyone streams, rather than
reconfiguring the stream under them. The design document lists the kinder
options; this one is at least predictable.

**The service runs as root** because libcamera needs `/dev/video*`,
`/dev/media*` and `/dev/dma_heap/*`. Clients do not: D-Bus policy grants the
`camera` group access, and the recipe creates that group.

**One process, for now.** ATLAS spawns a service per camera so a HAL crash
cannot take the framework with it. Here `CameraDevice` is that seam but runs
in-process; until it moves out, a HAL fault kills the service and systemd
restarts it.

## Building

Inside the Yocto build it is the `esca-camera` recipe in `meta-sparc-esca`.
Out of tree:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build

It needs `libcamera` and `sdbus-c++` development packages, so a plain desktop
host will not have them unless you install both.
