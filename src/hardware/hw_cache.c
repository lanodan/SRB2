// SONIC ROBO BLAST 2
//-----------------------------------------------------------------------------
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2020 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file hw_cache.c
/// \brief load and convert graphics to the hardware format

#include "../doomdef.h"

#ifdef HWRENDER
#include "hw_glob.h"
#include "hw_drv.h"
#include "hw_batching.h"

#include "../doomstat.h"    //gamemode
#include "../i_video.h"     //rendermode
#include "../r_data.h"
#include "../r_textures.h"
#include "../w_wad.h"
#include "../z_zone.h"
#include "../v_video.h"
#include "../r_draw.h"
#include "../r_patch.h"
#include "../r_picformats.h"
#include "../p_setup.h"

INT32 patchformat = GL_TEXFMT_AP_88; // use alpha for holes
INT32 textureformat = GL_TEXFMT_P_8; // use chromakey for hole

static INT32 format2bpp(GLTextureFormat_t format)
{
	if (format == GL_TEXFMT_RGBA)
		return 4;
	else if (format == GL_TEXFMT_ALPHA_INTENSITY_88 || format == GL_TEXFMT_AP_88)
		return 2;
	else
		return 1;
}

static colorlookup_t texel_colorlookup;

// Lactozilla: Compare the pixel's RGB color with the palette's.
// If they match, remap it.
static void ColormapRGBAPixel(RGBA_t *texelu32, UINT8 *texel, const UINT8 *colormap)
{
	UINT8 *basepal = V_CacheBasePalette();
	INT32 i = 0;

	for (; i < 256; i++)
	{
		UINT8 r = *(basepal), g = *(basepal + 1), b = *(basepal + 2);
		UINT32 rgb = R_PutRgbaRGB(r, g, b);

		if (rgb == R_GetRgbaRGB(texelu32->rgba))
		{
			// Find the RGBA color of the mapped palette index
			RGBA_t mapped = V_GetColor(colormap[i]);

			// Convert to the target bit depth
			if (texel)
			{
				*texel = GetColorLUT(&texel_colorlookup, mapped.s.red, mapped.s.green, mapped.s.blue);
				texelu32->rgba = mapped.rgba;
			}
			else // This preserves the source pixel's translucency.
				texelu32->rgba = R_GetRgbaRGB(mapped.rgba) + R_PutRgbaA(R_GetRgbaA(texelu32->rgba));

			// Stop looking for a matching color
			break;
		}

		basepal += 3;
	}
}

// This code was originally placed directly in HWR_DrawPatchInCache.
// It is now split from it for my sanity! (and the sanity of others)
// -- Monster Iestyn (13/02/19)
static void HWR_DrawColumnInCache(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp, pictureformat_t format)
{
	fixed_t yfrac, position, count;
	UINT8 *dest;
	const UINT8 *source = NULL;
	const UINT16 *sourceu16 = NULL;
	const UINT32 *sourceu32 = NULL;
	INT32 topdelta, prevdelta = -1;
	INT32 originy = 0;

	// for writing a pixel to dest
	RGBA_t colortemp;
	UINT8 alpha = 0;
	UINT8 texel = 0;
	UINT16 texelu16;
	RGBA_t texelu32;
	INT32 sourcebpp = Picture_FormatBPP(format);

	static UINT32 (*PixelBlendFunction)(RGBA_t background, RGBA_t foreground, int style, UINT8 alpha) = ASTBlendPixel;
	(void)patchheight; // This parameter is unused

	if (originPatch) // originPatch can be NULL here, unlike in the software version
	{
		PixelBlendFunction = ASTBlendTexturePixel;
		originy = originPatch->originy;
	}

	memset(&texelu32, 0x00, sizeof(RGBA_t));

	while (patchcol->topdelta != 0xff)
	{
		topdelta = patchcol->topdelta;
		if (topdelta <= prevdelta)
			topdelta += prevdelta;
		prevdelta = topdelta;
		count = ((patchcol->length * scale_y) + (FRACUNIT/2)) >> FRACBITS;
		position = originy + topdelta;

		source = (const UINT8 *)patchcol + 3;
		if (sourcebpp == PICDEPTH_32BPP)
			sourceu32 = (const UINT32 *)source;
		else if (sourcebpp == PICDEPTH_16BPP)
			sourceu16 = (const UINT16 *)source;

		yfrac = 0;
		//yfracstep = (patchcol->length << FRACBITS) / count;
		if (position < 0)
		{
			yfrac = -position<<FRACBITS;
			count += (((position * scale_y) + (FRACUNIT/2)) >> FRACBITS);
			position = 0;
		}

		position = ((position * scale_y) + (FRACUNIT/2)) >> FRACBITS;

		if (position < 0)
			position = 0;

		if (position + count >= pblockheight)
			count = pblockheight - position;

		dest = block + (position*blockmodulo);
		while (count > 0)
		{
			count--;

			// Read the texel
			if (sourcebpp == PICDEPTH_32BPP)
			{
				UINT32 s32 = sourceu32[yfrac>>FRACBITS];
				texelu32.rgba = s32;
				alpha = texelu32.s.alpha;

				// Convert to the target bit depth
				if (bpp < 3)
					texel = GetColorLUT(&texel_colorlookup, texelu32.s.red, texelu32.s.green, texelu32.s.blue);
			}
			else
			{
				if (sourcebpp == PICDEPTH_16BPP)
				{
					UINT16 px = sourceu16[yfrac>>FRACBITS];
					texel = (px & 0xFF);
					alpha = ((sourceu16[yfrac>>FRACBITS] & 0xFF00) >> 8);
				}
				else
					texel = source[yfrac>>FRACBITS];

				// Set translucency for 8bpp source pictures
				if (sourcebpp == 8)
				{
					alpha = 0xFF;
					// If the mipmap is chromakeyed, check if the texel's color
					// is equivalent to the chroma key's color index.
					if ((mipmap->flags & TF_CHROMAKEYED) && (texel == HWR_PATCHES_CHROMAKEY_COLORINDEX))
						alpha = 0x00;
				}
			}

			//Hurdler: 25/04/2000: now support colormap in hardware mode
			if (mipmap->colormap)
			{
				if (sourcebpp == PICDEPTH_32BPP)
					ColormapRGBAPixel(&texelu32, (bpp < 3) ? (&texel) : NULL, mipmap->colormap);
				else
					texel = mipmap->colormap[texel];
			}

			// Convert to the target bit depth
			if ((sourcebpp <= 16) && (bpp >= 3))
				texelu32 = V_GetColor(texel);

			// hope compiler will get this switch out of the loops (dreams...)
			// gcc do it ! but vcc not ! (why don't use cygwin gcc for win32 ?)
			// Alam: SRB2 uses Mingw, HUGS
			switch (bpp)
			{
				case 2 :
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 texel = ASTBlendPaletteIndexes(*(dest+1), texel, originPatch->style, originPatch->alpha);
						 texelu16 = (UINT16)((alpha<<8) | texel);
						 memcpy(dest, &texelu16, sizeof(UINT16));
						 break;
				case 3 : colortemp = texelu32;
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = PixelBlendFunction(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t)-sizeof(UINT8));
						 break;
				case 4 : colortemp = texelu32;
						 colortemp.s.alpha = alpha;
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = PixelBlendFunction(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t));
						 break;
				// default is 1
				default:
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 *dest = ASTBlendPaletteIndexes(*dest, texel, originPatch->style, originPatch->alpha);
						 else
							 *dest = texel;
						 break;
			}

			dest += blockmodulo;
			yfrac += yfracstep;
		}
		if (sourcebpp == PICDEPTH_32BPP)
			patchcol = (const column_t *)((const UINT32 *)patchcol + patchcol->length);
		else if (sourcebpp == PICDEPTH_16BPP)
			patchcol = (const column_t *)((const UINT16 *)patchcol + patchcol->length);
		else
			patchcol = (const column_t *)((const UINT8 *)patchcol + patchcol->length);
		patchcol = (const column_t *)((const UINT8 *)patchcol + 4);
	}
}

