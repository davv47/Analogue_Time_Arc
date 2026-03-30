// index.js — PebbleKit JS
console.log("index.js v11 loaded");

var MAX_CALENDARS = 10;

// Cached geolocation position
var s_last_lat = null;
var s_last_lng = null;

// ─── Weather ──────────────────────────────────────────────────────────────────

function fetchWeather(latitude, longitude, useFahrenheit) {
    var unit = useFahrenheit ? "fahrenheit" : "celsius";
    var url = "https://api.open-meteo.com/v1/forecast"
        + "?latitude=" + latitude
        + "&longitude=" + longitude
        + "&current=temperature_2m,weather_code"
        + "&temperature_unit=" + unit;

    var xhr = new XMLHttpRequest();
    xhr.open("GET", url, true);
    xhr.onload = function() {
        if (xhr.status === 200) {
            try {
                var data = JSON.parse(xhr.responseText);
                var temp = Math.round(data.current.temperature_2m);
                var code = data.current.weather_code;
                console.log("Weather: " + temp + "° code=" + code);
                Pebble.sendAppMessage({ 6: temp, 7: code }, function() {
                    console.log("Weather sent");
                }, function(e) {
                    console.log("Weather send failed: " + JSON.stringify(e));
                });
            } catch (e) { console.log("Weather parse error: " + e); }
        }
    };
    xhr.send();
}

// ─── ICS Parsing ─────────────────────────────────────────────────────────────

function parseICSDate(str) {
    if (!str) return null;
    str = str.replace(/^TZID=[^:]+:/, "");
    var basic = str.replace(/[-:]/g, "");
    var year   = parseInt(basic.substring(0, 4), 10);
    var month  = parseInt(basic.substring(4, 6), 10) - 1;
    var day    = parseInt(basic.substring(6, 8), 10);
    var hour   = basic.length >= 13 ? parseInt(basic.substring(9, 11), 10) : 0;
    var minute = basic.length >= 13 ? parseInt(basic.substring(11, 13), 10) : 0;
    var second = basic.length >= 15 ? parseInt(basic.substring(13, 15), 10) : 0;
    var isUTC  = str.charAt(str.length - 1) === "Z";
    return isUTC
        ? new Date(Date.UTC(year, month, day, hour, minute, second))
        : new Date(year, month, day, hour, minute, second);
}

function unfoldICS(raw) {
    return raw.replace(/\r\n[ \t]/g, "").replace(/\n[ \t]/g, "");
}

function parseDuration(str) {
    var ms = 0;
    var weeks   = str.match(/(\d+)W/);
    var days    = str.match(/(\d+)D/);
    var hours   = str.match(/(\d+)H/);
    var minutes = str.match(/(\d+)M/);
    var seconds = str.match(/(\d+)S/);
    if (weeks)   ms += parseInt(weeks[1],   10) * 7 * 24 * 60 * 60 * 1000;
    if (days)    ms += parseInt(days[1],    10) * 24 * 60 * 60 * 1000;
    if (hours)   ms += parseInt(hours[1],   10) * 60 * 60 * 1000;
    if (minutes) ms += parseInt(minutes[1], 10) * 60 * 1000;
    if (seconds) ms += parseInt(seconds[1], 10) * 1000;
    return ms;
}

// ─── RRULE parsing ───────────────────────────────────────────────────────────
// Supports FREQ=DAILY/WEEKLY/MONTHLY/YEARLY, INTERVAL, COUNT, UNTIL, BYDAY.
// Returns an array of occurrence start Dates within [now, cutoff].

var BYDAY_MAP = { SU: 0, MO: 1, TU: 2, WE: 3, TH: 4, FR: 5, SA: 6 };

function parseRRule(rruleStr) {
    var rule = {};
    var parts = rruleStr.split(";");
    for (var i = 0; i < parts.length; i++) {
        var eq = parts[i].indexOf("=");
        if (eq < 0) continue;
        var k = parts[i].substring(0, eq).toUpperCase();
        var v = parts[i].substring(eq + 1);
        rule[k] = v;
    }
    return rule;
}

// Returns midnight (local) of the date represented by a Date object
function dateOnly(d) {
    return new Date(d.getFullYear(), d.getMonth(), d.getDate());
}

