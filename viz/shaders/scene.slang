// scene.slang — the viewer's scene pass.
//
// Conventions shared with the physics core (PLAN.md §6.3):
//   * world units are metres, +Y up, right-handed;
//   * the plate's macro basis (macroN/macroU/macroV) is computed on the host with
//     the same formula the compute shaders use, so the drawn mesh and the traced
//     rays agree by construction;
//   * the plate grid is sampled cell-edged, hx = -W/2 + i*W/(GS-1), exactly like
//     shaders/forward.slang, and read straight out of the compute-written yGrid /
//     nGrid storage buffers (vertex pulling: no vertex or index buffer at all);
//   * the receiver cylinder uses the *same* parameterisation as
//     common.slang::computeReceiverSample: angle = px*2pi/157,
//     P = (R sin, cy + py*(H/50) - H/2, -R cos), so the flux texture maps 1:1
//     onto the drawn surface (one quad per receiver pixel);
//   * depth is reversed-Z with an infinite far plane: near -> 1, infinity -> 0,
//     depth test GREATER, clear 0.
//
// The matrix is passed as four explicit float4 rows and applied with dot()
// products, so there is no matrix-layout ambiguity between Slang and SPIR-V.

// ------------------------------------------------------------------- UBO ---
struct SceneUBO {
    float4 row0;          // view-projection row 0 (column-vector convention)
    float4 row1;
    float4 row2;
    float4 row3;
    float4 camPos;        // xyz = eye
    float4 camForward;    // xyz = forward, w = tan(fovY/2)
    float4 camRight;      // xyz = right,   w = aspect
    float4 camUp;         // xyz = up
    float4 sunDir;        // xyz = direction towards the sun, w = dni/1000
    float4 macroN;        // plate macro normal (world)
    float4 macroU;        // plate macro u (world)
    float4 macroV;        // plate macro v (world)
    float4 plate;         // x = width, y = length, z = gridSize, w = deform scale
    float4 receiverP;     // x = radius, y = height, z = cx, w = cy
    float4 receiverPos;   // xyz = receiver centre, w = pixelWidth
    float4 helioPos;      // xyz = plate centre
    float4 flux;          // x = multiplier, y = log floor, z = log ceil, w = exposure
    float4 options;       // x = debug view, y = show beams, z = heat mode, w = time
    float4 ground;        // x = grid spacing, y = fade radius, z = beam intensity, w = pixelHeight
    float4 aimPoint;      // xyz = aim point on the receiver (drives the reflected beam)
    float4 viewport;      // x = width px, y = height px, z = 1/width, w = 1/height
    float4 uiParams;      // x,y,w,h = flux-map inset rect in pixels (w < 0 disables it)
    // One record per heliostat of the field: pos.xyz + w = 1 when it is the
    // selected mirror (the only one that is ray traced and deformable),
    // nrm/u/v = its macro basis (flat-plate orientation).
    float4 helioRec[4][4];   // [i][0] = pos|selected, [i][1] = macroN, [i][2] = macroU, [i][3] = macroV
    float4 stats;         // x = S95 level W/px (drawn as a contour on the inset), y = flux peak
    // xy = flux-weighted centroid of the map, in 0..1 texture coordinates, computed
    // on the CPU from the readback. The inset rolls the cylinder (u is periodic) so
    // that the spot always lands in the middle of the panel, whichever mirror is
    // being traced -- without this, a spot that straddles the u = 0 seam is drawn
    // cut in half.
    float4 fluxCenter;    // x = u centre (0..1, periodic), y = v centre
};

// The scene pass has its own descriptor set layout at set index 0. The reused
// compute chain keeps *its* layout (the upstream binding table) at its own set 0:
// descriptor sets are per-pipeline-layout objects, so there is no conflict, and
// no dummy bindings have to be threaded through the graphics pipelines.
[[vk::binding(0)]] ConstantBuffer<SceneUBO> scene;
[[vk::binding(1)]] Sampler2D fluxTex;   // combined image sampler: one binding
[[vk::binding(2)]] StructuredBuffer<float> yGrid;
[[vk::binding(3)]] StructuredBuffer<float4> nGrid;
[[vk::binding(4)]] StructuredBuffer<float> boltHeights;

struct ScenePC {
    uint mode;     // 0 ground, 1 tower, 2 receiver, 3 plate, 4 bolt, 5 beam, 6 foundation
    uint res;      // columns / grid size / segments
    uint extra;    // rows / bolt index / beam index
    float scale;   // object-specific scale (bolt radius in m, beam width, ...)
};
[[vk::push_constant]] ScenePC pc;

