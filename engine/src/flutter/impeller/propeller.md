## Propeller, and Impeller REDO

High level design:
  * Designed _only_ for modern GPU
  * Everything cachable
  * Analytical anti aliasing
  * Fix shader set + ubershaders
  * Proper fix for non-rotate/scale transforms


Each frame Flutter produces a Layer tree, with Picture nodes as layers. The Pictures themselves
are represented in the display list format. These pictures are trivially cacheable via existing framework
optimizations. A display list contains "high level" rendering operation descriptions.

Propeller is designed to produce "medium-level" cacheable picture representations from a high level display list:
vertex data in GPU buffers, program selection, texture atlases et cetera. This workload is desgined to run
from any thread, but primarily after picture construction on the UI thread.

Per-frame, the rasterizer thread queues up topologically sorted set of the "medium level" rendering operation,
performs any deferred work, and encodes GPU rendering operations. These operations may be cached in a texture
if complexity permits and the operations is compatible (For example, an existing saveLayer operation is a cache
candidate since it already renders to a texture). These choices are designed to minimize any CPU work for common
operations like scrolling.

## Cache Hierarchy

### Dealing with Scale Transforms

Many cache entries have to be effectively keyed based on scale, but the framework allows scaling/transforms to
change without notifying children. We modify layer tree so that any mutation or addition of non-scale/non-rotate
transforms triggers child invalidation of internal caches.

### Glyph atlas

Switch to a closer to skia level design. The glyph atlas uses fixed size pages (2k x 2K) and allocates more and 
compacts based on usage heuristics. Cache entries are populated during raster dispatch when total scale is known
which then lazily updates the picture description so that subsequent frames are treated as fully cached.

### Path and Shadow Cache

Every path is statically assigned one of four techniques at MLR build time (bounds, convexity, and a
segment-intersection sweep). All curve sources (including baked rects/rrects/ovals/arcs) go through a single chopper that emits quadratics at go through a single chopper that includes DPR only. No technique requires subdivision to line segments: concave and self-intersecting fills keep their quadratics (see Parity fill), so every tessellation is scale-independent and survives a scale change unchanged. The chopper is deliberately COARSE — a fixed 2 quads per conic, 3 per cubic — because the AA ramp lives in the sliver between each quad and its control net, and fine chopping starves it.

- Coverage atlas — device size under 32px. Rasterized on CPU and stamped as a quad instead of tessellated. Below that threshold the analytic tiers are wildly disproportionate: a 2px dot costs 144 vertices through the convex Loop-Blinn path — eight chopped conics with fringe strips, all describing structure finer than the antialiasing ramp — where a cached quad costs 6. Entries key on QUANTIZED DEVICE SIZE (1/8px), so scale is already folded in and the error is uniform rather than growing with the shape; they live in the glyph atlas, share the alpha_coverage branch, and therefore batch with text.
    - The real multiplier is reuse, not vertex count: `drawPoints` with 16k dots draws the SAME dot 16k times, so it is one rasterization and one atlas entry against 16k re-tessellations today. Measured on 16k points: 13.3ms and 61.5MB of vertex data become 22us and 2.6MB.
    - Resolution is deferred like text. The builder has no atlas — baking placements at build time would break on compaction — so a recipe records the key plus every instance centre, and the flatten emits 6 vertices per centre once the placement is known. One recipe covers every instance of a shape in a call.
    - Instances keep their fractional position and sample bilinearly. A coverage blob is smooth, so interpolation reads as a subpixel shift rather than the blur it would be on a glyph — which is why shapes need none of the phase variants text carries.
    - Currently discs only, which covers round-capped points and the stroker's round joins and caps. Both are the same observation: every disc on a stroke is the SAME disc, because the width never varies along it, so a polyline with N round joints is one entry and six vertices per joint rather than a 144-vertex tessellation at each. A stroke past the threshold keeps its tessellated joins.
    - Arbitrary paths are deliberately excluded: they would need a geometry hash per draw, which can cost more than it saves.
