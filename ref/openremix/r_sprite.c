/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_sprite.c - Sprites, Beams & Temp Entities Subsystem Implementation
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_sprite.h"
#include "r_textures.h"
#include "r_ui.h"
#include "sprite.h"
#include "r_efx.h"
#include "customentity.h"
#include "r_bsp.h"
#include "r_lights.h"

#define NOISE_DIVISIONS   64
#define GLARE_FALLOFF     19000.0f
#define HASH_UNIT_QUAD    0x53505254554E4954ULL // "SPRTUNIT"
#define HASH_DYNAMIC_BASE 0xD700000000000000ULL

#ifndef M_PI2_F
#define M_PI2_F 6.28318530717958647692f
#endif

// Static unit billboard quad mesh handle
static remixapi_MeshHandle g_unitQuadMesh = NULL;
static remixapi_MaterialHandle g_whiteMaterial = NULL;

static dynamic_mesh_slot_t g_dynamicMeshes[MAX_DYNAMIC_MESH_SLOTS];
static uint32_t            g_dynamicMeshIndex = 0;

// Fractal & Sine noise arrays for beams
static float rgNoise[NOISE_DIVISIONS + 1];

/*
 * Noise Generators
 */
static void FracNoise( float *noise, int divs )
{
	int div2 = divs >> 1;
	if( divs < 2 ) return;

	noise[div2] = ( noise[0] + noise[divs] ) * 0.5f + (float)divs * gEngfuncs.COM_RandomFloat( -0.125f, 0.125f );
	if( div2 > 1 )
	{
		FracNoise( &noise[div2], div2 );
		FracNoise( noise, div2 );
	}
}

static void SineNoise( float *noise, int divs )
{
	float freq = 0.0f;
	float step = M_PI_F / (float)divs;
	for( int i = 0; i < divs; i++ )
	{
		noise[i] = sinf( freq );
		freq += step;
	}
}

/*
 * Helper: Create or retrieve material for sprite texture
 */
remixapi_MaterialHandle R_GetSpriteMaterial( int texnum )
{
	if( texnum > 0 && texnum < MAX_REMIX_TEXTURES )
	{
		remixapi_MaterialHandle mat = R_Textures_GetMaterial( (unsigned int)texnum );
		if( mat ) return mat;
	}

	if( !g_whiteMaterial && g_remix.CreateMaterial )
	{
		remixapi_MaterialInfoOpaqueEXT opaque;
		memset( &opaque, 0, sizeof( opaque ) );
		opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
		opaque.albedoConstant.x = 1.0f;
		opaque.albedoConstant.y = 1.0f;
		opaque.albedoConstant.z = 1.0f;
		opaque.opacityConstant  = 1.0f;
		opaque.alphaTestType    = 7; // ALWAYS pass
		opaque.useDrawCallAlphaState = 1;

		remixapi_MaterialInfo info;
		memset( &info, 0, sizeof( info ) );
		info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
		info.pNext = &opaque;
		info.hash  = 0x1ULL;
		g_remix.CreateMaterial( &info, &g_whiteMaterial );
	}
	return g_whiteMaterial;
}

/*
 * R_Sprite_Init - Build cached unit quad mesh
 */
static cvar_t *rt_tracer_emis = NULL;
static cvar_t *rt_glow_sprites = NULL;
static cvar_t *rt_sprite_debug = NULL;
static cvar_t *rt_beam_emis    = NULL;

void R_Sprite_Init( void )
{
	rt_sprite_debug = gEngfuncs.Cvar_Get( "rt_debug", "0", 0, "Print what the renderer draws and changes: each kind of sprite (model, render mode, effects, scale), light style changes" );
	rt_glow_sprites = gEngfuncs.Cvar_Get( "rt_glow_sprites", "0", FCVAR_ARCHIVE, "Draw the glare sprites that fake bloom on lights (they grow with distance)" );
	rt_beam_emis   = gEngfuncs.Cvar_Get( "rt_beam_emis", "4", FCVAR_ARCHIVE, "Glow of beams (gluon gun, lasers, lightning, trails), relative to other additive effects (rt_fx_emis)" );
	rt_tracer_emis = gEngfuncs.Cvar_Get( "rt_tracer_emis", "6", FCVAR_ARCHIVE, "Glow of sparks and tracers, relative to other additive effects (rt_fx_emis)" );

	if( !g_remix.CreateMesh )
		return;

	// Build 1x1 Unit Billboard Quad centered at (0, 0, 0)
	remixapi_HardcodedVertex vertices[4];
	memset( vertices, 0, sizeof( vertices ) );

	vertices[0].position[0] = -0.5f; vertices[0].position[1] = -0.5f; vertices[0].position[2] = 0.0f;
	vertices[0].normal[2]   = 1.0f;
	vertices[0].texcoord[0] = 0.0f;  vertices[0].texcoord[1] = 1.0f;
	vertices[0].color       = 0xFFFFFFFF;

	vertices[1].position[0] = -0.5f; vertices[1].position[1] = 0.5f;  vertices[1].position[2] = 0.0f;
	vertices[1].normal[2]   = 1.0f;
	vertices[1].texcoord[0] = 0.0f;  vertices[1].texcoord[1] = 0.0f;
	vertices[1].color       = 0xFFFFFFFF;

	vertices[2].position[0] = 0.5f;  vertices[2].position[1] = 0.5f;  vertices[2].position[2] = 0.0f;
	vertices[2].normal[2]   = 1.0f;
	vertices[2].texcoord[0] = 1.0f;  vertices[2].texcoord[1] = 0.0f;
	vertices[2].color       = 0xFFFFFFFF;

	vertices[3].position[0] = 0.5f;  vertices[3].position[1] = -0.5f; vertices[3].position[2] = 0.0f;
	vertices[3].normal[2]   = 1.0f;
	vertices[3].texcoord[0] = 1.0f;  vertices[3].texcoord[1] = 1.0f;
	vertices[3].color       = 0xFFFFFFFF;

	uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

	remixapi_MaterialHandle defaultMat = R_GetSpriteMaterial( 0 );

	remixapi_MeshInfoSurfaceTriangles surface;
	memset( &surface, 0, sizeof( surface ) );
	surface.vertices_values   = vertices;
	surface.vertices_count    = 4;
	surface.indices_values    = indices;
	surface.indices_count     = 6;
	surface.skinning_hasvalue = 0;
	surface.material          = defaultMat;

	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ) );
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash            = HASH_UNIT_QUAD;
	meshInfo.surfaces_values = &surface;
	meshInfo.surfaces_count  = 1;

	remixapi_ErrorCode err = g_remix.CreateMesh( &meshInfo, &g_unitQuadMesh );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: Failed to create unit quad mesh (%d)\n", (int)err );
	}

	memset( g_dynamicMeshes, 0, sizeof( g_dynamicMeshes ) );
	g_dynamicMeshIndex = 0;

	gEngfuncs.Con_DPrintf( "ref_openremix: Sprite subsystem initialized\n" );
}

/*
 * R_Sprite_Shutdown - Release unit quad and dynamic meshes
 */
void R_Sprite_Shutdown( void )
{
	if( g_unitQuadMesh && g_remix.DestroyMesh )
	{
		g_remix.DestroyMesh( g_unitQuadMesh );
		g_unitQuadMesh = NULL;
	}

	if( g_remix.DestroyMesh )
	{
		for( uint32_t i = 0; i < MAX_DYNAMIC_MESH_SLOTS; i++ )
		{
			if( g_dynamicMeshes[i].handle )
			{
				g_remix.DestroyMesh( g_dynamicMeshes[i].handle );
				g_dynamicMeshes[i].handle = NULL;
			}
		}
	}

	if( g_whiteMaterial && g_remix.DestroyMaterial )
	{
		g_remix.DestroyMaterial( g_whiteMaterial );
		g_whiteMaterial = NULL;
	}

	memset( g_dynamicMeshes, 0, sizeof( g_dynamicMeshes ) );
	g_dynamicMeshIndex = 0;
}

