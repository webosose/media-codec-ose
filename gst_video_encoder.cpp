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

#include "gst_video_encoder.h"

#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <cmath>
#include <cstring>
#include <sstream>
#include <map>
#include <memory>
#include <gio/gio.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>

#include "base/log.h"

const std::string kFormatYUV = "YUY2";
#define CURR_TIME_INTERVAL_MS    100
#define LOAD_DONE_TIMEOUT_MS     10

#define MEDIA_VIDEO_MAX      (15 * 1024 * 1024)  // 15MB
#define QUEUE_MAX_SIZE       (12 * 1024 * 1024)  // 12MB
#define QUEUE_MAX_TIME       (10 * GST_SECOND)   // 10Secs

#define BUFFER_MIN_PERCENT 50
#define MEDIA_CHANNEL_MAX  2

#define V4L2ENCODER

namespace mcil {

namespace encoder {

std::shared_ptr<VideoEncoder> VideoEncoder::Create(MCIL_VIDEO_CODEC type) {
  return std::make_shared<mcil::encoder::GstVideoEncoder>();
}

bool VideoEncoder::IsCodecSupported(MCIL_VIDEO_CODEC videoCodec) {
  MCIL_INFO_PRINT("%d %s videoCodec(%d)", __LINE__, __FUNCTION__, videoCodec);
  return (videoCodec == MCIL_VIDEO_CODEC_H264);
}

GstVideoEncoder::GstVideoEncoder()
 : VideoEncoder() {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);
}

GstVideoEncoder::~GstVideoEncoder() {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);
  gst_element_set_state(pipeline_, GST_STATE_NULL);
}

bool GstVideoEncoder::Init(const ENCODER_INIT_DATA_T* loadData,
                            NEWFRAME_CALLBACK_T new_frame_cb) {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);

  if (!CreatePipeline(loadData)) {
    MCIL_INFO_PRINT("CreatePipeline Failed");
    return false;
  }

  VideoEncoder::Init(loadData, new_frame_cb);
  bitrate_ = 0;

  return true;
}

bool GstVideoEncoder::Deinit() {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);
  return true;
}

MCIL_MEDIA_STATUS_T GstVideoEncoder::Feed(const uint8_t* bufferPtr,
                                       size_t bufferSize) {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);

  if (!pipeline_) {
    MCIL_INFO_PRINT("Pipeline is null");
    return MCIL_MEDIA_ERROR;
  }

  guint8 *feedBuffer = (guint8 *)g_malloc(bufferSize);
  if (feedBuffer == NULL) {
    MCIL_DEBUG_PRINT("memory allocation error!!!!!");
    return MCIL_MEDIA_ERROR;
  }

  memcpy(feedBuffer, bufferPtr, bufferSize);

  MCIL_INFO_PRINT("bufferPtr(%p) length:%lu\n", feedBuffer, bufferSize);
  GstBuffer *gstBuffer = gst_buffer_new_wrapped(feedBuffer, bufferSize);
  if (!gstBuffer) {
    MCIL_INFO_PRINT("Buffer wrapping error");
    return MCIL_MEDIA_ERROR;
  }

  GstFlowReturn gstReturn = gst_app_src_push_buffer((GstAppSrc*)source_,
                                                    gstBuffer);
  if (gstReturn < GST_FLOW_OK) {
    MCIL_INFO_PRINT("gst_app_src_push_buffer errCode[ %d ]", gstReturn);
    return MCIL_MEDIA_ERROR;
  }
  return MCIL_MEDIA_OK;
}

bool GstVideoEncoder::UpdateEncodingParams(const ENCODING_PARAMS_T* properties) {
  if (encoder_ && properties->bitRate > 0 && bitrate_ != properties->bitRate) {
    MCIL_INFO_PRINT("%d %s : video_bitrate=%d", __LINE__, __FUNCTION__, properties->bitRate);
#if defined(V4L2ENCODER)
    GstStructure* extraCtrls = gst_structure_new ("extra-controls",
                                                  "video_bitrate", G_TYPE_INT, properties->bitRate,
                                                  NULL);
    g_object_set(G_OBJECT(encoder_), "extra-controls", extraCtrls, NULL);
#else
    g_object_set(G_OBJECT(encoder_), "target-bitrate", properties->bitRate, NULL);
#endif
    bitrate_ = properties->bitRate;
  }
  return true;
}

