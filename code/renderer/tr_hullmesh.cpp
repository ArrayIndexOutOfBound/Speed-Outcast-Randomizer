// tr_hullmesh.cpp -- Hull mesh portalization and per-frame visible geometry.
//
// Ported from Q1's hullmesh.c / gl_hullmesh.c to the Q3/JO SP renderer.

// leave this as first line for PCH reasons...
#include "../server/exe_headers.h"
#include "tr_local.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define HM_MAX_POINTS   64
#define HM_BOGUS_RANGE  65536.0f
#define HM_SIDESPACE    24.0f
#define HM_ON_EPSILON   0.1f

#define HM_SIDE_FRONT   0
#define HM_SIDE_BACK    1
#define HM_SIDE_ON      2

#define HM_SOLID        (-1)
#define HM_EMPTY        (-2)

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    int    numpoints;
    vec3_t points[HM_MAX_POINTS];
} hm_winding_t;

typedef struct hm_portal_s {
    cplane_t* plane;
    struct hm_node_s* nodes[2];
    struct hm_portal_s* next[2];
    hm_winding_t* winding;
} hm_portal_t;

typedef struct hm_node_s {
    cplane_t* plane;
    struct hm_node_s* children[2];
    struct hm_node_s* parent;
    int                 contents;
    struct hm_portal_s* portals;
    mnode_t* src;
} hm_node_t;

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

static struct hullmesh_s s_world_hm;
static cplane_t s_bound_planes[6];

static hm_winding_t* HM_NewWinding(void) {
    hm_winding_t* w = (hm_winding_t*)malloc(sizeof * w);
    memset(w, 0, sizeof * w);
    return w;
}
static void HM_FreeWinding(hm_winding_t* w) { free(w); }

static hm_portal_t* HM_AllocPortal(void) {
    hm_portal_t* p = (hm_portal_t*)malloc(sizeof * p);
    memset(p, 0, sizeof * p);
    return p;
}
static void HM_FreePortal(hm_portal_t* p) { free(p); }

static void HM_AddPortalToNodes(hm_portal_t* p, hm_node_t* front, hm_node_t* back) {
    if (p->nodes[0] || p->nodes[1])
        ri.Error(ERR_DROP, "HM_AddPortalToNodes: already included");
    p->nodes[0] = front; p->next[0] = front->portals; front->portals = p;
    p->nodes[1] = back;  p->next[1] = back->portals;  back->portals = p;
}

static void HM_RemovePortalFromNode(hm_portal_t* portal, hm_node_t* l) {
    hm_portal_t** pp = &l->portals;
    for (;; ) {
        hm_portal_t* t = *pp;
        if (!t) ri.Error(ERR_DROP, "HM_RemovePortalFromNode: portal not in leaf");
        if (t == portal) break;
        pp = (t->nodes[0] == l) ? &t->next[0] : &t->next[1];
    }
    if (portal->nodes[0] == l) { *pp = portal->next[0]; portal->nodes[0] = NULL; }
    else if (portal->nodes[1] == l) { *pp = portal->next[1]; portal->nodes[1] = NULL; }
}

static hm_winding_t* HM_BaseWindingForPlane(cplane_t* p) {
    int    x = -1;
    float  max = -HM_BOGUS_RANGE;
    for (int i = 0; i < 3; i++) {
        float v = (float)fabs(p->normal[i]);
        if (v > max) { x = i; max = v; }
    }
    if (x == -1) ri.Error(ERR_DROP, "HM_BaseWindingForPlane: no axis");

    vec3_t vup = { 0,0,0 };
    vup[(x < 2) ? 2 : 0] = 1.0f;
    float v = DotProduct(vup, p->normal);
    VectorMA(vup, -v, p->normal, vup);
    VectorNormalize(vup);
    vec3_t org, vright;
    VectorScale(p->normal, p->dist, org);
    CrossProduct(vup, p->normal, vright);
    VectorScale(vup, 8192.0f, vup);
    VectorScale(vright, 8192.0f, vright);

    hm_winding_t* w = HM_NewWinding();
    VectorSubtract(org, vright, w->points[0]); VectorAdd(w->points[0], vup, w->points[0]);
    VectorAdd(org, vright, w->points[1]);      VectorAdd(w->points[1], vup, w->points[1]);
    VectorAdd(org, vright, w->points[2]);      VectorSubtract(w->points[2], vup, w->points[2]);
    VectorSubtract(org, vright, w->points[3]); VectorSubtract(w->points[3], vup, w->points[3]);
    w->numpoints = 4;
    return w;
}

