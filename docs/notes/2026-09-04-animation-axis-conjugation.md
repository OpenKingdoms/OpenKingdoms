# Animation axis conjugation fix (2026-09-04)

Symptom: body parts animate in wrong directions. Archer "fires backwards" (attack1 arm
swings land behind the unit). Static poses fine.

Root cause: **3DO/COB model frame is LEFT-handed** (+x=model left, +y=up, +z=back, proven
from arasword/arabow 3do offsets: LegUL +x / LegUR −x, capes +z). Our model→world map
correctly carries det=−1 (why statics look right), but compose_node_xforms built
RIGHT-handed Euler matrices. Rotations are pseudovectors, conjugating as
R(A·n, det(A)·θ) = R(A·n, −θ) → **every TURN/SPIN played with inverted sense on all axes**.
Translations are true vectors → MOVE signs were fine.

Oracle: tools/tauniverse Scriptor BOS sources (arasword walk/attack1): knee <+41.13°> must
flex shin backward, elbow <−88.95°> forearm forward, ArmUR z <+23.55> abducts outward,
Torso y <−12.3> winds right. All four said "negate".

Fixes (units.c):
1. ANGLE_TO_RAD negated (one token, since both live + ghost paths go through
   compose_node_xforms).
2. AimWeapon heading arg → CCW delta (hdg − aim), signed shortest-way emission
   (((d + 0x8000) & 0xffff) − 0x8000). Scripts (arassh) do signed math on it. MUST land
   with fix 1: previously CW arg cancelled the inverted matrix, and an independent change
   flips turrets 2×Δ.
3. XZ_ATAN port (GET 12): (hdg − ang) & 0xffff.
4. Bonus: COB_POS_TO_MODEL was 1/65536, 65536× too small. COB linear values share the
   raw 3DO fixed space (move [-0.6] → −98304, Hip y=1894644). Scale = 1.0. All MOVE piece
   animations were invisible before this.

AimWeapon note: arabow's AimWeapon has NO turn opcodes (signal/wait/SetMaxReloadTime
pattern). The visual comes from attack1 arm turns. The ±0x8000 seen in some scripts
(araat, verharp...) is a per-model 180° rest-pose correction, NOT an engine convention.

Open (separate dig): Rz·Ry·Rx composition order vs legacy for multi-axis pieces.

Update 2026-09-10: the MOVE claim above is wrong for x and z, and the order is
settled. See 2026-09-10-piece-translation-axes.md.

Validation: unit-test table on compose_node_xforms (rot[0]=7488 → child z>0, etc.), ARAAT
turret east-aim regression, render_probe walker knee-fold check. See the agent report in
git history for the table.
