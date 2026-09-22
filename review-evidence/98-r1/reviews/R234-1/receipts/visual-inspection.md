[R234] Direct visual inspection, 2026-09-22.

Inspected both archived author PNGs (1022 x 1294):
- public/archive/review-evidence/98-r1/author/initial/render/F10.2-base-8452f56.png
- public/archive/review-evidence/98-r1/author/initial/render/F10.2-head-88a4eb4.png

Also inspected the reviewer-rendered final-head image at its original size
(511 x 647): render/F10.2-bc997e7.png, mmdc 11.16.0, white background, scale 1.

Both real states and the initial pseudostate are visible; all five arrow paths
retain their direction and endpoints. The revised return label is readable and
complete: restoration belongs to LINK_DOWN; declaration belongs to the next
LINK_UP. No label text is clipped, collides with another label's text, or escapes
the image. The adjacent gray label backgrounds touch/overlap slightly, as in the
base rendering; their text remains distinct at native size. The return arrow
still visibly terminates at DEFAULTS. The four unchanged labels remain readable.

SVG structure was separately checked in svg-structure.json and
independent-render.json. Node IDs, positions, all five edge paths and arrow-end
markers match across archived base/head; exactly one label record changes. The
independent final-head SVG has the same nodes, edges and labels as the archived
88a4eb4 head SVG. This inspection covers these actual mmdc renders; it does not
claim a separate GitHub browser-render screenshot.
