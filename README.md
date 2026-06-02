# openrgb_hue_patches
A collection of modifications to the Hue controllers in Open RGB to enhance control over hue lighting.

# OpenRGB 1.0rc2 — Philips Hue Patches

**Branch/tag patched:** `release_candidate_1.0rc2` (main, commit ~320420db)  
**Tested on:** Ubuntu 26.04, GNOME 50.1, Wayland, AMD Radeon RX 9070  
**Hue bridge:** Philips Hue Bridge v2 (firmware 1.xxx)  
**Entertainment areas:** Multiple (e.g. "Mike's pc", "Corrines Office", "Our Bedroom")

---

## Overview

Five bugs were fixed and two new features added to the Philips Hue Entertainment Mode support in OpenRGB. All changes are isolated to the Hue controller files and the main dialog. No other subsystems are touched.

---

## Files Changed

```
Controllers/PhilipsHueController/
  PhilipsHueEntertainmentController.h        ← mutex added
  PhilipsHueEntertainmentController.cpp      ← no-flash connect, state restore, mutex locks
  RGBController_PhilipsHue.cpp               ← DeviceUpdateLEDs made no-op
  RGBController_PhilipsHueEntertainment.h    ← user_enabled gate flag added
  RGBController_PhilipsHueEntertainment.cpp  ← rate limiter + user_enabled gate in DeviceUpdateMode

qt/OpenRGBDialog/
  OpenRGBDialog.h                            ← hueMenu member, removed actionHueSync + slot
  OpenRGBDialog.cpp                          ← hueMenu, threading fix, user_enabled gate, <thread> include
```

---

## Bug 1 — Hue controls ALL bridge lights on launch, turning them off

**File:** `Controllers/PhilipsHueController/RGBController_PhilipsHue.cpp`

**Symptom:**  
Every time OpenRGB launched, all Philips Hue bulbs connected to the bridge — including lights in other rooms (bedroom, living room) — would be switched off or set to black. This happened regardless of whether you were using the Hue entertainment feature.

**Root cause:**  
`RGBController_PhilipsHue` is the non-entertainment (plain REST API) controller that OpenRGB creates for every individual Hue light on the bridge. Its `DeviceUpdateLEDs()` was calling the REST API to set the colour on every profile load / device init, sending whatever was in the colour buffer (typically 0x000000 / black).

**Fix:**  
`DeviceUpdateLEDs()` and `DeviceUpdateMode()` are made no-ops. The entertainment-mode path (`PhilipsHueEntertainmentController`) handles actual communication; the plain Hue device controller should not be pushing colours independently.

```cpp
// BEFORE
void RGBController_PhilipsHue::DeviceUpdateLEDs()
{
    controller->SetColor(colors[0]);   // was sending to every bulb
}

// AFTER
void RGBController_PhilipsHue::DeviceUpdateLEDs()
{
    // intentional no-op — entertainment controller handles all updates
}
```

**Caveat:**  
If you use OpenRGB to manually set individual Hue bulb colours via the Devices tab (non-entertainment mode), this change means those manual selections will no longer be sent. For most users running entertainment mode this is not needed. If you need the manual REST-API path back, revert this change and configure your bridge to limit OpenRGB's scope to an entertainment area only.

---

## Bug 2 — Lights flash white/black for ~1 second when entertainment mode connects

**File:** `Controllers/PhilipsHueController/PhilipsHueEntertainmentController.cpp` — `Connect()`

**Symptom:**  
Every time entertainment mode connected (on OpenRGB launch, on profile switch, or after enabling via the tray menu), all Hue bulbs in the entertainment area would briefly flash white or black before settling into the correct colour.

**Root cause:**  
The Philips Hue DTLS entertainment stream starts in an unknown colour state. The first packet sent sets the colour, but there was no initial packet being sent — so the bridge's own default (white flash at full brightness) was displayed for the duration of the DTLS handshake setup.

**Fix:**  
Before calling `entertainment->connect()`, each light's current state (on/off, brightness, CIE XY colour) is read from the bridge via REST API and stored in `saved_states`. Immediately after the DTLS connection is established, the first packet sent over the stream reproduces those saved colours. From the user's perspective the lights never change colour on connect.

```cpp
// Connect() now:
// 1. Read all light states → saved_states
// 2. entertainment->connect() (DTLS handshake)
// 3. Immediately send a packet reproducing saved colours
// 4. Set connected = true
```

