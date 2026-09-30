// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include "core/frontend/emu_window.h"

namespace Frontend {

class DrmLeaseClient;
class DrmLeaseDumbProof;
class DrmLeaseTouch;

/// Secondary EmuWindow backed by a DRM lease instead of a windowing system.
/// The Vulkan renderer presents to it through VK_KHR_display; touch for the
/// panel is read directly from evdev.
class DrmLeaseWindow final : public EmuWindow {
public:
    DrmLeaseWindow();
    ~DrmLeaseWindow() override;

    /// Leases a connector and derives the layout from the panel's mode.
    /// connector_name may be empty to accept the first eligible offer;
    /// internal_only restricts auto-selection to internal panel types.
    /// rotation is the content rotation in degrees (0/90/180/270), or -1 to
    /// pick automatically from the panel orientation.
    bool Initialize(const std::string& connector_name, int rotation,
                    const std::string& touch_device, bool internal_only);

    bool IsDumbProof() const noexcept {
        return dumb_proof != nullptr;
    }

    void PollEvents() override;

    // The panel's mode and rotation are fixed; ignore settings-driven updates
    // so a global layout apply cannot clobber the lease output.
    void UpdateCurrentFramebufferLayout(unsigned width, unsigned height,
                                        bool is_portrait_mode = {}) override;

    // Scales a decoded bottom-screen frame into the persistent dumb buffer.
    // A no-op unless AZAHAR_DRM_LEASE_DUMB_PROOF latched the scanout.
    void PresentCpuFrame(const u8* pixels, u32 width, u32 height, bool rotate90,
                         bool flip180) override;

private:
    std::unique_ptr<DrmLeaseClient> client;
    // Holds the latched dumb framebuffer for AZAHAR_DRM_LEASE_DUMB_PROOF.
    // Defined in the cpp so this header does not include xf86drmMode.h.
    std::unique_ptr<DrmLeaseDumbProof> dumb_proof;
    std::unique_ptr<DrmLeaseTouch> touch;
    u32 mode_width = 0;
    u32 mode_height = 0;
    bool rotate90 = false;
    bool flip180 = false;
};

} // namespace Frontend