/*
 * R_Sprite_NewMap - Clear dynamic meshes on map change
 */
void R_Sprite_NewMap( void )
{
	if( g_remix.DestroyMesh )
	{
		for( uint32_t i = 0; i < MAX_DYNAMIC_MESH_SLOTS; i++ )
		{
			if( g_dynamicMeshes[i].handle )
			{
				g_remix.DestroyMesh( g_dynamicMeshes[i].handle );
				g_dynamicMeshes[i].handle = NULL;
			}
		}
	}
	g_dynamicMeshIndex = 0;
}

/*
 * R_Sprite_BeginFrame - Reset dynamic mesh allocation index
 */
void R_Sprite_BeginFrame( void )
{
	g_dynamicMeshIndex = 0;
}

/*
 * Helper: Allocate dynamic mesh slot
 */
dynamic_mesh_slot_t *R_AllocDynamicMeshSlot( void )
{
	if( g_dynamicMeshIndex >= MAX_DYNAMIC_MESH_SLOTS )
		g_dynamicMeshIndex = 0;

	dynamic_mesh_slot_t *slot = &g_dynamicMeshes[g_dynamicMeshIndex];
	slot->hash = HASH_DYNAMIC_BASE + (uint64_t)g_dynamicMeshIndex;
	g_dynamicMeshIndex++;

	return slot;
}

/*
 * Helper: Configure blend, identity, and raster state
 */
void R_SetupSpriteBlendState( remixapi_InstanceInfoBlendEXT *pBlend,
                              remixapi_InstanceIdentityEXT *pIdent,
                              remixapi_InstanceRasterStateEXT *pRaster,
                              uint64_t instanceId,
                              int rendermode )
{
	if( pRaster )
	{
		memset( pRaster, 0, sizeof( *pRaster ) );
		pRaster->sType              = REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT;
		pRaster->depthTest          = 1;
		pRaster->depthWrite         = ( rendermode == kRenderNormal ) ? 1 : 0;
		pRaster->depthCompare       = 3; // VK_COMPARE_OP_LESS_OR_EQUAL
		pRaster->disableColorWrites = 0;
		pRaster->pNext              = NULL;
	}

	if( pBlend )
	{
		memset( pBlend, 0, sizeof( *pBlend ) );
		pBlend->sType               = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		pBlend->pNext               = pRaster;
		pBlend->writeMask           = 15;
		pBlend->alphaTestCompareOp  = 7; // ALWAYS pass test
		pBlend->alphaTestEnabled    = 0;

		// Configure modulation: Texel * Vertex
		pBlend->textureColorArg1Source = 1; // Texel
		pBlend->textureColorArg2Source = 2; // Vertex
		pBlend->textureColorOperation  = 3; // Modulate

		pBlend->textureAlphaArg1Source = 1; // Texel
		pBlend->textureAlphaArg2Source = 2; // Vertex
		pBlend->textureAlphaOperation  = 3; // Modulate

		switch( rendermode )
		{
		case kRenderTransAdd:
		case kRenderGlow:
			pBlend->alphaBlendEnabled   = 1;
			pBlend->srcColorBlendFactor = 6; // VK_BLEND_FACTOR_SRC_ALPHA
			pBlend->dstColorBlendFactor = 1; // VK_BLEND_FACTOR_ONE
			pBlend->srcAlphaBlendFactor = 1;
			pBlend->dstAlphaBlendFactor = 1;
			pBlend->colorBlendOp        = 0;
			pBlend->alphaBlendOp        = 0;
			break;

		case kRenderTransColor:
		case kRenderTransTexture:
		case kRenderTransAlpha:
			pBlend->alphaBlendEnabled   = 1;
			pBlend->srcColorBlendFactor = 6; // VK_BLEND_FACTOR_SRC_ALPHA
			pBlend->dstColorBlendFactor = 7; // VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
			pBlend->srcAlphaBlendFactor = 6;
			pBlend->dstAlphaBlendFactor = 7;
			pBlend->colorBlendOp        = 0;
			pBlend->alphaBlendOp        = 0;
			break;

		case kRenderNormal:
		default:
			pBlend->alphaBlendEnabled   = 0;
			break;
		}
	}

	if( pIdent )
	{
		memset( pIdent, 0, sizeof( *pIdent ) );
		pIdent->sType          = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
		pIdent->pNext          = pBlend;
		pIdent->classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT; // 2
		pIdent->rasterVisible  = 1;
		pIdent->instanceId     = instanceId ? instanceId : 1ULL;
	}
}

/*
 * Occlusion and glare blend calculation
 */
qboolean R_SpriteOccluded( cl_entity_t *e, vec3_t origin, float *pscale )
{
	if( e->curstate.rendermode == kRenderGlow )
	{
		vec3_t glowDist;
		VectorSubtract( origin, RI.vieworg, glowDist );
		float dist = VectorLength( glowDist );

		if( gEngfuncs.EV_VisTraceLine )
		{
			pmtrace_t *ptr = gEngfuncs.EV_VisTraceLine( RI.vieworg, origin, PM_GLASS_IGNORE | PM_STUDIO_IGNORE );
			if( ( 1.0f - ptr->fraction ) * dist > 8.0f )
				return true; // Occluded by world geometry
		}

		if( e->curstate.renderfx != kRenderFxNoDissipation )
		{
			float brightness = GLARE_FALLOFF / ( dist * dist );
			brightness = bound( 0.05f, brightness, 1.0f );
			*pscale *= dist * ( 1.0f / 200.0f );
			tr.blend *= brightness;
		}
	}

	return false;
}

static uint32_t R_SpriteDebugHash( const char *name )
{
	uint32_t h = 2166136261u;
	while( *name )
		h = ( h ^ (uint8_t)*name++ ) * 16777619u;
	return h;
}

/*
 * R_DrawSpriteModel - Main sprite rendering callback
 */
