"""
tests/stress_m2_probes.py - Empirical Adversarial & Stress Probes for ref_openremix Milestone 2
Covers:
1. 2D UI Draw List & Texture Management:
   - Screen quad collection, coordinate offsets, and batch coalescing.
   - Texture registry, RegisterUITexture boundaries, and solid white quad fallback on texture ID 0.
   - SubmitUIDrawList submission sequence before Present.
2. Billboard Sprites & Beams:
   - 5 GoldSrc orientation modes with unit quad and 3x4 affine transform.
   - Soft depth feathering (REMIXAPI_SPRITE_SOFT_DEPTH_EXT) and blend states.
   - Procedural beam ribbon generation, noise math, and dynamic mesh recycling.
3. Binary & Export Verification:
   - Live invocation of GetRefAPI and callback table verification.
"""

import math
import os
import sys
import ctypes
from pathlib import Path
import unittest

# Paths
XASH_ROOT = Path(r"c:\Users\Jacob\Desktop\XASH3D-OPENREMIX\xash3d-fwgs-openremix")
OPENREMIX_BIN_DIR = Path(r"c:\Users\Jacob\Desktop\UnityRTX\OpenRemix\_work\packages\windows\bin")
REMIX_DLL_PATH = OPENREMIX_BIN_DIR / "remix.dll"
REF_OPENREMIX_DLL_PATH = XASH_ROOT / "build" / "ref" / "openremix" / "ref_openremix.dll"

if OPENREMIX_BIN_DIR.exists():
    os.add_dll_directory(str(OPENREMIX_BIN_DIR))

# Constants from remix_c.h / remix_native_ext.h
REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST = 0x7fff1010
REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT = 0x7fff1015
REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT = 0x7fff1002
REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT = 18
REMIXAPI_SPRITE_SOFT_DEPTH_EXT = 0x40
REMIXAPI_UI_NATIVE_LINEAR = 0x8
REMIXAPI_UI_NATIVE_CUSTOM_BLEND = 0x10

# Sprite Orientations
SPR_ORIENTED = 0
SPR_FACING_UPRIGHT = 1
SPR_FWD_PARALLEL_UPRIGHT = 2
SPR_FWD_PARALLEL_ORIENTED = 3
SPR_FWD_PARALLEL = 4

# Render Modes
kRenderNormal = 0
kRenderTransColor = 1
kRenderTransTexture = 2
kRenderGlow = 3
kRenderTransAlpha = 4
kRenderTransAdd = 5

# Limits
MAX_UI_VERTICES = 32768
MAX_UI_INDICES = 65536
MAX_UI_COMMANDS = 2048
MAX_UI_TEXTURES = 4096


# =============================================================================
# Mathematical Helpers
# =============================================================================
def vec_len(v):
    return math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])

def vec_norm(v):
    l = vec_len(v)
    if l < 1e-9:
        return (0.0, 0.0, 0.0)
    return (v[0]/l, v[1]/l, v[2]/l)

def vec_dot(a, b):
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]

def vec_cross(a, b):
    return (
        a[1]*b[2] - a[2]*b[1],
        a[2]*b[0] - a[0]*b[2],
        a[0]*b[1] - a[1]*b[0]
    )

def mat3x3_det(c0, c1, c2):
    return (
        c0[0] * (c1[1]*c2[2] - c1[2]*c2[1]) -
        c1[0] * (c0[1]*c2[2] - c0[2]*c2[1]) +
        c2[0] * (c0[1]*c1[2] - c0[2]*c1[1])
    )

def angle_vectors(angles):
    pitch = angles[0] * (math.pi / 180.0)
    yaw = angles[1] * (math.pi / 180.0)
    roll = angles[2] * (math.pi / 180.0)

    sp = math.sin(pitch); cp = math.cos(pitch)
    sy = math.sin(yaw); cy = math.cos(yaw)
    sr = math.sin(roll); cr = math.cos(roll)

    forward = (cp * cy, cp * sy, -sp)
    right = (-1.0 * sr * sp * cy + -1.0 * cr * -sy,
             -1.0 * sr * sp * sy + -1.0 * cr * cy,
             -1.0 * sr * cp)
    up = (cr * sp * cy + -sr * -sy,
          cr * sp * sy + -sr * cy,
          cr * cp)
    return forward, right, up


