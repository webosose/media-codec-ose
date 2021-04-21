// Copyright (c) 2021 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0


#ifndef SRC_ENCODER_GST_VIDEO_ENCODER_H_
#define SRC_ENCODER_GST_VIDEO_ENCODER_H_

#include "base/video_encoder.h"

#include <gst/gst.h>

using namespace std;

namespace mcil {

namespace encoder {

class GstVideoEncoder : public VideoEncoder {
 public:
  GstVideoEncoder();
  ~GstVideoEncoder();

  bool Init(const ENCODER_INIT_DATA_T* loadData,
            NEWFRAME_CALLBACK_T new_frame_cb) override;
  bool Deinit() override;
  MCIL_MEDIA_STATUS_T Feed(const uint8_t* bufferPtr, size_t bufferSize) override;

  bool UpdateEncodingParams(const ENCODING_PARAMS_T* properties) override;

  static gboolean HandleBusMessage(
      GstBus *bus_, GstMessage *message, gpointer user_data);

 private:
  bool CreatePipeline(const ENCODER_INIT_DATA_T* loadData);
  bool CreateEncoder(MCIL_VIDEO_CODEC codecType);
  bool CreateSink();
  bool LinkElements(const ENCODER_INIT_DATA_T* loadData);

  static GstFlowReturn OnNewSample(GstElement* elt, gpointer* data);

  GstBus *bus_ = nullptr;
  bool load_complete_ = false;
  GstElement *pipeline_ = nullptr;
  GstElement *source_ = nullptr;
  GstElement *filter_YUY2_ = nullptr;
  GstElement *parse_ = nullptr;
  GstElement *converter_ = nullptr;
  GstElement *filter_NV12_ = nullptr;
  GstElement *encoder_ = nullptr;
  GstElement *sink_ = nullptr;
  GstCaps *caps_YUY2_ = nullptr;
  GstCaps *caps_NV12_ = nullptr;
  CALLBACK_T cbFunction_ = nullptr;

  int32_t bitrate_ = 0;
};

}  // namespace encoder

}  // namespace mcil

#endif  // SRC_ENCODER_GST_VIDEO_ENCODER_H_
