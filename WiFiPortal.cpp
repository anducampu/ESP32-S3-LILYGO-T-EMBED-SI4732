#include "WiFiPortal.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

#include "CatControl.h"   // catNetQuiesce(): the CAT task owns the TCP sockets


#define NET_NS             "wifinet"
#define NET_CONNECT_MS     10000UL   // per-network attempt
#define NET_PORTAL_MS      300000UL  // give up on an unattended portal after 5 min
#define NET_PORTAL_GRACE   8000UL    // keep the AP alive briefly after a good connect
#define NET_HOSTNAME       "t-embed-cat"

static Preferences prefs;
static WebServer  *web = nullptr;
static DNSServer  *dns = nullptr;
static bool        portalOpen = false;   // listeners live; the objects outlive this

#define PORTAL_HTTP_PORT  80

static bool     enabled  = false;
static NetState state    = NET_OFF;
static uint16_t catPort  = NET_DEFAULT_PORT;

static char    credSsid[NET_MAX_CREDS][33];
static char    credPass[NET_MAX_CREDS][65];
static uint8_t credCount = 0;

static uint8_t  tryIdx           = 0;
static uint32_t stateSince       = 0;
static uint32_t portalSince      = 0;
static uint32_t portalGraceUntil = 0;
static bool     mdnsUp           = false;
static bool     portalSawRequest = false;  // did anyone actually open the page?
static uint8_t  portalIdleCycles = 0;      // consecutive unattended portal windows
static uint32_t portalRetryAt    = 0;      // next saved-network attempt while in portal
static uint8_t  portalRetryIdx   = 0;      // which saved network that attempt uses
static bool     setupOnly        = false;  // portal opened purely to configure WiFi
static bool     radioOn          = false;  // WiFi hardware powered
static bool     suspended        = false;  // powered down while USB is the link
static bool     powerDownPending = false;  // teardown waiting on the CAT barrier
static uint32_t powerDownRetryAt = 0;
static uint32_t joinUntil        = 0;      // association window of a join we started

// True while a station join we started is still in its association window. The scan
// guards need this rather than `state != NET_CONNECTING`: the portal's own retry
// calls WiFi.begin() with the state left at NET_PORTAL, so testing the state alone
// let a Rescan abort a join in flight -- which is exactly the failure both guards
// were written to prevent.
static bool joinInFlight()
{
  return state == NET_CONNECTING ||
         (joinUntil && (int32_t)(millis() - joinUntil) < 0);
}

static void beginConnect(uint8_t idx);

// ---------------------------------------------------------------- persistence
static void loadAll()
{
  prefs.begin(NET_NS, true);
  enabled  = prefs.getBool("en", false);
  catPort  = prefs.getUShort("port", NET_DEFAULT_PORT);
  credCount = prefs.getUChar("n", 0);
  if (credCount > NET_MAX_CREDS) credCount = NET_MAX_CREDS;

  char key[6];
  for (uint8_t i = 0; i < credCount; i++)
  {
    snprintf(key, sizeof(key), "s%u", i);
    String s = prefs.getString(key, "");
    snprintf(key, sizeof(key), "p%u", i);
    String p = prefs.getString(key, "");
    strlcpy(credSsid[i], s.c_str(), sizeof(credSsid[i]));
    strlcpy(credPass[i], p.c_str(), sizeof(credPass[i]));
  }
  prefs.end();

  if (catPort == 0) catPort = NET_DEFAULT_PORT;
}

static void saveCreds()
{
  prefs.begin(NET_NS, false);
  prefs.putUChar("n", credCount);
  char key[6];
  for (uint8_t i = 0; i < credCount; i++)
  {
    snprintf(key, sizeof(key), "s%u", i);
    prefs.putString(key, credSsid[i]);
    snprintf(key, sizeof(key), "p%u", i);
    prefs.putString(key, credPass[i]);
  }
  // Clear any stale slots left behind by a delete.
  for (uint8_t i = credCount; i < NET_MAX_CREDS; i++)
  {
    snprintf(key, sizeof(key), "s%u", i);
    prefs.remove(key);
    snprintf(key, sizeof(key), "p%u", i);
    prefs.remove(key);
  }
  prefs.end();
}