static void HWR_DrawFlippedColumnInCache(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp, pictureformat_t format)
{
	fixed_t yfrac, position, count;
	UINT8 *dest;
	const UINT8 *source = NULL;
	const UINT16 *sourceu16 = NULL;
	const UINT32 *sourceu32 = NULL;
	INT32 topdelta, prevdelta = -1;
	INT32 originy = 0;

	// for writing a pixel to dest
	RGBA_t colortemp;
	UINT8 alpha = 0;
	UINT8 texel = 0;
	UINT16 texelu16;
	RGBA_t texelu32;
	INT32 sourcebpp = Picture_FormatBPP(format);

	static UINT32 (*PixelBlendFunction)(RGBA_t background, RGBA_t foreground, int style, UINT8 alpha) = ASTBlendPixel;

	if (originPatch) // originPatch can be NULL here, unlike in the software version
	{
		PixelBlendFunction = ASTBlendTexturePixel;
		originy = originPatch->originy;
	}

	memset(&texelu32, 0x00, sizeof(RGBA_t));

	while (patchcol->topdelta != 0xff)
	{
		topdelta = patchcol->topdelta;
		if (topdelta <= prevdelta)
			topdelta += prevdelta;
		prevdelta = topdelta;
		topdelta = patchheight-patchcol->length-topdelta;
		count  = ((patchcol->length * scale_y) + (FRACUNIT/2)) >> FRACBITS;
		position = originy + topdelta;

		source = (const UINT8 *)patchcol + 3;
		if (sourcebpp == PICDEPTH_32BPP)
			sourceu32 = (const UINT32 *)source;
		else if (sourcebpp == PICDEPTH_16BPP)
			sourceu16 = (const UINT16 *)source;

		yfrac = (patchcol->length-1) << FRACBITS;

		if (position < 0)
		{
			yfrac += position<<FRACBITS;
			count += (((position * scale_y) + (FRACUNIT/2)) >> FRACBITS);
			position = 0;
		}

		position = ((position * scale_y) + (FRACUNIT/2)) >> FRACBITS;

		if (position < 0)
			position = 0;

		if (position + count >= pblockheight)
			count = pblockheight - position;

		dest = block + (position*blockmodulo);
		while (count > 0)
		{
			count--;

			// Read the texel
			if (sourcebpp == PICDEPTH_32BPP)
			{
				UINT32 s32 = sourceu32[yfrac>>FRACBITS];
				texelu32.rgba = s32;
				alpha = texelu32.s.alpha;

				// Convert to the target bit depth
				if (bpp < 3)
					texel = GetColorLUT(&texel_colorlookup, texelu32.s.red, texelu32.s.green, texelu32.s.blue);
			}
			else
			{
				if (sourcebpp == PICDEPTH_16BPP)
				{
					UINT16 px = sourceu16[yfrac>>FRACBITS];
					texel = (px & 0xFF);
					alpha = ((sourceu16[yfrac>>FRACBITS] & 0xFF00) >> 8);
				}
				else
					texel = source[yfrac>>FRACBITS];

				// Set translucency for 8bpp source pictures
				if (sourcebpp == 8)
				{
					alpha = 0xFF;
					// If the mipmap is chromakeyed, check if the texel's color
					// is equivalent to the chroma key's color index.
					if ((mipmap->flags & TF_CHROMAKEYED) && (texel == HWR_PATCHES_CHROMAKEY_COLORINDEX))
						alpha = 0x00;
				}
			}

			//Hurdler: 25/04/2000: now support colormap in hardware mode
			if (mipmap->colormap)
			{
				if (sourcebpp == PICDEPTH_32BPP)
					ColormapRGBAPixel(&texelu32, (bpp < 3) ? (&texel) : NULL, mipmap->colormap);
				else
					texel = mipmap->colormap[texel];
			}

			// Convert to the target bit depth
			if ((sourcebpp <= 16) && (bpp >= 3))
				texelu32 = V_GetColor(texel);

			// hope compiler will get this switch out of the loops (dreams...)
			// gcc do it ! but vcc not ! (why don't use cygwin gcc for win32 ?)
			// Alam: SRB2 uses Mingw, HUGS
			switch (bpp)
			{
				case 2 :
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 texel = ASTBlendPaletteIndexes(*(dest+1), texel, originPatch->style, originPatch->alpha);
						 texelu16 = (UINT16)((alpha<<8) | texel);
						 memcpy(dest, &texelu16, sizeof(UINT16));
						 break;
				case 3 : colortemp = texelu32;
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = PixelBlendFunction(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t)-sizeof(UINT8));
						 break;
				case 4 : colortemp = texelu32;
						 colortemp.s.alpha = alpha;
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
						 {
							 RGBA_t rgbatexel;
							 rgbatexel.rgba = *(UINT32 *)dest;
							 colortemp.rgba = PixelBlendFunction(rgbatexel, colortemp, originPatch->style, originPatch->alpha);
						 }
						 memcpy(dest, &colortemp, sizeof(RGBA_t));
						 break;
				// default is 1
				default:
						 if ((originPatch != NULL) && (originPatch->style != AST_COPY))
							 *dest = ASTBlendPaletteIndexes(*dest, texel, originPatch->style, originPatch->alpha);
						 else
							 *dest = texel;
						 break;
			}

			dest += blockmodulo;
			yfrac -= yfracstep;
		}
		if (sourcebpp == PICDEPTH_32BPP)
			patchcol = (const column_t *)((const UINT32 *)patchcol + patchcol->length);
		else if (sourcebpp == PICDEPTH_16BPP)
			patchcol = (const column_t *)((const UINT16 *)patchcol + patchcol->length);
		else
			patchcol = (const column_t *)((const UINT8 *)patchcol + patchcol->length);
		patchcol = (const column_t *)((const UINT8 *)patchcol + 4);
	}
}