static hm_winding_t* HM_ClipWinding(hm_winding_t* in, cplane_t* split, qboolean keepon) {
    float dists[HM_MAX_POINTS]; int sides[HM_MAX_POINTS], counts[3] = { 0,0,0 };
    for (int i = 0; i < in->numpoints; i++) {
        float d = DotProduct(in->points[i], split->normal) - split->dist;
        dists[i] = d;
        sides[i] = (d > HM_ON_EPSILON) ? HM_SIDE_FRONT : (d < -HM_ON_EPSILON) ? HM_SIDE_BACK : HM_SIDE_ON;
        counts[sides[i]]++;
    }
    sides[in->numpoints] = sides[0]; dists[in->numpoints] = dists[0];
    if (keepon && !counts[0] && !counts[1]) return in;
    if (!counts[0]) { HM_FreeWinding(in); return NULL; }
    if (!counts[1]) return in;

    hm_winding_t* nw = HM_NewWinding();
    for (int i = 0; i < in->numpoints; i++) {
        float* p1 = in->points[i];
        if (sides[i] == HM_SIDE_ON || sides[i] == HM_SIDE_FRONT)
            VectorCopy(p1, nw->points[nw->numpoints++]);
        if (sides[i + 1] == HM_SIDE_ON || sides[i + 1] == sides[i]) continue;
        float* p2 = in->points[(i + 1) % in->numpoints], dot = dists[i] / (dists[i] - dists[i + 1]);
        vec3_t mid;
        for (int j = 0;j < 3;j++) {
            if (split->normal[j] == 1) mid[j] = split->dist;
            else if (split->normal[j] == -1) mid[j] = -split->dist;
            else                           mid[j] = p1[j] + dot * (p2[j] - p1[j]);
        }
        VectorCopy(mid, nw->points[nw->numpoints++]);
    }
    HM_FreeWinding(in); return nw;
}

static void HM_DivideWinding(hm_winding_t* in, cplane_t* split, hm_winding_t** front, hm_winding_t** back) {
    float dists[HM_MAX_POINTS]; int sides[HM_MAX_POINTS], counts[3] = { 0,0,0 };
    for (int i = 0;i < in->numpoints;i++) {
        float d = DotProduct(in->points[i], split->normal) - split->dist;
        dists[i] = d; sides[i] = (d > HM_ON_EPSILON) ? HM_SIDE_FRONT : (d < -HM_ON_EPSILON) ? HM_SIDE_BACK : HM_SIDE_ON;
        counts[sides[i]]++;
    }
    sides[in->numpoints] = sides[0]; dists[in->numpoints] = dists[0];
    *front = *back = NULL;
    if (!counts[0]) { *back = in;  return; }
    if (!counts[1]) { *front = in; return; }

    hm_winding_t* f = HM_NewWinding(), * b = HM_NewWinding();
    *front = f; *back = b;
    for (int i = 0;i < in->numpoints;i++) {
        float* p1 = in->points[i];
        if (sides[i] == HM_SIDE_ON) {
            VectorCopy(p1, f->points[f->numpoints++]);
            VectorCopy(p1, b->points[b->numpoints++]); continue;
        }
        if (sides[i] == HM_SIDE_FRONT) VectorCopy(p1, f->points[f->numpoints++]);
        if (sides[i] == HM_SIDE_BACK)  VectorCopy(p1, b->points[b->numpoints++]);
        if (sides[i + 1] == HM_SIDE_ON || sides[i + 1] == sides[i]) continue;
        float* p2 = in->points[(i + 1) % in->numpoints], dot = dists[i] / (dists[i] - dists[i + 1]);
        vec3_t mid;
        for (int j = 0;j < 3;j++) {
            if (split->normal[j] == 1) mid[j] = split->dist;
            else if (split->normal[j] == -1) mid[j] = -split->dist;
            else                           mid[j] = p1[j] + dot * (p2[j] - p1[j]);
        }
        VectorCopy(mid, f->points[f->numpoints++]);
        VectorCopy(mid, b->points[b->numpoints++]);
    }
}

