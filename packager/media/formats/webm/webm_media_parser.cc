// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "packager/media/formats/webm/webm_media_parser.h"

#include <string>

#include "packager/base/callback.h"
#include "packager/base/callback_helpers.h"
#include "packager/base/logging.h"
#include "packager/media/base/timestamp.h"
#include "packager/media/formats/webm/webm_cluster_parser.h"
#include "packager/media/formats/webm/webm_constants.h"
#include "packager/media/formats/webm/webm_content_encodings.h"
#include "packager/media/formats/webm/webm_info_parser.h"
#include "packager/media/formats/webm/webm_tracks_parser.h"

namespace edash_packager {
namespace media {

WebMMediaParser::WebMMediaParser()
    : state_(kWaitingForInit), unknown_segment_size_(false) {}

WebMMediaParser::~WebMMediaParser() {}

void WebMMediaParser::Init(const InitCB& init_cb,
                           const NewSampleCB& new_sample_cb,
                           KeySource* decryption_key_source) {
  DCHECK_EQ(state_, kWaitingForInit);
  DCHECK(init_cb_.is_null());
  DCHECK(!init_cb.is_null());
  DCHECK(!new_sample_cb.is_null());

  ChangeState(kParsingHeaders);
  init_cb_ = init_cb;
  new_sample_cb_ = new_sample_cb;
  decryption_key_source_ = decryption_key_source;
  ignore_text_tracks_ = true;
}

void WebMMediaParser::Flush() {
  DCHECK_NE(state_, kWaitingForInit);

  byte_queue_.Reset();
  if (cluster_parser_)
    cluster_parser_->Flush();
  if (state_ == kParsingClusters) {
    ChangeState(kParsingHeaders);
  }
}

bool WebMMediaParser::Parse(const uint8_t* buf, int size) {
  DCHECK_NE(state_, kWaitingForInit);

  if (state_ == kError)
    return false;

  byte_queue_.Push(buf, size);

  int result = 0;
  int bytes_parsed = 0;
  const uint8_t* cur = NULL;
  int cur_size = 0;

  byte_queue_.Peek(&cur, &cur_size);
  while (cur_size > 0) {
    State oldState = state_;
    switch (state_) {
      case kParsingHeaders:
        result = ParseInfoAndTracks(cur, cur_size);
        break;

      case kParsingClusters:
        result = ParseCluster(cur, cur_size);
        break;

      case kWaitingForInit:
      case kError:
        return false;
    }

    if (result < 0) {
      ChangeState(kError);
      return false;
    }

    if (state_ == oldState && result == 0)
      break;

    DCHECK_GE(result, 0);
    cur += result;
    cur_size -= result;
    bytes_parsed += result;
  }

  byte_queue_.Pop(bytes_parsed);
  return true;
}

void WebMMediaParser::ChangeState(State new_state) {
  DVLOG(1) << "ChangeState() : " << state_ << " -> " << new_state;
  state_ = new_state;
}

int WebMMediaParser::ParseInfoAndTracks(const uint8_t* data, int size) {
  DVLOG(2) << "ParseInfoAndTracks()";
  DCHECK(data);
  DCHECK_GT(size, 0);

  const uint8_t* cur = data;
  int cur_size = size;
  int bytes_parsed = 0;

  int id;
  int64_t element_size;
  int result = WebMParseElementHeader(cur, cur_size, &id, &element_size);

  if (result <= 0)
    return result;

  switch (id) {
    case kWebMIdEBMLHeader:
    case kWebMIdSeekHead:
    case kWebMIdVoid:
    case kWebMIdCRC32:
    case kWebMIdCues:
    case kWebMIdChapters:
    case kWebMIdTags:
    case kWebMIdAttachments:
      // TODO: Implement support for chapters.
      if (cur_size < (result + element_size)) {
        // We don't have the whole element yet. Signal we need more data.
        return 0;
      }
      // Skip the element.
      return result + element_size;
      break;
    case kWebMIdCluster:
      if (!cluster_parser_) {
        LOG(ERROR) << "Found Cluster element before Info.";
        return -1;
      }
      ChangeState(kParsingClusters);
      return 0;
      break;
    case kWebMIdSegment:
      // Segment of unknown size indicates live stream.
      if (element_size == kWebMUnknownSize)
        unknown_segment_size_ = true;
      // Just consume the segment header.
      return result;
      break;
    case kWebMIdInfo:
      // We've found the element we are looking for.
      break;
    default: {
      LOG(ERROR) << "Unexpected element ID 0x" << std::hex << id;
      return -1;
    }
  }

  WebMInfoParser info_parser;
  result = info_parser.Parse(cur, cur_size);

  if (result <= 0)
    return result;

  cur += result;
  cur_size -= result;
  bytes_parsed += result;

  WebMTracksParser tracks_parser(ignore_text_tracks_);
  result = tracks_parser.Parse(cur, cur_size);

  if (result <= 0)
    return result;

  bytes_parsed += result;

  double timecode_scale_in_us = info_parser.timecode_scale() / 1000.0;
  int64_t duration_in_us = info_parser.duration() * timecode_scale_in_us;

  std::vector<scoped_refptr<StreamInfo>> streams;
  AudioCodec audio_codec = kCodecOpus;
  if (tracks_parser.audio_stream_info()) {
    streams.push_back(tracks_parser.audio_stream_info());
    streams.back()->set_duration(duration_in_us);
    if (streams.back()->is_encrypted())
      OnEncryptedMediaInitData(tracks_parser.audio_encryption_key_id());
    audio_codec = tracks_parser.audio_stream_info()->codec();
  } else {
    VLOG(1) << "No audio track info found.";
  }

  if (tracks_parser.video_stream_info()) {
    streams.push_back(tracks_parser.video_stream_info());
    streams.back()->set_duration(duration_in_us);
    if (streams.back()->is_encrypted())
      OnEncryptedMediaInitData(tracks_parser.video_encryption_key_id());
  } else {
    VLOG(1) << "No video track info found.";
  }

  init_cb_.Run(streams);

  cluster_parser_.reset(new WebMClusterParser(
      info_parser.timecode_scale(), tracks_parser.audio_track_num(),
      tracks_parser.GetAudioDefaultDuration(timecode_scale_in_us),
      tracks_parser.video_track_num(),
      tracks_parser.GetVideoDefaultDuration(timecode_scale_in_us),
      tracks_parser.text_tracks(), tracks_parser.ignored_tracks(),
      tracks_parser.audio_encryption_key_id(),
      tracks_parser.video_encryption_key_id(), audio_codec, new_sample_cb_));

  return bytes_parsed;
}

int WebMMediaParser::ParseCluster(const uint8_t* data, int size) {
  if (!cluster_parser_)
    return -1;

  int bytes_parsed = cluster_parser_->Parse(data, size);
  if (bytes_parsed < 0)
    return bytes_parsed;

  bool cluster_ended = cluster_parser_->cluster_ended();
  if (cluster_ended) {
    ChangeState(kParsingHeaders);
  }

  return bytes_parsed;
}

void WebMMediaParser::OnEncryptedMediaInitData(const std::string& key_id) {
  NOTIMPLEMENTED() << "WebM decryption is not implemented yet.";
}

}  // namespace media
}  // namespace edash_packager