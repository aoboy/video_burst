#pragma once

// Constants mirroring the Python reference simulator (cam_stream_sim.py)
// and the numbered parameters in docs/BURST_RESILIENT_CAMERA_SYSTEM.md.

namespace bursts {

// Water marks (fraction of per-stream ring buffer capacity).
inline constexpr double kWatermarkLow = 0.20;
inline constexpr double kWatermarkHigh = 0.60;
inline constexpr double kWatermarkCritical = 0.85;
inline constexpr double kWatermarkOverflow = 0.95;

// Adaptation stage thresholds: a stream enters the next stage when the PI
// controller demands more than (1 - stage) bitrate reduction.
inline constexpr double kAdaptStageCrf = 0.80;         // reduction < 0.20 -> CRF only
inline constexpr double kAdaptStageResolution = 0.50;  // reduction < 0.50 -> resolution downscale
inline constexpr double kAdaptStageTemporal = 0.20;    // reduction < 0.80 -> temporal decimation
inline constexpr double kAdaptStageShed = 0.10;        // reduction >= 0.80 -> shed non-client data

// Traffic model (Part III / simulator).
inline constexpr double kGopPeriodSeconds = 2.0;
inline constexpr double kGopVariationAmplitude = 0.3;
inline constexpr double kNoiseFraction = 0.1;
inline constexpr double kMinBitrateBps = 100'000.0;    // 100 kbps floor

// Codec adaptation parameters (Part II, section 3.3).
inline constexpr double kMaxCrfOffset = 12.0;
inline constexpr int kResolutionScaleFull = 1;
inline constexpr int kResolutionScaleHalf = 2;
inline constexpr int kResolutionScaleQuarter = 4;

}  // namespace bursts