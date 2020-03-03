/******************************************************************************
 * Copyright (C) 2017, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 ******************************************************************************/
/******************************************************************************
 *
 *  Copyright (C) 2016 The Android Open Source Project
 *  Copyright (C) 2009-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_a2dp_sink"

#include <string.h>

#include "bt_common.h"
#include "btif_a2dp.h"
#include "btif_a2dp_sink.h"
#include "btif_av.h"
#include "btif_avk.h"
#include "bta_avk_api.h"
#include "btif_av_co.h"
#include "btif_avrcp_audio_track.h"
#include "btif_util.h"
#include "osi/include/fixed_queue.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "osi/include/thread.h"

#include "oi_codec_sbc.h"
#include "oi_status.h"

/**
 * The receiving queue buffer size.
 */
#define MAX_INPUT_A2DP_FRAME_QUEUE_SZ (MAX_PCM_FRAME_NUM_PER_TICK * 4)

#define BTIF_SINK_MEDIA_TIME_TICK_MS 20

/* define APTX/AAC incoming data time interval */
#define BTIF_SINK_MEDIA_TIME_NON_SBC_TICK_MS 10

/* In case of A2DP Sink, we will delay start by 5 AVDTP Packets */
#define MAX_A2DP_DELAYED_START_FRAME_COUNT 5

#define MAX_SINK_MEDIA_WORKQUEUE_COUNT 1024
#define BTIF_DELAYED_CREATE_AUDIO_TRACK_MS 120

#define TRACK_CREATE_MAX_RETRY_ATTEMPTS 2

enum {
  BTIF_A2DP_SINK_STATE_OFF,
  BTIF_A2DP_SINK_STATE_STARTING_UP,
  BTIF_A2DP_SINK_STATE_RUNNING,
  BTIF_A2DP_SINK_STATE_SHUTTING_DOWN
};

/* BTIF Media Sink command event definition */
enum {
  BTIF_MEDIA_SINK_DECODER_UPDATE = 1,
  BTIF_MEDIA_SINK_CLEAR_TRACK,
  BTIF_MEDIA_SINK_SET_FOCUS_STATE,
  BTIF_MEDIA_SINK_AUDIO_RX_FLUSH
};

/* BTIF Audio Track status definition */
enum {
  BTIF_AUDIO_TRACK_STATUS_IDLE,
  BTIF_AUDIO_TRACK_STATUS_STARTED,
  BTIF_AUDIO_TRACK_STATUS_UPDATEING,
  BTIF_AUDIO_TRACK_STATUS_UPDATED
};

typedef struct {
  BT_HDR hdr;
  uint8_t codec_info[AVDT_CODEC_SIZE];
  int index;
} tBTIF_MEDIA_SINK_DECODER_UPDATE;

typedef struct {
  BT_HDR hdr;
  btif_a2dp_sink_focus_state_t focus_state;
} tBTIF_MEDIA_SINK_FOCUS_UPDATE;

typedef struct {
  uint16_t num_frames_to_be_processed;
  uint16_t len;
  uint16_t offset;
  uint16_t layer_specific;
  uint64_t enque_ns;
} tBT_SINK_HDR;

extern uint64_t btif_avk_update_reported_delay(uint64_t inst_delay);
extern bool btif_avk_is_sink_delay_report_supported();
extern int btif_max_avk_clients;

/* BTIF A2DP Sink control block */
typedef struct {
  thread_t* worker_thread;
  fixed_queue_t* cmd_msg_queue;
  fixed_queue_t* rx_audio_queue;
  bool rx_flush; /* discards any incoming data when true */
  alarm_t* decode_alarm;
  uint8_t frames_to_process;
  tA2DP_SAMPLE_RATE sample_rate;
  tA2DP_CHANNEL_COUNT channel_count;
  btif_a2dp_sink_focus_state_t rx_focus_state; /* audio focus state */
  void* audio_track;
  int index;
  uint8_t audio_track_status;
  std::mutex audio_track_mutex;
  alarm_t* audio_track_alarm;
  uint8_t track_create_retry_cnt;
  uint32_t latency; /* latency of rendering Audio samples at MMAudio */
  tA2DP_CODEC_TYPE codec_type;
} tBTIF_A2DP_SINK_CB;

static tBTIF_A2DP_SINK_CB btif_a2dp_sink_cb;

static int btif_a2dp_sink_state = BTIF_A2DP_SINK_STATE_OFF;

static OI_CODEC_SBC_DECODER_CONTEXT btif_a2dp_sink_context;
static uint32_t btif_a2dp_sink_context_data[CODEC_DATA_WORDS(
    2, SBC_CODEC_FAST_FILTER_BUFFERS)];
static int16_t
    btif_a2dp_sink_pcm_data[15 * SBC_MAX_SAMPLES_PER_FRAME * SBC_MAX_CHANNELS];

static void btif_a2dp_sink_startup_delayed(void* context);
static void btif_a2dp_sink_shutdown_delayed(void* context);
static void btif_a2dp_sink_command_ready(fixed_queue_t* queue, void* context);
static void btif_decode_alarm_cb(void* context);
static void btif_a2dp_sink_audio_handle_start_decoding(void);
static void btif_a2dp_sink_avk_handle_timer(UNUSED_ATTR void* context);
static void btif_a2dp_sink_audio_rx_flush_req(void);
/* Handle incoming media packets A2DP SINK streaming */
static void btif_a2dp_sink_handle_inc_media(tBT_SINK_HDR* p_msg);
static void btif_a2dp_sink_decoder_update_event(
    tBTIF_MEDIA_SINK_DECODER_UPDATE* p_buf);
