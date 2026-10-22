/*
 * r_sky.c - The sky for OpenRemix's sky camera: a skybox (the six env/ faces as a cube),
 * or on maps without one, Quake-style scrolling cloud layers (R_SetSkyCloudsTextures)
 *
 * Sky instances are rasterized into the sky layer that camera rays show where they
 * meet no geometry. The sky camera turns with the view but stays at the origin,
 * so the sky is centred there and never moves.
 */

#include "r_local.h"
#include "r_sky.h"
#include "r_remix.h"
#include "r_textures.h"

#define SKY_SIDES     6
#define SKY_HALF_SIZE 1024.0f
#define CLOUD_STEPS   12 // quads along a cube face's edge (SKYCLOUDS_QUALITY)
#define CLOUD_VERTS   ( SKY_SIDES * ( CLOUD_STEPS + 1 ) * ( CLOUD_STEPS + 1 ))
#define CLOUD_INDICES ( SKY_SIDES * CLOUD_STEPS * CLOUD_STEPS * 6 )

// Face i shows the engine's texture r_skyTexOrder[i] (ref/common/ref_skybox.c)
static const int s_skyTexOrder[SKY_SIDES] = { 0, 2, 1, 3, 4, 5 };

// 1 = s, 2 = t, 3 = distance; negative flips the axis (ref/gl/gl_warp.c)
static const int s_stToVec[SKY_SIDES][3] =
{
	{  3, -1,  2 },
	{ -3,  1,  2 },
	{  1,  3,  2 },
	{ -1, -3,  2 },
	{ -2, -1,  3 },
	{  2, -1, -3 }
};

static int                     s_skyTextures[SKY_SIDES];
static qboolean                s_skyLoaded;
static remixapi_MeshHandle     s_skyMesh;
static remixapi_MaterialHandle s_skyMaterials[SKY_SIDES];
static float                   s_skyBrightness;
static cvar_t                 *rt_sky_brightness;
static cvar_t                 *rt_sky_light;

// Cloud layers: 0 the solid back layer, 1 the alpha layer drawn over it
static int                      s_cloudTextures[2];
static remixapi_MaterialHandle  s_cloudMaterials[2];
static remixapi_MeshHandle      s_cloudMeshes[2];
static float                    s_cloudBrightness;
static remixapi_HardcodedVertex s_cloudVerts[CLOUD_VERTS];
static uint32_t                 s_cloudIndices[CLOUD_INDICES];
static qboolean                 s_cloudGeometry;

static qboolean R_Sky_CloudsActive( void )
{
	return !s_skyLoaded && s_cloudTextures[0] && s_cloudTextures[1];
}

static void R_Sky_DestroyMesh( void )
{
	if( s_skyMesh && g_remix.DestroyMesh )
		g_remix.DestroyMesh( s_skyMesh );
	s_skyMesh = NULL;

	for( int i = 0; i < SKY_SIDES; i++ )
	{
		if( s_skyMaterials[i] && g_remix.DestroyMaterial )
			g_remix.DestroyMaterial( s_skyMaterials[i] );
		s_skyMaterials[i] = NULL;
	}
}

static void R_Sky_DestroyClouds( void )
{
	for( int i = 0; i < 2; i++ )
	{
		if( s_cloudMeshes[i] && g_remix.DestroyMesh )
			g_remix.DestroyMesh( s_cloudMeshes[i] );
		s_cloudMeshes[i] = NULL;
		if( s_cloudMaterials[i] && g_remix.DestroyMaterial )
			g_remix.DestroyMaterial( s_cloudMaterials[i] );
		s_cloudMaterials[i] = NULL;
	}
}

/*
 * The sky layer is shown as texture * brightness, unlit and before exposure: the factor puts
 * the texture's 0-1 colours on the scale of the path-traced world.
 */
static float R_Sky_Brightness( void )
{
	return rt_sky_brightness ? Q_max( rt_sky_brightness->value, 0.0f ) : 1.0f;
}

void R_Sky_Init( void )
{
	rt_sky_brightness = gEngfuncs.Cvar_Get( "rt_sky_brightness", "8.0", FCVAR_ARCHIVE, "Skybox brightness multiplier" );
	rt_sky_light      = gEngfuncs.Cvar_Get( "rt_sky_light", "1.0", FCVAR_ARCHIVE, "Light from the skybox on the scene (0 = none)" );
}

