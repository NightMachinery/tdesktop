RANGES = [('/* VOICE_DECLARATIONS */',
  'purple/hooks/media_playback.h',
  'class VoicePlaybackAdmission final {',
  '[[nodiscard]] std::optional<HistoryItem*> ConfinedVoiceNext('),
 ('/* VOICE_STATE */',
  'purple/purple_voice_queue.cpp',
  'using Type = AudioMsgId::Type;',
  '[[nodiscard]] QString ContinuationName('),
 ('/* VOICE_BRIDGE */',
  'purple/purple_voice_queue.cpp',
  'void StartVoiceQueue() {',
  'void AddVoiceQueueSetting('),
 ('/* NATIVE_PLAY */',
  'media/player/media_player_instance.cpp',
  'void Instance::play(\n\t\tconst AudioMsgId &audioId,',
  'void Instance::playPause(\n\t\tconst AudioMsgId &audioId,'),
 ('/* NATIVE_ITEM_MOVE */',
  'media/player/media_player_instance.cpp',
  'bool Instance::moveInItem(',
  'bool Instance::moveInPlaylist('),
 ('/* NATIVE_PAUSE */',
  'media/player/media_player_instance.cpp',
  'void Instance::pauseSongForVoice() {',
  'void Instance::resumeSongAfterVoice('),
 ('/* NATIVE_RESUME */',
  'media/player/media_player_instance.cpp',
  'void Instance::resumeSongAfterVoice(',
  'bool Instance::next('),
 ('/* NATIVE_PLAYLIST_MOVE */',
  'media/player/media_player_instance.cpp',
  'bool Instance::moveInPlaylist(',
  'void Instance::updatePowerSaveBlocker('),
 ('/* NATIVE_END */',
  'media/player/media_player_instance.cpp',
  'template <typename CheckCallback>\nvoid Instance::emitUpdate(',
  'void Instance::setupShortcuts(')]


def voice_queue_include(sources):
    template = (sources / 'purple/tests/voice_queue_bridge.inc.in').read_text()
    for token, name, start, end in RANGES:
        text = (sources / name).read_text()
        begin = text.index(start)
        body = text[begin:text.index(end, begin)]
        if template.count(token) != 1:
            raise ValueError('voice source placeholder changed: ' + token)
        template = template.replace(token, body)
    native = (sources / 'media/player/media_player_instance.cpp').read_text()
    start = 'void Instance::play(\n\t\tconst AudioMsgId &audioId,'
    begin = native.index(start)
    play = native[begin:native.index('void Instance::playPause(\n\t\tconst AudioMsgId &audioId,', begin)]
    begin = native.index('bool Instance::moveInItem(')
    move = native[begin:native.index('bool Instance::moveInPlaylist(', begin)]
    guard = '\tif (!current() || (data->type == AudioMsgId::Type::Voice && !voiceTransition())) return true;\n'
    if move.count(guard) != 1:
        raise ValueError('native navigation negative-control guard changed')
    template = template.replace('/* MUTANT_PLAY */', play)
    template = template.replace('/* MUTANT_MOVE */', move.replace(guard, '', 1))
    return template
