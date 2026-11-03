# Copyright 2026 Rodrigo Pérez-Rodríguez
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
The images of a request, encoded once as lossless PNG (no resizing, no OpenCV).

The encoding is byte for byte the one of attention's router (llm_router/http_backends.py):
8 bits per channel, filter type 0 on every row, zlib level 6. Encodings rgb8, bgr8, 8UC3
(read as RGB), rgba8, bgra8, mono8 and 8UC1.
"""

import base64
import hashlib
import struct
import zlib

CHANNELS = {'rgb8': 3, 'bgr8': 3, '8UC3': 3, 'rgba8': 4, 'bgra8': 4, 'mono8': 1, '8UC1': 1}


def image_to_png(image):
    """PNG bytes of a sensor_msgs/Image (rgb8, bgr8, rgba8, bgra8, mono8 or 8UC3)."""
    encoding = image.encoding
    if encoding not in CHANNELS:
        raise ValueError(f'Unsupported image encoding "{encoding}"')
    n = CHANNELS[encoding]
    width, height, step = image.width, image.height, image.step
    data = bytes(image.data)
    rows = []
    for y in range(height):
        row = data[y * step:y * step + width * n]
        if encoding in ('bgr8', 'bgra8'):
            # Swap B and R of every pixel
            pixels = bytearray(row)
            pixels[0::n], pixels[2::n] = row[2::n], row[0::n]
            row = bytes(pixels)
        rows.append(b'\x00' + row)  # filter type 0 (none) for every row
    color_type = {1: 0, 3: 2, 4: 6}[n]

    def chunk(kind, payload):
        return (struct.pack('>I', len(payload)) + kind + payload +
                struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))

    header = struct.pack('>IIBBBBB', width, height, 8, color_type, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) +
            chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))


def image_to_jpeg(image, quality=95):
    """
    Return the JPEG bytes of a sensor_msgs/Image as attention's Gemini bridge makes them.

    The image is converted to bgr8 (as cv_bridge::toCvCopy(image, "bgr8")), not resized, and
    encoded by OpenCV at the given quality.
    """
    try:
        import cv2
        import numpy as np
    except ImportError as error:  # optional: only Gemini requests with images need it
        raise ValueError('Gemini images need OpenCV for Python (python3-opencv) and numpy') \
            from error
    encoding = image.encoding
    if encoding not in CHANNELS:
        raise ValueError(f'Unsupported image encoding "{encoding}"')
    n = CHANNELS[encoding]
    rows = np.frombuffer(bytes(image.data), dtype=np.uint8).reshape(image.height, image.step)
    pixels = rows[:, :image.width * n].reshape(image.height, image.width, n)
    convert = {'rgb8': cv2.COLOR_RGB2BGR, 'rgba8': cv2.COLOR_RGBA2BGR,
               'bgra8': cv2.COLOR_BGRA2BGR, 'mono8': cv2.COLOR_GRAY2BGR,
               '8UC1': cv2.COLOR_GRAY2BGR}.get(encoding)
    bgr = cv2.cvtColor(pixels, convert) if convert is not None else np.ascontiguousarray(pixels)
    ok, buffer = cv2.imencode('.jpeg', bgr, [cv2.IMWRITE_JPEG_QUALITY, int(quality)])
    if not ok:
        raise ValueError('The image could not be encoded as JPEG')
    return buffer.tobytes()


class EncodedImage:
    """An image of a request: the message, its PNG bytes, their base64 and their hash."""

    def __init__(self, message=None, png=None):
        self.message = message
        self.png = png if png is not None else image_to_png(message)
        self.sha256 = hashlib.sha256(self.png).hexdigest()
        self._base64 = None

    @property
    def jpeg_base64(self):
        """Return the base64 of the image as JPEG (quality 95), computed once (Gemini)."""
        if getattr(self, '_jpeg_base64', None) is None:
            self._jpeg_base64 = base64.b64encode(image_to_jpeg(self.message)).decode()
        return self._jpeg_base64

    @property
    def base64(self):
        if self._base64 is None:
            self._base64 = base64.b64encode(self.png).decode()
        return self._base64
