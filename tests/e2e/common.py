"""
tests/e2e/common.py - Common Utilities for ref_openremix E2E Test Suite
Provides PE binary inspection, Remix C API constants, Xash3D RefAPI definitions,
mathematical validators (FNV-1a, coordinate transforms), and test fixture helpers.
"""

import os
import sys
import struct
import math
import re
from pathlib import Path

# Paths
PROJECT_ROOT = Path(r"c:\Users\Jacob\Desktop\XASH3D-OPENREMIX\xash3d-fwgs-openremix")
OPENREMIX_ROOT = Path(r"c:\Users\Jacob\Desktop\UnityRTX\OpenRemix")
REMIX_DLL_PATH = OPENREMIX_ROOT / "_work" / "packages" / "windows" / "bin" / "remix.dll"
SDL3_DLL_PATH = OPENREMIX_ROOT / "_work" / "packages" / "windows" / "bin" / "SDL3.dll"
REMIX_C_HEADER = OPENREMIX_ROOT / "public" / "include" / "remix" / "remix_c.h"
REMIX_NATIVE_EXT_HEADER = OPENREMIX_ROOT / "public" / "include" / "remix" / "remix_native_ext.h"
ENGINE_REF_API_HEADER = PROJECT_ROOT / "engine" / "ref_api.h"
COMMON_COM_MODEL_HEADER = PROJECT_ROOT / "common" / "com_model.h"
COMMON_CONST_HEADER = PROJECT_ROOT / "common" / "const.h"
ROOT_WSCRIPT = PROJECT_ROOT / "wscript"
REF_OPENREMIX_DIR = PROJECT_ROOT / "ref" / "openremix"

# Expected Contract Constants
REF_API_VERSION_EXPECTED = 20
REMIXAPI_VERSION_MAJOR_EXPECTED = 0
REMIXAPI_VERSION_MINOR_EXPECTED = 1003
REMIXAPI_VERSION_PATCH_EXPECTED = 0
REMIXAPI_ERROR_CODE_SUCCESS = 0
REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION = 8
REMIXAPI_ERROR_CODE_INVALID_ARGUMENT = 2
REMIXAPI_ERROR_CODE_INVALID_PATH = 4
PE_MACHINE_AMD64 = 0x8664  # IMAGE_FILE_MACHINE_AMD64

def remixapi_version_make(major: int, minor: int, patch: int) -> int:
    """Computes the 64-bit packed Remix API version."""
    return ((major & 0xFFFF) << 48) | ((minor & 0xFFFF) << 16) | (patch & 0xFFFF)

REMIXAPI_COMPILED_VERSION = remixapi_version_make(
    REMIXAPI_VERSION_MAJOR_EXPECTED,
    REMIXAPI_VERSION_MINOR_EXPECTED,
    REMIXAPI_VERSION_PATCH_EXPECTED,
)

def fnv1a_64(data: bytes, seed: int = 0xCBF29CE484222325) -> int:
    """Computes standard 64-bit FNV-1a hash according to OpenRemix modding spec."""
    FNV_PRIME = 0x100000001B3
    MASK64 = 0xFFFFFFFFFFFFFFFF
    h = seed & MASK64
    for b in data:
        h = (h ^ b) & MASK64
        h = (h * FNV_PRIME) & MASK64
    return h

def fnv1a_64_str(text: str) -> int:
    """Computes 64-bit FNV-1a hash of normalized string (lowercase, forward slashes)."""
    norm = text.strip().replace("\\", "/").lower().encode("utf-8")
    return fnv1a_64(norm)