static void saveEnabled()
{
  prefs.begin(NET_NS, false);
  prefs.putBool("en", enabled);
  prefs.end();
}

static void savePort()
{
  prefs.begin(NET_NS, false);
  prefs.putUShort("port", catPort);
  prefs.end();
}

// Adds or replaces a network. Returns the slot it landed in.
static int8_t credUpsert(const char *ssid, const char *pass)
{
  if (!ssid || !*ssid) return -1;

  for (uint8_t i = 0; i < credCount; i++)
  {
    if (!strcmp(credSsid[i], ssid))
    {
      // An empty field means "leave it alone", never "the password is now
      // blank". Overwriting here destroyed a working credential whenever the
      // form was submitted for any other reason.
      if (pass && *pass)
      {
        strlcpy(credPass[i], pass, sizeof(credPass[i]));
        saveCreds();
      }
      return (int8_t)i;
    }
  }

  uint8_t slot;
  if (credCount < NET_MAX_CREDS)
  {
    slot = credCount++;
  }
  else
  {
    // Full: drop the oldest entry and append.
    for (uint8_t i = 1; i < NET_MAX_CREDS; i++)
    {
      strlcpy(credSsid[i - 1], credSsid[i], sizeof(credSsid[0]));
      strlcpy(credPass[i - 1], credPass[i], sizeof(credPass[0]));
    }
    slot = NET_MAX_CREDS - 1;
    // The shift moved every entry down one, and tryIdx indexes this same array.
    // credDelete() already tracked that; this path did not, so saving a sixth
    // network mid-join named the wrong SSID on screen and skipped an entry on the
    // next retry.
    if (tryIdx) tryIdx--;
    if (portalRetryIdx) portalRetryIdx--;
  }
  strlcpy(credSsid[slot], ssid, sizeof(credSsid[0]));
  strlcpy(credPass[slot], pass ? pass : "", sizeof(credPass[0]));
  saveCreds();
  return (int8_t)slot;
}

static void credDelete(uint8_t idx)
{
  if (idx >= credCount) return;
  for (uint8_t i = idx + 1; i < credCount; i++)
  {
    strlcpy(credSsid[i - 1], credSsid[i], sizeof(credSsid[0]));
    strlcpy(credPass[i - 1], credPass[i], sizeof(credPass[0]));
  }
  credCount--;
  // tryIdx indexes this same array: leaving it alone made the connecting banner name
  // a network that no longer exists, and let the timeout path skip a valid entry.
  if (tryIdx >= credCount) tryIdx = 0;
  else if (tryIdx > idx)   tryIdx--;
  // The portal's round-robin indexes the same array. Left alone it pointed one slot
  // past where it meant to, so the entry that moved into the deleted slot was never
  // retried in that pass -- if that was the only network in range, the radio stayed
  // in the portal, which is the failure the walk was added to fix.
  if (portalRetryIdx >= credCount) portalRetryIdx = 0;
  else if (portalRetryIdx > idx)   portalRetryIdx--;
  saveCreds();
}

