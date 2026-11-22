// Shape-only WebGL2.  The glue wraps every call in a type-checked invoke_ and
// discards most results; what it reads back are a handful of limits and
// extension handles.
const CONST = {
  VERSION: 0x1f02, SHADING_LANGUAGE_VERSION: 0x8b8c, VENDOR: 0x1f00, RENDERER: 0x1f01,
  MAX_TEXTURE_SIZE: 0x0d33, MAX_CUBE_MAP_TEXTURE_SIZE: 0x851c, MAX_VERTEX_ATTRIBS: 0x8869,
  MAX_VERTEX_UNIFORM_VECTORS: 0x8dfb, MAX_VARYING_VECTORS: 0x8dfc,
  MAX_COMBINED_TEXTURE_IMAGE_UNITS: 0x8b4d, MAX_VERTEX_TEXTURE_IMAGE_UNITS: 0x8b4c,
  MAX_TEXTURE_IMAGE_UNITS: 0x8872, MAX_FRAGMENT_UNIFORM_VECTORS: 0x8dfd,
  MAX_DRAW_BUFFERS: 0x8824, MAX_COLOR_ATTACHMENTS: 0x8cdf,
  MAX_RENDERBUFFER_SIZE: 0x84e8, MAX_SAMPLES: 0x8d57, MAX_UNIFORM_BUFFER_BINDINGS: 0x8a2f,
  MAX_3D_TEXTURE_SIZE: 0x8073, MAX_ARRAY_TEXTURE_LAYERS: 0x88ff,
  MAX_ELEMENT_INDEX: 0x8d6b, MAX_VERTEX_OUTPUT_COMPONENTS: 0x9122,
  MAX_FRAGMENT_INPUT_COMPONENTS: 0x9125, MAX_SERVER_WAIT_TIMEOUT: 0x9111,
  MAX_TEXTURE_MAX_ANISOTROPY_EXT: 0x84ff,
};