static void btif_a2dp_sink_set_focus_state_event(
    btif_a2dp_sink_focus_state_t state);
static void btif_a2dp_sink_audio_rx_flush_event(void);
static void btif_a2dp_sink_clear_track_event_req(void);

/*Handle APTX/AAC incoming data ***/
static void btif_non_sbc_decode_alarm_cb(void* context);
static void btif_a2dp_sink_audio_handle_start_non_sbc_decoding(void);
static void btif_handle_incoming_encoded_data(UNUSED_ATTR void* context);

UNUSED_ATTR static const char* dump_media_event(uint16_t event) {
  switch (event) {
    CASE_RETURN_STR(BTIF_MEDIA_SINK_DECODER_UPDATE)
    CASE_RETURN_STR(BTIF_MEDIA_SINK_CLEAR_TRACK)
    CASE_RETURN_STR(BTIF_MEDIA_SINK_SET_FOCUS_STATE)
    CASE_RETURN_STR(BTIF_MEDIA_SINK_AUDIO_RX_FLUSH)
    default:
      break;
  }
  return "UNKNOWN A2DP SINK EVENT";
}

bool btif_a2dp_sink_startup(void) {
  if (btif_a2dp_sink_state != BTIF_A2DP_SINK_STATE_OFF) {
    BTIF_TRACE_ERROR("%s: A2DP Sink media task already running", __func__);
    return false;
  }

  memset(&btif_a2dp_sink_cb, 0, sizeof(btif_a2dp_sink_cb));
  btif_a2dp_sink_state = BTIF_A2DP_SINK_STATE_STARTING_UP;

  BTIF_TRACE_EVENT("## A2DP SINK START MEDIA THREAD ##");

  /* Start A2DP Sink media task */
  btif_a2dp_sink_cb.worker_thread =
     thread_new_sized("btif_a2dp_sink_worker_thread", MAX_SINK_MEDIA_WORKQUEUE_COUNT);
  if (btif_a2dp_sink_cb.worker_thread == NULL) {
    BTIF_TRACE_ERROR("%s: unable to start up media thread", __func__);
    btif_a2dp_sink_state = BTIF_A2DP_SINK_STATE_OFF;
    return false;
  }

  btif_a2dp_sink_cb.rx_focus_state = BTIF_A2DP_SINK_FOCUS_NOT_GRANTED;
  btif_a2dp_sink_cb.audio_track = NULL;
  btif_a2dp_sink_cb.index = btif_max_avk_clients;
  btif_a2dp_sink_cb.track_create_retry_cnt = 0;
  btif_a2dp_sink_cb.audio_track_status = BTIF_AUDIO_TRACK_STATUS_IDLE;
  btif_a2dp_sink_cb.audio_track_alarm = alarm_new("btif.a2dp_sink_create_audiotrack");


  btif_a2dp_sink_cb.rx_audio_queue = fixed_queue_new(SIZE_MAX);

  btif_a2dp_sink_cb.cmd_msg_queue = fixed_queue_new(SIZE_MAX);
  fixed_queue_register_dequeue(
      btif_a2dp_sink_cb.cmd_msg_queue,
      thread_get_reactor(btif_a2dp_sink_cb.worker_thread),
      btif_a2dp_sink_command_ready, NULL);

  BTIF_TRACE_EVENT("## A2DP SINK MEDIA THREAD STARTED ##");

  /* Schedule the rest of the startup operations */
  thread_post(btif_a2dp_sink_cb.worker_thread, btif_a2dp_sink_startup_delayed,
              NULL);

  return true;
}

static void btif_a2dp_sink_startup_delayed(UNUSED_ATTR void* context) {
  raise_priority_a2dp(TASK_HIGH_MEDIA);
  btif_a2dp_sink_state = BTIF_A2DP_SINK_STATE_RUNNING;
}

void btif_a2dp_sink_shutdown(void) {
  if ((btif_a2dp_sink_state == BTIF_A2DP_SINK_STATE_OFF) ||
      (btif_a2dp_sink_state == BTIF_A2DP_SINK_STATE_SHUTTING_DOWN)) {
    return;
  }

  /* Make sure no channels are restarted while shutting down */
  btif_a2dp_sink_state = BTIF_A2DP_SINK_STATE_SHUTTING_DOWN;

  BTIF_TRACE_EVENT("## A2DP SINK STOP MEDIA THREAD ##");

  // Stop the timer
  alarm_free(btif_a2dp_sink_cb.decode_alarm);
  btif_a2dp_sink_cb.decode_alarm = NULL;

  alarm_free(btif_a2dp_sink_cb.audio_track_alarm);
  btif_a2dp_sink_cb.audio_track_alarm = NULL;
  btif_a2dp_sink_cb.track_create_retry_cnt = 0;
  // Exit the thread
  fixed_queue_free(btif_a2dp_sink_cb.cmd_msg_queue, NULL);
  btif_a2dp_sink_cb.cmd_msg_queue = NULL;
  thread_post(btif_a2dp_sink_cb.worker_thread, btif_a2dp_sink_shutdown_delayed,
              NULL);
  thread_free(btif_a2dp_sink_cb.worker_thread);
  btif_a2dp_sink_cb.worker_thread = NULL;
}

