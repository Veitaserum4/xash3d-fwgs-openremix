/*
 * ref_openremix - Native OpenRemix Renderer Backend for Xash3D FWGS
 * r_studio.c - Studio model skeletal animation, software skinning, and Remix submission
 */

#include "r_local.h"
#include "r_studio.h"
#include "r_remix.h"
#include "r_studioint.h"

// Set while R_DrawPlayerBody draws the local player's body (reflection-only instances)
static qboolean s_drawingPlayerBody = false;
static cvar_t  *rt_player_body = NULL;

// Master studio state instance
studio_draw_state_t g_studio;

// Engine studio interface pointer
static r_studio_interface_t *pStudioDraw = NULL;

// Active studio model context
static studiohdr_t          *m_pStudioHeader = NULL;
static mstudiomodel_t       *m_pSubModel     = NULL;
static mstudiobodyparts_t   *m_pBodyPart     = NULL;
static int                   g_nFaceFlags    = 0;
static int                   g_nForceFaceFlags = 0;
static cvar_t               *cl_righthand = NULL;

// Scratch buffers for triangle evaluation & Remix mesh building
static remixapi_HardcodedVertex g_scratchVerts[MAX_STUDIO_SCRATCH_VERTS];
static uint32_t                 g_scratchIndices[MAX_STUDIO_SCRATCH_INDICES];

/*
====================
R_StudioMakeGeometryHash
====================
*/
static uint64_t R_StudioMakeGeometryHash( int entIndex, int bodypart, int submodel, int mesh )
{
	uint64_t h = 0xcbf29ce484222325ULL;
	uint32_t p1 = ((uint32_t)entIndex & 0xFFFF) | (((uint32_t)bodypart & 0xFF) << 16) | (((uint32_t)submodel & 0xFF) << 24);
	uint32_t p2 = (uint32_t)mesh;

	const uint8_t *b = (const uint8_t *)&p1;
	for( int i = 0; i < 4; i++ ) { h ^= b[i]; h *= 0x100000001b3ULL; }
	b = (const uint8_t *)&p2;
	for( int i = 0; i < 4; i++ ) { h ^= b[i]; h *= 0x100000001b3ULL; }
	return h ? h : 1ULL;
}

/*
====================
R_StudioMakeReplacementHash
====================
*/
static uint64_t R_StudioMakeReplacementHash( const char *modelName, int bodypart, int submodel, int mesh )
{
	uint64_t h = 0xcbf29ce484222325ULL;
	if( modelName )
	{
		while( *modelName )
		{
			h ^= (uint8_t)(*modelName++);
			h *= 0x100000001b3ULL;
		}
	}
	uint32_t packed = ((uint32_t)bodypart & 0xFF) | (((uint32_t)submodel & 0xFF) << 8) | (((uint32_t)mesh & 0xFFFF) << 16);
	const uint8_t *b = (const uint8_t *)&packed;
	for( int i = 0; i < 4; i++ ) { h ^= b[i]; h *= 0x100000001b3ULL; }
	return h ? h : 1ULL;
}

/*
====================
R_StudioLerpMovement
====================
*/
void R_StudioLerpMovement( cl_entity_t *e, double time, vec3_t origin, vec3_t angles )
{
	float f = 1.0f;

	if( g_studio.interpolate && ( time < e->curstate.animtime + 1.0f ) && ( e->curstate.animtime != e->latched.prevanimtime ) )
		f = (float)( ( time - e->curstate.animtime ) / ( e->curstate.animtime - e->latched.prevanimtime ) );

	VectorLerp( e->latched.prevorigin, f, e->curstate.origin, origin );

	if( !VectorCompareEpsilon( e->curstate.angles, e->latched.prevangles, ON_EPSILON ) )
	{
		vec4_t q, q1, q2;
		AngleQuaternion( e->curstate.angles, q1, false );
		AngleQuaternion( e->latched.prevangles, q2, false );
		QuaternionSlerp( q2, q1, f, q );
		QuaternionAngle( q, angles );
	}
	else
	{
		VectorCopy( e->curstate.angles, angles );
	}
}

/*
====================
R_StudioSetUpTransform
====================
*/
static void R_StudioSetUpTransform( cl_entity_t *e )
{
	vec3_t origin = Vec3( e->origin );
	vec3_t angles = Vec3( e->angles );

	if( e->curstate.movetype == MOVETYPE_STEP && !FBitSet( gp_host->features, ENGINE_COMPUTE_STUDIO_LERP ) )
	{
		R_StudioLerpMovement( e, g_studio.time, origin, angles );
	}

	if( !FBitSet( gp_host->features, ENGINE_COMPENSATE_QUAKE_BUG ) )
		angles[PITCH] = -angles[PITCH];

	if( e->player )
		angles[PITCH] = 0.0f;

	Matrix3x4_CreateFromEntity( g_studio.rotationmatrix, angles, origin, 1.0f );

	if( tr.fFlipViewModel )
	{
		g_studio.rotationmatrix[0][1] = -g_studio.rotationmatrix[0][1];
		g_studio.rotationmatrix[1][1] = -g_studio.rotationmatrix[1][1];
		g_studio.rotationmatrix[2][1] = -g_studio.rotationmatrix[2][1];
	}
}

/*
====================
R_StudioEstimateFrame
====================
*/
float R_StudioEstimateFrame( cl_entity_t *e, mstudioseqdesc_t *pseqdesc, double time )
{
	double dfdt, f;

	if( g_studio.interpolate )
	{
		if( time < e->curstate.animtime ) dfdt = 0.0;
		else dfdt = ( time - e->curstate.animtime ) * e->curstate.framerate * pseqdesc->fps;
	}
	else dfdt = 0.0;

	if( pseqdesc->numframes <= 1 ) f = 0.0;
	else f = ( e->curstate.frame * ( pseqdesc->numframes - 1 ) ) / 256.0f;

	f += dfdt;

	if( pseqdesc->flags & STUDIO_LOOPING )
	{
		if( pseqdesc->numframes > 1 )
			f -= (int)( f / ( pseqdesc->numframes - 1 ) ) * ( pseqdesc->numframes - 1 );
		if( f < 0 ) f += ( pseqdesc->numframes - 1 );
	}
	else
	{
		if( f >= pseqdesc->numframes - 1.001 )
			f = pseqdesc->numframes - 1.001;
		if( f < 0.0 ) f = 0.0;
	}
	return (float)f;
}

