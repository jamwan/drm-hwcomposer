/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ANDROID_DRM_HWC_TWO_H_
#define ANDROID_DRM_HWC_TWO_H_

#include <hardware/hwcomposer2.h>
#include <math.h>

#include <array>
#include <map>

#include "compositor/DrmDisplayCompositor.h"
#include "compositor/Planner.h"
#include "drm/ResourceManager.h"
#include "drm/VSyncWorker.h"
#include "drmhwcomposer.h"

namespace android {

class Backend;

class DrmHwcTwo : public hwc2_device_t {
 public:
  static int HookDevOpen(const struct hw_module_t *module, const char *name,
                         struct hw_device_t **dev);

  DrmHwcTwo();

  HWC2::Error Init();

  std::pair<HWC2_PFN_HOTPLUG, hwc2_callback_data_t> hotplug_callback_{};
  std::pair<HWC2_PFN_VSYNC, hwc2_callback_data_t> vsync_callback_{};
#if PLATFORM_SDK_VERSION > 29
  std::pair<HWC2_PFN_VSYNC_2_4, hwc2_callback_data_t> vsync_2_4_callback_{};
#endif
  std::pair<HWC2_PFN_REFRESH, hwc2_callback_data_t> refresh_callback_{};

  std::mutex callback_lock_;

  class HwcLayer {
   public:
    HWC2::Composition sf_type() const {
      return sf_type_;
    }
    HWC2::Composition validated_type() const {
      return validated_type_;
    }
    void accept_type_change() {
      sf_type_ = validated_type_;
    }
    void set_validated_type(HWC2::Composition type) {
      validated_type_ = type;
    }
    bool type_changed() const {
      return sf_type_ != validated_type_;
    }

    uint32_t z_order() const {
      return z_order_;
    }

    buffer_handle_t buffer() {
      return buffer_;
    }
    void set_buffer(buffer_handle_t buffer) {
      buffer_ = buffer;
    }

    hwc_rect_t display_frame() {
      return display_frame_;
    }

    void PopulateDrmLayer(DrmHwcLayer *layer);

    bool RequireScalingOrPhasing() {
      float src_width = source_crop_.right - source_crop_.left;
      float src_height = source_crop_.bottom - source_crop_.top;

      float dest_width = display_frame_.right - display_frame_.left;
      float dest_height = display_frame_.bottom - display_frame_.top;

      bool scaling = src_width != dest_width || src_height != dest_height;
      bool phasing = (source_crop_.left - floor(source_crop_.left) != 0) ||
                     (source_crop_.top - floor(source_crop_.top) != 0);
      return scaling || phasing;
    }

    // Layer hooks
    HWC2::Error SetCursorPosition(int32_t /*x*/, int32_t /*y*/);
    HWC2::Error SetLayerBlendMode(int32_t mode);
    HWC2::Error SetLayerBuffer(buffer_handle_t buffer, int32_t acquire_fence);
    HWC2::Error SetLayerColor(hwc_color_t /*color*/);
    HWC2::Error SetLayerCompositionType(int32_t type);
    HWC2::Error SetLayerDataspace(int32_t dataspace);
    HWC2::Error SetLayerDisplayFrame(hwc_rect_t frame);
    HWC2::Error SetLayerPlaneAlpha(float alpha);
    HWC2::Error SetLayerSidebandStream(const native_handle_t *stream);
    HWC2::Error SetLayerSourceCrop(hwc_frect_t crop);
    HWC2::Error SetLayerSurfaceDamage(hwc_region_t damage);
    HWC2::Error SetLayerTransform(int32_t transform);
    HWC2::Error SetLayerVisibleRegion(hwc_region_t visible);
    HWC2::Error SetLayerZOrder(uint32_t order);

    UniqueFd acquire_fence_;

    /*
     * Release fence is not used.
     * There is no release fence support available in the DRM/KMS. In case no
     * release fence provided application will use this buffer for writing when
     * the next frame present fence is signaled.
     */
    UniqueFd release_fence_;

   private:
    // sf_type_ stores the initial type given to us by surfaceflinger,
    // validated_type_ stores the type after running ValidateDisplay
    HWC2::Composition sf_type_ = HWC2::Composition::Invalid;
    HWC2::Composition validated_type_ = HWC2::Composition::Invalid;

