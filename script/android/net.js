// android/net.js — the ConnectivityManager / WifiManager surface of the andemu
// route, translated onto the Zelto brokered network state (sys.wifi/sys.airplane).
import * as settings from "zelto/settings";

export const TYPE_WIFI = 1;
export const TYPE_MOBILE = 0;

function connected() {
  const airplane = settings.getInt("sys.airplane", 0) === 1;
  const wifi = settings.getInt("sys.wifi", 1) === 1;
  return !airplane && wifi;
}

export class ConnectivityManager {
  // getActiveNetworkInfo() -> a NetworkInfo-like object (deprecated in Android but
  // still widely used); getNetworkCapabilities-era code reads isConnected too.
  getActiveNetworkInfo() {
    const isUp = connected();
    return {
      isConnected: () => isUp,
      isConnectedOrConnecting: () => isUp,
      getType: () => TYPE_WIFI,
      getTypeName: () => "WIFI",
    };
  }

  isActiveNetworkMetered() {
    return false;
  }
}

export class WifiManager {
  isWifiEnabled() {
    return settings.getInt("sys.wifi", 1) === 1;
  }

  getConnectionInfo() {
    return {
      getSSID: () => '"zelto-sim"',
      getNetworkId: () => (this.isWifiEnabled() ? 0 : -1),
    };
  }
}
