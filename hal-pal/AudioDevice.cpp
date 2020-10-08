/*
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
 * Copyright (C) 2013 The Android Open Source Project
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
 *
 * This file was modified by DTS, Inc. The portions of the
 * code modified by DTS, Inc are copyrighted and
 * licensed separately, as follows:
 *
 * (C) 2014 DTS, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AHAL: AudioDevice"
#define ATRACE_TAG (ATRACE_TAG_AUDIO|ATRACE_TAG_HAL)
/*#define LOG_NDEBUG 0*/
/*#define VERY_VERY_VERBOSE_LOGGING*/
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(a...) do { } while(0)
#endif

#include "AudioDevice.h"

#include <dlfcn.h>
#include <log/log.h>
#include <cutils/str_parms.h>

#include <vector>
#include <map>

#include "PalApi.h"
#include "PalDefs.h"

#include "audio_extn.h"
#include "audio_hidl.h"
#include "battery_listener.h"

card_status_t AudioDevice::sndCardState = CARD_STATUS_ONLINE;

AudioDevice::~AudioDevice() {
    audio_extn_gef_deinit(adev_);
    audio_extn_sound_trigger_deinit(adev_);
    pal_deinit();
}

std::shared_ptr<AudioDevice> AudioDevice::GetInstance() {
    if (!adev_) {
        adev_ = std::shared_ptr<AudioDevice> (new AudioDevice());
        device_ = std::shared_ptr<audio_hw_device_t> (new audio_hw_device_t());
    }

    return adev_;
}

std::shared_ptr<AudioDevice> AudioDevice::GetInstance(audio_hw_device_t* device) {
    if (device == (audio_hw_device_t*)device_.get())
        return AudioDevice::adev_;
    else
        return NULL;
}

std::shared_ptr<StreamOutPrimary> AudioDevice::CreateStreamOut(
                        audio_io_handle_t handle,
                        const std::set<audio_devices_t>& devices,
                        audio_output_flags_t flags,
                        struct audio_config *config,
                        audio_stream_out **stream_out,
                        const char *address) {
    std::shared_ptr<StreamOutPrimary> astream (new StreamOutPrimary(handle,
                                              devices, flags, config, address,
                                              fnp_offload_effect_start_output_,
                                              fnp_offload_effect_stop_output_,
                                              fnp_visualizer_start_output_,
                                              fnp_visualizer_stop_output_));
    astream->GetStreamHandle(stream_out);
    out_list_mutex.lock();
    stream_out_list_.push_back(astream);
    ALOGE("%s: output stream %d %p", __func__,(int)stream_out_list_.size(), stream_out);
    if (flags & AUDIO_OUTPUT_FLAG_PRIMARY) {
        if (voice_)
            voice_->stream_out_primary_ = astream;
    }
    out_list_mutex.unlock();
    return astream;
}

void AudioDevice::CloseStreamOut(std::shared_ptr<StreamOutPrimary> stream) {
    out_list_mutex.lock();
    auto iter =
        std::find(stream_out_list_.begin(), stream_out_list_.end(), stream);
    if (iter == stream_out_list_.end()) {
        ALOGE("%s: invalid output stream", __func__);
    } else {
        stream_out_list_.erase(iter);
    }
    out_list_mutex.unlock();
}

int AudioDevice::CreateAudioPatch(audio_patch_handle_t *handle,
                                  const std::vector<struct audio_port_config>& sources,
                                  const std::vector<struct audio_port_config>& sinks) {
    int ret = 0;
    bool new_patch = false;
    AudioPatch *patch = NULL;
    std::shared_ptr<StreamPrimary> stream = nullptr;
    AudioPatch::PatchType patch_type = AudioPatch::PATCH_NONE;
    audio_io_handle_t io_handle = AUDIO_IO_HANDLE_NONE;
    audio_source_t input_source = AUDIO_SOURCE_DEFAULT;
    std::set<audio_devices_t> device_types;

    ALOGD("%s: enter: num sources %zu, num_sinks %zu", __func__, sources.size(), sinks.size());

    if (!handle || sources.empty() || sources.size() > AUDIO_PATCH_PORTS_MAX ||
        sinks.empty() || sinks.size() > AUDIO_PATCH_PORTS_MAX) {
        ALOGE("%s: Invalid patch arguments", __func__);
        return -EINVAL;
    }

    if (sources.size() > 1) {
        ALOGE("%s: Multiple sources are not supported", __func__);
        return -EINVAL;
    }

    ALOGD("%s: source role %d, source type %d", __func__, sources[0].role, sources[0].type);

    // Populate source/sink information and fetch stream info
    switch (sources[0].type) {
        case AUDIO_PORT_TYPE_DEVICE: // Patch for audio capture or loopback
            device_types.insert(sources[0].ext.device.type);
            if (sinks[0].type == AUDIO_PORT_TYPE_MIX) {
                io_handle = sinks[0].ext.mix.handle;
                input_source = sinks[0].ext.mix.usecase.source;
                patch_type = AudioPatch::PATCH_CAPTURE;
                ALOGD("%s: Capture patch from device %x to mix %d",
                      __func__, sources[0].ext.device.type, sinks[0].ext.mix.handle);
            } else {
                /*Device to device patch is not implemented.
                  This space will need changes if audio HAL
                  handles device to device patches in the future.*/
                patch_type = AudioPatch::PATCH_DEVICE_LOOPBACK;
                ALOGE("%s Device to device patches not supported", __func__);
                return -ENOSYS;
            }
            break;
        case AUDIO_PORT_TYPE_MIX: // Patch for audio playback
            io_handle = sources[0].ext.mix.handle;
            for (const auto &sink : sinks)
               device_types.insert(sink.ext.device.type);
            patch_type = AudioPatch::PATCH_PLAYBACK;
            ALOGD("%s: Playback patch from mix handle %d to device %x", __func__,
                  io_handle, AudioExtn::get_device_types(device_types));
            break;
        case AUDIO_PORT_TYPE_SESSION:
        case AUDIO_PORT_TYPE_NONE:
            ALOGE("%s: Unsupported source type %d", __func__, sources[0].type);
            return -EINVAL;
    }

    if (patch_type == AudioPatch::PATCH_PLAYBACK)
        stream = OutGetStream(io_handle);
    else
        stream = InGetStream(io_handle);

    if(!stream){
        ALOGE("%s: Failed to fetch stream with io handle %d", __func__, io_handle);
        return -EINVAL;
    }

    // empty patch...generate new handle
    if (*handle == AUDIO_PATCH_HANDLE_NONE) {
        patch = new AudioPatch(patch_type, sources, sinks);
        *handle = patch->handle;
        new_patch = true;
    } else {
        std::lock_guard<std::mutex> lock(patch_map_mutex);
        auto it = patch_map_.find(*handle);
        if (it == patch_map_.end()) {
            ALOGE("%s: Unable to fetch patch with handle %d", __func__, *handle);
            return -EINVAL;
        }
        patch = &(*it->second);
        patch->type = patch_type;
        patch->sources = sources;
        patch->sinks = sinks;
    }

    ret = stream->RouteStream(device_types);
    if (voice_ && patch_type == AudioPatch::PATCH_PLAYBACK)
        ret |= voice_->RouteStream(device_types);

    if (ret) {
        if (new_patch)
            delete patch;
        ALOGE("%s: Stream routing failed for io_handle %d", __func__, io_handle);
    } else if (new_patch) {
        // new patch...add to patch map
        std::lock_guard<std::mutex> lock(patch_map_mutex);
        patch_map_[patch->handle] = patch;
        ALOGD("%s: Added a new patch with handle %d", __func__, patch->handle);
    }

    return ret;
}

