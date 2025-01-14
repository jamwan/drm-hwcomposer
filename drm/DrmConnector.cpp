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

#define LOG_TAG "hwc-drm-connector"

#include "DrmConnector.h"

#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <sstream>

#include "DrmDevice.h"
#include "utils/log.h"

namespace android {

constexpr size_t kTypesCount = 17;

DrmConnector::DrmConnector(DrmDevice *drm, drmModeConnectorPtr c,
                           DrmEncoder *current_encoder,
                           std::vector<DrmEncoder *> &possible_encoders)
    : drm_(drm),
      id_(c->connector_id),
      encoder_(current_encoder),
      display_(-1),
      type_(c->connector_type),
      type_id_(c->connector_type_id),
      state_(c->connection),
      mm_width_(c->mmWidth),
      mm_height_(c->mmHeight),
      possible_encoders_(possible_encoders) {
}

int DrmConnector::Init() {
  int ret = drm_->GetConnectorProperty(*this, "DPMS", &dpms_property_);
  if (ret) {
    ALOGE("Could not get DPMS property\n");
    return ret;
  }
  ret = drm_->GetConnectorProperty(*this, "CRTC_ID", &crtc_id_property_);
  if (ret) {
    ALOGE("Could not get CRTC_ID property\n");
    return ret;
  }
  UpdateEdidProperty();
  if (writeback()) {
    ret = drm_->GetConnectorProperty(*this, "WRITEBACK_PIXEL_FORMATS",
                                     &writeback_pixel_formats_);
    if (ret) {
      ALOGE("Could not get WRITEBACK_PIXEL_FORMATS connector_id = %d\n", id_);
      return ret;
    }
    ret = drm_->GetConnectorProperty(*this, "WRITEBACK_FB_ID",
                                     &writeback_fb_id_);
    if (ret) {
      ALOGE("Could not get WRITEBACK_FB_ID connector_id = %d\n", id_);
      return ret;
    }
    ret = drm_->GetConnectorProperty(*this, "WRITEBACK_OUT_FENCE_PTR",
                                     &writeback_out_fence_);
    if (ret) {
      ALOGE("Could not get WRITEBACK_OUT_FENCE_PTR connector_id = %d\n", id_);
      return ret;
    }
  }
  return 0;
}

int DrmConnector::UpdateEdidProperty() {
  int ret = drm_->GetConnectorProperty(*this, "EDID", &edid_property_);
  if (ret) {
    ALOGW("Could not get EDID property\n");
  }
  return ret;
}

auto DrmConnector::GetEdidBlob() -> DrmModePropertyBlobUnique {
  uint64_t blob_id = 0;
  int ret = UpdateEdidProperty();
  if (ret != 0) {
    return DrmModePropertyBlobUnique();
  }

  std::tie(ret, blob_id) = edid_property().value();
  if (ret != 0) {
    return DrmModePropertyBlobUnique();
  }

  return MakeDrmModePropertyBlobUnique(drm_->fd(), blob_id);
}

uint32_t DrmConnector::id() const {
  return id_;
}

int DrmConnector::display() const {
  return display_;
}

void DrmConnector::set_display(int display) {
  display_ = display;
}

bool DrmConnector::internal() const {
  return type_ == DRM_MODE_CONNECTOR_LVDS || type_ == DRM_MODE_CONNECTOR_eDP ||
         type_ == DRM_MODE_CONNECTOR_DSI ||
         type_ == DRM_MODE_CONNECTOR_VIRTUAL || type_ == DRM_MODE_CONNECTOR_DPI;
}

bool DrmConnector::external() const {
  return type_ == DRM_MODE_CONNECTOR_HDMIA ||
         type_ == DRM_MODE_CONNECTOR_DisplayPort ||
         type_ == DRM_MODE_CONNECTOR_DVID || type_ == DRM_MODE_CONNECTOR_DVII ||
         type_ == DRM_MODE_CONNECTOR_VGA;
}

bool DrmConnector::writeback() const {
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
  return type_ == DRM_MODE_CONNECTOR_WRITEBACK;
#else
  return false;
#endif
}

bool DrmConnector::valid_type() const {
  return internal() || external() || writeback();
}

std::string DrmConnector::name() const {
  constexpr std::array<const char *, kTypesCount> kNames =
      {"None",   "VGA",  "DVI-I",     "DVI-D",   "DVI-A", "Composite",
       "SVIDEO", "LVDS", "Component", "DIN",     "DP",    "HDMI-A",
       "HDMI-B", "TV",   "eDP",       "Virtual", "DSI"};

  if (type_ < kTypesCount) {
    std::ostringstream name_buf;
    name_buf << kNames[type_] << "-" << type_id_;
    return name_buf.str();
  }

  ALOGE("Unknown type in connector %d, could not make his name", id_);
  return "None";
}

int DrmConnector::UpdateModes() {
  int fd = drm_->fd();

  drmModeConnectorPtr c = drmModeGetConnector(fd, id_);
  if (!c) {
    ALOGE("Failed to get connector %d", id_);
    return -ENODEV;
  }

  state_ = c->connection;

  bool preferred_mode_found = false;
  std::vector<DrmMode> new_modes;
  for (int i = 0; i < c->count_modes; ++i) {
    bool exists = false;
    for (const DrmMode &mode : modes_) {
      if (mode == c->modes[i]) {
        new_modes.push_back(mode);
        exists = true;
        break;
      }
    }
    if (!exists) {
      DrmMode m(&c->modes[i]);
      m.set_id(drm_->next_mode_id());
      new_modes.push_back(m);
    }
    // Use only the first DRM_MODE_TYPE_PREFERRED mode found
    if (!preferred_mode_found &&
        (new_modes.back().type() & DRM_MODE_TYPE_PREFERRED)) {
      preferred_mode_id_ = new_modes.back().id();
      preferred_mode_found = true;
    }
  }
  modes_.swap(new_modes);
  if (!preferred_mode_found && !modes_.empty()) {
    preferred_mode_id_ = modes_[0].id();
  }
  return 0;
}

const DrmMode &DrmConnector::active_mode() const {
  return active_mode_;
}

void DrmConnector::set_active_mode(const DrmMode &mode) {
  active_mode_ = mode;
}

const DrmProperty &DrmConnector::dpms_property() const {
  return dpms_property_;
}

const DrmProperty &DrmConnector::crtc_id_property() const {
  return crtc_id_property_;
}

const DrmProperty &DrmConnector::edid_property() const {
  return edid_property_;
}

const DrmProperty &DrmConnector::writeback_pixel_formats() const {
  return writeback_pixel_formats_;
}

const DrmProperty &DrmConnector::writeback_fb_id() const {
  return writeback_fb_id_;
}

const DrmProperty &DrmConnector::writeback_out_fence() const {
  return writeback_out_fence_;
}

DrmEncoder *DrmConnector::encoder() const {
  return encoder_;
}

void DrmConnector::set_encoder(DrmEncoder *encoder) {
  encoder_ = encoder;
}

drmModeConnection DrmConnector::state() const {
  return state_;
}

uint32_t DrmConnector::mm_width() const {
  return mm_width_;
}

uint32_t DrmConnector::mm_height() const {
  return mm_height_;
}
}  // namespace android
