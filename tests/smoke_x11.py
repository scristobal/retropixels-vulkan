#!/usr/bin/env python3
"""X11 integration test: pixels, swapchain recreation, unmap/remap, Escape.

Requires Python 3, Pillow, libX11, and an X11 display (Xvfb also works).
Run: XDG_SESSION_TYPE=x11 python3 tests/smoke_x11.py build/RetroPixels
The executable inherits VK_DRIVER_FILES and Vulkan validation settings.
"""

import ctypes as c
import os
from pathlib import Path
import subprocess
import sys
import time

from PIL import Image


class XImage(c.Structure):
    _fields_ = [
        ("width", c.c_int), ("height", c.c_int), ("xoffset", c.c_int),
        ("format", c.c_int), ("data", c.c_void_p), ("byte_order", c.c_int),
        ("bitmap_unit", c.c_int), ("bitmap_bit_order", c.c_int),
        ("bitmap_pad", c.c_int), ("depth", c.c_int), ("bytes_per_line", c.c_int),
        ("bits_per_pixel", c.c_int), ("red_mask", c.c_ulong),
        ("green_mask", c.c_ulong), ("blue_mask", c.c_ulong),
    ]


class XKeyEvent(c.Structure):
    _fields_ = [
        ("type", c.c_int), ("serial", c.c_ulong), ("send_event", c.c_int),
        ("display", c.c_void_p), ("window", c.c_ulong), ("root", c.c_ulong),
        ("subwindow", c.c_ulong), ("time", c.c_ulong), ("x", c.c_int),
        ("y", c.c_int), ("x_root", c.c_int), ("y_root", c.c_int),
        ("state", c.c_uint), ("keycode", c.c_uint), ("same_screen", c.c_int),
    ]


class XEvent(c.Union):
    _fields_ = [("key", XKeyEvent), ("padding", c.c_long * 24)]


class XSetWindowAttributes(c.Structure):
    _fields_ = [
        ("background_pixmap", c.c_ulong), ("background_pixel", c.c_ulong),
        ("border_pixmap", c.c_ulong), ("border_pixel", c.c_ulong),
        ("bit_gravity", c.c_int), ("win_gravity", c.c_int), ("backing_store", c.c_int),
        ("backing_planes", c.c_ulong), ("backing_pixel", c.c_ulong),
        ("save_under", c.c_int), ("event_mask", c.c_long),
        ("do_not_propagate_mask", c.c_long), ("override_redirect", c.c_int),
        ("colormap", c.c_ulong), ("cursor", c.c_ulong),
    ]


x = c.CDLL("libX11.so.6")


def bind(name, result, *arguments):
    function = getattr(x, name)
    function.restype = result
    function.argtypes = list(arguments)
    return function


Display = c.c_void_p
Window = c.c_ulong
UIntP = c.POINTER(c.c_uint)
WindowP = c.POINTER(Window)
open_display = bind("XOpenDisplay", Display, c.c_char_p)
close_display = bind("XCloseDisplay", c.c_int, Display)
root_window = bind("XDefaultRootWindow", Window, Display)
query_tree = bind("XQueryTree", c.c_int, Display, Window, WindowP, WindowP,
                  c.POINTER(WindowP), UIntP)
fetch_name = bind("XFetchName", c.c_int, Display, Window, c.POINTER(c.c_void_p))
intern_atom = bind("XInternAtom", c.c_ulong, Display, c.c_char_p, c.c_int)
get_property = bind("XGetWindowProperty", c.c_int, Display, Window, c.c_ulong,
                    c.c_long, c.c_long, c.c_int, c.c_ulong, c.POINTER(c.c_ulong),
                    c.POINTER(c.c_int), c.POINTER(c.c_ulong), c.POINTER(c.c_ulong),
                    c.POINTER(c.c_void_p))
free = bind("XFree", c.c_int, c.c_void_p)
geometry = bind("XGetGeometry", c.c_int, Display, Window, WindowP,
                c.POINTER(c.c_int), c.POINTER(c.c_int), UIntP, UIntP, UIntP, UIntP)
resize = bind("XResizeWindow", c.c_int, Display, Window, c.c_uint, c.c_uint)
unmap = bind("XUnmapWindow", c.c_int, Display, Window)
map_window = bind("XMapRaised", c.c_int, Display, Window)
change_attributes = bind("XChangeWindowAttributes", c.c_int, Display, Window,
                         c.c_ulong, c.POINTER(XSetWindowAttributes))
sync = bind("XSync", c.c_int, Display, c.c_int)
get_image = bind("XGetImage", c.POINTER(XImage), Display, Window, c.c_int,
                 c.c_int, c.c_uint, c.c_uint, c.c_ulong, c.c_int)
get_pixel = bind("XGetPixel", c.c_ulong, c.POINTER(XImage), c.c_int, c.c_int)
destroy_image = bind("XDestroyImage", c.c_int, c.POINTER(XImage))
keycode = bind("XKeysymToKeycode", c.c_ubyte, Display, c.c_ulong)
send_event = bind("XSendEvent", c.c_int, Display, Window, c.c_int,
                  c.c_long, c.POINTER(XEvent))


