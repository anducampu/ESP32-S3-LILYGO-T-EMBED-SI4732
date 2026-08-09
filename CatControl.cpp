#include "CatControl.h"
#include "WiFiPortal.h"

#include <WiFi.h>
#include <Preferences.h>

// These mirror the mode constants in the main sketch. The radio hooks speak in
// them, so if the .ino ever renumbers FM/LSB/USB/AM, fix these too.
#define RMODE_FM   0
#define RMODE_LSB  1
#define RMODE_USB  2
#define RMODE_AM   3

#define CAT_NS            "cat"
#define CAT_CMD_MAX       32     // longest Kenwood command we will buffer
#define CAT_NET_CLIENTS   4
#define CAT_IDLE_MS       5000UL // "client active" window for the status badge
#define CAT_AI_MIN_MS     120UL  // auto-info push rate limit

// NetworkClient::write() retries WIFI_CLIENT_MAX_WRITE_RETRY (10) times with a
// hardcoded 1 s select() each, and resets that counter on a partial send, so a
// peer whose window is shut can block a single print() for 10 s or more. The core
// exposes no availableForWrite() to gate on (Print's base returns 0, so testing it
// would suppress every reply), and setTimeout() only reaches the send() syscall,
// not the loop's own select. What is left is not writing to sockets that have gone
// quiet: a real CAT client polls several times a second.
#define CAT_REAP_MS       120000UL // silent this long => presumed dead, drop it
#define CAT_AI_ALIVE_MS   30000UL  // only push unsolicited data to proven-live peers

static Preferences prefs;

static bool usbEnabled = false;

// --------------------------------------------------------------- radio snapshot
// The CAT task must never touch the SI4735 or the display: both are owned by the
// main loop. Queries are answered from this snapshot, refreshed once per loop
// iteration, so an answer is at most one iteration stale (single-digit ms) and
// costs nothing to produce.
struct CatSnapshot
{
  uint32_t freqHz;
  uint8_t  mode;
  uint8_t  volume;
  int16_t  rssi;
  bool     muted;
};

static CatSnapshot     snap = {0, 0, 0, 0, false};
static portMUX_TYPE    snapMux = portMUX_INITIALIZER_UNLOCKED;

static CatSnapshot catSnapGet()
{
  CatSnapshot c;
  portENTER_CRITICAL(&snapMux);
  c = snap;
  portEXIT_CRITICAL(&snapMux);
  return c;
}

// ------------------------------------------------------------- set marshalling
// A set has to run on the main loop (it retunes the chip and redraws). The task
// hands one over and waits for it, so a client that writes then immediately reads
// back -- which is exactly what Hamlib and Flrig do to verify -- sees the new
// value rather than a stale snapshot.
enum { REQ_NONE = 0, REQ_FREQ, REQ_MODE, REQ_VOLUME, REQ_MUTE };

static volatile uint8_t  reqKind = REQ_NONE;
static volatile uint32_t reqVal  = 0;
static volatile bool     reqOk   = false;

// Draining reqDone is not enough on its own. If catRequest() gives up while the loop
// is already servicing that request, two things can still go wrong: the loop's
// `reqKind = REQ_NONE` can land after the *next* request was queued and swallow it
// (the client's set silently does nothing and the task then burns a full timeout),
// and the completion it signals can be collected by that next request as its own
// result. Tagging every request lets both sides tell them apart.
static volatile uint8_t  reqSeq    = 0;   // bumped by the task per request
static volatile uint8_t  reqAckSeq = 0;   // last sequence the loop completed
static SemaphoreHandle_t reqMutex = nullptr;
static SemaphoreHandle_t reqDone  = nullptr;
static TaskHandle_t      catTaskH = nullptr;

