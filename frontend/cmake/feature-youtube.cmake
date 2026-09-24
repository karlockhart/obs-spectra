# A Google API client can also be supplied as the JSON file the Google Cloud
# Console downloads for a "Desktop app" OAuth client (client_secret_*.json).
# Drop it into the build or source folder (both are git-ignored), or point
# YOUTUBE_CLIENT_JSON at it. It is
# used when YOUTUBE_CLIENTID is not set, unobfuscated (a desktop client's
# secret is not confidential).
set(YOUTUBE_CLIENT_JSON "" CACHE FILEPATH "Google API client JSON for the YouTube integration")
if(NOT YOUTUBE_CLIENTID)
  set(_youtube_client_file "${YOUTUBE_CLIENT_JSON}")
  if(NOT _youtube_client_file)
    file(
      GLOB _youtube_client_candidates
      "${CMAKE_BINARY_DIR}/client_secret*.json"
      "${CMAKE_BINARY_DIR}/youtube-client.json"
      "${CMAKE_SOURCE_DIR}/client_secret*.json"
      "${CMAKE_SOURCE_DIR}/youtube-client.json"
    )
    if(_youtube_client_candidates)
      list(GET _youtube_client_candidates 0 _youtube_client_file)
    endif()
  endif()
  if(_youtube_client_file AND EXISTS "${_youtube_client_file}")
    file(READ "${_youtube_client_file}" _youtube_client_json)
    # The client sits under "installed" (desktop) or "web"
    string(JSON _youtube_client_kind MEMBER "${_youtube_client_json}" 0)
    string(JSON _youtube_client_id ERROR_VARIABLE _youtube_json_error GET "${_youtube_client_json}" "${_youtube_client_kind}" client_id)
    string(JSON _youtube_client_secret ERROR_VARIABLE _youtube_json_error GET "${_youtube_client_json}" "${_youtube_client_kind}" client_secret)
    if(_youtube_client_id AND _youtube_client_secret AND NOT _youtube_client_id MATCHES "NOTFOUND" AND NOT _youtube_client_secret MATCHES "NOTFOUND")
      set(YOUTUBE_CLIENTID "${_youtube_client_id}")
      set(YOUTUBE_SECRET "${_youtube_client_secret}")
      set(YOUTUBE_CLIENTID_HASH 0)
      set(YOUTUBE_SECRET_HASH 0)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_youtube_client_file}")
      message(STATUS "YouTube API client read from ${_youtube_client_file}")
    else()
      message(WARNING "Could not read a Google API client from ${_youtube_client_file}")
    endif()
  endif()
  unset(_youtube_client_file)
  unset(_youtube_client_candidates)
  unset(_youtube_client_json)
  unset(_youtube_client_kind)
  unset(_youtube_client_id)
  unset(_youtube_client_secret)
  unset(_youtube_json_error)
endif()

if(
  YOUTUBE_CLIENTID
  AND YOUTUBE_SECRET
  AND YOUTUBE_CLIENTID_HASH MATCHES "^(0|[a-fA-F0-9]+)$"
  AND YOUTUBE_SECRET_HASH MATCHES "^(0|[a-fA-F0-9]+)$"
  AND TARGET OBS::browser-panels
)
  target_sources(
    obs-studio
    PRIVATE
      dialogs/OBSYoutubeActions.cpp
      dialogs/OBSYoutubeActions.hpp
      docks/YouTubeAppDock.cpp
      docks/YouTubeAppDock.hpp
      docks/YouTubeChatDock.cpp
      docks/YouTubeChatDock.hpp
      forms/OBSYoutubeActions.ui
      oauth/YoutubeAuth.cpp
      oauth/YoutubeAuth.hpp
      utility/YoutubeApiWrappers.cpp
      utility/YoutubeApiWrappers.hpp
  )

  target_enable_feature(obs-studio "YouTube API connection" YOUTUBE_ENABLED)
else()
  target_disable_feature(obs-studio "YouTube API connection")
  set(YOUTUBE_SECRET_HASH 0)
  set(YOUTUBE_CLIENTID_HASH 0)
endif()
