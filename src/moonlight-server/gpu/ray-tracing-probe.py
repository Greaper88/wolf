#!/usr/bin/env python3
"""Optional, isolated hardware/Vulkan capability query; no rendering benchmark.

The server owns the deadline and caches this result. Native libraries are loaded
only in this child, and all queries bind to the requested physical render device.
No model-name/PCI blacklist or runtime SDK dependency is used.
"""
import ctypes as c
import os
from pathlib import Path
import stat
import sys

U = c.c_uint32
P = c.c_void_p

def vk_struct(name, fields):
    return type(name, (c.Structure,), {'_fields_': [('sType', U), ('pNext', P)] + fields})

App = vk_struct('App', [('name', c.c_char_p), ('version', U), ('engine', c.c_char_p), ('engineVersion', U), ('apiVersion', U)])
InstanceInfo = vk_struct('InstanceInfo', [('flags', U), ('app', P), ('layers', U), ('layerNames', P), ('extensions', U), ('extensionNames', P)])
PCI = vk_struct('PCI', [('domain', U), ('bus', U), ('device', U), ('function', U)])
DRM = vk_struct('DRM', [('hasPrimary', U), ('hasRender', U), ('primaryMajor', c.c_int64), ('primaryMinor', c.c_int64), ('renderMajor', c.c_int64), ('renderMinor', c.c_int64)])
Driver = vk_struct('Driver', [('id', U), ('name', c.c_char * 256), ('info', c.c_char * 256), ('conformance', c.c_ubyte * 4)])
# Only the stable property prefix is read; the opaque tail is larger than the
# remaining Vulkan 1.x core properties and preserves its native alignment.
class Properties(c.Structure):
    _fields_ = [('api', U), ('driver', U), ('vendor', U), ('device', U), ('kind', U), ('name', c.c_char * 256), ('opaque', c.c_uint64 * 256)]
Props2 = vk_struct('Props2', [('properties', Properties)])
Features2 = vk_struct('Features2', [('core', U * 64)])
Address = vk_struct('Address', [('enabled', U), ('capture', U), ('multiDevice', U)])
Acceleration = vk_struct('Acceleration', [('enabled', U), ('capture', U), ('indirect', U), ('host', U), ('update', U)])
Query = vk_struct('Query', [('enabled', U)])
Pipeline = vk_struct('Pipeline', [('enabled', U), ('capture', U), ('mixed', U), ('indirect', U), ('culling', U)])
QueueInfo = vk_struct('QueueInfo', [('flags', U), ('family', U), ('count', U), ('priorities', P)])
DeviceInfo = vk_struct('DeviceInfo', [('flags', U), ('queueCount', U), ('queues', P), ('layerCount', U), ('layers', P), ('extensionCount', U), ('extensions', P), ('features', P)])
class Extension(c.Structure):
    _fields_ = [('name', c.c_char * 256), ('version', U)]
class Queue(c.Structure):
    _fields_ = [('flags', U), ('count', U), ('timestampBits', U), ('granularity', U * 3)]

def pointer(value):
    return c.cast(c.pointer(value), P)

def fn(library, name, result, args):
    value = getattr(library, name)
    value.restype, value.argtypes = result, args
    return value

def check(result):
    if result != 0:
        raise RuntimeError('native-query-failed')

def chain(*values):
    for index, value in enumerate(values):
        value.pNext = pointer(values[index + 1]) if index + 1 < len(values) else None
    return pointer(values[0])

