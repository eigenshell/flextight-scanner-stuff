"""Tests for tools/xml2dtstyle — FlexColor film-profile → darktable style.

Wire-format constants (op_params struct fields, blend blob layout, module
versions) are pinned here as literals decoded with struct.unpack, per
project rule: tests must not round-trip through the translator's own
encoder. Format authority: darktable 5.6.2 source, branch darktable-5.6.x
(commit 20891f6) — see docs/recon/film-profiles.md for the field mapping.

Fixture facts (tests/fixtures/*.xml are SYNTHETIC: the FlexColor plist
key structure with invented values — no Hasselblad profile data is
redistributed; tests needing real profiles use a local extracted CAB and
skip without it):
  Synthetic_Color_Negative: identity master curve, USM 80/10/10/0, Sat 20,
      Gamma 2.2, Highlight [15000,11500,14000,15100], Shadow [300,350,4000,7000]
  Synthetic_BW_Negative: master curve (0,0)(100,40)(190,180)(255,255),
      USM -40/10/10/0, Sat 15, Highlight [14500,14500,14300,14200],
      Shadow [400,400,400,400]
"""

import base64
import io
import plistlib
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from flextight.fff import write_fff
from tools.xml2dtstyle import (
    ProfileError,
    build_plugins,
    convert_directory,
    default_blendop_params,
    parse_profile,
    profile_to_dtstyle,
)

FIXTURES = Path(__file__).parent / "fixtures"
CAB_DIR = (
    Path(__file__).parent.parent / "extracted" / "fc4891win" / "cab"
)
DT_CLI = shutil.which("darktable-cli")

COLOR_NEG = FIXTURES / "Synthetic_Color_Negative.xml"
BW_NEG = FIXTURES / "Synthetic_BW_Negative.xml"


def _plugins_by_op(plugins):
    return {p.operation: p for p in plugins}


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


def test_parse_portra_fixture():
    p = parse_profile(COLOR_NEG)
    assert p.name == "Synthetic_Color_Negative"
    assert p.film_type == 1  # color negative
    # 7 gradation channels, 2 identity points each in this profile
    assert len(p.gradations) == 7
    assert all(len(ch) == 2 for ch in p.gradations)
    assert (p.gradations[0][0].x, p.gradations[0][0].y) == (0, 0)
    # USM params present
    assert (p.usm_amount, p.usm_radius, p.usm_dark_limit, p.usm_noise_limit) == (
        80,
        10,
        10,
        0,
    )
    assert p.usm_col_factor == [100, 100, 100]
    # 14-bit endpoints (pinned literals)
    assert p.highlight == [15000, 11500, 14000, 15100]
    assert p.shadow == [300, 350, 4000, 7000]
    assert p.gray[0] == 128
    assert p.gamma == 2.2
    assert p.saturation == 20


def test_parse_hp5_non_identity_master_curve():
    p = parse_profile(BW_NEG)
    assert p.film_type == 2  # B&W: master-curve-only is decisive evidence
    master = p.gradations[0]
    assert [(pt.x, pt.y, pt.dy) for pt in master] == [
        (0, 0, 1),
        (100, 40, 1),
        (190, 180, 1),
        (255, 255, 1),
    ]
    # channels 1..6 are identity in every shipped profile
    for ch in p.gradations[1:]:
        assert [(pt.x, pt.y) for pt in ch] == [(0, 0), (255, 255)]


def test_scancode_normalization_pinned():
    """15000/16383 and 300/16383 → pinned floats (max 14-bit code divisor)."""
    p = parse_profile(COLOR_NEG)
    white = p.highlight[0] / 16383.0
    black = p.shadow[0] / 16383.0
    assert white == pytest.approx(0.9155832265152902, rel=0, abs=2e-9)  # 15000/16383
    assert black == pytest.approx(0.018311664530305805, rel=0, abs=2e-9)  # 300/16383


