/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_ui.c - 2D HUD, Menus & UI Compositing Subsystem Implementation
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_ui.h"

ui_state_t g_ui;

// Internal 2D TriAPI accumulator
static struct
{
	int      primitive;
	int      vertexCount;
	float    currentUV[2];
	uint32_t currentColor;
	float    tempVerts[64][2];
	float    tempUVs[64][2];
	uint32_t tempColors[64];
} s_tri2D;

/*
 * R_UI_Init - Initialize UI quad buffers and clear texture registry
 */
void R_UI_Init( void )
{
	memset( &g_ui, 0, sizeof( g_ui ) );
	g_ui.currentColor     = 0xFFFFFFFF; // Opaque white default
	g_ui.currentTextureId = 0;          // Default white texture
	g_ui.currentFlags     = REMIXAPI_UI_NATIVE_LINEAR;
	g_ui.initialized      = true;

	gEngfuncs.Con_DPrintf( "ref_openremix: 2D UI Compositor initialized\n" );
}

/*
 * R_UI_Shutdown - Release all registered UI textures from OpenRemix
 */
void R_UI_Shutdown( void )
{
	if( !g_ui.initialized )
		return;

	if( g_remix.FreeUITexture )
	{
		for( int i = 1; i < MAX_UI_TEXTURES; i++ )
		{
			if( g_ui.textures[i].registered )
			{
				g_remix.FreeUITexture( (remixapi_UITextureHandle)i );
				g_ui.textures[i].registered = false;
			}
		}
	}

	memset( &g_ui, 0, sizeof( g_ui ) );
	gEngfuncs.Con_DPrintf( "ref_openremix: 2D UI Compositor shutdown complete\n" );
}

/*
 * R_UI_BeginFrame - Reset vertex and index counters for the new frame
 */
void R_UI_BeginFrame( void )
{
	g_ui.vertexCount      = 0;
	g_ui.indexCount       = 0;
	g_ui.commandCount     = 0;
	g_ui.currentColor     = 0xFFFFFFFF;
	g_ui.currentTextureId = 0;
	g_ui.currentFlags     = REMIXAPI_UI_NATIVE_LINEAR;
}

/*
 * R_UI_Clear - Clear UI buffers immediately
 */
void R_UI_Clear( void )
{
	g_ui.vertexCount  = 0;
	g_ui.indexCount   = 0;
	g_ui.commandCount = 0;
}

/*
 * R_UI_Set2DMode - Switch renderer into 2D mode
 */
void R_UI_Set2DMode( qboolean enable )
{
	tr.is2DMode = enable;
	if( enable )
	{
		tr.offset2d[0] = 0.0f;
		tr.offset2d[1] = 0.0f;
	}
}

/*
 * R_UI_Set2DOffset - Set coordinate translation for nested menus / VGUI panels
 */
void R_UI_Set2DOffset( float x, float y )
{
	tr.offset2d[0] = x;
	tr.offset2d[1] = y;
}

/*
 * R_UI_RegisterTexture - Register an RGBA/BGRA texture buffer with OpenRemix UI
 */
int R_UI_RegisterTexture( int texnum, const char *name, int width, int height, const void *pixels, remixapi_Format format )
{
	if( texnum <= 0 || texnum >= MAX_UI_TEXTURES || !pixels || width <= 0 || height <= 0 )
		return 0;

	if( !g_remix.RegisterUITexture )
		return texnum;

	// OpenRemix requires 32-bit RGBA or BGRA formats
	if( format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && format != REMIXAPI_FORMAT_R8G8B8A8_SRGB &&
	    format != REMIXAPI_FORMAT_B8G8R8A8_UNORM && format != REMIXAPI_FORMAT_B8G8R8A8_SRGB )
	{
		format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
	}

	uint64_t dataSize = (uint64_t)width * (uint64_t)height * 4ULL;
	remixapi_ErrorCode err = g_remix.RegisterUITexture(
		(remixapi_UITextureHandle)texnum,
		(uint32_t)width,
		(uint32_t)height,
		format,
		pixels,
		dataSize
	);

	if( err == REMIXAPI_ERROR_CODE_SUCCESS )
	{
		ui_texture_entry_t *entry = &g_ui.textures[texnum];
		entry->registered = true;
		entry->width      = (uint32_t)width;
		entry->height     = (uint32_t)height;
		entry->format     = format;
		if( name )
			Q_strncpy( entry->name, name, sizeof( entry->name ) );
	}
	else
	{
		gEngfuncs.Con_Reportf( S_WARN "ref_openremix: RegisterUITexture failed for %s (id %d, err %d)\n",
		                      name ? name : "unnamed", texnum, (int)err );
	}

	return texnum;
}

