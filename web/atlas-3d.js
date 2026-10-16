/*
 * atlas-3d.js -- the Asset Atlas's 3D view: models built from a 3DO the
 * way the engine bakes one, glTF binaries, a picture on a plane, and a
 * small WebGL renderer that draws one or two of them side by side. No DOM,
 * so the worker draws thumbnails with it on an OffscreenCanvas.
 */
(function (root, factory) {
  'use strict';
  var api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OKAtlas3D = api;
})(this, function () {
  'use strict';

  /* ── matrices, column major ─────────────────────────────────────────── */

  function mul(a, b) {
    var o = new Float32Array(16);
    for (var c = 0; c < 4; c++)
      for (var r = 0; r < 4; r++)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  }
  function perspective(fovy, aspect, near, far) {
    var f = 1 / Math.tan(fovy / 2), o = new Float32Array(16);
    o[0] = f / aspect; o[5] = f; o[10] = (far + near) / (near - far); o[11] = -1; o[14] = 2 * far * near / (near - far);
    return o;
  }
  function lookAt(eye, at, up) {
    var z = norm3([eye[0] - at[0], eye[1] - at[1], eye[2] - at[2]]);
    var x = norm3(cross(up, z)), y = cross(z, x);
    return new Float32Array([x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
      -dot(x, eye), -dot(y, eye), -dot(z, eye), 1]);
  }
  function cross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
  function dot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  function norm3(a) { var l = Math.sqrt(dot(a, a)) || 1; return [a[0] / l, a[1] / l, a[2] / l]; }
  function ident() { return new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]); }
  function trs(t, r, s) {
    var x = r[0], y = r[1], z = r[2], w = r[3];
    var o = new Float32Array([
      (1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0], 0,
      2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1], 0,
      2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
      t[0], t[1], t[2], 1]);
    return o;
  }

  /* ── models ─────────────────────────────────────────────────────────── */

  /* A model is {groups: [{pos, nrm, uv, col, tex, emTex, color, emissive,
     cutoff, nearest, repeat, name}], min, max}: triangle soup in world
     pixels, y up, the front toward +z. */

  function bounds(groups) {
    var min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
    groups.forEach(function (g) {
      for (var i = 0; i < g.pos.length; i += 3)
        for (var a = 0; a < 3; a++) {
          var v = g.pos[i + a];
          if (v < min[a]) min[a] = v;
          if (v > max[a]) max[a] = v;
        }
    });
    if (min[0] === Infinity) return { min: [0, 0, 0], max: [0, 0, 0] };
    return { min: min, max: max };
  }

  function flatNormals(pos) {
    var n = new Float32Array(pos.length);
    for (var i = 0; i < pos.length; i += 9) {
      var e1 = [pos[i + 3] - pos[i], pos[i + 4] - pos[i + 1], pos[i + 5] - pos[i + 2]];
      var e2 = [pos[i + 6] - pos[i], pos[i + 7] - pos[i + 1], pos[i + 8] - pos[i + 2]];
      var c = norm3(cross(e1, e2));
      for (var k = 0; k < 3; k++) { n[i + k * 3] = c[0]; n[i + k * 3 + 1] = c[1]; n[i + k * 3 + 2] = c[2]; }
    }
    return n;
  }

  /* A 3DO as atlas_dec.c bakes it: a group a texture, flat faces coloured
   from the palette. */
  function from3do(d, textures, palette) {
    var by = {}, order = [];
    for (var t = 0; t < d.tris; t++) {
      var id = d.tex[t];
      var name = id >= 0 ? d.names[id] : '';
      var key = name && textures[name] ? name : '';
      if (!by[key]) { by[key] = []; order.push(key); }
      by[key].push(t);
    }
    var groups = order.map(function (key) {
      var tris = by[key], n = tris.length;
      var pos = new Float32Array(n * 9), uv = new Float32Array(n * 6), col = key ? null : new Float32Array(n * 9);
      tris.forEach(function (t, i) {
        pos.set(d.pos.subarray(t * 9, t * 9 + 9), i * 9);
        uv.set(d.uv.subarray(t * 6, t * 6 + 6), i * 6);
        if (col) {
          var w = palette ? palette[d.col[t]] : 0xFFB0B0B0;
          var r = (w & 255) / 255, g = ((w >>> 8) & 255) / 255, b = ((w >>> 16) & 255) / 255;
          for (var k = 0; k < 3; k++) { col[i * 9 + k * 3] = r; col[i * 9 + k * 3 + 1] = g; col[i * 9 + k * 3 + 2] = b; }
        }
      });
      return { name: key || 'flat colour', pos: pos, nrm: flatNormals(pos), uv: key ? uv : null, col: col,
               tex: key ? textures[key] : null, color: [1, 1, 1, 1], cutoff: 0.5, nearest: true };
    });
    var b = bounds(groups);
    return { groups: groups, min: b.min, max: b.max };
  }

  function planeModel(w, h, tex, tiles) {
    var W = w * tiles, H = h * tiles;
    var pos = new Float32Array([-W / 2, 0, 0, W / 2, 0, 0, W / 2, H, 0, -W / 2, 0, 0, W / 2, H, 0, -W / 2, H, 0]);
    var uv = new Float32Array([0, tiles, tiles, tiles, tiles, 0, 0, tiles, tiles, 0, 0, 0]);
    var g = { name: 'picture', pos: pos, nrm: flatNormals(pos), uv: uv, tex: tex, color: [1, 1, 1, 1], cutoff: 0.02,
              nearest: true, repeat: true, unlit: true };
    return { groups: [g], min: [-W / 2, 0, 0], max: [W / 2, H, 0] };
  }

  /* ── glTF binary ────────────────────────────────────────────────────── */

  var COMPONENTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };

  function accessorData(j, bin, index) {
    var a = j.accessors[index], v = j.bufferViews[a.bufferView];
    var n = COMPONENTS[a.type] || 1;
    var Ctor = { 5120: Int8Array, 5121: Uint8Array, 5122: Int16Array, 5123: Uint16Array, 5125: Uint32Array, 5126: Float32Array }[a.componentType];
    if (!Ctor || !v) return null;
    var stride = v.byteStride || n * Ctor.BYTES_PER_ELEMENT;
    var off = (v.byteOffset || 0) + (a.byteOffset || 0);
    var out = new Float32Array(a.count * n);
    var dv = new DataView(bin.buffer, bin.byteOffset, bin.byteLength);
    var norm = a.normalized ? { 5121: 255, 5123: 65535, 5120: 127, 5122: 32767 }[a.componentType] || 1 : 1;
    for (var i = 0; i < a.count; i++) {
      for (var c = 0; c < n; c++) {
        var p = off + i * stride + c * Ctor.BYTES_PER_ELEMENT, x;
        switch (a.componentType) {
          case 5126: x = dv.getFloat32(p, true); break;
          case 5125: x = dv.getUint32(p, true); break;
          case 5123: x = dv.getUint16(p, true); break;
          case 5122: x = dv.getInt16(p, true); break;
          case 5121: x = dv.getUint8(p); break;
          default: x = dv.getInt8(p);
        }
        out[i * n + c] = x / norm;
      }
    }
    return out;
  }

  function transformPoint(m, x, y, z) {
    return [m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]];
  }

  /* A glTF binary as a model in world pixels (opts.scale), its pictures
   by opts.image and okPaint ones by opts.paint. */
  function fromGlb(bytes, opts) {
    opts = opts || {};
    var parsed = readGlb(bytes);
    if (!parsed || !parsed.json.meshes) return Promise.reject(new Error('This is not a glTF binary the atlas reads.'));
    var j = parsed.json, bin = parsed.bin || new Uint8Array(0);
    if ((j.extensionsRequired || []).length) return Promise.reject(new Error('This model needs ' + j.extensionsRequired.join(', ') + ', which the atlas does not read.'));
    var scale = opts.scale || 1;
    var root = ident();
    if (scale !== 1) { root[0] = root[5] = root[10] = scale; }
    var images = {};
    function image(i) {
      if (images[i]) return images[i];
      var im = (j.images || [])[i];
      if (!im || im.bufferView === undefined || !opts.image) return (images[i] = Promise.resolve(null));
      var v = j.bufferViews[im.bufferView];
      var b = bin.subarray(v.byteOffset || 0, (v.byteOffset || 0) + v.byteLength);
      return (images[i] = opts.image(b, im.mimeType || 'image/png').catch(function () { return null; }));
    }
    function texture(ref) {
      if (!ref || !j.textures || !j.textures[ref.index]) return Promise.resolve(null);
      return image(j.textures[ref.index].source);
    }
    var mats = (j.materials || []).map(function (m) {
      var pbr = m.pbrMetallicRoughness || {}, ex = m.extras || {};
      var color = (pbr.baseColorFactor || [1, 1, 1, 1]).slice();
      var em = (m.emissiveFactor || [0, 0, 0]).slice();
      var strength = ((m.extensions || {}).KHR_materials_emissive_strength || {}).emissiveStrength || 1;
      em = em.map(function (x) { return x * strength; });
      var mode = m.alphaMode || 'OPAQUE', paint = ex.okPaint;
      var base = paint ? (opts.paint ? Promise.resolve(opts.paint(paint)).catch(function () { return null; }) : Promise.resolve(null))
                       : texture(pbr.baseColorTexture);
      return Promise.all([base, texture(m.emissiveTexture)]).then(function (t) {
        var out = { name: m.name || '', tex: t[0], emTex: t[1], color: color, emissive: em, cutoff: 0, painted: !!paint,
                    double: !!m.doubleSided };
        if (paint) {
          mode = paint.alpha === 'mask' ? 'MASK' : 'OPAQUE';
          var tint = paint.tint || 1, fb = ex.okFallback;
          if (t[0]) out.color = fb ? [tint, tint, tint, 1] : [color[0] * tint, color[1] * tint, color[2] * tint, 1];
          else out.color = fb ? [fb[0], fb[1], fb[2], 1] : [0.5 * color[0] * tint, 0.5 * color[1] * tint, 0.5 * color[2] * tint, 1];
        }
        out.cutoff = mode === 'MASK' ? (m.alphaCutoff !== undefined ? m.alphaCutoff : 0.5) : mode === 'BLEND' ? 0.02 : 0;
        if (mode === 'OPAQUE') out.color[3] = 1;
        return out;
      });
    });
    return Promise.all(mats).then(function (materials) {
      var groups = [];
      function visit(ni, parent) {
        var node = j.nodes[ni];
        var local = node.matrix ? new Float32Array(node.matrix)
          : trs(node.translation || [0, 0, 0], node.rotation || [0, 0, 0, 1], node.scale || [1, 1, 1]);
        var world = mul(parent, local);
        if (node.mesh !== undefined && j.meshes[node.mesh]) {
          j.meshes[node.mesh].primitives.forEach(function (p) {
            if ((p.mode === undefined ? 4 : p.mode) !== 4 || p.attributes.POSITION === undefined) return;
            var P = accessorData(j, bin, p.attributes.POSITION);
            var N = p.attributes.NORMAL !== undefined ? accessorData(j, bin, p.attributes.NORMAL) : null;
            var T = p.attributes.TEXCOORD_0 !== undefined ? accessorData(j, bin, p.attributes.TEXCOORD_0) : null;
            var I = p.indices !== undefined ? accessorData(j, bin, p.indices) : null;
            if (!P) return;
            var count = I ? I.length : P.length / 3;
            count -= count % 3;
            var pos = new Float32Array(count * 3), nrm = N ? new Float32Array(count * 3) : null, uv = T ? new Float32Array(count * 2) : null;
            var nm = [world[0], world[1], world[2], 0, world[4], world[5], world[6], 0, world[8], world[9], world[10], 0, 0, 0, 0, 1];
            for (var i = 0; i < count; i++) {
              var vi = I ? I[i] : i;
              var w = transformPoint(world, P[vi * 3], P[vi * 3 + 1], P[vi * 3 + 2]);
              pos[i * 3] = w[0]; pos[i * 3 + 1] = w[1]; pos[i * 3 + 2] = w[2];
              if (nrm) {
                var nn = norm3(transformPoint(nm, N[vi * 3], N[vi * 3 + 1], N[vi * 3 + 2]));
                nrm[i * 3] = nn[0]; nrm[i * 3 + 1] = nn[1]; nrm[i * 3 + 2] = nn[2];
              }
              if (uv) { uv[i * 2] = T[vi * 2]; uv[i * 2 + 1] = T[vi * 2 + 1]; }
            }
            var m = materials[p.material] || { tex: null, color: [0.7, 0.7, 0.7, 1], emissive: [0, 0, 0], cutoff: 0 };
            groups.push({ name: m.name, pos: pos, nrm: nrm || flatNormals(pos), uv: uv, tex: m.tex, emTex: m.emTex,
                          color: m.color, emissive: m.emissive, cutoff: m.cutoff, repeat: true, painted: m.painted });
          });
        }
        (node.children || []).forEach(function (c) { visit(c, world); });
      }
      var scene = (j.scenes || [])[j.scene || 0];
      var roots = scene ? scene.nodes : j.nodes.map(function (n, i) { return i; });
      roots.forEach(function (n) { visit(n, root); });
      var b = bounds(groups);
      return { groups: groups, min: b.min, max: b.max, json: j };
    });
  }

  function readGlb(bytes) {
    var b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    var dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    if (b.length < 20 || dv.getUint32(0, true) !== 0x46546C67) return null;
    var len = dv.getUint32(12, true);
    if (dv.getUint32(16, true) !== 0x4E4F534A || 20 + len > b.length) return null;
    var json;
    try { json = JSON.parse(new TextDecoder().decode(b.subarray(20, 20 + len))); } catch (e) { return null; }
    var at = 20 + len, bin = null;
    if (at + 8 <= b.length && dv.getUint32(at + 4, true) === 0x004E4942) bin = b.subarray(at + 8, Math.min(b.length, at + 8 + dv.getUint32(at, true)));
    return { json: json, bin: bin };
  }

  /* An okPaint picture made as GlbLoader.Paint makes it: bleed, gain,
   alpha and border. */
  function paintPicture(img, spec) {
    var w = img.w, h = img.h, n = w * h, src = img.rgba;
    var rgb = new Float32Array(n * 3), known = new Uint8Array(n), have = 0, i, c;
    for (i = 0; i < n; i++) {
      for (c = 0; c < 3; c++) rgb[i * 3 + c] = src[i * 4 + c] / 255;
      known[i] = src[i * 4 + 3] > 127 ? 1 : 0;
      have += known[i];
    }
    if (spec.bleed && have) {
      var acc = new Float32Array(n * 3), cnt = new Uint8Array(n);
      while (have < n) {
        acc.fill(0); cnt.fill(0);
        for (var y = 0; y < h; y++)
          for (var x = 0; x < w; x++) {
            var at = y * w + x;
            if (known[at]) continue;
            [[0, 1], [0, -1], [1, 0], [-1, 0]].forEach(function (d) {
              var sy = y + d[0], sx = x + d[1];
              if (sy < 0 || sy >= h || sx < 0 || sx >= w) return;
              var s = sy * w + sx;
              if (!known[s]) return;
              acc[at * 3] += rgb[s * 3]; acc[at * 3 + 1] += rgb[s * 3 + 1]; acc[at * 3 + 2] += rgb[s * 3 + 2];
              cnt[at]++;
            });
          }
        var grew = [];
        for (i = 0; i < n; i++) if (!known[i] && cnt[i]) {
          for (c = 0; c < 3; c++) rgb[i * 3 + c] = acc[i * 3 + c] / cnt[i];
          grew.push(i);
        }
        if (!grew.length) break;
        grew.forEach(function (g) { known[g] = 1; });
        have += grew.length;
      }
    }
    var gain = spec.gain || 1, opaque = spec.alpha !== 'mask', out = new Uint8Array(n * 4);
    for (i = 0; i < n; i++) {
      for (c = 0; c < 3; c++) out[i * 4 + c] = Math.round(Math.min(1, Math.max(0, rgb[i * 3 + c] * gain)) * 255);
      out[i * 4 + 3] = opaque ? 255 : src[i * 4 + 3];
      if (spec.border && (i % w === 0 || i % w === w - 1 || i < w || i >= n - w)) out[i * 4 + 3] = 0;
    }
    return { w: w, h: h, rgba: out };
  }

  /* ── the renderer ───────────────────────────────────────────────────── */

  var VS = [
    'attribute vec3 aPos; attribute vec3 aNrm; attribute vec2 aUv; attribute vec3 aCol;',
    'uniform mat4 uMvp; varying vec3 vNrm; varying vec2 vUv; varying vec3 vCol; varying vec3 vPos;',
    'void main() { vNrm = aNrm; vUv = aUv; vCol = aCol; vPos = aPos; gl_Position = uMvp * vec4(aPos, 1.0); }'
  ].join('\n');
  var FS = [
    'precision mediump float;',
    'uniform sampler2D uTex; uniform sampler2D uEm; uniform vec4 uColor; uniform vec3 uEmissive;',
    'uniform float uHasTex; uniform float uHasEm; uniform float uCutoff; uniform float uFract; uniform float uUnlit;',
    'uniform vec3 uLight; uniform vec3 uSky; uniform vec3 uGround; uniform vec3 uEye;',
    'varying vec3 vNrm; varying vec2 vUv; varying vec3 vCol; varying vec3 vPos;',
    'void main() {',
    '  vec2 uv = uFract > 0.5 ? fract(vUv) : vUv;',
    '  vec4 base = uColor * vec4(vCol, 1.0);',
    '  if (uHasTex > 0.5) base *= texture2D(uTex, uv);',
    '  if (base.a < uCutoff) discard;',
    '  vec3 n = normalize(vNrm); if (!gl_FrontFacing) n = -n;',
    '  vec3 lit = mix(uGround, uSky, n.y * 0.5 + 0.5) + vec3(0.85) * max(dot(n, uLight), 0.0);',
    '  vec3 v = normalize(uEye - vPos); vec3 hlf = normalize(uLight + v);',
    '  float spec = pow(max(dot(n, hlf), 0.0), 32.0) * 0.12;',
    '  vec3 c = uUnlit > 0.5 ? base.rgb : base.rgb * lit + spec;',
    '  vec3 em = uEmissive; if (uHasEm > 0.5) em *= texture2D(uEm, uv).rgb;',
    '  gl_FragColor = vec4(c + em, 1.0);',
    '}'
  ].join('\n');
  var LINE_FS = 'precision mediump float; uniform vec4 uColor; void main() { gl_FragColor = uColor; }';
  var LINE_VS = 'attribute vec3 aPos; uniform mat4 uMvp; void main() { gl_Position = uMvp * vec4(aPos, 1.0); }';

  function isPow2(n) { return n > 0 && (n & (n - 1)) === 0; }

  function renderer(canvas, opts) {
    opts = opts || {};
    var attrs = { antialias: true, alpha: false, premultipliedAlpha: false, preserveDrawingBuffer: !!opts.preserve };
    var gl = canvas.getContext('webgl2', attrs), gl2 = !!gl;
    if (!gl) gl = canvas.getContext('webgl', attrs) || canvas.getContext('experimental-webgl', attrs);
    if (!gl) return null;
    function shader(type, src) {
      var s = gl.createShader(type);
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
      return s;
    }
    function program(vs, fs, names) {
      var p = gl.createProgram();
      gl.attachShader(p, shader(gl.VERTEX_SHADER, vs));
      gl.attachShader(p, shader(gl.FRAGMENT_SHADER, fs));
      ['aPos', 'aNrm', 'aUv', 'aCol'].forEach(function (a, i) { gl.bindAttribLocation(p, i, a); });
      gl.linkProgram(p);
      var u = {};
      names.forEach(function (n) { u[n] = gl.getUniformLocation(p, n); });
      return { p: p, u: u };
    }
    var main = program(VS, FS, ['uMvp', 'uTex', 'uEm', 'uColor', 'uEmissive', 'uHasTex', 'uHasEm', 'uCutoff', 'uFract', 'uUnlit', 'uLight', 'uSky', 'uGround', 'uEye']);
    var line = program(LINE_VS, LINE_FS, ['uMvp', 'uColor']);
    var textures = new Map();

    function upload(src, nearest, repeat) {
      if (!src) return null;
      var key = src;
      var cached = textures.get(key);
      if (cached && cached.nearest === nearest) return cached.t;
      var t = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      var w = src.w || src.width, h = src.h || src.height;
      if (src.rgba) gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, src.rgba);
      else gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, src);
      var mip = !nearest && (gl2 || (isPow2(w) && isPow2(h)));
      var wrap = repeat && (gl2 || (isPow2(w) && isPow2(h))) ? gl.REPEAT : gl.CLAMP_TO_EDGE;
      if (mip) gl.generateMipmap(gl.TEXTURE_2D);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, nearest ? gl.NEAREST : mip ? gl.LINEAR_MIPMAP_LINEAR : gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, nearest ? gl.NEAREST : gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrap);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrap);
      textures.set(key, { t: t, nearest: nearest, native: wrap === gl.REPEAT });
      return t;
    }

    function buffers(g) {
      if (g._buf && g._gl === gl) return g._buf;
      function buf(data) {
        if (!data) return null;
        var b = gl.createBuffer();
        gl.bindBuffer(gl.ARRAY_BUFFER, b);
        gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
        return b;
      }
      g._gl = gl;
      g._buf = { pos: buf(g.pos), nrm: buf(g.nrm), uv: buf(g.uv), col: buf(g.col), n: g.pos.length / 3 };
      return g._buf;
    }

    function attrib(i, b, size, fallback) {
      if (b) {
        gl.bindBuffer(gl.ARRAY_BUFFER, b);
        gl.enableVertexAttribArray(i);
        gl.vertexAttribPointer(i, size, gl.FLOAT, false, 0, 0);
      } else {
        gl.disableVertexAttribArray(i);
        gl.vertexAttrib4f(i, fallback[0], fallback[1], fallback[2], 1);
      }
    }

    var gridCache = {};
    function grid(extent) {
      var cells = Math.min(64, Math.max(2, Math.ceil(extent / 16)));
      if (gridCache[cells]) return gridCache[cells];
      var pts = [], e = cells * 16;
      for (var k = -cells; k <= cells; k++) {
        pts.push(k * 16, 0, -e, k * 16, 0, e, -e, 0, k * 16, e, 0, k * 16);
      }
      var b = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, b);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(pts), gl.STATIC_DRAW);
      return (gridCache[cells] = { b: b, n: pts.length / 3 });
    }

    var view = { yaw: -0.4, pitch: 0.55, zoom: 1, panX: 0, panY: 0 };

    /* Draws models side by side, one viewport each, under one camera so
       they are shown at the same scale. opts: background [r,g,b], grid. */
    function draw(models, o) {
      o = o || {};
      var W = canvas.width, H = canvas.height, n = Math.max(1, models.length);
      var bg = o.background || [0.12, 0.12, 0.13];
      gl.viewport(0, 0, W, H);
      gl.disable(gl.SCISSOR_TEST);
      gl.clearColor(bg[0], bg[1], bg[2], 1);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
      var lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
      models.forEach(function (m) {
        if (!m) return;
        for (var a = 0; a < 3; a++) { lo[a] = Math.min(lo[a], m.min[a]); hi[a] = Math.max(hi[a], m.max[a]); }
      });
      if (lo[0] === Infinity) return;
      var flat = o.flat;
      var center = [0, (lo[1] + hi[1]) / 2, flat ? 0 : 0];
      if (n === 1) center = [(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2];
      var radius = Math.max(1, Math.sqrt(Math.pow(hi[0] - lo[0], 2) + Math.pow(hi[1] - lo[1], 2) + Math.pow(hi[2] - lo[2], 2)) / 2);
      var vw = W / n, aspect = vw / H, fov = 0.6;
      var dist = radius / Math.sin(fov / 2) * 1.05 / view.zoom;
      var cp = Math.cos(view.pitch), sp = Math.sin(view.pitch);
      var eye = [center[0] + dist * cp * Math.sin(view.yaw), center[1] + dist * sp, center[2] + dist * cp * Math.cos(view.yaw)];
      var right = norm3([Math.cos(view.yaw), 0, -Math.sin(view.yaw)]);
      var up = cross(norm3([center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]]), right);
      var pan = [right[0] * view.panX * radius + up[0] * view.panY * radius, right[1] * view.panX * radius + up[1] * view.panY * radius,
                 right[2] * view.panX * radius + up[2] * view.panY * radius];
      var at = [center[0] - pan[0], center[1] - pan[1], center[2] - pan[2]];
      eye = [eye[0] - pan[0], eye[1] - pan[1], eye[2] - pan[2]];
      var vp = mul(perspective(fov, aspect, Math.max(0.5, dist - radius * 4), dist + radius * 4), lookAt(eye, at, [0, 1, 0]));
      var L = norm3([-0.45, 0.8, 0.55]);
      gl.enable(gl.DEPTH_TEST);
      gl.disable(gl.CULL_FACE);
      gl.disable(gl.BLEND);
      models.forEach(function (m, i) {
        gl.viewport(Math.round(i * vw), 0, Math.round(vw), H);
        if (!m) return;
        if (o.grid !== false && !flat) {
          var gr = grid(Math.max(hi[0] - lo[0], hi[2] - lo[2]) * 0.75 + 16);
          gl.useProgram(line.p);
          gl.uniformMatrix4fv(line.u.uMvp, false, vp);
          var gc = o.gridColor || [0.5, 0.5, 0.5, 1];
          gl.uniform4f(line.u.uColor, gc[0], gc[1], gc[2], 1);
          for (var a = 1; a < 4; a++) gl.disableVertexAttribArray(a);
          attrib(0, gr.b, 3);
          gl.drawArrays(gl.LINES, 0, gr.n);
        }
        gl.useProgram(main.p);
        gl.uniformMatrix4fv(main.u.uMvp, false, vp);
        gl.uniform3fv(main.u.uLight, L);
        gl.uniform3fv(main.u.uSky, o.sky || [0.42, 0.44, 0.48]);
        gl.uniform3fv(main.u.uGround, o.ground || [0.2, 0.18, 0.16]);
        gl.uniform3fv(main.u.uEye, eye);
        gl.uniform1i(main.u.uTex, 0);
        gl.uniform1i(main.u.uEm, 1);
        m.groups.forEach(function (g) {
          var b = buffers(g);
          attrib(0, b.pos, 3);
          attrib(1, b.nrm, 3, [0, 1, 0]);
          attrib(2, b.uv, 2, [0, 0, 0]);
          attrib(3, b.col, 3, [1, 1, 1]);
          var t = g.uv ? upload(g.tex, !!g.nearest, !!g.repeat) : null;
          var e = g.uv ? upload(g.emTex, false, !!g.repeat) : null;
          gl.activeTexture(gl.TEXTURE0);
          gl.bindTexture(gl.TEXTURE_2D, t);
          gl.activeTexture(gl.TEXTURE1);
          gl.bindTexture(gl.TEXTURE_2D, e);
          var native = t && textures.get(g.tex).native;
          gl.uniform1f(main.u.uHasTex, t ? 1 : 0);
          gl.uniform1f(main.u.uHasEm, e ? 1 : 0);
          gl.uniform1f(main.u.uFract, g.repeat && t && !native ? 1 : 0);
          gl.uniform1f(main.u.uUnlit, g.unlit ? 1 : 0);
          gl.uniform4fv(main.u.uColor, g.color || [1, 1, 1, 1]);
          gl.uniform3fv(main.u.uEmissive, g.emissive || [0, 0, 0]);
          gl.uniform1f(main.u.uCutoff, g.cutoff || 0);
          gl.drawArrays(gl.TRIANGLES, 0, b.n);
        });
      });
    }

    function release(model) {
      if (!model) return;
      model.groups.forEach(function (g) {
        if (g._buf && g._gl === gl) {
          ['pos', 'nrm', 'uv', 'col'].forEach(function (k) { if (g._buf[k]) gl.deleteBuffer(g._buf[k]); });
          g._buf = null;
        }
        [g.tex, g.emTex].forEach(function (s) {
          var c = s && textures.get(s);
          if (c) { gl.deleteTexture(c.t); textures.delete(s); }
        });
      });
    }

    return { gl: gl, view: view, draw: draw, release: release, webgl2: gl2,
             lost: function () { return gl.isContextLost(); } };
  }

  return {
    from3do: from3do,
    fromGlb: fromGlb,
    readGlb: readGlb,
    planeModel: planeModel,
    paintPicture: paintPicture,
    flatNormals: flatNormals,
    bounds: bounds,
    renderer: renderer,
    mul: mul,
    perspective: perspective,
    lookAt: lookAt
  };
});