void R_DrawSpriteModel( cl_entity_t *e )
{
	if( !e || !e->model || e->model->type != mod_sprite || !e->model->cache.data )
		return;

	if( !g_unitQuadMesh || !g_remix.DrawInstance )
		return;

	if( rt_sprite_debug && rt_sprite_debug->value > 0.0f )
	{
		static uint32_t seen[256];
		static int numSeen = 0;
		const uint32_t key = R_SpriteDebugHash( e->model->name ) ^ (uint32_t)( ( e->curstate.rendermode << 8 ) ^ ( e->curstate.renderfx << 16 ) ^ ( (int)( e->curstate.scale * 100 ) << 24 ) );
		qboolean known = false;
		for( int i = 0; i < numSeen && !known; i++ )
			known = seen[i] == key;
		if( !known && numSeen < ARRAYSIZE( seen ))
		{
			seen[numSeen++] = key;
			gEngfuncs.Con_Printf( "sprite: %s ent %d rendermode %d renderfx %d amt %d scale %.2f origin %.0f %.0f %.0f\n", e->model->name, e->index,
			                      e->curstate.rendermode, e->curstate.renderfx, e->curstate.renderamt, e->curstate.scale, e->origin[0], e->origin[1], e->origin[2] );
		}
	}

	// Glare sprites (glow render mode) fake bloom on lights: the dissipating ones grow with distance
	// and fade when occluded, the others (env_glow's flare1 and glow01) stay at full brightness.
	// The path tracer's lights and bloom make them look out of place. Temp entities (index 0) in
	// this mode with no dissipation are effects, such as ricochets, and stay
	if( e->curstate.rendermode == kRenderGlow && ( e->curstate.renderfx != kRenderFxNoDissipation || e->index > 0 ) &&
	    !( rt_glow_sprites && rt_glow_sprites->value > 0.0f ))
		return;

	model_t *model = e->model;
	msprite_t *psprite = (msprite_t *)model->cache.data;

	vec3_t origin;
	VectorCopy( e->origin, origin );

	// Follow attachment if entity follows another model
	if( e->curstate.aiment > 0 && e->curstate.movetype == MOVETYPE_FOLLOW )
	{
		cl_entity_t *parent = CL_GetEntityByIndex( e->curstate.aiment );
		if( parent && parent->model )
		{
			if( parent->model->type == mod_studio && e->curstate.body > 0 )
			{
				int num = bound( 1, e->curstate.body, MAXSTUDIOATTACHMENTS );
				VectorCopy( parent->attachment[num - 1], origin );
			}
			else
			{
				VectorCopy( parent->origin, origin );
			}
		}
	}

	float scale = ( e->curstate.scale > 0.0f ) ? e->curstate.scale : 1.0f;
	if( R_SpriteOccluded( e, origin, &scale ) )
		return;

	// Calculate entity blend factor
	if( e->curstate.rendermode != kRenderNormal )
		tr.blend = (float)CL_FxBlend( e ) / 255.0f;
	else
		tr.blend = 1.0f;

	if( tr.blend <= 0.001f )
		return;

	// Retrieve frame
	mspriteframe_t *frame = NULL;
	if( gEngfuncs.R_GetSpriteFrame )
		frame = gEngfuncs.R_GetSpriteFrame( model, (int)e->curstate.frame, e->angles[YAW] );
	if( !frame )
		return;

	// 1. Compute Billboard Basis Vectors
	vec3_t v_right, v_up, v_forward;
	int type = psprite->type;

	if( e->angles[ROLL] != 0.0f && type == SPR_FWD_PARALLEL )
		type = SPR_FWD_PARALLEL_ORIENTED;

	switch( type )
	{
	case SPR_ORIENTED:
		AngleVectors( e->angles, v_forward, v_right, v_up );
		VectorMA( origin, -0.01f, v_forward, origin ); // Avoid z-fighting
		break;

	case SPR_FACING_UPRIGHT:
		VectorSet( v_right, origin[1] - RI.vieworg[1], -( origin[0] - RI.vieworg[0] ), 0.0f );
		VectorNormalize( v_right );
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		CrossProduct( v_right, v_up, v_forward );
		break;

	case SPR_FWD_PARALLEL_UPRIGHT:
		if( RI.vforward[2] > 0.9998f || RI.vforward[2] < -0.9998f )
			return; // Parallel to view line
		VectorSet( v_up, 0.0f, 0.0f, 1.0f );
		VectorSet( v_right, RI.vforward[1], -RI.vforward[0], 0.0f );
		VectorNormalize( v_right );
		CrossProduct( v_right, v_up, v_forward );
		break;

	case SPR_FWD_PARALLEL_ORIENTED:
		{
			float angle = e->angles[ROLL] * ( M_PI_F / 180.0f );
			float sr = sinf( angle );
			float cr = cosf( angle );
			for( int i = 0; i < 3; i++ )
			{
				v_right[i] = ( RI.vright[i] * cr + RI.vup[i] * sr );
				v_up[i]    = ( -RI.vright[i] * sr + RI.vup[i] * cr );
			}
			VectorCopy( RI.vforward, v_forward );
		}
		break;

	case SPR_FWD_PARALLEL:
	default:
		VectorCopy( RI.vright, v_right );
		VectorCopy( RI.vup, v_up );
		VectorCopy( RI.vforward, v_forward );
		break;
	}

	// 2. Compute Quad Dimensions and Center Position
	float w = (float)( frame->right - frame->left ) * scale;
	float h = (float)( frame->up - frame->down ) * scale;
	float offsetX = (float)( frame->left + frame->right ) * 0.5f * scale;
	float offsetY = (float)( frame->down + frame->up ) * 0.5f * scale;

	vec3_t center;
	VectorCopy( origin, center );
	VectorMA( center, offsetX, v_right, center );
	VectorMA( center, offsetY, v_up, center );

	// 3. Construct 3x4 Affine Transform Matrix
	remixapi_Transform transform;
	transform.matrix[0][0] = v_right[0] * w;
	transform.matrix[0][1] = v_up[0] * h;
	transform.matrix[0][2] = v_forward[0];
	transform.matrix[0][3] = center[0];

	transform.matrix[1][0] = v_right[1] * w;
	transform.matrix[1][1] = v_up[1] * h;
	transform.matrix[1][2] = v_forward[1];
	transform.matrix[1][3] = center[1];

	transform.matrix[2][0] = v_right[2] * w;
	transform.matrix[2][1] = v_up[2] * h;
	transform.matrix[2][2] = v_forward[2];
	transform.matrix[2][3] = center[2];

	// 4. Configure Blend, Material, and Raster Extensions
	remixapi_InstanceRasterStateEXT rasterState;
	remixapi_InstanceInfoBlendEXT blendExt;
	remixapi_InstanceIdentityEXT identExt;
	uint64_t spriteInstId = ( (uint64_t)e->index << 32 ) ^ ( (uint64_t)e->curstate.frame << 16 ) ^ (uint64_t)(uintptr_t)model;
	if( !spriteInstId ) spriteInstId = 1ULL;

	R_SetupSpriteBlendState( &blendExt, &identExt, &rasterState, spriteInstId, e->curstate.rendermode );

	// Setup sprite color tint & alpha from curstate
	color24 c = e->curstate.rendercolor;
	if( !c.r && !c.g && !c.b ) { c.r = c.g = c.b = 255; } // Protect against Hammer 0 0 0 bug
	uint8_t a = (uint8_t)bound( 0, (int)( tr.blend * 255.0f ), 255 );
	blendExt.tFactor = ( (uint32_t)a << 24 ) | ( (uint32_t)c.r << 16 ) | ( (uint32_t)c.g << 8 ) | (uint32_t)c.b;
	blendExt.textureColorArg2Source = 3; // Factor
	blendExt.textureAlphaArg2Source = 3; // Factor

	// Retrieve frame texture material
	remixapi_MaterialHandle mat = R_Textures_GetMaterial( (unsigned int)frame->gl_texturenum );
	if( !mat ) mat = R_GetSpriteMaterial( 0 );

	// Additive glows go to the path tracer as self-illuminated emitters (see R_Textures_CreateFx)
	int rendermode = e->curstate.rendermode;
	remixapi_MaterialHandle fxMat = NULL;
	if( rendermode == kRenderTransAdd || rendermode == kRenderGlow )
		fxMat = R_Textures_GetFxMaterial( (unsigned int)frame->gl_texturenum );

	if( fxMat )
	{
		mat = fxMat;
		R_SetupSpriteBlendState( &blendExt, &identExt, &rasterState, spriteInstId, kRenderTransAlpha );
		identExt.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;

		// Move additive intensity from colour into coverage
		uint32_t m = Q_max( c.r, Q_max( c.g, c.b ) );
		uint32_t fa = (uint32_t)a * m / 255;
		blendExt.tFactor = ( fa << 24 ) | ( ( (uint32_t)c.r * 255 / m ) << 16 ) | ( ( (uint32_t)c.g * 255 / m ) << 8 ) | ( (uint32_t)c.b * 255 / m );
		blendExt.textureColorArg2Source = 3; // Factor
		blendExt.textureAlphaArg2Source = 3; // Factor
	}

	remixapi_InstanceInfoMaterialEXT matExt;
	memset( &matExt, 0, sizeof( matExt ) );
	matExt.sType    = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
	matExt.pNext    = &identExt;
	matExt.material = mat;

	// 5. Submit Instance to OpenRemix
	remixapi_InstanceInfo inst;
	memset( &inst, 0, sizeof( inst ) );
	inst.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	inst.pNext         = &matExt;
	inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
	inst.mesh          = g_unitQuadMesh;
	inst.transform     = transform;
	inst.doubleSided   = 1;

	g_remix.DrawInstance( &inst );
}