    buffer_handle_t buffer_ = NULL;
    hwc_rect_t display_frame_;
    float alpha_ = 1.0f;
    hwc_frect_t source_crop_;
    DrmHwcTransform transform_ = DrmHwcTransform::kIdentity;
    uint32_t z_order_ = 0;
    DrmHwcBlending blending_ = DrmHwcBlending::kNone;
    DrmHwcColorSpace color_space_ = DrmHwcColorSpace::kUndefined;
    DrmHwcSampleRange sample_range_ = DrmHwcSampleRange::kUndefined;
  };

  class HwcDisplay {
   public:
    HwcDisplay(ResourceManager *resource_manager, DrmDevice *drm,
               hwc2_display_t handle, HWC2::DisplayType type, DrmHwcTwo *hwc2);
    HwcDisplay(const HwcDisplay &) = delete;
    HWC2::Error Init(std::vector<DrmPlane *> *planes);

    HWC2::Error CreateComposition(bool test);
    std::vector<DrmHwcTwo::HwcLayer *> GetOrderLayersByZPos();

    void ClearDisplay();

    std::string Dump();

    // HWC Hooks
    HWC2::Error AcceptDisplayChanges();
    HWC2::Error CreateLayer(hwc2_layer_t *layer);
    HWC2::Error DestroyLayer(hwc2_layer_t layer);
    HWC2::Error GetActiveConfig(hwc2_config_t *config);
    HWC2::Error GetChangedCompositionTypes(uint32_t *num_elements,
                                           hwc2_layer_t *layers,
                                           int32_t *types);
    HWC2::Error GetClientTargetSupport(uint32_t width, uint32_t height,
                                       int32_t format, int32_t dataspace);
    HWC2::Error GetColorModes(uint32_t *num_modes, int32_t *modes);
    HWC2::Error GetDisplayAttribute(hwc2_config_t config, int32_t attribute,
                                    int32_t *value);
    HWC2::Error GetDisplayConfigs(uint32_t *num_configs,
                                  hwc2_config_t *configs);
    HWC2::Error GetDisplayName(uint32_t *size, char *name);
    HWC2::Error GetDisplayRequests(int32_t *display_requests,
                                   uint32_t *num_elements, hwc2_layer_t *layers,
                                   int32_t *layer_requests);
    HWC2::Error GetDisplayType(int32_t *type);
#if PLATFORM_SDK_VERSION > 27
    HWC2::Error GetRenderIntents(int32_t mode, uint32_t *outNumIntents,
                                 int32_t *outIntents);
    HWC2::Error SetColorModeWithIntent(int32_t mode, int32_t intent);
#endif
#if PLATFORM_SDK_VERSION > 28
    HWC2::Error GetDisplayIdentificationData(uint8_t *outPort,
                                             uint32_t *outDataSize,
                                             uint8_t *outData);
    HWC2::Error GetDisplayCapabilities(uint32_t *outNumCapabilities,
                                       uint32_t *outCapabilities);
    HWC2::Error GetDisplayBrightnessSupport(bool *supported);
    HWC2::Error SetDisplayBrightness(float);
#endif
#if PLATFORM_SDK_VERSION > 29
    HWC2::Error GetDisplayConnectionType(uint32_t *outType);
    HWC2::Error GetDisplayVsyncPeriod(hwc2_vsync_period_t *outVsyncPeriod);

    HWC2::Error SetActiveConfigWithConstraints(
        hwc2_config_t config,
        hwc_vsync_period_change_constraints_t *vsyncPeriodChangeConstraints,
        hwc_vsync_period_change_timeline_t *outTimeline);
    HWC2::Error SetAutoLowLatencyMode(bool on);
    HWC2::Error GetSupportedContentTypes(
        uint32_t *outNumSupportedContentTypes,
        const uint32_t *outSupportedContentTypes);

    HWC2::Error SetContentType(int32_t contentType);
#endif