int AudioDevice::ReleaseAudioPatch(audio_patch_handle_t handle){
    int ret = 0;
    AudioPatch *patch = NULL;
    std::shared_ptr<StreamPrimary> stream = nullptr;
    audio_io_handle_t io_handle = AUDIO_IO_HANDLE_NONE;
    AudioPatch::PatchType patch_type = AudioPatch::PATCH_NONE;

    ALOGD("%s: Release patch with handle %d", __func__, handle);

    if (handle == AUDIO_PATCH_HANDLE_NONE) {
        ALOGE("%s: Invalid patch handle %d", __func__, handle);
        return -EINVAL;
    }

    // grab the io_handle from the patch
    patch_map_mutex.lock();
    auto patch_it = patch_map_.find(handle);
    if (patch_it == patch_map_.end() || !patch_it->second) {
        ALOGE("%s: Patch info not found with handle %d", __func__, handle);
        return -EINVAL;
    }
    patch = &(*patch_it->second);
    patch_type = patch->type;
    switch (patch->sources[0].type) {
        case AUDIO_PORT_TYPE_MIX:
            io_handle = patch->sources[0].ext.mix.handle;
            break;
        case AUDIO_PORT_TYPE_DEVICE:
            if (patch->type == AudioPatch::PATCH_CAPTURE)
                io_handle = patch->sinks[0].ext.mix.handle;
            break;
        case AUDIO_PORT_TYPE_SESSION:
        case AUDIO_PORT_TYPE_NONE:
            ALOGD("%s: Invalid port type: %d", __func__, patch->sources[0].type);
            return -EINVAL;
    }
    patch_map_mutex.unlock();

    if (patch_type == AudioPatch::PATCH_PLAYBACK)
        stream = OutGetStream(io_handle);
    else
        stream = InGetStream(io_handle);

    if (!stream){
        ALOGE("%s: Failed to fetch stream with io handle %d", __func__, io_handle);
        return -EINVAL;
    }

    ret = stream->RouteStream({AUDIO_DEVICE_NONE});
    if (patch_type == AudioPatch::PATCH_PLAYBACK)
        ret |= voice_->RouteStream({AUDIO_DEVICE_NONE});

    if (ret)
        ALOGE("%s: Stream routing failed for io_handle %d", __func__, io_handle);

    std::lock_guard lock(patch_map_mutex);
    patch_map_.erase(handle);
    delete patch;

    ALOGD("%s: Successfully released patch %d", __func__, handle);
    return ret;
}

std::shared_ptr<StreamInPrimary> AudioDevice::CreateStreamIn(
                                        audio_io_handle_t handle,
                                        const std::set<audio_devices_t>& devices,
                                        audio_input_flags_t flags,
                                        struct audio_config *config,
                                        const char *address,
                                        audio_stream_in **stream_in,
                                        audio_source_t source) {
    std::shared_ptr<StreamInPrimary> astream (new StreamInPrimary(handle,
                                              devices, flags, config,
                                              address, source));
    astream->GetStreamHandle(stream_in);
    in_list_mutex.lock();
    stream_in_list_.push_back(astream);
    in_list_mutex.unlock();
    ALOGD("%s: input stream %d %p", __func__,(int)stream_in_list_.size(), stream_in);
    return astream;
}

void AudioDevice::CloseStreamIn(std::shared_ptr<StreamInPrimary> stream) {
    in_list_mutex.lock();
    auto iter =
        std::find(stream_in_list_.begin(), stream_in_list_.end(), stream);
    if (iter == stream_in_list_.end()) {
        ALOGE("%s: invalid output stream", __func__);
    } else {
        stream_in_list_.erase(iter);
    }
    in_list_mutex.unlock();
}

static int adev_close(hw_device_t *device __unused) {
    return 0;
}

static int adev_init_check(const struct audio_hw_device *dev __unused) {
    return 0;
}

void adev_on_battery_status_changed(bool charging)
{
    std::shared_ptr<AudioDevice>adevice = AudioDevice::GetInstance();
    ALOGD("%s: battery status changed to %scharging",
        __func__, charging ? "" : "not ");
    adevice->SetChargingMode(charging);
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume) {
    std::shared_ptr<AudioDevice>adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return -EINVAL;
    }

    return adevice->SetVoiceVolume(volume);
}

static int adev_pal_global_callback(uint32_t event_id, uint32_t *event_data,
                                     void *cookie) {
    ALOGD("%s: event_id (%d), event_data (%d), cookie (%p)",
          __func__, event_id, *event_data, cookie);
    switch (event_id) {
    case PAL_SND_CARD_STATE :
        AudioDevice::sndCardState = (card_status_t)*event_data;
        ALOGD("%s: sound card status changed %d sndCardState %d", __func__,
              *event_data, AudioDevice::sndCardState);
        break;
    default :
       ALOGE("%s: Invalid event id:%d", __func__, event_id);
       return -EINVAL;
    }
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
                            audio_io_handle_t handle,
                            audio_devices_t devices,
                            audio_output_flags_t flags,
                            struct audio_config *config,
                            struct audio_stream_out **stream_out,
                            const char *address) {
    int32_t ret = 0;
    std::shared_ptr<StreamOutPrimary> astream;

    ALOGD("%s: enter: format(%#x) sample_rate(%d) channel_mask(%#x) devices(%#x)\
        flags(%#x) address(%s)", __func__, config->format, config->sample_rate,
        config->channel_mask, devices, flags, address);

    std::shared_ptr<AudioDevice>adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        goto exit;
    }

    /* This check is added for oflload streams, so that
     * flinger will fallback to DB stream during SSR.
     */
    if (AudioDevice::sndCardState == CARD_STATUS_OFFLINE &&
        (flags & AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD ||
        flags & AUDIO_OUTPUT_FLAG_DIRECT)) {
        ALOGE("%s: sound card offline", __func__);
        return -ENODEV;
    }

    astream = adevice->OutGetStream(handle);
    if (astream == nullptr)
        astream = adevice->CreateStreamOut(handle, {devices}, flags, config, stream_out, address);