/*
 * R_Sky_GetEnvironment - Uniform radiance for rays that leave the world: the skybox's mean
 * colour at its displayed brightness, scaled by rt_sky_light. False without a sky.
 */
qboolean R_Sky_GetEnvironment( float radiance[3] )
{
	const float scale = rt_sky_light ? rt_sky_light->value : 1.0f;
	if( scale <= 0.0f || ( !s_skyLoaded && !R_Sky_CloudsActive() ))
		return false;

	// The skybox's faces, or the clouds' solid layer
	const int *textures = s_skyLoaded ? s_skyTextures : s_cloudTextures;
	const int count = s_skyLoaded ? SKY_SIDES : 1;
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	int faces = 0;
	for( int i = 0; i < count; i++ )
	{
		float color[3];
		if( textures[i] && R_Textures_GetAverageColor( textures[i], color ))
		{
			VectorAdd( sum, color, sum );
			faces++;
		}
	}
	if( !faces )
		return false;

	VectorScale( sum, R_Sky_Brightness() * scale / faces, radiance );
	return true;
}

void R_Sky_Setup( int *skyboxTextures )
{
	R_Sky_DestroyMesh();
	s_skyLoaded = false;
	memset( s_skyTextures, 0, sizeof( s_skyTextures ));

	if( !skyboxTextures )
		return;

	for( int i = 0; i < SKY_SIDES; i++ )
	{
		s_skyTextures[i] = skyboxTextures[i];
		if( skyboxTextures[i] )
			s_skyLoaded = true;
	}
}

void R_Sky_SetClouds( int solidskyTexture, int alphaskyTexture )
{
	R_Sky_DestroyClouds();
	s_cloudTextures[0] = solidskyTexture;
	s_cloudTextures[1] = alphaskyTexture;
}

void R_Sky_Shutdown( void )
{
	R_Sky_DestroyMesh();
	R_Sky_DestroyClouds();
	s_skyLoaded = false;
	s_cloudTextures[0] = s_cloudTextures[1] = 0;
}

static void R_Sky_MakeVertex( float s, float t, int axis, remixapi_HardcodedVertex *v )
{
	const vec3_t b = { s * SKY_HALF_SIZE, t * SKY_HALF_SIZE, SKY_HALF_SIZE };

	memset( v, 0, sizeof( *v ));
	for( int j = 0; j < 3; j++ )
	{
		const int k = s_stToVec[axis][j];
		v->position[j] = ( k < 0 ) ? -b[-k - 1] : b[k - 1];
	}

	// Facing the camera at the centre
	const float len = sqrtf( v->position[0] * v->position[0] + v->position[1] * v->position[1] + v->position[2] * v->position[2] );
	for( int j = 0; j < 3; j++ )
		v->normal[j] = -v->position[j] / len;

	v->texcoord[0] = ( s + 1.0f ) * 0.5f;
	v->texcoord[1] = 1.0f - ( t + 1.0f ) * 0.5f;
	v->color = 0xFFFFFFFF;
}

static qboolean R_Sky_BuildMesh( void )
{
	static remixapi_HardcodedVertex verts[SKY_SIDES][4];
	static uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
	remixapi_MeshInfoSurfaceTriangles surfaces[SKY_SIDES];
	uint32_t count = 0;
	uint64_t hash = 0xcbf29ce484222325ULL;

	memset( surfaces, 0, sizeof( surfaces ));
	for( int i = 0; i < SKY_SIDES; i++ )
	{
		const int texnum = s_skyTextures[s_skyTexOrder[i]];
		remixapi_MaterialHandle material = texnum ? R_Textures_CreateSkyMaterial( texnum, s_skyBrightness, false ) : NULL;
		if( !material )
			continue;
		s_skyMaterials[i] = material;

		R_Sky_MakeVertex( -1.0f, -1.0f, i, &verts[i][0] );
		R_Sky_MakeVertex( -1.0f,  1.0f, i, &verts[i][1] );
		R_Sky_MakeVertex(  1.0f,  1.0f, i, &verts[i][2] );
		R_Sky_MakeVertex(  1.0f, -1.0f, i, &verts[i][3] );

		remixapi_MeshInfoSurfaceTriangles *st = &surfaces[count++];
		st->vertices_values   = verts[i];
		st->vertices_count    = 4;
		st->indices_values    = indices;
		st->indices_count     = 6;
		st->skinning_hasvalue = 0;
		st->material          = material;

		hash = ( hash ^ (uint64_t)texnum ) * 0x100000001b3ULL;
		hash = ( hash ^ (uint64_t)i ) * 0x100000001b3ULL;
	}

	if( !count || !g_remix.CreateMeshBatched )
	{
		R_Sky_DestroyMesh();
		return false;
	}

	remixapi_MeshInfo meshInfo;
	memset( &meshInfo, 0, sizeof( meshInfo ));
	meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
	meshInfo.hash            = hash ? hash : 1ULL;
	meshInfo.surfaces_values = surfaces;
	meshInfo.surfaces_count  = count;

	remixapi_ErrorCode err = g_remix.CreateMeshBatched( &meshInfo, &s_skyMesh );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: sky mesh creation failed: %s (%d)\n", R_Remix_GetErrorString( err ), (int)err );
		s_skyMesh = NULL;
		R_Sky_DestroyMesh();
		s_skyLoaded = false; // do not retry every frame
		return false;
	}
	return true;
}