class PEBinaryInspector:
    """Parses Windows PE32+ (AMD64 64-bit) binary files without external dependencies."""

    def __init__(self, filepath: Path):
        self.filepath = Path(filepath)
        self.exists = self.filepath.exists()
        self.is_valid_pe = False
        self.is_amd64 = False
        self.is_pe32_plus = False
        self.exports = []
        self.export_rvas = {}
        self.sections = []
        self.subsystem = 0
        if self.exists:
            self._parse()

    def _parse(self):
        try:
            with open(self.filepath, "rb") as f:
                dos_header = f.read(64)
                if len(dos_header) < 64 or dos_header[:2] != b"MZ":
                    return
                pe_offset = struct.unpack("<I", dos_header[60:64])[0]
                f.seek(pe_offset)
                pe_sig = f.read(4)
                if pe_sig != b"PE\x00\x00":
                    return
                self.is_valid_pe = True

                # COFF Header (20 bytes)
                coff = f.read(20)
                machine, num_sections, timedatestamp, _, _, opt_hdr_size, characteristics = struct.unpack(
                    "<HHIIIHH", coff
                )
                self.is_amd64 = (machine == PE_MACHINE_AMD64)

                # Optional Header
                opt_magic = struct.unpack("<H", f.read(2))[0]
                self.is_pe32_plus = (opt_magic == 0x20B)  # 0x20B = PE32+ (64-bit)

                if not self.is_pe32_plus:
                    return

                # Read remaining standard & windows specific fields
                f.seek(pe_offset + 24 + 68)  # Subsystem offset in PE32+
                self.subsystem = struct.unpack("<H", f.read(2))[0]

                # Data Directory starts at offset 112 from beginning of Optional Header
                f.seek(pe_offset + 24 + 112)
                export_rva, export_size = struct.unpack("<II", f.read(8))

                # Section headers start at pe_offset + 24 + opt_hdr_size
                f.seek(pe_offset + 24 + opt_hdr_size)
                self.sections = []
                for _ in range(num_sections):
                    sdata = f.read(40)
                    if len(sdata) < 40 or sdata[0] == 0:
                        break
                    sname = sdata[:8].rstrip(b"\x00").decode("latin1", errors="ignore")
                    vsize, vrva, rsize, roff = struct.unpack("<IIII", sdata[8:24])
                    self.sections.append((sname, vrva, vsize, roff, rsize))

                if export_rva == 0 or export_size == 0:
                    return

                exp_off = self._rva_to_offset(export_rva)
                if exp_off is None:
                    return

                f.seek(exp_off)
                exp_dir = f.read(40)
                if len(exp_dir) < 40:
                    return
                num_funcs, num_names, addr_funcs, addr_names, addr_ord = struct.unpack(
                    "<IIIII", exp_dir[20:40]
                )

                names_off = self._rva_to_offset(addr_names)
                ords_off = self._rva_to_offset(addr_ord)
                funcs_off = self._rva_to_offset(addr_funcs)
                if not (names_off and ords_off and funcs_off):
                    return

                f.seek(names_off)
                name_rvas = [struct.unpack("<I", f.read(4))[0] for _ in range(num_names)]

                f.seek(ords_off)
                ordinals = [struct.unpack("<H", f.read(2))[0] for _ in range(num_names)]

                for nrva, ord_idx in zip(name_rvas, ordinals):
                    noff = self._rva_to_offset(nrva)
                    if noff:
                        f.seek(noff)
                        s = bytearray()
                        while True:
                            c = f.read(1)
                            if not c or c == b"\x00":
                                break
                            s.extend(c)
                        sym_name = s.decode("utf-8", errors="ignore")
                        self.exports.append(sym_name)

                        # Lookup function RVA
                        f.seek(funcs_off + ord_idx * 4)
                        func_rva = struct.unpack("<I", f.read(4))[0]
                        self.export_rvas[sym_name] = func_rva
        except Exception:
            pass

    def _rva_to_offset(self, rva: int):
        for _, vrva, vsize, roff, rsize in self.sections:
            span = max(vsize, rsize)
            if vrva <= rva < vrva + span:
                return roff + (rva - vrva)
        return None

    def has_export(self, symbol_name: str) -> bool:
        return symbol_name in self.exports

