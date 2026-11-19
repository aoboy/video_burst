#!/usr/bin/env python3
"""
cam_stream_sim.py — Discrete-Event Simulation of Burst-Resilient Camera Stream Processing

Simulates N camera streams (H.265) with bursty traffic, hierarchical buffering,
and PI-controller-based dynamic adaptation. Validates the architecture and
modeling described in the companion documentation.

Usage:
    python cam_stream_sim.py --cameras 5000 --burst-cameras 200 --duration 300 --plot
    python cam_stream_sim.py --cameras 1000 --burst-cameras 50 --nic 40 --duration 300 --plot

Recovered from description_output.txt (transcript) and updated:
  - Output paths are portable (local ./output, auto-created) instead of the
    sandbox-only /mnt/agents/output/.
  - The --burst-recovery CLI flag is now wired to the existing
    SimulationConfig.burst_recovery_rate field (previously unreachable).
"""

import argparse
import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple
from collections import deque
import random
import sys

# =============================================================================
# CONFIGURATION DEFAULTS
# =============================================================================

DEFAULT_BASELINE_BITRATE = 3_000_000       # 3 Mbps per stream (H.265 1080p30)
DEFAULT_BURST_FACTOR_MIN = 3.0
DEFAULT_BURST_FACTOR_MAX = 10.0
DEFAULT_BURST_ARRIVAL_RATE = 0.001         # per stream per second
DEFAULT_BURST_RECOVERY_RATE = 0.1            # per second
DEFAULT_PER_STREAM_BUFFER_S = 2.0          # seconds of buffer at baseline
DEFAULT_EMERGENCY_POOL_GB = 64.0
DEFAULT_NIC_BANDWIDTH_GBPS = 25.0
DEFAULT_PI_KP = 0.5
DEFAULT_PI_KI = 0.1
DEFAULT_PI_HZ = 100.0
DEFAULT_CONTROLLER_SETPOINT = 0.4          # 40% buffer occupancy target

# All generated artifacts (plots, metrics) go here; auto-created at runtime.
OUTPUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "output")

# Water marks (fraction of per-stream buffer capacity)
WM_LOW = 0.20
WM_HIGH = 0.60
WM_CRITICAL = 0.85
WM_OVERFLOW = 0.95

# Adaptation stages (bitrate reduction thresholds)
ADAPT_STAGE_CRF = 0.80          # Reduce CRF first (up to 20% bitrate cut)
ADAPT_STAGE_RESOLUTION = 0.50   # Downscale resolution (halving ~50% cut)
ADAPT_STAGE_TEMPORAL = 0.20     # Drop B-frames, then P-frames
ADAPT_STAGE_SHED = 0.10         # Shed non-critical data


# =============================================================================
# DATA CLASSES
# =============================================================================

@dataclass
class StreamState:
    """State of a single camera stream."""
    stream_id: int
    
    # Bitrate dynamics
    baseline_bitrate: float = DEFAULT_BASELINE_BITRATE
    current_bitrate: float = DEFAULT_BASELINE_BITRATE
    burst_factor: float = 1.0
    
    # Burst model (Markov)
    is_bursting: bool = False
    burst_start_time: Optional[float] = None
    burst_duration: float = 0.0
    
    # Buffer state (bits)
    buffer_capacity: float = 0.0
    buffer_occupancy: float = 0.0
    buffer_write_idx: int = 0
    buffer_read_idx: int = 0
    
    # Emergency pool
    emergency_allocated: bool = False
    emergency_occupancy: float = 0.0
    
    # PI Controller
    integral_error: float = 0.0
    last_error: float = 0.0
    
    # Adaptation state
    adaptation_level: int = 0       # 0=none, 1=CRF, 2=resolution, 3=temporal, 4=shed
    crf_offset: float = 0.0
    resolution_scale: int = 1       # 1=full, 2=half, 4=quarter
    drop_b_frames: bool = False
    drop_p_frames: bool = False
    
    # Statistics
    frames_received: int = 0
    frames_dropped: int = 0
    frames_served: int = 0
    bytes_received: int = 0
    bytes_served: int = 0
    total_dropped_bits: int = 0
    continuity_gaps: int = 0
    last_served_time: float = 0.0
    
    # EWMA for burst detection
    ewma_slow: float = 0.0
    ewma_fast: float = 0.0
    
    # History for plotting
    occupancy_history: List[float] = field(default_factory=list)
    bitrate_history: List[float] = field(default_factory=list)
    adaptation_history: List[int] = field(default_factory=list)


