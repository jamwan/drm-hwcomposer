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

#define LOG_TAG "hwc-vsync-worker"

#include "VSyncWorker.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "utils/log.h"

namespace android {

VSyncWorker::VSyncWorker()
    : Worker("vsync", HAL_PRIORITY_URGENT_DISPLAY),
      drm_(nullptr),
      display_(-1),
      enabled_(false),
      last_timestamp_(-1) {
}

auto VSyncWorker::Init(DrmDevice *drm, int display,
                       std::function<void(uint64_t /*timestamp*/)> callback)
    -> int {
  drm_ = drm;
  display_ = display;
  callback_ = std::move(callback);

  return InitWorker();
}

void VSyncWorker::VSyncControl(bool enabled) {
  Lock();
  enabled_ = enabled;
  last_timestamp_ = -1;
  Unlock();

  Signal();
}

/*
 * Returns the timestamp of the next vsync in phase with last_timestamp_.
 * For example:
 *  last_timestamp_ = 137
 *  frame_ns = 50
 *  current = 683
 *
 *  ret = (50 * ((683 - 137)/50 + 1)) + 137
 *  ret = 687
 *
 *  Thus, we must sleep until timestamp 687 to maintain phase with the last
 *  timestamp.
 */
int64_t VSyncWorker::GetPhasedVSync(int64_t frame_ns, int64_t current) const {
  if (last_timestamp_ < 0)
    return current + frame_ns;

  return frame_ns * ((current - last_timestamp_) / frame_ns + 1) +
         last_timestamp_;
}

static const int64_t kOneSecondNs = 1 * 1000 * 1000 * 1000;

int VSyncWorker::SyntheticWaitVBlank(int64_t *timestamp) {
  struct timespec vsync {};
  int ret = clock_gettime(CLOCK_MONOTONIC, &vsync);
  if (ret)
    return ret;

  float refresh = 60.0F;  // Default to 60Hz refresh rate
  DrmConnector *conn = drm_->GetConnectorForDisplay(display_);
  if (conn && conn->active_mode().v_refresh() != 0.0F)
    refresh = conn->active_mode().v_refresh();
  else
    ALOGW("Vsync worker active with conn=%p refresh=%f\n", conn,
          conn ? conn->active_mode().v_refresh() : 0.0F);

  int64_t phased_timestamp = GetPhasedVSync(kOneSecondNs /
                                                static_cast<int>(refresh),
                                            vsync.tv_sec * kOneSecondNs +
                                                vsync.tv_nsec);
  vsync.tv_sec = phased_timestamp / kOneSecondNs;
  vsync.tv_nsec = int(phased_timestamp - (vsync.tv_sec * kOneSecondNs));
  do {
    ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &vsync, nullptr);
  } while (ret == -1 && errno == EINTR);
  if (ret)
    return ret;

  *timestamp = (int64_t)vsync.tv_sec * kOneSecondNs + (int64_t)vsync.tv_nsec;
  return 0;
}

void VSyncWorker::Routine() {
  int ret = 0;

  Lock();
  if (!enabled_) {
    ret = WaitForSignalOrExitLocked();
    if (ret == -EINTR) {
      Unlock();
      return;
    }
  }

  int display = display_;
  Unlock();

  DrmCrtc *crtc = drm_->GetCrtcForDisplay(display);
  if (!crtc) {
    ALOGE("Failed to get crtc for display");
    return;
  }
  uint32_t high_crtc = (crtc->pipe() << DRM_VBLANK_HIGH_CRTC_SHIFT);

  drmVBlank vblank;
  memset(&vblank, 0, sizeof(vblank));
  vblank.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE |
                                           (high_crtc &
                                            DRM_VBLANK_HIGH_CRTC_MASK));
  vblank.request.sequence = 1;

  int64_t timestamp = 0;
  ret = drmWaitVBlank(drm_->fd(), &vblank);
  if (ret == -EINTR)
    return;

  if (ret) {
    ret = SyntheticWaitVBlank(&timestamp);
    if (ret)
      return;
  } else {
    timestamp = (int64_t)vblank.reply.tval_sec * kOneSecondNs +
                (int64_t)vblank.reply.tval_usec * 1000;
  }

  if (!enabled_)
    return;

  if (callback_) {
    callback_(timestamp);
  }

  last_timestamp_ = timestamp;
}
}  // namespace android