def test_malformed_plist_actionable_error(tmp_path):
    bad = tmp_path / "broken.xml"
    bad.write_bytes(b"this is not a plist at all")
    with pytest.raises(ProfileError, match="broken.xml: not a readable Apple plist"):
        parse_profile(bad)


def test_non_imagesetting_plist_actionable(tmp_path):
    other = tmp_path / "messages.xml"
    other.write_bytes(
        b'<?xml version="1.0" encoding="UTF-8"?>'
        b'<plist version="1.0"><dict><key>Messages</key><array/></dict></plist>'
    )
    with pytest.raises(ProfileError, match="missing ImageSetting"):
        parse_profile(other)


def test_wrong_type_actionable(tmp_path):
    plist = plistlib.load(open(COLOR_NEG, "rb"))
    plist["ImageSetting"]["ImageCorrection"]["USMAmount"] = "lots"
    bad = tmp_path / "badtypes.xml"
    with open(bad, "wb") as fh:
        plistlib.dump(plist, fh)
    with pytest.raises(ProfileError, match="USMAmount must be an int, got str"):
        parse_profile(bad)


def test_non_identity_color_corr_rejected(tmp_path):
    plist = plistlib.load(open(COLOR_NEG, "rb"))
    plist["ImageSetting"]["ImageCorrection"]["ColorCorr"] = [1] * 36
    bad = tmp_path / "badcc.xml"
    with open(bad, "wb") as fh:
        plistlib.dump(plist, fh)
    with pytest.raises(ProfileError, match="non-identity ColorCorr"):
        parse_profile(bad)


# ---------------------------------------------------------------------------
# Translator: decoded op_params blobs (pinned literals, no round-trip)
# ---------------------------------------------------------------------------


def test_profilegamma_params():
    p = parse_profile(COLOR_NEG)
    plugins = _plugins_by_op(build_plugins(p))
    blob = plugins["profile_gamma"].op_params
    assert len(blob) == 28
    mode, linear, gamma, dyn, grey, shadows, security = struct.unpack("<i6f", blob)
    assert mode == 1  # PROFILEGAMMA_GAMMA
    assert linear == 0.0
    assert gamma == 0.4545454680919647  # exponent 1/2.2 as C float
    assert (dyn, grey, shadows, security) == (10.0, 18.0, -5.0, 0.0)
    assert plugins["profile_gamma"].module_version == 2


def test_rgblevels_params():
    p = parse_profile(COLOR_NEG)
    plugins = _plugins_by_op(build_plugins(p))
    blob = plugins["rgblevels"].op_params
    assert len(blob) == 44
    autoscale, preserve, *levels = struct.unpack("<2i9f", blob)
    assert autoscale == 1  # DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS
    assert preserve == 1  # DT_RGB_NORM_LUMINANCE
    # R = channel index 1 (index 0 is master): black shadow[1] 350/16383,
    # white highlight[1] 11500/16383, midpoint between
    assert levels[0] == 0.02136360853910446  # 350/16383 as C float
    assert levels[2] == 0.7019471526145935  # 11500/16383 as C float
    assert levels[1] == 0.3616553843021393  # (350 + 11500) / 2 / 16383 as C float
    # B: shadow[3] == 7000, highlight[3] == 15100
    assert levels[6] == 0.42727217078208923  # 7000/16383 as C float
    assert levels[8] == 0.921687126159668  # 15100/16383 as C float


