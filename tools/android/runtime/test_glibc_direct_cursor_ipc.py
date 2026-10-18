"""Direct Android Mesa frames must not block on unused X11 cursor IPC."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[3]
PATCH=(ROOT/"tools/android/mesa-patches/0007-wsi-x11-sw-frames-straight-to-the-app.patch").read_text()
RUN=(ROOT/"tools/android/device/run-iss.sh").read_text()
PATCH2=(ROOT/"tools/android/mesa-patches/0008-wsi-x11-sw-present-direct-even-with-mit-shm.patch").read_text()

class DirectPresentCursorTests(unittest.TestCase):
    def test_cursor_roundtrips_are_bypassed_on_android(self):
        self.assertIn("export ISS_HIDE_X11_CURSOR=1",RUN)
        self.assertIn('const bool show_cursor = !os_get_option("ISS_HIDE_X11_CURSOR")',PATCH)
        self.assertIn('if (show_cursor) cursor_cookie = xcb_xfixes_get_cursor_image',PATCH)
        self.assertIn('show_cursor ? xcb_xfixes_get_cursor_image_reply',PATCH)
        self.assertIn('show_cursor && d->root ? xcb_translate_coordinates_reply',PATCH)
        self.assertIn('if (show_cursor) xcb_discard_reply',PATCH)
        self.assertNotIn('+   xcb_flush(chain->conn);',PATCH)
    def test_completed_frame_protocol_and_mit_shm_override_remain(self):
        self.assertIn('memcpy(frame, image->base.cpu_map',PATCH)
        self.assertIn('atomic_store(&sh->published, slot)',PATCH)
        self.assertIn('FXPRESENT_SLOTS 3',PATCH)
        self.assertIn('!chain->has_mit_shm || chain->fxpresent',PATCH2)

if __name__=="__main__":
    unittest.main()