static hm_node_t* HM_MakeHeadnodePortals(hm_node_t* root, const vec3_t mins, const vec3_t maxs) {
    hm_node_t* outside = (hm_node_t*)malloc(sizeof * outside);
    memset(outside, 0, sizeof * outside);
    outside->contents = HM_SOLID;

    hm_portal_t* portals[6];
    for (int i = 0;i < 3;i++) for (int j = 0;j < 2;j++) {
        int n = j * 3 + i;
        cplane_t* pl = &s_bound_planes[n]; memset(pl, 0, sizeof * pl);
        if (j) { pl->normal[i] = -1; pl->dist = -(maxs[i] + HM_SIDESPACE); }
        else { pl->normal[i] = 1; pl->dist = mins[i] - HM_SIDESPACE; }
        hm_portal_t* p = HM_AllocPortal(); portals[n] = p;
        p->plane = pl; p->winding = HM_BaseWindingForPlane(pl);
        HM_AddPortalToNodes(p, root, outside);
    }
    for (int i = 0;i < 6;i++) for (int j = 0;j < 6;j++) {
        if (j == i) continue;
        portals[i]->winding = HM_ClipWinding(portals[i]->winding, &s_bound_planes[j], qtrue);
    }
    return outside;
}

static void HM_CutNodePortals_r(hm_node_t* node) {
    if (node->contents) return;

    cplane_t* plane = node->plane;
    hm_node_t* f = node->children[0], * b = node->children[1];

    hm_portal_t* np = HM_AllocPortal(); np->plane = plane;
    hm_winding_t* w = HM_BaseWindingForPlane(plane);
    for (hm_node_t* a = node;a->parent;a = a->parent) {
        cplane_t cp = *a->parent->plane;
        if (a->parent->children[1] == a) { cp.dist = -cp.dist; VectorNegate(cp.normal, cp.normal); }
        w = HM_ClipWinding(w, &cp, qtrue);
        if (!w) { break; }
    }
    for (int i = 0;w && i < 6;i++) { w = HM_ClipWinding(w, &s_bound_planes[i], qtrue); }
    if (w) { np->winding = w; HM_AddPortalToNodes(np, f, b); }

    hm_portal_t* p, * next;
    for (p = node->portals;p;p = next) {
        int side = (p->nodes[0] == node) ? 0 : 1;
        next = p->next[side];
        hm_node_t* other = p->nodes[!side];
        HM_RemovePortalFromNode(p, p->nodes[0]);
        HM_RemovePortalFromNode(p, p->nodes[1]);
        hm_winding_t* fw, * bw;
        HM_DivideWinding(p->winding, plane, &fw, &bw);
        if (!fw) {
            if (side == 0) HM_AddPortalToNodes(p, b, other); else HM_AddPortalToNodes(p, other, b);
        }
        else if (!bw) {
            if (side == 0) HM_AddPortalToNodes(p, f, other); else HM_AddPortalToNodes(p, other, f);
        }
        else {
            hm_portal_t* np2 = HM_AllocPortal(); *np2 = *p;
            np2->winding = bw; HM_FreeWinding(p->winding); p->winding = fw;
            if (side == 0) { HM_AddPortalToNodes(p, f, other); HM_AddPortalToNodes(np2, b, other); }
            else { HM_AddPortalToNodes(p, other, f); HM_AddPortalToNodes(np2, other, b); }
        }
    }
    HM_CutNodePortals_r(f);
    HM_CutNodePortals_r(b);
}

static void HM_FreeAllPortals(hm_node_t* node) {
    if (!node->contents) { HM_FreeAllPortals(node->children[0]); HM_FreeAllPortals(node->children[1]); }
    hm_portal_t* p, * next;
    for (p = node->portals;p;p = next) {
        next = (p->nodes[0] == node) ? p->next[0] : p->next[1];
        HM_RemovePortalFromNode(p, p->nodes[0]);
        HM_RemovePortalFromNode(p, p->nodes[1]);
        if (p->winding) HM_FreeWinding(p->winding);
        HM_FreePortal(p);
    }
}

static void HM_FreeNodeTree(hm_node_t* node) {
    if (!node) return;
    HM_FreeNodeTree(node->children[0]);
    HM_FreeNodeTree(node->children[1]);
    free(node);
}

