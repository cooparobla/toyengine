"""Typed builders for toyengine asset kinds, plus a generic writer for the rest.

Import from `toyproject` directly -- these names are re-exported there.
"""

from .generic import write_asset
from .material import Material
from .mesh import (Mesh, face_normal, perpendicular_tangents, vertex_normals,
                   write_mesh)
from .node import Component, Obj, rgb, vec3
from .object_asset import ObjectAsset
from .scene import Scene
from .texture import write_heightmap, write_texture

__all__ = [
    "Component",
    "Material",
    "Mesh",
    "Obj",
    "ObjectAsset",
    "Scene",
    "face_normal",
    "perpendicular_tangents",
    "rgb",
    "vec3",
    "vertex_normals",
    "write_asset",
    "write_heightmap",
    "write_mesh",
    "write_texture",
]