/*
 * R_UI_UpdateTextureRegion - Update a sub-region of an existing registered UI texture
 */
void R_UI_UpdateTextureRegion( int texnum, int x, int y, int width, int height, const void *pixels, remixapi_Format format, int rowPitch )
{
	if( texnum <= 0 || texnum >= MAX_UI_TEXTURES || !pixels || width <= 0 || height <= 0 )
		return;

	if( !g_ui.textures[texnum].registered )
	{
		R_UI_RegisterTexture( texnum, NULL, width, height, pixels, format );
		return;
	}

	if( g_remix.RegisterUITexture )
	{
		// Fallback / standard path: update texture with OpenRemix
		if( x == 0 && y == 0 && width == (int)g_ui.textures[texnum].width && height == (int)g_ui.textures[texnum].height )
		{
			g_remix.RegisterUITexture(
				(remixapi_UITextureHandle)texnum,
				(uint32_t)width,
				(uint32_t)height,
				format,
				pixels,
				(uint64_t)width * (uint64_t)height * 4ULL
			);
		}
	}
	(void)rowPitch;
}

/*
 * R_UI_FreeTexture - Free a texture from OpenRemix UI
 */
void R_UI_FreeTexture( int texnum )
{
	if( texnum <= 0 || texnum >= MAX_UI_TEXTURES )
		return;

	if( g_ui.textures[texnum].registered && g_remix.FreeUITexture )
	{
		g_remix.FreeUITexture( (remixapi_UITextureHandle)texnum );
		g_ui.textures[texnum].registered = false;
	}
}

/*
 * R_UI_GetTextureDimensions - Query texture width and height
 */
qboolean R_UI_GetTextureDimensions( int texnum, int *width, int *height )
{
	if( texnum <= 0 || texnum >= MAX_UI_TEXTURES || !g_ui.textures[texnum].registered )
	{
		if( width )  *width  = 0;
		if( height ) *height = 0;
		return false;
	}

	if( width )  *width  = (int)g_ui.textures[texnum].width;
	if( height ) *height = (int)g_ui.textures[texnum].height;
	return true;
}

/*
 * Helper: Append Quad to UI Buffers with Command Batching
 */
