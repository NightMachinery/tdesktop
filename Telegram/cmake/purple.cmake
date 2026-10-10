# Purple Telegram's part of the Telegram target: its sources, the purple-core
# submodule, and the branding. Telegram/CMakeLists.txt pulls this file in with
# one line, so an upstream merge meets that line instead of these lists. New
# Purple sources go here, never into CMakeLists.txt. See
# docs/purple/upstream_hooks.md.
#
# The include has to sit right after the build_macstore endif, because:
# - the branding below overrides bundle_identifier and output_name, which
#   CMakeLists.txt reads next (bundle_identifier_plist, the target properties,
#   Telegram.plist and the output folder test);
# - the -I order stays SourceFiles, then purple-core, because the call below
#   names both. CMakeLists.txt adds SourceFiles again further down, and CMake
#   drops that duplicate;
# - set_source_files_properties is scoped to a directory, and include() stays
#   in Telegram/, so the toml++ property still reaches the purple-core sources.
# The Purple objects are linked after upstream's, not among them.

# The Work Mode core (parser, splicer, state, engine) lives in the purple-core
# submodule so the Android app can compile the very same sources - see
# docs/purple/config.md. Only the platform layer around it is in SourceFiles.
get_filename_component(purple_core_loc ThirdParty/purple_core REALPATH)

if (NOT build_macstore)
    if (CMAKE_GENERATOR STREQUAL Xcode)
        set(bundle_identifier "com.tdesktop.PurpleTelegram$<$<CONFIG:Debug>:Debug>")
    else()
        set(bundle_identifier "com.tdesktop.PurpleTelegram")
    endif()
    set(output_name "Purple Telegram")
endif()

nice_target_sources(Telegram ${src_loc}
PRIVATE
    purple/purple_bypass.h
    purple/purple_chat_menus.cpp
    purple/purple_chat_menus.h
    purple/purple_config.cpp
    purple/purple_config.h
    purple/purple_device.cpp
    purple/purple_device.h
    purple/purple_focus.cpp
    purple/purple_focus.h
    purple/purple_folder_strip.cpp
    purple/purple_folder_strip.h
    purple/purple_window_session_controller.h
    purple/purple_work_view.cpp
    purple/purple_gate.cpp
    purple/purple_gate.h
    purple/purple_history.cpp
    purple/purple_instant_replaces.cpp
    purple/purple_instant_replaces.h
    purple/purple_last_seen.cpp
    purple/purple_last_seen.h
    purple/purple_last_seen_ui.cpp
    purple/purple_last_seen_ui.h
    purple/purple_list_menu.cpp
    purple/purple_list_menu.h
    purple/purple_mute.cpp
    purple/purple_mute.h
    purple/hooks/dialogs_inner_widget.h
    purple/hooks/history.h
    purple/hooks/mute.h
    purple/purple_peek.cpp
    purple/purple_peek.h
    purple/purple_pinned_music.cpp
    purple/purple_pinned_music.h
    purple/purple_preset_box.cpp
    purple/purple_preset_box.h
    purple/purple_readme.cpp
    purple/purple_readme.h
    purple/purple_schedule.cpp
    purple/purple_schedule.h
    purple/purple_schedule_box.cpp
    purple/purple_schedule_box.h
    purple/purple_session_settings.cpp
    purple/purple_session_settings.h
    purple/purple_screentime_box.cpp
    purple/purple_screentime_box.h
    purple/purple_screentime_cover.cpp
    purple/purple_screentime_cover.h
    purple/purple_screentime_history_widget.cpp
    purple/purple_screentime_history_widget.h
    purple/purple_screentime_recorder.cpp
    purple/purple_screentime_recorder.h
    purple/purple_settings_chat.h
    purple/purple_settings_section.cpp
    purple/purple_settings_section.h
    purple/purple_sync.cpp
    purple/purple_sync.h
    purple/purple_sync_account_binding.cpp
    purple/purple_sync_account_binding.h
    purple/purple_sync_account_inventory.cpp
    purple/purple_sync_account_inventory.h
    purple/purple_sync_account_setup.cpp
    purple/purple_sync_account_setup.h
    purple/purple_sync_candidate_reader.cpp
    purple/purple_sync_candidate_reader.h
    purple/purple_sync_config_apply.cpp
    purple/purple_sync_config_apply.h
    purple/purple_sync_config_history.cpp
    purple/purple_sync_config_history.h
    purple/purple_sync_config_post.cpp
    purple/purple_sync_config_post.h
    purple/purple_sync_config_publish.cpp
    purple/purple_sync_config_publish.h
    purple/purple_sync_config_review.cpp
    purple/purple_sync_config_review.h
    purple/purple_sync_config_text.cpp
    purple/purple_sync_config_text.h
    purple/purple_sync_history_scanner.cpp
    purple/purple_sync_history_scanner.h
    purple/purple_sync_local_store.cpp
    purple/purple_sync_local_store.h
    purple/purple_sync_review_box.cpp
    purple/purple_sync_review_box.h
    purple/purple_sync_setup_box.cpp
    purple/purple_sync_setup_box.h
    purple/purple_ui_witness.cpp
    purple/purple_ui_witness.h
    purple/purple_ui_witness_geometry.cpp
    purple/purple_ui_witness_geometry.h
    purple/purple_ui_witness_protocol.cpp
    purple/purple_ui_witness_protocol.h
)

