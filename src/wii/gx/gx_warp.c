/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2008 Eluan Miranda

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// gl_warp.c -- sky and water polygons

#include "../../generic/quakedef.h"
#include "gxutils.h"

extern	model_t	*loadmodel;

extern int		skytexturenum;

int		solidskytexture;
int		alphaskytexture;

gltexture_t	*skybox_textures[6];

float	speedscale;		// for top sky and bottom sky

int	    skyimage[5]; // Where sky images are stored
char	skybox_name[32] = ""; //name of current skybox, or "" if no skybox
// cut off down for half skybox
char	*suf[5] = {"rt", "bk", "lf", "ft", "up" };

int loadtextureimage (char* filename, int matchwidth, int matchheight, qboolean complain, qboolean mipmap);

msurface_t	*warpface;

extern cvar_t gl_subdivide_size;

void BoundPoly (int numverts, float *verts, vec3_t mins, vec3_t maxs)
{
	int		i, j;
	float	*v;

	mins[0] = mins[1] = mins[2] = 9999;
	maxs[0] = maxs[1] = maxs[2] = -9999;
	v = verts;
	for (i=0 ; i<numverts ; i++)
		for (j=0 ; j<3 ; j++, v++)
		{
			if (*v < mins[j])
				mins[j] = *v;
			if (*v > maxs[j])
				maxs[j] = *v;
		}
}

void SubdividePolygon (int numverts, float *verts)
{
	int		i, j, k;
	vec3_t	mins, maxs;
	float	m;
	float	*v;
	vec3_t	front[64], back[64];
	int		f, b;
	float	dist[64];
	float	frac;
	glpoly_t	*poly;
	float	s, t;

	if (numverts > 60)
		Sys_Error ("numverts = %i", numverts);

	BoundPoly (numverts, verts, mins, maxs);

	for (i=0 ; i<3 ; i++)
	{
		m = (mins[i] + maxs[i]) * 0.5;
		m = gl_subdivide_size.value * floor (m/gl_subdivide_size.value + 0.5);
		if (maxs[i] - m < 8)
			continue;
		if (m - mins[i] < 8)
			continue;

		// cut it
		v = verts + i;
		for (j=0 ; j<numverts ; j++, v+= 3)
			dist[j] = *v - m;

		// wrap cases
		dist[j] = dist[0];
		v-=i;
		VectorCopy (verts, v);

		f = b = 0;
		v = verts;
		for (j=0 ; j<numverts ; j++, v+= 3)
		{
			if (dist[j] >= 0)
			{
				VectorCopy (v, front[f]);
				f++;
			}
			if (dist[j] <= 0)
			{
				VectorCopy (v, back[b]);
				b++;
			}
			if (dist[j] == 0 || dist[j+1] == 0)
				continue;
			if ( (dist[j] > 0) != (dist[j+1] > 0) )
			{
				// clip point
				frac = dist[j] / (dist[j] - dist[j+1]);
				for (k=0 ; k<3 ; k++)
					front[f][k] = back[b][k] = v[k] + frac*(v[3+k] - v[k]);
				f++;
				b++;
			}
		}

		SubdividePolygon (f, front[0]);
		SubdividePolygon (b, back[0]);
		return;
	}

	poly = Hunk_Alloc (sizeof(glpoly_t) + (numverts-4) * VERTEXSIZE*sizeof(float));
	poly->next = warpface->polys;
	warpface->polys = poly;
	poly->numverts = numverts;
	for (i=0 ; i<numverts ; i++, verts+= 3)
	{
		VectorCopy (verts, poly->verts[i]);
		s = DotProduct (verts, warpface->texinfo->vecs[0]);
		t = DotProduct (verts, warpface->texinfo->vecs[1]);
		poly->verts[i][3] = s;
		poly->verts[i][4] = t;
	}
}

/*
================
GL_SubdivideSurface

Breaks a polygon up along axial 64 unit
boundaries so that turbulent and sky warps
can be done reasonably.
================
*/
void GL_SubdivideSurface (msurface_t *fa)
{
	vec3_t		verts[64];
	int			numverts;
	int			i;
	int			lindex;
	float		*vec;

	warpface = fa;

	//
	// convert edges back to a normal polygon
	//
	numverts = 0;
	for (i=0 ; i<fa->numedges ; i++)
	{
		lindex = loadmodel->surfedges[fa->firstedge + i];

		if (lindex > 0)
			vec = loadmodel->vertexes[loadmodel->edges[lindex].v[0]].position;
		else
			vec = loadmodel->vertexes[loadmodel->edges[-lindex].v[1]].position;
		VectorCopy (vec, verts[numverts]);
		numverts++;
	}

	SubdividePolygon (numverts, verts[0]);
}

