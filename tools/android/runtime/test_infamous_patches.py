"""Synthetic G3PROBE5 / inFAMOUS XML validation."""
from pathlib import Path
import sys, struct, tempfile, unittest, xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'port/scripts'))
from iss_mask_patches import compile_masks,mask_tokens
from patches import game_profile,selected_patches,profile_version_matches
from unittest.mock import patch
XML=ROOT/'port/patches/InfamousSecondSon.xml'

def fixture(folder,patterns,merge_contained=False):
    code=bytearray(b'\xcc'*40)
    if merge_contained:
        patterns=sorted(patterns,key=lambda x:len(mask_tokens(x)),reverse=True)
    for item in patterns:
        payload=bytes(0x90 if x is None else x for x in mask_tokens(item))
        if merge_contained and payload in code:
            continue
        code.extend(b'\xcc'*19)
        code.extend(payload)
    code.extend(b'\xcc'*40)
    elf=bytearray(512)
    struct.pack_into('<Q',elf,0x20,64)
    struct.pack_into('<HH',elf,0x36,56,1)
    struct.pack_into('<IIQQQQQQ',elf,64,1,5,0,0,0,len(code),len(code),4096)
    (folder/'eboot.elf').write_bytes(elf)
    hdr=struct.pack('<8s6Q',b'G3PROBE5',len(code),0,1,0,0,0)
    hdr+=struct.pack('<Q',0)+struct.pack('<4Q',0,0,0,0)
    hdr+=struct.pack('<Q',0)+struct.pack('<Q',0)+struct.pack('<3Q',0,len(code),5)
    (folder/'boot-linked.bin').write_bytes(hdr+code)

