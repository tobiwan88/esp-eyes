/* efx v2 compiler (JSON description -> .efx binary), byte-identical to tools/efx/compiler.py.
 * API (browser: window.EfxCompile, Node: module.exports):
 *   compile(doc) -> Uint8Array          doc = parsed JSON object; throws Error(message)
 *   check(bytes) -> {duration, shapes, gaze_keys, bytes, mirror}; throws Error(message)
 *   fromBytes(bytes) -> {bytes, converted, info}
 *       binary (starts with "EFX1") is checked as is; anything else must be a JSON object
 *       (UTF-8, optional BOM) and is compiled. The extension is never looked at.
 *   suggestName(doc, filename) -> string matching [a-z0-9_-]{1,31}
 * Python rounding is reproduced: round() is half-to-even, % is non-negative.
 */
(function (g) {
  'use strict';

  var MAX_BYTES = 4096, MAX_SHAPES = 16, MAX_KEYS = 12, MAX_GAZE = 12;
  var HDR = 16, GAZE_SZ = 6, SHAPE_HDR = 5, KEY_SZ = 10;
  var KINDS = ['circle', 'ellipse', 'rect', 'ring', 'heart', 'sparkle', 'glyph', 'line', 'triangle'];
  var EASINGS = ['linear', 'smooth', 'in', 'out', 'back', 'step'];
  var GLYPHS = { '%': 10, 'euro': 11, '€': 11 };
  var COLORS = {
    red: '#ff2020', pink: '#ff6fa8', hotpink: '#ff3c8c', orange: '#ff9a1a', yellow: '#ffe030',
    green: '#30d040', lime: '#a0ff30', cyan: '#30e0f0', blue: '#3070ff', purple: '#9040e0',
    white: '#ffffff', black: '#000000', gold: '#ffc820', gray: '#808080', brown: '#8a5a2a'
  };
  var EYES = { both: 3, left: 1, right: 2 };
  var i;
  for (i = 0; i < 10; i++) GLYPHS[String(i)] = i;
  for (i = 0; i < 8; i++) GLYPHS['kana' + i] = 12 + i;
  var COLOR_NAMES = Object.keys(COLORS).sort().join(', ');

  function hasOwn(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }
  function getOr(o, k, def) { return hasOwn(o, k) ? o[k] : def; }

  // An error carrying the Python exception class it stands for (EfxError, TypeError, struct.error).
  function fail(msg, cls) { var e = new Error(msg); e.pyClass = cls || 'EfxError'; return e; }

  // Python round(x) for floats: half to even, exact on the binary value.
  function pyRound(x) {
    var f = Math.floor(x), d = x - f;
    var r;
    if (d < 0.5) r = f;
    else if (d > 0.5) r = f + 1;
    else r = (f % 2 === 0) ? f : f + 1;
    return r + 0; // no -0
  }
  // Python float % (result takes the sign of the divisor).
  function pyMod(a, b) {
    var r = a % b;
    if (r !== 0 && ((b < 0) !== (r < 0))) r += b;
    return r;
  }
  function pmodInt(a, m) { return ((a % m) + m) % m; }

  function pyType(v) {
    if (v === null || v === undefined) return 'NoneType';
    if (typeof v === 'string') return 'str';
    if (typeof v === 'boolean') return 'bool';
    if (typeof v === 'number') return Number.isInteger(v) ? 'int' : 'float';
    if (Array.isArray(v)) return 'list';
    return 'dict';
  }
  function strRepr(s) {
    var q = (s.indexOf("'") >= 0 && s.indexOf('"') < 0) ? '"' : "'";
    var out = q;
    for (var k = 0; k < s.length; k++) {
      var ch = s.charAt(k), c = s.charCodeAt(k);
      if (ch === '\\') out += '\\\\';
      else if (ch === q) out += '\\' + q;
      else if (ch === '\n') out += '\\n';
      else if (ch === '\r') out += '\\r';
      else if (ch === '\t') out += '\\t';
      else if (c < 0x20 || c === 0x7f) out += '\\x' + (c < 16 ? '0' : '') + c.toString(16);
      else out += ch;
    }
    return out + q;
  }
  function pyRepr(v) {
    if (v === null || v === undefined) return 'None';
    if (typeof v === 'boolean') return v ? 'True' : 'False';
    if (typeof v === 'number') return String(v);
    if (typeof v === 'string') return strRepr(v);
    if (Array.isArray(v)) return '[' + v.map(pyRepr).join(', ') + ']';
    return '{' + Object.keys(v).map(function (k) { return strRepr(k) + ': ' + pyRepr(v[k]); }).join(', ') + '}';
  }
  function pyStr(v) {
    if (typeof v === 'string') return v;
    if (v === null || v === undefined || typeof v === 'boolean' || Array.isArray(v) || typeof v === 'object') return pyRepr(v);
    return String(v);
  }
  // Python truthiness for JSON values.
  function pyTruthy(v) {
    if (v === null || v === undefined || v === false) return false;
    if (typeof v === 'number') return v !== 0;
    if (typeof v === 'string' || Array.isArray(v)) return v.length > 0;
    if (typeof v === 'object') return Object.keys(v).length > 0;
    return true;
  }
  function isNum(v) { return typeof v === 'number'; }

  // _num(d, key, default): missing key -> default (null means "required").
  function num(d, key, def) {
    var v = getOr(d, key, def === undefined ? null : def);
    if (!isNum(v)) throw fail("'" + key + "' must be a number");
    return v;
  }

  // Python bitwise operand: bool counts as int, floats and other types raise TypeError.
  function intOperand(v, op) {
    if (typeof v === 'boolean') return v ? 1 : 0;
    if (isNum(v) && Number.isInteger(v)) return v;
    throw fail("unsupported operand type(s) for " + op + ": '" + pyType(v) + "' and 'int'", 'TypeError');
  }

  function rgb565(c) {
    var r, gg, b;
    if (typeof c === 'string') {
      var lc = c.toLowerCase();
      if (hasOwn(COLORS, lc)) c = COLORS[lc];
      var m = /^#([0-9a-fA-F]{6})$/.exec(c);
      if (!m) throw fail('bad colour ' + strRepr(c) + ': use #rrggbb or one of ' + COLOR_NAMES);
      var v = parseInt(m[1], 16);
      r = v >> 16; gg = (v >> 8) & 255; b = v & 255;
    } else if (Array.isArray(c) && c.length === 3) {
      r = intOperand(c[0], '&');
      gg = intOperand(c[1], '&');
      b = intOperand(c[2], '>>');
    } else {
      throw fail('bad colour ' + pyRepr(c));
    }
    return (((r & 0xF8) << 8) | ((gg & 0xFC) << 3) | (b >> 3));
  }

  function crc16(bytes, start, end) {
    var crc = 0xFFFF;
    for (var p = start; p < end; p++) {
      crc ^= bytes[p] << 8;
      for (var k = 0; k < 8; k++) {
        crc = (crc & 0x8000) ? (((crc << 1) ^ 0x1021) & 0xFFFF) : ((crc << 1) & 0xFFFF);
      }
    }
    return crc;
  }

  function aux(kind, s) {
    var v;
    if (kind === 'ellipse' || kind === 'rect') {
      v = pyRound(num(s, 'ratio', 1.0) * 32);
      if (!(v >= 1 && v <= 255)) throw fail('ratio must be 0.03 .. 7.9');
      return v;
    }
    if (kind === 'ring') {
      v = pyRound(num(s, 'inner', 0.6) * 255);
      if (!(v >= 0 && v <= 255)) throw fail('inner must be 0..1');
      return v;
    }
    if (kind === 'glyph') {
      var gl = pyStr(getOr(s, 'glyph', '0'));
      if (!hasOwn(GLYPHS, gl)) throw fail('unknown glyph ' + strRepr(gl) + ': use 0-9, %, euro, kana0..kana7');
      return GLYPHS[gl];
    }
    if (kind === 'line' || kind === 'triangle') {
      return pmodInt(pyRound(pyMod(num(s, 'angle', 0.0), 360) / 360 * 256), 256);
    }
    return 0;
  }

  function push16(arr, v) { arr.push(v & 255, (v >> 8) & 255); }

  function compile(doc) {
    if (doc === null || typeof doc !== 'object' || Array.isArray(doc)) throw fail('top level must be an object');
    var dur = pyRound(num(doc, 'duration'));
    var shapes = getOr(doc, 'shapes', null);
    var gaze = getOr(doc, 'gaze', []);
    if (!Array.isArray(shapes) || !(shapes.length >= 1 && shapes.length <= MAX_SHAPES)) {
      throw fail("'shapes' needs 1.." + MAX_SHAPES + ' entries');
    }
    if (!Array.isArray(gaze) || gaze.length > MAX_GAZE) throw fail("'gaze' has at most " + MAX_GAZE + ' keys');
    if (!(dur >= 200 && dur <= 30000)) throw fail('duration must be 200..30000 ms');

    var out = [];
    var last = -1, n, t, gx, gy, pupil, lid, g, s, w, kind, eyes, easing, keys, mask, flags, lt, k, x, y, size, col;
    for (n = 0; n < gaze.length; n++) {
      g = gaze[n];
      t = pyRound(num(g, 't'));
      if (t <= last || t > 65535) throw fail('gaze key times must increase');
      last = t;
      gx = num(g, 'x', 0);
      gy = num(g, 'y', 0);
      if (!(gx >= -1 && gx <= 1 && gy >= -1 && gy <= 1)) throw fail('gaze x/y must be -1..1');
      pupil = hasOwn(g, 'pupil') ? pyRound(num(g, 'pupil', 0) * 255) : 0;
      lid = hasOwn(g, 'lid') ? 1 + pyRound(num(g, 'lid') * 254) : 0;
      if (!(pupil >= 0 && pupil <= 255 && lid >= 0 && lid <= 255)) throw fail('pupil and lid must be 0..1');
      push16(out, t);
      out.push(pyRound(gx * 127) & 255, pyRound(gy * 127) & 255, pupil, lid);
    }

    for (n = 0; n < shapes.length; n++) {
      s = shapes[n];
      w = 'shape ' + (n + 1);
      kind = getOr(s, 'kind', null);
      if (KINDS.indexOf(kind) < 0) throw fail(w + ': kind must be one of ' + KINDS.join(', '));
      eyes = getOr(s, 'eyes', 'both');
      if (!hasOwn(EYES, eyes) || typeof eyes !== 'string') throw fail(w + ': eyes must be both, left or right');
      easing = getOr(s, 'easing', 'smooth');
      if (EASINGS.indexOf(easing) < 0) throw fail(w + ': easing must be one of ' + EASINGS.join(', '));
      keys = getOr(s, 'keys', null);
      if (!Array.isArray(keys) || !(keys.length >= 1 && keys.length <= MAX_KEYS)) {
        throw fail(w + ": 'keys' needs 1.." + MAX_KEYS + ' entries');
      }
      mask = EYES[eyes] | (pyTruthy(getOr(s, 'world', null)) ? 4 : 0);
      flags = (pyTruthy(getOr(s, 'loop', null)) ? 1 : 0) | (pyTruthy(getOr(s, 'hold', null)) ? 2 : 0) |
              (EASINGS.indexOf(easing) << 4);
      try {
        out.push(KINDS.indexOf(kind), mask, flags, aux(kind, s), keys.length);
        lt = -1;
        for (k = 0; k < keys.length; k++) {
          var kk = keys[k];
          t = pyRound(num(kk, 't'));
          if (t <= lt || t > 65535) throw fail(w + ': key times must increase');
          lt = t;
          x = pyRound(num(kk, 'x'));
          y = pyRound(num(kk, 'y'));
          size = pyRound(num(kk, 'size'));
          if (!(x >= -1000 && x <= 1000 && y >= -1000 && y <= 1000 && size >= 0 && size <= 400)) {
            throw fail(w + ': x/y within -1000..1000, size within 0..400');
          }
          col = rgb565(getOr(kk, 'color', getOr(s, 'color', 'white')));
          if (col < 0 || col > 65535) throw fail("'H' format requires 0 <= number <= 65535", 'struct');
          push16(out, t); push16(out, x); push16(out, y); push16(out, size); push16(out, col);
        }
      } catch (e) {
        if (e.pyClass === 'EfxError' || !e.pyClass) throw e;
        throw fail(w + ': ' + e.message, e.pyClass);
      }
    }

    var fl = pyTruthy(getOr(doc, 'mirror', null)) ? 1 : 0;
    var data = [0x45, 0x46, 0x58, 0x31, 1, fl];
    push16(data, dur);
    data.push(shapes.length, gaze.length);
    push16(data, out.length);
    push16(data, crc16(out, 0, out.length));
    push16(data, 0);
    data = data.concat(out);
    if (data.length > MAX_BYTES) {
      throw fail('file too big: ' + data.length + ' bytes (max ' + MAX_BYTES + '); use fewer shapes or keys');
    }
    var bytes = new Uint8Array(data);
    check(bytes);
    return bytes;
  }

  function toBytes(input) {
    if (input instanceof Uint8Array) return input;
    if (Object.prototype.toString.call(input) === '[object ArrayBuffer]') return new Uint8Array(input);
    if (Array.isArray(input)) return Uint8Array.from(input);
    throw fail('expected bytes');
  }

  function u16(b, p) { return b[p] | (b[p + 1] << 8); }
  function i16(b, p) { var v = u16(b, p); return v >= 0x8000 ? v - 0x10000 : v; }

  function check(input) {
    var b = toBytes(input);
    var len = b.length, p, k, last, t, lt, kind, mask, fl, aux_, nk, ns, ng, x, y, sz, dur, plen, crc, ver;
    if (len < HDR || b[0] !== 0x45 || b[1] !== 0x46 || b[2] !== 0x58 || b[3] !== 0x31) {
      throw fail('not an .efx file (bad header)');
    }
    if (len > MAX_BYTES) throw fail('file length does not match header');
    ver = b[4];
    dur = u16(b, 6); ns = b[8]; ng = b[9]; plen = u16(b, 10); crc = u16(b, 12);
    var hflags = b[5];
    if (ver !== 1) throw fail('unsupported efx version');
    if (len !== HDR + plen) throw fail('file length does not match header');
    if (crc16(b, HDR, len) !== crc) throw fail('checksum mismatch');
    if (!(dur >= 200 && dur <= 30000 && ns >= 1 && ns <= MAX_SHAPES && ng <= MAX_GAZE)) {
      throw fail('bad shape or gaze count');
    }
    p = HDR;
    last = -1;
    for (k = 0; k < ng; k++) {
      if (p + GAZE_SZ > len) throw fail('file length does not match header');
      t = u16(b, p);
      if (t <= last) throw fail('key times must increase');
      last = t;
      p += GAZE_SZ;
    }
    for (k = 0; k < ns; k++) {
      if (p + SHAPE_HDR > len) throw fail('file length does not match header');
      kind = b[p]; mask = b[p + 1]; fl = b[p + 2]; aux_ = b[p + 3]; nk = b[p + 4];
      if (kind > 8 || (mask & 3) === 0 || (mask & ~7) || (fl >> 4) > 5 || (kind === 6 && aux_ >= 20)) {
        throw fail('bad shape field');
      }
      if (!(nk >= 1 && nk <= MAX_KEYS)) throw fail('bad shape or gaze count');
      p += SHAPE_HDR;
      lt = -1;
      for (var j = 0; j < nk; j++) {
        if (p + KEY_SZ > len) throw fail('file length does not match header');
        t = u16(b, p); x = i16(b, p + 2); y = i16(b, p + 4); sz = i16(b, p + 6);
        if (t <= lt) throw fail('key times must increase');
        if (!(x >= -1000 && x <= 1000 && y >= -1000 && y <= 1000 && sz >= 0 && sz <= 400)) {
          throw fail('value out of range');
        }
        lt = t;
        p += KEY_SZ;
      }
    }
    if (p !== len) throw fail('file length does not match header');
    return { duration: dur, shapes: ns, gaze_keys: ng, bytes: len, mirror: !!(hflags & 1) };
  }

  function bytesToText(b) {
    if (typeof TextDecoder !== 'undefined') {
      try { return new TextDecoder('utf-8', { fatal: true }).decode(b); } catch (e) { return null; }
    }
    var pct = '';
    for (var k = 0; k < b.length; k++) pct += '%' + (b[k] < 16 ? '0' : '') + b[k].toString(16);
    try { return decodeURIComponent(pct); } catch (e) { return null; }
  }

  var NOT_EFX = 'not an .efx file and not valid JSON';

  function fromBytes(input) {
    var b = toBytes(input);
    if (b.length >= 4 && b[0] === 0x45 && b[1] === 0x46 && b[2] === 0x58 && b[3] === 0x31) {
      return { bytes: b, converted: false, info: check(b) };
    }
    var text = bytesToText(b);
    if (text === null) throw new Error(NOT_EFX);
    if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1);
    text = text.replace(/^\s+|\s+$/g, '');
    var doc;
    try { doc = JSON.parse(text); } catch (e) { throw new Error(NOT_EFX); }
    if (doc === null || typeof doc !== 'object' || Array.isArray(doc)) throw new Error(NOT_EFX);
    var out = compile(doc);
    return { bytes: out, converted: true, info: check(out) };
  }

  function slug(name) {
    var s = String(name).toLowerCase().replace(/[^a-z0-9_-]/g, '_').slice(0, 31);
    return s || 'effect';
  }

  function suggestName(doc, filename) {
    if (doc && typeof doc.name === 'string' && doc.name.replace(/\s+/g, '') !== '') return slug(doc.name);
    var base = String(filename || '').split(/[\\\/]/).pop();
    var dot = base.lastIndexOf('.');
    if (dot > 0) base = base.slice(0, dot);
    return slug(base);
  }

  var api = { compile: compile, check: check, fromBytes: fromBytes, suggestName: suggestName };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else g.EfxCompile = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
