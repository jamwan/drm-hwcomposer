/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "hwc-drm-plane"

#include "DrmPlane.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>

#include "DrmDevice.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "utils/log.h"

namespace android {

DrmPlane::DrmPlane(DrmDevice *drm, drmModePlanePtr p)
    : drm_(drm),
      id_(p->plane_id),
      possible_crtc_mask_(p->possible_crtcs),
      formats_(p->formats, p->formats + p->count_formats) {
}

int DrmPlane::Init() {
  DrmProperty p;

  if (!GetPlaneProperty("type", p)) {
    return -ENOTSUP;
  }

  int ret = 0;
  uint64_t type = 0;
  std::tie(ret, type) = p.value();
  if (ret) {
    ALOGE("Failed to get plane type property value");
    return ret;
  }
  switch (type) {
    case DRM_PLANE_TYPE_OVERLAY:
    case DRM_PLANE_TYPE_PRIMARY:
    case DRM_PLANE_TYPE_CURSOR:
      type_ = (uint32_t)type;
      break;
    default:
      ALOGE("Invalid plane type %" PRIu64, type);
      return -EINVAL;
  }

  if (!GetPlaneProperty("CRTC_ID", crtc_property_) ||
      !GetPlaneProperty("FB_ID", fb_property_) ||
      !GetPlaneProperty("CRTC_X", crtc_x_property_) ||
      !GetPlaneProperty("CRTC_Y", crtc_y_property_) ||
      !GetPlaneProperty("CRTC_W", crtc_w_property_) ||
      !GetPlaneProperty("CRTC_H", crtc_h_property_) ||
      !GetPlaneProperty("SRC_X", src_x_property_) ||
      !GetPlaneProperty("SRC_Y", src_y_property_) ||
      !GetPlaneProperty("SRC_W", src_w_property_) ||
      !GetPlaneProperty("SRC_H", src_h_property_)) {
    return -ENOTSUP;
  }

  GetPlaneProperty("zpos", zpos_property_, Presence::kOptional);

  if (GetPlaneProperty("rotation", rotation_property_, Presence::kOptional)) {
    rotation_property_.AddEnumToMap("rotate-0", DrmHwcTransform::kIdentity,
                                    transform_enum_map_);
    rotation_property_.AddEnumToMap("rotate-90", DrmHwcTransform::kRotate90,
                                    transform_enum_map_);
    rotation_property_.AddEnumToMap("rotate-180", DrmHwcTransform::kRotate180,
                                    transform_enum_map_);
    rotation_property_.AddEnumToMap("rotate-270", DrmHwcTransform::kRotate270,
                                    transform_enum_map_);
    rotation_property_.AddEnumToMap("reflect-x", DrmHwcTransform::kFlipH,
                                    transform_enum_map_);
    rotation_property_.AddEnumToMap("reflect-y", DrmHwcTransform::kFlipV,
                                    transform_enum_map_);
  }

  GetPlaneProperty("alpha", alpha_property_, Presence::kOptional);

  if (GetPlaneProperty("pixel blend mode", blend_property_,
                       Presence::kOptional)) {
    blend_property_.AddEnumToMap("Pre-multiplied", DrmHwcBlending::kPreMult,
                                 blending_enum_map_);
    blend_property_.AddEnumToMap("Coverage", DrmHwcBlending::kCoverage,
                                 blending_enum_map_);
    blend_property_.AddEnumToMap("None", DrmHwcBlending::kNone,
                                 blending_enum_map_);
  }

  GetPlaneProperty("IN_FENCE_FD", in_fence_fd_property_, Presence::kOptional);

  if (HasNonRgbFormat()) {
    if (GetPlaneProperty("COLOR_ENCODING", color_encoding_propery_,
                         Presence::kOptional)) {
      color_encoding_propery_.AddEnumToMap("ITU-R BT.709 YCbCr",
                                           DrmHwcColorSpace::kItuRec709,
                                           color_encoding_enum_map_);
      color_encoding_propery_.AddEnumToMap("ITU-R BT.601 YCbCr",
                                           DrmHwcColorSpace::kItuRec601,
                                           color_encoding_enum_map_);
      color_encoding_propery_.AddEnumToMap("ITU-R BT.2020 YCbCr",
                                           DrmHwcColorSpace::kItuRec2020,
                                           color_encoding_enum_map_);
    }

    if (GetPlaneProperty("COLOR_RANGE", color_range_property_,
                         Presence::kOptional)) {
      color_range_property_.AddEnumToMap("YCbCr full range",
                                         DrmHwcSampleRange::kFullRange,
                                         color_range_enum_map_);
      color_range_property_.AddEnumToMap("YCbCr limited range",
                                         DrmHwcSampleRange::kLimitedRange,
                                         color_range_enum_map_);
    }
  }

  return 0;
}

uint32_t DrmPlane::id() const {
  return id_;
}

bool DrmPlane::GetCrtcSupported(const DrmCrtc &crtc) const {
  return ((1 << crtc.pipe()) & possible_crtc_mask_) != 0;
}

bool DrmPlane::IsValidForLayer(DrmHwcLayer *layer) {
  if (!rotation_property_) {
    if (layer->transform != DrmHwcTransform::kIdentity) {
      ALOGV("No rotation property on plane %d", id_);
      return false;
    }
  } else {
    if (transform_enum_map_.count(layer->transform) == 0) {
      ALOGV("Transform is not supported on plane %d", id_);
      return false;
    }
  }

  if (alpha_property_.id() == 0 && layer->alpha != 0xffff) {
    ALOGV("Alpha is not supported on plane %d", id_);
    return false;
  }

  if (blending_enum_map_.count(layer->blending) == 0 &&
      layer->blending != DrmHwcBlending::kNone &&
      layer->blending != DrmHwcBlending::kPreMult) {
    ALOGV("Blending is not supported on plane %d", id_);
    return false;
  }

  uint32_t format = layer->buffer_info.format;
  if (!IsFormatSupported(format)) {
    ALOGV("Plane %d does not supports %c%c%c%c format", id_, format,
          format >> 8, format >> 16, format >> 24);
    return false;
  }

  return true;
}

uint32_t DrmPlane::type() const {
  return type_;
}

bool DrmPlane::IsFormatSupported(uint32_t format) const {
  return std::find(std::begin(formats_), std::end(formats_), format) !=
         std::end(formats_);
}

bool DrmPlane::HasNonRgbFormat() const {
  return std::find_if_not(std::begin(formats_), std::end(formats_),
                          [](uint32_t format) {
                            return BufferInfoGetter::IsDrmFormatRgb(format);
                          }) != std::end(formats_);
}

static uint64_t ToDrmRotation(DrmHwcTransform transform) {
  uint64_t rotation = 0;
  if (transform & DrmHwcTransform::kFlipH)
    rotation |= DRM_MODE_REFLECT_X;
  if (transform & DrmHwcTransform::kFlipV)
    rotation |= DRM_MODE_REFLECT_Y;
  if (transform & DrmHwcTransform::kRotate90)
    rotation |= DRM_MODE_ROTATE_90;
  else if (transform & DrmHwcTransform::kRotate180)
    rotation |= DRM_MODE_ROTATE_180;
  else if (transform & DrmHwcTransform::kRotate270)
    rotation |= DRM_MODE_ROTATE_270;
  else
    rotation |= DRM_MODE_ROTATE_0;

  return rotation;
}

auto DrmPlane::AtomicSetState(drmModeAtomicReq &pset, DrmHwcLayer &layer,
                              uint32_t zpos, uint32_t crtc_id) -> int {
  if (!layer.FbIdHandle) {
    ALOGE("Expected a valid framebuffer for pset");
    return -EINVAL;
  }

  if (zpos_property_ && !zpos_property_.is_immutable()) {
    uint64_t min_zpos = 0;

    // Ignore ret and use min_zpos as 0 by default
    std::tie(std::ignore, min_zpos) = zpos_property_.range_min();

    if (!zpos_property_.AtomicSet(pset, zpos + min_zpos)) {
      return -EINVAL;
    }
  }

  if (layer.acquire_fence &&
      !in_fence_fd_property_.AtomicSet(pset, layer.acquire_fence.Get())) {
    return -EINVAL;
  }

  if (!crtc_property_.AtomicSet(pset, crtc_id) ||
      !fb_property_.AtomicSet(pset, layer.FbIdHandle->GetFbId()) ||
      !crtc_x_property_.AtomicSet(pset, layer.display_frame.left) ||
      !crtc_y_property_.AtomicSet(pset, layer.display_frame.top) ||
      !crtc_w_property_.AtomicSet(pset, layer.display_frame.right -
                                            layer.display_frame.left) ||
      !crtc_h_property_.AtomicSet(pset, layer.display_frame.bottom -
                                            layer.display_frame.top) ||
      !src_x_property_.AtomicSet(pset, (int)(layer.source_crop.left) << 16) ||
      !src_y_property_.AtomicSet(pset, (int)(layer.source_crop.top) << 16) ||
      !src_w_property_.AtomicSet(pset, (int)(layer.source_crop.right -
                                             layer.source_crop.left)
                                           << 16) ||
      !src_h_property_.AtomicSet(pset, (int)(layer.source_crop.bottom -
                                             layer.source_crop.top)
                                           << 16)) {
    return -EINVAL;
  }

  if (rotation_property_ &&
      !rotation_property_.AtomicSet(pset, ToDrmRotation(layer.transform))) {
    return -EINVAL;
  }

  if (alpha_property_ && !alpha_property_.AtomicSet(pset, layer.alpha)) {
    return -EINVAL;
  }

  if (blending_enum_map_.count(layer.blending) != 0 &&
      !blend_property_.AtomicSet(pset, blending_enum_map_[layer.blending])) {
    return -EINVAL;
  }

  if (color_encoding_enum_map_.count(layer.color_space) != 0 &&
      !color_encoding_propery_
           .AtomicSet(pset, color_encoding_enum_map_[layer.color_space])) {
    return -EINVAL;
  }

  if (color_range_enum_map_.count(layer.sample_range) != 0 &&
      !color_range_property_
           .AtomicSet(pset, color_range_enum_map_[layer.sample_range])) {
    return -EINVAL;
  }

  return 0;
}

auto DrmPlane::AtomicDisablePlane(drmModeAtomicReq &pset) -> int {
  if (!crtc_property_.AtomicSet(pset, 0) || !fb_property_.AtomicSet(pset, 0)) {
    return -EINVAL;
  }

  return 0;
}

const DrmProperty &DrmPlane::zpos_property() const {
  return zpos_property_;
}

auto DrmPlane::GetPlaneProperty(const char *prop_name, DrmProperty &property,
                                Presence presence) -> bool {
  int err = drm_->GetProperty(id_, DRM_MODE_OBJECT_PLANE, prop_name, &property);
  if (err != 0) {
    if (presence == Presence::kMandatory) {
      ALOGE("Could not get mandatory property \"%s\" from plane %d", prop_name,
            id_);
    } else {
      ALOGV("Could not get optional property \"%s\" from plane %d", prop_name,
            id_);
    }
    return false;
  }

  return true;
}

}  // namespace android
