# SAT-1 — Task table & WCET evidence

Measured with App 3's WCET-max macro wrapped around each task body (throwaway
instrumented build; submitted firmware is unmodified App 5). Maxima captured over a
multi-minute run including button presses so the responder's logging path was exercised.

| Task            | Period T             | WCET C (measured max) | U = C/T  | Priority | Deadline                    |
|-----------------|---------------------:|----------------------:|---------:|---------:|-----------------------------|
| aocs_sensor     | 50 ms                | 33 µs          | 0.0007   | 8        | next period (50 ms)         |
| attitude_update | 50 ms (event-driven) | 2011 µs          | 0.0402   | 8        | before queue fills (400 ms) |
| coordinator     | 50 ms (event-driven) | 2907 µs         | 0.0581  | 9        | next cycle                  |
| downlink        | 50 ms (event-driven) | 2855 µs          | 0.0571   | 12       | next cycle                  |
| monitor (Core 0)| 1000 ms              | — (non-critical)      | —        | 4        | soft                        |

Total utilization U = 0.156.
Rate-monotonic bound for n=4 tasks: 4(2^(1/4) − 1) ≈ 0.757. EDF bound: 1.0.
U ≪ 0.757, so the set is schedulable by RM with large margin — held deliberately as
headroom for the UART logging bursts that the 400 ms consumer-stall budget anticipates.

Notes:
- Measured maxima are dominated by ESP_LOGI (vprintf + UART), the costliest work on
  Core 1 — the same effect the 400 ms consumer-stall budget was sized against.
- The coordinator's figure includes preemption: its notify wakes the higher-priority
  downlink task, which runs (and logs) before the coordinator's end-timestamp is
  taken. The measured value is therefore execution + preemption — a conservative
  over-estimate of C, which is the safe direction for the schedulability claim.
- The three event-driven tasks inherit the producer's 20 Hz cadence, so 50 ms is their
  effective period for utilization purposes.
- The consumer's real deadline is the queue-overflow horizon: depth 8 at 20 Hz = 400 ms.
- Wokwi simulates the CPU; absolute µs differ from silicon, but the utilization
  conclusion is robust to that (C would need to grow ~100× to threaten the bound).
