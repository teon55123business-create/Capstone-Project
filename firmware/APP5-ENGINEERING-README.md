# App 5 — dual-core IPC pipeline (Space theme)

- Theme: Space — AOCS sample → attitude update → downlink packet.

# System Architecture
My pipeline: an aocs_sensor task produces simulated gyro samples at 20 Hz, an attitude_update task consumes them and checks the rate against a deadband, and a coordinator
waits for both halves of each cycle before waking the downlink task to emit a telemetry packet. The button on GPIO18 acts as a ground-station "send telemetry now" command
and wakes downlink directly from its ISR. Everything real-time runs on Core 1; the monitor (serial or web, picked by USE_WEBSERVER) runs on Core 0 and shows the same
fields either way: queue depth, last sample, event bits, and the four heartbeats.

# Three Core-1 tasks using three IPC primitives

Queue (aocs_sensor → attitude_update):
    samples are actual data — a timestamp and a value that need to arrive in order. The queue copies by value, keeps FIFO order, and
    buffers bursts.

Event group (producer + consumer → coordinator):
    the coordinator has to wait for "sample taken" AND "state updated" at the same time, which is exactly what one atomic
    xEventGroupWaitBits call does. See analysis #3 for why two semaphores would be worse here.

Direct task notification (coordinator → downlink, and button ISR → downlink):
    pure 1-to-1 wake-up with no payload. No kernel object to create, works from an ISR, and it's the fastest option (measured in #4).