def amd_hardware(node):
    library = c.CDLL('libdrm_amdgpu.so.1')
    initialize = fn(library, 'amdgpu_device_initialize', c.c_int, [c.c_int, P, P, P])
    query = fn(library, 'amdgpu_query_hw_ip_info', c.c_int, [P, U, U, P])
    release = fn(library, 'amdgpu_device_deinitialize', c.c_int, [P])
    # drm_amdgpu_info_hw_ip's stable prefix and alignment, from libdrm's UAPI.
    class IP(c.Structure):
        _fields_ = [('major', U), ('minor', U), ('capabilities', c.c_uint64), ('startAlignment', U), ('sizeAlignment', U), ('rings', U), ('discoveryVersion', U)]
    device, major, minor = P(), U(), U()
    fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
    try:
        check(initialize(fd, c.byref(major), c.byref(minor), c.byref(device)))
        try:
            ip = IP()
            check(query(device, 0, 0, c.byref(ip)))  # AMDGPU_HW_IP_GFX
            if ip.major < 10 or (ip.major == 10 and ip.minor < 3):
                return False
            if ip.major == 10 and ip.minor == 3 or ip.major in (11, 12):
                return True  # RDNA2/3/4 graphics IP has ray accelerators.
            return None  # A future/unknown IP is not silently certified.
        finally:
            release(device)
    finally:
        os.close(fd)

def nvidia_hardware(bdf):
    cuda = c.CDLL('libcuda.so.1')
    check(fn(cuda, 'cuInit', c.c_int, [U])(0))
    device, major, minor = c.c_int(), c.c_int(), c.c_int()
    check(fn(cuda, 'cuDeviceGetByPCIBusId', c.c_int, [P, c.c_char_p])(c.byref(device), bdf.encode()))
    attribute = fn(cuda, 'cuDeviceGetAttribute', c.c_int, [P, c.c_int, c.c_int])
    check(attribute(c.byref(major), 75, device))
    check(attribute(c.byref(minor), 76, device))
    if (major.value, minor.value) < (7, 5):
        return False
    # Compute capability alone is insufficient: GTX 16xx and A100 lack RT cores.
    # Public OptiX ABI table sizes (7.4 through 9.1); only the stable first five
    # entries are used. No SDK headers or NVIDIA implementation are bundled.
    try:
        optix = c.CDLL('libnvoptix.so.1')
    except OSError:
        raise RuntimeError('optix-driver-library-unavailable') from None
    get_table = fn(optix, 'optixQueryFunctionTable', c.c_int, [c.c_int, U, P, P, P, c.c_size_t])
    table = None
    for abi, slots in ((118, 58), (105, 52), (93, 50), (87, 48), (84, 48), (68, 45), (60, 41), (55, 41)):
        candidate = (P * slots)()
        if get_table(abi, 0, None, None, candidate, c.sizeof(candidate)) == 0:
            table = candidate
            break
    if table is None or not all(table[i] for i in (2, 3, 4)):
        raise RuntimeError('optix-hardware-query-unavailable')
    create = c.CFUNCTYPE(c.c_int, P, P, P)(table[2])
    destroy = c.CFUNCTYPE(c.c_int, P)(table[3])
    property_query = c.CFUNCTYPE(c.c_int, P, c.c_int, P, c.c_size_t)(table[4])
    context, optix_context = P(), P()
    retain = fn(cuda, 'cuDevicePrimaryCtxRetain', c.c_int, [P, c.c_int])
    release = fn(cuda, 'cuDevicePrimaryCtxRelease_v2', c.c_int, [c.c_int])
    push = fn(cuda, 'cuCtxPushCurrent_v2', c.c_int, [P])
    pop = fn(cuda, 'cuCtxPopCurrent_v2', c.c_int, [P])
    class ContextOptions(c.Structure):
        _fields_ = [('logCallback', P), ('logData', P), ('logLevel', U), ('validationMode', U)]
    check(retain(c.byref(context), device))
    try:
        check(push(context))
        try:
            options = ContextOptions()
            check(create(context, c.byref(options), c.byref(optix_context)))
            try:
                version = U()
                check(property_query(optix_context, 0x2005, c.byref(version), c.sizeof(version)))
                return version.value > 0  # OPTIX_DEVICE_PROPERTY_RTCORE_VERSION
            finally:
                destroy(optix_context)
        finally:
            popped = P()
            pop(c.byref(popped))
    finally:
        release(device)