class InfamousPatchTests(unittest.TestCase):
    def test_catalog_and_conflicts(self):
        self.assertEqual(game_profile('CUSA00004')[2],'01.00')
        allpatch=ET.parse(XML).getroot().findall('Metadata')
        self.assertEqual(len(allpatch),9)
        self.assertNotIn('Disable light beams',{x.get('Name') for x in allpatch})
        self.assertTrue(all(x.get('AppVer')=='01.00' for x in allpatch))
        self.assertEqual(selected_patches(XML,'01.00',''),[])
        with self.assertRaisesRegex(ValueError,'choose one resolution'):
            selected_patches(XML,'01.00','Render resolution 1280x720;Render resolution 1600x900')
    def test_fsr_only_native_smaa_bypass_patch(self):
        root = ET.parse(XML).getroot()
        entries = [m for m in root.findall('Metadata')
                   if m.get('Name') == 'Disable native SMAA for FSR']
        self.assertEqual(len(entries), 1)
        entry = entries[0]
        self.assertEqual(entry.get('AppVer'), '01.00')
        self.assertEqual(entry.get('isEnabled'), 'false')
        lines = entry.findall('.//Line')
        self.assertEqual(len(lines), 2)
        self.assertEqual([x.get('Offset') for x in lines], ['13', '0'])
        self.assertEqual([x.get('Value') for x in lines], ['01', 'C6874502000001'])
        # In the original x86-64 constructor the 0x245 byte defaults to zero;
        # force one, and force the property setter to store one as well.
        original = bytes.fromhex('C6 87 44 02 00 00 00 C6 87 45 02 00 00 00 '
                                 'C7 87 48 02 00 00 40 88 B7 45 02 00 00 '
                                 '80 8F F7 00 00 00 08 C3')
        patched = bytearray(original)
        for line in lines:
            mask = bytes(mask_tokens(line.get('Address')))
            self.assertEqual(original.count(mask), 1)
            offset = original.index(mask) + int(line.get('Offset'), 0)
            data = bytes.fromhex(line.get('Value'))
            patched[offset:offset+len(data)] = data
        self.assertEqual(patched[7:14], bytes.fromhex('C6874502000001'))
        self.assertEqual(patched[20:27], bytes.fromhex('C6874502000001'))
        launcher = (ROOT/'android/app/src/ro/cobrabm/fexdroid/BionicGameActivity.java').read_text()
        self.assertIn('if(fsr3)patches.add("Disable native SMAA for FSR")', launcher)
        self.assertNotIn('ISS_FSR_PRE_TAA_720', launcher)

    def test_regions_and_verified_executable_version_gate(self):
        expected=game_profile('CUSA00004')
        for title in ('CUSA00223','CUSA00263'):
            self.assertEqual(game_profile(title),expected)
        self.assertIsNone(game_profile('CUSA01624'))
        with patch('patches.game_app_version',return_value='01.07'),patch('patches.eboot_sha256',return_value='2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab'):
            self.assertTrue(profile_version_matches('game',expected))
        with patch('patches.game_app_version',return_value='01.07'),patch('patches.eboot_sha256',return_value='different executable'):
            self.assertFalse(profile_version_matches('game',expected))
    def test_compile_all_catalog_signatures(self):
        for item in ET.parse(XML).getroot().findall('Metadata'):
            sigs=[x.get('Address') for x in item.findall('.//Line')]
            with self.subTest(item.get('Name')),tempfile.TemporaryDirectory() as d:
                path=Path(d);fixture(path,sigs,merge_contained=True)
                catalog=ET.parse(XML)
                # Synthetic fixtures test signatures independently of the real ELF gate.
                for meta in catalog.getroot().findall('Metadata'):meta.attrib.pop('ElfSHA256',None)
                test_xml=path/'catalog.xml';catalog.write(test_xml)
                writes=compile_masks(test_xml,[item.get('Name')],'01.00',path/'boot-linked.bin',path/'eboot.elf')
                self.assertEqual(len(writes),len(sigs))
    def test_render_patches_reject_an_unverified_executable(self):
        for name in ('Disable shadow maps',):
            item=next(x for x in ET.parse(XML).getroot().findall('Metadata') if x.get('Name')==name)
            self.assertEqual(len(item.get('ElfSHA256')),64)
            self.assertEqual(item.get('AllowedEbootSHA256'),'2d1ca79630d7bbe6fa29575f59aa7996d43071d74ac8e4f7d40137967d9039ab')
            with tempfile.TemporaryDirectory() as d:
                path=Path(d);fixture(path,[x.get('Address') for x in item.findall('.//Line')])
                with self.assertRaisesRegex(ValueError,'ELF hash mismatch'):
                    compile_masks(XML,[name],'01.00',path/'boot-linked.bin',path/'eboot.elf')

    def test_verified_regional_shadow_source_only(self):
        item=next(x for x in ET.parse(XML).getroot().findall('Metadata')
                  if x.get('Name')=='Disable shadow maps')
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)
            fixture(path,[x.get('Address') for x in item.findall('.//Line')])
            original=path/'eboot.bin'
            original.write_bytes(b'regional-source-fixture')
            # The converted ELF digest differs across SELF conversion. The
            # original signed SELF must independently match the pinned hash.
            from unittest.mock import Mock
            verified_source=item.get('AllowedEbootSHA256')
            with patch('iss_mask_patches.hashlib.sha256',
                       side_effect=[Mock(hexdigest=lambda:'other-elf-hash'),
                                    Mock(hexdigest=lambda:verified_source)]):
                writes=compile_masks(XML,['Disable shadow maps'],'01.00',
                                     path/'boot-linked.bin',path/'eboot.elf',original)
                self.assertEqual(len(writes),2)
            with patch('iss_mask_patches.hashlib.sha256',
                       side_effect=[Mock(hexdigest=lambda:'other-elf-hash'),
                                    Mock(hexdigest=lambda:'wrong-source')]):
                with self.assertRaisesRegex(ValueError,'ELF hash mismatch'):
                    compile_masks(XML,['Disable shadow maps'],'01.00',
                                  path/'boot-linked.bin',path/'eboot.elf',original)

    def test_fail_closed_when_missing_ambiguous_or_wrong_version(self):
        item=next(x for x in ET.parse(XML).getroot().findall('Metadata') if x.get('Name')=='Disable motion blur')
        sigs=[x.get('Address') for x in item.findall('.//Line')]
        with tempfile.TemporaryDirectory() as d:
            path=Path(d);fixture(path,sigs[:1])
            with self.assertRaisesRegex(ValueError,'0 matches'):
                compile_masks(XML,[item.get('Name')],'01.00',path/'boot-linked.bin',path/'eboot.elf')
            fixture(path,sigs+sigs)
            with self.assertRaisesRegex(ValueError,'2 matches'):
                compile_masks(XML,[item.get('Name')],'01.00',path/'boot-linked.bin',path/'eboot.elf')
            with self.assertRaisesRegex(ValueError,'missing'):
                compile_masks(XML,[item.get('Name')],'01.07',path/'boot-linked.bin',path/'eboot.elf')
if __name__=='__main__': unittest.main()
