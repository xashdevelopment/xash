/*
 * esp_ext.c - Extended client-side ESP for Xash3D FWGS
 * Educational use; gated by sv_cheats. Draws 2D boxes, optional LOS, labels, health tint.
 */

#include "client.h"
#include "triangleapi.h"
#include "com_model.h"
#include "const.h"
#include "cdll_int.h"
#include "mathlib.h"
#include "pm_defs.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// Define bool for C compatibility
typedef int bool;
#define true 1
#define false 0
#define nullptr NULL

// ESP Cvars
static convar_t clesp          = { "clesp",          "0",  FCVAR_CLIENTDLL };
static convar_t clesprate     = { "clesprate",     "30", FCVAR_CLIENTDLL };
static convar_t clesppad      = { "clesppad",      "6",  FCVAR_CLIENTDLL };
static convar_t clespwidth    = { "clespwidth",    "2",  FCVAR_CLIENTDLL };
static convar_t clespalpha    = { "clespalpha",    "220",FCVAR_CLIENTDLL };
static convar_t clesplos      = { "clesplos",      "0",  FCVAR_CLIENTDLL };
static convar_t clesplabels   = { "clesplabels",   "1",  FCVAR_CLIENTDLL };
static convar_t clespscientists = { "clespscientists", "0", FCVAR_CLIENTDLL };

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int WorldToScreenPx(const vec3_t world, int *sx, int *sy)
{
    float nx, ny, nz;
    int behind = gEngfuncs.pTriAPI->WorldToScreen((float*)&world, &nx, &ny, &nz);
    if (behind) return false; // behind camera
    *sx = (int)((1.0f + nx) * (float)refState.width / 2.0f);
    *sy = (int)((1.0f - ny) * (float)refState.height / 2.0f);
    return (*sx >= 0 && *sx < refState.width && *sy >= 0 && *sy < refState.height);
}

static void DrawRect(int x1, int y1, int x2, int y2, int r, int g, int b, int a, int w, int fill)
{
    x1 = clampi(x1, 0, refState.width - 1);
    x2 = clampi(x2, 0, refState.width - 1);
    y1 = clampi(y1, 0, refState.height - 1);
    y2 = clampi(y2, 0, refState.height - 1);
    if (x2 - x1 < 2 || y2 - y1 < 2) return;

    // Outline via filled strips for consistent thickness
    gEngfuncs.pfnFillRGBABlend(x1, y1, x2 - x1 + 1, w, r, g, b, a);           // top
    gEngfuncs.pfnFillRGBABlend(x1, y2 - w + 1, x2 - x1 + 1, w, r, g, b, a);   // bottom
    gEngfuncs.pfnFillRGBABlend(x1, y1, w, y2 - y1 + 1, r, g, b, a);           // left
    gEngfuncs.pfnFillRGBABlend(x2 - w + 1, y1, w, y2 - y1 + 1, r, g, b, a);   // right

    if (fill)
    {
        const int fa = a / 6;
        gEngfuncs.pfnFillRGBABlend(x1 + w, y1 + w, (x2 - x1 + 1) - 2 * w, (y2 - y1 + 1) - 2 * w, r, g, b, fa);
    }
}

static int ComputeScreenBoxFromBBox(const vec3_t org, const vec3_t mins, const vec3_t maxs, int pad, int *x1, int *y1, int *x2, int *y2)
{
    vec3_t c[8] = {
        { org[0] + mins[0], org[1] + mins[1], org[2] + mins[2] },
        { org[0] + maxs[0], org[1] + mins[1], org[2] + mins[2] },
        { org[0] + maxs[0], org[1] + maxs[1], org[2] + mins[2] },
        { org[0] + mins[0], org[1] + maxs[1], org[2] + mins[2] },
        { org[0] + mins[0], org[1] + mins[1], org[2] + maxs[2] },
        { org[0] + maxs[0], org[1] + mins[1], org[2] + maxs[2] },
        { org[0] + maxs[0], org[1] + maxs[1], org[2] + maxs[2] },
        { org[0] + mins[0], org[1] + maxs[1], org[2] + maxs[2] }
    };

    int any = false;
    int minx =  100000, miny =  100000;
    int maxx = -100000, maxy = -100000;

    for (int i = 0; i < 8; ++i)
    {
        int sx, sy;
        if (!WorldToScreenPx(c[i], &sx, &sy)) continue;
        any = true;
        if (sx < minx) minx = sx; if (sy < miny) miny = sy;
        if (sx > maxx) maxx = sx; if (sy > maxy) maxy = sy;
    }
    if (!any) return false;

    minx -= pad; miny -= pad; maxx += pad; maxy += pad;
    *x1 = minx; *y1 = miny; *x2 = maxx; *y2 = maxy;
    return true;
}