static void btif_a2dp_sink_shutdown_delayed(UNUSED_ATTR void* context) {
  fixed_queue_free(btif_a2dp_sink_cb.rx_audio_queue, NULL);
  btif_a2dp_sink_cb.rx_audio_queue = NULL;

  btif_a2dp_sink_state = BTIF_A2DP_SINK_STATE_OFF;
}

tA2DP_SAMPLE_RATE btif_a2dp_sink_get_sample_rate(void) {
  return btif_a2dp_sink_cb.sample_rate;
}

tA2DP_CHANNEL_COUNT btif_a2dp_sink_get_channel_count(void) {
  return btif_a2dp_sink_cb.channel_count;
}

uint8_t btif_a2dp_sink_get_codec_type(void) {
  return btif_a2dp_sink_cb.codec_type;
}

static void btif_a2dp_sink_command_ready(fixed_queue_t* queue,
                                         UNUSED_ATTR void* context) {
  BT_HDR* p_msg = (BT_HDR*)fixed_queue_dequeue(queue);

  LOG_DEBUG(LOG_TAG, "%s: event %d %s", __func__, p_msg->event,
              dump_media_event(p_msg->event));

  switch (p_msg->event) {
    case BTIF_MEDIA_SINK_DECODER_UPDATE:
      btif_a2dp_sink_decoder_update_event(
          (tBTIF_MEDIA_SINK_DECODER_UPDATE*)p_msg);
      break;
    case BTIF_MEDIA_SINK_CLEAR_TRACK:
      btif_a2dp_sink_clear_track_event();
      break;
    case BTIF_MEDIA_SINK_SET_FOCUS_STATE: {
      btif_a2dp_sink_focus_state_t state =
          ((tBTIF_MEDIA_SINK_FOCUS_UPDATE*)p_msg)->focus_state;
      btif_a2dp_sink_set_focus_state_event(state);
      break;
    }
    case BTIF_MEDIA_SINK_AUDIO_RX_FLUSH:
      btif_a2dp_sink_audio_rx_flush_event();
      break;
    default:
      BTIF_TRACE_ERROR("ERROR in %s unknown event %d", __func__, p_msg->event);
      break;
  }

  osi_free(p_msg);
  LOG_VERBOSE(LOG_TAG, "%s: %s DONE", __func__, dump_media_event(p_msg->event));
}

void btif_a2dp_sink_update_decoder(const uint8_t* p_codec_info, int index) {

  if(p_codec_info) {
    tBTIF_MEDIA_SINK_DECODER_UPDATE* p_buf =
        reinterpret_cast<tBTIF_MEDIA_SINK_DECODER_UPDATE*>(
            osi_malloc(sizeof(tBTIF_MEDIA_SINK_DECODER_UPDATE)));

    BTIF_TRACE_DEBUG("%s: p_codec_info[%x:%x:%x:%x:%x:%x], index:%d", __func__,
                     p_codec_info[1], p_codec_info[2], p_codec_info[3],
                     p_codec_info[4], p_codec_info[5], p_codec_info[6],
                     index);
    btif_a2dp_sink_cb.audio_track_status = BTIF_AUDIO_TRACK_STATUS_UPDATEING;
    memcpy(p_buf->codec_info, p_codec_info, AVDT_CODEC_SIZE);
    p_buf->hdr.event = BTIF_MEDIA_SINK_DECODER_UPDATE;
    p_buf->index = index;
    fixed_queue_enqueue(btif_a2dp_sink_cb.cmd_msg_queue, p_buf);
  } else {
      btif_a2dp_sink_cb.index = index;
      BTIF_TRACE_DEBUG("%s only update index:%d",__func__, index);
  }
}

bool btif_a2dp_sink_check_codec(const uint8_t* p_codec_info) {
  if (btif_a2dp_sink_cb.index == btif_max_avk_clients)
      return true;
    tA2DP_CODEC_TYPE codec_type = A2DP_GetCodecType(p_codec_info);
    tA2DP_SAMPLE_RATE sample_rate = A2DP_GetTrackSampleRate(p_codec_info);
    tA2DP_CHANNEL_COUNT channel_count = A2DP_GetTrackChannelCount(p_codec_info);
    if ( codec_type    != btif_a2dp_sink_cb.codec_type    ||
         sample_rate   != btif_a2dp_sink_cb.sample_rate   ||
         channel_count != btif_a2dp_sink_cb.channel_count) {
        BTIF_TRACE_DEBUG("btif_a2dp_sink_check_codec not equal, should update audio_track");
        return true;
    } else {
        BTIF_TRACE_DEBUG("btif_a2dp_sink_check_codec equal, not update audio_track");
        return false;
    }
}

bool btif_a2dp_sink_check_sho(const uint8_t* p_codec_info, int index) {
    if (index != btif_a2dp_sink_cb.index || 
         btif_a2dp_sink_check_codec(p_codec_info)) {
        BTIF_TRACE_DEBUG("btif_a2dp_sink_check_sho true");
        return true;
    } else {
        BTIF_TRACE_DEBUG("btif_a2dp_sink_check_sho false");
        return false;
    }
}