exit:
    return ret;
}

void adev_close_output_stream(struct audio_hw_device *dev,
                              struct audio_stream_out *stream) {
    std::shared_ptr<StreamOutPrimary> astream_out;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return;
    }

    astream_out = adevice->OutGetStream((audio_stream_t*)stream);
    if (!astream_out) {
        ALOGE("%s: invalid astream_in object", __func__);
        return;
    }

    ALOGD("%s: enter:stream_handle(%p)", __func__, astream_out.get());

    adevice->CloseStreamOut(astream_out);
}

void adev_close_input_stream(struct audio_hw_device *dev,
                             struct audio_stream_in *stream)
{
    std::shared_ptr<StreamInPrimary> astream_in;
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return;
    }

    astream_in = adevice->InGetStream((audio_stream_t*)stream);
    if (!astream_in) {
        ALOGE("%s: invalid astream_in object", __func__);
        return;
    }

    ALOGD("%s: enter:stream_handle(%p)", __func__, astream_in.get());

    adevice->CloseStreamIn(astream_in);
}

static int adev_open_input_stream(struct audio_hw_device *dev,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in,
                                  audio_input_flags_t flags,
                                  const char *address,
                                  audio_source_t source) {
    int32_t ret = 0;
    bool ret_error = false;
    std::shared_ptr<StreamInPrimary> astream = nullptr;
    ALOGD("%s: enter: sample_rate(%d) channel_mask(%#x) devices(%#x)\
        io_handle(%d) source(%d) format %x", __func__, config->sample_rate,
        config->channel_mask, devices, handle, source, config->format);

    std::shared_ptr<AudioDevice>adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        goto exit;
    }
    if ((config->format == AUDIO_FORMAT_PCM_FLOAT) ||
        (config->format == AUDIO_FORMAT_PCM_32_BIT) ||
        (config->format == AUDIO_FORMAT_PCM_24_BIT_PACKED) ||
        (config->format == AUDIO_FORMAT_PCM_8_24_BIT)) {
        //astream->bit_width = 24;
        if ((source != AUDIO_SOURCE_UNPROCESSED) &&
                (source != AUDIO_SOURCE_CAMCORDER)) {
            config->format = AUDIO_FORMAT_PCM_16_BIT;
            if (config->sample_rate > 48000)
                config->sample_rate = 48000;
            ret_error = true;
        } else if (!(config->format == AUDIO_FORMAT_PCM_24_BIT_PACKED ||
                    config->format == AUDIO_FORMAT_PCM_8_24_BIT)) {
            config->format = AUDIO_FORMAT_PCM_24_BIT_PACKED;
            ret_error = true;
        }

        if (ret_error) {
            ret = -EINVAL;
            goto exit;
        }
    }

    if (config->format == AUDIO_FORMAT_PCM_FLOAT) {
        ALOGE("%s: format not supported", __func__);
        config->format = AUDIO_FORMAT_PCM_16_BIT;
        ret = -EINVAL;
        goto exit;
    }

    astream = adevice->InGetStream(handle);
    if (astream == nullptr)
        astream = adevice->CreateStreamIn(handle, {devices}, flags, config,
                address, stream_in, source);

  exit:
      return ret;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    std::shared_ptr<AudioDevice>adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return -EINVAL;
    }

    return adevice->SetMode(mode);
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return -EINVAL;
    }

    return adevice->SetMicMute(state);
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state) {
    std::shared_ptr<AudioDevice> adevice =
        AudioDevice::GetInstance((audio_hw_device_t *)dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return -EINVAL;
    }

    return adevice->GetMicMute(state);
}

static int adev_set_master_volume(struct audio_hw_device *dev __unused,
                                  float volume __unused) {
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev __unused,
                                  float *volume __unused) {
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev __unused,
                                bool muted __unused) {
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev __unused,
                                bool *muted __unused) {
    return -ENOSYS;
}

static int adev_set_parameters(struct audio_hw_device *dev,
                               const char *kvpairs) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance(dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return -EINVAL;
    }

    return adevice->SetParameters(kvpairs);
}

static char* adev_get_parameters(const struct audio_hw_device *dev,
                                 const char *keys) {
    std::shared_ptr<AudioDevice> adevice =
        AudioDevice::GetInstance((audio_hw_device_t*)dev);
    if (!adevice) {
        ALOGE("%s: invalid adevice object", __func__);
        return NULL;
    }

    return adevice->GetParameters(keys);
}

static size_t adev_get_input_buffer_size(
                                const struct audio_hw_device *dev __unused,
                                const struct audio_config *config __unused) {
    return BUF_SIZE_CAPTURE * NO_OF_BUF;
}

int adev_release_audio_patch(struct audio_hw_device *dev,
                             audio_patch_handle_t handle) {
    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance(dev);
    if (!adevice){
        ALOGE("%s: GetInstance() failed", __func__);
        return -EINVAL;
    }
    return adevice->ReleaseAudioPatch(handle);
}

int adev_create_audio_patch(struct audio_hw_device *dev,
                            unsigned int num_sources,
                            const struct audio_port_config *sources,
                            unsigned int num_sinks,
                            const struct audio_port_config *sinks,
                            audio_patch_handle_t *handle) {

    if (!handle){
        ALOGE("%s: Invalid handle", __func__);
        return -EINVAL;
    }

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance(dev);
    if (!adevice){
        ALOGE("%s: GetInstance() failed", __func__);
        return -EINVAL;
    }

    std::vector<struct audio_port_config> source_vec(sources, sources + num_sources);
    std::vector<struct audio_port_config> sink_vec(sinks, sinks + num_sinks);

    return adevice->CreateAudioPatch(handle, source_vec, sink_vec);
}

int adev_get_audio_port(struct audio_hw_device *dev,
                        struct audio_port *config) {
    std::ignore = dev;
    std::ignore = config;

    return 0;
}

