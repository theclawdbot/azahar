// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "citra_qt/drm_lease/drm_lease_client.h"
#include "citra_qt/drm_lease/drm_lease_touch.h"
#include "citra_qt/drm_lease/drm_lease_window.h"
#include "common/logging/log.h"
#include "core/frontend/framebuffer_layout.h"

namespace Frontend {

namespace {

// The preferred (or first) mode of the leased connector; VK_KHR_display picks
// its mode independently, this only sizes the framebuffer layout.
bool ReadConnectorMode(int lease_fd, u32 connector_id, u32& width, u32& height) {
    drmModeConnector* connector = drmModeGetConnector(lease_fd, connector_id);
    if (!connector || connector->count_modes < 1) {
        if (connector) {
            drmModeFreeConnector(connector);
        }
        return false;
    }

    drmModeModeInfo mode = connector->modes[0];
    for (int i = 0; i < connector->count_modes; i++) {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            mode = connector->modes[i];
            break;
        }
    }

    width = mode.hdisplay;
    height = mode.vdisplay;
    drmModeFreeConnector(connector);
    return true;
}

// Content rotation from the connector's "panel orientation" property, or -1
// when absent. Left/Right Side Up map per the compositor convention.
int ReadPanelOrientation(int lease_fd, u32 connector_id) {
    drmModeObjectProperties* props =
        drmModeObjectGetProperties(lease_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props) {
        return -1;
    }

    int rotation = -1;
    for (u32 i = 0; i < props->count_props && rotation < 0; i++) {
        drmModePropertyRes* prop = drmModeGetProperty(lease_fd, props->props[i]);
        if (!prop) {
            continue;
        }
        if (std::strcmp(prop->name, "panel orientation") == 0) {
            for (int j = 0; j < prop->count_enums; j++) {
                if (prop->enums[j].value != props->prop_values[i]) {
                    continue;
                }
                const char* name = prop->enums[j].name;
                if (std::strcmp(name, "Normal") == 0) {
                    rotation = 0;
                } else if (std::strcmp(name, "Upside Down") == 0) {
                    rotation = 180;
                } else if (std::strcmp(name, "Left Side Up") == 0) {
                    rotation = 90;
                } else if (std::strcmp(name, "Right Side Up") == 0) {
                    rotation = 270;
                }
                break;
            }
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return rotation;
}

} // namespace

DrmLeaseWindow::DrmLeaseWindow() : EmuWindow(true) {}

void DrmLeaseWindow::PollEvents() {
    if (client) {
        client->PumpEvents();
    }
}

DrmLeaseWindow::~DrmLeaseWindow() = default;

bool DrmLeaseWindow::Initialize(const std::string& connector_name, int rotation,
                                const std::string& touch_device, bool internal_only) {
    client = std::make_unique<DrmLeaseClient>();
    if (!client->Acquire(connector_name, internal_only)) {
        client.reset();
        return false;
    }

    if (!ReadConnectorMode(client->GetLeaseFd(), client->GetConnectorId(), mode_width,
                           mode_height)) {
        LOG_ERROR(Frontend, "drm-lease: failed to read a mode from the leased connector");
        client.reset();
        return false;
    }

    if (rotation < 0) {
        rotation = ReadPanelOrientation(client->GetLeaseFd(), client->GetConnectorId());
        if (rotation < 0) {
            rotation = 0;
            LOG_INFO(Frontend,
                     "drm-lease: connector reports no panel orientation, assuming unrotated; "
                     "set AZAHAR_DRM_LEASE_ROTATION if the output is sideways");
        } else {
            LOG_INFO(Frontend, "drm-lease: panel orientation property gives rotation {}", rotation);
        }
    }
    rotate90 = rotation == 90 || rotation == 270;
    flip180 = rotation == 180 || rotation == 270;

    window_info.type = WindowSystemType::Drm;
    window_info.drm_lease_fd = client->GetLeaseFd();
    window_info.drm_connector_id = client->GetConnectorId();
    window_info.drm_mode_width = mode_width;
    window_info.drm_mode_height = mode_height;

    UpdateCurrentFramebufferLayout(mode_width, mode_height);

    if (!touch_device.empty()) {
        touch = std::make_unique<DrmLeaseTouch>(*this, touch_device);
    }

    LOG_INFO(Frontend, "drm-lease: secondary output ready, {}x{} rotate90={} flip180={}",
             mode_width, mode_height, rotate90, flip180);
    return true;
}

void DrmLeaseWindow::UpdateCurrentFramebufferLayout(unsigned width, unsigned height,
                                                    bool is_portrait_mode) {
    // Bottom-screen-only layout in the panel's fixed mode.
    Layout::FramebufferLayout layout =
        Layout::SeparateWindowsLayout(mode_width, mode_height, true, rotate90);
    layout.is_flipped = flip180;
    NotifyFramebufferLayoutChanged(layout);
}

} // namespace Frontend
