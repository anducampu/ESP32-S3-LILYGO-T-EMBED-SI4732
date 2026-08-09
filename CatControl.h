/*
  CatControl.h — Kenwood TS-480 style CAT control for the T-Embed SI4732 radio.

  Two independent transports, each switchable from the radio menu:
    * USB CDC serial  (shares the USB port with the boot log — see catUsbOwnsStream)
    * TCP socket over WiFi (transport enable lives in WiFiPortal / netSetEnabled)

  Protocol notes
  --------------
  The command subset and, in particular, the exact byte layout of the "IF;"
  answer were taken from Hamlib's own Kenwood backend (rigs/kenwood/kenwood.c),
  so what we emit is what Hamlib actually parses:

      offset len  field
      0      2    "IF"
      2      11   P1  frequency, Hz
      13     4    P2  frequency step
      17     6    P3  RIT/XIT offset, sign + 5 digits
      23     1    P4  RIT on/off
      24     1    P5  XIT on/off
      25     1    P6  memory channel bank
      26     2    P7  memory channel number
      28     1    P8  0 = RX, 1 = TX
      29     1    P9  mode
      30     1    P10 VFO: 0 = A, 1 = B, 2 = MEM
      31     1    P11 scan status
      32     1    P12 split
      33     1    P13 tone/CTCSS
      34     2    P14 tone number
      36     1    P15 shift status
      37     1    ";"

  That is 37 characters plus the terminator; Hamlib's caps->if_len defaults to 37
  and it strips the ';' before checking the length, so the answer must be exactly
  38 bytes on the wire.

  Reply discipline (learned against Hamlib and Flrig, verified on hardware):
    * a *query* is answered;
    * a *set* is silent whether it succeeds or is rejected;
    * an unrecognised *query* gets "?;";
    * an unrecognised *set* is silent, because a real Kenwood acknowledges a set
      with silence -- answering it queues a reply nobody reads and shifts every
      later answer by one, which is what made Flrig report "Transceiver not
      responding" over a healthy socket;
    * "TX;" is the one explicit "N;": refusing PTT on a receiver is information
      the client should have, and Hamlib maps it to -RIG_ENAVAIL.
  A rejected set is recorded and readable with "ZZE;" instead.
*/

#ifndef CATCONTROL_H
#define CATCONTROL_H

#include <Arduino.h>

// ---------------------------------------------------------------- transports
void catBegin();          // restore settings from NVS; safe before the radio is up
void catStartTask();      // start the CAT service task (after the radio is ready)
void catServiceFromLoop();// call from loop(): applies queued sets, refreshes the cache

// Why a task at all: Flrig waits a *fixed* tcpip_ping_delay per command and its
// poll cycle is 21 commands, so the per-command latency is multiplied by 21 for
// every refresh of its display. Serviced from loop(), a reply could be delayed by a
// sprite push, an ADC sweep or an I2C read, so the wait had to be set to 400 ms --
// an 8.4 s refresh, which is why the frequency shown on the PC lagged the knob.
// The task answers queries from a snapshot the main loop refreshes, so replies are
// a few milliseconds regardless of what the display is doing.

bool catUsbGetEnabled();
void catUsbSetEnabled(bool en);
bool catUsbClientActive();      // a host spoke to us in the last few seconds

bool catNetClientActive();      // a TCP client is connected
uint8_t catNetClientCount();

// Ask the CAT task to release every TCP socket and wait up to waitMs for it to
// confirm. The WiFi stack must not be powered down until this returns true:
// esp_wifi_stop() frees lwIP state under whatever is open, and the CAT task lives on
// the other core, so a teardown can land inside its accept(), read() or write().
// Returns false if the task did not get out in time -- it can be blocked in a socket
// write for many seconds -- in which case the caller must retry rather than proceed.
bool catNetQuiesce(uint32_t waitMs = 500);

bool catAnyClientActive();

// True when CAT owns the USB serial port, i.e. when printing a diagnostic line
// would corrupt the command stream. Boot logging must honour this.
bool catUsbOwnsStream();

// ------------------------------------------------- radio hooks (in the .ino)
uint32_t catRadioGetFreqHz();
bool     catRadioSetFreqHz(uint32_t hz);   // false => out of range on every band
uint8_t  catRadioGetMode();                // sketch constants: FM/LSB/USB/AM
bool     catRadioSetMode(uint8_t mode);
uint8_t  catRadioGetVolume();              // 0..63
void     catRadioSetVolume(uint8_t v);
int      catRadioGetRssiDbuv();
bool     catRadioGetMute();
void     catRadioSetMute(bool m);
void     catRadioTouched();                // a CAT set landed: refresh the UI

// Local diagnostics, reachable as "ZZB;" / "ZZP;". Not part of the Kenwood set;
// they exist so GPIO trouble can be measured over CAT instead of guessed at.
void     catRadioDiag(char kind, char *out, size_t n);

// "ZZW;" answers from a WiFi snapshot the loop publishes -- the CAT task must not
// call into the WiFi stack itself. See CatNetSnap in the .cpp.

#endif