static hm_node_t* HM_ConvertNodes(mnode_t* in) {
    hm_node_t* out = (hm_node_t*)malloc(sizeof * out);
    memset(out, 0, sizeof * out);
    out->src = in;

    if (in->contents == CONTENTS_NODE) {
        out->plane = in->plane; out->contents = 0;
        out->children[0] = HM_ConvertNodes(in->children[0]);
        out->children[1] = HM_ConvertNodes(in->children[1]);
        out->children[0]->parent = out;
        out->children[1]->parent = out;
    }
    else {
        out->contents = (in->nummarksurfaces == 0) ? HM_SOLID : HM_EMPTY;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Array construction helpers
// ---------------------------------------------------------------------------

typedef struct { int faces, verts; } hm_count_t;
static void HM_CountFace(hm_portal_t* p, hm_winding_t* w, qboolean fl, void* ctx) {
    hm_count_t* c = (hm_count_t*)ctx; c->faces++; c->verts += w->numpoints;
}

typedef struct {
    int nv, ni;
    hm_vertex_t* verts;
    int* idx;
} hm_write_t;

typedef void (*hm_cb_t)(hm_portal_t*, hm_winding_t*, qboolean, void*);

static void HM_VisitWindings(hm_node_t* node, hm_cb_t cb, void* ctx) {
    if (!node->contents) {
        HM_VisitWindings(node->children[0], cb, ctx);
        HM_VisitWindings(node->children[1], cb, ctx);
        return;
    }
    if (node->contents != HM_SOLID) return;
    for (hm_portal_t* p = node->portals;p;) {
        hm_winding_t* w = p->winding;
        if (w) {
            qboolean n0s = (p->nodes[0]->contents == HM_SOLID);
            qboolean n1s = (p->nodes[1]->contents == HM_SOLID);
            if ((p->nodes[0] == node && !n1s) || (p->nodes[1] == node && !n0s))
                cb(p, w, (p->nodes[0] == node), ctx);
        }
        p = (p->nodes[0] == node) ? p->next[0] : p->next[1];
    }
}

// Player Extents setup
static const float PLAYER_HALF_X = 15.0f;
static const float PLAYER_HALF_Y = 15.0f;
static const float PLAYER_HALF_Z_UP = 1.0f;
static const float PLAYER_HALF_Z_DN = 32.0f;

static float HM_PlayerOffset(const vec3_t out_normal) {
    float dx = fabsf(out_normal[0]) * PLAYER_HALF_X;
    float dy = fabsf(out_normal[1]) * PLAYER_HALF_Y;
    float dz = (out_normal[2] >= 0.0f)
        ? out_normal[2] * PLAYER_HALF_Z_UP
        : -out_normal[2] * PLAYER_HALF_Z_DN;
    return dx + dy + dz;
}

static void HM_WriteFace(hm_portal_t* p, hm_winding_t* w, qboolean flip, void* ctx)
{
    hm_write_t* wc = (hm_write_t*)ctx;

    vec3_t out_normal;
    if (flip) VectorNegate(p->plane->normal, out_normal);
    else      VectorCopy(p->plane->normal, out_normal);

    float offset = HM_PlayerOffset(out_normal);
    int base_v = wc->nv;

    for (int i = 0;i < w->numpoints;i++) {
        vec3_t* pos = &wc->verts[base_v + i].position;
        (*pos)[0] = w->points[i][0] + out_normal[0] * offset;
        (*pos)[1] = w->points[i][1] + out_normal[1] * offset;
        (*pos)[2] = w->points[i][2] + out_normal[2] * offset;
        VectorCopy(out_normal, wc->verts[base_v + i].normal);
    }

    for (int i = 0;i < w->numpoints;i++) {
        vec3_t* pos = &wc->verts[base_v + w->numpoints + i].position;
        VectorCopy(w->points[i], *pos);
        VectorCopy(out_normal, wc->verts[base_v + w->numpoints + i].normal);
    }

    int o1 = flip ? 2 : 1, o2 = flip ? 1 : 2;
    for (int i = 0;i < w->numpoints - 2;i++) {
        wc->idx[wc->ni++] = base_v;
        wc->idx[wc->ni++] = base_v + i + o1;
        wc->idx[wc->ni++] = base_v + i + o2;
    }

    // Skirt Triangles
    for (int i = 0;i < w->numpoints;i++) {
        int next_i = (i + 1) % w->numpoints;
        int s_curr = base_v + i;
        int s_next = base_v + next_i;
        int o_curr = base_v + w->numpoints + i;
        int o_next = base_v + w->numpoints + next_i;

        wc->idx[wc->ni++] = s_curr; wc->idx[wc->ni++] = s_next; wc->idx[wc->ni++] = o_curr;
        wc->idx[wc->ni++] = s_next; wc->idx[wc->ni++] = o_next; wc->idx[wc->ni++] = o_curr;
    }
    wc->nv += 2 * w->numpoints;
}

static void HM_WriteFaceLines(hm_portal_t* p, hm_winding_t* w, qboolean flip, void* ctx)
{
    hm_write_t* wc = (hm_write_t*)ctx;

    vec3_t out_normal;
    if (flip) VectorNegate(p->plane->normal, out_normal);
    else      VectorCopy(p->plane->normal, out_normal);
    float offset = HM_PlayerOffset(out_normal);

    for (int i = 0;i < w->numpoints;i++) {
        vec3_t* pos = &wc->verts[wc->nv + i].position;
        (*pos)[0] = w->points[i][0] + out_normal[0] * offset;
        (*pos)[1] = w->points[i][1] + out_normal[1] * offset;
        (*pos)[2] = w->points[i][2] + out_normal[2] * offset;
        VectorCopy(out_normal, wc->verts[wc->nv + i].normal);
    }

    for (int i = 0;i < w->numpoints - 1;i++) { wc->idx[wc->ni++] = wc->nv + i; wc->idx[wc->ni++] = wc->nv + i + 1; }
    wc->idx[wc->ni++] = wc->nv + w->numpoints - 1; wc->idx[wc->ni++] = wc->nv;
    wc->nv += w->numpoints;
}

// ---------------------------------------------------------------------------
// DETAIL BRUSH, GRID & CURVE Writer (SF_FACE, SF_GRID, SF_TRIANGLES)
// ---------------------------------------------------------------------------

static void HM_WriteFaceSurface(srfSurfaceFace_t* face, hm_write_t* wc) {
    int base_v = wc->nv;
    vec3_t norm;
    VectorCopy(face->plane.normal, norm);
    float offset = HM_PlayerOffset(norm);

    for (int i = 0; i < face->numPoints; i++) {
        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = face->points[i][0] + norm[0] * offset;
        (*s_pos)[1] = face->points[i][1] + norm[1] * offset;
        (*s_pos)[2] = face->points[i][2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);
    }

    for (int i = 0; i < face->numPoints; i++) {
        vec3_t* o_pos = &wc->verts[base_v + face->numPoints + i].position;
        (*o_pos)[0] = face->points[i][0];
        (*o_pos)[1] = face->points[i][1];
        (*o_pos)[2] = face->points[i][2];
        VectorCopy(norm, wc->verts[base_v + face->numPoints + i].normal);
    }

    int* indices = (int*)((byte*)face + face->ofsIndices);
    for (int i = 0; i < face->numIndices; i += 3) {
        wc->idx[wc->ni++] = base_v + indices[i];
        wc->idx[wc->ni++] = base_v + indices[i + 1];
        wc->idx[wc->ni++] = base_v + indices[i + 2];
    }

    for (int i = 0; i < face->numPoints; i++) {
        int next_i = (i + 1) % face->numPoints;
        int s_curr = base_v + i;
        int s_next = base_v + next_i;
        int o_curr = base_v + face->numPoints + i;
        int o_next = base_v + face->numPoints + next_i;

        wc->idx[wc->ni++] = s_curr; wc->idx[wc->ni++] = s_next; wc->idx[wc->ni++] = o_curr;
        wc->idx[wc->ni++] = s_next; wc->idx[wc->ni++] = o_next; wc->idx[wc->ni++] = o_curr;
    }
    wc->nv += 2 * face->numPoints;
}

static void HM_WriteFaceSurfaceLines(srfSurfaceFace_t* face, hm_write_t* wc) {
    int base_v = wc->nv;
    vec3_t norm;
    VectorCopy(face->plane.normal, norm);
    float offset = HM_PlayerOffset(norm);

    for (int i = 0; i < face->numPoints; i++) {
        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = face->points[i][0] + norm[0] * offset;
        (*s_pos)[1] = face->points[i][1] + norm[1] * offset;
        (*s_pos)[2] = face->points[i][2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);
    }

    for (int i = 0; i < face->numPoints; i++) {
        wc->idx[wc->ni++] = base_v + i;
        wc->idx[wc->ni++] = base_v + ((i + 1) % face->numPoints);
    }
    wc->nv += face->numPoints;
}

static void HM_WriteGrid(srfGridMesh_t* grid, hm_write_t* wc) {
    int w = grid->width, h = grid->height, base_v = wc->nv;

    for (int i = 0; i < w * h; i++) {
        vec3_t pos, norm;
        VectorCopy(grid->verts[i].xyz, pos);
        VectorCopy(grid->verts[i].normal, norm);
        float offset = HM_PlayerOffset(norm);

        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = pos[0] + norm[0] * offset;
        (*s_pos)[1] = pos[1] + norm[1] * offset;
        (*s_pos)[2] = pos[2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);

        vec3_t* o_pos = &wc->verts[base_v + w * h + i].position;
        VectorCopy(pos, *o_pos);
        VectorCopy(norm, wc->verts[base_v + w * h + i].normal);
    }

    for (int row = 0; row < h - 1; row++) {
        for (int col = 0; col < w - 1; col++) {
            int v0 = row * w + col;
            int v1 = v0 + 1;
            int v2 = v0 + w + 1;
            int v3 = v0 + w;

            wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v3; wc->idx[wc->ni++] = base_v + v1;
            wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + v3; wc->idx[wc->ni++] = base_v + v2;
        }
    }

    for (int col = 0; col < w - 1; col++) {  // Top
        int v0 = col, v1 = col + 1;
        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + w * h + v0;
        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + w * h + v1; wc->idx[wc->ni++] = base_v + w * h + v0;
    }
    for (int col = 0; col < w - 1; col++) {  // Bottom
        int v0 = (h - 1) * w + col, v1 = (h - 1) * w + col + 1;
        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + w * h + v1;
        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + w * h + v0; wc->idx[wc->ni++] = base_v + w * h + v1;
    }
    for (int row = 0; row < h - 1; row++) {  // Left
        int v0 = row * w, v1 = (row + 1) * w;
        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + w * h + v1;
        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + w * h + v0; wc->idx[wc->ni++] = base_v + w * h + v1;
    }
    for (int row = 0; row < h - 1; row++) {  // Right
        int v0 = row * w + w - 1, v1 = (row + 1) * w + w - 1;
        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + w * h + v0;
        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + w * h + v1; wc->idx[wc->ni++] = base_v + w * h + v0;
    }
    wc->nv += 2 * w * h;
}

static void HM_WriteGridLines(srfGridMesh_t* grid, hm_write_t* wc) {
    int w = grid->width, h = grid->height, base_v = wc->nv;

    for (int i = 0; i < w * h; i++) {
        vec3_t pos, norm;
        VectorCopy(grid->verts[i].xyz, pos);
        VectorCopy(grid->verts[i].normal, norm);
        float offset = HM_PlayerOffset(norm);

        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = pos[0] + norm[0] * offset;
        (*s_pos)[1] = pos[1] + norm[1] * offset;
        (*s_pos)[2] = pos[2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);
    }

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int v0 = row * w + col;
            if (col < w - 1) {
                wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v0 + 1;
            }
            if (row < h - 1) {
                wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v0 + w;
            }
        }
    }
    wc->nv += w * h;
}