// Simplified patch caching function
// for use by sprites and other patches that are not part of a wall texture
// no alpha or flipping should be present since we do not want non-texture graphics to have them
// no offsets are used either
// -- Monster Iestyn (13/02/19)
static void HWR_DrawPatchInCache(GLMipmap_t *mipmap,
	INT32 pblockwidth, INT32 pblockheight,
	INT32 pwidth, INT32 pheight,
	const patch_t *realpatch,
	pictureformat_t format)
{
	INT32 ncols;
	fixed_t xfrac, xfracstep;
	fixed_t yfracstep, scale_y;
	const column_t *patchcol;
	UINT8 *block = mipmap->data;
	INT32 bpp;
	INT32 blockmodulo;

	if (pwidth <= 0 || pheight <= 0)
		return;

	ncols = pwidth;

	// source advance
	xfrac = 0;
	xfracstep = FRACUNIT;
	yfracstep = FRACUNIT;
	scale_y   = FRACUNIT;

	bpp = format2bpp(mipmap->format);

	if (bpp < 1 || bpp > 4)
		I_Error("HWR_DrawPatchInCache: no drawer defined for this bpp (%d)\n",bpp);

	// Initialize the texel color lookup table
	InitColorLUT(&texel_colorlookup, pLocalPalette, false);

	// NOTE: should this actually be pblockwidth*bpp?
	blockmodulo = pblockwidth*bpp;

	// Draw each column to the block cache
	for (; ncols--; block += bpp, xfrac += xfracstep)
	{
		patchcol = (const column_t *)((const UINT8 *)realpatch->columns + (realpatch->columnofs[xfrac>>FRACBITS]));

		HWR_DrawColumnInCache(patchcol, block, mipmap,
								pblockheight, blockmodulo,
								yfracstep, scale_y,
								NULL, pheight, // not that pheight is going to get used anyway...
								bpp, format);
	}
}

// This function we use for caching patches that belong to textures
static void HWR_DrawTexturePatchInCache(GLMipmap_t *mipmap,
	INT32 pblockwidth, INT32 pblockheight,
	texture_t *texture, texpatch_t *patch,
	const softwarepatch_t *realpatch,
	pictureformat_t format)
{
	INT32 x, x1, x2;
	INT32 col, ncols;
	fixed_t xfrac, xfracstep;
	fixed_t yfracstep, scale_y;
	const column_t *patchcol;
	UINT8 *block = mipmap->data;
	INT32 bpp;
	INT32 blockmodulo;
	INT32 width, height;
	// Column drawing function pointer.
	static void (*ColumnDrawerPointer)(const column_t *patchcol, UINT8 *block, GLMipmap_t *mipmap,
								INT32 pblockheight, INT32 blockmodulo,
								fixed_t yfracstep, fixed_t scale_y,
								texpatch_t *originPatch, INT32 patchheight,
								INT32 bpp, pictureformat_t format);

	if (texture->width <= 0 || texture->height <= 0)
		return;

	ColumnDrawerPointer = (patch->flip & 2) ? HWR_DrawFlippedColumnInCache : HWR_DrawColumnInCache;

	x1 = patch->originx;
	width = SHORT(realpatch->width);
	height = SHORT(realpatch->height);
	x2 = x1 + width;

	if (x1 > texture->width || x2 < 0)
		return; // patch not located within texture's x bounds, ignore

	if (patch->originy > texture->height || (patch->originy + height) < 0)
		return; // patch not located within texture's y bounds, ignore

	// patch is actually inside the texture!
	// now check if texture is partly off-screen and adjust accordingly

	// left edge
	if (x1 < 0)
		x = 0;
	else
		x = x1;

	// right edge
	if (x2 > texture->width)
		x2 = texture->width;


	col = x * pblockwidth / texture->width;
	ncols = ((x2 - x) * pblockwidth) / texture->width;

/*
	CONS_Debug(DBG_RENDER, "patch %dx%d texture %dx%d block %dx%d\n",
															width, height,
															texture->width,          texture->height,
															pblockwidth,             pblockheight);
	CONS_Debug(DBG_RENDER, "      col %d ncols %d x %d\n", col, ncols, x);
*/

	// source advance
	xfrac = 0;
	if (x1 < 0)
		xfrac = -x1<<FRACBITS;

	xfracstep = (texture->width << FRACBITS) / pblockwidth;
	yfracstep = (texture->height<< FRACBITS) / pblockheight;
	scale_y   = (pblockheight  << FRACBITS) / texture->height;

	bpp = format2bpp(mipmap->format);

	if (bpp < 1 || bpp > 4)
		I_Error("HWR_DrawTexturePatchInCache: no drawer defined for this bpp (%d)\n",bpp);

	// Initialize the texel color lookup table
	InitColorLUT(&texel_colorlookup, pLocalPalette, false);

	// NOTE: should this actually be pblockwidth*bpp?
	blockmodulo = pblockwidth*bpp;

	// Draw each column to the block cache
	for (block += col*bpp; ncols--; block += bpp, xfrac += xfracstep)
	{
		if (patch->flip & 1)
			patchcol = (const column_t *)((const UINT8 *)realpatch + LONG(realpatch->columnofs[(width-1)-(xfrac>>FRACBITS)]));
		else
			patchcol = (const column_t *)((const UINT8 *)realpatch + LONG(realpatch->columnofs[xfrac>>FRACBITS]));

		ColumnDrawerPointer(patchcol, block, mipmap,
								pblockheight, blockmodulo,
								yfracstep, scale_y,
								patch, height,
								bpp, format);
	}
}