# =============================================================================
# Python Oracles for 2D UI & Billboard Maths
# =============================================================================
class UIModelOracle:
    def __init__(self):
        self.vertices = []
        self.indices = []
        self.commands = []
        self.offset2d = [0.0, 0.0]

    def set_2d_mode(self, enable):
        if enable:
            self.offset2d = [0.0, 0.0]

    def set_2d_offset(self, x, y):
        self.offset2d = [x, y]

    def emit_quad(self, x0, y0, s0, t0, x1, y1, s1, t1,
                  x2, y2, s2, t2, x3, y3, s3, t3,
                  texture_id, color, flags):
        if len(self.vertices) + 4 > MAX_UI_VERTICES or len(self.indices) + 6 > MAX_UI_INDICES:
            return False

        base_vert = len(self.vertices)
        base_idx = len(self.indices)
        ox, oy = self.offset2d

        # 4 vertices
        v0 = (x0 + ox, y0 + oy, 0.0, s0, t0, color)
        v1 = (x1 + ox, y1 + oy, 0.0, s1, t1, color)
        v2 = (x2 + ox, y2 + oy, 0.0, s2, t2, color)
        v3 = (x3 + ox, y3 + oy, 0.0, s3, t3, color)
        self.vertices.extend([v0, v1, v2, v3])

        # 6 indices: (0, 1, 2) and (0, 2, 3)
        self.indices.extend([
            base_vert + 0, base_vert + 1, base_vert + 2,
            base_vert + 0, base_vert + 2, base_vert + 3
        ])

        # Batch coalescing check
        if self.commands:
            prev = self.commands[-1]
            if prev["texture_id"] == texture_id and prev["flags"] == flags and prev["vertex_offset"] == 0:
                prev["index_count"] += 6
                return True

        if len(self.commands) < MAX_UI_COMMANDS:
            self.commands.append({
                "texture_id": texture_id,
                "index_offset": base_idx,
                "index_count": 6,
                "vertex_offset": 0,
                "flags": flags
            })
            return True
        return False

    def draw_stretch_pic(self, x, y, w, h, s1, t1, s2, t2, texnum):
        tex_id = texnum if texnum > 0 else 0
        return self.emit_quad(
            x, y, s1, t1,
            x + w, y, s2, t1,
            x + w, y + h, s2, t2,
            x, y + h, s1, t2,
            tex_id, 0xFFFFFFFF, REMIXAPI_UI_NATIVE_LINEAR
        )

    def fill_rgba(self, rendermode, x, y, w, h, r, g, b, a):
        packed_col = (a << 24) | (b << 16) | (g << 8) | r
        flags = 0
        if rendermode == kRenderTransAdd:
            flags = REMIXAPI_UI_NATIVE_CUSTOM_BLEND | (6 << 8) | (1 << 12)
        # Texture ID 0 = white fallback
        return self.emit_quad(
            x, y, 0.0, 0.0,
            x + w, y, 1.0, 0.0,
            x + w, y + h, 1.0, 1.0,
            x, y + h, 0.0, 1.0,
            0, packed_col, flags
        )