/*
 * Beam Math Helper: Compute Normal Perpendicular to Viewer and Segment
 */
static void R_BeamComputeNormal( const vec3_t vStart, const vec3_t vNext, vec3_t pNormal )
{
	vec3_t vTangent, vDirToBeam;
	VectorSubtract( vNext, vStart, vTangent );
	VectorSubtract( vStart, RI.vieworg, vDirToBeam );
	CrossProduct( vTangent, vDirToBeam, pNormal );
	VectorNormalize( pNormal );
}

/*
 * Draw Segmented Glowing Beam Ribbon
 */
static void R_DrawSegs( const vec3_t source, const vec3_t delta, float width, float amplitude,
                        float freq, float speed, int segments, int flags,
                        float r, float g, float b, float brightness )
{
	if( segments < 2 )
		return;

	float length = VectorLength( delta );
	const float maxWidth = width * 0.5f;
	float div = 1.0f / (float)( segments - 1 );

	// Too many segments for the width would overlap: use fewer (as the GL renderer does)
	if( length * div < maxWidth * 1.414f )
	{
		segments = (int)( length / ( maxWidth * 1.414f )) + 1;
		if( segments < 2 ) segments = 2;
	}

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	div = 1.0f / (float)( segments - 1 );
	length *= 0.01f;
	float vStep = length * div;
	float vLast = fmodf( freq * speed, 1.0f );
	float scale = amplitude;

	if( FBitSet( flags, FBEAM_SINENOISE ))
	{
		// A spiral (the gluon gun's outer beam): at least 16 segments, its amplitude in units
		if( segments < 16 )
		{
			segments = 16;
			div = 1.0f / (float)( segments - 1 );
		}
		scale *= 100.0f;
		length = segments * 0.1f;
	}
	else
	{
		scale *= length * 2.0f;
	}

	int noiseStep = (int)( (float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f );
	int noiseIndex = 0;

	vec3_t perp1, beamDir;
	VectorNormalize2( delta, beamDir );
	CrossProduct( RI.vforward, beamDir, perp1 );
	VectorNormalize( perp1 );

	vec3_t curSegPos, nextSegPos, vNormal, vPoint1, vPoint2;
	VectorCopy( source, curSegPos );

	R_UI_TriRenderMode( kRenderTransAdd );
	R_UI_TriColor4f( r, g, b, brightness );
	R_UI_TriBegin( TRI_TRIANGLE_STRIP );

	for( int i = 0; i < segments; i++ )
	{
		float fraction = (float)i * div;
		VectorMA( source, fraction, delta, nextSegPos );

		if( scale != 0.0f && i != 0 && i != segments - 1 )
		{
			float factor = rgNoise[( noiseIndex >> 16 ) & ( NOISE_DIVISIONS - 1 )] * scale;
			if( FBitSet( flags, FBEAM_SINENOISE ) )
			{
				float s = sinf( fraction * M_PI_F * length + freq );
				float c = cosf( fraction * M_PI_F * length + freq );
				VectorMA( nextSegPos, ( factor * s ), RI.vup, nextSegPos );
				VectorMA( nextSegPos, ( factor * c ), RI.vright, nextSegPos );
			}
			else
			{
				VectorMA( nextSegPos, factor, perp1, nextSegPos );
			}
		}

		R_BeamComputeNormal( curSegPos, nextSegPos, vNormal );
		VectorMA( nextSegPos, width, vNormal, vPoint1 );
		VectorMA( nextSegPos, -width, vNormal, vPoint2 );

		R_UI_TriBrightness( 1.0f );
		R_UI_TriTexCoord2f( 0.0f, vLast );
		R_UI_TriVertex3fv( vPoint1 );

		R_UI_TriBrightness( 1.0f );
		R_UI_TriTexCoord2f( 1.0f, vLast );
		R_UI_TriVertex3fv( vPoint2 );

		VectorCopy( nextSegPos, curSegPos );
		vLast += vStep;
		noiseIndex += noiseStep;
	}

	R_UI_TriEnd();
}

/*
 * Draw Beam Disk
 */
static void R_DrawDisk( const vec3_t source, const vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	if( segments < 2 ) return;
	if( segments > NOISE_DIVISIONS ) segments = NOISE_DIVISIONS;

	float length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;

	float div = 1.0f / (float)( segments - 1 );
	float vStep = length * div;
	float vLast = fmodf( freq * speed, 1.0f );
	(void)scale;

	float w = fmodf( freq, width * 0.1f ) * delta[2];

	for( int i = 0; i < segments; i++ )
	{
		float fraction = (float)i * div;
		float s = sinf( fraction * M_PI2_F );
		float c = cosf( fraction * M_PI2_F );

		vec3_t point;
		VectorCopy( source, point );

		R_UI_TriBrightness( 1.0f );
		R_UI_TriTexCoord2f( 1.0f, vLast );
		R_UI_TriVertex3fv( point );

		point[0] = s * w + source[0];
		point[1] = c * w + source[1];
		point[2] = source[2];

		R_UI_TriBrightness( 1.0f );
		R_UI_TriTexCoord2f( 0.0f, vLast );
		R_UI_TriVertex3fv( point );

		vLast += vStep;
	}
}

/*
 * Draw Beam Cylinder
 */
static void R_DrawCylinder( const vec3_t source, const vec3_t delta, float width, float scale, float freq, float speed, int segments )
{
	if( segments < 2 ) return;
	if( segments > NOISE_DIVISIONS ) segments = NOISE_DIVISIONS;

	float length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;

	float div = 1.0f / (float)( segments - 1 );
	float vStep = length * div;
	float vLast = fmodf( freq * speed, 1.0f );
	(void)scale;

	for( int i = 0; i < segments; i++ )
	{
		float fraction = (float)i * div;
		float s = sinf( fraction * M_PI2_F );
		float c = cosf( fraction * M_PI2_F );

		vec3_t point;
		point[0] = s * freq * delta[2] + source[0];
		point[1] = c * freq * delta[2] + source[1];
		point[2] = source[2] + width;

		R_UI_TriBrightness( 0.0f );
		R_UI_TriTexCoord2f( 1.0f, vLast );
		R_UI_TriVertex3fv( point );

		point[0] = s * freq * ( delta[2] + width ) + source[0];
		point[1] = c * freq * ( delta[2] + width ) + source[1];
		point[2] = source[2] - width;

		R_UI_TriBrightness( 1.0f );
		R_UI_TriTexCoord2f( 0.0f, vLast );
		R_UI_TriVertex3fv( point );

		vLast += vStep;
	}
}

/*
 * Draw Beam Ring
 */
static void R_DrawRing( const vec3_t source, const vec3_t delta, float width, float amplitude, float freq, float speed, int segments )
{
	if( segments < 2 ) return;

	segments = (int)( (float)segments * M_PI_F );
	if( segments > NOISE_DIVISIONS * 8 ) segments = NOISE_DIVISIONS * 8;

	float length = VectorLength( delta ) * 0.01f * M_PI_F;
	if( length < 0.5f ) length = 0.5f;

	float div = 1.0f / (float)( segments - 1 );
	float vStep = length * div / 8.0f;
	float vLast = fmodf( freq * speed, 1.0f );
	float scale = amplitude * length / 8.0f;

	int noiseStep = (int)( (float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f ) * 8;
	int noiseIndex = 0;

	vec3_t halfDelta;
	VectorScale( delta, 0.5f, halfDelta );
	vec3_t center;
	VectorAdd( source, halfDelta, center );

	vec3_t xaxis;
	VectorCopy( halfDelta, xaxis );
	float radius = VectorLength( xaxis );
	if( radius < 0.1f ) return;

	vec3_t yaxis;
	yaxis[0] = xaxis[1];
	yaxis[1] = -xaxis[0];
	yaxis[2] = 0.0f;
	VectorNormalize( yaxis );
	VectorScale( yaxis, radius, yaxis );

	vec3_t screenLast = { 0 };

	for( int i = 0; i < segments + 1; i++ )
	{
		float fraction = (float)i * div;
		float x = sinf( fraction * M_PI2_F );
		float y = cosf( fraction * M_PI2_F );

		vec3_t point;
		point[0] = center[0] + xaxis[0] * x + yaxis[0] * y;
		point[1] = center[1] + xaxis[1] * x + yaxis[1] * y;
		point[2] = center[2] + xaxis[2] * x + yaxis[2] * y;

		float factor = rgNoise[( noiseIndex >> 16 ) & ( NOISE_DIVISIONS - 1 )] * scale;
		VectorMA( point, factor, RI.vup, point );
		factor *= cosf( fraction * M_PI_F * 24.0f + freq );
		VectorMA( point, factor, RI.vright, point );

		vec3_t screen;
		R_UI_TriWorldToScreen( point, screen );

		if( i != 0 )
		{
			vec3_t tmp, normal;
			VectorSubtract( screen, screenLast, tmp );
			tmp[2] = 0.0f;
			VectorNormalize( tmp );

			VectorScale( RI.vup, tmp[0], normal );
			VectorMA( normal, tmp[1], RI.vright, normal );

			vec3_t p1, p2;
			VectorMA( point, width, normal, p1 );
			VectorMA( point, -width, normal, p2 );

			vLast += vStep;
			R_UI_TriTexCoord2f( 1.0f, vLast );
			R_UI_TriVertex3fv( p2 );
			R_UI_TriTexCoord2f( 0.0f, vLast );
			R_UI_TriVertex3fv( p1 );
		}

		VectorCopy( screen, screenLast );
		noiseIndex += noiseStep;
	}
}

/*
 * Draw single beam
 */
/*
 * R_BeamComputePoint - Attachment point (or origin) of a beam's start/end entity
 */
static qboolean R_BeamComputePoint( int beamEnt, vec3_t pt )
{
	cl_entity_t *ent = gEngfuncs.R_BeamGetEntity( beamEnt );
	int attach = BEAMENT_ATTACHMENT( beamEnt < 0 ? -beamEnt : beamEnt );

	if( !ent )
	{
		VectorClear( pt );
		return false;
	}

	if( attach > 0 )
		VectorCopy( ent->attachment[attach - 1], pt );
	else if( gp_cl && ent->index == ( gp_cl->playernum + 1 ) )
		VectorCopy( gp_cl->simorg, pt );
	else
		VectorCopy( ent->origin, pt );

	return true;
}

/*
 * R_BeamRecomputeEndpoints - Follow the entities a beam is attached to
 */
static qboolean R_BeamRecomputeEndpoints( BEAM *pbeam )
{
	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ) )
	{
		cl_entity_t *start = gEngfuncs.R_BeamGetEntity( pbeam->startEntity );
		if( R_BeamComputePoint( pbeam->startEntity, pbeam->source ) )
		{
			if( !pbeam->pFollowModel )
				pbeam->pFollowModel = start->model;
			SetBits( pbeam->flags, FBEAM_STARTVISIBLE );
		}
		else if( !FBitSet( pbeam->flags, FBEAM_FOREVER ) )
		{
			ClearBits( pbeam->flags, FBEAM_STARTENTITY );
		}
	}

	if( FBitSet( pbeam->flags, FBEAM_ENDENTITY ) )
	{
		cl_entity_t *end = gEngfuncs.R_BeamGetEntity( pbeam->endEntity );
		if( R_BeamComputePoint( pbeam->endEntity, pbeam->target ) )
		{
			if( !pbeam->pFollowModel )
				pbeam->pFollowModel = end->model;
			SetBits( pbeam->flags, FBEAM_ENDVISIBLE );
		}
		else
		{
			if( !FBitSet( pbeam->flags, FBEAM_FOREVER ) )
			{
				ClearBits( pbeam->flags, FBEAM_ENDENTITY );
				if( gp_cl ) pbeam->die = gp_cl->time;
			}
			return false;
		}
	}

	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ) && !FBitSet( pbeam->flags, FBEAM_STARTVISIBLE ) )
		return false;
	return true;
}

