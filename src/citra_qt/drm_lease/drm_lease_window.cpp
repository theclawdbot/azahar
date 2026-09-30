// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

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

// Direct libdrm/KMS solid-frame proof. Modesets the already-leased connector
// from the lease fd and latches one dumb framebuffer. Never calls
// getDrmDisplayEXT / acquireDrmDisplayEXT / vkCreateDisplayPlaneSurfaceKHR.
class DrmLeaseDumbProof {
public:
    DrmLeaseDumbProof(int fd_, u32 connector_id_) : fd{fd_}, connector_id{connector_id_} {}

    ~DrmLeaseDumbProof() {
        DisableScanout();
    }

    DrmLeaseDumbProof(const DrmLeaseDumbProof&) = delete;
    DrmLeaseDumbProof& operator=(const DrmLeaseDumbProof&) = delete;

    bool Run() {
        if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof failed to enable universal planes: {}",
                      std::strerror(errno));
            return false;
        }

        const int atomic_rc = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        if (atomic_rc == 0) {
            atomic = true;
        } else if (errno == EINVAL || errno == EOPNOTSUPP) {
            atomic = false;
            LOG_INFO(Frontend, "drm-lease: dumb proof using legacy modeset ({})",
                     std::strerror(errno));
        } else {
            LOG_ERROR(Frontend, "drm-lease: dumb proof atomic cap failed: {}",
                      std::strerror(errno));
            return false;
        }

        drmModeRes* res = drmModeGetResources(fd);
        if (!res) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof drmModeGetResources failed: {}",
                      std::strerror(errno));
            return false;
        }
        if (res->count_crtcs == 0) {
            LOG_ERROR(Frontend,
                      "drm-lease: dumb proof lease has no CRTC (connector {}); compositor leased "
                      "a connector without a CRTC",
                      connector_id);
            drmModeFreeResources(res);
            return false;
        }

        drmModeConnector* connector = drmModeGetConnector(fd, connector_id);
        if (!connector || connector->count_modes < 1) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof connector {} has no modes", connector_id);
            if (connector) {
                drmModeFreeConnector(connector);
            }
            drmModeFreeResources(res);
            return false;
        }

        mode = connector->modes[0];
        for (int i = 0; i < connector->count_modes; i++) {
            if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
                mode = connector->modes[i];
                break;
            }
        }

        if (!ChooseCrtc(res, connector)) {
            drmModeFreeConnector(connector);
            drmModeFreeResources(res);
            return false;
        }
        drmModeFreeConnector(connector);

        if (atomic && !ChoosePlane(res)) {
            drmModeFreeResources(res);
            return false;
        }
        drmModeFreeResources(res);

        if (crtc_id == 0 || (atomic && plane_id == 0)) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof refusing modeset, crtc {} plane {}", crtc_id,
                      plane_id);
            return false;
        }

        LOG_INFO(Frontend,
                 "drm-lease: dumb proof connector {} crtc {} plane {} format {:#x} mode {}x{}",
                 connector_id, crtc_id, plane_id, format, mode.hdisplay, mode.vdisplay);

        if (!CreateFramebuffer()) {
            return false;
        }
        return Modeset();
    }

