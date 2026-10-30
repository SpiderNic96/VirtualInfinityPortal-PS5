Real artwork for the TV picker and the phone remote.

Name:    the 7-digit figure id first (from tools/figures.tsv), anything after it is ignored:
           1000001.png   1000001_mr-incredible.png   p3000003.png
Format:  PNG with a transparent background (WebP / JPEG are converted, but have no transparency).
Size:    anything from about 128 px up; 512 x 512 or larger is ideal. Any shape: nothing is stretched.

What the build does with each file (tools/build_data.py, every build, or --check-art to only check):
  - trims the transparent margin around the render
  - scales it to fit its box and centres it: characters get a small margin,
    round discs fill the circle, play sets and hex discs fill the hexagon
  - a disc or hexagon without a transparent background is cut to its circle / hexagon
  - a character without a transparent background is used but flagged (it shows as a square)
  - writes it as web/ui/p<id>.png, exactly 160 x 160 RGBA
Problems (bad name, unknown id, duplicate, unreadable) are listed in data/dimod/art_report.txt.

Fetched from the wiki? Tidy it first: python3 tools/clean_art.py --reject <ids of wrong pictures>
(crops in-game disc cards to the disc, clears plain backdrops, sets wrong pictures aside).
