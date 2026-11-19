# Burst-Resilient Camera Stream Processing System

> **Merged documentation: ARCHITECTURE + DESIGN + MODELING**
> Reconstructed and merged from `kimi_web_output.txt` (a Kimi chat transcript that produced `README.md`, `ARCHITECTURE.md`, `DESIGN.md`, `MODELING.md`, and the Python simulator `cam_stream_sim.py`). This file consolidates the three substantive technical documents (Architecture, Design, Modeling) into a single reference.

---

## Problem Statement

A single physical server must ingest, process, and persist up to **N = 5000** concurrent H.265 camera streams with an aggregate baseline bandwidth **B**. At arbitrary times *Tᵢ*, a subset of *K < N* streams experience traffic bursts that temporarily exceed the system's sustainable capacity, causing frame loss and interrupted client feeds.

## Assumptions & Constraints

| Parameter | Value / Constraint |
|---|---|
| N | ≤ 5000 ingress streams |
| Codec | H.265 (HEVC) — variable bitrate, GOP-structured |
| Hardware | Single physical server (no cloud offload) |
| Network | 25/40/100 GbE NIC; kernel-bypass or high-performance socket path |
| Storage | Tiered: RAM → NVMe SSD → SATA/SAS (DB) |
| Burst definition | K streams increase bitrate by factor α (typically 3×–10×) for window W |
| Client requirement | Zero visible interruption; latency ≤ 2 s normal, ≤ 5 s during burst |

## Solution Summary

The system employs a three-tier defense against bursts:

1. **Temporal Absorption (Buffering)** — Large per-stream ring buffers and a shared emergency pool absorb bursts up to tens of seconds.
2. **Dynamic Adaptation** — A control loop monitors buffer fill levels and commands cameras (or transcodes locally) to reduce bitrate via CRF increase, resolution downscale, or frame-rate decimation.
3. **Intelligent Shedding** — If buffers near exhaustion, non-client-facing data (high-res archive, analytics frames) are shed first; client streams are preserved.

> **Core insight:** decouple ingestion from client serving. Clients are served from a staged buffer pool. Bursts are absorbed in RAM, while an adaptation engine proactively throttles ingress bitrate before buffers exhaust. Non-critical metadata may be temporarily shed, but the client-visible stream remains continuous.

---

# PART I — Architecture

## 1. High-Level Topology

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                         SINGLE PHYSICAL SERVER                              │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                         INGRESS LAYER                               │    │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐    │    │
│  │  │ 25/40/100   │  │   DPDK /    │  │   Stream    │  │  NIC     │    │    │
│  │  │    GbE NIC  │──│ Kernel-Bypass│─│  Demuxer    │──│  HW RSS  │    │    │
│  │  │  (Multi-Q)  │  │  (Zero-Copy)│  │  (H.265 NAL)│  │  Queues  │    │    │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                        │
│                                    ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                     BUFFER & ADAPTATION LAYER                       │    │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐    │    │
│  │  │ Per-Stream  │  │   Shared    │  │   Burst     │  │  PI      │    │    │
│  │  │ Ring Buffer │  │  Emergency  │  │  Detector   │  │Controller│    │    │
│  │  │  (2s each)  │  │    Pool     │  │  (Markov)   │  │          │    │    │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                        │
│                                    ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                   PROCESSING & TRANSCODE LAYER                      │    │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐    │    │
│  │  │  H.265      │  │   Quality   │  │   Frame     │  │  GPU/FPGA│    │    │
│  │  │  Decoder    │  │   Scaler    │  │  Scheduler  │  │  Offload │    │    │
│  │  │  (HW accel) │  │  (CRF/Res)  │  │  (Priority) │  │  (opt)   │    │    │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
│                                    │                                        │
│                                    ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐    │
│  │                      STORAGE & DISTRIBUTION LAYER                   │    │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌──────────┐    │    │
│  │  │   RAM       │  │   NVMe      │  │   Database  │  │  Client  │    │    │
│  │  │  Staging    │──│  Write-Back │──│  (Archive)  │──│  Server  │    │    │
│  │  │   Pool      │  │   Journal   │  │  (Timescale)│  │ (WebRTC) │    │    │
│  │  └─────────────┘  └─────────────┘  └─────────────┘  └──────────┘    │    │
│  └─────────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 2. Component Descriptions

### 2.1 Ingress Layer

**NIC with Hardware RSS (Receive Side Scaling)**

- 25/40/100 GbE NIC with ≥ 16 hardware queues.
- Flow Director / RSS hashes on source IP+port to distribute streams across CPU cores.
- Eliminates single-core bottleneck at 15+ Gbps.

**DPDK / Kernel-Bypass (Optional but Recommended)**

- For 5000 streams, kernel networking overhead is manageable with busy-polling (e.g., `SO_BUSY_POLL`, XDP).
- DPDK provides zero-copy packet reception directly into user-space buffers.
- Reduces per-packet latency from ~5 µs (kernel) to ~1 µs (DPDK).

**H.265 Stream Demuxer**