# Queue contract
aocs_sample_t { uint32_t timestamp_ms; int value; } — 8 bytes, copied by value, depth 8. The producer sends with a 10 ms timeout and drops + logs on failure; the
consumer blocks up to 500 ms and warns if nothing arrives (that's 10 producer periods, so a timeout means the sensor is actually stuck).

# Web monitor / terminal monitor (Core 0)

Both monitors show the required fields: queue depth + last item, event-group bit values, and the per-task heartbeats (prod/cons/coord/resp).

- USE_WEBSERVER 0 — serial monitor, no Wi-Fi. One status line per second plus [downlink] packet #N lines (logged every 20th so the serial
  port doesn't slow the Wokwi sim).
- USE_WEBSERVER 1 — web monitor. Connects to Wokwi-GUEST, serial prints "Got IP", then Wokwi's network indicator opens the page. It's my
  App 1 pattern — an HTML shell plus a /state JSON endpoint polled by JS — slowed from 4 Hz to 1 Hz so the handler stays out of the
  latency measurements.

# Button as event source

Button (GPIO18, internal pull-up, falling edge) → button_isr → vTaskNotifyGiveFromISR(downlink) → portYIELD_FROM_ISR. Each press emits one
downlink packet and bumps hb_resp by exactly one on the monitor line. The ISR keeps a 200 µs debounce gate — sufficient because the Wokwi
button has "bounce": "0" set in diagram.json; see the note under analysis #4 for why real hardware needs a ms-scale gate.

## Engineering analysis prompts

1. Why pin the web server to Core 0? What goes wrong if it's on Core 1?

    I kept the scaffold's choice (my App 1 already set core_id = 0 too). ESP-IDF's Wi-Fi and lwIP stacks live on Core 0, so the HTTP handler stays next to the network stack
    it uses. The bigger reason: HTTP work is bursty and unpredictable — a request costs milliseconds of TCP handling and string formatting at random times. On Core 1 that would
    inject jitter into the 20 Hz pipeline, and a slow client could stall the consumer long enough to fill the queue. The monitor would be causing the drops it reports.
    Separate cores keep observation from disturbing the thing being observed.

2. Queue depth — how sized? The burst I protect against

    Producer: 20 Hz, one 8-byte sample every 50 ms. The consumer normally finishes in well under 50 ms, but it can be held off by the coordinator (prio 9), downlink (prio 12),
    and log bursts — I budget 400 ms for the worst stall, deliberately pessimistic. Burst to absorb: 20 Hz * 0.4 s = 8 samples, so depth 8 (64 bytes total). In practice the
    monitor shows q_depth at 0 the whole run, so the depth really is just burst headroom. Back-pressure policy — log + drop newest: if the send still fails after 10 ms, the
    sample is dropped and logged. Blocking the producer would distort the 20 Hz sampling cadence, and dropping silently would hide the loss. Dropping the newest is fine for
    this theme: gyro rates change smoothly, the queued samples are at most 400 ms old, and the next sample comes 50 ms later. (For the Medical theme the answer flips — you
    don't drop ECG samples.)

3. Event group vs N semaphores — when is each better?

    The coordinator needs "wait until both bits are set." An event group does that in one atomic blocking call with clear-on-exit, so no cycle can be double-counted.
    With two binary semaphores you take them one after the other: while blocked on B you've already consumed A, the ordering is baked into the code, and there's no atomic
    clear-both. Adding a pipeline stage later is one more bit instead of another handle. Semaphores still win when you need counting (event bits are binary — five sets before
    a wait look like one, which is exactly why the data rides in the queue), simple 1-to-1 signaling, or a mutex with priority inheritance. Event groups are also capped at 24
    bits and wake-up costs more because the kernel re-checks every waiter's condition.

4. Direct notification vs binary semaphore — measured wakeup latencies

    Method (App 3 pattern): esp_timer_get_time() right before the give/notify and again right after the take returns in downlink; track count, average, and max on the monitor
    line. Task-path runs were hands-off with the full pipeline live; for the ISR path I disabled the coordinator's signal so the button was the only wake source, 50+ presses
    each. Validity check on every run: resp == coord for the task path, one sample per press for the ISR path.

  - Measured on Wokwi:

| **Path**    | **Notification**                 | **Semaphore**                    |
| ----------- | -------------------------------- | -------------------------------- |
| Task → Task | 35 µs avg / 36 µs max (n = 1661) | 41 µs avg / 42 µs max (n = 1921) |
| ISR → Task  | 16 µs avg / 17 µs max (n = 56)   | 17 µs avg / 18 µs max (n = 61)   |

    Notification performed better in both tests. I measured about a 15% improvement for the task-to-task path instead of the ~45% reported by FreeRTOS because my results
    include the full context switch, not just the unblock operation. The ISR-to-task path was also faster since Core 1 was nearly idle. A consistent ~1 µs gap between
    average and maximum latency was a good sanity check; much larger gaps indicated contaminated runs.

  One issue was Wokwi's default button bounce. Without "bounce": "0" in diagram.json, a single press generated multiple wake-ups and inflated the maximum latency.
   For testing, I disabled bounce and temporarily increased the ISR gate to 20 ms. The submitted code keeps the 200 µs gate, which works in simulation but would
   need to be several milliseconds on real hardware to properly debounce a mechanical switch.

# Concurrency Diagram

```
                 Core 1                         Core 0
        ┌──────────────────┐          ┌──────────────────┐
        │                  │          │                  │
        │  aocs_sensor     │          │ Monitor / Web    │
        │  Priority 8      │          │ Priority 4       │
        │                  │          │                  │
        └────────┬─────────┘          └──────────────────┘
             │       │                         │
  sends data │       │ sets EVENT BIT 0        │ reads status
             ▼       │ (produced)              ▼
        ┌──────────┐ │                ┌──────────────────┐
        │          │ │                │  Queue depth     │
        │  data_q  │ │                │  Last sample     │
        │ 8 x samp │ │                │  Event bits      │
        │          │ │                │  Heartbeats      │
        └────┬─────┘ │                └──────────────────┘
             │       │
receives data│       │
             ▼       │
        ┌──────────────────┐
        │                  │
        │ attitude_update  │
        │ Priority 8       │
        │                  │
        └────────┬─────────┘
                 │
                 │ sets EVENT BIT 1
                 │ (processed)
                 ▼
        ┌──────────────────┐
        │ EVENT GROUP      │
        │                  │
        │ Bit 0: Produced  │
        │ Bit 1: Processed │
        │ Clear on exit    │
        └────────┬─────────┘
                 │
                 │ waits for BOTH bits
                 ▼
        ┌──────────────────┐
        │                  │
        │  coordinator     │
        │  Priority 9      │
        │                  │
        └────────┬─────────┘
                 │
                 │ task notification
                 ▼
        ┌──────────────────┐
        │                  │
        │   downlink       │
        │   Priority 12    │
        │                  │
        └──────────────────┘
                 ▲
                 │
                 │ vTaskNotifyGiveFromISR()
                 │
        ┌──────────────────┐
        │                  │
        │ Button ISR       │
        │ GPIO18           │
        │                  │
        └──────────────────┘

        ┌─────────────────────────────────────┐
        │ Shared state (lock-free 32-bit)     │
        │ - Heartbeats (prod/cons/coord/resp) │
        │ - Last sample (timestamp, value)    │
        │ - Queue depth & event bits via API  │
        └─────────────────────────────────────┘
```

## Honor code

All reused infrastructure from Apps 1–3 is cited below, block by block.

## Citations

- Course scaffold: task skeletons, event-group/notification wiring, button ISR, heartbeats, serial monitor, USE_WEBSERVER switch (the scaffold header notes its own AI-assisted parts).
- My App 1 (Satellite health beacon): wifi_init_sta(), wifi_event_handler(), start_webserver(), and the /state JSON + fetch polling page, ported into the USE_WEBSERVER=1 block.
- App 3: the latency measurement pattern used in analysis prompt 4.
- AI assistance (Claude): producer/consumer bodies, queue sizing rationale, the App 1 port, and drafting this README — all reviewed and verified by me.
