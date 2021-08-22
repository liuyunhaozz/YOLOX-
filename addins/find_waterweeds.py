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


for xml in xmlset:

    tree = ET.parse(xml_path + xml)
    root = tree.getroot()

    for obj in root.findall('object'):
        name = obj.find('name').text
        if name == 'waterweeds':
            root.remove(obj)

    tree.write('Annotations/' + xml)


print('Done!')
