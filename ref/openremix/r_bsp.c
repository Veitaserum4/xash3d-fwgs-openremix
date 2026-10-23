/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_bsp.c - BSP geometry extraction, triangulation, batching & instancing
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_bsp.h"
#include "entity_types.h"
#include "r_sprite.h"
#include "r_textures.h"

bsp_state_t g_bspState;

//
// 64-bit FNV-1a Hash Helper
//
static inline uint64_t R_HashString64( const char *str )
{
	uint64_t hash = 14695981039346656037ULL;
	if( !str ) return hash;
	while( *str )
	{
		hash ^= (uint64_t)(unsigned char)(*str++);
		hash *= 1099511628211ULL;
	}
	return hash ? hash : 1ULL; // OpenRemix forbids hash == 0
}

//
// Edge Position Extraction
//
static void R_BSP_GetEdgePosition( const model_t *mod, const msurface_t *fa, int i, vec3_t out_pos )
{
	const int lindex = mod->surfedges[fa->firstedge + i];

	if( mod->flags & MODEL_QBSP2 )
	{
		const medge32_t *pedges = mod->edges32;
		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, out_pos );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, out_pos );
	}
	else
	{
		const medge16_t *pedges = mod->edges16;
		if( lindex > 0 )
			VectorCopy( mod->vertexes[pedges[lindex].v[0]].position, out_pos );
		else
			VectorCopy( mod->vertexes[pedges[-lindex].v[1]].position, out_pos );
	}
}

//
// Collinear Point Elimination for BSP Convex Polygons
//
static int R_BSP_EliminateCollinear( int numVerts, vec3_t *polyPositions )
{
	int lnum = numVerts;
	for( int i = 0; i < lnum && lnum >= 3; i++ )
	{
		vec3_t v1, v2;
		const float *prev = polyPositions[( i + lnum - 1 ) % lnum];
		const float *curr = polyPositions[i];
		const float *next = polyPositions[( i + 1 ) % lnum];

		VectorSubtract( curr, prev, v1 );
		VectorSubtract( next, prev, v2 );

		float l1 = VectorLength( v1 );
		float l2 = VectorLength( v2 );

		if( l1 > 1e-4f && l2 > 1e-4f )
		{
			float inv1 = 1.0f / l1;
			float inv2 = 1.0f / l2;
			VectorScale( v1, inv1, v1 );
			VectorScale( v2, inv2, v2 );

			if( ( fabsf( v1[0] - v2[0] ) <= 0.001f ) &&
			    ( fabsf( v1[1] - v2[1] ) <= 0.001f ) &&
			    ( fabsf( v1[2] - v2[2] ) <= 0.001f ) )
			{
				for( int j = i + 1; j < lnum; j++ )
				{
					VectorCopy( polyPositions[j], polyPositions[j - 1] );
				}
				lnum--;
				i--;
			}
		}
	}
	return lnum;
}

//
// Texture UV Calculation
//
static void R_BSP_GetTextureCoords( const vec3_t pos, const msurface_t *surf, float *out_u, float *out_v )
{
	const mtexinfo_t *info = surf->texinfo;
	if( !info )
	{
		*out_u = 0.0f;
		*out_v = 0.0f;
		return;
	}

	float s = DotProduct( pos, info->vecs[0] ) + info->vecs[0][3];
	float t = DotProduct( pos, info->vecs[1] ) + info->vecs[1][3];

	float texW = ( info->texture && info->texture->width > 0 ) ? (float)info->texture->width : 16.0f;
	float texH = ( info->texture && info->texture->height > 0 ) ? (float)info->texture->height : 16.0f;

	*out_u = s / texW;
	*out_v = t / texH;
}

//
// Texture Batch Bucket Structure
//
typedef struct bsp_bucket_s
{
	texture_t                *texture;
	remixapi_MaterialHandle   material;

	remixapi_HardcodedVertex *verts;
	uint32_t                  numVerts;
	uint32_t                  allocVerts;

	uint32_t                 *indices;
	uint32_t                  numIndices;
	uint32_t                  allocIndices;
} bsp_bucket_t;

static void R_BSP_AppendVertex( bsp_bucket_t *bucket, const remixapi_HardcodedVertex *v )
{
	if( bucket->numVerts >= bucket->allocVerts )
	{
		uint32_t newAlloc = bucket->allocVerts ? ( bucket->allocVerts * 2 ) : 256;
		bucket->verts = (remixapi_HardcodedVertex *)Mem_Realloc( r_temppool, bucket->verts,
		                                                         newAlloc * sizeof( remixapi_HardcodedVertex ) );
		bucket->allocVerts = newAlloc;
	}
	bucket->verts[bucket->numVerts++] = *v;
}

