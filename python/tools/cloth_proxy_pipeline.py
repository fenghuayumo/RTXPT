from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np
import open3d as o3d
import trimesh
from pygltflib import GLTF2


def load_render_mesh(path: Path) -> trimesh.Trimesh:
    loaded = trimesh.load(path, force="scene", process=False)
    if isinstance(loaded, trimesh.Trimesh):
        mesh = loaded
    elif isinstance(loaded, trimesh.Scene):
        meshes = [
            item
            for item in loaded.dump(concatenate=False)
            if isinstance(item, trimesh.Trimesh) and len(item.vertices) > 0 and len(item.faces) > 0
        ]
        if not meshes:
            raise RuntimeError(f"No triangle meshes found in {path}")
        mesh = meshes[0].copy() if len(meshes) == 1 else trimesh.util.concatenate(meshes)
    else:
        raise TypeError(f"Unsupported asset type: {type(loaded)!r}")

    if not isinstance(mesh, trimesh.Trimesh):
        raise TypeError(f"Expected a Trimesh, got {type(mesh)!r}")
    if len(mesh.vertices) == 0 or len(mesh.faces) == 0:
        raise RuntimeError(f"{path} does not contain usable triangle geometry")
    return mesh.copy()


def clean_mesh_for_sampling(mesh: trimesh.Trimesh) -> trimesh.Trimesh:
    result = mesh.copy()
    result.update_faces(result.nondegenerate_faces())
    result.remove_unreferenced_vertices()
    return result


def mesh_stats(mesh: trimesh.Trimesh) -> dict[str, Any]:
    stats: dict[str, Any] = {
        "vertices": int(len(mesh.vertices)),
        "faces": int(len(mesh.faces)),
        "trimesh_watertight": bool(mesh.is_watertight),
        "winding_consistent": bool(mesh.is_winding_consistent),
        "euler_number": int(mesh.euler_number),
        "bounds_min": np.asarray(mesh.bounds[0]).astype(float).tolist(),
        "bounds_max": np.asarray(mesh.bounds[1]).astype(float).tolist(),
    }
    try:
        stats["volume"] = float(mesh.volume)
    except Exception:
        stats["volume"] = None
    return stats


def edge_topology_stats(mesh: trimesh.Trimesh) -> dict[str, int]:
    edges = np.asarray(mesh.edges_sorted)
    if len(edges) == 0:
        return {
            "unique_edges": 0,
            "boundary_edges": 0,
            "nonmanifold_edges": 0,
            "max_edge_face_count": 0,
        }
    _, counts = np.unique(edges, axis=0, return_counts=True)
    return {
        "unique_edges": int(len(counts)),
        "boundary_edges": int(np.sum(counts == 1)),
        "nonmanifold_edges": int(np.sum(counts > 2)),
        "max_edge_face_count": int(np.max(counts)),
    }


def o3d_stats(mesh: o3d.geometry.TriangleMesh) -> dict[str, Any]:
    return {
        "open3d_edge_manifold": bool(mesh.is_edge_manifold()),
        "open3d_vertex_manifold": bool(mesh.is_vertex_manifold()),
        "open3d_self_intersecting": bool(mesh.is_self_intersecting()),
        "open3d_watertight": bool(mesh.is_watertight()),
    }


def sample_points_with_normals(mesh: trimesh.Trimesh, count: int) -> tuple[np.ndarray, np.ndarray]:
    points, face_index = trimesh.sample.sample_surface(mesh, count=count)
    normals = np.asarray(mesh.face_normals[face_index], dtype=np.float64)
    return np.asarray(points, dtype=np.float64), normals