static const float kPi = 3.14159265358979323846f;

float4 toClip(float3 worldPos) {
    float4 p = float4(worldPos, 1.0f);
    return float4(dot(p, scene.row0), dot(p, scene.row1), dot(p, scene.row2), dot(p, scene.row3));
}

float3 toWorldDir(float3 local) {
    return local.x * scene.macroU.xyz + local.y * scene.macroN.xyz + local.z * scene.macroV.xyz;
}

// Per-heliostat variants: the field draws every mirror, but only the selected one
// has a computed surface (yGrid/nGrid); the others are drawn as flat plates aimed
// at the receiver, which is also visually how an idle heliostat looks.
float3 helioPosOf(uint i) { return scene.helioRec[i][0].xyz; }
bool helioSelected(uint i) { return scene.helioRec[i][0].w > 0.5f; }
float3 helioToWorldDir(uint i, float3 local) {
    return local.x * scene.helioRec[i][2].xyz + local.y * scene.helioRec[i][1].xyz +
           local.z * scene.helioRec[i][3].xyz;
}

// --------------------------------------------------------------- geometry ---
// Vertex-index driven "mesh" generators. Each draw call supplies one object and
// the vertex shader expands it; there is no vertex or index buffer anywhere.

// Quad helper: local (i,j) in [0,1]^2, corner index 0..5 -> two CCW triangles.
void quadCorner(uint corner, out float u, out float v) {
    if (corner == 0u)      { u = 0.0f; v = 0.0f; }
    else if (corner == 1u) { u = 1.0f; v = 0.0f; }
    else if (corner == 2u) { u = 1.0f; v = 1.0f; }
    else if (corner == 3u) { u = 0.0f; v = 0.0f; }
    else if (corner == 4u) { u = 1.0f; v = 1.0f; }
    else                   { u = 0.0f; v = 1.0f; }
}

// Ground: a large quad at y = groundLevel, centred on the receiver.
static const float kGroundLevel = -7.0f;

// Foundation + tower dimensions (visual only; the physics has no tower).
static const float kTowerRadius = 3.0f;
static const float kPedestalRadius = 1.1f;

struct VSOut {
    float4 pos : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float2 uv : TEXCOORD2;      // flux texture coordinate / screen uv for sky
    float2 extra : TEXCOORD3;   // raw grid coordinates (debug views)
    float4 color : COLOR0;      // emissive tint (bolts, beams)
};

// ---------------------------------------------------------------- sky (D0) --
struct SkyOut {
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};
[shader("vertex")]
SkyOut vsSky(uint id : SV_VertexID) {
    // Fullscreen triangle from gl_VertexIndex: no vertex buffer is bound.
    float2 p = float2((id == 2u) ? 3.0f : -1.0f, (id == 1u) ? 3.0f : -1.0f);
    SkyOut o;
    o.pos = float4(p, 0.0f, 1.0f);   // z = 0 = infinitely far in reversed-Z
    o.ndc = p;
    return o;
}

float3 skyColor(float3 dir) {
    const float3 zenith = float3(0.055f, 0.16f, 0.42f);
    const float3 horizon = float3(0.62f, 0.72f, 0.90f);
    float t = saturate(dir.y);
    float3 sky = lerp(horizon, zenith, pow(t, 0.55f));
    // Below the horizon fade to a dusty ground haze (the ground plane takes over
    // wherever the camera can actually see it).
    sky = lerp(sky, float3(0.19f, 0.18f, 0.17f), saturate(-dir.y * 4.0f));

    float3 sun = normalize(scene.sunDir.xyz);
    float c = saturate(dot(dir, sun));
    float disc = smoothstep(0.99980f, 0.99993f, c);
    float glow = pow(c, 300.0f) * 4.0f + pow(c, 14.0f) * 0.30f + pow(c, 3.0f) * 0.03f;
    float3 sunCol = float3(1.0f, 0.93f, 0.80f);
    return sky + sunCol * (disc * 16.0f + glow);
}

float3 skyRay(float2 ndc) {
    const float tanHalf = scene.camForward.w;
    const float aspect = scene.camRight.w;
    return normalize(scene.camForward.xyz + scene.camRight.xyz * (ndc.x * tanHalf * aspect) +
                     scene.camUp.xyz * (ndc.y * tanHalf));
}

