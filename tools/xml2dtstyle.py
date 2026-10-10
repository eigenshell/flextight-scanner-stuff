"""FlexColor film-profile XML (Apple plist) → darktable .dtstyle translator.

FlexColor ships one ImageSetting plist per film stock (extracted from the
Windows installer CAB as ``*.xml_2``). Each profile layers per-film
corrections — a master gradation curve, unsharp-mask settings, saturation,
gamma, and per-channel highlight/shadow endpoints — over the "Flextight
Input" ICC. This tool translates those corrections into a darktable style
(.dtstyle) that the open-source pipeline can apply to a scan, reproducing
the FlexColor film look.

.darktable style format authority: darktable 5.6.2's own source, branch
darktable-5.6.x (commit 20891f6), https://github.com/darktable-org/darktable:
  - src/common/styles.c            — .dtstyle XML layout (dt_styles_save_to_file)
  - src/common/database.c          — data.styles / data.style_items schema
  - src/develop/blend.c, blend.h   — dt_develop_blend_params_t layout, version
Every op_params blob below is a raw little-endian C struct per the module's
dt_iop_*_params_t definition; the struct field order is quoted in the
encoder docstrings. Do not reorder fields without re-reading the source.

Module set chosen for darktable 5.6.2 (all are in the default iop order, so
a style without an iop_list slots correctly; emission order mirrors that
order — profile_gamma 25.0, colorbalancergb 33.5, rgblevels 50.2,
rgbcurve 50.5, sharpen 53.0 per src/common/iop_order.c — but the XML order
is cosmetic since darktable re-resolves iop_order at apply time):
  profile_gamma  (v2)  FlexColor Gamma          → gamma encode, exponent 1/gamma
  colorbalancergb(v5)  Saturation               → global saturation slider
  rgblevels      (v1)  Highlight/Shadow codes   → per-channel black/white points
  rgbcurve       (v1)  Gradations[0] master     → linked-channel tone curve
  sharpen        (v1)  USM (negative amounts)   → unsharp mask

Pinned ASSUMPTIONS (evidence in docs/recon/film-profiles.md; each is a
one-line change here if disproven by hardware ground truth):
  - Gradations channel 0 is the master (RGB-linked) curve. FlexColor's
    Gradations pop-up lists "RGB, Red, Green, Blue, Cyan, Magenta, Yellow"
    (FlexColor 4 manual); all 98 shipped profiles use a non-identity curve
    in channel 0 ONLY, including B&W films (FilmType 2), where per-colour
    curves would be meaningless.
  - Gray/Highlight/Shadow 4-element arrays are [master, R, G, B]; index 0 is
    the shared/default value (Gray[0] == 128 in all shipped profiles) and
    the per-channel endpoints are indices 1..3.
  - Scanner codes normalize by 16383 (max 14-bit code), curve points by 255.
  - FlexColor USM Amount: 0 applies ~120 units of built-in sharpening,
    -120 applies none, typical maximum 250 (The Digital Coach FlexColor
    sharpening tutorial). Mapped linearly onto darktable sharpen amount
    (0..2). USMRadius 10 (constant in all shipped profiles) maps to the
    darktable default radius 2.0 (scale 0.2). USMDarkLimit has no
    darktable equivalent and is dropped; USMNoiseLimit maps to threshold.
  - FlexColor Saturation slider (-100..100) maps to colorbalancergb
    saturation_global (-1..1) by dividing by 100.
  - Pipeline order assumption: FlexColor applies gamma encoding before the
    gradation curve (the curve points live in gamma-encoded space), matching
    darktable's iop order (profile_gamma precedes rgbcurve).

Not translated (identity or unrepresentable in all 98 shipped profiles):
ColorCorr (36 ints, all zero), Contrast/Brightness/Lightness (all zero),
ApplyDust/DustLevel (dust removal is a scanner hardware pass), DotColor,
EndPoints, AutoHighlight/AutoShadow, USMColFactor (all [100,100,100]).
"""

from __future__ import annotations

import argparse
import base64
import plistlib
import struct
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO, Union

# ---------------------------------------------------------------------------
# Pinned wire constants. Sources: darktable-5.6.x @ 20891f6 (see module
# docstring). These are literals on purpose — tests pin the same values.
# ---------------------------------------------------------------------------

# .dtstyle container (src/common/styles.c: dt_styles_save_to_file).
STYLE_XML_VERSION = "1.0"