//=========================================================



// speed up sin calculations - Ed
float	turbsin[] =
{
	#include "gx_warp_sin.h"
};
#define TURBSCALE (256.0 / (2 * M_PI))

/*
=============
EmitWaterPolys

Does a water warp on the pre-fragmented glpoly_t chain
=============
*/
void EmitWaterPolys (msurface_t *fa)
{
	glpoly_t	*p;
	float		*v;
	int			i;
	float		s, t, os, ot;

	for (p=fa->polys ; p ; p=p->next)
	{
		GX_Begin (GX_TRIANGLEFAN, GX_VTXFMT0, p->numverts);
		for (i=0,v=p->verts[0] ; i<p->numverts ; i++, v+=VERTEXSIZE)
		{
			os = v[3];
			ot = v[4];

			s = os + turbsin[(int)((ot*0.125+realtime) * TURBSCALE) & 255];
			s *= (1.0/64);

			t = ot + turbsin[(int)((os*0.125+realtime) * TURBSCALE) & 255];
			t *= (1.0/64);

			GX_Position3f32(v[0], v[1], v[2]);
			GX_Color4u8(0xff, 0xff, 0xff, r_wateralpha.value * 0xff); // ELUTODO issues with draw order AND shoudn't be enabled if the map doesn't have watervis info
			GX_TexCoord2f32(s, t);
		}
		GX_End ();
	}
}




/*
=============
EmitSkyPolys
=============
*/
void EmitSkyPolys (msurface_t *fa)
{
	glpoly_t	*p;
	float		*v;
	int			i;
	float	s, t;
	vec3_t	dir;
	float	length;

	for (p=fa->polys ; p ; p=p->next)
	{
		GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, p->numverts);
		for (i=0,v=p->verts[0] ; i<p->numverts ; i++, v+=VERTEXSIZE)
		{
			VectorSubtract (v, r_origin, dir);
			dir[2] *= 3;	// flatten the sphere

			length = dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2];
			length = sqrt (length);
			length = 6*63/length;

			dir[0] *= length;
			dir[1] *= length;

			s = (speedscale + dir[0]) * (1.0/128);
			t = (speedscale + dir[1]) * (1.0/128);

			GX_Position3f32(v[0], v[1], v[2]);
			GX_Color4u8(0xff, 0xff, 0xff, 0xff);
			GX_TexCoord2f32(s, t);
		}
		GX_End ();
	}
}

/*
===============
EmitBothSkyLayers

Does a sky warp on the pre-fragmented gxpoly_t chain
This will be called for brushmodels, the world
will have them chained together.
===============
*/
void EmitBothSkyLayers (msurface_t *fa)
{
	int			i;
	int			lindex;
	float		*vec;

	GL_DisableMultitexture();

	GL_Bind0 (solidskytexture);
	speedscale = realtime*8;
	speedscale -= (int)speedscale & ~127 ;

	EmitSkyPolys (fa);

	GX_SetBlendMode(GX_BM_BLEND, gxu_blend_src_value, gxu_blend_dst_value, GX_LO_NOOP); 
	GL_Bind0 (alphaskytexture);
	speedscale = realtime*16;
	speedscale -= (int)speedscale & ~127 ;

	EmitSkyPolys (fa);

	GX_SetBlendMode(GX_BM_NONE, gxu_blend_src_value, gxu_blend_dst_value, GX_LO_NOOP); 
}

#ifndef QUAKE2

/*
=================
R_DrawSkyChain
=================
*/
void R_DrawSkyChain (msurface_t *s)
{
	msurface_t	*fa;

	GL_DisableMultitexture();

	GL_Bind0(solidskytexture);
	speedscale = realtime*8;
	speedscale -= (int)speedscale & ~127 ;

	for (fa=s ; fa ; fa=fa->texturechain)
		EmitSkyPolys (fa);

	QGX_Blend(TRUE);
	GL_Bind0 (alphaskytexture);
	speedscale = realtime*16;
	speedscale -= (int)speedscale & ~127 ;

	for (fa=s ; fa ; fa=fa->texturechain)
		EmitSkyPolys (fa);

	QGX_Blend(FALSE);
}

#endif

/*
=================================================================

  Quake 2 environment sky

=================================================================
*/
#define	SKY_TEX		2000


