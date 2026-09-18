/*
 * gl3d.c -- the 3D view's renderer, see tak_gl3d.h.
 *
 * The GL entry points are fetched through SDL on the desktop and are
 * the linked functions in the browser, behind one table, so the draw
 * code reads the same on both. Shaders are written to GLSL ES 1.00,
 * which every WebGL context and every desktop compatibility context
 * accepts, with only the version line differing.
 */

#include "tak_gl3d.h"
#include "tak_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <GLES2/gl2.h>
#define GLF(name) gl##name
#else
#include <SDL_opengl.h>
#define GLF(name) gl.name
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

/* ── The entry points ─────────────────────────────────────────────── */

#define GL3D_FUNCS(X) \
    X(void,   Enable,           (GLenum)) \
    X(void,   Disable,          (GLenum)) \
    X(GLboolean, IsEnabled,     (GLenum)) \
    X(void,   GetIntegerv,      (GLenum, GLint *)) \
    X(void,   GetBooleanv,      (GLenum, GLboolean *)) \
    X(void,   GetFloatv,        (GLenum, GLfloat *)) \
    X(GLenum, GetError,         (void)) \
    X(const GLubyte *, GetString, (GLenum)) \
    X(void,   Viewport,         (GLint, GLint, GLsizei, GLsizei)) \
    X(void,   Scissor,          (GLint, GLint, GLsizei, GLsizei)) \
    X(void,   Clear,            (GLbitfield)) \
    X(void,   ClearColor,       (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void,   DepthFunc,        (GLenum)) \
    X(void,   DepthMask,        (GLboolean)) \
    X(void,   ColorMask,        (GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void,   CullFace,         (GLenum)) \
    X(void,   FrontFace,        (GLenum)) \
    X(void,   BlendFunc,        (GLenum, GLenum)) \
    X(void,   BlendFuncSeparate,(GLenum, GLenum, GLenum, GLenum)) \
    X(void,   BlendEquation,    (GLenum)) \
    X(void,   BlendEquationSeparate, (GLenum, GLenum)) \
    X(void,   ActiveTexture,    (GLenum)) \
    X(void,   BindTexture,      (GLenum, GLuint)) \
    X(void,   GenTextures,      (GLsizei, GLuint *)) \
    X(void,   DeleteTextures,   (GLsizei, const GLuint *)) \
    X(void,   TexImage2D,       (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void,   TexSubImage2D,    (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void,   TexParameteri,    (GLenum, GLenum, GLint)) \
    X(void,   PixelStorei,      (GLenum, GLint)) \
    X(void,   GenBuffers,       (GLsizei, GLuint *)) \
    X(void,   DeleteBuffers,    (GLsizei, const GLuint *)) \
    X(void,   BindBuffer,       (GLenum, GLuint)) \
    X(void,   BufferData,       (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(GLuint, CreateShader,     (GLenum)) \
    X(void,   ShaderSource,     (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void,   CompileShader,    (GLuint)) \
    X(void,   GetShaderiv,      (GLuint, GLenum, GLint *)) \
    X(void,   GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   DeleteShader,     (GLuint)) \
    X(GLuint, CreateProgram,    (void)) \
    X(void,   AttachShader,     (GLuint, GLuint)) \
    X(void,   LinkProgram,      (GLuint)) \
    X(void,   GetProgramiv,     (GLuint, GLenum, GLint *)) \
    X(void,   GetProgramInfoLog,(GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void,   UseProgram,       (GLuint)) \
    X(void,   DeleteProgram,    (GLuint)) \
    X(GLint,  GetUniformLocation, (GLuint, const GLchar *)) \
    X(void,   BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void,   Uniform1i,        (GLint, GLint)) \
    X(void,   Uniform1f,        (GLint, GLfloat)) \
    X(void,   Uniform2f,        (GLint, GLfloat, GLfloat)) \
    X(void,   Uniform3f,        (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void,   Uniform4fv,       (GLint, GLsizei, const GLfloat *)) \
    X(void,   UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void,   VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void,   EnableVertexAttribArray, (GLuint)) \
    X(void,   DisableVertexAttribArray, (GLuint)) \
    X(void,   GetVertexAttribiv, (GLuint, GLenum, GLint *)) \
    X(void,   DrawElements,     (GLenum, GLsizei, GLenum, const void *))

#ifndef __EMSCRIPTEN__
static struct {
#define GL3D_DECL(ret, name, args) ret (APIENTRY *name) args;
    GL3D_FUNCS(GL3D_DECL)
#undef GL3D_DECL
    /* Optional: absent on a bare GL 2.1 context, then no mipmaps. */
    void (APIENTRY *GenerateMipmap)(GLenum);
    /* Desktop only, for putting SDL's fixed function arrays back. */
    void (APIENTRY *EnableClientState)(GLenum);
    void (APIENTRY *DisableClientState)(GLenum);
    void (APIENTRY *Color4f)(GLfloat, GLfloat, GLfloat, GLfloat);
} gl;
#ifndef GL_VERTEX_ARRAY
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#endif
#endif

/* ── State ────────────────────────────────────────────────────────── */

struct GL3D_Texture { GLuint id; int w, h; };
struct GL3D_Mesh    { GLuint vbo, ibo; GL3D_Layout layout; int vert_count, index_count; };

typedef struct Program {
    GLuint id;
    GLint  u_vp, u_model, u_eye, u_light, u_tex, u_fog, u_mapsize;
    GLint  u_nodes, u_texscale, u_textured, u_alpha, u_alphacut, u_nsign;
    GLint  u_usefog, u_nodebase;
    /* The PBR program alone. */
    GLint  u_nrmtex, u_mrtex, u_emtex, u_hasnrm, u_hasmr, u_hasem;
    GLint  u_emissive, u_metallic, u_roughness, u_nrmscale, u_blend;
} Program;

static struct {
    int         ready;
    SDL_Window *window;
    SDL_Renderer *renderer;
    Program     terrain, model, model_pbr, sprite;
    int         max_nodes;
#ifdef __EMSCRIPTEN__
    /* The scene draws inside this render target of SDL's. */
    SDL_Texture *target;
    int         target_w, target_h;
    SDL_Rect    target_dst;
    int         in_target;
    /* SDL gives the target a colour attachment only. */
    GLuint      depth_rb;
    int         depth_w, depth_h;
#endif
    float       viewproj[16], eye[3], light[3];
    GL3D_Texture *fog;
    float       map_w, map_h;
    GLuint      stream_vbo, stream_ibo;
    /* SDL's GL state at BeginFrame, put back at EndFrame. */
    struct {
        GLint program, active_tex, tex2d, array_buf, elem_buf;
        GLint viewport[4], scissor[4];
        GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
        GLint blend_eq_rgb, blend_eq_a, cull_mode, front_face, depth_func;
        GLboolean blend, scissor_on, depth_test, cull, depth_mask, color_mask[4];
        GLboolean tex2d_on, va_vertex, va_color, va_texcoord;
        GLint attrib_on[8];
        GLfloat clear_color[4], color[4];
    } saved;
    SDL_Texture *bound_sdl_tex;   /* the atlas bound through SDL this frame */
    int frame_open;
} g;

int GL3D_LayoutFloats(GL3D_Layout layout) {
    switch (layout) {
    case GL3D_LAYOUT_TERRAIN: return 8;
    case GL3D_LAYOUT_MODEL:   return 13;
    case GL3D_LAYOUT_MODEL_PBR: return 17;
    case GL3D_LAYOUT_SPRITE:  return 9;
    default: return 0;
    }
}

int GL3D_Available(void) { return g.ready; }
int GL3D_MaxNodesPerDraw(void) { return g.max_nodes; }

/* ── Shaders ──────────────────────────────────────────────────────── */

#ifdef __EMSCRIPTEN__
#define GLSL_VERSION "#version 100\n"
#else
#define GLSL_VERSION "#version 120\n"
#endif
#define GLSL_PRECISION "#ifdef GL_ES\nprecision highp float;\n#endif\n"

static const char *k_terrain_vs =
    GLSL_VERSION GLSL_PRECISION
    "attribute vec3 a_pos; attribute vec3 a_nrm; attribute vec2 a_uv;\n"
    "uniform mat4 u_vp; uniform vec3 u_light; uniform vec3 u_eye; uniform vec2 u_mapsize;\n"
    "varying vec2 v_uv; varying float v_light; varying vec2 v_fog; varying float v_dist;\n"
    "void main() {\n"
    "  gl_Position = u_vp * vec4(a_pos, 1.0);\n"
    "  v_uv = a_uv;\n"
    "  v_light = 0.58 + 0.42 * max(dot(normalize(a_nrm), u_light), 0.0);\n"
    "  v_fog = a_pos.xz / u_mapsize;\n"
    "  v_dist = length(a_pos - u_eye);\n"
    "}\n";

static const char *k_terrain_fs =
    GLSL_VERSION GLSL_PRECISION
    "uniform sampler2D u_tex; uniform sampler2D u_fog;\n"
    "varying vec2 v_uv; varying float v_light; varying vec2 v_fog; varying float v_dist;\n"
    "void main() {\n"
    "  vec3 c = texture2D(u_tex, v_uv).rgb * v_light;\n"
    "  float f = texture2D(u_fog, v_fog).r;\n"
    "  float vis = f < 0.25 ? 0.0 : (f < 0.75 ? 0.5 : 1.0);\n"
    "  float haze = clamp((v_dist - 2500.0) / 9000.0, 0.0, 0.55);\n"
    "  c = mix(c * vis, vec3(0.62, 0.70, 0.80), haze * step(0.25, f));\n"
    "  gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

/* %d is the node budget, filled in at build time. */
static const char *k_model_vs_fmt =
    GLSL_VERSION GLSL_PRECISION
    "attribute vec3 a_pos; attribute vec3 a_nrm; attribute vec2 a_uv;\n"
    "attribute vec4 a_col; attribute float a_node;\n"
    "uniform mat4 u_vp; uniform mat4 u_model; uniform vec4 u_nodes[%d];\n"
    "uniform vec2 u_texscale; uniform vec3 u_light; uniform vec3 u_eye; uniform float u_nsign;\n"
    "uniform float u_nodebase;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying float v_light; varying float v_dist;\n"
    "void main() {\n"
    "  int n = (int(a_node) - int(u_nodebase)) * 3;\n"
#ifdef __EMSCRIPTEN__
    /* ES 1.00 guarantees a uniform array index only as a constant or
     * a loop counter, so the row is found by walking. */
    "  vec4 r0 = u_nodes[0]; vec4 r1 = u_nodes[1]; vec4 r2 = u_nodes[2];\n"
    "  for (int i = 0; i < %d; i += 3) {\n"
    "    if (i == n) { r0 = u_nodes[i]; r1 = u_nodes[i + 1]; r2 = u_nodes[i + 2]; }\n"
    "  }\n"
#else
    "  vec4 r0 = u_nodes[n]; vec4 r1 = u_nodes[n + 1]; vec4 r2 = u_nodes[n + 2];\n"
#endif
    "  vec3 p = vec3(dot(r0.xyz, a_pos) + r0.w, dot(r1.xyz, a_pos) + r1.w, dot(r2.xyz, a_pos) + r2.w);\n"
    "  vec3 nn = vec3(dot(r0.xyz, a_nrm), dot(r1.xyz, a_nrm), dot(r2.xyz, a_nrm));\n"
    "  vec4 wp = u_model * vec4(p, 1.0);\n"
    "  vec3 wn = normalize(mat3(u_model) * nn) * u_nsign;\n"
    "  gl_Position = u_vp * wp;\n"
    "  v_uv = a_uv * u_texscale;\n"
    "  v_col = a_col;\n"
    "  v_light = 0.5 + 0.5 * max(dot(wn, u_light), 0.0);\n"
    "  v_dist = length(wp.xyz - u_eye);\n"
    "}\n";

static const char *k_model_fs =
    GLSL_VERSION GLSL_PRECISION
    "uniform sampler2D u_tex; uniform float u_textured; uniform float u_alpha;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying float v_light; varying float v_dist;\n"
    "void main() {\n"
    "  vec4 t = u_textured > 0.5 ? texture2D(u_tex, v_uv) : vec4(1.0);\n"
    "  vec4 c = t * v_col;\n"
    "  if (c.a < 0.2) discard;\n"
    "  float haze = clamp((v_dist - 2500.0) / 9000.0, 0.0, 0.55);\n"
    "  vec3 rgb = mix(c.rgb * v_light, vec3(0.62, 0.70, 0.80), haze);\n"
    "  gl_FragColor = vec4(rgb, c.a * u_alpha);\n"
    "}\n";

/* The same piece transform, with a tangent carried along so the
 * fragment can bend its normal by a map. The bitangent's sign is the
 * tangent's fourth number, as glTF has it. */
static const char *k_model_pbr_vs_fmt =
    GLSL_VERSION GLSL_PRECISION
    "attribute vec3 a_pos; attribute vec3 a_nrm; attribute vec2 a_uv;\n"
    "attribute vec4 a_col; attribute float a_node; attribute vec4 a_tan;\n"
    "uniform mat4 u_vp; uniform mat4 u_model; uniform vec4 u_nodes[%d];\n"
    "uniform vec2 u_texscale; uniform float u_nsign; uniform float u_nodebase;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying vec3 v_nrm; varying vec3 v_tan;\n"
    "varying vec3 v_bit; varying vec3 v_wpos;\n"
    "void main() {\n"
    "  int n = (int(a_node) - int(u_nodebase)) * 3;\n"
#ifdef __EMSCRIPTEN__
    "  vec4 r0 = u_nodes[0]; vec4 r1 = u_nodes[1]; vec4 r2 = u_nodes[2];\n"
    "  for (int i = 0; i < %d; i += 3) {\n"
    "    if (i == n) { r0 = u_nodes[i]; r1 = u_nodes[i + 1]; r2 = u_nodes[i + 2]; }\n"
    "  }\n"
#else
    "  vec4 r0 = u_nodes[n]; vec4 r1 = u_nodes[n + 1]; vec4 r2 = u_nodes[n + 2];\n"
#endif
    "  vec3 p = vec3(dot(r0.xyz, a_pos) + r0.w, dot(r1.xyz, a_pos) + r1.w, dot(r2.xyz, a_pos) + r2.w);\n"
    "  vec3 nn = vec3(dot(r0.xyz, a_nrm), dot(r1.xyz, a_nrm), dot(r2.xyz, a_nrm));\n"
    "  vec3 tt = vec3(dot(r0.xyz, a_tan.xyz), dot(r1.xyz, a_tan.xyz), dot(r2.xyz, a_tan.xyz));\n"
    "  vec4 wp = u_model * vec4(p, 1.0);\n"
    "  vec3 wn = normalize(mat3(u_model) * nn) * u_nsign;\n"
    "  vec3 wt = mat3(u_model) * tt;\n"
    "  float tl = length(wt);\n"
    "  wt = tl > 0.0 ? wt / tl : vec3(0.0);\n"
    "  gl_Position = u_vp * wp;\n"
    "  v_uv = a_uv * u_texscale;\n"
    "  v_col = a_col;\n"
    "  v_nrm = wn;\n"
    "  v_tan = wt;\n"
    "  v_bit = cross(wn, wt) * a_tan.w;\n"
    "  v_wpos = wp.xyz;\n"
    "}\n";

/* Half lambert like the shipped models, so the two sit together, with
 * a highlight whose sharpness and colour come from roughness and
 * metal, and light the surface gives off added on top. */
static const char *k_model_pbr_fs =
    GLSL_VERSION GLSL_PRECISION
    "uniform sampler2D u_tex; uniform sampler2D u_nrmtex; uniform sampler2D u_mrtex;\n"
    "uniform sampler2D u_emtex;\n"
    "uniform float u_textured; uniform float u_hasnrm; uniform float u_hasmr; uniform float u_hasem;\n"
    "uniform float u_alpha; uniform float u_alphacut; uniform float u_blend;\n"
    "uniform float u_metallic; uniform float u_roughness; uniform float u_nrmscale;\n"
    "uniform vec3 u_emissive; uniform vec3 u_light; uniform vec3 u_eye;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying vec3 v_nrm; varying vec3 v_tan;\n"
    "varying vec3 v_bit; varying vec3 v_wpos;\n"
    "void main() {\n"
    "  vec4 t = u_textured > 0.5 ? texture2D(u_tex, v_uv) : vec4(1.0);\n"
    "  vec4 c = t * v_col;\n"
    "  if (u_alphacut > 0.0 && c.a < u_alphacut) discard;\n"
    "  float alpha = u_blend > 0.5 ? c.a : 1.0;\n"
    "  vec3 N = normalize(v_nrm);\n"
    "  if (u_hasnrm > 0.5) {\n"
    "    vec3 tn = texture2D(u_nrmtex, v_uv).xyz * 2.0 - 1.0;\n"
    "    tn.xy *= u_nrmscale;\n"
    "    vec3 T = v_tan; vec3 B = v_bit;\n"
    "    if (dot(T, T) > 0.0) N = normalize(tn.x * normalize(T) + tn.y * normalize(B) + tn.z * N);\n"
    "  }\n"
    "  float rough = u_roughness; float metal = u_metallic;\n"
    "  if (u_hasmr > 0.5) { vec4 mr = texture2D(u_mrtex, v_uv); rough *= mr.g; metal *= mr.b; }\n"
    "  rough = clamp(rough, 0.04, 1.0);\n"
    "  vec3 L = normalize(u_light);\n"
    "  vec3 V = normalize(u_eye - v_wpos);\n"
    "  vec3 H = normalize(L + V);\n"
    "  float ndl = max(dot(N, L), 0.0);\n"
    "  float ndh = max(dot(N, H), 0.0);\n"
    "  float diff = 0.5 + 0.5 * ndl;\n"
    "  float shin = mix(96.0, 4.0, rough);\n"
    "  float spec = pow(ndh, shin) * (1.0 - 0.75 * rough) * ndl;\n"
    "  vec3 f0 = mix(vec3(0.04), c.rgb, metal);\n"
    "  vec3 rgb = c.rgb * (1.0 - 0.9 * metal) * diff + f0 * spec;\n"
    "  if (u_hasem > 0.5) rgb += texture2D(u_emtex, v_uv).rgb * u_emissive; else rgb += u_emissive;\n"
    "  float haze = clamp((length(v_wpos - u_eye) - 2500.0) / 9000.0, 0.0, 0.55);\n"
    "  rgb = mix(rgb, vec3(0.62, 0.70, 0.80), haze);\n"
    "  gl_FragColor = vec4(rgb, alpha * u_alpha);\n"
    "}\n";

static const char *k_sprite_vs =
    GLSL_VERSION GLSL_PRECISION
    "attribute vec3 a_pos; attribute vec2 a_uv; attribute vec4 a_col;\n"
    "uniform mat4 u_vp; uniform vec3 u_eye; uniform vec2 u_mapsize;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying float v_dist; varying vec2 v_fog;\n"
    "void main() {\n"
    "  gl_Position = u_vp * vec4(a_pos, 1.0);\n"
    "  v_uv = a_uv; v_col = a_col;\n"
    "  v_dist = length(a_pos - u_eye);\n"
    "  v_fog = a_pos.xz / u_mapsize;\n"
    "}\n";

static const char *k_sprite_fs =
    GLSL_VERSION GLSL_PRECISION
    "uniform sampler2D u_tex; uniform sampler2D u_fog;\n"
    "uniform float u_textured; uniform float u_alphacut; uniform float u_usefog;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying float v_dist; varying vec2 v_fog;\n"
    "void main() {\n"
    "  vec4 t = u_textured > 0.5 ? texture2D(u_tex, v_uv) : vec4(1.0);\n"
    "  vec4 c = t * v_col;\n"
    "  if (c.a < u_alphacut) discard;\n"
    "  float f = u_usefog > 0.5 ? texture2D(u_fog, v_fog).r : 1.0;\n"
    "  float vis = f < 0.25 ? 0.0 : (f < 0.75 ? 0.5 : 1.0);\n"
    "  float lit = step(0.25, f);\n"
    "  float haze = clamp((v_dist - 2500.0) / 9000.0, 0.0, 0.55);\n"
    "  gl_FragColor = vec4(mix(c.rgb * vis, vec3(0.62, 0.70, 0.80), haze * lit), c.a * lit);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *src) {
    GLuint s = GLF(CreateShader)(kind);
    const GLchar *srcs[1] = { src };
    GLF(ShaderSource)(s, 1, srcs, NULL);
    GLF(CompileShader)(s);
    GLint ok = 0;
    GLF(GetShaderiv)(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        GLF(GetShaderInfoLog)(s, (GLsizei)sizeof(log), &n, log);
        fprintf(stderr, "GL3D: shader failed: %.*s\n", (int)n, log);
        GLF(DeleteShader)(s);
        return 0;
    }
    return s;
}

static int build_program(Program *p, const char *vs_src, const char *fs_src) {
    memset(p, 0, sizeof(*p));
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return -1;
    p->id = GLF(CreateProgram)();
    GLF(AttachShader)(p->id, vs);
    GLF(AttachShader)(p->id, fs);
    GLF(BindAttribLocation)(p->id, 0, "a_pos");
    GLF(BindAttribLocation)(p->id, 1, "a_nrm");
    GLF(BindAttribLocation)(p->id, 2, "a_uv");
    GLF(BindAttribLocation)(p->id, 3, "a_col");
    GLF(BindAttribLocation)(p->id, 4, "a_node");
    GLF(BindAttribLocation)(p->id, 5, "a_tan");
    GLF(LinkProgram)(p->id);
    GLF(DeleteShader)(vs);
    GLF(DeleteShader)(fs);
    GLint ok = 0;
    GLF(GetProgramiv)(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        GLF(GetProgramInfoLog)(p->id, (GLsizei)sizeof(log), &n, log);
        fprintf(stderr, "GL3D: link failed: %.*s\n", (int)n, log);
        GLF(DeleteProgram)(p->id);
        p->id = 0;
        return -1;
    }
    p->u_vp       = GLF(GetUniformLocation)(p->id, "u_vp");
    p->u_model    = GLF(GetUniformLocation)(p->id, "u_model");
    p->u_eye      = GLF(GetUniformLocation)(p->id, "u_eye");
    p->u_light    = GLF(GetUniformLocation)(p->id, "u_light");
    p->u_tex      = GLF(GetUniformLocation)(p->id, "u_tex");
    p->u_fog      = GLF(GetUniformLocation)(p->id, "u_fog");
    p->u_mapsize  = GLF(GetUniformLocation)(p->id, "u_mapsize");
    p->u_nodes    = GLF(GetUniformLocation)(p->id, "u_nodes");
    p->u_texscale = GLF(GetUniformLocation)(p->id, "u_texscale");
    p->u_textured = GLF(GetUniformLocation)(p->id, "u_textured");
    p->u_alpha    = GLF(GetUniformLocation)(p->id, "u_alpha");
    p->u_alphacut = GLF(GetUniformLocation)(p->id, "u_alphacut");
    p->u_nsign    = GLF(GetUniformLocation)(p->id, "u_nsign");
    p->u_usefog   = GLF(GetUniformLocation)(p->id, "u_usefog");
    p->u_nodebase = GLF(GetUniformLocation)(p->id, "u_nodebase");
    p->u_nrmtex   = GLF(GetUniformLocation)(p->id, "u_nrmtex");
    p->u_mrtex    = GLF(GetUniformLocation)(p->id, "u_mrtex");
    p->u_emtex    = GLF(GetUniformLocation)(p->id, "u_emtex");
    p->u_hasnrm   = GLF(GetUniformLocation)(p->id, "u_hasnrm");
    p->u_hasmr    = GLF(GetUniformLocation)(p->id, "u_hasmr");
    p->u_hasem    = GLF(GetUniformLocation)(p->id, "u_hasem");
    p->u_emissive = GLF(GetUniformLocation)(p->id, "u_emissive");
    p->u_metallic = GLF(GetUniformLocation)(p->id, "u_metallic");
    p->u_roughness = GLF(GetUniformLocation)(p->id, "u_roughness");
    p->u_nrmscale = GLF(GetUniformLocation)(p->id, "u_nrmscale");
    p->u_blend    = GLF(GetUniformLocation)(p->id, "u_blend");
    return 0;
}

/* The model vertex shaders take the node row count in their text. */
static char *format_model_vs(const char *fmt, int nodes) {
    char *out = (char *)tak_malloc(strlen(fmt) + 64);
    if (!out) return NULL;
#ifdef __EMSCRIPTEN__
    sprintf(out, fmt, nodes * 3, nodes * 3);
#else
    sprintf(out, fmt, nodes * 3);
#endif
    return out;
}

/* ── Init ─────────────────────────────────────────────────────────── */

int GL3D_Init(SDL_Window *window, SDL_Renderer *renderer) {
    if (g.ready) return 0;
    memset(&g, 0, sizeof(g));
    if (!window || !renderer) return -1;
    if (!SDL_GL_GetCurrentContext()) {
        fprintf(stderr, "GL3D: the renderer has no GL context\n");
        return -1;
    }
#ifndef __EMSCRIPTEN__
#define GL3D_LOAD(ret, name, args) \
    gl.name = (ret (APIENTRY *) args)SDL_GL_GetProcAddress("gl" #name); \
    if (!gl.name) { fprintf(stderr, "GL3D: no gl" #name "\n"); return -1; }
    GL3D_FUNCS(GL3D_LOAD)
#undef GL3D_LOAD
    gl.GenerateMipmap = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glGenerateMipmap");
    gl.EnableClientState = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glEnableClientState");
    gl.DisableClientState = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glDisableClientState");
    gl.Color4f = (void (APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat))SDL_GL_GetProcAddress("glColor4f");
#endif
    g.window = window;
    g.renderer = renderer;

    /* The node budget: three vec4 rows per node out of the vertex
     * uniform store, less what the matrices take. */
    GLint vec4s = 0;
#ifdef __EMSCRIPTEN__
    GLF(GetIntegerv)(GL_MAX_VERTEX_UNIFORM_VECTORS, &vec4s);
#else
    GLF(GetIntegerv)(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &vec4s);
    vec4s /= 4;
#endif
    (void)GLF(GetError)();
    int nodes = (vec4s - 16) / 3;
    if (nodes > 64) nodes = 64;
    if (nodes < 8) nodes = 8;
    g.max_nodes = nodes;

    char *model_vs = format_model_vs(k_model_vs_fmt, nodes);
    char *pbr_vs = format_model_vs(k_model_pbr_vs_fmt, nodes);
    if (!model_vs || !pbr_vs) {
        if (model_vs) tak_free(model_vs);
        if (pbr_vs) tak_free(pbr_vs);
        return -1;
    }
    int rc = build_program(&g.terrain, k_terrain_vs, k_terrain_fs);
    if (rc == 0) rc = build_program(&g.model, model_vs, k_model_fs);
    if (rc == 0) rc = build_program(&g.model_pbr, pbr_vs, k_model_pbr_fs);
    if (rc == 0) rc = build_program(&g.sprite, k_sprite_vs, k_sprite_fs);
    tak_free(model_vs);
    tak_free(pbr_vs);
    if (rc != 0) return -1;

    GLF(GenBuffers)(1, &g.stream_vbo);
    GLF(GenBuffers)(1, &g.stream_ibo);
    fprintf(stderr, "GL3D: %s, %d nodes per draw\n",
            (const char *)GLF(GetString)(GL_VERSION), g.max_nodes);
    g.ready = 1;
    return 0;
}

void GL3D_Shutdown(void) {
    if (!g.ready) return;
    if (g.terrain.id) GLF(DeleteProgram)(g.terrain.id);
    if (g.model.id)   GLF(DeleteProgram)(g.model.id);
    if (g.model_pbr.id) GLF(DeleteProgram)(g.model_pbr.id);
    if (g.sprite.id)  GLF(DeleteProgram)(g.sprite.id);
    if (g.stream_vbo) GLF(DeleteBuffers)(1, &g.stream_vbo);
    if (g.stream_ibo) GLF(DeleteBuffers)(1, &g.stream_ibo);
#ifdef __EMSCRIPTEN__
    if (g.depth_rb) glDeleteRenderbuffers(1, &g.depth_rb);
    if (g.target) SDL_DestroyTexture(g.target);
#endif
    memset(&g, 0, sizeof(g));
}

/* ── Frame ────────────────────────────────────────────────────────── */

static void save_state(void) {
    GLF(GetIntegerv)(GL_CURRENT_PROGRAM, &g.saved.program);
    GLF(GetIntegerv)(GL_ACTIVE_TEXTURE, &g.saved.active_tex);
    GLF(GetIntegerv)(GL_TEXTURE_BINDING_2D, &g.saved.tex2d);
    GLF(GetIntegerv)(GL_ARRAY_BUFFER_BINDING, &g.saved.array_buf);
    GLF(GetIntegerv)(GL_ELEMENT_ARRAY_BUFFER_BINDING, &g.saved.elem_buf);
    GLF(GetIntegerv)(GL_VIEWPORT, g.saved.viewport);
    GLF(GetIntegerv)(GL_SCISSOR_BOX, g.saved.scissor);
    GLF(GetIntegerv)(GL_BLEND_SRC_RGB, &g.saved.blend_src_rgb);
    GLF(GetIntegerv)(GL_BLEND_DST_RGB, &g.saved.blend_dst_rgb);
    GLF(GetIntegerv)(GL_BLEND_SRC_ALPHA, &g.saved.blend_src_a);
    GLF(GetIntegerv)(GL_BLEND_DST_ALPHA, &g.saved.blend_dst_a);
    GLF(GetIntegerv)(GL_BLEND_EQUATION_RGB, &g.saved.blend_eq_rgb);
    GLF(GetIntegerv)(GL_BLEND_EQUATION_ALPHA, &g.saved.blend_eq_a);
    GLF(GetIntegerv)(GL_CULL_FACE_MODE, &g.saved.cull_mode);
    GLF(GetIntegerv)(GL_FRONT_FACE, &g.saved.front_face);
    GLF(GetIntegerv)(GL_DEPTH_FUNC, &g.saved.depth_func);
    g.saved.blend      = GLF(IsEnabled)(GL_BLEND);
    g.saved.scissor_on = GLF(IsEnabled)(GL_SCISSOR_TEST);
    g.saved.depth_test = GLF(IsEnabled)(GL_DEPTH_TEST);
    g.saved.cull       = GLF(IsEnabled)(GL_CULL_FACE);
    GLF(GetBooleanv)(GL_DEPTH_WRITEMASK, &g.saved.depth_mask);
    GLF(GetBooleanv)(GL_COLOR_WRITEMASK, g.saved.color_mask);
    /* SDL sets its clear colour and its draw colour only when they
     * change, so both have to come back exactly as they were. */
    GLF(GetFloatv)(GL_COLOR_CLEAR_VALUE, g.saved.clear_color);
#ifndef __EMSCRIPTEN__
    GLF(GetFloatv)(GL_CURRENT_COLOR, g.saved.color);
#endif
    for (int i = 0; i < 8; i++) {
        g.saved.attrib_on[i] = 0;
        GLF(GetVertexAttribiv)((GLuint)i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &g.saved.attrib_on[i]);
    }
#ifndef __EMSCRIPTEN__
    g.saved.tex2d_on    = GLF(IsEnabled)(GL_TEXTURE_2D);
    g.saved.va_vertex   = GLF(IsEnabled)(GL_VERTEX_ARRAY);
    g.saved.va_color    = GLF(IsEnabled)(GL_COLOR_ARRAY);
    g.saved.va_texcoord = GLF(IsEnabled)(GL_TEXTURE_COORD_ARRAY);
#endif
    (void)GLF(GetError)();
}

static void set_enabled(GLenum cap, GLboolean on) {
    if (on) GLF(Enable)(cap); else GLF(Disable)(cap);
}

static void restore_state(void) {
    for (int i = 0; i < 8; i++) {
        if (g.saved.attrib_on[i]) GLF(EnableVertexAttribArray)((GLuint)i);
        else GLF(DisableVertexAttribArray)((GLuint)i);
    }
    GLF(BindBuffer)(GL_ARRAY_BUFFER, (GLuint)g.saved.array_buf);
    GLF(BindBuffer)(GL_ELEMENT_ARRAY_BUFFER, (GLuint)g.saved.elem_buf);
    GLF(UseProgram)((GLuint)g.saved.program);
    GLF(ActiveTexture)((GLenum)g.saved.active_tex);
    if (g.bound_sdl_tex) {
        /* Unbinding through SDL keeps its texture cache truthful. */
        SDL_GL_UnbindTexture(g.bound_sdl_tex);
        g.bound_sdl_tex = NULL;
    } else {
        GLF(BindTexture)(GL_TEXTURE_2D, (GLuint)g.saved.tex2d);
#ifndef __EMSCRIPTEN__
        set_enabled(GL_TEXTURE_2D, g.saved.tex2d_on);
#endif
    }
    GLF(Viewport)(g.saved.viewport[0], g.saved.viewport[1],
                  g.saved.viewport[2], g.saved.viewport[3]);
    GLF(Scissor)(g.saved.scissor[0], g.saved.scissor[1],
                 g.saved.scissor[2], g.saved.scissor[3]);
    set_enabled(GL_SCISSOR_TEST, g.saved.scissor_on);
    set_enabled(GL_BLEND, g.saved.blend);
    GLF(BlendFuncSeparate)((GLenum)g.saved.blend_src_rgb, (GLenum)g.saved.blend_dst_rgb,
                           (GLenum)g.saved.blend_src_a, (GLenum)g.saved.blend_dst_a);
    GLF(BlendEquationSeparate)((GLenum)g.saved.blend_eq_rgb, (GLenum)g.saved.blend_eq_a);
    set_enabled(GL_DEPTH_TEST, g.saved.depth_test);
    GLF(DepthFunc)((GLenum)g.saved.depth_func);
    GLF(DepthMask)(g.saved.depth_mask);
    GLF(ColorMask)(g.saved.color_mask[0], g.saved.color_mask[1],
                   g.saved.color_mask[2], g.saved.color_mask[3]);
    set_enabled(GL_CULL_FACE, g.saved.cull);
    GLF(CullFace)((GLenum)g.saved.cull_mode);
    GLF(FrontFace)((GLenum)g.saved.front_face);
    GLF(ClearColor)(g.saved.clear_color[0], g.saved.clear_color[1],
                    g.saved.clear_color[2], g.saved.clear_color[3]);
#ifndef __EMSCRIPTEN__
    if (gl.Color4f) {
        gl.Color4f(g.saved.color[0], g.saved.color[1], g.saved.color[2], g.saved.color[3]);
    }
    if (gl.EnableClientState && gl.DisableClientState) {
        (g.saved.va_vertex ? gl.EnableClientState : gl.DisableClientState)(GL_VERTEX_ARRAY);
        (g.saved.va_color ? gl.EnableClientState : gl.DisableClientState)(GL_COLOR_ARRAY);
        (g.saved.va_texcoord ? gl.EnableClientState : gl.DisableClientState)(GL_TEXTURE_COORD_ARRAY);
    }
#endif
    (void)GLF(GetError)();
}

#ifdef __EMSCRIPTEN__
/* The framebuffer SDL binds for its target texture has no depth
 * attachment, so one of ours is attached each frame. Without it
 * nothing in the scene is depth tested. */
static void attach_depth(int w, int h) {
    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    if (fbo == 0) return;
    if (g.depth_rb && (g.depth_w != w || g.depth_h != h)) {
        glDeleteRenderbuffers(1, &g.depth_rb);
        g.depth_rb = 0;
    }
    if (!g.depth_rb) {
        glGenRenderbuffers(1, &g.depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, g.depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        g.depth_w = w;
        g.depth_h = h;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              g.depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    }
}
#endif

void GL3D_BeginFrame(SDL_Renderer *renderer, int win_w, int win_h,
                     const SDL_Rect *viewport, const float sky[3]) {
    if (!g.ready || g.frame_open) return;
    SDL_RenderFlush(renderer);
    save_state();
    g.frame_open = 1;
    g.bound_sdl_tex = NULL;

    /* The viewport in drawable pixels, y up, which on a high density
     * screen is not the window's own pixel grid. */
    int dw = win_w, dh = win_h;
    SDL_GL_GetDrawableSize(g.window, &dw, &dh);
    if (win_w < 1) win_w = 1;
    if (win_h < 1) win_h = 1;
    SDL_Rect r = viewport ? *viewport : (SDL_Rect){ 0, 0, win_w, win_h };
    int x0 = r.x * dw / win_w;
    int x1 = (r.x + r.w) * dw / win_w;
    int y0 = r.y * dh / win_h;
    int y1 = (r.y + r.h) * dh / win_h;
#ifdef __EMSCRIPTEN__
    {
        int tw = x1 - x0, th = y1 - y0;
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        if (!g.target || g.target_w != tw || g.target_h != th) {
            if (g.target) SDL_DestroyTexture(g.target);
            if (g.depth_rb) { glDeleteRenderbuffers(1, &g.depth_rb); g.depth_rb = 0; }
            g.target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_TARGET, tw, th);
            g.target_w = tw;
            g.target_h = th;
        }
        g.target_dst = r;
        g.in_target = 0;
        /* SDL binds its own framebuffer for the target and flushes,
         * so the GL draws that follow land in the texture. */
        if (g.target && SDL_SetRenderTarget(renderer, g.target) == 0) {
            SDL_RenderFlush(renderer);
            g.in_target = 1;
            attach_depth(tw, th);
            GLF(Viewport)(0, 0, tw, th);
            GLF(Scissor)(0, 0, tw, th);
        } else {
            GLF(Viewport)(x0, dh - y1, x1 - x0, y1 - y0);
            GLF(Scissor)(x0, dh - y1, x1 - x0, y1 - y0);
        }
    }
#else
    GLF(Viewport)(x0, dh - y1, x1 - x0, y1 - y0);
    GLF(Scissor)(x0, dh - y1, x1 - x0, y1 - y0);
#endif
    GLF(Enable)(GL_SCISSOR_TEST);

    GLF(ColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    GLF(DepthMask)(GL_TRUE);
    GLF(ClearColor)(sky[0], sky[1], sky[2], 1.0f);
    GLF(Clear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    GLF(Enable)(GL_DEPTH_TEST);
    GLF(DepthFunc)(GL_LEQUAL);
    GLF(Disable)(GL_BLEND);
    GLF(Disable)(GL_CULL_FACE);
    GLF(ActiveTexture)(GL_TEXTURE0);
#ifndef __EMSCRIPTEN__
    GLF(Disable)(GL_TEXTURE_2D);
#endif
    for (int i = 0; i < 8; i++) GLF(DisableVertexAttribArray)((GLuint)i);
}

void GL3D_EndFrame(void) {
    if (!g.ready || !g.frame_open) return;
    restore_state();
    g.frame_open = 0;
#ifdef __EMSCRIPTEN__
    if (g.in_target) {
        SDL_SetRenderTarget(g.renderer, NULL);
        g.in_target = 0;
        /* Rows run top down in the texture and the scene was drawn
         * bottom up, hence the flip. */
        SDL_RenderCopyEx(g.renderer, g.target, NULL, &g.target_dst, 0.0, NULL,
                         SDL_FLIP_VERTICAL);
    }
#endif
}

void GL3D_SetCamera(const float viewproj[16], const float eye[3],
                    const float light_dir[3]) {
    memcpy(g.viewproj, viewproj, sizeof(g.viewproj));
    memcpy(g.eye, eye, sizeof(g.eye));
    memcpy(g.light, light_dir, sizeof(g.light));
}

void GL3D_SetFog(GL3D_Texture *fog, float map_w, float map_h) {
    g.fog = fog;
    g.map_w = map_w > 0.0f ? map_w : 1.0f;
    g.map_h = map_h > 0.0f ? map_h : 1.0f;
}

/* ── Textures ─────────────────────────────────────────────────────── */

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif

/* Our images are tightly packed. SDL leaves the row length of its last
 * upload behind, and it sets its own again before its next one. */
static void unpack_tight(int alignment) {
    GLF(PixelStorei)(GL_UNPACK_ALIGNMENT, alignment);
#ifndef __EMSCRIPTEN__
    GLF(PixelStorei)(GL_UNPACK_ROW_LENGTH, 0);
#endif
}

GL3D_Texture *GL3D_UploadTextureRGBA(const uint32_t *rgba, int w, int h,
                                     int mipmap, int linear) {
    if (!g.ready || !rgba || w <= 0 || h <= 0) return NULL;
    GL3D_Texture *t = (GL3D_Texture *)tak_malloc(sizeof(*t));
    if (!t) return NULL;
    t->w = w; t->h = h;
    GLF(GenTextures)(1, &t->id);
    GLF(BindTexture)(GL_TEXTURE_2D, t->id);
    unpack_tight(4);
    GLF(TexImage2D)(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    /* WebGL 1 samples a mipmapped texture of any other size as black. */
    int pot = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    int have_mips = 0;
#ifdef __EMSCRIPTEN__
    if (mipmap && pot) { glGenerateMipmap(GL_TEXTURE_2D); have_mips = 1; }
#else
    if (mipmap && pot && gl.GenerateMipmap) { gl.GenerateMipmap(GL_TEXTURE_2D); have_mips = 1; }
#endif
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
        have_mips ? GL_LINEAR_MIPMAP_LINEAR : (linear ? GL_LINEAR : GL_NEAREST));
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLF(BindTexture)(GL_TEXTURE_2D, 0);
    return t;
}

GL3D_Texture *GL3D_UploadTextureGrey(const uint8_t *grey, int w, int h) {
    if (!g.ready || !grey || w <= 0 || h <= 0) return NULL;
    GL3D_Texture *t = (GL3D_Texture *)tak_malloc(sizeof(*t));
    if (!t) return NULL;
    t->w = w; t->h = h;
    GLF(GenTextures)(1, &t->id);
    GLF(BindTexture)(GL_TEXTURE_2D, t->id);
    unpack_tight(1);
    GLF(TexImage2D)(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, grey);
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GLF(TexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLF(PixelStorei)(GL_UNPACK_ALIGNMENT, 4);
    GLF(BindTexture)(GL_TEXTURE_2D, 0);
    return t;
}

void GL3D_UpdateTextureGrey(GL3D_Texture *tex, const uint8_t *grey, int w, int h) {
    if (!g.ready || !tex || !grey || w != tex->w || h != tex->h) return;
    GLF(BindTexture)(GL_TEXTURE_2D, tex->id);
    unpack_tight(1);
    GLF(TexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, grey);
    GLF(PixelStorei)(GL_UNPACK_ALIGNMENT, 4);
    GLF(BindTexture)(GL_TEXTURE_2D, 0);
}

void GL3D_FreeTexture(GL3D_Texture *tex) {
    if (!tex) return;
    if (g.ready && tex->id) GLF(DeleteTextures)(1, &tex->id);
    tak_free(tex);
}

/* ── Meshes ───────────────────────────────────────────────────────── */

GL3D_Mesh *GL3D_UploadMesh(GL3D_Layout layout, const float *verts, int vert_count,
                           const uint16_t *indices, int index_count) {
    if (!g.ready || !verts || !indices || vert_count <= 0 || index_count <= 0) return NULL;
    int floats = GL3D_LayoutFloats(layout);
    if (floats == 0) return NULL;
    GL3D_Mesh *m = (GL3D_Mesh *)tak_malloc(sizeof(*m));
    if (!m) return NULL;
    m->layout = layout;
    m->vert_count = vert_count;
    m->index_count = index_count;
    GLF(GenBuffers)(1, &m->vbo);
    GLF(GenBuffers)(1, &m->ibo);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, m->vbo);
    GLF(BufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * (size_t)floats * (size_t)vert_count),
                    verts, GL_STATIC_DRAW);
    GLF(BindBuffer)(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    GLF(BufferData)(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint16_t) * (size_t)index_count),
                    indices, GL_STATIC_DRAW);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, 0);
    GLF(BindBuffer)(GL_ELEMENT_ARRAY_BUFFER, 0);
    return m;
}

void GL3D_FreeMesh(GL3D_Mesh *mesh) {
    if (!mesh) return;
    if (g.ready) {
        if (mesh->vbo) GLF(DeleteBuffers)(1, &mesh->vbo);
        if (mesh->ibo) GLF(DeleteBuffers)(1, &mesh->ibo);
    }
    tak_free(mesh);
}

/* Point the attribute arrays at a bound buffer of the given layout. */
static void bind_layout(GL3D_Layout layout) {
    GLsizei stride = (GLsizei)(sizeof(float) * (size_t)GL3D_LayoutFloats(layout));
    const char *base = NULL;
    for (int i = 0; i < 8; i++) GLF(DisableVertexAttribArray)((GLuint)i);
    switch (layout) {
    case GL3D_LAYOUT_TERRAIN:
        GLF(VertexAttribPointer)(0, 3, GL_FLOAT, GL_FALSE, stride, base + 0);
        GLF(VertexAttribPointer)(1, 3, GL_FLOAT, GL_FALSE, stride, base + 12);
        GLF(VertexAttribPointer)(2, 2, GL_FLOAT, GL_FALSE, stride, base + 24);
        GLF(EnableVertexAttribArray)(0);
        GLF(EnableVertexAttribArray)(1);
        GLF(EnableVertexAttribArray)(2);
        break;
    case GL3D_LAYOUT_MODEL:
        GLF(VertexAttribPointer)(0, 3, GL_FLOAT, GL_FALSE, stride, base + 0);
        GLF(VertexAttribPointer)(1, 3, GL_FLOAT, GL_FALSE, stride, base + 12);
        GLF(VertexAttribPointer)(2, 2, GL_FLOAT, GL_FALSE, stride, base + 24);
        GLF(VertexAttribPointer)(3, 4, GL_FLOAT, GL_FALSE, stride, base + 32);
        GLF(VertexAttribPointer)(4, 1, GL_FLOAT, GL_FALSE, stride, base + 48);
        for (int i = 0; i < 5; i++) GLF(EnableVertexAttribArray)((GLuint)i);
        break;
    case GL3D_LAYOUT_MODEL_PBR:
        GLF(VertexAttribPointer)(0, 3, GL_FLOAT, GL_FALSE, stride, base + 0);
        GLF(VertexAttribPointer)(1, 3, GL_FLOAT, GL_FALSE, stride, base + 12);
        GLF(VertexAttribPointer)(2, 2, GL_FLOAT, GL_FALSE, stride, base + 24);
        GLF(VertexAttribPointer)(3, 4, GL_FLOAT, GL_FALSE, stride, base + 32);
        GLF(VertexAttribPointer)(4, 1, GL_FLOAT, GL_FALSE, stride, base + 48);
        GLF(VertexAttribPointer)(5, 4, GL_FLOAT, GL_FALSE, stride, base + 52);
        for (int i = 0; i < 6; i++) GLF(EnableVertexAttribArray)((GLuint)i);
        break;
    case GL3D_LAYOUT_SPRITE:
        GLF(VertexAttribPointer)(0, 3, GL_FLOAT, GL_FALSE, stride, base + 0);
        GLF(VertexAttribPointer)(2, 2, GL_FLOAT, GL_FALSE, stride, base + 12);
        GLF(VertexAttribPointer)(3, 4, GL_FLOAT, GL_FALSE, stride, base + 20);
        GLF(EnableVertexAttribArray)(0);
        GLF(EnableVertexAttribArray)(2);
        GLF(EnableVertexAttribArray)(3);
        break;
    }
}

static void use_common(const Program *p) {
    GLF(UseProgram)(p->id);
    GLF(UniformMatrix4fv)(p->u_vp, 1, GL_FALSE, g.viewproj);
    if (p->u_eye >= 0)   GLF(Uniform3f)(p->u_eye, g.eye[0], g.eye[1], g.eye[2]);
    if (p->u_light >= 0) GLF(Uniform3f)(p->u_light, g.light[0], g.light[1], g.light[2]);
    if (p->u_tex >= 0)   GLF(Uniform1i)(p->u_tex, 0);
}

/* Bind one of our textures on unit 0, or an SDL texture, and hand back
 * the coordinate scale the SDL texture wants. */
static void bind_texture(GL3D_Texture *tex, SDL_Texture *sdl_tex, float *sx, float *sy) {
    *sx = 1.0f; *sy = 1.0f;
    GLF(ActiveTexture)(GL_TEXTURE0);
    if (sdl_tex) {
        float tw = 1.0f, th = 1.0f;
        if (SDL_GL_BindTexture(sdl_tex, &tw, &th) == 0) {
            g.bound_sdl_tex = sdl_tex;
            *sx = tw; *sy = th;
        }
        return;
    }
    if (g.bound_sdl_tex) {
        SDL_GL_UnbindTexture(g.bound_sdl_tex);
        g.bound_sdl_tex = NULL;
    }
    GLF(BindTexture)(GL_TEXTURE_2D, tex ? tex->id : 0);
}

/* ── Draws ────────────────────────────────────────────────────────── */

void GL3D_DrawTerrain(const GL3D_Mesh *mesh, GL3D_Texture *tex,
                      int first_index, int index_count) {
    if (!g.ready || !g.frame_open || !mesh || index_count <= 0) return;
    const Program *p = &g.terrain;
    use_common(p);
    GLF(Uniform2f)(p->u_mapsize, g.map_w, g.map_h);
    float sx, sy;
    bind_texture(tex, NULL, &sx, &sy);
    GLF(ActiveTexture)(GL_TEXTURE1);
    GLF(BindTexture)(GL_TEXTURE_2D, g.fog ? g.fog->id : 0);
    GLF(Uniform1i)(p->u_fog, 1);
    GLF(ActiveTexture)(GL_TEXTURE0);
    GLF(Disable)(GL_BLEND);
    GLF(DepthMask)(GL_TRUE);
    GLF(Disable)(GL_CULL_FACE);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, mesh->vbo);
    GLF(BindBuffer)(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo);
    bind_layout(GL3D_LAYOUT_TERRAIN);
    GLF(DrawElements)(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT,
                      (const char *)NULL + (size_t)first_index * sizeof(uint16_t));
}

void GL3D_DrawModel(const GL3D_Mesh *mesh, const float model[16],
                    const float *node_xforms, const uint8_t *node_hidden,
                    int node_count, const GL3D_ModelBatch *batches,
                    int batch_count, float alpha) {
    if (!g.ready || !g.frame_open || !mesh || !node_xforms || node_count <= 0) return;
    const int pbr = mesh->layout == GL3D_LAYOUT_MODEL_PBR;
    const Program *p = pbr ? &g.model_pbr : &g.model;
    use_common(p);
    if (pbr) {
        GLF(Uniform1i)(p->u_nrmtex, 1);
        GLF(Uniform1i)(p->u_mrtex, 2);
        GLF(Uniform1i)(p->u_emtex, 3);
    }
    GLF(UniformMatrix4fv)(p->u_model, 1, GL_FALSE, model);
    GLF(Uniform1f)(p->u_alpha, alpha);
    /* The model to map mapping mirrors one axis, which turns a normal
     * the other way round. */
    GLF(Uniform1f)(p->u_nsign, -1.0f);
    if (alpha < 1.0f) {
        GLF(Enable)(GL_BLEND);
        GLF(BlendFunc)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        GLF(DepthMask)(GL_FALSE);
    } else {
        GLF(Disable)(GL_BLEND);
        GLF(DepthMask)(GL_TRUE);
    }
    GLF(Enable)(GL_CULL_FACE);
    GLF(CullFace)(GL_BACK);
    GLF(FrontFace)(GL_CW);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, mesh->vbo);
    GLF(BindBuffer)(GL_ELEMENT_ARRAY_BUFFER, mesh->ibo);
    bind_layout(mesh->layout);

    float rows[64 * 12];
    for (int b = 0; b < batch_count; b++) {
        const GL3D_ModelBatch *bt = &batches[b];
        if (bt->index_count <= 0) continue;
        int lo = bt->node_lo, hi = bt->node_hi;
        if (lo < 0) lo = 0;
        if (hi >= node_count) hi = node_count - 1;
        int n = hi - lo + 1;
        if (n <= 0 || n > g.max_nodes) continue;
        /* A hidden piece collapses onto its origin and draws nothing. */
        for (int i = 0; i < n; i++) {
            const float *x = node_xforms + (size_t)(lo + i) * 12;
            float *r = rows + (size_t)i * 12;
            if (node_hidden && node_hidden[lo + i]) {
                memset(r, 0, sizeof(float) * 12);
                continue;
            }
            r[0] = x[0]; r[1] = x[1]; r[2] = x[2]; r[3]  = x[9];
            r[4] = x[3]; r[5] = x[4]; r[6] = x[5]; r[7]  = x[10];
            r[8] = x[6]; r[9] = x[7]; r[10] = x[8]; r[11] = x[11];
        }
        GLF(Uniform4fv)(p->u_nodes, n * 3, rows);
        GLF(Uniform1f)(p->u_nodebase, (float)lo);
        float sx, sy;
        bind_texture(bt->tex, bt->sdl_tex, &sx, &sy);
        GLF(Uniform2f)(p->u_texscale, sx, sy);
        GLF(Uniform1f)(p->u_textured, (bt->tex || bt->sdl_tex) ? 1.0f : 0.0f);
        if (pbr) {
            /* The surface's maps, on their own units, and the state
             * this batch asks for. Blending parts come last in the
             * mesh, so what they blend over is already there. */
            GLF(ActiveTexture)(GL_TEXTURE1);
            GLF(BindTexture)(GL_TEXTURE_2D, bt->normal_tex ? bt->normal_tex->id : 0);
            GLF(ActiveTexture)(GL_TEXTURE2);
            GLF(BindTexture)(GL_TEXTURE_2D, bt->mr_tex ? bt->mr_tex->id : 0);
            GLF(ActiveTexture)(GL_TEXTURE3);
            GLF(BindTexture)(GL_TEXTURE_2D, bt->emissive_tex ? bt->emissive_tex->id : 0);
            GLF(ActiveTexture)(GL_TEXTURE0);
            GLF(Uniform1f)(p->u_hasnrm, bt->normal_tex ? 1.0f : 0.0f);
            GLF(Uniform1f)(p->u_hasmr, bt->mr_tex ? 1.0f : 0.0f);
            GLF(Uniform1f)(p->u_hasem, bt->emissive_tex ? 1.0f : 0.0f);
            GLF(Uniform3f)(p->u_emissive, bt->emissive[0], bt->emissive[1], bt->emissive[2]);
            GLF(Uniform1f)(p->u_metallic, bt->metallic);
            GLF(Uniform1f)(p->u_roughness, bt->roughness);
            GLF(Uniform1f)(p->u_nrmscale, bt->normal_scale);
            GLF(Uniform1f)(p->u_alphacut, bt->alpha_cutoff);
            GLF(Uniform1f)(p->u_blend, bt->blend ? 1.0f : 0.0f);
            if (bt->blend || alpha < 1.0f) {
                GLF(Enable)(GL_BLEND);
                GLF(BlendFunc)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                GLF(DepthMask)(GL_FALSE);
            } else {
                GLF(Disable)(GL_BLEND);
                GLF(DepthMask)(GL_TRUE);
            }
            if (bt->double_sided) GLF(Disable)(GL_CULL_FACE);
            else GLF(Enable)(GL_CULL_FACE);
        }
        GLF(DrawElements)(GL_TRIANGLES, bt->index_count, GL_UNSIGNED_SHORT,
                          (const char *)NULL + (size_t)bt->first_index * sizeof(uint16_t));
    }
    if (pbr) {
        for (int u = 3; u >= 1; u--) {
            GLF(ActiveTexture)(GL_TEXTURE0 + (GLenum)u);
            GLF(BindTexture)(GL_TEXTURE_2D, 0);
        }
        GLF(ActiveTexture)(GL_TEXTURE0);
    }
    GLF(Disable)(GL_CULL_FACE);
    GLF(DepthMask)(GL_TRUE);
    GLF(Disable)(GL_BLEND);
}

void GL3D_DrawSprites(const float *verts, int vert_count,
                      const uint16_t *indices, int index_count,
                      GL3D_Texture *tex, int alpha_cut, int blend) {
    if (!g.ready || !g.frame_open || !verts || !indices) return;
    if (vert_count <= 0 || index_count <= 0) return;
    const Program *p = &g.sprite;
    use_common(p);
    float sx, sy;
    bind_texture(tex, NULL, &sx, &sy);
    GLF(Uniform1f)(p->u_textured, tex ? 1.0f : 0.0f);
    GLF(Uniform1f)(p->u_alphacut, alpha_cut ? 0.5f : 0.0f);
    /* Blended surfaces and billboards vanish under the fog the way the
     * ground does; the plain rings draw as they are. */
    GLF(Uniform1f)(p->u_usefog, (g.fog && (blend || tex)) ? 1.0f : 0.0f);
    GLF(Uniform2f)(p->u_mapsize, g.map_w, g.map_h);
    GLF(ActiveTexture)(GL_TEXTURE1);
    GLF(BindTexture)(GL_TEXTURE_2D, g.fog ? g.fog->id : 0);
    GLF(Uniform1i)(p->u_fog, 1);
    GLF(ActiveTexture)(GL_TEXTURE0);
    if (blend) {
        GLF(Enable)(GL_BLEND);
        GLF(BlendFunc)(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        GLF(DepthMask)(GL_FALSE);
    } else {
        GLF(Disable)(GL_BLEND);
        GLF(DepthMask)(GL_TRUE);
    }
    GLF(Disable)(GL_CULL_FACE);
    GLF(BindBuffer)(GL_ARRAY_BUFFER, g.stream_vbo);
    GLF(BufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(float) * 9 * (size_t)vert_count),
                    verts, GL_STREAM_DRAW);
    GLF(BindBuffer)(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo);
    GLF(BufferData)(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(uint16_t) * (size_t)index_count),
                    indices, GL_STREAM_DRAW);
    bind_layout(GL3D_LAYOUT_SPRITE);
    GLF(DrawElements)(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, NULL);
    GLF(DepthMask)(GL_TRUE);
    GLF(Disable)(GL_BLEND);
}