# Blending (src/develop/blend.h). DEVELOP_BLEND_VERSION for darktable 5.6.2.
BLENDOP_VERSION = 14
DEVELOP_MASK_DISABLED = 0  # mask_mode: no mask
DEVELOP_BLEND_CS_NONE = 0  # blend_cst: module default
DEVELOP_BLEND_NORMAL2 = 0x18  # blend_mode: normal (non-clamping)
DEVELOP_COMBINE_NORM_EXCL = 0x00  # mask_combine
DEVELOP_MASK_GUIDE_IN_AFTER_BLUR = 0x05
INVALID_MASKID = -1
_DEVELOP_BLENDIF_SIZE = 16  # DEVELOP_BLENDIF_SIZE (src/develop/blend.h:171)

# Scanner/code scales.
SCANCODE_MAX = 16383.0  # max 14-bit scanner code (Highlight/Shadow/Gray)
CURVE_MAX = 255.0  # FlexColor gradation curve point scale

# FlexColor → darktable USM mapping (assumptions, see module docstring).
USM_BUILTIN_BASELINE = 120.0  # sharpening applied at FlexColor Amount == 0
USM_FULL_SCALE = 250.0  # typical FlexColor Amount maximum
DT_SHARPEN_AMOUNT_MAX = 2.0  # dt_iop_sharpen_params_t.amount $MAX (sharpen.c)
USM_RADIUS_SCALE = 0.2  # FlexColor USMRadius 10 → darktable radius 2.0

# Gradations channel semantics (ASSUMPTION — see module docstring).
GRADATION_CHANNELS = ("master", "red", "green", "blue", "cyan", "magenta", "yellow")
GRADATION_MASTER = 0

# darktable module versions (DT_MODULE_INTROSPECTION in each module's source).
MOD_VERSION = {
    "profile_gamma": 2,  # src/iop/profile_gamma.c:39
    "rgblevels": 1,  # src/iop/rgblevels.c:32
    "rgbcurve": 1,  # src/iop/rgbcurve.c:40
    "colorbalancergb": 5,  # src/iop/colorbalancergb.c:52
    "sharpen": 1,  # src/iop/sharpen.c:39
}

# src/iop/rgbcurve.c
RGBCURVE_MAX_CHANNELS = 3
RGBCURVE_MAXNODES = 20  # DT_IOP_RGBCURVE_MAXNODES == MAX_ANCHORS (curve_tools.h:33)
RGBCURVE_MONOTONE_HERMITE = 2  # curve interpolation type (curve_tools.h:29)
DT_S_SCALE_AUTOMATIC_RGB = 0  # linked channels
DT_RGB_NORM_LUMINANCE = 1  # preserve_colors (src/common/rgb_norms.h)

# src/iop/rgblevels.c
DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS = 1

# src/iop/profile_gamma.c
PROFILEGAMMA_GAMMA = 1

# src/iop/colorbalancergb.c: field order of dt_iop_colorbalancergb_params_t
# (32 floats, DT_MODULE_INTROSPECTION(5)) PLUS the trailing v5 enum
# dt_iop_colorbalancrgb_saturation_t saturation_formula (4 bytes,
# $DEFAULT 1 = DT_COLORBALANCE_SATURATION_DTUCS). The blob is 132 bytes;
# omitting the enum makes darktable reject the blob for params_size
# mismatch and silently run module defaults (styles.c apply path).
_COLORBALANCERGB_FIELDS = (
    "shadows_Y shadows_C shadows_H midtones_Y midtones_C midtones_H "
    "highlights_Y highlights_C highlights_H global_Y global_C global_H "
    "shadows_weight white_fulcrum highlights_weight "
    "chroma_shadows chroma_highlights chroma_global chroma_midtones "
    "saturation_global saturation_highlights saturation_midtones saturation_shadows "
    "hue_angle brilliance_global brilliance_highlights brilliance_midtones "
    "brilliance_shadows mask_grey_fulcrum vibrance grey_fulcrum contrast"
).split()
DT_COLORBALANCE_SATURATION_DTUCS = 1  # saturation_formula default (v5 tail)
# $DEFAULT values for the non-zero fields; all others default to 0.0.
_COLORBALANCERGB_DEFAULTS = {
    "shadows_weight": 1.0,
    "white_fulcrum": 0.0,
    "highlights_weight": 1.0,
    "mask_grey_fulcrum": 0.1845,
    "grey_fulcrum": 0.1845,
}


