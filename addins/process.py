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


xml_path = '../train/box/'
img_path = '../train/image/'

err_xml_path = '../err/err_box/'
err_img_path = '../err/err_img/'

xmlset = os.listdir(xml_path)
imgset = os.listdir(img_path)

for xml in xmlset:

    tree = ET.parse(xml_path + xml)
    root = tree.getroot()
    annotation = []
    amount = 0
    for obj in root.findall('object'):
        name = obj.find('name').text
        if name == 'waterweeds':
            continue
        else:
            amount += 1
    if not amount:
        shutil.move(xml_path + xml, err_xml_path + xml)
        shutil.move(img_path + xml.split('.')[0] + '.jpg', err_img_path + xml.split('.')[0] + '.jpg')




print('Done!')