/*
 * Draw a followed beam (TE_BEAMFOLLOW: rocket smoke, hornet and grenade trails). The trail is a list
 * of engine particles dropped along the entity's path, fading out over the beam's life
 */
static void R_DrawBeamFollow( BEAM *pbeam, float frametime )
{
	gEngfuncs.R_FreeDeadParticles( &pbeam->particles );

	particle_t *particles = pbeam->particles;
	particle_t *pnew = NULL;

	vec3_t delta;
	float div = 0.0f;
	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY ))
	{
		if( particles )
		{
			VectorSubtract( particles->org, pbeam->source, delta );
			div = VectorLength( delta );
			if( div >= 32.0f )
				pnew = gEngfuncs.CL_AllocParticleFast();
		}
		else
		{
			pnew = gEngfuncs.CL_AllocParticleFast();
		}
	}

	if( pnew )
	{
		VectorCopy( pbeam->source, pnew->org );
		pnew->die = gp_cl->time + pbeam->amplitude;
		VectorClear( pnew->vel );
		pnew->next = particles;
		pbeam->particles = pnew;
		particles = pnew;
	}

	if( !particles )
		return;

	vec3_t screen, screenLast;
	if( !pnew && div != 0.0f )
	{
		VectorCopy( pbeam->source, delta );
		R_UI_TriWorldToScreen( pbeam->source, screenLast );
		R_UI_TriWorldToScreen( particles->org, screen );
	}
	else if( particles->next )
	{
		VectorCopy( particles->org, delta );
		R_UI_TriWorldToScreen( particles->org, screenLast );
		R_UI_TriWorldToScreen( particles->next->org, screen );
		particles = particles->next;
	}
	else
	{
		return;
	}

	// The ribbon's side: perpendicular to the trail's direction on the screen
	vec3_t tmp, normal, last1, last2;
	VectorSubtract( screen, screenLast, tmp );
	tmp[2] = 0.0f;
	VectorNormalize( tmp );
	VectorScale( RI.vup, tmp[0], normal );
	VectorMA( normal, tmp[1], RI.vright, normal );
	VectorMA( delta, pbeam->width, normal, last1 );
	VectorMA( delta, -pbeam->width, normal, last2 );

	div = 1.0f / pbeam->amplitude;
	float fraction = ( pbeam->die - gp_cl->time ) * div;

	while( particles )
	{
		R_UI_TriBrightness( fraction );
		R_UI_TriTexCoord2f( 1.0f, 1.0f );
		R_UI_TriVertex3fv( last2 );
		R_UI_TriBrightness( fraction );
		R_UI_TriTexCoord2f( 0.0f, 1.0f );
		R_UI_TriVertex3fv( last1 );

		R_UI_TriWorldToScreen( particles->org, screen );
		VectorSubtract( screen, screenLast, tmp );
		tmp[2] = 0.0f;
		VectorNormalize( tmp );
		VectorScale( RI.vup, tmp[0], normal );
		VectorMA( normal, tmp[1], RI.vright, normal );
		VectorMA( particles->org, pbeam->width, normal, last1 );
		VectorMA( particles->org, -pbeam->width, normal, last2 );

		fraction = particles->next ? ( particles->die - gp_cl->time ) * div : 0.0f;

		R_UI_TriBrightness( fraction );
		R_UI_TriTexCoord2f( 0.0f, 0.0f );
		R_UI_TriVertex3fv( last1 );
		R_UI_TriBrightness( fraction );
		R_UI_TriTexCoord2f( 1.0f, 0.0f );
		R_UI_TriVertex3fv( last2 );

		VectorCopy( screen, screenLast );
		particles = particles->next;
	}

	// Drift the trail if its particles have a velocity
	for( particles = pbeam->particles; particles; particles = particles->next )
		VectorMA( particles->org, frametime, particles->vel, particles->org );
}

