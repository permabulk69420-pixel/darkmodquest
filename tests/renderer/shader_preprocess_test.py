#!/usr/bin/env python3
"""Check shipping shader variants through the production preprocessing functions."""
import unittest
from shipping_shader_source import source

class ShippingShaderVariants(unittest.TestCase):
    def test_multiview_define_is_resolved_for_every_shipping_variant(self):
        paths=['stages/depth/depth.vert.glsl','stages/mobile/forward.vert.glsl',
            'stages/mobile/forward.frag.glsl',*(f'stages/mobile/forward_{n}.frag.glsl' for n in (1,2,4))]
        for path in paths:
            with self.subTest(path=path):
                stereo=source(path,{'QUEST_MULTIVIEW':'1'})
                mono=source(path)
                self.assertTrue('#define QUEST_MULTIVIEW 1\n' in stereo,f'{path}: engine did not enable the requested multiview define')
                self.assertFalse('#define QUEST_MULTIVIEW' in mono,f'{path}: ordinary shader unexpectedly enables multiview')
                self.assertTrue(stereo.startswith('#version 320 es\n'))
                self.assertNotIn('#pragma tdm_',stereo)
                self.assertNotIn('#pragma tdm_',mono)

if __name__=='__main__':unittest.main(verbosity=2)
