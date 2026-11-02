# Blue Star "smart ready" AC: Wi-Fi port protocol

Reverse-engineered on a Blue Star **IE518TNUR** cooling-only inverter split (indoor display board Megmeet **MD50BS**) with an ESP32 on the
AC's empty Wi-Fi port, acting as the Tuya Wi-Fi module and logging everything, then confirmed by a week of
continuous logging on that unit and a second model, **IA318MXU** (same display board).

Status markers used below: ✅ confirmed · 🟡 strong hypothesis · ❓ unknown.

---

## 1. Physical layer

| Item | Value |
|---|---|
| Connector | 4-pin JST-XH (2.5 mm) header on the indoor display board, labelled `R_WIFI T_WIFI GND 5V` |
| Label perspective | **module** side: `R_WIFI` = the AC transmits, `T_WIFI` = the AC receives |
| Serial | 9600 baud, 8N1 |
| Levels | AC TX idles at **5 V**; the AC's RX input floats at ~1.5 V with no module attached |
| Power | `5V` pin powers the add-on module; it stays on while the AC is in standby and goes off with mains |

## 2. Transport: Tuya MCU serial ✅

Standard Tuya MCU framing: `55 AA ver cmd lenH lenL data… sum` (sum = byte sum mod 256).
Module frames use version `0x00`, the AC's MCU answers with `0x03`.

* Product query reply (IE518TNUR): `{"p":"krxpotecxqamrq8p","v":"1.0.0","m":1}`
* The MCU **ignores heartbeats until it has seen the full init**:
  `0x01 product query → 0x02 working mode → 0x03 network status (value 0x04 = connected to cloud) → 0x08 query`.
  The stock ESPHome `tuya:` component stalls here, which is why this project ships its own component.
* After init the MCU is **push-only**: it reports changes with cmd `0x07` (DP report). `0x08` gets no reply.
* Heartbeat (`0x00`) every 15 s keeps the link up. A heartbeat answer of `00` means the MCU restarted:
  run the init again.
* Leave **≥ 400 ms between frames** you send; back-to-back frames get dropped.

### Data points

| DP | Type | Meaning |
|---|---|---|
| 1 | bool | power |
| 2 | value | set temperature, whole °C |
| **101** | string | **Blue Star key/value record**, carries everything below |

Writes use Tuya cmd `0x06` (DP set). The MCU echoes every accepted write, and **the AC beeps on every write**.

## 3. DP101 record format ✅

```
*LLKKV…CCCC#
```

* `LL`: hex count of characters in `KK` + `V`
* `KK`: key, 2 hex digits
* `V`: value, hex, length depends on key
* `CCCC`: **CRC-16/MODBUS** (poly 0x8005 reflected, init 0xFFFF) over the ASCII `*LLKKV…`, uppercase hex

Examples (verified against captured frames):

| Record | Meaning |
|---|---|
| `*03151DACF#` | key `15` (display light) = `1` |
| `*031501A0E#` | key `15` = `0` |
| `*05021046EFD#` | key `02` (setpoint) = `104` → 26.0 °C |

To **read** a key that the MCU does not push, write a dummy value: the MCU answers with the real value instead
of echoing (worked for keys 24, 25, 29, 61, 62). Writing key `00` = `0` returns a full **state dump** (§5).

## 4. Keys

### Controls ✅

| Key | Values | Meaning |
|---|---|---|
| 01 | 0 / 1 | power |
| 02 | 3 hex = °C × 10 | setpoint (`104` = 26.0, `0A0` = 16.0). Half degrees work (`109` = 26.5). Range 16–30. |
| 03 | 0 fan · 2 cool · 3 dry · 4 auto ("AI Pro") | mode. Dry and auto force setpoint 24. `1` (heat) was accepted on the cooling-only IE518TNUR; its effect is unknown. |
| 04 | 1 quiet · 2 · 3 · 4 · 6 turbo · 7 auto | fan. **1** is a quieter step than the remote's lowest; **0** stops the indoor fan (don't use while cooling: the coil can ice). |
| 05 | 0 swing · 1 top … 5 bottom · 6 off | vertical vane |
| 06 | 0 swing · 6 off | horizontal swing |
| 07 | 3 on · 0 off | Turbo ("110 % boost"): forces setpoint 16 and fan 6, **cancels itself after ~17–20 min** |
| 11 | 1 / 0 | Comfort Sleep: raises the real target +1 °C/h for 2 h **internally**; key 02 never shows it |
| 12 | 1 / 0 | temperature unit °F (setpoint is then re-sent as °F × 10) |
| 13 | 0 off · 1 Autumn · 2 Summer · 3 Monsoon | "Climate"/season mode (remote Mode+Eco) |
| 15 | 1 / 0 | display light |
| 22 | 1 / 0 | energy saver / Eco (forces setpoint 24) |
| 23 | `1`/`2` + packed settings | "Dual user preference" profiles |
| 55 | 0 normal · 1 100 % · 2 80 % · 3 60 % · 4 40 % | convertible capacity (also caps the fan) |
| 58 | 1 muted · 0 | buzzer mute (remote: hold Display 5 s). Every remote press sends `58=0`. |