static void R_UI_EmitQuad( float x0, float y0, float s0, float t0,
                           float x1, float y1, float s1, float t1,
                           float x2, float y2, float s2, float t2,
                           float x3, float y3, float s3, float t3,
                           remixapi_UITextureHandle textureId,
                           uint32_t color,
                           uint32_t flags )
{
	if( !isfinite( x0 ) || !isfinite( y0 ) || !isfinite( s0 ) || !isfinite( t0 ) ||
	    !isfinite( x1 ) || !isfinite( y1 ) || !isfinite( s1 ) || !isfinite( t1 ) ||
	    !isfinite( x2 ) || !isfinite( y2 ) || !isfinite( s2 ) || !isfinite( t2 ) ||
	    !isfinite( x3 ) || !isfinite( y3 ) || !isfinite( s3 ) || !isfinite( t3 ) )
		return;

	if( textureId != 0 && ( (size_t)textureId >= MAX_UI_TEXTURES || !g_ui.textures[(size_t)textureId].registered ) )
		textureId = 0;

	if( g_ui.vertexCount + 4 > MAX_UI_VERTICES || g_ui.indexCount + 6 > MAX_UI_INDICES )
		return;

	uint32_t baseVertex = g_ui.vertexCount;
	uint32_t baseIndex  = g_ui.indexCount;

	// Apply 2D coordinate offset
	float offX = tr.offset2d[0];
	float offY = tr.offset2d[1];

	// Vertex 0: Top-Left
	g_ui.vertices[baseVertex + 0].x     = x0 + offX;
	g_ui.vertices[baseVertex + 0].y     = y0 + offY;
	g_ui.vertices[baseVertex + 0].z     = 0.0f;
	g_ui.vertices[baseVertex + 0].u     = s0;
	g_ui.vertices[baseVertex + 0].v     = t0;
	g_ui.vertices[baseVertex + 0].color = color;

	// Vertex 1: Top-Right
	g_ui.vertices[baseVertex + 1].x     = x1 + offX;
	g_ui.vertices[baseVertex + 1].y     = y1 + offY;
	g_ui.vertices[baseVertex + 1].z     = 0.0f;
	g_ui.vertices[baseVertex + 1].u     = s1;
	g_ui.vertices[baseVertex + 1].v     = t1;
	g_ui.vertices[baseVertex + 1].color = color;

	// Vertex 2: Bottom-Right
	g_ui.vertices[baseVertex + 2].x     = x2 + offX;
	g_ui.vertices[baseVertex + 2].y     = y2 + offY;
	g_ui.vertices[baseVertex + 2].z     = 0.0f;
	g_ui.vertices[baseVertex + 2].u     = s2;
	g_ui.vertices[baseVertex + 2].v     = t2;
	g_ui.vertices[baseVertex + 2].color = color;

	// Vertex 3: Bottom-Left
	g_ui.vertices[baseVertex + 3].x     = x3 + offX;
	g_ui.vertices[baseVertex + 3].y     = y3 + offY;
	g_ui.vertices[baseVertex + 3].z     = 0.0f;
	g_ui.vertices[baseVertex + 3].u     = s3;
	g_ui.vertices[baseVertex + 3].v     = t3;
	g_ui.vertices[baseVertex + 3].color = color;

	// Two triangles: (0, 1, 2) and (0, 2, 3)
	g_ui.indices[baseIndex + 0] = baseVertex + 0;
	g_ui.indices[baseIndex + 1] = baseVertex + 1;
	g_ui.indices[baseIndex + 2] = baseVertex + 2;
	g_ui.indices[baseIndex + 3] = baseVertex + 0;
	g_ui.indices[baseIndex + 4] = baseVertex + 2;
	g_ui.indices[baseIndex + 5] = baseVertex + 3;

	g_ui.vertexCount += 4;
	g_ui.indexCount  += 6;

	// Check if this quad can be batched with the previous command
	if( g_ui.commandCount > 0 )
	{
		remixapi_UIDrawCommand *prevCmd = &g_ui.commands[g_ui.commandCount - 1];
		if( prevCmd->textureId == textureId && prevCmd->flags == flags && prevCmd->vertexOffset == 0 )
		{
			prevCmd->indexCount += 6;
			return;
		}
	}

	// Emit new command
	if( g_ui.commandCount < MAX_UI_COMMANDS )
	{
		remixapi_UIDrawCommand *cmd = &g_ui.commands[g_ui.commandCount++];
		cmd->textureId    = textureId;
		cmd->indexOffset  = baseIndex;
		cmd->indexCount   = 6;
		cmd->vertexOffset = 0;
		cmd->flags        = flags;
	}
}

/*
 * R_UI_DrawStretchPic - Primary engine callback for 2D textured sprites and font glyphs
 */