static void HM_WriteTriangles(srfTriangles_t* tri, hm_write_t* wc) {
    int base_v = wc->nv;

    for (int i = 0; i < tri->numVerts; i++) {
        vec3_t pos, norm;
        VectorCopy(tri->verts[i].xyz, pos);
        VectorCopy(tri->verts[i].normal, norm);
        float offset = HM_PlayerOffset(norm);

        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = pos[0] + norm[0] * offset;
        (*s_pos)[1] = pos[1] + norm[1] * offset;
        (*s_pos)[2] = pos[2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);

        vec3_t* o_pos = &wc->verts[base_v + tri->numVerts + i].position;
        VectorCopy(pos, *o_pos);
        VectorCopy(norm, wc->verts[base_v + tri->numVerts + i].normal);
    }

    for (int i = 0; i < tri->numIndexes; i += 3) {
        int v0 = tri->indexes[i], v1 = tri->indexes[i + 1], v2 = tri->indexes[i + 2];

        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + v2;

        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + tri->numVerts + v0;
        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + tri->numVerts + v1; wc->idx[wc->ni++] = base_v + tri->numVerts + v0;

        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + v2; wc->idx[wc->ni++] = base_v + tri->numVerts + v1;
        wc->idx[wc->ni++] = base_v + v2; wc->idx[wc->ni++] = base_v + tri->numVerts + v2; wc->idx[wc->ni++] = base_v + tri->numVerts + v1;

        wc->idx[wc->ni++] = base_v + v2; wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + tri->numVerts + v2;
        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + tri->numVerts + v0; wc->idx[wc->ni++] = base_v + tri->numVerts + v2;
    }
    wc->nv += 2 * tri->numVerts;
}