- Parses RTP/UDP or RTSP streams to extract H.265 NAL units.
- Identifies stream ID, sequence number, timestamp.
- Detects GOP boundaries (I-frame, P-frame, B-frame).
- Routes NAL units to the appropriate per-stream ring buffer.

### 2.2 Buffer & Adaptation Layer

**Per-Stream Ring Buffer**

- Fixed-size circular buffer per camera (e.g., 2 seconds @ max bitrate).
- Lock-free SPSC (single-producer, single-consumer) queue per stream.
- Producer: ingress thread. Consumer: processing thread.
- Water marks:
  - **Low (20%):** Normal operation.
  - **High (60%):** Trigger pre-emptive adaptation (mild CRF increase).
  - **Critical (85%):** Aggressive adaptation + emergency pool allocation.
  - **Overflow (95%):** Shed non-critical data; preserve I-frames for client continuity.

**Shared Emergency Pool**

- A large shared memory pool (e.g., 64 GB) allocated from huge pages.
- Dynamically assigned to bursty streams when their per-stream buffer hits the critical mark.
- Released back to the pool when burst subsides.

**Burst Detector**

- Monitors per-stream bitrate using an exponentially weighted moving average (EWMA).
- Flags a stream as "bursty" when instantaneous bitrate > 2× EWMA baseline.
- Triggers the PI controller and alerts the frame scheduler.

**PI Controller (Proportional-Integral)**

- Control variable: target bitrate reduction Δb.
- Error term: `e(t) = buffer_occupancy(t) − target_occupancy (40%)`.
- Output: `Δb(t) = Kₚ·e(t) + Kᵢ·∫e(t)dt`.
- Δb is translated into codec parameters: CRF offset, resolution scale factor, or frame-skip ratio.

### 2.3 Processing & Transcode Layer

**H.265 Decoder (Hardware-Accelerated)**

- Intel QSV, NVIDIA NVDEC, or AMD VCE for parallel decode of multiple streams.
- Decode is only performed if analytics or transcoding is required.
- For passthrough (no processing), NAL units are forwarded directly to storage/distribution.

**Quality Scaler**

- Adjusts CRF (Constant Rate Factor) to increase compression.
- Downscales resolution (e.g., 1080p → 720p → 480p) via fast bilinear or Lanczos.
- Temporal decimation: drops B-frames first, then P-frames, never I-frames.
- All adjustments are reversible — when burst ends, quality is restored.

**Frame Scheduler**

Priority classes:

1. **P0 (Critical):** Client-facing live stream — never dropped.
2. **P1 (Analytics):** AI/ML inference frames — sheddable with delay.
3. **P2 (Archive):** Full-resolution storage — sheddable, write-behind.

- Weighted fair queuing (WFQ) across streams within each priority.

**GPU/FPGA Offload (Optional)**

- NVIDIA Jetson / Tesla or Intel Flex for mass-transcode.
- Can transcode 100+ 1080p H.265 streams per GPU.
- For 5000 streams, a pool of 4–8 GPUs provides massive parallel transcoding.

### 2.4 Storage & Distribution Layer

**RAM Staging Pool**

- Hot buffers for immediate client access.
- Client server reads from here with sub-millisecond latency.

**NVMe Write-Back Journal**

- Fast append-only journal on NVMe SSD (e.g., Intel Optane or Samsung PM1733).
- Absorbs write spikes during bursts.
- Asynchronously flushed to the database.

**Database (TimescaleDB / ClickHouse)**

- Stores metadata, stream health, and compressed archive segments.
- Uses hypertables for time-series data.
- Write path decoupled via NVMe journal — DB never blocks the live path.

**Client Server (WebRTC / RTMP / HLS)**

- Serves live streams from RAM staging.
- During burst, may serve slightly delayed or lower-quality streams, but never drops the connection.
- Implements adaptive bitrate (ABR) so clients seamlessly switch to lower-quality variants.

## 3. Data Flow During Burst

**Normal Operation:**

```text
Camera ──► NIC ──► Demux ──► Ring Buffer ──► Passthrough ──► Client
                            └─► NVMe Journal ──► DB
```

**Burst Detected (K streams, 3× bitrate):**

```text
Camera ──► NIC ──► Demux ──► Ring Buffer ──► Quality Scaler ──► Client
                          │                 │
                          ▼                 ▼
                    Emergency Pool    Lower CRF / 720p
                          │
                Burst Detector ──► PI Controller
```

**Critical Overflow (buffers > 95%):**

```text
Camera ──► NIC ──► Demux ──► Ring Buffer ──► I-frame only ──► Client
                          └─► Shed P/B frames
                          └─► Delay archive writes
```

## 4. Hardware Layout (Single Server)