static void R_BSP_AppendIndex( bsp_bucket_t *bucket, uint32_t idx )
{
	if( bucket->numIndices >= bucket->allocIndices )
	{
		uint32_t newAlloc = bucket->allocIndices ? ( bucket->allocIndices * 2 ) : 512;
		bucket->indices = (uint32_t *)Mem_Realloc( r_temppool, bucket->indices,
		                                           newAlloc * sizeof( uint32_t ) );
		bucket->allocIndices = newAlloc;
	}
	bucket->indices[bucket->numIndices++] = idx;
}

//
// Glass / Translucent Submodel Detection
//
static qboolean R_BSP_IsSubmodelWater( const model_t *submod )
{
	if( !submod || !submod->surfaces )
		return false;

	for( int s = 0; s < submod->nummodelsurfaces; s++ )
	{
		const msurface_t *surf = &submod->surfaces[submod->firstmodelsurface + s];
		if( surf->texinfo && surf->texinfo->texture && ( surf->texinfo->texture->name[0] == '!' || surf->texinfo->texture->name[0] == '*' ) )
			return true;
	}
	return false;
}

static qboolean R_BSP_IsSubmodelGlass( const model_t *submod, const char *subName )
{
	if( !submod )
		return false;

	// func_water: drawn with its water material (a refractive body), never as glass
	if( R_BSP_IsSubmodelWater( submod ) )
		return false;

	// 1. Check if any texture assigned to this submodel is recognized as glass
	if( submod->surfaces && submod->nummodelsurfaces > 0 )
	{
		for( int s = 0; s < submod->nummodelsurfaces; s++ )
		{
			const msurface_t *surf = &submod->surfaces[submod->firstmodelsurface + s];
			if( surf->texinfo && surf->texinfo->texture && surf->texinfo->texture->name )
			{
				if( R_Textures_IsGlass( surf->texinfo->texture->name ) )
					return true;
			}
		}
	}

	// 2. Parse map entities string for this submodel's entity definition
	if( tr.worldmodel && tr.worldmodel->entities && subName && subName[0] )
	{
		const char *ents = tr.worldmodel->entities;
		char targetModel[32];
		Q_snprintf( targetModel, sizeof( targetModel ), "\"model\" \"%s\"", subName );

		// Each entity that names this model, within its own braces
		for( const char *foundModel = strstr( ents, targetModel ); foundModel; foundModel = strstr( foundModel + 1, targetModel ))
		{
			const char *pos = foundModel;
			while( pos > ents && *pos != '{' )
				pos--;
			const char *end = strchr( foundModel, '}' );
			if( !end ) break;

			{
				// Check rendermode: 2 = kRenderTransTexture, 1 = kRenderTransColor
				const char *rm = strstr( pos, "\"rendermode\"" );
				if( rm && rm < end )
				{
					const char *val = rm + 12;
					while( *val == ' ' || *val == '\"' || *val == '\t' ) val++;
					int mode = atoi( val );
					if( mode == kRenderTransTexture || mode == kRenderTransColor )
						return true;
					// Additive/glow brushes are light effects, never glass (their renderamt is brightness)
					if( mode == kRenderTransAdd || mode == kRenderGlow )
						return false;
				}

				// Check material: 0 = matGlass in SDK, 7 = matGlass in Hammer / GoldSrc func_breakable
				const char *mat = strstr( pos, "\"material\"" );
				if( mat && mat < end )
				{
					const char *val = mat + 10;
					while( *val == ' ' || *val == '\"' || *val == '\t' ) val++;
					int matType = atoi( val );
					if( matType == 0 || matType == 7 )
						return true;
				}

				// Check renderamt
				const char *ra = strstr( pos, "\"renderamt\"" );
				if( ra && ra < end )
				{
					const char *val = ra + 11;
					while( *val == ' ' || *val == '\"' || *val == '\t' ) val++;
					int amt = atoi( val );
					if( amt > 0 && amt < 255 )
						return true;
				}
			}
		}
	}

	return false;
}

