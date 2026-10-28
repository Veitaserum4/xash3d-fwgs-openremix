/*
 * r_decals.c - Decals (blood, bullet holes, scorch marks, sprays) on brush surfaces
 *
 * Placement and clipping follow the GL renderer (ref/gl/gl_decals.c): a decal is clipped to each
 * surface it touches and linked to it (msurface_t::pdecals), so the engine can save and restore
 * them. The clipped polygons of a model's decals form one mesh, drawn as a decal instance: the
 * path tracer paints it over the surface beneath (alpha over), so it takes that surface's lighting
 */

#include "r_local.h"
#include "r_remix.h"
#include "r_decals.h"
#include "r_sprite.h"
#include "r_textures.h"

#ifndef MAX_RENDER_DECALS
#define MAX_RENDER_DECALS 4096
#endif

#define DECAL_OVERLAP_DISTANCE 2
#define DECAL_DISTANCE         4  // too big values produce more clipped polygons
#define MAX_DECALCLIPVERT      32 // produced vertexes of fragmented decal

// empirically determined constants for minimizing overlapping decals
#define MAX_OVERLAP_DECALS 6
#define MIN_DECAL_SCALE    0.01f
#define MAX_DECAL_SCALE    16.0f

// clip edges
#define LEFT_EDGE   0
#define RIGHT_EDGE  1
#define TOP_EDGE    2
#define BOTTOM_EDGE 3

// How far a decal's mesh stands off its surface (units): the path tracer resolves the decal
// against the surface below, this only keeps their triangles apart. It peels the paint over a
// surface front to back by distance, so decals stacked on one surface each stand a step further
// out than the one below (two at one distance would show only one)
#define DECAL_SURFACE_OFFSET 0.05f
#define DECAL_LAYER_STEP     0.02f
#define DECAL_MAX_LAYERS     16

typedef struct
{
	vec3_t   m_Position; // world coordinates of the decal center
	model_t *m_pModel;   // the model the decal is going to be applied in
	int      m_iTexture; // The decal material
	int      m_Size;     // Size of the decal (in world coords)
	int      m_Flags;
	int      m_Entity;   // Entity the decal is applied to.
	float    m_scale;
	int      m_decalWidth;
	int      m_decalHeight;
	vec3_t   m_Basis[3];
} decalinfo_t;

static float g_DecalClipVerts[MAX_DECALCLIPVERT][VERTEXSIZE];
static float g_DecalClipVerts2[MAX_DECALCLIPVERT][VERTEXSIZE];

static decal_t gDecalPool[MAX_RENDER_DECALS];
static int     gDecalCount;

// Meshes of the models' decals, rebuilt when any decal changes
#define MAX_DECAL_MESHES 256
typedef struct
{
	const model_t      *model;
	remixapi_MeshHandle mesh;
	uint32_t            generation;
	uint64_t            hash;
} decal_mesh_t;

static decal_mesh_t s_decalMeshes[MAX_DECAL_MESHES];
static int          s_numDecalMeshes;
static uint32_t     s_decalGeneration = 1; // bumped by every placed or removed decal
static uint64_t     s_meshSerial;

static cvar_t *r_decals_cvar;
static cvar_t *rt_decals;

static void R_DecalChanged( void )
{
	s_decalGeneration++;
}

// unlink pdecal from any surface it's attached to
static void R_DecalUnlink( decal_t *pdecal )
{
	if( pdecal->psurface )
	{
		if( pdecal->psurface->pdecals == pdecal )
		{
			pdecal->psurface->pdecals = pdecal->pnext;
		}
		else
		{
			decal_t *tmp = pdecal->psurface->pdecals;
			if( !tmp ) gEngfuncs.Host_Error( "%s: bad decal list\n", __func__ );

			while( tmp->pnext )
			{
				if( tmp->pnext == pdecal )
				{
					tmp->pnext = pdecal->pnext;
					break;
				}
				tmp = tmp->pnext;
			}
		}
		R_DecalChanged();
	}

	if( pdecal->polys )
		Mem_Free( pdecal->polys );

	pdecal->psurface = NULL;
	pdecal->polys = NULL;
}

// Just reuse next decal in list
// A decal that spans multiple surfaces will use multiple decal_t pool entries,
// as each surface needs it's own.
static decal_t *R_DecalAlloc( decal_t *pdecal )
{
	int limit = MAX_RENDER_DECALS;

	if( r_decals_cvar && r_decals_cvar->value < limit )
		limit = r_decals_cvar->value;

	if( limit <= 0 ) return NULL;

	if( !pdecal )
	{
		int count = 0;

		// check for the odd possiblity of infinte loop
		do
		{
			if( gDecalCount >= limit )
				gDecalCount = 0;

			pdecal = &gDecalPool[gDecalCount]; // reuse next decal
			gDecalCount++;
			count++;
		} while( FBitSet( pdecal->flags, FDECAL_PERMANENT ) && count < limit );
	}

	// if decal is already linked to a surface, unlink it.
	R_DecalUnlink( pdecal );

	return pdecal;
}