```text
┌────────────────────────────────────────────────────────┐
│  Motherboard: Dual-socket AMD EPYC / Intel Xeon        │
│  RAM: 512 GB DDR4-3200 (or DDR5-4800)                  │
│  NIC: Mellanox ConnectX-6 DX 100 GbE (2× for HA)       │
│  Storage:                                              │
│    - 2× 3.84 TB NVMe U.2 (RAID-1 journal)              │
│    - 8× 16 TB SATA/SAS (RAID-10 archive)               │
│  GPU (optional): 4× NVIDIA A2 / Intel Flex 170         │
│  OS: Linux (Ubuntu 22.04 LTS or RHEL 9)                │
│  Kernel: 6.x with PREEMPT_RT or low-latency profile    │
└────────────────────────────────────────────────────────┘
```

## 5. Threading Model

```text
┌────────────────────────────────────────────────────────────┐
│  Core 0-1:    OS / Management / Monitoring                 │
│  Core 2-5:    NIC Polling (DPDK / busy-poll)               │
│  Core 6-13:   Ingress Demux (1 core per 625 streams)       │
│  Core 14-21:  Buffer Management + Burst Detection          │
│  Core 22-29:  Processing / Transcode (CPU fallback)        │
│  Core 30-37:  Storage Writer (NVMe journal + DB flush)     │
│  Core 38-47:  Client Server (WebRTC / distribution)        │
│  GPU 0-3:     Hardware transcode (if equipped)             │
└────────────────────────────────────────────────────────────┘
```

## 6. Failure Modes & Mitigation

| Failure | Mitigation |
|---|---|
| Single core saturation | RSS redistribution; migrate streams to idle cores |
| NVMe journal full | Backpressure to PI controller; increase adaptation aggressiveness |
| DB write stall | NVMe journal acts as infinite (disk-sized) buffer; alerts ops |
| GPU transcode failure | Fallback to CPU scaler (slower but functional) |
| Power / thermal throttle | Reduce non-critical processing; preserve client streams |
| NIC RX drops | Increase ring descriptor count; enable flow control (PAUSE frames) |

## 7. Scaling Within the Box

While the constraint is a single physical server, the architecture is designed to maximize utilization:

- **Vertical scaling:** More CPU cores, more RAM, faster NIC, GPUs.
- **NUMA awareness:** Pin threads to NUMA nodes local to their NIC / GPU / NVMe.
- **Memory hierarchy:** L3 cache → DRAM → NVMe → SATA, with data placement optimized for access patterns.


---

# PART II — Design

## 1. Design Principles

1. **Client Continuity First:** The live client feed is the highest priority. All other data (archive, analytics) is secondary and sheddable.
2. **Graceful Degradation:** When resources are scarce, reduce quality (bitrate, resolution, frame rate) rather than drop connections.
3. **Predictive, Not Reactive:** Detect burst onset early via EWMA deviation and begin adaptation before buffers fill.
4. **Zero-Copy Where Possible:** Minimize memory copies between ingress, buffers, and distribution.
5. **Deterministic Behavior:** Lock-free data structures, pre-allocated pools, and bounded queues ensure predictable latency.

## 2. Core Data Structures

### 2.1 Per-Stream Ring Buffer (`StreamRingBuffer`)

```c
struct StreamRingBuffer {
    uint32_t    stream_id;          // Unique camera identifier
    uint8_t*    base_addr;          // Pointer to buffer memory (hugepage-backed)
    size_t      capacity;           // Bytes (e.g., 2s @ 10 Mbps = 2.5 MB)

    // Lock-free indices (atomic)
    alignas(64) std::atomic<size_t> write_idx;
    alignas(64) std::atomic<size_t> read_idx;

    // Water marks (bytes)
    size_t      wm_low;             // 20% of capacity
    size_t      wm_high;            // 60% of capacity
    size_t      wm_critical;        // 85% of capacity
    size_t      wm_overflow;        // 95% of capacity

    // State
    StreamState state;              // NORMAL, ADAPTING, CRITICAL, OVERFLOW
    float       current_crf;        // Current CRF offset applied
    uint32_t    current_scale;      // Resolution scale factor (1, 2, 4)

    // Statistics
    uint64_t    frames_received;
    uint64_t    frames_dropped;
    uint64_t    bytes_received;
    uint64_t    bytes_served;
};
```

- **Alignment:** The struct is padded to 64-byte cache lines to prevent false sharing between cores handling different streams.
- **Lock-Free Semantics:** Single producer (ingress thread) and single consumer (processing thread). Uses power-of-2 sizing with bitwise AND for index wrapping.

### 2.2 Shared Emergency Pool (`EmergencyPool`)

```c
struct EmergencyPool {
    uint8_t*    base_addr;          // Hugepage-backed contiguous block
    size_t      total_size;         // e.g., 64 GB
    size_t      block_size;         // e.g., 2.5 MB (one ring buffer worth)

    // Bitmap allocator (lock-free stack)
    alignas(64) std::atomic<uint32_t> free_stack_top;
    uint32_t*   free_stack;         // Array of free block indices

    // Per-block metadata
    struct BlockMeta {
        uint32_t stream_id;         // Which stream owns this block
        uint64_t acquired_at;       // Timestamp (for timeout/reclamation)
    }* block_meta;
};
```