bool GstVideoEncoder::CreateEncoder(MCIL_VIDEO_CODEC codecType) {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);

  if (codecType == MCIL_VIDEO_CODEC_H264) {
#if defined(V4L2ENCODER)
    encoder_ = gst_element_factory_make ("v4l2h264enc", "encoder");
    MCIL_INFO_PRINT("use v4l2h264enc");
#else
    encoder_ = gst_element_factory_make ("omxh264enc", "encoder");
    MCIL_INFO_PRINT("use omxh264enc");
#endif
  } else if (codecType == MCIL_VIDEO_CODEC_VP8) {
    encoder_ = gst_element_factory_make ("omxvp8enc", "encoder");
  } else {
    MCIL_INFO_PRINT("%d %s ==> Unsupported Codedc", __LINE__, __FUNCTION__);
    return false;
  }

  if (!encoder_) {
    MCIL_INFO_PRINT("encoder_ element creation failed.");
    return false;
  }
  return true;
}

bool GstVideoEncoder::CreateSink() {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);
  sink_ = gst_element_factory_make ("appsink", "sink");
  if (!sink_) {
    MCIL_INFO_PRINT("sink_ element creation failed.");
    return false;
  }
  g_object_set (G_OBJECT (sink_), "emit-signals", TRUE, "sync", FALSE, NULL);
  g_signal_connect(sink_, "new-sample", G_CALLBACK(OnNewSample), this);

  return true;
}

bool GstVideoEncoder::LinkElements(const ENCODER_INIT_DATA_T* loadData) {
  MCIL_INFO_PRINT("%d %s, width: %d, height: %d", __LINE__, __FUNCTION__, loadData->width, loadData->height);

  filter_YUY2_ = gst_element_factory_make("capsfilter", "filter-YUY2");
  if (!filter_YUY2_) {
    MCIL_INFO_PRINT("filter_YUY2_(%p) Failed", filter_YUY2_);
    return false;
  }

  caps_YUY2_ = gst_caps_new_simple("video/x-raw",
                                   "width", G_TYPE_INT, loadData->width,
                                   "height", G_TYPE_INT, loadData->height,
                                   "framerate", GST_TYPE_FRACTION, loadData->frameRate, 1,
                                   "format", G_TYPE_STRING, "I420",
                                   NULL);
  g_object_set(G_OBJECT(filter_YUY2_), "caps", caps_YUY2_, NULL);

#if defined(V4L2ENCODER)
  MCIL_INFO_PRINT("Do not use NV12 caps filter for V4L2");
#else
  filter_NV12_ = gst_element_factory_make("capsfilter", "filter-NV");
  if (!filter_NV12_) {
    MCIL_INFO_PRINT("filter_ element creation failed.");
    return false;
  }

  caps_NV12_ = gst_caps_new_simple("video/x-raw",
                                   "format", G_TYPE_STRING, "NV12",
                                   NULL);
  g_object_set(G_OBJECT(filter_NV12_), "caps", caps_NV12_, NULL);
#endif

  converter_ = gst_element_factory_make("videoconvert", "converted");
  if (!converter_) {
    MCIL_INFO_PRINT("converter_(%p) Failed", converter_);
    return false;
  }

  parse_ = gst_element_factory_make("rawvideoparse", "parser");
  if (!parse_) {
    MCIL_INFO_PRINT("parse_(%p) Failed", parse_);
    return false;
  }

  g_object_set(G_OBJECT(parse_), "width", loadData->width, NULL);
  g_object_set(G_OBJECT(parse_), "height", loadData->height, NULL);
  if (MCIL_PIXEL_I420 == loadData->pixelFormat) {
    g_object_set(G_OBJECT(parse_), "format", 2, NULL);
  }

#if defined(V4L2ENCODER)
  gst_bin_add_many(GST_BIN(pipeline_), source_, filter_YUY2_, parse_, converter_, encoder_, sink_, NULL);
#else
  gst_bin_add_many(GST_BIN(pipeline_), source_, filter_YUY2_, parse_, converter_, filter_NV12_, encoder_, sink_, NULL);
#endif
  MCIL_INFO_PRINT(" GstVideoEncoder elements added to bin  \n ");

  if (TRUE != gst_element_link(source_, filter_YUY2_)) {
    MCIL_INFO_PRINT ("elements could not be linked - source_ & filter_YUY2 \n");
    return false;
  }

  if (TRUE != gst_element_link(filter_YUY2_, parse_)) {
    MCIL_INFO_PRINT ("elements could not be linked - filter_YUY2 & converter_ \n");
      return false;
  }

  if (TRUE != gst_element_link(parse_, converter_)) {
    MCIL_INFO_PRINT ("elements could not be linked - parse_ & converter_ \n");
    return false;
  }

#if defined(V4L2ENCODER)
  if (TRUE != gst_element_link(converter_, encoder_)) {
    MCIL_INFO_PRINT ("elements could not be linked - converter_ & encoder_ \n");
    return false;
  }
#else
  if (TRUE != gst_element_link(converter_, filter_NV12_)) {
    MCIL_INFO_PRINT ("elements could not be linked - converter_ & filter_NV12_ \n");
    return false;
  }

  if (TRUE != gst_element_link(filter_NV12_, encoder_)) {
    MCIL_INFO_PRINT ("elements could not be linked - filter_NV12_ & encoder_ \n");
    return false;
  }
#endif

  if (TRUE != gst_element_link(encoder_, sink_)) {
    MCIL_INFO_PRINT ("elements could not be linked - encoder_ & sink_ \n");
    return false;
  }

  return true;
}

