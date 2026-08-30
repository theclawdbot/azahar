// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace Frontend {

class EmuWindow;

/// Reads a touchscreen evdev device directly and feeds it to an EmuWindow as
/// framebuffer-space touch input. Used for screens driven through a DRM lease,
/// where no windowing system delivers input for the panel. The device is
/// grabbed (EVIOCGRAB) so the compositor stops seeing its events.
class DrmLeaseTouch {
public:
    /// device_path: an evdev node path, an evdev device name, or "auto" to
    /// pick the only direct-touch device (preferring one named like the
    /// bottom screen when several exist).
    DrmLeaseTouch(EmuWindow& window, std::string device_path);
    ~DrmLeaseTouch();

    DrmLeaseTouch(const DrmLeaseTouch&) = delete;
    DrmLeaseTouch& operator=(const DrmLeaseTouch&) = delete;

private:
    void Run(std::stop_token token);
    int OpenDevice();
    void ReadLoop(std::stop_token token, int fd);

    EmuWindow& window;
    std::string device_path;
    bool swap_xy = false;
    bool invert_x = false;
    bool invert_y = false;
    std::jthread thread;
};

} // namespace Frontend