int adev_set_audio_port_config(struct audio_hw_device *dev,
                               const struct audio_port_config *config)
{
    std::ignore = dev;
    std::ignore = config;

    return 0;
}

static int adev_dump(const audio_hw_device_t *device __unused, int fd __unused)
{
    return 0;
}

static int adev_get_microphones(const struct audio_hw_device *dev __unused,
                struct audio_microphone_characteristic_t *mic_array __unused,
                size_t *mic_count __unused) {
    return -ENOSYS;
}

int AudioDevice::Init(hw_device_t **device, const hw_module_t *module) {
    int ret = 0;
    /* default audio HAL major version */
    uint32_t maj_version = 3;

    ret = pal_init();
    if (ret) {
        ALOGE("%s:(%d) pal_init failed ret=(%d)", __func__, __LINE__, ret);
        return -EINVAL;
    }

    ret = pal_register_global_callback(&adev_pal_global_callback, this);
    if (ret) {
        ALOGE("%s:(%d) pal register callback failed ret=(%d)", __func__, __LINE__, ret);
    }

    adev_->device_.get()->common.tag = HARDWARE_DEVICE_TAG;
    adev_->device_.get()->common.version =
                                HARDWARE_DEVICE_API_VERSION(maj_version, 0);
    adev_->device_.get()->common.close = adev_close;
    adev_->device_.get()->init_check = adev_init_check;
    adev_->device_.get()->set_voice_volume = adev_set_voice_volume;
    adev_->device_.get()->set_master_volume = adev_set_master_volume;
    adev_->device_.get()->get_master_volume = adev_get_master_volume;
    adev_->device_.get()->set_master_mute = adev_set_master_mute;
    adev_->device_.get()->get_master_mute = adev_get_master_mute;
    adev_->device_.get()->set_mode = adev_set_mode;
    adev_->device_.get()->set_mic_mute = adev_set_mic_mute;
    adev_->device_.get()->get_mic_mute = adev_get_mic_mute;
    adev_->device_.get()->set_parameters = adev_set_parameters;
    adev_->device_.get()->get_parameters = adev_get_parameters;
    adev_->device_.get()->get_input_buffer_size = adev_get_input_buffer_size;
    adev_->device_.get()->open_output_stream = adev_open_output_stream;
    adev_->device_.get()->close_output_stream = adev_close_output_stream;
    adev_->device_.get()->open_input_stream = adev_open_input_stream;
    adev_->device_.get()->close_input_stream = adev_close_input_stream;
    adev_->device_.get()->create_audio_patch = adev_create_audio_patch;
    adev_->device_.get()->release_audio_patch = adev_release_audio_patch;
    adev_->device_.get()->get_audio_port = adev_get_audio_port;
    adev_->device_.get()->set_audio_port_config = adev_set_audio_port_config;
    adev_->device_.get()->dump = adev_dump;
    adev_->device_.get()->get_microphones = adev_get_microphones;
    adev_->device_.get()->common.module = (struct hw_module_t *)module;
    *device = &(adev_->device_.get()->common);

    // visualizer lib
    if (access(VISUALIZER_LIBRARY_PATH, R_OK) == 0) {
        visualizer_lib_ = dlopen(VISUALIZER_LIBRARY_PATH, RTLD_NOW);
        if (visualizer_lib_ == NULL) {
            ALOGE("%s: DLOPEN failed for %s", __func__, VISUALIZER_LIBRARY_PATH);
        } else {
            ALOGV("%s: DLOPEN successful for %s", __func__, VISUALIZER_LIBRARY_PATH);
            fnp_visualizer_start_output_ =
                        (int (*)(audio_io_handle_t, pal_stream_handle_t*))dlsym(visualizer_lib_,
                                                        "visualizer_hal_start_output");
            fnp_visualizer_stop_output_ =
                        (int (*)(audio_io_handle_t, pal_stream_handle_t*))dlsym(visualizer_lib_,
                                                        "visualizer_hal_stop_output");
        }
    }

    // offload effect lib
    if (access(OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH, R_OK) == 0) {
        offload_effects_lib_ = dlopen(OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH,
                                      RTLD_NOW);
        if (offload_effects_lib_ == NULL) {
            ALOGE("%s: DLOPEN failed for %s", __func__,
                  OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH);
        } else {
            ALOGV("%s: DLOPEN successful for %s", __func__,
                  OFFLOAD_EFFECTS_BUNDLE_LIBRARY_PATH);
            fnp_offload_effect_start_output_ =
                (int (*)(audio_io_handle_t, pal_stream_handle_t*))dlsym(
                                    offload_effects_lib_,
                                    "offload_effects_bundle_hal_start_output");
            fnp_offload_effect_stop_output_ =
                (int (*)(audio_io_handle_t, pal_stream_handle_t*))dlsym(
                                    offload_effects_lib_,
                                    "offload_effects_bundle_hal_stop_output");
        }
    }
    audio_extn_sound_trigger_init(adev_);
    AudioExtn::hfp_feature_init(property_get_bool("vendor.audio.feature.hfp.enable", false));
    /* no feature configurations yet */
    AudioExtn::battery_listener_feature_init(true);
    AudioExtn::battery_properties_listener_init(adev_on_battery_status_changed);
    audio_extn_hidl_init();
    voice_ = VoiceInit();
    mute_ = false;
    current_rotation = PAL_SPEAKER_ROTATION_LR;

    FillAndroidDeviceMap();
    audio_extn_gef_init(adev_);
    adev_init_ref_count += 1;

    return ret;
}

std::shared_ptr<AudioVoice> AudioDevice::VoiceInit() {
    std::shared_ptr<AudioVoice> voice (new AudioVoice());

    return voice;

}

int AudioDevice::SetGEFParam(void *data, int length) {
    return pal_set_param(PAL_PARAM_ID_UIEFFECT, data, length);
}


int AudioDevice::GetGEFParam(void *data, int *length) {
    return pal_get_param(PAL_PARAM_ID_UIEFFECT, nullptr, (size_t *)length, data);
}

std::shared_ptr<StreamOutPrimary> AudioDevice::OutGetStream(audio_io_handle_t handle) {
    std::shared_ptr<StreamOutPrimary> astream_out = NULL;
    out_list_mutex.lock();
    for (int i = 0; i < stream_out_list_.size(); i++) {
        if (stream_out_list_[i]->handle_ == handle) {
            ALOGI("%s: Found existing stream associated with iohandle %d",
                  __func__, handle);
            astream_out = stream_out_list_[i];
            break;
        }
    }

    out_list_mutex.unlock();
    return astream_out;
}

