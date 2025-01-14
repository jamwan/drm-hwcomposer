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

#define LOG_TAG "hwc-drm-display-composition"

#include "DrmDisplayComposition.h"

#include <sync/sync.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "DrmDisplayCompositor.h"
#include "Planner.h"
#include "drm/DrmDevice.h"
#include "utils/log.h"

namespace android {

DrmDisplayComposition::DrmDisplayComposition(DrmCrtc *crtc, Planner *planner)
    : crtc_(crtc),  // Can be NULL if we haven't modeset yet
      planner_(planner) {
}

bool DrmDisplayComposition::validate_composition_type(DrmCompositionType des) {
  return type_ == DRM_COMPOSITION_TYPE_EMPTY || type_ == des;
}

int DrmDisplayComposition::SetLayers(DrmHwcLayer *layers, size_t num_layers,
                                     bool geometry_changed) {
  if (!validate_composition_type(DRM_COMPOSITION_TYPE_FRAME))
    return -EINVAL;

  geometry_changed_ = geometry_changed;

  for (size_t layer_index = 0; layer_index < num_layers; layer_index++) {
    layers_.emplace_back(std::move(layers[layer_index]));
  }

  type_ = DRM_COMPOSITION_TYPE_FRAME;
  return 0;
}

int DrmDisplayComposition::SetDpmsMode(uint32_t dpms_mode) {
  if (!validate_composition_type(DRM_COMPOSITION_TYPE_DPMS))
    return -EINVAL;
  dpms_mode_ = dpms_mode;
  type_ = DRM_COMPOSITION_TYPE_DPMS;
  return 0;
}

int DrmDisplayComposition::SetDisplayMode(const DrmMode &display_mode) {
  if (!validate_composition_type(DRM_COMPOSITION_TYPE_MODESET)) {
    ALOGE("SetDisplayMode() Failed to validate composition type");
    return -EINVAL;
  }
  display_mode_ = display_mode;
  dpms_mode_ = DRM_MODE_DPMS_ON;
  type_ = DRM_COMPOSITION_TYPE_MODESET;
  return 0;
}

int DrmDisplayComposition::AddPlaneDisable(DrmPlane *plane) {
  composition_planes_.emplace_back(DrmCompositionPlane::Type::kDisable, plane);
  return 0;
}

int DrmDisplayComposition::AddPlaneComposition(DrmCompositionPlane plane) {
  composition_planes_.emplace_back(std::move(plane));
  return 0;
}

int DrmDisplayComposition::Plan(std::vector<DrmPlane *> *primary_planes,
                                std::vector<DrmPlane *> *overlay_planes) {
  if (type_ != DRM_COMPOSITION_TYPE_FRAME)
    return 0;

  std::map<size_t, DrmHwcLayer *> to_composite;

  for (size_t i = 0; i < layers_.size(); ++i)
    to_composite.emplace(std::make_pair(i, &layers_[i]));

  int ret = 0;
  std::tie(ret,
           composition_planes_) = planner_->ProvisionPlanes(to_composite, crtc_,
                                                            primary_planes,
                                                            overlay_planes);
  if (ret) {
    ALOGV("Planner failed provisioning planes ret=%d", ret);
    return ret;
  }

  // Remove the planes we used from the pool before returning. This ensures they
  // won't be reused by another display in the composition.
  for (auto &i : composition_planes_) {
    if (!i.plane())
      continue;

    // make sure that source layers are ordered based on zorder
    std::sort(i.source_layers().begin(), i.source_layers().end());

    std::vector<DrmPlane *> *container = nullptr;
    if (i.plane()->type() == DRM_PLANE_TYPE_PRIMARY)
      container = primary_planes;
    else
      container = overlay_planes;
    for (auto j = container->begin(); j != container->end(); ++j) {
      if (*j == i.plane()) {
        container->erase(j);
        break;
      }
    }
  }

  return 0;
}

}  // namespace android
