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
    purple/purple_desktop_archive_store.cpp
    purple/purple_desktop_archive_store.h
    purple/purple_archive_attention.cpp
    purple/purple_archive_attention.h
    purple/purple_archive_sqlite.cpp
    purple/purple_archive_sqlite.h
    purple/purple_archive_deleted_box.cpp
    purple/purple_archive_deleted_box.h
    purple/purple_archive_menu.cpp
    purple/purple_archive_menu.h
    purple/purple_archive_maintenance.cpp
    purple/purple_archive_maintenance.h
    purple/purple_archive_purge_box.cpp
    purple/purple_archive_purge_box.h
    purple/purple_archive_queries.cpp
    purple/purple_archive_queries.h
    purple/purple_archive_crypto.cpp
    purple/purple_archive_crypto.h
    purple/purple_archive_history_box.cpp
    purple/purple_archive_history_box.h
    purple/purple_archive_capture_snapshot.cpp
    purple/purple_archive_capture.cpp
    purple/purple_archive_capture.h
    purple/purple_archive_journal.cpp
    purple/purple_archive_journal.h
    purple/purple_archive_service.cpp
    purple/purple_archive_service.h
    purple/purple_archive_acquired_source.cpp
    purple/purple_archive_acquired_source.h
    purple/purple_archive_lifecycle_capture.cpp
    purple/purple_archive_lifecycle_capture.h
    purple/purple_archive_stream.cpp
    purple/purple_archive_stream.h
    purple/purple_archive_media.cpp
    purple/purple_archive_media.h
    purple/purple_archive_media_access.cpp
    purple/purple_archive_media_access.h
    purple/purple_archive_media_loader.cpp
    purple/purple_archive_media_loader.h
    purple/purple_archive_media_box.cpp
    purple/purple_archive_media_box.h
    purple/purple_archive_files.cpp
    purple/purple_archive_files.h
    purple/purple_account_config.cpp
    purple/purple_account_appwide_removal.h
    purple/purple_account_config.h
    purple/purple_account_send_queue.h
    purple/purple_app_services.cpp
    purple/purple_app_services.h
    purple/purple_build_checks.cpp
    purple/purple_bypass.h
    purple/purple_chat_menus.cpp
    purple/purple_chat_menus.h
    purple/purple_markdown_source.cpp
    purple/purple_markdown_source.h
    purple/purple_compose_receipt.cpp
    purple/purple_compose_receipt.h
    purple/purple_compose_source.cpp
    purple/purple_compose_source.h
    purple/purple_copy.cpp
    purple/purple_copy.h
    purple/purple_copy_messages.cpp
    purple/purple_article_markdown.cpp
    purple/purple_article_markdown.h
    purple/purple_config.cpp
    purple/purple_config.h
    purple/purple_config_file.cpp
    purple/purple_config_file.h
    purple/purple_device.cpp
    purple/purple_device.h
    purple/purple_focus.cpp
    purple/purple_focus.h
    purple/purple_folder_strip.cpp
    purple/purple_folder_strip.h
    purple/purple_view_ids.h
    purple/purple_window_session_controller.h
    purple/purple_work_view.cpp
    purple/purple_work_view.h
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
    purple/purple_notification_privacy.cpp
    purple/purple_notification_privacy.h
    purple/hooks/dialogs_inner_widget.h
    purple/hooks/chat_filters.h
    purple/hooks/history.h
    purple/hooks/mute.h
    purple/hooks/work_view.h
    purple/purple_peek.cpp
    purple/purple_peek.h
    purple/purple_peek_controls.cpp
    purple/purple_peek_controls.h
    purple/purple_wait_gate.cpp
    purple/purple_wait_gate.h
    purple/purple_wait_key_filter.cpp
    purple/purple_wait_key_filter.h
    purple/purple_pinned_music.cpp
    purple/purple_pinned_music.h
    purple/purple_preset_box.cpp
    purple/purple_preset_box.h
    purple/purple_readme.cpp
    purple/purple_readme.h
    purple/purple_row_mark.cpp
    purple/purple_row_mark.h
    purple/purple_schedule.cpp
    purple/purple_schedule.h
    purple/purple_schedule_box.cpp
    purple/purple_schedule_box.h
    purple/purple_session_settings.cpp
    purple/purple_session_settings.h
    purple/purple_suggestions.cpp
    purple/purple_suggestions.h
    purple/purple_picker.cpp
    purple/purple_picker.h
    purple/purple_screentime_box.cpp
    purple/purple_screentime_box.h
    purple/purple_screentime_cover.cpp
    purple/purple_screentime_cover.h
    purple/purple_screentime_history_widget.cpp
    purple/purple_screentime_history_widget.h
    purple/purple_screentime_recorder.cpp
    purple/purple_screentime_recorder.h
    purple/purple_storage_passcode.cpp
    purple/purple_storage_passcode.h
    purple/purple_settings_chat.h
    purple/purple_settings_section.cpp
    purple/purple_settings_section.h
    purple/purple_sync.cpp
    purple/purple_sync.h
    purple/purple_sync_account_binding.cpp
    purple/purple_sync_account_binding.h
    purple/purple_sync_account_inventory.cpp
    purple/purple_sync_account_inventory.h
    purple/purple_sync_account_recovery.cpp
    purple/purple_sync_account_recovery.h
    purple/purple_sync_account_setup.cpp
    purple/purple_sync_account_setup.h
    purple/purple_sync_candidate_reader.cpp
    purple/purple_sync_candidate_reader.h
    purple/purple_sync_config_apply.cpp
    purple/purple_sync_config_apply.h
    purple/purple_sync_config_history.cpp
    purple/purple_sync_config_history.h
    purple/purple_sync_history_guard.h
    purple/purple_sync_config_post.cpp
    purple/purple_sync_config_post.h
    purple/purple_sync_config_upload.cpp
    purple/purple_sync_config_upload.h
    purple/purple_sync_config_publish.cpp
    purple/purple_sync_config_publish.h
    purple/purple_sync_config_review.cpp
    purple/purple_sync_config_review.h
    purple/purple_sync_config_text.cpp
    purple/purple_sync_config_text.h
    purple/purple_sync_chat_service.cpp
    purple/purple_sync_chat_service.h
    purple/purple_sync_chat_store.cpp
    purple/purple_sync_chat_store.h
    purple/purple_sync_chat_discovery.cpp
    purple/purple_sync_chat_discovery.h
    purple/purple_sync_chat_cache.cpp
    purple/purple_sync_chat_cache.h
    purple/purple_sync_chat_write.cpp
    purple/purple_sync_chat_write.h
    purple/purple_sync_chat_io.cpp
    purple/purple_sync_chat_io.h
    purple/purple_sync_chat_scope.h
    purple/purple_sync_chat_requests.cpp
    purple/purple_sync_chat_requests.h
    purple/purple_sync_chat_facts.cpp
    purple/purple_sync_chat_facts.h
    purple/purple_sync_history_scanner.cpp
    purple/purple_sync_history_scanner.h
    purple/purple_sync_auto_store.cpp
    purple/purple_sync_auto_store.h
    purple/purple_sync_auto_transport.cpp
    purple/purple_sync_auto_transport.h
    purple/purple_sync_auto_service.cpp
    purple/purple_sync_auto_service.h
    purple/purple_sync_local_store.cpp
    purple/purple_sync_local_store.h
    purple/purple_sync_review_box.cpp
    purple/purple_sync_review_box.h
    purple/purple_sync_setup_box.cpp
    purple/purple_sync_setup_box.h
    purple/purple_translation_policy.cpp
    purple/purple_translation_policy.h
    purple/purple_ui_witness.cpp
    purple/purple_ui_witness.h
    purple/purple_user_control.cpp
    purple/purple_user_control.h
    purple/purple_ui_witness_geometry.cpp
    purple/purple_ui_witness_geometry.h
    purple/purple_ui_witness_protocol.cpp
    purple/purple_ui_witness_protocol.h
    purple/purple_user_privacy.cpp
    purple/purple_user_privacy.h
)

