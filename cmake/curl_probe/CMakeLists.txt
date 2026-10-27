# Whether the system's libcurl can reach the lobby (speaks wss). Its own
# directory: the CURL::libcurl target find_package makes stays here.
find_package(CURL QUIET)
set(ok OFF)
if(CURL_FOUND)
    try_run(SPIKEOUT_CURL_WSS_RUN SPIKEOUT_CURL_WSS_BUILT ${CMAKE_BINARY_DIR}/curl_ws_check
        ${CMAKE_CURRENT_SOURCE_DIR}/../curl_ws_check.cpp
        LINK_LIBRARIES CURL::libcurl)
    if(SPIKEOUT_CURL_WSS_BUILT AND SPIKEOUT_CURL_WSS_RUN EQUAL 0)
        set(ok ON)
    endif()
endif()
set(SPIKEOUT_SYSTEM_CURL ${ok} PARENT_SCOPE)
set(SPIKEOUT_SYSTEM_CURL_SEEN ${CURL_FOUND} PARENT_SCOPE)
