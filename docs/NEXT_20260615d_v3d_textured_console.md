# NEXT (epic): V3D texturing -> textured cube boot splash, fast GPU text console, emoji

Bryan's vision (2026-06-15): (1) a boot splash -- a small 3-D cube TEXTURED with
`art/aries_screen.png`, spinning + bouncing, then vanishing back to text mode; (2) a V3D
text mode that "looks nice and is very fast"; (3) emoji support (browser-style color
emoji, cf. the "Browser" column of unicode.org/emoji/charts/full-emoji-list.html).

**These are ONE epic with ONE enabler: V3D TEXTURE MAPPING (the TMU).** Phases 2-4
(clear/triangle/cube) are flat / per-vertex color. Texturing -- sampling a texture in
the fragment shader -- unlocks all three. Build it once (Phase 5), then 5a/5b/5c follow.

Builds on the completed V3D project ([[project_v3d_phase4]] cube, ..._phase3 triangle,
..._phase2 clear, ..._design). Same discipline: byte-exact host golden CL gate
(scripts/v3d_clcheck.py), QEMU graceful-refusal gate, HW-verify on the real Pi.

## Phase 5 -- V3D texture mapping (the foundation, the hard part)

The borrowed Random06457 QPU shaders (src/gpu/v3d_shaders.h) are flat/varying-color. Need:
- A TEXTURE-SAMPLING fragment QPU shader (a TMU0 read: write the s/t coords to the TMU,
  read back the texel). CHECK tools/v3d_ref/ (the Random06457 MIT reference) for a
  textured example to borrow/adapt -- that is how Phases 3/4 got their shaders.
- Texture config: the TMU's TEXTURE_UNIFORMS / config record (base addr, width, height,
  format e.g. RGBA8888, wrap/filter) emitted into the uniforms + CL.
- UV (s,t) varyings per vertex (extend the attr records + the vertex/coord shaders).
- Texture memory LAYOUT: V3D samples textures in a tiled format ("T-format" / utile
  swizzle), NOT linear. Either (a) emit textures in raster/"LT" (linear) format if the
  HW + the texture config allow it for our sizes, or (b) pre-swizzle the texture into
  T-format at upload. This is the main new complexity -- nail it with a small textured
  quad first (a "Phase 5 textured triangle/quad", golden-gated) before the cube.
- Pre-bake the image: like /images/splash.raw, add a build step that converts a PNG ->
  raw RGBA (+ T-format swizzle) into a CPIO/disk asset; no runtime PNG decoder needed.

Validate Phase 5 with a textured quad (1 face of aries_screen) to the live FB, HW-verified,
before 5a.

## Phase 5a -- textured spinning + BOUNCING cube boot splash -> text

- Map the aries texture onto the cube faces (UVs in v3d_cube.c; reuse the spin transform).
- BOUNCE: add a screen-space translation that reflects off the viewport edges (classic
  DVD-logo bounce) on top of the spin.
- Lifecycle: run ~N seconds at boot (a /proc/v3d.splash[.N] verb, or hook the getty/boot
  path BEFORE the login prompt), then v3d release (the existing --gpu-release path) ->
  fb_console restored = "text mode". Single-buffer tearing is fine (or Phase 4b
  double-buffer if wanted).
- Reuses the cube pipeline + Phase 5 texturing; mostly geometry + animation + a verb.

## Phase 5b -- V3D text console (nice + fast)

- Build a GLYPH ATLAS texture: bake the fb_console bitmap font (or a nicer TTF rasterized
  at build time) into one atlas texture; each glyph is a sub-rect (UV).
- Render text as a BATCH of textured quads (one quad/glyph, UV into the atlas) -- V3D
  draws hundreds of quads per submit, vastly faster than the CPU per-pixel glyph blit in
  fb_console. "Looks nice" = AA font, better typeface, sub-pixel.
- INTEGRATION (the design decision): does GPU-text REPLACE fb_console (V3D owns the FB
  persistently -- a bigger shift, since today V3D suspends the console and owns the FB
  only during a job), or is it a MODE you toggle? A persistent GPU console means the
  display_server drives V3D continuously + a damage/dirty-rect re-render of the char grid
  on each tty update. Scope this carefully -- it is a console-rendering subsystem, the
  largest of the three.

## Phase 5c -- emoji (color glyphs)

- Color emoji = color (RGBA) glyph quads -- falls straight out of the Phase 5b textured
  text path (the atlas is just RGBA, glyphs are colored).
- DATA SOURCE (decision): the unicode "Browser" column is per-browser (not a fixed
  downloadable set). The portable, browser-like choice is an open COLOR EMOJI set --
  Noto Color Emoji (Google/OFL, == the chart's "Goog" column look) or Twemoji (MIT). Bake
  the needed glyphs into a color atlas at build time (or load-on-demand -- thousands of
  emoji is a big atlas).
- RENDER PATH: UTF-8 decode in the tty -> codepoint -> if emoji, draw the color glyph quad
  from the emoji atlas. Handle multi-byte UTF-8 + (stretch) ZWJ sequences (flags,
  family/profession combos) which map a codepoint SEQUENCE to one glyph.

## Recommended order + open decisions

1. **Phase 5 texturing** (textured quad, golden-gated, HW-verified) -- the enabler.
2. **Phase 5a splash** -- self-contained, highly visible, proves texturing end-to-end (the
   fun first deliverable).
3. **Phase 5b GPU text console** -- the big one; decide replace-vs-toggle + the FB-ownership
   model first.
4. **Phase 5c emoji** -- rides 5b; decide the emoji data source (Noto vs Twemoji).

OPEN DECISIONS to confirm with Bryan: (a) emoji glyph source (Noto Color Emoji vs
Twemoji); (b) GPU text -- replace fb_console or a toggleable mode; (c) splash every boot
vs first-boot/toggle, and duration. None block Phase 5 (texturing) itself.

RISK/scope: Phase 5 texturing is real V3D work (the TMU + texture tiling are the unknowns
-- borrow from tools/v3d_ref). 5b is a rendering subsystem. Worth doing as a staged epic
exactly like Phases 2-4. Verify on the real Pi (QEMU has no V3D).