- Additive Loop-Blinn — CONVEX single-contour fills; all built-in shapes (rect/rrect/oval/arc/pie/chord) bake here. Convex → fan; convex rings / stroked closed shapes → radial strip (inner boundary by closed-form erosion, so self-intersection is exact and an empty erosion degenerates to a plain outer fill). Concave, self-intersecting and multi-contour fills go to Parity fill instead. Inward-bulging curves route the interior polygon through the control point with the implicit sign flipped — still purely additive, no erase pass. Boundary triangles are split so each touches at most one boundary edge. Coverage is 0.5 + f/fwidth(f) on f = u² − v for curves, straight edges (linear degenerate (0, d)), and interior (saturated) alike. Cached scale-independent; fully batched.
- Stroke strips — all strokes. Segment quads, join wedges, and cap pieces emitted along the cached centerline+normals; round joins/caps are discs (they overlap generously and their silhouette IS the round profile). Pieces need not be watertight with each other: silhouette edges carry analytic AA, edges abutting a neighbouring piece are hard, so no boundary is ever ramped twice. Hairlines render 1px wide with coverage × width. TRANSLUCENT strokes accumulate coverage additively (kPlus over white) in a virtual canvas and composite once, so joins and self-intersections blend exactly rather than double-darkening. Fully batched when opaque.
- Parity fill (Kokojima) — concave, self-intersecting, and multi-contour fills; also the general drawPath fill. Per segment: a fan triangle over the CHORD from a per-contour anchor, plus (for curve segments) the standalone Loop-Blinn curve triangle covering the chord↔curve lune. Emitted white with XOR blending into a virtual canvas, then composited in the paint colour — no stencil buffer, no linearization.
    - XOR needs no orientation test (an outward bulge and an inward dent are the same toggle) and no discard (α = 0 outside a curve leaves the destination untouched). Any anchor works: a closed contour's fan covers a point an odd number of times exactly when the point is inside it.
    - AA is continuous, not sampled: coverage rides the alpha, so the toggle is antialiased — over an empty pixel XOR writes c, over a filled one 1 − c. (This is what alpha-to-coverage bought the original on 2006 hardware; per-fragment analytic coverage is strictly better.)
    - Edge classification is load-bearing: a LINE segment's fan edge IS the silhouette, so it carries the ramp plus a 1px fringe; a CURVE segment's fan triangle is HARD, because its chord is internal and antialiasing it would conflate against the lune. The two edges meeting the anchor stay hard, so adjacent fans cancel exactly (iso-lines of f run parallel to the chord).
    - Holes fall out of parity with no hole detection, and the exactly-once composite makes translucent fills correct through self-overlap.
    - Both fill rules come from ONE accumulator. Attachment slot 7 is a dedicated R16Float winding buffer (signed and unclamped — a unorm target would clamp back-facing contributions to zero); the fragment negates coverage when `[[front_facing]]` is false and the blend simply adds, so an outward bulge adds and an inward dent subtracts with no CPU orientation test. The composite then maps the accumulator: `saturate(|w|)` for nonzero, `1 − |fract(w/2)·2 − 1|` for even-odd. Both are exact on integer windings *and* on the fractional values that antialias a boundary.
    - It costs no canvas depth and needs no scope: the accumulator is always present, so a concave fill is three draws (accumulate → resolve into the current canvas → zero the accumulator) that nest freely inside scopes. Slot 7 is why `kMLRMaxCanvasDepth` is 6, and the tile footprint drops to 4 + 6×4 + 2 = 30 bytes/pixel.
    - Triangle ORIENTATION is data, not incidental: the fan carries the contour direction, curve triangles keep their natural orientation, and each fringe quad is forced to match its fan triangle (its natural order is the opposite one) so the antialiased outer ramp accumulates the same sign.
    - Reference: Kokojima, Sugita, Saito & Takemoto, *Resolution Independent Rendering of Deformable Vector Objects Using Graphics Hardware*, SIGGRAPH 2006 Sketches.