void R_UI_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int texnum )
{
	if( !isfinite( x ) || !isfinite( y ) || !isfinite( w ) || !isfinite( h ) ||
	    !isfinite( s1 ) || !isfinite( t1 ) || !isfinite( s2 ) || !isfinite( t2 ) )
		return;

	remixapi_UITextureHandle textureId = 0;
	if( texnum > 0 && texnum < MAX_UI_TEXTURES && g_ui.textures[texnum].registered )
		textureId = (remixapi_UITextureHandle)texnum;

	uint32_t flags = REMIXAPI_UI_NATIVE_LINEAR;

	R_UI_EmitQuad(
		x,         y,         s1, t1,  // Top-left
		x + w,     y,         s2, t1,  // Top-right
		x + w,     y + h,     s2, t2,  // Bottom-right
		x,         y + h,     s1, t2,  // Bottom-left
		textureId,
		g_ui.currentColor,
		flags
	);
}

/*
 * R_UI_FillRGBA - Solid color rect drawing (console background, selection boxes, HUD panels)
 */
void R_UI_FillRGBA( int rendermode, float x, float y, float w, float h, byte r, byte g, byte b, byte a )
{
	if( !isfinite( x ) || !isfinite( y ) || !isfinite( w ) || !isfinite( h ) )
		return;

	uint32_t packedColor = ( (uint32_t)a << 24 ) | ( (uint32_t)b << 16 ) | ( (uint32_t)g << 8 ) | (uint32_t)r;
	uint32_t flags = 0;

	if( rendermode == kRenderTransAdd )
	{
		// Custom blend: Src = SRC_ALPHA (6), Dst = ONE (1)
		flags = REMIXAPI_UI_NATIVE_CUSTOM_BLEND | ( 6 << 8 ) | ( 1 << 12 );
	}

	// Texture ID 0 is the built-in 1x1 white texture
	R_UI_EmitQuad(
		x,     y,     0.0f, 0.0f,
		x + w, y,     1.0f, 0.0f,
		x + w, y + h, 1.0f, 1.0f,
		x,     y + h, 0.0f, 1.0f,
		0,
		packedColor,
		flags
	);
}

/*
 * R_UI_DrawPoly - Arbitrary convex polygon drawing in 2D
 */
void R_UI_DrawPoly( const float *verts, int numverts, int texnum )
{
	if( numverts < 3 || !verts )
		return;

	remixapi_UITextureHandle textureId = 0;
	if( texnum > 0 && texnum < MAX_UI_TEXTURES && g_ui.textures[texnum].registered )
		textureId = (remixapi_UITextureHandle)texnum;

	uint32_t flags = REMIXAPI_UI_NATIVE_LINEAR;

	// Triangulate convex fan: (0, i, i+1)
	for( int i = 1; i < numverts - 1; i++ )
	{
		float x0 = verts[0 * 4 + 0], y0 = verts[0 * 4 + 1], s0 = verts[0 * 4 + 2], t0 = verts[0 * 4 + 3];
		float x1 = verts[i * 4 + 0], y1 = verts[i * 4 + 1], s1 = verts[i * 4 + 2], t1 = verts[i * 4 + 3];
		float x2 = verts[(i+1)*4+0], y2 = verts[(i+1)*4+1], s2 = verts[(i+1)*4+2], t2 = verts[(i+1)*4+3];

		R_UI_EmitQuad(
			x0, y0, s0, t0,
			x1, y1, s1, t1,
			x2, y2, s2, t2,
			x2, y2, s2, t2,
			textureId,
			g_ui.currentColor,
			flags
		);
	}
}

/*
 * R_UI_DrawCharacter - Direct character glyph helper
 */