- **Allocation Strategy:** When a per-stream buffer hits `wm_critical`, it requests an emergency block. If available, the block is appended as an extension to the ring buffer (logically a two-segment ring). If unavailable, the stream enters OVERFLOW state.
- **Reclamation:** A background thread scans `acquired_at` timestamps. Blocks held for > 30 seconds (configurable) are forcibly reclaimed to prevent starvation.

### 2.3 NAL Unit Descriptor (`NALDescriptor`)

```c
struct NALDescriptor {
    uint32_t    stream_id;
    uint64_t    pts;                // Presentation timestamp (µs)
    uint64_t    dts;                // Decode timestamp (µs)
    uint16_t    nal_type;           // H.265 NAL unit type
    uint16_t    flags;              // KEY_FRAME, B_FRAME, P_FRAME, etc.
    uint32_t    length;             // Payload length in bytes
    uint8_t*    payload;            // Pointer into ring buffer (zero-copy)
};
```

## 3. Algorithms

### 3.1 Burst Detection Algorithm

Each stream maintains two EWMAs:

- **Slow EWMA (α = 0.01):** Represents baseline bitrate.
- **Fast EWMA (α = 0.2):** Represents instantaneous bitrate.

```python
def detect_burst(stream):
    fast = ewma_fast.update(stream.instantaneous_bitrate)
    slow = ewma_slow.update(stream.instantaneous_bitrate)

    ratio = fast / slow if slow > 0 else 1.0

    if ratio > BURST_THRESHOLD and stream.state == NORMAL:
        stream.state = BURSTING
        trigger_adaptation(stream, ratio)
    elif ratio < RECOVERY_THRESHOLD and stream.state == BURSTING:
        stream.state = RECOVERING
        schedule_quality_restoration(stream)
```

**Parameters:**

- `BURST_THRESHOLD = 2.0` (fast EWMA > 2× slow EWMA)
- `RECOVERY_THRESHOLD = 1.3` (fast EWMA < 1.3× slow EWMA for > 5 seconds)

### 3.2 PI Controller for Adaptation

The controller runs at 100 Hz (every 10 ms) for all active streams.

```python
def pi_controller_step(stream):
    # Target occupancy: 40% of buffer capacity
    target = stream.capacity * 0.4
    error = stream.occupancy() - target

    # Proportional term
    p_term = KP * error

    # Integral term (clamped to prevent windup)
    stream.integral += KI * error * DT
    stream.integral = clamp(stream.integral, -MAX_INTEGRAL, MAX_INTEGRAL)

    # Control output: desired bitrate reduction (bps)
    reduction = p_term + stream.integral

    # Map reduction to codec parameters
    if reduction > 0:
        apply_bitrate_reduction(stream, reduction)
    else:
        apply_bitrate_increase(stream, -reduction)
```

**Tuning Parameters** (determined empirically / via simulation):

- `KP = 0.5 / capacity` (normalized by buffer size)
- `KI = 0.1 / capacity`
- `DT = 0.01 s`
- `MAX_INTEGRAL = 0.3 * capacity`

### 3.3 Bitrate-to-Codec Parameter Mapping

Given a desired bitrate reduction Δb (bps), the system maps this to actionable parameters in order of visual impact (least noticeable first):

```python
def apply_bitrate_reduction(stream, delta_b):
    current_b = stream.current_bitrate
    target_b = current_b - delta_b
    ratio = target_b / current_b

    # Stage 1: Increase CRF (quality reduction, ~20% bitrate reduction per +6 CRF)
    if ratio >= 0.6:
        crf_increase = min(6 * (1 - ratio) / 0.2, 12)
        stream.crf += crf_increase
        return

    # Stage 2: Downscale resolution (halving resolution ≈ 50% bitrate reduction)
    if ratio >= 0.3:
        if stream.resolution == 1080:
            stream.resolution = 720
        elif stream.resolution == 720:
            stream.resolution = 480
        # Re-tune CRF for new resolution
        stream.crf = base_crf_for_resolution(stream.resolution)
        return

    # Stage 3: Temporal decimation (drop B-frames, then P-frames)
    if ratio >= 0.1:
        stream.drop_b_frames = True
        if ratio < 0.2:
            stream.drop_p_frames = True  # Serve I-frames only (stuttery but continuous)
        return

    # Stage 4: Emergency — shed non-client data, preserve stream at all costs
    stream.shed_priority = P2  # Shed archive and analytics
```

### 3.4 Frame Scheduling (Weighted Fair Queuing)

The frame scheduler ensures that client-facing streams (P0) are never starved, even when archive writes (P2) are backlogged.

```python
def schedule_frame(streams, current_time):
    for priority in [P0, P1, P2]:
        ready = [s for s in streams if s.has_ready_frame() and s.priority == priority]

        if priority == P0:
            # Strict priority: serve all P0 frames immediately
            for s in ready:
                emit_frame(s, current_time)
        else:
            # Weighted fair queuing for P1/P2
            for s in ready:
                if s.virtual_finish_time <= current_time:
                    emit_frame(s, current_time)
                    s.virtual_finish_time += s.frame_size / s.weight
```

**Weights:**