- Shadows — two geometry options: feathered ring mesh (SkShadowUtils-style concentric rings with per-vertex coverage approximating the Gaussian; preferred when σ is large relative to feature size since rings collapse gracefully), or widened implicit (reuse the shape's Loop-Blinn geometry outset by ~3σ, falloff normalized by σ instead of fwidth through a Gaussian transfer; σ rides in Paint).
- Offscreen coverage (optimization only) — static expensive path redrawn many frames, under the general cache-op-as-texture policy. R8 at exact scale; MSAA permitted only here; composites as one batched quad.

### Backdrop Filter

Determine area impacted by filter, re-dispatch draws to offscreen texture at 1/2 resilution, render, et cettera,
without ending main pass.


### Medium Level Representation.

Batched and Presorted. Sorting works by combining non-overlapping compatible draws. Only necessary when draws
dont already combine. Record time batching is a cheap windowed pass within a single picture; batching ACROSS
pictures is the render-time scheduler's job (see Draw Scheduling). Each Medium Rep.
picture is One or more vertex buffers, a range of missing glyphs/glyph bit, vector of textures, and vector of draw commands, rendering bounds. It has some queryable properties like opaque occlusion, coverage.

####

Text geometry depends on exact scale (glyph raster size, subpixel phase) and atlas state (UVs), neither known at MLR build time. The MLR must store additional information on how to render text (the shaped run), plus a stamp containing information
on the best guess for the current scale. this best guess is used to eagerly populate the cache and vertex buffer, and
thrown away and updated if it doesnt match. Typically DPR is sufficient guess.

For text, we do not want to continually re-upload vertex data due to small translations. Subpixel-eligible glyphs rasterize four X-phase variants into a single 4×-wide atlas slot. The vertex shader reconstructs the un-snapped device position from the baked snapped position and 2-bit phase, applies the draw's residual translation from the paint buffer, then re-snaps and selects the phase variant per vertex. Y snaps to whole pixels and rotated draws bypass snapping and sample phase 0 with bilinear.

## Rendering Design

### Draw Scheduling

The per-frame flatten hands the renderer one flat list of draws covering every picture in a pass. Turning that
list into as few draw calls as possible is a scheduling problem, not a merging problem.

Each draw gets a **level**: one past the deepest draw it must follow. Draw B depends on draw A when A comes
first, the two share an attachment, and their pass-space bounds overlap. Everything a draw touches is
pixel-local, so a shared attachment plus overlapping bounds is exactly the condition under which order is
observable — nothing else constrains it, including which picture the draw came from.

Two draws that land on the same level can never conflict: a conflict would have pushed the later one deeper.
So a level's draws may issue in any order, and every draw in a level sharing a routing (technique, canvas,
canvas source, reset, winding mode, fill rule, blend, opacity, scissor) collapses into a single call.

This replaced a greedy backward merge — walk back from each draw, merge into the first compatible run, stop at
the first conflict. That walk cannot recover from a real dependency sitting behind it, which is the common case
whenever a picture draws overlapping shapes. A grid of icons, each its own DisplayList, each filling several
mutually overlapping concave paths, is the worst version of it:

```
Recording order — 3 icons, each its own picture, paths within an icon overlap:

    icon 0              icon 1              icon 2
  A0 C0 A1 C1 A2 C2   A0 C0 A1 C1 A2 C2   A0 C0 A1 C1 A2 C2

  An = accumulate winding for path n   -> writes attachment 7
  Cn = composite path n                -> reads + CLEARS 7, writes colour 0

  Within an icon: A1 must follow C0 — C0 drains the accumulator that A1
  would otherwise corrupt. A real dependency.
  Across icons:   nothing overlaps. Everything is free to batch.

Greedy backward merge:

  icon 1's A0 walks back, skips C2/A2/C1/A1 (disjoint bounds), merges  ✓
  icon 1's A1 walks back one step, hits its OWN C0, and stops.         ✗
                └─ correct barrier, but the walk dies there and A1 can
                   never reach icon 0's A1, which is compatible.
  => 4 stranded calls per icon, on every icon, forever.

Levels — order by depth, not by distance:

  level:    0    1    2    3    4    5
  icon 0:   A0   C0   A1   C1   A2   C2
  icon 1:   A0   C0   A1   C1   A2   C2
  icon 2:   A0   C0   A1   C1   A2   C2
            └────┴────┴────┴────┴────┴──> one draw call per column

  The dependency chain inside an icon is preserved exactly (A1 is still
  below C0); icons simply stopped being in each other's way.
```

Measured on that shape at benchmark scale — 15 icons x 3 overlapping even-odd paths, plus a row background per
row — 93 draws schedule into **7 calls**: six levels plus the row rects. The greedy scheduler produced 63.

Scissor-tier rect clips are part of a run's routing, which makes them the one piece of non-pixel state that
can fence otherwise-mergeable draws, so they are normalized before scheduling. A scissor that already contains
everything the draw can touch (its bounds intersected with the pass coverage) removes nothing and is dropped
entirely, letting the draw batch with its unscissored neighbours — a viewport-sized list clip stops splitting
the list's contents. What survives is snapped to device pixels, so two rects that resolve to the same scissor
compare equal instead of splitting a run. The encoder likewise compares the *resolved* device rect before
re-binding: comparing float rects re-emits identical `setScissorRect` calls, and an explicit whole-target
scissor is the same state as none at all.

Cost is one mask test (an integer AND, which rejects most candidates) and, when the masks intersect, a rect
overlap test, per candidate scanned. Scanning every earlier draw would be quadratic, so the scan is windowed
and whatever drops out of the window folds into a per-attachment high-water level. Folding is conservative in
the safe direction: it can only push a level deeper, never let two conflicting draws share one. The window
depth follows a fixed comparison budget, so ordinary frames scan hundreds of draws back for a few tens of
microseconds and only pathological draw counts reach the cap, where the fold takes over and simply batches
less.