[shader("fragment")]
float4 fsSky(SkyOut i) : SV_Target {
    float3 col = skyColor(skyRay(i.ndc));
    if (scene.options.x > 1.5f && scene.options.x < 2.5f) {
        // Debug view (F7): flat sky so the geometry heat map is readable.
        col = float3(0.05f, 0.06f, 0.08f);
    }
    return float4(col, 1.0f);
}

// ------------------------------------------------------------ scene vertex --
// Actuator node layout: (u, v) in [0,1]^2 on the plate, recovered from the
// influence-function peaks in data_proxy/influence_phi.bin (the bolt positions
// themselves live upstream in the optimizer). The layout is not a uniform grid:
// the outer columns sit closer to the edge, which is what the 7x5 "chebyshev-ish"
// actuator pattern produces. Values are the refined peak positions of phi_b.
static const float2 kBoltUV[35] = {
    {0.000000f, 0.000000f}, {0.209679f, 0.043892f}, {0.355320f, 0.048503f},
    {0.500000f, 0.041110f}, {0.644680f, 0.048503f}, {0.790321f, 0.043892f},
    {1.000000f, 0.000000f}, {0.042627f, 0.281803f}, {0.209791f, 0.282209f},
    {0.355308f, 0.283050f}, {0.500000f, 0.282380f}, {0.644692f, 0.283050f},
    {0.790209f, 0.282209f}, {0.957373f, 0.281803f}, {0.037641f, 0.500000f},
    {0.209845f, 0.500000f}, {0.355326f, 0.500000f}, {0.500000f, 0.500000f},
    {0.644674f, 0.500000f}, {0.790155f, 0.500000f}, {0.962359f, 0.500000f},
    {0.042627f, 0.718197f}, {0.209791f, 0.717791f}, {0.355308f, 0.716950f},
    {0.500000f, 0.717620f}, {0.644692f, 0.716950f}, {0.790209f, 0.717791f},
    {0.957373f, 0.718197f}, {0.000000f, 1.000000f}, {0.209679f, 0.956108f},
    {0.355320f, 0.951497f}, {0.500000f, 0.958890f}, {0.644680f, 0.951497f},
    {0.790321f, 0.956108f}, {1.000000f, 1.000000f},
};