void btif_a2dp_sink_on_idle(void) {
  BTIF_TRACE_EVENT("## ON A2DP IDLE ##");
  if (btif_a2dp_sink_state == BTIF_A2DP_SINK_STATE_OFF) return;

  btif_a2dp_sink_audio_handle_stop_decoding();
  btif_a2dp_sink_clear_track_event_req();
  BTIF_TRACE_DEBUG("Stopped BT track");
}

void btif_a2dp_sink_on_stopped(UNUSED_ATTR tBTA_AV_SUSPEND* p_av_suspend) {
  if (btif_a2dp_sink_state == BTIF_A2DP_SINK_STATE_OFF) return;

  btif_a2dp_sink_audio_handle_stop_decoding();
}

void btif_a2dp_sink_on_suspended(UNUSED_ATTR tBTA_AV_SUSPEND* p_av_suspend) {
  if (btif_a2dp_sink_state == BTIF_A2DP_SINK_STATE_OFF) return;

  btif_a2dp_sink_audio_handle_stop_decoding();
}

void btif_a2dp_sink_audio_handle_stop_decoding(void) {
  if (btif_a2dp_sink_cb.index == btif_max_avk_clients)
      return;
  btif_a2dp_sink_cb.rx_flush = true;
  btif_a2dp_sink_audio_rx_flush_req();

  alarm_free(btif_a2dp_sink_cb.decode_alarm);
  btif_a2dp_sink_cb.decode_alarm = NULL;
#ifndef OS_GENERIC
  {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    BtifAvrcpAudioTrackPause(btif_a2dp_sink_cb.audio_track);
    btif_a2dp_sink_cb.audio_track_status = BTIF_AUDIO_TRACK_STATUS_IDLE;
  }
#endif
}

static void btif_decode_alarm_cb(UNUSED_ATTR void* context) {
  if (btif_a2dp_sink_cb.worker_thread != NULL) {
    thread_post(btif_a2dp_sink_cb.worker_thread,
                btif_a2dp_sink_avk_handle_timer, NULL);
  }
}

static void btif_non_sbc_decode_alarm_cb(UNUSED_ATTR void* context){
  if (btif_a2dp_sink_cb.worker_thread != NULL) {
    thread_post(btif_a2dp_sink_cb.worker_thread,
        btif_handle_incoming_encoded_data, NULL);
  }
}

void btif_a2dp_sink_clear_track_event(void) {
  if (btif_a2dp_sink_cb.index == btif_max_avk_clients)
      return;
  BTIF_TRACE_DEBUG("%s", __func__);

#ifndef OS_GENERIC
  std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
  BtifAvrcpAudioTrackStop(btif_a2dp_sink_cb.audio_track);
  BtifAvrcpAudioTrackDelete(btif_a2dp_sink_cb.audio_track);
#endif
  btif_a2dp_sink_cb.audio_track = NULL;
  btif_a2dp_sink_cb.index = btif_max_avk_clients;
  btif_a2dp_sink_cb.latency = 0;
}

static void btif_a2dp_sink_audio_handle_start_decoding(void) {
  if (btif_a2dp_sink_cb.decode_alarm != NULL)
    return;  // Already started decoding

#ifndef OS_GENERIC
  {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    BtifAvrcpAudioTrackStart(btif_a2dp_sink_cb.audio_track);
  }
#endif

  btif_a2dp_sink_cb.decode_alarm = alarm_new_periodic("btif.a2dp_sink_decode");
  if (btif_a2dp_sink_cb.decode_alarm == NULL) {
    LOG_ERROR(LOG_TAG, "%s: unable to allocate decode alarm", __func__);
    return;
  }
  alarm_set(btif_a2dp_sink_cb.decode_alarm, BTIF_SINK_MEDIA_TIME_TICK_MS,
            btif_decode_alarm_cb, NULL);
  BTIF_TRACE_DEBUG("Track Started and decode_alarm is set");
}

static void btif_a2dp_sink_audio_handle_start_non_sbc_decoding(void){
  if (btif_a2dp_sink_cb.decode_alarm != NULL){
    APPL_TRACE_DEBUG("%s: Already started Incoming",__func__);
    return;  // Already started non_sbc_decoding
  }

#ifndef OS_GENERIC
    /* Avoid audiotrack start multiple times */
    if ((btif_a2dp_sink_cb.audio_track_status != BTIF_AUDIO_TRACK_STATUS_STARTED) &&
        (btif_a2dp_sink_cb.audio_track)) {
      std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
      BtifAvrcpAudioTrackStart(btif_a2dp_sink_cb.audio_track);
      btif_a2dp_sink_cb.audio_track_status = BTIF_AUDIO_TRACK_STATUS_STARTED;
    }
#endif

  btif_a2dp_sink_cb.decode_alarm = alarm_new_periodic("btif.a2dp_sink_non_sbc_decode");
  if (btif_a2dp_sink_cb.decode_alarm == NULL) {
    LOG_ERROR(LOG_TAG, "%s: unable to allocate non_sbc_decode_alarm", __func__);
    return;
  }
  alarm_set(btif_a2dp_sink_cb.decode_alarm, BTIF_SINK_MEDIA_TIME_NON_SBC_TICK_MS,
            btif_non_sbc_decode_alarm_cb, NULL);
  APPL_TRACE_DEBUG("Track Started and non_sbc_decode_alarm is set");
 }

