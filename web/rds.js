'use strict';
/*
  RDS / RBDS decoder (IEC 62106 / NRSC-4). Fed raw groups as the firmware sends
  them over ZZRG: 18 hex chars "AAAABBBBCCCCDDDDEE", EE packing the four 2-bit
  block error levels with A in the top bits. The tuner is configured to drop
  groups with uncorrectable blocks, so every group that reaches here is usable.

  Decoded: PI (country, coverage, programme ref, RBDS call letters), PTY, TP, TA,
  MS, DI, PS (with confirmation), AF list (method A and B), ECC, language, PIN,
  TMC id, RadioText A/B, RT+, ODA directory, clock time, PTYN, EON, Long PS,
  15B fast tuning; plus group statistics and a raw log.
*/

const RDS = (() => {
  // ---------------------------------------------------------------- tables
  const PTY_EU = ['None', 'News', 'Current affairs', 'Information', 'Sport', 'Education', 'Drama',
    'Culture', 'Science', 'Varied', 'Pop music', 'Rock music', 'Easy listening', 'Light classical',
    'Serious classical', 'Other music', 'Weather', 'Finance', "Children's programmes", 'Social affairs',
    'Religion', 'Phone-in', 'Travel', 'Leisure', 'Jazz music', 'Country music', 'National music',
    'Oldies music', 'Folk music', 'Documentary', 'Alarm test', 'Alarm'];
  const PTY_NA = ['None', 'News', 'Information', 'Sports', 'Talk', 'Rock', 'Classic rock', 'Adult hits',
    'Soft rock', 'Top 40', 'Country', 'Oldies', 'Soft', 'Nostalgia', 'Jazz', 'Classical',
    'Rhythm and blues', 'Soft rhythm and blues', 'Language', 'Religious music', 'Religious talk',
    'Personality', 'Public', 'College', 'Spanish talk', 'Spanish music', 'Hip hop', 'Unassigned',
    'Unassigned', 'Weather', 'Emergency test', 'Emergency'];

  // European Broadcasting Area: ECC -> PI country nibble (1..F) -> ISO country.
  const ECC = {
    0xE0: [, 'DE', 'DZ', 'AD', 'IL', 'IT', 'BE', 'RU', 'PS', 'AL', 'AT', 'HU', 'MT', 'DE', , 'EG'],
    0xE1: [, 'GR', 'CY', 'SM', 'CH', 'JO', 'FI', 'LU', 'BG', 'DK', 'GI', 'IQ', 'GB', 'LY', 'RO', 'FR'],
    0xE2: [, 'MA', 'CZ', 'PL', 'VA', 'SK', 'SY', 'TN', , 'LI', 'IS', 'MC', 'LT', 'RS', 'ES', 'NO'],
    0xE3: [, 'ME', 'IE', 'TR', 'MK', , , , 'NL', 'LV', 'LB', 'AZ', 'HR', 'KZ', 'SE', 'BY'],
    0xE4: [, 'MD', 'EE', 'KG', , , 'UA', 'XK', 'PT', 'SI', 'AM', 'UZ', 'GE', , 'TM', 'BA'],
  };
  const COUNTRY = {
    AD: 'Andorra', AL: 'Albania', AM: 'Armenia', AT: 'Austria', AZ: 'Azerbaijan', BA: 'Bosnia and Herzegovina',
    BE: 'Belgium', BG: 'Bulgaria', BY: 'Belarus', CH: 'Switzerland', CY: 'Cyprus', CZ: 'Czechia', DE: 'Germany',
    DK: 'Denmark', DZ: 'Algeria', EE: 'Estonia', EG: 'Egypt', ES: 'Spain', FI: 'Finland', FR: 'France',
    GB: 'United Kingdom', GE: 'Georgia', GI: 'Gibraltar', GR: 'Greece', HR: 'Croatia', HU: 'Hungary',
    IE: 'Ireland', IL: 'Israel', IQ: 'Iraq', IS: 'Iceland', IT: 'Italy', JO: 'Jordan', KG: 'Kyrgyzstan',
    KZ: 'Kazakhstan', LB: 'Lebanon', LI: 'Liechtenstein', LT: 'Lithuania', LU: 'Luxembourg', LV: 'Latvia',
    LY: 'Libya', MA: 'Morocco', MC: 'Monaco', MD: 'Moldova', ME: 'Montenegro', MK: 'North Macedonia',
    MT: 'Malta', NL: 'Netherlands', NO: 'Norway', PL: 'Poland', PS: 'Palestine', PT: 'Portugal',
    RO: 'Romania', RS: 'Serbia', RU: 'Russia', SE: 'Sweden', SI: 'Slovenia', SK: 'Slovakia', SM: 'San Marino',
    SY: 'Syria', TM: 'Turkmenistan', TN: 'Tunisia', TR: 'Türkiye', UA: 'Ukraine', UZ: 'Uzbekistan',
    VA: 'Vatican', XK: 'Kosovo',
  };
  const LANG = ['Unknown', 'Albanian', 'Breton', 'Catalan', 'Croatian', 'Welsh', 'Czech', 'Danish', 'German',
    'English', 'Spanish', 'Esperanto', 'Estonian', 'Basque', 'Faroese', 'French', 'Frisian', 'Irish', 'Gaelic',
    'Galician', 'Icelandic', 'Italian', 'Sámi', 'Latin', 'Latvian', 'Luxembourgish', 'Lithuanian',
    'Hungarian', 'Maltese', 'Dutch', 'Norwegian', 'Occitan', 'Polish', 'Portuguese', 'Romanian', 'Romansh',
    'Serbian', 'Slovak', 'Slovene', 'Finnish', 'Swedish', 'Turkish', 'Flemish', 'Walloon'];
  const COVERAGE = ['Local', 'International', 'National', 'Supra-regional'];
  const ODA = {
    0x4BD7: 'RadioText+ (RT+)', 0x4BD8: 'RT+ for eRT', 0x6552: 'Enhanced RadioText (eRT)',
    0xCD46: 'Traffic Message Channel (TMC)', 0xCD47: 'Traffic Message Channel (TMC)',
    0x0093: 'DAB cross-referencing', 0xABCF: 'RF power monitoring (WorldCast)', 0xC3B0: 'iTunes tagging', 0x4AA1: 'RASANT',
  };
  const RTP = ['', 'Title', 'Album', 'Track no.', 'Artist', 'Composition', 'Movement', 'Conductor',
    'Composer', 'Band', 'Comment', 'Genre', 'News', 'Local news', 'Stock market', 'Sport', 'Lottery',
    'Horoscope', 'Daily diversion', 'Health', 'Event', 'Scene', 'Cinema', 'Stupidity machine',
    'Date / time', 'Weather', 'Traffic', 'Alarm', 'Advertisement', 'URL', 'Other', 'Short name',
    'Long name', 'Now on air', 'Next', 'Programme part', 'Host', 'Editorial staff', 'Frequency',
    'Homepage', 'Sub-channel', 'Hotline phone', 'Studio phone', 'Other phone', 'Studio SMS', 'Other SMS',
    'Hotline e-mail', 'Studio e-mail', 'Other e-mail', 'MMS', 'Chat', 'Chat centre', 'Vote question',
    'Vote centre', '', '', '', '', '', 'Place', 'Appointment', 'Identifier', 'Purchase', 'Get data'];

  // RDS G0 character set, 0x80..0xFF (IEC 62106 Annex E). Below 0x80 it is ASCII.
  const G0 = [
    'áàéèíìóòúùÑÇŞß¡Ĳ', // 0x80
    'âäêëîïôöûüñçşğıĳ', // 0x90
    'ªα©‰Ğěňőπ€£$←↑→↓', // 0xA0
    'º¹²³±İńűµ¿÷°¼½¾§', // 0xB0
    'ÁÀÉÈÍÌÓÒÚÙŘČŠŽÐĿ', // 0xC0
    'ÂÄÊËÎÏÔÖÛÜřčšžđŀ', // 0xD0
    'ÃÅÆŒŷÝÕØÞŊŔĆŚŹŦð', // 0xE0
    'ãåæœŵýõøþŋŕćśźŧ ', // 0xF0
  ].join('');
  const chr = (c) => (c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : c >= 0x80 ? G0[c - 0x80] : ' ');

  const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, '0');

  function afFreq(code, lfmf) {
    if (lfmf) {
      if (code >= 1 && code <= 15) return { khz: 153 + (code - 1) * 9, band: 'LF' };
      if (code >= 16 && code <= 135) return { khz: 531 + (code - 16) * 9, band: 'MF' };
      return null;
    }
    if (code >= 1 && code <= 204) return { khz: 87500 + code * 100, band: 'FM' };
    return null;
  }

  // RBDS: PI -> US call letters for the arithmetic K/W ranges.
  function callLetters(pi) {
    let n, first;
    if (pi >= 0x1000 && pi <= 0x54A7) { first = 'K'; n = pi - 0x1000; }
    else if (pi >= 0x54A8 && pi <= 0x994F) { first = 'W'; n = pi - 0x54A8; }
    else if ((pi & 0xF000) === 0xA000 || (pi & 0xFF00) === 0xAF00) return 'nationally / regionally linked';
    else return null;
    const L = (i) => String.fromCharCode(65 + i);
    return first + L(Math.floor(n / 676)) + L(Math.floor((n % 676) / 26)) + L(n % 26);
  }

  function mjdToDate(mjd) {
    const yp = Math.floor((mjd - 15078.2) / 365.25);
    const mp = Math.floor((mjd - 14956.1 - Math.floor(yp * 365.25)) / 30.6001);
    const d = mjd - 14956 - Math.floor(yp * 365.25) - Math.floor(mp * 30.6001);
    const k = mp === 14 || mp === 15 ? 1 : 0;
    return [yp + k + 1900, mp - 1 - k * 12, d];
  }

  const GROUPS = [];
  for (let t = 0; t < 16; t++) GROUPS.push(t + 'A', t + 'B');

  // ---------------------------------------------------------------- decoder
  class Decoder {
    constructor() { this.reset(); }

    reset() {
      this.pi = null; this.piCand = null;
      this.pty = null; this.tp = null; this.ta = null; this.ms = null;
      this.di = { stereo: null, head: null, compressed: null, dynPty: null };
      this.psBuf = Array(8).fill(null); this.ps = ''; this.psConfirmed = ''; this.psLastFull = '';
      this.rtBuf = Array(64).fill(null); this.rtAB = null; this.rtVer = null; this.rtEnd = 64;
      this.rt = ''; this.rtComplete = ''; this.rtFlips = 0;
      this.af = new Map(); this.afExpect = null; this.afPairs = 0; this.afPairsWithTuned = 0;
      this.ecc = null; this.lang = null; this.pin = null; this.tmcId = null; this.paging = null;
      this.ct = null; this.ctRejected = 0; this.ble = 0;
      this.ptynBuf = Array(8).fill(null); this.ptynAB = null; this.ptyn = '';
      this.odaSeen = {};
      this.oda = new Map();                   // "11A" -> {aid, name, msg}
      this.rtpGroup = null; this.rtp = { toggle: null, running: false, items: new Map() };
      this.eon = new Map();                   // PI -> {pi, ps[], af:Set, pty, ta, tp, mapped:Set}
      this.lpsBytes = Array(32).fill(null); this.lps = '';
      this.counts = Object.fromEntries(GROUPS.map((g) => [g, 0]));
      this.total = 0; this.corrected = 0; this.times = [];
      this.log = [];
      this.tunedKhz = null;
      this.changed = 0;
    }

    // tunedHz lets AF method B be recognised (each pair names the tuned frequency).
    push(hex, now = performance.now(), tunedHz = null) {
      if (hex.length !== 18) return;
      const A = parseInt(hex.slice(0, 4), 16), B = parseInt(hex.slice(4, 8), 16);
      const C = parseInt(hex.slice(8, 12), 16), D = parseInt(hex.slice(12, 16), 16);
      const E = parseInt(hex.slice(16, 18), 16);
      if ([A, B, C, D, E].some(Number.isNaN)) return;
      if (tunedHz) this.tunedKhz = Math.round(tunedHz / 100) * 100 / 1000;

      const type = B >> 12, ver = (B >> 11) & 1, name = type + (ver ? 'B' : 'A');
      this.counts[name]++; this.total++;
      if (E) this.corrected++;
      this.ble = E;
      this.times.push(now);
      while (this.times.length && now - this.times[0] > 10000) this.times.shift();
      this.log.push({ hex, name, t: now });
      if (this.log.length > 60) this.log.shift();

      // PI needs to be seen twice before it is believed.
      if (A === this.piCand) this.pi = A; else this.piCand = A;
      this.tp = !!((B >> 10) & 1);
      this.pty = (B >> 5) & 31;

      const h = this['g' + name];
      if (h) h.call(this, B, C, D);
      else if (this.rtpGroup === name) this.rtPlus(B, C, D);
      this.changed++;
    }

    // ---- 0A / 0B: basic tuning and switching information
    g0A(B, C, D) { this.basic(B, D); this.afPair(C >> 8, C & 0xFF); }
    g0B(B, C, D) { this.basic(B, D); }
    basic(B, D) {
      this.ta = !!((B >> 4) & 1);
      this.ms = (B >> 3) & 1 ? 'Music' : 'Speech';
      const seg = B & 3, bit = !!((B >> 2) & 1);
      this.di[['dynPty', 'compressed', 'head', 'stereo'][seg]] = bit;
      this.psBuf[seg * 2] = chr(D >> 8);
      this.psBuf[seg * 2 + 1] = chr(D & 0xFF);
      this.ps = this.psBuf.map((c) => c ?? '·').join('');
      if (seg === 3 && this.psBuf.every((c) => c != null)) {
        const full = this.psBuf.join('');
        // Two identical complete passes in a row: stations that scroll text through
        // PS would otherwise flash garbage halves as the station name.
        if (full === this.psLastFull) this.psConfirmed = full;
        this.psLastFull = full;
      }
    }
    afPair(a, b) {
      for (const code of [a, b]) {
        if (code >= 224 && code <= 249) { this.afExpect = code - 224; this.afLfmf = false; continue; }
        if (code === 250) { this.afLfmf = true; continue; }
        if (code === 205 || code === 0) continue;
        const f = afFreq(code, this.afLfmf);
        this.afLfmf = false;
        if (f) {
          const k = f.band === 'FM' ? f.khz / 1000 : f.khz;
          const e = this.af.get(f.khz) || { ...f, seen: 0 };
          e.seen++; this.af.set(f.khz, e);
          if (f.band === 'FM' && this.tunedKhz != null && Math.abs(k - this.tunedKhz) < 0.05) this.afPairsWithTuned++;
        }
      }
      this.afPairs++;
    }
    get afMethod() {
      if (this.afPairs < 6) return null;
      return this.afPairsWithTuned / this.afPairs > 0.6 ? 'B' : 'A';
    }

    // ---- 1A / 1B: programme item number and slow labelling codes
    g1A(B, C, D) {
      const variant = (C >> 12) & 7;
      if (variant === 0) { this.ecc = C & 0xFF; this.paging = (C >> 8) & 15; }
      else if (variant === 1) this.tmcId = C & 0xFFF;
      else if (variant === 3) this.lang = C & 0xFF;
      this.pinFrom(D);
    }
    g1B(B, C, D) { this.pinFrom(D); }
    pinFrom(D) {
      const day = D >> 11, hr = (D >> 6) & 31, min = D & 63;
      if (day) this.pin = { day, hr, min };
    }

    // ---- 2A / 2B: RadioText
    g2A(B, C, D) { this.rtChars(B, [C >> 8, C & 0xFF, D >> 8, D & 0xFF], 4, 64); }
    g2B(B, C, D) { this.rtChars(B, [D >> 8, D & 0xFF], 2, 32); }
    rtChars(B, bytes, per, maxLen) {
      const ab = (B >> 4) & 1, addr = B & 15;
      if (this.rtVer !== per) { this.rtVer = per; this.rtBuf.fill(null); this.rtEnd = maxLen; }
      if (this.rtAB !== null && ab !== this.rtAB) {
        // A/B flip: the station started a new message.
        this.rtBuf.fill(null); this.rtEnd = maxLen; this.rtFlips++;
      }
      this.rtAB = ab;
      bytes.forEach((c, i) => {
        const pos = addr * per + i;
        if (pos >= this.rtEnd) return;
        if (c === 0x0D) { this.rtEnd = pos; return; }
        this.rtBuf[pos] = chr(c);
      });
      const cells = this.rtBuf.slice(0, this.rtEnd);
      this.rt = cells.map((c) => c ?? ' ').join('').replace(/\s+$/, '');
      if (cells.every((c) => c != null)) this.rtComplete = this.rt;
    }

    // ---- 3A: Open Data Application directory
    g3A(B, C, D) {
      const app = B & 31, g = (app >> 1) + ((app & 1) ? 'B' : 'A');
      const aid = D, name = ODA[aid] || `Unknown application ${hex4(aid)}`;
      // Listed only once announced twice: on a weak signal one wrongly corrected 3A
      // otherwise invents an application.
      const key = `${app}:${aid}`;
      this.odaSeen[key] = (this.odaSeen[key] || 0) + 1;
      if (this.odaSeen[key] < 2) return;
      this.oda.set(app ? g : '3A', { aid, name, msg: C });
      if (aid === 0x4BD7 && app) this.rtpGroup = g;
    }

    // ---- RT+ (carried in the group the 3A directory names)
    rtPlus(B, C, D) {
      const toggle = (B >> 4) & 1, running = !!((B >> 3) & 1);
      if (this.rtp.toggle !== null && toggle !== this.rtp.toggle) this.rtp.items.clear();
      this.rtp.toggle = toggle; this.rtp.running = running;
      const tags = [
        [((B & 7) << 3) | (C >> 13), (C >> 7) & 63, (C >> 1) & 63],
        [((C & 1) << 5) | (D >> 11), (D >> 5) & 63, D & 31],
      ];
      const src = this.rtComplete || this.rt;
      for (const [type, start, len] of tags) {
        if (!type || !src) continue;
        const text = src.substr(start, len + 1).trim();
        if (text) this.rtp.items.set(type, text);
      }
    }

    // ---- 4A: clock time and date
    g4A(B, C, D) {
      const mjd = ((B & 3) << 15) | (C >> 1);
      const hr = ((C & 1) << 4) | (D >> 12), min = (D >> 6) & 63;
      const off = ((D >> 5) & 1 ? -1 : 1) * (D & 31) * 30;
      if (hr > 23 || min > 59 || mjd < 15079) return;
      // Time has no redundancy, and the tuner passes blocks it corrected from 3-5
      // bit errors (level 2). Take CT only from clean or lightly corrected C and D,
      // and only if the date is plausible -- one bad group once showed 1959.
      if (((this.ble >> 2) & 3) > 1 || (this.ble & 3) > 1) { this.ctRejected++; return; }
      const [y, m, d] = mjdToDate(mjd);
      if (y < 2010 || y > 2099) { this.ctRejected++; return; }
      this.ct = { utc: new Date(Date.UTC(y, m - 1, d, hr, min)), offset: off, at: Date.now() };
    }

    // ---- 10A: programme type name
    g10A(B, C, D) {
      const ab = (B >> 4) & 1, seg = B & 1;
      if (this.ptynAB !== null && ab !== this.ptynAB) this.ptynBuf.fill(null);
      this.ptynAB = ab;
      [C >> 8, C & 0xFF, D >> 8, D & 0xFF].forEach((c, i) => { this.ptynBuf[seg * 4 + i] = chr(c); });
      this.ptyn = this.ptynBuf.map((c) => c ?? ' ').join('').trim();
    }

    // ---- 14A / 14B: enhanced other networks
    eonEntry(pi) {
      let e = this.eon.get(pi);
      if (!e) { e = { pi, ps: Array(8).fill(null), af: new Set(), mapped: new Set(), pty: null, ta: null, tp: null, pin: null }; this.eon.set(pi, e); }
      e.seen = (e.seen || 0) + 1;
      return e;
    }
    g14A(B, C, D) {
      const v = B & 15, e = this.eonEntry(D);
      e.tp = !!((B >> 4) & 1);
      if (v <= 3) { e.ps[v * 2] = chr(C >> 8); e.ps[v * 2 + 1] = chr(C & 0xFF); }
      else if (v === 4) for (const code of [C >> 8, C & 0xFF]) { const f = afFreq(code, false); if (f) e.af.add(f.khz); }
      else if (v >= 5 && v <= 9) { const f = afFreq(C & 0xFF, false); if (f) e.mapped.add(f.khz); }
      else if (v === 13) { e.pty = C >> 11; e.ta = !!(C & 1); }
      else if (v === 14) { const day = C >> 11; if (day) e.pin = { day, hr: (C >> 6) & 31, min: C & 63 }; }
    }
    g14B(B, C, D) {
      const e = this.eonEntry(D);
      e.tp = !!((B >> 4) & 1); e.ta = !!((B >> 3) & 1);
    }

    // ---- 15A: long PS (RDS2) / 15B: fast basic tuning
    g15A(B, C, D) {
      const seg = B & 7;
      [C >> 8, C & 0xFF, D >> 8, D & 0xFF].forEach((c, i) => { this.lpsBytes[seg * 4 + i] = c; });
      const bytes = [];
      for (const b of this.lpsBytes) { if (b === null || b === 0x0D) break; bytes.push(b); }
      try { this.lps = new TextDecoder('utf-8', { fatal: false }).decode(new Uint8Array(bytes)).trim(); } catch { }
    }
    g15B(B, C, D) {
      this.ta = !!((B >> 4) & 1);
      this.ms = (B >> 3) & 1 ? 'Music' : 'Speech';
      this.di[['dynPty', 'compressed', 'head', 'stereo'][B & 3]] = !!((B >> 2) & 1);
    }

    // ---------------------------------------------------------- derived views
    get rate() {
      if (this.times.length < 2) return 0;
      return (this.times.length - 1) / ((this.times[this.times.length - 1] - this.times[0]) / 1000 || 1);
    }

    piInfo(region) {
      if (this.pi == null) return null;
      const pi = this.pi, cn = pi >> 12, area = (pi >> 8) & 15, ref = pi & 0xFF;
      const info = {
        hex: hex4(pi), countryNibble: cn.toString(16).toUpperCase(),
        coverage: area < 4 ? COVERAGE[area] : `Regional ${area - 3}`, ref,
      };
      if (region === 'NA') info.call = callLetters(pi);
      const iso = this.ecc != null && ECC[this.ecc] ? ECC[this.ecc][cn] : null;
      if (iso) info.country = `${COUNTRY[iso] || iso} (${iso})`;
      // No ECC yet (many stations never send group 1A): the PI nibble alone narrows
      // it to one country per ECC table.
      info.candidates = Object.values(ECC).map((row) => row[cn]).filter(Boolean).map((c) => COUNTRY[c] || c);
      return info;
    }

    ptyName(region, code = this.pty) {
      return code == null ? null : (region === 'NA' ? PTY_NA : PTY_EU)[code];
    }

    get language() { return this.lang == null ? null : (LANG[this.lang] || `code ${this.lang.toString(16)}`); }
    get eccHex() { return this.ecc == null ? null : this.ecc.toString(16).toUpperCase(); }

    get rtPlusItems() {
      return [...this.rtp.items].map(([t, text]) => ({ type: t, label: RTP[t] || `Tag ${t}`, text }));
    }

    get afList() {
      return [...this.af.values()].sort((a, b) => (a.band === b.band ? a.khz - b.khz : a.band < b.band ? -1 : 1));
    }

    get eonList() {
      // Seen twice and carrying something: one wrongly corrected 14A otherwise
      // invents an empty network.
      return [...this.eon.values()].filter((e) => e.seen >= 2 &&
        (e.ps.some((c) => c) || e.af.size || e.mapped.size || e.pty != null)).map((e) => ({
        ...e, piHex: hex4(e.pi), psText: e.ps.some((c) => c) ? e.ps.map((c) => c ?? ' ').join('').trim() : '',
      }));
    }
  }

  return { Decoder, GROUPS, hex4 };
})();