[shader("vertex")]
VSOut vsScene(uint vid : SV_VertexID) {
    VSOut o;
    o.nrm = float3(0.0f, 1.0f, 0.0f);
    o.uv = float2(0.0f, 0.0f);
    o.extra = float2(0.0f, 0.0f);
    o.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float3 world = float3(0.0f, 0.0f, 0.0f);

    const float3 recvC = float3(scene.receiverP.z, scene.receiverP.w, scene.receiverPos.z);
    const float radius = scene.receiverP.x;
    const float height = scene.receiverP.y;
    const uint px = uint(scene.receiverPos.w);        // 157
    const uint py = uint(max(scene.ground.w, 1.0f));  // 50

    if (pc.mode == 0u) {
        // ---- ground quad (two triangles, from a 4-corner index) ----
        const float extent = 2000.0f;
        float u, v;
        quadCorner(vid, u, v);
        // Keep the field centred on the receiver with a little forward bias so
        // the far edge is not clipped by the 2000 m near/far design.
        world = float3(recvC.x + (u - 0.5f) * 2.0f * extent, kGroundLevel, recvC.z + (v - 0.5f) * 2.0f * extent);
        o.nrm = float3(0.0f, 1.0f, 0.0f);
    } else if (pc.mode == 1u) {
        // ---- tower: vertical cylinder from the ground to the receiver ----
        const uint seg = pc.res;
        uint face = vid / 6u, corner = vid % 6u;
        float cu, cv;
        quadCorner(corner, cu, cv);
        float a0 = (float(face) + cu) * (2.0f * kPi / float(seg));
        float y = lerp(kGroundLevel, recvC.y - height * 0.5f, cv);
        float3 n = float3(sin(a0), 0.0f, -cos(a0));
        world = float3(recvC.x, y, recvC.z) + n * kTowerRadius;
        o.nrm = n;
        o.extra = float2(float(face) / float(seg), cv);
    } else if (pc.mode == 2u) {
        // ---- receiver cylinder: one quad per flux texel ----
        // Corner convention identical to computeReceiverSample(): px indexes the
        // angle, py the height, and the texture coordinate is the pixel *centre*
        // so bilinear filtering reproduces the pixel grid.
        uint face = vid / 6u, corner = vid % 6u;          // face = row * px + col
        uint col = face % px, row = face / px;
        float cu, cv;
        quadCorner(corner, cu, cv);
        float colf = float(col) + cu;
        float rowf = float(row) + cv;
        // Unwrapped indices so the wrap-around column interpolates forward.
        float a = colf * (2.0f * kPi / float(px));
        float y = recvC.y - height * 0.5f + rowf * (height / float(py));
        float3 n = float3(sin(a), 0.0f, -cos(a));
        world = float3(recvC.x + radius * sin(a), y, recvC.z - radius * cos(a));
        o.nrm = n;
        o.uv = float2((colf + 0.5f) / float(px), (rowf + 0.5f) / float(py));
        o.extra = float2(colf, rowf);
    } else if (pc.mode == 3u) {
        // ---- plate: vertex pulling from the compute-written yGrid / nGrid ----
        const uint gs = pc.res;               // 32
        const uint cells = gs - 1u;
        uint quad = vid / 6u, corner = vid % 6u;
        uint i = quad % cells, j = quad / cells;
        float cu, cv;
        quadCorner(corner, cu, cv);
        float fi = float(i) + cu, fj = float(j) + cv;
        // Cell-edged sampling, exactly as forward.slang does it.
        float hx = -scene.plate.x * 0.5f + fi * (scene.plate.x / float(cells));
        float hz = -scene.plate.y * 0.5f + fj * (scene.plate.y / float(cells));
        uint iu = min(uint(fi), cells), iv = min(uint(fj), cells);
        uint gridIdx = iv * gs + iu;
        // pc.extra selects the heliostat of the field. Only the selected mirror has
        // a computed surface; the rest are drawn flat (their yGrid entries are
        // stale), aimed at the receiver with their own macro basis.
        const uint hi = pc.extra;
        const bool active = helioSelected(hi);
        float y = (active ? yGrid[gridIdx] : 0.0f) * scene.plate.w;   // w = deform exaggeration
        float3 localN = active ? nGrid[gridIdx].xyz : float3(0.0f, 1.0f, 0.0f);
        uint gridIdxSafe = min(gridIdx, 1023u);
        world = helioPosOf(hi) + helioToWorldDir(hi, float3(hx, y, hz));
        o.nrm = normalize(helioToWorldDir(hi, active ? nGrid[gridIdxSafe].xyz : float3(0.0f, 1.0f, 0.0f)));
        o.extra = float2(fi / float(cells), fj / float(cells));
        o.uv = float2(fi, fj);
        // color.xyz carries this plate's macro normal (used by the debug views),
        // color.w marks the active/selected mirror.
        o.color = float4(normalize(scene.helioRec[hi][1].xyz), active ? 1.0f : 0.0f);
    } else if (pc.mode == 4u) {
        // ---- actuator marker: a small box sitting ON the bolt node ----
        //
        // Built exactly like every other primitive in this file: the six faces are
        // selected with explicit branches and the corners come from quadCorner(), so
        // there is *no dynamic array indexing* in this path. The previous version
        // indexed local arrays (`dirs[tri[face][k]]`) to build an octahedron, and that
        // path silently produced no geometry at all in some views (measured: a 2 m
        // marker covered 155k pixels in the receiver closeup and exactly 0 pixels in
        // the overview, at the same scale and mirror).
        //
        // The marker is also drawn in the *per-mirror* frame (pc.extra = the mirror's
        // field index) instead of the scene-level macro basis: that basis is the
        // engine's, which includes the beam-target aim offset, so moving the beam
        // target used to rotate the whole marker cloud relative to its plate (measured
        // 7.9 deg of frame error, every corner 0.83 m off the plate, worst on the
        // South mirror whose near-vertical normal makes that frame hypersensitive).
        //
        // The height comes from the grid (bilinear, i.e. the surface the plate's
        // triangles interpolate) and the marker is anchored so its base sits on the
        // surface instead of being half buried in the opaque plate.
        uint perBolt = 36u;                       // 6 faces * 2 triangles * 3 vertices
        uint bolt = vid / perBolt;
        uint v = vid % perBolt;
        uint face = v / 6u;                       // 0..5
        float cu, cv;
        quadCorner(v % 6u, cu, cv);
        float3 fn, fa, fb;                        // face normal + the two in-plane axes
        if (face == 0u)      { fn = float3( 1, 0, 0); fa = float3(0, 1, 0); fb = float3(0, 0, 1); }
        else if (face == 1u) { fn = float3(-1, 0, 0); fa = float3(0, 1, 0); fb = float3(0, 0, 1); }
        else if (face == 2u) { fn = float3( 0, 1, 0); fa = float3(1, 0, 0); fb = float3(0, 0, 1); }
        else if (face == 3u) { fn = float3( 0,-1, 0); fa = float3(1, 0, 0); fb = float3(0, 0, 1); }
        else if (face == 4u) { fn = float3( 0, 0, 1); fa = float3(1, 0, 0); fb = float3(0, 1, 0); }
        else                 { fn = float3( 0, 0,-1); fa = float3(1, 0, 0); fb = float3(0, 1, 0); }

        uint gs = pc.res;
        const uint hi = pc.extra;                 // the mirror these markers belong to
        const bool boltActive = helioSelected(hi);
        float2 uv01 = kBoltUV[bolt];
        float y = 0.0f;
        if (boltActive) {
            float fi = uv01.x * float(gs - 1u);
            float fj = uv01.y * float(gs - 1u);
            uint i0 = min(uint(fi), gs - 2u), j0 = min(uint(fj), gs - 2u);
            float fx = fi - float(i0), fz = fj - float(j0);
            float y00 = yGrid[j0 * gs + i0], y10 = yGrid[j0 * gs + i0 + 1u];
            float y01 = yGrid[(j0 + 1u) * gs + i0], y11 = yGrid[(j0 + 1u) * gs + i0 + 1u];
            y = lerp(lerp(y00, y10, fx), lerp(y01, y11, fx), fz) * scene.plate.w;
        }
        float hx = (uv01.x - 0.5f) * scene.plate.x;
        float hz = (uv01.y - 0.5f) * scene.plate.y;
        // Everything is built in the plate's local frame: +x = width, +y = surface
        // normal, +z = length. The base face touches the surface (+2% so it never
        // z-fights the plate), the box is 2*scale on a side.
        const float3 local = float3(hx, y + pc.scale * 1.02f, hz) + fn * pc.scale +
                             fa * ((cu * 2.0f - 1.0f) * pc.scale) + fb * ((cv * 2.0f - 1.0f) * pc.scale);
        world = helioPosOf(hi) + helioToWorldDir(hi, local);
        o.nrm = normalize(helioToWorldDir(hi, fn));
        o.color = float4(0.9f, 0.55f, 0.1f, 1.0f) * (1.0f + abs(boltHeights[min(bolt, 34u)]) * 40.0f);
    } else if (pc.mode == 5u) {
        // ---- light beam: camera-facing cross of two quads ----
        uint quadIdx = vid / 6u, corner = vid % 6u;   // 0..1 sun beam, 2..3 reflected
        float cu, cv;
        quadCorner(corner, cu, cv);
        float3 sun = normalize(scene.sunDir.xyz);
        // The reflected beam ends exactly on the physics aim point (which the
        // beam-target slider moves along the receiver circumference).
        float3 recvAim = scene.aimPoint.xyz;
        float3 a, b;
        if (quadIdx < 2u) { a = scene.helioPos.xyz; b = recvAim; }
        else { a = scene.helioPos.xyz; b = scene.helioPos.xyz + sun * 260.0f; }
        float3 axis = b - a;
        float3 toEye = normalize(scene.camPos.xyz - (a + axis * 0.5f));
        float3 side = normalize(cross(axis, toEye));
        if ((quadIdx % 2u) == 1u) side = normalize(cross(axis, side));
        float width = pc.scale * ((quadIdx < 2u) ? 1.0f : 1.6f);
        world = lerp(a, b, cv) + side * ((cu - 0.5f) * width);
        o.nrm = -toEye;
        o.uv = float2(cu, cv);
        // Tint: reflected (plate -> receiver) warm, incoming (sun -> plate) pale.
        o.color = (quadIdx < 2u) ? float4(1.0f, 0.60f, 0.22f, 1.0f) : float4(1.0f, 0.95f, 0.80f, 1.0f);
    } else if (pc.mode == 6u) {
        // ---- foundation slab under a plate (one per field heliostat) ----
        float u, v;
        quadCorner(vid, u, v);
        float3 hp = helioPosOf(pc.extra);
        world = float3(hp.x + (u - 0.5f) * 16.0f, kGroundLevel + 0.4f, hp.z + (v - 0.5f) * 12.0f);
    } else if (pc.mode == 7u) {
        // ---- cardinal axis marker painted on the field: 0=N, 1=E, 2=S, 3=W ----
        // The world axes of this scene: +Z is where the heliostat looks (south for a
        // northern-hemisphere field), +X is east. The markers make that mapping
        // readable in any camera orientation. Written with explicit branches rather
        // than a dynamically indexed array: it keeps the codegen obvious.
        float u, v;
        quadCorner(vid, u, v);
        const uint axis = pc.extra & 3u;
        float2 d = float2(0.0f, -1.0f);
        float3 col = float3(0.25f, 0.50f, 1.00f);
        if (axis == 1u) { d = float2(1.0f, 0.0f); col = float3(0.25f, 0.85f, 0.45f); }
        else if (axis == 2u) { d = float2(0.0f, 1.0f); col = float3(1.00f, 0.35f, 0.30f); }
        else if (axis == 3u) { d = float2(-1.0f, 0.0f); col = float3(0.98f, 0.72f, 0.20f); }
        const float2 perp = float2(-d.y, d.x);
        // Start away from the tower so the marker is not hidden by it (and not by
        // the HUD panel in the upper-left), and run well past the field radius.
        const float len = 320.0f;
        float3 c = float3(scene.receiverP.z, kGroundLevel + 2.0f, scene.receiverPos.z);
        const float along = lerp(22.0f, len, v);
        const float side = (u - 0.5f) * pc.scale;
        world = c + float3(d.x * along + perp.x * side, 0.0f, d.y * along + perp.y * side);
        o.nrm = float3(0.0f, 1.0f, 0.0f);
        o.color = float4(col, 1.0f);
    } else {
        // Unused mode: emit a degenerate vertex (that draw is simply not issued).
        world = scene.helioPos.xyz;
    }

    o.world = world;
    o.pos = toClip(world);
    return o;
}