/*
====================
R_StudioEstimateInterpolant
====================
*/
static float R_StudioEstimateInterpolant( cl_entity_t *e )
{
	float dadt = 1.0f;

	if( g_studio.interpolate && ( e->curstate.animtime >= e->latched.prevanimtime + 0.01f ) )
	{
		dadt = (float)( ( g_studio.time - e->curstate.animtime ) / 0.1f );
		if( dadt > 2.0f ) dadt = 2.0f;
	}

	return dadt;
}

/*
====================
R_StudioCalcBoneAdj
====================
*/
static void R_StudioCalcBoneAdj( float dadt, float *adj, const byte *pcontroller1, const byte *pcontroller2, byte mouthopen )
{
	mstudiobonecontroller_t *pbonecontroller =
		(mstudiobonecontroller_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bonecontrollerindex);

	for( int j = 0; j < m_pStudioHeader->numbonecontrollers; j++ )
	{
		float value = 0.0f;
		int i = pbonecontroller[j].index;

		if( i == STUDIO_MOUTH )
		{
			value = (float)mouthopen / 64.0f;
			value = bound( 0.0f, value, 1.0f );
			value = ( 1.0f - value ) * pbonecontroller[j].start + value * pbonecontroller[j].end;
		}
		else if( i < 4 )
		{
			if( FBitSet( pbonecontroller[j].type, STUDIO_RLOOP ) )
			{
				if( abs( pcontroller1[i] - pcontroller2[i] ) > 128 )
				{
					int a = ( pcontroller1[i] + 128 ) % 256;
					int b = ( pcontroller2[i] + 128 ) % 256;
					value = ( ( a * dadt ) + ( b * ( 1.0f - dadt ) ) - 128 ) * ( 360.0f / 256.0f ) + pbonecontroller[j].start;
				}
				else
				{
					value = ( ( pcontroller1[i] * dadt + ( pcontroller2[i] ) * ( 1.0f - dadt ) ) ) * ( 360.0f / 256.0f ) + pbonecontroller[j].start;
				}
			}
			else
			{
				value = ( pcontroller1[i] * dadt + pcontroller2[i] * ( 1.0f - dadt ) ) / 255.0f;
				value = bound( 0.0f, value, 1.0f );
				value = ( 1.0f - value ) * pbonecontroller[j].start + value * pbonecontroller[j].end;
			}
		}

		switch( pbonecontroller[j].type & STUDIO_TYPES )
		{
		case STUDIO_XR:
		case STUDIO_YR:
		case STUDIO_ZR:
			adj[j] = DEG2RAD( value );
			break;
		case STUDIO_X:
		case STUDIO_Y:
		case STUDIO_Z:
			adj[j] = value;
			break;
		}
	}
}

/*
====================
R_StudioCalcRotations
====================
*/
static void R_StudioCalcRotations( cl_entity_t *e, float pos[][3], vec4_t *q, mstudioseqdesc_t *pseqdesc, mstudioanim_t *panim, float f )
{
	float adj[MAXSTUDIOCONTROLLERS];

	if( f > pseqdesc->numframes - 1 )
		f = 0.0f;
	else if( f < -0.01f )
		f = -0.01f;

	int frame = (int)f;
	float dadt = R_StudioEstimateInterpolant( e );
	float s = ( f - frame );

	mstudiobone_t *pbone = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	R_StudioCalcBoneAdj( dadt, adj, e->curstate.controller, e->latched.prevcontroller, e->mouth.mouthopen );

	for( int i = 0; i < m_pStudioHeader->numbones; i++, pbone++, panim++ )
		R_StudioCalcBones( frame, s, pbone, panim, adj, pos[i], q[i] );

	if( pseqdesc->motiontype & STUDIO_X ) pos[pseqdesc->motionbone][0] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Y ) pos[pseqdesc->motionbone][1] = 0.0f;
	if( pseqdesc->motiontype & STUDIO_Z ) pos[pseqdesc->motionbone][2] = 0.0f;
}

/*
====================
R_StudioSaveBones
====================
*/
static void R_StudioSaveBones( void )
{
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);
	g_studio.cached_numbones = m_pStudioHeader->numbones;
	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		Matrix3x4_Copy( g_studio.cached_bonestransform[i], g_studio.bonestransform[i] );
		Matrix3x4_Copy( g_studio.cached_lighttransform[i], g_studio.lighttransform[i] );
		Q_strncpy( g_studio.cached_bonenames[i], pbones[i].name, sizeof( g_studio.cached_bonenames[i] ) );
	}

	if( m_pStudioHeader->numattachments > 0 && RI.currententity )
	{
		mstudioattachment_t *pAtt = (mstudioattachment_t *)((byte *)m_pStudioHeader + m_pStudioHeader->attachmentindex);
		int maxAtt = Q_min( MAXSTUDIOATTACHMENTS, m_pStudioHeader->numattachments );
		for( int i = 0; i < maxAtt; i++ )
		{
			Matrix3x4_VectorTransform( g_studio.lighttransform[pAtt[i].bone], pAtt[i].org, RI.currententity->attachment[i] );
		}
	}
}