def test_rgbcurve_params_identity_profile():
    """An identity master curve still yields a valid linked curve."""
    p = parse_profile(COLOR_NEG)
    plugins = _plugins_by_op(build_plugins(p))
    blob = plugins["rgbcurve"].op_params
    assert len(blob) == 516
    floats = struct.unpack("<120f", blob[:480])
    num_nodes = struct.unpack("<3i", blob[480:492])
    curve_type = struct.unpack("<3i", blob[492:504])
    autoscale, compensate, preserve = struct.unpack("<3i", blob[504:516])
    assert num_nodes == (2, 2, 2)
    assert curve_type == (2, 2, 2)  # MONOTONE_HERMITE
    assert autoscale == 0  # DT_S_SCALE_AUTOMATIC_RGB (linked)
    assert compensate == 0
    assert preserve == 1  # DT_RGB_NORM_LUMINANCE
    # channel R identity nodes at floats[0:4] == (0,0),(1,1); rest zero-padded
    assert floats[0:4] == (0.0, 0.0, 1.0, 1.0)
    assert all(f == 0.0 for f in floats[4:40])  # remaining R slots unused
    # G/B identity fallback nodes at slots 40..44 and 80..84
    assert floats[40:44] == (0.0, 0.0, 1.0, 1.0)
    assert floats[80:84] == (0.0, 0.0, 1.0, 1.0)


def test_rgbcurve_params_non_identity():
    p = parse_profile(BW_NEG)
    plugins = _plugins_by_op(build_plugins(p))
    floats = struct.unpack("<120f", plugins["rgbcurve"].op_params[:480])
    # master curve (0,0)(100,40)(190,180)(255,255) normalized by 255
    assert floats[0:8] == (
        0.0,
        0.0,
        0.3921568691730499,
        0.1568627506494522,
        0.7450980544090271,
        0.7058823704719543,
        1.0,
        1.0,
    )
    num_nodes = struct.unpack("<3i", plugins["rgbcurve"].op_params[480:492])
    assert num_nodes == (4, 2, 2)


def test_colorbalancergb_params():
    p = parse_profile(COLOR_NEG)
    plugins = _plugins_by_op(build_plugins(p))
    blob = plugins["colorbalancergb"].op_params
    # 32 floats + trailing v5 saturation_formula enum = 132 bytes. A
    # 128-byte blob is silently rejected by darktable (params_size
    # mismatch) and the module runs defaults — saturation lost.
    assert len(blob) == 132
    fields = struct.unpack("<32f", blob[:128])
    saturation_formula = struct.unpack("<i", blob[128:132])[0]
    assert saturation_formula == 1  # DT_COLORBALANCE_SATURATION_DTUCS
    # saturation_global is field index 19 (0-based) in the pinned v5 order
    assert fields[19] == 0.20000000298023224  # 0.20 as C float
    # non-zero defaults: shadows_weight=1.0 (12), highlights_weight=1.0 (14),
    # mask_grey_fulcrum=0.1845 (28), grey_fulcrum=0.1845 (30)
    assert fields[12] == 1.0
    assert fields[14] == 1.0
    assert fields[28] == 0.18449999392032623  # 0.1845 as C float
    assert fields[30] == 0.18449999392032623  # 0.1845 as C float
    assert all(v == 0.0 for i, v in enumerate(fields) if i not in (12, 14, 19, 28, 30))


def test_sharpen_params():
    """USM amount mapping (pinned): 80 → 1.6, -40 → 0.64; radius 10 → 2.0."""
    portra = _plugins_by_op(build_plugins(parse_profile(COLOR_NEG)))
    assert struct.unpack("<3f", portra["sharpen"].op_params) == (2.0, 1.600000023841858, 0.0)
    hp5 = _plugins_by_op(build_plugins(parse_profile(BW_NEG)))
    radius, amount, threshold = struct.unpack("<3f", hp5["sharpen"].op_params)
    assert radius == 2.0
    assert amount == 0.6399999856948853  # 0.64 as C float
    assert threshold == 0.0


def test_default_blendop_blob():
    blob = default_blendop_params()
    assert len(blob) == 420
    # mask_mode, blend_cst, blend_mode, blend_parameter, opacity
    mask_mode, blend_cst, blend_mode = struct.unpack_from("<3I", blob, 0)
    opacity = struct.unpack_from("<f", blob, 16)[0]
    assert (mask_mode, blend_cst, blend_mode) == (0, 0, 0x18)
    assert opacity == 100.0
    # raster_mask_id == INVALID_MASKID near the tail
    raster_mask_id = struct.unpack_from("<i", blob, 412)[0]
    assert raster_mask_id == -1