//
// Extract Surfaces and Build a Batched Mesh
//
static remixapi_MeshHandle R_BSP_BuildMeshFromSurfaces( const model_t *mod, int firstSurf, int numSurfs, uint64_t meshHash, qboolean isGlass )
{
	if( !mod || numSurfs <= 0 )
		return NULL;

	int numTextures = mod->numtextures;
	texture_t **textures = mod->textures;

	if( ( numTextures <= 0 || !textures ) && tr.worldmodel )
	{
		numTextures = tr.worldmodel->numtextures;
		textures = tr.worldmodel->textures;
	}

	if( numTextures <= 0 )
		numTextures = 1;

	// Allocate buckets for textures
	bsp_bucket_t *buckets = (bsp_bucket_t *)Mem_Calloc( r_temppool, numTextures * sizeof( bsp_bucket_t ) );
	if( !buckets )
		return NULL;

	// Populate buckets
	for( int s = 0; s < numSurfs; s++ )
	{
		const msurface_t *surf = &mod->surfaces[firstSurf + s];

		if( surf->numedges < 3 )
			continue;

		// Filter sky faces: sky rendered via environment / sky category
		if( surf->flags & SURF_DRAWSKY )
			continue;

		// Filter editor triggers
		if( surf->texinfo && surf->texinfo->texture )
		{
			const char *tname = surf->texinfo->texture->name;
			if( !Q_strnicmp( tname, "null", 4 ) ||
			    !Q_strnicmp( tname, "aaatrigger", 10 ) ||
			    !Q_strnicmp( tname, "clip", 4 ) )
			{
				continue;
			}
		}

		// Find bucket index
		int bucketIdx = 0;
		if( textures && surf->texinfo && surf->texinfo->texture )
		{
			for( int t = 0; t < numTextures; t++ )
			{
				if( textures[t] == surf->texinfo->texture )
				{
					bucketIdx = t;
					break;
				}
			}
			buckets[bucketIdx].texture = surf->texinfo->texture;
		}
		else if( surf->texinfo && surf->texinfo->texture )
		{
			buckets[0].texture = surf->texinfo->texture;
		}

		bsp_bucket_t *b = &buckets[bucketIdx];

		// Face normal
		vec3_t faceNormal;
		if( surf->flags & SURF_PLANEBACK )
			VectorNegate( surf->plane->normal, faceNormal );
		else
			VectorCopy( surf->plane->normal, faceNormal );

		// Extract polygon vertices
		int numVerts = surf->numedges;
		if( numVerts > 64 )
			numVerts = 64; // Sanity limit for bsp convex polygon

		vec3_t polyPositions[64];
		for( int i = 0; i < numVerts; i++ )
		{
			R_BSP_GetEdgePosition( mod, surf, i, polyPositions[i] );
		}

		// Eliminate collinear T-junction vertices to prevent zero-area degenerate triangles
		numVerts = R_BSP_EliminateCollinear( numVerts, polyPositions );
		if( numVerts < 3 )
			continue;

		// Compute geometric normal to align fan winding with outward face normal
		vec3_t e1, e2, geomNormal;
		VectorSubtract( polyPositions[1], polyPositions[0], e1 );
		VectorSubtract( polyPositions[2], polyPositions[0], e2 );
		CrossProduct( e1, e2, geomNormal );

		qboolean reverseWinding = ( DotProduct( geomNormal, faceNormal ) < 0.0f );

		// Base vertex offset in current bucket
		uint32_t baseVertex = b->numVerts;

		// Append vertices
		for( int i = 0; i < numVerts; i++ )
		{
			remixapi_HardcodedVertex v;
			memset( &v, 0, sizeof( v ) );
			v.position[0] = polyPositions[i][0];
			v.position[1] = polyPositions[i][1];
			v.position[2] = polyPositions[i][2];

			v.normal[0] = faceNormal[0];
			v.normal[1] = faceNormal[1];
			v.normal[2] = faceNormal[2];

			R_BSP_GetTextureCoords( polyPositions[i], surf, &v.texcoord[0], &v.texcoord[1] );
			v.color = 0xFFFFFFFF; // RGBA opaque white

			R_BSP_AppendVertex( b, &v );
		}

		// Triangle fan indices: wound counter-clockwise matching outward face normal
		for( int k = 0; k < numVerts - 2; k++ )
		{
			if( !reverseWinding )
			{
				R_BSP_AppendIndex( b, baseVertex + 0 );
				R_BSP_AppendIndex( b, baseVertex + k + 1 );
				R_BSP_AppendIndex( b, baseVertex + k + 2 );
			}
			else
			{
				R_BSP_AppendIndex( b, baseVertex + 0 );
				R_BSP_AppendIndex( b, baseVertex + k + 2 );
				R_BSP_AppendIndex( b, baseVertex + k + 1 );
			}
		}
	}

	// Count buckets with valid triangles
	uint32_t activeSurfaces = 0;
	for( int t = 0; t < numTextures; t++ )
	{
		if( buckets[t].numIndices > 0 && buckets[t].numVerts > 0 )
			activeSurfaces++;
	}

	if( activeSurfaces == 0 )
	{
		// Free bucket memory
		for( int t = 0; t < numTextures; t++ )
		{
			if( buckets[t].verts ) Mem_Free( buckets[t].verts );
			if( buckets[t].indices ) Mem_Free( buckets[t].indices );
		}
		Mem_Free( buckets );
		return NULL;
	}

	// Build remixapi_MeshInfoSurfaceTriangles array
	remixapi_MeshInfoSurfaceTriangles *surfaceTriangles =
		(remixapi_MeshInfoSurfaceTriangles *)Mem_Calloc( r_temppool, activeSurfaces * sizeof( remixapi_MeshInfoSurfaceTriangles ) );

	uint32_t sIdx = 0;
	for( int t = 0; t < numTextures; t++ )
	{
		if( buckets[t].numIndices == 0 || buckets[t].numVerts == 0 )
			continue;

		remixapi_MeshInfoSurfaceTriangles *st = &surfaceTriangles[sIdx++];
		st->vertices_values   = buckets[t].verts;
		st->vertices_count    = buckets[t].numVerts;
		st->indices_values    = buckets[t].indices;
		st->indices_count     = buckets[t].numIndices;
		st->skinning_hasvalue = 0;
		if( isGlass && !R_Textures_GlassTextured() )
		{
			st->material = R_Textures_GetGlassMaterial();
		}
		else
		{
			st->material = R_Textures_GetOrCreateForBSP( buckets[t].texture, mod );
		}
	}

	// Build master MeshInfo
	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ) );
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.pNext           = NULL;
	meshInfo.hash            = meshHash;
	meshInfo.surfaces_values = surfaceTriangles;
	meshInfo.surfaces_count  = activeSurfaces;

	uint32_t totalVerts = 0, totalIndices = 0;
	for( int t = 0; t < numTextures; t++ )
	{
		totalVerts += buckets[t].numVerts;
		totalIndices += buckets[t].numIndices;
	}

	remixapi_MeshHandle meshHandle = NULL;
	if( g_remix.CreateMeshBatched )
	{
		remixapi_ErrorCode err = g_remix.CreateMeshBatched( &meshInfo, &meshHandle );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: CreateMeshBatched failed with code %d\n", (int)err );
			meshHandle = NULL;
		}
		else
		{
			gEngfuncs.Con_Printf( "^2ref_openremix: BLAS built for %s: %u surfaces, %u verts, %u tris^7\n",
			                      mod->name, activeSurfaces, totalVerts, totalIndices / 3 );
		}
	}

	// Clean up temporary allocations
	for( int t = 0; t < numTextures; t++ )
	{
		if( buckets[t].verts ) Mem_Free( buckets[t].verts );
		if( buckets[t].indices ) Mem_Free( buckets[t].indices );
	}
	Mem_Free( buckets );
	Mem_Free( surfaceTriangles );

	return meshHandle;
}

