// Parses human-friendly duration strings ("10ms", "2h", "1h 30m", "10 hours", "90")
// into milliseconds, for the manual-entry time fields on the Settings page. A bare
// number with no unit is interpreted using defaultUnit.
(function (global) {
    var UNIT_MS = {
        ms: 1, msec: 1, msecs: 1, millisecond: 1, milliseconds: 1,
        s: 1000, sec: 1000, secs: 1000, second: 1000, seconds: 1000,
        m: 60000, min: 60000, mins: 60000, minute: 60000, minutes: 60000,
        h: 3600000, hr: 3600000, hrs: 3600000, hour: 3600000, hours: 3600000,
        d: 86400000, day: 86400000, days: 86400000
    };

    function parseDurationMs(input, defaultUnit) {
        if (input == null) return null;
        var str = String(input).trim().toLowerCase();
        if (str === '') return null;

        var defaultMs = UNIT_MS[(defaultUnit || 'ms').toLowerCase()] || 1;

        var re = /([0-9]*\.?[0-9]+)\s*([a-z]*)/g;
        var total = 0;
        var matched = false;
        var m;
        while ((m = re.exec(str)) !== null) {
            var amount = parseFloat(m[1]);
            var unitStr = m[2];
            var unitMs = unitStr ? UNIT_MS[unitStr] : defaultMs;
            if (unitMs === undefined || isNaN(amount)) {
                return null; // unrecognized unit -> reject the whole string
            }
            total += amount * unitMs;
            matched = true;
        }
        return matched ? total : null;
    }

    // Renders a millisecond value back into a short human string for a text field,
    // e.g. 1500 -> "1.5s", 90000 -> "1.5m".
    function formatDurationShort(ms) {
        if (ms == null || isNaN(ms)) return '';
        var trim = function (n) { return Math.round(n * 100) / 100; };
        if (ms < 1000) return trim(ms) + 'ms';
        if (ms < 60000) return trim(ms / 1000) + 's';
        if (ms < 3600000) return trim(ms / 60000) + 'm';
        if (ms < 86400000) return trim(ms / 3600000) + 'h';
        return trim(ms / 86400000) + 'd';
    }

    global.parseDurationMs = parseDurationMs;
    global.formatDurationShort = formatDurationShort;
})(window);