gboolean GstVideoEncoder::HandleBusMessage(
    GstBus *bus_, GstMessage *message, gpointer user_data)
{
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);
  GstMessageType messageType = GST_MESSAGE_TYPE(message);
  if (messageType != GST_MESSAGE_QOS && messageType != GST_MESSAGE_TAG) {
    MCIL_INFO_PRINT("Element[ %s ][ %d ][ %s ]",
                    GST_MESSAGE_SRC_NAME(message),
                    messageType, gst_message_type_get_name(messageType));
  }

  GstVideoEncoder *encoder = reinterpret_cast<GstVideoEncoder *>(user_data);
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      if (encoder->cbFunction_) {
        ENCODING_ERRORS_T error;
        error.errorCode = 5;
        error.errorStr =(gchar *)"Dummy Str";
        encoder->cbFunction_(ENCODER_CB_NOTIFY_ERROR, 0, nullptr, &error);
      }
      break;
    }

    case GST_MESSAGE_EOS: {
      MCIL_INFO_PRINT("Got endOfStream");
      if (encoder->cbFunction_)
        encoder->cbFunction_(ENCODER_CB_NOTIFY_EOS, 0, nullptr, nullptr);
      break;
    }

    case GST_MESSAGE_ASYNC_DONE: {
      MCIL_INFO_PRINT("ASYNC DONE");
      if (!encoder->load_complete_ && encoder->cbFunction_) {
        encoder->cbFunction_(ENCODER_CB_LOAD_COMPLETE, 0, nullptr, nullptr);
        encoder->load_complete_ = true;
      }
      break;
    }

    case GST_STATE_PAUSED: {
      MCIL_INFO_PRINT("PAUSED");
      if (encoder->cbFunction_)
        encoder->cbFunction_(ENCODER_CB_NOTIFY_PAUSED, 0, nullptr, nullptr);
      break;
    }

    case GST_STATE_PLAYING: {
      MCIL_INFO_PRINT("PLAYING");
      if (encoder->cbFunction_)
        encoder->cbFunction_(ENCODER_CB_NOTIFY_PLAYING, 0, nullptr, nullptr);
      break;
    }

    case GST_MESSAGE_STATE_CHANGED: {
      GstState oldState = GST_STATE_NULL;
      GstState newState = GST_STATE_NULL;
      gst_message_parse_state_changed(message,
                                      &oldState, &newState, NULL);
      MCIL_INFO_PRINT("Element[%s] State changed ...%s -> %s",
                      GST_MESSAGE_SRC_NAME(message),
                      gst_element_state_get_name(oldState),
                      gst_element_state_get_name(newState));
      break;
    }

    default:
      break;
  }

  return true;
}

