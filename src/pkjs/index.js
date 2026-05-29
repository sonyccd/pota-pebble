/* jshint esversion: 5 */
'use strict';

var Keys = require('message_keys');

var POTA_API_URL      = 'https://api.pota.app/spot/activator';
var POLL_INTERVAL_MS  = 30000;
var MAX_SPOTS         = 30;
var DEFAULT_BAND_MASK = 0x1FFF;  // all 13 bands
var DEFAULT_MODE_MASK = 0x00FF;  // all 8 modes

/* Band frequency ranges in kHz, LSB-first to match watch bitmask */
var BANDS = [
  { min:    1800, max:    2000 },  // 0: 160m
  { min:    3500, max:    4000 },  // 1: 80m
  { min:    5332, max:    5405 },  // 2: 60m
  { min:    7000, max:    7300 },  // 3: 40m
  { min:   10100, max:   10150 },  // 4: 30m
  { min:   14000, max:   14350 },  // 5: 20m
  { min:   18068, max:   18168 },  // 6: 17m
  { min:   21000, max:   21450 },  // 7: 15m
  { min:   24890, max:   24990 },  // 8: 12m
  { min:   28000, max:   29700 },  // 9: 10m
  { min:   50000, max:   54000 },  // 10: 6m
  { min:  144000, max:  148000 },  // 11: 2m
  { min:  420000, max:  450000 }   // 12: 70cm
];

/* Mode names, LSB-first to match watch bitmask */
var MODES = ['SSB', 'CW', 'FT8', 'FT4', 'RTTY', 'AM', 'FM', 'Digital'];

/* ---- Settings (localStorage is canonical) ---- */

var bandMask = parseInt(localStorage.getItem('bandMask'), 10);
var modeMask = parseInt(localStorage.getItem('modeMask'), 10);
if (isNaN(bandMask)) bandMask = DEFAULT_BAND_MASK;
if (isNaN(modeMask)) modeMask = DEFAULT_MODE_MASK;

function saveSettings() {
  localStorage.setItem('bandMask', bandMask);
  localStorage.setItem('modeMask', modeMask);
}

/* ---- Filter helpers ---- */

function bandBitForFreq(khz) {
  for (var i = 0; i < BANDS.length; i++) {
    if (khz >= BANDS[i].min && khz <= BANDS[i].max) return i;
  }
  return -1;
}

function modeBitForMode(mode) {
  var m = normalizeMode(mode);
  var i = MODES.indexOf(m);
  return i >= 0 ? i : 7;  // unknown modes → Digital (bit 7)
}

function matchesFilter(spot) {
  var khz = parseFloat(spot.frequency) || 0;
  var bb  = bandBitForFreq(khz);
  if (bb < 0 || !((bandMask >> bb) & 1)) return false;
  var mb  = modeBitForMode(spot.mode);
  return !!((modeMask >> mb) & 1);
}

function normalizeMode(mode) {
  var m = (mode || '').toUpperCase().trim();
  if (m === 'LSB' || m === 'USB') return 'SSB';
  return m;
}

function spotKey(spot) {
  return spot.spotId != null ? String(spot.spotId) : (spot.activator + '_' + spot.frequency);
}

/* ---- AppMessage send queue ---- */

var msgQueue = [];
var msgBusy  = false;
var stopCount = 0;  // incremented on stopPolling; guards against stale ACK callbacks

function sendNext() {
  if (msgBusy || msgQueue.length === 0) return;
  msgBusy = true;
  var msg = msgQueue.shift();
  var capturedStop = stopCount;
  Pebble.sendAppMessage(msg,
    function() {
      if (stopCount !== capturedStop) return;  // stopPolling fired; discard
      msgBusy = false;
      sendNext();
    },
    function(e) {
      if (stopCount !== capturedStop) return;
      console.log('[POTA] Send failed: ' + JSON.stringify(e));
      msgBusy = false;
      sendNext();
    }
  );
}

function queueMessage(dict) {
  msgQueue.push(dict);
  sendNext();
}

/* ---- Settings sync to watch ---- */

function sendSettingsSync() {
  var msg = {};
  msg[Keys.SETTINGS_SYNC] = 1;
  msg[Keys.BAND_MASK]     = bandMask;
  msg[Keys.MODE_MASK]     = modeMask;
  queueMessage(msg);
}

/* ---- Batch sender ---- */

