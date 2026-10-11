"""Probe identity, hardware classification and cleanup tests without a GPU."""
import ctypes as c
import importlib.util
import os
from pathlib import Path
import stat
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('rt', Path(__file__).resolve().parents[2] / 'src/moonlight-server/gpu/ray-tracing-probe.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)
BDF = '0000:03:00.0'

class FakeVulkan:
    features = True
    matched = True
    vendor = 0x1002
    backend = 3
    calls = 0
    closed = False
    def __init__(self):
        type(self).calls += 1
    def devices(self): return [1]
    def extensions(self, device): return {'VK_EXT_pci_bus_info', 'VK_EXT_physical_device_drm'}
    def properties(self, device, extensions):
        props = rt.Props2()
        props.properties.kind, props.properties.vendor = 1, self.vendor
        props.properties.api = (1 << 22) | (2 << 12)
        pci = rt.PCI(domain=0, bus=3 if self.matched else 4, device=0, function=0)
        drm = rt.DRM(hasRender=1, renderMajor=226, renderMinor=131)
        return props, pci, drm, rt.Driver(id=self.backend)
    def verify_features(self, device, extensions): return self.features
    def close(self): type(self).closed = True

class ProbeTests(unittest.TestCase):
    def setUp(self):
        FakeVulkan.features = FakeVulkan.matched = True
        FakeVulkan.vendor, FakeVulkan.backend = 0x1002, 3
        FakeVulkan.calls, FakeVulkan.closed = 0, False
        self.patches = [patch.object(rt.os, 'stat', return_value=SimpleNamespace(st_mode=stat.S_IFCHR, st_rdev=os.makedev(226, 131))),
                        patch.object(rt.Path, 'resolve', lambda p: Path('amdgpu' if p.name == 'driver' else BDF)),
                        patch.object(rt, 'Vulkan', FakeVulkan), patch.dict(os.environ, {}, clear=True)]
        for p in self.patches: p.start()
    def tearDown(self):
        for p in reversed(self.patches): p.stop()
    def test_real_hardware_and_usable_driver_are_required(self):
        with patch.object(rt, 'amd_hardware', return_value=True):
            self.assertEqual(rt.probe('/dev/dri/renderD131', BDF, 'amdgpu')[0], True)
        self.assertTrue(FakeVulkan.closed)
    def test_emulated_extensions_cannot_certify_old_hardware(self):
        with patch.object(rt, 'amd_hardware', return_value=False):
            self.assertEqual(rt.probe('/dev/dri/renderD131', BDF, 'amdgpu')[0], False)
        self.assertEqual(FakeVulkan.calls, 0)
    def test_unknown_hardware_never_becomes_supported_from_api_bits(self):
        with patch.object(rt, 'amd_hardware', return_value=None):
            self.assertIsNone(rt.probe('/dev/dri/renderD131', BDF, 'amdgpu')[0])
    def test_forced_emulation_is_not_certified(self):
        for key in ('RADV_PERFTEST', 'RADV_DEBUG', 'RADV_EXPERIMENTAL'):
            with patch.object(rt, 'amd_hardware', return_value=True), patch.dict(os.environ, {key: 'emulate_rt'}):
                self.assertIsNone(rt.probe('/dev/dri/renderD131', BDF, 'amdgpu')[0])
        self.assertEqual(FakeVulkan.calls, 0)
    def test_matching_render_node_cannot_override_wrong_pci_identity(self):
        FakeVulkan.matched = False
        with patch.object(rt, 'amd_hardware', return_value=True):
            self.assertIsNone(rt.probe('/dev/dri/renderD131', BDF, 'amdgpu')[0])
        self.assertTrue(FakeVulkan.closed)
    def test_broken_vulkan_support_is_not_usable(self):
        FakeVulkan.features = False
        with patch.object(rt, 'amd_hardware', return_value=True):
            self.assertEqual(rt.probe('/dev/dri/renderD131', BDF, 'amdgpu')[0], False)
    def test_software_device_cannot_match_physical_gpu(self):
        props, pci, drm, _ = FakeVulkan().properties(1, {})
        props.properties.kind = 4
        self.assertFalse(rt.matches_device(props, pci, drm, FakeVulkan().extensions(1), BDF, os.makedev(226, 131)))

class Function:
    def __init__(self, call): self.call = call
    def __call__(self, *args): return self.call(*args)

class FeatureVulkan(rt.Vulkan):
    def __init__(self):
        self.query, self.pipeline, self.acceleration, self.address = True, False, True, True
        self.enabled_extensions = set()
        self.create_status = 0
    def call(self, name, result, args, *values):
        if name == 'vkGetPhysicalDeviceFeatures2':
            address = values[1]._obj.pNext
            while address:
                head = c.cast(address, c.POINTER(rt.Address)).contents
                supported = {1000257000: self.address, 1000150013: self.acceleration,
                             1000348013: self.query, 1000347000: self.pipeline}
                head.enabled = int(supported[head.sType])
                address = head.pNext
            return None
        if name == 'vkGetPhysicalDeviceQueueFamilyProperties':
            values[1]._obj.value = 1
            if values[2] is not None:
                values[2][0].flags, values[2][0].count = 1, 1
            return None
        if name == 'vkCreateDevice':
            info = values[1]._obj
            encoded = c.cast(info.extensions, c.POINTER(c.c_char_p))
            self.enabled_extensions = {encoded[i].decode() for i in range(info.extensionCount)}
            values[3]._obj.value = 1
            return self.create_status
        if name == 'vkDestroyDevice': return None
        raise AssertionError(name)

class FeatureTests(unittest.TestCase):
    def setUp(self):
        self.vulkan = FeatureVulkan()
        self.extensions = {'VK_KHR_acceleration_structure', 'VK_KHR_deferred_host_operations',
                           'VK_KHR_ray_query', 'VK_KHR_ray_tracing_pipeline'}
    def test_ray_query_can_work_without_pipeline_support(self):
        self.assertTrue(self.vulkan.verify_features(1, self.extensions))
        self.assertIn('VK_KHR_ray_query', self.vulkan.enabled_extensions)
        self.assertNotIn('VK_KHR_ray_tracing_pipeline', self.vulkan.enabled_extensions)
    def test_pipeline_can_work_without_ray_query(self):
        self.vulkan.query, self.vulkan.pipeline = False, True
        self.assertTrue(self.vulkan.verify_features(1, self.extensions))
        self.assertIn('VK_KHR_ray_tracing_pipeline', self.vulkan.enabled_extensions)
    def test_extension_names_without_feature_bits_do_not_pass(self):
        self.vulkan.query = False
        self.assertFalse(self.vulkan.verify_features(1, self.extensions))
        self.assertFalse(self.vulkan.enabled_extensions)
    def test_acceleration_and_buffer_address_are_required(self):
        self.vulkan.address = False
        self.assertFalse(self.vulkan.verify_features(1, self.extensions))
        self.assertFalse(self.vulkan.enabled_extensions)
    def test_device_creation_failure_is_not_certified(self):
        self.vulkan.create_status = -8
        with self.assertRaises(RuntimeError): self.vulkan.verify_features(1, self.extensions)

class NvidiaTests(unittest.TestCase):
    def test_optix_rtcore_property_distinguishes_compute_capable_non_rt_cards(self):
        for rt_version in (0, 10):
            destroyed, released = [], []
            cuda = SimpleNamespace(cuInit=Function(lambda flags: 0),
                cuDeviceGetByPCIBusId=Function(lambda out, bdf: setattr(out._obj, 'value', 0) or 0),
                cuDeviceGetAttribute=Function(lambda out, key, device: setattr(out._obj, 'value', 7 if key == 75 else 5) or 0),
                cuDevicePrimaryCtxRetain=Function(lambda out, device: setattr(out._obj, 'value', 1) or 0),
                cuCtxPushCurrent_v2=Function(lambda context: 0),
                cuCtxPopCurrent_v2=Function(lambda out: setattr(out._obj, 'value', 1) or 0),
                cuDevicePrimaryCtxRelease_v2=Function(lambda device: released.append(1) or 0))
            create = c.CFUNCTYPE(c.c_int, rt.P, rt.P, rt.P)(lambda ctx, opts, out: setattr(c.cast(out, c.POINTER(rt.P)).contents, 'value', 2) or 0)
            destroy = c.CFUNCTYPE(c.c_int, rt.P)(lambda ctx: destroyed.append(1) or 0)
            prop = c.CFUNCTYPE(c.c_int, rt.P, c.c_int, rt.P, c.c_size_t)(lambda ctx, key, out, size: setattr(c.cast(out, c.POINTER(rt.U)).contents, 'value', rt_version) or 0)
            def table(abi, count, keys, values, out, size):
                if abi != 105: return 1
                self.assertEqual(size, 52 * c.sizeof(rt.P))
                for i, callback in ((2, create), (3, destroy), (4, prop)):
                    out[i] = c.cast(callback, rt.P).value
                return 0
            optix = SimpleNamespace(optixQueryFunctionTable=Function(table))
            with patch.object(rt.c, 'CDLL', side_effect=lambda name: cuda if name == 'libcuda.so.1' else optix):
                self.assertEqual(rt.nvidia_hardware(BDF), rt_version > 0)
            self.assertEqual(destroyed, [1])
            self.assertEqual(released, [1])

if __name__ == '__main__':
    unittest.main()