static UINT8 *MakeBlock(GLMipmap_t *grMipmap)
{
	UINT8 *block;
	INT32 bpp, i;
	UINT16 bu16 = ((0x00 <<8) | HWR_PATCHES_CHROMAKEY_COLORINDEX);
	INT32 blocksize = (grMipmap->width * grMipmap->height);

	bpp =  format2bpp(grMipmap->format);
	block = Z_Malloc(blocksize*bpp, PU_HWRCACHE, &(grMipmap->data));

	switch (bpp)
	{
		case 1: memset(block, HWR_PATCHES_CHROMAKEY_COLORINDEX, blocksize); break;
		case 2:
				// fill background with chromakey, alpha = 0
				for (i = 0; i < blocksize; i++)
				//[segabor]
					memcpy(block+i*sizeof(UINT16), &bu16, sizeof(UINT16));
				break;
		case 4: memset(block, 0x00, blocksize*sizeof(UINT32)); break;
	}

	return block;
}

//
// Create a composite texture from patches, adapt the texture size to a power of 2
// height and width for the hardware texture cache.
//
static void HWR_GenerateTexture(INT32 texnum, GLMapTexture_t *grtex)
{
	UINT8 *block;
	texture_t *texture;
	texpatch_t *patch;
	softwarepatch_t *realpatch;
	UINT8 *pdata;
	INT32 blockwidth, blockheight, blocksize;

	INT32 i;
	boolean skyspecial = false; //poor hack for Legacy large skies..

	texture = textures[texnum];

	// hack the Legacy skies..
	if (texture->name[0] == 'S' &&
	    texture->name[1] == 'K' &&
	    texture->name[2] == 'Y' &&
	    (texture->name[4] == 0 ||
	     texture->name[5] == 0)
	   )
	{
		skyspecial = true;
		grtex->mipmap.flags = TF_WRAPXY; // don't use the chromakey for sky
	}
	else
		grtex->mipmap.flags = TF_CHROMAKEYED | TF_WRAPXY;

	grtex->mipmap.width = (UINT16)texture->width;
	grtex->mipmap.height = (UINT16)texture->height;
	grtex->mipmap.format = textureformat;

	blockwidth = texture->width;
	blockheight = texture->height;
	blocksize = (blockwidth * blockheight);
	block = MakeBlock(&grtex->mipmap);

	if (skyspecial) //Hurdler: not efficient, but better than holes in the sky (and it's done only at level loading)
	{
		INT32 j;
		RGBA_t col;

		col = V_GetColor(HWR_PATCHES_CHROMAKEY_COLORINDEX);
		for (j = 0; j < blockheight; j++)
		{
			for (i = 0; i < blockwidth; i++)
			{
				block[4*(j*blockwidth+i)+0] = col.s.red;
				block[4*(j*blockwidth+i)+1] = col.s.green;
				block[4*(j*blockwidth+i)+2] = col.s.blue;
				block[4*(j*blockwidth+i)+3] = 0xff;
			}
		}
	}

	if (texture->format == PICFMT_NONE)
		texture->format = PICFMT_PATCH;

	// Composite the columns together.
	for (i = 0, patch = texture->patches; i < texture->patchcount; i++, patch++)
	{
		pictureformat_t format = PICFMT_PATCH;
		size_t lumplength = W_LumpLengthPwad(patch->wad, patch->lump);
		pdata = W_CacheLumpNumPwad(patch->wad, patch->lump, PU_CACHE);
		realpatch = (softwarepatch_t *)pdata;

#ifndef NO_PNG_LUMPS
		if (Picture_IsLumpPNG((UINT8 *)realpatch, lumplength))
		{
#ifdef PICTURES_ALLOWDEPTH
			format = PICFMT_PATCH32;
#endif
			realpatch = (softwarepatch_t *)Picture_PNGConvert(pdata, Picture_PatchFormatTranslation(format), NULL, NULL, NULL, NULL, lumplength, NULL, 0);
		}
		else
#endif
#ifdef WALLFLATS
		if (texture->type == TEXTURETYPE_FLAT)
			realpatch = (softwarepatch_t *)Picture_Convert(PICFMT_FLAT, pdata, Picture_PatchFormatTranslation(format), 0, NULL, texture->width, texture->height, 0, 0, 0);
		else
#endif
		{
			(void)lumplength;
		}

		HWR_DrawTexturePatchInCache(&grtex->mipmap, blockwidth, blockheight, texture, patch, realpatch, format);
	}
	//Hurdler: not efficient at all but I don't remember exactly how HWR_DrawPatchInCache works :(
	if (format2bpp(grtex->mipmap.format)==4)
	{
		for (i = 3; i < blocksize*4; i += 4) // blocksize*4 because blocksize doesn't include the bpp
		{
			if (block[i] == 0)
			{
				grtex->mipmap.flags |= TF_TRANSPARENT;
				break;
			}
		}
	}

	grtex->scaleX = 1.0f/(texture->width*FRACUNIT);
	grtex->scaleY = 1.0f/(texture->height*FRACUNIT);
}

// patch may be NULL if grMipmap has been initialised already and makebitmap is false
void HWR_MakePatch (patch_t *patch, GLPatch_t *grPatch, GLMipmap_t *grMipmap, boolean makebitmap)
{
	// don't do it twice (like a cache)
	if (grMipmap->width == 0)
	{
		grMipmap->width = grMipmap->height = 1;
		while (grMipmap->width < patch->width) grMipmap->width <<= 1;
		while (grMipmap->height < patch->height) grMipmap->height <<= 1;

		// no wrap around, no chroma key
		grMipmap->flags = 0;

		// setup the texture info
		grMipmap->format = patchformat;

		grPatch->max_s = (float)patch->width / (float)grMipmap->width;
		grPatch->max_t = (float)patch->height / (float)grMipmap->height;
	}

	Z_Free(grMipmap->data);
	grMipmap->data = NULL;

	if (makebitmap)
	{
		patch_t *source = patch;

#ifdef PICTURES_ALLOWDEPTH
		patch_t *tc = Patch_GetTruecolor(source);
		if (tc)
		{
			source = tc;
			grPatch->picfmt = PICFMT_PATCH32;
		}
#endif

		MakeBlock(grMipmap);

		HWR_DrawPatchInCache(grMipmap,
			grMipmap->width, grMipmap->height,
			source->width, source->height,
			source, grPatch->picfmt);
	}
}


// =================================================
//             CACHING HANDLING
// =================================================

static size_t gl_numtextures = 0; // Texture count
static GLMapTexture_t *gl_textures; // For all textures
static GLMapTexture_t *gl_flats; // For all (texture) flats, as normal flats don't need to be cached
boolean gl_maptexturesloaded = false;