function sendBatch(spots, newCount) {
  var startMsg = {};
  startMsg[Keys.SPOTS_BATCH_START] = spots.length;
  queueMessage(startMsg);

  spots.forEach(function(s, i) {
    var msg = {};
    msg[Keys.SPOTS_BATCH_ITEM] = 1;
    msg[Keys.SPOT_INDEX]       = i;
    msg[Keys.SPOT_ID]          = spotKey(s);
    msg[Keys.SPOT_CALLSIGN]    = s.activator || '';
    msg[Keys.SPOT_FREQ]        = String(s.frequency || '');
    msg[Keys.SPOT_MODE]        = normalizeMode(s.mode);
    msg[Keys.SPOT_PARK_REF]    = s.reference || '';
    msg[Keys.SPOT_LOCATION]    = (s.name || s.locationDesc || '').substring(0, 60);
    msg[Keys.SPOT_COMMENT]     = (s.comments || '').substring(0, 60);
    msg[Keys.SPOT_TIMESTAMP]   = (Math.floor(new Date(s.spotTime).getTime() / 1000)) | 0;
    queueMessage(msg);
  });

  var endMsg = {};
  endMsg[Keys.SPOTS_BATCH_END]  = 1;
  endMsg[Keys.NEW_SPOT_COUNT]   = newCount;
  queueMessage(endMsg);
}

/* ---- Polling ---- */

var pollTimer   = null;
var lastSpotIds = {};
var isFirstPoll = true;

function fetchAndProcess() {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', POTA_API_URL, true);
  try { xhr.setRequestHeader('User-Agent', 'POTAPebble/0.1 (KK4PWJ)'); } catch (ignore) {}

  xhr.onload = function() {
    if (xhr.status !== 200) {
      console.log('[POTA] API error status=' + xhr.status);
      return;
    }
    var all;
    try { all = JSON.parse(xhr.responseText); } catch (e) {
      console.log('[POTA] JSON parse error: ' + e);
      return;
    }
    if (!Array.isArray(all)) return;

    var filtered = all.filter(matchesFilter);

    filtered.sort(function(a, b) {
      return new Date(b.spotTime) - new Date(a.spotTime);
    });
    if (filtered.length > MAX_SPOTS) filtered = filtered.slice(0, MAX_SPOTS);

    var newIds   = {};
    var newCount = 0;
    filtered.forEach(function(s) {
      var id = spotKey(s);
      newIds[id] = true;
      if (!isFirstPoll && !lastSpotIds[id]) newCount++;
    });

    console.log('[POTA] total=' + all.length + ' filtered=' + filtered.length +
                ' new=' + newCount + (isFirstPoll ? ' (first)' : ''));

    lastSpotIds = newIds;
    isFirstPoll = false;

    sendBatch(filtered, newCount);
  };

  xhr.onerror = function() { console.log('[POTA] Network error'); };
  xhr.send();
}

function startPolling() {
  if (pollTimer !== null) return;
  /* Preserve lastSpotIds across brief stops (e.g. opening detail view) so
     new-spot vibration isn't suppressed on the first poll after returning.
     Only treat as a fresh start when we have no prior state at all. */
  if (Object.keys(lastSpotIds).length === 0) {
    isFirstPoll = true;
  }
  msgQueue = [];
  msgBusy  = false;
  fetchAndProcess();
  pollTimer = setInterval(fetchAndProcess, POLL_INTERVAL_MS);
  console.log('[POTA] Polling started');
}

function stopPolling() {
  if (pollTimer !== null) { clearInterval(pollTimer); pollTimer = null; }
  stopCount++;  // invalidates any in-flight sendAppMessage callbacks
  msgQueue = [];
  msgBusy  = false;
  console.log('[POTA] Polling stopped');
}

/* ---- Pebble event wiring ---- */

Pebble.addEventListener('ready', function() {
  console.log('[POTA] JS ready — bandMask=' + bandMask + ' modeMask=' + modeMask);
  sendSettingsSync();
});

Pebble.addEventListener('appmessage', function(e) {
  var dict = e.payload;
  if (dict.hasOwnProperty(Keys.POLL_START)) {
    startPolling();
  } else if (dict.hasOwnProperty(Keys.POLL_STOP)) {
    stopPolling();
  } else if (dict.hasOwnProperty(Keys.SETTINGS_UPDATE)) {
    bandMask = dict[Keys.BAND_MASK] !== undefined ? dict[Keys.BAND_MASK] : bandMask;
    modeMask = dict[Keys.MODE_MASK] !== undefined ? dict[Keys.MODE_MASK] : modeMask;
    saveSettings();
    console.log('[POTA] Settings updated: bands=' + bandMask + ' modes=' + modeMask);
  }
});