---

## Bug 3 — Lights left in entertainment mode after disconnect; previous state not restored

**File:** `Controllers/PhilipsHueController/PhilipsHueEntertainmentController.cpp` — `Disconnect()`

**Symptom:**  
When switching profiles or disconnecting entertainment mode, the Hue lights would stay stuck at whatever colour the stream last sent. They would not return to the colours/scenes they were showing before OpenRGB took over. The Hue app could not control them until the bridge timed out of entertainment mode (~10 seconds after the last DTLS packet).

**Root cause:**  
`Disconnect()` was calling `entertainment->disconnect()` (which exits DTLS and takes the bridge out of entertainment mode) but never telling the lights what to show next. They remained at their last streamed colour until the next Hue app command.

**Fix:**  
`Disconnect()` uses the `saved_states` gathered at connect time to restore each light to its previous state via the REST API using a 200ms transition time. On/off state, brightness, and CIE XY colour are all restored. The saved state is then cleared.

```cpp
// Disconnect() now:
// 1. entertainment->disconnect()
// 2. connected = false; delete entertainment
// 3. For each light: REST API → restore on/brightness/colour (transition 200ms)
// 4. saved_states.clear()
```

---

## Bug 4 — CPU spike to 20%+ when effects plugin is streaming to Hue

**File:** `Controllers/PhilipsHueController/RGBController_PhilipsHueEntertainment.cpp` — `DeviceUpdateLEDs()`

**Symptom:**  
When the OpenRGB effects plugin (e.g. Ambient screen sync) was active and driving the Hue entertainment area, CPU usage for the OpenRGB process climbed to a sustained 15–25%. The Hue DTLS protocol can accept up to ~60 packets per second, but the effects plugin was calling `DeviceUpdateLEDs()` far more frequently than that, spinning the CPU with redundant network sends.

**Fix:**  
A 40ms rate limiter (`std::chrono::steady_clock`) was added to `DeviceUpdateLEDs()`. Calls arriving faster than 40ms apart (>25 fps) are silently dropped. This matches the practical update rate of the Philips Hue DTLS stream and brings CPU usage back to near-zero when idle.

```cpp
void RGBController_PhilipsHueEntertainment::DeviceUpdateLEDs()
{
    auto now = std::chrono::steady_clock::now();
    if(std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_time).count() < 40)
    {
        return;   // ← rate limit: max ~25fps to Hue bridge
    }
    last_update_time = now;

    if(active_mode == 0)
    {
        controller->SetColor(&colors[0]);
    }
}
```

`last_update_time` is initialised to `now - 1000ms` in the constructor so the very first call always goes through.

---

## Bug 5 — Crash (SIGSEGV) when reconnecting entertainment mode after disconnect

**Files:**  
- `Controllers/PhilipsHueController/PhilipsHueEntertainmentController.h`  
- `Controllers/PhilipsHueController/PhilipsHueEntertainmentController.cpp`

**Symptom:**  
After connecting and then disconnecting entertainment mode via the tray menu, clicking "Direct" to reconnect would crash OpenRGB with a segmentation fault. The crash was not reproducible on first connect, only on reconnect.

**Root cause:**  
`Connect()` and `Disconnect()` are called from detached background threads (see Feature 1 below). `Disconnect()` restores all lights via REST API — with several lights this can take 500ms–2s. If the user clicked "Direct" again before `Disconnect()` had finished, a second background thread started calling `Connect()`, which also immediately begins REST API calls on the same `hueplusplus::Bridge` object. Two threads hammering a non-thread-safe object simultaneously → SIGSEGV.

The same race could also occur between `SetColor()` (called by the effects plugin up to 25fps) and `Connect()`/`Disconnect()`.

**Fix:**  
A `std::mutex connection_mutex` was added to `PhilipsHueEntertainmentController`. All three methods that touch `bridge`, `entertainment`, or `connected` acquire a `std::lock_guard` at entry:

- `Connect()` — holds the lock for its full duration (REST reads + DTLS setup)
- `Disconnect()` — holds the lock for its full duration (DTLS teardown + REST restores)
- `SetColor()` — holds the lock briefly while building and sending a DTLS packet

The practical effect: if a `Disconnect()` is still running its light-restoration REST calls and you click "Direct", `Connect()` will wait cleanly until `Disconnect()` finishes, then proceed. No torn state, no crash.