class ProfileError(Exception):
    """Raised on a malformed film-profile plist; message names file and key."""


# ---------------------------------------------------------------------------
# Parser: plist → FilmProfile
# ---------------------------------------------------------------------------


@dataclass
class CurvePoint:
    """One gradation-curve control point (raw 0..255 codes)."""

    x: int
    y: int
    dy: int


@dataclass
class FilmProfile:
    """The subset of ImageSetting.ImageCorrection that carries the film look."""

    name: str
    film_type: int
    apply_cc: bool
    apply_curves: bool
    apply_usm: bool
    apply_sliders: bool
    gradations: list[list[CurvePoint]] = field(default_factory=list)
    usm_amount: int = 0
    usm_radius: int = 0
    usm_dark_limit: int = 0
    usm_noise_limit: int = 0
    usm_col_factor: list[int] = field(default_factory=list)
    gamma: float = 2.0
    saturation: int = 0
    contrast: int = 0
    brightness: int = 0
    lightness: int = 0
    gray: list[int] = field(default_factory=list)  # [master, R, G, B], 0..16383
    highlight: list[int] = field(default_factory=list)
    shadow: list[int] = field(default_factory=list)
    color_corr: list[int] = field(default_factory=list)


def _as_int(ic: dict, key: str, source: str) -> int:
    """Strict int coercion: bools are rejected, not silently truncated."""
    v = ic.get(key)
    if isinstance(v, bool) or not isinstance(v, int):
        raise ProfileError(
            f"{source}: ImageCorrection.{key} must be an int, "
            f"got {type(v).__name__}: {v!r}"
        )
    return v


def _as_bool(ic: dict, key: str, source: str) -> bool:
    v = ic.get(key)
    if not isinstance(v, bool):
        raise ProfileError(
            f"{source}: ImageCorrection.{key} must be a bool, "
            f"got {type(v).__name__}: {v!r}"
        )
    return v


def _as_int_list(ic: dict, key: str, source: str, length: int | None = None) -> list[int]:
    v = ic.get(key)
    if not isinstance(v, list) or any(isinstance(x, bool) or not isinstance(x, int) for x in v):
        raise ProfileError(
            f"{source}: ImageCorrection.{key} must be a list of ints, got {v!r}"
        )
    if length is not None and len(v) != length:
        raise ProfileError(
            f"{source}: ImageCorrection.{key} must have {length} entries, got {len(v)}"
        )
    return list(v)


def parse_profile(path_or_buffer: Union[str, Path, BinaryIO]) -> FilmProfile:
    """Parse one FlexColor film-profile plist into a FilmProfile.

    *path_or_buffer* is a filesystem path or a binary file object. Raises
    ProfileError with the file name and offending key on any deviation from
    the ImageSetting/ImageCorrection contract.
    """
    if isinstance(path_or_buffer, (str, Path)):
        path: Path | None = Path(path_or_buffer)
        source = str(path)
    else:
        path = None
        source = str(getattr(path_or_buffer, "name", "<buffer>"))
    try:
        if path is not None:
            with open(path, "rb") as fh:
                plist = plistlib.load(fh)
        else:
            plist = plistlib.load(path_or_buffer)  # type: ignore[arg-type]
    except (OSError, plistlib.InvalidFileException) as e:
        raise ProfileError(f"{source}: not a readable Apple plist: {e}") from e
    return _profile_from_plist(plist, source)


