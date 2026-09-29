# Revision 20 manufacturing review

Reviewed 2026-09-28 against `enclosure.scad` and `dimensions.json`. **The current prototype is not fully qualified for JLC3DP manufacture or supplier insert installation.** All five final revision 20 STL meshes passed topology checks and all four drawing pages passed independent dimension/hash and rendered visual checks. Numerical viewer checks passed with 73,814 finite faces and exact SW1/SW2/encoder pad envelopes; the PCB travel stop cutaway displayed cleanly, with the gap visible and no reported console errors or warnings. Physical fit remains unverified. Mesh integrity does not establish material strength, printed fit, finish quality or supplier acceptance. No supplier has been selected and no quote obtained. The user rejected Protolabs' published starting cost; pursue lower-cost quotations first.

## Geometry requiring supplier review

The assembled body remains 80 × 56 × 29 mm, or 30 mm including raised controls.
Revision 20 adds a lid-owned PCB anti-lift stop: a 3 × 1.2 mm R0.2 foot at
(72.5,42.3), Z21.5, nominally 0.2 mm above the PCB. It widens to a 4 × 2.4 mm
R0.4 root at Z23.5 and joins the lid. The upper display-face bearing patch is component-free solder mask: no drill
centers, component bodies or mask/paste openings were found inside the reviewed
footprint.

The prior lower seat at X71-74, Y42.1-42.9 conflicted with RESET and its pads.
The replacement is X69.3-70.5, Y32.75-34.75, bearing at Z19.7 with the same
2.4 mm² area and a 1.2 × 2 mm section. This separate lower bearing land includes
tented vias beneath solder mask; it is not unperforated bare FR4. The upper
stop and relocated lower seat
are independent, with no intended clamp force. Print the new lid and midframe;
revision 19 base, button strip and power slider remain reusable. Five parts and
six M2 insert positions are retained.

| Feature | Current nominal dimension | Action before manufacture |
|---|---:|---|
| PCB anti-lift gap | 0.2 mm above nominal PCB | Review printed/PCB tolerance stack; lid must close without force before screw tightening. |
| Relocated USB-side lower seat | 0.275 mm minimum sourced body gap; 0.29 mm to USB-hole mask | Confirm actual solder/component envelopes and printed dimensions. |
| Wheel-side hardware clearance band | 0.6 mm wall | Thicken or obtain explicit material/process acceptance while preserving wheel and PCB clearance. |
| Closed right screw well | 0.75 mm rim | Review installation and screw loads; preserve its closed wall. |
| Button return arms | 0.8 mm thick | Qualify repeated flexing; consider a tougher material for the separate button strip. |
| Lid receiver entrance | 0.8 mm minimum inboard web | Review strength and finishing access. |
| Midframe alignment keys | 1.5 mm thick, 2.5 mm high | Review as positioning features, including handling loads. |
| Lid-key and LCD fits | 0.2 mm per side | Confirm clearance after finishing; currently only a nominal fit. |
| Button-to-hole clearance | 0.4 mm radial | Increase or qualify the movement gap for the selected process. |
| PCB/button anchor pilots | Ø1.2 mm | Review blind-hole formation, screw engagement and coating blockage. |
| Open cable bay | R1 cutter X64.5-81, Y-1-28.65 | Review midframe stiffness and battery retention; approximately 91.3% of battery footprint remains covered. |
| Captive power slider | 0.3 mm Y guide gaps; 0.2 mm nominal gap above/below flange | Qualify free travel and preserve gaps after finishing. |
| Power-slider roof and retainer | 1.2 mm roof; 1.2 mm shelf; 5 mm bridge | Review resin strength, bridge loads and anchor screw retention. |
| Power-slider socket | 2 mm square with 2.4 mm entry | Confirm actual handle height/fit and complete switch travel; keep coating and adhesive out. |

JLC's SLA wall table specifies 1.0 mm for 50 × 50 mm regions and 1.5 mm for 100 × 100 mm regions; positioning and fastening features are recommended above 1.5 mm. Its minimum nominal SLA assembly/movement gaps are 0.2/0.5 mm. Oil-sprayed holes should be at least Ø2 mm. Published SLA part/hole tolerances are ±0.2/±0.3 mm, and the dimensional tolerance excludes coating and deformation. These rules make the thin features and close fits above unresolved rather than automatically acceptable. [JLC design guideline](https://jlc3dp.com/help/article/3d-printing-design-guideline)