```cpp
// PhilipsHueEntertainmentController.h — added:
#include <mutex>
// ...
std::mutex connection_mutex;

// PhilipsHueEntertainmentController.cpp — added at top of each method:
std::lock_guard<std::mutex> lock(connection_mutex);
```

---

## Feature — Hue per-area tray menu (replaces broken single toggle)

**Files:**  
- `qt/OpenRGBDialog/OpenRGBDialog.h`  
- `qt/OpenRGBDialog/OpenRGBDialog.cpp`

**What was there before:**  
A single tray action "Connect Hue Sync" / "Disconnect Hue Sync" that toggled all entertainment areas together. This called `DeviceUpdateMode()` directly on the Qt GUI thread, which triggered the DTLS handshake (a blocking network operation) and caused the GUI to freeze or crash.

**What is there now:**  
A **Hue** submenu in the system tray that lists each entertainment area by name. Each area has its own submenu with **Direct** and **Disconnected** options. The current mode is shown with a checkmark. The menu is rebuilt dynamically every time the tray is opened, so it always reflects the live state.

```
System Tray →
  Hue ▶
    Mike's pc ▶
      ✓ Direct
        Disconnected
    Corrines Office ▶
        Direct
      ✓ Disconnected
    Our Bedroom ▶
        Direct
      ✓ Disconnected
```

**Threading fix:**  
`DeviceUpdateMode()` is no longer called on the GUI thread. `SetMode()` (a simple integer assignment) runs synchronously on the GUI thread to update the checkmark immediately. `DeviceUpdateMode()` (which triggers Connect/Disconnect and all associated network I/O) is dispatched to a detached `std::thread`:

```cpp
ctrl->SetMode(new_mode);
std::thread([ctrl]() { ctrl->DeviceUpdateMode(); }).detach();
```

**Changes in `OpenRGBDialog.h`:**
- Replaced `QAction* actionHueSync` with `QMenu* hueMenu`
- Removed `void on_HueSyncToggle()` private slot

**Changes in `OpenRGBDialog.cpp`:**
- Added `#include <thread>` (was being pulled in transitively; now explicit)
- Replaced the `actionHueSync` single-action setup with `hueMenu = new QMenu(tr("Hue"), this)`
- The `trayIconMenu::aboutToShow` lambda now clears and rebuilds `hueMenu` dynamically on every open
- Removed `on_HueSyncToggle()` function body

---

## Feature 2 — Tray menu is the sole gatekeeper for Hue connection (autoregister-safe)

**Files:**
- `Controllers/PhilipsHueController/RGBController_PhilipsHueEntertainment.h`
- `Controllers/PhilipsHueController/RGBController_PhilipsHueEntertainment.cpp`
- `qt/OpenRGBDialog/OpenRGBDialog.cpp`

**The problem this solves:**  
OpenRGB's Visual Map plugin, when set to `autoregister`, calls `ForceDirectMode()` on every controller it registers, which chains through to `DeviceUpdateMode()`. For the Hue entertainment controller this means the DTLS connection is opened automatically as soon as the Visual Map loads — before the user has explicitly asked for it. This causes the entertainment area lights to be taken over on every launch even if the user only wanted them synced on demand.

The previous workaround was to leave `autoregister` **off** in Visual Map and manually click Register, then tick the area in the Effects plugin, every time you wanted to use Hue. Clunky.

**The fix:**  
A `bool user_enabled` flag (default `false`) was added to `RGBController_PhilipsHueEntertainment`. `DeviceUpdateMode()` now checks this flag whenever anything asks it to switch to Direct mode (mode 0). If `user_enabled` is false, it silently reverts `active_mode` back to 1 (Disconnected) and returns without connecting:

```cpp
void RGBController_PhilipsHueEntertainment::DeviceUpdateMode()
{
    if(active_mode == 0)
    {
        if(!user_enabled)
        {
            active_mode = 1;   // ← gate: not user-approved, stay disconnected
            return;
        }
        // ... normal Connect() path
    }
    else
    {
        controller->Disconnect();
    }
}
```

The tray menu lambdas set `user_enabled` via a `static_cast` before firing the background thread:

```cpp
// Direct clicked:
static_cast<RGBController_PhilipsHueEntertainment*>(ctrl)->user_enabled = true;
ctrl->SetMode(0);
std::thread([ctrl]() { ctrl->DeviceUpdateMode(); }).detach();

// Disconnected clicked:
static_cast<RGBController_PhilipsHueEntertainment*>(ctrl)->user_enabled = false;
ctrl->SetMode(1);
std::thread([ctrl]() { ctrl->DeviceUpdateMode(); }).detach();
```

**Result — recommended workflow with this fix:**

1. In Visual Map, set `autoregister` **on** for your Hue entertainment area and position your lights on the map. Save.
2. In the Effects plugin, enable the Ambient (or any) effect for the Hue area. Save your effects profile.
3. On every subsequent launch, Visual Map auto-registers and Effects starts running — but because `user_enabled = false`, zero DTLS packets are sent. Your other room lights are untouched. Hue is completely silent.
4. When you want screen sync: **Tray → Hue → [area name] → Direct**. Connects instantly, starts streaming.
5. When done: **Tray → Hue → [area name] → Disconnected**. Lights restored to pre-sync state. Even if Visual Map now tries to force Direct again (e.g. on a rescan), the gate blocks it.

No manual zone loading, no Effects tab tickboxing needed after initial setup.

**Note:** `user_enabled` is an in-session flag — it resets to `false` on every OpenRGB restart. This is intentional: the safe default is "do not touch Hue lights unless I explicitly ask". If you want Hue to auto-connect on every launch, that would require a SettingsManager persistence step (not implemented here).

---

## How to apply these patches to a clean OpenRGB source tree

1. Copy the 7 files from this folder into the matching paths in your OpenRGB source tree, replacing the originals.
2. From the source root, rebuild:
   ```bash
   qmake OpenRGB.pro
   make -j$(nproc)
   ```
3. The binary is `./openrgb` in the source root.

No changes to `.pro` files, CMake, or any other build system file are required. All changes are purely C++ and Qt.

---

## Known limitations / caveats

- **Visual Map + Hue (autoregister):** With Feature 2 applied, it is now safe to enable `autoregister` in Visual Map for the Hue entertainment area. The `user_enabled` gate will silently block the auto-connect. Position your lights in Visual Map with autoregister ON — they will load correctly on every launch without touching the bridge.

- **Whole-house bridge scope:** OpenRGB exposes every light connected to the bridge, not just your office/room. The `DeviceUpdateLEDs()` no-op in `RGBController_PhilipsHue` prevents the plain controller from touching bedroom/living room lights. Entertainment mode is scoped to whichever entertainment area you configured in the Hue app — configure your entertainment areas carefully in the Philips Hue app before using OpenRGB.

- **`user_enabled` resets on restart:** The gate flag is in-session only. On every OpenRGB launch, Hue starts in Disconnected state regardless of what it was doing when you last closed it. Click Direct from the tray when you want to re-enable. This is a deliberate safety default.

- **Reconnect timing:** After clicking Disconnect, the bridge takes up to 10 seconds to fully exit entertainment mode. The mutex ensures `Connect()` will wait for `Disconnect()` to finish its REST restore calls before attempting to reconnect. Clicking Direct immediately after Disconnect is safe — it will queue up cleanly behind the cleanup.

- **`SetColor()` blocks during connect/disconnect:** The effects plugin's update thread will pause briefly (for the duration of the connect/disconnect handshake) while the mutex is held. This typically manifests as the Hue lights pausing for 1–3 seconds during the transition, which is expected and harmless.

---

## Testing performed

- Launch OpenRGB → Hue lights not disturbed (Bug 1 fix confirmed)
- Switch to Hue effects profile → connect via tray → no white flash on connect (Bug 2 fix confirmed)
- Disconnect via tray → lights return to previous state (Bug 3 fix confirmed)
- Run Ambient screen sync → CPU stays at ~1–2% (Bug 4 fix confirmed)
- Connect → Disconnect → immediately click Direct again → no crash (Bug 5 fix confirmed)
- Tray menu shows correct checkmarks reflecting live mode state (Feature 1 confirmed)
- Multiple entertainment areas each appear as separate submenus (Feature 1 confirmed)
- Visual Map autoregister ON → launch → Hue lights untouched, no auto-connect (Feature 2 confirmed)
- Tray → Direct → DTLS connects instantly without manual zone loading (Feature 2 confirmed)
- Tray → Disconnected → lights restored; Visual Map re-registration attempt blocked by gate (Feature 2 confirmed)