std::shared_ptr<StreamOutPrimary> AudioDevice::OutGetStream(audio_stream_t* stream_out) {

    std::shared_ptr<StreamOutPrimary> astream_out;
    ALOGV("%s: stream_out(%p)", __func__, stream_out);
    out_list_mutex.lock();
    for (int i = 0; i < stream_out_list_.size(); i++) {
        if (stream_out_list_[i]->stream_.get() ==
                                        (audio_stream_out*) stream_out) {
            ALOGV("%s: Found stream associated with stream_out", __func__);
            astream_out = stream_out_list_[i];
            break;
        }
    }
    out_list_mutex.unlock();
    ALOGV("%s: astream_out(%p)", __func__, astream_out->stream_.get());

    return astream_out;
}

std::shared_ptr<StreamInPrimary> AudioDevice::InGetStream (audio_io_handle_t handle) {
    std::shared_ptr<StreamInPrimary> astream_in = NULL;
    in_list_mutex.lock();
    for (int i = 0; i < stream_in_list_.size(); i++) {
        if (stream_in_list_[i]->handle_ == handle) {
            ALOGI("%s: Found existing stream associated with iohandle %d",
                  __func__, handle);
            astream_in = stream_in_list_[i];
            break;
        }
    }
    in_list_mutex.unlock();
    return astream_in;
}

std::shared_ptr<StreamInPrimary> AudioDevice::InGetStream (audio_stream_t* stream_in) {
    std::shared_ptr<StreamInPrimary> astream_in;

    ALOGV("%s: stream_in(%p)", __func__, stream_in);
    in_list_mutex.lock();
    for (int i = 0; i < stream_in_list_.size(); i++) {
        if (stream_in_list_[i]->stream_.get() == (audio_stream_in*) stream_in) {
            ALOGV("%s: Found existing stream associated with astream_in", __func__);
            astream_in = stream_in_list_[i];
            break;
        }
    }
    in_list_mutex.unlock();
    ALOGV("%s: astream_in(%p)", __func__, astream_in->stream_.get());
    return astream_in;
}

int AudioDevice::SetMicMute(bool state) {
    int ret;

    mute_ = state;
    if (voice_)
        ret = voice_->SetMicMute(state);

    return 0;
}

int AudioDevice::GetMicMute(bool *state) {
    *state = mute_;

    return 0;
}

int AudioDevice::SetMode(const audio_mode_t mode) {
    int ret = 0;

    ALOGD("%s: enter: %d", __func__, mode);
    voice_->SetMode(mode);

    return ret;
}

int AudioDevice::add_input_headset_if_usb_out_headset(int *device_count,
                                                      pal_device_id_t** pal_device_ids)
{
    bool is_usb_headset = false;
    int count = *device_count;
    pal_device_id_t* temp = NULL;

    for (int i = 0; i < count; i++) {
         if (*pal_device_ids[i] == PAL_DEVICE_OUT_USB_HEADSET) {
             is_usb_headset = true;
             break;
         }
    }

    if (is_usb_headset) {
        temp = (pal_device_id_t *) realloc(*pal_device_ids,
            (count + 1) * sizeof(pal_device_id_t));
        if (!temp)
            return -ENOMEM;
        *pal_device_ids = temp;
        temp[count] = PAL_DEVICE_IN_USB_HEADSET;
        *device_count = count + 1;
        usb_input_dev_enabled = true;
    }
    return 0;
}

