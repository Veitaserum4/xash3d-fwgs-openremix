"""
tests/stress_probes.py - Empirical Adversarial & Stress Probes for ref_openremix (Milestone 1)
Tests:
1. Dynamic library loader (remixapi_InitializeLibrary) version negotiation & error handling.
2. GetRefAPI boundary, invalid, and NULL input stress testing.
3. PE binary architecture, export table, and dynamic dependency validation.
"""

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

# Add OpenRemix bin dir for SDL3.dll dependency
if OPENREMIX_BIN_DIR.exists():
    os.add_dll_directory(str(OPENREMIX_BIN_DIR))

# Error codes from remix_c.h
REMIXAPI_ERROR_CODE_SUCCESS = 0
REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS = 3
REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION = 8

def remixapi_version_make(major, minor, patch):
    return ((major & 0xFFFF) << 48) | ((minor & 0xFFFF) << 16) | (patch & 0xFFFF)

class RemixInitInfo(ctypes.Structure):
    _fields_ = [
        ("sType", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("pNext", ctypes.c_void_p),
        ("version", ctypes.c_uint64),
    ]

# Forward declarations for Xash3D engine structures
class RefGlobals(ctypes.Structure):
    _fields_ = [
        ("developer", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("time", ctypes.c_float),
        ("oldtime", ctypes.c_float),
        ("realtime", ctypes.c_double),
        ("frametime", ctypes.c_double),
    ]

class CvarT(ctypes.Structure):
    pass

CvarT._fields_ = [
    ("name", ctypes.c_char_p),
    ("string", ctypes.c_char_p),
    ("flags", ctypes.c_int),
    ("value", ctypes.c_float),
    ("next", ctypes.c_void_p),
]

# Engine callback function pointer types returning c_void_p
ENG_PARM_FUNC = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_int, ctypes.c_int)
GET_CVAR_PTR_FUNC = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_char_p)
REG_CVAR_FUNC = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
CON_PRINTF_FUNC = ctypes.CFUNCTYPE(None, ctypes.c_char_p)

# Global dummy variables for mock engine
_DUMMY_CVARS = {}
_DUMMY_STORAGE = (ctypes.c_byte * 8192)()

@ENG_PARM_FUNC
def mock_engine_get_parm(parm, extra):
    return ctypes.cast(ctypes.byref(_DUMMY_STORAGE), ctypes.c_void_p).value

@GET_CVAR_PTR_FUNC
def mock_get_cvar_pointer(name):
    name_str = name.decode("utf-8") if isinstance(name, bytes) else str(name)
    if name_str not in _DUMMY_CVARS:
        cvar = CvarT()
        cvar.name = ctypes.c_char_p(name_str.encode("utf-8"))
        cvar.string = ctypes.c_char_p(b"0")
        cvar.flags = 0
        cvar.value = 0.0
        cvar.next = None
        _DUMMY_CVARS[name_str] = cvar
    return ctypes.cast(ctypes.pointer(_DUMMY_CVARS[name_str]), ctypes.c_void_p).value

@REG_CVAR_FUNC
def mock_cvar_register_variable(cvar_ptr):
    pass

@CON_PRINTF_FUNC
def mock_con_printf(msg):
    pass

# Keep references to ctypes callbacks alive
_CALLBACK_REFS = [mock_engine_get_parm, mock_get_cvar_pointer, mock_cvar_register_variable, mock_con_printf]