// find decal image and grab size from it
static void R_GetDecalDimensions( int texture, int *width, int *height )
{
	*width = *height = 1; // to avoid divide by zero

	if( texture > 0 && texture < MAX_REMIX_TEXTURES && g_remixTextures[texture].registered )
	{
		*width  = Q_max( 1, (int)g_remixTextures[texture].width );
		*height = Q_max( 1, (int)g_remixTextures[texture].height );
	}
}

// The surface's polygon, from the model's edges (the GL renderer keeps it in surf->polys)
static int R_DecalSurfaceVerts( const model_t *mod, const msurface_t *surf, float *verts, int maxVerts )
{
	const int numVerts = Q_min( surf->numedges, maxVerts );

	for( int i = 0; i < numVerts; i++, verts += VERTEXSIZE )
	{
		const int lindex = mod->surfedges[surf->firstedge + i];
		int vert;

		if( mod->flags & MODEL_QBSP2 )
			vert = lindex > 0 ? mod->edges32[lindex].v[0] : mod->edges32[-lindex].v[1];
		else
			vert = lindex > 0 ? mod->edges16[lindex].v[0] : mod->edges16[-lindex].v[1];

		VectorCopy( mod->vertexes[vert].position, verts );
	}
	return numVerts;
}

/*
===============
R_DecalFixMirroring

Texture axes of a BSP surface can be mirrored relative to the side the player sees. A decal that
follows these axes then shows up mirrored, which is unreadable for text and logos: flip one axis
===============
*/
static void R_DecalFixMirroring( const vec3_t normal, vec3_t s, vec3_t t )
{
	vec3_t cross;

	CrossProduct( s, t, cross );

	// non-mirrored mapping has S x T pointing into the surface
	if( DotProduct( cross, normal ) <= 0.001f )
		return;

	if( fabsf( normal[2] ) < 0.7071f && fabsf( s[2] ) > fabsf( t[2] ))
		VectorNegate( t, t );
	else
		VectorNegate( s, s );
}

static void R_DecalSurfaceNormal( const msurface_t *surf, vec3_t normal )
{
	if( surf->flags & SURF_PLANEBACK )
		VectorNegate( surf->plane->normal, normal );
	else
		VectorCopy( surf->plane->normal, normal );
}

// compute the decal basis based on surface normal
static void R_DecalComputeBasis( msurface_t *surf, int flags, vec3_t textureSpaceBasis[3] )
{
	vec3_t surfaceNormal;

	R_DecalSurfaceNormal( surf, surfaceNormal );
	VectorNormalize2( surfaceNormal, textureSpaceBasis[2] );
	VectorNormalize2( surf->texinfo->vecs[0], textureSpaceBasis[0] );
	VectorNormalize2( surf->texinfo->vecs[1], textureSpaceBasis[1] );

	// custom decals (player sprays) must stay readable on any surface
	if( FBitSet( flags, FDECAL_CUSTOM ))
		R_DecalFixMirroring( textureSpaceBasis[2], textureSpaceBasis[0], textureSpaceBasis[1] );
}

static void R_SetupDecalTextureSpaceBasis( decal_t *pDecal, msurface_t *surf, int texture, vec3_t textureSpaceBasis[3], float decalWorldScale[2] )
{
	int width, height;

	// Compute the non-scaled decal basis
	R_DecalComputeBasis( surf, pDecal->flags, textureSpaceBasis );
	R_GetDecalDimensions( texture, &width, &height );

	// world width of decal = ptexture->width / pDecal->scale
	// world height of decal = ptexture->height / pDecal->scale
	// scale is inverse, scales world space to decal u/v space [0,1]
	decalWorldScale[0] = (float)pDecal->scale / width;
	decalWorldScale[1] = (float)pDecal->scale / height;

	VectorScale( textureSpaceBasis[0], decalWorldScale[0], textureSpaceBasis[0] );
	VectorScale( textureSpaceBasis[1], decalWorldScale[1], textureSpaceBasis[1] );
}

// Figure out where the decal maps onto the surface.
static void R_SetupDecalClip( decal_t *pDecal, msurface_t *surf, int texture, vec3_t textureSpaceBasis[3], float decalWorldScale[2] )
{
	R_SetupDecalTextureSpaceBasis( pDecal, surf, texture, textureSpaceBasis, decalWorldScale );

	// Generate texture coordinates for each vertex in decal s,t space
	pDecal->dx = DotProduct( pDecal->position, textureSpaceBasis[0] );
	pDecal->dy = DotProduct( pDecal->position, textureSpaceBasis[1] );
}