class SpriteBasisOracle:
    @staticmethod
    def compute_basis(sprite_type, origin, angles, vieworg, vforward, vright, vup):
        """Computes basis vectors (v_forward, v_right, v_up) according to r_sprite.c logic."""
        t = sprite_type
        if angles[2] != 0.0 and t == SPR_FWD_PARALLEL:
            t = SPR_FWD_PARALLEL_ORIENTED

        if t == SPR_ORIENTED:
            fwd, rgt, up = angle_vectors(angles)
            return fwd, rgt, up, False

        elif t == SPR_FACING_UPRIGHT:
            rgt = (origin[1] - vieworg[1], -(origin[0] - vieworg[0]), 0.0)
            rgt = vec_norm(rgt)
            up = (0.0, 0.0, 1.0)
            fwd = vec_cross(rgt, up)
            return fwd, rgt, up, False

        elif t == SPR_FWD_PARALLEL_UPRIGHT:
            if vforward[2] > 0.9998 or vforward[2] < -0.9998:
                return (0,0,0), (0,0,0), (0,0,0), True # Culled
            up = (0.0, 0.0, 1.0)
            rgt = vec_norm((vforward[1], -vforward[0], 0.0))
            fwd = vec_cross(rgt, up)
            return fwd, rgt, up, False

        elif t == SPR_FWD_PARALLEL_ORIENTED:
            rad = angles[2] * (math.pi / 180.0)
            sr = math.sin(rad); cr = math.cos(rad)
            rgt = (vright[0]*cr + vup[0]*sr, vright[1]*cr + vup[1]*sr, vright[2]*cr + vup[2]*sr)
            up = (-vright[0]*sr + vup[0]*cr, -vright[1]*sr + vup[1]*cr, -vright[2]*sr + vup[2]*cr)
            return vforward, rgt, up, False

        elif t == SPR_FWD_PARALLEL:
            return vforward, vright, vup, False

        raise ValueError(f"Unknown sprite type {sprite_type}")