void R_UI_DrawCharacter( int x, int y, int number, int r, int g, int b, int texnum, int charW, int charH )
{
	uint32_t prevColor = g_ui.currentColor;
	g_ui.currentColor = ( 255u << 24 ) | ( (uint32_t)b << 16 ) | ( (uint32_t)g << 8 ) | (uint32_t)r;

	float col = (float)( number % 16 ) * 0.0625f;
	float row = (float)( number / 16 ) * 0.0625f;

	R_UI_DrawStretchPic( (float)x, (float)y, (float)charW, (float)charH, col, row, col + 0.0625f, row + 0.0625f, texnum );

	g_ui.currentColor = prevColor;
}

/*
 * 2D TriAPI Implementation: Accumulate TriAPI calls in 2D mode into UI draw lists
 */
void R_UI_TriRenderMode( int mode )
{
	(void)mode;
}

void R_UI_TriBegin( int primitiveCode )
{
	s_tri2D.primitive   = primitiveCode;
	s_tri2D.vertexCount = 0;
}

void R_UI_TriColor4ub( byte r, byte g, byte b, byte a )
{
	s_tri2D.currentColor = ( (uint32_t)a << 24 ) | ( (uint32_t)b << 16 ) | ( (uint32_t)g << 8 ) | (uint32_t)r;
	g_ui.currentColor    = s_tri2D.currentColor;
}

void R_UI_TriColor4f( float r, float g, float b, float a )
{
	byte br = (byte)bound( 0, (int)( r * 255.0f ), 255 );
	byte bg = (byte)bound( 0, (int)( g * 255.0f ), 255 );
	byte bb = (byte)bound( 0, (int)( b * 255.0f ), 255 );
	byte ba = (byte)bound( 0, (int)( a * 255.0f ), 255 );
	R_UI_TriColor4ub( br, bg, bb, ba );
}

void R_UI_TriTexCoord2f( float u, float v )
{
	s_tri2D.currentUV[0] = u;
	s_tri2D.currentUV[1] = v;
}

void R_UI_TriVertex3f( float x, float y, float z )
{
	(void)z;
	if( s_tri2D.vertexCount < 64 )
	{
		int idx = s_tri2D.vertexCount++;
		s_tri2D.tempVerts[idx][0]  = x;
		s_tri2D.tempVerts[idx][1]  = y;
		s_tri2D.tempUVs[idx][0]    = s_tri2D.currentUV[0];
		s_tri2D.tempUVs[idx][1]    = s_tri2D.currentUV[1];
		s_tri2D.tempColors[idx]    = s_tri2D.currentColor;
	}
}

void R_UI_TriVertex3fv( const float *worldPnt )
{
	if( worldPnt )
		R_UI_TriVertex3f( worldPnt[0], worldPnt[1], worldPnt[2] );
}