### Mask blurs

A blur MASK filter blurs a shape's coverage rather than the pixels under or around it. It matters more than it
looks: `BoxShadow::toPaint` sets a blur mask filter rather than calling `drawShadow`, so **every Flutter
BoxShadow arrives this way** — dropping the filter renders a hard-edged blob exactly where a soft shadow
belongs.

A blurred convex shape is what the shadow mesh already draws, so for convex geometry this costs no pass and no
texture: build the silhouette, hand it to the shadow mesh in the draw's own colour, and it batches as ordinary
`kColor` content like every other shadow. The gaussian LUT is a CDF normalized across ±2σ, so the penumbra band
that reproduces a blur of `sigma` is exactly `2 * sigma` wide on each side. Concave shapes need a real coverage
pass and warn for now.

All four blur styles fall out of that one band. Writing `S` for the shape's coverage and `B` for its blur, the
band already runs from `inner` (a blur radius inside, full coverage) through the silhouette (0.5) to `outer`
(zero) — so the styles differ only in where the interior fan stops and which stretch the ring spans:

```
             inner            silhouette            outer
   coverage    1 ================= 0.5 ================ 0

   normal  B         fan→inner   | ring inner ─────────→ outer
   outer   B(1-S)    (no fan)    | ring silhouette ────→ outer
   solid   max(S,B)  fan→SILHOUETTE | ring silhouette ─→ outer
   inner   B·S       fan→inner   | ring inner → SILHOUETTE
```

`solid` is the one with a trap. The obvious build — draw the blur, then the shape solid over it — overlaps
along the boundary, and src-over composites that seam twice on exactly the translucent colours shadows are made
of, leaving a dark line where the eye is drawn. Accumulating coverage in a canvas instead fixes the double
blend but over-covers (`S + B ≈ 1` where `max(S, B) ≈ 0.5`). Cutting the single mesh differently avoids both:
the fan's outer boundary and the ring's inner boundary are the same POSITIONS carrying different `u`, so the
triangles abut watertight and the coverage step across them is deliberate. One draw, no overlap, still batched.

The penumbra ring is inset from the silhouette, and how far it may go is bounded twice. Past the INRADIUS — the
centroid's distance to the nearest edge — the polygon eats itself; past a rounded corner's own RADIUS that
corner turns inside out and neighbouring offset points cross. Either way the fan and ring triangles overlap and
a translucent shadow composites those pixels twice, speckling the penumbra. The bound is one distance shared by
every vertex: clamping per vertex (each by its own distance to the centroid) insets different vertices by
different amounts, which stops the result being an inset polygon at all.

When that bound bites, the inner ring is NOT a blur radius inside — so its uv is placed at the distance it
actually reached, and the centroid's at its own distance from the outline. Asserting full coverage at a ring
that never got a blur radius in would invent light the blur never had: a small shape under a big blur would
read as a solid core with a skirt rather than the soft lump it is. With the uvs following the geometry the
interior peaks below 1, as a real gaussian of a small shape does. This stays a mesh approximation — coverage
that far inside depends on the whole outline, not the nearest edge, so a blur several times wider than its
shape still reads high — but it is monotonic in the right direction and costs nothing beyond the mesh.

Building this exposed a bug in the shared penumbra ring. Offsetting a corner along the AVERAGED edge normal
moves it less than `blur_radius` from either edge — at a right angle only `blur_radius/√2` — which narrowed the
band along every edge of a rectangle by 30%. The miter scale `2/|n0 + n1|` (that is, `1/cos(half angle)`)
restores the perpendicular distance, clamped by `kMLRShadowMiterLimit` so a near-cusp corner produces a long
spike rather than an unbounded one. Elevation shadows were slightly too tight for the same reason and are now
correct.

### Filters

Filters split by what an output pixel depends on, and that split decides everything else about how they run.

**Color filters are framebuffer local** — output at a pixel depends only on the input at that same pixel — so
they never need to read a neighbourhood. All four DisplayList filters are supported (blend with any of the 29
modes, the 4x5 matrix, and both gamma conversions), and they run in whichever of three tiers fits where the
filter came from:

- **Folded into the color (no GPU work at all).** See below — this is the common case.
- **Canvas fetch**, for a filter inside a picture (a `saveLayer` with a filtered paint): content renders into a
  virtual canvas and the composite that carries it down a level transforms the color on the way. No texture, no
  pass — `CompositeFilterMain`, same vertex layout as everything else.