int AudioDevice::SetParameters(const char *kvpairs) {
    int ret = 0, val = 0;
    struct str_parms *parms;
    char value[32];
    int pal_device_count = 0;
    pal_device_id_t* pal_device_ids = NULL;

    ALOGD("%s: enter: %s", __func__, kvpairs);
    ret = voice_->VoiceSetParameters(kvpairs);
    if (ret)
        ALOGE("%s: Error in VoiceSetParameters %d", __func__, ret);

    parms = str_parms_create_str(kvpairs);
    ret = str_parms_get_str(parms, "screen_state", value, sizeof(value));

    if (ret >= 0) {
        pal_param_screen_state_t param_screen_st;
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0) {
            param_screen_st.screen_state = true;
            ALOGD("%s:%d - screen = on", __func__, __LINE__);
            ret = pal_set_param( PAL_PARAM_ID_SCREEN_STATE, (void*)&param_screen_st, sizeof(pal_param_screen_state_t));
        }
        else {
            ALOGD("%s:%d - screen = off", __func__, __LINE__);
            param_screen_st.screen_state = false;
            ret = pal_set_param( PAL_PARAM_ID_SCREEN_STATE, (void*)&param_screen_st, sizeof(pal_param_screen_state_t));
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_CONNECT,
                            value, sizeof(value));
    if (ret >= 0) {
        pal_param_device_connection_t param_device_connection;
        val = atoi(value);
        audio_devices_t device = (audio_devices_t)val;

        if (audio_is_usb_out_device(device) || audio_is_usb_in_device(device)) {
            ret = str_parms_get_str(parms, "card", value, sizeof(value));
            if (ret >= 0) {
                param_device_connection.device_config.usb_addr.card_id = atoi(value);
                if ((usb_card_id_ == param_device_connection.device_config.usb_addr.card_id) &&
                    (audio_is_usb_in_device(device)) && (usb_input_dev_enabled == true)) {
                    ALOGI("%s: plugin card :%d device num=%d already added", __func__, usb_card_id_,
                          param_device_connection.device_config.usb_addr.device_num);
                    return 0;
                }

                usb_card_id_ = param_device_connection.device_config.usb_addr.card_id;
                ALOGI("%s: plugin card=%d", __func__,
                    param_device_connection.device_config.usb_addr.card_id);
            }
            ret = str_parms_get_str(parms, "device", value, sizeof(value));
            if (ret >= 0) {
                param_device_connection.device_config.usb_addr.device_num = atoi(value);
                usb_dev_num_ = param_device_connection.device_config.usb_addr.device_num;
                ALOGI("%s: plugin device num=%d", __func__,
                    param_device_connection.device_config.usb_addr.device_num);
            }
        } else if (val == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            int controller = -1, stream = -1;
            AudioExtn::get_controller_stream_from_params(parms, &controller, &stream);
            param_device_connection.device_config.dp_config.controller = controller;
            dp_controller = controller;
            param_device_connection.device_config.dp_config.stream = stream;
            dp_stream = stream;
            ALOGI("%s: plugin device cont %d stream %d", __func__, controller, stream);
        }

        if (device) {
            pal_device_ids = (pal_device_id_t *) calloc(1, sizeof(pal_device_id_t));
            pal_device_count = GetPalDeviceIds({device}, pal_device_ids);
            ret = add_input_headset_if_usb_out_headset(&pal_device_count, &pal_device_ids);
            if (ret) {
                free(pal_device_ids);
                ALOGE("%s: adding input headset failed, error:%d", __func__, ret);
                return ret;
            }
            for (int i = 0; i < pal_device_count; i++) {
                param_device_connection.connection_state = true;
                param_device_connection.id = pal_device_ids[i];
                ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
                        (void*)&param_device_connection,
                        sizeof(pal_param_device_connection_t));
                if (ret!=0) {
                    ALOGE("%s: pal set param failed for device connection, pal_device_ids:%d",
                          __func__, pal_device_ids[i]);
                }
                ALOGI("%s: pal set param success  for device connection", __func__);
            }
            if (pal_device_ids) {
                free(pal_device_ids);
                pal_device_ids = NULL;
            }
        }
    }

    /* Checking for Device rotation */
    ret = str_parms_get_int(parms, "rotation", &val);
    if (ret >= 0) {
        int isRotationReq = 0;
        pal_param_device_rotation_t param_device_rotation;
        switch (val) {
        case 270:
        {
            if (PAL_SPEAKER_ROTATION_LR == current_rotation) {
                /* Device rotated from normal position to inverted landscape. */
                current_rotation = PAL_SPEAKER_ROTATION_RL;
                isRotationReq = 1;
                param_device_rotation.rotation_type = PAL_SPEAKER_ROTATION_RL;
            }
        }
        break;
        case 0:
        case 180:
        case 90:
        {
            if (PAL_SPEAKER_ROTATION_RL == current_rotation) {
                /* Phone was in inverted landspace and now is changed to portrait
                 * or inverted portrait. Notify PAL to swap the speaker.
                 */
                current_rotation = PAL_SPEAKER_ROTATION_LR;
                isRotationReq = 1;
                param_device_rotation.rotation_type = PAL_SPEAKER_ROTATION_LR;
            }
        }
        break;
        default:
            ALOGE("%s: unexpected rotation of %d", __func__, val);
            isRotationReq = -EINVAL;
        }
        if (1 == isRotationReq) {
            /* Swap the speakers */
            ALOGD("%s: Swapping the speakers ", __func__);
            ret = pal_set_param(PAL_PARAM_ID_DEVICE_ROTATION,
                    (void*)&param_device_rotation,
                    sizeof(pal_param_device_rotation_t));
            ALOGD("%s: Speakers swapped ", __func__);
        }
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_DEVICE_DISCONNECT,
                            value, sizeof(value));
    if (ret >= 0) {
        pal_param_device_connection_t param_device_connection;
        val = atoi(value);
        audio_devices_t device = (audio_devices_t)val;
        if (audio_is_usb_out_device(device) || audio_is_usb_in_device(device)) {
            ret = str_parms_get_str(parms, "card", value, sizeof(value));
            if (ret >= 0)
                param_device_connection.device_config.usb_addr.card_id = atoi(value);
            ret = str_parms_get_str(parms, "device", value, sizeof(value));
            if (ret >= 0)
                param_device_connection.device_config.usb_addr.device_num = atoi(value);
            if ((usb_card_id_ == param_device_connection.device_config.usb_addr.card_id) &&
                (audio_is_usb_in_device(device)) && (usb_input_dev_enabled == true)) {
                   usb_input_dev_enabled = false;
            }
        } else if (val == AUDIO_DEVICE_OUT_AUX_DIGITAL) {
            int controller = -1, stream = -1;
            AudioExtn::get_controller_stream_from_params(parms, &controller, &stream);
            param_device_connection.device_config.dp_config.controller = controller;
            param_device_connection.device_config.dp_config.stream = stream;
            dp_stream = stream;
            ALOGI("%s: plugin device cont %d stream %d", __func__, controller, stream);
        }

        if (device) {
            pal_device_ids = (pal_device_id_t *) calloc(1, sizeof(pal_device_id_t));
            pal_device_count = GetPalDeviceIds({device}, pal_device_ids);
            for (int i = 0; i < pal_device_count; i++) {
                param_device_connection.connection_state = false;
                param_device_connection.id = pal_device_ids[i];
                ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
                        (void*)&param_device_connection,
                        sizeof(pal_param_device_connection_t));
                if (ret!=0) {
                    ALOGE("%s: pal set param failed for device disconnect", __func__);
                }
                ALOGI("%s: pal set param sucess for device disconnect", __func__);
            }
            if (pal_device_ids) {
                free(pal_device_ids);
                pal_device_ids = NULL;
            }
        }
    }

    if (pal_device_ids) {
        free(pal_device_ids);
        pal_device_ids = NULL;
    }

    ret = str_parms_get_str(parms, "BT_SCO", value, sizeof(value));
    if (ret >= 0) {
        pal_param_btsco_t param_bt_sco;
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            param_bt_sco.bt_sco_on = true;
        else
            param_bt_sco.bt_sco_on = false;

        ALOGI("%s: BTSCO on = %d", __func__, param_bt_sco.bt_sco_on);
        ret = pal_set_param(PAL_PARAM_ID_BT_SCO, (void *)&param_bt_sco,
                            sizeof(pal_param_btsco_t));
    }

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_BT_SCO_WB, value, sizeof(value));
    if (ret >= 0) {
        pal_param_btsco_t param_bt_sco;
        if (strcmp(value, AUDIO_PARAMETER_VALUE_ON) == 0)
            param_bt_sco.bt_wb_speech_enabled = true;
        else
            param_bt_sco.bt_wb_speech_enabled = false;

        ALOGI("%s: BTSCO WB mode = %d", __func__, param_bt_sco.bt_wb_speech_enabled);
        ret = pal_set_param(PAL_PARAM_ID_BT_SCO_WB, (void *)&param_bt_sco,
                            sizeof(pal_param_btsco_t));
     }

    AudioExtn::audio_extn_hfp_set_parameters(adev_, parms);

    ret = str_parms_get_str(parms, AUDIO_PARAMETER_RECONFIG_A2DP, value, sizeof(value));
    if (ret >= 0) {
        pal_param_bta2dp_t param_bt_a2dp;
        param_bt_a2dp.reconfigured = true;

        ALOGI("%s: BT A2DP Reconfig command received", __func__);
        ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_RECONFIG, (void *)&param_bt_a2dp,
                            sizeof(pal_param_bta2dp_t));
    }

    ret = str_parms_get_str(parms, "A2dpSuspended" , value, sizeof(value));
    if (ret >= 0) {
        pal_param_bta2dp_t param_bt_a2dp;

        if (strncmp(value, "true", 4) == 0)
            param_bt_a2dp.a2dp_suspended = true;
        else
            param_bt_a2dp.a2dp_suspended = false;

        ALOGI("%s: BT A2DP Suspended = %s, command received", __func__, value);
        ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void *)&param_bt_a2dp,
                            sizeof(pal_param_bta2dp_t));
    }

    ret = str_parms_get_str(parms, "TwsChannelConfig", value, sizeof(value));
    if (ret >= 0) {
        pal_param_bta2dp_t param_bt_a2dp;

        ALOGI("Setting tws channel mode to %s", value);
        if (!(strncmp(value, "mono", strlen(value))))
           param_bt_a2dp.is_tws_mono_mode_on = true;
        else if (!(strncmp(value,"dual-mono",strlen(value))))
            param_bt_a2dp.is_tws_mono_mode_on = false;
        ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_TWS_CONFIG, (void *)&param_bt_a2dp,
                            sizeof(pal_param_bta2dp_t));
    }

    ret = str_parms_get_str(parms, "bt_swb", value, sizeof(value));
    if (ret >= 0) {
        pal_param_btsco_t param_bt_sco;

        val = atoi(value);
        param_bt_sco.bt_swb_speech_mode = val;
        ALOGI("%s: BTSCO SWB mode = 0x%x", __func__, val);
        ret = pal_set_param(PAL_PARAM_ID_BT_SCO_SWB, (void *)&param_bt_sco,
                            sizeof(pal_param_btsco_t));
    }

    str_parms_destroy(parms);

    ALOGD("%s: exit: %s", __func__, kvpairs);
    return ret;
}