// --------------------------------------------------------------------- portal
static String htmlEscape(const String &in)
{
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++)
  {
    char c = in[i];
    switch (c)
    {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
  return out;
}

static const char PORTAL_CSS[] PROGMEM =
  "*{box-sizing:border-box}"
  "body{margin:0;padding:18px;background:#101418;color:#e6edf3;"
  "font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}"
  "main{max-width:520px;margin:0 auto}"
  "h1{font-size:19px;margin:0 0 2px}"
  "p.sub{margin:0 0 18px;color:#8b949e;font-size:13px}"
  "section{background:#161b22;border:1px solid #26303b;border-radius:10px;"
  "padding:14px;margin-bottom:14px}"
  "h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;"
  "color:#8b949e;margin:0 0 10px}"
  "label{display:block;font-size:13px;color:#8b949e;margin:10px 0 4px}"
  "input{width:100%;padding:9px 10px;border-radius:7px;border:1px solid #30363d;"
  "background:#0d1117;color:#e6edf3;font-size:15px}"
  "button{width:100%;margin-top:14px;padding:11px;border:0;border-radius:7px;"
  "background:#2f81f7;color:#fff;font-size:15px;font-weight:600}"
  "ul{list-style:none;margin:0;padding:0}"
  "li{display:flex;align-items:center;gap:8px;padding:8px 0;"
  "border-bottom:1px solid #21262d}"
  "li:last-child{border-bottom:0}"
  "li .n{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
  "li .m{color:#8b949e;font-size:12px;font-variant-numeric:tabular-nums}"
  "a{color:#2f81f7;text-decoration:none}"
  ".pick{cursor:pointer}"
  ".del{color:#f85149}"
  ".ok{color:#3fb950}.warn{color:#d29922}"
  ".kv{display:flex;justify-content:space-between;gap:10px;padding:3px 0;font-size:14px}"
  ".kv span:first-child{color:#8b949e}";

static void portalSendPage(const String &notice)
{
  int n = WiFi.scanComplete();

  String h;
  h.reserve(9000);   // String silently drops an append it cannot grow for
  h += F("<!doctype html><html><head><meta charset=utf-8>"
         "<meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>T-Embed CAT setup</title><style>");
  h += FPSTR(PORTAL_CSS);
  h += F("</style></head><body><main>"
         "<h1>T-Embed CAT</h1>"
         "<p class=sub>WiFi setup for remote rig control</p>");

  if (notice.length())
    h += "<section><b class=ok>" + htmlEscape(notice) + "</b></section>";

  // ---- current status
  h += F("<section><h2>Status</h2>");
  h += "<div class=kv><span>State</span><span>";
  switch (state)
  {
    case NET_CONNECTED:  h += F("<b class=ok>connected</b>"); break;
    case NET_CONNECTING: h += F("<b class=warn>connecting\xE2\x80\xA6</b>"); break;
    case NET_PORTAL:     h += F("setup portal"); break;
    default:             h += F("idle"); break;
  }
  h += "</span></div>";
  if (WiFi.status() == WL_CONNECTED)
  {
    h += "<div class=kv><span>Network</span><span>" + htmlEscape(WiFi.SSID()) + "</span></div>";
    h += "<div class=kv><span>Address</span><span>" + WiFi.localIP().toString() + "</span></div>";
  }
  h += "<div class=kv><span>CAT port</span><span>" + String(catPort) + "</span></div>";
  h += "<div class=kv><span>Hostname</span><span>" NET_HOSTNAME ".local</span></div>";
  h += F("</section>");

  // ---- add a network
  h += F("<section><h2>Add network</h2><form method=POST action=/save>"
         "<label for=ssid>Network name</label>"
         "<input id=ssid name=ssid maxlength=32 autocapitalize=off autocorrect=off required>"
         "<label for=pass>Password <small>(blank for an open network)</small></label>"
         "<input id=pass name=pass type=password maxlength=64>"
         "<button type=submit>Save &amp; connect</button></form>"
         "<p class=sub style='margin:8px 0 0'>Leave the password blank to keep the "
         "one already stored for a known network.</p></section>");

  // Separate form: changing the port must not require touching credentials.
  h += F("<section><h2>CAT TCP port</h2><form method=POST action=/port>"
         "<input name=port type=number min=1 max=65535 value=");
  h += String(catPort);
  h += F("><button type=submit>Save port</button></form></section>");

  // ---- scan results
  h += F("<section><h2>Networks in range</h2><ul>");
  if (n <= 0)
  {
    h += F("<li><span class=n>");
    h += (n == -1) ? F("scanning\xE2\x80\xA6 reload in a moment") : F("none found");
    h += F("</span></li>");
  }
  else
  {
    if (n > 15) n = 15;
    for (int i = 0; i < n; i++)
    {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      h += "<li><a class='n pick' data-ssid=\"" + htmlEscape(ssid) + "\">";
      h += htmlEscape(ssid);
      h += "</a><span class=m>";
      h += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? F("open ") : F("\xF0\x9F\x94\x92 ");
      h += String(WiFi.RSSI(i)) + " dBm</span></li>";
    }
  }
  h += F("</ul><p style='margin:10px 0 0'><a href=/scan>Rescan</a></p></section>");

  // ---- saved networks
  h += F("<section><h2>Saved networks</h2><ul>");
  if (!credCount)
  {
    h += F("<li><span class=n>none yet</span></li>");
  }
  else
  {
    for (uint8_t i = 0; i < credCount; i++)
    {
      h += "<li><span class=n>" + htmlEscape(credSsid[i]) + "</span>";
      h += "<a class=del href=/del?i=" + String(i) + ">remove</a></li>";
    }
  }
  h += "</ul><p class=sub style='margin:10px 0 0'>Tried in order, " +
       String(NET_MAX_CREDS) + " max.</p></section>";

  // The SSID travels as attribute data and is read back with dataset, so it is
  // never parsed as JavaScript source. See F6 note above.
  h += F("<script>document.querySelectorAll('a.pick').forEach(function(a){"
         "a.addEventListener('click',function(){"
         "document.getElementById('ssid').value=a.dataset.ssid;"
         "document.getElementById('pass').value='';"
         "document.getElementById('pass').focus();});});</script>"
         "</main></body></html>");

  web->send(200, "text/html; charset=utf-8", h);
}

// A page hit means someone is actually configuring, which is what makes an open AP
// acceptable. Without this the 5-minute timeout bounded nothing: with one stored
// network out of range, netStopPortal() -> beginConnect(0) -> list exhausted ->
// netStartPortal() reset the clock, so the radio alternated 300 s of open AP with
// ~10 s of connecting, indefinitely.
static void portalNoteRequest() { portalSawRequest = true; }

static void portalHandleRoot()  { portalNoteRequest(); portalSendPage(""); }

static void portalHandleScan()
{
  portalNoteRequest();
  // An explicit request, so honour it even though it briefly leaves the channel --
  // but never mid-join: a scan aborts an in-flight WiFi.begin(), which made
  // freshly entered credentials look wrong.
  if (!joinInFlight() && WiFi.scanComplete() != -1) WiFi.scanNetworks(true, false);
  web->sendHeader("Location", "/", true);
  web->send(302, "text/plain", "");
}

static void portalHandleSave()
{
  portalNoteRequest();
  String ssid = web->arg("ssid");
  String pass = web->arg("pass");

  ssid.trim();
  if (!ssid.length())
  {
    portalSendPage("");
    return;
  }

  int8_t slot = credUpsert(ssid.c_str(), pass.c_str());
  portalSendPage("Saved \"" + ssid + "\" - connecting, watch the radio screen.");

  // Configuring a network means they want to use it, so promote a setup-only
  // portal into the transport actually being enabled (and persisted).
  if (setupOnly)
  {
    setupOnly = false;
    enabled = true;
    saveEnabled();
  }
  if (slot >= 0) beginConnect((uint8_t)slot);
}

// Accepts only a plain decimal index that actually exists. toInt() maps any
// non-numeric argument to 0, which silently deleted the first saved network.
static void portalHandleDel()
{
  portalNoteRequest();
  if (web->hasArg("i"))
  {
    String a = web->arg("i");
    bool numeric = a.length() > 0 && a.length() <= 3;
    for (size_t i = 0; numeric && i < a.length(); i++)
      if (!isdigit((unsigned char)a[i])) numeric = false;
    if (numeric)
    {
      long idx = a.toInt();
      if (idx >= 0 && idx < (long)credCount) credDelete((uint8_t)idx);
    }
  }
  web->sendHeader("Location", "/", true);
  web->send(302, "text/plain", "");
}

static void portalHandlePort()
{
  portalNoteRequest();
  if (web->hasArg("port"))
  {
    long p = web->arg("port").toInt();
    // Refuse the portal's own listener. Both would bind under SO_REUSEADDR with no
    // conflict reported, so whichever bound last won the wildcard: browsers got fed
    // to the CAT parser, or CAT clients got HTTP back. catPort lives in the wifinet
    // namespace, so that state was unreachable from the very UI it broke.
    if (p >= 1 && p <= 65535 && p != PORTAL_HTTP_PORT && (uint16_t)p != catPort)
    {
      catPort = (uint16_t)p;
      savePort();
    }
  }
  web->sendHeader("Location", "/", true);
  web->send(302, "text/plain", "");
}

// Any unknown request is bounced to the portal root. This is what makes the
// "sign in to network" prompt appear on Android, iOS and Windows, all of which
// probe well-known URLs and expect something other than a clean 204/200.
static void portalHandleNotFound()
{
  portalNoteRequest();
  web->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  web->send(302, "text/plain", "");
}

void netStartPortal()
{
  // Raising the AP and settling the state are separate steps: this is also the
  // landing point when a connect attempt fails while the portal is already up,
  // and in that case the state still has to move out of NET_CONNECTING or the
  // timeout branch re-fires on every single poll.
  if (!portalOpen)
  {
    WiFi.mode(WIFI_AP_STA);          // AP_STA so we can keep scanning
    radioOn = true;
    powerDownPending = false;        // deliberately bringing the radio back up
    WiFi.softAP(NET_AP_SSID);

    // The setup portal has to be reachable from a phone across a room, so it gets
    // the same power as the station. (Before this it was left at the chip default
    // because the cap was only applied in onConnected().)
    WiFi.setTxPower(WIFI_POWER_17dBm);

    // Allocated once and reused; see netStopPortal() for why they are never freed.
    if (!dns)
    {
      dns = new DNSServer();
      dns->setErrorReplyCode(DNSReplyCode::NoError);
    }
    dns->start(53, "*", WiFi.softAPIP());

    if (!web)
    {
      web = new WebServer(PORTAL_HTTP_PORT);
      web->on("/", HTTP_GET, portalHandleRoot);
      web->on("/scan", HTTP_GET, portalHandleScan);
      web->on("/save", HTTP_POST, portalHandleSave);
      web->on("/del", HTTP_GET, portalHandleDel);
      web->on("/port", HTTP_POST, portalHandlePort);
      web->onNotFound(portalHandleNotFound);
    }
    web->begin(PORTAL_HTTP_PORT);
    web->enableDelay(false);          // the core sleeps 1 ms per handleClient() otherwise

    portalOpen = true;
    portalSawRequest = false;

    // A scan takes the station off-channel for seconds, which drops a live CAT link
    // -- precisely what the comment below promises not to do -- and aborts an
    // in-flight WiFi.begin(), which made freshly typed credentials look wrong. Skip
    // it while associated or associating; the page still offers a Rescan link.
    if (WiFi.status() != WL_CONNECTED && !joinInFlight())
      WiFi.scanNetworks(true, false);
  }

  // Opening the portal from the menu while already online must not knock the
  // station (and therefore the CAT server) offline.
  if (WiFi.status() != WL_CONNECTED) state = NET_PORTAL;

  // Restart the idle clock: whoever landed here is actively configuring.
  portalSince = millis();
  portalGraceUntil = 0;
}

// Opens the portal for configuration without switching the transport on. The
// WiFi Cfg menu entry used to call netSetEnabled(true), which persisted the
// transport as enabled -- so merely looking at the setup page left the WiFi radio
// running for good.
void netOpenSetup()
{
  if (!enabled) setupOnly = true;
  netStartPortal();
}

// Take the WiFi hardware down.
//
// Ordering is the whole point. The CAT task owns the TCP sockets and runs on the
// other core, so it must be out of the socket path before esp_wifi_stop() frees the
// lwIP state underneath it: move the state somewhere pollNet() refuses to serve
// first, then wait on the barrier, and only then pull the power. Doing this inline
// -- as every caller used to -- meant a menu toggle could land a use-after-free in
// an accept() or read() already in flight.
static void netPowerDown()
{
  state = NET_OFF;

  // The barrier can genuinely miss: the CAT task can sit in a socket write for ten
  // seconds against a peer that has stopped reading, and no wait we can hold the
  // radio loop for will outlast that. Powering WiFi down on a miss would free lwIP
  // under it -- the very use-after-free this exists to prevent -- so leave the RF up
  // and retry from netPoll() instead. state is already NET_OFF, so nothing is served
  // in the meantime, and the task releases everything the moment it unblocks.
  if (!catNetQuiesce(powerDownPending ? 20 : 500))
  {
    powerDownPending = true;
    powerDownRetryAt = millis() + 250;
    return;
  }
  powerDownPending = false;

  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  WiFi.scanDelete();                 // a few KB of scan results nobody can see now
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);               // stop the RF entirely: no beacons, no scan
  radioOn = false;
}