// Quick and dirty sutherland Hodgman clipper: clip polygon to decal in texture space
static int R_ClipInside( float *vert, int edge )
{
	switch( edge )
	{
	case LEFT_EDGE:   return vert[3] > 0.0f;
	case RIGHT_EDGE:  return vert[3] < 1.0f;
	case TOP_EDGE:    return vert[4] > 0.0f;
	case BOTTOM_EDGE: return vert[4] < 1.0f;
	}
	return 0;
}

static void R_ClipIntersect( float *one, float *two, float *out, int edge )
{
	// t is the fraction of the clipped point between one & two
	// vert[0..2] is X, Y, Z, vert[3] is u, vert[4] is v
	float t;
	if( edge < TOP_EDGE )
	{
		if( edge == LEFT_EDGE )
		{
			t = (( one[3] - 0.0f ) / ( one[3] - two[3] ));
			out[3] = out[5] = 0.0f;
		}
		else
		{
			t = (( one[3] - 1.0f ) / ( one[3] - two[3] ));
			out[3] = out[5] = 1.0f;
		}

		out[4] = one[4] + ( two[4] - one[4] ) * t;
		out[6] = one[6] + ( two[6] - one[6] ) * t;
	}
	else
	{
		if( edge == TOP_EDGE )
		{
			t = (( one[4] - 0.0f ) / ( one[4] - two[4] ));
			out[4] = out[6] = 0.0f;
		}
		else
		{
			t = (( one[4] - 1.0f ) / ( one[4] - two[4] ));
			out[4] = out[6] = 1.0f;
		}

		out[3] = one[3] + ( two[3] - one[3] ) * t;
		out[5] = one[5] + ( two[5] - one[5] ) * t;
	}

	VectorLerp( one, t, two, out );
}

static int SHClip( float *vert, int vertCount, float *out, int edge )
{
	int outCount = 0;

	if( vertCount <= 0 )
		return 0;

	float *s = &vert[( vertCount - 1 ) * VERTEXSIZE];

	for( int j = 0; j < vertCount; j++ )
	{
		float *p = &vert[j * VERTEXSIZE];

		if( outCount >= MAX_DECALCLIPVERT - 2 )
			break;

		if( R_ClipInside( p, edge ))
		{
			if( R_ClipInside( s, edge ))
			{
				memcpy( out, p, sizeof( float ) * VERTEXSIZE );
				out += VERTEXSIZE;
				outCount++;
			}
			else
			{
				R_ClipIntersect( s, p, out, edge );
				out += VERTEXSIZE;
				outCount++;

				memcpy( out, p, sizeof( float ) * VERTEXSIZE );
				out += VERTEXSIZE;
				outCount++;
			}
		}
		else if( R_ClipInside( s, edge ))
		{
			R_ClipIntersect( p, s, out, edge );
			out += VERTEXSIZE;
			outCount++;
		}

		s = p;
	}

	return outCount;
}

static float *R_DoDecalSHClip( float *pInVerts, int nStartVerts, int *pVertCount )
{
	float *pOutVerts = g_DecalClipVerts[0];

	// clip the polygon to the decal texture space
	int outCount = SHClip( pInVerts, nStartVerts, g_DecalClipVerts2[0], LEFT_EDGE );
	outCount = SHClip( g_DecalClipVerts2[0], outCount, g_DecalClipVerts[0], RIGHT_EDGE );
	outCount = SHClip( g_DecalClipVerts[0], outCount, g_DecalClipVerts2[0], TOP_EDGE );
	outCount = SHClip( g_DecalClipVerts2[0], outCount, pOutVerts, BOTTOM_EDGE );

	*pVertCount = outCount;
	return pOutVerts;
}

// Generate clipped vertex list for decal pdecal projected onto polygon psurf
static float *R_DecalVertsClip( const model_t *mod, decal_t *pDecal, msurface_t *surf, int texture, int *pVertCount )
{
	float  decalWorldScale[2];
	vec3_t textureSpaceBasis[3];

	// figure out where the decal maps onto the surface.
	R_SetupDecalClip( pDecal, surf, texture, textureSpaceBasis, decalWorldScale );

	// build the initial list of vertices from the surface verts
	const int numVerts = R_DecalSurfaceVerts( mod, surf, g_DecalClipVerts[0], MAX_DECALCLIPVERT );
	float *v = g_DecalClipVerts[0];
	for( int i = 0; i < numVerts; i++, v += VERTEXSIZE )
	{
		v[3] = DotProduct( v, textureSpaceBasis[0] ) - pDecal->dx + 0.5f;
		v[4] = DotProduct( v, textureSpaceBasis[1] ) - pDecal->dy + 0.5f;
		v[5] = v[6] = 0.0f;
	}

	return R_DoDecalSHClip( g_DecalClipVerts[0], numVerts, pVertCount );
}

