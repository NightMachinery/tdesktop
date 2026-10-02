# Writes upstream's lang.strings followed by Purple's purple.strings to one
# file, for codegen_lang. purple_lang.cmake runs it as a build step:
#
#   cmake -Dupstream_file=... -Dpurple_file=... -Doutput_file=... -P this
#
# Both files are copied byte for byte; only the comment line between them is
# added.

foreach (name upstream_file purple_file output_file)
    if (NOT DEFINED ${name})
        message(FATAL_ERROR "purple_lang_merge.cmake: -D${name}=... is missing")
    endif()
endforeach()

file(READ ${upstream_file} text)
file(WRITE ${output_file} "${text}")
file(READ ${purple_file} text)
file(APPEND ${output_file}
    "\n// Telegram/Resources/langs/purple.strings starts here.\n${text}")