def matches_device(props, pci, drm, extensions, bdf, number):
    if props.properties.kind not in (1, 2, 3):
        return False
    identities = []
    if 'VK_EXT_physical_device_drm' in extensions and drm.hasRender:
        identities.append((drm.renderMajor, drm.renderMinor) == (os.major(number), os.minor(number)))
    if 'VK_EXT_pci_bus_info' in extensions:
        identities.append(f'{pci.domain:04x}:{pci.bus:02x}:{pci.device:02x}.{pci.function}' == bdf.lower())
    return bool(identities) and all(identities)

class Vulkan:
    def __init__(self):
        self.library = c.CDLL('libvulkan.so.1')
        self.instance = P()
        app = App(sType=0, name=b'wolf-rt-probe', apiVersion=(1 << 22) | (2 << 12))
        info = InstanceInfo(sType=1, app=pointer(app))
        check(self.call('vkCreateInstance', c.c_int, [P, P, P], c.byref(info), None, c.byref(self.instance)))

    def call(self, name, result, args, *values):
        return fn(self.library, name, result, args)(*values)

    def close(self):
        self.call('vkDestroyInstance', None, [P, P], self.instance, None)

    def devices(self):
        count = U()
        check(self.call('vkEnumeratePhysicalDevices', c.c_int, [P, P, P], self.instance, c.byref(count), None))
        if count.value > 64:
            raise RuntimeError('too-many-vulkan-devices')
        values = (P * count.value)()
        check(self.call('vkEnumeratePhysicalDevices', c.c_int, [P, P, P], self.instance, c.byref(count), values))
        return list(values)[:count.value]

    def extensions(self, device):
        count = U()
        check(self.call('vkEnumerateDeviceExtensionProperties', c.c_int, [P, c.c_char_p, P, P], device, None, c.byref(count), None))
        if count.value > 4096:
            raise RuntimeError('too-many-vulkan-extensions')
        values = (Extension * count.value)()
        check(self.call('vkEnumerateDeviceExtensionProperties', c.c_int, [P, c.c_char_p, P, P], device, None, c.byref(count), values))
        return {value.name.decode() for value in values[:count.value]}

    def properties(self, device, extensions):
        props, pci, drm, driver = Props2(sType=1000059001), PCI(sType=1000212000), DRM(sType=1000353000), Driver(sType=1000196000)
        values = [driver]
        if 'VK_EXT_pci_bus_info' in extensions:
            values.append(pci)
        if 'VK_EXT_physical_device_drm' in extensions:
            values.append(drm)
        props.pNext = chain(*values)
        self.call('vkGetPhysicalDeviceProperties2', None, [P, P], device, c.byref(props))
        return props, pci, drm, driver

    def verify_features(self, device, extensions):
        required = {'VK_KHR_acceleration_structure', 'VK_KHR_deferred_host_operations'}
        if not required <= extensions:
            return False
        acceleration, query, pipeline, address = Acceleration(sType=1000150013), Query(sType=1000348013), Pipeline(sType=1000347000), Address(sType=1000257000)
        values = [address, acceleration]
        if 'VK_KHR_ray_query' in extensions:
            values.append(query)
        if 'VK_KHR_ray_tracing_pipeline' in extensions:
            values.append(pipeline)
        features = Features2(sType=1000059000, pNext=chain(*values))
        self.call('vkGetPhysicalDeviceFeatures2', None, [P, P], device, c.byref(features))
        if not acceleration.enabled or not address.enabled or not (query.enabled or pipeline.enabled):
            return False
        names = list(required)
        # Enable only the queried core RT features, rather than every optional bit.
        address, acceleration = Address(sType=1000257000, enabled=1), Acceleration(sType=1000150013, enabled=1)
        enabled = [address, acceleration]
        if query.enabled:
            enabled.append(Query(sType=1000348013, enabled=1))
            names.append('VK_KHR_ray_query')
        if pipeline.enabled:
            enabled.append(Pipeline(sType=1000347000, enabled=1))
            names.append('VK_KHR_ray_tracing_pipeline')
        count = U()
        self.call('vkGetPhysicalDeviceQueueFamilyProperties', None, [P, P, P], device, c.byref(count), None)
        if count.value > 64:
            raise RuntimeError('too-many-vulkan-queues')
        queues = (Queue * count.value)()
        self.call('vkGetPhysicalDeviceQueueFamilyProperties', None, [P, P, P], device, c.byref(count), queues)
        family = next((i for i, q in enumerate(queues[:count.value]) if q.count and q.flags & 3), None)
        if family is None:
            return False
        priority = c.c_float(1)
        queue = QueueInfo(sType=2, family=family, count=1, priorities=pointer(priority))
        encoded = (c.c_char_p * len(names))(*(name.encode() for name in names))
        info = DeviceInfo(sType=3, pNext=chain(*enabled), queueCount=1, queues=pointer(queue), extensionCount=len(names), extensions=c.cast(encoded, P))
        logical = P()
        check(self.call('vkCreateDevice', c.c_int, [P, P, P, P], device, c.byref(info), None, c.byref(logical)))
        self.call('vkDestroyDevice', None, [P, P], logical, None)
        return True