@dataclass
class SimulationConfig:
    """Simulation configuration."""
    n_cameras: int = 5000
    max_burst_cameras: int = 200
    duration_seconds: float = 300.0
    dt: float = 0.01                       # 10ms time step (100 Hz)
    
    baseline_bitrate: float = DEFAULT_BASELINE_BITRATE
    burst_factor_min: float = DEFAULT_BURST_FACTOR_MIN
    burst_factor_max: float = DEFAULT_BURST_FACTOR_MAX
    burst_arrival_rate: float = DEFAULT_BURST_ARRIVAL_RATE
    burst_recovery_rate: float = DEFAULT_BURST_RECOVERY_RATE
    
    per_stream_buffer_s: float = DEFAULT_PER_STREAM_BUFFER_S
    emergency_pool_gb: float = DEFAULT_EMERGENCY_POOL_GB
    nic_bandwidth_gbps: float = DEFAULT_NIC_BANDWIDTH_GBPS
    
    pi_kp: float = DEFAULT_PI_KP
    pi_ki: float = DEFAULT_PI_KI
    pi_hz: float = DEFAULT_PI_HZ
    controller_setpoint: float = DEFAULT_CONTROLLER_SETPOINT
    
    seed: int = 42
    plot: bool = False
    verbose: bool = False


# =============================================================================
# BURST DETECTOR
# =============================================================================

class BurstDetector:
    """
    Detects burst onset/offset using dual EWMA (slow and fast).
    """
    def __init__(self, alpha_slow: float = 0.01, alpha_fast: float = 0.2,
                 burst_threshold: float = 2.0, recovery_threshold: float = 1.3):
        self.alpha_slow = alpha_slow
        self.alpha_fast = alpha_fast
        self.burst_threshold = burst_threshold
        self.recovery_threshold = recovery_threshold
    
    def update(self, stream: StreamState, instantaneous_bitrate: float, dt: float) -> Tuple[bool, bool]:
        """
        Update EWMAs and return (burst_detected, recovery_detected).
        """
        # Initialize on first call
        if stream.ewma_slow == 0:
            stream.ewma_slow = instantaneous_bitrate
            stream.ewma_fast = instantaneous_bitrate
            return False, False
        
        # Update EWMAs
        stream.ewma_slow = (1 - self.alpha_slow) * stream.ewma_slow + self.alpha_slow * instantaneous_bitrate
        stream.ewma_fast = (1 - self.alpha_fast) * stream.ewma_fast + self.alpha_fast * instantaneous_bitrate
        
        ratio = stream.ewma_fast / stream.ewma_slow if stream.ewma_slow > 0 else 1.0
        
        burst_detected = False
        recovery_detected = False
        
        if ratio > self.burst_threshold and not stream.is_bursting:
            burst_detected = True
        elif ratio < self.recovery_threshold and stream.is_bursting:
            recovery_detected = True
        
        return burst_detected, recovery_detected


# =============================================================================
# PI CONTROLLER
# =============================================================================

class PIController:
    """
    Proportional-Integral controller for bitrate adaptation.
    """
    def __init__(self, kp: float, ki: float, setpoint: float, 
                 max_integral: float = None, dt: float = 0.01):
        self.kp = kp
        self.ki = ki
        self.setpoint = setpoint
        self.dt = dt
        self.max_integral = max_integral if max_integral is not None else 0.3
    
    def step(self, stream: StreamState) -> float:
        """
        Compute desired bitrate reduction factor (0 = no reduction, 1 = full reduction).
        """
        occupancy_ratio = stream.buffer_occupancy / stream.buffer_capacity if stream.buffer_capacity > 0 else 0
        error = occupancy_ratio - self.setpoint
        
        # Proportional term
        p_term = self.kp * error
        
        # Integral term with anti-windup
        stream.integral_error += self.ki * error * self.dt
        stream.integral_error = max(-self.max_integral, min(self.max_integral, stream.integral_error))
        
        # Control output: desired reduction in bitrate (as fraction of current)
        reduction = p_term + stream.integral_error
        reduction = max(0.0, min(1.0, reduction))
        
        return reduction


# =============================================================================
# ADAPTATION ENGINE
# =============================================================================