#if 1
/*
==================
Sky_LoadSkyBox
==================
*/
//char	*suf[6] = {"rt", "bk", "lf", "ft", "up", "dn"};
void Sky_LoadSkyBox(char* name)
{
	if (strcmp(skybox_name, name) == 0)
		return; //no change

	//turn off skybox if sky is set to ""
	if (name[0] == '0') {
		skybox_name[0] = 0;
		return;
	}

	// Do sides one way and top another, bottom is not done
    for (int i = 0; i < 4; i++)
    {
        int mark = Hunk_LowMark ();

		skyimage[i] = loadtextureimage (va("gfx/env/%s%s", name, suf[i]), 0, 0, false, false);

		if(!(skyimage))
		{
			Con_Printf("Sky: %s[%s] not found, used std\n", name, suf[i]);
			skyimage[i] = loadtextureimage (va("gfx/env/skybox%s", suf[i]), 0, 0, false, false);
		    if(!(skyimage))
		    {
			    Sys_Error("STD SKY NOT FOUND!");
			}

		}
        Hunk_FreeToLowMark (mark);
    }

	int mark = Hunk_LowMark ();
	skyimage[4] = loadtextureimage (va("gfx/env/%sup", name), 0, 0, false, false);
	if(!(skyimage))
	{
		Con_Printf("Sky: %s[%s] not found, used std\n", name, suf[4]);
		skyimage[4] = loadtextureimage (va("gfx/env/skybox%s", suf[4]), 0, 0, false, false);
		if(!(skyimage))
		{
			Sys_Error("STD SKY NOT FOUND!");
		}

	}
	Hunk_FreeToLowMark (mark);

	strcpy(skybox_name, name);
}

/*
=================
Sky_NewMap
=================
*/
void Sky_NewMap (void)
{
	char	key[128], value[4096];
	char	*data;

    //purge old sky textures
    //UnloadSkyTexture ();

	//
	// initially no sky
	//
	Sky_LoadSkyBox (""); //not used

	//
	// read worldspawn (this is so ugly, and shouldn't it be done on the server?)
	//
	data = cl.worldmodel->entities;
	if (!data)
		return; //FIXME: how could this possibly ever happen? -- if there's no
	// worldspawn then the sever wouldn't send the loadmap message to the client

	data = COM_Parse(data);

	if (!data) //should never happen
		return; // error

	if (com_token[0] != '{') //should never happen
		return; // error

	while (1)
	{
		data = COM_Parse(data);

		if (!data)
			return; // error

		if (com_token[0] == '}')
			break; // end of worldspawn

		if (com_token[0] == '_')
			strcpy(key, com_token + 1);
		else
			strcpy(key, com_token);
		while (key[strlen(key)-1] == ' ') // remove trailing spaces
			key[strlen(key)-1] = 0;

		data = COM_Parse(data);
		if (!data)
			return; // error

		strcpy(value, com_token);

        if (!strcmp("sky", key))
            Sky_LoadSkyBox(value);
        else if (!strcmp("skyname", key)) //half-life
            Sky_LoadSkyBox(value);
        else if (!strcmp("qlsky", key)) //quake lives
            Sky_LoadSkyBox(value);
	}
}

/*
=================
Sky_SkyCommand_f
=================
*/
void Sky_SkyCommand_f (void)
{
	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf("\"sky\" is \"%s\"\n", skybox_name);
		break;
	case 2:
		Sky_LoadSkyBox(Cmd_Argv(1));
		break;
	default:
		Con_Printf("usage: sky <skyname>\n");
	}
}

/*
=============
Sky_Init
=============
*/
void Sky_Init (void)
{
	int		i;

	Cmd_AddCommand ("sky",Sky_SkyCommand_f);

	for (i=0; i<5; i++)
		skyimage[i] = NULL;
}

#endif

static vec3_t	skyclip[6] = {
	{1,1,0},
	{1,-1,0},
	{0,-1,1},
	{0,1,1},
	{1,0,1},
	{-1,0,1}
};
int	c_sky;

// 1 = s, 2 = t, 3 = 2048
static int	st_to_vec[6][3] =
{
	{3,-1,2},
	{-3,1,2},

	{1,3,2},
	{-1,-3,2},

	{-2,-1,3},		// 0 degrees yaw, look straight up
	{2,-1,-3}		// look straight down

//	{-1,2,3},
//	{1,2,-3}
};

// s = [0]/[2], t = [1]/[2]
static int	vec_to_st[6][3] =
{
	{-2,3,1},
	{2,3,-1},

	{1,3,2},
	{-1,3,-2},

	{-2,-1,3},
	{-2,1,-3}

//	{-1,2,3},
//	{1,2,-3}
};

static float	skymins[2][6], skymaxs[2][6];