// ----------------------------------------------------------- flux heat map --
// Compact viridis-like ramp (5 stops, smooth): perceptual, and it keeps the
// spot's structure readable after ACES + bloom.
float3 heatRamp(float t) {
    t = saturate(t);
    const float3 c0 = float3(0.267f, 0.005f, 0.329f);
    const float3 c1 = float3(0.188f, 0.407f, 0.556f);
    const float3 c2 = float3(0.208f, 0.718f, 0.472f);
    const float3 c3 = float3(0.993f, 0.906f, 0.143f);
    const float3 c4 = float3(0.992f, 0.984f, 0.752f);
    float s = t * 4.0f;
    if (s < 1.0f) return lerp(c0, c1, s);
    if (s < 2.0f) return lerp(c1, c2, s - 1.0f);
    if (s < 3.0f) return lerp(c2, c3, s - 2.0f);
    return lerp(c3, c4, s - 3.0f);
}

// log10(1+x) normalisation between the floor and the ceiling flux.
float fluxNorm(float flux) {
    float lo = log(1.0f + max(scene.flux.y, 1e-3f));
    float hi = log(1.0f + max(scene.flux.z, scene.flux.y + 1e-3f));
    return saturate((log(1.0f + max(flux, 0.0f)) - lo) / max(hi - lo, 1e-6f));
}