/*
====================
R_StudioFxTransform
====================
*/
static void R_StudioFxTransform( cl_entity_t *ent, matrix3x4 transform )
{
	switch( ent->curstate.renderfx )
	{
	case kRenderFxDistort:
	case kRenderFxHologram:
		if( !gEngfuncs.COM_RandomLong( 0, 49 ) )
		{
			int axis = gEngfuncs.COM_RandomLong( 0, 1 );
			if( axis == 1 ) axis = 2; // choose between x & z
			VectorScale( transform[axis], gEngfuncs.COM_RandomFloat( 1.0f, 1.484f ), transform[axis] );
		}
		else if( !gEngfuncs.COM_RandomLong( 0, 49 ) )
		{
			float offset = gEngfuncs.COM_RandomFloat( -10.0f, 10.0f );
			transform[gEngfuncs.COM_RandomLong( 0, 2 )][3] += offset;
		}
		break;
	case kRenderFxExplode:
		{
			float scale = 1.0f + (float)( g_studio.time - ent->curstate.animtime ) * 10.0f;
			if( scale > 2.0f ) scale = 2.0f;
			transform[0][1] *= scale;
			transform[1][1] *= scale;
			transform[2][1] *= scale;
		}
		break;
	default:
		break;
	}
}

/*
====================
R_StudioMergeBones
====================
*/
static void R_StudioMergeBones( cl_entity_t *e, model_t *m_pSubModel )
{
	static vec4_t q[MAXSTUDIOBONES];
	static float pos[MAXSTUDIOBONES][3];

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = (mstudioanim_t *)gEngfuncs.R_StudioGetAnim( m_pStudioHeader, m_pSubModel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );
	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		int j;
		for( j = 0; j < g_studio.cached_numbones; j++ )
		{
			if( !Q_stricmp( pbones[i].name, g_studio.cached_bonenames[j] ) )
			{
				Matrix3x4_Copy( g_studio.bonestransform[i], g_studio.cached_bonestransform[j] );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.cached_lighttransform[j] );
				break;
			}
		}

		if( j >= g_studio.cached_numbones )
		{
			matrix3x4 bonematrix;
			Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );
			if( pbones[i].parent == -1 )
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
				Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );
				R_StudioFxTransform( e, g_studio.bonestransform[i] );
			}
			else
			{
				Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
				Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
			}
		}
	}

	R_StudioSaveBones();
}

/*
====================
R_StudioSetupBones
====================
*/
static void R_StudioSetupBones( cl_entity_t *e )
{
	static vec3_t pos[MAXSTUDIOBONES];
	static vec4_t q[MAXSTUDIOBONES];
	static vec3_t pos2[MAXSTUDIOBONES];
	static vec4_t q2[MAXSTUDIOBONES];

	if( e->curstate.sequence >= m_pStudioHeader->numseq )
		e->curstate.sequence = 0;

	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->curstate.sequence;
	float f = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );

	mstudioanim_t *panim = (mstudioanim_t *)gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pseqdesc );
	R_StudioCalcRotations( e, pos, q, pseqdesc, panim, f );

	// Multi-blend sequences
	if( pseqdesc->numblends > 1 )
	{
		panim += m_pStudioHeader->numbones;
		R_StudioCalcRotations( e, pos2, q2, pseqdesc, panim, f );

		float dadt = 1.0f;
		float s = ( e->curstate.blending[0] * dadt + e->latched.prevblending[0] * ( 1.0f - dadt ) ) / 255.0f;
		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q2, pos2, s );
	}

	// Sequence transition slerp
	if( g_studio.interpolate && e->latched.sequencetime && ( e->latched.sequencetime + 0.2f > g_studio.time ) && ( e->latched.prevsequence < m_pStudioHeader->numseq ) )
	{
		static vec3_t pos1b[MAXSTUDIOBONES];
		static vec4_t q1b[MAXSTUDIOBONES];

		mstudioseqdesc_t *pprevseq = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + e->latched.prevsequence;
		mstudioanim_t *pprevanim = (mstudioanim_t *)gEngfuncs.R_StudioGetAnim( m_pStudioHeader, RI.currentmodel, pprevseq );
		R_StudioCalcRotations( e, pos1b, q1b, pprevseq, pprevanim, e->latched.prevframe );

		float s = 1.0f - (float)( g_studio.time - e->latched.sequencetime ) / 0.2f;
		R_StudioSlerpBones( m_pStudioHeader->numbones, q, pos, q1b, pos1b, s );
	}
	else
	{
		e->latched.prevframe = f;
	}

	mstudiobone_t *pbones = (mstudiobone_t *)((byte *)m_pStudioHeader + m_pStudioHeader->boneindex);

	// Concatenate hierarchical bone matrices
	for( int i = 0; i < m_pStudioHeader->numbones; i++ )
	{
		matrix3x4 bonematrix;
		Matrix3x4_FromOriginQuat( bonematrix, q[i], pos[i] );

		if( pbones[i].parent == -1 )
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.rotationmatrix, bonematrix );
			Matrix3x4_Copy( g_studio.lighttransform[i], g_studio.bonestransform[i] );
		}
		else
		{
			Matrix3x4_ConcatTransforms( g_studio.bonestransform[i], g_studio.bonestransform[pbones[i].parent], bonematrix );
			Matrix3x4_ConcatTransforms( g_studio.lighttransform[i], g_studio.lighttransform[pbones[i].parent], bonematrix );
		}
	}

	R_StudioSaveBones();
}

