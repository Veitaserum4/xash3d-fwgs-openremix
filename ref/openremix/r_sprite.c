/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_sprite.c - Sprites, Beams & Temp Entities Subsystem Implementation
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_sprite.h"
#include "sprite.h"

#define NOISE_DIVISIONS   64
#define GLARE_FALLOFF     19000.0f
#define HASH_UNIT_QUAD    0x53505254554E4954ULL // "SPRTUNIT"
#define HASH_DYNAMIC_BASE 0xD700000000000000ULL

// Static unit billboard quad mesh handle
static remixapi_MeshHandle g_unitQuadMesh = NULL;
static remixapi_MaterialHandle g_whiteMaterial = NULL;

// Dynamic mesh ring buffer for procedural beams and particle clouds
typedef struct dynamic_mesh_slot_s
{
	remixapi_MeshHandle handle;
	uint64_t            hash;
} dynamic_mesh_slot_t;

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
static remixapi_MaterialHandle R_GetSpriteMaterial( int texnum )
{
	(void)texnum;
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
void R_Sprite_Init( void )
{
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
static dynamic_mesh_slot_t *R_AllocDynamicMeshSlot( void )
{
	if( g_dynamicMeshIndex >= MAX_DYNAMIC_MESH_SLOTS )
		g_dynamicMeshIndex = 0;

	dynamic_mesh_slot_t *slot = &g_dynamicMeshes[g_dynamicMeshIndex];
	slot->hash = HASH_DYNAMIC_BASE + (uint64_t)g_dynamicMeshIndex;
	g_dynamicMeshIndex++;

	return slot;
}

/*
 * Helper: Configure blend and sprite soft-depth state
 */
static void R_SetupSpriteBlendState( remixapi_InstanceInfoBlendEXT *pBlend,
                                     remixapi_InstanceIdentityEXT *pIdent,
                                     remixapi_InstanceInfoSpriteEXT *pSprite,
                                     int rendermode )
{
	memset( pBlend, 0, sizeof( *pBlend ) );
	pBlend->sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
	pBlend->writeMask = 15;
	pBlend->alphaTestCompareOp = 7; // ALWAYS pass test

	memset( pIdent, 0, sizeof( *pIdent ) );
	pIdent->sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
	pIdent->pNext = pBlend;
	pIdent->rasterVisible = 1;

	memset( pSprite, 0, sizeof( *pSprite ) );
	pSprite->sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT;
	pSprite->pNext = pIdent;
	pSprite->flags = REMIXAPI_SPRITE_SOFT_DEPTH_EXT;
	pSprite->softDistance = 0.4f;

	switch( rendermode )
	{
	case kRenderTransAdd:
	case kRenderGlow:
		pBlend->alphaBlendEnabled   = 1;
		pBlend->srcColorBlendFactor = 6; // VK_BLEND_FACTOR_SRC_ALPHA
		pBlend->dstColorBlendFactor = 1; // VK_BLEND_FACTOR_ONE
		pBlend->srcAlphaBlendFactor = 6;
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

/*
 * R_DrawSpriteModel - Main sprite rendering callback
 */
void R_DrawSpriteModel( cl_entity_t *e )
{
	if( !e || !e->model || e->model->type != mod_sprite || !e->model->cache.data )
		return;

	if( !g_unitQuadMesh || !g_remix.DrawInstance )
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

	// 4. Configure Blend and Soft-depth Extensions
	remixapi_InstanceInfoBlendEXT blendExt;
	remixapi_InstanceIdentityEXT identExt;
	remixapi_InstanceInfoSpriteEXT spriteExt;
	R_SetupSpriteBlendState( &blendExt, &identExt, &spriteExt, e->curstate.rendermode );

	// 5. Submit Instance to OpenRemix
	remixapi_InstanceInfo inst;
	memset( &inst, 0, sizeof( inst ) );
	inst.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	inst.pNext         = &spriteExt;
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
	if( segments < 2 || !g_remix.CreateMesh || !g_remix.DrawInstance )
		return;

	if( segments > NOISE_DIVISIONS )
		segments = NOISE_DIVISIONS;

	float length = VectorLength( delta ) * 0.01f;
	if( length < 0.5f ) length = 0.5f;

	float div = 1.0f / (float)( segments - 1 );
	float vStep = length * div;
	float vLast = fmodf( freq * speed, 1.0f );
	float scale = amplitude * length;

	int noiseStep = (int)( (float)( NOISE_DIVISIONS - 1 ) * div * 65536.0f );
	int noiseIndex = 0;

	vec3_t perp1;
	R_BeamComputeNormal( source, delta, perp1 );

	// Build dynamic vertices and indices
	uint32_t vertCount = (uint32_t)segments * 2;
	uint32_t indexCount = (uint32_t)( segments - 1 ) * 6;

	remixapi_HardcodedVertex verts[NOISE_DIVISIONS * 2];
	uint32_t indices[NOISE_DIVISIONS * 6];
	memset( verts, 0, sizeof( verts ) );

	vec3_t curSegPos, nextSegPos, vNormal, vPoint1, vPoint2;
	VectorCopy( source, curSegPos );

	uint32_t packedColor = ( (uint32_t)bound( 0, (int)( brightness * 255.0f ), 255 ) << 24 ) |
	                       ( (uint32_t)bound( 0, (int)( b * brightness * 255.0f ), 255 ) << 16 ) |
	                       ( (uint32_t)bound( 0, (int)( g * brightness * 255.0f ), 255 ) << 8 ) |
	                       (uint32_t)bound( 0, (int)( r * brightness * 255.0f ), 255 );

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
		VectorMA( nextSegPos, ( width * 0.5f ), vNormal, vPoint1 );
		VectorMA( nextSegPos, ( -width * 0.5f ), vNormal, vPoint2 );

		// Vertex 2*i + 0
		VectorCopy( vPoint1, verts[i * 2 + 0].position );
		VectorCopy( vNormal, verts[i * 2 + 0].normal );
		verts[i * 2 + 0].texcoord[0] = 0.0f;
		verts[i * 2 + 0].texcoord[1] = vLast;
		verts[i * 2 + 0].color       = packedColor;

		// Vertex 2*i + 1
		VectorCopy( vPoint2, verts[i * 2 + 1].position );
		VectorCopy( vNormal, verts[i * 2 + 1].normal );
		verts[i * 2 + 1].texcoord[0] = 1.0f;
		verts[i * 2 + 1].texcoord[1] = vLast;
		verts[i * 2 + 1].color       = packedColor;

		VectorCopy( nextSegPos, curSegPos );
		vLast += vStep;
		noiseIndex += noiseStep;
	}

	// Triangulate strip into triangle indices
	for( int i = 0; i < segments - 1; i++ )
	{
		uint32_t i0 = (uint32_t)i * 2 + 0;
		uint32_t i1 = (uint32_t)i * 2 + 1;
		uint32_t i2 = (uint32_t)( i + 1 ) * 2 + 0;
		uint32_t i3 = (uint32_t)( i + 1 ) * 2 + 1;

		indices[i * 6 + 0] = i0;
		indices[i * 6 + 1] = i1;
		indices[i * 6 + 2] = i2;
		indices[i * 6 + 3] = i1;
		indices[i * 6 + 4] = i3;
		indices[i * 6 + 5] = i2;
	}

	// Allocate dynamic mesh slot
	dynamic_mesh_slot_t *slot = R_AllocDynamicMeshSlot();
	if( slot->handle && g_remix.DestroyMesh )
	{
		g_remix.DestroyMesh( slot->handle );
		slot->handle = NULL;
	}

	remixapi_MeshInfoSurfaceTriangles surface;
	memset( &surface, 0, sizeof( surface ) );
	surface.vertices_values   = verts;
	surface.vertices_count    = vertCount;
	surface.indices_values    = indices;
	surface.indices_count     = indexCount;
	surface.skinning_hasvalue = 0;
	surface.material          = R_GetSpriteMaterial( 0 );

	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ) );
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash            = slot->hash;
	meshInfo.surfaces_values = &surface;
	meshInfo.surfaces_count  = 1;

	remixapi_ErrorCode err = g_remix.CreateMesh( &meshInfo, &slot->handle );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS || !slot->handle )
		return;

	// Submit Instance with Additive Blend and BEAM category
	remixapi_InstanceInfoBlendEXT blendExt;
	remixapi_InstanceIdentityEXT identExt;
	remixapi_InstanceInfoSpriteEXT spriteExt;
	R_SetupSpriteBlendState( &blendExt, &identExt, &spriteExt, kRenderTransAdd );

	remixapi_Transform identityTransform;
	memset( &identityTransform, 0, sizeof( identityTransform ) );
	identityTransform.matrix[0][0] = 1.0f;
	identityTransform.matrix[1][1] = 1.0f;
	identityTransform.matrix[2][2] = 1.0f;

	remixapi_InstanceInfo inst;
	memset( &inst, 0, sizeof( inst ) );
	inst.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	inst.pNext         = &spriteExt;
	inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_BEAM;
	inst.mesh          = slot->handle;
	inst.transform     = identityTransform;
	inst.doubleSided   = 1;

	g_remix.DrawInstance( &inst );
}