float3 heatColor(float flux) {
    float t = fluxNorm(flux);
    if (scene.options.z > 1.5f) return float3(t, t, t);       // grayscale mode
    if (scene.options.z > 0.5f) return float3(flux * 0.002f, flux * 0.0004f, 0.0f);
    return heatRamp(t);
}

// ----------------------------------------------------------------- lighting --
float3 sunLightDir() { return normalize(scene.sunDir.xyz); }

// Simple analytic environment: sky above, warm ground bounce below.
float3 envColor(float3 dir) {
    float t = saturate(dir.y * 0.5f + 0.5f);
    return lerp(float3(0.14f, 0.13f, 0.12f), float3(0.32f, 0.45f, 0.72f), t);
}

float3 shadeSurface(float3 albedo, float3 n, float3 viewDir, float gloss, float3 emissive) {
    float3 l = sunLightDir();
    float ndl = saturate(dot(n, l));
    float3 h = normalize(l + viewDir);
    float spec = pow(saturate(dot(n, h)), gloss) * (0.04f + 0.96f * pow(1.0f - saturate(dot(h, l)), 5.0f));
    float3 ambient = envColor(n) * 0.35f;
    float3 col = albedo * (ndl * 1.35f + ambient) + spec * float3(1.0f, 0.95f, 0.85f) * 3.0f;
    return col + emissive;
}