// Add calendar months safely (handles month-end overflow by clamping)
function addMonths(d, n) {
    var result = new Date(d.getTime());
    var day = result.getDate();
    result.setDate(1);
    result.setMonth(result.getMonth() + n);
    var maxDay = new Date(result.getFullYear(), result.getMonth() + 1, 0).getDate();
    result.setDate(Math.min(day, maxDay));
    return result;
}

function expandRRule(dtstart, rruleStr, now, cutoff) {
    var rule     = parseRRule(rruleStr);
    var freq     = rule.FREQ || "";
    var interval = rule.INTERVAL ? parseInt(rule.INTERVAL, 10) : 1;
    var count    = rule.COUNT    ? parseInt(rule.COUNT,    10) : -1;
    var until    = rule.UNTIL    ? parseICSDate(rule.UNTIL) : null;
    var occurrences = [];

    // BYDAY: array of day-of-week numbers (0=Sun..6=Sat)
    var byDay = [];
    if (rule.BYDAY) {
        var dayTokens = rule.BYDAY.split(",");
        for (var d = 0; d < dayTokens.length; d++) {
            // Strip optional ordinal prefix e.g. "+1MO" -> "MO"
            var token = dayTokens[d].replace(/^[+-]?\d*/, "").toUpperCase();
            if (BYDAY_MAP.hasOwnProperty(token)) byDay.push(BYDAY_MAP[token]);
        }
    }

    // Hard cap: never iterate more than 500 steps to protect against
    // runaway loops on very old recurring events with no UNTIL/COUNT.
    var MAX_ITER = 500;
    var iter     = 0;
    var cursor   = new Date(dtstart.getTime());

    while (iter++ < MAX_ITER) {
        // Stop if we have gone past cutoff or hit UNTIL
        if (cursor > cutoff) break;
        if (until && cursor > until) break;
        if (count === 0) break;

        if (freq === "WEEKLY" && byDay.length > 0) {
            // For weekly+BYDAY, check each day of the current week
            // (week starting on the same weekday as dtstart to honour WKST=MO etc.)
            // Simpler: just check all 7 days of the ISO week containing cursor.
            var weekStart = new Date(cursor.getTime());
            weekStart.setDate(weekStart.getDate() - weekStart.getDay()); // back to Sunday
            for (var wd = 0; wd < 7; wd++) {
                var candidate = new Date(weekStart.getFullYear(),
                                         weekStart.getMonth(),
                                         weekStart.getDate() + wd,
                                         dtstart.getHours(),
                                         dtstart.getMinutes(),
                                         dtstart.getSeconds());
                if (candidate < dtstart) continue;       // before series start
                if (until && candidate > until) continue;
                if (candidate > cutoff) continue;
                var inByDay = false;
                for (var b = 0; b < byDay.length; b++) {
                    if (candidate.getDay() === byDay[b]) { inByDay = true; break; }
                }
                if (!inByDay) continue;
                if (candidate >= now) {
                    occurrences.push(new Date(candidate.getTime()));
                    if (count > 0 && --count === 0) break;
                }
            }
            // Advance cursor by interval weeks
            cursor.setDate(cursor.getDate() + 7 * interval);

        } else {
            // Simple case: one occurrence per step
            if (cursor >= now) {
                occurrences.push(new Date(cursor.getTime()));
                if (count > 0 && --count === 0) break;
            }
            if (freq === "DAILY") {
                cursor.setDate(cursor.getDate() + interval);
            } else if (freq === "WEEKLY") {
                cursor.setDate(cursor.getDate() + 7 * interval);
            } else if (freq === "MONTHLY") {
                cursor = addMonths(cursor, interval);
            } else if (freq === "YEARLY") {
                cursor = addMonths(cursor, 12 * interval);
            } else {
                break; // unknown freq
            }
        }
    }

    return occurrences;
}

// Convert a Date+duration into an arc event and push if it falls in window
function pushArcEvent(events, calendarIndex, start, durationMs, now, cutoff) {
    var end = new Date(start.getTime() + durationMs);
    if (end <= now || start >= cutoff) return;
    var startMins    = (start.getHours() * 60 + start.getMinutes()) % 720;
    var endMins      = (end.getHours()   * 60 + end.getMinutes())   % 720;
    var durationMins = endMins - startMins;
    if (durationMins <= 0) durationMins += 720;
    durationMins = Math.min(durationMins, 720);
    if (durationMins > 0) {
        events.push({ calIndex: calendarIndex, startMins: startMins, durationMins: durationMins });
    }
}