// Check for intersecting decals on this surface
static decal_t *R_DecalIntersect( decalinfo_t *decalinfo, msurface_t *surf, int *pcount )
{
	decal_t *plast = NULL;
	float lastArea = 2;
	*pcount = 0;

	// precalculate the extents of decalinfo's decal in world space.
	int mapSize[2];
	R_GetDecalDimensions( decalinfo->m_iTexture, &mapSize[0], &mapSize[1] );
	vec3_t decalExtents[2];
	VectorScale( decalinfo->m_Basis[0], (( mapSize[0] / decalinfo->m_scale ) * 0.5f ), decalExtents[0] );
	VectorScale( decalinfo->m_Basis[1], (( mapSize[1] / decalinfo->m_scale ) * 0.5f ), decalExtents[1] );

	for( decal_t *pDecal = surf->pdecals; pDecal; pDecal = pDecal->pnext )
	{
		// Don't steal permanent decals
		if( FBitSet( pDecal->flags, FDECAL_PERMANENT ))
			continue;

		vec3_t testBasis[3];
		vec3_t testPosition[2];
		float  testWorldScale[2];

		R_SetupDecalTextureSpaceBasis( pDecal, surf, pDecal->texture, testBasis, testWorldScale );

		VectorSubtract( decalinfo->m_Position, decalExtents[0], testPosition[0] );
		VectorSubtract( decalinfo->m_Position, decalExtents[1], testPosition[1] );

		// project the extents of the new decal into this decal's [0,0,1,1] clip space
		vec2_t vDecalMin = {
			DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f,
			DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f };

		VectorAdd( decalinfo->m_Position, decalExtents[0], testPosition[0] );
		VectorAdd( decalinfo->m_Position, decalExtents[1], testPosition[1] );

		vec2_t vDecalMax = {
			DotProduct( testPosition[0], testBasis[0] ) - pDecal->dx + 0.5f,
			DotProduct( testPosition[1], testBasis[1] ) - pDecal->dy + 0.5f };

		vec2_t vUnionMin = { Q_max( vDecalMin[0], 0 ), Q_max( vDecalMin[1], 0 ) };
		vec2_t vUnionMax = { Q_min( vDecalMax[0], 1 ), Q_min( vDecalMax[1], 1 ) };

		if( vUnionMin[0] < 1 && vUnionMin[1] < 1 && vUnionMax[0] > 0 && vUnionMax[1] > 0 )
		{
			// how much of this intersects the (0,0) - (1,1) bbox
			float flArea = ( vUnionMax[0] - vUnionMin[0] ) * ( vUnionMax[1] - vUnionMin[1] );

			if( flArea > 0.6f )
			{
				*pcount += 1;

				if( !plast || flArea <= lastArea )
				{
					plast = pDecal;
					lastArea = flArea;
				}
			}
		}
	}
	return plast;
}

// Keeps the decal's clipped polygon with it
static void R_DecalCreatePoly( const model_t *mod, decal_t *pdecal, msurface_t *surf )
{
	int lnumverts;
	float *v = R_DecalVertsClip( mod, pdecal, surf, pdecal->texture, &lnumverts );
	if( !lnumverts )
		return;

	glpoly2_t *poly = Mem_Calloc( r_temppool, sizeof( glpoly2_t ) + lnumverts * VERTEXSIZE * sizeof( float ));
	poly->next = NULL;
	poly->flags = surf->flags;
	poly->numverts = lnumverts;
	memcpy( poly->verts[0], v, lnumverts * VERTEXSIZE * sizeof( float ));
	pdecal->polys = poly;
}

// Add the decal to the surface's list of decals.
static void R_AddDecalToSurface( const model_t *mod, decal_t *pdecal, msurface_t *surf )
{
	pdecal->pnext = NULL;
	decal_t *pold = surf->pdecals;

	if( pold )
	{
		while( pold->pnext )
			pold = pold->pnext;
		pold->pnext = pdecal;
	}
	else
	{
		surf->pdecals = pdecal;
	}

	pdecal->psurface = surf;
	R_DecalCreatePoly( mod, pdecal, surf );
	R_DecalChanged();
}

static void R_DecalCreate( decalinfo_t *decalinfo, msurface_t *surf, float x, float y )
{
	int count;
	decal_t *pold = R_DecalIntersect( decalinfo, surf, &count );
	if( count < MAX_OVERLAP_DECALS ) pold = NULL;

	decal_t *pdecal = R_DecalAlloc( pold );
	if( !pdecal ) return; // r_decals 0

	pdecal->flags = decalinfo->m_Flags;
	VectorCopy( decalinfo->m_Position, pdecal->position );
	pdecal->dx = x;
	pdecal->dy = y;
	pdecal->scale = decalinfo->m_scale;
	pdecal->entityIndex = decalinfo->m_Entity;
	pdecal->texture = decalinfo->m_iTexture;

	// check to see if the decal actually intersects the surface
	int vertCount;
	R_DecalVertsClip( decalinfo->m_pModel, pdecal, surf, decalinfo->m_iTexture, &vertCount );
	if( !vertCount )
	{
		R_DecalUnlink( pdecal );
		return;
	}

	R_AddDecalToSurface( decalinfo->m_pModel, pdecal, surf );
}