// ------------------------------------------------------------ scene fragment --
[shader("fragment")]
float4 fsScene(VSOut i) : SV_Target {
    const float3 viewDir = normalize(scene.camPos.xyz - i.world);
    const float3 n = normalize(i.nrm);
    const float debug = scene.options.x;

    if (pc.mode == 0u) {
        // ---- ground: procedural grid + distance fade ----
        float2 g = i.world.xz / max(scene.ground.x, 1.0f);
        float2 fw = fwidth(g);
        float2 grid = abs(frac(g - 0.5f) - 0.5f) / max(fw, 1e-5f);
        float line = 1.0f - saturate(min(grid.x, grid.y));
        float lineFar = 1.0f - saturate(min(grid.x, grid.y) * 0.25f);
        float3 base = float3(0.135f, 0.135f, 0.125f);
        float3 col = lerp(base, base * 1.9f, saturate(line * 0.6f + lineFar * 0.25f));
        float dist = length(i.world - scene.camPos.xyz);
        col *= 1.0f - saturate((dist - 250.0f) / 900.0f) * 0.85f;
        float3 lit = shadeSurface(col, n, viewDir, 8.0f, float3(0.0f, 0.0f, 0.0f));
        if (debug > 0.5f) lit = col;
        return float4(lit, 1.0f);
    }
    if (pc.mode == 1u || pc.mode == 6u) {
        // ---- tower / foundation: dark structural steel ----
        float3 albedo = float3(0.16f, 0.165f, 0.18f);
        float3 col = shadeSurface(albedo, n, viewDir, 24.0f, float3(0.0f, 0.0f, 0.0f));
        if (debug > 0.5f) col = albedo;
        return float4(col, 1.0f);
    }
    if (pc.mode == 2u) {
        // ---- receiver: flux heat map as an emissive surface ----
        float flux = fluxTex.SampleLevel(i.uv, 0.0f).r;
        float t = fluxNorm(flux);
        // Emissive response: the low end keeps the spot's structure readable after
        // ACES, the quadratic term feeds the bloom threshold.
        float3 emis = heatColor(flux) * (t * t * 3.0f + t * 0.6f) * scene.flux.x;
        // Structural body: dull white ceramic in the cold areas.
        float3 albedo = float3(0.42f, 0.43f, 0.45f);
        float3 col = shadeSurface(albedo, n, viewDir, 32.0f, emis) * 0.35f + emis;
        if (debug > 1.5f && debug < 2.5f) col = float3(t, t * 0.4f, 0.0f);
        return float4(col, 1.0f);
    }
    if (pc.mode == 3u) {
        // ---- plate: glass with a mirror-like response ----
        const float3 l = sunLightDir();
        float ndl = saturate(dot(n, l));
        // Front/back test: only the reflecting face carries the coating.
        bool front = dot(n, l) > 0.0f;
        float3 h = normalize(l + viewDir);
        float spec = pow(saturate(dot(n, h)), 900.0f) * 8.0f;
        float fres = 0.04f + 0.96f * pow(1.0f - saturate(dot(n, viewDir)), 5.0f);
        float3 env = envColor(reflect(-viewDir, n));
        float3 col = env * fres * 0.8f + float3(0.05f, 0.07f, 0.09f) * (0.4f + ndl);
        if (front) col += spec * float3(1.0f, 0.97f, 0.9f);
        // Thin-film-ish glass tint + a subtle facet grid so the deformation reads.
        col += float3(0.02f, 0.03f, 0.04f);
        float2 cell = frac(i.uv);
        float edge = 1.0f - saturate(min(min(cell.x, cell.y), min(1.0f - cell.x, 1.0f - cell.y)) * 6.0f);
        col += edge * 0.03f;

        if (debug > 0.5f && debug < 1.5f) {
            col = n * 0.5f + 0.5f;                                    // world normal
        } else if (debug > 1.5f && debug < 2.5f) {
            // Slope error against *this* plate's macro normal (i.color.xyz), so the
            // inactive mirrors correctly read as zero deviation.
            float dev = acos(clamp(dot(n, normalize(i.color.xyz)), -1.0f, 1.0f));
            col = heatRamp(saturate(dev / 0.03f));                     // slope error, 30 mrad full scale
        } else if (debug > 2.5f) {
            float yy = i.color.w > 0.5f
                           ? yGrid[min(uint(i.uv.y) * uint(scene.plate.z) + uint(i.uv.x), 1023u)]
                           : 0.0f;
            col = heatRamp(saturate((yy * 1000.0f + 60.0f) / 120.0f));  // height, +-60 mm
        }
        // Dim the inactive mirrors a little so the selected one reads as "the one
        // being traced" without hiding the field layout.
        if (i.color.w < 0.5f) col *= 0.55f;
        return float4(col, 1.0f);
    }
    if (pc.mode == 4u) {
        // ---- bolt marker ----
        float3 col = i.color.rgb * (0.35f + 0.65f * saturate(dot(n, sunLightDir())));
        return float4(col, 1.0f);
    }
    if (pc.mode == 7u) {
        // ---- cardinal axis marker: flat, unlit, slight pulse so it reads on video ----
        float3 col = i.color.rgb * (0.55f + 0.25f * sin(scene.options.w * 1.5f));
        return float4(col, 1.0f);
    }

    // ---- beam: additive, soft-edged, with a travelling energy pulse ----
    float edge = 1.0f - abs(i.uv.x * 2.0f - 1.0f);
    float soft = pow(saturate(edge), 2.2f);
    float along = i.uv.y;
    float fade = pow(sin(saturate(along) * kPi), 0.7f);
    float pulse = 0.55f + 0.45f * sin((along * 6.0f - scene.options.w * 0.9f) * kPi);
    float a = soft * fade * pulse * 0.16f * scene.options.y;
    return float4(i.color.rgb * a, a);
}