private:
    static bool IsNegativeErrno(int rc, int err) {
        return rc == -err || (rc == -1 && errno == err);
    }

    bool ChooseCrtc(drmModeRes* res, drmModeConnector* connector) {
        if (connector->encoder_id != 0) {
            drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoder_id);
            if (encoder) {
                if (encoder->crtc_id != 0) {
                    crtc_id = encoder->crtc_id;
                    drmModeFreeEncoder(encoder);
                    return true;
                }
                drmModeFreeEncoder(encoder);
            }
        }

        for (int i = 0; i < connector->count_encoders; i++) {
            drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoders[i]);
            if (!encoder) {
                continue;
            }
            for (int c = 0; c < res->count_crtcs; c++) {
                if (encoder->possible_crtcs & (1u << c)) {
                    crtc_id = res->crtcs[c];
                    drmModeFreeEncoder(encoder);
                    return crtc_id != 0;
                }
            }
            drmModeFreeEncoder(encoder);
        }

        LOG_ERROR(Frontend, "drm-lease: dumb proof found no CRTC for connector {}", connector_id);
        return false;
    }

    static bool PlaneSupportsFormat(const drmModePlane* plane, u32 fmt) {
        for (u32 i = 0; i < plane->count_formats; i++) {
            if (plane->formats[i] == fmt) {
                return true;
            }
        }
        return false;
    }

    // Primary plane type is the "type" enum property, not a field on drmModePlane.
    static bool PlaneType(int fd, u32 plane_id, u64& type_out) {
        drmModeObjectProperties* props =
            drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
        if (!props) {
            return false;
        }
        bool found = false;
        for (u32 i = 0; i < props->count_props; i++) {
            drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
            if (!prop) {
                continue;
            }
            if (std::strcmp(prop->name, "type") == 0) {
                type_out = props->prop_values[i];
                found = true;
            }
            drmModeFreeProperty(prop);
            if (found) {
                break;
            }
        }
        drmModeFreeObjectProperties(props);
        return found;
    }

    bool ChoosePlane(drmModeRes* res) {
        int crtc_index = -1;
        for (int i = 0; i < res->count_crtcs; i++) {
            if (res->crtcs[i] == crtc_id) {
                crtc_index = i;
                break;
            }
        }
        if (crtc_index < 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof CRTC {} is not in the lease resources",
                      crtc_id);
            return false;
        }

        drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
        if (!planes) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof drmModeGetPlaneResources failed: {}",
                      std::strerror(errno));
            return false;
        }

        u32 fallback = 0;
        u32 fallback_format = 0;
        for (u32 i = 0; i < planes->count_planes; i++) {
            drmModePlane* plane = drmModeGetPlane(fd, planes->planes[i]);
            if (!plane) {
                continue;
            }
            const bool compatible = (plane->possible_crtcs & (1u << crtc_index)) != 0;
            u64 type = 0;
            const bool primary = compatible && PlaneType(fd, plane->plane_id, type) &&
                                 type == DRM_PLANE_TYPE_PRIMARY;
            if (primary) {
                const bool xrgb = PlaneSupportsFormat(plane, DRM_FORMAT_XRGB8888);
                const bool argb = PlaneSupportsFormat(plane, DRM_FORMAT_ARGB8888);
                if (xrgb || argb) {
                    const u32 fmt = xrgb ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_ARGB8888;
                    if (plane->crtc_id == crtc_id && xrgb) {
                        plane_id = plane->plane_id;
                        format = fmt;
                        drmModeFreePlane(plane);
                        break;
                    }
                    if (fallback == 0) {
                        fallback = plane->plane_id;
                        fallback_format = fmt;
                    }
                }
            }
            drmModeFreePlane(plane);
        }
        drmModeFreePlaneResources(planes);

        if (plane_id == 0) {
            plane_id = fallback;
            format = fallback_format;
        }
        if (plane_id == 0) {
            LOG_ERROR(Frontend,
                      "drm-lease: dumb proof found no Primary XRGB8888/ARGB8888 plane for CRTC {}",
                      crtc_id);
            return false;
        }
        return true;
    }

    bool CreateFramebuffer() {
        drm_mode_create_dumb creq{};
        creq.width = mode.hdisplay;
        creq.height = mode.vdisplay;
        creq.bpp = 32;
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof CREATE_DUMB failed: {}",
                      std::strerror(errno));
            return false;
        }
        dumb_handle = creq.handle;
        dumb_size = creq.size;

        const u32 handles[4] = {creq.handle, 0, 0, 0};
        const u32 pitches[4] = {creq.pitch, 0, 0, 0};
        const u32 offsets[4] = {0, 0, 0, 0};
        if (drmModeAddFB2(fd, creq.width, creq.height, format, handles, pitches, offsets, &fb_id,
                          0) != 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof AddFB2 format {:#x} pitch {} failed: {}",
                      format, creq.pitch, std::strerror(errno));
            return false;
        }

        drm_mode_map_dumb mreq{};
        mreq.handle = creq.handle;
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof MAP_DUMB failed: {}", std::strerror(errno));
            return false;
        }
        void* map = mmap(nullptr, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
        if (map == MAP_FAILED) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof mmap failed: {}", std::strerror(errno));
            return false;
        }

        if ((creq.pitch % sizeof(u32)) != 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof invalid pitch {}", creq.pitch);
            munmap(map, creq.size);
            return false;
        }

        mapped = static_cast<u8*>(map);
        mapped_size = creq.size;
        pitch = creq.pitch;
        width = creq.width;
        height = creq.height;

        // XRGB8888 and ARGB8888 are both little-endian A:R:G:B. Alpha is 0xff
        // so an ARGB-only plane is still opaque orange (0x00ff4020 is XRGB).
        // The mapping stays for the lifetime of the proof so later CPU frames
        // can replace this latch without another modeset.
        const u32 pixel = format == DRM_FORMAT_ARGB8888 ? 0xffff4020u : 0x00ff4020u;
        for (u32 y = 0; y < height; y++) {
            auto* row = reinterpret_cast<u32*>(mapped + static_cast<u64>(y) * pitch);
            for (u32 x = 0; x < width; x++) {
                row[x] = pixel;
            }
        }
        return true;
    }