//
// Subsystem Lifecycle Functions
//
void R_BSP_Init( void )
{
	memset( &g_bspState, 0, sizeof( g_bspState ) );
}

void R_BSP_Teardown( void )
{
	if( g_bspState.worldMesh && g_remix.DestroyMesh )
	{
		g_remix.DestroyMesh( g_bspState.worldMesh );
		g_bspState.worldMesh = NULL;
	}

	for( uint32_t i = 0; i < g_bspState.numSubmodels; i++ )
	{
		if( g_bspState.submodels[i].meshHandle && g_remix.DestroyMesh )
		{
			g_remix.DestroyMesh( g_bspState.submodels[i].meshHandle );
			g_bspState.submodels[i].meshHandle = NULL;
		}
	}
	g_bspState.numSubmodels = 0;
}

void R_BSP_Shutdown( void )
{
	R_BSP_Teardown();
}

static int s_brushLogCount = 0;

void R_BSP_NewMap( model_t *worldmodel )
{
	R_BSP_Teardown();
	s_brushLogCount = 0;

	if( !worldmodel )
		return;

	// 1. Build Static World Mesh
	uint64_t worldHash = R_HashString64( worldmodel->name );
	g_bspState.worldMeshHash = worldHash;
	g_bspState.worldMesh = R_BSP_BuildMeshFromSurfaces( worldmodel, 0, worldmodel->nummodelsurfaces, worldHash, false );

	gEngfuncs.Con_Reportf( "^2ref_openremix: Built static world BLAS for %s (handle %p, hash 0x%llx)^7\n",
	                       worldmodel->name, (void *)g_bspState.worldMesh, (unsigned long long)worldHash );

	// 2. Build Submodel Meshes (*1, *2, ...)
	if( worldmodel->numsubmodels > 1 )
	{
		for( int i = 1; i < worldmodel->numsubmodels && g_bspState.numSubmodels < MAX_BSP_SUBMODELS; i++ )
		{
			char subName[16];
			Q_snprintf( subName, sizeof( subName ), "*%d", i );

			// Locate model in engine model cache
			model_t *submod = NULL;
			if( gEngfuncs.Mod_ForName )
				submod = gEngfuncs.Mod_ForName( subName, false, false );

			if( !submod && gp_cl && gp_cl->models )
			{
				for( int m = 1; m < gp_cl->nummodels; m++ )
				{
					if( gp_cl->models[m] && !strcmp( gp_cl->models[m]->name, subName ) )
					{
						submod = gp_cl->models[m];
						break;
					}
				}
			}

			if( !submod || submod->nummodelsurfaces <= 0 )
				continue;

			char hashStr[128];
			Q_snprintf( hashStr, sizeof( hashStr ), "%s_%s", worldmodel->name, subName );
			uint64_t subHash = R_HashString64( hashStr );

			qboolean isGlass = R_BSP_IsSubmodelGlass( submod, subName );
			remixapi_MeshHandle subMesh = R_BSP_BuildMeshFromSurfaces( submod, submod->firstmodelsurface,
			                                                          submod->nummodelsurfaces, subHash, isGlass );
			if( subMesh )
			{
				bsp_submodel_mesh_t *entry = &g_bspState.submodels[g_bspState.numSubmodels++];
				entry->model      = submod;
				entry->meshHandle = subMesh;
				entry->hash       = subHash;
				entry->isGlass    = isGlass;
				Q_strncpy( entry->name, subName, sizeof( entry->name ) );
			}
		}

		gEngfuncs.Con_Reportf( "^2ref_openremix: Built %u brush submodel BLAS meshes^7\n", g_bspState.numSubmodels );
	}
}