def probe(node, bdf, driver):
    info = os.stat(node)
    sys_device = Path('/sys/class/drm') / Path(node).name / 'device'
    if not stat.S_ISCHR(info.st_mode) or sys_device.resolve().name.lower() != bdf.lower() or (sys_device / 'driver').resolve().name != driver:
        return None, 'render-device-identity-unverified'
    hardware = None
    if driver == 'amdgpu':
        hardware = amd_hardware(node)
        emulation = any('emulate_rt' in os.environ.get(key, '').split(',') for key in ('RADV_PERFTEST', 'RADV_DEBUG', 'RADV_EXPERIMENTAL'))
        if emulation or os.environ.get('radv_emulate_rt', '').lower() in ('true', '1'):
            return None, 'ray-tracing-emulation-forced'
    elif driver == 'nvidia':
        hardware = nvidia_hardware(bdf)
    if hardware is False:
        return False, 'dedicated-ray-tracing-hardware-unavailable'
    vulkan = Vulkan()
    try:
        for device in vulkan.devices():
            extensions = vulkan.extensions(device)
            props, pci, drm, backend = vulkan.properties(device, extensions)
            if not matches_device(props, pci, drm, extensions, bdf, info.st_rdev):
                continue
            if props.properties.api < (1 << 22) | (2 << 12):
                return False, 'vulkan-12-required'
            usable = vulkan.verify_features(device, extensions)
            if not usable:
                return False, 'vulkan-ray-tracing-unavailable'
            if driver in ('i915', 'xe') and backend.id == 6 and props.properties.vendor == 0x8086:
                # ANV only exposes these RT features on its hardware RT path.
                hardware = True
            if hardware is not True:
                return None, 'dedicated-hardware-query-unavailable'
            return True, 'hardware-and-vulkan-device-verified'
        return None, 'vulkan-device-identity-unverified'
    finally:
        vulkan.close()

def main():
    try:
        if len(sys.argv) != 4:
            return 2
        supported, reason = probe(*sys.argv[1:])
    except Exception as error:
        reason = str(error) if isinstance(error, RuntimeError) else 'native-ray-tracing-query-failed'
        if not reason or len(reason) > 256 or any(ch not in 'abcdefghijklmnopqrstuvwxyz0123456789-' for ch in reason):
            reason = 'native-ray-tracing-query-failed'
        supported = None
    value = -1 if supported is None else int(supported)
    reply = f'WOLF_RT_V1 {value} {reason}\n'.encode()
    return 0 if os.write(3, reply) == len(reply) else 2

if __name__ == '__main__':
    sys.exit(main())