def build_watertight_proxy(
    render_mesh: trimesh.Trimesh,
    target_faces: int,
    samples: int,
    poisson_depth: int,
    poisson_scale: float,
) -> tuple[trimesh.Trimesh, dict[str, Any]]:
    source = clean_mesh_for_sampling(render_mesh)
    points, normals = sample_points_with_normals(source, samples)

    pcd = o3d.geometry.PointCloud()
    pcd.points = o3d.utility.Vector3dVector(points)
    pcd.normals = o3d.utility.Vector3dVector(normals)

    with o3d.utility.VerbosityContextManager(o3d.utility.VerbosityLevel.Error):
        poisson_mesh, densities = o3d.geometry.TriangleMesh.create_from_point_cloud_poisson(
            pcd,
            depth=poisson_depth,
            scale=poisson_scale,
            linear_fit=False,
        )

    poisson_mesh.remove_degenerate_triangles()
    poisson_mesh.remove_duplicated_triangles()
    poisson_mesh.remove_duplicated_vertices()
    poisson_mesh.compute_vertex_normals()

    proxy_o3d = poisson_mesh
    if len(proxy_o3d.triangles) > target_faces:
        proxy_o3d = proxy_o3d.simplify_quadric_decimation(
            target_number_of_triangles=target_faces,
        )

    proxy_o3d.remove_degenerate_triangles()
    proxy_o3d.remove_duplicated_triangles()
    proxy_o3d.remove_duplicated_vertices()
    proxy_o3d.remove_non_manifold_edges()
    proxy_o3d.compute_vertex_normals()

    proxy = trimesh.Trimesh(
        vertices=np.asarray(proxy_o3d.vertices),
        faces=np.asarray(proxy_o3d.triangles),
        process=True,
    )
    trimesh.repair.fix_normals(proxy, multibody=True)

    hole_repair: dict[str, Any] = {
        "attempted": False,
        "filled": False,
        "before": edge_topology_stats(proxy),
        "after": None,
    }
    if not proxy.is_watertight:
        hole_repair["attempted"] = True
        hole_repair["filled"] = bool(trimesh.repair.fill_holes(proxy))
        proxy.remove_unreferenced_vertices()
        trimesh.repair.fix_normals(proxy, multibody=True)
        hole_repair["after"] = edge_topology_stats(proxy)

    report = {
        "samples": int(samples),
        "poisson_depth": int(poisson_depth),
        "poisson_scale": float(poisson_scale),
        "poisson_vertices": int(len(poisson_mesh.vertices)),
        "poisson_faces": int(len(poisson_mesh.triangles)),
        "poisson_density_min": float(np.min(densities)),
        "poisson_density_max": float(np.max(densities)),
        "poisson_density_mean": float(np.mean(densities)),
        "proxy_open3d": o3d_stats(proxy_o3d),
        "hole_repair": hole_repair,
        "proxy_trimesh": mesh_stats(proxy),
    }
    return proxy, report


def make_raycast_scene(mesh: trimesh.Trimesh) -> o3d.t.geometry.RaycastingScene:
    vertices = np.asarray(mesh.vertices, dtype=np.float32)
    faces = np.asarray(mesh.faces, dtype=np.uint32)

    tmesh = o3d.t.geometry.TriangleMesh()
    tmesh.vertex["positions"] = o3d.core.Tensor(vertices, dtype=o3d.core.Dtype.Float32)
    tmesh.triangle["indices"] = o3d.core.Tensor(faces, dtype=o3d.core.Dtype.UInt32)

    scene = o3d.t.geometry.RaycastingScene()
    scene.add_triangles(tmesh)
    return scene