def test_dtstyle_xml_structure():
    p = parse_profile(BW_NEG)
    xml_bytes, plugins = profile_to_dtstyle(p)
    root = ET.fromstring(xml_bytes)
    assert root.tag == "darktable_style"
    assert root.get("version") == "1.0"
    info = root.find("info")
    assert info.find("name").text == "Synthetic_BW_Negative"
    ops = [el.find("operation").text for el in root.find("style")]
    # Emission mirrors the default iop order (src/common/iop_order.c @
    # darktable-5.6.x: profile_gamma 25.0, colorbalancergb 33.5,
    # rgblevels 50.2, rgbcurve 50.5, sharpen 53.0); darktable re-resolves
    # iop_order at apply time, so this order is cosmetic.
    assert ops == [
        "profile_gamma",
        "colorbalancergb",
        "rgblevels",
        "rgbcurve",
        "sharpen",
    ]
    # base64 in the XML decodes to the same bytes the encoder produced —
    # checked against a pinned literal, not a re-encode: sharpen amount 0.64
    for el in root.find("style"):
        if el.find("operation").text == "sharpen":
            blob = base64.b64decode(el.find("op_params").text)
            assert struct.unpack("<3f", blob)[1] == 0.6399999856948853
            assert int(el.find("blendop_version").text) == 14  # DEVELOP_BLEND_VERSION @5.6.2
            blend = base64.b64decode(el.find("blendop_params").text)
            assert len(blend) == 420


# ---------------------------------------------------------------------------
# Batch mode
# ---------------------------------------------------------------------------


def test_batch_skips_non_profiles(tmp_path):
    src = tmp_path / "cab"
    src.mkdir()
    shutil.copy(COLOR_NEG, src / "Synthetic_Color_Negative.xml_2")
    shutil.copy(BW_NEG, src / "Synthetic_BW_Negative.xml_2")
    (src / "messages.xml_2").write_bytes(
        b'<?xml version="1.0" encoding="UTF-8"?>'
        b'<plist version="1.0"><dict><key>Messages</key><array/></dict></plist>'
    )
    out = tmp_path / "styles"
    written, skipped = convert_directory(src, out)
    assert (written, skipped) == (2, 1)
    assert (out / "Synthetic_Color_Negative.dtstyle").exists()
    # generated file parses and names the style
    root = ET.fromstring((out / "Synthetic_BW_Negative.dtstyle").read_bytes())
    assert root.find("info/name").text == "Synthetic_BW_Negative"


def test_batch_skips_non_plist_xml_2(tmp_path):
    """A *.xml_2 that is not an Apple plist is skipped, not fatal.

    Regression: plistlib.InvalidFileException used to escape convert_directory
    raw, aborting the whole batch.
    """
    src = tmp_path / "cab"
    src.mkdir()
    shutil.copy(COLOR_NEG, src / "Synthetic_Color_Negative.xml_2")
    (src / "garbage.xml_2").write_bytes(b"\x00\x01\x02not a plist at all")
    out = tmp_path / "styles"
    written, skipped = convert_directory(src, out)
    assert (written, skipped) == (1, 1)
    assert (out / "Synthetic_Color_Negative.dtstyle").exists()


def test_parse_rejects_short_shadow_list(tmp_path):
    """A Shadow array with fewer than 4 entries names the file and field.

    Regression: length went unchecked, so a short list died later with a
    bare IndexError inside encode_rgblevels (no file name, batch aborted).
    """
    plist = plistlib.load(open(COLOR_NEG, "rb"))
    plist["ImageSetting"]["ImageCorrection"]["Shadow"] = [320, 320]
    bad = tmp_path / "Short_Shadow.xml_2"
    bad.write_bytes(plistlib.dumps(plist))
    with pytest.raises(ProfileError, match="Shadow"):
        parse_profile(bad)