def _profile_from_plist(plist: object, source: str) -> FilmProfile:
    """Validate an already-loaded plist dict and build a FilmProfile.

    Raises ProfileError with *source* (file name) and the offending key on
    any deviation from the ImageSetting/ImageCorrection contract.
    """
    if not isinstance(plist, dict):
        raise ProfileError(f"{source}: top-level plist value must be a dict")
    setting = plist.get("ImageSetting")
    if not isinstance(setting, dict):
        raise ProfileError(
            f"{source}: not a film profile (missing ImageSetting dict; "
            "only ImageSetting plists are translatable)"
        )
    ic = setting.get("ImageCorrection")
    if not isinstance(ic, dict):
        raise ProfileError(f"{source}: ImageSetting missing ImageCorrection dict")

    gradations_raw = ic.get("Gradations")
    if (
        not isinstance(gradations_raw, list)
        or len(gradations_raw) != len(GRADATION_CHANNELS)
        or any(not isinstance(ch, dict) for ch in gradations_raw)
    ):
        raise ProfileError(
            f"{source}: ImageCorrection.Gradations must be a list of "
            f"{len(GRADATION_CHANNELS)} channel dicts, got {gradations_raw!r}"
        )
    gradations: list[list[CurvePoint]] = []
    for ch_index, ch in enumerate(gradations_raw):
        points_raw = ch.get("Points")
        if (
            not isinstance(points_raw, list)
            or not points_raw
            or any(not isinstance(p, dict) for p in points_raw)
        ):
            raise ProfileError(
                f"{source}: Gradations[{ch_index}].Points must be a non-empty "
                f"list of dicts, got {points_raw!r}"
            )
        points: list[CurvePoint] = []
        for p in points_raw:
            try:
                x, y, dy = (_as_int(p, k, f"{source} Gradations[{ch_index}]") for k in ("X", "Y", "DY"))
            except KeyError as e:
                raise ProfileError(
                    f"{source}: Gradations[{ch_index}] point missing key {e}"
                ) from e
            for label, v in (("X", x), ("Y", y)):
                if not 0 <= v <= 255:
                    raise ProfileError(
                        f"{source}: Gradations[{ch_index}] point {label}={v} "
                        "outside 0..255"
                    )
            points.append(CurvePoint(x=x, y=y, dy=dy))
        if points[0].x != 0 or points[-1].x != 255:
            raise ProfileError(
                f"{source}: Gradations[{ch_index}] curve must span X=0..255, "
                f"got {points[0].x}..{points[-1].x}"
            )
        gradations.append(points)

    color_corr = _as_int_list(ic, "ColorCorr", source)
    if any(v != 0 for v in color_corr):
        raise ProfileError(
            f"{source}: non-identity ColorCorr is not translatable "
            f"(expected all-zero, got {color_corr})"
        )

    gamma = ic.get("Gamma")
    if isinstance(gamma, bool) or not isinstance(gamma, (int, float)):
        raise ProfileError(f"{source}: ImageCorrection.Gamma must be a number")
    if gamma <= 0:
        raise ProfileError(f"{source}: ImageCorrection.Gamma must be > 0, got {gamma}")

    # These sliders are zero/identity in every shipped profile and have no
    # chosen-module mapping; like ColorCorr, a non-identity value is rejected
    # rather than silently dropped.
    contrast = _as_int(ic, "Contrast", source)
    brightness = _as_int(ic, "Brightness", source)
    lightness = _as_int(ic, "Lightness", source)
    for key, value in (
        ("Contrast", contrast),
        ("Brightness", brightness),
        ("Lightness", lightness),
    ):
        if value != 0:
            raise ProfileError(
                f"{source}: non-zero {key} is not translatable "
                f"(expected 0 in all shipped profiles, got {value})"
            )
    usm_col_factor = _as_int_list(ic, "USMColFactor", source)
    if usm_col_factor != [100, 100, 100]:
        raise ProfileError(
            f"{source}: USMColFactor other than [100, 100, 100] is not "
            f"translatable (no per-channel USM weighting), got {usm_col_factor}"
        )

    return FilmProfile(
        name=Path(source).stem,
        film_type=_as_int(ic, "FilmType", source),
        apply_cc=_as_bool(ic, "ApplyCC", source),
        apply_curves=_as_bool(ic, "ApplyCurves", source),
        apply_usm=_as_bool(ic, "ApplyUSM", source),
        apply_sliders=_as_bool(ic, "ApplySliders", source),
        gradations=gradations,
        usm_amount=_as_int(ic, "USMAmount", source),
        usm_radius=_as_int(ic, "USMRadius", source),
        usm_dark_limit=_as_int(ic, "USMDarkLimit", source),
        usm_noise_limit=_as_int(ic, "USMNoiseLimit", source),
        usm_col_factor=usm_col_factor,
        gamma=float(gamma),
        saturation=_as_int(ic, "Saturation", source),
        contrast=contrast,
        brightness=brightness,
        lightness=lightness,
        gray=_as_int_list(ic, "Gray", source, length=4),
        highlight=_as_int_list(ic, "Highlight", source, length=4),
        shadow=_as_int_list(ic, "Shadow", source, length=4),
        color_corr=color_corr,
    )