    HWC2::Error GetDozeSupport(int32_t *support);
    HWC2::Error GetHdrCapabilities(uint32_t *num_types, int32_t *types,
                                   float *max_luminance,
                                   float *max_average_luminance,
                                   float *min_luminance);
    HWC2::Error GetReleaseFences(uint32_t *num_elements, hwc2_layer_t *layers,
                                 int32_t *fences);
    HWC2::Error PresentDisplay(int32_t *present_fence);
    HWC2::Error SetActiveConfig(hwc2_config_t config);
    HWC2::Error ChosePreferredConfig();
    HWC2::Error SetClientTarget(buffer_handle_t target, int32_t acquire_fence,
                                int32_t dataspace, hwc_region_t damage);
    HWC2::Error SetColorMode(int32_t mode);
    HWC2::Error SetColorTransform(const float *matrix, int32_t hint);
    HWC2::Error SetOutputBuffer(buffer_handle_t buffer, int32_t release_fence);
    HWC2::Error SetPowerMode(int32_t mode);
    HWC2::Error SetVsyncEnabled(int32_t enabled);
    HWC2::Error ValidateDisplay(uint32_t *num_types, uint32_t *num_requests);
    HwcLayer *get_layer(hwc2_layer_t layer) {
      auto it = layers_.find(layer);
      if (it == layers_.end())
        return nullptr;
      return &it->second;
    }

    /* Statistics */
    struct Stats {
      Stats minus(Stats b) {
        return {total_frames_ - b.total_frames_,
                total_pixops_ - b.total_pixops_,
                gpu_pixops_ - b.gpu_pixops_,
                failed_kms_validate_ - b.failed_kms_validate_,
                failed_kms_present_ - b.failed_kms_present_,
                frames_flattened_ - b.frames_flattened_};
      }

      uint32_t total_frames_ = 0;
      uint64_t total_pixops_ = 0;
      uint64_t gpu_pixops_ = 0;
      uint32_t failed_kms_validate_ = 0;
      uint32_t failed_kms_present_ = 0;
      uint32_t frames_flattened_ = 0;
    };

    const Backend *backend() const {
      return backend_.get();
    }
    void set_backend(std::unique_ptr<Backend> backend) {
      backend_ = std::move(backend);
    }

    const std::vector<DrmPlane *> &primary_planes() const {
      return primary_planes_;
    }

    const std::vector<DrmPlane *> &overlay_planes() const {
      return overlay_planes_;
    }

    std::map<hwc2_layer_t, HwcLayer> &layers() {
      return layers_;
    }

    const DrmDisplayCompositor &compositor() const {
      return compositor_;
    }

    const DrmDevice *drm() const {
      return drm_;
    }

    const DrmConnector *connector() const {
      return connector_;
    }

    ResourceManager *resource_manager() const {
      return resource_manager_;
    }

    android_color_transform_t &color_transform_hint() {
      return color_transform_hint_;
    }

    Stats &total_stats() {
      return total_stats_;
    }

    /* returns true if composition should be sent to client */
    bool ProcessClientFlatteningState(bool skip) {
      int flattenning_state = flattenning_state_;
      if (flattenning_state == ClientFlattenningState::Disabled) {
        return false;
      }

      if (skip) {
        flattenning_state_ = ClientFlattenningState::NotRequired;
        return false;
      }

      if (flattenning_state == ClientFlattenningState::ClientRefreshRequested) {
        flattenning_state_ = ClientFlattenningState::Flattened;
        return true;
      }

      flattening_vsync_worker_.VSyncControl(true);
      flattenning_state_ = ClientFlattenningState::VsyncCountdownMax;
      return false;
    }

   private:
    enum ClientFlattenningState : int32_t {
      Disabled = -3,
      NotRequired = -2,
      Flattened = -1,
      ClientRefreshRequested = 0,
      VsyncCountdownMax = 60, /* 1 sec @ 60FPS */
    };

    std::atomic_int flattenning_state_{ClientFlattenningState::NotRequired};
    VSyncWorker flattening_vsync_worker_;

    void AddFenceToPresentFence(UniqueFd fd);

    constexpr static size_t MATRIX_SIZE = 16;

    DrmHwcTwo *hwc2_;

    ResourceManager *resource_manager_;
    DrmDevice *drm_;
    DrmDisplayCompositor compositor_;
    std::unique_ptr<Planner> planner_;

    std::vector<DrmPlane *> primary_planes_;
    std::vector<DrmPlane *> overlay_planes_;

    std::unique_ptr<Backend> backend_;