function parseICS(raw, calendarIndex, now, cutoff) {
    var text  = unfoldICS(raw);
    var lines = text.split(/\r?\n/);
    var events = [];
    var inEvent = false, current = null;

    // Collect EXDATE values at calendar level too (some feeds put them outside VEVENT)
    var globalExdates = {};

    for (var i = 0; i < lines.length; i++) {
        var line = lines[i].trim();
        if (line === "BEGIN:VEVENT") {
            inEvent = true;
            current = { dtstart: null, dtend: null, duration: null, rrule: null, exdates: {} };
            continue;
        }
        if (line === "END:VEVENT") {
            inEvent = false;
            if (current && current.dtstart) {
                var start = current.dtstart;
                var durationMs;
                if (current.dtend) {
                    durationMs = current.dtend.getTime() - start.getTime();
                } else if (current.duration) {
                    durationMs = current.duration;
                } else {
                    durationMs = 60 * 60 * 1000; // default 1 hour
                }

                if (current.rrule) {
                    // Recurring event: expand occurrences
                    var occurrences = expandRRule(start, current.rrule, now, cutoff);
                    for (var o = 0; o < occurrences.length; o++) {
                        var occ = occurrences[o];
                        // Check EXDATE: compare date-only strings to handle UTC vs local
                        var occKey = dateOnly(occ).getTime();
                        if (current.exdates[occKey]) continue;
                        pushArcEvent(events, calendarIndex, occ, durationMs, now, cutoff);
                    }
                } else {
                    // Single event
                    pushArcEvent(events, calendarIndex, start, durationMs, now, cutoff);
                }
            }
            current = null;
            continue;
        }
        if (!inEvent || !current) continue;
        var colonIdx = line.indexOf(":");
        if (colonIdx < 0) continue;
        // Key may have parameters before colon e.g. "DTSTART;TZID=..."
        var rawKey  = line.substring(0, colonIdx);
        var key     = rawKey.toUpperCase().split(";")[0];
        var value   = line.substring(colonIdx + 1);
        if (key === "DTSTART") {
            current.dtstart = parseICSDate(value);
        } else if (key === "DTEND") {
            current.dtend = parseICSDate(value);
        } else if (key === "DURATION") {
            current.duration = parseDuration(value);
        } else if (key === "RRULE") {
            current.rrule = value;
        } else if (key === "EXDATE") {
            // EXDATE may list multiple dates comma-separated
            var exParts = value.split(",");
            for (var ex = 0; ex < exParts.length; ex++) {
                var exDate = parseICSDate(exParts[ex].trim());
                if (exDate) current.exdates[dateOnly(exDate).getTime()] = true;
            }
        }
    }
    return events;
}

// ─── Send display settings ────────────────────────────────────────────────────

function sendDisplaySettings() {
    var config = JSON.parse(localStorage.getItem("calendarConfig") || "{}");

    var defaults = [0, 4, 8, 2, 9, 5, 1, 7, 10, 3];
    var colors = [];
    for (var i = 0; i < MAX_CALENDARS; i++) {
        var key = "color" + i;
        colors.push(typeof config[key] === "number" ? config[key] : defaults[i]);
    }
    var packedColors = colors.join(",");

    var msg = {
        2:  config.useFahrenheit ? 1 : 0,
        3:  (config.showDate  !== false) ? 1 : 0,
        8:  (config.showTicks !== false) ? 1 : 0,
        12: typeof config.hourColor === "number" ? config.hourColor : 11,
        13: packedColors,
    };
    Pebble.sendAppMessage(msg, function() {
        console.log("Display settings sent: " + JSON.stringify(msg));
    }, function(e) {
        console.log("Display settings send failed: " + JSON.stringify(e));
    });
}

// ─── Send events ─────────────────────────────────────────────────────────────

function sendEventsToWatch(events) {
    events.sort(function(a, b) { return a.startMins - b.startMins; });
    events = events.slice(0, 10);
    var packed = events.map(function(e) {
        return e.startMins + "," + e.durationMins + "," + e.calIndex;
    }).join("|");
    console.log("Sending " + events.length + " events: " + packed);
    Pebble.sendAppMessage({ 5: packed }, function() {
        console.log("Events sent");
    }, function(e) {
        console.log("Send failed: " + JSON.stringify(e));
    });
}

// ─── Fetch calendars ──────────────────────────────────────────────────────────