void R_BeamDraw( BEAM *pbeam, float frametime )
{
	if( !pbeam )
		return;

	// Every beam reaching here is drawn (the engine frees dead ones), as in the GL renderer
	SetBits( pbeam->flags, FBEAM_ISACTIVE );

	if( FBitSet( pbeam->flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY ) )
	{
		if( !R_BeamRecomputeEndpoints( pbeam ) )
		{
			ClearBits( pbeam->flags, FBEAM_ISACTIVE );
			return;
		}

		VectorSubtract( pbeam->target, pbeam->source, pbeam->delta );
		if( pbeam->amplitude >= 0.50f )
			pbeam->segments = VectorLength( pbeam->delta ) * 0.25f + 3.0f;
		else
			pbeam->segments = VectorLength( pbeam->delta ) * 0.075f + 3.0f;
	}

	vec3_t delta;
	VectorSubtract( pbeam->target, pbeam->source, delta );
	if( VectorLength( delta ) < 0.1f && pbeam->type != TE_BEAMFOLLOW )
		return;

	pbeam->freq += frametime;
	if( frametime != 0.0f )
	{
		rgNoise[0] = 0.0f;
		rgNoise[NOISE_DIVISIONS] = 0.0f;
		if( pbeam->amplitude != 0.0f )
		{
			if( FBitSet( pbeam->flags, FBEAM_SINENOISE ) )
				SineNoise( rgNoise, NOISE_DIVISIONS );
			else
				FracNoise( rgNoise, NOISE_DIVISIONS );
		}
	}

	float brightness = pbeam->brightness;
	if( FBitSet( pbeam->flags, FBEAM_FADEIN ) )
		brightness *= pbeam->t;
	else if( FBitSet( pbeam->flags, FBEAM_FADEOUT ) )
		brightness *= ( 1.0f - pbeam->t );

	int segments = (int)pbeam->segments;
	if( segments < 2 ) segments = 2;

	model_t *model = CL_ModelHandle( pbeam->modelIndex );
	if( !model || model->type != mod_sprite )
		return;

	double curtime = gp_cl ? gp_cl->time : tr.realtime;
	int frameIdx = (int)( pbeam->frame + pbeam->frameRate * curtime ) % ( pbeam->frameCount ? pbeam->frameCount : 1 );
	if( !R_UI_TriSpriteTexture( model, frameIdx ) )
		return;

	if( rt_sprite_debug && rt_sprite_debug->value > 0.0f )
	{
		static uint32_t seen[64];
		static int numSeen;
		const uint32_t key = R_SpriteDebugHash( model->name ) ^ (uint32_t)( pbeam->type << 4 ) ^ (uint32_t)( pbeam->flags << 12 ) ^ (uint32_t)( pbeam->width * 10.0f ) * 2654435761u ^ (uint32_t)( pbeam->amplitude * 100.0f ) * 40503u;
		qboolean found = false;
		for( int i = 0; i < numSeen; i++ )
			found = found || seen[i] == key;
		if( !found && numSeen < 64 )
		{
			seen[numSeen++] = key;
			gEngfuncs.Con_Printf( "beam %s type %d flags 0x%x width %.1f amp %.2f segs %d bright %.2f rgb %.2f %.2f %.2f len %.0f\n", model->name, pbeam->type, pbeam->flags,
			                      pbeam->width, pbeam->amplitude, (int)pbeam->segments, brightness, pbeam->r, pbeam->g, pbeam->b, VectorLength( delta ));
		}
	}

	R_UI_TriFxBoost( rt_beam_emis ? rt_beam_emis->value : 4.0f );

	switch( pbeam->type )
	{
	case TE_BEAMFOLLOW:
		R_UI_TriRenderMode( kRenderTransAdd );
		R_UI_TriColor4f( pbeam->r, pbeam->g, pbeam->b, brightness );
		R_UI_TriBegin( TRI_QUADS );
		R_DrawBeamFollow( pbeam, frametime );
		R_UI_TriEnd();
		break;

	case TE_BEAMDISK:
		R_UI_TriRenderMode( kRenderTransAdd );
		R_UI_TriColor4f( pbeam->r, pbeam->g, pbeam->b, brightness );
		R_UI_TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawDisk( pbeam->source, delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, segments );
		R_UI_TriEnd();
		break;

	case TE_BEAMCYLINDER:
		R_UI_TriRenderMode( kRenderTransAdd );
		R_UI_TriColor4f( pbeam->r, pbeam->g, pbeam->b, brightness );
		R_UI_TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawCylinder( pbeam->source, delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, segments );
		R_UI_TriEnd();
		break;

	case TE_BEAMRING:
		R_UI_TriRenderMode( kRenderTransAdd );
		R_UI_TriColor4f( pbeam->r, pbeam->g, pbeam->b, brightness );
		R_UI_TriBegin( TRI_TRIANGLE_STRIP );
		R_DrawRing( pbeam->source, delta, pbeam->width, pbeam->amplitude, pbeam->freq, pbeam->speed, segments );
		R_UI_TriEnd();
		break;

	case TE_BEAMPOINTS:
	case TE_BEAMHOSE:
	default:
		R_Lights_AddBeamLight( pbeam->source, pbeam->target, pbeam->r, pbeam->g, pbeam->b, brightness );
		R_DrawSegs( pbeam->source, delta, pbeam->width, pbeam->amplitude,
		            pbeam->freq, pbeam->speed, segments, pbeam->flags,
		            pbeam->r, pbeam->g, pbeam->b, brightness );
		break;
	}

	R_UI_TriFxBoost( 1.0f );
}

/*
 * R_BeamDrawCustomEntity - Build a beam from a server env_beam entity (ET_BEAM)
 */