- **Texture composition**, for a filter layer in the flow layer tree (`ColorFilterLayer`): children render to
  their own pass and `FragmentMainColorFilter` filters the sampled texels as the composite reads them. This
  costs a pass but caches like one — an unchanged subtree recycles its texture and only the composite re-runs.

The last two evaluate the identical filter math and differ only in where the color comes from, so an effect
cannot change appearance depending on whether it arrived as a `saveLayer` or a `ColorFilterLayer`. Matrix and
gamma are defined on unpremultiplied components and round-trip through straight alpha; blend is defined
premultiplied and stays that way. A blend filter's MODE is a specialization constant, not uniform data: a
runtime mode would compile all 29 blend functions into every filter pipeline and branch through them per
fragment, so instead each pipeline is built lazily and compiles exactly the one function it uses — and every
non-blend filter kind shares a single variant. `setInvertColors` is just a matrix filter, composing after the
paint's own as a second nested scope.

**The important case never reaches the GPU at all.** A filter over a color that is constant across the draw is
exactly reproducible on the CPU, so solid fills, strokes, and glyph runs fold the filter into their color at
record time and emit an ordinary draw — no scope, no canvas, no composite. Only per-pixel color (a gradient or
sampled texels) has to be filtered on the GPU, and only those draws open a scope. The CPU evaluator and the
shader are held to agreeing: a parity test renders each filter through a scope and compares against the folded
constant.

**Image filters sample neighbouring pixels**, so a canvas fetch cannot serve them at all — it only ever sees the
pixel under the fragment. They always compose a texture: the subtree becomes its own pass and the composite
filters as it samples. Both the `ImageFilterLayer` in the layer tree and a blurred `saveLayer` inside a picture
land here. Caching is the ordinary pass mechanism — the content pass keys on its content, and the filter mixes
into the parent's `composite_key`, since it changes the parent's pixels without changing the child texture.

A gaussian blur is separable, and is recorded as **two nested filter layers** rather than one 2D kernel — so it
falls out of the existing pass machinery with no special casing, and costs 2R+1 taps twice instead of squared:

```
  vertical filter layer          pass 2: samples pass 1, blurs in y  ─┐
    horizontal filter layer      pass 1: samples pass 0, blurs in x   │ each
      content                    pass 0: the subtree, cached          │ cacheable
                                                                     ─┘
  Coverage is outset by the blur radius at every level: the filter reads
  past the content AND writes past it, so the pass has to hold the bleed.
```

A MATRIX image filter is the other implemented kind, and it needs no shader at all. It reads no neighbouring
pixels — it transforms the rendered result — so the children render to a pass exactly as before and the
composite samples that pass over a quad whose four corners are the matrix applied to the coverage corners.
Residual transforms are rigid, which is precisely why the matrix rides the composite GEOMETRY: an arbitrary
affine (skew, non-uniform scale, rotation) is expressible there and could never be carried by a residual. The
matrix mixes into `composite_key`, since it changes the parent's pixels while leaving the child texture
identical.

Dilate, erode and compose are still unimplemented; they warn and render their content unfiltered rather than
dropping it.

**Backdrop filters** filter what is ALREADY on the destination, which is a read-after-write on the pass target
in the middle of a pass — the one thing the plan-everything-up-front model has no natural place for. The same
locality split decides how expensive that is, and the cheap half is very cheap:

A **framebuffer-local backdrop** (a color filter, or a `DlColorFilterImageFilter`) needs no capture at all. The
destination arrives through programmable blending — the pixel under the fragment is exactly and only what the
filter needs — so it is one quad over the layer bounds that fetches its own attachment, filters it, and
composites the result straight back. `FragmentMainBackdrop`, blending disabled (the destination is already in
hand; letting the hardware blend it again would count it twice). No pass, no texture, no readback, and the
draw batches like any other. Ordering falls out of the level scheduler for free: the backdrop reads and writes
attachment 0, so it lands after exactly the draws beneath it that it overlaps, and draws elsewhere on the
target keep batching straight past it.

The quad is emitted BEFORE the layer's own scope opens — the backdrop belongs to the destination being
composited into, not to the layer's content. (A consequence: a translucent layer's alpha reaches its content
but not its backdrop, which warns rather than rendering something subtly wrong.)

A **blur backdrop** cannot come through here — it needs neighbours, so it needs the destination as a real
sampleable texture. The usual answer is to end the pass, resolve, sample and restart, which on a tile GPU costs
a full store *and* reload of the entire framebuffer even for a small frosted panel, and breaks the batching the
draw scheduler works to preserve.

