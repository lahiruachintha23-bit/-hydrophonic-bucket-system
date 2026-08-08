#pragma once

// ---------------------------------------------------------------------------
// LOCAL SECRETS — gitignored, never committed.
//
// Pre-filled with the values that were previously hardcoded in src/main.cpp.
//
// >>> THREE VALUES BELOW STILL NEED FILLING IN (marked FILL_ME). <<<
//     Until then the device will connect to WiFi but fail Firebase auth.
//     See the checklist printed after setup for how to get them.
// ---------------------------------------------------------------------------

// ---- WiFi ----
// NOTE: this password was previously committed in src/main.cpp. If that ever
// reached a public repo, change the WiFi password and update it here.
#define WIFI_SSID          "Infinix NOTE 40"
#define WIFI_PASSWORD      "Achi@234"

// ---- Firebase ----
// Firebase console -> Project settings -> General -> "Web API Key".
// Safe to be public; the database rules are what actually protect you.
#define FIREBASE_API_KEY   "AIzaSyDjj5KLWLrV0kM7gP6eiRLNcxqssXuVThA"

#define FIREBASE_DB_URL    "https://hydrophonic-bucket-system-default-rtdb.asia-southeast1.firebasedatabase.app/"

// Firebase console -> Authentication -> Users -> Add user.
// Use the SAME email/password you will log into the dashboard with.
#define FIREBASE_USER_EMAIL     "lahiruachintha@gmail.com"
#define FIREBASE_USER_PASSWORD  "lahiruAdmin"

// ---- GSM alerts ----
#define GSM_PHONE_NUMBER   "+94740879724"