class AdaptationEngine:
    """
    Maps controller output to concrete codec parameters.
    """
    def __init__(self):
        pass
    
    def apply(self, stream: StreamState, reduction: float) -> None:
        """
        Apply adaptation based on reduction demand (0.0 to 1.0).
        """
        if reduction < 0.1:
            # No adaptation needed
            if stream.adaptation_level > 0:
                self._restore(stream)
            return
        
        # Stage 1: CRF increase (least noticeable)
        if reduction < (1 - ADAPT_STAGE_CRF):
            stream.adaptation_level = 1
            stream.crf_offset = min(12, reduction * 60)  # Up to +12 CRF
            stream.resolution_scale = 1
            stream.drop_b_frames = False
            stream.drop_p_frames = False
        
        # Stage 2: Resolution downscale
        elif reduction < (1 - ADAPT_STAGE_RESOLUTION):
            stream.adaptation_level = 2
            stream.crf_offset = 6
            if stream.resolution_scale == 1:
                stream.resolution_scale = 2  # 1080p -> 540p equivalent
            stream.drop_b_frames = False
            stream.drop_p_frames = False
        
        # Stage 3: Temporal decimation
        elif reduction < (1 - ADAPT_STAGE_TEMPORAL):
            stream.adaptation_level = 3
            stream.crf_offset = 6
            stream.resolution_scale = 2
            stream.drop_b_frames = True
            stream.drop_p_frames = False
        
        # Stage 4: Aggressive shedding (preserve I-frames only if needed)
        else:
            stream.adaptation_level = 4
            stream.crf_offset = 12
            stream.resolution_scale = 4
            stream.drop_b_frames = True
            stream.drop_p_frames = True
    
    def _restore(self, stream: StreamState) -> None:
        """Gradually restore quality when burst subsides."""
        stream.adaptation_level = 0
        stream.crf_offset = max(0, stream.crf_offset - 0.5)
        if stream.crf_offset < 0.5:
            stream.crf_offset = 0
        if stream.crf_offset == 0:
            stream.resolution_scale = 1
            stream.drop_b_frames = False
            stream.drop_p_frames = False


# =============================================================================
# MAIN SIMULATOR
# =============================================================================