static void btif_a2dp_sink_handle_inc_media(tBT_SINK_HDR* p_msg) {
  uint8_t* sbc_start_frame = ((uint8_t*)(p_msg + 1) + p_msg->offset + 1);
  int count;
  uint32_t pcmBytes, availPcmBytes;
  int16_t* pcmDataPointer =
      btif_a2dp_sink_pcm_data; /* Will be overwritten on next packet receipt */
  OI_STATUS status;
  int num_sbc_frames = p_msg->num_frames_to_be_processed;
  uint32_t sbc_frame_len = p_msg->len - 1;
  availPcmBytes = sizeof(btif_a2dp_sink_pcm_data);

  //if ((btif_av_get_peer_sep() == AVDT_TSEP_SNK) ||
  if ((btif_a2dp_sink_cb.rx_flush)) {
    BTIF_TRACE_DEBUG("State Changed happened in this tick");
    return;
  }

  BTIF_TRACE_DEBUG("%s Number of SBC frames %d, frame_len %d", __func__,
                   num_sbc_frames, sbc_frame_len);

  for (count = 0; count < num_sbc_frames && sbc_frame_len != 0; count++) {
    pcmBytes = availPcmBytes;
    status = OI_CODEC_SBC_DecodeFrame(
        &btif_a2dp_sink_context, (const OI_BYTE**)&sbc_start_frame,
        (uint32_t*)&sbc_frame_len, (int16_t*)pcmDataPointer,
        (uint32_t*)&pcmBytes);
    if (!OI_SUCCESS(status)) {
      BTIF_TRACE_ERROR("%s: Decoding failure: %d", __func__, status);
      break;
    }
    availPcmBytes -= pcmBytes;
    pcmDataPointer += pcmBytes / 2;
    p_msg->offset += (p_msg->len - 1) - sbc_frame_len;
    p_msg->len = sbc_frame_len + 1;
  }

#ifndef OS_GENERIC
  {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    BtifAvrcpAudioTrackWriteData(
      btif_a2dp_sink_cb.audio_track, (void*)btif_a2dp_sink_pcm_data,
      (sizeof(btif_a2dp_sink_pcm_data) - availPcmBytes));
  }
#endif
}

static void btif_a2dp_sink_avk_handle_timer(UNUSED_ATTR void* context) {
  tBT_SINK_HDR* p_msg;
  int num_sbc_frames;
  int num_frames_to_process;
  uint64_t inst_delay = 0;       /* avg delay incurred per frame in 20 ms */
  uint64_t inst_delay_total = 0; /* sum of delay for all frames processed till now */

  if (fixed_queue_is_empty(btif_a2dp_sink_cb.rx_audio_queue)) {
    BTIF_TRACE_DEBUG("%s: empty queue", __func__);
    return;
  }

  /* Don't do anything in case of focus not granted */
  if (btif_a2dp_sink_cb.rx_focus_state == BTIF_A2DP_SINK_FOCUS_NOT_GRANTED) {
    BTIF_TRACE_DEBUG("%s: skipping frames since focus is not present",
                     __func__);
    return;
  }
  /* Play only in BTIF_A2DP_SINK_FOCUS_GRANTED case */
  if (btif_a2dp_sink_cb.rx_flush) {
    fixed_queue_flush(btif_a2dp_sink_cb.rx_audio_queue, osi_free);
    return;
  }

  num_frames_to_process = btif_a2dp_sink_cb.frames_to_process;
  BTIF_TRACE_DEBUG(" Process Frames + ");

  do {
    p_msg = (tBT_SINK_HDR*)fixed_queue_try_peek_first(
        btif_a2dp_sink_cb.rx_audio_queue);
    if (p_msg == NULL) return;
    /* Number of frames in queue packets */
    num_sbc_frames = p_msg->num_frames_to_be_processed;
    BTIF_TRACE_DEBUG("Frames left in topmost packet %d", num_sbc_frames);
    BTIF_TRACE_DEBUG("Remaining frames to process in tick %d",
                     num_frames_to_process);
    BTIF_TRACE_DEBUG("Number of packets in queue %d",
                     fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue));

    if (num_sbc_frames > num_frames_to_process) {
      /* Queue packet has more frames */
      p_msg->num_frames_to_be_processed = num_frames_to_process;
      btif_a2dp_sink_handle_inc_media(p_msg);
      if (btif_avk_is_sink_delay_report_supported()) {
        struct timespec ts_now;
        uint64_t curr_time;
        clock_gettime(CLOCK_BOOTTIME, &ts_now);
        curr_time = (uint64_t)ts_now.tv_sec * 1000000000 + ts_now.tv_nsec;
        if (curr_time > p_msg->enque_ns) {
          inst_delay_total += p_msg->num_frames_to_be_processed * (curr_time - p_msg->enque_ns);
        }
      }
      p_msg->num_frames_to_be_processed =
          num_sbc_frames - num_frames_to_process;
      num_frames_to_process = 0;
      break;
    }
    /* Queue packet has less frames */
    btif_a2dp_sink_handle_inc_media(p_msg);
    p_msg =
        (tBT_SINK_HDR*)fixed_queue_try_dequeue(btif_a2dp_sink_cb.rx_audio_queue);
    if (p_msg == NULL) {
      BTIF_TRACE_ERROR("Insufficient data in queue");
      break;
    }
    num_frames_to_process =
        num_frames_to_process - p_msg->num_frames_to_be_processed;
    osi_free(p_msg);
  } while (num_frames_to_process > 0);

  if (btif_avk_is_sink_delay_report_supported()) {
    inst_delay = inst_delay_total / btif_a2dp_sink_cb.frames_to_process;
    btif_avk_update_reported_delay(inst_delay);
  }

  BTIF_TRACE_DEBUG("Process Frames - ");
}