// Listeners and AP down, nothing more. Split out of netStopPortal() because the
// suspend and disable paths must not run its tail: that tail can reach
// netSetEnabled(false), which writes `en`=false to NVS -- so plugging in a USB cable
// while the portal was up permanently erased the user's WiFi CAT preference, which is
// precisely what suspending is supposed to preserve.
static void portalShutdown()
{
  // Stop the listeners but never free them. DNSServer::start() registers an AsyncUDP
  // callback capturing `this`, serviced on the async_udp task, and DNSServer::stop()
  // only closes the socket -- the lwIP receive callback is unregistered in the
  // destructor. Deleting from the loop task while a captive-portal probe was in
  // flight was a use-after-free, and teardown happens exactly when a phone is
  // hammering those probes. A few hundred bytes held for the session is the right
  // trade for that.
  if (web) web->stop();
  if (dns) dns->stop();
  portalOpen = false;

  WiFi.softAPdisconnect(true);
  // Drop the AP interface entirely rather than idling in AP_STA.
  if (WiFi.status() == WL_CONNECTED) WiFi.mode(WIFI_STA);
  if (state == NET_PORTAL) state = NET_OFF;
  portalGraceUntil = 0;
}

void netStopPortal()
{
  portalShutdown();

  // Opened purely to configure WiFi: put the hardware back to sleep rather than
  // leaving a station associated for a transport nobody switched on.
  if (setupOnly)
  {
    setupOnly = false;
    // ...or tethered, where the transport may have been switched on but the hardware
    // still has to go back down when the portal closes.
    if (!enabled || suspended) netPowerDown();
    return;
  }

  // Called from netSetEnabled(false) as part of shutting the transport down;
  // `enabled` is already clear by then, so do not resurrect anything.
  if (!enabled) return;

  // Tethered. The portal is only ever up in this state because the user asked for it
  // from the menu; the automatic suspend takes back over the moment it closes, and
  // nothing below may start a join or write the preference away.
  if (suspended) { netPowerDown(); return; }

  if (WiFi.status() == WL_CONNECTED)
  {
    state = NET_CONNECTED;      // e.g. the portal was opened from the menu
  }
  else if (credCount && portalIdleCycles < 3)
  {
    beginConnect(0);            // something to try: go back to connecting
  }
  else
  {
    // Nothing configured and the portal is gone. Switch the transport off
    // rather than leave the badge lit with nothing serving.
    netSetEnabled(false);
  }
}

