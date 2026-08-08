#pragma once

// ---------------------------------------------------------------------------
// TEMPLATE — this file IS committed to git. Do not put real secrets here.
//
// Setup:
//   1. Copy this file to  include/secrets.h
//   2. Fill in your real values there
//   3. include/secrets.h is gitignored and will never be committed
// ---------------------------------------------------------------------------

// ---- WiFi ----
#define WIFI_SSID          "your-wifi-name"
#define WIFI_PASSWORD      "your-wifi-password"

// ---- Firebase ----
// Realtime Database URL, including the trailing slash.
//
// The database rules are open (public read/write), so the device does not sign
// in and needs no API key or Auth account. That also means this URL is the only
// thing between a stranger and your pumps — it ships in the public dashboard
// bundle, so anyone who reads the page source can control them. If you ever
// want that closed off, put the rules back to "auth != null", restore the
// email/password fields here, and remove signer.test_mode in setupFirebase().
#define FIREBASE_DB_URL    "https://your-project-default-rtdb.asia-southeast1.firebasedatabase.app/"

// ---- GSM alerts ----
#define GSM_PHONE_NUMBER   "+10000000000"