//
// Submodel Lookup (with on-demand construction fallback)
//
remixapi_MeshHandle R_BSP_GetSubmodelMesh( model_t *mod )
{
	if( !mod || mod->type != mod_brush || mod->nummodelsurfaces <= 0 )
		return NULL;

	for( uint32_t i = 0; i < g_bspState.numSubmodels; i++ )
	{
		if( g_bspState.submodels[i].model == mod || !strcmp( g_bspState.submodels[i].name, mod->name ) )
			return g_bspState.submodels[i].meshHandle;
	}

	// On-demand build if not yet cached
	if( g_bspState.numSubmodels < MAX_BSP_SUBMODELS )
	{
		const char *wname = tr.worldmodel ? tr.worldmodel->name : "bsp";
		char hashStr[128];
		Q_snprintf( hashStr, sizeof( hashStr ), "%s_%s", wname, mod->name );
		uint64_t subHash = R_HashString64( hashStr );

		qboolean isGlass = R_BSP_IsSubmodelGlass( mod, mod->name );
		remixapi_MeshHandle subMesh = R_BSP_BuildMeshFromSurfaces( mod, mod->firstmodelsurface,
		                                                          mod->nummodelsurfaces, subHash, isGlass );
		if( subMesh )
		{
			bsp_submodel_mesh_t *entry = &g_bspState.submodels[g_bspState.numSubmodels++];
			entry->model      = mod;
			entry->meshHandle = subMesh;
			entry->hash       = subHash;
			entry->isGlass    = isGlass;
			Q_strncpy( entry->name, mod->name, sizeof( entry->name ) );
			gEngfuncs.Con_Reportf( "^2ref_openremix: Built brush submodel BLAS on-demand for %s (handle %p, glass=%d)^7\n",
			                       mod->name, (void *)subMesh, (int)isGlass );
			return subMesh;
		}
	}

	return NULL;
}

//
// Entity List Collection Hooks
//
int g_bspSceneCount = 1;