static int HasLineOfSight(const vec3_t src, const vec3_t dst)
{
    pmtrace_t tr;
    gEngfuncs.pEventAPI->EV_SetUpPlayerPrediction(false, true);
    gEngfuncs.pEventAPI->EV_PushPMStates();
    gEngfuncs.pEventAPI->EV_SetSolidPlayers(-1);
    gEngfuncs.pEventAPI->EV_SetTraceHull(2); // standing hull
    gEngfuncs.pEventAPI->EV_PlayerTrace(src, dst, PM_NORMAL, -1, &tr);
    gEngfuncs.pEventAPI->EV_PopPMStates();
    return tr.fraction >= 1.0f;
}

static int IsScientistModel(const char* name)
{
    if (!name) return false;
    return strstr(name, "scientist.mdl") != NULL;
}

static void DrawLabel(int x, int y, const char* txt, int r, int g, int b, int a)
{
    // Small shadow then text
    gEngfuncs.pfnDrawSetTextColor(0, 0, 0);
    gEngfuncs.pfnDrawConsoleString(x + 1, y + 1, (char*)txt);
    gEngfuncs.pfnDrawSetTextColor((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f);
    gEngfuncs.pfnDrawConsoleString(x, y, (char*)txt);
}

// ESP state structure
typedef struct
{
    float next_time;
} ESP_State_t;

static ESP_State_t gESPState = { 0.0f };

static void ESP_Init(void)
{
    gEngfuncs.pfnRegisterVariable(clesp.name,          clesp.string,          clesp.flags);
    gEngfuncs.pfnRegisterVariable(clesprate.name,     clesprate.string,     clesprate.flags);
    gEngfuncs.pfnRegisterVariable(clesppad.name,      clesppad.string,      clesppad.flags);
    gEngfuncs.pfnRegisterVariable(clespwidth.name,    clespwidth.string,    clespwidth.flags);
    gEngfuncs.pfnRegisterVariable(clespalpha.name,    clespalpha.string,    clespalpha.flags);
    gEngfuncs.pfnRegisterVariable(clesplos.name,      clesplos.string,      clesplos.flags);
    gEngfuncs.pfnRegisterVariable(clesplabels.name,   clesplabels.string,   clesplabels.flags);
    gEngfuncs.pfnRegisterVariable(clespscientists.name, clespscientists.string, clespscientists.flags);
}

static void ESP_VidInit(void)
{
    // Nothing to initialize for video
}

static void ESP_Redraw(float time, int intermission)
{
    if (intermission) return;

    // sv_cheats gate: if not set, hard suppress rendering
    float cheats = gEngfuncs.pfnGetCvarFloat("sv_cheats");
    if (cheats < 1.0f) return;

    if (clesp.value < 1.0f) return;

    float rate = clampf(clesprate.value, 15.0f, 90.0f);
    float interval = 1.0f / rate;
    if (time < gESPState.next_time) return;
    gESPState.next_time = time + interval;

    int pad   = clampi((int)clesppad.value,   0, 24);
    int width = clampi((int)clespwidth.value, 1, 6);
    int alpha = clampi((int)clespalpha.value, 30, 255);
    int losOnly   = clesplos.value > 0.0f;
    int showLabels= clesplabels.value > 0.0f;
    int onlySci   = clespscientists.value > 0.0f;

    cl_entity_t* local = gEngfuncs.GetLocalPlayer();
    if (!local) return;

    vec3_t eye;
    VectorCopy(local->origin, eye);
    eye[0] += local->curstate.view_ofs[0];
    eye[1] += local->curstate.view_ofs[1];
    eye[2] += local->curstate.view_ofs[2];

    for (int i = 1; i < clgame.maxEntities; ++i)
    {
        cl_entity_t* ent = &clgame.entities[i];
        if (!ent || !ent->model) continue;
        if (onlySci && !IsScientistModel(ent->model->name)) continue;

        // LOS filter
        if (losOnly && !HasLineOfSight(eye, ent->origin))
            continue;

        // BBox preference: use curstate mins/maxs; fallback to human-sized box
        vec3_t mins = ent->curstate.mins;
        vec3_t maxs = ent->curstate.maxs;
        if (mins[0] == 0 && mins[1] == 0 && mins[2] == 0 && 
            maxs[0] == 0 && maxs[1] == 0 && maxs[2] == 0)
        {
            mins[0] = mins[1] = mins[2] = -12;
            maxs[0] = maxs[1] = 12;
            maxs[2] = 72;
        }

        int x1, y1, x2, y2;
        if (!ComputeScreenBoxFromBBox(ent->origin, mins, maxs, pad, &x1, &y1, &x2, &y2))
            continue;

        // Health tint: red base, add green component if health is high
        int health = ent->curstate.health; // may be 0 for some ents
        int rr = 255, gg = 0, bb = 0;
        if (health > 0)
        {
            gg = clampi(health, 0, 255) / 2; // soft tint
        }

        DrawRect(x1, y1, x2, y2, rr, gg, bb, alpha, width, 1);

        if (showLabels)
        {
            char buf[64];
            const char* name = ent->model->name ? ent->model->name : "entity";
            int labelX = x1;
            int labelY = y1 - 10;
            snprintf(buf, sizeof(buf), "%s  hp:%d", name, health);
            DrawLabel(labelX, labelY, buf, 255, 255, 255, 255);
        }
    }
}

// Global hooks (for external use) - already defined above
