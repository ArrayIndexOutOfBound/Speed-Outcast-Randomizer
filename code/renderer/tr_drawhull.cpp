// tr_drawhull.cpp -- Hull overlay rendering, r_drawHull cvar.
//
// Ported from Q1's gl_hullmesh.c to the Q3/JO SP renderer.

// leave this as first line for PCH reasons...
#include "../server/exe_headers.h"
#include "tr_local.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Cvar
// ---------------------------------------------------------------------------

cvar_t* r_drawHull;

extern const struct hullmesh_s* R_GetWorldHullMesh(void);

// ---------------------------------------------------------------------------
// Colour computation 
// ---------------------------------------------------------------------------

static void Hull_Color3f(const float* normal, const float* pos)
{
    // Use the camera's forward axis as a directional "headlamp".
    // By eliminating the per-vertex position from the light math, coplanar 
    // polygons evaluate to the EXACT same color, making Z-fighting invisible!
    const float* cam_fwd = backEnd.viewParms. or .axis[0];

    // Dot product against camera forward vector
    float d = -(cam_fwd[0] * normal[0] + cam_fwd[1] * normal[1] + cam_fwd[2] * normal[2]);
    if (d < 0.0f) d = 0.0f;
    float shade = 0.25f + 0.75f * d;

    float r = (normal[0] * 0.25f + 0.75f) * shade;
    float g = (normal[1] * 0.25f + 0.75f) * shade;
    float b = (normal[2] * 0.25f + 0.75f) * shade;

    if (r < 0.0f)r = 0.0f; if (r > 1.0f)r = 1.0f;
    if (g < 0.0f)g = 0.0f; if (g > 1.0f)g = 1.0f;
    if (b < 0.0f)b = 0.0f; if (b > 1.0f)b = 1.0f;

    qglColor3f(r, g, b);
}

// ---------------------------------------------------------------------------
// DrawHullMesh -- world hull
// ---------------------------------------------------------------------------

static void DrawHullMesh(const struct hullmesh_s* m)
{
    int i, vi;
    int face_end = m->face_start + m->face_count;
    int line_end = m->line_start + m->line_count;

    // Pass 1 — opaque coloured fill.
    GL_State(GLS_DEFAULT);
    qglEnable(GL_POLYGON_OFFSET_FILL);
    // Hardcode offset to push solid polygons BACKWARD into the depth buffer
    qglPolygonOffset(1.0f, 2.0f);

    qglBegin(GL_TRIANGLES);
    for (i = m->face_start; i < face_end; i++) {
        vi = m->indices[i];
        Hull_Color3f(m->vertices[vi].normal, m->vertices[vi].position);
        qglVertex3fv(m->vertices[vi].position);
    }
    qglEnd();
    qglDisable(GL_POLYGON_OFFSET_FILL);

    // Pass 2 — white wireframe. 
    // DepthRange hack removed so hidden backfaces don't draw through walls!
    GL_State(GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE);
    qglColor3f(0, 0, 0);

    qglBegin(GL_LINES);
    for (i = m->line_start; i < line_end; i++) {
        vi = m->indices[i];
        qglVertex3fv(m->vertices[vi].position);
    }
    qglEnd();
}

// ---------------------------------------------------------------------------
// Unit-cube AABB for per-entity bounding boxes
// ---------------------------------------------------------------------------

static const float s_cube[24][6] = {
    {0,0,1, 0,0,1},{1,0,1, 0,0,1},{1,1,1, 0,0,1},{0,1,1, 0,0,1},
    {1,0,0, 0,0,-1},{0,0,0, 0,0,-1},{0,1,0, 0,0,-1},{1,1,0, 0,0,-1},
    {0,0,0,-1,0,0},{0,0,1,-1,0,0},{0,1,1,-1,0,0},{0,1,0,-1,0,0},
    {1,0,1, 1,0,0},{1,0,0, 1,0,0},{1,1,0, 1,0,0},{1,1,1, 1,0,0},
    {0,1,1, 0,1,0},{1,1,1, 0,1,0},{1,1,0, 0,1,0},{0,1,0, 0,1,0},
    {0,0,0, 0,-1,0},{1,0,0, 0,-1,0},{1,0,1, 0,-1,0},{0,0,1, 0,-1,0},
};
static const int s_quad_idx[24] = {
    3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12, 19,18,17,16, 23,22,21,20
};
static const int s_line_idx[48] = {
    0,1,1,2,2,3,3,0, 4,5,5,6,6,7,7,4,
    8,9,9,10,10,11,11,8, 12,13,13,14,14,15,15,12,
    16,17,17,18,18,19,19,16, 20,21,21,22,22,23,23,20,
};