void R_UI_TriEnd( void )
{
	if( s_tri2D.vertexCount < 3 )
		return;

	remixapi_UITextureHandle textureId = 0;
	if( g_ui.currentTextureId > 0 && (size_t)g_ui.currentTextureId < MAX_UI_TEXTURES && g_ui.textures[(size_t)g_ui.currentTextureId].registered )
		textureId = g_ui.currentTextureId;

	uint32_t flags = REMIXAPI_UI_NATIVE_LINEAR;

	if( s_tri2D.primitive == TRI_QUADS && s_tri2D.vertexCount >= 4 )
	{
		for( int i = 0; i + 3 < s_tri2D.vertexCount; i += 4 )
		{
			R_UI_EmitQuad(
				s_tri2D.tempVerts[i+0][0], s_tri2D.tempVerts[i+0][1], s_tri2D.tempUVs[i+0][0], s_tri2D.tempUVs[i+0][1],
				s_tri2D.tempVerts[i+1][0], s_tri2D.tempVerts[i+1][1], s_tri2D.tempUVs[i+1][0], s_tri2D.tempUVs[i+1][1],
				s_tri2D.tempVerts[i+2][0], s_tri2D.tempVerts[i+2][1], s_tri2D.tempUVs[i+2][0], s_tri2D.tempUVs[i+2][1],
				s_tri2D.tempVerts[i+3][0], s_tri2D.tempVerts[i+3][1], s_tri2D.tempUVs[i+3][0], s_tri2D.tempUVs[i+3][1],
				textureId,
				s_tri2D.tempColors[i+0],
				flags
			);
		}
	}
	else if( s_tri2D.primitive == TRI_TRIANGLES )
	{
		for( int i = 0; i + 2 < s_tri2D.vertexCount; i += 3 )
		{
			R_UI_EmitQuad(
				s_tri2D.tempVerts[i+0][0], s_tri2D.tempVerts[i+0][1], s_tri2D.tempUVs[i+0][0], s_tri2D.tempUVs[i+0][1],
				s_tri2D.tempVerts[i+1][0], s_tri2D.tempVerts[i+1][1], s_tri2D.tempUVs[i+1][0], s_tri2D.tempUVs[i+1][1],
				s_tri2D.tempVerts[i+2][0], s_tri2D.tempVerts[i+2][1], s_tri2D.tempUVs[i+2][0], s_tri2D.tempUVs[i+2][1],
				s_tri2D.tempVerts[i+2][0], s_tri2D.tempVerts[i+2][1], s_tri2D.tempUVs[i+2][0], s_tri2D.tempUVs[i+2][1],
				textureId,
				s_tri2D.tempColors[i+0],
				flags
			);
		}
	}
	else if( s_tri2D.primitive == TRI_TRIANGLE_FAN || s_tri2D.primitive == TRI_POLYGON )
	{
		for( int i = 1; i + 1 < s_tri2D.vertexCount; i++ )
		{
			R_UI_EmitQuad(
				s_tri2D.tempVerts[0][0],   s_tri2D.tempVerts[0][1],   s_tri2D.tempUVs[0][0],   s_tri2D.tempUVs[0][1],
				s_tri2D.tempVerts[i][0],   s_tri2D.tempVerts[i][1],   s_tri2D.tempUVs[i][0],   s_tri2D.tempUVs[i][1],
				s_tri2D.tempVerts[i+1][0], s_tri2D.tempVerts[i+1][1], s_tri2D.tempUVs[i+1][0], s_tri2D.tempUVs[i+1][1],
				s_tri2D.tempVerts[i+1][0], s_tri2D.tempVerts[i+1][1], s_tri2D.tempUVs[i+1][0], s_tri2D.tempUVs[i+1][1],
				textureId,
				s_tri2D.tempColors[0],
				flags
			);
		}
	}
}

/*
 * R_UI_EndFrame - Submit compiled UI draw list to OpenRemix before Present
 */
void R_UI_EndFrame( void )
{
	if( !g_remix.SubmitUIDrawList )
		return;

	if( g_ui.commandCount == 0 || g_ui.vertexCount == 0 )
	{
		g_remix.SubmitUIDrawList( NULL );
		return;
	}

	uint32_t width  = ( gpGlobals && gpGlobals->width > 0 )  ? (uint32_t)gpGlobals->width  : 1920;
	uint32_t height = ( gpGlobals && gpGlobals->height > 0 ) ? (uint32_t)gpGlobals->height : 1080;

	remixapi_UIDrawList drawList;
	memset( &drawList, 0, sizeof( drawList ) );
	drawList.sType         = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
	drawList.pNext         = NULL;
	drawList.displayWidth  = width;
	drawList.displayHeight = height;
	drawList.pVertices     = g_ui.vertices;
	drawList.vertexCount   = g_ui.vertexCount;
	drawList.pIndices      = g_ui.indices;
	drawList.indexCount    = g_ui.indexCount;
	drawList.pCommands     = g_ui.commands;
	drawList.commandCount  = g_ui.commandCount;

	remixapi_ErrorCode err = g_remix.SubmitUIDrawList( &drawList );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_WARN "ref_openremix: SubmitUIDrawList failed: %s (code %d)\n",
		                      R_Remix_GetErrorString( err ), (int)err );
	}
}