/* when true media task discards any rx frames */
void btif_a2dp_sink_set_rx_flush(bool enable) {
  BTIF_TRACE_EVENT("## DROP RX %d ##", enable);
  btif_a2dp_sink_cb.rx_flush = enable;
}

static void btif_a2dp_sink_audio_rx_flush_event(void) {
  /* Flush all received SBC buffers (encoded) */
  BTIF_TRACE_DEBUG("%s", __func__);
  fixed_queue_flush(btif_a2dp_sink_cb.rx_audio_queue, osi_free);
}

static void btif_a2dp_sink_decoder_update_event(
    tBTIF_MEDIA_SINK_DECODER_UPDATE* p_buf) {
  OI_STATUS status;

  BTIF_TRACE_DEBUG("%s: p_codec_info[%x:%x:%x:%x:%x:%x]", __func__,
                   p_buf->codec_info[1], p_buf->codec_info[2],
                   p_buf->codec_info[3], p_buf->codec_info[4],
                   p_buf->codec_info[5], p_buf->codec_info[6]);
  {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    if (btif_a2dp_sink_cb.audio_track != NULL) {
      BTIF_TRACE_EVENT("%s, btif_a2dp_sink_cb.audio_track != NULL", __func__);
      return;
    }
  }

  // clear earlier alarm (if any) and media packet queue
  alarm_free(btif_a2dp_sink_cb.decode_alarm);
  BTIF_TRACE_DEBUG("%s: clear decode alarm.", __func__);
  btif_a2dp_sink_cb.decode_alarm = NULL;
  btif_a2dp_sink_audio_rx_flush_event();

  uint8_t codec_type = A2DP_GetCodecType(p_buf->codec_info);
  BTIF_TRACE_ERROR("%s: codec_type:0x%x", __func__, codec_type);

  int sample_rate = A2DP_GetTrackSampleRate(p_buf->codec_info);
  if (sample_rate == -1) {
    BTIF_TRACE_ERROR("%s: cannot get the track frequency", __func__);
    return;
  }
  int channel_count = A2DP_GetTrackChannelCount(p_buf->codec_info);
  if (channel_count == -1) {
    BTIF_TRACE_ERROR("%s: cannot get the channel count", __func__);
    return;
  }
  int channel_type = A2DP_GetSinkTrackChannelType(p_buf->codec_info);
  if (channel_type == -1) {
    BTIF_TRACE_ERROR("%s: cannot get the Sink channel type", __func__);
    return;
  }
  btif_a2dp_sink_cb.codec_type = codec_type;
  btif_a2dp_sink_cb.sample_rate = sample_rate;
  btif_a2dp_sink_cb.channel_count = channel_count;
  btif_a2dp_sink_cb.index = p_buf->index;

  btif_a2dp_sink_cb.rx_flush = false;
  BTIF_TRACE_DEBUG("%s: Reset to Sink role", __func__);
  if (codec_type == A2DP_MEDIA_CT_SBC) {
      status = OI_CODEC_SBC_DecoderReset(
          &btif_a2dp_sink_context, btif_a2dp_sink_context_data,
          sizeof(btif_a2dp_sink_context_data), 2, 2, false);
      if (!OI_SUCCESS(status)) {
        APPL_TRACE_ERROR("%s: OI_CODEC_SBC_DecoderReset failed with error code %d",
                         __func__, status);
      }
  }

  BTIF_TRACE_DEBUG("%s: A2dpSink: create track, Codec:%d ", __func__, codec_type);
  {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    btif_a2dp_sink_cb.audio_track =
#ifndef OS_GENERIC
    BtifAvrcpAudioTrackCreate(sample_rate, channel_type, codec_type);
#else
      NULL;
#endif
  }
  if (btif_a2dp_sink_cb.audio_track == NULL) {
    BTIF_TRACE_ERROR("%s: A2dpSink: Track creation failed at time %d", __func__, btif_a2dp_sink_cb.track_create_retry_cnt);
    if (btif_a2dp_sink_cb.track_create_retry_cnt < TRACK_CREATE_MAX_RETRY_ATTEMPTS) {
      if (btif_a2dp_sink_cb.audio_track_alarm != NULL) {
        /* cancel it first then set the audio track create alarm */
        if (alarm_is_scheduled(btif_a2dp_sink_cb.audio_track_alarm)) {
          alarm_cancel(btif_a2dp_sink_cb.audio_track_alarm);
          BTIF_TRACE_DEBUG("%s: Deleting previously queued timer if any.", __func__);
        }
        alarm_set_on_mloop(btif_a2dp_sink_cb.audio_track_alarm, BTIF_DELAYED_CREATE_AUDIO_TRACK_MS,
             (alarm_callback_t)btif_a2dp_sink_update_decoder,
             (uint8_t*)(p_buf->codec_info));
      }
      btif_a2dp_sink_cb.track_create_retry_cnt++;
    }
    else
      btif_a2dp_sink_cb.track_create_retry_cnt = 0;
    return;
  }
  btif_a2dp_sink_cb.track_create_retry_cnt = 0;

  btif_a2dp_sink_cb.audio_track_status = BTIF_AUDIO_TRACK_STATUS_UPDATED;
  BTIF_TRACE_DEBUG("%s: btif_a2dp_sink_cb.audio_track_status BTIF_AUDIO_TRACK_STATUS_UPDATED.", __func__);
  if (btif_avk_is_sink_delay_report_supported()) {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    btif_a2dp_sink_cb.latency = BtifAvrcpAudioTrackLatency(btif_a2dp_sink_cb.audio_track);
  }

  if (codec_type == A2DP_MEDIA_CT_SBC) {
      btif_a2dp_sink_cb.frames_to_process = A2DP_GetSinkFramesCountToProcess(
          BTIF_SINK_MEDIA_TIME_TICK_MS, p_buf->codec_info);
      BTIF_TRACE_DEBUG("%s: Frames to be processed in 20 ms %d", __func__,
                       btif_a2dp_sink_cb.frames_to_process);
      if (btif_a2dp_sink_cb.frames_to_process == 0) {
        BTIF_TRACE_ERROR("%s: Cannot compute the number of frames to process",
                         __func__);
      }
  }
}