def find_window(display, parent, pid):
    name = c.c_void_p()
    if fetch_name(display, parent, c.byref(name)) and name.value:
        title = c.string_at(name).decode(errors="replace")
        free(name)
        if title == "RetroPixel (Vulkan)":
            atom = intern_atom(display, b"_NET_WM_PID", False)
            actual_type, count, remaining = c.c_ulong(), c.c_ulong(), c.c_ulong()
            actual_format, data = c.c_int(), c.c_void_p()
            status = get_property(display, parent, atom, 0, 1, False, 6,
                                  c.byref(actual_type), c.byref(actual_format),
                                  c.byref(count), c.byref(remaining), c.byref(data))
            try:
                if (status == 0 and actual_format.value == 32 and count.value == 1 and data.value
                        and c.cast(data, c.POINTER(c.c_ulong))[0] == pid):
                    return parent
            finally:
                if data.value:
                    free(data)
    root, ancestor, children, count = Window(), Window(), WindowP(), c.c_uint()
    if not query_tree(display, parent, c.byref(root), c.byref(ancestor),
                      c.byref(children), c.byref(count)):
        return None
    try:
        for child in list(children[:count.value]):
            found = find_window(display, child, pid)
            if found:
                return found
    finally:
        if children:
            free(children)
    return None


def dimensions(display, window):
    root, left, top = Window(), c.c_int(), c.c_int()
    width, height, border, depth = (c.c_uint() for _ in range(4))
    geometry(display, window, c.byref(root), c.byref(left), c.byref(top),
             c.byref(width), c.byref(height), c.byref(border), c.byref(depth))
    return width.value, height.value


def channel(pixel, mask):
    shift = (mask & -mask).bit_length() - 1
    return round(((pixel & mask) >> shift) * 255 / (mask >> shift))


def check_pixels(display, window, sprite):
    width, height = dimensions(display, window)
    if not width or not height:
        return False
    image = get_image(display, window, 0, 0, width, height, c.c_ulong(-1), 2)
    if not image:
        return False
    try:
        # Square powers of two exercise each mip; larger sizes exercise magnification.
        reference = sprite
        while reference.width > min(width, height):
            size = reference.width // 2
            # Filter channels in float, then quantize once per mip (no premultiplied alpha).
            reference = Image.merge("RGBA", [plane.convert("F").resize((size, size), Image.Resampling.BOX)
                                             .point(lambda value: value + 0.5).convert("L")
                                             for plane in reference.split()])
        masks = image.contents.red_mask, image.contents.green_mask, image.contents.blue_mask
        for y in range(reference.height):
            for col in range(reference.width):
                rgba = reference.getpixel((col, y))
                expected = rgba[:3] if rgba[3] else (20, 20, 26)
                pixel = get_pixel(image, int((col + 0.5) * width / reference.width),
                                  int((y + 0.5) * height / reference.height))
                actual = tuple(channel(pixel, mask) for mask in masks)
                if any(abs(a - b) > 1 for a, b in zip(actual, expected)):
                    check_pixels.mismatch = f"texel ({col}, {y}): got {actual}, expected {expected}"
                    return False
        return True
    finally:
        destroy_image(image)


def wait_until(condition, process, description):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(f"App exited during {description}: {process.returncode}")
        result = condition()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {description}; {getattr(check_pixels, 'mismatch', '')}")


def main():
    executable = Path(sys.argv[1] if len(sys.argv) > 1 else "build/RetroPixels").resolve()
    sprite = Image.open(Path(__file__).resolve().parents[1] / "assets/avatar-1x.png").convert("RGBA")
    display = open_display(None)
    if not display:
        raise RuntimeError("Cannot open DISPLAY. Run this test on an X11 display.")
    environment = dict(os.environ, XDG_SESSION_TYPE="x11")
    environment.pop("WAYLAND_DISPLAY", None)
    process = subprocess.Popen([str(executable), "--validation", "--hidden"], env=environment,
                               cwd=executable.parent, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    try:
        window = wait_until(lambda: find_window(display, root_window(display), process.pid), process, "the window")
        # Bypass window-manager size constraints so the mipmap test can reach 1x1.
        attributes = XSetWindowAttributes(override_redirect=True)
        change_attributes(display, window, 1 << 9, c.byref(attributes))
        map_window(display, window)
        sync(display, False)
        wait_until(lambda: check_pixels(display, window, sprite), process, "initial sprite pixels")
        print(f"Sprite pixels and orientation match at {dimensions(display, window)}.")
        for size in [(320, 240), (800, 600), (32, 32), (16, 16), (8, 8),
                     (4, 4), (2, 2), (1, 1), (640, 480)]:
            resize(display, window, *size)
            sync(display, False)
            wait_until(lambda: dimensions(display, window) == size, process, f"resize to {size}")
            wait_until(lambda: check_pixels(display, window, sprite), process, f"pixels at {size}")
            print(f"Resize and sprite pixels match at {size}.")
        unmap(display, window)
        sync(display, False)
        time.sleep(0.2)
        map_window(display, window)
        sync(display, False)
        wait_until(lambda: check_pixels(display, window, sprite), process, "unmap/remap")
        print("Rendering resumes after unmap/remap.")
        event = XEvent()
        event.key = XKeyEvent(type=2, send_event=True, display=display, window=window,
                              root=root_window(display), keycode=keycode(display, 0xFF1B), same_screen=True)
        send_event(display, window, False, 1, c.byref(event))
        sync(display, False)
        output = process.communicate(timeout=10)[0]
        print(output, end="")
        assert process.returncode == 0, f"App exited with {process.returncode}"
        assert "Vulkan warning:" not in output and "Vulkan error:" not in output, output
        print("Escape exits cleanly; no Vulkan validation warnings or errors.")
    finally:
        if process.poll() is None:
            process.terminate()
            print(process.communicate(timeout=10)[0], end="")
        close_display(display)


if __name__ == "__main__":
    main()