/*
====================
R_StudioGenerateNormals
====================
*/
static void R_StudioGenerateNormals( void )
{
	if( !m_pSubModel )
		return;

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorClear( g_studio.norms[i] );

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex) + j;
		short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);
		short count;

		while(( count = *(ptricmds++) ))
		{
			if( count < 0 )
			{
				count = -count;
				if( count > 2 )
				{
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( count -= 2; count > 0; count--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];
						vec3_t e0, e1, norm;
						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );
						v1 = v2;
					}
				}
				else ptricmds += count * 4;
			}
			else
			{
				if( count > 2 )
				{
					qboolean odd = false;
					int v0 = ptricmds[0]; ptricmds += 4;
					int v1 = ptricmds[0]; ptricmds += 4;

					for( count -= 2; count > 0; count--, ptricmds += 4 )
					{
						int v2 = ptricmds[0];
						vec3_t e0, e1, norm;
						VectorSubtract( g_studio.verts[v1], g_studio.verts[v0], e0 );
						VectorSubtract( g_studio.verts[v2], g_studio.verts[v0], e1 );
						CrossProduct( e1, e0, norm );

						VectorAdd( g_studio.norms[v0], norm, g_studio.norms[v0] );
						VectorAdd( g_studio.norms[v1], norm, g_studio.norms[v1] );
						VectorAdd( g_studio.norms[v2], norm, g_studio.norms[v2] );

						if( odd ) v1 = v2;
						else v0 = v2;
						odd = !odd;
					}
				}
				else ptricmds += count * 4;
			}
		}
	}

	for( int i = 0; i < m_pSubModel->numverts; i++ )
		VectorNormalize( g_studio.norms[i] );
}

/*
====================
R_StudioSubmitMeshToRemix
====================
*/
static void R_StudioSubmitMeshToRemix( int bodypartIdx, int submodelIdx, int meshIdx, mstudiomesh_t *pmesh, mstudiotexture_t *ptexture )
{
	const qboolean isViewModel = ( RI.currententity == tr.viewent );
	short *ptricmds = (short *)((byte *)m_pStudioHeader + pmesh->triindex);

	float s = ( ptexture && ptexture->width > 0 ) ? ( 1.0f / (float)ptexture->width ) : ( 1.0f / 64.0f );
	float t = ( ptexture && ptexture->height > 0 ) ? ( 1.0f / (float)ptexture->height ) : ( 1.0f / 64.0f );

	uint32_t vertCount = 0;
	uint32_t indexCount = 0;
	short count;

	while(( count = *(ptricmds++) ))
	{
		qboolean isStrip = true;
		if( count < 0 )
		{
			isStrip = false;
			count = -count;
		}

		uint32_t stripStart = vertCount;

		for( int i = 0; i < count; i++, ptricmds += 4 )
		{
			if( vertCount >= MAX_STUDIO_SCRATCH_VERTS )
				break;

			short vertIdx = ptricmds[0];
			short normIdx = ptricmds[1];

			remixapi_HardcodedVertex *v = &g_scratchVerts[vertCount];
			memset( v, 0, sizeof( *v ) );

			// Position & Normal from deformed arrays
			v->position[0] = g_studio.verts[vertIdx][0];
			v->position[1] = g_studio.verts[vertIdx][1];
			v->position[2] = g_studio.verts[vertIdx][2];

			v->normal[0] = g_studio.norms[vertIdx][0];
			v->normal[1] = g_studio.norms[vertIdx][1];
			v->normal[2] = g_studio.norms[vertIdx][2];

			// Texture Coordinates
			if( FBitSet( g_nFaceFlags, STUDIO_NF_CHROME ) )
			{
				v->texcoord[0] = g_studio.chrome[normIdx][0] * s;
				v->texcoord[1] = g_studio.chrome[normIdx][1] * t;
			}
			else if( FBitSet( g_nFaceFlags, STUDIO_NF_UV_COORDS ) )
			{
				v->texcoord[0] = HalfToFloat( ptricmds[2] );
				v->texcoord[1] = HalfToFloat( ptricmds[3] );
			}
			else
			{
				v->texcoord[0] = ptricmds[2] * s;
				v->texcoord[1] = ptricmds[3] * t;
			}

			v->color = 0xFFFFFFFF; // White opaque
			vertCount++;
		}

		// Triangulate strip or fan into index triplets
		if( isStrip )
		{
			for( int k = 0; k < count - 2; k++ )
			{
				if( indexCount + 3 > MAX_STUDIO_SCRATCH_INDICES ) break;
				if( ( k & 1 ) == 0 )
				{
					g_scratchIndices[indexCount++] = stripStart + k;
					g_scratchIndices[indexCount++] = stripStart + k + 1;
					g_scratchIndices[indexCount++] = stripStart + k + 2;
				}
				else
				{
					g_scratchIndices[indexCount++] = stripStart + k + 1;
					g_scratchIndices[indexCount++] = stripStart + k;
					g_scratchIndices[indexCount++] = stripStart + k + 2;
				}
			}
		}
		else // Triangle Fan
		{
			for( int k = 0; k < count - 2; k++ )
			{
				if( indexCount + 3 > MAX_STUDIO_SCRATCH_INDICES ) break;
				g_scratchIndices[indexCount++] = stripStart;
				g_scratchIndices[indexCount++] = stripStart + k + 1;
				g_scratchIndices[indexCount++] = stripStart + k + 2;
			}
		}
	}

	if( vertCount == 0 || indexCount == 0 )
		return;

	// Build Remix surface structure
	remixapi_MeshInfoSurfaceTriangles surface;
	memset( &surface, 0, sizeof( surface ) );
	surface.vertices_values   = g_scratchVerts;
	surface.vertices_count    = vertCount;
	surface.indices_values    = g_scratchIndices;
	surface.indices_count     = indexCount;
	surface.skinning_hasvalue = 0;
	surface.material          = R_Textures_GetOrCreateForStudio( ptexture, m_pStudioHeader );

	// Extensions for in-place GPU BLAS update and motion vectors
	remixapi_MeshInfoMotionEXT motionExt;
	memset( &motionExt, 0, sizeof( motionExt ) );
	motionExt.sType   = REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT;
	motionExt.pNext   = NULL;
	motionExt.enabled = 1;

	remixapi_MeshInfoUpdateEXT updateExt;
	memset( &updateExt, 0, sizeof( updateExt ) );
	updateExt.sType   = REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT;
	updateExt.pNext   = &motionExt;
	updateExt.enabled = 1;

	int entIndex = ( RI.currententity ) ? ( RI.currententity->index ? RI.currententity->index : (int)( ( (uintptr_t)RI.currententity >> 4 ) & 0x7FFF ) ) : 0;
	const char *modName = ( RI.currentmodel ) ? RI.currentmodel->name : "studio";
	uint64_t geomHash = R_StudioMakeGeometryHash( entIndex, bodypartIdx, submodelIdx, meshIdx );
	uint64_t modHash  = R_StudioMakeReplacementHash( modName, bodypartIdx, submodelIdx, meshIdx );

	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ) );
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.pNext           = &updateExt;
	meshInfo.hash            = geomHash;
	meshInfo.surfaces_values = &surface;
	meshInfo.surfaces_count  = 1;

	remixapi_MeshHandle meshHandle = NULL;
	if( !g_remix.CreateMeshBatched || g_remix.CreateMeshBatched( &meshInfo, &meshHandle ) != REMIXAPI_ERROR_CODE_SUCCESS )
		return;

	// Configure Instance Category Flags
	remixapi_InstanceCategoryFlags categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_SMOOTH_NORMALS;

	if( isViewModel )
		categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
	else if( s_drawingPlayerBody )
		categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_BODY; // seen only in reflections

	if( FBitSet( g_nFaceFlags, STUDIO_NF_MASKED ) )
		categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT;

	if( FBitSet( g_nFaceFlags, STUDIO_NF_ADDITIVE ) || ( RI.currententity && RI.currententity->curstate.rendermode == kRenderTransAdd ) )
		categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_LEGACY_EMISSIVE;

	// USD modding replacement hash extension
	remixapi_InstanceIdentityEXT identityExt;
	memset( &identityExt, 0, sizeof( identityExt ) );
	identityExt.sType          = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
	identityExt.pNext          = NULL;
	identityExt.instanceId     = geomHash;
	identityExt.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
	identityExt.rasterVisible  = 1;

	remixapi_InstanceInfoReplacementHashEXT replaceExt;
	memset( &replaceExt, 0, sizeof( replaceExt ) );
	replaceExt.sType    = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT;
	replaceExt.pNext    = &identityExt;
	replaceExt.meshHash = modHash;

	// Identity transform since vertices are already deformed in world coordinates
	remixapi_Transform identityTransform = {
		.matrix = {
			{ 1.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f }
		}
	};

	remixapi_InstanceInfo instanceInfo;
	memset( &instanceInfo, 0, sizeof( instanceInfo ) );
	instanceInfo.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instanceInfo.pNext         = &replaceExt;
	instanceInfo.categoryFlags = categoryFlags;
	instanceInfo.mesh          = meshHandle;
	instanceInfo.transform     = identityTransform;
	instanceInfo.doubleSided   = 1;
	if( g_remix.DrawInstance )
	{
		remixapi_ErrorCode err = g_remix.DrawInstance( &instanceInfo );
		if( err != REMIXAPI_ERROR_CODE_SUCCESS )
		{
			static int s_studioDrawErrors = 0;
			if( ++s_studioDrawErrors <= 5 )
			{
				gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: DrawInstance for studio %s failed: %s (%d)\n",
				                       modName, R_Remix_GetErrorString( err ), (int)err );
			}
		}
	}
}