bool netPortalActive() { return portalOpen; }
bool netRadioOn()      { return radioOn; }

// ------------------------------------------------------------- state machine
static void beginConnect(uint8_t idx)
{
  if (!credCount) { netStartPortal(); return; }
  if (idx >= credCount) { netStartPortal(); return; }

  // Reconfiguring the station interface invalidates every open socket, and the CAT
  // task services those from the other core -- so fence it out first. Best effort
  // only: we are usually here *because* the link died, so the sockets it may still be
  // holding are already dead, and refusing to reconnect would be worse.
  catNetQuiesce(200);
  powerDownPending = false;      // we are deliberately bringing the radio back up

  tryIdx = idx;
  if (!portalOpen) WiFi.mode(WIFI_STA);
  radioOn = true;

  WiFi.setHostname(NET_HOSTNAME);
  WiFi.begin(credSsid[tryIdx], credPass[tryIdx]);

  state = NET_CONNECTING;
  stateSince = millis();
  joinUntil  = millis() + NET_CONNECT_MS;
}

static void onConnected()
{
  state = NET_CONNECTED;
  joinUntil = 0;              // the association window is over; scans are safe again

  // TX power was capped at 11 dBm here to limit self-generated noise on AM/SW.
  // Measured consequence: 65% packet loss to the AP, which made CAT unusable --
  // replies arrived in 8-34 ms when they arrived at all. A control link that drops
  // two thirds of its packets is worse than a slightly noisier receiver, and the
  // user already has a clean answer for the noise: switch the transport off.
  // 17 dBm keeps a little margin off maximum without starving the uplink.
  WiFi.setTxPower(WIFI_POWER_17dBm);

  // Turn modem sleep off. With it on, the station only wakes on the AP's DTIM
  // beacon, which measured a 261 ms median CAT round trip -- fine for Hamlib's
  // second-long timeouts, but Flrig waits tcpip_ping_delay (200 ms by default)
  // and declares the rig dead. A control link needs latency in the low
  // milliseconds; the cost is roughly 80 mA more draw while WiFi CAT is on,
  // which is the user's explicit choice when enabling this transport.
  WiFi.setSleep(false);

  if (!mdnsUp && MDNS.begin(NET_HOSTNAME))
  {
    MDNS.addService("cat", "tcp", catPort);
    mdnsUp = true;
  }

  // If the portal is up it was probably how we got here; leave it briefly so the
  // browser can render the result page before the AP disappears.
  if (portalOpen) portalGraceUntil = millis() + NET_PORTAL_GRACE;
}

