/*
  WiFiPortal.h — multi-network WiFi store, non-blocking connect state machine,
  and an AP captive portal for first-time setup.

  Up to NET_MAX_CREDS SSID/password pairs live in NVS. When the WiFi CAT
  transport is switched on the state machine walks the saved list; if the list is
  empty, or nothing on it answers, it raises a soft AP with a captive portal so
  credentials can be entered from a phone or laptop.

  Nothing here blocks: netPoll() must be called from loop() and returns
  immediately. Scans are asynchronous.
*/

#ifndef WIFIPORTAL_H
#define WIFIPORTAL_H

#include <Arduino.h>

#define NET_MAX_CREDS     5
#define NET_AP_SSID       "T-Embed-CAT"
#define NET_DEFAULT_PORT  1234

enum NetState
{
  NET_OFF = 0,      // transport disabled, radio silent
  NET_CONNECTING,   // trying a saved network
  NET_CONNECTED,    // station up, CAT TCP server listening
  NET_PORTAL        // soft AP + captive portal running
};

void netBegin();
void netPoll();

bool netEnabled();             // the user's persisted preference
void netSetEnabled(bool en);   // persisted

// Suspend powers the WiFi hardware down without touching the stored preference, so
// it comes back by itself. Used while a USB host is attached: tethered, the radio
// does not need WiFi for control, and the RF is better off quiet.
void netSetSuspended(bool sus);
bool netSuspended();

NetState netGetState();

// Human-readable one-liners for the radio's screen.
String netStatusShort();       // "192.168.1.42", "connecting", "AP setup", "off"
String netStatusDetail();

String   netIP();
String   netApIP();            // soft-AP address, for the setup banner
String   netSSID();
uint8_t  netCredCount();
uint16_t netGetCatPort();

void netStartPortal();         // force the setup portal up
void netOpenSetup();           // portal for configuration only: does not enable the
                               // transport, and powers WiFi back down on close
void netStopPortal();
bool netPortalActive();
bool netRadioOn();             // is the WiFi hardware powered at all?

#endif