# ---------------------------------------------------------------------------
# Translator: FilmProfile → darktable module params (raw C structs)
# ---------------------------------------------------------------------------


@dataclass
class StylePlugin:
    """One <plugin> entry of a .dtstyle: a module instance with its params."""

    num: int
    operation: str
    module_version: int
    op_params: bytes
    enabled: int = 1
    blendop_params: bytes = b""
    blendop_version: int = BLENDOP_VERSION
    multi_priority: int = 0
    multi_name: str = ""
    multi_name_hand_edited: int = 0


def default_blendop_params() -> bytes:
    """dt_develop_blend_params_t (blend.h) with default_blendop_params values
    (src/develop/blend.c:41). 420 bytes, little-endian."""
    blendif_parameters = []
    for _ in range(_DEVELOP_BLENDIF_SIZE):
        blendif_parameters += [0.0, 0.0, 1.0, 1.0]
    blob = struct.pack(
        "<3I f f I i I f I f f f f I 2I 64f 16f 20s i i i",
        DEVELOP_MASK_DISABLED,  # mask_mode
        DEVELOP_BLEND_CS_NONE,  # blend_cst
        DEVELOP_BLEND_NORMAL2,  # blend_mode
        0.0,  # blend_parameter
        100.0,  # opacity (percent)
        DEVELOP_COMBINE_NORM_EXCL,  # mask_combine
        0,  # mask_id
        0,  # blendif
        0.0,  # feathering_radius
        DEVELOP_MASK_GUIDE_IN_AFTER_BLUR,  # feathering_guide
        0.0,  # blur_radius
        0.0,  # contrast
        0.0,  # brightness
        0.0,  # details
        1,  # feather_version
        0, 0,  # reserved
        *blendif_parameters,
        *([0.0] * _DEVELOP_BLENDIF_SIZE),  # blendif_boost_factors
        b"\x00" * 20,  # raster_mask_source (dt_dev_operation_t[20])
        0,  # raster_mask_instance
        INVALID_MASKID,  # raster_mask_id
        0,  # raster_mask_invert
    )
    assert len(blob) == 420, len(blob)
    return blob


def encode_profilegamma(gamma: float) -> bytes:
    """dt_iop_profilegamma_params_t (src/iop/profile_gamma.c, v2, 28 bytes).

    GAMMA mode applies out = in**gamma_param, so FlexColor's display gamma
    maps to exponent 1/gamma. The log-mode fields are inert in GAMMA mode
    but are part of the struct; defaults pinned from the source $DEFAULTs.
    """
    return struct.pack(
        "<i 6f",
        PROFILEGAMMA_GAMMA,  # mode
        0.0,  # linear (no linear toe)
        1.0 / gamma,  # gamma (exponent)
        10.0,  # dynamic_range (log-mode only)
        18.0,  # grey_point (log-mode only)
        -5.0,  # shadows_range (log-mode only)
        0.0,  # security_factor (log-mode only)
    )


def encode_rgblevels(highlight: list[int], shadow: list[int]) -> bytes:
    """dt_iop_rgblevels_params_t (src/iop/rgblevels.c, v1, 44 bytes).

    Independent channels; levels[ch] = [black, middle, white] as 0..1
    floats. Scanner codes normalize by SCANCODE_MAX. Middle is set to the
    black/white midpoint so _compute_lut yields inv_gamma == 1 (no gamma
    pivot); FlexColor's Gray balance is not provably this pivot, so the
    neutral choice is made (ASSUMPTION, see module docstring).
    """
    levels: list[float] = []
    for ch in range(3):
        black = shadow[ch + 1] / SCANCODE_MAX
        white = highlight[ch + 1] / SCANCODE_MAX
        levels += [black, (black + white) / 2.0, white]
    return struct.pack(
        "<2i 9f",
        DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS,  # autoscale
        DT_RGB_NORM_LUMINANCE,  # preserve_colors
        *levels,
    )


