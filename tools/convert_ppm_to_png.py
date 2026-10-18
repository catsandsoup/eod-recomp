import sys
import os
from PIL import Image

def convert_dir(dir_path):
    if not os.path.isdir(dir_path):
        print(f"Directory not found: {dir_path}")
        return
    count = 0
    for fn in sorted(os.listdir(dir_path)):
        if fn.endswith('.ppm'):
            ppm_path = os.path.join(dir_path, fn)
            png_name = fn[:-4] + '.png'
            png_path = os.path.join(dir_path, png_name)
            try:
                with Image.open(ppm_path) as img:
                    img.save(png_path, format='PNG')
                count += 1
            except Exception as e:
                print(f"Error converting {ppm_path}: {e}")
    print(f"Converted {count} PPM images in {dir_path}")

if __name__ == '__main__':
    for d in sys.argv[1:]:
        convert_dir(d)