/*
 * Draw single beam
 */
void R_BeamDraw( BEAM *pbeam, float frametime )
{
	if( !pbeam || !FBitSet( pbeam->flags, FBEAM_ISACTIVE ) )
		return;

	vec3_t delta;
	VectorSubtract( pbeam->target, pbeam->source, delta );
	if( VectorLength( delta ) < 0.1f )
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

	R_DrawSegs( pbeam->source, delta, pbeam->width, pbeam->amplitude,
	            pbeam->freq, pbeam->speed, segments, pbeam->flags,
	            pbeam->r, pbeam->g, pbeam->b, brightness );
}

/*
 * CL_DrawBeams - Draw all active server and client beams
 */
void CL_DrawBeams( int fTrans, BEAM *active_beams )
{
	(void)fTrans;

	for( BEAM *pBeam = active_beams; pBeam; pBeam = pBeam->next )
	{
		float dt = (float)tr.frametime > 0.0f ? (float)tr.frametime : 0.016f;
		R_BeamDraw( pBeam, dt );
	}
}

/*
 * CL_DrawParticles - Draw active particle system quads
 */
void CL_DrawParticles( double frametime, particle_t *particles, float partsize )
{
	if( !particles || !g_remix.CreateMesh || !g_remix.DrawInstance )
		return;

	// Count particles (up to 2048)
	uint32_t count = 0;
	for( particle_t *p = particles; p && count < 2048; p = p->next )
	{
		if( p->type != pt_blob || p->unused == 255 )
			count++;
	}

	if( count == 0 )
		return;

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

	uint32_t cur = 0;
	for( particle_t *p = particles; p && cur < count; p = p->next )
	{
		if( p->type == pt_blob && p->unused != 255 )
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

		uint32_t packedColor = ( (uint32_t)alpha << 24 ) | ( (uint32_t)col.b << 16 ) | ( (uint32_t)col.g << 8 ) | (uint32_t)col.r;

		uint32_t vBase = cur * 4;
		uint32_t iBase = cur * 6;

		// 4 Quad Vertices
		VectorSet( verts[vBase + 0].position, p->org[0] - right[0] + up[0], p->org[1] - right[1] + up[1], p->org[2] - right[2] + up[2] );
		verts[vBase + 0].texcoord[0] = 0.0f; verts[vBase + 0].texcoord[1] = 1.0f;
		verts[vBase + 0].color       = packedColor;

		VectorSet( verts[vBase + 1].position, p->org[0] + right[0] + up[0], p->org[1] + right[1] + up[1], p->org[2] + right[2] + up[2] );
		verts[vBase + 1].texcoord[0] = 0.0f; verts[vBase + 1].texcoord[1] = 0.0f;
		verts[vBase + 1].color       = packedColor;

		VectorSet( verts[vBase + 2].position, p->org[0] + right[0] - up[0], p->org[1] - right[1] - up[1], p->org[2] - right[2] - up[2] );
		verts[vBase + 2].texcoord[0] = 1.0f; verts[vBase + 2].texcoord[1] = 0.0f;
		verts[vBase + 2].color       = packedColor;

		VectorSet( verts[vBase + 3].position, p->org[0] - right[0] - up[0], p->org[1] - right[1] - up[1], p->org[2] - right[2] - up[2] );
		verts[vBase + 3].texcoord[0] = 1.0f; verts[vBase + 3].texcoord[1] = 1.0f;
		verts[vBase + 3].color       = packedColor;

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

	remixapi_MeshInfoSurfaceTriangles surface;
	memset( &surface, 0, sizeof( surface ) );
	surface.vertices_values   = verts;
	surface.vertices_count    = vertCount;
	surface.indices_values    = indices;
	surface.indices_count     = indexCount;
	surface.skinning_hasvalue = 0;
	surface.material          = R_GetSpriteMaterial( 0 );

	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ) );
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash            = slot->hash;
	meshInfo.surfaces_values = &surface;
	meshInfo.surfaces_count  = 1;

	remixapi_ErrorCode err = g_remix.CreateMesh( &meshInfo, &slot->handle );
	if( err == REMIXAPI_ERROR_CODE_SUCCESS && slot->handle )
	{
		remixapi_InstanceInfoBlendEXT blendExt;
		remixapi_InstanceIdentityEXT identExt;
		remixapi_InstanceInfoSpriteEXT spriteExt;
		R_SetupSpriteBlendState( &blendExt, &identExt, &spriteExt, kRenderTransAlpha );

		remixapi_Transform identityTransform;
		memset( &identityTransform, 0, sizeof( identityTransform ) );
		identityTransform.matrix[0][0] = 1.0f;
		identityTransform.matrix[1][1] = 1.0f;
		identityTransform.matrix[2][2] = 1.0f;

		remixapi_InstanceInfo inst;
		memset( &inst, 0, sizeof( inst ) );
		inst.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		inst.pNext         = &spriteExt;
		inst.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
		inst.mesh          = slot->handle;
		inst.transform     = identityTransform;
		inst.doubleSided   = 1;

		g_remix.DrawInstance( &inst );
	}

	Mem_Free( verts );
	Mem_Free( indices );
}

/*
 * CL_DrawTracers - Draw gunshot tracers
 */
void CL_DrawTracers( double frametime, particle_t *tracers )
{
	(void)frametime;
	(void)tracers;
}