def encode_rgbcurve(points: list[CurvePoint]) -> bytes:
    """dt_iop_rgbcurve_params_t (src/iop/rgbcurve.c, v1, 516 bytes).

    Linked-channels mode (AUTOMATIC_RGB): the R curve drives all channels,
    so the master gradation curve lands in curve_nodes[0] and G/B carry a
    2-point identity. Nodes are x/255, y/255; interpolation MONOTONE_HERMITE.
    """
    if len(points) > RGBCURVE_MAXNODES:
        raise ProfileError(
            f"gradation curve has {len(points)} points, "
            f"darktable rgbcurve supports at most {RGBCURVE_MAXNODES}"
        )
    nodes = [0.0] * (RGBCURVE_MAX_CHANNELS * RGBCURVE_MAXNODES * 2)
    for i, p in enumerate(points):
        nodes[2 * i] = p.x / CURVE_MAX
        nodes[2 * i + 1] = p.y / CURVE_MAX
    num_nodes = [len(points), 2, 2]
    # identity fallback nodes for G/B at slots [2],[3] = (0,0),(1,1)
    nodes[RGBCURVE_MAXNODES * 2 * 1 + 0] = 0.0
    nodes[RGBCURVE_MAXNODES * 2 * 1 + 1] = 0.0
    nodes[RGBCURVE_MAXNODES * 2 * 1 + 2] = 1.0
    nodes[RGBCURVE_MAXNODES * 2 * 1 + 3] = 1.0
    nodes[RGBCURVE_MAXNODES * 2 * 2 + 0] = 0.0
    nodes[RGBCURVE_MAXNODES * 2 * 2 + 1] = 0.0
    nodes[RGBCURVE_MAXNODES * 2 * 2 + 2] = 1.0
    nodes[RGBCURVE_MAXNODES * 2 * 2 + 3] = 1.0
    return struct.pack(
        "<120f 3i 3i i i i",
        *nodes,
        *num_nodes,
        *(RGBCURVE_MONOTONE_HERMITE,) * 3,  # curve_type
        DT_S_SCALE_AUTOMATIC_RGB,  # curve_autoscale
        0,  # compensate_middle_grey (gboolean)
        DT_RGB_NORM_LUMINANCE,  # preserve_colors
    )


def encode_colorbalancergb(saturation: int) -> bytes:
    """dt_iop_colorbalancergb_params_t (src/iop/colorbalancergb.c, v5, 132 bytes).

    32 floats in the declaration order pinned in _COLORBALANCERGB_FIELDS,
    followed by the v5 saturation_formula enum (4 bytes, default DTUCS);
    only saturation_global is driven (FlexColor Saturation / 100).
    """
    values = {
        name: _COLORBALANCERGB_DEFAULTS.get(name, 0.0)
        for name in _COLORBALANCERGB_FIELDS
    }
    values["saturation_global"] = saturation / 100.0
    return struct.pack(
        "<32f i",
        *(values[name] for name in _COLORBALANCERGB_FIELDS),
        DT_COLORBALANCE_SATURATION_DTUCS,
    )


def encode_sharpen(amount: int, radius: int, noise_limit: int) -> bytes:
    """dt_iop_sharpen_params_t (src/iop/sharpen.c, v1, 12 bytes).

    FlexColor USM Amount maps linearly from [-baseline, +full_scale] onto
    darktable amount [0, 2] (ASSUMPTION — see module docstring); the amount
    is clamped because FlexColor's documented range exceeds the mapping.
    """
    dt_amount = (amount + USM_BUILTIN_BASELINE) / USM_FULL_SCALE * DT_SHARPEN_AMOUNT_MAX
    dt_amount = min(max(dt_amount, 0.0), DT_SHARPEN_AMOUNT_MAX)
    return struct.pack(
        "<3f",
        radius * USM_RADIUS_SCALE,
        dt_amount,
        float(noise_limit),
    )