public:
    void Present(const u8* pixels, u32 src_width, u32 src_height, bool rotate90, bool flip180) {
        if (!mapped || !pixels || src_width == 0 || src_height == 0 || pitch < width * sizeof(u32) ||
            static_cast<u64>(pitch) * height > mapped_size) {
            return;
        }

        const u32 content_w = rotate90 ? src_height : src_width;
        const u32 content_h = rotate90 ? src_width : src_height;
        if (content_w > width || content_h > height) {
            return;
        }
        const u32 scale = std::min(width / content_w, height / content_h);
        const u32 dest_w = content_w * scale;
        const u32 dest_h = content_h * scale;
        const u32 origin_x = (width - dest_w) / 2;
        const u32 origin_y = (height - dest_h) / 2;

        for (u32 y = 0; y < height; y++) {
            auto* row = reinterpret_cast<u32*>(mapped + static_cast<u64>(y) * pitch);
            const bool in_y = y >= origin_y && y < origin_y + dest_h;
            const u32 dest_y = in_y ? (y - origin_y) / scale : 0;
            for (u32 x = 0; x < width; x++) {
                if (!in_y || x < origin_x || x >= origin_x + dest_w) {
                    row[x] = 0;
                    continue;
                }
                const u32 dest_x = (x - origin_x) / scale;
                u32 src_x = dest_x;
                u32 src_y = dest_y;
                if (rotate90) {
                    if (!flip180) {
                        // 90 degrees clockwise into the panel.
                        src_x = dest_y;
                        src_y = src_height - 1 - dest_x;
                    } else {
                        // 270 degrees clockwise (90 plus a 180 flip).
                        src_x = src_width - 1 - dest_y;
                        src_y = dest_x;
                    }
                } else if (flip180) {
                    src_x = src_width - 1 - dest_x;
                    src_y = src_height - 1 - dest_y;
                }
                const u8* src =
                    pixels + (static_cast<std::size_t>(src_y) * src_width + src_x) * 4;
                row[x] = (0xffu << 24) | (static_cast<u32>(src[0]) << 16) |
                         (static_cast<u32>(src[1]) << 8) | static_cast<u32>(src[2]);
            }
        }
    }

    static u32 FindProperty(int fd, u32 object_id, u32 object_type, const char* name) {
        drmModeObjectProperties* props = drmModeObjectGetProperties(fd, object_id, object_type);
        if (!props) {
            return 0;
        }
        u32 id = 0;
        for (u32 i = 0; i < props->count_props; i++) {
            drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
            if (!prop) {
                continue;
            }
            if (std::strcmp(prop->name, name) == 0) {
                id = prop->prop_id;
            }
            drmModeFreeProperty(prop);
            if (id != 0) {
                break;
            }
        }
        drmModeFreeObjectProperties(props);
        return id;
    }

    bool AddPlaneProperties(drmModeAtomicReq* req, u32 fb) {
        const u32 fb_prop = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
        const u32 crtc_prop = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        const u32 src_x = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
        const u32 src_y = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        const u32 src_w = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
        const u32 src_h = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
        const u32 crtc_x = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        const u32 crtc_y = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        const u32 crtc_w = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        const u32 crtc_h = FindProperty(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
        if (fb_prop == 0 || crtc_prop == 0 || src_x == 0 || src_y == 0 || src_w == 0 ||
            src_h == 0 || crtc_x == 0 || crtc_y == 0 || crtc_w == 0 || crtc_h == 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof missing plane property");
            return false;
        }
        const u64 src_w_value = static_cast<u64>(mode.hdisplay) << 16;
        const u64 src_h_value = static_cast<u64>(mode.vdisplay) << 16;
        return drmModeAtomicAddProperty(req, plane_id, fb_prop, fb) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, crtc_prop, fb == 0 ? 0 : crtc_id) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, src_x, 0) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, src_y, 0) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, src_w, fb == 0 ? 0 : src_w_value) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, src_h, fb == 0 ? 0 : src_h_value) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, crtc_x, 0) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, crtc_y, 0) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, crtc_w, fb == 0 ? 0 : mode.hdisplay) >= 0 &&
               drmModeAtomicAddProperty(req, plane_id, crtc_h, fb == 0 ? 0 : mode.vdisplay) >= 0;
    }

    bool Modeset() {
        if (!atomic) {
            const int rc =
                drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode);
            if (rc != 0) {
                LOG_ERROR(Frontend, "drm-lease: dumb proof legacy drmModeSetCrtc failed: {}",
                          std::strerror(errno));
                return false;
            }
            LOG_INFO(Frontend, "drm-lease: dumb proof legacy modeset ok");
            modeset = true;
            return true;
        }

        const u32 conn_crtc = FindProperty(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        const u32 crtc_active = FindProperty(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        const u32 crtc_mode = FindProperty(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        if (conn_crtc == 0 || crtc_active == 0 || crtc_mode == 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof missing connector/crtc property");
            return false;
        }
        if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob_id) != 0) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof mode blob failed: {}", std::strerror(errno));
            return false;
        }

        drmModeAtomicReq* req = drmModeAtomicAlloc();
        if (!req) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof failed to allocate atomic request");
            return false;
        }
        if (drmModeAtomicAddProperty(req, connector_id, conn_crtc, crtc_id) < 0 ||
            drmModeAtomicAddProperty(req, crtc_id, crtc_active, 1) < 0 ||
            drmModeAtomicAddProperty(req, crtc_id, crtc_mode, blob_id) < 0 ||
            !AddPlaneProperties(req, fb_id)) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof failed to build atomic request");
            drmModeAtomicFree(req);
            return false;
        }

        int test_rc = drmModeAtomicCommit(
            fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        if (test_rc != 0) {
            const int err = errno;
            if (IsNegativeErrno(test_rc, EINVAL)) {
                LOG_ERROR(Frontend, "drm-lease: dumb proof atomic test commit -EINVAL");
            } else if (IsNegativeErrno(test_rc, EACCES)) {
                LOG_ERROR(Frontend, "drm-lease: dumb proof lease-not-master (-EACCES)");
            } else {
                LOG_ERROR(Frontend, "drm-lease: dumb proof atomic test commit failed: {}",
                          std::strerror(err));
            }
            drmModeAtomicFree(req);
            return false;
        }

        const int rc =
            drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        const int err = errno;
        drmModeAtomicFree(req);
        if (rc != 0) {
            if (IsNegativeErrno(rc, EACCES)) {
                LOG_ERROR(Frontend, "drm-lease: dumb proof lease-not-master (-EACCES)");
            } else if (IsNegativeErrno(rc, EINVAL)) {
                LOG_ERROR(Frontend, "drm-lease: dumb proof atomic commit -EINVAL");
            } else {
                LOG_ERROR(Frontend, "drm-lease: dumb proof atomic commit failed: {}",
                          std::strerror(err));
            }
            return false;
        }

        LOG_INFO(Frontend, "drm-lease: dumb proof blocking atomic commit ok");
        modeset = true;
        return true;
    }

    void DisableScanout() {
        if (modeset && atomic && plane_id != 0) {
            const u32 conn_crtc =
                FindProperty(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
            const u32 crtc_active = FindProperty(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
            const u32 crtc_mode = FindProperty(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
            drmModeAtomicReq* req = drmModeAtomicAlloc();
            if (req && conn_crtc != 0 && crtc_active != 0 && crtc_mode != 0 &&
                AddPlaneProperties(req, 0) &&
                drmModeAtomicAddProperty(req, connector_id, conn_crtc, 0) >= 0 &&
                drmModeAtomicAddProperty(req, crtc_id, crtc_active, 0) >= 0 &&
                drmModeAtomicAddProperty(req, crtc_id, crtc_mode, 0) >= 0) {
                const int rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
                if (rc != 0) {
                    LOG_ERROR(Frontend, "drm-lease: dumb proof failed to disable scanout: {}",
                              std::strerror(errno));
                }
            } else {
                LOG_ERROR(Frontend, "drm-lease: dumb proof could not build disable request");
            }
            if (req) {
                drmModeAtomicFree(req);
            }
        } else if (modeset && !atomic && crtc_id != 0) {
            drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr);
        }
        modeset = false;

        if (blob_id != 0) {
            drmModeDestroyPropertyBlob(fd, blob_id);
            blob_id = 0;
        }
        if (fb_id != 0) {
            drmModeRmFB(fd, fb_id);
            fb_id = 0;
        }
        if (mapped) {
            munmap(mapped, mapped_size);
            mapped = nullptr;
            mapped_size = 0;
        }
        if (dumb_handle != 0) {
            drm_mode_destroy_dumb dreq{};
            dreq.handle = dumb_handle;
            ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
            dumb_handle = 0;
        }
        // The lease fd belongs to DrmLeaseClient. Do not close it and do not
        // restore the previous mode; the compositor does that when the lease ends.
    }

    int fd = -1;
    u32 connector_id = 0;
    bool atomic = false;
    bool modeset = false;
    u32 crtc_id = 0;
    u32 plane_id = 0;
    u32 format = DRM_FORMAT_XRGB8888;
    u32 fb_id = 0;
    u32 blob_id = 0;
    u32 dumb_handle = 0;
    u64 dumb_size = 0;
    u8* mapped = nullptr;
    u64 mapped_size = 0;
    u32 pitch = 0;
    u32 width = 0;
    u32 height = 0;
    drmModeModeInfo mode{};
};

DrmLeaseWindow::DrmLeaseWindow() : EmuWindow(true) {}

void DrmLeaseWindow::PollEvents() {
    if (client) {
        client->PumpEvents();
    }
}

void DrmLeaseWindow::PresentCpuFrame(const u8* pixels, u32 width, u32 height, bool content_rotate90,
                                     bool content_flip180) {
    if (dumb_proof) {
        dumb_proof->Present(pixels, width, height, content_rotate90, content_flip180);
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

    const char* proof = std::getenv("AZAHAR_DRM_LEASE_DUMB_PROOF");
    if (proof && std::strcmp(proof, "0") != 0) {
        dumb_proof = std::make_unique<DrmLeaseDumbProof>(client->GetLeaseFd(),
                                                         client->GetConnectorId());
        if (!dumb_proof->Run()) {
            LOG_ERROR(Frontend, "drm-lease: dumb proof failed");
            dumb_proof.reset();
            client.reset();
            return false;
        }
        // Lease fd stays open so the CRTC holds the dumb fb. Do not publish a
        // Drm window: RendererVulkan must not build a VK_KHR_display surface.
        window_info.type = WindowSystemType::Headless;
        UpdateCurrentFramebufferLayout(mode_width, mode_height);
        if (!touch_device.empty()) {
            touch = std::make_unique<DrmLeaseTouch>(*this, touch_device);
        }
        LOG_INFO(Frontend, "drm-lease: dumb proof latched, Vulkan display surface skipped");
        return true;
    }

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