bool GstVideoEncoder::CreatePipeline(const ENCODER_INIT_DATA_T* loadData) {
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);

  gst_init(NULL, NULL);
  gst_pb_utils_init();
  pipeline_ = gst_pipeline_new("video-encoder");
  MCIL_INFO_PRINT("pipeline_ = %p", pipeline_);
  if (!pipeline_) {
    MCIL_INFO_PRINT("Cannot create encoder pipeline!");
    return false;
  }

  source_ = gst_element_factory_make ("appsrc", "app-source");
  if (!source_) {
    MCIL_INFO_PRINT("source_ element creation failed.");
    return false;
  }
  g_object_set(source_, "format", GST_FORMAT_TIME, NULL);
  g_object_set(source_, "do-timestamp", true, NULL);

  if (!CreateEncoder(loadData->codecType))
  {
    MCIL_INFO_PRINT("Encoder creation failed !!!");
    return false;
  }

  if (!CreateSink())
  {
    MCIL_INFO_PRINT("Sink creation failed !!!");
    return false;
  }

  if (!LinkElements(loadData))
  {
    MCIL_INFO_PRINT("element linking failed !!!");
    return false;
  }

  bus_ = gst_pipeline_get_bus(GST_PIPELINE (pipeline_));
  gst_bus_add_watch(bus_, GstVideoEncoder::HandleBusMessage, this);
  gst_object_unref(bus_);

  return gst_element_set_state(pipeline_, GST_STATE_PLAYING);
}

/* called when the appsink notifies us that there is a new buffer ready for
 * processing */
GstFlowReturn GstVideoEncoder::OnNewSample(GstElement* elt, gpointer* data)
{
  MCIL_INFO_PRINT("%d %s", __LINE__, __FUNCTION__);
  GstVideoEncoder *encoder = reinterpret_cast<GstVideoEncoder*>(data);
  GstSample *sample;
  GstBuffer *app_buffer, *buffer;
  GstElement *source;
  GstFlowReturn ret;

  /* get the sample from appsink */
  sample = gst_app_sink_pull_sample (GST_APP_SINK (elt));
  if (NULL != sample) {
    GstMapInfo map = {};
    GstBuffer *buffer;
    buffer = gst_sample_get_buffer(sample);
    gst_buffer_map(buffer, &map, GST_MAP_READ);
    MCIL_INFO_PRINT("%d %s data size:%lu", __LINE__, __FUNCTION__, map.size);

    if ((NULL == map.data) && (map.size == 0)) {
      MCIL_INFO_PRINT("%d %s Empty buffer", __LINE__, __FUNCTION__);
      return GST_FLOW_OK;
    }

    bool is_key_frame = false;
    if(!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)){
      is_key_frame = true;
    }

    uint64_t bufferTimestamp = GST_BUFFER_TIMESTAMP(buffer);
    if (nullptr != encoder->new_frame_cb_) {
      encoder->new_frame_cb_(static_cast<const uint8_t*>(map.data), map.size,
                             bufferTimestamp, is_key_frame);
    }

    gst_sample_unref(sample);
    gst_buffer_unmap(buffer, &map);
  }
  return GST_FLOW_OK;
}

}  // namespace encoder

}  // namespace mcil