nice_target_sources(lib_ffmpeg ${src_loc} PRIVATE purple/purple_build_checks.cpp)

if (APPLE)
    nice_target_sources(Telegram ${src_loc} PRIVATE purple/purple_ui_witness_mac.mm purple/purple_archive_backup_mac.mm)
endif()

nice_target_sources(Telegram ${purple_core_loc}
PRIVATE
    purple/purple_archive_envelope.cpp
    purple/purple_archive_envelope.h
    purple/purple_archive_lifecycle.cpp
    purple/purple_archive_lifecycle.h
    purple/purple_archive_staging.cpp
    purple/purple_archive_staging.h
    purple/purple_archive_media_lifecycle.cpp
    purple/purple_archive_media_lifecycle.h
    purple/purple_archive_views.cpp
    purple/purple_archive_views.h
    purple/purple_archive_rows.h
    purple/purple_archive_sql.cpp
    purple/purple_archive_sql.h
    purple/purple_archive_sql_body.cpp
    purple/purple_archive_sql_internal.h
    purple/purple_archive_sql_maintenance.cpp
    purple/purple_archive_sql_maintenance.h
    purple/purple_archive_codec.cpp
    purple/purple_archive_codec.h
    purple/purple_archive_internal.h
    purple/purple_archive_model.cpp
    purple/purple_archive_model.h
    purple/purple_archive_source.cpp
    purple/purple_archive_source.h
    purple/purple_archive_store.cpp
    purple/purple_archive_store.h
    purple/purple_peer_token.cpp
    purple/purple_peer_token.h
    purple/purple_sync_chat.cpp
    purple/purple_sync_chat.h
    purple/purple_sync_chat_carry.cpp
    purple/purple_sync_chat_carry.h
    purple/purple_sync_chat_carry_codec.cpp
    purple/purple_sync_chat_journal.cpp
    purple/purple_sync_chat_journal.h
    purple/purple_account_files.cpp
    purple/purple_account_files.h
    purple/purple_local_choice.cpp
    purple/purple_local_choice.h
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
    purple/purple_sync_auto.cpp
    purple/purple_sync_auto.h
    purple/purple_sync_auto_state.cpp
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
    ${purple_core_loc}/purple/purple_local_choice.cpp
    ${purple_core_loc}/purple/purple_config_diff.cpp
    ${purple_core_loc}/purple/purple_settings.cpp
    ${purple_core_loc}/purple/purple_splice.cpp
    ${purple_core_loc}/purple/purple_state.cpp
    PROPERTIES INCLUDE_DIRECTORIES ${purple_core_loc}/tomlplusplus)

include(${CMAKE_CURRENT_LIST_DIR}/purple_version.cmake)

set(purple_sqlite_loc ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/purple_sqlite)
add_library(purple_sqlite STATIC ${purple_sqlite_loc}/purple_sqlite.c)
set_target_properties(purple_sqlite PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_definitions(purple_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_TEMP_STORE=3 SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_SHARED_CACHE
    SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_ENABLE_API_ARMOR)
find_package(Threads REQUIRED)
target_link_libraries(purple_sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(Telegram PRIVATE purple_sqlite desktop-app::external_openssl_crypto)
if (WIN32)
    target_link_libraries(Telegram PRIVATE Advapi32)
endif()
set_source_files_properties(${src_loc}/purple/purple_archive_sqlite.cpp
    PROPERTIES INCLUDE_DIRECTORIES ${purple_sqlite_loc})