uint32_t get_audiotrack_latency() {
  BTIF_TRACE_DEBUG("%s: latency = %d", __func__,btif_a2dp_sink_cb.latency);
  return btif_a2dp_sink_cb.latency;
}
static void btif_handle_incoming_encoded_data(UNUSED_ATTR void* context) {
    BTIF_TRACE_DEBUG("%s", __func__);
    uint8_t *start_frame_addr;
    tBT_SINK_HDR* p_msg = NULL;

    p_msg = (tBT_SINK_HDR *)fixed_queue_try_dequeue(btif_a2dp_sink_cb.rx_audio_queue);
    // Write encoded media packet to AudioTrack
    if (p_msg != NULL) {
        if (btif_a2dp_sink_cb.audio_track != NULL) {
            start_frame_addr = ((uint8_t*)(p_msg + 1) + p_msg->offset);
            std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
            BtifAvrcpAudioTrackWriteData(
                    btif_a2dp_sink_cb.audio_track, (void*)start_frame_addr, p_msg->len);
        }
        osi_free(p_msg);
    }
}

uint8_t btif_a2dp_sink_enqueue_buf(BT_HDR* p_pkt) {
  BTIF_TRACE_VERBOSE("%s: rx_flush: %d audio_track_status:%d, codec_type:0x%x", __func__,
                     btif_a2dp_sink_cb.rx_flush,
                     btif_a2dp_sink_cb.audio_track_status,
                     btif_a2dp_sink_cb.codec_type);
  if (btif_a2dp_sink_cb.rx_flush) /* Flush enabled, do not enqueue */
    return fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue);

  /* Do not enqueue during updating codec */
  if (btif_a2dp_sink_cb.audio_track_status == BTIF_AUDIO_TRACK_STATUS_UPDATEING) {
    BTIF_TRACE_VERBOSE("%s btif_a2dp_sink_cb.audio_track_status == BTIF_AUDIO_TRACK_STATUS_UPDATEING", __func__);
    return fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue);
  }

  if (fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue) ==
      MAX_INPUT_A2DP_FRAME_QUEUE_SZ) {
    uint8_t ret = fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue);
    osi_free(fixed_queue_try_dequeue(btif_a2dp_sink_cb.rx_audio_queue));
    return ret;
  }

  BTIF_TRACE_VERBOSE("%s +", __func__);
  /* Allocate and queue this buffer */
  tBT_SINK_HDR* p_msg = reinterpret_cast<tBT_SINK_HDR*>(
      osi_malloc(sizeof(tBT_SINK_HDR) + p_pkt->offset + p_pkt->len));
  memcpy((uint8_t*)(p_msg + 1), (uint8_t*)(p_pkt + 1) + p_pkt->offset,
         p_pkt->len);
  p_msg->num_frames_to_be_processed =
      (*((uint8_t*)(p_pkt + 1) + p_pkt->offset)) & 0x0f;
  p_msg->len = p_pkt->len;
  p_msg->offset = 0;
  p_msg->layer_specific = p_pkt->layer_specific;

  if (btif_avk_is_sink_delay_report_supported()) {
    struct timespec ts_now;
    clock_gettime(CLOCK_BOOTTIME, &ts_now);
    p_msg->enque_ns = (uint64_t)ts_now.tv_sec * 1000000000 + ts_now.tv_nsec;
  }

  BTIF_TRACE_VERBOSE("%s: frames to process %d, len %d", __func__,
                     p_msg->num_frames_to_be_processed, p_msg->len);
  fixed_queue_enqueue(btif_a2dp_sink_cb.rx_audio_queue, p_msg);
  if (btif_a2dp_sink_cb.audio_track != NULL &&
      btif_a2dp_sink_cb.codec_type == A2DP_MEDIA_CT_SBC &&
      fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue) ==
          MAX_A2DP_DELAYED_START_FRAME_COUNT) {
    btif_a2dp_sink_audio_handle_start_decoding();
  } else if (btif_a2dp_sink_cb.audio_track != NULL &&
    (btif_a2dp_sink_cb.codec_type == A2DP_MEDIA_CT_AAC ||
    btif_a2dp_sink_cb.codec_type == A2DP_MEDIA_CT_NON_A2DP)) {
    if (fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue) != 0) {
         btif_a2dp_sink_audio_handle_start_non_sbc_decoding();
    }
  }else {
    BTIF_TRACE_ERROR("%s audio_track is NULL!",__func__);
  }

  return fixed_queue_length(btif_a2dp_sink_cb.rx_audio_queue);
}