void netBegin()
{
  loadAll();
  if (!enabled || suspended) { state = NET_OFF; return; }
  if (credCount) beginConnect(0);
  else netStartPortal();
}

void netPoll()
{
  // A power-down the CAT barrier would not grant yet. Nothing is served while this is
  // pending (state is NET_OFF), so just keep asking until the task lets go.
  if (powerDownPending)
  {
    if ((int32_t)(millis() - powerDownRetryAt) >= 0) netPowerDown();
    if (powerDownPending) return;
  }

  // Service the portal whenever it exists, whatever the station state is.
  if (portalOpen)
  {
    // DNSServer::processNextRequest() is a no-op here; AsyncUDP serves DNS itself.
    // handleClient() can block while it waits on a request body, which parks the
    // radio loop. CAT is unaffected -- it runs on its own task now -- but the
    // encoder still relies on the ISR accumulator to not lose detents here.
    web->handleClient();

    // Nobody finished the setup: take the open access point back down.
    if (!portalGraceUntil && millis() - portalSince > NET_PORTAL_MS)
    {
      if (portalSawRequest) portalIdleCycles = 0;
      else                  portalIdleCycles++;
      netStopPortal();            // decides what happens next
      if (!enabled) return;
    }
  }

  // `suspended` means "do not bring WiFi up by ourselves", not "stop servicing what
  // is already up". netOpenSetup() can legitimately raise the portal while tethered,
  // and skipping the state machine in that case left the hardware powered but never
  // advancing: associated with no CAT server, or amber forever. Once the portal
  // closes, netStopPortal() powers it back down and this gate re-engages.
  if (suspended && !radioOn) return;
  if (!enabled && !setupOnly) return;

  // The station can finish associating while we are in any state. Opening the
  // setup portal during a join used to leave NET_CONNECTING behind for good, so
  // onConnected() never ran: no CAT server, modem sleep left on, no mDNS, for the
  // rest of the session.
  if (state != NET_CONNECTED && WiFi.status() == WL_CONNECTED) onConnected();

  switch (state)
  {
    case NET_CONNECTING:
      if (WiFi.status() == WL_CONNECTED)
      {
        onConnected();
      }
      else if (millis() - stateSince > NET_CONNECT_MS)
      {
        // Same reason as beginConnect(): this drops the station's sockets, which the
        // CAT task holds on the other core.
        catNetQuiesce(200);
        WiFi.disconnect();
        joinUntil = 0;
        if (tryIdx + 1 < credCount) beginConnect(tryIdx + 1);
        else netStartPortal();          // whole list exhausted
      }
      break;

    case NET_CONNECTED:
      if (WiFi.status() != WL_CONNECTED)
      {
        mdnsUp = false;
        beginConnect(0);
      }
      else if (portalGraceUntil && (int32_t)(millis() - portalGraceUntil) >= 0)
      {
        netStopPortal();                // keeps NET_CONNECTED, we are online
      }
      break;

    case NET_PORTAL:
      // Keep trying the saved networks. This state used to be terminal, so opening
      // the portal during a join -- which the WiFi Cfg menu entry does -- parked a
      // correctly configured radio with no CAT server until the portal timed out.
      //
      // One network per cycle, walking the list. Retrying only slot 0 meant a radio
      // whose first saved network was out of range never left the portal by itself,
      // even with a known network right in front of it.
      if (credCount && (!portalRetryAt || (int32_t)(millis() - portalRetryAt) >= 0))
      {
        portalRetryAt = millis() + 30000;
        if (portalRetryIdx >= credCount) portalRetryIdx = 0;
        tryIdx = portalRetryIdx;
        WiFi.begin(credSsid[portalRetryIdx], credPass[portalRetryIdx]);
        // The state stays NET_PORTAL here, so joinInFlight() is what keeps a Rescan
        // from aborting this association.
        joinUntil = millis() + NET_CONNECT_MS;
        portalRetryIdx++;
      }
      break;

    case NET_OFF:
    default:
      break;
  }
}