void R_BSP_ClearEntities( void )
{
	g_bspState.num_solid_entities = 0;
	g_bspState.num_trans_entities = 0;
	g_bspState.num_beam_entities = 0;
	g_bspSceneCount++;
}

qboolean R_BSP_AddEntity( cl_entity_t *clent, int type )
{
	if( !clent || !clent->model )
		return false;

	if( FBitSet( clent->curstate.effects, EF_NODRAW ) )
		return false;

	// Prevent duplicate entity additions within the same scene
	if( clent->visframe == g_bspSceneCount )
		return true;
	clent->visframe = g_bspSceneCount;

	// Server beams keep beam type/flags in rendermode and the sprite in modelindex: they are
	// not sprites to billboard, CL_DrawBeams rebuilds them as beams (see R_BeamDrawCustomEntity)
	if( type == ET_BEAM )
	{
		if( CL_FxBlend( clent ) <= 0 )
			return true;
		if( g_bspState.num_beam_entities < MAX_VISIBLE_PACKET )
		{
			g_bspState.beam_entities[g_bspState.num_beam_entities++] = clent;
			return true;
		}
		return false;
	}

	// Same rule as the GL renderer: Normal and TransAlpha (cutout grates) ignore renderamt,
	// every other mode at zero blend is invisible. Maps hide brushes this way (e.g. the
	// intact/broken states of a panel), so drawing them showed stray "glass" boxes.
	if( clent->curstate.rendermode != kRenderNormal && clent->curstate.rendermode != kRenderTransAlpha && CL_FxBlend( clent ) <= 0 )
		return true; // invisible, consumed

	if( clent->curstate.rendermode == kRenderNormal )
	{
		if( g_bspState.num_solid_entities < MAX_VISIBLE_PACKET )
		{
			g_bspState.solid_entities[g_bspState.num_solid_entities++] = clent;
			return true;
		}
	}
	else
	{
		if( g_bspState.num_trans_entities < MAX_VISIBLE_PACKET )
		{
			g_bspState.trans_entities[g_bspState.num_trans_entities++] = clent;
			return true;
		}
	}

	return false;
}

//
// Rendering & Instancing
//
void R_BSP_DrawWorld( void )
{
	if( !g_bspState.worldMesh || !g_remix.DrawInstance )
		return;

	// Identity transform in affine 3x4 row-major format
	remixapi_Transform identityTransform = {
		.matrix = {
			{ 1.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
		}
	};

	remixapi_InstanceIdentityEXT identityExt;
	memset( &identityExt, 0, sizeof( identityExt ) );
	identityExt.sType          = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
	identityExt.pNext          = NULL;
	identityExt.instanceId     = g_bspState.worldMeshHash ? g_bspState.worldMeshHash : 1ULL;
	identityExt.classification = REMIXAPI_INSTANCE_STATIC_EXT;
	identityExt.rasterVisible  = 1;

	remixapi_InstanceInfo instance;
	memset( &instance, 0, sizeof( instance ) );
	instance.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.pNext         = &identityExt;
	instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
	instance.mesh          = g_bspState.worldMesh;
	instance.transform     = identityTransform;
	instance.doubleSided   = 1;

	remixapi_ErrorCode err = g_remix.DrawInstance( &instance );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		static int s_worldDrawErrors = 0;
		if( ++s_worldDrawErrors <= 5 )
		{
			gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: DrawInstance for world failed: %s (%d)\n",
			                       R_Remix_GetErrorString( err ), (int)err );
		}
	}
}