// Track consecutive failures per calendar slot to avoid hammering dead URLs
var s_cal_fail_count = [0,0,0,0,0,0,0,0,0,0];
var MAX_FAIL_COUNT = 3;

function fetchCalendar(url, calendarIndex, now, cutoff, callback) {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", url, true);
    xhr.onload = function() {
        if (xhr.status === 200) {
            try {
                s_cal_fail_count[calendarIndex] = 0; // reset on success
                callback(null, parseICS(xhr.responseText, calendarIndex, now, cutoff));
            } catch (e) {
                console.log("ICS parse error cal " + calendarIndex + ": " + e);
                s_cal_fail_count[calendarIndex]++;
                callback(e, []);
            }
        } else {
            console.log("HTTP " + xhr.status + " for cal " + calendarIndex);
            s_cal_fail_count[calendarIndex]++;
            callback(new Error("HTTP " + xhr.status), []);
        }
    };
    xhr.onerror = function() {
        s_cal_fail_count[calendarIndex]++;
        callback(new Error("Network error"), []);
    };
    xhr.send();
}

function fetchAllCalendars() {
    var config = JSON.parse(localStorage.getItem("calendarConfig") || "{}");

    var entries = [];
    for (var i = 0; i < MAX_CALENDARS; i++) {
        var url = config["url" + i] || "";
        if (url.length > 0) {
            if (s_cal_fail_count[i] >= MAX_FAIL_COUNT) {
                console.log("Skipping cal " + i + " after " + s_cal_fail_count[i] + " failures");
                continue;
            }
            entries.push({ url: url, idx: i });
        }
    }

    if (entries.length === 0) {
        console.log("No calendar URLs to fetch");
        return;
    }

    var now    = new Date();
    var cutoff = new Date(now.getTime() + 12 * 60 * 60 * 1000);
    console.log("Fetching " + entries.length + " calendars");

    var allEvents = [], pending = entries.length;
    entries.forEach(function(entry) {
        fetchCalendar(entry.url, entry.idx, now, cutoff, function(err, events) {
            if (!err) allEvents = allEvents.concat(events);
            if (--pending === 0) sendEventsToWatch(allEvents);
        });
    });
}

// ─── Combined refresh (weather + calendars) ───────────────────────────────────

function refreshWeather(cfg) {
    navigator.geolocation.getCurrentPosition(function(pos) {
        s_last_lat = pos.coords.latitude;
        s_last_lng = pos.coords.longitude;
        fetchWeather(s_last_lat, s_last_lng, cfg.useFahrenheit || false);
    }, function(err) {
        console.log("Geolocation error: " + err.message);
        // Fall back to cached position if available
        if (s_last_lat !== null) {
            console.log("Using cached position");
            fetchWeather(s_last_lat, s_last_lng, cfg.useFahrenheit || false);
        }
    }, {
        timeout: 10000,
        maximumAge: 60 * 60 * 1000  // accept a cached position up to 1 hour old
    });
}

function refreshAll() {
    var cfg = JSON.parse(localStorage.getItem("calendarConfig") || "{}");
    refreshWeather(cfg);
    fetchAllCalendars();
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

Pebble.addEventListener("ready", function() {
    console.log("PebbleKit JS ready");

    var config = JSON.parse(localStorage.getItem("calendarConfig") || "{}");
    sendDisplaySettings();
    refreshWeather(config);
    fetchAllCalendars();

    // Single consolidated interval: weather + calendars every 60 minutes
    setInterval(refreshAll, 60 * 60 * 1000);
});

Pebble.addEventListener("showConfiguration", function() {
    var config = localStorage.getItem("calendarConfig") || "{}";
    Pebble.openURL("https://davv47.github.io/pebble-analogue-config/index.html?config="
                   + encodeURIComponent(config));
});

Pebble.addEventListener("webviewclosed", function(e) {
    if (e.response && e.response !== "CANCELLED") {
        try {
            var raw = e.response;
            var decoded;
            try { decoded = decodeURIComponent(raw); } catch(err) { decoded = raw; }
            var config = JSON.parse(decoded);
            localStorage.setItem("calendarConfig", JSON.stringify(config));
            console.log("Config saved");
            // Reset failure counts so newly configured URLs get a fresh attempt
            s_cal_fail_count = [0,0,0,0,0,0,0,0,0,0];
            sendDisplaySettings();
            refreshWeather(config);
            fetchAllCalendars();
        } catch (err) {
            console.log("Failed to parse config response: " + err);
        }
    }
});
