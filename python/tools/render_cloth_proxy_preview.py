from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
import trimesh
from mpl_toolkits.mplot3d.art3d import Poly3DCollection


def load_mesh(path: Path) -> trimesh.Trimesh:
    loaded = trimesh.load(path, force="scene", process=False)
    if isinstance(loaded, trimesh.Trimesh):
        return loaded.copy()
    if isinstance(loaded, trimesh.Scene):
        meshes = [
            item
            for item in loaded.dump(concatenate=False)
            if isinstance(item, trimesh.Trimesh) and len(item.vertices) > 0 and len(item.faces) > 0
        ]
        if not meshes:
            raise RuntimeError(f"No triangle mesh found in {path}")
        return meshes[0].copy() if len(meshes) == 1 else trimesh.util.concatenate(meshes)
    raise TypeError(f"Unsupported asset type: {type(loaded)!r}")


def sample_face_indices(mesh: trimesh.Trimesh, max_faces: int, seed: int) -> np.ndarray:
    face_count = len(mesh.faces)
    if face_count <= max_faces:
        return np.arange(face_count)
    rng = np.random.default_rng(seed)
    return np.sort(rng.choice(face_count, size=max_faces, replace=False))


def shaded_face_colors(normals: np.ndarray, base_color: tuple[float, float, float]) -> np.ndarray:
    light = np.asarray([0.25, -0.55, 0.80], dtype=np.float64)
    light /= np.linalg.norm(light)
    intensity = np.clip(normals @ light, 0.0, 1.0)
    intensity = 0.30 + 0.70 * intensity
    colors = np.asarray(base_color, dtype=np.float64)[None, :] * intensity[:, None]
    return np.column_stack([np.clip(colors, 0.0, 1.0), np.full(len(colors), 1.0)])


def mesh_base_color(mesh: trimesh.Trimesh, fallback: tuple[float, float, float]) -> tuple[float, float, float]:
    material = getattr(mesh.visual, "material", None)
    kind = getattr(mesh.visual, "kind", None)
    if material is not None:
        base_color = getattr(material, "baseColorFactor", None)
        if base_color is not None:
            base_array = np.asarray(base_color[:3], dtype=np.float64)
            if base_array.max() > 1.0:
                base_array = base_array / 255.0
            return tuple(float(component) for component in base_array)

        diffuse = getattr(material, "diffuse", None)
        if diffuse is not None:
            diffuse_array = np.asarray(diffuse, dtype=np.float64)
            if diffuse_array.max() > 1.0:
                diffuse_array = diffuse_array / 255.0
            return tuple(float(component) for component in diffuse_array[:3])

        main_color = getattr(material, "main_color", None)
        if main_color is not None and kind != "texture":
            main_array = np.asarray(main_color, dtype=np.float64)
            if main_array.max() > 1.0:
                main_array = main_array / 255.0
            return tuple(float(component) for component in main_array[:3])

    vertex_colors = getattr(mesh.visual, "vertex_colors", None)
    if vertex_colors is not None and len(vertex_colors) > 0:
        color_array = np.asarray(vertex_colors[:, :3], dtype=np.float64)
        if color_array.max() > 1.0:
            color_array = color_array / 255.0
        return tuple(float(component) for component in color_array.mean(axis=0))

    return fallback


def set_axes_equal(ax: plt.Axes, bounds: np.ndarray) -> None:
    center = bounds.mean(axis=0)
    radius = float(np.max(bounds[1] - bounds[0]) * 0.5)
    ax.set_xlim(center[0] - radius, center[0] + radius)
    ax.set_ylim(center[1] - radius, center[1] + radius)
    ax.set_zlim(center[2] - radius, center[2] + radius)


def render_mesh_panel(
    ax: plt.Axes,
    mesh: trimesh.Trimesh,
    title: str,
    bounds: np.ndarray,
    max_faces: int,
    color: tuple[float, float, float],
    seed: int,
) -> None:
    face_ids = sample_face_indices(mesh, max_faces=max_faces, seed=seed)
    triangles = np.asarray(mesh.triangles[face_ids], dtype=np.float64)
    normals = np.asarray(mesh.face_normals[face_ids], dtype=np.float64)

    collection = Poly3DCollection(
        triangles,
        facecolors=shaded_face_colors(normals, color),
        edgecolors=(0.08, 0.09, 0.10, 0.08),
        linewidths=0.04,
    )
    collection.set_zsort("average")
    ax.add_collection3d(collection)
    ax.view_init(elev=18, azim=-58, roll=0)
    set_axes_equal(ax, bounds)
    ax.set_title(title, fontsize=12)
    ax.set_axis_off()


def render_preview(args: argparse.Namespace) -> None:
    high = load_mesh(Path(args.high).resolve())
    proxy = load_mesh(Path(args.proxy).resolve())
    mapped = load_mesh(Path(args.mapped).resolve())

    all_bounds = np.stack([high.bounds, proxy.bounds, mapped.bounds], axis=0)
    bounds = np.stack([all_bounds[:, 0, :].min(axis=0), all_bounds[:, 1, :].max(axis=0)])

    fig = plt.figure(figsize=(15, 5), dpi=args.dpi)
    fallback_pink = (179.0 / 255.0, 34.0 / 255.0, 102.0 / 255.0)
    panels = [
        (
            high,
            f"render high\n{len(high.vertices)} vertices / {len(high.faces)} faces",
            args.high_faces,
            mesh_base_color(high, fallback_pink),
            11,
        ),
        (
            proxy,
            f"watertight sim proxy\n{len(proxy.vertices)} vertices / {len(proxy.faces)} faces",
            args.proxy_faces,
            mesh_base_color(proxy, fallback_pink),
            23,
        ),
        (
            mapped,
            f"mapped high test\n{len(mapped.vertices)} vertices / {len(mapped.faces)} faces",
            args.high_faces,
            mesh_base_color(mapped, fallback_pink),
            37,
        ),
    ]
    for index, (mesh, title, max_faces, color, seed) in enumerate(panels, start=1):
        ax = fig.add_subplot(1, 3, index, projection="3d")
        render_mesh_panel(ax, mesh, title, bounds, max_faces, color, seed)

    fig.tight_layout(pad=0.4)
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, facecolor="white", bbox_inches="tight", pad_inches=0.08)
    plt.close(fig)
    print(output)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Render an offscreen preview for cloth proxy mapping tests.")
    parser.add_argument("--high", required=True, help="Original high-resolution render mesh.")
    parser.add_argument("--proxy", required=True, help="Generated simulation proxy mesh.")
    parser.add_argument("--mapped", required=True, help="Mapped high-resolution deformation test mesh.")
    parser.add_argument("--output", default="test_output/cloth_proxy/cloth_proxy_preview.png")
    parser.add_argument("--high-faces", type=int, default=30000, help="Sampled faces for high mesh panels.")
    parser.add_argument("--proxy-faces", type=int, default=12000, help="Sampled faces for the proxy panel.")
    parser.add_argument("--dpi", type=int, default=160)
    return parser


def main() -> None:
    args = make_parser().parse_args()
    render_preview(args)


if __name__ == "__main__":
    main()