if (APPLE)
    nice_target_sources(Telegram ${src_loc} PRIVATE purple/purple_ui_witness_mac.mm)
endif()

nice_target_sources(Telegram ${purple_core_loc}
PRIVATE
    purple/purple_config_diff.cpp
    purple/purple_config_diff.h
    purple/purple_config_payload.cpp
    purple/purple_config_payload.h
    purple/purple_config_sync.cpp
    purple/purple_config_sync.h
    purple/purple_engine.cpp
    purple/purple_engine.h
    purple/purple_passcode.cpp
    purple/purple_passcode.h
    purple/purple_screentime.cpp
    purple/purple_screentime.h
    purple/purple_settings.cpp
    purple/purple_settings.h
    purple/purple_splice.cpp
    purple/purple_splice.h
    purple/purple_state.cpp
    purple/purple_state.h
    purple/purple_sync_config_describe.cpp
    purple/purple_sync_config_describe.h
    purple/purple_sync_config_flow.cpp
    purple/purple_sync_config_flow.h
    purple/purple_sync_envelope.cpp
    purple/purple_sync_envelope.h
    purple/purple_sync_directory.cpp
    purple/purple_sync_directory.h
    purple/purple_sync_inventory.cpp
    purple/purple_sync_inventory.h
    purple/purple_sync_json.cpp
    purple/purple_sync_json.h
    purple/purple_sync_local_state.cpp
    purple/purple_sync_local_state.h
    purple/purple_sync_status.cpp
    purple/purple_sync_status.h
    purple/purple_types.h
)

target_include_directories(Telegram PRIVATE ${src_loc} ${purple_core_loc})

# toml++ is vendored as a single header rather than pulled from Homebrew, so the
# packaged build has one less keg that can shadow or conflict with another - see
# docs/mac/build.md. Scoped to the files that include it: putting a 486KB
# header-only parser on the include path of every translation unit would change
# their compile commands and force a full rebuild for no reason.
set_source_files_properties(
    ${purple_core_loc}/purple/purple_config_diff.cpp
    ${purple_core_loc}/purple/purple_settings.cpp
    ${purple_core_loc}/purple/purple_splice.cpp
    ${purple_core_loc}/purple/purple_state.cpp
    PROPERTIES INCLUDE_DIRECTORIES ${purple_core_loc}/tomlplusplus)

include(${CMAKE_CURRENT_LIST_DIR}/purple_version.cmake)
