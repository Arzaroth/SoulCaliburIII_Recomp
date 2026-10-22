import sys, glob, os, re
from PIL import Image
d, out = sys.argv[1], sys.argv[2]
files = sorted(glob.glob(os.path.join(d, 'frame_*.rgba')), key=lambda f: int(re.search(r'frame_(\d+)_', f).group(1)))
sel = files[-int(sys.argv[3]):] if len(sys.argv) > 3 else files
ims = []
for f in sel:
    w, h = map(int, re.search(r'_(\d+)x(\d+)\.rgba', f).groups())
    im = Image.frombytes('RGBA', (w, h), open(f, 'rb').read()).convert('RGB').resize((w // 2, h // 2))
    ims.append(im)
cols = 3
rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (ims[0].width * cols, ims[0].height * rows))
for i, im in enumerate(ims):
    sheet.paste(im, ((i % cols) * im.width, (i // cols) * im.height))
sheet.save(out)