int AudioDevice::SetVoiceVolume(float volume) {
    int ret = 0;

    ret = voice_->SetVoiceVolume(volume);

    return ret;
}

char* AudioDevice::GetParameters(const char *keys) {
    int32_t ret;
    char *str;
    char value[256]={0};
    size_t size = 0;
    struct str_parms *reply = str_parms_create();
    struct str_parms *query = str_parms_create_str(keys);

    if (!query || !reply) {
        if (reply) {
            str_parms_destroy(reply);
        }
        if (query) {
            str_parms_destroy(query);
        }
        ALOGE("%s: failed to create query or reply", __func__);
        return NULL;
    }

    ret = str_parms_get_str(query, AUDIO_PARAMETER_A2DP_RECONFIG_SUPPORTED,
                            value, sizeof(value));
    if (ret >= 0) {
        pal_param_bta2dp_t *param_bt_a2dp;
        int32_t val = 0;

        ret = pal_get_param(PAL_PARAM_ID_BT_A2DP_RECONFIG_SUPPORTED,
                            (void **)&param_bt_a2dp, &size, nullptr);
        if (!ret) {
            if (size < sizeof(pal_param_bta2dp_t)) {
                ALOGE("Size returned is smaller for BT_A2DP_RECONFIG_SUPPORTED");
                goto exit;
            }
            val = param_bt_a2dp->reconfig_supported;
            str_parms_add_int(reply, AUDIO_PARAMETER_A2DP_RECONFIG_SUPPORTED, val);
            ALOGV("%s: isReconfigA2dpSupported = %d", __func__, val);
        }
    }

exit:
    str = str_parms_to_str(reply);
    str_parms_destroy(query);
    str_parms_destroy(reply);

    ALOGV_IF(str != NULL, "%s: exit: returns - %s", __func__, str);
    return str;
}

void AudioDevice::FillAndroidDeviceMap() {
    android_device_map_.clear();
    /* go through all devices and pushback */

    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_EARPIECE, PAL_DEVICE_OUT_HANDSET));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_SPEAKER, PAL_DEVICE_OUT_SPEAKER));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_WIRED_HEADSET, PAL_DEVICE_OUT_WIRED_HEADSET));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_WIRED_HEADPHONE, PAL_DEVICE_OUT_WIRED_HEADPHONE));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BLUETOOTH_SCO, PAL_DEVICE_OUT_BLUETOOTH_SCO));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET, PAL_DEVICE_OUT_BLUETOOTH_SCO));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT, PAL_DEVICE_OUT_BLUETOOTH_SCO_CARKIT));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP, PAL_DEVICE_OUT_BLUETOOTH_A2DP));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES, PAL_DEVICE_OUT_BLUETOOTH_A2DP_HEADPHONES));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER, PAL_DEVICE_OUT_BLUETOOTH_A2DP_SPEAKER));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_AUX_DIGITAL, PAL_DEVICE_OUT_AUX_DIGITAL));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_HDMI, PAL_DEVICE_OUT_HDMI));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_ANLG_DOCK_HEADSET, PAL_DEVICE_OUT_ANLG_DOCK_HEADSET));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_DGTL_DOCK_HEADSET, PAL_DEVICE_OUT_DGTL_DOCK_HEADSET));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_USB_ACCESSORY, PAL_DEVICE_OUT_USB_ACCESSORY));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_USB_DEVICE, PAL_DEVICE_OUT_USB_DEVICE));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_REMOTE_SUBMIX, PAL_DEVICE_OUT_REMOTE_SUBMIX));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_TELEPHONY_TX, PAL_DEVICE_NONE));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_LINE, PAL_DEVICE_OUT_WIRED_HEADPHONE));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_HDMI_ARC, PAL_DEVICE_OUT_HDMI_ARC));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_SPDIF, PAL_DEVICE_OUT_SPDIF));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_FM, PAL_DEVICE_OUT_FM));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_AUX_LINE, PAL_DEVICE_OUT_AUX_LINE));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_SPEAKER_SAFE, PAL_DEVICE_OUT_SPEAKER_SAFE));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_IP, PAL_DEVICE_OUT_IP));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_BUS, PAL_DEVICE_OUT_BUS));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_PROXY, PAL_DEVICE_OUT_PROXY));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_USB_HEADSET, PAL_DEVICE_OUT_USB_HEADSET));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_OUT_DEFAULT, PAL_DEVICE_OUT_SPEAKER));

    /* go through all in devices and pushback */

    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_BUILTIN_MIC, PAL_DEVICE_IN_HANDSET_MIC));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_BACK_MIC, PAL_DEVICE_IN_SPEAKER_MIC));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_COMMUNICATION, PAL_DEVICE_IN_COMMUNICATION));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_AMBIENT, PAL_DEVICE_IN_AMBIENT);
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET, PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_WIRED_HEADSET, PAL_DEVICE_IN_WIRED_HEADSET));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_AUX_DIGITAL, PAL_DEVICE_IN_AUX_DIGITAL));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_HDMI, PAL_DEVICE_IN_HDMI));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_VOICE_CALL, PAL_DEVICE_IN_HANDSET_MIC));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_TELEPHONY_RX, PAL_DEVICE_IN_TELEPHONY_RX);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_REMOTE_SUBMIX, PAL_DEVICE_IN_REMOTE_SUBMIX);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_ANLG_DOCK_HEADSET, PAL_DEVICE_IN_ANLG_DOCK_HEADSET);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_DGTL_DOCK_HEADSET, PAL_DEVICE_IN_DGTL_DOCK_HEADSET);
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_USB_ACCESSORY, PAL_DEVICE_IN_USB_ACCESSORY));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_USB_DEVICE, PAL_DEVICE_IN_USB_HEADSET));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_FM_TUNER, PAL_DEVICE_IN_FM_TUNER));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_TV_TUNER, PAL_DEVICE_IN_TV_TUNER);
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_LINE, PAL_DEVICE_IN_LINE));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_SPDIF, PAL_DEVICE_IN_SPDIF));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_BLUETOOTH_A2DP, PAL_DEVICE_IN_BLUETOOTH_A2DP);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_LOOPBACK, PAL_DEVICE_IN_LOOPBACK);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_IP, PAL_DEVICE_IN_IP);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_BUS, PAL_DEVICE_IN_BUS);
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_PROXY, PAL_DEVICE_IN_PROXY));
    android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_USB_HEADSET, PAL_DEVICE_IN_USB_HEADSET));
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_HDMI_ARC, PAL_DEVICE_IN_HDMI_ARC);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_BLUETOOTH_BLE, PAL_DEVICE_IN_BLUETOOTH_BLE);
    //android_device_map_.insert(std::make_pair(AUDIO_DEVICE_IN_DEFAULT, PAL_DEVICE_IN_DEFAULT));
}