static void R_DecalSurface( msurface_t *surf, decalinfo_t *decalinfo )
{
	mtexinfo_t *tex = surf->texinfo;
	connstate_t state = ENGINE_GET_PARM( PARM_CONNSTATE );

	// we in restore mode
	if( state == ca_connected || state == ca_validate )
	{
		// the decal may already be on this surface, from another level
		for( decal_t *decal = surf->pdecals; decal; decal = decal->pnext )
		{
			if( VectorCompare( decal->position, decalinfo->m_Position ) && decal->texture == decalinfo->m_iTexture )
				return;
		}
	}

	vec4_t textureU = Vec4( tex->vecs[0] );
	vec4_t textureV = Vec4( tex->vecs[1] );

	// project decal center into the texture space of the surface
	float s = DotProduct( decalinfo->m_Position, textureU ) + textureU[3] - surf->texturemins[0];
	float t = DotProduct( decalinfo->m_Position, textureV ) + textureV[3] - surf->texturemins[1];

	// Determine the decal basis (measured in world space)
	R_DecalComputeBasis( surf, decalinfo->m_Flags, decalinfo->m_Basis );

	// an effective width and height (axis aligned) in the parent texture space
	float w = fabs( decalinfo->m_decalWidth  * DotProduct( textureU, decalinfo->m_Basis[0] )) +
	          fabs( decalinfo->m_decalHeight * DotProduct( textureU, decalinfo->m_Basis[1] ));

	float h = fabs( decalinfo->m_decalWidth  * DotProduct( textureV, decalinfo->m_Basis[0] )) +
	          fabs( decalinfo->m_decalHeight * DotProduct( textureV, decalinfo->m_Basis[1] ));

	// move s,t to upper left corner
	s -= ( w * 0.5f );
	t -= ( h * 0.5f );

	// Is this rect within the surface?
	if( s <= -w || t <= -h || s > ( surf->extents[0] + w ) || t > ( surf->extents[1] + h ))
		return;

	R_DecalCreate( decalinfo, surf, s, t );
}

// iterate over all surfaces on a node, looking for surfaces to decal
static void R_DecalNodeSurfaces( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	int firstsurface = node_firstsurface( node, model );
	int numsurfaces  = node_numsurfaces( node, model );

	msurface_t *surf = model->surfaces + firstsurface;

	for( int i = 0; i < numsurfaces; i++, surf++ )
	{
		// never apply decals on the water or sky surfaces, nor on see-through ones (grates):
		// the GL renderer masks those with the stencil, here they would cover the holes
		if( surf->flags & ( SURF_DRAWTURB | SURF_DRAWSKY | SURF_CONVEYOR | SURF_TRANSPARENT ))
			continue;

		R_DecalSurface( surf, decalinfo );
	}
}

// Recursive routine to find surface to apply a decal to
static void R_DecalNode( model_t *model, mnode_t *node, decalinfo_t *decalinfo )
{
	if( node->contents < 0 )
		return; // hit a leaf

	mplane_t *splitplane = node->plane;
	float dist = DotProduct( decalinfo->m_Position, splitplane->normal ) - splitplane->dist;

	if( dist > decalinfo->m_Size )
	{
		R_DecalNode( model, node_child( node, 0, model ), decalinfo );
	}
	else if( dist < -decalinfo->m_Size )
	{
		R_DecalNode( model, node_child( node, 1, model ), decalinfo );
	}
	else
	{
		if( dist < DECAL_DISTANCE && dist > -DECAL_DISTANCE )
			R_DecalNodeSurfaces( model, node, decalinfo );

		R_DecalNode( model, node_child( node, 0, model ), decalinfo );
		R_DecalNode( model, node_child( node, 1, model ), decalinfo );
	}
}

void R_Decals_Init( void )
{
	r_decals_cvar = gEngfuncs.pfnGetCvarPointer( "r_decals" );
	rt_decals     = gEngfuncs.Cvar_Get( "rt_decals", "1", FCVAR_ARCHIVE, "Draw decals (blood, bullet holes, scorch marks, sprays)" );
}

