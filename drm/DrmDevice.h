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

#ifndef ANDROID_DRM_H_
#define ANDROID_DRM_H_

#include <stdint.h>

#include <map>
#include <tuple>

#include "DrmConnector.h"
#include "DrmCrtc.h"
#include "DrmEncoder.h"
#include "DrmEventListener.h"
#include "DrmFbImporter.h"
#include "DrmPlane.h"
#include "utils/UniqueFd.h"

namespace android {

class DrmFbImporter;
class DrmPlane;

class DrmDevice {
 public:
  DrmDevice();
  ~DrmDevice();

  std::tuple<int, int> Init(const char *path, int num_displays);

  int fd() const {
    return fd_.Get();
  }

  const std::vector<std::unique_ptr<DrmConnector>> &connectors() const {
    return connectors_;
  }

  const std::vector<std::unique_ptr<DrmPlane>> &planes() const {
    return planes_;
  }

  std::pair<uint32_t, uint32_t> min_resolution() const {
    return min_resolution_;
  }

  std::pair<uint32_t, uint32_t> max_resolution() const {
    return max_resolution_;
  }

  DrmConnector *GetConnectorForDisplay(int display) const;
  DrmConnector *GetWritebackConnectorForDisplay(int display) const;
  DrmConnector *AvailableWritebackConnector(int display) const;
  DrmCrtc *GetCrtcForDisplay(int display) const;
  DrmPlane *GetPlane(uint32_t id) const;
  DrmEventListener *event_listener();

  int GetCrtcProperty(const DrmCrtc &crtc, const char *prop_name,
                      DrmProperty *property) const;
  int GetConnectorProperty(const DrmConnector &connector, const char *prop_name,
                           DrmProperty *property) const;

  std::string GetName() const;

  const std::vector<std::unique_ptr<DrmCrtc>> &crtcs() const;
  uint32_t next_mode_id();

  auto RegisterUserPropertyBlob(void *data, size_t length) const
      -> DrmModeUserPropertyBlobUnique;

  bool HandlesDisplay(int display) const;
  void RegisterHotplugHandler(DrmEventHandler *handler) {
    event_listener_.RegisterHotplugHandler(handler);
  }

  bool HasAddFb2ModifiersSupport() const {
    return HasAddFb2ModifiersSupport_;
  }

  DrmFbImporter &GetDrmFbImporter() {
    return *mDrmFbImporter.get();
  }

  static auto IsKMSDev(const char *path) -> bool;

  int GetProperty(uint32_t obj_id, uint32_t obj_type, const char *prop_name,
                  DrmProperty *property) const;

 private:
  int TryEncoderForDisplay(int display, DrmEncoder *enc);

  int CreateDisplayPipe(DrmConnector *connector);
  int AttachWriteback(DrmConnector *display_conn);

  UniqueFd fd_;
  uint32_t mode_id_ = 0;

  std::vector<std::unique_ptr<DrmConnector>> connectors_;
  std::vector<std::unique_ptr<DrmConnector>> writeback_connectors_;
  std::vector<std::unique_ptr<DrmEncoder>> encoders_;
  std::vector<std::unique_ptr<DrmCrtc>> crtcs_;
  std::vector<std::unique_ptr<DrmPlane>> planes_;
  DrmEventListener event_listener_;

  std::pair<uint32_t, uint32_t> min_resolution_;
  std::pair<uint32_t, uint32_t> max_resolution_;
  std::map<int, int> displays_;

  bool HasAddFb2ModifiersSupport_{};

  std::shared_ptr<DrmDevice> self;

  std::unique_ptr<DrmFbImporter> mDrmFbImporter;
};
}  // namespace android

#endif  // ANDROID_DRM_H_