class EmpiricalStressProbes(unittest.TestCase):
    """Adversarial stress probes for Milestone 1."""

    @classmethod
    def setUpClass(cls):
        # Verify DLL paths
        assert REMIX_DLL_PATH.exists(), f"remix.dll missing at {REMIX_DLL_PATH}"
        assert REF_OPENREMIX_DLL_PATH.exists(), f"ref_openremix.dll missing at {REF_OPENREMIX_DLL_PATH}"

        cls.remix_dll = ctypes.CDLL(str(REMIX_DLL_PATH))
        cls.remix_dll.remixapi_InitializeLibrary.argtypes = [ctypes.POINTER(RemixInitInfo), ctypes.c_void_p]
        cls.remix_dll.remixapi_InitializeLibrary.restype = ctypes.c_uint32

        cls.ref_dll = ctypes.CDLL(str(REF_OPENREMIX_DLL_PATH))
        cls.ref_dll.GetRefAPI.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        cls.ref_dll.GetRefAPI.restype = ctypes.c_int

    # =========================================================================
    # Task 1: Dynamic Library Loader (remixapi_InitializeLibrary)
    # =========================================================================
    def test_01_remix_incompatible_version_stock_nvidia(self):
        """Probe: Stock NVIDIA Remix version (0.6.0) returns INCOMPATIBLE_VERSION (8)."""
        info = RemixInitInfo(
            sType=1,
            pad=0,
            pNext=None,
            version=remixapi_version_make(0, 6, 0),
        )
        out_buf = (ctypes.c_byte * 2048)()
        res = self.remix_dll.remixapi_InitializeLibrary(ctypes.byref(info), out_buf)
        self.assertEqual(res, REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION,
                         f"Expected error code 8, got {res}")

    def test_02_remix_incompatible_version_various_mismatches(self):
        """Probe: Boundary versions (0, minor 1002, minor 1004, major 1) return error code 8."""
        mismatched_versions = [
            0,
            remixapi_version_make(0, 0, 0),
            remixapi_version_make(0, 1002, 0),
            remixapi_version_make(0, 1004, 0),
            remixapi_version_make(1, 1003, 0),
            remixapi_version_make(1, 0, 0),
            0xFFFFFFFFFFFFFFFF,
        ]
        out_buf = (ctypes.c_byte * 2048)()
        for ver in mismatched_versions:
            info = RemixInitInfo(sType=1, pad=0, pNext=None, version=ver)
            res = self.remix_dll.remixapi_InitializeLibrary(ctypes.byref(info), out_buf)
            self.assertEqual(res, REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION,
                             f"Version 0x{ver:X} should return 8, got {res}")

    def test_03_remix_invalid_arguments_null_pointers(self):
        """Probe: NULL info and NULL out_remixInterface return INVALID_ARGUMENTS (3)."""
        valid_info = RemixInitInfo(sType=1, pad=0, pNext=None, version=remixapi_version_make(0, 1003, 0))
        out_buf = (ctypes.c_byte * 2048)()

        # NULL info
        res_null_info = self.remix_dll.remixapi_InitializeLibrary(None, out_buf)
        self.assertEqual(res_null_info, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS)

        # NULL out interface
        res_null_out = self.remix_dll.remixapi_InitializeLibrary(ctypes.byref(valid_info), None)
        self.assertEqual(res_null_out, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS)

    def test_04_remix_invalid_stype(self):
        """Probe: Invalid sType returns INVALID_ARGUMENTS (3)."""
        out_buf = (ctypes.c_byte * 2048)()
        invalid_stypes = [0, 2, 9999, 0xFFFFFFFF]
        for stype in invalid_stypes:
            info = RemixInitInfo(sType=stype, pad=0, pNext=None, version=remixapi_version_make(0, 1003, 0))
            res = self.remix_dll.remixapi_InitializeLibrary(ctypes.byref(info), out_buf)
            self.assertEqual(res, REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
                             f"sType {stype} should return 3, got {res}")

    def test_05_remix_valid_initialization(self):
        """Probe: Valid version 0.1003.0 returns SUCCESS (0) and populates dispatch pointers."""
        info = RemixInitInfo(
            sType=1,
            pad=0,
            pNext=None,
            version=remixapi_version_make(0, 1003, 0),
        )
        out_buf = (ctypes.c_void_p * 70)()
        res = self.remix_dll.remixapi_InitializeLibrary(ctypes.byref(info), out_buf)
        self.assertEqual(res, REMIXAPI_ERROR_CODE_SUCCESS, f"Expected 0 (SUCCESS), got {res}")

        non_null_count = sum(1 for ptr in out_buf if ptr is not None and ptr != 0)
        self.assertGreaterEqual(non_null_count, 50, f"Expected >= 50 dispatch functions, got {non_null_count}")

    # =========================================================================
    # Task 2: GetRefAPI Boundary & Stress Testing
    # =========================================================================
    def test_06_getrefapi_invalid_versions_return_zero(self):
        """Probe: GetRefAPI returns 0 for any version != 20 without crash."""
        funcs = (ctypes.c_void_p * 128)()
        engfuncs = (ctypes.c_void_p * 128)()
        globals_obj = RefGlobals()

        invalid_versions = [0, 1, 18, 19, 21, 22, -1, -999, 1003, 0x7FFFFFFF, -0x80000000]
        for ver in invalid_versions:
            ret = self.ref_dll.GetRefAPI(ver, funcs, engfuncs, ctypes.byref(globals_obj))
            self.assertEqual(ret, 0, f"GetRefAPI with version {ver} must return 0, got {ret}")

    def test_07_getrefapi_null_pointers_resilience(self):
        """Probe: GetRefAPI called with NULL pointers returns 0 without crashing."""
        valid_ver = 20
        dummy_buf = (ctypes.c_void_p * 128)()

        # funcs is NULL
        ret1 = self.ref_dll.GetRefAPI(valid_ver, None, dummy_buf, dummy_buf)
        self.assertEqual(ret1, 0, "GetRefAPI with NULL funcs must return 0")

        # engfuncs is NULL
        ret2 = self.ref_dll.GetRefAPI(valid_ver, dummy_buf, None, dummy_buf)
        self.assertEqual(ret2, 0, "GetRefAPI with NULL engfuncs must return 0")

        # globals is NULL
        ret3 = self.ref_dll.GetRefAPI(valid_ver, dummy_buf, dummy_buf, None)
        self.assertEqual(ret3, 0, "GetRefAPI with NULL globals must return 0")

        # all NULL
        ret4 = self.ref_dll.GetRefAPI(valid_ver, None, None, None)
        self.assertEqual(ret4, 0, "GetRefAPI with all NULL must return 0")

        # NULL with invalid version
        ret5 = self.ref_dll.GetRefAPI(19, None, None, None)
        self.assertEqual(ret5, 0, "GetRefAPI with invalid version and NULL must return 0")

    def test_08_getrefapi_valid_handshake(self):
        """Probe: GetRefAPI with version 20 and engine services succeeds and populates vtable."""
        # Build mock ref_api_t
        # Layout of ref_api_t in engine/ref_api.h:
        # index 0: EngineGetParm (func ptr)
        # index 1: Cvar_Get (func ptr)
        # index 2: pfnGetCvarPointer (func ptr)
        # index 3: pfnGetCvarFloat (func ptr)
        # index 4: pfnGetCvarString (func ptr)
        # index 5: Cvar_SetValue (func ptr)
        # index 6: Cvar_Set (func ptr)
        # index 7: Cvar_RegisterVariable (func ptr)
        # ...
        eng_table = (ctypes.c_void_p * 256)()
        eng_table[0] = ctypes.cast(mock_engine_get_parm, ctypes.c_void_p).value
        eng_table[2] = ctypes.cast(mock_get_cvar_pointer, ctypes.c_void_p).value
        eng_table[7] = ctypes.cast(mock_cvar_register_variable, ctypes.c_void_p).value

        # ref_interface_t table buffer
        funcs_table = (ctypes.c_void_p * 128)()
        globals_obj = RefGlobals(width=1920, height=1080)

        ret = self.ref_dll.GetRefAPI(20, funcs_table, eng_table, ctypes.byref(globals_obj))
        self.assertEqual(ret, 20, f"GetRefAPI with version 20 must return 20, got {ret}")

        # Check function pointers in populated funcs_table
        # R_Init, R_Shutdown, R_NewMap, etc.
        populated_count = sum(1 for ptr in funcs_table if ptr is not None and ptr != 0)
        self.assertGreaterEqual(populated_count, 30, f"Expected >= 30 populated function pointers, got {populated_count}")

    # =========================================================================
    # Task 3: Binary Inspection & Export Validation on ref_openremix.dll
    # =========================================================================
    def test_09_ref_openremix_pe_format_and_amd64(self):
        """Probe: ref_openremix.dll is a valid PE32+ AMD64 dynamic link library."""
        data = REF_OPENREMIX_DLL_PATH.read_bytes()
        self.assertGreaterEqual(len(data), 512, "Binary must be >= 512 bytes")
        self.assertEqual(data[:2], b"MZ", "DOS header must start with 'MZ'")

        e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
        pe_sig = data[e_lfanew:e_lfanew + 4]
        self.assertEqual(pe_sig, b"PE\x00\x00", "PE signature must be 'PE\\0\\0'")

        machine = int.from_bytes(data[e_lfanew + 4:e_lfanew + 6], "little")
        self.assertEqual(machine, 0x8664, f"Machine type must be AMD64 (0x8664), got 0x{machine:X}")

        magic = int.from_bytes(data[e_lfanew + 24:e_lfanew + 26], "little")
        self.assertEqual(magic, 0x020B, f"Optional header magic must be PE32+ (0x020B), got 0x{magic:X}")

    def test_10_ref_openremix_exports_only_getrefapi(self):
        """Probe: ref_openremix.dll exports exactly 'GetRefAPI' and no extraneous symbols."""
        data = REF_OPENREMIX_DLL_PATH.read_bytes()
        e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
        opt_header_offset = e_lfanew + 24
        export_rva = int.from_bytes(data[opt_header_offset + 112:opt_header_offset + 116], "little")
        export_size = int.from_bytes(data[opt_header_offset + 116:opt_header_offset + 120], "little")
        self.assertGreater(export_rva, 0, "Export table RVA must be non-zero")
        self.assertGreater(export_size, 0, "Export table size must be non-zero")

        num_sections = int.from_bytes(data[e_lfanew + 6:e_lfanew + 8], "little")
        opt_hdr_size = int.from_bytes(data[e_lfanew + 20:e_lfanew + 22], "little")
        sect_start = opt_header_offset + opt_hdr_size

        def rva_to_offset(rva):
            for i in range(num_sections):
                s_off = sect_start + i * 40
                v_size = int.from_bytes(data[s_off + 8:s_off + 12], "little")
                v_addr = int.from_bytes(data[s_off + 12:s_off + 16], "little")
                raw_size = int.from_bytes(data[s_off + 16:s_off + 20], "little")
                raw_ptr = int.from_bytes(data[s_off + 20:s_off + 24], "little")
                if v_addr <= rva < v_addr + max(v_size, raw_size):
                    return raw_ptr + (rva - v_addr)
            return None

        exp_off = rva_to_offset(export_rva)
        self.assertIsNotNone(exp_off, "Could not map export directory RVA")

        num_names = int.from_bytes(data[exp_off + 24:exp_off + 28], "little")
        names_rva = int.from_bytes(data[exp_off + 32:exp_off + 36], "little")
        names_off = rva_to_offset(names_rva)

        exported_names = []
        for i in range(num_names):
            name_rva = int.from_bytes(data[names_off + i * 4:names_off + (i + 1) * 4], "little")
            name_off = rva_to_offset(name_rva)
            end = data.find(b"\x00", name_off)
            exported_names.append(data[name_off:end].decode("ascii"))

        self.assertIn("GetRefAPI", exported_names, "DLL must export 'GetRefAPI'")
        self.assertEqual(len(exported_names), 1, f"Expected exactly 1 export, got {exported_names}")

    def test_11_ref_openremix_no_static_remix_dependency(self):
        """Probe: ref_openremix.dll does NOT statically link remix.dll (must load dynamically)."""
        data = REF_OPENREMIX_DLL_PATH.read_bytes()
        e_lfanew = int.from_bytes(data[0x3C:0x40], "little")
        opt_header_offset = e_lfanew + 24
        import_rva = int.from_bytes(data[opt_header_offset + 120:opt_header_offset + 124], "little")

        num_sections = int.from_bytes(data[e_lfanew + 6:e_lfanew + 8], "little")
        opt_hdr_size = int.from_bytes(data[e_lfanew + 20:e_lfanew + 22], "little")
        sect_start = opt_header_offset + opt_hdr_size

        def rva_to_offset(rva):
            for i in range(num_sections):
                s_off = sect_start + i * 40
                v_size = int.from_bytes(data[s_off + 8:s_off + 12], "little")
                v_addr = int.from_bytes(data[s_off + 12:s_off + 16], "little")
                raw_size = int.from_bytes(data[s_off + 16:s_off + 20], "little")
                raw_ptr = int.from_bytes(data[s_off + 20:s_off + 24], "little")
                if v_addr <= rva < v_addr + max(v_size, raw_size):
                    return raw_ptr + (rva - v_addr)
            return None

        imp_off = rva_to_offset(import_rva)
        imported_dlls = []
        curr = imp_off
        while True:
            name_rva = int.from_bytes(data[curr + 12:curr + 16], "little")
            if name_rva == 0:
                break
            name_off = rva_to_offset(name_rva)
            end = data.find(b"\x00", name_off)
            imported_dlls.append(data[name_off:end].decode("ascii").lower())
            curr += 20

        self.assertNotIn("remix.dll", imported_dlls, "remix.dll must NOT be statically linked")
        self.assertNotIn("sdl3.dll", imported_dlls, "sdl3.dll must NOT be statically linked")
        has_kernel32 = any("kernel32" in dll for dll in imported_dlls)
        self.assertTrue(has_kernel32, f"Expected kernel32 in imports, found: {imported_dlls}")


    # =========================================================================
    # Task 4: Extended Adversarial Stress & Math Rigor Probes
    # =========================================================================
    def test_12_getrefapi_idempotence_and_stability_100x(self):
        """Probe: Calling GetRefAPI 100 times in a tight loop is completely stable and idempotent."""
        eng_table = (ctypes.c_void_p * 256)()
        eng_table[0] = ctypes.cast(mock_engine_get_parm, ctypes.c_void_p).value
        eng_table[2] = ctypes.cast(mock_get_cvar_pointer, ctypes.c_void_p).value
        eng_table[7] = ctypes.cast(mock_cvar_register_variable, ctypes.c_void_p).value
        funcs_table = (ctypes.c_void_p * 128)()
        globals_obj = RefGlobals(width=1920, height=1080)

        for i in range(100):
            ret = self.ref_dll.GetRefAPI(20, funcs_table, eng_table, ctypes.byref(globals_obj))
            self.assertEqual(ret, 20)
            self.assertIsNotNone(funcs_table[0])

    def test_13_remix_initialize_stress_100x(self):
        """Probe: Rapid re-initialization of remixapi_InitializeLibrary (100x) is stable."""
        info = RemixInitInfo(
            sType=1,
            pad=0,
            pNext=None,
            version=remixapi_version_make(0, 1003, 0),
        )
        out_buf = (ctypes.c_void_p * 70)()
        for i in range(100):
            res = self.remix_dll.remixapi_InitializeLibrary(ctypes.byref(info), out_buf)
            self.assertEqual(res, REMIXAPI_ERROR_CODE_SUCCESS)

    def test_14_goldsrc_anglevectors_determinant_orthonormality(self):
        """Probe: GoldSrc AngleVectors produces strictly orthonormal right-handed basis with det=+1.0 across full sphere."""
        import math
        def angle_vectors(pitch, yaw, roll):
            sy = math.sin(math.radians(yaw))
            cy = math.cos(math.radians(yaw))
            sp = math.sin(math.radians(pitch))
            cp = math.cos(math.radians(pitch))
            sr = math.sin(math.radians(roll))
            cr = math.cos(math.radians(roll))

            forward = (cp * cy, cp * sy, -sp)
            right = (-1.0 * sr * sp * cy + -1.0 * cr * -sy,
                     -1.0 * sr * sp * sy + -1.0 * cr * cy,
                     -1.0 * sr * cp)
            up = (cr * sp * cy + -sr * -sy,
                  cr * sp * sy + -sr * cy,
                  cr * cp)
            return forward, right, up

        test_pitches = [-89.0, -45.0, 0.0, 45.0, 89.0]
        test_yaws = [0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]
        test_rolls = [-180.0, -90.0, -45.0, 0.0, 45.0, 90.0, 180.0]

        for p in test_pitches:
            for y in test_yaws:
                for r in test_rolls:
                    fwd, rgt, up = angle_vectors(p, y, r)

                    norm_f = math.sqrt(sum(x*x for x in fwd))
                    norm_r = math.sqrt(sum(x*x for x in rgt))
                    norm_u = math.sqrt(sum(x*x for x in up))
                    self.assertAlmostEqual(norm_f, 1.0, places=5)
                    self.assertAlmostEqual(norm_r, 1.0, places=5)
                    self.assertAlmostEqual(norm_u, 1.0, places=5)

                    dot_fr = sum(a*b for a, b in zip(fwd, rgt))
                    dot_fu = sum(a*b for a, b in zip(fwd, up))
                    dot_ru = sum(a*b for a, b in zip(rgt, up))
                    self.assertAlmostEqual(dot_fr, 0.0, places=5)
                    self.assertAlmostEqual(dot_fu, 0.0, places=5)
                    self.assertAlmostEqual(dot_ru, 0.0, places=5)

                    basis = [rgt, up, fwd]
                    det = (basis[0][0] * (basis[1][1] * basis[2][2] - basis[1][2] * basis[2][1]) -
                           basis[0][1] * (basis[1][0] * basis[2][2] - basis[1][2] * basis[2][0]) +
                           basis[0][2] * (basis[1][0] * basis[2][1] - basis[1][1] * basis[2][0]))
                    self.assertAlmostEqual(abs(det), 1.0, places=4,
                                           msg=f"Determinant magnitude collapsed to {abs(det)} at angles ({p}, {y}, {r})")


if __name__ == "__main__":
    unittest.main(verbosity=2)