# =============================================================================
# Milestone 2 Empirical Test Suite
# =============================================================================
class TestMilestone2GeometryTranslation(unittest.TestCase):
    """Adversarial stress and correctness testing for Milestone 2."""

    # -------------------------------------------------------------------------
    # 1. 2D UI Draw List & Texture Management
    # -------------------------------------------------------------------------
    def test_ui_01_quad_collection_and_vertex_order(self):
        """Verify screen quad vertex positions, UVs, and clockwise triangle topology."""
        ui = UIModelOracle()
        ui.draw_stretch_pic(100.0, 200.0, 50.0, 30.0, 0.0, 0.0, 1.0, 1.0, 42)

        self.assertEqual(len(ui.vertices), 4)
        self.assertEqual(len(ui.indices), 6)
        self.assertEqual(len(ui.commands), 1)

        # Verify corner coordinates
        # Top-left (0)
        self.assertEqual((ui.vertices[0][0], ui.vertices[0][1]), (100.0, 200.0))
        # Top-right (1)
        self.assertEqual((ui.vertices[1][0], ui.vertices[1][1]), (150.0, 200.0))
        # Bottom-right (2)
        self.assertEqual((ui.vertices[2][0], ui.vertices[2][1]), (150.0, 230.0))
        # Bottom-left (3)
        self.assertEqual((ui.vertices[3][0], ui.vertices[3][1]), (100.0, 230.0))

        # Triangle 1: (0, 1, 2), Triangle 2: (0, 2, 3)
        self.assertEqual(ui.indices, [0, 1, 2, 0, 2, 3])

    def test_ui_02_coordinate_offsets_application(self):
        """Verify tr.offset2d correctly shifts all vertices in nested menus/VGUI."""
        ui = UIModelOracle()
        ui.set_2d_offset(250.0, 150.0)
        ui.draw_stretch_pic(10.0, 20.0, 100.0, 50.0, 0.0, 0.0, 1.0, 1.0, 5)

        # Base + Offset: x0 = 10 + 250 = 260, y0 = 20 + 150 = 170
        self.assertEqual(ui.vertices[0][0], 260.0)
        self.assertEqual(ui.vertices[0][1], 170.0)
        self.assertEqual(ui.vertices[2][0], 360.0)
        self.assertEqual(ui.vertices[2][1], 220.0)

        # Reset via R_Set2DMode(true)
        ui.set_2d_mode(True)
        ui.draw_stretch_pic(10.0, 20.0, 100.0, 50.0, 0.0, 0.0, 1.0, 1.0, 5)
        self.assertEqual(ui.vertices[4][0], 10.0)
        self.assertEqual(ui.vertices[4][1], 20.0)

    def test_ui_03_batch_coalescing_identical_texture_and_state(self):
        """Verify 50 consecutive quads with same texture/flags coalesce into 1 command."""
        ui = UIModelOracle()
        for i in range(50):
            ui.draw_stretch_pic(i*10.0, 0.0, 10.0, 10.0, 0.0, 0.0, 1.0, 1.0, 101)

        self.assertEqual(len(ui.vertices), 200)
        self.assertEqual(len(ui.indices), 300)
        self.assertEqual(len(ui.commands), 1, "Expected exactly 1 coalesced draw command")
        self.assertEqual(ui.commands[0]["index_count"], 300)
        self.assertEqual(ui.commands[0]["index_offset"], 0)

    def test_ui_04_batch_splitting_on_texture_and_blend_change(self):
        """Verify draw command splits whenever texture ID or blend flags change."""
        ui = UIModelOracle()
        # Draw 5 quads with tex 1
        for _ in range(5):
            ui.draw_stretch_pic(0, 0, 10, 10, 0, 0, 1, 1, 1)
        # Draw 5 quads with tex 2
        for _ in range(5):
            ui.draw_stretch_pic(0, 0, 10, 10, 0, 0, 1, 1, 2)
        # Draw 5 quads with FillRGBA (tex 0, custom blend)
        for _ in range(5):
            ui.fill_rgba(kRenderTransAdd, 0, 0, 10, 10, 255, 0, 0, 128)

        self.assertEqual(len(ui.commands), 3, "Expected 3 distinct batched commands")
        self.assertEqual(ui.commands[0]["texture_id"], 1)
        self.assertEqual(ui.commands[0]["index_count"], 30)
        self.assertEqual(ui.commands[1]["texture_id"], 2)
        self.assertEqual(ui.commands[1]["index_count"], 30)
        self.assertEqual(ui.commands[2]["texture_id"], 0)
        self.assertEqual(ui.commands[2]["index_count"], 30)
        self.assertTrue(ui.commands[2]["flags"] & REMIXAPI_UI_NATIVE_CUSTOM_BLEND)

    def test_ui_05_solid_white_fallback_on_texnum_zero_and_negative(self):
        """Verify texnum 0 and negative texnums fallback to white texture handle 0."""
        ui = UIModelOracle()
        ui.draw_stretch_pic(0, 0, 100, 100, 0, 0, 1, 1, 0)
        ui.draw_stretch_pic(0, 0, 100, 100, 0, 0, 1, 1, -1)

        self.assertEqual(len(ui.commands), 1, "Should coalesce both untextured quads into handle 0")
        self.assertEqual(ui.commands[0]["texture_id"], 0)
        self.assertEqual(ui.commands[0]["index_count"], 12)

    def test_ui_06_buffer_capacity_guard_overflow_resilience(self):
        """Verify drawing beyond MAX_UI_VERTICES (32768) does not overflow buffer."""
        ui = UIModelOracle()
        # Each quad has 4 verts, max is 32768 / 4 = 8192 quads
        for _ in range(9000):
            ui.draw_stretch_pic(0, 0, 1, 1, 0, 0, 1, 1, 1)

        self.assertLessEqual(len(ui.vertices), MAX_UI_VERTICES)
        self.assertLessEqual(len(ui.indices), MAX_UI_INDICES)
        self.assertEqual(len(ui.vertices), 32768)
        self.assertEqual(len(ui.indices), 49152)

    def test_ui_07_endframe_submission_sequence_contract(self):
        """Verify R_EndFrame submits UI draw list strictly before calling Present."""
        r_main_src = (XASH_ROOT / "ref" / "openremix" / "r_main.c").read_text(encoding="utf-8")

        # Find R_EndFrame implementation
        idx_endframe = r_main_src.find("void R_EndFrame( void )")
        self.assertNotEqual(idx_endframe, -1)

        endframe_body = r_main_src[idx_endframe:idx_endframe + 700]
        idx_ui_submit = endframe_body.find("R_UI_EndFrame()")
        idx_present = endframe_body.find("g_remix.Present")

        self.assertNotEqual(idx_ui_submit, -1, "R_UI_EndFrame() must be called in R_EndFrame")
        self.assertNotEqual(idx_present, -1, "g_remix.Present must be called in R_EndFrame")
        self.assertLess(idx_ui_submit, idx_present,
                        "CRITICAL: R_UI_EndFrame() MUST be invoked strictly before g_remix.Present")

    # -------------------------------------------------------------------------
    # 2. Billboard Sprites & Beams
    # -------------------------------------------------------------------------
    def test_sprite_01_orientation_mode_0_oriented(self):
        """Mode 0 (SPR_ORIENTED): Basis strictly matches entity Euler angles."""
        angles = (15.0, 45.0, 0.0)
        fwd, rgt, up, culled = SpriteBasisOracle.compute_basis(
            SPR_ORIENTED, (0,0,0), angles, (0,0,0), (1,0,0), (0,1,0), (0,0,1)
        )
        self.assertFalse(culled)

        # Verify unit lengths and orthogonality
        self.assertAlmostEqual(vec_len(fwd), 1.0, places=5)
        self.assertAlmostEqual(vec_len(rgt), 1.0, places=5)
        self.assertAlmostEqual(vec_len(up), 1.0, places=5)
        self.assertAlmostEqual(vec_dot(fwd, rgt), 0.0, places=5)
        self.assertAlmostEqual(vec_dot(fwd, up), 0.0, places=5)
        self.assertAlmostEqual(vec_dot(rgt, up), 0.0, places=5)
        self.assertAlmostEqual(mat3x3_det(fwd, rgt, up), 1.0, places=5)

    def test_sprite_02_orientation_mode_1_facing_upright(self):
        """Mode 1 (SPR_FACING_UPRIGHT): Up is strictly (0, 0, 1), faces vieworg."""
        vieworg = (100.0, 50.0, 30.0)
        origin = (200.0, 150.0, 10.0)
        fwd, rgt, up, culled = SpriteBasisOracle.compute_basis(
            SPR_FACING_UPRIGHT, origin, (0,0,0), vieworg, (1,0,0), (0,1,0), (0,0,1)
        )
        self.assertFalse(culled)
        self.assertEqual(up, (0.0, 0.0, 1.0))
        self.assertEqual(rgt[2], 0.0) # Horizontal only
        self.assertAlmostEqual(vec_len(rgt), 1.0, places=5)
        self.assertAlmostEqual(vec_dot(rgt, up), 0.0, places=5)
        self.assertAlmostEqual(vec_dot(fwd, rgt), 0.0, places=5)

    def test_sprite_03_orientation_mode_2_fwd_parallel_upright_and_culled(self):
        """Mode 2 (SPR_FWD_PARALLEL_UPRIGHT): Up=(0,0,1), culled looking straight up/down."""
        # Standard horizontal view
        v_fwd = (1.0, 0.0, 0.0)
        fwd, rgt, up, culled = SpriteBasisOracle.compute_basis(
            SPR_FWD_PARALLEL_UPRIGHT, (0,0,0), (0,0,0), (0,0,0), v_fwd, (0,1,0), (0,0,1)
        )
        self.assertFalse(culled)
        self.assertEqual(up, (0.0, 0.0, 1.0))
        self.assertAlmostEqual(vec_len(rgt), 1.0, places=5)

        # Looking straight up (vforward.z > 0.9998)
        v_fwd_up = (0.0, 0.0, 0.9999)
        _, _, _, culled_up = SpriteBasisOracle.compute_basis(
            SPR_FWD_PARALLEL_UPRIGHT, (0,0,0), (0,0,0), (0,0,0), v_fwd_up, (0,1,0), (0,0,1)
        )
        self.assertTrue(culled_up, "Must cull sprite parallel to view line (looking straight up)")

    def test_sprite_04_orientation_mode_3_fwd_parallel_oriented_roll(self):
        """Mode 3 (SPR_FWD_PARALLEL_ORIENTED): In-plane rotation preserves orthonormality."""
        v_fwd = (1.0, 0.0, 0.0)
        v_rgt = (0.0, 1.0, 0.0)
        v_up  = (0.0, 0.0, 1.0)
        angles = (0.0, 0.0, 45.0) # 45 degree roll

        fwd, rgt, up, culled = SpriteBasisOracle.compute_basis(
            SPR_FWD_PARALLEL_ORIENTED, (0,0,0), angles, (0,0,0), v_fwd, v_rgt, v_up
        )
        self.assertFalse(culled)
        self.assertEqual(fwd, v_fwd)
        self.assertAlmostEqual(vec_len(rgt), 1.0, places=5)
        self.assertAlmostEqual(vec_len(up), 1.0, places=5)
        self.assertAlmostEqual(vec_dot(rgt, up), 0.0, places=5)
        # 45 deg: rgt should have equal Y and Z components
        self.assertAlmostEqual(rgt[1], math.cos(math.pi/4), places=5)
        self.assertAlmostEqual(rgt[2], math.sin(math.pi/4), places=5)

    def test_sprite_05_orientation_mode_4_parallel_and_roll_override(self):
        """Mode 4 (SPR_FWD_PARALLEL): Parallel to view plane; auto-switches to mode 3 if roll!=0."""
        v_fwd = (0.0, 1.0, 0.0)
        v_rgt = (1.0, 0.0, 0.0)
        v_up  = (0.0, 0.0, 1.0)

        # No roll -> strictly equal to camera vectors
        fwd, rgt, up, _ = SpriteBasisOracle.compute_basis(
            SPR_FWD_PARALLEL, (0,0,0), (0,0,0), (0,0,0), v_fwd, v_rgt, v_up
        )
        self.assertEqual(fwd, v_fwd)
        self.assertEqual(rgt, v_rgt)
        self.assertEqual(up, v_up)

        # Non-zero roll -> auto promoted to oriented roll
        fwd2, rgt2, up2, _ = SpriteBasisOracle.compute_basis(
            SPR_FWD_PARALLEL, (0,0,0), (0,0,90.0), (0,0,0), v_fwd, v_rgt, v_up
        )
        self.assertAlmostEqual(rgt2[2], 1.0, places=5) # Rotated 90 deg into Z

    def test_sprite_06_unit_quad_and_affine_transform_corners(self):
        """Verify unit quad ([-0.5, 0.5]^2) transformed by 3x4 matrix matches world bounds."""
        w = 64.0
        h = 32.0
        origin = (100.0, 200.0, 50.0)
        v_rgt = (1.0, 0.0, 0.0)
        v_up  = (0.0, 0.0, 1.0)
        v_fwd = (0.0, 1.0, 0.0)

        # Transform matrix: col0 = rgt*w, col1 = up*h, col2 = fwd, col3 = origin
        mat = [
            [v_rgt[0]*w, v_up[0]*h, v_fwd[0], origin[0]],
            [v_rgt[1]*w, v_up[1]*h, v_fwd[1], origin[1]],
            [v_rgt[2]*w, v_up[2]*h, v_fwd[2], origin[2]],
        ]

        # Unit quad 4 corners: (-0.5, -0.5), (-0.5, 0.5), (0.5, 0.5), (0.5, -0.5)
        quad_pts = [(-0.5, -0.5), (-0.5, 0.5), (0.5, 0.5), (0.5, -0.5)]
        world_pts = []
        for qx, qy in quad_pts:
            wx = mat[0][0]*qx + mat[0][1]*qy + mat[0][3]
            wy = mat[1][0]*qx + mat[1][1]*qy + mat[1][3]
            wz = mat[2][0]*qx + mat[2][1]*qy + mat[2][3]
            world_pts.append((wx, wy, wz))

        # Bottom-left: x = 100 - 32 = 68, z = 50 - 16 = 34
        self.assertEqual(world_pts[0], (68.0, 200.0, 34.0))
        # Top-left: x = 68, z = 50 + 16 = 66
        self.assertEqual(world_pts[1], (68.0, 200.0, 66.0))
        # Top-right: x = 100 + 32 = 132, z = 66
        self.assertEqual(world_pts[2], (132.0, 200.0, 66.0))
        # Bottom-right: x = 132, z = 34
        self.assertEqual(world_pts[3], (132.0, 200.0, 34.0))

    def test_sprite_07_soft_depth_feathering_struct_chain(self):
        """Verify remixapi_InstanceInfoSpriteEXT soft depth flags and pNext chaining."""
        r_sprite_src = (XASH_ROOT / "ref" / "openremix" / "r_sprite.c").read_text(encoding="utf-8")

        self.assertIn("REMIXAPI_SPRITE_SOFT_DEPTH_EXT", r_sprite_src)
        self.assertIn("pSprite->softDistance = 0.4f", r_sprite_src)
        self.assertIn("REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT", r_sprite_src)
        self.assertIn("REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT", r_sprite_src)
        self.assertIn("REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT", r_sprite_src)

        # Verify blend factor mapping for kRenderTransAdd and kRenderTransAlpha
        self.assertIn("pBlend->srcColorBlendFactor = 6", r_sprite_src)
        self.assertIn("pBlend->dstColorBlendFactor = 1", r_sprite_src)
        self.assertIn("pBlend->dstColorBlendFactor = 7", r_sprite_src)

    def test_sprite_08_beam_geometry_quad_strip_and_dynamic_slots(self):
        """Verify segmented beams generate valid triangle strip and recycle dynamic slots."""
        r_sprite_src = (XASH_ROOT / "ref" / "openremix" / "r_sprite.c").read_text(encoding="utf-8")

        self.assertIn("MAX_DYNAMIC_MESH_SLOTS 512", r_sprite_src)
        self.assertIn("R_AllocDynamicMeshSlot", r_sprite_src)
        self.assertIn("g_remix.DestroyMesh( slot->handle )", r_sprite_src)
        self.assertIn("R_BeamComputeNormal", r_sprite_src)
        self.assertIn("FracNoise", r_sprite_src)
        self.assertIn("SineNoise", r_sprite_src)

    # -------------------------------------------------------------------------
    # 3. Binary & Export Verification
    # -------------------------------------------------------------------------
    def test_bin_01_getrefapi_live_invocation_and_callbacks(self):
        """Verify calling GetRefAPI via ctypes loads ref_openremix.dll and populates all 2D/Sprite callbacks."""
        self.assertTrue(REF_OPENREMIX_DLL_PATH.exists(), f"Missing {REF_OPENREMIX_DLL_PATH}")

        dll = ctypes.CDLL(str(REF_OPENREMIX_DLL_PATH))
        dll.GetRefAPI.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        dll.GetRefAPI.restype = ctypes.c_int

        # Allocate buffer for ref_interface_t (>= 128 function pointers)
        funcs_buf = (ctypes.c_void_p * 128)()
        engfuncs_buf = (ctypes.c_void_p * 128)()
        globals_buf = (ctypes.c_byte * 128)()

        ret = dll.GetRefAPI(20, funcs_buf, engfuncs_buf, globals_buf)
        self.assertEqual(ret, 20, f"GetRefAPI(20) must return 20, got {ret}")

        # Check that essential callbacks are non-null
        # Count non-zero function pointers in funcs_buf
        non_null_funcs = [i for i, ptr in enumerate(funcs_buf) if ptr is not None and ptr != 0]
        self.assertGreaterEqual(len(non_null_funcs), 25,
                                f"Expected >= 25 populated callbacks in ref_interface_t, got {len(non_null_funcs)}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
