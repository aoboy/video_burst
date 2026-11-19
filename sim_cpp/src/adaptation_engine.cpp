#include "burst/adaptation_engine.h"

#include <algorithm>

#include "burst/constants.h"

namespace bursts {

void AdaptationEngine::Apply(StreamState& stream, double reduction) {
  if (reduction < 0.1) {
    if (stream.adaptation_level > 0) {
      Restore(stream);
    }
    return;
  }

  // Stage 1: CRF increase (least noticeable). reduction < (1 - 0.80) = 0.20
  if (reduction < (1.0 - kAdaptStageCrf)) {
    stream.adaptation_level = 1;
    stream.crf_offset = std::min(kMaxCrfOffset, reduction * 60.0);  // up to +12 CRF
    stream.resolution_scale = kResolutionScaleFull;
    stream.drop_b_frames = false;
    stream.drop_p_frames = false;
    return;
  }

  // Stage 2: Resolution downscale. reduction < (1 - 0.50) = 0.50
  if (reduction < (1.0 - kAdaptStageResolution)) {
    stream.adaptation_level = 2;
    stream.crf_offset = 6.0;
    if (stream.resolution_scale == kResolutionScaleFull) {
      stream.resolution_scale = kResolutionScaleHalf;  // 1080p -> 540p equivalent
    }
    stream.drop_b_frames = false;
    stream.drop_p_frames = false;
    return;
  }

  // Stage 3: Temporal decimation. reduction < (1 - 0.20) = 0.80
  if (reduction < (1.0 - kAdaptStageTemporal)) {
    stream.adaptation_level = 3;
    stream.crf_offset = 6.0;
    stream.resolution_scale = kResolutionScaleHalf;
    stream.drop_b_frames = true;
    stream.drop_p_frames = false;
    return;
  }

  // Stage 4: Aggressive shedding (preserve I-frames only if needed).
  stream.adaptation_level = 4;
  stream.crf_offset = kMaxCrfOffset;
  stream.resolution_scale = kResolutionScaleQuarter;
  stream.drop_b_frames = true;
  stream.drop_p_frames = true;
}

void AdaptationEngine::Restore(StreamState& stream) {
  // Gradually restore quality when the burst subsides.
  stream.adaptation_level = 0;
  stream.crf_offset = std::max(0.0, stream.crf_offset - 0.5);
  if (stream.crf_offset < 0.5) {
    stream.crf_offset = 0.0;
  }
  if (stream.crf_offset == 0.0) {
    stream.resolution_scale = kResolutionScaleFull;
    stream.drop_b_frames = false;
    stream.drop_p_frames = false;
  }
}

}  // namespace bursts