- P0 (Client live): weight = ∞ (strict priority)
- P1 (Analytics): weight = 2.0
- P2 (Archive): weight = 1.0

### 3.5 Write-Behind Storage Protocol

To prevent DB write stalls from blocking the live path:

1. Live frames are always written to RAM staging first.
2. A background "storage writer" thread drains RAM staging to NVMe journal.
3. The NVMe journal is an append-only circular buffer (e.g., 2 TB).
4. Another background thread replays NVMe journal → Database.
5. If NVMe journal fills up, backpressure is applied to P2 (archive) streams.
6. If backpressure propagates to P0, the PI controller increases adaptation.

**Durability Levels:**

- **P0:** RAM staging only (no persistence requirement for live feed).
- **P1:** RAM staging + NVMe journal (analytics can tolerate seconds of delay).
- **P2:** RAM staging + NVMe journal + DB (archive is eventually consistent).

## 4. State Machine per Stream

```text
                    ┌─────────────┐
                    │   NORMAL    │
                    └──────┬──────┘
                           │ buffer > wm_high OR burst detected
                           ▼
                   ┌─────────────┐
         ┌─────────│  ADAPTING   │◄────────┐
         │         └──────┬──────┘         │
         │                │ buffer > wm_critical
         │                ▼                │
         │         ┌─────────────┐         │
         │         │  CRITICAL   │         │
         │         └──────┬──────┘         │
         │                │ buffer > wm_overflow
         │                ▼                │
         │         ┌─────────────┐         │
         └────────►│  OVERFLOW   │─────────┘
          recovery │  (shedding) │
                   └──────┬──────┘
                          │ ratio < recovery_threshold
                          ▼
                   ┌─────────────┐
                   │  RECOVERING │
                   └──────┬──────┘
                          │ quality fully restored
                          ▼
                   ┌─────────────┐
                   │   NORMAL    │
                   └─────────────┘
```

## 5. API Contracts

### 5.1 Ingress → Buffer Manager

```c
// Called by ingress thread (per packet)
BufferResult ingest_nal(uint32_t stream_id, NALDescriptor* nal);

enum BufferResult {
    BUFFER_OK,           // NAL accepted into ring buffer
    BUFFER_ADAPT,        // NAL accepted, but adaptation triggered
    BUFFER_EMERGENCY,    // NAL accepted into emergency pool
    BUFFER_DROP,         // NAL dropped (overflow)
    BUFFER_ERROR         // Invalid stream_id
};
```

### 5.2 Buffer Manager → Processing

```c
// Called by processing thread
bool consume_nal(uint32_t stream_id, NALDescriptor* out_nal);

// Query state
StreamState get_stream_state(uint32_t stream_id);
float get_stream_occupancy(uint32_t stream_id);  // 0.0 – 1.0
```

### 5.3 Controller → Adaptation

```c
// Called by PI controller at 100 Hz
void request_bitrate_adjustment(uint32_t stream_id, float delta_bps);

// Called by burst detector
void notify_burst_start(uint32_t stream_id, float severity);
void notify_burst_end(uint32_t stream_id);
```

## 6. Configuration Parameters

| Parameter              | Default | Description                        |
|------------------------|---------|------------------------------------|
| `N_MAX_STREAMS`        | 5000    | Maximum concurrent ingress streams |
| `PER_STREAM_BUFFER_MS` | 2000    | Per-stream ring buffer duration    |
| `EMERGENCY_POOL_GB`    | 64      | Shared emergency pool size         |
| `BURST_THRESHOLD`      | 2.0     | EWMA ratio to declare burst        |
| `RECOVERY_THRESHOLD`   | 1.3     | EWMA ratio to declare recovery     |
| `PI_KP`                | 0.5     | Proportional gain (normalized)     |
| `PI_KI`                | 0.1     | Integral gain (normalized)         |
| `PI_HZ`                | 100     | Controller update frequency        |
| `MAX_CRF_OFFSET`       | 12      | Maximum CRF increase from baseline |
| `MIN_RESOLUTION`       | 480     | Minimum vertical resolution        |
| `NVME_JOURNAL_GB`      | 2048    | NVMe circular journal size         |
| `DB_FLUSH_INTERVAL_MS` | 1000    | Database batch flush interval      |

## 7. Monitoring & Observability

### 7.1 Key Metrics (Prometheus-compatible)

```text
ingress_bitrate_total{stream_id}        # Current ingress bitrate per stream
buffer_occupancy_ratio{stream_id}       # 0.0 – 1.0
frames_dropped_total{stream_id,reason}  # reason: overflow, adaptation, shed
adaptation_events_total{stream_id,type} # type: crf, resolution, temporal
client_continuity_ratio{stream_id}      # 1.0 = no gaps in client feed
burst_duration_seconds{stream_id}       # Duration of last burst
pi_controller_output{stream_id}         # Current reduction demand
```

### 7.2 Alerting Rules