static void HM_WriteTrianglesLines(srfTriangles_t* tri, hm_write_t* wc) {
    int base_v = wc->nv;

    for (int i = 0; i < tri->numVerts; i++) {
        vec3_t pos, norm;
        VectorCopy(tri->verts[i].xyz, pos);
        VectorCopy(tri->verts[i].normal, norm);
        float offset = HM_PlayerOffset(norm);

        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = pos[0] + norm[0] * offset;
        (*s_pos)[1] = pos[1] + norm[1] * offset;
        (*s_pos)[2] = pos[2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);
    }

    for (int i = 0; i < tri->numIndexes; i += 3) {
        int v0 = tri->indexes[i], v1 = tri->indexes[i + 1], v2 = tri->indexes[i + 2];

        wc->idx[wc->ni++] = base_v + v0; wc->idx[wc->ni++] = base_v + v1;
        wc->idx[wc->ni++] = base_v + v1; wc->idx[wc->ni++] = base_v + v2;
        wc->idx[wc->ni++] = base_v + v2; wc->idx[wc->ni++] = base_v + v0;
    }
    wc->nv += tri->numVerts;
}

// 4. POLYGON SURFACES (SF_POLY)
static void HM_WritePoly(srfPoly_t* poly, hm_write_t* wc) {
    if (poly->numVerts < 3) return; // Safety check
    int base_v = wc->nv;

    // Calculate face normal from the first 3 vertices
    vec3_t v0, v1, norm;
    VectorSubtract(poly->verts[1].xyz, poly->verts[0].xyz, v0);
    VectorSubtract(poly->verts[2].xyz, poly->verts[0].xyz, v1);
    CrossProduct(v0, v1, norm);
    VectorNormalize(norm);

    float offset = HM_PlayerOffset(norm);

    for (int i = 0; i < poly->numVerts; i++) {
        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = poly->verts[i].xyz[0] + norm[0] * offset;
        (*s_pos)[1] = poly->verts[i].xyz[1] + norm[1] * offset;
        (*s_pos)[2] = poly->verts[i].xyz[2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);

        vec3_t* o_pos = &wc->verts[base_v + poly->numVerts + i].position;
        VectorCopy(poly->verts[i].xyz, *o_pos);
        VectorCopy(norm, wc->verts[base_v + poly->numVerts + i].normal);
    }

    for (int i = 0; i < poly->numVerts - 2; i++) {
        wc->idx[wc->ni++] = base_v;
        wc->idx[wc->ni++] = base_v + i + 1;
        wc->idx[wc->ni++] = base_v + i + 2;
    }

    // Skirts
    for (int i = 0; i < poly->numVerts; i++) {
        int next_i = (i + 1) % poly->numVerts;
        int s_curr = base_v + i;
        int s_next = base_v + next_i;
        int o_curr = base_v + poly->numVerts + i;
        int o_next = base_v + poly->numVerts + next_i;

        wc->idx[wc->ni++] = s_curr; wc->idx[wc->ni++] = s_next; wc->idx[wc->ni++] = o_curr;
        wc->idx[wc->ni++] = s_next; wc->idx[wc->ni++] = o_next; wc->idx[wc->ni++] = o_curr;
    }
    wc->nv += 2 * poly->numVerts;
}