static void DrawBox(const vec3_t mins, const vec3_t maxs)
{
    int i, vi;
    float scale[3];
    scale[0] = maxs[0] - mins[0]; scale[1] = maxs[1] - mins[1]; scale[2] = maxs[2] - mins[2];

    GL_State(GLS_DEFAULT);
    qglEnable(GL_POLYGON_OFFSET_FILL);
    qglPolygonOffset(1.0f, 2.0f); // Push filled faces back

    qglBegin(GL_QUADS);
    for (i = 0; i < 24; i++) {
        vi = s_quad_idx[i];
        const float* v = s_cube[vi];
        float p[3] = { mins[0] + v[0] * scale[0], mins[1] + v[1] * scale[1], mins[2] + v[2] * scale[2] };
        float n[3] = { v[3], v[4], v[5] };
        Hull_Color3f(n, p);
        qglVertex3fv(p);
    }
    qglEnd();
    qglDisable(GL_POLYGON_OFFSET_FILL);

    GL_State(GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE);
    qglColor3f(0, 0, 0); // Normal depth test for lines

    qglBegin(GL_LINES);
    for (i = 0; i < 48; i++) {
        vi = s_line_idx[i];
        const float* v = s_cube[vi];
        float p[3] = { mins[0] + v[0] * scale[0], mins[1] + v[1] * scale[1], mins[2] + v[2] * scale[2] };
        qglVertex3fv(p);
    }
    qglEnd();
}

// ---------------------------------------------------------------------------
// DrawCross -- view-entity marker
// ---------------------------------------------------------------------------

static void DrawCross(const vec3_t origin)
{
    const float L = 8.0f;
    const float D = L / 1.732050808f;

    GL_State(GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE);
    // Kept DepthRange(0,0) specifically for the player cross so it's always visible
    qglDepthRange(0, 0);
    qglColor3f(0, 0, 0);
    qglBegin(GL_LINES);
    qglVertex3f(origin[0] - L, origin[1], origin[2]); qglVertex3f(origin[0] + L, origin[1], origin[2]);
    qglVertex3f(origin[0], origin[1] - L, origin[2]); qglVertex3f(origin[0], origin[1] + L, origin[2]);
    qglVertex3f(origin[0], origin[1], origin[2] - L); qglVertex3f(origin[0], origin[1], origin[2] + L);
    qglVertex3f(origin[0] - D, origin[1] - D, origin[2] - D); qglVertex3f(origin[0] + D, origin[1] + D, origin[2] + D);
    qglVertex3f(origin[0] - D, origin[1] + D, origin[2] - D); qglVertex3f(origin[0] + D, origin[1] - D, origin[2] + D);
    qglVertex3f(origin[0] + D, origin[1] - D, origin[2] - D); qglVertex3f(origin[0] - D, origin[1] + D, origin[2] + D);
    qglVertex3f(origin[0] + D, origin[1] + D, origin[2] - D); qglVertex3f(origin[0] - D, origin[1] - D, origin[2] + D);
    qglEnd();
    qglDepthRange(0, 1);
}

// ---------------------------------------------------------------------------
// R_DrawHullEntity
// ---------------------------------------------------------------------------

static void R_DrawHullEntity(trRefEntity_t* ent)
{
    if (ent->e.renderfx & RF_FIRST_PERSON) {
        DrawCross(ent->e.origin);
        return;
    }
    if (ent->e.reType != RT_MODEL) return;

    vec3_t lmins, lmaxs;
    R_ModelBounds(ent->e.hModel, lmins, lmaxs);
    if (VectorCompare(lmins, lmaxs)) return;

    vec3_t wmins, wmaxs;
    VectorAdd(lmins, ent->e.origin, wmins);
    VectorAdd(lmaxs, ent->e.origin, wmaxs);
    DrawBox(wmins, wmaxs);
}

// ---------------------------------------------------------------------------
// R_DrawHullMesh 
// ---------------------------------------------------------------------------

void R_DrawHullMesh(void)
{
    if (!r_drawHull || !r_drawHull->integer) return;

    qglLoadMatrixf(backEnd.viewParms.world.modelMatrix);

    GL_Cull(CT_TWO_SIDED); // Required to see inside the expanded hulls

    // 1. World hull
    const struct hullmesh_s* wm = R_GetWorldHullMesh();
    if (wm) DrawHullMesh(wm);

    // 2. Per-entity bounding boxes
    for (int i = 0; i < tr.refdef.num_entities; i++)
        R_DrawHullEntity(&tr.refdef.entities[i]);

    // Restore cull state and polygon mode
    GL_Cull(CT_FRONT_SIDED);
    GL_State(GLS_DEFAULT);
}

// ---------------------------------------------------------------------------
// R_InitHullDraw
// ---------------------------------------------------------------------------

void R_InitHullDraw(void)
{
    r_drawHull = ri.Cvar_Get("r_drawHull", "0", CVAR_CHEAT);
}