### Timers ✅

| Key | Meaning |
|---|---|
| 08 | flags: 0 none · 1 on-timer · 2 off-timer · 3 both |
| 09 | on-timer, 4 hex, **minutes from now** |
| 10 | off-timer, 4 hex, minutes from now |

The AC has **no clock**: the remote converts clock times into relative minutes. Pressing Sleep 3× on the
remote clears both timers.

### Status / counters

| Key | Status | Meaning |
|---|---|---|
| 99 | ✅ | room temperature, 3 hex = °C × 10, pushed every 60 s |
| 44 | ✅ | **AC on-time** counter, +1 per 15 min while the AC is on; survives power cuts |
| 43 | ✅ | **compressor run-time** counter. Units differ by model: **30 min** per tick on IE518TNUR, **15 min** on IA318MXU |
| 59 | ✅ | 8-hex copy of the k43 counter |
| 60 | 🟡 | 8-hex counter that steps with k43 at a fixed offset (k59 − 2 or −3 on IE518TNUR, k59 − 0x2E on IA318MXU) |
| 24 | 🟡 | `[A][B][C]`, latched at every k43 tick: **A = compressor speed** (behaves like inverter Hz: IE518TNUR mostly 38, 45 under load, 55 only in turbo; IA318MXU 56 → 52 → 47 while pulling down, then 32 = its floor). B = a counter tied to k43 (IE518TNUR: k43 − 0x21; IA318MXU: the low byte of k43). C = 0 on IE518TNUR; on IA318MXU a step value 6 → 5 → 4 → 1 that falls with speed and later at night (outdoor-fan stage?) |
| 25 | ❓ | one byte sent with each k43 tick. Not a counter, not power, no clean relation to k24 A |
| 62 | ❓ | a temperature (`0EB` = 23.5 °C), read on request only. It read 23.5 °C every time, including overnight and while it was 25.8 °C outside, so it is **not** outdoor ambient |
| 29 | 🟡 | live effective settings in profile format |
| 61 | ❓ | `01BBDDDD`, constant (ID?) |
| 17, 30 | ❓ | echo as `1000` (structured) |
| 14, 16, 18–21, 31, 32, 38 | ❓ | accept writes silently, no visible effect |
| 28, 45, 46 | ❓ | reject value 1 |

**Compressor speed cannot be read live.** Asking for key 24 between ticks (writing `24=0`) returns the value
latched at the last tick: a unit in turbo for 10 minutes still answered with its pre-turbo value. So the
dashboard can only show a speed once per 15/30 minutes of compressor running.

**No energy metering.** No field behaves like an energy or power counter. The Blue Star app's kWh figures are
described as "indicative", so they are probably estimated from run-time. Use a plug or CT meter if you need
real energy.

## 5. Key-00 state dump

Write `00=0` → the MCU answers with one long record. IE518TNUR sends 94 characters, IA318MXU 111.

| Position | Meaning |
|---|---|
| 0 | k01 power |
| 1–3 | k02 setpoint |
| 4 | k03 mode |
| 5 | k04 fan |
| 6 | k05 vane |
| 7 | k06 side swing |
| 9 | k08 timer flags |
| 10–13 | k09 on-timer |
| 14–17 | k10 off-timer |
| 23 | k15 display light |
| 31–33 | room temperature (k99) |
| 39–42 | k43 compressor counter |
| 43–50 | k44 on-time counter |
| 86–89 | k60 |
| **103** (IA318MXU only) | **filter-clean ("FC") alert flag**: `1` while the display shows FC, `0` after reset |

Example (IE518TNUR): `11042216000000000000000100000000FF000000021000000390031170000040001000001001000000000000210000`

The remaining positions are mostly constant; the extra 17 characters on IA318MXU are unmapped apart from 103.

## 6. Behaviour worth knowing

* **Power-on burst:** when the AC is switched on it pushes keys 01, 02, 11, 15, 43, 59.
* **After a mains cut the AC resumes** its previous state (auto-restart). The add-on loses power with the AC,
  reboots when mains returns and re-reads the full state, so Home Assistant shows the restored settings.
* **Filter clean (FC):** shown after 300 h of running; cleared by pressing the remote's vertical-swing
  button 3× within 5 s, or automatically after 20 h. The IA318MXU dump exposes it (position 103).
* **Remote-only:** Mode + Light (show room temperature) and setting the remote's clock send nothing.
* **Module reset glitch:** while the module's TX pin floats during a reboot, the MCU reads garbage and silently
  turns the display light off (it does not report this). A pull-up on that line fixes it; the firmware also
  restores the light after boot.

## 7. Tools

- `tools/tuya_send.py` builds raw Tuya frames and DP101 records (with CRC) to inject through the firmware's
  `send_frame` / `send_record` actions.
- `tools/ac_events.py` turns long-term ESP logs into a per-counter-tick table (k24, k25, room temperature,
  setpoint, fan, turbo, sleep).
- `tools/ac_syslog.py` is a minimal receiver for the firmware's optional `syslog:` output.