static void HM_WritePolyLines(srfPoly_t* poly, hm_write_t* wc) {
    if (poly->numVerts < 3) return;
    int base_v = wc->nv;

    vec3_t v0, v1, norm;
    VectorSubtract(poly->verts[1].xyz, poly->verts[0].xyz, v0);
    VectorSubtract(poly->verts[2].xyz, poly->verts[0].xyz, v1);
    CrossProduct(v0, v1, norm);
    VectorNormalize(norm);

    float offset = HM_PlayerOffset(norm);

    for (int i = 0; i < poly->numVerts; i++) {
        vec3_t* s_pos = &wc->verts[base_v + i].position;
        (*s_pos)[0] = poly->verts[i].xyz[0] + norm[0] * offset;
        (*s_pos)[1] = poly->verts[i].xyz[1] + norm[1] * offset;
        (*s_pos)[2] = poly->verts[i].xyz[2] + norm[2] * offset;
        VectorCopy(norm, wc->verts[base_v + i].normal);
    }

    for (int i = 0; i < poly->numVerts; i++) {
        wc->idx[wc->ni++] = base_v + i;
        wc->idx[wc->ni++] = base_v + ((i + 1) % poly->numVerts);
    }
    wc->nv += poly->numVerts;
}

// ---------------------------------------------------------------------------
// Public: build
// ---------------------------------------------------------------------------
void R_FreeWorldHullMesh(void)
{
    free(s_world_hm.vertices); free(s_world_hm.indices);
    if (s_world_hm.root) HM_FreeNodeTree((hm_node_t*)s_world_hm.root);
    memset(&s_world_hm, 0, sizeof s_world_hm);
}

