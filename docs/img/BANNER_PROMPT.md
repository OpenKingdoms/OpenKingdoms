# Banner — image-generation prompt

`banner.svg` is the hand-drawn placeholder. For a painted version, feed the
prompt below to an image model (Nano Banana / Gemini, Midjourney, etc.),
then export at 2400×640 and save as `banner.png`. Point the README at the
PNG when it lands.

Keep the result **original**: no Cavedog/Atari logo, no box art, no
recognisable units or characters from the game. The banner ships in the
repo and must be ours.

---

## Prompt

> A wide horizontal banner (aspect ratio 15:4) in the style of a
> **Carolingian illuminated manuscript page**, c. 800 AD, in the manner of
> the Godescalc Evangelistary and the Codex Aureus of St. Emmeram.
>
> The ground is aged purple-dyed vellum (deep Tyrian purple, slightly
> mottled) inside a border of cream parchment. The border is a thick band
> of **gold-leaf interlace knotwork** with red and green ribbons weaving
> over and under, framed by fine gold rules, with a **round medallion in
> each corner** holding a four-petal rosette.
>
> Centred on the purple field, the word **"OpenTAK"** in large
> **Carolingian uncial / capitalis lettering in burnished gold leaf**, with
> subtle tooled texture and a fine dark outline, as if written in gold ink
> on purple parchment. Beneath it a thin gold rule with a small red dot at
> the centre.
>
> To the left of the field, a square **illuminated initial panel**: a
> Solomon's-knot interlace in gold and vermilion on purple, with a small
> green centre stone. Acanthus-leaf scrolls in green and gold curl in the
> four corners of the purple field.
>
> Along the bottom of the border, four small **heraldic roundels** in
> royal blue, crimson, teal-green, and earthen brown, each rimmed in gold —
> abstract, no figures.
>
> Flat, hieratic, two-dimensional composition; no perspective, no
> photorealism, no 3D bevels. Muted, aged pigments: gold leaf, Tyrian
> purple, vermilion, verdigris green, ochre, lapis blue. Fine hairline
> pen-work visible in the gold. Even lighting like a museum scan of a
> manuscript folio, slight parchment grain, very faint gilding crackle.
>
> **No** text other than "OpenTAK". No dragons, no knights, no castles, no
> game characters, no modern logos, no watermark.

## Negative prompt (if the tool takes one)

> photorealistic, 3D render, bevel, lens flare, neon, fantasy game art,
> video game logo, box art, characters, faces, dragons, castles, extra
> text, watermark, signature, blurry, low contrast

## Variations worth trying

- "…the field is **cream vellum** and the lettering is **purple and gold**"
  (Lorsch Gospels palette) — lighter, reads better on light GitHub themes.
- Add "in the border, tiny **gold Carolingian minuscule** filler text,
  illegible" for texture — but check it doesn't hallucinate real words.
- Ask for the interlace as a **separate tileable strip** if you want to
  rebuild the SVG border with a painted texture.

## Checks before committing a generated image

- Nothing in it resembles the game's logo, unit art, or box art.
- The only legible text is "OpenTAK".
- Save the prompt and model used here, so provenance is recorded.