/*
==============
R_ClearSkyBox
==============
*/
void R_ClearSkyBox (void)
{
	int		i;

	for (i=0 ; i<5 ; i++)
	{
		skymins[0][i] = skymins[1][i] = 9999;
		skymaxs[0][i] = skymaxs[1][i] = -9999;
	}
}


void MakeSkyVec (float s, float t, int axis)
{
	vec3_t		v, b;
	int			j, k;

	b[0] = s*2048;
	b[1] = t*2048;
	b[2] = 2048;

	for (j=0 ; j<3 ; j++)
	{
		k = st_to_vec[axis][j];
		if (k < 0)
			v[j] = -b[-k - 1];
		else
			v[j] = b[k - 1];
		v[j] += r_origin[j];
	}

	// avoid bilerp seam
	s = (s+1)*0.5;
	t = (t+1)*0.5;

	if (s < 1.0/512)
		s = 1.0/512;
	else if (s > 511.0/512)
		s = 511.0/512;
	if (t < 1.0/512)
		t = 1.0/512;
	else if (t > 511.0/512)
		t = 511.0/512;

	t = 1.0 - t;
	GX_Position3f32(*v, *(v+1), *(v+2));
	GX_Color4u8(gxu_cur_r, gxu_cur_g, gxu_cur_b, gxu_cur_a);
	GX_TexCoord2f32 (s, t);
}

/*
==============
R_DrawSkyBox
==============
*/

float skynormals[5][3] = {
	{ 1.f, 0.f, 0.f },
	{ -1.f, 0.f, 0.f },
	{ 0.f, 1.f, 0.f },
	{ 0.f, -1.f, 0.f },
	{ 0.f, 0.f, 1.f }
};

float skyrt[5][3] = {
	{ 0.f, -1.f, 0.f },
	{ 0.f, 1.f, 0.f },
	{ 1.f, 0.f, 0.f },
	{ -1.f, 0.f, 0.f },
	{ 0.f, -1.f, 0.f }
};

float skyup[5][3] = {
	{ 0.f, 0.f, 1.f },
	{ 0.f, 0.f, 1.f },
	{ 0.f, 0.f, 1.f },
	{ 0.f, 0.f, 1.f },
	{ -1.f, 0.f, 0.f }
};

//#ifdef QUAKE2

/*
==============
R_DrawSkyBox
==============
*/

//Currently under construction
int	skytexorder[6] = {0,2,1,3,4,5};
void R_DrawSkyBox (void)
{
	int		i, j, k;
	vec3_t	v;
	float	s, t;

#if 0
	glEnable (GL_BLEND);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glColor4f (1,1,1,0.5);
	glDisable (GL_DEPTH_TEST);
#endif
	for (i=0 ; i<6 ; i++)
	{
		if (skymins[0][i] >= skymaxs[0][i]
		|| skymins[1][i] >= skymaxs[1][i])
			continue;

		GL_Bind0 (skyimage[skytexorder[i]]);
#if 0
	skymins[0][i] = -1;
	skymins[1][i] = -1;
	skymaxs[0][i] = 1;
	skymaxs[1][i] = 1;
#endif
		GX_Begin (GX_QUADS, GX_VTXFMT0, 4);
		MakeSkyVec (skymins[0][i], skymins[1][i], i);
		MakeSkyVec (skymins[0][i], skymaxs[1][i], i);
		MakeSkyVec (skymaxs[0][i], skymaxs[1][i], i);
		MakeSkyVec (skymaxs[0][i], skymins[1][i], i);
		GX_End ();
	}
#if 0
	glDisable (GL_BLEND);
	glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	glColor4f (1,1,1,0.5);
	glEnable (GL_DEPTH_TEST);
#endif
}

//#endif

//===============================================================

/*
=============
R_InitSky

A sky texture is 256*128, with the right side being a masked overlay
==============
*/
void R_InitSky (texture_t *mt)
{
	int			i, j, p;
	byte		*src;
	byte		trans[128*128];

	src = (byte *)mt + mt->offsets[0];

	for (i=0 ; i<128 ; i++)
		for (j=0 ; j<128 ; j++)
			trans[(i*128) + j] = src[i*256 + j + 128];

	solidskytexture = GL_LoadTexture("render_solidskytexture", 128, 128, trans, TRUE, TRUE, FALSE, 1);

	for (i=0 ; i<128 ; i++)
		for (j=0 ; j<128 ; j++)
		{
			p = src[i*256 + j];
			if (p == 0)
				trans[(i*128) + j] = 255;
			else
				trans[(i*128) + j] = p;
		}

	alphaskytexture = GL_LoadTexture("render_alphaskytexture", 128, 128, trans, TRUE, TRUE, FALSE, 1);
}