class CameraStreamSimulator:
    def __init__(self, config: SimulationConfig):
        self.cfg = config
        self.rng = np.random.RandomState(config.seed)
        
        # System capacity
        self.nic_bandwidth = config.nic_bandwidth_gbps * 1e9  # bits/sec
        self.per_stream_buffer = config.baseline_bitrate * config.per_stream_buffer_s
        self.emergency_pool_total = config.emergency_pool_gb * 8e9  # bits
        self.emergency_pool_used = 0.0
        
        # Initialize streams
        self.streams: List[StreamState] = []
        for i in range(config.n_cameras):
            s = StreamState(
                stream_id=i,
                baseline_bitrate=config.baseline_bitrate,
                current_bitrate=config.baseline_bitrate,
                buffer_capacity=self.per_stream_buffer,
                buffer_occupancy=self.per_stream_buffer * 0.2,  # Start at 20%
            )
            s.ewma_slow = config.baseline_bitrate
            s.ewma_fast = config.baseline_bitrate
            self.streams.append(s)
        
        # Subsystems
        self.burst_detector = BurstDetector()
        self.pi_controller = PIController(
            kp=config.pi_kp,
            ki=config.pi_ki,
            setpoint=config.controller_setpoint,
            dt=config.dt
        )
        self.adaptation_engine = AdaptationEngine()
        
        # Simulation state
        self.current_time = 0.0
        self.total_bits_received = 0.0
        self.total_bits_dropped = 0.0
        self.total_bits_served = 0.0
        
        # Global history
        self.time_history: List[float] = []
        self.total_ingress_history: List[float] = []
        self.total_egress_history: List[float] = []
        self.total_buffer_history: List[float] = []
        self.emergency_pool_history: List[float] = []
        self.burst_count_history: List[int] = []
        self.adaptation_count_history: List[int] = []
        self.dropped_frames_history: List[int] = []
        self.continuity_history: List[float] = []
    
    def _generate_burst_duration(self) -> float:
        """Pareto-distributed burst duration."""
        # Pareto with shape=2, scale=2 seconds
        return 2.0 + self.rng.pareto(2.0) * 2.0
    
    def _generate_burst_factor(self) -> float:
        """Uniform burst factor between min and max."""
        return self.rng.uniform(self.cfg.burst_factor_min, self.cfg.burst_factor_max)
    
    def _update_burst_states(self) -> None:
        """Markov transition for burst states."""
        for s in self.streams:
            if not s.is_bursting:
                # Probability of entering burst in this time step
                p_burst = 1 - np.exp(-self.cfg.burst_arrival_rate * self.cfg.dt)
                if self.rng.random() < p_burst:
                    s.is_bursting = True
                    s.burst_start_time = self.current_time
                    s.burst_duration = self._generate_burst_duration()
                    s.burst_factor = self._generate_burst_factor()
            else:
                # Check if burst should end
                if self.current_time - s.burst_start_time >= s.burst_duration:
                    s.is_bursting = False
                    s.burst_factor = 1.0
                    s.burst_start_time = None
    
    def _compute_instantaneous_bitrate(self, stream: StreamState) -> float:
        """Compute instantaneous bitrate with GOP variation and noise."""
        base = stream.baseline_bitrate * stream.burst_factor
        
        # GOP variation: I-frames are larger (simulate with sine wave)
        gop_period = 2.0  # 2-second GOP
        gop_variation = 0.3 * base * np.sin(2 * np.pi * self.current_time / gop_period)
        
        # Gaussian noise
        noise = self.rng.normal(0, 0.1 * base)
        
        bitrate = base + gop_variation + noise
        return max(bitrate, 100_000)  # Minimum 100 kbps
    
    def _apply_adaptation_to_bitrate(self, stream: StreamState, raw_bitrate: float) -> float:
        """Apply adaptation parameters to effective bitrate."""
        effective = raw_bitrate
        
        # CRF adjustment: each +6 CRF ~ 20% bitrate reduction
        if stream.crf_offset > 0:
            reduction_factor = 1.0 - (stream.crf_offset / 6.0) * 0.2
            effective *= max(0.3, reduction_factor)
        
        # Resolution downscale: halving resolution ~ 50% bitrate reduction
        if stream.resolution_scale == 2:
            effective *= 0.5
        elif stream.resolution_scale == 4:
            effective *= 0.25
        
        # Temporal decimation: dropping B-frames saves ~20%, P-frames saves ~40%
        if stream.drop_b_frames and not stream.drop_p_frames:
            effective *= 0.8
        elif stream.drop_p_frames:
            effective *= 0.4
        
        return effective
    
    def _ingest(self, stream: StreamState, bits: float) -> float:
        """
        Ingest bits into stream buffer. Returns actually accepted bits.
        """
        stream.frames_received += 1
        stream.bytes_received += int(bits / 8)
        
        # Try per-stream buffer first
        available = stream.buffer_capacity - stream.buffer_occupancy
        
        if bits <= available:
            stream.buffer_occupancy += bits
            return bits
        
        # Fill what we can in per-stream buffer
        accepted = available
        stream.buffer_occupancy = stream.buffer_capacity
        remaining = bits - accepted
        
        # Try emergency pool
        if remaining > 0 and not stream.emergency_allocated:
            if self.emergency_pool_used + remaining <= self.emergency_pool_total:
                stream.emergency_allocated = True
                stream.emergency_occupancy = remaining
                self.emergency_pool_used += remaining
                accepted += remaining
                remaining = 0
            elif self.emergency_pool_used < self.emergency_pool_total:
                partial = self.emergency_pool_total - self.emergency_pool_used
                stream.emergency_allocated = True
                stream.emergency_occupancy = partial
                self.emergency_pool_used += partial
                accepted += partial
                remaining -= partial
        elif remaining > 0 and stream.emergency_allocated:
            emergency_available = self.emergency_pool_total - self.emergency_pool_used + stream.emergency_occupancy
            can_take = min(remaining, emergency_available)
            stream.emergency_occupancy += can_take
            self.emergency_pool_used += can_take
            accepted += can_take
            remaining -= can_take
        
        # Drop remaining
        if remaining > 0:
            stream.frames_dropped += 1
            stream.total_dropped_bits += int(remaining)
            self.total_bits_dropped += remaining
        
        return accepted
    
    def _drain_buffers(self, stream: StreamState, drain_rate: float) -> None:
        """Drain buffer at sustainable rate."""
        # Drain per-stream buffer first
        drain_from_main = min(stream.buffer_occupancy, drain_rate)
        stream.buffer_occupancy -= drain_from_main
        stream.bytes_served += int(drain_from_main / 8)
        stream.frames_served += 1
        
        remaining_drain = drain_rate - drain_from_main
        
        # Drain emergency pool
        if remaining_drain > 0 and stream.emergency_allocated:
            drain_from_emergency = min(stream.emergency_occupancy, remaining_drain)
            stream.emergency_occupancy -= drain_from_emergency
            self.emergency_pool_used -= drain_from_emergency
            stream.bytes_served += int(drain_from_emergency / 8)
            
            if stream.emergency_occupancy <= 0:
                stream.emergency_allocated = False
                stream.emergency_occupancy = 0
        
        stream.last_served_time = self.current_time
    
    def _run_controller(self, stream: StreamState) -> None:
        """Run PI controller and adaptation engine."""
        reduction = self.pi_controller.step(stream)
        self.adaptation_engine.apply(stream, reduction)
    
    def step(self) -> None:
        """Execute one simulation time step."""
        self._update_burst_states()
        
        total_ingress = 0.0
        total_egress = 0.0
        total_buffer = 0.0
        burst_count = 0
        adaptation_count = 0
        dropped_frames = 0
        
        # Phase 1: Ingress and burst detection
        for s in self.streams:
            raw_bitrate = self._compute_instantaneous_bitrate(s)
            
            # Burst detection
            burst_detected, recovery_detected = self.burst_detector.update(
                s, raw_bitrate, self.cfg.dt
            )
            
            if burst_detected:
                pass  # Already handled in _update_burst_states
            if recovery_detected:
                pass  # Already handled in _update_burst_states
            
            # Apply adaptation to effective bitrate
            effective_bitrate = self._apply_adaptation_to_bitrate(s, raw_bitrate)
            s.current_bitrate = effective_bitrate
            
            # Ingest bits for this time step
            bits_this_step = effective_bitrate * self.cfg.dt
            accepted = self._ingest(s, bits_this_step)
            
            total_ingress += bits_this_step
            total_buffer += s.buffer_occupancy + s.emergency_occupancy
            
            if s.is_bursting:
                burst_count += 1
            if s.adaptation_level > 0:
                adaptation_count += 1
            dropped_frames += s.frames_dropped
        
        # Phase 2: Controller (runs at PI_HZ)
        controller_interval = 1.0 / self.cfg.pi_hz
        if int(self.current_time / self.cfg.dt) % int(controller_interval / self.cfg.dt) == 0:
            for s in self.streams:
                self._run_controller(s)
        
        # Phase 3: Egress (drain buffers at sustainable rate)
        # Each stream gets its fair share of the sustainable bandwidth
        sustainable_per_stream = self.nic_bandwidth / self.cfg.n_cameras
        
        for s in self.streams:
            # During burst, we can drain more aggressively from buffer
            # but capped by what client can consume
            drain_rate = sustainable_per_stream
            
            # If buffer is filling, increase drain rate temporarily
            occupancy_ratio = s.buffer_occupancy / s.buffer_capacity if s.buffer_capacity > 0 else 0
            if occupancy_ratio > WM_HIGH:
                drain_rate *= 1.5  # Drain faster to catch up
            
            self._drain_buffers(s, drain_rate * self.cfg.dt)
            total_egress += drain_rate * self.cfg.dt
        
        # Record history
        self.time_history.append(self.current_time)
        self.total_ingress_history.append(total_ingress)
        self.total_egress_history.append(total_egress)
        self.total_buffer_history.append(total_buffer)
        self.emergency_pool_history.append(self.emergency_pool_used)
        self.burst_count_history.append(burst_count)
        self.adaptation_count_history.append(adaptation_count)
        self.dropped_frames_history.append(dropped_frames)
        
        # Compute continuity ratio
        continuous_streams = sum(
            1 for s in self.streams 
            if self.current_time - s.last_served_time < 2 * self.cfg.dt
        )
        self.continuity_history.append(continuous_streams / self.cfg.n_cameras)
        
        # Record per-stream history (sample every 100ms to save memory)
        if int(self.current_time * 10) % 10 == 0:
            for s in self.streams:
                occ_ratio = s.buffer_occupancy / s.buffer_capacity if s.buffer_capacity > 0 else 0
                s.occupancy_history.append(occ_ratio)
                s.bitrate_history.append(s.current_bitrate / 1e6)  # Mbps
                s.adaptation_history.append(s.adaptation_level)
        
        self.current_time += self.cfg.dt
    
    def run(self) -> Dict:
        """Run the full simulation."""
        n_steps = int(self.cfg.duration_seconds / self.cfg.dt)
        
        print(f"Starting simulation: N={self.cfg.n_cameras}, "
              f"Duration={self.cfg.duration_seconds}s, dt={self.cfg.dt}s")
        print(f"NIC Bandwidth: {self.cfg.nic_bandwidth_gbps} Gbps")
        print(f"Per-stream buffer: {self.per_stream_buffer/1e6:.2f} Mb")
        print(f"Emergency pool: {self.cfg.emergency_pool_gb} GB")
        print()
        
        for step in range(n_steps):
            self.step()
            
            if self.cfg.verbose and step % 1000 == 0:
                t = self.current_time
                ingress_gbps = self.total_ingress_history[-1] / self.cfg.dt / 1e9
                egress_gbps = self.total_egress_history[-1] / self.cfg.dt / 1e9
                bursts = self.burst_count_history[-1]
                cont = self.continuity_history[-1]
                print(f"[{t:6.2f}s] Ingress: {ingress_gbps:5.2f} Gbps | "
                      f"Egress: {egress_gbps:5.2f} Gbps | Bursts: {bursts:4d} | "
                      f"Continuity: {cont:.4f}")
        
        return self._compute_metrics()
    
    def _compute_metrics(self) -> Dict:
        """Compute final simulation metrics."""
        metrics = {
            'duration_seconds': self.cfg.duration_seconds,
            'n_cameras': self.cfg.n_cameras,
            'nic_bandwidth_gbps': self.cfg.nic_bandwidth_gbps,
            
            # Traffic metrics
            'mean_ingress_gbps': np.mean(self.total_ingress_history) / self.cfg.dt / 1e9,
            'max_ingress_gbps': np.max(self.total_ingress_history) / self.cfg.dt / 1e9,
            'mean_egress_gbps': np.mean(self.total_egress_history) / self.cfg.dt / 1e9,
            
            # Burst metrics
            'max_concurrent_bursts': np.max(self.burst_count_history),
            'mean_concurrent_bursts': np.mean(self.burst_count_history),
            
            # Buffer metrics
            'max_total_buffer_gb': np.max(self.total_buffer_history) / 8e9,
            'mean_total_buffer_gb': np.mean(self.total_buffer_history) / 8e9,
            'max_emergency_pool_gb': np.max(self.emergency_pool_history) / 8e9,
            'mean_emergency_pool_gb': np.mean(self.emergency_pool_history) / 8e9,
            
            # Quality metrics
            'mean_continuity_ratio': np.mean(self.continuity_history),
            'min_continuity_ratio': np.min(self.continuity_history),
            'mean_adaptation_level': np.mean(self.adaptation_count_history) / self.cfg.n_cameras,
            'max_adaptation_level': np.max(self.adaptation_count_history) / self.cfg.n_cameras,
            
            # Drop metrics
            'total_dropped_bits': self.total_bits_dropped,
            'total_dropped_gb': self.total_bits_dropped / 8e9,
            'drop_ratio': self.total_bits_dropped / sum(self.total_ingress_history) if sum(self.total_ingress_history) > 0 else 0,
        }
        
        # Per-stream metrics
        metrics['streams_with_drops'] = sum(1 for s in self.streams if s.frames_dropped > 0)
        metrics['max_frames_dropped_per_stream'] = max(s.frames_dropped for s in self.streams)
        metrics['mean_frames_dropped_per_stream'] = np.mean([s.frames_dropped for s in self.streams])
        
        return metrics
    
    def print_metrics(self, metrics: Dict) -> None:
        """Print simulation results."""
        print()
        print("=" * 70)
        print("SIMULATION RESULTS")
        print("=" * 70)
        
        print()
        print("--- Traffic ---")
        print(f"  Mean Ingress:     {metrics['mean_ingress_gbps']:.2f} Gbps")
        print(f"  Max Ingress:      {metrics['max_ingress_gbps']:.2f} Gbps")
        print(f"  NIC Capacity:     {metrics['nic_bandwidth_gbps']:.2f} Gbps")
        print(f"  Mean Egress:      {metrics['mean_egress_gbps']:.2f} Gbps")
        
        print()
        print("--- Bursts ---")
        print(f"  Max Concurrent:   {metrics['max_concurrent_bursts']}")
        print(f"  Mean Concurrent:  {metrics['mean_concurrent_bursts']:.2f}")
        
        print()
        print("--- Buffers ---")
        print(f"  Max Total Buffer: {metrics['max_total_buffer_gb']:.2f} GB")
        print(f"  Mean Total Buffer:{metrics['mean_total_buffer_gb']:.2f} GB")
        print(f"  Max Emergency:    {metrics['max_emergency_pool_gb']:.2f} GB")
        
        print()
        print("--- Quality of Service ---")
        print(f"  Mean Continuity:  {metrics['mean_continuity_ratio']*100:.4f}%")
        print(f"  Min Continuity:   {metrics['min_continuity_ratio']*100:.4f}%")
        print(f"  Mean Adaptation:  {metrics['mean_adaptation_level']*100:.2f}% of streams")
        
        print()
        print("--- Drops ---")
        print(f"  Total Dropped:    {metrics['total_dropped_gb']:.4f} GB")
        print(f"  Drop Ratio:       {metrics['drop_ratio']*100:.4f}%")
        print(f"  Streams Affected: {metrics['streams_with_drops']} / {self.cfg.n_cameras}")
        
        print()
        print("=" * 70)
    
    def plot(self, output_prefix: str = "cam_stream_sim") -> None:
        """Generate visualization plots."""
        fig, axes = plt.subplots(3, 2, figsize=(16, 14))
        fig.suptitle(f"Camera Stream Burst Simulation (N={self.cfg.n_cameras}, "
                     f"NIC={self.cfg.nic_bandwidth_gbps} Gbps)", fontsize=14, fontweight='bold')
        
        time_arr = np.array(self.time_history)
        
        # Plot 1: Ingress vs Egress vs Capacity
        ax = axes[0, 0]
        ax.plot(time_arr, np.array(self.total_ingress_history) / self.cfg.dt / 1e9, 
                label='Ingress', color='red', alpha=0.8, linewidth=1)
        ax.plot(time_arr, np.array(self.total_egress_history) / self.cfg.dt / 1e9, 
                label='Sustainable Egress', color='green', alpha=0.8, linewidth=1)
        ax.axhline(y=self.cfg.nic_bandwidth_gbps, color='blue', linestyle='--', 
                   label=f'NIC Capacity ({self.cfg.nic_bandwidth_gbps} Gbps)')
        ax.fill_between(time_arr, 0, np.array(self.total_ingress_history) / self.cfg.dt / 1e9, 
                        alpha=0.2, color='red')
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('Bandwidth (Gbps)')
        ax.set_title('Aggregate Bandwidth')
        ax.legend(loc='upper right')
        ax.grid(True, alpha=0.3)
        
        # Plot 2: Buffer Occupancy
        ax = axes[0, 1]
        ax.plot(time_arr, np.array(self.total_buffer_history) / 8e9, 
                color='purple', linewidth=1)
        ax.axhline(y=self.cfg.emergency_pool_gb, color='orange', linestyle='--',
                   label=f'Emergency Pool ({self.cfg.emergency_pool_gb} GB)')
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('Buffer (GB)')
        ax.set_title('Total Buffer Occupancy')
        ax.legend(loc='upper right')
        ax.grid(True, alpha=0.3)
        
        # Plot 3: Burst Count
        ax = axes[1, 0]
        ax.fill_between(time_arr, 0, self.burst_count_history, alpha=0.5, color='red')
        ax.plot(time_arr, self.burst_count_history, color='darkred', linewidth=1)
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('Concurrent Bursts')
        ax.set_title('Active Burst Streams')
        ax.grid(True, alpha=0.3)
        
        # Plot 4: Client Continuity Ratio
        ax = axes[1, 1]
        ax.plot(time_arr, np.array(self.continuity_history) * 100, 
                color='green', linewidth=1.5)
        ax.axhline(y=100, color='black', linestyle='--', alpha=0.5)
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('Continuity (%)')
        ax.set_title('Client Feed Continuity Ratio')
        ax.set_ylim([0, 105])
        ax.grid(True, alpha=0.3)
        
        # Plot 5: Sample Stream Bitrate (first 5 streams)
        ax = axes[2, 0]
        sample_streams = [0, self.cfg.n_cameras//4, self.cfg.n_cameras//2, 
                         3*self.cfg.n_cameras//4, self.cfg.n_cameras-1]
        colors = plt.cm.tab10(np.linspace(0, 1, len(sample_streams)))
        for idx, s_idx in enumerate(sample_streams):
            s = self.streams[s_idx]
            if s.bitrate_history:
                t_sample = np.arange(len(s.bitrate_history)) * 0.1
                ax.plot(t_sample, s.bitrate_history, 
                       label=f'Stream {s_idx}', color=colors[idx], alpha=0.8, linewidth=1)
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('Bitrate (Mbps)')
        ax.set_title('Sample Stream Bitrates')
        ax.legend(loc='upper right', fontsize=8)
        ax.grid(True, alpha=0.3)
        
        # Plot 6: Sample Stream Buffer Occupancy
        ax = axes[2, 1]
        for idx, s_idx in enumerate(sample_streams):
            s = self.streams[s_idx]
            if s.occupancy_history:
                t_sample = np.arange(len(s.occupancy_history)) * 0.1
                ax.plot(t_sample, np.array(s.occupancy_history) * 100,
                       label=f'Stream {s_idx}', color=colors[idx], alpha=0.8, linewidth=1)
        ax.axhline(y=WM_HIGH*100, color='orange', linestyle='--', alpha=0.5, label='High WM')
        ax.axhline(y=WM_CRITICAL*100, color='red', linestyle='--', alpha=0.5, label='Critical WM')
        ax.set_xlabel('Time (s)')
        ax.set_ylabel('Occupancy (%)')
        ax.set_title('Sample Stream Buffer Occupancy')
        ax.legend(loc='upper right', fontsize=8)
        ax.grid(True, alpha=0.3)
        
        plt.tight_layout(rect=[0, 0.03, 1, 0.95])
        
        plot_path = os.path.join(OUTPUT_DIR, f"{output_prefix}.png")
        plt.savefig(plot_path, dpi=150, bbox_inches='tight')
        print(f"Plot saved to: {plot_path}")
        plt.close()


# =============================================================================
# COMMAND LINE INTERFACE
# =============================================================================

def parse_args() -> SimulationConfig:
    parser = argparse.ArgumentParser(
        description='Camera Stream Burst Simulation',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Baseline scenario: 5000 cameras, up to 200 bursting
  python cam_stream_sim.py --cameras 5000 --burst-cameras 200 --duration 300 --plot

  # Stress test: 1000 cameras, 500 bursting, 40 Gbps NIC
  python cam_stream_sim.py --cameras 1000 --burst-cameras 500 --nic 40 --duration 300 --plot

  # Low-latency config: smaller buffers, aggressive controller
  python cam_stream_sim.py --cameras 5000 --burst-cameras 100 
      --buffer-s 1.0 --kp 1.0 --ki 0.2 --duration 300 --plot
        """)

    parser.add_argument('--cameras', '-n', type=int, default=5000,
                        help='Number of camera streams (default: 5000)')
    parser.add_argument('--burst-cameras', '-k', type=int, default=200,
                        help='Expected max concurrent bursts (default: 200)')
    parser.add_argument('--duration', '-d', type=float, default=300.0,
                        help='Simulation duration in seconds (default: 300)')
    parser.add_argument('--nic', type=float, default=25.0,
                        help='NIC bandwidth in Gbps (default: 25)')
    parser.add_argument('--buffer-s', type=float, default=2.0,
                        help='Per-stream buffer duration in seconds (default: 2)')
    parser.add_argument('--emergency-gb', type=float, default=64.0,
                        help='Emergency pool size in GB (default: 64)')
    parser.add_argument('--burst-min', type=float, default=3.0,
                        help='Minimum burst factor (default: 3)')
    parser.add_argument('--burst-max', type=float, default=10.0,
                        help='Maximum burst factor (default: 10)')
    parser.add_argument('--burst-rate', type=float, default=0.001,
                        help='Burst arrival rate per stream per second (default: 0.001)')
    parser.add_argument('--burst-recovery', type=float, default=0.1,
                        help='Burst recovery rate per second (default: 0.1)')
    parser.add_argument('--kp', type=float, default=0.5,
                        help='PI controller proportional gain (default: 0.5)')
    parser.add_argument('--ki', type=float, default=0.1,
                        help='PI controller integral gain (default: 0.1)')
    parser.add_argument('--seed', type=int, default=42,
                        help='Random seed (default: 42)')
    parser.add_argument('--plot', action='store_true',
                        help='Generate plots')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print per-step progress')
    
    args = parser.parse_args()
    
    return SimulationConfig(
        n_cameras=args.cameras,
        max_burst_cameras=args.burst_cameras,
        duration_seconds=args.duration,
        nic_bandwidth_gbps=args.nic,
        per_stream_buffer_s=args.buffer_s,
        emergency_pool_gb=args.emergency_gb,
        burst_factor_min=args.burst_min,
        burst_factor_max=args.burst_max,
        burst_arrival_rate=args.burst_rate,
        burst_recovery_rate=args.burst_recovery,
        pi_kp=args.kp,
        pi_ki=args.ki,
        seed=args.seed,
        plot=args.plot,
        verbose=args.verbose,
    )


def main():
    config = parse_args()

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    sim = CameraStreamSimulator(config)
    metrics = sim.run()
    sim.print_metrics(metrics)

    if config.plot:
        sim.plot()

    # Write metrics to file
    metrics_path = os.path.join(OUTPUT_DIR, "simulation_metrics.txt")
    with open(metrics_path, "w") as f:
        f.write("Camera Stream Burst Simulation - Metrics\n")
        f.write("=" * 50 + "\n\n")
        for key, value in metrics.items():
            if isinstance(value, float):
                f.write(f"{key}: {value:.6f}\n")
            else:
                f.write(f"{key}: {value}\n")

    print()
    print(f"Metrics saved to: {metrics_path}")


if __name__ == "__main__":
    main()