static bool catRequest(uint8_t kind, uint32_t val)
{
  if (!reqMutex || !reqDone) return false;
  if (xSemaphoreTake(reqMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return false;

  // Drop a completion left over from a previous timeout. catRequest() abandons a
  // request after 3 s, but the loop may already have latched it and still gives
  // reqDone afterwards -- a binary semaphore left signalled made the *next* set
  // return instantly carrying the *previous* result.
  xSemaphoreTake(reqDone, 0);

  uint8_t seq = (uint8_t)(reqSeq + 1);
  if (seq == 0) seq = 1;                   // 0 means "nothing acknowledged yet"
  reqVal  = val;
  reqOk   = false;
  reqSeq  = seq;
  reqKind = kind;                          // published last: the loop keys off this

  // Generous: a mode change re-uploads the SSB patch, a band change retunes.
  bool ok = false;
  uint32_t deadline = millis() + 3000;
  for (;;)
  {
    int32_t left = (int32_t)(deadline - millis());
    if (left <= 0) break;
    if (xSemaphoreTake(reqDone, pdMS_TO_TICKS(left)) != pdTRUE) break;
    // A completion for an abandoned request carries an older tag: ignore it and keep
    // waiting rather than reporting somebody else's result as ours.
    if (reqAckSeq == seq) { ok = reqOk; break; }
  }
  if (reqAckSeq != seq) reqKind = REQ_NONE;   // timed out; abandon it

  xSemaphoreGive(reqMutex);
  return ok;
}

// Per-connection state. aiMode, reportCw and vfoBHz used to be single globals
// shared by the USB port and all four TCP slots, so one client's "AI1;" pushed
// unsolicited frames into another client's stream and desynchronised its
// request/reply pairing, and one client's "MD3;" made every other client's "MD;"
// answer CW. They also outlived the client that set them.
struct CatChannel
{
  char     buf[CAT_CMD_MAX + 1];
  uint8_t  blen;
  bool     discarding;   // dropping an over-long line until its terminator
  uint8_t  aiMode;       // 0 = no unsolicited output on this connection
  bool     reportCw;     // this client asked for CW, which we demodulate as USB
  uint32_t vfoBHz;       // shadow VFO B, per client
};

static void catChannelReset(CatChannel &c)
{
  c.blen = 0;
  c.discarding = false;
  c.aiMode = 0;
  c.reportCw = false;
  c.vfoBHz = 0;
}

static WiFiServer *netServer = nullptr;
static WiFiClient  netClient[CAT_NET_CLIENTS];
static CatChannel  netChan[CAT_NET_CLIENTS];
static uint32_t    netSeen[CAT_NET_CLIENTS] = {0};   // last activity, for eviction
static uint16_t    netServerPort = 0;
static uint32_t    netBindRetryAt = 0;               // backoff after a failed bind

// Published by the CAT task, read by the UI. The main loop used to walk netClient[]
// itself to count live peers, which meant calling connected() -- a recv(MSG_PEEK)
// syscall -- on handles this task concurrently assigns and stops. WiFiClient is a
// shared_ptr, so that is a genuine data race (the reader can be left holding a freed
// socket handle), and it cost four syscalls on every redraw.
static volatile uint8_t netLiveCount = 0;

// Teardown handshakes with the main loop; see catNetQuiesce() / catUsbQuiesce().
static volatile bool netQuiesceReq = false;
static volatile bool netQuiesced   = false;
static volatile bool usbQuiesceReq = false;
static volatile bool usbQuiesced   = false;

static CatChannel usbChan;
static volatile uint32_t usbLastRx = 0;   // written by the task, read by the UI

// Pushes are rate limited globally: every connection's auto-info contends for the
// same radio and I2C bus.
static uint32_t lastAiPush  = 0;
static uint32_t aiLastFreq  = 0;
static uint8_t  aiLastMode  = 0xFF;
static bool     aiSeeded    = false;

// Last set we declined to apply, readable as "ZZE;".
//
// Rejected sets stay silent on purpose: Kenwood acknowledges a set with silence,
// and Flrig sends MD/AG sets without reading a reply, so an "N;" here desynchronises
// its stream and reproduces "Transceiver not responding" (verified on hardware).
// The cost is that a client cannot tell a rejected set from an applied one -- both
// Hamlib and Flrig re-read the value, so they see the truth, but nothing said *why*.
// This records it instead of injecting a reply nobody is reading.
static char lastReject[28] = "";

static void noteReject(const char *what)
{
  strlcpy(lastReject, what, sizeof(lastReject));
}

// ---------------------------------------------------------------- persistence
static void loadSettings()
{
  prefs.begin(CAT_NS, true);
  usbEnabled = prefs.getBool("usb", false);
  prefs.end();
}

// A CDC write blocks for ~100 ms per call when no host is draining the buffer.
// That only matters while CAT owns the port, where it could stall the radio
// loop; with CAT off we leave the stock timeout so the [BOOT] diagnostics still
// reach an attached terminal the way they always did.
static void applyUsbTxTimeout()
{
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // Not 0: that makes every write under backpressure return short, which
  // truncates replies. A small bounded value completes a 38-byte answer without
  // parking the loop the way the stock 100 ms would.
  Serial.setTxTimeoutMs(usbEnabled ? 10 : 100);
#endif
}

static void saveUsbEnabled()
{
  prefs.begin(CAT_NS, false);
  prefs.putBool("usb", usbEnabled);
  prefs.end();
}

// --------------------------------------------------------------- output path
// The CDC write timeout is cut to 10 ms while CAT owns the port (see
// applyUsbTxTimeout), so a stalled host cannot park the radio loop -- but that
// makes HWCDC::write return a short count under backpressure, and Kenwood framing
// is terminator-based: an answer truncated
// before its ';' leaves the client's parser permanently one field out of step.
// Every reply goes out through here as one buffer with a bounded retry, so a
// momentary stall costs latency rather than framing.
static void catEmit(Stream &out, const char *msg)
{
  size_t len = strlen(msg), sent = 0;
  uint32_t deadline = millis() + 60;
  while (sent < len)
  {
    size_t w = out.write((const uint8_t *)msg + sent, len - sent);
    if (w > 0) { sent += w; continue; }
    if ((int32_t)(millis() - deadline) >= 0) break;   // peer is not draining
    delay(1);
  }
}

// ------------------------------------------------------------ mode conversion
static char modeToKenwood(uint8_t m, bool reportCw)
{
  switch (m)
  {
    case RMODE_LSB: return '1';
    case RMODE_USB: return reportCw ? '3' : '2';
    case RMODE_FM:  return '4';
    case RMODE_AM:  return '5';
    default:        return '5';
  }
}

// Returns 0xFF for a Kenwood mode this radio cannot do.
static uint8_t kenwoodToMode(char d)
{
  switch (d)
  {
    case '1': return RMODE_LSB;
    case '2': return RMODE_USB;
    case '3': return RMODE_USB;   // CW -> USB + a narrow SSB filter
    case '4': return RMODE_FM;
    case '5': return RMODE_AM;
    default:  return 0xFF;
  }
}

// --------------------------------------------------------------- S-meter scale
// Hamlib turns the TS-480 raw S-meter into dB relative to S9 with
// (raw * 4) - 54, so raw 13-14 reads as S9. On HF, S9 is 34 dBuV, hence
// raw = (dBuV - 34 + 54) / 4. The TS-480 meter tops out at 30.
static uint16_t rssiToKenwoodMeter(int dbuv)
{
  int raw = (dbuv + 20) / 4;
  if (raw < 0)  raw = 0;
  if (raw > 30) raw = 30;
  return (uint16_t)raw;
}

// ------------------------------------------------------------- answer builders
static void buildIF(char *out, size_t n, bool reportCw)
{
  // One snapshot for the whole answer. Sampling per field could pair a frequency
  // from before a retune with the mode from after it.
  CatSnapshot c = catSnapGet();
  snprintf(out, n,
           "IF%011lu"  // P1  frequency, Hz
           "%04u"      // P2  frequency step
           "%+06d"     // P3  RIT/XIT offset
           "%c"        // P4  RIT off
           "%c"        // P5  XIT off
           "%c"        // P6  memory bank
           "%02u"      // P7  memory channel
           "%c"        // P8  RX
           "%c"        // P9  mode
           "%c"        // P10 VFO A
           "%c"        // P11 no scan
           "%c"        // P12 simplex
           "%c"        // P13 no tone
           "%02u"      // P14 tone number
           "%c"        // P15 no shift
           ";",
           (unsigned long)c.freqHz,
           0u,
           0,
           '0', '0', '0',
           0u,
           '0',
           modeToKenwood(c.mode, reportCw),
           '0', '0', '0', '0',
           0u,
           '0');
}

// ------------------------------------------------------------- command engine
// cmd is NUL-terminated with the ';' already stripped.
static void catExecute(const char *cmd, CatChannel &ch, Stream &out)
{
  char buf[48];
  size_t len = strlen(cmd);

  if (len < 2)
  {
    catEmit(out, "?;");
    return;
  }

  const char *arg = cmd + 2;
  size_t      alen = len - 2;

  // ---- identity / power / auto-info -------------------------------------
  if (!strncmp(cmd, "ID", 2))
  {
    if (alen) return;
    catEmit(out, "ID020;");                 // 020 = TS-480
    return;
  }
  if (!strncmp(cmd, "PS", 2))
  {
    if (!alen) catEmit(out, "PS1;");        // always powered while we can answer
    return;
  }
  if (!strncmp(cmd, "AI", 2))
  {
    if (!alen) { snprintf(buf, sizeof(buf), "AI%u;", ch.aiMode); catEmit(out, buf); }
    else if (arg[0] >= '0' && arg[0] <= '4') ch.aiMode = arg[0] - '0';
    return;
  }

  // ---- frequency --------------------------------------------------------
  if (!strncmp(cmd, "FA", 2))
  {
    if (!alen)
    {
      snprintf(buf, sizeof(buf), "FA%011lu;", (unsigned long)catSnapGet().freqHz);
      catEmit(out, buf);
      return;
    }
    if (alen != 11) { noteReject("FA malformed"); return; }
    uint32_t hz = strtoul(arg, nullptr, 10);
    // A frequency no band can reach is simply not applied. Staying silent keeps
    // the stream in step; the client's next FA read shows the unchanged value,
    // which is both honest and self-correcting.
    if (!catRequest(REQ_FREQ, hz)) noteReject("FA out of band");
    return;
  }
  if (!strncmp(cmd, "FB", 2))
  {
    if (!alen)
    {
      if (!ch.vfoBHz) ch.vfoBHz = catSnapGet().freqHz;
      snprintf(buf, sizeof(buf), "FB%011lu;", (unsigned long)ch.vfoBHz);
      catEmit(out, buf);
      return;
    }
    if (alen != 11) return;               // malformed set: stay silent
    ch.vfoBHz = strtoul(arg, nullptr, 10);   // single-VFO radio: remembered only
    return;
  }

  // ---- VFO selection (single VFO, so always A) --------------------------
  if (!strncmp(cmd, "FR", 2) || !strncmp(cmd, "FT", 2))
  {
    if (!alen) { snprintf(buf, sizeof(buf), "%c%c0;", cmd[0], cmd[1]); catEmit(out, buf); }
    return;
  }

  // ---- mode -------------------------------------------------------------
  if (!strncmp(cmd, "MD", 2))
  {
    if (!alen)
    {
      snprintf(buf, sizeof(buf), "MD%c;", modeToKenwood(catSnapGet().mode, ch.reportCw));
      catEmit(out, buf);
      return;
    }
    uint8_t m = kenwoodToMode(arg[0]);
    if (m == 0xFF) { noteReject("MD unsupported"); return; }
    ch.reportCw = (arg[0] == '3');
    if (!catRequest(REQ_MODE, m)) noteReject("MD wrong band");
    return;
  }

  // ---- S-meter ----------------------------------------------------------
  if (!strncmp(cmd, "SM", 2))
  {
    // Hamlib checks the answer length against the command it sent, so echo the
    // selector back: "SM0;" -> "SM0nnnn;", bare "SM;" -> "SMnnnn;".
    uint16_t meter = rssiToKenwoodMeter(catSnapGet().rssi);
    if (alen >= 1) snprintf(buf, sizeof(buf), "SM%c%04u;", arg[0], meter);
    else           snprintf(buf, sizeof(buf), "SM%04u;", meter);
    catEmit(out, buf);
    return;
  }

  // ---- AF gain ----------------------------------------------------------
  if (!strncmp(cmd, "AG", 2))
  {
    // Hamlib queries this as "AG0;", i.e. selector present but no value, so a
    // one-character argument is still a read. Echo the selector back: it checks
    // the answer length against the command it sent.
    if (alen <= 1)
    {
      // Round both conversions. Truncating each way made every read/write round
      // trip lose a step, so a client that echoed back what it read walked the
      // volume down to zero one notch at a time.
      uint16_t v255 = ((uint16_t)catSnapGet().volume * 255u + 31u) / 63u;
      if (alen == 1) snprintf(buf, sizeof(buf), "AG%c%03u;", arg[0], v255);
      else           snprintf(buf, sizeof(buf), "AG%03u;", v255);
      catEmit(out, buf);
      return;
    }
    // Accept "AG0nnn" (with the RX selector) and the bare "AGnnn".
    const char *digits = (alen == 4) ? arg + 1 : arg;
    if (alen != 4 && alen != 3) { noteReject("AG malformed"); return; }
    uint16_t v255 = (uint16_t)strtoul(digits, nullptr, 10);
    if (v255 > 255) v255 = 255;
    catRequest(REQ_VOLUME, (uint32_t)((v255 * 63u + 127u) / 255u));
    return;
  }

  // ---- squelch (stub, keeps clients from erroring) ----------------------
  if (!strncmp(cmd, "SQ", 2))
  {
    if (alen <= 1) catEmit(out, "SQ0000;");
    return;
  }

  // ---- mute -------------------------------------------------------------
  if (!strncmp(cmd, "MU", 2))
  {
    if (!alen) { snprintf(buf, sizeof(buf), "MU%c;", catSnapGet().muted ? '1' : '0'); catEmit(out, buf); }
    else if (arg[0] == '0' || arg[0] == '1') catRequest(REQ_MUTE, arg[0] == '1' ? 1 : 0);
    return;
  }

  // ---- composite status -------------------------------------------------
  if (!strncmp(cmd, "IF", 2))
  {
    if (alen) return;
    buildIF(buf, sizeof(buf), ch.reportCw);
    catEmit(out, buf);
    return;
  }

  // ---- ZZ: local diagnostics (not Kenwood) -----------------------------
  if (!strncmp(cmd, "ZZ", 2))
  {
    if (alen == 1 && arg[0] == 'E')
    {
      char z[48];
      snprintf(z, sizeof(z), "ZZE %s;", *lastReject ? lastReject : "none");
      catEmit(out, z);
      return;
    }
    if (alen == 1 && arg[0] == 'N')
    {
      char d[160]; size_t u = 0;
      for (int i = 0; i < CAT_NET_CLIENTS; i++)
      {
        int w = snprintf(d + u, (u < sizeof(d)) ? sizeof(d) - u : 0, " s%d=%s/%lus",
                         i,
                         netClient[i].connected() ? "live" : "free",
                         netSeen[i] ? (unsigned long)((millis() - netSeen[i]) / 1000) : 0UL);
        if (w > 0) u += (size_t)w;
      }
      char z[200];
      snprintf(z, sizeof(z), "ZZN%s;", d);
      catEmit(out, z);
      return;
    }
    if (alen == 1 && (arg[0] == 'B' || arg[0] == 'P'))
    {
      char d[200], z[224];
      catRadioDiag(arg[0], d, sizeof(d));
      snprintf(z, sizeof(z), "ZZ%c%s;", arg[0], d);
      catEmit(out, z);
      return;
    }
    catEmit(out, "?;");
    return;
  }

  // ---- PTT: receive-only radio -----------------------------------------
  if (!strncmp(cmd, "RX", 2)) return;        // already receiving; silent success
  if (!strncmp(cmd, "TX", 2)) { catEmit(out, "N;"); return; }

  // An unrecognised command that carries data is a *set*, and a real Kenwood
  // acknowledges a set with silence. Answering "?;" here injects a reply nobody
  // is reading, which shifts every later answer by one: Flrig sends its EX menu
  // sets at startup, banks our "?;" replies, then reads one of them in place of
  // the frequency and declares the transceiver dead. So only an unknown *query*
  // -- a command with no argument -- gets an answer.
  if (alen) return;
  catEmit(out, "?;");
}

// ------------------------------------------------------------ stream plumbing
// Feeds one byte into a per-connection buffer and runs the command on ';'.
// Returns false when the input cannot be CAT at all, meaning the caller should
// drop the connection.
static bool catFeed(char c, CatChannel &ch, Stream &out)
{
  if (c == '\r' || c == '\n') return true;   // tolerate terminal line endings

  if (c == ';')
  {
    if (ch.discarding)
    {
      // End of the over-long line. Kenwood's "O" means discard everything up to
      // the terminator; re-arming the parser mid-line instead meant the tail of a
      // garbled line ("...MD5;") was executed as a real command, and each 32-byte
      // chunk emitted another unrequested "O;" that shifted every later answer.
      ch.discarding = false;
      ch.blen = 0;
      return true;
    }
    ch.buf[ch.blen] = '\0';
    if (ch.blen) catExecute(ch.buf, ch, out);
    ch.blen = 0;
    return true;
  }

  if (ch.discarding) return true;            // still swallowing the bad line

  // Kenwood commands are uppercase letters and digits only. Anything else means
  // the peer is not speaking CAT -- most importantly a browser: pointing any page
  // at http://radio:1234/ used to feed "GET / HTTP/1.1" straight into the parser,
  // so a visited web page could drive the radio with fetch(). The space in the
  // request line trips this on the fourth byte.
  bool legal = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '+' || c == '-';
  if (!legal)
  {
    ch.blen = 0;
    return false;
  }

  if (ch.blen >= CAT_CMD_MAX)
  {
    // Kenwood's "too many characters without a terminator": answer once, then
    // swallow the rest of the line rather than parsing its tail.
    ch.blen = 0;
    ch.discarding = true;
    catEmit(out, "O;");
    return true;
  }
  ch.buf[ch.blen++] = c;
  return true;
}

static void pollUsb()
{
  // The main loop is about to rewrite this channel's parser state; stay out of it.
  if (usbQuiesceReq) { usbQuiesced = true; return; }

  if (!usbEnabled) return;

  int n = Serial.available();
  if (n > 0) usbLastRx = millis();

  while (n-- > 0)
  {
    int c = Serial.read();
    if (c < 0) break;
    // No connection to drop on USB; swallow to the next terminator instead.
    if (!catFeed((char)c, usbChan, Serial)) { usbChan.blen = 0; usbChan.discarding = true; }
  }
}

static void startNetServer(uint16_t port)
{
  if (netServer && netServerPort == port) return;
  if (netServer) { netServer->end(); delete netServer; netServer = nullptr; }

  // catPoll() runs several times per loop iteration, so an unconditional retry
  // churned socket()/bind()/close() hundreds of times a second while a bind kept
  // failing (typically the port still in TIME_WAIT).
  if (netBindRetryAt && (int32_t)(millis() - netBindRetryAt) < 0) return;

  netServer = new WiFiServer(port);
  netServer->begin();

  // begin() returns void and fails silently -- typically the port is still in
  // TIME_WAIT from the previous session. Latching netServerPort regardless meant
  // the early-return above skipped every later attempt, so CAT never came back
  // until a reboot. operator bool() exposes the real listening state.
  if (!*netServer)
  {
    delete netServer;
    netServer = nullptr;
    netServerPort = 0;          // leave it unset so a later poll retries
    netBindRetryAt = millis() + 2000;
    return;
  }

  netServer->setNoDelay(true);
  netServerPort = port;
  netBindRetryAt = 0;
}

// NOTE on the client checks below and in pollNet(): NetworkClient::operator bool()
// is not a handle test -- it calls connected(), which issues a recv(MSG_PEEK) syscall
// whenever the socket is up. Writing `netClient[i] && netClient[i].connected()` cost
// two syscalls per live peer per pass, i.e. a thousand a second at the 2 ms poll.
// connected() already returns 0 for an unused slot, so it is the only test needed.
static void stopNetServer()
{
  for (int i = 0; i < CAT_NET_CLIENTS; i++)
  {
    netClient[i].stop();          // cheap and safe on an already-closed slot
    catChannelReset(netChan[i]);
  }
  if (netServer)
  {
    netServer->end();
    delete netServer;
    netServer = nullptr;
  }
  netServerPort = 0;
  netLiveCount = 0;
}

// Called from the main loop before the WiFi hardware is powered down. Asks the CAT
// task to drop every socket and waits for it to confirm, because esp_wifi_stop()
// frees lwIP state under whatever is still open -- and this task, on the other core,
// could be inside accept() or read() on exactly those descriptors.
//
// Normally satisfied in a task tick or two. It can miss, though, and the caller has
// to know: NetworkClient::write() retries ten times with a hardcoded 1 s select()
// each (see the note at the top of this file), so a peer whose window has shut can
// park this task inside catEmit() for upwards of 10 s -- far longer than any wait we
// can afford to hold the radio loop for. Returning false says "still in there";
// powering WiFi down anyway would be the exact use-after-free this exists to stop.
bool catNetQuiesce(uint32_t waitMs)
{
  if (!catTaskH)          // no task yet: nothing else can be holding a socket
  {
    stopNetServer();
    return true;
  }

  netQuiesced   = false;
  netQuiesceReq = true;

  uint32_t t0 = millis();
  while (!netQuiesced && (millis() - t0) < waitMs)
  {
    // Service any set still in flight. The task blocks in catRequest() waiting for
    // exactly this, and a task parked there can never reach pollNet() to answer the
    // barrier -- so without it a teardown during a CAT set burnt the whole wait.
    catServiceFromLoop();
    delay(2);
  }

  // Always drop the barrier before returning. Leaving it latched on a miss would
  // stop the task serving for good; the caller keeps its own retry state instead.
  bool done = netQuiesced;
  netQuiesceReq = false;
  return done;
}

// Same idea for the USB channel. catUsbSetEnabled() rewrites usbChan, and the task
// can be sitting inside catRequest() holding that channel for up to 3 s, so a plain
// delay proves nothing about who owns the parser state.
static void catUsbQuiesce()
{
  if (!catTaskH) return;

  usbQuiesced   = false;
  usbQuiesceReq = true;

  uint32_t t0 = millis();
  while (!usbQuiesced && (millis() - t0) < 3500)
  {
    catServiceFromLoop();      // releases a task parked in catRequest()
    delay(2);
  }
}

static void pollNet()
{
  // A teardown is pending on the main loop: let go of everything and acknowledge.
  if (netQuiesceReq)
  {
    stopNetServer();
    netQuiesced = true;
    return;
  }

  // The TCP transport only exists while the station interface is up. netSuspended()
  // covers the tethered case: the station can still be associated while the state
  // machine is parked, and nothing should be served in that window.
  if (!netEnabled() || netSuspended() || netGetState() != NET_CONNECTED)
  {
    // Unconditional. Gating on netServer left client sockets open and their channels
    // un-reset whenever a rebind had failed (netServer == nullptr with peers still
    // held from the previous listener), while netLiveCount below reported nobody --
    // and those sockets then survived into the WiFi teardown. stopNetServer() already
    // no-ops on a null server.
    stopNetServer();
    return;
  }

  startNetServer(netGetCatPort());

  // Only accepting new connections depends on the listener. Returning here on a
  // failed bind also skipped the service loop below, so four already-connected
  // clients were never read, answered or reaped -- they saw ESTABLISHED sockets
  // and a dead rig, the exact symptom the eviction logic exists to prevent.
  WiFiClient incoming = netServer ? netServer->accept() : WiFiClient();
  if (incoming)
  {
    int slot = -1;
    for (int i = 0; i < CAT_NET_CLIENTS; i++)
      if (!netClient[i].connected()) { slot = i; break; }

    if (slot < 0)
    {
      // Every slot is nominally live. Refusing the newcomer was wrong: a client
      // whose process was killed can leave a half-open socket that still reports
      // connected(), so the stale entries silently locked out every later
      // connection -- the socket looked ESTAB from the PC while the radio never
      // read a byte of it. Evict the least recently active client instead: for a
      // control link the newest arrival is the one someone is actually using.
      slot = 0;
      for (int i = 1; i < CAT_NET_CLIENTS; i++)
        if (netSeen[i] < netSeen[slot]) slot = i;
    }

    netClient[slot].stop();
    netClient[slot] = incoming;
    netClient[slot].setNoDelay(true);
    catChannelReset(netChan[slot]);   // never inherit the previous client's AI/CW
    netSeen[slot] = millis();
  }

  // Counted here, where connected() is being called anyway, and published for the
  // UI to read: see netLiveCount.
  uint8_t live = 0;

  for (int i = 0; i < CAT_NET_CLIENTS; i++)
  {
    if (!netClient[i].connected()) { netClient[i].stop(); catChannelReset(netChan[i]); continue; }

    // Reap silent peers rather than waiting for a new connection to evict them.
    // A half-open socket keeps reporting connected() forever, and writing to one
    // is what stalls the loop.
    if (netSeen[i] && (millis() - netSeen[i] > CAT_REAP_MS))
    {
      netClient[i].stop();
      catChannelReset(netChan[i]);
      continue;
    }

    live++;

    int n = netClient[i].available();
    if (n > 0) netSeen[i] = millis();
    while (n-- > 0)
    {
      int c = netClient[i].read();
      if (c < 0) break;
      if (!catFeed((char)c, netChan[i], netClient[i]))
      {
        netClient[i].stop();
        catChannelReset(netChan[i]);
        live--;
        break;
      }
    }
  }

  netLiveCount = live;
}

// Unsolicited IF; on a state change, when the client asked for it with AI.
static void pushAutoInfo()
{
  CatSnapshot c = catSnapGet();
  uint32_t f = c.freqHz;
  uint8_t  m = c.mode;

  if (!aiSeeded)
  {
    aiSeeded = true;
    aiLastFreq = f;
    aiLastMode = m;
    return;
  }

  if (f == aiLastFreq && m == aiLastMode) return;

  // Does anyone actually want unsolicited output?
  bool wantUsb = usbEnabled && usbChan.aiMode;
  bool wantNet = false;
  for (int i = 0; i < CAT_NET_CLIENTS; i++)
    if (netChan[i].aiMode && netClient[i].connected() &&
        netSeen[i] && (millis() - netSeen[i] < CAT_AI_ALIVE_MS))
      wantNet = true;

  if (!wantUsb && !wantNet) { aiLastFreq = f; aiLastMode = m; return; }

  // Advance the baseline only once the push actually goes out. Updating it first
  // meant a change arriving inside the rate-limit window was swallowed for good,
  // so the client's last known state stayed wrong until something else moved.
  if (millis() - lastAiPush < CAT_AI_MIN_MS) return;
  lastAiPush = millis();
  aiLastFreq = f;
  aiLastMode = m;

  char buf[48];

  // Each connection gets its own frame: the CW flag is per client, so the mode
  // digit can legitimately differ between them.
  if (wantUsb)
  {
    buildIF(buf, sizeof(buf), usbChan.reportCw);
    catEmit(Serial, buf);
  }
  for (int i = 0; i < CAT_NET_CLIENTS; i++)
    if (netChan[i].aiMode && netClient[i].connected() &&
        netSeen[i] && (millis() - netSeen[i] < CAT_AI_ALIVE_MS))
    {
      buildIF(buf, sizeof(buf), netChan[i].reportCw);
      catEmit(netClient[i], buf);
    }
}

// ------------------------------------------------------------------ public API
void catBegin()
{
  // Called very early in setup(), before the SI4732 is up, so that the boot log
  // knows whether CAT owns the USB port. Must not touch the radio hooks.
  loadSettings();
  applyUsbTxTimeout();
}

// Runs on the core the Arduino loop does not use, so a reply never waits on a
// sprite push or an I2C read.
static void catTaskFn(void *)
{
  for (;;)
  {
    pollUsb();
    pollNet();
    pushAutoInfo();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void catStartTask()
{
  if (catTaskH) return;
  reqMutex = xSemaphoreCreateMutex();
  reqDone  = xSemaphoreCreateBinary();
  catServiceFromLoop();                    // seed the snapshot before serving
  // Core 0: the sketch pins its loop and events to core 1 (see the FQBN).
  // 8 KB: the ZZ diagnostic handlers put ~450 bytes of buffers in a single frame on
  // top of the lwIP call depth, and a stack overflow here takes the whole radio down.
  xTaskCreatePinnedToCore(catTaskFn, "cat", 8192, nullptr, 2, &catTaskH, 0);
}

static void catRefreshSnapshot()
{
  CatSnapshot c;
  c.freqHz = catRadioGetFreqHz();
  c.mode   = catRadioGetMode();
  c.volume = catRadioGetVolume();
  c.rssi   = (int16_t)catRadioGetRssiDbuv();
  c.muted  = catRadioGetMute();

  portENTER_CRITICAL(&snapMux);
  snap = c;
  portEXIT_CRITICAL(&snapMux);
}

// Called from loop(): apply a queued set, then refresh the snapshot.
void catServiceFromLoop()
{
  uint8_t k = reqKind;
  if (k != REQ_NONE)
  {
    uint8_t  seq = reqSeq;
    bool ok = false;
    uint32_t v = reqVal;
    switch (k)
    {
      case REQ_FREQ:   ok = catRadioSetFreqHz(v); break;
      case REQ_MODE:   ok = catRadioSetMode((uint8_t)v); break;
      case REQ_VOLUME: catRadioSetVolume((uint8_t)v); ok = true; break;
      case REQ_MUTE:   catRadioSetMute(v != 0); ok = true; break;
    }
    if (ok) catRadioTouched();
    reqOk     = ok;
    reqAckSeq = seq;

    // Retract only the request we actually serviced. catRequest() may have given up
    // and queued a newer one while this was retuning; clearing that would swallow it.
    if (reqSeq == seq) reqKind = REQ_NONE;

    // Refresh *before* releasing the waiter. Doing it afterwards let the task wake
    // and answer the client's verify read from the pre-set snapshot, so every
    // set-then-read came back one step behind.
    catRefreshSnapshot();
    if (reqDone) xSemaphoreGive(reqDone);
    return;
  }

  catRefreshSnapshot();
}

bool catUsbGetEnabled() { return usbEnabled; }

void catUsbSetEnabled(bool en)
{
  if (usbEnabled == en) return;

  // No longer forces the WiFi transport off. It used to, but that wrote the
  // preference to NVS, so turning CAT USB on permanently destroyed the user's
  // choice of WiFi CAT and unplugging never brought it back. The USB-host suspend
  // in loop() already powers WiFi down whenever the radio is tethered, which is
  // automatic, reversible, and covers the case this was written for.
  // Fence the CAT task out of pollUsb() before touching the channel it parses into.
  // It can be parked in catRequest() for seconds holding usbChan, so the handover
  // needs an acknowledgement, not a sleep.
  catUsbQuiesce();

  usbEnabled = en;
  catChannelReset(usbChan);
  if (en) { while (Serial.available()) Serial.read(); }   // drop terminal leftovers
  applyUsbTxTimeout();

  usbQuiesceReq = false;        // release the fence
  saveUsbEnabled();
}

bool catUsbClientActive()
{
  return usbEnabled && usbLastRx && (millis() - usbLastRx < CAT_IDLE_MS);
}

// Reads the count the CAT task publishes. Never touches netClient[]: those handles
// belong to the task, and inspecting them from the loop was both a shared_ptr race
// and four syscalls per redraw.
uint8_t catNetClientCount() { return netLiveCount; }

bool catNetClientActive() { return catNetClientCount() > 0; }

bool catAnyClientActive() { return catUsbClientActive() || catNetClientActive(); }

bool catUsbOwnsStream() { return usbEnabled; }
