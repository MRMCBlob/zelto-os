// android/os.js — BatteryManager, plus Handler/Looper, on the andemu route.
//
// BatteryManager reads the Zelto brokered battery keys (sys.battery_pct /
// sys.battery_charging). Handler/Looper is Android's main-thread post queue; a
// Zelto script app already runs on a single loop with a live setTimeout, so
// postDelayed IS setTimeout and post() is a zero-delay one — no threads involved.
import * as settings from "zelto/settings";

export class BatteryManager {
  // getIntProperty(id) — only CAPACITY is backed; anything else returns Android's
  // "unknown" (Integer.MIN_VALUE would be pedantic; -1 is the common sentinel).
  getIntProperty(id) {
    if (id === BatteryManager.BATTERY_PROPERTY_CAPACITY) {
      return settings.getInt("sys.battery_pct", -1);
    }
    return -1;
  }

  isCharging() {
    return settings.getInt("sys.battery_charging", 0) === 1;
  }

  // Convenience accessors (not stock Android, but ergonomic and harmless).
  get capacity() {
    return settings.getInt("sys.battery_pct", -1);
  }
}

BatteryManager.BATTERY_PROPERTY_CAPACITY = 4;
BatteryManager.BATTERY_STATUS_CHARGING = 2;
BatteryManager.BATTERY_STATUS_DISCHARGING = 3;

// Looper: on Android, the thread's message loop. The guest has exactly one loop,
// so getMainLooper()/myLooper() return the same token and Looper.loop() is a no-op
// (the runtime already pumps it).
export const Looper = {
  getMainLooper() {
    return { isCurrentThread: () => true };
  },
  myLooper() {
    return { isCurrentThread: () => true };
  },
  loop() {},
  prepare() {},
};

// Handler: posts Runnables (functions) onto the loop. postDelayed becomes
// setTimeout; the returned token lets removeCallbacks cancel a still-pending post,
// which is the one behaviour Android code actually relies on.
export class Handler {
  constructor(_looper) {
    this._timers = new Map();  // Runnable -> timeout id
  }

  post(runnable) {
    return this.postDelayed(runnable, 0);
  }

  postDelayed(runnable, delayMs) {
    if (typeof runnable !== "function") return false;
    const id = setTimeout(() => {
      this._timers.delete(runnable);
      runnable();
    }, Math.max(0, delayMs | 0));
    this._timers.set(runnable, id);
    return true;
  }

  removeCallbacks(runnable) {
    const id = this._timers.get(runnable);
    if (id !== undefined) {
      clearTimeout(id);
      this._timers.delete(runnable);
    }
  }
}