The HRO K3-1235S-F1 drawing specifies a 2 mm slider handle height, while the board BOM states 2.5 mm. The installed height, socket engagement and slider feel remain unverified. The guide permits ±0.3 mm lateral motion while the 2 mm socket around a nominal 1.5 mm handle permits only ±0.25 mm before contact. The retainer provides the intended downward load path, not complete lateral isolation of the switch. [HRO switch drawing](https://static.chipdip.ru/lib/844/DOC012844376.pdf)

The lower-seat review also found 0.425 mm horizontal clearance to the BOOT
mask pad (0.451 mm Euclidean), 0.35 mm to
an encoder pad and 0.63 mm radial wheel clearance. The nearby nonplated USB
locating hole is at (69.72,35.39), drill Ø0.6 with Ø0.7 mask opening. These
plan clearances do not establish installed fit or board contact pressure. The
0.2 mm anti-lift gap can be consumed by tolerances; do not draw a binding lid
onto the PCB with the case screws.

## Inserts and clear finish

The model has **six Ø2.7 × 3.4 mm provisional insert pilots**, four in the lid and two in base posts, intended for the user's **M2 inserts, Ø3 × 3.2 mm**, with M2 × 8 mm nylon screws. These are heat-set pilot dimensions, not approved adhesive-installation pockets. Select the supplier's actual insert first, then adapt its pocket, supporting material, insertion access and screw-tip clearance.

JLC's standard installed-insert service lists only M3/M4/M5 and excludes custom specifications. It requires 3 mm minimum insert-hole wall thickness, a flat installation area of at least 14 × 14 mm, and a PNG/PDF drawing marking insert types and locations. The smallest listed candidate is M3 × 4 × 3, using a Ø3.6 × 5 mm hole. Adopting this service requires substantial redesign; enlarging the current pilots alone is insufficient. [JLC insert service](https://jlc3dp.com/help/article/threaded-insert-service)

Transparent 8001 includes sanding and oil spraying, but complex parts may retain haze, bubbles or texture; complete optical clarity is not promised. Its published heat-deflection temperature is 53°C. Clear SLA resin needs an approved bonded/press-fit insert approach, not an assumed thermoplastic heat-setting process. [8001 specification](https://jlc3dp.com/help/article/photosensitive-8001-resin), [JLC insert/material guidance](https://jlc3dp.com/blog/threaded-inserts-3d-printing)

The user prefers a clear resin case. Exterior clear gloss is acceptable, but the
slider running slot, flange pocket, retainer guide and handle socket must be
kept free of coating buildup. Do not bond the moving cap. Its overlapping flange
covers the opening but is not a gasket and has no waterproof/IP-rating claim.

## Alternative providers and cost status

**PCBWay: quotation candidate.** Its 3D-print ordering instructions offer inserts with a 2D drawing, but M2 availability, hardware dimensions and installation in clear resin remain unconfirmed. UTR-8100 Transparent with spray varnish is a documented finish; complex interiors may remain less clear. [Insert ordering](https://www.pcbway.com/helpcenter/3d_ordering/How_do_I_place_3D_printing_order_.html), [UTR-8100](https://www.pcbway.com/rapid-prototyping//3d-printing/plastic/resin/UTR-8100/). Its FAQ publishes a **US$25 minimum order value excluding shipping**. This is not a price for this five-part enclosure, finishing or six installed inserts. [PCBWay pricing FAQ](https://www.pcbway.com/rapid-prototyping/cnc-faq.html)

**Protolabs: technically supported, rejected on cost.** Its SLA insert table explicitly includes M2 × 0.4, installed using screw-to-expand inserts and epoxy. Insert OD/length and suitable boss/pocket dimensions still need confirmation. WaterShed XC 11122 with Custom Clear Finish is documented. [M2 SLA inserts](https://www.protolabs.com/resources/blog/threading-and-inserts-for-3d-printing/), [clear WaterShed finish](https://www.protolabs.com/services/3d-printing/plastic/abs/watershed-xc-11122/). Published 3D-printing prices start around **US$95**, not a quote for our parts. The user declined pursuing this price level. [Protolabs pricing](https://www.protolabs.com/help-center/pricing-and-payment-options/)

## Quotation brief

Request one set of five separate parts: base, lid, midframe, button strip with retaining shelf, and power slider, in millimetres. Request smooth transparent exterior shells with polished/custom clear finish, and an itemized alternative using a tougher material for the button strip/retainer and moving slider. Keep their functional guiding and socket surfaces finish-free. Include **six factory-installed M2 × 0.4 brass inserts: four lid, two base**, compatible with existing M2 × 8 mm screws.

Ask the supplier to identify the insert part number, OD/length, required pocket/boss geometry and resin-compatible installation method; review the thin regions and post-finish clearances above; and price printing, finish, inserts/installation and shipping separately. Require confirmation of realistic clarity, visible support/coating marks and any proposed geometry changes before releasing manufacturing files. Prepare a dimensioned insert-location drawing after the hardware specification is agreed.