int AudioDevice::GetPalDeviceIds(const std::set<audio_devices_t>& hal_device_ids,
                                 pal_device_id_t* pal_device_id) {
    int device_count = 0;
    if (!pal_device_id) {
        ALOGE("%s: invalid pal device id", __func__);
        goto error;
    }

    // pal device ids is supposed to have to space for the new ids
    ALOGD("%s: haldeviceIds: %zu", __func__, hal_device_ids.size());

    for(auto hal_device_id : hal_device_ids) {
        // skip AUDIO_DEVICE_NONE as device count not 0
        if (hal_device_id != AUDIO_DEVICE_NONE) {
            auto it = android_device_map_.find(hal_device_id);
            if (it != android_device_map_.end() &&
               audio_is_input_device(it->first) == audio_is_input_device(hal_device_id)) {
                ALOGD("%s: Found haldeviceId: %x and PAL Device ID %d", __func__,
                        it->first, it->second);
                if (it->second == PAL_DEVICE_OUT_AUX_DIGITAL ||
                        it->second == PAL_DEVICE_OUT_HDMI) {
                   ALOGE("%s: dp_controller: %d dp_stream: %d", __func__,
                           dp_controller, dp_stream);
                   if (dp_controller * MAX_STREAMS_PER_CONTROLLER + dp_stream) {
                      pal_device_id[device_count] = PAL_DEVICE_OUT_AUX_DIGITAL_1;
                   } else {
                      pal_device_id[device_count] = it->second;
                   }
                } else {
                   pal_device_id[device_count] = it->second;
                }
            }
        }
        ++device_count;
    }

error:
    ALOGD("%s: devices allocated %zu, pal device ids before returning %d",
          __func__, hal_device_ids.size(), device_count);
    return device_count;
}

void AudioDevice::SetChargingMode(bool is_charging) {
    int32_t result = 0;
    pal_param_charging_state_t charge_state;

    ALOGD("%s: enter, is_charging %d", __func__, is_charging);
    is_charging_ = is_charging;
    charge_state.charging_state = is_charging;

    result = pal_set_param(PAL_PARAM_ID_CHARGING_STATE, (void*)&charge_state,
                        sizeof(pal_param_charging_state_t));
    if (result)
        ALOGD("%s: error while handling charging event result(%d)\n",
            __func__, result);

    ALOGD("%s: exit", __func__);
}

hw_device_t* AudioDevice::GetAudioDeviceCommon()
{
    return &(adev_->device_.get()->common);
}

static int adev_open(const hw_module_t *module, const char *name __unused,
                     hw_device_t **device) {
    int32_t ret = 0;
    ALOGD("%s: enter", __func__);

    std::shared_ptr<AudioDevice> adevice = AudioDevice::GetInstance();

    if (!adevice) {
        ALOGE("%s: error, GetInstance failed", __func__);
    }

    adevice->adev_init_mutex.lock();
    if (adevice->adev_init_ref_count != 0) {
        *device = adevice->GetAudioDeviceCommon();
        adevice->adev_init_ref_count++;
        adevice->adev_init_mutex.unlock();
        ALOGD("%s: returning existing instance of adev, exiting", __func__);
        return 0;
    }

    ret = adevice->Init(device, module);

    if (ret || (*device == NULL)) {
        ALOGE("%s: error, audio device init failed, ret(%d),*device(%p)",
            __func__, ret, *device);
    }
    adevice->adev_init_mutex.unlock();

    ALOGV("%s: exit", __func__);
    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "QTI Audio HAL",
        .author = "The Linux Foundation",
        .methods = &hal_module_methods,
    },
};

audio_patch_handle_t AudioPatch::generate_patch_handle_l(){
    static audio_patch_handle_t handles = AUDIO_PATCH_HANDLE_NONE;
    if (++handles < 0)
        handles = AUDIO_PATCH_HANDLE_NONE + 1;
    return handles;
}

AudioPatch::AudioPatch(PatchType patch_type,
                       const std::vector<struct audio_port_config>& sources,
                       const std::vector<struct audio_port_config>& sinks):
                       type(patch_type), sources(sources), sinks(sinks){
        static std::mutex patch_lock;
        std::lock_guard<std::mutex> lock(patch_lock);
        handle = AudioPatch::generate_patch_handle_l();
}