static void R_BeamDrawCustomEntity( cl_entity_t *ent, float frametime )
{
	model_t *sprite = CL_ModelHandle( ent->curstate.modelindex );
	if( !sprite )
		return;

	double cltime = gp_cl ? gp_cl->time : tr.realtime;
	float amp = ent->curstate.body / 100.0f;

	BEAM beam;
	memset( &beam, 0, sizeof( beam ) );
	beam.type       = TE_BEAMPOINTS;
	beam.modelIndex = ent->curstate.modelindex;
	beam.frameCount = sprite->numframes;
	beam.frame      = (float)ent->curstate.frame;
	beam.frameRate  = ent->curstate.framerate;
	VectorCopy( ent->origin, beam.source );
	VectorCopy( ent->curstate.angles, beam.target ); // server beams carry the end point in angles
	VectorSubtract( beam.target, beam.source, beam.delta );
	beam.speed      = ent->curstate.animtime;
	beam.freq       = beam.speed * (float)cltime;
	beam.die        = (float)cltime;
	beam.amplitude  = amp;
	beam.brightness = CL_FxBlend( ent ) / 255.0f;
	beam.width      = ent->curstate.scale;
	beam.r          = ent->curstate.rendercolor.r / 255.0f;
	beam.g          = ent->curstate.rendercolor.g / 255.0f;
	beam.b          = ent->curstate.rendercolor.b / 255.0f;
	if( amp >= 0.50f )
		beam.segments = VectorLength( beam.delta ) * 0.25f + 3.0f;
	else
		beam.segments = VectorLength( beam.delta ) * 0.075f + 3.0f;

	switch( ent->curstate.rendermode & 0x0F )
	{
	case BEAM_ENTPOINT:
		if( ent->curstate.sequence )
		{
			SetBits( beam.flags, FBEAM_STARTENTITY );
			beam.startEntity = ent->curstate.sequence;
		}
		if( ent->curstate.skin )
		{
			SetBits( beam.flags, FBEAM_ENDENTITY );
			beam.endEntity = ent->curstate.skin;
		}
		break;
	case BEAM_ENTS:
		SetBits( beam.flags, FBEAM_STARTENTITY | FBEAM_ENDENTITY );
		beam.startEntity = ent->curstate.sequence;
		beam.endEntity   = ent->curstate.skin;
		break;
	case BEAM_HOSE:
		beam.type = TE_BEAMHOSE;
		break;
	case BEAM_POINTS:
	default:
		break;
	}

	int beamFlags = ent->curstate.rendermode & 0xF0;
	if( FBitSet( beamFlags, BEAM_FSINE ) )     SetBits( beam.flags, FBEAM_SINENOISE );
	if( FBitSet( beamFlags, BEAM_FSOLID ) )    SetBits( beam.flags, FBEAM_SOLID );
	if( FBitSet( beamFlags, BEAM_FSHADEIN ) )  SetBits( beam.flags, FBEAM_SHADEIN );
	if( FBitSet( beamFlags, BEAM_FSHADEOUT ) ) SetBits( beam.flags, FBEAM_SHADEOUT );

	R_BeamDraw( &beam, frametime );
}

/*
 * CL_DrawBeams - Draw all active server and client beams
 */
void CL_DrawBeams( int fTrans, BEAM *active_beams )
{
	// CL_DrawEFX calls this for both the solid and transparent pass; beams are all additive,
	// so draw them once to avoid submitting every beam twice per frame
	if( !fTrans )
		return;

	float dt = (float)tr.frametime > 0.0f ? (float)tr.frametime : 0.016f;

	// Server beams (env_beam): parameters live in the entity, no persistent beam chain
	for( uint32_t i = 0; i < g_bspState.num_beam_entities; i++ )
		R_BeamDrawCustomEntity( g_bspState.beam_entities[i], dt );

	for( BEAM *pBeam = active_beams; pBeam; pBeam = pBeam->next )
		R_BeamDraw( pBeam, dt );

	R_UI_TriFlush();
}

/*
 * CL_DrawParticles - Draw active particle system quads
 */
void CL_DrawParticles( double frametime, particle_t *particles, float partsize )
{
	if( !particles || !g_remix.CreateMesh || !g_remix.DrawInstance )
		return;

	// Count particles (up to 2048 drawn; the rest are still simulated below)
	uint32_t count = 0;
	for( particle_t *p = particles; p && count < 2048; p = p->next )
	{
		if( p->type != pt_blob || p->unused == 255 )
			count++;
	}

	if( count == 0 )
	{
		for( particle_t *p = particles; p; p = p->next )
		{
			if( gEngfuncs.CL_ThinkParticle )
				gEngfuncs.CL_ThinkParticle( frametime, p );
		}
		return;
	}

	uint32_t vertCount  = count * 4;
	uint32_t indexCount = count * 6;

	remixapi_HardcodedVertex *verts = (remixapi_HardcodedVertex *)Mem_Calloc( r_temppool, vertCount * sizeof( remixapi_HardcodedVertex ) );
	uint32_t *indices = (uint32_t *)Mem_Calloc( r_temppool, indexCount * sizeof( uint32_t ) );
	if( !verts || !indices )
	{
		if( verts ) Mem_Free( verts );
		if( indices ) Mem_Free( indices );
		return;
	}

	// Particle quads face the camera, so give the path tracer a valid normal (zeroed normals produce NaNs)
	vec3_t facing;
	VectorNegate( RI.vforward, facing );

	uint32_t cur = 0;
	for( particle_t *p = particles; p; p = p->next )
	{
		if( cur >= count || ( p->type == pt_blob && p->unused != 255 ) )
		{
			if( gEngfuncs.CL_ThinkParticle )
				gEngfuncs.CL_ThinkParticle( frametime, p );
			continue;
		}

		float size = partsize;
		float distToCam = ( p->org[0] - RI.vieworg[0] ) * RI.vforward[0] +
		                  ( p->org[1] - RI.vieworg[1] ) * RI.vforward[1] +
		                  ( p->org[2] - RI.vieworg[2] ) * RI.vforward[2];

		if( distToCam > 20.0f )
			size += distToCam * 0.002f;

		vec3_t right, up;
		VectorScale( RI.vright, size, right );
		VectorScale( RI.vup, size, up );

		int colorIdx = bound( 0, (int)p->color, 255 );
		color24 col = tr.palette ? tr.palette[colorIdx] : (color24){ 255, 255, 255 };

		double cltime = ( gp_cl ) ? gp_cl->time : 0.0;
		int alpha = (int)( 255.0f * ( p->die - (float)cltime ) * 16.0f );
		if( alpha > 255 || p->type == pt_static ) alpha = 255;
		if( alpha < 0 ) alpha = 0;

		uint32_t packedColor = ( (uint32_t)alpha << 24 ) | ( (uint32_t)col.r << 16 ) | ( (uint32_t)col.g << 8 ) | (uint32_t)col.b;

		uint32_t vBase = cur * 4;
		uint32_t iBase = cur * 6;

		// 4 Quad Vertices
		VectorSet( verts[vBase + 0].position, p->org[0] - right[0] + up[0], p->org[1] - right[1] + up[1], p->org[2] - right[2] + up[2] );
		verts[vBase + 0].texcoord[0] = 0.0f; verts[vBase + 0].texcoord[1] = 1.0f;
		verts[vBase + 0].color       = packedColor;

		VectorSet( verts[vBase + 1].position, p->org[0] + right[0] + up[0], p->org[1] + right[1] + up[1], p->org[2] + right[2] + up[2] );
		verts[vBase + 1].texcoord[0] = 0.0f; verts[vBase + 1].texcoord[1] = 0.0f;
		verts[vBase + 1].color       = packedColor;

		VectorSet( verts[vBase + 2].position, p->org[0] + right[0] - up[0], p->org[1] + right[1] - up[1], p->org[2] + right[2] - up[2] );
		verts[vBase + 2].texcoord[0] = 1.0f; verts[vBase + 2].texcoord[1] = 0.0f;
		verts[vBase + 2].color       = packedColor;

		VectorSet( verts[vBase + 3].position, p->org[0] - right[0] - up[0], p->org[1] - right[1] - up[1], p->org[2] - right[2] - up[2] );
		verts[vBase + 3].texcoord[0] = 1.0f; verts[vBase + 3].texcoord[1] = 1.0f;
		verts[vBase + 3].color       = packedColor;

		for( int k = 0; k < 4; k++ )
			VectorCopy( facing, verts[vBase + k].normal );

		indices[iBase + 0] = vBase + 0;
		indices[iBase + 1] = vBase + 1;
		indices[iBase + 2] = vBase + 2;
		indices[iBase + 3] = vBase + 0;
		indices[iBase + 4] = vBase + 2;
		indices[iBase + 5] = vBase + 3;

		if( gEngfuncs.CL_ThinkParticle )
			gEngfuncs.CL_ThinkParticle( frametime, p );
		cur++;
	}

	dynamic_mesh_slot_t *slot = R_AllocDynamicMeshSlot();
	if( slot->handle && g_remix.DestroyMesh )
	{
		g_remix.DestroyMesh( slot->handle );
		slot->handle = NULL;
	}

	// Cache the lookup: R_Textures_Find scans the whole texture table
	if( tr.particleTexture <= 0 )
		tr.particleTexture = R_Textures_Find( REF_PARTICLE_TEXTURE );
	int partTex = tr.particleTexture;

	remixapi_MeshInfoSurfaceTriangles surface;
	memset( &surface, 0, sizeof( surface ) );
	surface.vertices_values   = verts;
	surface.vertices_count    = vertCount;
	surface.indices_values    = indices;
	surface.indices_count     = indexCount;
	surface.skinning_hasvalue = 0;
	surface.material          = R_GetSpriteMaterial( partTex );

	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ) );
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash            = slot->hash;
	meshInfo.surfaces_values = &surface;
	meshInfo.surfaces_count  = 1;

	remixapi_ErrorCode err = g_remix.CreateMesh( &meshInfo, &slot->handle );
	if( err == REMIXAPI_ERROR_CODE_SUCCESS && slot->handle )
	{
		remixapi_InstanceRasterStateEXT rasterState;
		remixapi_InstanceInfoBlendEXT blendExt;
		remixapi_InstanceIdentityEXT identExt;
		R_SetupSpriteBlendState( &blendExt, &identExt, &rasterState, slot->hash, kRenderTransAlpha );

		remixapi_Transform identityTransform;
		memset( &identityTransform, 0, sizeof( identityTransform ) );
		identityTransform.matrix[0][0] = 1.0f;
		identityTransform.matrix[1][1] = 1.0f;
		identityTransform.matrix[2][2] = 1.0f;

		remixapi_InstanceInfo inst;
		memset( &inst, 0, sizeof( inst ) );
		inst.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		inst.pNext         = &identExt;
		inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
		inst.mesh          = slot->handle;
		inst.transform     = identityTransform;
		inst.doubleSided   = 1;

		g_remix.DrawInstance( &inst );
	}

	Mem_Free( verts );
	Mem_Free( indices );
}