void HWR_FreeTexture(patch_t *patch)
{
	if (!patch)
		return;

	if (patch->hardware)
	{
		GLPatch_t *grPatch = patch->hardware;

		HWR_FreeTextureColormaps(patch);

		if (grPatch->mipmap)
		{
			if (vid.glstate == VID_GL_LIBRARY_LOADED)
				HWD.pfnDeleteTexture(grPatch->mipmap);
			if (grPatch->mipmap->data)
				Z_Free(grPatch->mipmap->data);
			Z_Free(grPatch->mipmap);
		}

		Z_Free(patch->hardware);
	}

	patch->hardware = NULL;
}

// Called by HWR_FreePatchCache.
void HWR_FreeTextureColormaps(patch_t *patch)
{
	GLPatch_t *pat;

	// The patch must be valid, obviously
	if (!patch)
		return;

	pat = patch->hardware;
	if (!pat)
		return;

	// The mipmap must be valid, obviously
	while (pat->mipmap)
	{
		// Confusing at first, but pat->mipmap->nextcolormap
		// at the beginning of the loop is the first colormap
		// from the linked list of colormaps.
		GLMipmap_t *next = NULL;

		// No mipmap in this patch, break out of the loop.
		if (!pat->mipmap)
			break;

		// No colormap mipmap either.
		if (!pat->mipmap->nextcolormap)
			break;

		// Set the first colormap to the one that comes after it.
		next = pat->mipmap->nextcolormap;
		pat->mipmap->nextcolormap = next->nextcolormap;

		// Free image data from memory.
		if (next->data)
			Z_Free(next->data);
		next->data = NULL;
		HWD.pfnDeleteTexture(next);

		// Free the old colormap mipmap from memory.
		free(next);
	}
}

static void HWR_FreePatchCache(boolean freeall)
{
	INT32 i;

	for (i = 0; i < numwadfiles; i++)
	{
		INT32 j = 0;
		for (; j < wadfiles[i]->numlumps; j++)
			(freeall ? HWR_FreeTexture : HWR_FreeTextureColormaps)(wadfiles[i]->patchcache[j]);
	}
}

void HWR_ClearAllTextures(void)
{
	HWR_FreeMapTextures();

	// free references to the textures
	HWD.pfnClearMipMapCache();

	// Alam: free the Z_Blocks before freeing it's users
	HWR_FreePatchCache(true);
}

// free all patch colormaps after each level: must be done after ClearMipMapCache!
void HWR_FreeColormapCache(void)
{
	HWR_FreePatchCache(false);
}

void HWR_InitMapTextures(void)
{
	gl_textures = NULL;
	gl_flats = NULL;
	gl_maptexturesloaded = false;
}

static void FreeMapTexture(GLMapTexture_t *tex)
{
	HWD.pfnDeleteTexture(&tex->mipmap);
	if (tex->mipmap.data)
		Z_Free(tex->mipmap.data);
}

void HWR_FreeMapTextures(void)
{
	size_t i;

	for (i = 0; i < gl_numtextures; i++)
	{
		FreeMapTexture(&gl_textures[i]);
		FreeMapTexture(&gl_flats[i]);
	}

	// now the heap don't have any 'user' pointing to our
	// texturecache info, we can free it
	if (gl_textures)
		free(gl_textures);
	if (gl_flats)
		free(gl_flats);
	gl_textures = NULL;
	gl_flats = NULL;
	gl_numtextures = 0;
	gl_maptexturesloaded = false;
}

void HWR_LoadMapTextures(size_t pnumtextures)
{
	// we must free it since numtextures changed
	HWR_FreeMapTextures();

	// Why not Z_Malloc?
	gl_numtextures = pnumtextures;
	gl_textures = calloc(gl_numtextures, sizeof(*gl_textures));
	gl_flats = calloc(gl_numtextures, sizeof(*gl_flats));

	// Doesn't tell you which it _is_, but hopefully
	// should never ever happen (right?!)
	if ((gl_textures == NULL) || (gl_flats == NULL))
		I_Error("HWR_LoadMapTextures: ran out of memory for OpenGL textures. Sad!");

	gl_maptexturesloaded = true;
}

void HWR_SetPalette(RGBA_t *palette)
{
	HWD.pfnSetPalette(palette);

	// hardware driver will flush there own cache if cache is non paletized
	// now flush data texture cache so 32 bit texture are recomputed
	if (patchformat == GL_TEXFMT_RGBA || textureformat == GL_TEXFMT_RGBA)
	{
		Z_FreeTag(PU_HWRCACHE);
		Z_FreeTag(PU_HWRCACHE_UNLOCKED);
	}
}

// --------------------------------------------------------------------------
// Make sure texture is downloaded and set it as the source
// --------------------------------------------------------------------------
GLMapTexture_t *HWR_GetTexture(INT32 tex)
{
	GLMapTexture_t *grtex;
#ifdef PARANOIA
	if ((unsigned)tex >= gl_numtextures)
		I_Error("HWR_GetTexture: tex >= numtextures\n");
#endif

	// Every texture in memory, stored in the
	// hardware renderer's bit depth format. Wow!
	grtex = &gl_textures[tex];

	// Generate texture if missing from the cache
	if (!grtex->mipmap.data && !grtex->mipmap.downloaded)
		HWR_GenerateTexture(tex, grtex);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grtex->mipmap.downloaded)
		HWD.pfnSetTexture(&grtex->mipmap);
	HWR_SetCurrentTexture(&grtex->mipmap);

	// The system-memory data can be purged now.
	Z_ChangeTag(grtex->mipmap.data, PU_HWRCACHE_UNLOCKED);

	return grtex;
}

static void HWR_CacheFlat(GLMipmap_t *grMipmap, lumpnum_t flatlumpnum)
{
	size_t size, pflatsize;

	// setup the texture info
	grMipmap->format = GL_TEXFMT_P_8;
	grMipmap->flags = TF_WRAPXY|TF_CHROMAKEYED;

	size = W_LumpLength(flatlumpnum);

	switch (size)
	{
		case 4194304: // 2048x2048 lump
			pflatsize = 2048;
			break;
		case 1048576: // 1024x1024 lump
			pflatsize = 1024;
			break;
		case 262144:// 512x512 lump
			pflatsize = 512;
			break;
		case 65536: // 256x256 lump
			pflatsize = 256;
			break;
		case 16384: // 128x128 lump
			pflatsize = 128;
			break;
		case 1024: // 32x32 lump
			pflatsize = 32;
			break;
		default: // 64x64 lump
			pflatsize = 64;
			break;
	}

	grMipmap->width  = (UINT16)pflatsize;
	grMipmap->height = (UINT16)pflatsize;

	// the flat raw data needn't be converted with palettized textures
	W_ReadLump(flatlumpnum, Z_Malloc(W_LumpLength(flatlumpnum),
		PU_HWRCACHE, &grMipmap->data));
}