// ------------------------------------------------------------------ public API
bool netEnabled()   { return enabled; }
bool netSuspended() { return suspended; }

void netSetSuspended(bool sus)
{
  if (suspended == sus) return;
  suspended = sus;

  if (sus)
  {
    // Same teardown as disabling, but `enabled` and NVS are left alone so the
    // user's choice survives and resumes when the cable comes out.
    // portalShutdown(), not netStopPortal(): the tail of the latter can reach
    // netSetEnabled(false) and persist the transport as disabled, so plugging in a
    // cable would erase the very preference this is meant to keep.
    setupOnly = false;
    portalShutdown();
    netPowerDown();
  }
  else if (enabled)
  {
    if (credCount) beginConnect(0);
    else           netStartPortal();
  }
}

void netSetEnabled(bool en)
{
  if (enabled == en) return;

  enabled = en;
  saveEnabled();

  if (en)
  {
    // Tethered: keep the stored preference, leave the hardware down. netPoll() skips
    // the state machine while suspended, so bringing the station up here parked it in
    // NET_CONNECTING for good -- onConnected() never ran, so there was no CAT server
    // and no mDNS, modem sleep stayed on, the icon sat amber, and the RF was running,
    // which is the one thing the USB suspend exists to prevent. netSetSuspended()
    // brings it up by itself when the cable comes out.
    if (suspended) { state = NET_OFF; return; }
    if (credCount) beginConnect(0);
    else netStartPortal();
  }
  else
  {
    setupOnly = false;
    portalShutdown();      // the tail would only try to resurrect what we are closing
    netPowerDown();
  }
}

NetState netGetState() { return state; }
uint8_t  netCredCount() { return credCount; }
uint16_t netGetCatPort() { return catPort; }

String netIP()
{
  if (state == NET_CONNECTED) return WiFi.localIP().toString();
  if (portalOpen) return WiFi.softAPIP().toString();
  return String();
}

String netApIP()
{
  return WiFi.softAPIP().toString();
}

String netSSID()
{
  if (state == NET_CONNECTED) return WiFi.SSID();
  if (portalOpen) return String(NET_AP_SSID);
  return String();
}

String netStatusShort()
{
  switch (state)
  {
    case NET_CONNECTED:  return WiFi.localIP().toString();
    case NET_CONNECTING: return String("conn...");
    case NET_PORTAL:     return String("AP setup");
    default:             return String("off");
  }
}

String netStatusDetail()
{
  switch (state)
  {
    case NET_CONNECTED:
      return WiFi.localIP().toString() + ":" + String(catPort);
    case NET_CONNECTING:
      return String("try ") + credSsid[tryIdx];
    case NET_PORTAL:
      return String(NET_AP_SSID) + " " + WiFi.softAPIP().toString();
    default:
      return String("disabled");
  }
}