// Tracer widths per particle type and built-in colours (index 4 follows the tracer* cvars)
static float   gTracerSize[11] = { 1.5f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static color24 gTracerColors[] =
{
	{ 255, 255, 255 }, // White
	{ 255, 0, 0 },     // Red
	{ 0, 255, 0 },     // Green
	{ 0, 0, 255 },     // Blue
	{ 0, 0, 0 },       // Tracer default, filled in from cvars
	{ 255, 167, 17 },  // Yellow-orange sparks
	{ 255, 130, 90 },  // Yellowish streaks (garg)
	{ 55, 60, 144 },   // Blue egon streak
	{ 255, 130, 90 },  // More yellowish streaks (garg)
	{ 255, 140, 90 },  // More yellowish streaks (garg)
	{ 200, 130, 90 },  // More red streaks (garg)
	{ 255, 120, 70 },  // Darker red streaks (garg)
};

/*
 * CL_DrawTracers - Draw and simulate tracers and spark streaks (port of the GL renderer's)
 */
void CL_DrawTracers( double frametime, particle_t *tracers )
{
	if( tracerred && tracergreen && tracerblue && traceralpha &&
	    FBitSet( tracerred->flags | tracergreen->flags | tracerblue->flags | traceralpha->flags, FCVAR_CHANGED ) )
	{
		color24 *custom = &gTracerColors[TRACER_COLORINDEX_DEFAULT];
		custom->r = (byte)( tracerred->value * traceralpha->value * 255 );
		custom->g = (byte)( tracergreen->value * traceralpha->value * 255 );
		custom->b = (byte)( tracerblue->value * traceralpha->value * 255 );
		ClearBits( tracerred->flags, FCVAR_CHANGED );
		ClearBits( tracergreen->flags, FCVAR_CHANGED );
		ClearBits( tracerblue->flags, FCVAR_CHANGED );
		ClearBits( traceralpha->flags, FCVAR_CHANGED );
	}

	if( !tracers || !gp_cl || !gp_movevars )
		return;

	// Additive dot sprite: goes to the path tracer as a self-illuminated streak
	if( !R_UI_TriSpriteTexture( gEngfuncs.GetDefaultSprite( REF_DOT_SPRITE ), 0 ) )
		return;

	float gravity = (float)frametime * gp_movevars->gravity;
	float scale = 1.0f - (float)frametime * 0.9f;
	if( scale < 0.0f ) scale = 0.0f;

	R_UI_TriRenderMode( kRenderTransAdd );
	// Sparks and tracers are white-hot: brighter than glows and beams
	R_UI_TriFxBoost( rt_tracer_emis ? rt_tracer_emis->value : 6.0f );

	for( particle_t *p = tracers; p; p = p->next )
	{
		float atten = p->die - (float)gp_cl->time;
		if( atten > 0.1f ) atten = 0.1f;

		vec3_t start, end, delta;
		VectorScale( p->vel, ( p->ramp * atten ), delta );
		VectorAdd( p->org, delta, end );
		VectorCopy( p->org, start );

		int type = bound( 0, (int)p->type, (int)( ARRAYSIZE( gTracerSize ) - 1 ) );

		vec3_t screen, screenLast;
		if( !R_UI_TriWorldToScreen( start, screen ) && !R_UI_TriWorldToScreen( end, screenLast ) )
		{
			// Width perpendicular to the streak's screen-space direction
			vec3_t tmp, tmp2, normal, verts[4];
			VectorSubtract( screen, screenLast, tmp );
			tmp[2] = 0.0f;
			VectorNormalize( tmp );

			VectorScale( RI.vup, tmp[0] * gTracerSize[type], normal );
			VectorScale( RI.vright, -tmp[1] * gTracerSize[type], tmp2 );
			VectorSubtract( normal, tmp2, normal );

			VectorSubtract( start, normal, verts[0] );
			VectorAdd( start, normal, verts[1] );
			VectorAdd( verts[0], delta, verts[2] );
			VectorAdd( verts[1], delta, verts[3] );

			if( p->color < 0 || p->color >= (int)ARRAYSIZE( gTracerColors ) )
				p->color = TRACER_COLORINDEX_DEFAULT;

			color24 color = gTracerColors[p->color];
			R_UI_TriColor4ub( color.r, color.g, color.b, (byte)bound( 0, p->unused, 255 ) );

			R_UI_TriBegin( TRI_QUADS );
			R_UI_TriTexCoord2f( 0.0f, 0.8f ); R_UI_TriVertex3fv( verts[2] );
			R_UI_TriTexCoord2f( 1.0f, 0.8f ); R_UI_TriVertex3fv( verts[3] );
			R_UI_TriTexCoord2f( 1.0f, 0.0f ); R_UI_TriVertex3fv( verts[1] );
			R_UI_TriTexCoord2f( 0.0f, 0.0f ); R_UI_TriVertex3fv( verts[0] );
			R_UI_TriEnd();
		}

		// Evaluate position
		VectorMA( p->org, (float)frametime, p->vel, p->org );

		if( p->type == pt_grav )
		{
			p->vel[0] *= scale;
			p->vel[1] *= scale;
			p->vel[2] -= gravity;

			int a = (int)( 255.0f * ( p->die - (float)gp_cl->time ) * 2.0f );
			p->unused = (short)bound( 0, a, 255 );
		}
		else if( p->type == pt_slowgrav )
		{
			p->vel[2] = gravity * 0.05f;
		}
	}

	R_UI_TriFlush();
	R_UI_TriFxBoost( 1.0f );
}