void R_BuildWorldHullMesh(void)
{
    R_FreeWorldHullMesh();
    if (!tr.world) return;

    vec3_t wmins, wmaxs;
    VectorCopy(tr.world->bmodels[0].bounds[0], wmins);
    VectorCopy(tr.world->bmodels[0].bounds[1], wmaxs);

    hm_node_t* root = HM_ConvertNodes(tr.world->nodes);
    hm_node_t* outside = HM_MakeHeadnodePortals(root, wmins, wmaxs);
    HM_CutNodePortals_r(root);

    hm_count_t c = { 0,0 };
    HM_VisitWindings(root, HM_CountFace, &c);
    if (c.verts == 0) { HM_FreeAllPortals(root); HM_FreeNodeTree(root); free(outside); return; }

    int max_verts = c.verts * 3;
    int max_indices = c.verts * 12;

    for (int i = 0; i < tr.world->numsurfaces; i++) {
        msurface_t* surf = &tr.world->surfaces[i];
        if (!surf->data) continue;
        int type = *(surfaceType_t*)surf->data;
        if (type == SF_GRID) {
            srfGridMesh_t* grid = (srfGridMesh_t*)surf->data;
            max_verts += grid->width * grid->height * 3;
            max_indices += grid->width * grid->height * 12 + (grid->width + grid->height) * 12;
        }
        else if (type == SF_TRIANGLES) {
            srfTriangles_t* tri = (srfTriangles_t*)surf->data;
            max_verts += tri->numVerts * 3;
            max_indices += tri->numIndexes * 7;
        }
        else if (type == SF_FACE) {
            srfSurfaceFace_t* face = (srfSurfaceFace_t*)surf->data;
            max_verts += face->numPoints * 3;
            max_indices += face->numIndices + face->numPoints * 12;
        }
        else if ( type == SF_POLY ) {
            srfPoly_t *poly = (srfPoly_t *)surf->data;
            max_verts += poly->numVerts * 3;
            max_indices += poly->numVerts * 12;
        }

    }

    max_verts += 5000;
    max_indices += 20000;

    s_world_hm.vertices = (hm_vertex_t*)malloc(max_verts * sizeof(hm_vertex_t));
    s_world_hm.indices = (int*)malloc(max_indices * sizeof(int));
    s_world_hm.num_vertices = max_verts;
    s_world_hm.num_indices = max_indices;
    memset(s_world_hm.vertices, 0, max_verts * sizeof(hm_vertex_t));
    memset(s_world_hm.indices, 0, max_indices * sizeof(int));

    hm_write_t wc = {};
    wc.verts = s_world_hm.vertices; wc.idx = s_world_hm.indices;

    // PASS 1: TRIANGLES (BSP + Details + Patches + Terrain)
    s_world_hm.face_start = 0;
    HM_VisitWindings(root, HM_WriteFace, &wc);

    for (int i = 0; i < tr.world->numsurfaces; i++) {
        msurface_t* surf = &tr.world->surfaces[i];
        if (!surf->data) continue;
        int type = *(surfaceType_t*)surf->data;
        if (type == SF_GRID) HM_WriteGrid((srfGridMesh_t*)surf->data, &wc);
        else if (type == SF_TRIANGLES) HM_WriteTriangles((srfTriangles_t*)surf->data, &wc);
        else if (type == SF_FACE) HM_WriteFaceSurface((srfSurfaceFace_t*)surf->data, &wc);
        else if (type == SF_POLY) HM_WritePoly((srfPoly_t*)surf->data, &wc);
    }
    s_world_hm.face_count = wc.ni;  // tr_drawhull natively pulls this!

    // PASS 2: LINES (BSP + Details + Patches + Terrain)
    s_world_hm.line_start = wc.ni;
    HM_VisitWindings(root, HM_WriteFaceLines, &wc);

    for (int i = 0; i < tr.world->numsurfaces; i++) {
        msurface_t* surf = &tr.world->surfaces[i];
        if (!surf->data) continue;
        int type = *(surfaceType_t*)surf->data;
        if (type == SF_GRID) HM_WriteGridLines((srfGridMesh_t*)surf->data, &wc);
        else if (type == SF_TRIANGLES) HM_WriteTrianglesLines((srfTriangles_t*)surf->data, &wc);
        else if (type == SF_FACE) HM_WriteFaceSurfaceLines((srfSurfaceFace_t*)surf->data, &wc);
        else if (type == SF_POLY) HM_WritePolyLines((srfPoly_t*)surf->data, &wc);
    }
    s_world_hm.line_count = wc.ni - s_world_hm.line_start; // tr_drawhull natively pulls this!

    HM_FreeAllPortals(root);
    free(outside);

    s_world_hm.root = root;
    s_world_hm.valid = qtrue;
}

const struct hullmesh_s* R_GetWorldHullMesh(void)
{
    return s_world_hm.valid ? &s_world_hm : NULL;
}