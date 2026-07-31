# SAT-1 — Hazard analysis & standard mapping

Severity categories follow a MIL-STD-882E-style scale (I catastrophic … IV negligible),
assessed for the system's mission context (telemetry availability). Each software
mitigation is mapped to the NASA software-safety practice it implements
(NASA-GB-8719.13 Software Safety Guidebook themes).

| ID | Hazard | Cause | Effect | Detection (in code) | Mitigation (in code) | Sev. | Practice mapping |
|----|--------|-------|--------|---------------------|----------------------|------|------------------|
| H1 | Sensor task stall | Task wedged / starved | Stale attitude data | Consumer 500 ms receive timeout warns; hb_prod frozen on 1 Hz monitor | Warning annunciated; last-known values held; rest of pipeline unaffected | III | Fault detection & annunciation |
| H2 | Processing stall → queue overflow | Downstream latency exceeds budget | Sample loss | Live queue depth on monitor; each drop logged with running count | Queue bounded by analysis (20 Hz × 400 ms = 8); log-and-drop keeps 20 Hz sampling cadence; loss visible, never silent | III | Resource margins; overload management |
| H3 | Downlink automation failure | Coordinator signal lost | Loss of telemetry | hb_resp diverges from hb_coord within 1 s on the monitor | Independent ground-command path: button ISR → direct task notification → downlink (shares nothing with the failed path) | II | Independent backup command path |
| H4 | Spurious command edges | Switch bounce / EMI | Command flood, wasted downlinks | Wake count vs. press count (validated during test) | ISR debounce gate; ms-scale gate specified for real hardware | IV | Input signal conditioning |
| H5 | Monitor / network failure | Wi-Fi loss, HTTP fault | Loss of observability only | Page stops refreshing; serial monitor fallback | Monitor isolated on Core 0 with lock-free reads — cannot perturb the Core 1 real-time plane | IV | Partitioning / isolation |
| H6 | Startup ordering race | Signaler ran before target task existed | Boot-loop crash (assert) | Found in test: xTaskGenericNotify assert on NULL handle | Creation order guarantees the responder handle is valid before any signaler can run | II | Initialization ordering — found & fixed in test |

Demonstrated in the demo video: H3 is injected live (coordinator notification severed),
detection shown via heartbeat divergence, degraded-mode telemetry shown via the manual
command path, full recovery on rebuild.
