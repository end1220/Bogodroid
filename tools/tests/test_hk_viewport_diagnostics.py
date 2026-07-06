#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN_SRC = ROOT / "projects" / "unityloader" / "main.cpp"
PLUGIN_SRC = (
    ROOT
    / "projects"
    / "unityloader"
    / "plugins"
    / "hollow_knight_viewport"
    / "hollow_knight_viewport.cpp"
)
PLUGIN_CMAKE = (
    ROOT
    / "projects"
    / "unityloader"
    / "plugins"
    / "hollow_knight_viewport"
    / "CMakeLists.txt"
)


def main() -> int:
    main_src = MAIN_SRC.read_text()
    src = PLUGIN_SRC.read_text()
    cmake = PLUGIN_CMAKE.read_text()

    assert "namespace hk_viewport" not in main_src
    assert "hk_viewport::init(&lil2cpp)" not in main_src
    assert "config_path_abs" in main_src
    assert "std::filesystem::absolute(argv[1])" in main_src
    assert "plugin_host::load(&lil2cpp, config_path_abs.c_str())" in main_src
    assert "#ifdef BD_ENABLE_LOG" in main_src
    assert "(void)tag;" in main_src
    assert "namespace hollow_knight_viewport" in src
    assert "bogodroid_plugin_init" in src
    assert "bogodroid_add_unityloader_plugin(hollow_knight_viewport" in cmake
    assert "game_patches.hollow_knight_viewport" in src
    assert "hk_viewport" in src
    assert 'cfg_bool("debug")' in src
    assert "g_debug" in src
    assert "force_full_viewport" in src
    assert "tk2d_width_safe" in src
    assert "ui_orthographic_width_safe" in src
    assert 'cfg_bool("enabled", 1)' in src
    assert 'cfg_bool("force_full_viewport", 1)' in src
    assert 'cfg_bool("tk2d_width_safe", 1)' in src

    assert '"ForceCameraAspect"' in src
    assert '"SetOverscanViewport"' in src
    assert '"AutoScaleViewport"' in src
    assert '"GameCameras"' in src
    assert '"SetOverscan"' in src
    assert '"GameSettings"' in src
    assert '"LoadOverscanSettings"' in src
    assert '"SaveOverscanSettings"' in src
    assert '"UnityEngine"' in src
    assert '"Camera"' in src
    assert '"set_rect_Injected"' in src
    assert '"set_orthographicSize"' in src
    assert '"get_orthographicSize"' in src
    assert '"tk2dCamera"' in src
    assert '"UpdateCameraMatrix"' in src

    assert "force_aspect_set_overscan_hook" in src
    assert "game_cameras_set_overscan_hook" in src
    assert "camera_set_rect_injected_hook" in src
    assert "g_camera_set_rect_injected_orig" in src
    assert "camera_set_orthographic_size_hook" in src
    assert "g_camera_set_orthographic_size_orig" in src
    assert "g_tk2d_update_camera_matrix_orig" in src
    assert "tk2d_update_camera_matrix_hook" in src
    assert "apply_full_camera_rect" in src
    assert "apply_tk2d_width_safe" in src
    assert "log_tk2d_camera_state" in src
    assert "Tk2dCameraOffsets" in src
    assert "Tk2dCameraSettingsOffsets" in src
    assert "apply_ui_width_safe_ortho" in src
    assert "remember_ortho_base" in src
    assert "remember_tk2d_base" in src
    assert "UnityEngine_Rect_o" in src
    assert "clamp_rect_to_full" in src
    assert "width_safe_scale" in src
    assert "return 1.0f" in src
    assert 'g_api->log("HKVIEW"' in src
    assert "g_api->register_il2cpp_post_init(&post_init" in src
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