Propeller does not read the target back at all. At the moment the planner reaches a backdrop layer,
`plan.passes[pass_index].entries` **is** the complete record of everything painted into this pass — so the
backdrop is *reconstructed* rather than recovered: take every prior entry overlapping the region, re-render it
into a small pass, blur that, and draw the result back. The capture pass is self-contained, so it simply
executes before the pass it feeds, and there is no read-after-write anywhere.

Three things keep the cost of re-rendering low:

- Entries that are themselves pass composites are **re-referenced, not re-rendered** — a nested subtree costs a
  texture sample here, not a second traversal.
- The capture renders **downsampled** (below), so it is a small fraction of the fragments.
- Only entries overlapping the region are captured at all.

It must be *every* overlapping entry rather than a recent few — that is what reproduces the destination
faithfully, blends beneath included. The capture also samples only the region's slice of the blurred texture on
the way back: the capture is deliberately wider than the region so the kernel has real pixels to read, and
stretching the whole thing across the region would rescale and displace the backdrop.

Reconstruction has one structural consequence: a capture pass references passes planned *before* it, so it can
point at LOWER indices — the only place the "children are created after their parent" invariant does not hold.
Cache keys are therefore resolved **on demand, in dependency order** (memoized, with open/done marking so a
cycle cannot recurse) rather than swept high index to low. That handles both directions and needs no invariant
about creation order at all — and it is what lets a backdrop recycle: if nothing underneath changed, the
capture's key is unchanged and neither it nor its blur is re-encoded. Change any content beneath and the
invalidation propagates up through the capture to the blur to the composite.

Because two backdrops over identical content with identical coverage hash the same, they share a cached
texture — a poor-man's version of `backdrop_id` sharing, without the id.

### Backdrop groups

A group of backdrops costs **one capture and one blur** between them, however many members it has. The capture
covers the union of their regions and each member composites its own slice of the shared blurred texture (the
same sub-rect UV the single case already needs). Three backdrops drop from seven passes to three.

Membership is decided two ways, and they differ in what has to be proven:

**By backdrop id**, the framework has declared that these read the same backdrop — the surface as it stood when
the group began. That is a semantic guarantee, not an optimization hint: members are *defined* not to observe
one another, so they share even when their regions overlap, and nothing has to be proven about where they sit.

**Automatically**, with no id, sharing has to be earned. Joining a group means reading the backdrop as it was
when that group started, so a backdrop may only join if **nothing drawn since the group began touches what it
reads** — its region outset by the blur radius, since that is what the kernel samples. A pre-walk tracks the
running union of content emitted since the open group started, and any backdrop that intersects it starts a new
group instead. This is what lets a grid of separate frosted panels collapse to one capture, while two
overlapping backdrops — where the second genuinely reads the first's output — stay separate.

Both kinds are then filtered by whether sharing is worth it: single-member groups and groups whose filters
disagree are discarded (Propeller shares the capture and the blur together or not at all), as are groups whose
bounding box is more than `kMaxBackdropUnionRatio` times the summed area of their regions. That last one is the
real tradeoff — merging swaps fragments for passes, reconstructing one larger area instead of several small
ones, which is a clear win while the union stays close to the sum and a loss once the members are far apart.

Pass-creating layers are barriers: the union is collected in root space, so a group cannot reach across into
another pass. Scissor-tier rect clips are not barriers, which matters because a `BackdropFilter` is normally
wrapped in a `ClipRect`.

### Culling backdrop work

A backdrop is a capture, a blur chain and a composite, and all of it is wasted when the result cannot be seen.
Two checks remove it:

- **Clipped away.** The region is intersected with the scissor before anything is planned. If nothing survives,
  the whole chain is never created; if part does, the capture, the blur and the composite all shrink to the
  visible part rather than reconstructing pixels that are about to be clipped off.
- **Occluded.** After planning, an entry whose result a LATER opaque draw completely covers is dropped along
  with the passes that exist only to feed it (`exclusive_passes`), which the renderer then skips entirely. The
  occluder test uses the `opaque_coverage` rects pictures already record for opaque axis-aligned fills, clipped
  by the occluder's own scissor. Only a single rect is allowed to hide a region — a union of rects could cover
  it between them, but the pairwise test is what pays off in practice (a solid sheet over a frosted panel) and
  it never claims occlusion that is not there. Translucent draws and pass composites are not occluders.

### Blur resolution

A blur destroys exactly the detail that extra resolution would carry, so blur passes render small. The scale is
adapted from impeller's `GaussianBlurFilterContents::CalculateScale`: powers of two only (cleanest resampling),
floored at 1/16, chosen to hold the *downsampled* sigma near 4. Sigma itself goes through impeller's taper and
the same `kKernelRadiusPerSigma` relationship, so both renderers truncate the gaussian tail identically.

