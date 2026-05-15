#!/usr/bin/env python
"""Demonstrates Reference/Realtime mode switching, DLSS settings, and OIDN
denoiser parameters from Python.

In embed mode:
    Rtxpt.exe --pythonScript Rtxpt/Python/Examples/example_modes_dlss_oidn.py

In extension mode (offline):
    set PYTHONPATH=<repo>/bin/Release;%PYTHONPATH%
    python example_modes_dlss_oidn.py
"""

import rtxpt


def configure_realtime_dlss(app):
    """Realtime mode + DLSS-RR + Reflex example."""
    s = app.settings
    print(f"DLSS supported    : {s.is_dlss_supported}")
    print(f"DLSS-RR supported : {s.is_dlss_rr_supported}")
    print(f"Reflex supported  : {s.is_reflex_supported}")

    # Convenience: mode + AA path in one call.
    app.set_realtime_mode(
        standalone_denoiser=True,           # NRD; ignored when realtime_aa == DLSS_RR
        realtime_aa=int(rtxpt.RealtimeAA.DLSS_RR),
    )

    # Per-knob DLSS configuration.
    s.dlss_mode             = int(rtxpt.DLSSMode.Balanced)
    s.dlss_rr_preset        = int(rtxpt.DLSSRRPreset.PresetE)
    s.dlss_rr_micro_jitter  = 0.1
    s.dlss_rr_brightness_clamp_k = 4096.0
    s.disable_restirs_with_dlss_rr = True

    # Frame generation (DLSS-G) - off in this example.
    s.dlss_fg_mode = int(rtxpt.DLSSFGMode.Off)

    # Reflex low-latency.
    if s.is_reflex_supported:
        s.reflex_mode      = int(rtxpt.ReflexMode.LowLatency)
        s.reflex_capped_fps = 0  # uncapped


def configure_reference_oidn(app):
    """Reference accumulation + OIDN denoiser at end of accumulation."""
    # Convenience: mode + OIDN parameters in one call.
    app.set_reference_mode(
        spp           = 256,
        oidn          = True,
        oidn_quality  = int(rtxpt.OidnQuality.High),
        oidn_passes   = int(rtxpt.OidnPasses.AlbedoNormal),
        oidn_prefilter= int(rtxpt.OidnPrefilter.Accurate),
    )

    # Tweak the path tracer for clean reference output.
    s = app.settings
    s.bounce_count                       = 12
    s.diffuse_bounce_count               = 3
    s.use_nee                            = True
    s.nee_type                           = 2          # NEE-AT
    s.reference_firefly_filter_enabled   = True
    s.reference_firefly_filter_threshold = 5.0
    s.oidn_use_gpu                       = True       # CUDA / HIP / SYCL when available

    # If you change OIDN parameters mid-run, mark them dirty so the renderer
    # re-denoises the next time accumulation completes:
    s.oidn_apply()


def main():
    if rtxpt.MODE == "extension":
        # Standalone (offline) - create our own renderer.
        renderer = rtxpt.Renderer(
            width=1280, height=720, headless=True,
            scene="bistro-programmer-art.scene.json",
        )
        app = renderer.app
    else:
        # Embedded - use the running Rtxpt.exe singleton.
        app = rtxpt.app()
        renderer = None

    print("\n=== Realtime / DLSS-RR ===")
    configure_realtime_dlss(app)

    print("\n=== Reference / OIDN ===")
    configure_reference_oidn(app)

    if renderer is not None:
        print("\n[rtxpt] Rendering reference frame to ref.png ...")
        renderer.step_until_accumulated()
        renderer.save_screenshot("ref.png")
        renderer.close()


if __name__ == "__main__":
    main()