// Shoots a decal onto the surface of the BSP.  position is the center of the decal in world coords
void R_DecalShoot( int textureIndex, int entityIndex, int modelIndex, vec3_t pos, int flags, float scale )
{
	if( textureIndex <= 0 || textureIndex >= MAX_REMIX_TEXTURES )
	{
		gEngfuncs.Con_Printf( S_ERROR "Decal has invalid texture!\n" );
		return;
	}

	cl_entity_t *ent = NULL;
	model_t *model = NULL;
	if( entityIndex > 0 )
	{
		ent = CL_GetEntityByIndex( entityIndex );

		if( modelIndex > 0 ) model = CL_ModelHandle( modelIndex );
		else if( ent != NULL ) model = CL_ModelHandle( ent->curstate.modelindex );
		else return;
	}
	else if( modelIndex > 0 )
		model = CL_ModelHandle( modelIndex );
	else model = CL_ModelHandle( 1 );

	if( !model || model->type != mod_brush )
		return;

	decalinfo_t decalInfo;
	decalInfo.m_pModel = model;
	hull_t *hull = &model->hulls[0]; // always use #0 hull

	// all the decals at 'first shoot' are placed into local space of the parent entity
	if( ent && !FBitSet( flags, FDECAL_LOCAL_SPACE ))
	{
		vec3_t pos_l;

		if( !VectorIsNull( ent->angles ))
		{
			matrix4x4 matrix;

			Matrix4x4_CreateFromEntity( matrix, ent->angles, ent->origin, 1.0f );
			Matrix4x4_VectorITransform( matrix, pos, pos_l );
		}
		else
		{
			VectorSubtract( pos, ent->origin, pos_l );
		}

		VectorCopy( pos_l, decalInfo.m_Position );
		SetBits( flags, FDECAL_LOCAL_SPACE );
	}
	else
	{
		VectorCopy( pos, decalInfo.m_Position );
	}

	// this decal must use landmark for correct transition
	if( !FBitSet( model->flags, MODEL_HAS_ORIGIN ))
		SetBits( flags, FDECAL_USE_LANDMARK );

	decalInfo.m_iTexture = textureIndex;
	decalInfo.m_Entity = entityIndex;
	decalInfo.m_Flags = flags;

	int width, height;
	R_GetDecalDimensions( textureIndex, &width, &height );
	decalInfo.m_Size = Q_max( width >> 1, height >> 1 );
	decalInfo.m_scale = bound( MIN_DECAL_SCALE, scale, MAX_DECAL_SCALE );

	// compute the decal dimensions in world space
	decalInfo.m_decalWidth = width / decalInfo.m_scale;
	decalInfo.m_decalHeight = height / decalInfo.m_scale;

	R_DecalNode( model, &model->nodes[hull->firstclipnode], &decalInfo );
}

/*
=============================================================

  DECALS SERIALIZATION

=============================================================
*/
static qboolean R_DecalUnProject( decal_t *pdecal, decallist_t *entry )
{
	if( !pdecal || !pdecal->psurface )
		return false;

	VectorCopy( pdecal->position, entry->position );
	entry->entityIndex = pdecal->entityIndex;
	R_DecalSurfaceNormal( pdecal->psurface, entry->impactPlaneNormal );
	return true;
}

static int DecalListAdd( decallist_t *pList, int count )
{
	decallist_t *pdecal = pList + count;

	for( int i = 0; i < count; i++ )
	{
		if( !Q_strcmp( pdecal->name, pList[i].name ) && pdecal->entityIndex == pList[i].entityIndex )
		{
			vec3_t tmp;
			VectorSubtract( pdecal->position, pList[i].position, tmp ); // Merge

			if( VectorLength( tmp ) < DECAL_OVERLAP_DISTANCE )
				return count;
		}
	}

	return count + 1; // this is a new decal
}

static int DecalDepthCompare( const void *a, const void *b )
{
	const decallist_t *elem1 = (const decallist_t *)a;
	const decallist_t *elem2 = (const decallist_t *)b;

	if( elem1->depth > elem2->depth )
		return 1;
	if( elem1->depth < elem2->depth )
		return -1;
	return 0;
}

// Called when the game saves (and across level changes): the decals to place again
int R_CreateDecalList( decallist_t *pList )
{
	int total = 0;

	if( tr.worldmodel )
	{
		for( int i = 0; i < MAX_RENDER_DECALS; i++ )
		{
			decal_t *decal = &gDecalPool[i];

			// decal is in use and is not a custom decal
			if( decal->psurface == NULL || FBitSet( decal->flags, FDECAL_DONTSAVE ))
				continue;

			// compute depth
			int depth = 0;
			for( decal_t *pdecals = decal->psurface->pdecals; pdecals && pdecals != decal; pdecals = pdecals->pnext )
				depth++;

			pList[total].depth = depth;
			pList[total].flags = decal->flags;
			pList[total].scale = decal->scale;

			R_DecalUnProject( decal, &pList[total] );
			COM_FileBase( R_Textures_GetName( decal->texture ), pList[total].name, sizeof( pList[total].name ));

			total = DecalListAdd( pList, total );
		}

		if( gEngfuncs.drawFuncs->R_CreateStudioDecalList )
			total += gEngfuncs.drawFuncs->R_CreateStudioDecalList( pList, total );
	}

	// sort the decals lowest depth first, so they can be re-applied in order
	qsort( pList, total, sizeof( decallist_t ), DecalDepthCompare );

	return total;
}

