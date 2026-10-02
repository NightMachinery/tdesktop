# Purple Telegram's strings live in Telegram/Resources/langs/purple.strings,
# so that upstream's lang.strings stays untouched. td_lang.cmake includes this
# file in place of its own generate_lang() call: a build step writes
# lang.strings followed by purple.strings into the build tree, and
# generate_lang() reads that file instead of upstream's.
#
# - The merged file is also named lang.strings, because codegen_lang writes
#   the input's file name into every generated header. The same name keeps
#   those headers byte-identical to a build without this file.
# - The merge runs at build time, not at configure time, so editing either
#   file regenerates the strings without a CMake reconfigure.
# - Purple keys come after upstream's. codegen_lang keeps each key's index in
#   gen/lang_auto.indices, so neither the order nor a new key renumbers
#   upstream's keys.
# - codegen_lang's errors name the merged file. The lines after its
#   "purple.strings starts here" comment are purple.strings.
#
# If upstream changes its generate_lang() call in td_lang.cmake, carry the
# change into the call below. See docs/purple/upstream_hooks.md.

set(purple_lang_merged ${CMAKE_CURRENT_BINARY_DIR}/purple_lang/lang.strings)

add_custom_command(
OUTPUT
    ${purple_lang_merged}
COMMAND
    ${CMAKE_COMMAND}
    -Dupstream_file=${res_loc}/langs/lang.strings
    -Dpurple_file=${res_loc}/langs/purple.strings
    -Doutput_file=${purple_lang_merged}
    -P ${CMAKE_CURRENT_LIST_DIR}/purple_lang_merge.cmake
COMMENT "Appending purple.strings to lang.strings"
DEPENDS
    ${res_loc}/langs/lang.strings
    ${res_loc}/langs/purple.strings
    ${CMAKE_CURRENT_LIST_DIR}/purple_lang_merge.cmake
VERBATIM
)

generate_lang(td_lang ${purple_lang_merged} ${src_loc})