void R_Sky_SetupCamera( const remixapi_CameraInfoParameterizedEXT *world )
{
	if(( !s_skyLoaded && !R_Sky_CloudsActive()) || !g_remix.SetupCamera )
		return;

	remixapi_CameraInfoParameterizedEXT sky = *world;
	sky.pNext = NULL;
	sky.position.x = sky.position.y = sky.position.z = 0.0f;
	sky.nearPlane = 1.0f;
	sky.farPlane  = SKY_HALF_SIZE * 4.0f;

	remixapi_CameraInfo info;
	memset( &info, 0, sizeof( info ));
	info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
	info.pNext = &sky;
	info.type  = REMIXAPI_CAMERA_TYPE_SKY;
	g_remix.SetupCamera( &info );
}

static void R_Sky_DrawInstance( remixapi_MeshHandle mesh, const void *pNext )
{
	remixapi_InstanceInfo instance;
	memset( &instance, 0, sizeof( instance ));
	instance.sType         = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
	instance.pNext         = pNext;
	instance.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_SKY;
	instance.mesh          = mesh;
	instance.transform     = (remixapi_Transform){ .matrix = {
		{ 1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f },
	} };
	instance.doubleSided = 1;

	remixapi_ErrorCode err = g_remix.DrawInstance( &instance );
	if( err != REMIXAPI_ERROR_CODE_SUCCESS )
	{
		static int s_skyDrawErrors = 0;
		if( ++s_skyDrawErrors <= 5 )
			gEngfuncs.Con_Reportf( S_ERROR "ref_openremix: DrawInstance for sky failed: %s (%d)\n", R_Remix_GetErrorString( err ), (int)err );
	}
}

/*
 * The cloud dome: a cube subdivided so the per-vertex projection below stays smooth
 */
static void R_Sky_BuildCloudGeometry( void )
{
	uint32_t v = 0, n = 0;
	for( int axis = 0; axis < SKY_SIDES; axis++ )
	{
		const uint32_t first = v;
		for( int j = 0; j <= CLOUD_STEPS; j++ )
		{
			for( int i = 0; i <= CLOUD_STEPS; i++ )
			{
				const float s = -1.0f + 2.0f * i / CLOUD_STEPS, t = -1.0f + 2.0f * j / CLOUD_STEPS;
				R_Sky_MakeVertex( s, t, axis, &s_cloudVerts[v++] );
			}
		}
		for( int j = 0; j < CLOUD_STEPS; j++ )
		{
			for( int i = 0; i < CLOUD_STEPS; i++ )
			{
				const uint32_t a = first + j * ( CLOUD_STEPS + 1 ) + i, b = a + 1;
				const uint32_t c = a + CLOUD_STEPS + 1, d = c + 1;
				s_cloudIndices[n++] = a; s_cloudIndices[n++] = c; s_cloudIndices[n++] = d;
				s_cloudIndices[n++] = a; s_cloudIndices[n++] = d; s_cloudIndices[n++] = b;
			}
		}
	}
	s_cloudGeometry = true;
}

/*
 * Each layer's texture is projected onto a flattened sphere around the viewer and scrolls
 * diagonally at its own speed (R_CloudTexCoord in ref/gl/gl_warp.c); the scroll changes the
 * texture coordinates every frame, so the layer's mesh is updated in place
 */
