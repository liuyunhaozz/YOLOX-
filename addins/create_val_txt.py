import os.path as osp
import xml.etree.ElementTree as ET

from glob import glob
from tqdm import tqdm
from PIL import Image
import os

import shutil

label_ids = {
    "holothurian": 1,
    "echinus": 2,
    "scallop": 3,
    "starfish": 4
    
}


xml_path = '../datasets/eus/Annotations/'
xmlset = os.listdir(xml_path)

with open('val.txt', 'w') as f:
    for i, xml in enumerate(xmlset):
        if i < 1500:
            name = xml.split('.')[0] + '\n'
            f.write(name)

print('Done!')