void R_BSP_DrawBrushEntity( cl_entity_t *ent )
{
	if( !ent || !ent->model || ent->model->type != mod_brush )
		return;

	remixapi_MeshHandle subMesh = R_BSP_GetSubmodelMesh( ent->model );
	if( !subMesh || !g_remix.DrawInstance )
		return;

	// Check if this entity should be rendered with ray-traced translucent glass
	qboolean isGlass = false;
	qboolean isAdditive = ( ent->curstate.rendermode == kRenderTransAdd || ent->curstate.rendermode == kRenderGlow );
	qboolean isWater = R_BSP_IsSubmodelWater( ent->model );
	if( isWater )
	{
		// Water bodies get their look from the water material, not the entity's blending
		isAdditive = false;
	}
	else if( isAdditive )
	{
		// Light effects (e.g. the resonance cascade column), never glass
	}
	else if( ent->curstate.rendermode == kRenderTransTexture || ent->curstate.rendermode == kRenderTransColor )
	{
		isGlass = true;
	}
	else
	{
		// Decided once when the submodel's mesh was built (R_BSP_GetSubmodelMesh): the check
		// scans the map's entity text, far too slow to repeat for every brush every frame
		for( uint32_t i = 0; i < g_bspState.numSubmodels; i++ )
		{
			if( g_bspState.submodels[i].model == ent->model || !strcmp( g_bspState.submodels[i].name, ent->model->name ) )
			{
				isGlass = g_bspState.submodels[i].isGlass;
				break;
			}
		}
	}

	// Build 4x4 transform from entity orientation
	vec3_t angles;
	if( VectorIsNull( ent->angles ) )
		VectorClear( angles );
	else
		VectorCopy( ent->angles, angles );

	matrix4x4 objMatrix;
	Matrix4x4_CreateFromEntity( objMatrix, angles, ent->origin, 1.0f );

	// Convert to 3x4 row-major remixapi_Transform
	remixapi_Transform transform;
	for( int r = 0; r < 3; r++ )
	{
		for( int c = 0; c < 4; c++ )
		{
			transform.matrix[r][c] = objMatrix[r][c];
		}
	}

	uint64_t entityInstanceId = R_HashString64( ent->model->name ) ^
	                            ( ( (uint64_t)(uintptr_t)ent ) << 16 ) ^
	                            ( ( (uint64_t)ent->index ) << 48 );
	if( !entityInstanceId )
		entityInstanceId = 0x8000000000000000ULL | (uint64_t)(uintptr_t)ent;

	static qboolean s_loggedBrushes[MAX_BSP_SUBMODELS] = { 0 };
	int subIdx = 0;
	if( ent->model->name[0] == '*' )
		subIdx = atoi( ent->model->name + 1 );
	if( subIdx >= 0 && subIdx < MAX_BSP_SUBMODELS && !s_loggedBrushes[subIdx] )
	{
		s_loggedBrushes[subIdx] = true;
		gEngfuncs.Con_Printf( "^2ref_openremix: Drawing brush %s (ent=%p, idx=%d, org=[%.1f, %.1f, %.1f], rendermode=%d, isGlass=%d)^7\n",
		                      ent->model->name, (void *)ent, ent->index,
		                      ent->origin[0], ent->origin[1], ent->origin[2],
		                      ent->curstate.rendermode, (int)isGlass );
	}

	remixapi_InstanceIdentityEXT identityExt;
	memset( &identityExt, 0, sizeof( identityExt ) );
	identityExt.sType          = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
	identityExt.pNext          = NULL;
	identityExt.instanceId     = entityInstanceId;
	identityExt.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
	identityExt.rasterVisible  = 1;

	remixapi_InstanceInfoMaterialEXT matExt;
	memset( &matExt, 0, sizeof( matExt ) );
	matExt.sType    = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
	matExt.pNext    = &identityExt;
	matExt.material = R_Textures_GetGlassMaterial();

	remixapi_InstanceInfoBlendEXT blendExt;
	memset( &blendExt, 0, sizeof( blendExt ) );
	blendExt.sType                = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
	blendExt.pNext                = &identityExt;
	blendExt.alphaBlendEnabled    = 1;
	blendExt.srcColorBlendFactor  = 6; // D3DBLEND_SRCALPHA
	blendExt.dstColorBlendFactor  = 7; // D3DBLEND_INVSRCALPHA
	blendExt.colorBlendOp         = 0; // D3DBLENDOP_ADD
	blendExt.srcAlphaBlendFactor  = 1; // D3DBLEND_ONE
	blendExt.dstAlphaBlendFactor  = 7; // D3DBLEND_INVSRCALPHA
	blendExt.alphaBlendOp         = 0; // D3DBLENDOP_ADD
	blendExt.writeMask            = 15; // RGBA write enabled

	// Additive brushes (beams, glows, energy columns): the path tracer only knows coverage
	// blending, so draw them as self-illuminated surfaces with renderamt as coverage.
	// They glow and light the scene; without a usable texture fall back to a raster overlay.
	remixapi_InstanceRasterStateEXT addRaster;
	remixapi_InstanceInfoBlendEXT addBlend;
	remixapi_InstanceIdentityEXT addIdent;
	remixapi_InstanceInfoMaterialEXT addMat;
	if( isAdditive )
	{
		int amt = CL_FxBlend( ent );
		if( amt <= 0 )
			return;

		remixapi_MaterialHandle emisMat = NULL;
		const model_t *mod = ent->model;
		if( mod->surfaces && mod->nummodelsurfaces > 0 )
		{
			const msurface_t *surf = &mod->surfaces[mod->firstmodelsurface];
			if( surf->texinfo && surf->texinfo->texture )
				emisMat = R_Textures_GetEmissiveMaterial( (unsigned int)surf->texinfo->texture->gl_texturenum );
		}

		if( emisMat )
		{
			R_SetupSpriteBlendState( &addBlend, &addIdent, &addRaster, entityInstanceId, kRenderTransAlpha );
			addIdent.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;

			memset( &addMat, 0, sizeof( addMat ) );
			addMat.sType    = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
			addMat.pNext    = &addIdent;
			addMat.material = emisMat;
		}
		else
		{
			R_SetupSpriteBlendState( &addBlend, &addIdent, &addRaster, entityInstanceId, kRenderTransAdd );
			memset( &addMat, 0, sizeof( addMat ) );
		}

		addBlend.tFactor                = ( (uint32_t)bound( 0, amt, 255 ) << 24 ) | 0x00FFFFFFu;
		addBlend.textureColorArg2Source = 3; // Factor
		addBlend.textureAlphaArg2Source = 3; // Factor
	}

	remixapi_InstanceInfo instance;
	memset( &instance, 0, sizeof( instance ) );
	instance.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	if( isAdditive )
	{
		instance.pNext = addMat.sType ? (void *)&addMat : (void *)&addIdent;
	}
	else if( isGlass && R_Textures_GlassTextured() )
	{
		// Textured glass: the brush's own textures, coverage from renderamt as in GoldSrc
		int amt = ( ent->curstate.rendermode == kRenderTransTexture || ent->curstate.rendermode == kRenderTransColor ) ? CL_FxBlend( ent ) : 255;
		R_SetupSpriteBlendState( &addBlend, &addIdent, &addRaster, entityInstanceId, kRenderTransAlpha );
		addIdent.classification         = REMIXAPI_INSTANCE_DYNAMIC_EXT;
		// Glass-named textures already carry rt_glass_texture opacity in their material:
		// divide it out so renderamt alone decides how opaque the entity is
		amt = (int)( amt / R_Textures_GlassOpacity() );
		addBlend.tFactor                = ( (uint32_t)bound( 1, amt, 255 ) << 24 ) | 0x00FFFFFFu;
		addBlend.textureAlphaArg1Source = 3; // Factor (texture alpha is opaque for glass)
		addBlend.textureAlphaOperation  = 1; // Select arg 1
		instance.pNext = &addIdent;
	}
	else if( isGlass )
	{
		// Ray-traced translucent glass: use material override without blendExt
		// so OpenRemix path tracer treats it as P::Transmission (refraction, reflection, transmission tint)
		instance.pNext = &matExt;
	}
	else if( !isWater && ent->curstate.rendermode != kRenderNormal && ent->curstate.rendermode != kRenderTransAlpha )
	{
		// Non-glass blending (e.g. additive glows)
		instance.pNext = &blendExt;
	}
	else
	{
		// Solid or alpha-tested cutout (e.g. grates)
		instance.pNext = &identityExt;
	}
	instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
	instance.mesh          = subMesh;
	instance.transform     = transform;
	instance.doubleSided   = 1;

	remixapi_ErrorCode err = g_remix.DrawInstance( &instance );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		static int s_brushDrawErrors = 0;
		if( ++s_brushDrawErrors <= 5 )
		{
			gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: DrawInstance for brush %s failed: %s (%d)\n",
			                       ent->model->name, R_Remix_GetErrorString( err ), (int)err );
		}
	}
}

void R_BSP_DrawEntities( void )
{
	// Draw solid brush entities
	for( uint32_t i = 0; i < g_bspState.num_solid_entities; i++ )
	{
		cl_entity_t *e = g_bspState.solid_entities[i];
		if( e && e->model && e->model->type == mod_brush && e->model != tr.worldmodel )
		{
			R_BSP_DrawBrushEntity( e );
		}
	}

	// Draw translucent brush entities
	for( uint32_t i = 0; i < g_bspState.num_trans_entities; i++ )
	{
		cl_entity_t *e = g_bspState.trans_entities[i];
		if( e && e->model && e->model->type == mod_brush && e->model != tr.worldmodel )
		{
			R_BSP_DrawBrushEntity( e );
		}
	}
}