def build_plugins(profile: FilmProfile) -> list[StylePlugin]:
    """Translate a profile into the ordered list of style plugin entries.

    Emission order mirrors darktable's default iop order (src/common/
    iop_order.c @ darktable-5.6.x): profile_gamma 25.0, colorbalancergb
    33.5, rgblevels 50.2, rgbcurve 50.5, sharpen 53.0. This is cosmetic —
    darktable re-resolves each item's iop_order from the pipeline when a
    style is applied (dt_ioppr_update_for_style_items); the load-bearing
    ordering fact is only that profile_gamma precedes the curves. The
    FlexColor apply-flags gate the corresponding modules; all shipped
    profiles set them true.
    """
    plugins: list[StylePlugin] = []
    blend = default_blendop_params()

    def add(operation: str, op_params: bytes, enabled: bool = True) -> None:
        plugins.append(
            StylePlugin(
                num=len(plugins),
                operation=operation,
                module_version=MOD_VERSION[operation],
                op_params=op_params,
                enabled=1 if enabled else 0,
                blendop_params=blend,
            )
        )

    add("profile_gamma", encode_profilegamma(profile.gamma))
    add("colorbalancergb", encode_colorbalancergb(profile.saturation))
    add(
        "rgblevels",
        encode_rgblevels(profile.highlight, profile.shadow),
        enabled=profile.apply_sliders,
    )
    add(
        "rgbcurve",
        encode_rgbcurve(profile.gradations[GRADATION_MASTER]),
        enabled=profile.apply_curves,
    )
    add(
        "sharpen",
        encode_sharpen(profile.usm_amount, profile.usm_radius, profile.usm_noise_limit),
        enabled=profile.apply_usm,
    )
    return plugins


def style_xml(name: str, description: str, plugins: list[StylePlugin]) -> bytes:
    """Serialize plugins to a .dtstyle document (layout per src/common/styles.c:
    dt_styles_save_to_file)."""
    root = ET.Element("darktable_style", {"version": STYLE_XML_VERSION})
    info = ET.SubElement(root, "info")
    ET.SubElement(info, "name").text = name
    ET.SubElement(info, "description").text = description
    style = ET.SubElement(root, "style")
    for p in plugins:
        plugin = ET.SubElement(style, "plugin")
        for tag, text in (
            ("num", str(p.num)),
            ("module", str(p.module_version)),
            ("operation", p.operation),
            ("op_params", base64.b64encode(p.op_params).decode("ascii")),
            ("enabled", str(p.enabled)),
            (
                "blendop_params",
                base64.b64encode(p.blendop_params).decode("ascii"),
            ),
            ("blendop_version", str(p.blendop_version)),
            ("multi_priority", str(p.multi_priority)),
            ("multi_name", p.multi_name),
            ("multi_name_hand_edited", str(p.multi_name_hand_edited)),
        ):
            ET.SubElement(plugin, tag).text = text
    return b'<?xml version="1.0" encoding="UTF-8"?>\n' + ET.tostring(root)


def profile_to_dtstyle(
    profile: FilmProfile, name: str | None = None
) -> tuple[bytes, list[StylePlugin]]:
    """Translate a parsed profile to (dtstyle_bytes, plugins)."""
    style_name = name or profile.name
    description = (
        f"FlexColor film profile {profile.name} "
        f"(translated by flextight tools/xml2dtstyle.py; "
        f"assumptions: docs/recon/film-profiles.md)"
    )
    plugins = build_plugins(profile)
    return style_xml(style_name, description, plugins), plugins


# ---------------------------------------------------------------------------
# Batch mode
# ---------------------------------------------------------------------------


def convert_directory(src_dir: Union[str, Path], out_dir: Union[str, Path]) -> tuple[int, int]:
    """Convert every film profile in *src_dir* to *out_dir*/*.dtstyle.

    Files that are not ImageSetting plists are skipped. Returns
    (written, skipped). Raises ProfileError on the first malformed
    ImageSetting plist (a bad file must not silently produce a bad style).
    """
    src, out = Path(src_dir), Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    written = skipped = 0
    # Film profiles ship as "*.xml_2"; the CAB also holds "*.xml" device
    # settings and other suffix generations that are not film profiles.
    for path in sorted(src.glob("*.xml_2")):
        try:
            with open(path, "rb") as fh:
                plist = plistlib.load(fh)
        except (OSError, plistlib.InvalidFileException):
            skipped += 1  # not an Apple plist at all (e.g. device binaries)
            continue
        if not (isinstance(plist, dict) and isinstance(plist.get("ImageSetting"), dict)):
            skipped += 1  # an Apple plist, but not a film profile
            continue
        profile = _profile_from_plist(plist, str(path))
        dtstyle, _ = profile_to_dtstyle(profile)
        (out / f"{path.stem}.dtstyle").write_bytes(dtstyle)
        written += 1
    return written, skipped


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src", help="directory with FlexColor *.xml_2 profiles")
    ap.add_argument("out", help="output directory for .dtstyle files")
    args = ap.parse_args(argv)
    written, skipped = convert_directory(args.src, args.out)
    print(f"converted {written} film profiles to {args.out} (skipped {skipped} non-profiles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