static void HWR_CacheTextureAsFlat(GLMipmap_t *grMipmap, INT32 texturenum)
{
	UINT8 *flat;
	size_t size;
#ifdef PICTURES_ALLOWDEPTH
	GLMapTexture_t *grtex;
#else
	UINT8 *converted;
#endif

#ifdef PICTURES_ALLOWDEPTH
	// Texture in hardware cache
	grtex = &gl_textures[texturenum];

	// Generate texture if missing from the cache
	// The texture CAN be downloaded but have no data,
	// which is perfectly fine when the GPU has it,
	// but not for this kind of conversion.
	if (!grtex->mipmap.data)
		HWR_GenerateTexture(texturenum, grtex);
#endif

	// setup the texture info
#ifdef PICTURES_ALLOWDEPTH
	grMipmap->format = textureformat;
#else
	grMipmap->format = GL_TEXFMT_P_8;
#endif

	grMipmap->flags = TF_WRAPXY|TF_CHROMAKEYED;
	grMipmap->width = (UINT16)textures[texturenum]->width;
	grMipmap->height = (UINT16)textures[texturenum]->height;

#ifdef PICTURES_ALLOWDEPTH
	size = (grMipmap->width * grMipmap->height) * format2bpp(textureformat);
	flat = Z_Malloc(size, PU_HWRCACHE, &grMipmap->data);
	M_Memcpy(flat, grtex->mipmap.data, size);
#else
	size = (grMipmap->width * grMipmap->height);
	flat = Z_Malloc(size, PU_HWRCACHE, &grMipmap->data);
	converted = (UINT8 *)Picture_TextureToFlat(texturenum);
	M_Memcpy(flat, converted, size);
	Z_Free(converted);
#endif
}

// Download a Doom 'flat' to the hardware cache and make it ready for use
void HWR_GetRawFlat(lumpnum_t flatlumpnum)
{
	GLMipmap_t *grmip;
	patch_t *patch;

	if (flatlumpnum == LUMPERROR)
		return;

	patch = HWR_GetCachedGLPatch(flatlumpnum);
	grmip = ((GLPatch_t *)Patch_AllocateHardwarePatch(patch))->mipmap;
	if (!grmip->downloaded && !grmip->data)
		HWR_CacheFlat(grmip, flatlumpnum);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grmip->downloaded)
		HWD.pfnSetTexture(grmip);
	HWR_SetCurrentTexture(grmip);

	// The system-memory data can be purged now.
	Z_ChangeTag(grmip->data, PU_HWRCACHE_UNLOCKED);
}

void HWR_GetLevelFlat(levelflat_t *levelflat)
{
	// Who knows?
	if (levelflat == NULL)
		return;

	if (levelflat->type == LEVELFLAT_FLAT)
		HWR_GetRawFlat(levelflat->u.flat.lumpnum);
	else if (levelflat->type == LEVELFLAT_TEXTURE)
	{
		GLMapTexture_t *grtex;
		INT32 texturenum = levelflat->u.texture.num;
#ifdef PARANOIA
		if ((unsigned)texturenum >= gl_numtextures)
			I_Error("HWR_GetLevelFlat: texturenum >= numtextures");
#endif

		// Who knows?
		if (texturenum == 0 || texturenum == -1)
			return;

		// Every texture in memory, stored as a 8-bit flat. Wow!
		grtex = &gl_flats[texturenum];

		// Generate flat if missing from the cache
		if (!grtex->mipmap.data && !grtex->mipmap.downloaded)
			HWR_CacheTextureAsFlat(&grtex->mipmap, texturenum);

		// If hardware does not have the texture, then call pfnSetTexture to upload it
		if (!grtex->mipmap.downloaded)
			HWD.pfnSetTexture(&grtex->mipmap);
		HWR_SetCurrentTexture(&grtex->mipmap);

		// The system-memory data can be purged now.
		Z_ChangeTag(grtex->mipmap.data, PU_HWRCACHE_UNLOCKED);
	}
	else if (levelflat->type == LEVELFLAT_PATCH)
	{
		patch_t *patch = W_CachePatchNum(levelflat->u.flat.lumpnum, PU_CACHE);
		levelflat->width = (UINT16)(patch->width);
		levelflat->height = (UINT16)(patch->height);
		HWR_GetPatch(patch);
	}
#ifndef NO_PNG_LUMPS
	else if (levelflat->type == LEVELFLAT_PNG)
	{
		INT32 pngwidth = 0, pngheight = 0;
		GLMipmap_t *mipmap = levelflat->mipmap;
		UINT8 *flat;
		size_t size;
		pictureformat_t format = PICFMT_FLAT32;
		INT32 fmtbpp = Picture_FormatBPP(format);

		// Cache the picture.
		if (!levelflat->mippic)
		{
			void *pic = Picture_PNGConvert(W_CacheLumpNum(levelflat->u.flat.lumpnum, PU_CACHE), format, &pngwidth, &pngheight, NULL, NULL, W_LumpLength(levelflat->u.flat.lumpnum), NULL, 0);

			Z_ChangeTag(pic, PU_LEVEL);
			Z_SetUser(pic, &levelflat->mippic);

			levelflat->width = (UINT16)pngwidth;
			levelflat->height = (UINT16)pngheight;
		}

		// Make the mipmap.
		if (mipmap == NULL)
		{
			mipmap = Z_Calloc(sizeof(GLMipmap_t), PU_STATIC, NULL);
			mipmap->format = (fmtbpp == PICDEPTH_32BPP ? GL_TEXFMT_RGBA : GL_TEXFMT_P_8);
			mipmap->flags = TF_WRAPXY|TF_CHROMAKEYED;
			levelflat->mipmap = mipmap;
		}

		if (!mipmap->data && !mipmap->downloaded)
		{
			if (levelflat->picture == NULL)
				I_Error("HWR_GetLevelFlat: levelflat->picture == NULL");
			mipmap->width = levelflat->width;
			mipmap->height = levelflat->height;
			size = (mipmap->width * mipmap->height) * (fmtbpp / 8);
			flat = Z_Malloc(size, PU_LEVEL, &mipmap->data);
			if (levelflat->mippic == NULL)
				I_Error("HWR_GetLevelFlat: levelflat->mippic == NULL");
			M_Memcpy(flat, levelflat->mippic, size);
		}

		// Tell the hardware driver to bind the current texture to the flat's mipmap
		HWR_SetCurrentTexture(mipmap);
	}
#endif
	else // set no texture
		HWR_SetCurrentTexture(NULL);
}