    VSyncWorker vsync_worker_;
    DrmConnector *connector_ = NULL;
    DrmCrtc *crtc_ = NULL;
    hwc2_display_t handle_;
    HWC2::DisplayType type_;
    uint32_t layer_idx_ = 0;
    std::map<hwc2_layer_t, HwcLayer> layers_;
    HwcLayer client_layer_;
    UniqueFd present_fence_;
    int32_t color_mode_{};
    std::array<float, MATRIX_SIZE> color_transform_matrix_{};
    android_color_transform_t color_transform_hint_;

    uint32_t frame_no_ = 0;
    Stats total_stats_;
    Stats prev_stats_;
    std::string DumpDelta(DrmHwcTwo::HwcDisplay::Stats delta);
  };

  class DrmHotplugHandler : public DrmEventHandler {
   public:
    DrmHotplugHandler(DrmHwcTwo *hwc2, DrmDevice *drm)
        : hwc2_(hwc2), drm_(drm) {
    }
    void HandleEvent(uint64_t timestamp_us);

   private:
    DrmHwcTwo *hwc2_;
    DrmDevice *drm_;
  };

 private:
  static DrmHwcTwo *toDrmHwcTwo(hwc2_device_t *dev) {
    return static_cast<DrmHwcTwo *>(dev);
  }

  template <typename PFN, typename T>
  static hwc2_function_pointer_t ToHook(T function) {
    static_assert(std::is_same<PFN, T>::value, "Incompatible fn pointer");
    return reinterpret_cast<hwc2_function_pointer_t>(function);
  }

  template <typename T, typename HookType, HookType func, typename... Args>
  static T DeviceHook(hwc2_device_t *dev, Args... args) {
    DrmHwcTwo *hwc = toDrmHwcTwo(dev);
    return static_cast<T>(((*hwc).*func)(std::forward<Args>(args)...));
  }

  static HwcDisplay *GetDisplay(DrmHwcTwo *hwc, hwc2_display_t display_handle) {
    auto it = hwc->displays_.find(display_handle);
    if (it == hwc->displays_.end())
      return nullptr;

    return &it->second;
  }

  template <typename HookType, HookType func, typename... Args>
  static int32_t DisplayHook(hwc2_device_t *dev, hwc2_display_t display_handle,
                             Args... args) {
    HwcDisplay *display = GetDisplay(toDrmHwcTwo(dev), display_handle);
    if (!display)
      return static_cast<int32_t>(HWC2::Error::BadDisplay);

    return static_cast<int32_t>((display->*func)(std::forward<Args>(args)...));
  }

  template <typename HookType, HookType func, typename... Args>
  static int32_t LayerHook(hwc2_device_t *dev, hwc2_display_t display_handle,
                           hwc2_layer_t layer_handle, Args... args) {
    HwcDisplay *display = GetDisplay(toDrmHwcTwo(dev), display_handle);
    if (!display)
      return static_cast<int32_t>(HWC2::Error::BadDisplay);

    HwcLayer *layer = display->get_layer(layer_handle);
    if (!layer)
      return static_cast<int32_t>(HWC2::Error::BadLayer);

    return static_cast<int32_t>((layer->*func)(std::forward<Args>(args)...));
  }

  // hwc2_device_t hooks
  static int HookDevClose(hw_device_t *dev);
  static void HookDevGetCapabilities(hwc2_device_t *dev, uint32_t *out_count,
                                     int32_t *out_capabilities);
  static hwc2_function_pointer_t HookDevGetFunction(struct hwc2_device *device,
                                                    int32_t descriptor);

  // Device functions
  HWC2::Error CreateVirtualDisplay(uint32_t width, uint32_t height,
                                   int32_t *format, hwc2_display_t *display);
  HWC2::Error DestroyVirtualDisplay(hwc2_display_t display);
  void Dump(uint32_t *outSize, char *outBuffer);
  uint32_t GetMaxVirtualDisplayCount();
  HWC2::Error RegisterCallback(int32_t descriptor, hwc2_callback_data_t data,
                               hwc2_function_pointer_t function);
  HWC2::Error CreateDisplay(hwc2_display_t displ, HWC2::DisplayType type);
  void HandleDisplayHotplug(hwc2_display_t displayid, int state);
  void HandleInitialHotplugState(DrmDevice *drmDevice);

  ResourceManager resource_manager_;
  std::map<hwc2_display_t, HwcDisplay> displays_;

  std::string mDumpString;
};
}  // namespace android

#endif