export function makeGL(canvas) {
  const params = {
    [CONST.VERSION]: "WebGL 2.0 (OpenGL ES 3.0)",
    [CONST.SHADING_LANGUAGE_VERSION]: "WebGL GLSL ES 3.00",
    [CONST.VENDOR]: "Google Inc. (Intel)",
    [CONST.RENDERER]: "ANGLE (Intel, Mesa Intel(R) UHD Graphics, OpenGL ES 3.0)",
    [CONST.MAX_TEXTURE_SIZE]: 16384,
    [CONST.MAX_CUBE_MAP_TEXTURE_SIZE]: 16384,
    [CONST.MAX_VERTEX_ATTRIBS]: 16,
    [CONST.MAX_VERTEX_UNIFORM_VECTORS]: 4096,
    [CONST.MAX_VARYING_VECTORS]: 32,
    [CONST.MAX_COMBINED_TEXTURE_IMAGE_UNITS]: 64,
    [CONST.MAX_VERTEX_TEXTURE_IMAGE_UNITS]: 32,
    [CONST.MAX_TEXTURE_IMAGE_UNITS]: 32,
    [CONST.MAX_FRAGMENT_UNIFORM_VECTORS]: 4096,
    [CONST.MAX_DRAW_BUFFERS]: 8,
    [CONST.MAX_COLOR_ATTACHMENTS]: 8,
    [CONST.MAX_RENDERBUFFER_SIZE]: 16384,
    [CONST.MAX_SAMPLES]: 4,
    [CONST.MAX_UNIFORM_BUFFER_BINDINGS]: 72,
    [CONST.MAX_3D_TEXTURE_SIZE]: 2048,
    [CONST.MAX_ARRAY_TEXTURE_LAYERS]: 2048,
    [CONST.MAX_ELEMENT_INDEX]: 4294967295,
    [CONST.MAX_VERTEX_OUTPUT_COMPONENTS]: 64,
    [CONST.MAX_FRAGMENT_INPUT_COMPONENTS]: 128,
    [CONST.MAX_SERVER_WAIT_TIMEOUT]: 0,
    [CONST.MAX_TEXTURE_MAX_ANISOTROPY_EXT]: 16,
    0x821b: "WebGL 2.0", // MAJOR_VERSION
    0x821c: 3, // MINOR_VERSION
    0x9245: 1, // UNMASKED_VENDOR_WEBGL
    0x9246: "ANGLE (Intel, Mesa Intel(R) UHD Graphics)", // UNMASKED_RENDERER_WEBGL
  };
  for (const [k, v] of Object.entries(CONST)) params[v] ??= 0;

  let seq = 1;
  const handle = () => ({ __gl: seq++ });
  const extCache = new Map();
  const ext = (name) => {
    if (extCache.has(name)) return extCache.get(name);
    const o = new Proxy({ name }, {
      get(t, p) {
        if (p in t) return t[p];
        if (typeof p === "string" && /^[A-Z0-9_]+$/.test(p)) return 0x9000 + p.length;
        return () => 0;
      },
    });
    extCache.set(name, o);
    return o;
  };

  const gl = {
    canvas,
    drawingBufferWidth: 1280,
    drawingBufferHeight: 720,
    __proto__: null,
    getExtension: (n) => (/^(WEBGL_lose_context|WEBGL_debug)/.test(n) ? ext(n) : ext(n)),
    getSupportedExtensions: () => ["EXT_color_buffer_float", "EXT_float_blend", "OES_texture_float_linear"],
    getParameter: (p) => (p in params ? params[p] : 0),
    getShaderPrecisionFormat: () => ({ precision: 23, rangeMin: 127, rangeMax: 127 }),
    getContextAttributes: () => ({
      alpha: false, antialias: false, depth: true, stencil: false, premultipliedAlpha: true,
      preserveDrawingBuffer: false, powerPreference: "default", failIfMajorPerformanceCaveat: false,
      desynchronized: false, xrCompatible: false,
    }),
    isContextLost: () => false,
    getError: () => 0,
    checkFramebufferStatus: () => 0x8cd5, // FRAMEBUFFER_COMPLETE
    getProgramParameter: (_p, k) => (k === 0x8b82 || k === 0x8b86 || k === 0x8b89 ? true : 0),
    getShaderParameter: (_s, k) => (k === 0x8b81 ? true : 0),
    getProgramInfoLog: () => "",
    getShaderInfoLog: () => "",
    getActiveUniform: (_p, i) => ({ name: "u" + i, size: 1, type: 0x1406 }),
    getActiveAttrib: (_p, i) => ({ name: "a" + i, size: 1, type: 0x1406 }),
    getActiveUniformBlockName: () => "b",
    getUniformLocation: () => handle(),
    getAttribLocation: () => 0,
    getUniformBlockIndex: () => 0,
    getFragDataLocation: () => 0,
    getSyncParameter: () => 0x911a, // ALREADY_SIGNALED
    clientWaitSync: () => 0x911a,
    fenceSync: () => handle(),
    createBuffer: () => handle(),
    createTexture: () => handle(),
    createFramebuffer: () => handle(),
    createRenderbuffer: () => handle(),
    createVertexArray: () => handle(),
    createProgram: () => handle(),
    createShader: () => handle(),
    createSampler: () => handle(),
    createQuery: () => handle(),
    createTransformFeedback: () => handle(),
    createVertexArrayOES: () => handle(),
    mapBufferRange: () => ({ __mapped: 1 }),
    getBufferSubData: () => {},
    readPixels: (x, y, w, h, f, t, dst) => {
      if (dst && dst.fill) dst.fill(0);
    },
    readBuffer: () => {},
  };
  for (const [k, v] of Object.entries(CONST)) gl[k] = v;

  return new Proxy(gl, {
    get(t, p) {
      if (p in t) return t[p];
      if (typeof p === "string" && /^[A-Z][A-Z0-9_]*$/.test(p)) return 0x8000 + (p.length << 4);
      if (typeof p === "string") return (t[p] = () => 0);
      return undefined;
    },
  });
}

export function make2D(canvas) {
  const data = { fillStyle: "#000", strokeStyle: "#000", font: "10px sans-serif", globalAlpha: 1 };
  return new Proxy(data, {
    get(t, p) {
      if (p in t) return t[p];
      if (p === "canvas") return canvas;
      if (p === "getImageData") return (_x, _y, w, h) => ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h });
      if (p === "createImageData") return (w, h) => ({ data: new Uint8ClampedArray(w * h * 4), width: w, height: h });
      if (p === "measureText") return (s) => ({ width: s.length * 6 });
      return () => {};
    },
    set(t, p, v) { t[p] = v; return true; },
  });
}