class BSPLightParser:
    """Reference implementation of xash-rt BSP entity light parsing (RT_ParseStaticLightEntities)."""

    @staticmethod
    def parse_entities(entity_text: str):
        """Parses entity lump text and returns list of extracted light dictionaries."""
        lights = []
        if not entity_text:
            return lights

        # Simple token-based parser mirroring gl_rlight.c COM_ParseFile
        entity_blocks = re.findall(r"\{([^{}]+)\}", entity_text)
        for block in entity_blocks:
            props = {}
            for line in block.splitlines():
                line = line.strip()
                if not line or line.startswith("//"):
                    continue
                match = re.match(r'"([^"]+)"\s+"([^"]*)"', line)
                if match:
                    props[match.group(1).lower()] = match.group(2)

            classname = props.get("classname", "")
            if classname in ("light", "light_spot", "light_environment"):
                light_entry = {
                    "classname": classname,
                    "origin": [0.0, 0.0, 0.0],
                    "color": [1.0, 1.0, 1.0],
                    "intensity": 300.0,
                    "is_spot": classname == "light_spot",
                    "inner_cone": 0.0,
                    "outer_cone": 90.0,
                    "direction": [0.0, 0.0, -1.0],
                    "pitch": -90.0,
                    "angle": 0.0,
                    "style": int(props.get("style", "0")),
                }

                # Parse origin
                if "origin" in props:
                    try:
                        coords = [float(x) for x in props["origin"].split()]
                        if len(coords) >= 3:
                            light_entry["origin"] = coords[:3]
                    except ValueError:
                        pass

                # Parse _light color & intensity
                if "_light" in props:
                    try:
                        parts = [float(x) for x in props["_light"].split()]
                        if len(parts) == 1:
                            val = parts[0] / 255.0
                            light_entry["color"] = [val, val, val]
                        elif len(parts) >= 3:
                            light_entry["color"] = [parts[0] / 255.0, parts[1] / 255.0, parts[2] / 255.0]
                            if len(parts) >= 4:
                                light_entry["intensity"] = parts[3]
                    except ValueError:
                        pass

                # Parse spotlight cones
                if "_cone" in props:
                    try:
                        light_entry["inner_cone"] = float(props["_cone"])
                    except ValueError:
                        pass
                if "_cone2" in props:
                    try:
                        light_entry["outer_cone"] = float(props["_cone2"])
                    except ValueError:
                        pass

                # Clamping rules from gl_rlight.c
                if light_entry["inner_cone"] >= light_entry["outer_cone"]:
                    light_entry["inner_cone"] = max(0.0, light_entry["outer_cone"] - 5.0)

                # Cull degenerate lights
                if light_entry["intensity"] <= 0.0 or sum(light_entry["color"]) <= 0.0:
                    continue

                lights.append(light_entry)

        return lights

class PBRSuffixScanner:
    """Reference scanner for PBR material textures according to xash-rt / OpenRemix conventions."""

    @staticmethod
    def detect_suffixes(base_path: str, available_files: list[str]):
        """Given a base texture name, returns detected PBR companion maps."""
        base_clean = Path(base_path).stem.lower()
        # Strip existing suffix if provided
        for s in ("_n", "_r", "_m", "_e", "_normal", "_rough", "_metal", "_emit"):
            if base_clean.endswith(s):
                base_clean = base_clean[:-len(s)]
                break

        res = {
            "base": base_path,
            "normal": None,
            "roughness": None,
            "metallic": None,
            "emission": None,
        }

        available_lower = {Path(f).stem.lower(): f for f in available_files}
        norm_candidates = [f"{base_clean}_n", f"{base_clean}_normal"]
        rough_candidates = [f"{base_clean}_r", f"{base_clean}_rough"]
        metal_candidates = [f"{base_clean}_m", f"{base_clean}_metal"]
        emit_candidates = [f"{base_clean}_e", f"{base_clean}_emit"]

        for nc in norm_candidates:
            if nc in available_lower:
                res["normal"] = available_lower[nc]
                break

        for rc in rough_candidates:
            if rc in available_lower:
                res["roughness"] = available_lower[rc]
                break

        for mc in metal_candidates:
            if mc in available_lower:
                res["metallic"] = available_lower[mc]
                break

        for ec in emit_candidates:
            if ec in available_lower:
                res["emission"] = available_lower[ec]
                break

        return res

class CoordinateTransformValidator:
    """Validates GoldSrc to Remix coordinate system transformations."""

    SCALE_FACTOR = 0.025  # meters per unit

    @staticmethod
    def goldsrc_to_remix_pos(goldsrc_pos: tuple[float, float, float]) -> tuple[float, float, float]:
        """GoldSrc (X right, Y forward, Z up) scaled to meters."""
        x, y, z = goldsrc_pos
        return (x * CoordinateTransformValidator.SCALE_FACTOR,
                y * CoordinateTransformValidator.SCALE_FACTOR,
                z * CoordinateTransformValidator.SCALE_FACTOR)

    @staticmethod
    def goldsrc_angles_to_vectors(pitch: float, yaw: float, roll: float):
        """Converts Quake Euler angles (degrees) to forward, right, up unit vectors."""
        p_rad = math.radians(pitch)
        y_rad = math.radians(yaw)
        r_rad = math.radians(roll)

        sp, cp = math.sin(p_rad), math.cos(p_rad)
        sy, cy = math.sin(y_rad), math.cos(y_rad)
        sr, cr = math.sin(r_rad), math.cos(r_rad)

        # Quake convention: pitch is inverted (positive is down)
        forward = (cp * cy, cp * sy, -sp)
        right = (-1 * sr * sp * cy + -1 * cr * -sy,
                 -1 * sr * sp * sy + -1 * cr * cy,
                 -1 * sr * cp)
        up = (cr * sp * cy + -sr * -sy,
              cr * sp * sy + -sr * cy,
              cr * cp)
        return forward, right, up
