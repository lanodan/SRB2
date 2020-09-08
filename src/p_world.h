// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 2020 by Sonic Team Junior.
// Copyright (C) 2020 by Jaime Ita Passos.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  p_world.h
/// \brief World state

#ifndef __P_WORLD__
#define __P_WORLD__

#include "p_setup.h"
#include "r_state.h"
#include "p_polyobj.h"
#include "doomstat.h"

// Player spawn spots for deathmatch.
#define MAX_DM_STARTS 64

#define WAYPOINTSEQUENCESIZE 256
#define NUMWAYPOINTSEQUENCES 256

typedef struct
{
	boolean visited;
	vector3_t pos;
	angle_t angle;
} worldplayerinfo_t;

typedef struct
{
	INT32 gamemap;
	worldplayerinfo_t playerinfo[MAXPLAYERS];
	thinker_t *thlist;

	INT32 players;

	size_t numvertexes, numsegs, numsectors, numsubsectors, numnodes, numlines, numsides, nummapthings;
	vertex_t *vertexes;
	seg_t *segs;
	sector_t *sectors;
	subsector_t *subsectors;
	node_t *nodes;
	line_t *lines;
	side_t *sides;
	mapthing_t *mapthings;

	sector_t *spawnsectors;
	line_t *spawnlines;
	side_t *spawnsides;

	size_t numflats;
	levelflat_t *flats;

	// Needed to store the number of the dummy sky flat.
	// Used for rendering, as well as tracking projectiles etc.
	INT32 skyflatnum;

	// Player spawn spots.
	mapthing_t *playerstarts[MAXPLAYERS]; // Cooperative
	mapthing_t *bluectfstarts[MAXPLAYERS]; // CTF
	mapthing_t *redctfstarts[MAXPLAYERS]; // CTF
	mapthing_t *deathmatchstarts[MAX_DM_STARTS];

	// Maintain single and multi player starting spots.
	INT32 numdmstarts, numcoopstarts, numredctfstarts, numbluectfstarts;

	mobj_t *skyboxmo[2]; // current skybox mobjs: 0 = viewpoint, 1 = centerpoint
	mobj_t *skyboxviewpnts[16]; // array of MT_SKYBOX viewpoint mobjs
	mobj_t *skyboxcenterpnts[16]; // array of MT_SKYBOX centerpoint mobjs

	mobj_t *waypoints[NUMWAYPOINTSEQUENCES][WAYPOINTSEQUENCESIZE];
	UINT16 numwaypoints[NUMWAYPOINTSEQUENCES];

	UINT8 *rejectmatrix; // for fast sight rejection
	INT32 *blockmaplump; // offsets in blockmap are from here
	INT32 *blockmap; // Big blockmap
	INT32 bmapwidth;
	INT32 bmapheight; // in mapblocks
	fixed_t bmaporgx;
	fixed_t bmaporgy; // origin of block map
	mobj_t **blocklinks; // for thing chains

	// The Polyobjects
	polyobj_t *PolyObjects;
	INT32 numPolyObjects;
	polymaplink_t **polyblocklinks; // Polyobject Blockmap -- initialized in P_LoadBlockMap
	polymaplink_t *po_bmap_freelist; // free list of blockmap links
} world_t;

extern world_t *world;
extern world_t *localworld;
extern world_t *viewworld;

extern world_t **worldlist;
extern INT32 numworlds;

world_t *P_InitWorld(void);
world_t *P_InitNewWorld(void);

void P_UnloadWorld(world_t *w);
void P_UnloadWorldList(void);
void P_UnloadWorldPlayer(player_t *player);

void P_SetGameWorld(world_t *w);
void P_SetViewWorld(world_t *w);

void P_SetWorld(world_t *w);
void P_SwitchWorld(player_t *player, world_t *w);
void P_SetWorldVisited(player_t *player, world_t *w);

void Command_Switchworld_f(void);
void Command_Listworlds_f(void);

INT32 P_AddLevelFlatForWorld(world_t *w, const char *flatname);

#endif