static void R_Sky_DrawClouds( void )
{
	const float brightness = R_Sky_Brightness();
	if( s_cloudMaterials[0] && fabsf( brightness - s_cloudBrightness ) > 0.001f * Q_max( s_cloudBrightness, 1.0f ))
		R_Sky_DestroyClouds();

	if( !s_cloudGeometry )
		R_Sky_BuildCloudGeometry();

	const double time = gp_cl ? gp_cl->time : 0.0;
	static const float speeds[2] = { 8.0f, 16.0f };
	static remixapi_HardcodedVertex layer[CLOUD_VERTS];

	for( int l = 0; l < 2; l++ )
	{
		if( !s_cloudMaterials[l] )
		{
			s_cloudBrightness = brightness;
			s_cloudMaterials[l] = R_Textures_CreateSkyMaterial( s_cloudTextures[l], brightness, true );
			if( !s_cloudMaterials[l] )
				return;
		}

		float scroll = (float)( time * speeds[l] );
		scroll -= (int)scroll & ~127;

		for( uint32_t v = 0; v < CLOUD_VERTS; v++ )
		{
			layer[v] = s_cloudVerts[v];
			vec3_t dir = { layer[v].position[0], layer[v].position[1], layer[v].position[2] * 3.0f };
			const float length = 6.0f * 63.0f / VectorLength( dir );
			layer[v].texcoord[0] = ( scroll + dir[0] * length ) * ( 1.0f / 128.0f );
			layer[v].texcoord[1] = ( scroll + dir[1] * length ) * ( 1.0f / 128.0f );
		}

		remixapi_MeshInfoSurfaceTriangles surface;
		memset( &surface, 0, sizeof( surface ));
		surface.vertices_values = layer;
		surface.vertices_count  = CLOUD_VERTS;
		surface.indices_values  = s_cloudIndices;
		surface.indices_count   = CLOUD_INDICES;
		surface.material        = s_cloudMaterials[l];

		remixapi_MeshInfoUpdateEXT updateExt;
		memset( &updateExt, 0, sizeof( updateExt ));
		updateExt.sType   = REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT;
		updateExt.enabled = 1;

		remixapi_MeshInfo meshInfo;
		memset( &meshInfo, 0, sizeof( meshInfo ));
		meshInfo.sType           = REMIXAPI_STRUCT_TYPE_MESH_INFO;
		meshInfo.pNext           = &updateExt;
		meshInfo.hash            = 0x534B59434C4F5544ULL + l; // "SKYCLOUD"
		meshInfo.surfaces_values = &surface;
		meshInfo.surfaces_count  = 1;

		if( !g_remix.CreateMeshBatched || g_remix.CreateMeshBatched( &meshInfo, &s_cloudMeshes[l] ) != REMIXAPI_ERROR_CODE_SUCCESS )
			return;

		// The alpha layer is blended over the solid one, as kRenderTransTexture
		remixapi_InstanceInfoBlendEXT blendExt;
		memset( &blendExt, 0, sizeof( blendExt ));
		blendExt.sType               = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		blendExt.alphaBlendEnabled   = 1;
		blendExt.srcColorBlendFactor = 6; // SRC_ALPHA
		blendExt.dstColorBlendFactor = 7; // ONE_MINUS_SRC_ALPHA
		blendExt.colorBlendOp        = 0; // ADD
		blendExt.srcAlphaBlendFactor = 1; // ONE
		blendExt.dstAlphaBlendFactor = 7; // ONE_MINUS_SRC_ALPHA
		blendExt.alphaBlendOp        = 0;
		blendExt.writeMask           = 15;

		R_Sky_DrawInstance( s_cloudMeshes[l], l ? &blendExt : NULL );
	}
}

void R_Sky_Draw( void )
{
	if( !g_remix.DrawInstance )
		return;
	if( R_Sky_CloudsActive())
	{
		R_Sky_DrawClouds();
		return;
	}
	if( !s_skyLoaded )
		return;

	// Rebuilt when rt_sky_brightness changes
	const float brightness = R_Sky_Brightness();
	if( s_skyMesh && fabsf( brightness - s_skyBrightness ) > 0.001f * Q_max( s_skyBrightness, 1.0f ))
		R_Sky_DestroyMesh();
	if( !s_skyMesh )
	{
		s_skyBrightness = brightness;
		if( !R_Sky_BuildMesh())
			return;
	}

	R_Sky_DrawInstance( s_skyMesh, NULL );
}