def triangle_basis(vertices: np.ndarray, faces: np.ndarray, triangle_ids: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    tri = vertices[faces[triangle_ids]]
    edge0 = tri[:, 1] - tri[:, 0]
    edge1 = tri[:, 2] - tri[:, 0]

    normal = np.cross(edge0, edge1)
    normal_len = np.linalg.norm(normal, axis=1, keepdims=True)
    normal = normal / np.maximum(normal_len, 1.0e-20)

    tangent = edge0
    tangent_len = np.linalg.norm(tangent, axis=1, keepdims=True)
    fallback = tangent_len[:, 0] < 1.0e-20
    if np.any(fallback):
        tangent[fallback] = edge1[fallback]
        tangent_len[fallback] = np.linalg.norm(tangent[fallback], axis=1, keepdims=True)
    tangent = tangent / np.maximum(tangent_len, 1.0e-20)

    bitangent = np.cross(normal, tangent)
    bitangent_len = np.linalg.norm(bitangent, axis=1, keepdims=True)
    bitangent = bitangent / np.maximum(bitangent_len, 1.0e-20)
    return tangent, bitangent, normal


def build_high_to_proxy_mapping(
    render_vertices: np.ndarray,
    proxy_mesh: trimesh.Trimesh,
    chunk_size: int,
) -> tuple[dict[str, np.ndarray], dict[str, Any]]:
    scene = make_raycast_scene(proxy_mesh)
    queries = np.asarray(render_vertices, dtype=np.float32)

    triangle_chunks: list[np.ndarray] = []
    bary_chunks: list[np.ndarray] = []
    closest_chunks: list[np.ndarray] = []

    for start in range(0, len(queries), chunk_size):
        query_chunk = o3d.core.Tensor(
            queries[start : start + chunk_size],
            dtype=o3d.core.Dtype.Float32,
        )
        closest = scene.compute_closest_points(query_chunk)
        triangle_chunks.append(closest["primitive_ids"].numpy().astype(np.int64))

        uv = closest["primitive_uvs"].numpy().astype(np.float32)
        bary = np.empty((len(uv), 3), dtype=np.float32)
        bary[:, 1] = uv[:, 0]
        bary[:, 2] = uv[:, 1]
        bary[:, 0] = 1.0 - bary[:, 1] - bary[:, 2]
        bary_chunks.append(bary)
        closest_chunks.append(closest["points"].numpy().astype(np.float32))

    triangle_ids = np.concatenate(triangle_chunks)
    barycentric = np.concatenate(bary_chunks)
    closest_points = np.concatenate(closest_chunks)

    proxy_vertices = np.asarray(proxy_mesh.vertices, dtype=np.float32)
    proxy_faces = np.asarray(proxy_mesh.faces, dtype=np.int32)
    tangent, bitangent, normal = triangle_basis(proxy_vertices, proxy_faces, triangle_ids)

    residual = queries - closest_points
    offset_local = np.stack(
        [
            np.sum(residual * tangent, axis=1),
            np.sum(residual * bitangent, axis=1),
            np.sum(residual * normal, axis=1),
        ],
        axis=1,
    ).astype(np.float32)

    distance = np.linalg.norm(residual, axis=1)
    mapping = {
        "triangle_indices": triangle_ids.astype(np.int32),
        "barycentric": barycentric.astype(np.float32),
        "offset_local": offset_local,
        "rest_distance": distance.astype(np.float32),
        "sim_vertices_rest": proxy_vertices,
        "sim_faces": proxy_faces,
    }
    report = {
        "render_vertices": int(len(render_vertices)),
        "distance_min": float(np.min(distance)),
        "distance_mean": float(np.mean(distance)),
        "distance_p95": float(np.percentile(distance, 95)),
        "distance_max": float(np.max(distance)),
    }
    return mapping, report


def reconstruct_render_vertices(mapping: dict[str, np.ndarray], sim_vertices: np.ndarray) -> np.ndarray:
    sim_faces = mapping["sim_faces"]
    triangle_ids = mapping["triangle_indices"]
    barycentric = mapping["barycentric"]
    offset_local = mapping["offset_local"]

    sim_vertices = np.asarray(sim_vertices, dtype=np.float32)
    tri = sim_vertices[sim_faces[triangle_ids]]
    points = np.sum(tri * barycentric[:, :, None], axis=1)

    tangent, bitangent, normal = triangle_basis(sim_vertices, sim_faces, triangle_ids)
    return (
        points
        + offset_local[:, 0:1] * tangent
        + offset_local[:, 1:2] * bitangent
        + offset_local[:, 2:3] * normal
    ).astype(np.float32)


def load_mapping(path: Path) -> dict[str, np.ndarray]:
    data = np.load(path)
    return {
        "triangle_indices": data["triangle_indices"],
        "barycentric": data["barycentric"],
        "offset_local": data["offset_local"],
        "rest_distance": data["rest_distance"],
        "sim_vertices_rest": data["sim_vertices_rest"],
        "sim_faces": data["sim_faces"],
    }


def save_mapping(path: Path, mapping: dict[str, np.ndarray], metadata: dict[str, Any]) -> None:
    np.savez_compressed(
        path,
        format_version=np.asarray([1], dtype=np.int32),
        metadata_json=np.asarray(json.dumps(metadata, indent=2)),
        **mapping,
    )


def make_test_sim_deformation(proxy_mesh: trimesh.Trimesh, amplitude_ratio: float) -> np.ndarray:
    vertices = np.asarray(proxy_mesh.vertices, dtype=np.float32)
    normals = np.asarray(proxy_mesh.vertex_normals, dtype=np.float32)
    bounds = np.asarray(proxy_mesh.bounds, dtype=np.float32)
    center = (bounds[0] + bounds[1]) * 0.5
    extent = np.maximum(bounds[1] - bounds[0], 1.0e-6)

    x = (vertices[:, 0] - center[0]) / extent[0]
    y = (vertices[:, 1] - center[1]) / extent[1]
    wave = np.sin(x * np.pi * 4.0) * np.cos(y * np.pi * 3.0)
    amplitude = float(np.max(extent) * amplitude_ratio)
    return vertices + normals * (wave[:, None] * amplitude).astype(np.float32)


def mapping_self_check(render_vertices: np.ndarray, mapping: dict[str, np.ndarray]) -> dict[str, float]:
    reconstructed = reconstruct_render_vertices(mapping, mapping["sim_vertices_rest"])
    error = np.linalg.norm(reconstructed - np.asarray(render_vertices, dtype=np.float32), axis=1)
    return {
        "rest_reconstruction_error_mean": float(np.mean(error)),
        "rest_reconstruction_error_max": float(np.max(error)),
    }


def update_accessor_vec3_float32(gltf: GLTF2, blob: bytearray, accessor_index: int, values: np.ndarray) -> None:
    accessor = gltf.accessors[accessor_index]
    if accessor.componentType != 5126 or accessor.type != "VEC3":
        raise RuntimeError(f"Accessor {accessor_index} is not FLOAT VEC3")
    if accessor.count != len(values):
        raise RuntimeError(f"Accessor {accessor_index} has {accessor.count} values, expected {len(values)}")
    if accessor.bufferView is None:
        raise RuntimeError(f"Accessor {accessor_index} has no bufferView")

    view = gltf.bufferViews[accessor.bufferView]
    start = (view.byteOffset or 0) + (accessor.byteOffset or 0)
    data = np.asarray(values, dtype="<f4")
    stride = view.byteStride

    if stride in {None, 12}:
        blob[start : start + data.nbytes] = data.tobytes()
    else:
        for index, row in enumerate(data):
            offset = start + index * stride
            blob[offset : offset + 12] = row.tobytes()

    accessor.min = data.min(axis=0).astype(float).tolist()
    accessor.max = data.max(axis=0).astype(float).tolist()


def compute_vertex_normals(vertices: np.ndarray, faces: np.ndarray) -> np.ndarray:
    mesh = trimesh.Trimesh(vertices=vertices, faces=faces, process=False)
    return np.asarray(mesh.vertex_normals, dtype=np.float32)


def export_deformed_render_glb_preserve(
    input_path: Path,
    vertices: np.ndarray,
    output_path: Path,
    update_normals: bool,
) -> bool:
    if input_path.suffix.lower() != ".glb" or output_path.suffix.lower() != ".glb":
        return False

    gltf = GLTF2().load(str(input_path))
    if not gltf.meshes:
        return False

    blob_data = gltf.binary_blob()
    if blob_data is None:
        return False
    blob = bytearray(blob_data)

    render_mesh = load_render_mesh(input_path)
    if len(render_mesh.vertices) != len(vertices):
        raise RuntimeError(
            f"Vertex count mismatch: render mesh has {len(render_mesh.vertices)}, frame has {len(vertices)}"
        )

    for mesh in gltf.meshes:
        for primitive in mesh.primitives:
            position_accessor = getattr(primitive.attributes, "POSITION", None)
            if position_accessor is not None:
                update_accessor_vec3_float32(gltf, blob, position_accessor, vertices)

            normal_accessor = getattr(primitive.attributes, "NORMAL", None)
            if update_normals and normal_accessor is not None:
                normals = compute_vertex_normals(vertices, np.asarray(render_mesh.faces, dtype=np.int64))
                update_accessor_vec3_float32(gltf, blob, normal_accessor, normals)

    gltf.set_binary_blob(bytes(blob))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    gltf.save_binary(str(output_path))
    return True


def export_deformed_render_mesh(input_path: Path, vertices: np.ndarray, output_path: Path) -> None:
    if export_deformed_render_glb_preserve(
        input_path=input_path,
        vertices=vertices,
        output_path=output_path,
        update_normals=True,
    ):
        return

    mesh = load_render_mesh(input_path)
    if len(mesh.vertices) != len(vertices):
        raise RuntimeError(
            f"Vertex count mismatch: render mesh has {len(mesh.vertices)}, frame has {len(vertices)}"
        )
    mesh.vertices = np.asarray(vertices, dtype=np.float64)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    mesh.export(output_path)


def build_command(args: argparse.Namespace) -> None:
    input_path = Path(args.input).resolve()
    output_dir = Path(args.out_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    render_mesh = load_render_mesh(input_path)
    proxy_mesh, proxy_report = build_watertight_proxy(
        render_mesh=render_mesh,
        target_faces=args.target_faces,
        samples=args.samples,
        poisson_depth=args.poisson_depth,
        poisson_scale=args.poisson_scale,
    )

    proxy_path = output_dir / args.proxy_name
    mapping_path = output_dir / args.mapping_name
    report_path = output_dir / args.report_name
    proxy_mesh.export(proxy_path)

    mapping, mapping_report = build_high_to_proxy_mapping(
        render_vertices=np.asarray(render_mesh.vertices, dtype=np.float32),
        proxy_mesh=proxy_mesh,
        chunk_size=args.chunk_size,
    )
    mapping_report.update(
        mapping_self_check(
            render_vertices=np.asarray(render_mesh.vertices, dtype=np.float32),
            mapping=mapping,
        )
    )

    metadata = {
        "source": str(input_path),
        "proxy": str(proxy_path),
        "render_mesh": mesh_stats(render_mesh),
        "proxy_build": proxy_report,
        "mapping": mapping_report,
    }
    save_mapping(mapping_path, mapping, metadata)

    outputs: dict[str, Any] = {
        "proxy": str(proxy_path),
        "mapping": str(mapping_path),
        "report": str(report_path),
    }

    if args.run_test:
        sim_frame = make_test_sim_deformation(proxy_mesh, args.test_amplitude)
        high_frame = reconstruct_render_vertices(mapping, sim_frame)

        sim_frame_path = output_dir / args.test_sim_frame_name
        high_test_path = output_dir / args.test_render_name
        np.save(sim_frame_path, sim_frame)
        export_deformed_render_mesh(input_path, high_frame, high_test_path)

        outputs["test_sim_frame"] = str(sim_frame_path)
        outputs["test_render_mesh"] = str(high_test_path)
        metadata["test"] = {
            "amplitude_ratio": float(args.test_amplitude),
            "sim_frame": str(sim_frame_path),
            "render_mesh": str(high_test_path),
        }

    report_path.write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps(outputs, indent=2))


def apply_command(args: argparse.Namespace) -> None:
    input_path = Path(args.input).resolve()
    mapping = load_mapping(Path(args.mapping).resolve())
    sim_vertices = np.load(Path(args.sim_vertices).resolve())

    expected_vertices = len(mapping["sim_vertices_rest"])
    if len(sim_vertices) != expected_vertices:
        raise RuntimeError(
            f"Simulation frame has {len(sim_vertices)} vertices, expected {expected_vertices}"
        )

    render_vertices = reconstruct_render_vertices(mapping, sim_vertices)
    export_deformed_render_mesh(input_path, render_vertices, Path(args.output).resolve())


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Create a watertight cloth simulation proxy and high-to-low deformation mapping.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    build = subparsers.add_parser("build", help="Build simulation proxy and mapping.")
    build.add_argument("--input", required=True, help="High-resolution render GLB/GLTF/OBJ.")
    build.add_argument("--out-dir", default="test_output/cloth_proxy", help="Output directory.")
    build.add_argument("--target-faces", type=int, default=12000, help="Target proxy triangle count.")
    build.add_argument("--samples", type=int, default=120000, help="Surface samples for Poisson reconstruction.")
    build.add_argument("--poisson-depth", type=int, default=8, help="Poisson reconstruction octree depth.")
    build.add_argument("--poisson-scale", type=float, default=1.05, help="Poisson reconstruction scale.")
    build.add_argument("--chunk-size", type=int, default=100000, help="Closest-point mapping chunk size.")
    build.add_argument("--proxy-name", default="cloth_sim_proxy.glb")
    build.add_argument("--mapping-name", default="cloth_high_to_sim_map.npz")
    build.add_argument("--report-name", default="cloth_proxy_report.json")
    build.add_argument("--run-test", action="store_true", help="Write a synthetic deformation test frame.")
    build.add_argument("--test-amplitude", type=float, default=0.015)
    build.add_argument("--test-sim-frame-name", default="cloth_sim_test_frame.npy")
    build.add_argument("--test-render-name", default="cloth_high_mapped_test.glb")
    build.set_defaults(func=build_command)

    apply = subparsers.add_parser("apply", help="Apply one simulated proxy vertex frame to the render mesh.")
    apply.add_argument("--input", required=True, help="Original high-resolution render GLB/GLTF/OBJ.")
    apply.add_argument("--mapping", required=True, help="Mapping .npz from the build command.")
    apply.add_argument("--sim-vertices", required=True, help=".npy array shaped [sim_vertex_count, 3].")
    apply.add_argument("--output", required=True, help="Output deformed render mesh path.")
    apply.set_defaults(func=apply_command)

    return parser


def main() -> None:
    parser = make_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