// remove all decals with specified texture
void R_DecalRemoveAll( int textureIndex )
{
	if( textureIndex < 0 || textureIndex >= MAX_REMIX_TEXTURES )
		return;

	for( int i = 0; i < gDecalCount; i++ )
	{
		decal_t *pdecal = &gDecalPool[i];

		if( FBitSet( pdecal->flags, FDECAL_PERMANENT ))
			continue;

		if( !textureIndex || pdecal->texture == textureIndex )
			R_DecalUnlink( pdecal );
	}
}

// remove all decals from anything, used for full decals restart
void R_ClearAllDecals( qboolean includePermanent )
{
	for( int i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		decal_t *pdecal = &gDecalPool[i];

		if( !includePermanent && FBitSet( pdecal->flags, FDECAL_PERMANENT ))
			continue;

		R_DecalUnlink( pdecal );
	}

	if( gEngfuncs.drawFuncs->R_ClearStudioDecals )
		gEngfuncs.drawFuncs->R_ClearStudioDecals();
}

/*
=============================================================

  DRAWING

=============================================================
*/

static void R_Decals_DestroyMesh( decal_mesh_t *m )
{
	if( m->mesh && g_remix.DestroyMesh )
		g_remix.DestroyMesh( m->mesh );
	m->mesh = NULL;
}

// A new map: its models' surfaces are new, the old decals went with the old ones
void R_Decals_NewMap( void )
{
	for( int i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		if( gDecalPool[i].polys )
			Mem_Free( gDecalPool[i].polys );
	}
	memset( gDecalPool, 0, sizeof( gDecalPool ));
	gDecalCount = 0;

	for( int i = 0; i < s_numDecalMeshes; i++ )
		R_Decals_DestroyMesh( &s_decalMeshes[i] );
	memset( s_decalMeshes, 0, sizeof( s_decalMeshes ));
	s_numDecalMeshes = 0;
	R_DecalChanged();
}

typedef struct
{
	int                        texture;
	remixapi_HardcodedVertex  *verts;
	uint32_t                   numVerts, maxVerts;
	uint32_t                  *indices;
	uint32_t                   numIndices, maxIndices;
} decal_bucket_t;

