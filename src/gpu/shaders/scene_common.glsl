// What the scene shaders share: the polygon and viewport records the CPU
// fills from m3::Real3dScene (gpu::Real3dGpu, std430, 16-byte fields).

struct Polygon {
    vec4 colour;  // r, g, b, alpha
    vec4 values;  // fog intensity, specular value, texture level base, -
    uvec4 tex;    // x, y, width, height (texels)
    uvec4 tex2;   // page y (0 or 1024), last mipmap level, wrap u | wrap v << 8, format
    uvec4 flags;  // kFlag* bits; micro id | micro min level << 8 | shininess << 16; viewport; -
};

struct Viewport {
    vec4 rect;            // x, y, width, height (pixels of the 496x384 screen)
    vec4 frustum;         // left, right, bottom, top (at distance 1)
    vec4 sun;             // direction, intensity
    vec4 light;           // ambient, fog density, fog start, fog attenuation
    vec4 fog_colour;      // times the fog ambient
    vec4 fog_colour_set;  // as set
    vec4 spot;            // centre x, y, radius x, y (pixels)
    vec4 spot2;           // start, extent, on (1/0), -
    vec4 spot_colour;
    vec4 spot_fog_colour;
};

// A quad drawn whole (--quads): its corners in eye space, their texture
// position and fixed shade, and their normals; position[0].w its depth bound
// (0: none; scene.vert).
struct Quad {
    vec4 position[4]; // x, y, z, - (the first: the depth bound)
    vec4 texture[4];  // u, v, shade, -
    vec4 normal[4];
};
const uint kNoQuad = 0xFFFFFFFFu;

const uint kFlagLit = 1u, kFlagFixedShading = 2u, kFlagSpecular = 4u, kFlagSmooth = 8u, kFlagTextured = 16u,
           kFlagContour = 32u, kFlagTextureAlpha = 64u, kFlagPolyAlpha = 128u, kFlagLayered = 256u,
           kFlagMicro = 512u, kFlagInverted = 1024u;

// The near plane, and how much nearer a decal (layered polygon) counts:
// depth is kNear / distance (times that), nearer is larger.
const float kNear = 1e-3;
const float kDecalBias = 1.0 + 1.0 / 512.0;