// --------------------+
// HWR_LoadPatchMipmap : Generates a patch into a mipmap, usually the mipmap inside the patch itself
// --------------------+
static void HWR_LoadPatchMipmap(patch_t *patch, GLMipmap_t *grMipmap)
{
	GLPatch_t *grPatch = patch->hardware;
	if (!grMipmap->downloaded && !grMipmap->data)
		HWR_MakePatch(patch, grPatch, grMipmap, true);

	// If hardware does not have the texture, then call pfnSetTexture to upload it
	if (!grMipmap->downloaded)
		HWD.pfnSetTexture(grMipmap);
	HWR_SetCurrentTexture(grMipmap);

	// The system-memory data can be purged now.
	Z_ChangeTag(grMipmap->data, PU_HWRCACHE_UNLOCKED);
}

// -----------------+
// HWR_GetPatch     : Download a patch to the hardware cache and make it ready for use
// -----------------+
void HWR_GetPatch(patch_t *patch)
{
	if (!patch->hardware)
		Patch_CreateGL(patch);
	HWR_LoadPatchMipmap(patch, ((GLPatch_t *)patch->hardware)->mipmap);
}

// -------------------+
// HWR_GetMappedPatch : Same as HWR_GetPatch for sprite color
// -------------------+
void HWR_GetMappedPatch(patch_t *patch, const UINT8 *colormap)
{
	GLPatch_t *grPatch;
	GLMipmap_t *grMipmap, *newMipmap;

	if (!patch->hardware)
		Patch_CreateGL(patch);
	grPatch = patch->hardware;

	if (colormap == colormaps || colormap == NULL)
	{
		// Load the default (green) color in hardware cache
		HWR_GetPatch(patch);
		return;
	}

	// search for the mimmap
	// skip the first (no colormap translated)
	for (grMipmap = grPatch->mipmap; grMipmap->nextcolormap; )
	{
		grMipmap = grMipmap->nextcolormap;
		if (grMipmap->colormap == colormap)
		{
			HWR_LoadPatchMipmap(patch, grMipmap);
			return;
		}
	}
	// not found, create it!
	// If we are here, the sprite with the current colormap is not already in hardware memory

	//BP: WARNING: don't free it manually without clearing the cache of harware renderer
	//              (it have a liste of mipmap)
	//    this malloc is cleared in HWR_FreeColormapCache
	//    (...) unfortunately z_malloc fragment alot the memory :(so malloc is better
	newMipmap = calloc(1, sizeof (*newMipmap));
	if (newMipmap == NULL)
		I_Error("%s: Out of memory", "HWR_GetMappedPatch");
	grMipmap->nextcolormap = newMipmap;

	newMipmap->colormap = colormap;
	HWR_LoadPatchMipmap(patch, newMipmap);
}

void HWR_UnlockCachedPatch(GLPatch_t *gpatch)
{
	if (!gpatch)
		return;

	Z_ChangeTag(gpatch->mipmap->data, PU_HWRCACHE_UNLOCKED);
	Z_ChangeTag(gpatch, PU_HWRPATCHINFO_UNLOCKED);
}

static const INT32 picmode2GR[] =
{
	GL_TEXFMT_P_8,                // PALETTE
	0,                            // INTENSITY          (unsupported yet)
	GL_TEXFMT_ALPHA_INTENSITY_88, // INTENSITY_ALPHA    (corona use this)
	0,                            // RGB24              (unsupported yet)
	GL_TEXFMT_RGBA,               // RGBA32             (opengl only)
};

static void HWR_DrawPicInCache(UINT8 *block, INT32 pblockwidth, INT32 pblockheight,
	INT32 blockmodulo, pic_t *pic, INT32 bpp)
{
	INT32 i,j;
	fixed_t posx, posy, stepx, stepy;
	UINT8 *dest, *src, texel;
	UINT16 texelu16;
	INT32 picbpp;
	RGBA_t col;

	stepy = ((INT32)SHORT(pic->height)<<FRACBITS)/pblockheight;
	stepx = ((INT32)SHORT(pic->width)<<FRACBITS)/pblockwidth;
	picbpp = format2bpp(picmode2GR[pic->mode]);
	posy = 0;
	for (j = 0; j < pblockheight; j++)
	{
		posx = 0;
		dest = &block[j*blockmodulo];
		src = &pic->data[(posy>>FRACBITS)*SHORT(pic->width)*picbpp];
		for (i = 0; i < pblockwidth;i++)
		{
			switch (pic->mode)
			{ // source bpp
				case PALETTE :
					texel = src[(posx+FRACUNIT/2)>>FRACBITS];
					switch (bpp)
					{ // destination bpp
						case 1 :
							*dest++ = texel; break;
						case 2 :
							texelu16 = (UINT16)(texel | 0xff00);
							memcpy(dest, &texelu16, sizeof(UINT16));
							dest += sizeof(UINT16);
							break;
						case 3 :
							col = V_GetColor(texel);
							memcpy(dest, &col, sizeof(RGBA_t)-sizeof(UINT8));
							dest += sizeof(RGBA_t)-sizeof(UINT8);
							break;
						case 4 :
							memcpy(dest, &V_GetColor(texel), sizeof(RGBA_t));
							dest += sizeof(RGBA_t);
							break;
					}
					break;
				case INTENSITY :
					*dest++ = src[(posx+FRACUNIT/2)>>FRACBITS];
					break;
				case INTENSITY_ALPHA : // assume dest bpp = 2
					memcpy(dest, src + ((posx+FRACUNIT/2)>>FRACBITS)*sizeof(UINT16), sizeof(UINT16));
					dest += sizeof(UINT16);
					break;
				case RGB24 :
					break;  // not supported yet
				case RGBA32 : // assume dest bpp = 4
					dest += sizeof(UINT32);
					memcpy(dest, src + ((posx+FRACUNIT/2)>>FRACBITS)*sizeof(UINT32), sizeof(UINT32));
					break;
			}
			posx += stepx;
		}
		posy += stepy;
	}
}