/*
====================
R_StudioDrawPoints
====================
*/
static void R_StudioDrawPoints( void )
{
	if( !m_pStudioHeader || !m_pSubModel )
		return;

	int m_skinnum = ( RI.currententity ) ? RI.currententity->curstate.skin : 0;
	mstudiotexture_t *ptexture = (mstudiotexture_t *)((byte *)m_pStudioHeader + m_pStudioHeader->textureindex);
	byte *pvertbone = ((byte *)m_pStudioHeader + m_pSubModel->vertinfoindex);
	vec3_t *pstudioverts = (vec3_t *)((byte *)m_pStudioHeader + m_pSubModel->vertindex);
	short *pskinref = (short *)((byte *)m_pStudioHeader + m_pStudioHeader->skinindex);

	if( m_skinnum > 0 && m_skinnum < m_pStudioHeader->numskinfamilies )
		pskinref += ( m_skinnum * m_pStudioHeader->numskinref );

	// CPU Software Skinning: evaluate deformed vertex positions
	for( int i = 0; i < m_pSubModel->numverts; i++ )
	{
		Matrix3x4_VectorTransform( g_studio.bonestransform[pvertbone[i]], pstudioverts[i], g_studio.verts[i] );
	}

	// Area-weighted normal smoothing for clean path-traced reflections
	R_StudioGenerateNormals();

	mstudiomesh_t *pmesh = (mstudiomesh_t *)((byte *)m_pStudioHeader + m_pSubModel->meshindex);

	int bodypartIdx = (int)( m_pBodyPart - (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex) );
	int submodelIdx = (int)( m_pSubModel - (mstudiomodel_t *)((byte *)m_pStudioHeader + m_pBodyPart->modelindex) );

	for( int j = 0; j < m_pSubModel->nummesh; j++ )
	{
		g_nFaceFlags = ptexture[pskinref[pmesh[j].skinref]].flags | g_nForceFaceFlags;
		R_StudioSubmitMeshToRemix( bodypartIdx, submodelIdx, j, &pmesh[j], &ptexture[pskinref[pmesh[j].skinref]] );
	}
}

