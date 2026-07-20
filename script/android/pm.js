// android/pm.js — the permission-model bridge (PackageManager / ActivityCompat).
//
// Android permission strings map onto the Zelto broker's permission names, so an
// Android app's runtime-permission flow (checkSelfPermission / requestPermissions)
// goes through the SAME zsysd consent dialog a native Zelto app gets. An unmapped
// permission is treated as denied rather than throwing — the compat contract.
import * as permissions from "zelto/permissions";

// Android manifest permission -> Zelto broker permission.
const MAP = {
  "android.permission.ACCESS_FINE_LOCATION": "location",
  "android.permission.ACCESS_COARSE_LOCATION": "location",
  "android.permission.ACCESS_BACKGROUND_LOCATION": "location",
  "android.permission.BODY_SENSORS": "sensors",
  "android.permission.HIGH_SAMPLING_RATE_SENSORS": "sensors",
  "android.permission.ACTIVITY_RECOGNITION": "sensors",
  "android.permission.ACCESS_NETWORK_STATE": "network",
  "android.permission.INTERNET": "network",
  "android.permission.POST_NOTIFICATIONS": "notifications",
};

export const PERMISSION_GRANTED = 0;
export const PERMISSION_DENIED = -1;

// Resolve an Android permission (full "android.permission.X" or bare "X") to its
// Zelto name, or null when there is no mapping.
export function toZelto(perm) {
  if (!perm) return null;
  if (MAP[perm]) return MAP[perm];
  return MAP["android.permission." + perm] || null;
}

// PackageManager.PERMISSION_GRANTED / _DENIED semantics.
export function checkSelfPermission(perm) {
  const z = toZelto(perm);
  if (!z) return PERMISSION_DENIED;
  return permissions.status(z) === "granted"
    ? PERMISSION_GRANTED
    : PERMISSION_DENIED;
}

// ActivityCompat.requestPermissions(perms, callback?) -> Promise<int[]>. Each
// entry is PERMISSION_GRANTED/_DENIED. Never throws; an unmapped or denied
// permission just comes back denied.
export async function requestPermissions(perms, callback) {
  const list = Array.isArray(perms) ? perms : [perms];
  const results = [];
  for (const p of list) {
    const z = toZelto(p);
    let granted = false;
    if (z) {
      try {
        granted = await permissions.request(z);
      } catch (_e) {
        granted = false;
      }
    }
    results.push(granted ? PERMISSION_GRANTED : PERMISSION_DENIED);
  }
  if (typeof callback === "function") callback(list, results);
  return results;
}
