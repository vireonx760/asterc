"""Test-only native code mapping: writable while loading, then executable RX."""
from contextlib import contextmanager
import ctypes
import mmap
import os
import platform


def native_x86_supported():
    return (platform.system() in ('Linux', 'Windows') and
            platform.machine().lower() in ('x86_64', 'amd64'))


@contextmanager
def executable_code(code):
    if not code:
        raise ValueError('empty native code')
    if os.name == 'nt':
        api = ctypes.WinDLL('kernel32', use_last_error=True)
        api.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
        api.VirtualAlloc.restype = ctypes.c_void_p
        api.VirtualProtect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong,
                                       ctypes.POINTER(ctypes.c_ulong)]
        api.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong]
        api.GetCurrentProcess.restype = ctypes.c_void_p
        api.FlushInstructionCache.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
        address = api.VirtualAlloc(None, len(code), 0x3000, 0x04)  # reserve/commit RW
        if not address:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            ctypes.memmove(address, code, len(code))
            previous = ctypes.c_ulong()
            if not api.VirtualProtect(address, len(code), 0x20, ctypes.byref(previous)):
                raise ctypes.WinError(ctypes.get_last_error())
            if not api.FlushInstructionCache(api.GetCurrentProcess(), address, len(code)):
                raise ctypes.WinError(ctypes.get_last_error())
            yield address
        finally:
            if not api.VirtualFree(address, 0, 0x8000):
                raise ctypes.WinError(ctypes.get_last_error())
    else:
        with mmap.mmap(-1, len(code), prot=mmap.PROT_READ | mmap.PROT_WRITE) as memory:
            memory.write(code)
            address = ctypes.addressof(ctypes.c_char.from_buffer(memory))
            libc = ctypes.CDLL(None, use_errno=True)
            libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
            if libc.mprotect(address, len(code), mmap.PROT_READ | mmap.PROT_EXEC):
                raise OSError(ctypes.get_errno(), 'mprotect RX')
            yield address