/*
====================
R_StudioDrawModel
====================
*/
int R_StudioDrawModel( int flags )
{
	if( !RI.currentmodel || !RI.currententity )
		return 0;

	m_pStudioHeader = (studiohdr_t *)gEngfuncs.Mod_Extradata( mod_studio, RI.currentmodel );
	if( !m_pStudioHeader )
		return 0;

	R_StudioSetUpTransform( RI.currententity );

	if( flags & STUDIO_RENDER )
	{
		if( m_pStudioHeader->numbodyparts == 0 )
			return 1;
	}

	// Bone evaluation
	if( RI.currententity->curstate.movetype == MOVETYPE_FOLLOW )
		R_StudioMergeBones( RI.currententity, RI.currentmodel );
	else
		R_StudioSetupBones( RI.currententity );

	if( flags & STUDIO_RENDER )
	{
		mstudiobodyparts_t *pbodypart = (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex);

		for( int i = 0; i < m_pStudioHeader->numbodyparts; i++ )
		{
			m_pBodyPart = &pbodypart[i];
			int baseVal = ( pbodypart[i].base > 0 ) ? pbodypart[i].base : 1;
			int submodelIdx = ( RI.currententity->curstate.body / baseVal ) % pbodypart[i].nummodels;
			m_pSubModel = (mstudiomodel_t *)((byte *)m_pStudioHeader + pbodypart[i].modelindex) + submodelIdx;

			R_StudioDrawPoints();
		}
	}

	return 1;
}

/*
====================
R_StudioSetupPlayerModel
====================
*/
static model_t *R_StudioSetupPlayerModel( int index )
{
	if( index < 0 || !gp_cl || index >= gp_cl->maxclients )
		return NULL;

	player_info_t *info = gEngfuncs.pfnPlayerInfo( index );
	player_model_t *state = &g_studio.player_models[index];

	if( info && info->model[0] )
	{
		if( Q_strcmp( state->name, info->model ) )
		{
			Q_strncpy( state->name, info->model, sizeof( state->name ) );
			state->name[sizeof( state->name ) - 1] = 0;

			Q_snprintf( state->modelname, sizeof( state->modelname ), "models/player/%s/%s.mdl", info->model, info->model );

			if( gEngfuncs.fsapi && gEngfuncs.fsapi->FileExists( state->modelname, false ) )
				state->model = (model_t *)gEngfuncs.Mod_ForName( state->modelname, false, true );
			else
				state->model = NULL;

			if( !state->model && RI.currententity )
				state->model = RI.currententity->model;
		}
	}
	else
	{
		if( RI.currententity && state->model != RI.currententity->model )
			state->model = RI.currententity->model;
	}

	return state->model;
}

/*
====================
R_StudioDrawPlayer
====================
*/
int R_StudioDrawPlayer( int flags, entity_state_t *pplayer )
{
	if( !pplayer || !gp_cl )
		return 0;

	int playerIdx = pplayer->number - 1;
	if( playerIdx < 0 || playerIdx >= gp_cl->maxclients )
		return 0;

	RI.currentmodel = R_StudioSetupPlayerModel( playerIdx );
	if( !RI.currentmodel )
		return 0;

	return R_StudioDrawModel( flags );
}

/*
====================
Studio Engine API Callbacks
====================
*/
static int g_studio_drawn_models = 0;

static cl_entity_t *pfnGetCurrentEntity( void )
{
	return RI.currententity;
}

static player_info_t *pfnPlayerInfo( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) )
		index = -1;

	return gEngfuncs.pfnPlayerInfo( index );
}

static entity_state_t *R_StudioGetPlayerState( int index )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ) && RI.currententity )
		return &RI.currententity->curstate;

	return gEngfuncs.pfnGetPlayerState( index );
}

static void pfnGetEngineTimes( int *framecount, double *current, double *old )
{
	if( framecount ) *framecount = tr.framecount;
	if( current ) *current = ( gp_cl ? gp_cl->time : tr.realtime );
	if( old ) *old = ( gp_cl ? gp_cl->oldtime : tr.realtime );
}

static void pfnGetViewInfo( float *origin, float *upv, float *rightv, float *vpnv )
{
	if( origin ) VectorCopy( RI.rvp.vieworigin, origin );
	if( vpnv ) VectorCopy( RI.vforward, vpnv );
	if( rightv ) VectorCopy( RI.vright, rightv );
	if( upv ) VectorCopy( RI.vup, upv );
}

static void pfnGetModelCounters( int **s, int **a )
{
	*s = &g_studio.framecount;
	*a = &g_studio_drawn_models;
}

static float ****pfnStudioGetBoneTransform( void )
{
	return (float ****)g_studio.bonestransform;
}

static float ****pfnStudioGetLightTransform( void )
{
	return (float ****)g_studio.lighttransform;
}

static float ***pfnStudioGetRotationMatrix( void )
{
	return (float ***)g_studio.rotationmatrix;
}

static void R_StudioSetupModel( int bodypart, void **ppbodypart, void **ppsubmodel )
{
	if( !m_pStudioHeader )
		return;

	if( bodypart >= m_pStudioHeader->numbodyparts )
		bodypart = 0;

	m_pBodyPart = (mstudiobodyparts_t *)((byte *)m_pStudioHeader + m_pStudioHeader->bodypartindex) + bodypart;

	int base = ( m_pBodyPart->base > 0 ) ? m_pBodyPart->base : 1;
	int index = ( RI.currententity ? ( RI.currententity->curstate.body / base ) : 0 );
	index = ( m_pBodyPart->nummodels > 0 ) ? ( index % m_pBodyPart->nummodels ) : 0;

	m_pSubModel = (mstudiomodel_t *)((byte *)m_pStudioHeader + m_pBodyPart->modelindex) + index;

	if( ppbodypart ) *ppbodypart = m_pBodyPart;
	if( ppsubmodel ) *ppsubmodel = m_pSubModel;
}

static int R_StudioCheckBBox( void )
{
	if( !RI.currententity || !RI.currentmodel || !m_pStudioHeader )
		return 0;

	return 1;
}

static void pfnStudioDynamicLight( cl_entity_t *ent, alight_t *plight )
{
	if( plight )
	{
		plight->ambientlight = 128;
		plight->shadelight = 192;
	}
}

static void R_StudioEntityLight( alight_t *plight )
{
	if( plight )
	{
		plight->ambientlight = 128;
		plight->shadelight = 192;
	}
}

static void R_StudioSetupLighting( alight_t *plighting )
{
}

static void R_StudioDrawHulls( void )
{
}