def test_parse_rejects_non_identity_sliders(tmp_path):
    """Non-zero Contrast/Brightness/Lightness and a USMColFactor other than
    [100, 100, 100] are rejected like non-identity ColorCorr, not dropped."""
    plist = plistlib.load(open(COLOR_NEG, "rb"))
    plist["ImageSetting"]["ImageCorrection"]["Contrast"] = 7
    bad = tmp_path / "Contrast.xml_2"
    bad.write_bytes(plistlib.dumps(plist))
    with pytest.raises(ProfileError, match="Contrast"):
        parse_profile(bad)
    plist = plistlib.load(open(COLOR_NEG, "rb"))
    plist["ImageSetting"]["ImageCorrection"]["USMColFactor"] = [100, 90, 100]
    bad = tmp_path / "USMColFactor.xml_2"
    bad.write_bytes(plistlib.dumps(plist))
    with pytest.raises(ProfileError, match="USMColFactor"):
        parse_profile(bad)


@pytest.mark.skipif(not CAB_DIR.exists(), reason="extracted installer CAB not present")
def test_batch_full_cab_count():
    with tempfile.TemporaryDirectory() as d:
        written, skipped = convert_directory(CAB_DIR, d)
    # 100 XMLs in the CAB: 98 film profiles + Default + messages
    assert (written, skipped) == (98, 2)


def test_cli_malformed_profile_error_style(tmp_path):
    """A malformed ImageCorrection plist must exit 1 with a one-line error,
    not a raw traceback — same contract as despeckle.py's CLI."""
    src = tmp_path / "cab"
    src.mkdir()
    plist = plistlib.load(open(COLOR_NEG, "rb"))
    plist["ImageSetting"]["ImageCorrection"]["USMAmount"] = "lots"
    bad = src / "Bad_USM.xml_2"
    bad.write_bytes(plistlib.dumps(plist))
    out = tmp_path / "styles"
    repo_root = Path(__file__).parent.parent
    proc = subprocess.run(
        [sys.executable, "-m", "tools.xml2dtstyle", str(src), str(out)],
        capture_output=True,
        cwd=repo_root,
    )
    assert proc.returncode == 1
    assert b"Traceback" not in proc.stderr
    assert b"xml2dtstyle: error:" in proc.stderr
    assert not (out / "Bad_USM.dtstyle").exists()


# ---------------------------------------------------------------------------
# darktable-cli acceptance: --style must apply and export with exit 0
# ---------------------------------------------------------------------------


def _fff_to_tiff(blob: bytes) -> bytes:
    """Make a write_fff blob palatable to generic TIFF readers (GraphicsMagick,
    darktable's non-raw loader): sort IFD entries ascending (LibRaw requires
    the Imacon tags descending, libtiff requires ascending) and add the
    baseline RGB tags write_fff omits (BitsPerSample, Compression,
    PhotometricInterpretation, SamplesPerPixel, PlanarConfiguration).
    darktable's rawspeed rejects scanner .fff outright (no decoder; dt issue
    #10266 — Flextight .fff opens only as TIFF)."""
    assert blob[:4] == b"II*\x00"
    ifd_off = struct.unpack_from("<I", blob, 4)[0]
    n = struct.unpack_from("<H", blob, ifd_off)[0]
    entries = [
        list(struct.unpack_from("<HHII", blob, ifd_off + 2 + i * 12))
        for i in range(n)
    ]
    delta = 5 * 12  # five new entries shift all out-of-line data
    entries += [
        [258, 3, 3, 0],  # BitsPerSample SHORT[3], backfilled below
        [259, 3, 1, 1],  # Compression = none
        [262, 3, 1, 2],  # PhotometricInterpretation = RGB
        [277, 3, 1, 3],  # SamplesPerPixel = 3
        [284, 3, 1, 1],  # PlanarConfiguration = chunky
    ]
    for e in entries:
        if e[0] in (273, 46274, 46275) or (e[1] == 2 and e[2] > 4):
            e[3] += delta
    for e in entries:
        if e[0] == 258:
            e[3] = len(blob) + delta
    entries.sort(key=lambda e: e[0])
    out = bytearray(blob[:ifd_off])
    out += struct.pack("<H", len(entries))
    for tag, typ, cnt, val in entries:
        out += struct.pack("<HHII", tag, typ, cnt, val)
    out += struct.pack("<I", 0)
    out += blob[ifd_off + 2 + 12 * n + 4:]
    out += struct.pack("<3H", 16, 16, 16)
    return bytes(out)