The point of targeting a fixed downsampled sigma is that **the tap count stops growing with blur strength**:

```
  sigma  1 -> scaled  1.00, resolution 1      3 taps  (full-res would be  3)
  sigma  4 -> scaled  3.95, resolution 1     13 taps  (full-res would be 13)
  sigma  8 -> scaled  7.78, resolution 1/2   13 taps  (full-res would be 27)
  sigma 16 -> scaled 15.14, resolution 1/4   13 taps  (full-res would be 49)
```

Coverage is outset by what the kernel actually **reads**, which is the tap count scaled back up — deliberately
not the full-resolution radius. Those two agree until the tap cap bites, and then they diverge in the dangerous
direction: the cap clamps the full-resolution radius while the low-resolution one, computed from a small
downsampled sigma, is nowhere near it. At sigma 24 the real reach is 36px against a capped radius of 24, so
outsetting by the radius would leave the blur sampling 12px outside its own texture.

Where that shows is worth knowing, because it is not symmetric. An image filter's coverage is its content plus
margin, so the capture edge sits in empty space and clamping there costs nothing. A backdrop reconstructs
content that continues past its capture, so its edge texels are real pixels and clamping smears them inward.

...on top of a quadratic saving in pixels. And because Propeller *plans* the content rather than receiving a
texture, it renders the subtree small in the first place instead of rendering it full size and shrinking it
afterwards — there is no separate downsample pass. Geometry stays in full-resolution coverage units and only
the texture and viewport shrink, so nothing is re-recorded and residual transforms stay rigid.

### Canvas Stack

To make group alpha and clipping fast, we'll exploit TBDR by building a stack of canvases, with secondary
canvases represented as memoryless alternative color attachments. On metal and vulkan, we can dynamically select
the rendering destination using color attachment output and framebuffer fetch. If depth exceeds the number of
canvas textures we can allocate, we spill to an offscreen. For platforms which don't support framebuffer fetch,
treat all textures as spilling.

### Text Rendering

100% via custom freetype. No support for COLR baked in initially, only alpha bitmaps or PNGs.

### Textures

Support the sampling via hardware sampling. All textures are bound in one giant array, with a configurable
max size that splits. For platforms without bindless, the size is 1. Texture key is texture id + sampler mode
combined as we use combined image samplers.

### Clipping

Rectangular clipping is applied in the vertex shader via re-mapping of coordinates and UVs. Non-rectangular
clips are treated as blends/offscreen rendering.

### Gradients

A gradient's parameters live in its own coordinate space, and `DlColorSource` carries a LOCAL MATRIX that maps
that space onto the geometry (`GradientRotation` and friends). Evaluating therefore maps the draw-local position
back the other way, so each `MLRGradient` stores the INVERSE as a 2x3 affine and the fragment shader applies it
before computing the interpolant. Only the translation carries units: the shader is handed a physical position
while the matrix was authored in logical coordinates, so the 2x2 is scale-invariant and the translation is
scaled. The affine is inverted directly rather than through the general 4x4 inverse -- cheaper, and it detects
singularity honestly (a degenerate matrix leaves the gradient untransformed). Perspective in a gradient matrix
is not expressible.

Ramps are drawn on the GPU, not rasterized on the CPU. A ramp is a piecewise linear interpolation between its
stops, which is exactly what the rasterizer already does with vertex colours — so a row is a strip of line
segments with a vertex per stop, and the hardware fills in the 1024 texels between them. Colours are
premultiplied at the vertices so the interpolation matches what the sampler reads back. All pending rows batch
into a single draw of independent line segments.

The ramp texture is allocated once at full row capacity and populated by a render pass that **clears the first
time and loads every time after**, so rows already drawn survive untouched. Adding a gradient costs one short
line strip rather than a 4KB CPU loop plus a re-upload of every row that came before it — and because rows are
reserved and never move, one texture is enough. Deduplication keys on the stops rather than on a rasterized
row, since there is no longer a row to hash.



Gradient uber shader with 1024x1024 atlas texture. The first N rows are reserved for trivial textures,
while the rest of the rows are always rendered at full resolution. Then we sample with bilinear to
render elsewhere. Ideally populated with fast SIMD. RGBA8888.

### Shader Layout

Uniform data only used for non-scale translate transform component.

#### Vertex Layout

All draws use the same vertex layout. Position is a separate buffer from the rest!