static void R_StudioDrawAbsBBox( void )
{
}

static void R_StudioDrawBones( void )
{
}

static void R_StudioSetupSkin( void *ptexturehdr, int index )
{
}

static void R_StudioSetRemapColors( int top, int bottom )
{
	if( gEngfuncs.CL_EntitySetRemapColors && RI.currententity && RI.currentmodel )
		gEngfuncs.CL_EntitySetRemapColors( RI.currententity, RI.currentmodel, top, bottom );
}

static void R_StudioClientEvents( void )
{
	if( !m_pStudioHeader || !RI.currententity )
		return;

	cl_entity_t *e = RI.currententity;
	if( g_studio.frametime == 0.0 )
		return;

	if( m_pStudioHeader->numattachments <= 0 )
	{
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[0] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[1] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[2] );
		Matrix3x4_OriginFromMatrix( g_studio.rotationmatrix, e->attachment[3] );
	}

	if( FBitSet( e->curstate.effects, EF_MUZZLEFLASH ) )
	{
		if( gEngfuncs.CL_AllocElight )
		{
			dlight_t *el = gEngfuncs.CL_AllocElight( 0 );
			if( el )
			{
				ClearBits( e->curstate.effects, EF_MUZZLEFLASH );
				VectorCopy( e->attachment[0], el->origin );
				el->die = ( gp_cl ? gp_cl->time : tr.realtime ) + 0.05f;
				el->color.r = 255;
				el->color.g = 192;
				el->color.b = 64;
				el->decay = 320;
				el->radius = 24;
			}
		}
	}

	int sequence = bound( 0, e->curstate.sequence, m_pStudioHeader->numseq - 1 );
	mstudioseqdesc_t *pseqdesc = (mstudioseqdesc_t *)((byte *)m_pStudioHeader + m_pStudioHeader->seqindex) + sequence;

	if( pseqdesc->numevents == 0 )
		return;

	float end = R_StudioEstimateFrame( e, pseqdesc, g_studio.time );
	float start = end - e->curstate.framerate * (float)g_studio.frametime * pseqdesc->fps;
	mstudioevent_t *pevent = (mstudioevent_t *)((byte *)m_pStudioHeader + pseqdesc->eventindex);

	if( e->latched.sequencetime == e->curstate.animtime )
	{
		if( !FBitSet( pseqdesc->flags, STUDIO_LOOPING ) )
			start = -0.01f;
	}

	for( int i = 0; i < pseqdesc->numevents; i++ )
	{
		if( pevent[i].event < EVENT_CLIENT )
			continue;

		if( (float)pevent[i].frame > start && pevent[i].frame <= end )
		{
			if( gEngfuncs.pfnStudioEvent )
				gEngfuncs.pfnStudioEvent( &pevent[i], e );
		}
	}
}

static int R_StudioGetForceFaceFlags( void )
{
	return g_nForceFaceFlags;
}

static void R_StudioSetForceFaceFlags( int flags )
{
	g_nForceFaceFlags = flags;
}

static void R_StudioSetHeader( void *header )
{
	m_pStudioHeader = (studiohdr_t *)header;
}

static void R_StudioSetRenderModel( struct model_s *model )
{
	RI.currentmodel = model;
}

static void R_StudioSetupRenderer( int rendermode )
{
	g_studio.rendermode = rendermode;
}

static void R_StudioRestoreRenderer( void )
{
}

static void R_StudioSetChromeOrigin( void )
{
}

static void GL_StudioDrawShadow( void )
{
}

static void GL_StudioSetRenderMode( int mode )
{
	g_studio.rendermode = mode;
}

static void R_StudioSetRenderamt( int iRenderamt )
{
	if( !RI.currententity ) return;
	RI.currententity->curstate.renderamt = iRenderamt;
	tr.blend = CL_FxBlend( RI.currententity ) / 255.0f;
}

static void R_StudioSetCullState( int iCull )
{
}

static void R_StudioRenderShadow( int iSprite, float *p1, float *p2, float *p3, float *p4 )
{
}

/*
====================
R_StudioFillAPI
====================
*/
qboolean R_StudioFillAPI( struct engine_studio_api_s *api, struct r_studio_interface_s *pDefaultDraw )
{
	if( !api || !pDefaultDraw )
		return false;

	if( gEngfuncs.pfnGetCvarPointer )
		cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );

	api->GetCurrentEntity        = pfnGetCurrentEntity;
	api->PlayerInfo              = pfnPlayerInfo;
	api->GetPlayerState          = R_StudioGetPlayerState;
	api->GetTimes                = pfnGetEngineTimes;
	api->GetViewInfo             = pfnGetViewInfo;
	api->GetModelCounters        = pfnGetModelCounters;
	api->StudioGetBoneTransform  = pfnStudioGetBoneTransform;
	api->StudioGetLightTransform = pfnStudioGetLightTransform;
	api->StudioGetRotationMatrix = pfnStudioGetRotationMatrix;
	api->StudioSetupModel        = R_StudioSetupModel;
	api->StudioCheckBBox         = R_StudioCheckBBox;
	api->StudioDynamicLight      = pfnStudioDynamicLight;
	api->StudioEntityLight       = R_StudioEntityLight;
	api->StudioSetupLighting     = R_StudioSetupLighting;
	api->StudioDrawPoints        = R_StudioDrawPoints;
	api->StudioDrawHulls         = R_StudioDrawHulls;
	api->StudioDrawAbsBBox       = R_StudioDrawAbsBBox;
	api->StudioDrawBones         = R_StudioDrawBones;
	api->StudioSetupSkin         = (void *)R_StudioSetupSkin;
	api->StudioSetRemapColors    = R_StudioSetRemapColors;
	api->SetupPlayerModel        = R_StudioSetupPlayerModel;
	api->StudioClientEvents      = R_StudioClientEvents;
	api->GetForceFaceFlags       = R_StudioGetForceFaceFlags;
	api->SetForceFaceFlags       = R_StudioSetForceFaceFlags;
	api->StudioSetHeader         = (void *)R_StudioSetHeader;
	api->SetRenderModel          = R_StudioSetRenderModel;
	api->SetupRenderer           = R_StudioSetupRenderer;
	api->RestoreRenderer         = R_StudioRestoreRenderer;
	api->SetChromeOrigin         = R_StudioSetChromeOrigin;
	api->GL_StudioDrawShadow     = GL_StudioDrawShadow;
	api->GL_SetRenderMode        = GL_StudioSetRenderMode;
	api->StudioSetRenderamt      = R_StudioSetRenderamt;
	api->StudioSetCullState      = R_StudioSetCullState;
	api->StudioRenderShadow      = R_StudioRenderShadow;

	pDefaultDraw->version         = STUDIO_INTERFACE_VERSION;
	pDefaultDraw->StudioDrawModel  = R_StudioDrawModel;
	pDefaultDraw->StudioDrawPlayer = R_StudioDrawPlayer;

	return true;
}