// -----------------+
// HWR_GetPic       : Download a Doom pic (raw row encoded with no 'holes')
// Returns          :
// -----------------+
patch_t *HWR_GetPic(lumpnum_t lumpnum)
{
	patch_t *patch = HWR_GetCachedGLPatch(lumpnum);
	GLPatch_t *grPatch = (GLPatch_t *)(patch->hardware);

	if (!grPatch->mipmap->downloaded && !grPatch->mipmap->data)
	{
		pic_t *pic;
		UINT8 *block;
		size_t len;

		pic = W_CacheLumpNum(lumpnum, PU_CACHE);
		patch->width = SHORT(pic->width);
		patch->height = SHORT(pic->height);
		len = W_LumpLength(lumpnum) - sizeof (pic_t);

		grPatch->mipmap->width = (UINT16)patch->width;
		grPatch->mipmap->height = (UINT16)patch->height;

		if (pic->mode == PALETTE)
			grPatch->mipmap->format = textureformat; // can be set by driver
		else
			grPatch->mipmap->format = picmode2GR[pic->mode];

		Z_Free(grPatch->mipmap->data);

		// allocate block
		block = MakeBlock(grPatch->mipmap);

		if (patch->width  == SHORT(pic->width) &&
			patch->height == SHORT(pic->height) &&
			format2bpp(grPatch->mipmap->format) == format2bpp(picmode2GR[pic->mode]))
		{
			// no conversion needed
			M_Memcpy(grPatch->mipmap->data, pic->data,len);
		}
		else
			HWR_DrawPicInCache(block, SHORT(pic->width), SHORT(pic->height),
			                   SHORT(pic->width)*format2bpp(grPatch->mipmap->format),
			                   pic,
			                   format2bpp(grPatch->mipmap->format));

		Z_Unlock(pic);
		Z_ChangeTag(block, PU_HWRCACHE_UNLOCKED);

		grPatch->mipmap->flags = 0;
		grPatch->max_s = grPatch->max_t = 1.0f;
	}
	HWD.pfnSetTexture(grPatch->mipmap);
	//CONS_Debug(DBG_RENDER, "picloaded at %x as texture %d\n",grPatch->mipmap->data, grPatch->mipmap->downloaded);

	return patch;
}

patch_t *HWR_GetCachedGLPatchPwad(UINT16 wadnum, UINT16 lumpnum)
{
	lumpcache_t *lumpcache = wadfiles[wadnum]->patchcache;
	if (!lumpcache[lumpnum])
	{
		void *ptr = Z_Calloc(sizeof(patch_t), PU_PATCH, &lumpcache[lumpnum]);
		Patch_Create(NULL, 0, ptr);
		Patch_AllocateHardwarePatch(ptr);
	}
	return (patch_t *)(lumpcache[lumpnum]);
}

patch_t *HWR_GetCachedGLPatch(lumpnum_t lumpnum)
{
	return HWR_GetCachedGLPatchPwad(WADFILENUM(lumpnum),LUMPNUM(lumpnum));
}

// Need to do this because they aren't powers of 2
static void HWR_DrawFadeMaskInCache(GLMipmap_t *mipmap, INT32 pblockwidth, INT32 pblockheight,
	lumpnum_t fademasklumpnum, UINT16 fmwidth, UINT16 fmheight)
{
	INT32 i,j;
	fixed_t posx, posy, stepx, stepy;
	UINT8 *block = mipmap->data; // places the data directly into here
	UINT8 *flat;
	UINT8 *dest, *src, texel;
	RGBA_t col;

	// Place the flats data into flat
	W_ReadLump(fademasklumpnum, Z_Malloc(W_LumpLength(fademasklumpnum),
		PU_HWRCACHE, &flat));

	stepy = ((INT32)SHORT(fmheight)<<FRACBITS)/pblockheight;
	stepx = ((INT32)SHORT(fmwidth)<<FRACBITS)/pblockwidth;
	posy = 0;
	for (j = 0; j < pblockheight; j++)
	{
		posx = 0;
		dest = &block[j*(mipmap->width)]; // 1bpp
		src = &flat[(posy>>FRACBITS)*SHORT(fmwidth)];
		for (i = 0; i < pblockwidth;i++)
		{
			// fademask bpp is always 1, and is used just for alpha
			texel = src[(posx)>>FRACBITS];
			col = V_GetColor(texel);
			*dest = col.s.red; // take the red level of the colour and use it for alpha, as fademasks do

			dest++;
			posx += stepx;
		}
		posy += stepy;
	}

	Z_Free(flat);
}

static void HWR_CacheFadeMask(GLMipmap_t *grMipmap, lumpnum_t fademasklumpnum)
{
	size_t size;
	UINT16 fmheight = 0, fmwidth = 0;

	// setup the texture info
	grMipmap->format = GL_TEXFMT_ALPHA_8; // put the correct alpha levels straight in so I don't need to convert it later
	grMipmap->flags = 0;

	size = W_LumpLength(fademasklumpnum);

	switch (size)
	{
		// None of these are powers of 2, so I'll need to do what is done for textures and make them powers of 2 before they can be used
		case 256000: // 640x400
			fmwidth = 640;
			fmheight = 400;
			break;
		case 64000: // 320x200
			fmwidth = 320;
			fmheight = 200;
			break;
		case 16000: // 160x100
			fmwidth = 160;
			fmheight = 100;
			break;
		case 4000: // 80x50 (minimum)
			fmwidth = 80;
			fmheight = 50;
			break;
		default: // Bad lump
			CONS_Alert(CONS_WARNING, "Fade mask lump of incorrect size, ignored\n"); // I should avoid this by checking the lumpnum in HWR_RunWipe
			break;
	}

	// Thankfully, this will still work for this scenario
	grMipmap->width  = fmwidth;
	grMipmap->height = fmheight;

	MakeBlock(grMipmap);

	HWR_DrawFadeMaskInCache(grMipmap, fmwidth, fmheight, fademasklumpnum, fmwidth, fmheight);

	// I DO need to convert this because it isn't power of 2 and we need the alpha
}


void HWR_GetFadeMask(lumpnum_t fademasklumpnum)
{
	patch_t *patch = HWR_GetCachedGLPatch(fademasklumpnum);
	GLMipmap_t *grmip = ((GLPatch_t *)Patch_AllocateHardwarePatch(patch))->mipmap;
	if (!grmip->downloaded && !grmip->data)
		HWR_CacheFadeMask(grmip, fademasklumpnum);

	HWD.pfnSetTexture(grmip);

	// The system-memory data can be purged now.
	Z_ChangeTag(grmip->data, PU_HWRCACHE_UNLOCKED);
}

#endif //HWRENDER