// Builds the mesh of a model's decals (NULL: it has none)
static remixapi_MeshHandle R_Decals_BuildMesh( const model_t *mod, uint64_t hash )
{
	const int first = mod->firstmodelsurface;
	const int last  = first + mod->nummodelsurfaces;

	decal_bucket_t buckets[64];
	int numBuckets = 0;

	for( int i = 0; i < MAX_RENDER_DECALS; i++ )
	{
		const decal_t *d = &gDecalPool[i];
		if( !d->psurface || !d->polys || d->texture <= 0 )
			continue;

		const int surfIndex = (int)( d->psurface - mod->surfaces );
		if( surfIndex < first || surfIndex >= last )
			continue;

		int b = 0;
		while( b < numBuckets && buckets[b].texture != d->texture )
			b++;
		if( b == numBuckets )
		{
			if( numBuckets == ARRAYSIZE( buckets ))
				continue;
			memset( &buckets[b], 0, sizeof( buckets[b] ));
			buckets[b].texture = d->texture;
			numBuckets++;
		}
		decal_bucket_t *bucket = &buckets[b];

		const glpoly2_t *poly = d->polys;
		const uint32_t n = (uint32_t)poly->numverts;
		if( n < 3 )
			continue;

		if( bucket->numVerts + n > bucket->maxVerts )
		{
			bucket->maxVerts = Q_max( bucket->maxVerts * 2, bucket->numVerts + n + 64 );
			bucket->verts = Mem_Realloc( r_temppool, bucket->verts, bucket->maxVerts * sizeof( remixapi_HardcodedVertex ));
		}
		if( bucket->numIndices + ( n - 2 ) * 3 > bucket->maxIndices )
		{
			bucket->maxIndices = Q_max( bucket->maxIndices * 2, bucket->numIndices + ( n - 2 ) * 3 + 192 );
			bucket->indices = Mem_Realloc( r_temppool, bucket->indices, bucket->maxIndices * sizeof( uint32_t ));
		}

		vec3_t normal;
		R_DecalSurfaceNormal( d->psurface, normal );

		// Its place in the surface's stack (older decals first)
		int layer = 0;
		for( const decal_t *below = d->psurface->pdecals; below && below != d && layer < DECAL_MAX_LAYERS - 1; below = below->pnext )
			layer++;
		const float offset = DECAL_SURFACE_OFFSET + DECAL_LAYER_STEP * layer;

		const uint32_t base = bucket->numVerts;
		for( uint32_t k = 0; k < n; k++ )
		{
			remixapi_HardcodedVertex *v = &bucket->verts[bucket->numVerts++];
			memset( v, 0, sizeof( *v ));
			VectorMA( poly->verts[k], offset, normal, v->position );
			VectorCopy( normal, v->normal );
			v->texcoord[0] = poly->verts[k][3];
			v->texcoord[1] = poly->verts[k][4];
			v->color = 0xFFFFFFFF;
		}
		for( uint32_t k = 0; k + 2 < n; k++ )
		{
			bucket->indices[bucket->numIndices++] = base;
			bucket->indices[bucket->numIndices++] = base + k + 1;
			bucket->indices[bucket->numIndices++] = base + k + 2;
		}
	}

	remixapi_MeshHandle mesh = NULL;
	if( numBuckets > 0 && g_remix.CreateMesh )
	{
		remixapi_MeshInfoSurfaceTriangles surfaces[ARRAYSIZE( buckets )];
		uint32_t numSurfaces = 0;
		for( int b = 0; b < numBuckets; b++ )
		{
			if( !buckets[b].numIndices )
				continue;
			remixapi_MeshInfoSurfaceTriangles *st = &surfaces[numSurfaces++];
			memset( st, 0, sizeof( *st ));
			st->vertices_values = buckets[b].verts;
			st->vertices_count  = buckets[b].numVerts;
			st->indices_values  = buckets[b].indices;
			st->indices_count   = buckets[b].numIndices;
			st->material        = R_GetSpriteMaterial( buckets[b].texture );
		}

		if( numSurfaces )
		{
			remixapi_MeshInfo info;
			memset( &info, 0, sizeof( info ));
			info.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			info.hash            = hash;
			info.surfaces_values = surfaces;
			info.surfaces_count  = numSurfaces;
			if( g_remix.CreateMesh( &info, &mesh ) != REMIXAPI_ERROR_CODE_SUCCESS )
				mesh = NULL;
		}
	}

	for( int b = 0; b < numBuckets; b++ )
	{
		if( buckets[b].verts ) Mem_Free( buckets[b].verts );
		if( buckets[b].indices ) Mem_Free( buckets[b].indices );
	}
	return mesh;
}

// Draws a brush model's decals (the world, or a brush entity with its transform)
void R_Decals_DrawModel( const model_t *mod, const remixapi_Transform *transform, uint64_t instanceId, qboolean isWorld )
{
	if( !mod || mod->type != mod_brush || !g_remix.DrawInstance )
		return;
	if( rt_decals && rt_decals->value <= 0.0f )
		return;

	decal_mesh_t *m = NULL;
	for( int i = 0; i < s_numDecalMeshes; i++ )
	{
		if( s_decalMeshes[i].model == mod )
		{
			m = &s_decalMeshes[i];
			break;
		}
	}
	if( !m )
	{
		if( s_numDecalMeshes == MAX_DECAL_MESHES )
			return;
		m = &s_decalMeshes[s_numDecalMeshes++];
		memset( m, 0, sizeof( *m ));
		m->model = mod;
	}

	if( m->generation != s_decalGeneration )
	{
		// A new hash each time: the old mesh may still be in flight
		R_Decals_DestroyMesh( m );
		m->hash = 0x444543414C000000ULL ^ ( (uint64_t)( m - s_decalMeshes ) << 32 ) ^ ++s_meshSerial; // "DECAL"
		m->mesh = R_Decals_BuildMesh( mod, m->hash );
		m->generation = s_decalGeneration;
	}
	if( !m->mesh )
		return;

	remixapi_InstanceRasterStateEXT raster;
	remixapi_InstanceInfoBlendEXT   blend;
	remixapi_InstanceIdentityEXT    ident;
	R_SetupSpriteBlendState( &blend, &ident, &raster, instanceId ^ 0x444543414C000000ULL, kRenderTransAlpha );
	ident.classification = isWorld ? REMIXAPI_INSTANCE_STATIC_EXT : REMIXAPI_INSTANCE_DYNAMIC_EXT;

	remixapi_InstanceInfo inst;
	memset( &inst, 0, sizeof( inst ));
	inst.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	inst.pNext         = &ident;
	inst.categoryFlags = ( isWorld ? REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC : REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC ) |
	                     REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_BAKED_LIGHTING;
	inst.mesh          = m->mesh;
	inst.transform     = *transform;
	inst.doubleSided   = 1;
	g_remix.DrawInstance( &inst );
}