/*
====================
R_StudioSetDrawInterface
====================
*/
void R_StudioSetDrawInterface( struct r_studio_interface_s *pDraw )
{
	pStudioDraw = pDraw;
}

/*
====================
R_StudioInit
====================
*/
void R_StudioInit( void )
{
	rt_player_body = gEngfuncs.Cvar_Get( "rt_player_body", "1", FCVAR_ARCHIVE, "Show your own player model in reflections (first person)" );
	memset( &g_studio, 0, sizeof( g_studio ) );
	pStudioDraw = NULL;
	if( gEngfuncs.pfnGetCvarPointer )
		cl_righthand = gEngfuncs.pfnGetCvarPointer( "cl_righthand" );
}

/*
====================
R_StudioShutdown
====================
*/
void R_StudioShutdown( void )
{
	memset( &g_studio, 0, sizeof( g_studio ) );
	pStudioDraw = NULL;
}

/*
====================
R_StudioNewMap
====================
*/
void R_StudioNewMap( void )
{
	for( int i = 0; i < MAX_CLIENTS; i++ )
	{
		memset( &g_studio.player_models[i], 0, sizeof( g_studio.player_models[i] ) );
	}
}

/*
====================
R_StudioSetupTimings
====================
*/
static void R_StudioSetupTimings( void )
{
	g_studio.frametime = tr.frametime;
	g_studio.time = tr.realtime;
	g_studio.framecount = tr.framecount;
	g_studio.interpolate = true;
}

/*
====================
R_AllowFlipViewModel
====================
*/
static qboolean R_AllowFlipViewModel( cl_entity_t *e )
{
	if( cl_righthand && cl_righthand->value > 0 )
	{
		if( e == tr.viewent )
			return true;
	}
	return false;
}

/*
====================
R_DrawStudioModel
====================
*/
/*
 * R_DrawPlayerBody - The local player's own model in first person, seen only in reflections
 * (mirrors, chrome, water) and bounce light. The engine does not pass the local player to
 * the renderer in first person (CL_AddVisibleEntity), but keeps its state up to date.
 */
void R_DrawPlayerBody( void )
{
	if( !rt_player_body || rt_player_body->value <= 0.0f || !gp_cl )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ) || ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer() )
		return;

	cl_entity_t *e = CL_GetEntityByIndex( gp_cl->playernum + 1 );
	if( !e || !e->player || !e->model || e->model->type != mod_studio || FBitSet( e->curstate.effects, EF_NODRAW ) )
		return;

	R_StudioSetupTimings();
	RI.currententity = e;
	RI.currentmodel  = e->model;

	// Render only: events (footsteps, muzzle flashes) already ran for the real player
	s_drawingPlayerBody = true;
	if( pStudioDraw && pStudioDraw->StudioDrawPlayer )
		pStudioDraw->StudioDrawPlayer( STUDIO_RENDER, &e->curstate );
	else
		R_StudioDrawPlayer( STUDIO_RENDER, &e->curstate );
	s_drawingPlayerBody = false;
}

void R_DrawStudioModel( cl_entity_t *e )
{
	if( !e || !e->model || e->model->type != mod_studio )
		return;

	R_StudioSetupTimings();
	RI.currententity = e;
	RI.currentmodel  = e->model;

	if( e->player )
	{
		if( pStudioDraw && pStudioDraw->StudioDrawPlayer )
			pStudioDraw->StudioDrawPlayer( STUDIO_RENDER | STUDIO_EVENTS, &e->curstate );
		else
			R_StudioDrawPlayer( STUDIO_RENDER | STUDIO_EVENTS, &e->curstate );
	}
	else
	{
		if( pStudioDraw && pStudioDraw->StudioDrawModel )
			pStudioDraw->StudioDrawModel( STUDIO_RENDER | STUDIO_EVENTS );
		else
			R_StudioDrawModel( STUDIO_RENDER | STUDIO_EVENTS );
	}
}

/*
====================
R_DrawViewModel
====================
*/
void R_DrawViewModel( void )
{
	cl_entity_t *view = tr.viewent;
	if( !view || !view->model || view->model->type != mod_studio )
		return;

	if( ENGINE_GET_PARM( PARM_THIRDPERSON ) )
		return;

	if( ENGINE_GET_PARM( PARM_LOCAL_HEALTH ) <= 0 || !CL_IsViewEntityLocalPlayer() )
		return;

	R_StudioSetupTimings();
	RI.currententity = view;
	RI.currentmodel  = view->model;

	tr.fFlipViewModel = R_AllowFlipViewModel( view );

	if( pStudioDraw && pStudioDraw->StudioDrawModel )
		pStudioDraw->StudioDrawModel( STUDIO_RENDER | STUDIO_EVENTS );
	else
		R_StudioDrawModel( STUDIO_RENDER | STUDIO_EVENTS );

	tr.fFlipViewModel = false;
}