def _gradient_fff_bytes(width: int = 64, height: int = 48) -> bytes:
    """64x48 16-bit RGB gradient as a write_fff byte string (no numpy)."""
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            v = (x * 65535) // (width - 1)
            pixels += struct.pack("<3H", v, v, v)
    buf = io.BytesIO()
    write_fff(buf, bytes(pixels), width, height, metadata={"make": "Imacon"})
    return buf.getvalue()


def _seed_style(db_path: Path, xml_bytes: bytes) -> str:
    """Insert a .dtstyle into a darktable data.db the same way
    dt_styles_import_from_file would (data.styles + data.style_items)."""
    root = ET.fromstring(xml_bytes)
    name = root.find("info/name").text
    db = sqlite3.connect(db_path)
    try:
        db.execute("DELETE FROM styles WHERE name = ?", (name,))
        db.execute(
            "INSERT INTO styles (name, description, iop_list) VALUES (?, ?, NULL)",
            (name, root.find("info/description").text),
        )
        style_id = db.execute(
            "SELECT id FROM styles WHERE name = ?", (name,)
        ).fetchone()[0]
        for plugin in root.find("style"):
            db.execute(
                "INSERT INTO style_items (styleid, num, module, operation,"
                " op_params, enabled, blendop_params, blendop_version,"
                " multi_priority, multi_name, multi_name_hand_edited)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                (
                    style_id,
                    int(plugin.find("num").text),
                    int(plugin.find("module").text),
                    plugin.find("operation").text,
                    base64.b64decode(plugin.find("op_params").text),
                    int(plugin.find("enabled").text),
                    base64.b64decode(plugin.find("blendop_params").text),
                    int(plugin.find("blendop_version").text),
                    int(plugin.find("multi_priority").text),
                    plugin.find("multi_name").text or "",
                    int(plugin.find("multi_name_hand_edited").text),
                ),
            )
        db.commit()
    finally:
        db.close()
    return name


@pytest.fixture(scope="module")
def darktable_config(tmp_path_factory):
    """Isolated darktable configdir with an initialized data.db."""
    if not DT_CLI:
        pytest.skip("darktable-cli not installed")
    cfg = tmp_path_factory.mktemp("dtcfg")
    input_tif = cfg / "input.tif"
    input_tif.write_bytes(_fff_to_tiff(_gradient_fff_bytes()))
    init = subprocess.run(
        [DT_CLI, str(input_tif), str(cfg / "init_out.tif"),
         "--core", "--configdir", str(cfg)],
        capture_output=True,
        timeout=180,
    )
    assert init.returncode == 0, init.stderr.decode(errors="replace")
    assert (cfg / "data.db").exists(), "darktable did not initialize data.db"
    return cfg, input_tif