Float2 x, y
Float2 u, v
Int32 color4
Int32 paint_index

This allows us to combine the configuration for multiple similar draws together
with the same ubershader. push constants or uniforms are only used for top level
transforms

Example:

DrawRRect(x1, y1, x2, y2, cornerRadi=10)
DrawRRect(x3, y3, x4, y4, cornerRadi=20)


Vertex:

[ x1, y1, x1, y2, .... xy, y4]
Paints: [0, 0, 0, ...., 1, 1, 1]

paint = 1              paint=2
[[corner_radii = 10], [corner_radii=20]]


#### Paint Buffer

An SSBO or Generic buffer which each draw indexes into. Contains a union of all dynamic data:

Paint {
  gradient_data, or -1
  transform offset
  texture index
}


### Examples:

```dart
Canvas canvas;
canvas.drawRect(Rect.makeLTRB(0, 0, 100, 100), Paint()..color = Colors.red)
canvas.drawRect(Rect.makeLTRB(100, 100, 200, 200), Paint()..color = Colors.blue)

```

This becomes:

```
Medium Level Representation:
{
Buffer_Pos: [(0, 0), (0, 100), (100, 0), (100, 100), (100, 100), (100, 200), (200, 100), (200, 200)]
Buffer_Rest: [(..)]
Index: [0, 1, 2, 1, 2, 3, 4, 5, 6, 5, 6, 7]
Draws: [ShaderX]
Bounds: (0, 0, 200, 200),
Opaque: [(0, 0, 100, 100), (0, 0, 200, 200)]
text: no
}
```


```dart
Canvas canvas;
canvas.drawParagraph(...)
```

```
Medium Level Representation:
{
Buffer_Pos: [Shaped Positions Yes]
Buffer_Rest: std::nullopt
Index: [0, 1, 2, 1, 2, 3, 4, 5, 6, 5, 6, 7]
Draws: [ShaderText]
Bounds: std::nullopt
text: yes
}
```

Shader Design:

```
// Spec constant for advanced blend mode, default false.
constant float advanced_blend;


float ComputeCoverage() {
    if (alpha_coverage) {
        return texture(textures[paint.texture_index], GetUV()).a;
    } else if (analytical_round_rect) {
        return ComputeCoveragePaint();
    } else {
        return 1;
    }
}

float SampleTexture() {
    if (alpha_coverage) {
        return (1, 1, 1, 1);
    } else {
        return texture(textures[paint.texture_index], GetUV());
    }
}


float4 Main() {
    float coverage = ComputeCoverage();
    float4 texture_color = SampleTexture();
    float4 interpolated_color = GetColor();
    float4 value = coverage * texture_color * interpolated_color;
    if (advanced_blend) {
       return AdvancedBlend(Framebuffer[0], value);
    } else {
        return value;
    }
}

```

### Texture Atlas / Page System

Used by both glyph atlas and gradients. We configure fixed size pages that we allocate from according to some rule.
For gradients it might be a fixed resolution, for glyphs it is skyline. We allocate until it is full, then allocate a
new page if necessary. Each frame we mark which ones are used. If we can replace in place, we do that. If not, we compact
whenever there are more than one pages and the ratios are such that total utilization is < 50% on at least 2.

This is a shared based class. Must be thread safe.

class PagedAtlas {
  PagedAtlas(Format, width, height, int max_pages, float compaction_percent)
    
  // Incremented whenever there is a compaction, must invalidate all references into it
  int getGeneration();

  Texture[], size_t GetTextures()

  (u, v, i) PlaceEntry(width, height);

  virtual (u, v, i)  PlacementStrategy(width, height);
}


### Layer Tree

Flutter is a mixture of Layers and Picutres. Pictures are converted to MLR objects, layers
combine these pictures with other operations like transforms, translations, filters. We must
flatten the layer tree so it can be traversed in something like a topo sort ordering

[ T ]
  |
[ T ]
  | \
  |  \ 
[ P ]  [P ]

Example: two transform layers, and two pictures. This can be expressed as a single render pass.

[ T ]
  |
[ O ]
  |
[ P ]

Example: Transform layer, opacity layer, and a picture. We can either fold the opacity into the picture
layer via the canvas stack, or render the picture to a texture with an offscreen render pass and cache it.

In the dispatcher, we'll covert flutter layer operations into a compact tree representation. layers which
do not require compositing will be flattened, layers that do will not. Then we topo sort and render. We use a
key/dirty bit signal from flutter - maybe just the MLR identity - to determine if an offscreen rendered texture
can be reused safey.

This operation replaces display list flatten - we avoid a single display list as that is information destroying.