// ------------------------------------------------------- flux map inset ------
// A screen-space panel in the bottom-right that shows the *whole* 157x50 flux
// texture as a 2D map (angle on x, receiver height on y), drawn after the
// composite so bloom and ACES never touch it. It shares the scene descriptor set,
// so it needs no extra bindings: the flux texture and the log-range are already
// there.
struct MapOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;   // 0..1 across the map
};

[shader("vertex")]
MapOut vsFluxMap(uint id : SV_VertexID) {
    // uiParams.xywh = the inset rect in pixels, orthographic pixel space.
    const float2 rect = scene.uiParams.xy;
    const float2 size = scene.uiParams.zw;
    float2 p = float2((id == 2u) ? 2.0f : 0.0f, (id == 1u) ? 2.0f : 0.0f);
    float2 px = rect + p * size;
    MapOut o;
    o.pos = float4(px.x * scene.viewport.z * 2.0f - 1.0f, px.y * scene.viewport.w * 2.0f - 1.0f, 0.0f, 1.0f);
    // v is flipped so the top of the inset is the top of the cylinder.
    o.uv = float2(p.x, 1.0f - p.y);
    return o;
}

[shader("fragment")]
float4 fsFluxMap(MapOut i) : SV_Target {
    const float2 uvIn = saturate(i.uv);
    // The receiver map is a cylinder unwrap: u is periodic (the whole 157 column
    // circumference) and v is the height. Roll u so the flux centroid sits in the
    // middle of the inset; clamp v, which shows the full height anyway.
    const float2 uv = float2(frac(uvIn.x - 0.5f + scene.fluxCenter.x),
                             saturate(uvIn.y - 0.5f + scene.fluxCenter.y));
    float flux = fluxTex.SampleLevel(uv, 0.0f).r;
    float t = fluxNorm(flux);
    float3 col = heatRamp(t) * (0.35f + 0.65f * t) * scene.flux.x;

    // 1 px frame on the inset edge (a UI frame, not a texture edge: the map has no
    // meaningful seam after the roll).
    float2 texel = float2(1.0f / 157.0f, 1.0f / 50.0f);
    float2 e = min(uvIn, 1.0f - uvIn) / texel;
    float border = 1.0f - saturate(min(e.x, e.y));
    col = lerp(col, float3(0.25f, 0.55f, 0.95f), border * 0.85f);

    // S95 contour: mark the pixels that the reported S95 area counts, so the
    // number in the panel can be checked against the picture.
    if (scene.stats.x > 0.0f && flux >= scene.stats.x) {
        float2 texel2 = uv * float2(157.0f, 50.0f);
        float2 cell = frac(texel2);
        float edge = min(min(cell.x, cell.y), min(1.0f - cell.x, 1.0f - cell.y));
        if (edge < 0.18f) col = lerp(col, float3(1.0f, 1.0f, 1.0f), 0.32f);
    }
    return float4(col, 0.96f);
}