def _decode_tiff_pixels(path):
    """Decode the strip payloads of a darktable-exported TIFF.

    darktable embeds a fresh ICC profile whose bytes are not reproducible
    across runs, so whole-file comparison is unsound; strip payloads are
    compared instead. darktable writes Zip-compressed strips with
    horizontal-differencing prediction, so the decoded bytes are predictor
    residuals, not absolute pixel values — still a faithful difference
    signal for the acceptance assertions below, but not pixels proper.
    """
    import zlib

    data = path.read_bytes()
    ifd = struct.unpack("<I", data[4:8])[0]
    n = struct.unpack("<H", data[ifd:ifd + 2])[0]
    tags = {}
    for i in range(n):
        tag, typ, cnt, val = struct.unpack(
            "<HHII", data[ifd + 2 + 12 * i:ifd + 14 + 12 * i])
        tags[tag] = (typ, cnt, val)
    assert tags[259][2] == 8, "test expects Zip-compressed strips"  # Compression
    # 317 = Predictor; 2 = horizontal differencing, so the decoded strip
    # bytes are predictor residuals (see docstring), not absolute pixels.
    assert tags[317][2] == 2, "test expects horizontal-differencing prediction"
    nstrips = tags[279][1]  # StripByteCounts count == strip count
    if tags[273][1] == 1:  # StripOffsets
        offs, counts = (tags[273][2],), (tags[279][2],)
    else:
        offs = struct.unpack(f"<{nstrips}I",
                             data[tags[273][2]:tags[273][2] + 4 * nstrips])
        counts = struct.unpack(f"<{nstrips}I",
                               data[tags[279][2]:tags[279][2] + 4 * nstrips])
    return b"".join(zlib.decompress(data[o:o + c])
                    for o, c in zip(offs, counts))


def _style_cases():
    cases = [
        ("synthetic_color", COLOR_NEG),
        ("synthetic_bw", BW_NEG),
    ]
    if CAB_DIR.exists():
        for stem in ("Agfa_Ultra_50", "Kodak_Portra_160_NC", "Ilford_Delta_100"):
            cases.append((stem.lower(), CAB_DIR / f"{stem}.xml_2"))
    return cases


@pytest.mark.skipif(not DT_CLI, reason="darktable-cli not installed")
@pytest.mark.parametrize("label,profile_path", _style_cases())
def test_darktable_cli_applies_style(darktable_config, label, profile_path):
    """darktable-cli --style <name> must exit 0 and export the image."""
    cfg, input_tif = darktable_config
    profile = parse_profile(profile_path)
    xml_bytes, _ = profile_to_dtstyle(profile)
    style_name = _seed_style(cfg / "data.db", xml_bytes)
    out = cfg / f"out_{label}.tif"
    proc = subprocess.run(
        [DT_CLI, str(input_tif), str(out), "--style", style_name,
         "--core", "--configdir", str(cfg)],
        capture_output=True,
        timeout=180,
    )
    assert proc.returncode == 0, proc.stderr.decode(errors="replace")
    assert out.exists() and out.stat().st_size > 0
    stderr = proc.stderr.decode(errors="replace")
    assert "cannot find the style" not in stderr
    # The style must actually alter the pixels — not merely apply cleanly.
    # (Whole-file comparison is unsound: darktable's embedded ICC profile
    # differs byte-wise between any two exports.)
    assert _decode_tiff_pixels(out) != _decode_tiff_pixels(cfg / "init_out.tif")


@pytest.mark.skipif(not DT_CLI, reason="darktable-cli not installed")
def test_darktable_cli_saturation_differs(darktable_config):
    """Two styles differing ONLY in Saturation must produce different
    exports. This pins the colorbalancergb blob end-to-end: a params blob
    darktable silently rejects (e.g. the old 128-byte blob without the v5
    saturation_formula tail) makes saturation a no-op and this test fails.
    """
    cfg, input_tif = darktable_config
    outputs = []
    for saturation, tag in ((0, "sat0"), (100, "sat100")):
        profile = parse_profile(COLOR_NEG)
        profile.saturation = saturation
        xml_bytes, _ = profile_to_dtstyle(profile, name=f"sat_{tag}")
        style_name = _seed_style(cfg / "data.db", xml_bytes)
        out = cfg / f"out_{tag}.tif"
        proc = subprocess.run(
            [DT_CLI, str(input_tif), str(out), "--style", style_name,
             "--core", "--configdir", str(cfg)],
            capture_output=True,
            timeout=180,
        )
        assert proc.returncode == 0, proc.stderr.decode(errors="replace")
        outputs.append(_decode_tiff_pixels(out))
    assert outputs[0] != outputs[1]