```yaml
- alert: HighBufferOccupancy
  expr: buffer_occupancy_ratio > 0.8
  for: 5s
  labels:
    severity: warning

- alert: ClientFeedInterrupted
  expr: client_continuity_ratio < 1.0
  for: 1s
  labels:
    severity: critical

- alert: EmergencyPoolExhausted
  expr: emergency_pool_free_bytes / emergency_pool_total_bytes < 0.1
  for: 10s
  labels:
    severity: critical
```

## 8. Testing Strategy

- **Unit Tests:** Lock-free queue correctness, PI controller stability, EWMA accuracy.
- **Integration Tests:** End-to-end burst injection with synthetic H.265 streams.
- **Chaos Tests:** Random CPU throttling, memory pressure, NIC queue drops.
- **Load Tests:** Ramp N from 0 to 5000 with Poisson-distributed bursts.
- **Long-Running Tests:** 72-hour soak with Markov-modulated burst patterns.


---

# PART III — Modeling

## 1. System Model

### 1.1 Definitions

| Symbol| Definition                             | Typical Value         |
|-------|----------------------------------------|-----------------------|
| N     | Total number of ingress camera streams | ≤ 5000                |
| K     | Number of bursty streams at time Tᵢ    | K < N                 |
| B     | Aggregate sustainable system bandwidth (bps) | 15–40 Gbps |
| bᵢ(t) | Instantaneous bitrate of stream i at time t | Variable |
| b̄     | Baseline average bitrate per stream | 3 Mbps (H.265 1080p) |
| α     | Burst multiplication factor | 3–10× |
| W     | Burst duration (seconds) | Pareto-distributed, mean 10 s |
| M     | Total RAM buffer pool (bits) | 512 Gb = 64 GB |
| m     | Per-stream ring buffer (bits) | 2 s × b̄ = 6 Mb = 0.75 MB |
| Mₑ    | Emergency pool size (bits) | 512 Gb = 64 GB |
| D     | Maximum allowable client latency | 2 s (normal), 5 s (burst) |
| R     | NVMe write bandwidth | 6 GB/s |
| C     | CPU processing capacity (streams @ 1080p) | 500–2000 (HW accel) |

> **Note on the source's notation:** the transcript inconsistently uses "512 Gb = 64 GB" for both M and Mₑ (i.e., 512 Gb·bits = 64 GB). The numerical examples below (e.g., the 213-second absorption window and the per-stream m = 6 Mb = 0.75 MB) are reproduced exactly as written in the source.

### 1.2 Conservation Equation

At any time *t*, the system must satisfy:

```text
Σᵢ₌₁ᴺ bᵢ(t) ≤ B + dM/dt
```

Where `dM/dt` is the rate at which the buffer pool is being depleted. When buffers are full (*M* = 0 available), the hard constraint becomes:

```text
Σᵢ₌₁ᴺ bᵢ(t) ≤ B
```

If this is violated and no adaptation occurs, frames must be dropped.

## 2. Traffic Model

### 2.1 Baseline Stream Model

Each camera stream is modeled as a variable bitrate (VBR) source with:

- GOP structure: I-B-B-P-B-B-P-... (H.265 typical)
- I-frame size: ~8× average frame size
- P-frame size: ~1.5× average frame size
- B-frame size: ~0.5× average frame size

The instantaneous bitrate can be approximated as:

```text
bᵢ(t) = b̄ + σ·sin(2πt/T_gop) + ε(t)
```

Where:

- b̄ = mean bitrate
- σ = amplitude of GOP-induced variation (~30% of b̄)
- T_gop = GOP period (e.g., 2 seconds for 60-frame GOP @ 30 fps)
- ε(t) = zero-mean Gaussian noise, std = 0.1·b̄

### 2.2 Burst Model: Markov-Modulated Poisson Process (MMPP)

Camera streams alternate between two states:

```text
         λ_on
NORMAL ───────► BURST
◄──────────────
    λ_off
```

- **NORMAL state:** Bitrate = b̄ (baseline)
- **BURST state:** Bitrate = α·b̄ (elevated)
- Transition rates:
  - λ_on: Rate of entering burst (events/second per stream)
  - λ_off: Rate of leaving burst = 1/E[W]

For K bursty streams out of N:

```text
E[K] = N · (λ_on / (λ_on + λ_off))
```

### 2.3 Aggregate Traffic During Burst

At time *Tᵢ* when K streams burst:

```text
B_total(Tᵢ) = (N − K)·b̄ + K·α·b̄
            = N·b̄ + K·b̄·(α − 1)
```

The excess bandwidth is:

```text
ΔB = B_total(Tᵢ) − B
   = N·b̄ + K·b̄·(α − 1) − B
```

For the system to survive without dropping client frames, we require:

```text
∫₀ᵂ ΔB(t) dt ≤ M_available
```

Where `M_available` is the total buffer capacity available to absorb the burst.

## 3. Buffer Model: Fluid Flow Approximation

### 3.1 Single-Stream Buffer

Model the per-stream ring buffer as a G/G/1 queue with:

- Arrival rate: λ(t) = bᵢ(t) (bits/second)
- Service rate: μ(t) = b̄ (sustainable drain rate)

The buffer occupancy Q(t) evolves as:

```text
dQ/dt = λ(t) − μ(t)    if Q(t) > 0 or λ(t) > μ(t)
dQ/dt = 0              if Q(t) = 0 and λ(t) ≤ μ(t)
```

### 3.2 Multi-Stream with Emergency Pool

For K bursty streams sharing emergency pool Mₑ:

```text
dQᵢ/dt = b̄·α − b̄ = b̄·(α − 1)    for i ∈ {bursty streams}
dQᵢ/dt = 0                        for i ∈ {normal streams}
```

Total buffer consumption rate:

```text
dQ_total/dt = K · b̄ · (α − 1)
```

Time to overflow (without adaptation):

```text
T_overflow = (K · m + Mₑ) / (K · b̄ · (α − 1))
```

Where `K·m` is the aggregate per-stream buffer and `Mₑ` is the emergency pool.

**Example:**

- K = 200, b̄ = 3 Mbps, α = 5, m = 6 Mb, Mₑ = 512 Gb
- T_overflow = (200 × 6×10⁶ + 512×10⁹) / (200 × 3×10⁶ × 4)
- T_overflow = (1.2×10⁹ + 512×10⁹) / (2.4×10⁹)
- **T_overflow ≈ 213 seconds**

This is the absorption window — the time the system can sustain the burst purely on buffering.

### 3.3 Buffer with Adaptation

When the PI controller activates at *t = t_adapt*, it reduces the ingress bitrate. Let the adaptation function be:

```text
bᵢ_adapted(t) = b̄ · α · (1 − β(t))
```

Where β(t) is the reduction factor applied by the controller. The buffer dynamics become:

```text
dQ/dt = K · b̄ · α · (1 − β(t)) − K · b̄    for t > t_adapt
```

For stability, we require β(t) to drive `dQ/dt ≤ 0` before overflow:

```text
β(t) ≥ 1 − 1/α = (α − 1)/α
```

For α = 5, we need β ≥ 0.8, i.e., **at least 80% bitrate reduction** to stop buffer growth.

## 4. PI Controller Analysis

### 4.1 Control Loop Transfer Function

The PI controller in the Laplace domain:

```text
C(s) = Kₚ + Kᵢ/s
```

The plant (buffer) is an integrator:

```text
P(s) = 1/s
```

Closed-loop transfer function:

```text
H(s) = C(s)·P(s) / (1 + C(s)·P(s))
     = (Kₚ·s + Kᵢ) / (s² + Kₚ·s + Kᵢ)
```

### 4.2 Stability Criteria

For a second-order system `s² + 2ζωₙs + ωₙ² = 0`:

```text
ωₙ = √Kᵢ          (natural frequency)
ζ  = Kₚ / (2√Kᵢ)  (damping ratio)
```

For critical damping (fastest settling without oscillation):

```text
ζ = 1  →  Kₚ = 2√Kᵢ
```

For 5% settling time:

```text
Tₛ ≈ 3 / (ζ·ωₙ) = 6 / Kₚ
```

With Kₚ = 0.5 (normalized), Tₛ ≈ 12 controller steps = **120 ms at 100 Hz**.

### 4.3 Anti-Windup

The integral term is clamped to prevent windup during sustained bursts:

```text
|I(t)| ≤ I_max = 0.3 · Q_max
```

Where `Q_max` is the maximum buffer capacity per stream.

## 5. Capacity Planning

### 5.1 Sustainable Capacity

The system can sustainably process:

```text
B = min(B_nic, B_cpu, B_disk, B_memory)
```

Where:

- **B_nic** = NIC bandwidth (e.g., 25 Gbps)
- **B_cpu** = CPU decode/encode capacity (e.g., 2000 × 3 Mbps = 6 Gbps with HW accel)
- **B_disk** = Sustained disk write bandwidth (e.g., NVMe RAID = 10 GB/s = 80 Gbps)
- **B_memory** = Memory bandwidth (e.g., DDR4-3200 quad-channel = 100+ GB/s)

For 5000 streams @ 3 Mbps = 15 Gbps, the NIC is the bottleneck on a 25 GbE link, but with 40/100 GbE, CPU or storage may become limiting.

### 5.2 Headroom Requirement

To absorb bursts without immediate adaptation:

```text
B_sustainable = N · b̄ · (1 + h)
```

Where *h* is headroom fraction. Typical values:

- *h = 0.2* (20% headroom) for predictable workloads
- *h = 0.5* (50% headroom) for highly bursty workloads

With h = 0.2 and N = 5000, b̄ = 3 Mbps:

- B_sustainable = 5000 × 3 × 1.2 = **18 Gbps**
- Requires 25 GbE NIC with some margin

### 5.3 Burst Absorption Budget

The total "burst energy" the system can absorb is:

```text
E_burst = M_total · D_max
```

Where:

- M_total = N·m + Mₑ (total buffer pool)
- D_max = maximum allowable latency increase

For M_total = 512 GB, D_max = 5 s:

- E_burst = 512 GB × 5 s = **2560 GB·s**

A burst of K = 500 streams at α = 5 for W = 10 s consumes:

- E_consumed = 500 × 3 Mbps × 4 × 10 s = 60 Gb = 7.5 GB
- Well within budget.

## 6. Quality-of-Service Guarantees

### 6.1 Client Continuity Guarantee

> **Theorem:** If the PI controller is tuned for critical damping and the emergency pool Mₑ ≥ K_max · b̄ · (α_max − 1) · T_react, then no client feed is interrupted during any burst.

**Proof Sketch:**

1. Burst detection occurs within T_detect = 5 / λ_on seconds (5 EWMA time constants).
2. Controller reacts within T_react = T_detect + Tₛ ≈ 200 ms.
3. During T_react, buffer grows by ΔQ = K · b̄ · (α − 1) · T_react.
4. If Mₑ ≥ ΔQ for worst-case K and α, no overflow occurs.
5. Once controller activates, β(t) drives `dQ/dt ≤ 0` within Tₛ.
6. Client feed is served from buffer, which never empties completely due to continuous (adapted) ingress. ∎

### 6.2 Latency Bound

End-to-end latency during burst:

```text
L_burst = L_normal + ΔL_buffer + ΔL_transcode
```

Where:

- L_normal ≈ 100–200 ms (network + processing + distribution)
- ΔL_buffer ≤ D_max = 5 s (buffer absorption)
- ΔL_transcode ≤ 50 ms (quality adaptation adds minimal latency)

**Guarantee:** L_burst ≤ 5.5 s with 99.9% probability.

## 7. Statistical Analysis

### 7.1 Probability of Concurrent Bursts

If each stream bursts independently with probability `p = λ_on / (λ_on + λ_off)`:

```text
P(K concurrent bursts) = C(N,K) · pᴷ · (1−p)^(N−K)
```

For N = 5000, p = 0.01 (1% of streams bursting at any time):

- E[K] = 50
- P(K > 200) ≈ 10⁻¹⁸ (negligible)

### 7.2 Expected Buffer Utilization

Using Little's Law for the buffer system:

```text
E[Q] = λ · E[W]
```

Where:

- λ = effective arrival rate into buffers
- E[W] = expected time a bit spends in buffer

During normal operation:

- E[Q] ≈ 0.3 · Q_max (target occupancy for PI controller)

During burst:

- E[Q] rises to 0.6–0.8 · Q_max before adaptation stabilizes.

## 8. Optimization Objectives

### 8.1 Multi-Objective Formulation

The system optimizes:

```text
minimize    Σᵢ w₁·Dᵢ + w₂·Lᵢ + w₃·(1 − Qᵢ)

subject to  Σᵢ bᵢ(t) ≤ B + dM/dt
            Qᵢ(t) ≤ Q_max
            Lᵢ ≤ L_max
            Dᵢ ≥ D_min
```

Where:

- Dᵢ = continuity metric for stream i (1.0 = perfect)
- Lᵢ = latency for stream i
- Qᵢ = quality metric (PSNR or VMAF)
- w₁, w₂, w₃ = weights (w₁ ≫ w₂ ≫ w₃ for client-priority systems)

### 8.2 Pareto Frontier

There is a fundamental trade-off between:

- **Latency ↔ Quality:** Lower latency requires less buffering, leaving less time for adaptation.
- **Continuity ↔ Quality:** To guarantee continuity, quality must be reduced during bursts.
- **Cost ↔ Performance:** More RAM and faster NICs improve all metrics but increase cost.

The design point chosen in this architecture prioritizes **continuity > latency > quality**.

## 9. Simulation Parameters for Validation

The `cam_stream_sim.py` simulator uses the following parameters derived from this model:

| Parameter | Symbol | Value |
|---|---|---|
| Baseline bitrate | b̄ | 3 Mbps |
| Burst factor | α | Uniform(3, 10) |
| Burst duration | W | Pareto(α=2, x_m=2) seconds |
| Burst arrival rate | λ_on | 0.001 / stream / second |
| Burst recovery rate | λ_off | 0.1 / second |
| Per-stream buffer | m | 6 Mb (2 s @ 3 Mbps) |
| Emergency pool | Mₑ | 512 Gb (64 GB) |
| PI Kₚ | Kₚ | 0.5 / capacity |
| PI Kᵢ | Kᵢ | 0.1 / capacity |
| Controller frequency | f | 100 Hz |
| Max latency | D_max | 5 s |

## 10. References

1. Kleinrock, L. (1975). *Queueing Systems, Volume 1: Theory*. Wiley.
2. Heffes, H. & Lucantoni, D. (1986). "A Markov-modulated characterization of packetized voice and data traffic." *IEEE Trans. Communications*.
3. Ramamritham, K. et al. (2004). "Scheduling algorithms for multimedia systems." *IEEE Multimedia*.
4. Wiegand, T. et al. (2003). "Overview of the H.264/AVC video coding standard." *IEEE Trans. CSVT*.
5. Sullivan, G.J. et al. (2012). "Overview of the High Efficiency Video Coding (HEVC) standard." *IEEE Trans. CSVT*.
