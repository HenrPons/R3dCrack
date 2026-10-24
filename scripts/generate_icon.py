"""Render the original RedCrack vector mark as a multi-resolution Windows icon.
Requires Pillow. No downloaded artwork or fonts.
"""
from pathlib import Path
from PIL import Image, ImageDraw
root = Path(__file__).resolve().parents[1]
assets = root / 'assets'
assets.mkdir(exist_ok=True)
# Angular R split by a dark diagonal fissure; readable even at taskbar sizes.
polygons = [([(66,55),(153,55),(191,86),(191,123),(153,153),(110,153),(110,205),(66,205)], '#ff5b69'),
            ([(125,143),(158,139),(201,205),(153,205)], '#db2947'),
            ([(110,88),(144,88),(157,99),(157,112),(143,123),(110,123)], '#0c1018'),
            ([(161,41),(130,113),(151,113),(103,219),(123,139),(101,139)], '#0c1018')]
svg=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">', '<rect x="8" y="8" width="240" height="240" rx="50" fill="#0c1018"/>']
for points,color in polygons: svg.append('<polygon points="'+ ' '.join(f'{x},{y}' for x,y in points)+'" fill="'+color+'"/>')
svg.append('</svg>')
(assets/'redcrack.svg').write_text('\n'.join(svg))
im=Image.new('RGBA',(1024,1024)); d=ImageDraw.Draw(im)
d.rounded_rectangle((32,32,992,992),radius=200,fill='#0c1018')
for points,color in polygons: d.polygon([(x*4,y*4) for x,y in points],fill=color)
im=im.resize((256,256),Image.Resampling.LANCZOS)
im.save(assets/'redcrack.png')
im.save(assets/'redcrack.ico',sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])