void btif_a2dp_sink_audio_rx_flush_req(void) {
  if (fixed_queue_is_empty(btif_a2dp_sink_cb.rx_audio_queue)) {
    /* Queue is already empty */
    return;
  }

  BT_HDR* p_buf = reinterpret_cast<BT_HDR*>(osi_malloc(sizeof(BT_HDR)));
  p_buf->event = BTIF_MEDIA_SINK_AUDIO_RX_FLUSH;
  fixed_queue_enqueue(btif_a2dp_sink_cb.cmd_msg_queue, p_buf);
}

void btif_a2dp_sink_debug_dump(UNUSED_ATTR int fd) {
  // Nothing to do
}

void btif_a2dp_sink_set_focus_state_req(btif_a2dp_sink_focus_state_t state) {
  tBTIF_MEDIA_SINK_FOCUS_UPDATE* p_buf =
      reinterpret_cast<tBTIF_MEDIA_SINK_FOCUS_UPDATE*>(
          osi_malloc(sizeof(tBTIF_MEDIA_SINK_FOCUS_UPDATE)));

  BTIF_TRACE_EVENT("%s", __func__);

  p_buf->focus_state = state;
  p_buf->hdr.event = BTIF_MEDIA_SINK_SET_FOCUS_STATE;
  fixed_queue_enqueue(btif_a2dp_sink_cb.cmd_msg_queue, p_buf);
}

static void btif_a2dp_sink_set_focus_state_event(
    btif_a2dp_sink_focus_state_t state) {
  if (!btif_avk_is_connected()) return;
  BTIF_TRACE_DEBUG("%s: setting focus state to %d", __func__, state);
  btif_a2dp_sink_cb.rx_focus_state = state;
  if (btif_a2dp_sink_cb.rx_focus_state == BTIF_A2DP_SINK_FOCUS_NOT_GRANTED) {
    fixed_queue_flush(btif_a2dp_sink_cb.rx_audio_queue, osi_free);
    btif_a2dp_sink_cb.rx_flush = true;
  } else if (btif_a2dp_sink_cb.rx_focus_state == BTIF_A2DP_SINK_FOCUS_GRANTED) {
    btif_a2dp_sink_cb.rx_flush = false;
  }
}

void btif_a2dp_sink_set_audio_track_gain(float gain) {
  BTIF_TRACE_DEBUG("%s set gain to %f", __func__, gain);
#ifndef OS_GENERIC
  if (btif_a2dp_sink_cb.audio_track != NULL) {
    std::lock_guard<std::mutex> lock(btif_a2dp_sink_cb.audio_track_mutex);
    BtifAvrcpSetAudioTrackGain(btif_a2dp_sink_cb.audio_track, gain);
  }
#endif
}

static void btif_a2dp_sink_clear_track_event_req(void) {
  BT_HDR* p_buf = reinterpret_cast<BT_HDR*>(osi_malloc(sizeof(BT_HDR)));

  p_buf->event = BTIF_MEDIA_SINK_CLEAR_TRACK;
  fixed_queue_enqueue(btif_a2dp_sink_cb.cmd_msg_queue, p_buf);
}

/*****************************************************************************
 *
 * Function        btif_a2dp_on_init
 *
 * Description
 *
 * Returns         void
 *
 ******************************************************************************/

void btif_a2dp_sink_on_init(void) {
  BTIF_TRACE_EVENT("## ON A2DP SINK INIT ## %s", __func__);
  btif_a2dp_sink_cb.rx_focus_state = BTIF_A2DP_SINK_FOCUS_GRANTED;
  btif_a2dp_sink_cb.audio_track = NULL;
  btif_a2dp_sink_cb.index = btif_max_avk_clients;
  btif_a2dp_sink_cb.audio_track_status = BTIF_AUDIO_TRACK_STATUS_IDLE;
}

void btif_avk_a2dp_on_suspended(tBTA_AVK_SUSPEND* p_av_suspend) {
  BTIF_TRACE_EVENT("## ON A2DP SINK SUSPENDED ## %s", __func__);
  btif_a2dp_sink_on_suspended((tBTA_AV_SUSPEND *)p_av_suspend);
}
